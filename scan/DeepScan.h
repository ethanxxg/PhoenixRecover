#pragma once
#include "ScanEngine.h"

class NtfsParser;

// ---------------------------------------------------------------------------
// FatDeepInfo – FAT geometry read from the boot sector at the start of
//               DeepScan::Run().  Used to anchor cluster → byte-offset
//               conversion when building FileNode.clusters for FAT files.
// ---------------------------------------------------------------------------
struct FatDeepInfo
{
    bool     valid             = false;
    uint32_t bytesPerSector    = 512;
    uint32_t sectorsPerCluster = 1;
    uint64_t partStartSector   = 0;   // abs sector of partition start
    uint64_t dataStartSector   = 0;   // abs sector of cluster 2
    uint8_t  fatType           = 0;   // 12 | 16 | 32
};

// ---------------------------------------------------------------------------
// DeepScan – sector-by-sector scan that looks for valid MFT records AND
//            FAT directory entries regardless of file system state.
//            Suitable for formatted or damaged volumes.
// ---------------------------------------------------------------------------
class DeepScan : public ScanEngine
{
public:
    ScanResult Run(SectorReader& reader, const Partition& partition) override;

private:
    // ── NTFS helpers ──────────────────────────────────────────────────────
    bool TryParseMftRecord(const uint8_t* buf, size_t bufLen,
                           uint64_t diskByteOffset,
                           std::vector<FileNode>& out);

    // NTFS parser used only for bitmap-based recoverability assessment.
    NtfsParser* m_ntfsHelper = nullptr;

    // ── FAT helpers ───────────────────────────────────────────────────────

    // Attempt to read the boot sector and populate FatDeepInfo.
    static bool DetectFatBpb(SectorReader& reader, const Partition& part,
                              FatDeepInfo& out);

    // Heuristic: returns true when a sector looks like a FAT directory page.
    // High specificity – avoids false-positives from random binary data.
    static bool IsLikelyFatDirSector(const uint8_t* p, uint32_t sectorSize);

    // Parse a FAT directory sector; appends found deleted FileNodes to `out`.
    // Returns the count of deleted files found.
    int TryParseFatDirSector(const uint8_t* p, uint32_t sectorSize,
                              uint64_t absSector,
                              const FatDeepInfo& fi,
                              std::vector<FileNode>& out);

    // Extract the 13 UTF-16 characters from one LFN entry (strips 0xFFFF pad).
    static std::wstring ExtractLfnChars(const uint8_t* lfnEntry);

    // Reconstruct the 8.3 name (ASCII→wstring, trim spaces, handle 0xE5).
    static std::wstring Reconstruct83Name(const uint8_t* dirEntry, bool deleted);
};
