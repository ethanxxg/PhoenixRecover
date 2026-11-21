#pragma once
#include <cstdint>

namespace Constants
{
    // ── Disk / I/O ───────────────────────────────────────────────────────
    constexpr uint32_t DEFAULT_SECTOR_SIZE       = 512;
    constexpr uint32_t MAX_SECTOR_SIZE           = 4096;
    constexpr uint32_t READ_BUFFER_SECTORS       = 128;    // 64 KB at 512 B/sector
    constexpr uint32_t BAD_SECTOR_RETRY_COUNT    = 3;
    constexpr uint32_t MAX_PHYSICAL_DRIVES       = 32;

    // ── NTFS ─────────────────────────────────────────────────────────────
    constexpr char     NTFS_OEM_ID[8]            = {'N','T','F','S',' ',' ',' ',' '};
    constexpr char     NTFS_FILE_MAGIC[4]        = {'F','I','L','E'};
    constexpr char     NTFS_INDX_MAGIC[4]        = {'I','N','D','X'};
    constexpr uint32_t NTFS_MFT_RECORD_SIZE      = 1024;
    constexpr uint64_t NTFS_SYSTEM_FILE_COUNT    = 16;     // First 16 records are system files

    // MFT record flags
    constexpr uint16_t MFT_FLAG_IN_USE           = 0x01;
    constexpr uint16_t MFT_FLAG_DIRECTORY        = 0x02;

    // NTFS attribute type codes
    constexpr uint32_t ATTR_STANDARD_INFORMATION = 0x10;
    constexpr uint32_t ATTR_ATTRIBUTE_LIST       = 0x20;
    constexpr uint32_t ATTR_FILE_NAME            = 0x30;
    constexpr uint32_t ATTR_OBJECT_ID            = 0x40;
    constexpr uint32_t ATTR_SECURITY_DESCRIPTOR  = 0x50;
    constexpr uint32_t ATTR_VOLUME_NAME          = 0x60;
    constexpr uint32_t ATTR_VOLUME_INFORMATION   = 0x70;
    constexpr uint32_t ATTR_DATA                 = 0x80;
    constexpr uint32_t ATTR_INDEX_ROOT           = 0x90;
    constexpr uint32_t ATTR_INDEX_ALLOCATION     = 0xA0;
    constexpr uint32_t ATTR_BITMAP               = 0xB0;
    constexpr uint32_t ATTR_REPARSE_POINT        = 0xC0;
    constexpr uint32_t ATTR_END                  = 0xFFFFFFFF;

    // FILE_NAME namespace types
    constexpr uint8_t  FN_NAMESPACE_POSIX        = 0;
    constexpr uint8_t  FN_NAMESPACE_WIN32        = 1;
    constexpr uint8_t  FN_NAMESPACE_DOS          = 2;
    constexpr uint8_t  FN_NAMESPACE_WIN32DOS     = 3;

    // ── FAT ──────────────────────────────────────────────────────────────
    constexpr uint8_t  FAT_ENTRY_DELETED         = 0xE5;
    constexpr uint8_t  FAT_ENTRY_FREE            = 0x00;
    constexpr uint8_t  FAT_ATTR_READ_ONLY        = 0x01;
    constexpr uint8_t  FAT_ATTR_HIDDEN           = 0x02;
    constexpr uint8_t  FAT_ATTR_SYSTEM           = 0x04;
    constexpr uint8_t  FAT_ATTR_VOLUME_ID        = 0x08;
    constexpr uint8_t  FAT_ATTR_DIRECTORY        = 0x10;
    constexpr uint8_t  FAT_ATTR_ARCHIVE          = 0x20;
    constexpr uint8_t  FAT_ATTR_LFN              = 0x0F;   // Long File Name entry marker
    constexpr uint32_t FAT32_EOC                 = 0x0FFFFFF8; // End-of-chain marker
    constexpr uint32_t FAT32_FREE_CLUSTER        = 0x00000000;
    constexpr uint32_t FAT32_MASK                = 0x0FFFFFFF;

    // ── exFAT ─────────────────────────────────────────────────────────────
    constexpr char     EXFAT_OEM_ID[8]           = {'E','X','F','A','T',' ',' ',' '};
    constexpr uint8_t  EXFAT_ENTRY_TYPE_FILE      = 0x85;
    constexpr uint8_t  EXFAT_ENTRY_TYPE_STREAM    = 0xC0;
    constexpr uint8_t  EXFAT_ENTRY_TYPE_FILENAME  = 0xC1;

    // ── Snapshot file ─────────────────────────────────────────────────────
    constexpr uint32_t SNAPSHOT_MAGIC            = 0x50525350; // "PRSP"
    constexpr uint32_t SNAPSHOT_VERSION         = 1;

    // ── Recovery ──────────────────────────────────────────────────────────
    constexpr uint32_t RECOVER_WRITE_BUFFER_SIZE = 4 * 1024 * 1024; // 4 MB write buffer
}
