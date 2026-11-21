#pragma once

#include <cstdint>

// DFTT official golden MD5/size tables (from index.html / 11-carve-fat.html).

struct DfttCarveGolden {
    const char* name;
    const char* md5;
    uint64_t    sizeBytes;
    bool        required;           // CI must pass
    bool        forbidValidCarve;   // corrupted — must not appear as Good+ JPEG
};

inline constexpr DfttCarveGolden kDftt11CarveGold[] = {
    // P0 — all official DFTT #11 goldens (structure carving)
    {"paul.jpg",          "37a49f97ed279832cd4f7bd002c826a2", 29885, true, false},
    {"haxor2.jpg",        "84e1dceac2eb127fef5bfdcb0eae324b", 24367, false, true},
    {"2003_document.doc", "e72f388b36f9370f19696b164c308482", 19968, true, false},
    {"enterprise.wav",    "7629b89adade055f6783dc1773274215", 318895, true, false},
    {"holly.xls",         "7917baf0219645afef8b381570c41211", 23040, true, false},
    {"lin_1.2.pdf",       "e026ec863410725ba1f5765a1874800d", 1399508, true, false},
    {"nlin_14.pdf",       "5b3e806e8c9c06a475cd45bf821af709", 122434, true, false},
    {"pumpkin.jpg",       "6c9859e5121ff54d5d6298f65f0bf3b3", 444314, true, false},
    {"shark.jpg",         "d83428b8742a075b57b0dc424cd297c4", 99298, true, false},
    {"sm1.gif",           "d25fb845e6a41395adaed8bd14db7bf2", 5498, true, false},
    {"surf.mov",          "5328d2b066f428ea95b2793849ab97fa", 550653, true, false},
    {"surf.wmv",          "ff085d0c4d0e0fdc8f3427db68e26266", 1036994, true, false},
    {"test.ppt",          "7b74c2c608d92f4bb76c1d3b6bd1decc", 11264, true, false},
    {"wword60t.zip",      "c0be59d49b7ee0fdc492d2df32f2c6c6", 78899, true, false},
    {"domopers.wmv",      "63c0c6986cf0a446cb54b0ac65a921a5", 8037267, true, false},
};

enum class DfttJpegExpect : uint8_t {
    RequiredFs,      // must recover via NTFS metadata + bytes
    RequiredCarve,   // must appear in RawScan SOI..EOI carve set
    ForbidCarve,     // must NOT appear as carved JPEG
    OptionalEmbed,   // embedded/container — document only (legacy)
    RequiredEmbed,   // embedded/container — must recover JPEG bytes
};

struct DfttJpegGolden {
    const char*       label;
    const char*       md5;
    DfttJpegExpect    expect;
};

inline constexpr DfttJpegGolden kDftt8JpegGold[] = {
    {"alloc/file1.jpg",           "75b8d00568815a36c3809b46fc84ba6d", DfttJpegExpect::RequiredFs},
    {"alloc/file2.dat",           "de5d83153339931371719f4e5c924eba", DfttJpegExpect::RequiredFs},
    {"invalid/file3.jpg",         "1ba4e91591f0541eda255ee26f7533bc", DfttJpegExpect::ForbidCarve},
    {"invalid/file4.jpg",         "c8de721102617158e8492121bdad3711", DfttJpegExpect::ForbidCarve},
    {"invalid/file5.rtf",         "86f14fc525648c39d878829f288c0543", DfttJpegExpect::ForbidCarve},
    {"del1/file6.jpg",            "afd55222024a4e22f7f5a3a665320763", DfttJpegExpect::RequiredFs},
    {"del2/file7.hmm",            "0c452c5800fcfa7c66027ae89c4f068a", DfttJpegExpect::RequiredFs},
    {"archive/file8.jpg (in zip)", "f9956284a89156ef6967b49eced9d1b1", DfttJpegExpect::RequiredEmbed},
    {"archive/file9.jpg (in zip)", "c5a6917669c77d20f30ecb39d389eb7d", DfttJpegExpect::RequiredEmbed},
    {"archive/file10.jpg (tar.gz)", "c476a66ccdc2796b4f6f8e27273dd788", DfttJpegExpect::RequiredEmbed},
    {"misc/file11.dat (suffix)",  "f407ab92da959c7ab03292cfe596a99d", DfttJpegExpect::RequiredEmbed},
    {"misc/file12.doc (ole)",     "61c0b55639e52d1ce82aba834ada2bab", DfttJpegExpect::RequiredEmbed},
    {"misc/file13.dll:here (ads)", "9b787e63e3b64562730c5aecaab1e1f8", DfttJpegExpect::RequiredEmbed},
};
