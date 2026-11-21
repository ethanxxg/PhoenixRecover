#pragma once

#include "../FileSystem.h"
#include "NtfsStructs.h"
#include "../../disk/SectorReader.h"
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <atomic>

// ---------------------------------------------------------------------------
// NtfsParser – implements FileSystem for NTFS volumes.
//
// Scan flow (real-time asynchronous):
//   1. Open()          – validates boot sector, calculates geometry constants.
//   2. ScanMetadata()  – reads every MFT record;
//                        • builds m_recordMap (ALL valid records, for path resolution)
//                        • for each normal/deleted file or directory found:
//                          - immediately resolves path (may be partial if parent not yet scanned)
//                          - calls m_onFileFound callback in real-time
//                        • after scan completes, calls BuildFullPaths() to improve
//                          any paths that were partial during scanning
//   3. GetAllFiles()   – returns the full file system node list to the caller.
// ---------------------------------------------------------------------------
class NtfsParser : public FileSystem
{
public:
    bool Open(SectorReader& reader, const Partition& partition) override;

    bool ScanMetadata(
        std::function<void(int)> onProgress = nullptr,
        std::atomic<bool>*       cancelFlag  = nullptr) override;

    std::vector<FileNode> GetAllFiles() const override { return m_files; }
    std::wstring          GetFsTypeName() const override { return L"NTFS"; }
    bool                  IsValid()       const override { return m_valid; }

    // Real-time file count (all discovered nodes so far).
    uint32_t GetCurrentFileCount() const override {
        return static_cast<uint32_t>(m_files.size());
    }
    // Total valid MFT records visited (system records excluded).
    uint32_t GetTotalRecordCount() const override { return m_totalRecords; }
    // In-use (normal) records seen.
    uint32_t GetInUseCount()       const override { return m_inUseCount; }

    // Decode a DataRun list into an ordered vector of absolute LCNs (1 per cluster).
    // Returns empty vector on error.
    static std::vector<uint64_t> DecodeDataRuns(const uint8_t* dataRunPtr,
                                                size_t maxBytes);

    // Assess recoverability by checking the cluster bitmap.
    // Returns Unknown if the bitmap cannot be read.
    RecoverabilityLevel AssessRecoverability(const FileNode& node);

    // ── USN Journal ($UsnJrnl) scan ───────────────────────────────────────
    // Phase-2 scan that reads the NTFS change journal via Windows FSCTL API.
    // Must be called AFTER ScanMetadata() (needs m_recordMap for path resolution).
    //
    // Finds deleted nodes whose MFT records were already reused – nodes that
    // the standard MFT scan cannot see. New FileNodes are appended to
    // m_files; files already found via MFT (same FRN) are not duplicated.
    //
    // Returns the number of NEW files added from the journal.
    // Returns 0 if no drive letter is available (unmounted / disk image).
    uint32_t ScanUsnJournal(std::function<void(int /*pct*/)> onProgress = nullptr,
                            const std::atomic<bool>* cancelFlag = nullptr);

    // Set callback for real-time file discovery
    void SetFileFoundCallback(FileFoundCallback cb) override { m_onFileFound = std::move(cb); }

private:
    bool ReadBootSector();
    bool ReadMftDataRuns();

    // Try to read and validate MFT record 0 from the given byte offset.
    // Used by ReadMftDataRuns for primary + $MFTMirr fallback.
    bool TryReadMftRecord0(uint64_t byteOffset, std::vector<uint8_t>& recBuf);

    // Apply Update Sequence Array fix-ups to an in-memory record buffer.
    bool ApplyUSA(uint8_t* recBuf, size_t recSize);

    // Parse one MFT record (already USA-corrected).
    // Always registers the record in m_recordMap for path resolution.
    // Appends to m_files if the record has a valid file name.
    bool ParseRecord(const uint8_t* recBuf, uint64_t recordIndex);

    // Parse $FILE_NAME attribute value; return false if name is empty.
    bool ParseFileName(const uint8_t* attrValue, size_t valueLen,
                       FileNode& out);

    // Parse $DATA attribute; fills out.clusters (non-resident)
    //                        or out.rawOffset / out.rawSize (resident).
    bool ParseDataAttr(const ATTR_HEADER_COMMON* hdr, uint64_t recordIndex,
                       FileNode& out);

    // After all MFT records are parsed, re-resolve paths for files that had
    // partial paths during real-time scanning (parent directories not yet scanned).
    // Updates paths from partial to full and notifies callback if path improved.
    void BuildFullPaths();

    // Walk the parent chain for one node; returns a path like \Folder\Sub\file.txt
    std::wstring ResolveFullPath(uint64_t parentId,
                                 const std::wstring& fileName) const;

    // ── $Recycle.Bin metadata parser ────────────────────────────────────────
    // Parse $Ixxxxx.xxx files in $Recycle.Bin to extract original paths and
    // deletion times. Matches $I files with corresponding $R files (by prefix)
    // and updates the FileNode's originalPath and modifyTime.
    // Must be called AFTER ScanMetadata() completes (needs m_files populated).
    // Returns the number of files successfully updated with metadata.
    uint32_t ParseRecycleBinMetadata();

    // ── State ─────────────────────────────────────────────────────────────
    SectorReader*   m_reader    = nullptr;
    Partition       m_part;
    NTFS_BOOT_SECTOR m_boot     = {};

    uint64_t m_bytesPerCluster  = 0;
    uint32_t m_recordSize       = 1024;
    uint64_t m_partStartBytes   = 0;    // Byte offset of partition start on disk
    uint64_t m_mftByteOffset    = 0;    // Byte offset of $MFT on disk

    // DataRuns of the $MFT file itself (needed when MFT is fragmented)
    std::vector<uint64_t> m_mftClusters;

    // Cluster bitmap ($Bitmap) – loaded lazily for recoverability checks
    std::vector<uint8_t> m_bitmap;
    bool                 m_bitmapLoaded = false;

    bool m_valid = false;

    // ALL valid MFT records (system + normal + deleted) keyed by record number.
    // Used only for parent-chain resolution; never exposed to callers.
    struct RecordInfo {
        std::wstring name;
        uint64_t     parentId = 0;
        bool         isDir    = false;
    };
    std::unordered_map<uint64_t, RecordInfo> m_recordMap;

    // Full file system node list (normal + deleted + directories).
    std::vector<FileNode> m_files;

    // Scan statistics (available after ScanMetadata completes).
    uint32_t m_totalRecords = 0;  // all valid records (excl. 0-15)
    uint32_t m_inUseCount   = 0;  // records with MFT_FLAG_IN_USE set

    // Real-time file discovery callback
    FileFoundCallback m_onFileFound;
};
