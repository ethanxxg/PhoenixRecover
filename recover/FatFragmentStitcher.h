#pragma once

#include "FormatPayloadScore.h"
#include "FileNode.h"
#include "../disk/Partition.h"
#include "../disk/SectorReader.h"
#include <cstdint>
#include <string>
#include <vector>

// FAT volume layout needed to map cluster numbers to absolute sectors.
struct FatVolumeGeometry {
    uint32_t bytesPerSector    = 512;
    uint32_t sectorsPerCluster = 1;
    uint64_t dataStartSector   = 0;
    uint32_t totalClusters     = 0;
    uint8_t  fatType           = 16; // 12, 16, or 32
};

// Product-side FAT fragment recovery: when the FAT chain is broken after delete,
// rank candidate cluster layouts by format structure (JPEG/PNG/PDF/ZIP/MP4…),
// without using golden hashes.
class FatFragmentStitcher {
public:
    // If fatTableBytes is non-null and non-empty, uses that FAT instead of loading again.
    FatFragmentStitcher(SectorReader& reader,
                        const FatVolumeGeometry& geo,
                        const std::vector<uint8_t>* fatTableBytes = nullptr);

    // Load first FAT copy into memory (no-op if already loaded or too large).
    bool LoadFatTable(uint64_t fatStartSector, uint32_t fatSizeSectors);

    // Try to replace absSectorChain with a higher-scoring layout.
    // absSectorChain entries are absolute disk sectors (FatParser convention).
    bool TryImprove(uint32_t startCluster,
                    uint64_t fileSize,
                    const std::wstring& extension,
                    std::vector<uint64_t>& absSectorChain);

    static bool TryImproveNode(SectorReader& reader,
                               const Partition& partition,
                               FileNode& node);

    static bool TryImproveExfatNode(SectorReader& reader,
                                    const Partition& partition,
                                    FileNode& node);

private:
    uint32_t NextCluster(uint32_t cluster) const;
    bool     IsFreeCluster(uint32_t cluster) const;
    uint64_t ClusterToAbsSector(uint32_t cluster) const;

    bool ReadChainBytes(const std::vector<uint32_t>& clusterNums,
                        uint64_t fileSize,
                        std::vector<uint8_t>& out) const;

    int ScoreChain(const std::vector<uint32_t>& clusterNums,
                   uint64_t fileSize,
                   const std::wstring& extLower) const;

    bool SearchAndPickBest(uint32_t startCluster,
                           uint64_t fileSize,
                           const std::wstring& extLower,
                           uint32_t clustersNeeded,
                           const std::vector<uint32_t>& baseClusterNums,
                           int baseScore,
                           std::vector<uint32_t>& outBestClusters) const;

    SectorReader&              m_reader;
    FatVolumeGeometry          m_geo;
    std::vector<uint8_t>       m_fatTable;
};
