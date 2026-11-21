#include "PartitionProbe.h"
#include "../common/Platform.h"
#include "../log/Logger.h"
#include "../common/Utils.h"

#include <algorithm>
#include <unordered_set>
#include <cstring>

namespace {

inline uint16_t LE16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t LE32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24)); }
inline uint64_t LE64(const uint8_t* p) {
    uint64_t lo = LE32(p);
    uint64_t hi = LE32(p + 4);
    return lo | (hi << 32);
}

static bool SafeMulU64(uint64_t a, uint64_t b, uint64_t& out)
{
    if (a != 0 && b > UINT64_MAX / a)
        return false;
    out = a * b;
    return true;
}

static bool FitsDevice(uint64_t totalSectors,
                       uint64_t startSector,
                       uint64_t sectorCount)
{
    return totalSectors > 0 &&
           startSector < totalSectors &&
           sectorCount > 0 &&
           sectorCount <= totalSectors - startSector;
}

static bool AddNoOverflow(uint64_t a, uint64_t b, uint64_t& out)
{
    if (b > UINT64_MAX - a)
        return false;
    out = a + b;
    return true;
}

// ext2 / ext3 / ext4 share magic 0xEF53; distinguish via rev_level + journal feature.
static std::wstring ExtFamilyLabelFromSuperblock(const uint8_t* sb)
{
    const uint32_t revLevel = LE32(sb + 0x4C);
    const uint32_t featRo   = LE32(sb + 0x3C);
    constexpr uint32_t kHasJournal = 0x4u;
    if (revLevel == 0)
        return L"ext2";
    if (featRo & kHasJournal)
        return L"ext3";
    return L"ext4";
}

static bool Has55AA(const uint8_t* sec, uint32_t sectorSize)
{
    if (sectorSize < 512) return false;
    return sec[510] == 0x55 && sec[511] == 0xAA;
}

struct MbrPart {
    uint8_t  type = 0;
    uint32_t lbaStart = 0;
    uint32_t lbaCount = 0;
};

static bool ParseMbrParts(const uint8_t* sec, std::vector<MbrPart>& out)
{
    out.clear();
    if (!Has55AA(sec, 512)) return false;
    const uint8_t* pt = sec + 446;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = pt + i * 16;
        uint8_t type = e[4];
        uint32_t start = LE32(e + 8);
        uint32_t count = LE32(e + 12);
        if (type == 0x00 || count == 0) continue;
        // Skip extended partitions; we do signature scan anyway.
        if (type == 0x05 || type == 0x0F || type == 0x85) continue;
        out.push_back(MbrPart{ type, start, count });
    }
    return true;
}

// GPT header at LBA1: "EFI PART"
static bool IsGptHeader(const uint8_t* sec, uint32_t sectorSize)
{
    (void)sectorSize;
    return std::memcmp(sec, "EFI PART", 8) == 0;
}

} // namespace

void PartitionProbe::AddOrMerge(std::vector<PartitionCandidate>& v, const PartitionCandidate& c)
{
    for (auto& it : v) {
        // 完全同起点：认为是同一分区，直接合并信息
        if (it.startSector == c.startSector) {
            if (it.totalSectors == 0 && c.totalSectors > 0) it.totalSectors = c.totalSectors;
            if (it.sizeBytes == 0 && c.sizeBytes > 0)       it.sizeBytes   = c.sizeBytes;
            if (it.fsType == L"Unknown" && c.fsType != L"Unknown") it.fsType = c.fsType;
            if (c.confidence > it.confidence) it.confidence = c.confidence;
            if (!c.source.empty() && it.source.find(c.source) == std::wstring::npos) {
                if (!it.source.empty()) it.source += L"+";
                it.source += c.source;
            }
            return;
        }

        // 起点非常接近且大小接近：认为是同一分区的不同猜测，也进行合并
        uint64_t diffStart = (it.startSector > c.startSector)
                             ? (it.startSector - c.startSector)
                             : (c.startSector - it.startSector);
        uint64_t baseSize  = it.sizeBytes ? it.sizeBytes : c.sizeBytes;
        uint64_t diffSize  = (it.sizeBytes > c.sizeBytes)
                             ? (it.sizeBytes - c.sizeBytes)
                             : (c.sizeBytes - it.sizeBytes);

        bool fsCompatible =
            (it.fsType == c.fsType) ||
            it.fsType == L"Unknown" ||
            c.fsType == L"Unknown";

        if (fsCompatible &&
            baseSize > 0 &&
            diffStart < 2048 &&                 // 起点差 < 2048 扇区（1MB 以内）
            diffSize <= baseSize / 20)          // 大小差 <= 5%，避免 diffSize*100 溢出
        {
            // 合并为一条，信息取“更好”的那一方
            if (it.totalSectors == 0 && c.totalSectors > 0) it.totalSectors = c.totalSectors;
            if (it.sizeBytes == 0 && c.sizeBytes > 0)       it.sizeBytes   = c.sizeBytes;
            if (it.fsType == L"Unknown" && c.fsType != L"Unknown") it.fsType = c.fsType;
            if (c.confidence > it.confidence) it.confidence = c.confidence;
            if (!c.source.empty() && it.source.find(c.source) == std::wstring::npos) {
                if (!it.source.empty()) it.source += L"+";
                it.source += c.source;
            }
            // 起点取更小的那个，以避免丢失最保守的边界
            if (c.startSector < it.startSector) it.startSector = c.startSector;
            return;
        }
    }
    v.push_back(c);
}

