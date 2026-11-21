#pragma once

#include <cstdint>

// NIST CFReDS file-carving-images — Graphic layouts from ImageLayouts.htm
// Sector size 512. L0 = contiguous; L1–L5 = fragmented / partial layouts.
// sourceByteOffset: when >0, embed bytes at startSector match source[offset:offset+embedBytes).

struct FileCarveGolden {
    const char* sourceName;
    uint64_t    startSector;
    uint64_t    sizeBytes;
    uint64_t    embedBytes;
    uint64_t    sourceByteOffset;
    bool        exactLayout;
};

struct FileCarveImageCase {
    const char*       imageName;
    const char*       layoutTag;
    FileCarveGolden*  files;
    size_t            fileCount;
};

// ── L0 contiguous (6 files, sector-aligned) ─────────────────────────────
inline FileCarveGolden kL0Files[] = {
    {"000_0021.png",      10000, 12966639, 12966639, 0, true},
    {"02010025.pcx",      45326,   806664,   806664, 0, true},
    {"09260002.jpg",      56902,    60716,    60716, 0, true},
    {"100_0304crop.bmp",  67021,  5111906,  5111906, 0, true},
    {"100_0018.tif",      87006, 14969328, 14969328, 0, true},
    {"100_0183.gif",     126243,   132948,   132948, 0, true},
};

// ── L1 fragmented in order ───────────────────────────────────────────────
inline FileCarveGolden kL1Files[] = {
    {"09260002.jpg",      10000,    60716,    30208, 0, false},
    {"100_0018.tif",      20119, 14969328,  4989440, 0, false},
    {"100_0304crop.bmp",  59356,  5111906,  2555904, 0, false},
    {"02010025.pcx",      79341,   806664,   268800, 0, false},
    {"100_0183.gif",      90917,   132948,    66048, 0, false},
    {"000_0021.png",     101177, 12966639,  4321792, 0, false},
};

// ── L2 fragmented out of order ───────────────────────────────────────────
inline FileCarveGolden kL2Files[] = {
    {"100_0018.tif",      10000, 14969328,  4989440, 0, false},
    {"09260002.jpg",      49237,    60716,    19968, 0, false},
    {"100_0304crop.bmp",  64349,  5111906,  2555904, 0, false},
    {"02010025.pcx",      79341,   806664,   268800, 0, false},
    {"100_0183.gif",      91005,   132948,    88064, 0, false},
    {"000_0021.png",     113841, 12966639,  6482944, 0, false},
};

// ── L3 missing middle fragments ─────────────────────────────────────────
inline FileCarveGolden kL3Files[] = {
    {"09260002.jpg",      10000,    60716,    30208, 0, false},
    {"100_0018.tif",      20059, 14969328,  9978880, 0, false},
    {"100_0304crop.bmp",  49549,  5111906,  2556002, 2555904, false},
    {"02010025.pcx",      64542,   806664,   268800, 0, false},
    {"100_0183.gif",      75593,   132948,    44032, 44032, false},
    {"000_0021.png",      85767, 12966639,  4323055, 8643584, false},
};

// ── L4 extra file + partial fragments (7 logical sources) ─────────────────
inline FileCarveGolden kL4Files[] = {
    {"09260002.jpg",      10000,    60716,    30208, 0, false},
    {"02010026.jpg",      10059,    61038,    61038, 0, false},
    {"100_0304crop.bmp",  20239,  5111906,  1703936, 0, false},
    {"100_0018.tif",      23567, 14969328, 14969328, 0, false},
    {"02010025.pcx",      69461,   806664,   402944, 0, false},
    {"100_0183.gif",      70248,   132948,    88064, 0, false},
    {"000_0021.png",      70420, 12966639, 12966639, 0, false},
};

// ── L5 interleaved fragments ─────────────────────────────────────────────
inline FileCarveGolden kL5Files[] = {
    {"09260002.jpg",      10000,    60716,    30208, 0, false},
    {"100_0304crop.bmp",  10059,  5111906,  2555904, 0, false},
    {"100_0018.tif",      30104, 14969328,  4989440, 0, false},
    {"02010025.pcx",      39849,   806664,   268800, 0, false},
    {"100_0183.gif",      70917,   132948,    66048, 0, false},
    {"000_0021.png",      71046, 12966639,  6482944, 0, false},
};

inline FileCarveImageCase kGraphicCarveCases[] = {
    {"L0_Graphic.dd", "L0_contiguous",       kL0Files, sizeof(kL0Files) / sizeof(kL0Files[0])},
    {"L1_Graphic.dd", "L1_frag_in_order",    kL1Files, sizeof(kL1Files) / sizeof(kL1Files[0])},
    {"L2_Graphic.dd", "L2_frag_disorder",    kL2Files, sizeof(kL2Files) / sizeof(kL2Files[0])},
    {"L3_Graphic.dd", "L3_missing_frags",    kL3Files, sizeof(kL3Files) / sizeof(kL3Files[0])},
    {"L4_Graphic.dd", "L4_extra_partial",    kL4Files, sizeof(kL4Files) / sizeof(kL4Files[0])},
    {"L5_Graphic.dd", "L5_interleaved",      kL5Files, sizeof(kL5Files) / sizeof(kL5Files[0])},
};

inline constexpr size_t kGraphicCarveCaseCount =
    sizeof(kGraphicCarveCases) / sizeof(kGraphicCarveCases[0]);
