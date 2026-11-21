#include "CfbReader.h"

#include <algorithm>
#include <cstring>

namespace {

inline uint16_t LE16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t LE32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

constexpr uint32_t kEndOfChain  = 0xFFFFFFFEu;
constexpr uint32_t kFreeSector  = 0xFFFFFFFFu;
constexpr uint32_t kFatSector   = 0xFFFFFFFDu;
constexpr uint32_t kDifatSector = 0xFFFFFFFCu;

bool IsSpecialSector(uint32_t v)
{
    return v >= 0xFFFFFFFCu;
}

std::wstring ReadUtf16Name(const uint8_t* p, uint8_t nameBytes)
{
    std::wstring name;
    if (nameBytes < 2)
        return name;
    const size_t chars = nameBytes / 2;
    name.reserve(chars);
    for (size_t i = 0; i + 1 < chars; ++i) {
        const wchar_t ch = static_cast<wchar_t>(LE16(p + i * 2));
        if (ch == 0)
            break;
        name.push_back(ch);
    }
    return name;
}

bool ReadFat(const uint8_t* data, size_t len, uint64_t sectorSize, uint32_t numFatSectors,
             std::vector<uint32_t>& fatOut)
{
    if (numFatSectors == 0 || numFatSectors > 4096 || sectorSize < 512)
        return false;

    fatOut.clear();
    fatOut.reserve(static_cast<size_t>(numFatSectors) * (sectorSize / 4));

    auto appendFatSector = [&](uint32_t secIndex) -> bool {
        const uint64_t off = 512 + static_cast<uint64_t>(secIndex) * sectorSize;
        if (off + sectorSize > len)
            return false;
        const uint32_t count = static_cast<uint32_t>(sectorSize / 4);
        for (uint32_t i = 0; i < count; ++i)
            fatOut.push_back(LE32(data + off + i * 4));
        return true;
    };

    uint32_t read = 0;
    for (int i = 0; i < 109 && read < numFatSectors; ++i) {
        const uint32_t sec = LE32(data + 0x4C + i * 4);
        if (sec == kFreeSector || sec == kEndOfChain)
            continue;
        if (!appendFatSector(sec))
            return false;
        ++read;
    }

    uint32_t difatStart = LE32(data + 0x48);
    while (read < numFatSectors && difatStart != kEndOfChain && difatStart != kFreeSector) {
        const uint64_t off = 512 + static_cast<uint64_t>(difatStart) * sectorSize;
        if (off + sectorSize > len)
            break;
        const uint32_t count = static_cast<uint32_t>(sectorSize / 4);
        for (uint32_t i = 0; i + 1 < count && read < numFatSectors; ++i) {
            const uint32_t sec = LE32(data + off + i * 4);
            if (sec == kFreeSector || sec == kEndOfChain)
                continue;
            if (!appendFatSector(sec))
                return false;
            ++read;
        }
        difatStart = LE32(data + off + (count - 1) * 4);
    }

    return !fatOut.empty();
}

bool ReadMiniFat(const uint8_t* data, size_t len, uint64_t sectorSize,
                 const std::vector<uint32_t>& fat, uint32_t miniFatStart,
                 uint32_t numMiniFatSectors, std::vector<uint32_t>& miniFatOut)
{
    if (miniFatStart == kEndOfChain || miniFatStart == kFreeSector || numMiniFatSectors == 0)
        return true;

    miniFatOut.clear();
    miniFatOut.reserve(static_cast<size_t>(numMiniFatSectors) * (sectorSize / 4));

    uint32_t sec = miniFatStart;
    for (uint32_t guard = 0; guard < numMiniFatSectors + 8 && sec != kEndOfChain; ++guard) {
        if (sec >= fat.size() || IsSpecialSector(sec))
            break;
        const uint64_t off = 512 + static_cast<uint64_t>(sec) * sectorSize;
        if (off + sectorSize > len)
            break;
        const uint32_t count = static_cast<uint32_t>(sectorSize / 4);
        for (uint32_t i = 0; i < count; ++i)
            miniFatOut.push_back(LE32(data + off + i * 4));
        sec = fat[sec];
    }
    return true;
}

std::vector<uint8_t> ReadChain(const uint8_t* data, size_t len, uint64_t sectorSize,
                               const std::vector<uint32_t>& fat, uint32_t startSector,
                               uint64_t byteCount)
{
    std::vector<uint8_t> out;
    if (startSector == kEndOfChain || startSector == kFreeSector || byteCount == 0)
        return out;
    out.reserve(static_cast<size_t>((std::min)(byteCount, size_t{16 * 1024 * 1024})));

    uint32_t sec = startSector;
    for (uint32_t guard = 0; guard < fat.size() + 8 && out.size() < byteCount; ++guard) {
        if (sec >= fat.size() || IsSpecialSector(sec))
            break;
        const uint64_t off = 512 + static_cast<uint64_t>(sec) * sectorSize;
        if (off + sectorSize > len)
            break;
        const size_t need = static_cast<size_t>(
            (std::min)(byteCount - out.size(), static_cast<uint64_t>(sectorSize)));
        out.insert(out.end(), data + off, data + off + need);
        sec = fat[sec];
    }
    return out;
}

std::vector<uint8_t> ReadMiniChain(const std::vector<uint8_t>& miniContainer,
                                   const std::vector<uint32_t>& miniFat, uint64_t miniSectorSize,
                                   uint32_t startMiniSector, uint64_t byteCount)
{
    std::vector<uint8_t> out;
    if (miniContainer.empty() || startMiniSector == kEndOfChain || byteCount == 0)
        return out;
    out.reserve(static_cast<size_t>((std::min)(byteCount, size_t{4 * 1024 * 1024})));

    uint32_t sec = startMiniSector;
    for (uint32_t guard = 0; guard < miniFat.size() + 8 && out.size() < byteCount; ++guard) {
        if (sec >= miniFat.size())
            break;
        const uint64_t off = static_cast<uint64_t>(sec) * miniSectorSize;
        if (off + miniSectorSize > miniContainer.size())
            break;
        const size_t need = static_cast<size_t>(
            (std::min)(byteCount - out.size(), miniSectorSize));
        out.insert(out.end(), miniContainer.data() + off,
                   miniContainer.data() + off + need);
        sec = miniFat[sec];
    }
    return out;
}

struct DirEntry
{
    std::wstring name;
    uint8_t      type       = 0;
    uint32_t     start      = 0;
    uint64_t     size       = 0;
    uint64_t     payloadOff = 0;
};

} // namespace

