#include "FileSystemModuleService.h"

#include "../disk/Partition.h"
#include "../disk/SectorReader.h"
#include "../fs/FileSystem.h"
#include "../fs/ext4/Ext4Parser.h"
#if !PHOENIX_EXT_FS_CODEPATH
#include "../fs/exfat/ExfatParser.h"
#include "../fs/fat/FatParser.h"
#include "../fs/ntfs/NtfsParser.h"
#include "../fs/refs/RefsParser.h"
#endif
#include "ScanEngine.h"

namespace FileSystemModuleService {

std::unique_ptr<FileSystem> CreateParser(const std::wstring& fsType)
{
    switch (ClassifyFsType(fsType)) {
#if !PHOENIX_EXT_FS_CODEPATH
    case FsModuleId::Ntfs:
        return std::make_unique<NtfsParser>();
    case FsModuleId::Fat:
        return std::make_unique<FatParser>();
    case FsModuleId::Exfat:
        return std::make_unique<ExfatParser>();
    case FsModuleId::Refs:
        return std::make_unique<RefsParser>();
#endif
    case FsModuleId::Ext4:
        return std::make_unique<Ext4Parser>();
    default:
        return nullptr;
    }
}

std::unique_ptr<FileSystem> CreateParserForPartition(SectorReader& reader,
                                                     const Partition& partition,
                                                     ScanResult& result)
{
    if (auto known = CreateParser(partition.fsType)) {
        return known;
    }

#if PHOENIX_EXT_FS_CODEPATH
    auto ext = std::make_unique<Ext4Parser>();
    if (ext->Open(reader, partition)) {
        return std::move(ext);
    }
    result.errorMsg = L"Could not identify ext2/ext3/ext4 file system on this volume";
    return nullptr;
#else
    auto ntfs = std::make_unique<NtfsParser>();
    if (ntfs->Open(reader, partition)) {
        return std::move(ntfs);
    }

    // exFAT has a distinct OEM ID ("EXFAT   ") at byte 3 and a shift-based
    // BPB.  Probe it before FAT: the FAT BPB reads bytesPerSector at offset 11,
    // which is zero on exFAT, so FAT would reject the volume anyway — but an
    // explicit probe makes the routing deterministic and lets an exFAT volume
    // be scanned even when partition.fsType is unknown (e.g. lost partitions,
    // raw images handed to the sector-based scanner).
    auto exfat = std::make_unique<ExfatParser>();
    if (exfat->Open(reader, partition)) {
        return std::move(exfat);
    }

    // ReFS before FAT: the ReFS VBR carries an NTFS-like BPB, so a FAT probe
    // on a wiped or half-valid ReFS volume could otherwise claim it.  The ReFS
    // engine validates VBR checksum + SUPB/CHKP copies, which FAT does not.
    auto refs = std::make_unique<RefsParser>();
    if (refs->Open(reader, partition)) {
        return std::move(refs);
    }

    auto fat = std::make_unique<FatParser>();
    if (fat->Open(reader, partition)) {
        return std::move(fat);
    }

    auto ext4 = std::make_unique<Ext4Parser>();
    if (ext4->Open(reader, partition)) {
        return std::move(ext4);
    }

    result.errorMsg = L"Could not identify file system for sector-based volume scan";
    return nullptr;
#endif
}

} // namespace FileSystemModuleService
