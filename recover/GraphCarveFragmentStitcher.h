#pragma once

#include "CarveFragmentBlock.h"
#include "FragmentStitchPolicy.h"
#include "FileNode.h"
#include "../disk/Partition.h"
#include "../disk/SectorReader.h"

#include <cstdint>
#include <vector>

// Graph-based ordering of carved / RAW byte spans (no ML).
class GraphCarveFragmentStitcher {
public:
    explicit GraphCarveFragmentStitcher(SectorReader& reader);

    // Reorder node.rawFragments (>=2) and update node.size / rawOffset / rawSize.
    bool TryImproveNode(const Partition& partition,
                        FileNode& node,
                        const FragmentStitchPolicy& policy) const;

    // Low-level: order arbitrary blocks; writes ordered spans to outOrdered.
    bool StitchBlocks(const std::wstring& extensionLower,
                      const std::vector<CarveFragmentBlock>& blocks,
                      std::vector<CarveFragmentBlock>& outOrdered,
                      int& outBestScore,
                      const FragmentStitchPolicy& policy) const;

private:
    struct LoadedBlock {
        CarveFragmentBlock span;
        std::vector<uint8_t> bytes;
    };

    bool LoadBlocks(const std::vector<CarveFragmentBlock>& blocks,
                    uint32_t maxBytesPerBlock,
                    std::vector<LoadedBlock>& out) const;

    bool HasFormatHeader(const std::wstring& extLower, const uint8_t* data, size_t len) const;

    bool CanLink(const std::wstring& extLower,
                 const LoadedBlock& a,
                 const LoadedBlock& b) const;

    int ScoreOrdered(const std::wstring& extLower,
                     const std::vector<LoadedBlock>& loaded,
                     const std::vector<int>& order) const;

    bool SearchBestOrder(const std::wstring& extLower,
                         const std::vector<LoadedBlock>& loaded,
                         std::vector<int>& outOrder,
                         int& outScore,
                         const FragmentStitchPolicy& policy) const;

    SectorReader& m_reader;
};
