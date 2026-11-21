#pragma once
#include <cstdint>

#pragma pack(push, 1)

// ── exFAT Boot Sector ─────────────────────────────────────────────────────
struct EXFAT_BOOT_SECTOR
{
    uint8_t  jumpBoot[3];
    char     oemName[8];        // "EXFAT   "
    uint8_t  unused[53];
    uint64_t partitionOffset;   // Sector offset of this volume on the medium
    uint64_t volumeLength;      // Total sectors in this volume
    uint32_t fatOffset;         // Sector offset of first FAT (from volume start)
    uint32_t fatLength;         // Length of each FAT in sectors
    uint32_t clusterHeapOffset; // Sector offset of cluster heap from volume start
    uint32_t clusterCount;      // Total number of clusters
    uint32_t rootDirectoryCluster; // First cluster of root directory
    uint32_t volumeSerialNumber;
    uint16_t fileSystemRevision; // 0x0100 = 1.00
    uint16_t volumeFlags;
    uint8_t  bytesPerSectorShift;  // log2(bytes per sector)
    uint8_t  sectorsPerClusterShift; // log2(sectors per cluster)
    uint8_t  numberOfFATs;
    uint8_t  driveSelect;
    uint8_t  percentInUse;
    uint8_t  reserved[7];
    // Boot code follows
};

// ── Directory Entry types ─────────────────────────────────────────────────
constexpr uint8_t EXFAT_ENTRY_EOD         = 0x00; // End of directory
constexpr uint8_t EXFAT_ENTRY_ALLOC_BMP   = 0x81; // Allocation bitmap
constexpr uint8_t EXFAT_ENTRY_UPCASE      = 0x82; // Up-Case table
constexpr uint8_t EXFAT_ENTRY_VOLUME_LABEL= 0x83; // Volume label
constexpr uint8_t EXFAT_ENTRY_FILE        = 0x85; // File / directory
constexpr uint8_t EXFAT_ENTRY_STREAM_EXT  = 0xC0; // Stream Extension
constexpr uint8_t EXFAT_ENTRY_FILE_NAME   = 0xC1; // File Name Extension

// Generic 32-byte directory entry
struct EXFAT_DIR_ENTRY
{
    uint8_t entryType;
    uint8_t data[31];
};

// File entry (0x85)
struct EXFAT_FILE_ENTRY
{
    uint8_t  entryType;         // 0x85 (or 0x05 if deleted)
    uint8_t  secondaryCount;    // Number of secondary entries following
    uint16_t setChecksum;
    uint16_t fileAttributes;    // Same as FAT attributes
    uint16_t reserved1;
    uint32_t createTime;
    uint32_t lastModifiedTime;
    uint32_t lastAccessedTime;
    uint8_t  create10msIncrement;
    uint8_t  lastModified10msIncrement;
    uint8_t  createUtcOffset;
    uint8_t  lastModifiedUtcOffset;
    uint8_t  lastAccessedUtcOffset;
    uint8_t  reserved2[7];
};

// Stream Extension (0xC0)
struct EXFAT_STREAM_EXT
{
    uint8_t  entryType;         // 0xC0
    uint8_t  generalSecondaryFlags;
    uint8_t  reserved1;
    uint8_t  nameLength;        // In Unicode characters
    uint16_t nameHash;
    uint16_t reserved2;
    uint64_t validDataLength;
    uint32_t reserved3;
    uint32_t firstCluster;
    uint64_t dataLength;
};

// File Name Extension (0xC1)
struct EXFAT_FILENAME_EXT
{
    uint8_t  entryType;         // 0xC1
    uint8_t  generalSecondaryFlags;
    uint16_t fileName[15];      // Up to 15 Unicode chars per entry
};

#pragma pack(pop)
