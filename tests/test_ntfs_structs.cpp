// NTFS 结构体 sizeof + DataRun 解码 单元测试
//
// 结构体 sizeof 测试依赖 <Windows.h>（NtfsStructs.h 包含之），仅 Windows 编译。
// DecodeDataRuns 算法使用独立实现（与 NtfsParser::DecodeDataRuns 相同逻辑），
// 仅依赖 Utils::ReadSignedLE（内联），可在任何平台编译。
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "common/Utils.h"

#ifdef _WIN32
#include "fs/ntfs/NtfsStructs.h"
#endif

#include <cstdio>

#define CHECK(cond) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL: %s  (line %d)\n", #cond, __LINE__); return 1; } \
    } while (0)

// ── Standalone DecodeDataRuns (mirrors NtfsParser::DecodeDataRuns) ────────
static std::vector<uint64_t> DecodeDataRuns(const uint8_t* p, size_t maxBytes)
{
    std::vector<uint64_t> result;
    if (!p || maxBytes == 0) return result;

    const uint8_t* end = p + maxBytes;
    int64_t currentLcn = 0;

    while (p < end) {
        uint8_t header = *p++;
        if (header == 0x00) break;

        int lenFieldSize = header & 0x0F;
        int offFieldSize = (header >> 4) & 0x0F;
        if (lenFieldSize == 0) break;
        if (p + lenFieldSize + offFieldSize > end) break;

        uint64_t runLen = 0;
        for (int i = 0; i < lenFieldSize; ++i)
            runLen |= static_cast<uint64_t>(p[i]) << (i * 8);
        p += lenFieldSize;

        if (offFieldSize == 0) {
            for (uint64_t k = 0; k < runLen; ++k)
                result.push_back(0);
            continue;
        }

        int64_t delta = Utils::ReadSignedLE(p, offFieldSize);
        p += offFieldSize;
        currentLcn += delta;
        if (currentLcn < 0) break;

        for (uint64_t k = 0; k < runLen; ++k)
            result.push_back(static_cast<uint64_t>(currentLcn + k));
    }

    return result;
}

int main()
{
    // ═══════════════════════════════════════════════════════════════════════
    // 1. Struct sizeof tests (Windows-only: NtfsStructs.h includes <Windows.h>)
    // ═══════════════════════════════════════════════════════════════════════
#ifdef _WIN32
    CHECK(sizeof(NTFS_BOOT_SECTOR)     == 84);
    CHECK(sizeof(MFT_RECORD_HEADER)    == 48);
    CHECK(sizeof(ATTR_HEADER_COMMON)   == 16);
    CHECK(sizeof(ATTR_RESIDENT)        == 24);
    CHECK(sizeof(ATTR_NONRESIDENT)     == 64);
    CHECK(sizeof(FILE_NAME_ATTR)       == 66);
    CHECK(sizeof(STANDARD_INFORMATION) == 72);

    // MftRef helpers
    {
        uint64_t ref = (static_cast<uint64_t>(42) << 48) | 0x0000000012ABULL;
        CHECK(MftRefRecordNumber(ref) == 0x12AB);
        CHECK(MftRefSequenceNum(ref)  == 42);
    }
#endif

    // ═══════════════════════════════════════════════════════════════════════
    // 2. DecodeDataRuns tests (pure C++, no Windows/Qt dependency)
    // ═══════════════════════════════════════════════════════════════════════

    // 2a. Empty / null input → empty result
    {
        CHECK(DecodeDataRuns(nullptr, 0).empty());
        CHECK(DecodeDataRuns(nullptr, 10).empty());

        uint8_t zero[] = { 0x00 };
        CHECK(DecodeDataRuns(zero, sizeof(zero)).empty());
    }

    // 2b. Single run: header=0x11 → len=1 byte, off=1 byte
    //     length=3, offset=+10 → clusters [10, 11, 12]
    {
        uint8_t data[] = { 0x11, 0x03, 0x0A, 0x00 };
        auto r = DecodeDataRuns(data, sizeof(data));
        CHECK(r.size() == 3);
        CHECK(r[0] == 10 && r[1] == 11 && r[2] == 12);
    }

    // 2c. Two runs with positive deltas
    //     Run 1: len=2, off=+100   → clusters [100, 101]
    //     Run 2: len=3, off=+50    → LCN=150, clusters [150, 151, 152]
    {
        uint8_t data[] = {
            0x11, 0x02, 0x64,       // header(1,1), len=2, off=+100
            0x11, 0x03, 0x32,       // header(1,1), len=3, off=+50
            0x00                    // terminator
        };
        auto r = DecodeDataRuns(data, sizeof(data));
        CHECK(r.size() == 5);
        CHECK(r[0] == 100 && r[1] == 101);
        CHECK(r[2] == 150 && r[3] == 151 && r[4] == 152);
    }

    // 2d. Negative offset (backtrack)
    //     Run 1: len=2, off=+20 → [20, 21]
    //     Run 2: len=1, off=-10 → LCN=10, [10]
    {
        uint8_t data[] = {
            0x11, 0x02, 0x14,       // len=2, off=+20
            0x11, 0x01, 0xF6,       // len=1, off=-10 (0xF6 signed = -10)
            0x00
        };
        auto r = DecodeDataRuns(data, sizeof(data));
        CHECK(r.size() == 3);
        CHECK(r[0] == 20 && r[1] == 21);
        CHECK(r[2] == 10);
    }

    // 2e. Sparse run (offset field size = 0 → LCN = 0 per cluster)
    {
        uint8_t data[] = {
            0x01, 0x04,             // header(0,1): lenFieldSize=1, offFieldSize=0; len=4
            0x00
        };
        auto r = DecodeDataRuns(data, sizeof(data));
        CHECK(r.size() == 4);
        for (auto v : r) CHECK(v == 0);
    }

    // 2f. Mixed: real run + sparse run + real run
    {
        uint8_t data[] = {
            0x11, 0x02, 0x0A,       // len=2, off=+10 → [10, 11]
            0x01, 0x03,             // sparse len=3    → [0, 0, 0]
            0x11, 0x01, 0x14,       // len=1, off=+20 → LCN=10+20=30, [30]
            0x00
        };
        auto r = DecodeDataRuns(data, sizeof(data));
        CHECK(r.size() == 6);
        CHECK(r[0] == 10 && r[1] == 11);
        CHECK(r[2] == 0 && r[3] == 0 && r[4] == 0);
        CHECK(r[5] == 30);
    }

    // 2g. Two-byte length and offset fields (header=0x22)
    {
        // Run: len=0x0100 (256), off=0x0200 (512) → clusters [512..767]
        uint8_t data[] = {
            0x22,                   // lenFieldSize=2, offFieldSize=2
            0x00, 0x01,             // length = 256 (LE)
            0x00, 0x02,             // offset = 512 (LE)
            0x00
        };
        auto r = DecodeDataRuns(data, sizeof(data));
        CHECK(r.size() == 256);
        CHECK(r[0] == 512);
        CHECK(r[255] == 767);
    }

    return 0;
}