bool PartitionProbe::ReadSector(SectorReader& r, uint64_t lba, std::vector<uint8_t>& out)
{
    const uint32_t sectorSize = r.GetSectorSize();
    if (!r.IsOpen() || sectorSize < 512 ||
        !FitsDevice(r.GetTotalSectors(), lba, 1)) {
        out.clear();
        return false;
    }
    out.resize(sectorSize);
    return r.ReadSectors(lba, out.data(), 1);
}

bool PartitionProbe::DetectByVbr(const uint8_t* sec, uint32_t sectorSize,
                                 std::wstring& outFs, uint64_t& outTotalSectors)
{
    outFs = L"Unknown";
    outTotalSectors = 0;
    if (!sec || sectorSize < 512) return false;
    if (!Has55AA(sec, sectorSize)) return false;

    // NTFS: OEM at offset 3 = "NTFS    "
    if (std::memcmp(sec + 3, "NTFS    ", 8) == 0) {
        outFs = L"NTFS";
        // Total sectors at offset 40 (8 bytes)
        outTotalSectors = LE64(sec + 40);
        return true;
    }

    // exFAT: OEM at offset 3 = "EXFAT   "
    if (std::memcmp(sec + 3, "EXFAT   ", 8) == 0) {
        outFs = L"exFAT";
        // exFAT: volume length in sectors at offset 72 (8 bytes)
        outTotalSectors = LE64(sec + 72);
        return true;
    }

    // FAT: check "FAT16   " at offset 54, "FAT32   " at offset 82
    if (std::memcmp(sec + 54, "FAT16   ", 8) == 0 || std::memcmp(sec + 54, "FAT12   ", 8) == 0) {
        outFs = L"FAT16";
        uint16_t tot16 = LE16(sec + 19);
        uint32_t tot32 = LE32(sec + 32);
        outTotalSectors = tot16 ? tot16 : tot32;
        return true;
    }
    if (std::memcmp(sec + 82, "FAT32   ", 8) == 0) {
        outFs = L"FAT32";
        uint16_t tot16 = LE16(sec + 19);
        uint32_t tot32 = LE32(sec + 32);
        outTotalSectors = tot16 ? tot16 : tot32;
        return true;
    }

    return false;
}

bool PartitionProbe::DetectExt4(SectorReader& r, uint64_t startSector, uint32_t sectorSize,
                                uint64_t& outTotalSectors, std::wstring* outFsType)
{
    outTotalSectors = 0;
    if (!r.IsOpen() || sectorSize < 512 ||
        !FitsDevice(r.GetTotalSectors(), startSector, 1))
        return false;

    // ext4 superblock at offset 1024 bytes from start
    uint64_t partBase = 0;
    uint64_t sbOff = 0;
    if (!SafeMulU64(startSector, static_cast<uint64_t>(sectorSize), partBase) ||
        !AddNoOverflow(partBase, 1024, sbOff))
        return false;

    uint8_t sb[2048]{};
    const uint64_t rawRead = static_cast<uint64_t>(sectorSize) * 2;
    const uint32_t toRead = static_cast<uint32_t>(
        (std::min)(rawRead, static_cast<uint64_t>(sizeof(sb))));
    if (sbOff > r.GetTotalBytes() ||
        static_cast<uint64_t>(toRead) > r.GetTotalBytes() - sbOff)
        return false;
    if (!r.ReadBytes(sbOff, sb, toRead)) return false;
    // magic at offset 0x38 (56) in superblock
    uint16_t magic = LE16(sb + 0x38);
    if (magic != 0xEF53)
        return false;

    // Compute total blocks: s_blocks_count_lo (0x04) + hi (0x150)
    uint64_t blocks = (uint64_t)LE32(sb + 0x04);
    // hi exists for ext4; safe read if buffer is big enough
    if (toRead >= 0x154)
        blocks |= (uint64_t)LE32(sb + 0x150) << 32;

    uint32_t logBlock = LE32(sb + 0x18);
    uint64_t blockSize = 1024ull << logBlock;
    if (blockSize == 0) return false;

    uint64_t totalBytes = 0;
    if (!SafeMulU64(blocks, blockSize, totalBytes))
        return false;
    outTotalSectors = totalBytes / sectorSize;
    if (outTotalSectors == 0)
        return false;
    if (outFsType)
        *outFsType = ExtFamilyLabelFromSuperblock(sb);
    return true;
}

