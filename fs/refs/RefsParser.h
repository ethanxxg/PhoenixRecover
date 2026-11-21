#pragma once

// ---------------------------------------------------------------------------
// RefsParser – FileSystem adapter for the header-only ReFS engine
//              (fs/refs/RefsVolume.h).
//
// The engine was built as a standalone read-only parser (A1-5a) and had no
// path into the product: no FileSystem implementation, no probe entry, no
// recoverable cluster list.  This adapter closes that gap:
//
//   SectorReader + Partition   ->  ReaderImage : IRefsImage
//   RefsVolume::Open           ->  VBR / SUPB / CHKP / container / object tables
//   RefsVolume::ListDirectory  ->  FileNode stream (names, times, attributes)
//   extents (VLCN)             ->  TranslateVlcn -> absolute cluster numbers,
//                                  which is exactly what RecoverEngine reads
//   inline data                ->  FileNode::residentData
//
// Coverage boundary (must not be over-claimed):
//   * Deleted files: ReFS does not keep unlinked directory entries, so only
//     live entries are enumerated.  There is no deleted-name recovery here.
//   * Split records (key_flags 0x02) are reported as entries without data
//     (dataAvailable = false) and marked Lost, never as zero-filled files.
//   * Nested directories: a directory entry carries a name, metadata and an
//     entry ordinal but NOT the child object id, so the tree is rebuilt from
//     the parent-child table (authoritative OID -> OID links) and the entry
//     names are bound to those OIDs in B+-tree order.  Bound against the
//     refs-golden corpus, which contains a nested subdir.
// ---------------------------------------------------------------------------

#include "../FileSystem.h"
#include "RefsVolume.h"
#include "../../disk/SectorReader.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RefsParser : public FileSystem
{
public:
    RefsParser();
    ~RefsParser() override;

    bool Open(SectorReader& reader, const Partition& partition) override;

    bool ScanMetadata(
        std::function<void(int)> onProgress = nullptr,
        std::atomic<bool>*       cancelFlag  = nullptr) override;

    std::vector<FileNode> GetAllFiles()       const override { return m_files; }
    std::wstring          GetFsTypeName()     const override { return L"ReFS"; }
    bool                  IsValid()           const override { return m_valid; }
    uint32_t              GetCurrentFileCount() const override
                              { return static_cast<uint32_t>(m_files.size()); }
    uint32_t              GetTotalRecordCount() const override { return m_totalEntries; }
    uint32_t              GetInUseCount()       const override { return m_inUseCount; }

    void SetFileFoundCallback(FileFoundCallback cb) override
         { m_onFileFound = std::move(cb); }

    // Last engine-level failure, for diagnostics ("could not identify ReFS").
    const std::wstring& LastError() const { return m_lastError; }

private:
    class ReaderImage;

    void EnumerateDirectory(uint64_t objectId, uint64_t parentNodeId,
                            const std::wstring& path, int depth,
                            std::atomic<bool>* cancelFlag);
    bool BuildNode(const phoenix::refs::RefsDirEntry& entry, uint64_t parentNodeId,
                   const std::wstring& path, FileNode& out);

    SectorReader*      m_reader = nullptr;
    Partition          m_part;
    std::unique_ptr<ReaderImage>        m_image;
    std::unique_ptr<phoenix::refs::RefsVolume> m_volume;

    bool                  m_valid        = false;
    uint32_t              m_totalEntries = 0;
    uint32_t              m_inUseCount   = 0;
    uint64_t              m_nextId       = 1;
    std::vector<FileNode> m_files;
    std::wstring          m_lastError;
    FileFoundCallback     m_onFileFound;
};
