#pragma once

#include "../FileSystem.h"
#include "FatStructs.h"
#include "../../disk/SectorReader.h"
#include "../../recover/FragmentStitchPolicy.h"
#include <vector>
#include <string>
#include <cstdint>

// ---------------------------------------------------------------------------
// FatParser – implements FileSystem for FAT12, FAT16, and FAT32 volumes.
// ---------------------------------------------------------------------------
class FatParser : public FileSystem
{
public:
    bool Open(SectorReader& reader, const Partition& partition) override;

    bool ScanMetadata(
        std::function<void(int)> onProgress = nullptr,
        std::atomic<bool>*       cancelFlag  = nullptr) override;

    std::vector<FileNode> GetAllFiles() const override { return m_files; }
    std::wstring          GetFsTypeName() const override;
    bool                  IsValid()       const override { return m_valid; }
    uint32_t              GetCurrentFileCount() const override
                              { return static_cast<uint32_t>(m_files.size()); }

    void SetFileFoundCallback(FileFoundCallback cb) override
         { m_onFileFound = std::move(cb); }

    void SetFragmentStitchPolicy(const FragmentStitchPolicy& policy)
    {
        m_fragmentStitchPolicy = policy;
    }

private:
    bool ReadBPB();
    void ScanDirectory(uint32_t startCluster, const std::wstring& path,
                       std::atomic<bool>* cancelFlag,
                       int depth = 0);

    // Fallback: linear scan of data area for orphan directory entries that are
    // no longer reachable via the FAT directory tree.  Results are placed
    // under a synthetic path like "\LostDir\".
    void ScanOrphanDirEntries(std::atomic<bool>* cancelFlag);

    // Returns the next cluster in the FAT chain; 0 = free / EOC
    uint32_t NextCluster(uint32_t cluster);

    // Collect all clusters in a chain starting at 'startCluster'.
    // When fileSize > 0 and the FAT entry is free (deleted file chain is lost),
    // the chain is speculatively extended with sequential clusters up to the
    // number required by fileSize.  This recovers most non-fragmented deleted files.
    std::vector<uint64_t> BuildClusterChain(uint32_t startCluster,
                                             uint64_t fileSize = 0,
                                             const std::wstring& extension = L"");

    // Absolute sector (on physical disk) for a given cluster
    uint64_t ClusterToAbsoluteSector(uint32_t cluster) const;

    // ── State ─────────────────────────────────────────────────────────────
    SectorReader*  m_reader       = nullptr;
    Partition      m_part;

    uint8_t  m_fatType            = 32;    // 12, 16, or 32
    uint32_t m_bytesPerSector     = 512;
    uint32_t m_sectorsPerCluster  = 1;
    uint32_t m_reservedSectors    = 0;
    uint8_t  m_numFATs            = 2;
    uint32_t m_fatSizeSectors     = 0;
    uint32_t m_rootCluster        = 2;     // FAT32
    uint32_t m_rootDirSectors     = 0;     // FAT12/16 (32 entries × 32 B = 1 sector each)
    uint64_t m_dataStartSector    = 0;     // Absolute sector on disk
    uint64_t m_fatStartSector     = 0;     // Absolute sector on disk
    uint64_t m_rootDirStartSector = 0;     // Absolute sector on disk (FAT16)
    uint64_t m_totalClusters      = 0;

    std::vector<uint8_t> m_fatTable; // Entire FAT loaded into RAM (up to 128 MB)

    bool m_valid = false;
    std::vector<FileNode> m_files;
    uint64_t m_nextId = 1;
    FileFoundCallback m_onFileFound;
    FragmentStitchPolicy m_fragmentStitchPolicy;
};
