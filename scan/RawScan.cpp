#include "RawScan.h"
#include "../disk/SectorReader.h"
#include "../log/Logger.h"
#include "carve/CarveZip.h"
#include "carve/CarveMp4.h"
#include "carve/CarveImage.h"
#include "carve/CarveStructure.h"
#include <chrono>
#include <climits>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <unordered_set>
#include <vector>

RawScan::RawScan(SignatureDB* db)
    : m_db(db ? db : &m_builtIn)
{}

namespace {

constexpr uint64_t kOversizedCarve          = 8u * 1024u * 1024u;
constexpr uint64_t kMinFragment             = 4096;
constexpr uint64_t kHeaderSearchStepsDefault = 16384;   // 8 MiB @ 512-byte stride
constexpr uint64_t kHeaderSearchStepsText    = 131072;  // 64 MiB for large text blobs
constexpr uint64_t kRescanHostScanCap          = 16u * 1024u * 1024u;
constexpr uint64_t kFooterSearchCap            = 16u * 1024u * 1024u;
constexpr uint64_t kContainerProbeCap          = 64u * 1024u;
constexpr uint64_t kSmallGraphicProbeCap       = 512u * 1024u;
constexpr uint64_t kLargeGraphicProbeCap       = 16u * 1024u * 1024u;

bool IsWeakTextBoundaryExt(const std::wstring& ext)
{
    std::wstring e = ext;
    for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
    return e == L"rtf" || e == L"html" || e == L"htm" || e == L"xml" ||
           e == L"css" || e == L"js" || e == L"json" || e == L"txt" ||
           e == L"ics" || e == L"csv";
}

// ── W8 / CAR-002: optional strict header-structure validation ─────────────
// A magic prefix alone is weak evidence: 8 MiB of random noise produced 115
// reported nodes (14.38 FP/MiB).  For the handful of formats with a rigid
// header immediately after the magic, require that structure to be plausible
// before emitting a node.  Reads only 32 bytes.  Formats without a rule are
// passed through unchanged, so this never reduces coverage for them.
// Callers opt in via RawScan::SetStrictHeaderValidation(true); default is OFF.
bool StrictHeaderStructureOk(SectorReader& reader, uint64_t off,
                             const std::wstring& extIn)
{
    std::wstring e = extIn;
    for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));

    uint8_t h[32]{};
    if (!reader.ReadBytes(off, h, sizeof(h))) return false;

    auto be32 = [&](size_t p) {
        return (static_cast<uint32_t>(h[p]) << 24) |
               (static_cast<uint32_t>(h[p + 1]) << 16) |
               (static_cast<uint32_t>(h[p + 2]) << 8) |
               static_cast<uint32_t>(h[p + 3]);
    };
    auto le16 = [&](size_t p) {
        return static_cast<uint32_t>(h[p]) |
               (static_cast<uint32_t>(h[p + 1]) << 8);
    };
    auto be16 = [&](size_t p) {
        return static_cast<uint32_t>(static_cast<uint32_t>(h[p]) << 8) |
               static_cast<uint32_t>(h[p + 1]);
    };

    if (e == L"png") {
        // 8-byte magic, then IHDR chunk: length=13 and type="IHDR".
        if (be32(8) != 13) return false;
        if (h[12] != 'I' || h[13] != 'H' || h[14] != 'D' || h[15] != 'R')
            return false;
        const uint32_t w = be32(16), ht = be32(20);
        return w > 0 && w < 100000 && ht > 0 && ht < 100000;
    }
    if (e == L"gif") {
        if (h[0] != 'G' || h[1] != 'I' || h[2] != 'F') return false;
        if (h[3] != '8' || (h[4] != '7' && h[4] != '9') || h[5] != 'a')
            return false;
        const uint32_t w = le16(6), ht = le16(8);
        return w > 0 && w < 100000 && ht > 0 && ht < 100000;
    }
    if (e == L"jpg" || e == L"jpeg") {
        if (h[0] != 0xFF || h[1] != 0xD8 || h[2] != 0xFF) return false;
        const uint8_t marker = h[3];
        const bool knownMarker =
            (marker >= 0xE0 && marker <= 0xEF) ||
            (marker >= 0xC0 && marker <= 0xCF) ||
            marker == 0xDB || marker == 0xDA || marker == 0xFE || marker == 0xD8;
        if (!knownMarker) return false;
        const uint32_t segLen = be16(4);
        return segLen >= 2 && segLen <= 4096;
    }
    if (e == L"pdf") {
        return h[0] == '%' && h[1] == 'P' && h[2] == 'D' && h[3] == 'F' &&
               h[4] == '-' && h[5] >= '0' && h[5] <= '9' &&
               h[6] == '.' && h[7] >= '0' && h[7] <= '9';
    }
    if (e == L"zip" || e == L"docx" || e == L"xlsx" || e == L"pptx" ||
        e == L"odt" || e == L"ods" || e == L"odp" || e == L"jar" ||
        e == L"apk" || e == L"epub") {
        if (h[0] != 'P' || h[1] != 'K' || h[2] != 0x03 || h[3] != 0x04)
            return false;
        return le16(4) <= 63;   // version needed to extract
    }
    return true;   // no rule for this extension: leave legacy behaviour intact
}

bool QuickValidateTiffHeader(SectorReader& reader, uint64_t off, uint64_t partByteEnd)
{
    uint8_t h[8]{};
    if (!reader.ReadBytes(off, h, 8))
        return false;
    const bool le = h[0] == 'I' && h[1] == 'I' && h[2] == 0x2A && h[3] == 0x00;
    const bool be = h[0] == 'M' && h[1] == 'M' && h[2] == 0x00 && h[3] == 0x2A;
    if (!le && !be)
        return false;
    const uint32_t ifdOff = le
        ? static_cast<uint32_t>(h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24))
        : static_cast<uint32_t>((h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7]);
    if (ifdOff < 8)
        return false;
    // Large IFD offsets (e.g. CFReDS 100_0018.tif) land in later fragments on
    // disorder/missing layouts — IFD bytes at start+ifdOff are not readable yet.
    constexpr uint32_t kCheapIfd = 65536u;
    if (ifdOff > kCheapIfd)
        return true;
    const uint64_t ifdAbs = off + ifdOff;
    if (ifdAbs + 2 > partByteEnd)
        return false;
    uint8_t cnt[2]{};
    if (!reader.ReadBytes(ifdAbs, cnt, 2))
        return false;
    const uint16_t nTags = le
        ? static_cast<uint16_t>(cnt[0] | (cnt[1] << 8))
        : static_cast<uint16_t>((cnt[0] << 8) | cnt[1]);
    return nTags > 0 && nTags <= 256;
}

uint32_t ReadTiffIfdRel(SectorReader& reader, uint64_t off)
{
    uint8_t h[8]{};
    if (!reader.ReadBytes(off, h, 8))
        return 0;
    const bool le = h[0] == 'I' && h[1] == 'I';
    if (le)
        return static_cast<uint32_t>(h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24));
    if (h[0] == 'M' && h[1] == 'M')
        return static_cast<uint32_t>((h[4] << 24) | (h[5] << 16) | (h[6] << 8) | h[7]);
    return 0;
}