bool PartitionProbe::DetectUfs1(SectorReader& r, uint64_t startSector, uint32_t sectorSize)
{
    if (!r.IsOpen() || sectorSize < 512 ||
        !FitsDevice(r.GetTotalSectors(), startSector, 1))
        return false;

    uint64_t partBase = 0;
    if (!SafeMulU64(startSector, static_cast<uint64_t>(sectorSize), partBase))
        return false;
    const uint64_t sbOffsets[] = {8192, 65536};

    for (uint64_t sbRel : sbOffsets) {
        uint8_t sb[1600]{};
        uint64_t sbOff = 0;
        if (!AddNoOverflow(partBase, sbRel, sbOff) ||
            sbOff > r.GetTotalBytes() ||
            sizeof(sb) > r.GetTotalBytes() - sbOff)
            continue;
        if (!r.ReadBytes(sbOff, sb, sizeof(sb)))
            continue;
        if (sizeof(sb) < 0x560)
            continue;
        const uint32_t magic = LE32(sb + 0x55c);
        if (magic == 0x011954u || magic == 0x19560119u || magic == 0x19540119u)
            return true;
    }
    return false;
}

std::vector<PartitionCandidate> PartitionProbe::DetectAllFileSystems(
    SectorReader& reader, uint64_t startSector, uint32_t sectorSize)
{
    std::vector<PartitionCandidate> out;
    if (!reader.IsOpen() || sectorSize == 0)
        return out;

    std::vector<uint8_t> sec;
    if (ReadSector(reader, startSector, sec) && sec.size() >= 512) {
        std::wstring fs;
        uint64_t vbrSectors = 0;
        if (DetectByVbr(sec.data(), sectorSize, fs, vbrSectors)) {
            PartitionCandidate c{};
            c.startSector  = startSector;
            c.totalSectors = vbrSectors;
            if (vbrSectors == 0 ||
                !FitsDevice(reader.GetTotalSectors(), startSector, vbrSectors) ||
                !SafeMulU64(vbrSectors, static_cast<uint64_t>(sectorSize), c.sizeBytes))
                c.sizeBytes = 0;
            c.fsType       = fs;
            c.source       = L"VBR";
            c.confidence   = 95;
            out.push_back(c);
        }
    }

    uint64_t extSectors = 0;
    std::wstring extFs;
    if (DetectExt4(reader, startSector, sectorSize, extSectors, &extFs)) {
        PartitionCandidate c{};
        c.startSector  = startSector;
        c.totalSectors = extSectors;
        if (SafeMulU64(extSectors, static_cast<uint64_t>(sectorSize), c.sizeBytes)) {
            c.fsType       = extFs.empty() ? L"ext4" : extFs;
            c.source       = L"Superblock";
            c.confidence   = 90;
            out.push_back(c);
        }
    }

    if (DetectUfs1(reader, startSector, sectorSize)) {
        PartitionCandidate c{};
        c.startSector = startSector;
        c.fsType      = L"UFS1";
        c.source      = L"Superblock";
        c.confidence  = 88;
        out.push_back(c);
    }

    return out;
}

Partition PartitionProbe::ToPartition(const PartitionCandidate& c, int physicalDiskIdx)
{
    Partition p;
    p.physicalDiskIdx = physicalDiskIdx;
    p.startSector = c.startSector;
    p.totalSectors = c.totalSectors;
    p.sizeBytes = c.sizeBytes;
    p.fsType = c.fsType;
    p.driveLetter.clear();
    p.label.clear();
    p.isActive = false;
    p.isSystem = false;
    return p;
}

