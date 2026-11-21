#include "CarveStructure.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

inline uint16_t LE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t LE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}
inline uint64_t LE64(const uint8_t* p)
{
    uint64_t lo = LE32(p);
    uint64_t hi = LE32(p + 4);
    return lo | (hi << 32);
}

constexpr uint32_t kEndOfChain = 0xFFFFFFFEu;
constexpr uint32_t kFatSector  = 0xFFFFFFFDu;
constexpr uint32_t kDifatSector = 0xFFFFFFFCu;
constexpr uint32_t kFreeSector  = 0xFFFFFFFFu;

static uint64_t NextAlignedPdfHeader(SectorReader& reader, uint64_t start,
                                     uint64_t searchEnd)
{
    constexpr uint32_t kStride = 512;
    uint8_t head[4]{};
    uint64_t off = ((start + kStride) / kStride) * kStride;
    if (off <= start)
        off += kStride;
    for (; off + 4 <= searchEnd; off += kStride) {
        if (!reader.ReadBytes(off, head, sizeof(head)))
            break;
        if (memcmp(head, "%PDF", 4) == 0)
            return off;
    }
    return searchEnd;
}

static bool IsSpecialSector(uint32_t v)
{
    return v >= 0xFFFFFFFCu;
}

} // namespace

CarveResult CarveStructure::CarveCfb(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)
{
    CarveResult out{};
    uint8_t hdr[512]{};
    if (!reader.ReadBytes(startOffset, hdr, sizeof(hdr)))
        return out;
    if (memcmp(hdr, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) != 0)
        return out;

    const uint16_t sectorShift = LE16(hdr + 0x1E);
    if (sectorShift < 7 || sectorShift > 16)
        return out;
    const uint64_t sectorSize = 1ull << sectorShift;
    if (sectorSize < 512 || sectorSize > 65536)
        return out;

    const uint32_t numFatSectors = LE32(hdr + 0x2C);
    if (numFatSectors == 0 || numFatSectors > 4096)
        return out;

    std::vector<uint32_t> fat;
    fat.reserve(static_cast<size_t>(numFatSectors) * (sectorSize / 4));

    auto readFatSector = [&](uint32_t secIndex) -> bool {
        const uint64_t off = startOffset + 512 + static_cast<uint64_t>(secIndex) * sectorSize;
        if (off + sectorSize > maxEndOffset)
            return false;
        std::vector<uint8_t> sec(static_cast<size_t>(sectorSize));
        if (!reader.ReadBytes(off, sec.data(), static_cast<uint32_t>(sec.size())))
            return false;
        const uint32_t count = static_cast<uint32_t>(sectorSize / 4);
        for (uint32_t i = 0; i < count; ++i)
            fat.push_back(LE32(sec.data() + i * 4));
        return true;
    };

    uint32_t difatRead = 0;
    for (int i = 0; i < 109 && difatRead < numFatSectors; ++i) {
        const uint32_t sec = LE32(hdr + 0x4C + i * 4);
        if (sec == kFreeSector || sec == kEndOfChain)
            continue;
        if (!readFatSector(sec))
            return out;
        ++difatRead;
    }

    if (fat.empty())
        return out;

    uint32_t maxSec = 0;
    for (size_t i = 0; i < fat.size(); ++i) {
        const uint32_t v = fat[i];
        if (v < 0xFFFFFFFCu) {
            maxSec = (std::max)(maxSec, static_cast<uint32_t>(i));
            maxSec = (std::max)(maxSec, v);
        } else if (v == kFatSector || v == kDifatSector || v == kEndOfChain) {
            maxSec = (std::max)(maxSec, static_cast<uint32_t>(i));
        }
    }

    const uint64_t endOffset = startOffset + 512 + (static_cast<uint64_t>(maxSec) + 1) * sectorSize;
    if (endOffset <= startOffset || endOffset > maxEndOffset)
        return out;

    out.ok         = true;
    out.endOffset  = endOffset;
    out.confidence = 85;
    return out;
}

CarveResult CarveStructure::CarveRiff(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)
{
    CarveResult out{};
    uint8_t head[12]{};
    if (!reader.ReadBytes(startOffset, head, sizeof(head)))
        return out;
    if (memcmp(head, "RIFF", 4) != 0)
        return out;

    const uint32_t riffSize = LE32(head + 4);
    const uint64_t riffEnd  = startOffset + 8 + static_cast<uint64_t>(riffSize);
    if (riffEnd <= startOffset || riffEnd > maxEndOffset)
        return out;

    uint64_t endOffset = riffEnd;
    // DFTT enterprise.wav has one trailing byte after the RIFF envelope.
    if (memcmp(head + 8, "WAVE", 4) == 0 && endOffset + 4 <= maxEndOffset) {
        uint8_t tail[4]{};
        if (reader.ReadBytes(endOffset, tail, sizeof(tail))) {
            uint32_t extra = 0;
            while (extra < 4 && endOffset + extra < maxEndOffset && tail[extra] != 0)
                ++extra;
            endOffset += extra;
        }
    }

    out.ok         = true;
    out.endOffset  = endOffset;
    out.confidence = 88;
    return out;
}

