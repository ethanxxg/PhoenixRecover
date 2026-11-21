#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <atomic>

#include "Partition.h"
#include "SectorReader.h"

struct PartitionCandidate
{
    uint64_t startSector  = 0;
    uint64_t totalSectors = 0;
    uint64_t sizeBytes    = 0;
    std::wstring fsType;          // "NTFS" | "FAT32" | "FAT16" | "exFAT" | "ext4" | "Unknown"
    std::wstring source;          // "MBR" | "GPT" | "Signature"
    int confidence = 0;           // 0..100
};

class PartitionProbe
{
public:
    enum class ProbeMode {
        Full,       // MBR + GPT + signature scan (default)
        TableOnly,  // MBR + GPT partition table only
        LostOnly,   // signature scan only — recover lost / overwritten partitions
    };

    struct Options {
        uint64_t signatureStrideSectors = 2048; // scan step for signature search
        uint64_t scanStartSector = 0;
        uint64_t scanEndSector   = 0;          // 0 => to disk end
        bool     includeSignatureScan = true;  // ignored when mode != Full
        ProbeMode mode = ProbeMode::Full;
    };

    // Probe any open volume (disk image or physical disk) for partition candidates.
    static std::vector<PartitionCandidate> ProbeVolume(
        SectorReader& reader,
        const Options& opt,
        const std::atomic<bool>* cancelFlag,
        std::function<void(int percent)> onProgress);

    // Probe a physical disk and return candidate partitions.
    static std::vector<PartitionCandidate> ProbePhysicalDisk(
        int physicalDiskIdx,
        const Options& opt,
        const std::atomic<bool>* cancelFlag,
        std::function<void(int percent)> onProgress);

    static Partition ToPartition(const PartitionCandidate& c, int physicalDiskIdx);

    static bool DetectByVbr(const uint8_t* sec, uint32_t sectorSize,
                            std::wstring& outFs, uint64_t& outTotalSectors);
    static bool DetectExt4(SectorReader& r, uint64_t startSector, uint32_t sectorSize,
                           uint64_t& outTotalSectors,
                           std::wstring* outFsType = nullptr);
    static bool DetectUfs1(SectorReader& r, uint64_t startSector, uint32_t sectorSize);

    // Enumerate every valid file-system superblock/VBR at the same partition start.
    // DFTT #10 expects a warning when NTFS coexists with ext2 or UFS1.
    static std::vector<PartitionCandidate> DetectAllFileSystems(
        SectorReader& reader, uint64_t startSector, uint32_t sectorSize);

private:
    static void AddOrMerge(std::vector<PartitionCandidate>& v, const PartitionCandidate& c);
    static bool ReadSector(SectorReader& r, uint64_t lba, std::vector<uint8_t>& out);
};

