#pragma once

#include "../FileSystem.h"
#include "ExfatStructs.h"
#include "../../disk/SectorReader.h"
#include <vector>
#include <string>
#include <atomic>
#include <functional>

class ExfatParser : public FileSystem
{
public:
    bool Open(SectorReader& reader, const Partition& partition) override;

    bool ScanMetadata(
        std::function<void(int)> onProgress = nullptr,
        std::atomic<bool>*       cancelFlag  = nullptr) override;

    std::vector<FileNode> GetAllFiles()       const override { return m_files; }
    std::wstring          GetFsTypeName()     const override { return L"exFAT"; }
    bool                  IsValid()           const override { return m_valid;  }
    uint32_t              GetCurrentFileCount() const override
                              { return static_cast<uint32_t>(m_files.size()); }
    uint32_t              GetTotalRecordCount() const override { return m_totalRecords; }
    uint32_t              GetInUseCount()       const override { return m_inUseCount;   }

    void SetFileFoundCallback(FileFoundCallback cb) override
         { m_onFileFound = std::move(cb); }

private:
    // ── Directory scanning ────────────────────────────────────────────────
    void ScanDirectory(uint32_t startCluster, const std::wstring& path,
                       std::atomic<bool>* cancelFlag, int depth = 0,
                       bool ancestorsDeleted = false);

    // ── Cluster helpers ───────────────────────────────────────────────────
    // Build a list of absolute sector numbers for the cluster chain.
    // If noFatChain is true the file is contiguous: extend sequentially
    // from startCluster until clusterCount clusters are collected.
    std::vector<uint64_t> BuildClusterChain(uint32_t startCluster,
                                             uint32_t clusterCount,
                                             bool     noFatChain,
                                             uint64_t fileSize,
                                             const std::wstring& extension);
    uint64_t ClusterToSector(uint32_t cluster) const;
    uint32_t NextCluster    (uint32_t cluster);

    // ── Allocation bitmap ─────────────────────────────────────────────────
    // Loaded once from the first pass through the root directory.
    bool     LoadAllocationBitmap();
    bool     IsClusterFree(uint32_t cluster) const;

    // Return recoverability for `clusterCount` clusters starting at firstCluster.
    // Uses the allocation bitmap if available; falls back to Unknown.
    RecoverabilityLevel AssessRecoverability(uint32_t firstCluster,
                                             uint32_t clusterCount);

    // ── Timestamp conversion ──────────────────────────────────────────────
    // exFAT packs date+time into one 32-bit word:
    //   [31:25] year-1980  [24:21] month  [20:16] day
    //   [15:11] hour       [10:5]  minute [4:0]   2-seconds
    // utcOffsetQuarters: signed 7-bit, each unit = 15 min; 0x80 = no offset.
    static uint64_t ExfatTimeToFiletime(uint32_t t, uint8_t utcOff = 0x80,
                                        uint8_t tenMsIncrement = 0);

    // ── State ─────────────────────────────────────────────────────────────
    SectorReader*      m_reader               = nullptr;
    Partition          m_part;
    EXFAT_BOOT_SECTOR  m_boot                 = {};

    uint32_t m_bytesPerSector                 = 512;
    uint32_t m_sectorsPerCluster              = 1;
    uint64_t m_clusterHeapStartSector         = 0;
    uint64_t m_fatStartSector                 = 0;
    uint64_t m_partStartSector                = 0;

    std::vector<uint8_t> m_fatTable;           // exFAT FAT  (32-bit entries)
    std::vector<uint8_t> m_allocBitmap;        // Allocation bitmap (1 bit/cluster)

    bool     m_valid          = false;
    bool     m_bitmapLoaded   = false;
    uint32_t m_totalClusters  = 0;
    uint32_t m_totalRecords   = 0;
    uint32_t m_inUseCount     = 0;

    std::vector<FileNode> m_files;
    uint64_t              m_nextId = 1;
    FileFoundCallback     m_onFileFound;
};
