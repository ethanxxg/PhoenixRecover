#pragma once

#include "ScanEngine.h"
#include "../disk/Partition.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ScanSerializer
//
//   Saves / loads the full scan result (ScanResult + Partition) to a binary
//   file with extension .prs  (Phoenix Recover Scan).
//
//   File format (little-endian):
//     [Header]
//       4 B  magic        "PHRS"
//       2 B  version      1
//       2 B  reserved
//     [Partition]
//       4 B  physicalDiskIdx
//       8 B  startSector
//       8 B  totalSectors
//       8 B  sizeBytes
//       str  fsType
//       str  driveLetter
//       str  label
//     [ScanResult meta]
//       8 B  scannedBytes
//       4 B  foundCount
//       4 B  totalRecords
//       4 B  inUseCount
//       8 B  duration (milliseconds as int64)
//     [Files]
//       4 B  count  N
//       For each FileNode:
//         8 B  id
//         str  name
//         str  originalPath
//         str  extension
//         8 B  size
//         8 B  createTime
//         8 B  modifyTime
//         8 B  accessTime
//         1 B  status          (FileStatus enum cast to uint8)
//         1 B  recoverability  (RecoverabilityLevel enum cast to uint8)
//         1 B  isDirectory
//         1 B  isNtfsCompressed
//         4 B  compressionUnit
//         str  fsType
//         8 B  parentId
//         4 B  rawOffset       (stored as 8 B)
//         8 B  rawSize
//         4 B  cluster count M
//         M×8B clusters
//         4 B  residentData length R
//         R×1B residentData
//
//   "str" = 4-B UTF-16 character count followed by count×2 bytes.
// ─────────────────────────────────────────────────────────────────────────────
class ScanSerializer
{
public:
    static constexpr uint16_t kVersion = 1;
    static constexpr const wchar_t* kFileExt = L".prs";  // for display only

    // Save result + partition to file.  Returns true on success.
    static bool Save(const std::wstring& filePath,
                     const ScanResult&   result,
                     const Partition&    partition);

    // Load result + partition from file.
    // Returns true on success; outResult / outPartition are populated.
    static bool Load(const std::wstring& filePath,
                     ScanResult&         outResult,
                     Partition&          outPartition);

    // Returns a suitable default filename based on the partition info
    // and the current local time, e.g. "C_NTFS_20260318_143022.prs"
    static std::wstring SuggestFilename(const Partition& part);
};
