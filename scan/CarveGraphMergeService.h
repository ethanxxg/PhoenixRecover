#pragma once

#include "../recover/FragmentStitchPolicy.h"
#include "../recover/FileNode.h"
#include "../disk/Partition.h"
#include "../disk/SectorReader.h"

#include <cstdint>
#include <vector>

// Post-process phase-3 RAW carve hits: cluster by extension + proximity, graph-stitch.
namespace CarveGraphMergeService {

struct MergeConfig {
    uint64_t clusterMaxGapBytes = 16ULL * 1024 * 1024;
    uint32_t minBlocksPerMerge  = 2;
};

// Mutates candidates in place: removes merged sources, appends stitched FileNode(s).
// Returns number of successful graph merges.
uint32_t MergeRawCarveCandidates(SectorReader& reader,
                                 const Partition& partition,
                                 std::vector<FileNode>& candidates,
                                 const FragmentStitchPolicy& stitchPolicy,
                                 const MergeConfig& config = {});

} // namespace CarveGraphMergeService
