#pragma once

#include "../common/Platform.h"

#include <cwchar>
#include <memory>
#include <string>

class FileSystem;
class SectorReader;
struct Partition;
struct ScanResult;

// Low-coupling factory for on-disk file system parsers (NTFS / FAT / exFAT / ext2–ext4).
// Callers (VolumeSectorScanner, tests) depend on this module — not on individual parsers.
namespace FileSystemModuleService {

enum class FsModuleId {
    Ntfs,
    Fat,
    Exfat,
    Ext4,
    Unknown,
};

inline bool EqualsFsType(const std::wstring& a, const wchar_t* b)
{
#if PHOENIX_OS_WINDOWS
    return _wcsicmp(a.c_str(), b) == 0;
#else
    return wcscasecmp(a.c_str(), b) == 0;
#endif
}

inline FsModuleId ClassifyFsType(const std::wstring& fsType)
{
    if (EqualsFsType(fsType, L"NTFS")) {
        return FsModuleId::Ntfs;
    }
    if (EqualsFsType(fsType, L"FAT32") || EqualsFsType(fsType, L"FAT16") ||
        EqualsFsType(fsType, L"FAT12") || EqualsFsType(fsType, L"FAT")) {
        return FsModuleId::Fat;
    }
    if (EqualsFsType(fsType, L"exFAT")) {
        return FsModuleId::Exfat;
    }
    if (EqualsFsType(fsType, L"ext4") || EqualsFsType(fsType, L"ext3") ||
        EqualsFsType(fsType, L"ext2") || EqualsFsType(fsType, L"EXT4") ||
        EqualsFsType(fsType, L"EXT3") || EqualsFsType(fsType, L"EXT2")) {
        return FsModuleId::Ext4;
    }
    return FsModuleId::Unknown;
}

inline bool SupportsFsType(const std::wstring& fsType)
{
#if PHOENIX_EXT_FS_CODEPATH
    return ClassifyFsType(fsType) == FsModuleId::Ext4;
#else
    return ClassifyFsType(fsType) != FsModuleId::Unknown;
#endif
}

std::unique_ptr<FileSystem> CreateParser(const std::wstring& fsType);

// Uses partition.fsType when known; otherwise probes
// NTFS → exFAT → FAT → ext2/3/4 (Linux ext-only builds probe ext only).
std::unique_ptr<FileSystem> CreateParserForPartition(SectorReader& reader,
                                                     const Partition& partition,
                                                     ScanResult& result);

} // namespace FileSystemModuleService
