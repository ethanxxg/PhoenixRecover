#pragma once

#include <cstdint>

// One contiguous byte range on disk (carving / RAW multi-span).
struct CarveFragmentBlock {
    uint64_t diskOffset = 0;
    uint64_t size       = 0;
};

// Alias used on FileNode for ordered spans after graph stitch.
using RawFragmentSpan = CarveFragmentBlock;
