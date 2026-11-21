#include "CarveImage.h"
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <vector>

namespace {

inline uint16_t LE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t LE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
inline uint16_t BE16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t BE32(const uint8_t* p)
{
    return static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static CarveResult Fail(const wchar_t* why)
{
    CarveResult r;
    r.reason = why;
    return r;
}

static CarveResult Ok(uint64_t end, int conf = 85)
{
    CarveResult r;
    r.ok         = true;
    r.endOffset  = end;
    r.confidence = conf;
    return r;
}

// Sector-aligned headers for another file embedded/interleaved before a false EOI/RLE end.
// JPEG SOI is excluded — 0xFFD8FF appears often inside JPEG entropy at sector boundaries.
static bool IsInterFileCapHeader(const uint8_t* head, size_t len)
{
    if (len < 4)
        return false;
    return (head[0] == 'B' && head[1] == 'M') ||
           (memcmp(head, "GIF8", 4) == 0) ||
           (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') ||
           (head[0] == 'I' && head[1] == 'I' && head[2] == 0x2A && head[3] == 0x00) ||
           (head[0] == 'M' && head[1] == 'M' && head[2] == 0x00 && head[3] == 0x2A);
}

static uint64_t FindInterFileCapBoundary(SectorReader& reader, uint64_t start,
                                         uint64_t beforeEnd, uint64_t maxEnd)
{
    constexpr uint32_t kStride = 512;
    uint8_t head[16]{};
    const uint64_t searchEnd = (std::min)(beforeEnd, maxEnd);
    for (uint64_t off = start + kStride; off + 8 <= searchEnd; off += kStride) {
        if (!reader.ReadBytes(off, head, 16))
            break;
        if (IsInterFileCapHeader(head, 16))
            return off;
    }
    return beforeEnd;
}

static CarveResult CapCarveBeforeNextFile(SectorReader& reader, uint64_t start,
                                          CarveResult cr, uint64_t maxEnd)
{
    if (!cr.ok || cr.endOffset <= start + 512)
        return cr;
    const uint64_t cap = FindInterFileCapBoundary(
        reader, start, cr.endOffset, maxEnd);
    if (cap > start + 512 && cap < cr.endOffset)
        cr.endOffset = cap;
    return cr;
}

static bool ReadU8(SectorReader& r, uint64_t off, uint8_t& out)
{
    return r.ReadBytes(off, &out, 1);
}

static bool SkipGifSubBlocks(SectorReader& reader, uint64_t& pos, uint64_t maxEnd)
{
    while (pos < maxEnd) {
        uint8_t sz = 0;
        if (!ReadU8(reader, pos++, sz))
            return false;
        if (sz == 0)
            return true;
        pos += sz;
        if (pos > maxEnd)
            return false;
    }
    return false;
}

static CarveResult ScanJpegEntropyEoi(SectorReader& reader, uint64_t pos, uint64_t maxEnd)
{
    constexpr uint32_t kChunk = 65536;
    constexpr uint64_t kMaxEntropyScan = 4u * 1024u * 1024u;
    maxEnd = (std::min)(maxEnd, pos + kMaxEntropyScan);
    std::vector<uint8_t> buf(kChunk);
    uint64_t p = pos;

    while (p + 1 < maxEnd) {
        const uint32_t toRead = static_cast<uint32_t>(
            (std::min)(static_cast<uint64_t>(kChunk), maxEnd - p));
        if (!reader.ReadBytes(p, buf.data(), toRead))
            break;

        uint32_t i = 0;
        while (i + 1 < toRead) {
            if (buf[i] != 0xFF) {
                ++i;
                continue;
            }
            uint8_t b1 = buf[i + 1];
            if (b1 == 0xD9)
                return Ok(p + i + 2, 88);
            if (b1 == 0x00) {
                i += 2;
                continue;
            }
            if (b1 >= 0xD0 && b1 <= 0xD7) {
                i += 2;
                continue;
            }
            if (i + 1 == toRead - 1) {
                uint8_t tail[2]{};
                if (!reader.ReadBytes(p + i, tail, 2))
                    return Fail(L"jpeg: no EOI after SOS");
                if (tail[1] == 0xD9)
                    return Ok(p + i + 2, 88);
                i += 2;
                continue;
            }
            i += 2;
        }
        p += toRead;
    }
    return Fail(L"jpeg: no EOI after SOS");
}

static CarveResult CarveJpeg(SectorReader& reader, uint64_t start, uint64_t maxEnd)
{
    uint8_t soi[2]{};
    if (!reader.ReadBytes(start, soi, 2) || soi[0] != 0xFF || soi[1] != 0xD8)
        return Fail(L"jpeg: bad SOI");

    uint64_t pos = start + 2;
    while (pos + 4 <= maxEnd) {
        uint8_t hdr[4]{};
        if (!reader.ReadBytes(pos, hdr, 4))
            break;
        if (hdr[0] != 0xFF)
            break;
        const uint8_t marker = hdr[1];
        if (marker == 0xD9)
            return Ok(pos + 2, 90);
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        const uint16_t segLen = static_cast<uint16_t>((hdr[2] << 8) | hdr[3]);
        if (segLen < 2)
            break;
        if (marker == 0xDA) {
            pos += 2 + segLen;
            const uint64_t eoiLimit = (std::min)(maxEnd, pos + 8u * 1024u * 1024u);
            return CapCarveBeforeNextFile(
                reader, start, ScanJpegEntropyEoi(reader, pos, eoiLimit), maxEnd);
        }
        pos += 2 + segLen;
    }
    return Fail(L"jpeg: no EOI");
}

static CarveResult CarveGif(SectorReader& reader, uint64_t start, uint64_t maxEnd)
{
    uint8_t hdr[13]{};
    if (!reader.ReadBytes(start, hdr, 13))
        return Fail(L"gif: header");
    if (memcmp(hdr, "GIF87a", 6) != 0 && memcmp(hdr, "GIF89a", 6) != 0)
        return Fail(L"gif: sig");

    uint64_t pos = start + 13;
    if (hdr[10] & 0x80) {
        const uint64_t gctBytes = 3ull * (1ull << ((hdr[10] & 0x07) + 1));
        pos += gctBytes;
    }

    while (pos < maxEnd) {
        uint8_t block = 0;
        if (!ReadU8(reader, pos, block))
            break;
        if (block == 0x3B)
            return Ok(pos + 1, 88);
        if (block == 0x2C) {
            uint8_t desc[9]{};
            if (!reader.ReadBytes(pos + 1, desc, 9))
                break;
            pos += 10;
            if (desc[8] & 0x80) {
                const uint64_t lctBytes = 3ull * (1ull << ((desc[8] & 0x07) + 1));
                pos += lctBytes;
            }
            if (!SkipGifSubBlocks(reader, pos, maxEnd))
                break;
            continue;
        }
        if (block == 0x21) {
            pos += 2;
            if (!SkipGifSubBlocks(reader, pos, maxEnd))
                break;
            continue;
        }
        break;
    }
    return Fail(L"gif: no trailer");
}

static CarveResult CarveBmp(SectorReader& reader, uint64_t start, uint64_t maxEnd)
{
    uint8_t hdr[14]{};
    if (!reader.ReadBytes(start, hdr, 14))
        return Fail(L"bmp: header");
    if (hdr[0] != 'B' || hdr[1] != 'M')
        return Fail(L"bmp: sig");
    const uint32_t fileSize = LE32(hdr + 2);
    if (fileSize < 14 || start + fileSize > maxEnd)
        return Fail(L"bmp: size");
    return Ok(start + fileSize, 92);
}

static CarveResult CarvePcx(SectorReader& reader, uint64_t start, uint64_t maxEnd)
{
    uint8_t h[128]{};
    if (!reader.ReadBytes(start, h, 128))
        return Fail(L"pcx: header");
    if (h[0] != 0x0A)
        return Fail(L"pcx: sig");

    const uint8_t encoding = h[2];
    const uint8_t bpp      = h[3];
    const uint8_t planes   = h[65];
    const uint16_t bpl     = LE16(h + 66);
    const int width        = static_cast<int>(LE16(h + 8) - LE16(h + 4) + 1);
    const int height       = static_cast<int>(LE16(h + 10) - LE16(h + 6) + 1);

    if (bpl == 0 || height <= 0 || width <= 0 || planes == 0)
        return Fail(L"pcx: dims");

    uint64_t pos = start + 128;
    const int targetLines = height * static_cast<int>(planes);

    if (encoding == 0) {
        pos = start + 128 + static_cast<uint64_t>(bpl) * static_cast<uint64_t>(targetLines);
    } else if (encoding == 1) {
        int carryCount = 0;
        int linesDone  = 0;
        while (linesDone < targetLines && pos < maxEnd) {
            int x = 0;
            while (x < bpl && pos < maxEnd) {
                if (carryCount > 0) {
                    const int n = (std::min)(carryCount, bpl - x);
                    x += n;
                    carryCount -= n;
                    continue;
                }
                uint8_t b = 0;
                if (!ReadU8(reader, pos++, b))
                    return Fail(L"pcx: rle");
                if ((b & 0xC0) == 0xC0) {
                    const int run = b & 0x3F;
                    if (run == 0)
                        return Fail(L"pcx: rle run0");
                    uint8_t val = 0;
                    if (!ReadU8(reader, pos++, val))
                        return Fail(L"pcx: rle val");
                    (void)val;
                    const int n = (std::min)(run, bpl - x);
                    x += n;
                    carryCount = run - n;
                } else {
                    x += 1;
                }
            }
            ++linesDone;
        }
        if (linesDone != targetLines)
            return Fail(L"pcx: rle incomplete");

        // Multi-plane RLE on fragmented layouts can finish one byte short of the
        // true stream length; absorb a single trailing pad byte when present.
        const uint64_t compressed = pos - (start + 128);
        if ((compressed & 1u) != 0 && pos < maxEnd) {
            uint8_t pad = 0;
            if (ReadU8(reader, pos, pad))
                ++pos;
        }
    } else {
        return Fail(L"pcx: encoding");
    }

    // VGA palette footer only on 8-bit single-plane PCX (version 5+).
    if (bpp == 8 && planes == 1 && h[1] >= 5) {
        if (pos + 769 > maxEnd)
            return Fail(L"pcx: palette range");
        uint8_t marker = 0;
        if (!ReadU8(reader, pos, marker) || marker != 0x0C)
            return Fail(L"pcx: palette marker");
        pos += 769;
    }

    if (pos <= start + 128 || pos > maxEnd)
        return Fail(L"pcx: end");
    return CapCarveBeforeNextFile(reader, start, Ok(pos, 86), maxEnd);
}

static CarveResult CarveTiff(SectorReader& reader, uint64_t start, uint64_t maxEnd)
{
    uint8_t hdr[8]{};
    if (!reader.ReadBytes(start, hdr, 8))
        return Fail(L"tif: header");

    const bool le = (hdr[0] == 'I' && hdr[1] == 'I');
    const bool be = (hdr[0] == 'M' && hdr[1] == 'M');
    if (!le && !be)
        return Fail(L"tif: endian");

    auto rd16 = [&](const uint8_t* p) -> uint16_t { return le ? LE16(p) : BE16(p); };
    auto rd32 = [&](const uint8_t* p) -> uint32_t { return le ? LE32(p) : BE32(p); };

    if (rd16(hdr + 2) != 42)
        return Fail(L"tif: magic");

    const uint32_t ifdRel = rd32(hdr + 4);
    if (ifdRel < 8)
        return Fail(L"tif: ifd");

    const uint64_t ifdOff = start + ifdRel;
    if (ifdOff + 2 > maxEnd)
        return Fail(L"tif: ifd range");

    uint8_t cntBuf[2]{};
    if (!reader.ReadBytes(ifdOff, cntBuf, 2))
        return Fail(L"tif: ifd cnt");
    const uint16_t nEntries = rd16(cntBuf);
    if (nEntries == 0 || nEntries > 256)
        return Fail(L"tif: ifd n");

    std::vector<uint32_t> stripOffs;
    std::vector<uint32_t> stripCnts;
    uint32_t stripOffSingle = 0;
    uint32_t stripCntSingle = 0;
    bool hasOff = false, hasCnt = false;

    uint32_t width = 0, height = 0, bps = 8, samples = 1;
    const uint64_t entBase = ifdOff + 2;
    for (uint16_t i = 0; i < nEntries; ++i) {
        uint8_t ent[12]{};
        const uint64_t eoff = entBase + static_cast<uint64_t>(i) * 12;
        if (!reader.ReadBytes(eoff, ent, 12))
            return Fail(L"tif: entry");

        const uint16_t tag  = rd16(ent);
        const uint16_t type = rd16(ent + 2);
        const uint32_t cnt  = rd32(ent + 4);
        const uint32_t val  = rd32(ent + 8);

        if (tag == 256 && cnt == 1)
            width = (type == 3) ? rd16(ent + 8) : val;
        if (tag == 257 && cnt == 1)
            height = (type == 3) ? rd16(ent + 8) : val;
        if (tag == 258 && cnt == 1)
            bps = (type == 3) ? rd16(ent + 8) : val;
        if (tag == 277 && cnt == 1)
            samples = (type == 3) ? rd16(ent + 8) : val;

        auto readVals = [&](uint16_t t, uint32_t c, std::vector<uint32_t>& out) -> bool {
            out.clear();
            if (c == 0)
                return true;
            const uint32_t esize = (t == 3) ? 2u : (t == 4 ? 4u : 0u);
            if (esize == 0)
                return false;
            const uint32_t total = esize * c;
            if (total <= 4) {
                const uint8_t* p = ent + 8;
                for (uint32_t k = 0; k < c; ++k) {
                    if (t == 3)
                        out.push_back(rd16(p + k * 2));
                    else
                        out.push_back(rd32(p + k * 4));
                }
                return true;
            }
            const uint64_t abs = start + val;
            std::vector<uint8_t> buf(total);
            if (!reader.ReadBytes(abs, buf.data(), total))
                return false;
            for (uint32_t k = 0; k < c; ++k) {
                if (t == 3)
                    out.push_back(rd16(buf.data() + k * 2));
                else
                    out.push_back(rd32(buf.data() + k * 4));
            }
            return true;
        };

        if (tag == 273) {
            if (cnt == 1 && type == 4) {
                stripOffSingle = val;
                hasOff         = true;
            } else {
                readVals(type, cnt, stripOffs);
            }
        }
        if (tag == 279) {
            if (cnt == 1 && type == 4) {
                stripCntSingle = val;
                hasCnt         = true;
            } else {
                readVals(type, cnt, stripCnts);
            }
        }
    }

    if (stripOffs.empty() && hasOff)
        stripOffs.push_back(stripOffSingle);
    if (stripCnts.empty() && hasCnt)
        stripCnts.push_back(stripCntSingle);

    if (stripOffs.empty() || stripCnts.empty())
        return Fail(L"tif: strips");

    uint64_t end = start;
    const size_t n = (std::min)(stripOffs.size(), stripCnts.size());
    uint64_t prevStripEnd = start;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t s0 = start + stripOffs[i];
        const uint64_t s1 = s0 + stripCnts[i];
        // Strips must lie within the logical file and stay physically reachable.
        if (i > 0 && s0 > prevStripEnd + 65536)
            break;
        uint8_t probe[1]{};
        if (!reader.ReadBytes(s0, probe, 1))
            break;
        if (s1 > end)
            end = s1;
        prevStripEnd = s1;
    }

    // IFD directory and optional next-IFD pointer sit after strip data.
    const uint64_t ifdEnd = ifdOff + 2 + static_cast<uint64_t>(nEntries) * 12 + 4;
    if (ifdEnd > end)
        end = ifdEnd;

    if (width > 0 && height > 0 && width < 65536 && height < 65536) {
        const uint64_t bytesPerPx =
            static_cast<uint64_t>((bps + 7) / 8) * static_cast<uint64_t>(samples > 0 ? samples : 1);
        const uint64_t sanity =
            start + width * height * bytesPerPx + static_cast<uint64_t>(ifdEnd - start) + 65536;
        if (sanity > start && sanity < end)
            end = sanity;
    }

    if (end <= start || end > maxEnd)
        return Fail(L"tif: end");
    return Ok(end, 90);
}

static CarveResult CarvePng(SectorReader& reader, uint64_t start, uint64_t maxEnd)
{
    uint8_t sig[8]{};
    if (!reader.ReadBytes(start, sig, 8))
        return Fail(L"png: sig");
    const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(sig, kSig, 8) != 0)
        return Fail(L"png: sig");

    uint64_t pos = start + 8;
    while (pos + 12 <= maxEnd) {
        uint8_t lenType[8]{};
        if (!reader.ReadBytes(pos, lenType, 8))
            break;
        const uint32_t clen = BE32(lenType);
        if (clen > 256u * 1024u * 1024u)
            break;
        const uint64_t chunkEnd = pos + 8 + clen + 4;
        if (chunkEnd > maxEnd)
            break;
        if (memcmp(lenType + 4, "IEND", 4) == 0)
            return Ok(chunkEnd, 92);
        pos = chunkEnd;
    }
    return Fail(L"png: no IEND");
}

} // namespace

namespace CarveImage {

CarveResult Carve(SectorReader& reader, uint64_t startOffset, uint64_t maxEnd,
                  const std::wstring& ext)
{
    std::wstring e = ext;
    for (wchar_t& c : e)
        c = static_cast<wchar_t>(towlower(c));

    if (e == L"jpg" || e == L"jpeg")
        return CarveJpeg(reader, startOffset, maxEnd);
    if (e == L"gif")
        return CarveGif(reader, startOffset, maxEnd);
    if (e == L"bmp")
        return CarveBmp(reader, startOffset, maxEnd);
    if (e == L"pcx")
        return CarvePcx(reader, startOffset, maxEnd);
    if (e == L"tif" || e == L"tiff")
        return CarveTiff(reader, startOffset, maxEnd);
    if (e == L"png")
        return CarvePng(reader, startOffset, maxEnd);

    return Fail(L"unsupported");
}

} // namespace CarveImage
