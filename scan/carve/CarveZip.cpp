#include "CarveZip.h"
#include <vector>
#include <algorithm>
#include <cstring>

namespace {
inline uint16_t LE16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t LE32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }
inline uint64_t LE64(const uint8_t* p) {
    uint64_t lo = LE32(p);
    uint64_t hi = LE32(p + 4);
    return lo | (hi << 32);
}

static bool FindEOCD(const std::vector<uint8_t>& tail, size_t& outPos)
{
    // EOCD signature: 50 4B 05 06
    if (tail.size() < 22) return false;
    for (size_t i = tail.size() - 22; i-- > 0; ) {
        if (tail[i] == 0x50 && tail[i+1] == 0x4B && tail[i+2] == 0x05 && tail[i+3] == 0x06) {
            outPos = i;
            return true;
        }
        if (i == 0) break;
    }
    return false;
}

static bool MemHas(const std::vector<uint8_t>& hay, const char* needle)
{
    const uint8_t* p = (const uint8_t*)needle;
    size_t n = strlen(needle);
    if (n == 0 || hay.size() < n) return false;
    for (size_t i = 0; i + n <= hay.size(); ++i) {
        if (memcmp(hay.data() + i, p, n) == 0) return true;
    }
    return false;
}

static bool HasSig(const uint8_t* p, uint32_t sig)
{
    return LE32(p) == sig;
}

// ZIP64 locator: 0x07064b50, 20 bytes
static bool ParseZip64Locator(const std::vector<uint8_t>& tail, size_t eocdPos,
                              uint64_t tailAbsOff, uint64_t& outZip64EocdAbs)
{
    outZip64EocdAbs = 0;
    if (eocdPos < 20) return false;
    size_t locPos = eocdPos - 20;
    const uint8_t* p = tail.data() + locPos;
    if (!HasSig(p, 0x07064b50)) return false;
    // [0] sig, [4] disk with zip64 eocd, [8] zip64 eocd offset (u64), [16] total disks
    outZip64EocdAbs = LE64(p + 8) + 0; // absolute within disk
    // NOTE: offset is relative to start of disk, so absolute = outZip64EocdAbs
    return outZip64EocdAbs > 0;
}

// ZIP64 EOCD record: signature 0x06064b50, size >= 44
static bool ReadZip64Eocd(SectorReader& reader, uint64_t zip64EocdAbs,
                          uint64_t& cdOff, uint64_t& cdSize, uint64_t& entriesTotal)
{
    uint8_t hdr[56]{};
    if (!reader.ReadBytes(zip64EocdAbs, hdr, (uint32_t)sizeof(hdr)))
        return false;
    if (!HasSig(hdr, 0x06064b50)) return false;
    uint64_t recSize = LE64(hdr + 4);
    if (recSize < 44) return false;
    // total entries at offset 32 (u64), cd size at 40 (u64), cd offset at 48 (u64)
    entriesTotal = LE64(hdr + 32);
    cdSize       = LE64(hdr + 40);
    cdOff        = LE64(hdr + 48);
    return true;
}

static bool IsOOXMLFromNames(bool hasContentTypes, bool hasWord, bool hasXl, bool hasPpt,
                             std::wstring& extOut)
{
    extOut.clear();
    if (!hasContentTypes) return false;
    if (hasWord && !hasXl && !hasPpt) { extOut = L"docx"; return true; }
    if (hasXl && !hasWord && !hasPpt) { extOut = L"xlsx"; return true; }
    if (hasPpt && !hasWord && !hasXl) { extOut = L"pptx"; return true; }
    // If multiple, keep as zip (macro packages, templates, or mixed content)
    return true;
}

} // namespace

