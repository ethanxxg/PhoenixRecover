#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>

// ── Common type aliases ────────────────────────────────────────────────────
using Byte       = uint8_t;
using ByteBuffer = std::vector<uint8_t>;

// ── Error codes ───────────────────────────────────────────────────────────
enum class ErrorCode
{
    Success = 0,
    AccessDenied,             // Need Administrator privileges
    DeviceNotFound,           // Physical disk / volume not found
    PartitionNotRecognized,   // Partition table invalid
    ReadError,                // I/O read failure
    BadSector,                // Encountered a bad sector
    InvalidFileSystem,        // FS signature not recognised
    RecoverPathSameAsSrc,     // Target == source partition  (forbidden)
    DiskFull,                 // Not enough space on target
    FileWriteError,           // Could not write recovered file
    Cancelled,                // User cancelled
    SnapshotInvalid,          // .prs snapshot file corrupt
    NotImplemented,           // Feature stub
    InvalidParameter,
    OutOfMemory,
};

inline const char* ErrorCodeToStr(ErrorCode c)
{
    switch (c) {
    case ErrorCode::Success:               return "Success";
    case ErrorCode::AccessDenied:          return "Access denied (run as Administrator)";
    case ErrorCode::DeviceNotFound:        return "Device not found";
    case ErrorCode::PartitionNotRecognized:return "Partition not recognised";
    case ErrorCode::ReadError:             return "Disk read error";
    case ErrorCode::BadSector:             return "Bad sector";
    case ErrorCode::InvalidFileSystem:     return "Invalid file system";
    case ErrorCode::RecoverPathSameAsSrc:  return "Recovery path must differ from source partition";
    case ErrorCode::DiskFull:              return "Target disk full";
    case ErrorCode::FileWriteError:        return "File write error";
    case ErrorCode::Cancelled:             return "Operation cancelled";
    case ErrorCode::SnapshotInvalid:       return "Snapshot file invalid";
    case ErrorCode::NotImplemented:        return "Not implemented";
    case ErrorCode::InvalidParameter:      return "Invalid parameter";
    case ErrorCode::OutOfMemory:           return "Out of memory";
    default:                               return "Unknown error";
    }
}

// ── Recoverability assessment ──────────────────────────────────────────────
enum class RecoverabilityLevel
{
    Excellent,  // All data clusters still free  (best chance)
    Good,       // <= 10 % clusters overwritten
    Poor,       // 10 % ~ 50 % clusters overwritten
    Lost,       // > 50 % clusters overwritten   (recovery unlikely)
    Unknown,    // Cannot determine (e.g. no bitmap, 0-byte file)
};

inline const char* RecoverabilityStr(RecoverabilityLevel r)
{
    switch (r) {
    case RecoverabilityLevel::Excellent: return "极好";
    case RecoverabilityLevel::Good:      return "良好";
    case RecoverabilityLevel::Poor:      return "较差";
    case RecoverabilityLevel::Lost:      return "极低";
    default:                             return "未知";
    }
}

// ── File status ───────────────────────────────────────────────────────────
enum class FileStatus
{
    Deleted,    // File was explicitly deleted
    Lost,       // Lost due to format / partition damage
    Normal,     // File system intact, file accessible
};

// ── Scan mode ─────────────────────────────────────────────────────────────
enum class ScanMode
{
    Quick,   // Parse FS metadata (MFT / FAT dir entries) for deleted records
    Deep,    // Sector-by-sector, reconstruct FS structures
    Raw,     // File-signature (file-carving) scan
};

inline const char* ScanModeStr(ScanMode m)
{
    switch (m) {
    case ScanMode::Quick: return "Quick Scan";
    case ScanMode::Deep:  return "Deep Scan";
    case ScanMode::Raw:   return "RAW Signature Scan";
    default:              return "Unknown";
    }
}
