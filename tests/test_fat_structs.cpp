// FAT 结构体、常量、日期时间转换 单元测试
#include <cstdint>
#include <cstdlib>

#include "fs/fat/FatStructs.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) return 1; \
    } while (0)

int main()
{
    // ── sizeof (packed structs) ──────────────────────────────────────────────
    CHECK(sizeof(FAT_BPB_COMMON) == 36);
    CHECK(sizeof(FAT32_BPB)      == 90);
    CHECK(sizeof(FAT16_BPB)      == 62);
    CHECK(sizeof(FAT_DIR_ENTRY)  == 32);
    CHECK(sizeof(FAT_LFN_ENTRY) == 32);

    // ── Attribute & marker constants ─────────────────────────────────────────
    CHECK(FAT_DELETED_MARKER == 0xE5);
    CHECK(FAT_FREE_MARKER    == 0x00);
    CHECK(FAT_ATTR_READ_ONLY == 0x01);
    CHECK(FAT_ATTR_HIDDEN    == 0x02);
    CHECK(FAT_ATTR_SYSTEM    == 0x04);
    CHECK(FAT_ATTR_VOLUME_ID == 0x08);
    CHECK(FAT_ATTR_DIRECTORY == 0x10);
    CHECK(FAT_ATTR_ARCHIVE   == 0x20);
    CHECK(FAT_ATTR_LFN       == 0x0F);

    // ── FAT32 cluster chain constants ────────────────────────────────────────
    CHECK(FAT32_MASK    == 0x0FFFFFFFU);
    CHECK(FAT32_EOC_MIN == 0x0FFFFFF8U);
    CHECK(FAT32_FREE    == 0x00000000U);
    CHECK(FAT32_BAD     == 0x0FFFFFF7U);

    // ── LFN attribute composition ────────────────────────────────────────────
    CHECK(FAT_ATTR_LFN == (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN |
                           FAT_ATTR_SYSTEM    | FAT_ATTR_VOLUME_ID));

    // ── FatDateTimeToFiletime (Windows-only: uses SystemTimeToFileTime) ──────
#ifdef _WIN32
    CHECK(FatDateTimeToFiletime(0, 0) == 0);

    // 2023-07-15 14:30:00 (even second, timeTenth >= 100 → no +1s adjust)
    uint16_t date1 = static_cast<uint16_t>(((2023 - 1980) << 9) | (7 << 5) | 15);
    uint16_t time1 = static_cast<uint16_t>((14 << 11) | (30 << 5) | 0);
    uint64_t ft1   = FatDateTimeToFiletime(date1, time1, 100);
    CHECK(ft1 != 0);

    // Monotonicity: 2024-01-01 same time → larger FILETIME
    uint16_t date2 = static_cast<uint16_t>(((2024 - 1980) << 9) | (1 << 5) | 1);
    uint64_t ft2   = FatDateTimeToFiletime(date2, time1, 100);
    CHECK(ft2 > ft1);

    // Same date, later time → larger FILETIME
    uint16_t time3 = static_cast<uint16_t>((15 << 11) | (0 << 5) | 0);
    uint64_t ft3   = FatDateTimeToFiletime(date1, time3, 100);
    CHECK(ft3 > ft1);
#endif

    return 0;
}
