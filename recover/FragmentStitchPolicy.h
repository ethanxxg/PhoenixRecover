#pragma once

#include <cstdint>

// Fragment reassembly strategy (FAT/NTFS greedy vs carve graph).
enum class FragmentStitchStrategy {
    HeuristicGreedy, // FAT/NTFS cluster-chain search + FormatPayloadScore (default)
    GraphCarve,      // Multi-span RAW/carve blocks: permutation + format scoring
};

// Product policy for format-aware fragment reassembly.
struct FragmentStitchPolicy {
    bool enabled = true;
    FragmentStitchStrategy strategy = FragmentStitchStrategy::HeuristicGreedy;

    // After phase-3 carving: cluster nearby RECOV_* spans and graph-stitch (uses GraphCarve).
    bool graphMergeCarvedSpans = true;

    // GraphCarve limits (keep scan/recovery responsive).
    uint32_t graphMaxBlocks          = 12;
    uint32_t graphMaxAssembleBytes   = 4u * 1024u * 1024u;
    int64_t  graphBudgetMs           = 50;
};