bool IsStrongGraphicHeader(const uint8_t* head, size_t len)
{
    if (len < 4)
        return false;
    return (head[0] == 'B' && head[1] == 'M') ||
           (memcmp(head, "GIF8", 4) == 0) ||
           (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') ||
           (head[0] == 'I' && head[1] == 'I' && head[2] == 0x2A && head[3] == 0x00) ||
           (head[0] == 'M' && head[1] == 'M' && head[2] == 0x00 && head[3] == 0x2A) ||
           (head[0] == 0x0A && head[1] == 0x05 && head[2] == 0x01) ||
           (len >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF);
}

bool IsInterFileCapHeader(const uint8_t* head, size_t len)
{
    if (len < 4)
        return false;
    return (head[0] == 'B' && head[1] == 'M') ||
           (memcmp(head, "GIF8", 4) == 0) ||
           (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') ||
           (head[0] == 'I' && head[1] == 'I' && head[2] == 0x2A && head[3] == 0x00) ||
           (head[0] == 'M' && head[1] == 'M' && head[2] == 0x00 && head[3] == 0x2A) ||
           (len >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF);
}

uint64_t FindNextStrongGraphicHeader(SectorReader& reader, uint64_t start, uint64_t searchEnd,
                                     uint64_t maxSteps = kHeaderSearchStepsDefault)
{
    constexpr uint32_t kStride = 512;
    uint8_t head[16]{};
    uint64_t steps = 0;
    for (uint64_t off = start + kStride; off + 8 <= searchEnd && steps < maxSteps;
         off += kStride, ++steps) {
        if (!reader.ReadBytes(off, head, 16))
            break;
        if (IsStrongGraphicHeader(head, 16))
            return off;
    }
    return searchEnd;
}

uint64_t FindInterFileCapBoundary(SectorReader& reader, uint64_t start, uint64_t searchEnd,
                                  uint64_t maxSteps = kHeaderSearchStepsDefault)
{
    constexpr uint32_t kStride = 512;
    uint8_t head[16]{};
    uint64_t steps = 0;
    for (uint64_t off = start + kStride; off + 8 <= searchEnd && steps < maxSteps;
         off += kStride, ++steps) {
        if (!reader.ReadBytes(off, head, 16))
            break;
        if (IsInterFileCapHeader(head, 16))
            return off;
    }
    return searchEnd;
}

bool IsStrongCarveHeader(const uint8_t* head, size_t len)
{
    if (len < 4)
        return false;
    if (head[0] == 'B' && head[1] == 'M')
        return true;
    if (len >= 4 && memcmp(head, "GIF8", 4) == 0)
        return true;
    if (len >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G')
        return true;
    if (len >= 4 && head[0] == '%' && head[1] == 'P' && head[2] == 'D' && head[3] == 'F')
        return true;
    if (len >= 4 && head[0] == 'R' && head[1] == 'I' && head[2] == 'F' && head[3] == 'F')
        return true;
    if (len >= 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 0x03 && head[3] == 0x04)
        return true;
    if (len >= 8 && memcmp(head, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0)
        return true;
    if (len >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF)
        return true;
    if (len >= 8 && memcmp(head, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8) == 0)
        return true;
    return false;
}

uint64_t FindNextStrongCarveHeader(SectorReader& reader, uint64_t start, uint64_t searchEnd,
                                   uint64_t maxSteps = kHeaderSearchStepsDefault)
{
    constexpr uint32_t kStride = 512;
    uint8_t head[16]{};
    const uint64_t aligned = ((start + kStride - 1) / kStride) * kStride;
    uint64_t steps = 0;
    for (uint64_t off = aligned; off + 8 <= searchEnd && steps < maxSteps;
         off += kStride, ++steps) {
        if (!reader.ReadBytes(off, head, sizeof(head)))
            continue;
        if (IsStrongCarveHeader(head, sizeof(head)))
            return off;
    }
    return searchEnd;
}

uint64_t ResolveNoFooterEnd(SectorReader& reader, uint64_t matchDiskOffset,
                            uint64_t maxEnd, const FileSignature& sig,
                            const std::wstring& ext, uint64_t partByteEnd)
{
    const uint64_t searchSteps = IsWeakTextBoundaryExt(ext)
                                     ? kHeaderSearchStepsText
                                     : kHeaderSearchStepsDefault;
    const uint64_t nextHdr =
        FindNextStrongCarveHeader(reader, matchDiskOffset + 512, maxEnd, searchSteps);
    if (nextHdr > matchDiskOffset + 512 &&
        nextHdr - matchDiskOffset <= sig.maxSize)
        return nextHdr;

    return 0;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
ScanResult RawScan::Run(SectorReader& reader, const Partition& partition)
{
    // A1-10: production callers drive the parallel path through this virtual
    // override via SetThreadCount(). With the default (1) we fall through to
    // the untouched serial implementation below — zero behavioural change.
    if (m_threadCount > 1)
        return Run(reader, partition, m_threadCount);

    ScanResult result;
    auto tStart = std::chrono::steady_clock::now();

    LOG_INFO(L"RawScan::Run – partition=" + partition.DisplayName());

    const uint32_t ss         = reader.GetSectorSize();
    const uint64_t partStart  = partition.startSector;
    uint64_t totalBytes       = partition.sizeBytes > 0
                                    ? partition.sizeBytes
                                    : partition.totalSectors * ss;
    const uint64_t imageBytes = reader.GetTotalBytes();
    if (imageBytes > 0 && (totalBytes == 0 || imageBytes < totalBytes))
        totalBytes = imageBytes;
    const uint64_t scanSectors = (totalBytes + ss - 1) / ss;

    uint64_t fileIdCounter = 0;
    if (ScanRangeCore(reader, partition, m_db, totalBytes, 0, scanSectors,
                      result.files, fileIdCounter))
        result.cancelled = true;

    for (const auto& n : result.files)
        ReportFile(n);

    RescanNestedGraphicHeaders(reader, partition, partStart * ss + totalBytes,
                               result.files, fileIdCounter);
    AddFragmentNodes(reader, partStart * ss + totalBytes, result.files, fileIdCounter);

    result.foundCount   = static_cast<uint32_t>(result.files.size());
    result.scannedBytes = scanSectors * ss;
    result.duration     = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStart);

    ReportProgress(result.scannedBytes, totalBytes, result.foundCount);

    LOG_INFO(L"RawScan done – found " + std::to_wstring(result.foundCount) +
             L" files in " + std::to_wstring(result.duration.count()) + L" ms");

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Core scan loop shared by the serial Run() and the parallel Run(numThreads).
// Scans [startSector, endSector) using the supplied 'db'.  Per-thread callers
// pass their own SignatureDB (ScanBatch is not thread-safe) and their own
// out-vector; the overlap prefix is primed so the only cross-chunk interaction
// is a 512-byte boundary region that the merge step dedups by rawOffset.
// ─────────────────────────────────────────────────────────────────────────────
bool RawScan::ScanRangeCore(SectorReader& reader, const Partition& partition,
                            SignatureDB* db, uint64_t totalBytes,
                            uint64_t startSector, uint64_t endSector,
                            std::vector<FileNode>& out, uint64_t& fileIdCounter)
{
    if (startSector >= endSector)
        return false;

    const uint32_t ss        = reader.GetSectorSize();
    const uint64_t partStart = partition.startSector;

    constexpr uint32_t kBatchSectors = 128;
    constexpr size_t   kBufSize      = kBatchSectors * 512;
    constexpr uint32_t kMatchStride  = 512;

    std::vector<uint8_t> buf(kBufSize + 512); // +512 for overlap
    std::vector<uint8_t> prevTail(512, 0);

    // For any chunk beyond the first, prime prevTail with the 512 bytes that
    // logically precede this range — exactly what the serial scan's inter-batch
    // overlap carries into each new batch.  startSector == 0 (the global start)
    // keeps the all-zero prefix, matching the original first batch.
    if (startSector > 0) {
        const uint64_t prefixByte =
            (partStart + startSector) * static_cast<uint64_t>(ss) - 512;
        if ((partStart + startSector) > 0)
            reader.ReadBytes(prefixByte, prevTail.data(), 512);
        // On a read failure prevTail stays zero-filled; acceptable (rare edge).
    }

    uint64_t sector = startSector;

    while (sector < endSector) {
        if (IsCancelled()) return true;

        uint32_t toRead = static_cast<uint32_t>(
            (std::min)(static_cast<uint64_t>(kBatchSectors), endSector - sector));

        // Place previous tail at the front for cross-sector signature detection
        size_t batchBytes = static_cast<size_t>(toRead) * ss;
        memcpy(buf.data(), prevTail.data(), prevTail.size());

        if (!reader.ReadSectors(partStart + sector, buf.data() + prevTail.size(),
                                toRead)) {
            sector += toRead;
            continue;
        }

        size_t totalBuf = prevTail.size() + batchBytes;

        // ── Streaming magic discovery (single O(N) Aho-Corasick pass) ──────
        const uint64_t batchStartByte =
            (partStart + sector) * ss - static_cast<uint64_t>(prevTail.size());

        std::vector<SignatureDB::ScanCandidate> batchCands;
        batchCands.reserve(64);
        db->ScanBatch(buf.data(), totalBuf, batchStartByte, kMatchStride, batchCands);

        uint64_t cursor = 0;  // next allowed buffer offset (mirrors the loop's i)
        for (const auto& cand : batchCands) {
            const size_t i = static_cast<size_t>(cand.fileStart - batchStartByte);
            if (i < cursor) continue;            // skipped by a prior seat/reject

            const FileSignature* sig = cand.sig;
            uint64_t matchDiskOffset = cand.fileStart;

            auto rejectMatch = [&](uint64_t skipBytes) {
                const uint64_t nextScanByte = matchDiskOffset + skipBytes;
                if (nextScanByte > batchStartByte) {
                    size_t newI = static_cast<size_t>(nextScanByte - batchStartByte);
                    newI = ((newI + kMatchStride - 1) / kMatchStride) * kMatchStride;
                    if (newI > i) cursor = newI;
                }
            };

            std::wstring ext = sig->extension;
            bool skipNestedJpeg = false;
            std::wstring se = ext;
            for (wchar_t& c : se) c = static_cast<wchar_t>(towlower(c));
            if (se == L"jpg" || se == L"jpeg") {
                for (const FileNode& parent : out) {
                    if (parent.rawOffset >= matchDiskOffset ||
                        matchDiskOffset >= parent.rawOffset + parent.size)
                        continue;
                    std::wstring pe = parent.extension;
                    for (wchar_t& c : pe) c = static_cast<wchar_t>(towlower(c));
                    if (pe == L"tif" || pe == L"tiff" || pe == L"jpg" || pe == L"jpeg" ||
                        pe == L"png" || pe == L"bmp" || pe == L"gif") {
                        skipNestedJpeg = true;
                        break;
                    }
                }
            }
            if (skipNestedJpeg) {
                rejectMatch(512);
                continue;
            }

            // Find file end
            uint64_t partByteStart = partStart * ss;
            uint64_t partByteEnd   = partByteStart + totalBytes;
            uint64_t maxEnd        = (std::min)(
                matchDiskOffset + sig->maxSize, partByteEnd);

            uint64_t fileEnd;
            std::wstring extOverride;
            int carveConf = -1;

            const uint64_t carveEnd = (std::min)(maxEnd, matchDiskOffset + kRescanHostScanCap);

            // Structure-aware carving for key containers and graphics (CFReDS carving).
            auto isGraphicExt = [](const std::wstring& x) {
                std::wstring e = x;
                for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
                return e == L"jpg" || e == L"jpeg" || e == L"gif" || e == L"bmp" ||
                       e == L"pcx" || e == L"tif" || e == L"tiff" || e == L"png";
            };

            auto isCfbExt = [](const std::wstring& x) {
                std::wstring e = x;
                for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
                return e == L"doc" || e == L"xls" || e == L"ppt" || e == L"msi" ||
                       e == L"msg" || e == L"mdb";
            };
            auto isRiffExt = [](const std::wstring& x) {
                std::wstring e = x;
                for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
                return e == L"wav" || e == L"avi" || e == L"wav";
            };

            auto isPdfExt = [](const std::wstring& x) {
                std::wstring e = x;
                for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
                return e == L"pdf";
            };

            uint64_t probeEnd = carveEnd;
            if (isGraphicExt(ext)) {
                std::wstring eLo = ext;
                for (wchar_t& c : eLo) c = static_cast<wchar_t>(towlower(c));
                if (eLo == L"tif" || eLo == L"tiff") {
                    const uint64_t hdrOff =
                        matchDiskOffset + static_cast<uint64_t>((std::max)(0, sig->headerOffset));
                    if (!QuickValidateTiffHeader(reader, hdrOff, partByteEnd)) {
                        rejectMatch(512);
                        continue;
                    }
                    probeEnd = (std::min)(carveEnd, matchDiskOffset + 65536u);
                } else if (eLo == L"png" || eLo == L"bmp" || eLo == L"pcx")
                    probeEnd = (std::min)(carveEnd, matchDiskOffset + kLargeGraphicProbeCap);
                else if (eLo == L"jpg" || eLo == L"jpeg" || eLo == L"gif")
                    probeEnd = (std::min)(carveEnd, matchDiskOffset + kSmallGraphicProbeCap);
                else
                    probeEnd = (std::min)(carveEnd, matchDiskOffset + kLargeGraphicProbeCap);
            } else if (ext == L"zip" || ext == L"mp4" || ext == L"m4v" || ext == L"m4a" ||
                       ext == L"mov" || ext == L"3gp" || ext == L"3g2" || ext == L"wmv" ||
                       ext == L"wma" || ext == L"gif" || isCfbExt(ext) || isRiffExt(ext) ||
                       isPdfExt(ext)) {
                probeEnd = (std::min)(carveEnd, matchDiskOffset + kContainerProbeCap);
            }

            if (ext == L"zip" || ext == L"mp4" || ext == L"m4v" || ext == L"m4a" ||
                ext == L"mov" || ext == L"3gp" || ext == L"3g2" || ext == L"wmv" ||
                ext == L"wma" || ext == L"gif" || isCfbExt(ext) || isRiffExt(ext) ||
                isPdfExt(ext) || isGraphicExt(ext)) {
                CarveResult cr{};
                if (ext == L"zip") {
                    cr = CarveZip::Carve(reader, matchDiskOffset, probeEnd);
                } else if (isGraphicExt(ext)) {
                    CarveResult probeCr =
                        CarveImage::Carve(reader, matchDiskOffset, probeEnd, ext);
                    cr = probeCr;
                    std::wstring eLoCarve = ext;
                    for (wchar_t& c : eLoCarve) c = static_cast<wchar_t>(towlower(c));
                    if (!cr.ok && (eLoCarve == L"tif" || eLoCarve == L"tiff") &&
                        probeEnd < carveEnd) {
                        cr = CarveImage::Carve(reader, matchDiskOffset, carveEnd, ext);
                    }
                } else if (isCfbExt(ext)) {
                    cr = CarveStructure::CarveCfb(reader, matchDiskOffset, probeEnd);
                } else if (isRiffExt(ext)) {
                    cr = CarveStructure::CarveRiff(reader, matchDiskOffset, probeEnd);
                } else if (isPdfExt(ext)) {
                    cr = CarveStructure::CarvePdf(reader, matchDiskOffset, probeEnd);
                } else if (ext == L"wmv" || ext == L"wma") {
                    cr = CarveStructure::CarveAsf(reader, matchDiskOffset, probeEnd);
                } else if (ext == L"gif") {
                    cr = CarveStructure::CarveGif(reader, matchDiskOffset, probeEnd);
                } else {
                    cr = CarveMp4::Carve(reader, matchDiskOffset, probeEnd);
                }
                if (cr.ok && cr.endOffset > matchDiskOffset) {
                    fileEnd = cr.endOffset;
                    extOverride = cr.extOverride;
                    carveConf = cr.confidence;
                } else if (isGraphicExt(ext)) {
                    std::wstring eLoFb = ext;
                    for (wchar_t& c : eLoFb) c = static_cast<wchar_t>(towlower(c));
                    const uint64_t searchCap =
                        (std::min)(partByteEnd, matchDiskOffset + kFooterSearchCap);
                    const uint64_t guessEnd = FindInterFileCapBoundary(
                        reader, matchDiskOffset, searchCap, searchCap);
                    const bool tiffLargeIfd =
                        (eLoFb == L"tif" || eLoFb == L"tiff") &&
                        ReadTiffIfdRel(reader, matchDiskOffset) >= 65536u;
                    if (tiffLargeIfd || eLoFb != L"tif" && eLoFb != L"tiff") {
                        if (guessEnd <= matchDiskOffset + 512) {
                            rejectMatch(512);
                            continue;
                        }
                        fileEnd   = guessEnd;
                        carveConf = tiffLargeIfd ? 60 : 55;
                    } else {
                        rejectMatch(512);
                        continue;
                    }
                } else {
                    rejectMatch(kFooterSearchCap);
                    continue;
                }
            } else if (!sig->footer.empty()) {
                fileEnd = FindFooter(reader, matchDiskOffset, *sig, maxEnd);
                if (fileEnd == 0) {
                    rejectMatch(kFooterSearchCap);
                    continue;
                }
            } else {
                fileEnd = ResolveNoFooterEnd(
                    reader, matchDiskOffset, maxEnd, *sig, ext, partByteEnd);
                if (fileEnd == 0) {
                    rejectMatch(kFooterSearchCap);
                    continue;
                }
            }

            uint64_t fileSize = fileEnd - matchDiskOffset;
            if (fileSize == 0) {
                rejectMatch(512);
                continue;
            }

            bool already = false;
            for (const FileNode& existing : out) {
                if (existing.rawOffset == matchDiskOffset) {
                    already = true;
                    break;
                }
            }
            if (already) {
                rejectMatch(512);
                continue;
            }

            // Build synthetic file name: RECOV_<offset_hex>.<ext>
            std::wostringstream nameOss;
            nameOss << L"RECOV_"
                    << std::hex << std::uppercase << std::setfill(L'0')
                    << std::setw(12) << matchDiskOffset
                    << L'.' << (extOverride.empty() ? sig->extension : extOverride);

            FileNode node;
            node.id          = ++fileIdCounter;
            node.name        = nameOss.str();
            node.extension   = extOverride.empty() ? sig->extension : extOverride;
            if (carveConf >= 0) {
                wchar_t pfx[64]{};
                swprintf_s(pfx, L"\\$CARVE\\C%02d\\", carveConf);
                node.originalPath = pfx + node.name;
            } else {
                node.originalPath = L"\\$RAW\\" + node.name;
            }
            node.size        = fileSize;
            node.rawOffset   = matchDiskOffset;
            node.rawSize     = fileSize;
            node.status      = FileStatus::Lost;
            node.fsType      = L"RAW";

            node.recoverability = RecoverabilityLevel::Unknown;
            if (carveConf >= 0) {
                if (carveConf >= 90) node.recoverability = RecoverabilityLevel::Excellent;
                else if (carveConf >= 75) node.recoverability = RecoverabilityLevel::Good;
                else if (carveConf >= 55) node.recoverability = RecoverabilityLevel::Poor;
                else node.recoverability = RecoverabilityLevel::Lost;
            }

            // W8 / CAR-002: opt-in strict header-structure gate.  OFF by
            // default so every existing caller keeps its exact behaviour.
            if (m_strictHeaderValidation &&
                !StrictHeaderStructureOk(reader, matchDiskOffset,
                                         node.extension)) {
                rejectMatch(512);
                continue;
            }

            out.push_back(node);

            uint64_t nextScanByte = fileEnd;
            if (nextScanByte > batchStartByte) {
                size_t newI = static_cast<size_t>(nextScanByte - batchStartByte);
                newI = ((newI + kMatchStride - 1) / kMatchStride) * kMatchStride;
                if (newI > i) cursor = newI;
            }
            cursor = (std::max)(cursor, static_cast<uint64_t>(i) + kMatchStride);  // advance like i += kMatchStride
        }

        // Save last 512 bytes as overlap for next iteration
        if (batchBytes >= 512)
            memcpy(prevTail.data(), buf.data() + prevTail.size() + batchBytes - 512, 512);

        sector += toRead;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel RAW scan: split the partition into per-thread ranges, each scanned
// by a worker that owns its own SectorReader (cloned from the image path) and
// its own SignatureDB (ScanBatch is not thread-safe).  Results are merged by
// rawOffset (boundary magic is detected in two adjacent chunks' 512-byte
// overlap → identical node → dedup keeps one).  The global post-processing
// passes run once on the merged set, so the output file set is identical to
// the serial Run().  Sources that cannot be cloned (devices / virtual disks)
// gracefully fall back to a single-threaded scan.
// ─────────────────────────────────────────────────────────────────────────────
ScanResult RawScan::Run(SectorReader& reader, const Partition& partition,
                        size_t numThreads)
{
    ScanResult result;
    auto tStart = std::chrono::steady_clock::now();

    LOG_INFO(L"RawScan::Run(parallel) – partition=" + partition.DisplayName() +
             L" threads=" + std::to_wstring(numThreads));

    const uint32_t ss         = reader.GetSectorSize();
    const uint64_t partStart  = partition.startSector;
    uint64_t totalBytes       = partition.sizeBytes > 0
                                    ? partition.sizeBytes
                                    : partition.totalSectors * ss;
    const uint64_t imageBytes = reader.GetTotalBytes();
    if (imageBytes > 0 && (totalBytes == 0 || imageBytes < totalBytes))
        totalBytes = imageBytes;
    const uint64_t scanSectors = (totalBytes + ss - 1) / ss;

    // Hard ceiling: each worker owns a SectorReader handle, so guard the public
    // 3-arg API against pathological values (CarveModuleService also clamps).
    if (numThreads == 0) numThreads = 1;
    numThreads = (std::min)(numThreads, static_cast<size_t>(64));

    result.scannedBytes = scanSectors * ss;

    if (scanSectors == 0) {
        result.foundCount = 0;
        result.duration   = std::chrono::milliseconds(0);
        return result;
    }

    const std::wstring imgPath  = reader.GetImagePath();
    const bool           canClone = !imgPath.empty() && numThreads > 1;

    if (numThreads <= 1 || !canClone) {
        // ── Serial-equivalent path (single thread, shared reader + m_db) ──
        uint64_t fid = 0;
        if (ScanRangeCore(reader, partition, m_db, totalBytes, 0, scanSectors,
                          result.files, fid))
            result.cancelled = true;
    } else {
        // ── Pre-clone one reader per thread (fail → fall back to serial) ──
        numThreads = (std::min)(numThreads, scanSectors);
        std::vector<std::unique_ptr<SectorReader>> readers(numThreads);
        bool allCloned = true;
        for (size_t t = 0; t < numThreads; ++t) {
            readers[t] = std::make_unique<SectorReader>();
            if (!readers[t]->OpenImage(imgPath)) { allCloned = false; break; }
            readers[t]->SetReadPolicy(reader.GetReadPolicy());
        }

        if (!allCloned) {
            uint64_t fid = 0;
            if (ScanRangeCore(reader, partition, m_db, totalBytes, 0, scanSectors,
                              result.files, fid))
                result.cancelled = true;
        } else {
            // Build contiguous, non-overlapping ranges [0, scanSectors).
            std::vector<std::pair<uint64_t, uint64_t>> ranges(numThreads);
            const uint64_t per = scanSectors / numThreads;
            const uint64_t rem = scanSectors % numThreads;
            uint64_t cur = 0;
            for (size_t t = 0; t < numThreads; ++t) {
                uint64_t len = per + (t < rem ? 1 : 0);
                ranges[t] = { cur, cur + len };
                cur += len;
            }

            // A1-10: snapshot the EFFECTIVE signature set on the calling thread.
            // Using a default-constructed SignatureDB per worker would silently
            // substitute the 513 built-ins and ignore a custom / extension-
            // filtered DB injected by CarveModuleService, making parallel and
            // serial results diverge.  Each worker builds its own matcher from
            // this identical snapshot (ScanBatch keeps mutable scratch state).
            const std::vector<FileSignature> sigSnapshot = m_db->GetAll();

            std::vector<std::vector<FileNode>> chunks(numThreads);
            std::vector<uint64_t>              chunkIds(numThreads, 0);
            std::vector<char>                  workerCancelled(numThreads, 0);

            auto worker = [&](size_t idx, uint64_t cs, uint64_t ce) {
                if (IsCancelled()) { workerCancelled[idx] = 1; return; }
                SignatureDB localDb(sigSnapshot);  // private, exact effective set
                if (ScanRangeCore(*readers[idx], partition, &localDb, totalBytes,
                                  cs, ce, chunks[idx], chunkIds[idx]))
                    workerCancelled[idx] = 1;
            };

            std::vector<std::thread> workers;
            workers.reserve(numThreads);
            for (size_t t = 0; t < numThreads; ++t)
                workers.emplace_back(worker, t, ranges[t].first, ranges[t].second);
            for (auto& w : workers) w.join();

            for (char c : workerCancelled) {
                if (c) { result.cancelled = true; break; }
            }

            // Merge: dedup by rawOffset.  Boundary magic appears in two adjacent
            // chunks' overlap and produces an identical node; keep the first.
            std::unordered_set<uint64_t> seen;
            for (size_t t = 0; t < numThreads; ++t) {
                for (auto& n : chunks[t]) {
                    if (n.rawOffset == 0) continue;
                    if (!seen.insert(n.rawOffset).second) continue;
                    result.files.push_back(std::move(n));
                }
            }
        }
    }

    // Renumber ids contiguously, then run the global passes once on the merge.
    uint64_t fileIdCounter = 0;
    for (auto& n : result.files) n.id = ++fileIdCounter;

    for (const auto& n : result.files)
        ReportFile(n);

    RescanNestedGraphicHeaders(reader, partition, partStart * ss + totalBytes,
                               result.files, fileIdCounter);
    AddFragmentNodes(reader, partStart * ss + totalBytes, result.files, fileIdCounter);

    result.foundCount = static_cast<uint32_t>(result.files.size());
    result.duration   = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStart);

    ReportProgress(result.scannedBytes, totalBytes, result.foundCount);

    LOG_INFO(L"RawScan done (parallel) – found " + std::to_wstring(result.foundCount) +
             L" files in " + std::to_wstring(result.duration.count()) + L" ms");

    return result;
}

namespace {

bool HasNodeAt(const std::vector<FileNode>& files, uint64_t off)
{
    for (const FileNode& n : files) {
        if (n.rawOffset == off)
            return true;
    }
    return false;
}

const FileNode* ContainerNode(const std::vector<FileNode>& files, uint64_t off)
{
    const FileNode* best = nullptr;
    for (const FileNode& n : files) {
        if (n.rawOffset < off && off < n.rawOffset + n.size) {
            if (!best || n.size > best->size)
                best = &n;
        }
    }
    return best;
}

uint64_t NextNodeStart(const std::vector<FileNode>& files, uint64_t off)
{
    uint64_t next = UINT64_MAX;
    for (const FileNode& n : files) {
        if (n.rawOffset > off && n.rawOffset < next)
            next = n.rawOffset;
    }
    return next;
}

bool CoveredByNode(const std::vector<FileNode>& files, uint64_t off)
{
    for (const FileNode& n : files) {
        if (n.rawOffset <= off && off < n.rawOffset + n.size)
            return true;
    }
    return false;
}

bool SameGraphicExt(const std::wstring& a, const std::wstring& b)
{
    std::wstring x = a, y = b;
    for (wchar_t& c : x) c = static_cast<wchar_t>(towlower(c));
    for (wchar_t& c : y) c = static_cast<wchar_t>(towlower(c));
    return x == y;
}

} // namespace

void RawScan::AddFragmentNodes(SectorReader& reader, uint64_t partByteEnd,
                               std::vector<FileNode>& files, uint64_t& fileIdCounter)
{
    const uint32_t ss = reader.GetSectorSize();
    if (ss == 0) return;

    std::vector<uint64_t> starts;
    starts.reserve(files.size());
    for (const FileNode& n : files)
        starts.push_back(n.rawOffset);
    std::sort(starts.begin(), starts.end());

    auto nextStartAfter = [&](uint64_t off) -> uint64_t {
        const auto it = std::upper_bound(starts.begin(), starts.end(), off);
        return (it != starts.end()) ? *it : UINT64_MAX;
    };

    std::vector<FileNode> extras;
    for (const FileNode& big : files) {
        if (big.size <= kOversizedCarve)
            continue;
        const uint64_t off = big.rawOffset + ss;
        if (off + kMinFragment > partByteEnd || HasNodeAt(files, off))
            continue;

        const uint64_t next = nextStartAfter(off);
        const uint64_t capNext = (next != UINT64_MAX) ? next : partByteEnd;
        const uint64_t bound = (std::min)(big.rawOffset + big.size,
                                          (std::min)(partByteEnd, capNext));
        if (bound <= off + kMinFragment)
            continue;

        extras.push_back(FileNode{});
        FileNode& frag = extras.back();
        frag.id          = ++fileIdCounter;
        frag.rawOffset   = off;
        frag.size        = bound - off;
        frag.rawSize     = frag.size;
        frag.extension   = L"frag";
        frag.status      = FileStatus::Lost;
        frag.fsType      = L"RAW";
        frag.recoverability = RecoverabilityLevel::Poor;

        std::wostringstream nameOss;
        nameOss << L"RECOV_FRAG_"
                << std::hex << std::uppercase << std::setfill(L'0')
                << std::setw(12) << off << L".frag";
        frag.name         = nameOss.str();
        frag.originalPath = L"\\$FRAG\\" + frag.name;
    }

    files.insert(files.end(), extras.begin(), extras.end());
}

void RawScan::RescanMissingGraphicHeaders(SectorReader& reader, const Partition& partition,
                                          uint64_t partByteStart, uint64_t partByteEnd,
                                          std::vector<FileNode>& files, uint64_t& fileIdCounter)
{
    constexpr uint32_t kStride = 512;
    constexpr uint64_t kRescanCarveCap = 16u * 1024u * 1024u;
    auto isGraphicExt = [](const std::wstring& x) {
        std::wstring e = x;
        for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
        return e == L"jpg" || e == L"jpeg" || e == L"gif" || e == L"bmp" ||
               e == L"pcx" || e == L"tif" || e == L"tiff" || e == L"png";
    };

    uint8_t head[16]{};
    auto tryAdd = [&](uint64_t off) {
        if (HasNodeAt(files, off) || off + 8 > partByteEnd)
            return;
        if (!reader.ReadBytes(off, head, 16))
            return;

        const FileSignature* sig = m_db->Match(head, 16);
        if (!sig || !isGraphicExt(sig->extension))
            return;

        const FileNode* container = ContainerNode(files, off);
        if (container && container->size <= kOversizedCarve &&
            SameGraphicExt(container->extension, sig->extension) &&
            off == container->rawOffset)
            return;

        const uint64_t maxEnd = (std::min)(
            off + sig->maxSize, (std::min)(off + kRescanCarveCap, partByteEnd));
        CarveResult cr = CarveImage::Carve(reader, off, maxEnd, sig->extension);
        uint64_t fileEnd = 0;
        int carveConf = -1;
        std::wstring extOverride;
        if (cr.ok && cr.endOffset > off) {
            fileEnd     = cr.endOffset;
            extOverride = cr.extOverride;
            carveConf   = cr.confidence;
        } else if (!sig->footer.empty()) {
            fileEnd = FindFooter(reader, off, *sig, maxEnd);
        } else {
            fileEnd = maxEnd;
        }

        const uint64_t fileSize = fileEnd - off;
        if (fileSize == 0)
            return;

        FileNode node;
        node.id        = ++fileIdCounter;
        node.rawOffset = off;
        node.size      = fileSize;
        node.rawSize   = fileSize;
        node.extension = extOverride.empty() ? sig->extension : extOverride;
        node.status    = FileStatus::Lost;
        node.fsType    = L"RAW";

        std::wostringstream nameOss;
        nameOss << L"RECOV_"
                << std::hex << std::uppercase << std::setfill(L'0')
                << std::setw(12) << off << L'.' << node.extension;
        node.name = nameOss.str();
        if (carveConf >= 0) {
            wchar_t pfx[64]{};
            swprintf_s(pfx, L"\\$CARVE\\C%02d\\", carveConf);
            node.originalPath = pfx + node.name;
        } else {
            node.originalPath = L"\\$RAW\\" + node.name;
        }
        node.recoverability = AssessRawStructure(reader, partition, node, *sig);
        files.push_back(node);
    };

    for (uint64_t off = partByteStart; off + 8 <= partByteEnd; off += kStride) {
        if (CoveredByNode(files, off))
            continue;
        tryAdd(off);
    }
}

void RawScan::RescanNestedGraphicHeaders(SectorReader& reader, const Partition& partition,
                                         uint64_t partByteEnd, std::vector<FileNode>& files,
                                         uint64_t& fileIdCounter)
{
    constexpr uint32_t kStride = 512;
    constexpr uint64_t kRescanCarveCap = 16u * 1024u * 1024u;
    auto isGraphicExt = [](const std::wstring& x) {
        std::wstring e = x;
        for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
        return e == L"jpg" || e == L"jpeg" || e == L"gif" || e == L"bmp" ||
               e == L"pcx" || e == L"tif" || e == L"tiff" || e == L"png";
    };
    auto isJpegExt = [](const std::wstring& x) {
        std::wstring e = x;
        for (wchar_t& c : e) c = static_cast<wchar_t>(towlower(c));
        return e == L"jpg" || e == L"jpeg";
    };

    uint8_t head[16]{};
    auto tryAdd = [&](uint64_t off, bool allowJpeg) {
        if (HasNodeAt(files, off) || off + 8 > partByteEnd)
            return;
        if (!reader.ReadBytes(off, head, 16))
            return;

        const bool strongHdr =
            (head[0] == 'B' && head[1] == 'M') ||
            (memcmp(head, "GIF8", 4) == 0) ||
            (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') ||
            (head[0] == 'I' && head[1] == 'I' && head[2] == 0x2A && head[3] == 0x00) ||
            (head[0] == 'M' && head[1] == 'M' && head[2] == 0x00 && head[3] == 0x2A) ||
            (head[0] == 0x0A && head[1] == 0x05 && head[2] == 0x01);
        const bool jpgHdr = head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF;
        if (!strongHdr && !(allowJpeg && jpgHdr))
            return;

        const FileSignature* sig = m_db->Match(head, 16);
        if (!sig || !isGraphicExt(sig->extension))
            return;
        if (!allowJpeg && isJpegExt(sig->extension))
            return;

        const FileNode* container = ContainerNode(files, off);
        if (container && container->size <= kOversizedCarve &&
            SameGraphicExt(container->extension, sig->extension) &&
            off == container->rawOffset)
            return;

        const uint64_t maxEnd = (std::min)(
            off + sig->maxSize, (std::min)(off + kRescanCarveCap, partByteEnd));
        CarveResult cr = CarveImage::Carve(reader, off, maxEnd, sig->extension);
        uint64_t fileEnd = 0;
        int carveConf = -1;
        std::wstring extOverride;
        if (cr.ok && cr.endOffset > off) {
            fileEnd     = cr.endOffset;
            extOverride = cr.extOverride;
            carveConf   = cr.confidence;
        } else {
            return;
        }

        const uint64_t fileSize = fileEnd - off;
        if (fileSize == 0)
            return;

        FileNode node;
        node.id        = ++fileIdCounter;
        node.rawOffset = off;
        node.size      = fileSize;
        node.rawSize   = fileSize;
        node.extension = extOverride.empty() ? sig->extension : extOverride;
        node.status    = FileStatus::Lost;
        node.fsType    = L"RAW";

        std::wostringstream nameOss;
        nameOss << L"RECOV_"
                << std::hex << std::uppercase << std::setfill(L'0')
                << std::setw(12) << off << L'.' << node.extension;
        node.name = nameOss.str();
        if (carveConf >= 0) {
            wchar_t pfx[64]{};
            swprintf_s(pfx, L"\\$CARVE\\C%02d\\", carveConf);
            node.originalPath = pfx + node.name;
        } else {
            node.originalPath = L"\\$RAW\\" + node.name;
        }
        node.recoverability = AssessRawStructure(reader, partition, node, *sig);
        files.push_back(node);
    };

    const std::vector<FileNode> hosts = files;
    const FileNode* largestBmp = nullptr;
    const FileNode* largestJpg = nullptr;
    for (const FileNode& host : hosts) {
        std::wstring hostExt = host.extension;
        for (wchar_t& c : hostExt) c = static_cast<wchar_t>(towlower(c));
        if (hostExt == L"bmp" && host.size > 512u * 1024u) {
            if (!largestBmp || host.size > largestBmp->size)
                largestBmp = &host;
        }
        if ((hostExt == L"jpg" || hostExt == L"jpeg") && host.size > 32u * 1024u) {
            if (!largestJpg || host.size > largestJpg->size)
                largestJpg = &host;
        }
    }

    for (const FileNode& host : hosts) {
        std::wstring hostExt = host.extension;
        for (wchar_t& c : hostExt) c = static_cast<wchar_t>(towlower(c));
        const bool bigBmp = largestBmp && host.rawOffset == largestBmp->rawOffset;
        const bool bigJpg = largestJpg && host.rawOffset == largestJpg->rawOffset;
        if (!bigBmp && !bigJpg)
            continue;

        const uint64_t regionEnd = (std::min)(
            (std::min)(host.rawOffset + host.size, host.rawOffset + kRescanHostScanCap),
            partByteEnd);
        const bool allowJpeg = bigJpg;
        for (uint64_t off = host.rawOffset + kStride; off + 8 <= regionEnd; off += kStride)
            tryAdd(off, allowJpeg);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
uint64_t RawScan::FindFooter(SectorReader& reader, uint64_t startOffset,
                              const FileSignature& sig, uint64_t maxOffset)
{
    const std::vector<uint8_t>& footer = sig.footer;
    if (footer.empty()) return 0;

    const uint64_t searchEnd = (std::min)(
        maxOffset, startOffset + (std::min)(sig.maxSize, kFooterSearchCap));

    constexpr uint32_t kSearchBuf = 65536; // 64 KB per chunk
    uint64_t pos = startOffset;

    while (pos < searchEnd) {
        uint32_t toRead = static_cast<uint32_t>(
            (std::min)(static_cast<uint64_t>(kSearchBuf), searchEnd - pos));

        std::vector<uint8_t> buf(toRead);
        if (!reader.ReadBytes(pos, buf.data(), toRead)) break;

        // Search for footer pattern
        for (uint32_t i = 0; i + footer.size() <= toRead; ++i) {
            if (memcmp(buf.data() + i, footer.data(), footer.size()) == 0) {
                return pos + i + footer.size(); // Return offset just past footer
            }
        }

        pos += toRead - static_cast<uint32_t>(footer.size()) + 1;
    }

    return 0; // Footer not found — caller should skip this match
}

// ─────────────────────────────────────────────────────────────────────────────
// Lightweight structural validation for RAW results.
// We only read a small amount of data around the header/footer to avoid
// heavy I/O.  This is best-effort and intentionally conservative – when in
// doubt we return RecoverabilityLevel::Good/Unknown rather than Lost.
// ─────────────────────────────────────────────────────────────────────────────
RecoverabilityLevel RawScan::AssessRawStructure(SectorReader& reader,
                                                const Partition& partition,
                                                const FileNode& node,
                                                const FileSignature& sig)
{
    (void)partition; // currently unused, kept for future enhancements

    // ── 通用校验：对所有签名类型统一做一层 header/footer 复核 ─────────────
    // 1) 再次从磁盘位置验证 header 是否匹配（防止缓冲区匹配误差、偏移问题）。
    if (!sig.header.empty()) {
        const uint64_t headerOffset =
            node.rawOffset +
            static_cast<uint64_t>((std::max)(0, sig.headerOffset));
        if (headerOffset + sig.header.size() <= node.rawOffset + node.size) {
            std::vector<uint8_t> buf(sig.header.size());
            if (!reader.ReadBytes(headerOffset, buf.data(),
                                  static_cast<uint32_t>(buf.size()))) {
                return RecoverabilityLevel::Unknown;
            }
            if (memcmp(buf.data(), sig.header.data(), sig.header.size()) != 0) {
                // 头部已经对不上，基本可以认定结构异常。
                return RecoverabilityLevel::Poor;
            }
        }
    }

    // 2) 如果定义了 footer，则在文件尾部 64KB 内搜索一次。
    if (!sig.footer.empty() && node.size >= sig.footer.size()) {
        uint64_t tailSize = std::min<uint64_t>(65536, node.size);
        std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
        uint64_t tailOff = node.rawOffset + node.size - tailSize;
        if (reader.ReadBytes(tailOff, tail.data(),
                             static_cast<uint32_t>(tailSize))) {
            bool found = false;
            for (size_t i = 0; i + sig.footer.size() <= tailSize; ++i) {
                if (memcmp(tail.data() + i, sig.footer.data(),
                           sig.footer.size()) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                // 对所有带 footer 的格式，如果完全找不到结束标记，认为至少是 Poor。
                // 下面按具体类型细化时可以再“抬高”或保持这一结论。
                // 此处暂不直接返回 Lost，以免过于激进。
                // 先记住这个状态：
                // （通过局部变量再结合下面的类型细化）
            }
        }
    }

    const std::wstring& ext = node.extension;

    auto toLower = [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    };

    std::wstring extLower;
    extLower.reserve(ext.size());
    for (wchar_t c : ext)
        extLower.push_back(toLower(c));

    // ─────────────────────────────────────────────────────────────────────
    // 1) 图片格式优先级（JPEG → PNG → GIF → BMP → TIFF → WebP/HEIC 等）
    // ─────────────────────────────────────────────────────────────────────

    // JPEG basic checks: SOI at start, basic segment sanity, EOI near end.
    if (extLower == L"jpg" || extLower == L"jpeg") {
        if (node.size < 4) return RecoverabilityLevel::Poor;

        uint8_t head[8] = {};
        if (!reader.ReadBytes(node.rawOffset, head, sizeof(head)))
            return RecoverabilityLevel::Unknown;

        // JPEG SOI: FF D8, next usually FF E0 / FF E1 etc.
        if (head[0] != 0xFF || head[1] != 0xD8)
            return RecoverabilityLevel::Poor;

        // Lightweight segment walk in the first few hundred bytes:
        // ensure no obviously invalid length fields.
        {
            const size_t kSegBuf = 512;
            std::vector<uint8_t> segBuf(kSegBuf);
            if (reader.ReadBytes(node.rawOffset, segBuf.data(), (uint32_t)segBuf.size())) {
                size_t p = 2; // after SOI
                int segCount = 0;
                while (p + 4 <= segBuf.size() && segCount < 16) {
                    if (segBuf[p] != 0xFF) {
                        break; // markers should be aligned at 0xFF
                    }
                    size_t q = p;
                    // skip fill bytes 0xFF
                    while (q < segBuf.size() && segBuf[q] == 0xFF) ++q;
                    if (q >= segBuf.size()) break;
                    uint8_t marker = segBuf[q];
                    ++segCount;
                    p = q + 1;

                    // Standalone markers with no length
                    if (marker == 0xD8 || marker == 0xD9 ||     // SOI / EOI
                        (marker >= 0xD0 && marker <= 0xD7) ||   // RSTn
                        marker == 0x01)                         // TEM
                    {
                        continue;
                    }

                    if (p + 2 > segBuf.size())
                        break;

                    uint16_t segLen = (uint16_t)((segBuf[p] << 8) | segBuf[p + 1]);
                    if (segLen < 2 || (size_t)p + segLen > segBuf.size()) {
                        // segment length impossible / way out of buffer → likely corrupted
                        return RecoverabilityLevel::Poor;
                    }
                    p += segLen;
                }
            }
        }

        // Check end-of-image marker FF D9 within last 64 KB (or whole file)
        uint64_t tailSize = std::min<uint64_t>(65536, node.size);
        std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
        uint64_t tailOff = node.rawOffset + node.size - tailSize;
        if (!reader.ReadBytes(tailOff, tail.data(), static_cast<uint32_t>(tailSize)))
            return RecoverabilityLevel::Good; // header OK, tail unreadable → assume good

        for (size_t i = tailSize >= 2 ? tailSize - 2 : 0; i + 1 < tailSize; ++i) {
            if (tail[i] == 0xFF && tail[i + 1] == 0xD9)
                return RecoverabilityLevel::Good;
        }
        // No EOI found – likely truncated.
        return RecoverabilityLevel::Poor;
    }

    // GIF: "GIF87a"/"GIF89a" + 逻辑屏幕宽高、全局调色板长度、文件结束 0x3B。
    if (extLower == L"gif") {
        if (node.size < 20) return RecoverabilityLevel::Poor;

        uint8_t hdr[13] = {};
        if (!reader.ReadBytes(node.rawOffset, hdr, sizeof(hdr)))
            return RecoverabilityLevel::Unknown;

        // Signature 已由 SignatureDB 保障为 GIF8[79]a，这里只做宽高与 GCT 的轻量校验。
        uint16_t width  = static_cast<uint16_t>(hdr[6] | (hdr[7] << 8));
        uint16_t height = static_cast<uint16_t>(hdr[8] | (hdr[9] << 8));
        if (width == 0 || height == 0)
            return RecoverabilityLevel::Poor;

        uint8_t packed = hdr[10];
        bool hasGct = (packed & 0x80) != 0;
        if (hasGct) {
            uint8_t gctSizeCode = static_cast<uint8_t>(packed & 0x07);
            uint64_t gctBytes = 3ull * (1ull << (gctSizeCode + 1));
            uint64_t minSize = 13 + gctBytes + 1; // 头 + GCT + 至少一个字节
            if (node.size < minSize)
                return RecoverabilityLevel::Poor;
        }

        // Trailer 0x3B 应该出现在文件末尾附近。
        uint8_t lastByte = 0;
        if (!reader.ReadBytes(node.rawOffset + node.size - 1, &lastByte, 1))
            return RecoverabilityLevel::Good;
        if (lastByte != 0x3B)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // BMP: "BM" + 文件大小字段、像素数据偏移等是否在范围内。
    if (extLower == L"bmp") {
        if (node.size < 54) return RecoverabilityLevel::Poor; // 文件头 14 + DIB 至少 40

        uint8_t hdr[54] = {};
        if (!reader.ReadBytes(node.rawOffset, hdr, sizeof(hdr)))
            return RecoverabilityLevel::Unknown;

        if (hdr[0] != 'B' || hdr[1] != 'M')
            return RecoverabilityLevel::Poor;

        uint32_t fileSize =
            static_cast<uint32_t>(hdr[2] | (hdr[3] << 8) | (hdr[4] << 16) | (hdr[5] << 24));
        uint32_t dataOff =
            static_cast<uint32_t>(hdr[10] | (hdr[11] << 8) | (hdr[12] << 16) | (hdr[13] << 24));
        if (fileSize != 0 && fileSize > node.size)
            return RecoverabilityLevel::Poor;
        if (dataOff >= node.size)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // TIFF / DNG / 部分 RAW：字节序标记 + 42 魔数 + 第一 IFD 偏移在文件范围内。
    if (extLower == L"tif" || extLower == L"tiff" ||
        extLower == L"dng" || extLower == L"cr2"  ||
        extLower == L"nef" || extLower == L"arw"  ||
        extLower == L"rw2" || extLower == L"orf"  ||
        extLower == L"nrw" || extLower == L"sr2"  ||
        extLower == L"pef" || extLower == L"rwl"  ||
        extLower == L"mos" || extLower == L"3fr") {
        if (node.size < 16) return RecoverabilityLevel::Poor;

        uint8_t hdr[16] = {};
        if (!reader.ReadBytes(node.rawOffset, hdr, sizeof(hdr)))
            return RecoverabilityLevel::Unknown;

        bool little = (hdr[0] == 'I' && hdr[1] == 'I');
        bool big    = (hdr[0] == 'M' && hdr[1] == 'M');
        if (!little && !big)
            return RecoverabilityLevel::Poor;

        auto rd16 = [little](const uint8_t* p) -> uint16_t {
            return little ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                          : static_cast<uint16_t>((p[0] << 8) | p[1]);
        };
        auto rd32 = [little](const uint8_t* p) -> uint32_t {
            return little
                ? static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
                : static_cast<uint32_t>((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
        };

        uint16_t magic = rd16(hdr + 2);
        if (magic != 42)
            return RecoverabilityLevel::Poor;

        uint32_t ifd0 = rd32(hdr + 4);
        if (ifd0 < 8 || ifd0 >= node.size - 2)
            return RecoverabilityLevel::Poor;

        // 进一步读取 IFD entry 数目是否“合理”（不过度越界）
        uint8_t cntBuf[2] = {};
        if (!reader.ReadBytes(node.rawOffset + ifd0, cntBuf, 2))
            return RecoverabilityLevel::Good;
        uint16_t entryCount = rd16(cntBuf);
        if (entryCount == 0 || entryCount > 1024)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // WebP：RIFF + "WEBP"，检查 RIFF 长度字段是否与文件大小大致相符。
    if (extLower == L"webp") {
        if (node.size < 16) return RecoverabilityLevel::Poor;
        uint8_t hdr[16] = {};
        if (!reader.ReadBytes(node.rawOffset, hdr, sizeof(hdr)))
            return RecoverabilityLevel::Unknown;
        // "RIFF" + size + "WEBP"
        if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F')
            return RecoverabilityLevel::Poor;
        uint32_t riffSize =
            static_cast<uint32_t>(hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24));
        // RIFF size 字段 = 文件总字节数 - 8
        if (riffSize + 8 > node.size + 16) // 允许少量截断
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // HEIC/HEIF/AVIF：ISO BMFF，依赖 SignatureDB 的 ftyp 头；这里仅检查第一个 box 长度。
    if (extLower == L"heic" || extLower == L"heif" || extLower == L"avif") {
        if (node.size < 16) return RecoverabilityLevel::Poor;
        uint8_t box[16] = {};
        if (!reader.ReadBytes(node.rawOffset, box, sizeof(box)))
            return RecoverabilityLevel::Unknown;
        uint32_t boxSize = (box[0] << 24) | (box[1] << 16) | (box[2] << 8) | box[3];
        if (boxSize < 16 || boxSize > node.size)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // ─────────────────────────────────────────────────────────────────────
    // 2) 办公文档优先级（PDF → OOXML/ZIP → CFB DOC/XLS/PPT → RTF 等）
    // ─────────────────────────────────────────────────────────────────────

        // PNG: check signature + IHDR chunk at start + IEND chunk near end.
    if (extLower == L"png") {
        if (node.size < 16) return RecoverabilityLevel::Poor;

        uint8_t sigBuf[8] = {};
        if (!reader.ReadBytes(node.rawOffset, sigBuf, 8))
            return RecoverabilityLevel::Unknown;

        const uint8_t pngSig[8] = { 0x89, 'P','N','G', 0x0D,0x0A,0x1A,0x0A };
        if (memcmp(sigBuf, pngSig, 8) != 0)
            return RecoverabilityLevel::Poor;

        // Validate IHDR chunk just after signature: length=13, type="IHDR", width/height non-zero
        uint8_t ihdr[8 + 13 + 4] = {}; // len+type + data(13) + crc
        if (!reader.ReadBytes(node.rawOffset + 8, ihdr, (uint32_t)sizeof(ihdr)))
            return RecoverabilityLevel::Unknown;

        uint32_t ihdrLen =
            (uint32_t)((ihdr[0] << 24) | (ihdr[1] << 16) | (ihdr[2] << 8) | ihdr[3]);
        if (ihdrLen != 13 || memcmp(ihdr + 4, "IHDR", 4) != 0)
            return RecoverabilityLevel::Poor;

        uint32_t w = (uint32_t)((ihdr[8] << 24) | (ihdr[9] << 16) | (ihdr[10] << 8) | ihdr[11]);
        uint32_t h = (uint32_t)((ihdr[12] << 24) | (ihdr[13] << 16) | (ihdr[14] << 8) | ihdr[15]);
        if (w == 0 || h == 0)
            return RecoverabilityLevel::Poor;
        // Avoid insane dimensions (protect against garbage)
        if (w > 100000 || h > 100000)
            return RecoverabilityLevel::Poor;

        uint64_t firstChunkEnd = 8 /*sig*/ + 8 /*len+type*/ + ihdrLen + 4 /*crc*/;
        if (firstChunkEnd > node.size)
            return RecoverabilityLevel::Poor;

        // IEND chunk near end
        uint64_t tailSize = std::min<uint64_t>(65536, node.size);
        std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
        uint64_t tailOff = node.rawOffset + node.size - tailSize;
        if (!reader.ReadBytes(tailOff, tail.data(), static_cast<uint32_t>(tailSize)))
            return RecoverabilityLevel::Good;

        // IEND chunk type bytes: 49 45 4E 44
        const uint8_t iend[4] = { 'I','E','N','D' };
        for (size_t i = 0; i + 8 <= tailSize; ++i) {
            if (memcmp(tail.data() + i + 4, iend, 4) == 0)
                return RecoverabilityLevel::Good;
        }
        return RecoverabilityLevel::Poor;
    }

    // PDF: %PDF- at start, %%EOF near end.
    if (extLower == L"pdf") {
        if (node.size < 8) return RecoverabilityLevel::Poor;

        uint8_t head[8] = {};
        if (!reader.ReadBytes(node.rawOffset, head, 8))
            return RecoverabilityLevel::Unknown;

        const uint8_t pdfMagic[5] = { '%','P','D','F','-' };
        if (memcmp(head, pdfMagic, sizeof(pdfMagic)) != 0)
            return RecoverabilityLevel::Poor;

        uint64_t tailSize = std::min<uint64_t>(65536, node.size);
        std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
        uint64_t tailOff = node.rawOffset + node.size - tailSize;
        if (!reader.ReadBytes(tailOff, tail.data(), static_cast<uint32_t>(tailSize)))
            return RecoverabilityLevel::Good;

        const uint8_t eofMagic[5] = { '%','%','E','O','F' };
        for (size_t i = 0; i + sizeof(eofMagic) <= tailSize; ++i) {
            if (memcmp(tail.data() + i, eofMagic, sizeof(eofMagic)) == 0)
                return RecoverabilityLevel::Good;
        }
        // 没找到 EOF，很多 PDF 仍可部分打开，标记为 Poor 而不是 Lost。
        return RecoverabilityLevel::Poor;
    }

    // Office Open XML / ZIP 容器（docx/xlsx/pptx/odt/ods/zip 等）：
    //   - 利用 SignatureDB 的 PK 头
    //   - 在末尾 64KB 内寻找 EOCD（"PK\005\006"），并做基本边界检查。
    auto isZipLike =
        (extLower == L"zip" || extLower == L"docx" || extLower == L"xlsx" ||
         extLower == L"pptx" || extLower == L"odt"  || extLower == L"ods"  ||
         extLower == L"odp"  || extLower == L"epub" || extLower == L"xps"  ||
         extLower == L"apk"  || extLower == L"ipa"  || extLower == L"whl"  ||
         extLower == L"swc"  || extLower == L"mscz" || extLower == L"pages"||
         extLower == L"key"  || extLower == L"numbers");

    if (isZipLike) {
        if (node.size < 22) return RecoverabilityLevel::Poor; // 最小 EOCD 结构

        uint64_t tailSize = std::min<uint64_t>(65536, node.size);
        std::vector<uint8_t> tail(static_cast<size_t>(tailSize));
        uint64_t tailOff = node.rawOffset + node.size - tailSize;
        if (!reader.ReadBytes(tailOff, tail.data(), static_cast<uint32_t>(tailSize)))
            return RecoverabilityLevel::Good;

        const uint8_t eocdSig[4] = { 'P','K',0x05,0x06 };
        for (size_t i = 0; i + 22 <= tailSize; ++i) {
            if (memcmp(tail.data() + i, eocdSig, 4) == 0) {
                // 读取 central directory size & offset 做简单边界检查。
                if (i + 20 <= tailSize) {
                    uint32_t cdSize =
                        tail[i + 12] | (tail[i + 13] << 8) |
                        (tail[i + 14] << 16) | (tail[i + 15] << 24);
                    uint32_t cdOff =
                        tail[i + 16] | (tail[i + 17] << 8) |
                        (tail[i + 18] << 16) | (tail[i + 19] << 24);
                    uint64_t absCdEnd = static_cast<uint64_t>(cdOff) + cdSize;
                    if (absCdEnd <= node.size + 0x10000)
                        return RecoverabilityLevel::Good;
                }
                // 即便结构字段略异常，只要 EOCD 找到，也视为可尝试恢复。
                return RecoverabilityLevel::Good;
            }
        }
        // 完全找不到 EOCD，通常意味着严重截断。
        return RecoverabilityLevel::Poor;
    }

    // 经典 CFB/OLE2 文档（doc/xls/ppt/msg/mdb 等）：在 header 基础上做更严格的轻量检查。
    if (extLower == L"doc" || extLower == L"xls" || extLower == L"ppt" ||
        extLower == L"msg" || extLower == L"mdb" || extLower == L"pub" ||
        extLower == L"vsd" || extLower == L"msi" || extLower == L"wps" ||
        extLower == L"thmx") {
        if (node.size < 512) return RecoverabilityLevel::Poor;
        // CFB header 已由 SignatureDB 匹配，这里对 sectorSize / 目录起始扇区做一次更细检查。
        uint8_t hdr[76] = {};
        if (!reader.ReadBytes(node.rawOffset, hdr, sizeof(hdr)))
            return RecoverabilityLevel::Unknown;

        uint16_t sectorShift = static_cast<uint16_t>(hdr[30] | (hdr[31] << 8));
        if (sectorShift < 9 || sectorShift > 16)
            return RecoverabilityLevel::Poor;

        uint16_t miniSectorShift = static_cast<uint16_t>(hdr[32] | (hdr[33] << 8));
        if (miniSectorShift != 6 && miniSectorShift != 9 && miniSectorShift != 12)
            return RecoverabilityLevel::Poor;

        // 目录 first sector index 位于 offset 0x30
        uint32_t dirSector =
            static_cast<uint32_t>(hdr[48] | (hdr[49] << 8) | (hdr[50] << 16) | (hdr[51] << 24));
        // 0xFFFFFFFF = no directory, 视为损坏
        if (dirSector == 0xFFFFFFFFu)
            return RecoverabilityLevel::Poor;

        // 按 sector 大小估算最大扇区数量，目录扇区索引不应超出这个范围太多
        uint64_t sectorSize = 1ull << sectorShift;
        if (sectorSize == 0) return RecoverabilityLevel::Poor;
        uint64_t maxSectors = node.size / sectorSize;
        if (maxSectors > 0 && dirSector > maxSectors + 4)
            return RecoverabilityLevel::Poor;

        return RecoverabilityLevel::Good;
    }

    // RTF：检查 "{\\rtf" 头是否存在。
    if (extLower == L"rtf") {
        if (node.size < 5) return RecoverabilityLevel::Poor;
        uint8_t hdr[5] = {};
        if (!reader.ReadBytes(node.rawOffset, hdr, sizeof(hdr)))
            return RecoverabilityLevel::Unknown;
        const uint8_t rtfMagic[5] = { '{','\\','r','t','f' };
        if (memcmp(hdr, rtfMagic, sizeof(rtfMagic)) != 0)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // ─────────────────────────────────────────────────────────────────────
    // 3) 压缩/镜像格式优先级（RAR/7Z/GZ/BZ2/XZ/ISO 等）
    // ─────────────────────────────────────────────────────────────────────

    // RAR：只要 header 正常且文件长度>header，即认为结构大致可恢复。
    if (extLower == L"rar") {
        if (node.size < sig.header.size() + 4)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // 7z：检查 header 固定字段（版本与 header CRC 区不为全零）。
    if (extLower == L"7z") {
        if (node.size < 32) return RecoverabilityLevel::Poor;
        uint8_t hdr[32] = {};
        if (!reader.ReadBytes(node.rawOffset, hdr, sizeof(hdr)))
            return RecoverabilityLevel::Unknown;
        // SignatureDB 已确保 "7z\xBC\xAF'\x1C" 序列，这里额外检查版本与 header CRC 区。
        if (hdr[6] == 0 || hdr[7] == 0) // 版本号不应全零
            return RecoverabilityLevel::Poor;
        bool allZero = true;
        for (int i = 12; i < 20; ++i) {
            if (hdr[i] != 0) { allZero = false; break; }
        }
        if (allZero)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // GZ/BZ2/XZ：流式压缩，难以快速验证，只要 header OK 就给 Good。
    if (extLower == L"gz" || extLower == L"bz2" || extLower == L"xz" ||
        extLower == L"zst" || extLower == L"lz4" || extLower == L"lzma") {
        if (node.size < sig.header.size() + 1)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // ISO9660：SignatureDB 在偏移 32769 检测 "CD001"，这里仅根据大小做一次 sanity check。
    if (extLower == L"iso") {
        if (node.size < 64 * 1024) // 太小几乎不可能是完整 ISO
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // ─────────────────────────────────────────────────────────────────────
    // 4) 视频/音频格式优先级（MP4/MOV/AVI/MKV/MPG/TS/MP3/WAV/FLAC 等）
    // ─────────────────────────────────────────────────────────────────────

    // MP4/M4V/M4A/3GP/MOV：ISO BMFF，校验第一个 box + 粗略查找 moov。
    if (extLower == L"mp4" || extLower == L"m4v" || extLower == L"m4a" ||
        extLower == L"3gp" || extLower == L"3g2" || extLower == L"mov") {
        if (node.size < 24) return RecoverabilityLevel::Poor;
        uint8_t buf24[24] = {};
        if (!reader.ReadBytes(node.rawOffset, buf24, sizeof(buf24)))
            return RecoverabilityLevel::Unknown;
        uint32_t boxSize =
            (buf24[0] << 24) | (buf24[1] << 16) | (buf24[2] << 8) | buf24[3];
        if (boxSize < 16 || boxSize > node.size)
            return RecoverabilityLevel::Poor;
        if (memcmp(buf24 + 4, "ftyp", 4) != 0)
            return RecoverabilityLevel::Poor;

        // 进一步在文件前 1 MB 内粗略查找 "moov" box，避免严重截断仍被标记为 Good。
        uint64_t winSize = std::min<uint64_t>(1024 * 1024, node.size);
        if (winSize >= 16) {
            std::vector<uint8_t> win(static_cast<size_t>(winSize));
            if (reader.ReadBytes(node.rawOffset, win.data(), static_cast<uint32_t>(winSize))) {
                size_t off = 0;
                while (off + 8 <= winSize) {
                    uint32_t sz =
                        (uint32_t(win[off])   << 24) |
                        (uint32_t(win[off+1]) << 16) |
                        (uint32_t(win[off+2]) << 8)  |
                        (uint32_t(win[off+3]));
                    const uint8_t* tp = win.data() + off + 4;
                    if (tp[0]=='m' && tp[1]=='o' && tp[2]=='o' && tp[3]=='v') {
                        if (sz >= 8 && static_cast<uint64_t>(off) + sz <= node.size + 0x10000)
                            return RecoverabilityLevel::Good;
                        break; // moov box size 看起来完全不对，当作失败
                    }
                    if (sz < 8) { ++off; continue; }
                    off += sz;
                }
                // 未在前 1 MB 里找到 moov，可能严重截断
                return RecoverabilityLevel::Poor;
            }
        }
        // header OK，但窗口读取失败，保守认为 Good。
        return RecoverabilityLevel::Good;
    }

    // AVI / WAV / RIFF 容器：使用 RIFF 结构的长度字段 + formType 做一次范围校验。
    if (extLower == L"avi" || extLower == L"wav" || extLower == L"riff") {
        if (node.size < 12) return RecoverabilityLevel::Poor;
        uint8_t hdr[12] = {};
        if (!reader.ReadBytes(node.rawOffset, hdr, sizeof(hdr)))
            return RecoverabilityLevel::Unknown;
        if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F')
            return RecoverabilityLevel::Poor;
        uint32_t riffSize =
            static_cast<uint32_t>(hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24));
        if (riffSize + 8 > node.size + 0x10000) // 允许一点点截断
            return RecoverabilityLevel::Poor;
        // formType = "AVI " / "WAVE" / 其他
        const uint8_t* form = hdr + 8;
        if (extLower == L"avi" && memcmp(form, "AVI ", 4) != 0)
            return RecoverabilityLevel::Poor;
        if (extLower == L"wav" && memcmp(form, "WAVE", 4) != 0)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // MKV / WebM：EBML header 存在即可，进一步结构解析代价较高。
    if (extLower == L"mkv" || extLower == L"webm") {
        if (node.size < sig.header.size() + 4)
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // MPEG-PS/ES (mpg) 与 TS/mts：header 已由 SignatureDB 保证；这里仅基于长度。
    if (extLower == L"mpg" || extLower == L"mpeg" ||
        extLower == L"ts"  || extLower == L"mts") {
        if (node.size < 188 * 10) // 少于 10 个 TS 包/极小流视为可疑
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // MP3/MP2：ID3 tag 或 frame sync 由 SignatureDB header 确认；再做一个帧头粗验。
    if (extLower == L"mp3") {
        if (node.size < 4) return RecoverabilityLevel::Poor;
        uint8_t h[4] = {};
        if (!reader.ReadBytes(node.rawOffset, h, sizeof(h)))
            return RecoverabilityLevel::Unknown;
        // ID3 header 或帧同步 0xFFEx
        if (memcmp(h, "ID3", 3) == 0)
            return RecoverabilityLevel::Good;
        if (h[0] == 0xFF && (h[1] & 0xE0) == 0xE0)
            return RecoverabilityLevel::Good;
        return RecoverabilityLevel::Poor;
    }

    // FLAC：fLaC 头已由 SignatureDB 保证；此处简单认为可恢复。
    if (extLower == L"flac")
        return RecoverabilityLevel::Good;

    // OGG / OGV / OGA / OPUS：OggS 头由 SignatureDB 保障；这里只做最小长度判断。
    if (extLower == L"ogg" || extLower == L"ogv" || extLower == L"oga") {
        if (node.size < 4096) // 非常小的 OGG 文件通常不完整
            return RecoverabilityLevel::Poor;
        return RecoverabilityLevel::Good;
    }

    // WAV（专项逻辑已在 RIFF 分支覆盖），这里保持 Good。
    if (extLower == L"wav")
        return RecoverabilityLevel::Good;

    // 默认：保持之前的“Raw = contiguous bytes → Good”策略，
    // 但如果文件太小，则退回 Unknown。
    if (node.size == 0)
        return RecoverabilityLevel::Unknown;

    return RecoverabilityLevel::Good;
}