CarveResult CarveZip::Carve(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)
{
    CarveResult r{};
    r.ok = false;
    r.endOffset = 0;
    r.confidence = 0;

    if (maxEndOffset <= startOffset + 22) {
        r.reason = L"cap too small";
        return r;
    }

    // Forward-scan the first 64 MiB for EOCD — maxSize caps are often far larger than the file.
    const uint64_t scanEnd = (std::min)(startOffset + 64u * 1024u * 1024u, maxEndOffset);

    struct EocdHit {
        uint64_t abs = 0;
        uint16_t diskNo = 0;
        uint16_t cdDisk = 0;
        uint16_t entriesThisDisk16 = 0;
        uint16_t entriesTotal16 = 0;
        uint32_t cdSize32 = 0;
        uint32_t cdOff32 = 0;
        uint16_t commentLen = 0;
    };
    EocdHit best{};
    bool haveBest = false;

    constexpr uint32_t kChunk = 65536;
    for (uint64_t pos = startOffset; pos < scanEnd;) {
        const uint32_t toRead = static_cast<uint32_t>(
            (std::min)(static_cast<uint64_t>(kChunk), scanEnd - pos));
        std::vector<uint8_t> buf(toRead);
        if (!reader.ReadBytes(pos, buf.data(), toRead))
            break;
        for (uint32_t i = 0; i + 22 <= toRead; ++i) {
            if (buf[i] != 0x50 || buf[i + 1] != 0x4B ||
                buf[i + 2] != 0x05 || buf[i + 3] != 0x06)
                continue;
            const uint8_t* e = buf.data() + i;
            EocdHit hit{};
            hit.abs                 = pos + i;
            hit.diskNo              = LE16(e + 4);
            hit.cdDisk              = LE16(e + 6);
            hit.entriesThisDisk16   = LE16(e + 8);
            hit.entriesTotal16      = LE16(e + 10);
            hit.cdSize32            = LE32(e + 12);
            hit.cdOff32             = LE32(e + 16);
            hit.commentLen          = LE16(e + 20);
            const uint64_t endAbs   = hit.abs + 22ull + hit.commentLen;
            if (endAbs > scanEnd)
                continue;
            const uint64_t cdAbs = startOffset + hit.cdOff32;
            if (cdAbs < startOffset || cdAbs >= hit.abs)
                continue;
            best     = hit;
            haveBest = true;
        }
        pos += toRead;
    }

    if (!haveBest) {
        r.reason = L"EOCD not found";
        return r;
    }

    const uint64_t eocdAbs = best.abs;
    const uint8_t eBuf[22] = {
        0x50, 0x4B, 0x05, 0x06,
        static_cast<uint8_t>(best.diskNo & 0xFF),
        static_cast<uint8_t>(best.diskNo >> 8),
        static_cast<uint8_t>(best.cdDisk & 0xFF),
        static_cast<uint8_t>(best.cdDisk >> 8),
        static_cast<uint8_t>(best.entriesThisDisk16 & 0xFF),
        static_cast<uint8_t>(best.entriesThisDisk16 >> 8),
        static_cast<uint8_t>(best.entriesTotal16 & 0xFF),
        static_cast<uint8_t>(best.entriesTotal16 >> 8),
        static_cast<uint8_t>(best.cdSize32 & 0xFF),
        static_cast<uint8_t>((best.cdSize32 >> 8) & 0xFF),
        static_cast<uint8_t>((best.cdSize32 >> 16) & 0xFF),
        static_cast<uint8_t>((best.cdSize32 >> 24) & 0xFF),
        static_cast<uint8_t>(best.cdOff32 & 0xFF),
        static_cast<uint8_t>((best.cdOff32 >> 8) & 0xFF),
        static_cast<uint8_t>((best.cdOff32 >> 16) & 0xFF),
        static_cast<uint8_t>((best.cdOff32 >> 24) & 0xFF),
        static_cast<uint8_t>(best.commentLen & 0xFF),
        static_cast<uint8_t>(best.commentLen >> 8),
    };
    const uint8_t* e = eBuf;

    uint16_t diskNo = best.diskNo;
    uint16_t cdDisk = best.cdDisk;
    uint16_t entriesThisDisk16 = best.entriesThisDisk16;
    uint16_t entriesTotal16 = best.entriesTotal16;
    uint32_t cdSize32 = best.cdSize32;
    uint32_t cdOff32  = best.cdOff32;
    uint16_t commentLen = best.commentLen;

    uint64_t endAbs = eocdAbs + 22ull + commentLen;
    if (endAbs > maxEndOffset) endAbs = maxEndOffset;

    // ZIP64 support if EOCD uses sentinel values
    uint64_t entriesTotal = entriesTotal16;
    uint64_t cdSize = cdSize32;
    uint64_t cdOff  = cdOff32;
    bool isZip64 = (entriesTotal16 == 0xFFFFu) || (cdSize32 == 0xFFFFFFFFu) || (cdOff32 == 0xFFFFFFFFu);
    if (isZip64) {
        uint64_t zip64EocdAbs = 0;
        if (eocdAbs >= startOffset + 20) {
            uint8_t locBuf[20]{};
            if (reader.ReadBytes(eocdAbs - 20, locBuf, 20) &&
                HasSig(locBuf, 0x07064b50)) {
                zip64EocdAbs = LE64(locBuf + 8);
            }
        }
        if (zip64EocdAbs > 0) {
            uint64_t cdOff64 = 0, cdSize64 = 0, ent64 = 0;
            if (ReadZip64Eocd(reader, zip64EocdAbs, cdOff64, cdSize64, ent64)) {
                cdOff = cdOff64;
                cdSize = cdSize64;
                entriesTotal = ent64;
            }
        }
    }

    // Basic validation
    int conf = 55;
    if (diskNo == 0 && cdDisk == 0) conf += 10;
    if (entriesTotal > 0 && cdSize > 0) conf += 10;
    if (!isZip64 && entriesThisDisk16 == entriesTotal16) conf += 5;
    if (isZip64) conf += 5;

    uint64_t cdAbs = startOffset + cdOff;
    if (cdAbs < startOffset || cdAbs >= maxEndOffset) {
        r.reason = L"CD offset out of range";
        return r;
    }

    // Ensure CD fits before EOCD (typical). We still accept if not.
    if (cdAbs + cdSize <= eocdAbs) conf += 5;
    else conf -= 10;

    // Read central directory (cap to 4MB) and parse entries
    uint64_t readCd = (std::min<uint64_t>)(cdSize, 4ull * 1024 * 1024);
    if (readCd < 46) {
        // empty zip may have cdSize=0; treat separately
        if (entriesTotal == 0 && cdSize == 0) {
            conf += 10;
            r.ok = true;
            r.endOffset = endAbs;
            r.confidence = (std::min)(100, conf);
            r.reason = L"empty zip";
            return r;
        }
        r.reason = L"CD too small";
        return r;
    }

    std::vector<uint8_t> cdBuf((size_t)readCd);
    if (!reader.ReadBytes(cdAbs, cdBuf.data(), (uint32_t)cdBuf.size())) {
        r.reason = L"CD read failed";
        return r;
    }

    struct CdEntry {
        std::vector<uint8_t> name;
        uint64_t relOff = 0;     // local header relative offset
        uint64_t compSize = 0;   // compressed size
        uint64_t uncompSize = 0; // uncompressed size
        uint16_t gpFlags = 0;
        uint16_t method  = 0;
    };
    std::vector<CdEntry> entries;
    entries.reserve(128);

    bool hasContentTypes = false;
    bool hasWord = false;
    bool hasXl   = false;
    bool hasPpt  = false;

    uint64_t parsedEntries = 0;
    bool cdSigOk = (cdBuf.size() >= 4 && HasSig(cdBuf.data(), 0x02014b50));
    if (cdSigOk) conf += 10; else conf -= 12;

    // Parse CD file headers: signature (4) + fixed 42 bytes => 46
    size_t pos = 0;
    while (pos + 46 <= cdBuf.size() && parsedEntries < entriesTotal) {
        const uint8_t* h = cdBuf.data() + pos;
        if (!HasSig(h, 0x02014b50)) {
            // try to resync by searching next signature (to reduce false negatives)
            size_t next = pos + 1;
            bool found = false;
            for (; next + 4 <= cdBuf.size(); ++next) {
                if (cdBuf[next] == 0x50 && cdBuf[next+1] == 0x4B &&
                    cdBuf[next+2] == 0x01 && cdBuf[next+3] == 0x02) {
                    pos = next;
                    found = true;
                    break;
                }
            }
            if (!found) break;
            continue;
        }

        uint16_t nameLen = LE16(h + 28);
        uint16_t extraLen = LE16(h + 30);
        uint16_t commentL = LE16(h + 32);
        uint16_t gpFlags = LE16(h + 8);
        uint16_t method  = LE16(h + 10);
        uint32_t comp32   = LE32(h + 20);
        uint32_t uncomp32 = LE32(h + 24);
        uint32_t relOff32 = LE32(h + 42);

        size_t nameOff = pos + 46;
        size_t nextPos = nameOff + nameLen + extraLen + commentL;
        if (nextPos > cdBuf.size()) break;

        // Extract name bytes
        const uint8_t* name = cdBuf.data() + nameOff;
        std::vector<uint8_t> nameBytes(name, name + nameLen);

        if (!hasContentTypes && MemHas(nameBytes, "[Content_Types].xml")) hasContentTypes = true;
        if (!hasWord && nameLen >= 5 && MemHas(nameBytes, "word/")) hasWord = true;
        if (!hasXl   && nameLen >= 3 && MemHas(nameBytes, "xl/"))   hasXl   = true;
        if (!hasPpt  && nameLen >= 4 && MemHas(nameBytes, "ppt/"))  hasPpt  = true;

        // Extract ZIP64 extended information if needed (header ID 0x0001)
        uint64_t relOff = relOff32;
        uint64_t compSize = comp32;
        uint64_t uncompSize = uncomp32;
        if (extraLen > 0 && (relOff32 == 0xFFFFFFFFu || comp32 == 0xFFFFFFFFu || uncomp32 == 0xFFFFFFFFu)) {
            const uint8_t* ex = cdBuf.data() + nameOff + nameLen;
            size_t exPos = 0;
            while (exPos + 4 <= extraLen) {
                uint16_t hid = LE16(ex + exPos);
                uint16_t hsz = LE16(ex + exPos + 2);
                exPos += 4;
                if (exPos + hsz > extraLen) break;
                if (hid == 0x0001) {
                    // Order: (uncompSize?) (compSize?) (relOff?) (diskStart?)
                    size_t zp = 0;
                    if (uncomp32 == 0xFFFFFFFFu && zp + 8 <= hsz) { uncompSize = LE64(ex + exPos + zp); zp += 8; }
                    if (comp32   == 0xFFFFFFFFu && zp + 8 <= hsz) { compSize   = LE64(ex + exPos + zp); zp += 8; }
                    if (relOff32 == 0xFFFFFFFFu && zp + 8 <= hsz) { relOff     = LE64(ex + exPos + zp); zp += 8; }
                    break;
                }
                exPos += hsz;
            }
        }

        // validate relative offset within range (best-effort)
        if (relOff > 0 && relOff < (maxEndOffset - startOffset)) {
            uint64_t absLocal = startOffset + relOff;
            if (absLocal >= startOffset && absLocal < maxEndOffset)
                conf += 1;
        }

        ++parsedEntries;
        pos = nextPos;

        if (entries.size() < 200) {
            CdEntry ce{};
            ce.name = std::move(nameBytes);
            ce.relOff = relOff;
            ce.compSize = compSize;
            ce.uncompSize = uncompSize;
            ce.gpFlags = gpFlags;
            ce.method  = method;
            entries.push_back(std::move(ce));
        }
    }

    // Confidence adjustments based on parsing quality
    if (parsedEntries > 0) conf += 5;
    if (entriesTotal > 0) {
        // if we parsed close to entriesTotal, boost
        if (parsedEntries == entriesTotal) conf += 12;
        else if (parsedEntries * 100 / entriesTotal >= 80) conf += 6;
        else conf -= 8;
    }

    // OOXML detection from names
    std::wstring ext;
    if (IsOOXMLFromNames(hasContentTypes, hasWord, hasXl, hasPpt, ext)) {
        if (!ext.empty()) r.extOverride = ext;
        if (hasContentTypes) conf += 8;
        if (hasWord || hasXl || hasPpt) conf += 7;
    }

    // ── Local header validation (sample N entries) ─────────────────────────
    // This drastically reduces false positives: we ensure CD entries point to
    // valid local headers (PK 03 04) and that file data ranges are plausible.
    const int kMaxCheck = 50;
    int checked = 0, okCount = 0;
    for (size_t i = 0; i < entries.size() && checked < kMaxCheck; ++i) {
        const auto& ce = entries[i];
        if (ce.relOff == 0) continue;
        uint64_t localAbs = startOffset + ce.relOff;
        if (localAbs < startOffset || localAbs + 30 > maxEndOffset) continue;

        uint8_t lh[64]{};
        if (!reader.ReadBytes(localAbs, lh, 30)) continue;
        ++checked;

        if (!HasSig(lh, 0x04034b50)) continue;
        uint16_t nameLen = LE16(lh + 26);
        uint16_t extraLen = LE16(lh + 28);
        uint64_t nameAbs = localAbs + 30;
        uint64_t dataAbs = nameAbs + nameLen + extraLen;

        // Read and compare filename bytes (best-effort; if can't read, skip)
        if (nameLen > 0 && nameLen <= 4096) {
            std::vector<uint8_t> nm(nameLen);
            if (reader.ReadBytes(nameAbs, nm.data(), (uint32_t)nm.size())) {
                // Compare to CD name (allow mismatch for some encodings, but prefer match)
                if (nm.size() == ce.name.size() && memcmp(nm.data(), ce.name.data(), nm.size()) == 0) {
                    // ok
                } else {
                    // tolerate but reduce confidence for this entry
                    // (don't fail; some zips use UTF-8 flags and different normalization)
                }
            }
        }

        // If data descriptor bit set, sizes may be unknown in local header; use CD size.
        uint64_t compSize = ce.compSize;
        if (compSize == 0 && ce.uncompSize == 0) {
            // directory entry, allow
            ++okCount;
            continue;
        }

        if (dataAbs >= maxEndOffset) continue;
        if (compSize > 0 && dataAbs + compSize <= maxEndOffset) {
            ++okCount;
        } else if (compSize == 0) {
            // allow unknown size
            ++okCount;
        }
    }

    if (checked >= 5) {
        int pct = (okCount * 100) / checked;
        if (pct >= 90) conf += 15;
        else if (pct >= 70) conf += 8;
        else if (pct >= 40) conf -= 5;
        else conf -= 15;
    } else if (checked > 0) {
        // low sample, small adjustment
        if (okCount == checked) conf += 5;
        else conf -= 5;
    }

    if (conf < 0) conf = 0;
    if (conf > 100) conf = 100;

    r.ok = true;
    r.endOffset = endAbs;
    r.confidence = conf;
    r.reason = r.extOverride.empty() ? L"EOCD+CD ok" : (L"OOXML: " + r.extOverride);
    return r;
}