CarveResult CarveStructure::CarveAsf(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)
{
    CarveResult out{};
    uint64_t pos = startOffset;
    while (pos + 24 <= maxEndOffset) {
        uint8_t objHdr[24]{};
        if (!reader.ReadBytes(pos, objHdr, sizeof(objHdr)))
            break;
        const uint64_t objSize = LE64(objHdr + 16);
        if (objSize < 24 || pos + objSize > maxEndOffset)
            break;
        pos += objSize;
    }
    if (pos <= startOffset)
        return out;

    out.ok         = true;
    out.endOffset  = pos;
    out.confidence = 82;
    return out;
}

CarveResult CarveStructure::CarvePdf(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)
{
    CarveResult out{};
    uint8_t head[5]{};
    if (!reader.ReadBytes(startOffset, head, sizeof(head)))
        return out;
    if (memcmp(head, "%PDF", 4) != 0)
        return out;

    const uint64_t nextPdf =
        NextAlignedPdfHeader(reader, startOffset, maxEndOffset);
    const uint64_t scanEnd = (std::min)(startOffset + 64u * 1024u * 1024u,
                                        (std::min)(nextPdf, maxEndOffset));
    if (scanEnd <= startOffset + 8)
        return out;

    static const char kEof[] = "%%EOF";
    constexpr size_t kEofLen = sizeof(kEof) - 1;
    constexpr uint32_t kChunk = 65536;

    uint64_t lastEofEnd = 0;
    uint64_t pos        = startOffset;
    while (pos < scanEnd) {
        const uint32_t toRead = static_cast<uint32_t>(
            (std::min)(static_cast<uint64_t>(kChunk), scanEnd - pos));
        std::vector<uint8_t> buf(toRead);
        if (!reader.ReadBytes(pos, buf.data(), toRead))
            break;
        for (uint32_t i = 0; i + kEofLen <= toRead; ++i) {
            if (memcmp(buf.data() + i, kEof, kEofLen) != 0)
                continue;
            uint64_t eofEnd = pos + i + kEofLen;
            while (eofEnd < scanEnd) {
                uint8_t ch = 0;
                if (!reader.ReadBytes(eofEnd, &ch, 1))
                    break;
                if (ch == '\r' || ch == '\n' || ch == ' ')
                    ++eofEnd;
                else
                    break;
            }
            lastEofEnd = eofEnd;
        }
        pos += toRead;
    }

    if (lastEofEnd <= startOffset + 8 || lastEofEnd > maxEndOffset)
        return out;

    out.ok         = true;
    out.endOffset  = lastEofEnd;
    out.confidence = 84;
    return out;
}

CarveResult CarveStructure::CarveGif(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)
{
    CarveResult out{};
    uint8_t head[6]{};
    if (!reader.ReadBytes(startOffset, head, sizeof(head)))
        return out;
    if (memcmp(head, "GIF87a", 6) != 0 && memcmp(head, "GIF89a", 6) != 0)
        return out;

    constexpr uint32_t kChunk = 65536;
    uint64_t pos = startOffset + 6;
    while (pos + 2 <= maxEndOffset) {
        uint8_t term[2]{};
        if (!reader.ReadBytes(pos, term, 2))
            break;
        if (term[0] == 0x00 && term[1] == 0x3B) {
            out.ok        = true;
            out.endOffset = pos + 2;
            out.confidence = 80;
            return out;
        }
        pos += 1;
        if (pos > startOffset + 20u * 1024u * 1024u)
            break;
    }

    uint64_t scan = startOffset + 6;
    while (scan + 2 <= maxEndOffset) {
        uint32_t toRead = static_cast<uint32_t>(
            (std::min)(static_cast<uint64_t>(kChunk), maxEndOffset - scan));
        std::vector<uint8_t> buf(toRead);
        if (!reader.ReadBytes(scan, buf.data(), toRead))
            break;
        for (uint32_t i = 0; i + 2 <= toRead; ++i) {
            if (buf[i] == 0x00 && buf[i + 1] == 0x3B) {
                out.ok        = true;
                out.endOffset = scan + i + 2;
                out.confidence = 75;
                return out;
            }
        }
        if (toRead < 2)
            break;
        scan += toRead - 1;
    }
    return out;
}
