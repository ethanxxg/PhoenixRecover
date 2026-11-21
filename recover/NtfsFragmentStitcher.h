#pragma once

#include "FormatPayloadScore.h"
#include "FileNode.h"
#include "../disk/Partition.h"
#include "../disk/SectorReader.h"

#include <cstdint>
#include <string>
#include <vector>

struct NtfsVolumeGeometry {
    uint32_t bytesPerSector    = 512;
    uint32_t sectorsPerCluster = 1;
    uint32_t bytesPerCluster   = 512;
    uint32_t recordSize        = 1024;
    uint64_t partStartBytes    = 0;
    uint64_t mftByteOffset     = 0;
    uint64_t totalClusters     = 0;
};

// NTFS fragment recovery: when data runs are wrong after delete/braid,
// search alternate free LCN layouts ranked by FormatPayloadScore.
class NtfsFragmentStitcher {
public:
    NtfsFragmentStitcher(SectorReader& reader, const NtfsVolumeGeometry& geo);

    bool LoadVolumeBitmap(const std::vector<uint64_t>& mftClusters);

    bool TryImprove(uint64_t startLcn,
                    uint64_t fileSize,
                    const std::wstring& extension,
                    std::vector<uint64_t>& lcnChain);

    static bool TryImproveNode(SectorReader& reader,
                               const Partition& partition,
                               FileNode& node);

private:
    bool IsFreeLcn(uint64_t lcn) const;

    bool ReadChainBytes(const std::vector<uint64_t>& lcns,
                        uint64_t fileSize,
                        std::vector<uint8_t>& out) const;

    int ScoreChain(const std::vector<uint64_t>& lcns,
                   uint64_t fileSize,
                   const std::wstring& extLower) const;

    bool SearchAndPickBest(uint64_t startLcn,
                           uint64_t fileSize,
                           const std::wstring& extLower,
                           uint32_t clustersNeeded,
                           const std::vector<uint64_t>& baseLcns,
                           int baseScore,
                           std::vector<uint64_t>& outBest) const;

    SectorReader&              m_reader;
    NtfsVolumeGeometry         m_geo;
    std::vector<uint8_t>       m_bitmap;
};
