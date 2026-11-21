#pragma once

#include "../common/Types.h"
#include "CarveFragmentBlock.h"
#include <string>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// FileNode – represents one recoverable file or directory discovered during
//            a scan.  Used by all scan engines and the recovery engine.
// ---------------------------------------------------------------------------
struct FileNode
{
    // ── Identity ──────────────────────────────────────────────────────────
    uint64_t     id            = 0;     // Unique ID within a scan result
                                         // (NTFS: MFT record number;
                                         //  FAT:  sequential index;
                                         //  RAW:  byte offset of file start)
    std::wstring name;                   // File name (may be synthetic for RAW)
    std::wstring originalPath;           // Full path as it existed on the volume
    std::wstring extension;              // Lower-case extension, e.g. L"jpg"

    // ── Metadata ─────────────────────────────────────────────────────────
    uint64_t     size          = 0;     // Logical file size in bytes
    uint64_t     createTime    = 0;     // Windows FILETIME (100-ns since 1601)
    uint64_t     modifyTime    = 0;
    uint64_t     accessTime    = 0;

    FileStatus           status         = FileStatus::Deleted;
    RecoverabilityLevel  recoverability = RecoverabilityLevel::Unknown;
    bool                 isDirectory    = false;
    std::wstring         fsType;        // "NTFS" | "FAT32" | "FAT16" | "exFAT" | "RAW"

    // ── Data location (filled by the relevant parser) ─────────────────────
    // NTFS / FAT: ordered list of absolute cluster numbers
    std::vector<uint64_t> clusters;

    // RAW scan: byte offset + size within the scanned volume
    uint64_t     rawOffset     = 0;
    uint64_t     rawSize       = 0;     // May differ from 'size' for RAW

    // Multi-span RAW / carved fragments (order matters after GraphCarve stitch).
    std::vector<RawFragmentSpan> rawFragments;

    // NTFS resident file: data is stored directly inside the MFT record.
    // Non-empty only for small files (< ~700 B) on NTFS volumes.
    // When present the file can ALWAYS be fully recovered (Excellent).
    std::vector<uint8_t> residentData;

    // ── NTFS compressed file ($DATA attribute has flag 0x0001) ────────────────
    // isNtfsCompressed : the $DATA attribute has ATTR_FLAG_COMPRESSED set.
    // compressionUnit  : number of clusters per compression unit
    //                    (typically 16 clusters = 64 KB for 4-KB clusters).
    //                    0  means not compressed.
    // When non-zero, the cluster list encodes sparse runs as LCN = 0;
    // RecoverEngine will decompress each unit with LZNT1.
    bool     isNtfsCompressed = false;
    uint32_t compressionUnit  = 0;

    // ── Parent directory reference ─────────────────────────────────────────
    uint64_t     parentId      = 0;     // Parent FileNode::id (0 = root)

    // ── Helper ────────────────────────────────────────────────────────────
    bool IsValid() const { return !name.empty() || rawOffset > 0; }
};
