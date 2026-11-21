#pragma once
#include <cstdint>

#pragma pack(push, 1)

// ── FAT BPB (common header for FAT12/16/32) ───────────────────────────────
struct FAT_BPB_COMMON
{
    uint8_t  jump[3];
    char     oemName[8];
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectors;
    uint8_t  numFATs;           // Usually 2
    uint16_t rootEntryCount;    // Non-zero for FAT12/16; 0 for FAT32
    uint16_t totalSectors16;    // 0 if FAT32 or > 65535 sectors
    uint8_t  mediaDescriptor;
    uint16_t fatSize16;         // Sectors per FAT (FAT12/16); 0 for FAT32
    uint16_t sectorsPerTrack;
    uint16_t numberOfHeads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
};

// ── FAT32 extended BPB ────────────────────────────────────────────────────
struct FAT32_BPB : FAT_BPB_COMMON
{
    uint32_t fatSize32;         // Sectors per FAT
    uint16_t extFlags;
    uint16_t fsVersion;
    uint32_t rootCluster;       // First cluster of root directory (usually 2)
    uint16_t fsInfoSector;
    uint16_t backupBootSector;
    uint8_t  reserved[12];
    uint8_t  driveNumber;
    uint8_t  reserved1;
    uint8_t  bootSignature;     // 0x29
    uint32_t volumeId;
    char     volumeLabel[11];
    char     fsType[8];         // "FAT32   "
};

// ── FAT16/12 extended BPB ─────────────────────────────────────────────────
struct FAT16_BPB : FAT_BPB_COMMON
{
    uint8_t  driveNumber;
    uint8_t  reserved1;
    uint8_t  bootSignature;     // 0x29
    uint32_t volumeId;
    char     volumeLabel[11];
    char     fsType[8];         // "FAT16   " or "FAT12   " or "FAT     "
};

// ── Directory Entry (8.3 format) ──────────────────────────────────────────
struct FAT_DIR_ENTRY
{
    char     name[8];           // Padded with spaces; [0]==0xE5 if deleted
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  createTimeTenth;   // Tenths of second
    uint16_t createTime;
    uint16_t createDate;
    uint16_t lastAccessDate;
    uint16_t clusterHigh;       // High 16 bits of first cluster (FAT32 only)
    uint16_t lastWriteTime;
    uint16_t lastWriteDate;
    uint16_t clusterLow;        // Low 16 bits of first cluster
    uint32_t fileSize;
};

// ── Long File Name entry ──────────────────────────────────────────────────
struct FAT_LFN_ENTRY
{
    uint8_t  order;             // Sequence number; 0x40 bit = last LFN entry
    uint16_t name1[5];          // Characters 1–5
    uint8_t  attr;              // Always 0x0F
    uint8_t  type;              // Always 0x00
    uint8_t  checksum;          // Checksum of 8.3 name
    uint16_t name2[6];          // Characters 6–11
    uint16_t clusterLow;        // Always 0x0000
    uint16_t name3[2];          // Characters 12–13
};

#pragma pack(pop)

// Attribute flag constants
constexpr uint8_t FAT_ATTR_READ_ONLY  = 0x01;
constexpr uint8_t FAT_ATTR_HIDDEN     = 0x02;
constexpr uint8_t FAT_ATTR_SYSTEM     = 0x04;
constexpr uint8_t FAT_ATTR_VOLUME_ID  = 0x08;
constexpr uint8_t FAT_ATTR_DIRECTORY  = 0x10;
constexpr uint8_t FAT_ATTR_ARCHIVE    = 0x20;
constexpr uint8_t FAT_ATTR_LFN       = 0x0F;

constexpr uint8_t FAT_DELETED_MARKER  = 0xE5;
constexpr uint8_t FAT_FREE_MARKER     = 0x00; // This entry and all following are free

// FAT32 cluster chain constants
constexpr uint32_t FAT32_MASK         = 0x0FFFFFFFU;
constexpr uint32_t FAT32_EOC_MIN      = 0x0FFFFFF8U;
constexpr uint32_t FAT32_FREE         = 0x00000000U;
constexpr uint32_t FAT32_BAD          = 0x0FFFFFF7U;

// Decode FAT date/time into Windows FILETIME (100-ns intervals since 1601)
uint64_t FatDateTimeToFiletime(uint16_t date, uint16_t time,
                               uint8_t timeTenth = 0);