bool CfbReader::EnumerateStreams(const uint8_t* data, size_t len, const StreamCallback& onStream)
{
    if (!data || len < 512 || !onStream)
        return false;
    if (memcmp(data, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) != 0)
        return false;

    const uint16_t sectorShift = LE16(data + 0x1E);
    if (sectorShift < 7 || sectorShift > 16)
        return false;
    const uint64_t sectorSize = 1ull << sectorShift;

    const uint16_t miniShift = LE16(data + 0x20);
    if (miniShift < 6 || miniShift > 20)
        return false;
    const uint64_t miniSectorSize = 1ull << miniShift;

    const uint32_t numFatSectors = LE32(data + 0x30);
    const uint32_t dirStart      = LE32(data + 0x34);
    const uint32_t miniCutoff    = LE32(data + 0x3C);
    const uint32_t miniFatStart  = LE32(data + 0x40);
    const uint32_t numMiniFat    = LE32(data + 0x44);

    std::vector<uint32_t> fat;
    if (!ReadFat(data, len, sectorSize, numFatSectors, fat))
        return false;

    std::vector<uint32_t> miniFat;
    if (!ReadMiniFat(data, len, sectorSize, fat, miniFatStart, numMiniFat, miniFat))
        return false;

    const uint64_t maxDirBytes = sectorSize * 64;
    std::vector<uint8_t> dirBytes =
        ReadChain(data, len, sectorSize, fat, dirStart, maxDirBytes);
    if (dirBytes.size() < 128)
        return false;

    std::vector<DirEntry> entries;
    entries.reserve(dirBytes.size() / 128);
    for (size_t pos = 0; pos + 128 <= dirBytes.size(); pos += 128) {
        const uint8_t* e = dirBytes.data() + pos;
        const uint8_t nameLen = e[64];
        DirEntry entry;
        entry.name = ReadUtf16Name(e, nameLen);
        entry.type = e[66];
        entry.start = LE32(e + 116);
        entry.size  = LE32(e + 120);
        if (entry.type == 0)
            continue;
        entries.push_back(std::move(entry));
    }

    const DirEntry* root = nullptr;
    for (const DirEntry& e : entries) {
        if (e.type == 5) {
            root = &e;
            break;
        }
    }

    std::vector<uint8_t> miniContainer;
    if (root && root->size > 0) {
        miniContainer = ReadChain(data, len, sectorSize, fat, root->start, root->size);
    }

    bool visited = false;
    for (const DirEntry& entry : entries) {
        if (entry.type != 2 || entry.size == 0 || entry.name.empty())
            continue;

        std::vector<uint8_t> payload;
        uint64_t payloadOff = 0;
        if (entry.size >= miniCutoff || miniContainer.empty()) {
            payload   = ReadChain(data, len, sectorSize, fat, entry.start, entry.size);
            payloadOff = (entry.start == kEndOfChain || entry.start == kFreeSector)
                             ? 0
                             : 512 + static_cast<uint64_t>(entry.start) * sectorSize;
        } else {
            payload   = ReadMiniChain(miniContainer, miniFat, miniSectorSize, entry.start,
                                      entry.size);
            payloadOff = 0; // mini-stream offset not mapped to host file directly
        }

        if (payload.empty())
            continue;

        CfbReader::StreamInfo info;
        info.name          = entry.name;
        info.offsetInFile  = payloadOff;
        info.data          = payload.data();
        info.size          = payload.size();
        onStream(info);
        visited = true;
    }

    return visited;
}
