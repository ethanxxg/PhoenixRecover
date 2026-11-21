#pragma once
#include <cstdint>

// All structures are packed (no compiler padding between fields).
#pragma pack(push, 1)

// ── NTFS Boot Sector (VBR) ────────────────────────────────────────────────
struct NTFS_BOOT_SECTOR
{
    uint8_t  jump[3];           // JMP short + NOP
    char     oemId[8];          // "NTFS    "
    uint16_t bytesPerSector;
    uint8_t  sectorsPerCluster;
    uint16_t reservedSectors;   // Always 0 on NTFS
    uint8_t  unused1[3];        // FAT fields not used
    uint16_t unused2;
    uint8_t  mediaDescriptor;   // 0xF8 for fixed disk
    uint16_t unused3;
    uint16_t sectorsPerTrack;
    uint16_t numberOfHeads;
    uint32_t hiddenSectors;
    uint32_t unused4;
    uint32_t unused5;
    uint64_t totalSectors;
    uint64_t mftStartCluster;   // LCN of $MFT
    uint64_t mftMirrorCluster;  // LCN of $MFTMirr
    int8_t   clustersPerRecord; // Negative => 2^(-n) bytes per record
    uint8_t  pad1[3];
    int8_t   clustersPerIndex;  // Negative => 2^(-n) bytes per index block
    uint8_t  pad2[3];
    uint64_t volumeSerialNumber;
    uint32_t checksum;
    // Boot code follows (448 bytes), then 0x55 0xAA
};

// ── MFT Record Header ─────────────────────────────────────────────────────
struct MFT_RECORD_HEADER
{
    char     magic[4];          // "FILE"
    uint16_t usaOffset;         // Offset to Update Sequence Array
    uint16_t usaCount;          // Number of entries in USA (including header)
    uint64_t logSeqNumber;      // $LogFile sequence number
    uint16_t sequenceNumber;    // Reuse count
    uint16_t hardLinkCount;
    uint16_t attrOffset;        // Offset to first attribute
    uint16_t flags;             // 0x01=in-use, 0x02=directory
    uint32_t bytesInUse;
    uint32_t bytesAllocated;
    uint64_t baseFileReference; // 0 for base records
    uint16_t nextAttrId;
    uint16_t pad;
    uint32_t mftRecordNumber;   // Only valid on NTFS 3.1+
};

// ── Attribute Header (common part) ────────────────────────────────────────
struct ATTR_HEADER_COMMON
{
    uint32_t type;
    uint32_t length;
    uint8_t  nonResident;   // 0 = resident
    uint8_t  nameLength;    // Attribute name length (Unicode chars)
    uint16_t nameOffset;
    uint16_t flags;         // 0x0001=compressed, 0x4000=encrypted, 0x8000=sparse
    uint16_t attrId;
};

// ── Resident attribute body ────────────────────────────────────────────────
struct ATTR_RESIDENT
{
    ATTR_HEADER_COMMON common;
    uint32_t valueLength;
    uint16_t valueOffset;
    uint8_t  indexedFlag;
    uint8_t  pad;
};

// ── Non-resident attribute body ───────────────────────────────────────────
struct ATTR_NONRESIDENT
{
    ATTR_HEADER_COMMON common;
    uint64_t startVcn;          // Starting VCN of data runs
    uint64_t lastVcn;           // Last VCN
    uint16_t dataRunOffset;     // Offset to DataRun list
    uint16_t compressionUnit;
    uint32_t pad;
    uint64_t allocatedSize;
    uint64_t dataSize;
    uint64_t initializedSize;
    // (compression: compressed size follows for compressed attributes)
};

// ── $FILE_NAME attribute value ────────────────────────────────────────────
struct FILE_NAME_ATTR
{
    uint64_t parentDirRef;      // MFT reference (48-bit recno + 16-bit seq)
    uint64_t createTime;        // Windows FILETIME
    uint64_t modifyTime;
    uint64_t mftModifyTime;
    uint64_t accessTime;
    uint64_t allocatedSize;
    uint64_t realSize;
    uint32_t fileAttributes;
    uint32_t reparseTagOrEaSize;
    uint8_t  nameLength;        // In Unicode characters
    uint8_t  nameType;          // 0=POSIX, 1=Win32, 2=DOS, 3=Win32&DOS
    // uint16_t name[nameLength] follows immediately
};

// ── $STANDARD_INFORMATION attribute value (NTFS 3.1) ─────────────────────
struct STANDARD_INFORMATION
{
    uint64_t createTime;
    uint64_t modifyTime;
    uint64_t mftModifyTime;
    uint64_t accessTime;
    uint32_t fileAttributes;
    uint32_t maxVersions;
    uint32_t version;
    uint32_t classId;
    uint32_t ownerId;
    uint32_t securityId;
    uint64_t quotaCharged;
    uint64_t usn;
};

#pragma pack(pop)

// MFT reference helpers
inline uint64_t MftRefRecordNumber(uint64_t ref) { return ref & 0x0000FFFFFFFFFFFFULL; }
inline uint16_t MftRefSequenceNum (uint64_t ref) { return static_cast<uint16_t>(ref >> 48); }