std::vector<PartitionCandidate> PartitionProbe::ProbeVolume(
    SectorReader& reader,
    const Options& opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress)
{
    std::vector<PartitionCandidate> out;

    if (!reader.IsOpen()) {
        LOG_ERROR(L"PartitionProbe::ProbeVolume: reader not open");
        return out;
    }

    const bool doTable =
        opt.mode == ProbeMode::Full || opt.mode == ProbeMode::TableOnly;
    const bool doSignature =
        opt.mode == ProbeMode::Full || opt.mode == ProbeMode::LostOnly;

    uint32_t secSize = reader.GetSectorSize();
    uint64_t totalSectors = reader.GetTotalSectors();

    if (secSize < 512 || totalSectors == 0) {
        LOG_ERROR(L"PartitionProbe::ProbeVolume: invalid reader geometry"
                  L" sectorSize=" + std::to_wstring(secSize) +
                  L" totalSectors=" + std::to_wstring(totalSectors));
        return out;
    }

    uint64_t scanStart = opt.scanStartSector;
    uint64_t scanEnd = opt.scanEndSector == 0 ? totalSectors : (std::min)(opt.scanEndSector, totalSectors);
    if (scanStart >= scanEnd) return out;

    std::vector<uint8_t> sec;

    // 1) MBR at LBA0
    if (doTable && ReadSector(reader, 0, sec) && sec.size() >= 512 && Has55AA(sec.data(), secSize)) {
        std::vector<MbrPart> parts;
        if (ParseMbrParts(sec.data(), parts)) {
            for (const auto& p : parts) {
                PartitionCandidate c{};
                c.startSector = p.lbaStart;
                c.totalSectors = p.lbaCount;
                if (!FitsDevice(totalSectors, c.startSector, c.totalSectors))
                    continue;
                if (!SafeMulU64(c.totalSectors, static_cast<uint64_t>(secSize), c.sizeBytes))
                    continue;
                c.source = L"MBR";
                c.confidence = 90;

                std::wstring fs;
                uint64_t vbrSectors = 0;
                if (ReadSector(reader, c.startSector, sec) &&
                    DetectByVbr(sec.data(), secSize, fs, vbrSectors)) {
                    c.fsType = fs;
                    if (vbrSectors > 0 && vbrSectors <= c.totalSectors) {
                        c.totalSectors = vbrSectors;
                        c.sizeBytes = c.totalSectors * (uint64_t)secSize;
                        c.confidence = 95;
                    }
                } else {
                    // ext4 not in VBR
                    uint64_t extSectors = 0;
                    if (DetectExt4(reader, c.startSector, secSize, extSectors, &c.fsType)) {
                        if (c.fsType.empty())
                            c.fsType = L"ext4";
                        if (extSectors > 0 && extSectors <= c.totalSectors) {
                            c.totalSectors = extSectors;
                            c.sizeBytes = c.totalSectors * (uint64_t)secSize;
                        }
                        c.confidence = 92;
                    } else {
                        c.fsType = L"Unknown";
                    }
                }
                AddOrMerge(out, c);
            }
        }
    }

    // 2) GPT header at LBA1 and entries
    if (doTable && ReadSector(reader, 1, sec) && sec.size() >= 92 && IsGptHeader(sec.data(), secSize)) {
        uint64_t partEntryLba = LE64(sec.data() + 72);
        uint32_t numEntries = LE32(sec.data() + 80);
        uint32_t entrySize  = LE32(sec.data() + 84);

        // Basic sanity
        if (partEntryLba > 0 && numEntries > 0 && entrySize >= 128 && entrySize <= 1024) {
            uint64_t bytes = 0;
            if (!SafeMulU64(static_cast<uint64_t>(numEntries), entrySize, bytes))
                return out;
            uint64_t sectorsToRead64 =
                (bytes + secSize - 1) / secSize;
            // Cap read to avoid huge allocations
            uint32_t sectorsToRead = static_cast<uint32_t>(
                (std::min)(sectorsToRead64, static_cast<uint64_t>(128)));

            if (!FitsDevice(totalSectors, partEntryLba, sectorsToRead))
                return out;

            std::vector<uint8_t> entries;
            entries.resize((size_t)sectorsToRead * secSize);
            if (reader.ReadSectors(partEntryLba, entries.data(), sectorsToRead)) {
                for (uint32_t i = 0; i < numEntries; ++i) {
                    size_t off = (size_t)i * entrySize;
                    if (off + 56 > entries.size()) break;
                    const uint8_t* e = entries.data() + off;
                    // type GUID empty?
                    bool allZero = true;
                    for (int k = 0; k < 16; ++k) if (e[k] != 0) { allZero = false; break; }
                    if (allZero) continue;

                    uint64_t firstLba = LE64(e + 32);
                    uint64_t lastLba  = LE64(e + 40);
                    if (firstLba == 0 || lastLba < firstLba) continue;

                    PartitionCandidate c{};
                    c.startSector = firstLba;
                    c.totalSectors = (lastLba - firstLba + 1);
                    if (!FitsDevice(totalSectors, c.startSector, c.totalSectors) ||
                        !SafeMulU64(c.totalSectors, static_cast<uint64_t>(secSize), c.sizeBytes))
                        continue;
                    c.source = L"GPT";
                    c.confidence = 95;

                    std::wstring fs;
                    uint64_t vbrSectors = 0;
                    if (ReadSector(reader, c.startSector, sec) &&
                        DetectByVbr(sec.data(), secSize, fs, vbrSectors)) {
                        c.fsType = fs;
                        if (vbrSectors > 0 && vbrSectors <= c.totalSectors) {
                            c.totalSectors = vbrSectors;
                            c.sizeBytes = c.totalSectors * (uint64_t)secSize;
                        }
                    } else {
                        uint64_t extSectors = 0;
                        if (DetectExt4(reader, c.startSector, secSize, extSectors, &c.fsType)) {
                            if (c.fsType.empty())
                                c.fsType = L"ext4";
                            if (extSectors > 0 && extSectors <= c.totalSectors) {
                                c.totalSectors = extSectors;
                                c.sizeBytes = c.totalSectors * (uint64_t)secSize;
                            }
                        } else {
                            c.fsType = L"Unknown";
                        }
                    }
                    AddOrMerge(out, c);
                }
            }
        }
    }

    // 3) Signature scan (common 1MiB alignment boundaries) — lost partition recovery
    if (doSignature && opt.includeSignatureScan) {
        uint64_t stride = opt.signatureStrideSectors ? opt.signatureStrideSectors : 2048;
        const uint64_t scanSpan = scanEnd - scanStart;
        uint64_t totalSteps = scanSpan / stride + (scanSpan % stride ? 1 : 0);
        if (totalSteps == 0) totalSteps = 1;

        uint64_t stepIdx = 0;
        for (uint64_t lba = scanStart; lba < scanEnd; lba += stride) {
            if (cancelFlag && cancelFlag->load()) break;

            ++stepIdx;
            if (onProgress) {
                int pct = (int)(stepIdx * 100 / totalSteps);
                onProgress((std::min)(pct, 100));
            }

            if (!ReadSector(reader, lba, sec)) continue;

            std::wstring fs;
            uint64_t vbrSectors = 0;
            if (DetectByVbr(sec.data(), secSize, fs, vbrSectors)) {
                PartitionCandidate c{};
                c.startSector = lba;
                c.totalSectors = vbrSectors;
                if (c.totalSectors > 0 &&
                    (!FitsDevice(totalSectors, c.startSector, c.totalSectors) ||
                     !SafeMulU64(c.totalSectors, static_cast<uint64_t>(secSize), c.sizeBytes)))
                    continue;
                c.fsType = fs;
                c.source = L"Signature";
                c.confidence = 80;
                if (fs == L"NTFS" || fs == L"exFAT") c.confidence = 85;
                if (c.totalSectors == 0) {
                    c.totalSectors = 0;
                    c.sizeBytes = 0;
                    c.confidence = 70;
                }
                AddOrMerge(out, c);
                continue;
            }

            uint64_t extSectors = 0;
            PartitionCandidate c{};
            if (DetectExt4(reader, lba, secSize, extSectors, &c.fsType)) {
                c.startSector = lba;
                c.totalSectors = extSectors;
                if (!FitsDevice(totalSectors, c.startSector, c.totalSectors) ||
                    !SafeMulU64(c.totalSectors, static_cast<uint64_t>(secSize), c.sizeBytes))
                    continue;
                if (c.fsType.empty())
                    c.fsType = L"ext4";
                c.source = L"Signature";
                c.confidence = 82;
                AddOrMerge(out, c);
                continue;
            }
        }
    }

    // Sort: confidence desc, startSector asc
    std::sort(out.begin(), out.end(), [](const PartitionCandidate& a, const PartitionCandidate& b) {
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        return a.startSector < b.startSector;
    });

    return out;
}

std::vector<PartitionCandidate> PartitionProbe::ProbePhysicalDisk(
    int physicalDiskIdx,
    const Options& opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress)
{
    SectorReader reader;
    if (!reader.Open(physicalDiskIdx)) {
        LOG_ERROR(L"PartitionProbe: cannot open disk " + std::to_wstring(physicalDiskIdx));
        return {};
    }
    return ProbeVolume(reader, opt, cancelFlag, onProgress);
}

