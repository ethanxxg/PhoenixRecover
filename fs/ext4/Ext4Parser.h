#pragma once

#include "../FileSystem.h"
#include "Ext4Structs.h"
#include "../../disk/SectorReader.h"
#include <vector>
#include <string>
#include <memory>
#include <set>
#include <unordered_map>
#include <atomic>
#include <functional>

class Ext4Parser : public FileSystem
{
public:
    bool Open(SectorReader& reader, const Partition& partition) override;

    bool ScanMetadata(
        std::function<void(int)> onProgress = nullptr,
        std::atomic<bool>*       cancelFlag  = nullptr) override;

    std::vector<FileNode> GetAllFiles()       const override { return m_files; }
    std::wstring          GetFsTypeName()     const override { return L"ext4"; }
    bool                  IsValid()           const override { return m_valid; }
    uint32_t              GetCurrentFileCount() const override
                              { return static_cast<uint32_t>(m_files.size()); }
    uint32_t              GetTotalRecordCount() const override { return m_totalInodes; }
    uint32_t              GetInUseCount()       const override { return m_inUseCount; }

    void SetFileFoundCallback(FileFoundCallback cb) override
         { m_onFileFound = std::move(cb); }

    // ── JBD2 replay diagnostics ───────────────────────────────────────────
    // The depth gate asserts on these so a recovered file cannot be attributed
    // to luck: the name only exists in the journal, so it can only come back
    // if the overlay was really built.
    uint32_t GetJournalCommitCount()   const { return m_journalCommits; }
    uint32_t GetJournalOverlayBlocks() const
         { return static_cast<uint32_t>(m_overlay.size()); }

    // ── External journal device (s_journal_dev != 0, s_journal_inum == 0) ──
    // ext4 may keep the JBD log on a separate device.  Recovery cannot guess
    // that device from the volume image, so the caller must supply it
    // explicitly before Open().  The superblock journal UUID is verified
    // against the external journal's s_uuid; a mismatch refuses replay.
    bool SetExternalJournalImage(const std::wstring& imagePath);

private:
    // ── Inode reading ─────────────────────────────────────────────────────
    bool ReadInode(uint32_t inodeNum, EXT4_INODE& outInode);

    // ── inline_data (EXT4_INLINE_DATA_FL) ─────────────────────────────────
    // Small files/directories keep their body inside the inode: the first 60
    // bytes live in i_block[], any remainder in the "system.data" in-inode
    // xattr (ibody).  Returns false when the payload cannot be fully
    // assembled, so callers never emit a silently truncated file.
    bool ReadInodeRaw(uint32_t inodeNum, std::vector<uint8_t>& raw);
    bool ReadInlineData(uint32_t inodeNum, const EXT4_INODE& inode,
                        uint64_t size, std::vector<uint8_t>& out);
    bool ParseInodeXattrData(const std::vector<uint8_t>& raw,
                             const EXT4_INODE& inode, uint64_t wantBytes,
                             std::vector<uint8_t>& out);
    
    // ── Block reading (handles 64-bit block addresses) ────────────────────
    bool ReadBlock(uint64_t blockNum, void* buffer);
    bool ReadBlocks(uint64_t startBlock, uint32_t count, void* buffer);

    // ── Byte-level read that honours the journal overlay ──────────────────
    // Every path that touches metadata (ReadBlock/ReadBlocks/ReadInode/
    // ReadInodeRaw) funnels through here, because ReadInode* address the inode
    // table arithmetically instead of going through ReadBlock.
    bool ReadFsBytes(uint64_t byteOffset, void* buffer, uint32_t len);

    // ── JBD2 journal replay ───────────────────────────────────────────────
    // The scan source is strictly read-only, so replay is an in-memory
    // overlay (block number -> committed content) rather than a write-back:
    // the "unflushed" metadata is presented to the parser, never to the disk.
    bool ReplayJournal();
    bool JournalBlockMap(const EXT4_INODE& jinode, std::vector<uint64_t>& map);
    bool ReadJournalAt(const std::vector<uint64_t>& map, uint32_t index, void* out);
    bool ReloadGroupDescriptors();
    static uint32_t Be32(const uint8_t* p)
    {
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
             | (static_cast<uint32_t>(p[2]) <<  8) |  static_cast<uint32_t>(p[3]);
    }
    static uint16_t Be16(const uint8_t* p)
    {
        return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
    }

    // ── Extent tree traversal ─────────────────────────────────────────────
    // Returns list of (physical_block, block_count) for the inode's data
    std::vector<std::pair<uint64_t, uint32_t>> GetExtentBlocks(const EXT4_INODE& inode);
    void ParseExtentNode(const uint8_t* data, uint16_t depth,
                         std::vector<std::pair<uint64_t, uint32_t>>& out,
                         uint64_t& nextLogicalBlock);

    // ── Legacy block map (i_block[0..14]) ─────────────────────────────────
    std::vector<std::pair<uint64_t, uint32_t>> GetIndirectBlocks(const EXT4_INODE& inode);
    void ReadIndirectLevel(uint64_t blockNum, int level,
                           std::vector<std::pair<uint64_t, uint32_t>>& out,
                           uint32_t& remaining);

    // ── Directory parsing ─────────────────────────────────────────────────
    std::vector<uint64_t> GetDirectoryLogicalBlocks(const EXT4_INODE& dirInode);
    bool ReadDirectoryLogicalBlock(const std::vector<uint64_t>& logicalBlocks,
                                   uint32_t logicalIdx, uint8_t* buffer);
    static bool IsDxRootBlock(const uint8_t* data, uint32_t blockSize);
    void ScanIndexedDirectory(uint32_t dirInodeNum, const std::wstring& path,
                              const EXT4_INODE& dirInode,
                              const std::vector<uint64_t>& logicalBlocks,
                              std::atomic<bool>* cancelFlag);
    void ScanDxBlock(uint32_t dirInodeNum, const std::wstring& path,
                     const std::vector<uint64_t>& logicalBlocks,
                     uint32_t logicalBlock, uint8_t nodeDepth,
                     uint32_t parentInode, std::atomic<bool>* cancelFlag);
    void ScanDirectory(uint32_t inodeNum, const std::wstring& path,
                       std::atomic<bool>* cancelFlag);
    void ParseDirBlock(const uint8_t* data, uint32_t size,
                       uint32_t parentInode, const std::wstring& path,
                       std::atomic<bool>* cancelFlag);
    // W6: ext2/ext3 unlink grows the PREVIOUS dirent's rec_len, leaving the
    // deleted dirent's bytes (including its original name) intact in the
    // slack. Scan that slack for residual dirents whose inode really looks
    // deleted so recovery keeps the on-disk name instead of "inode_<n>".
    void ScanDirentSlack(const uint8_t* data, uint32_t size,
                         uint32_t entryOffset, uint32_t entryRecLen,
                         uint32_t entryNameLen,
                         uint32_t parentInode, const std::wstring& path,
                         std::atomic<bool>* cancelFlag);
    void EmitDirEntry(uint32_t entryInode, uint8_t fileType, bool dentryDeleted,
                      const std::wstring& name, uint32_t parentInode,
                      const std::wstring& path, std::atomic<bool>* cancelFlag);
    static bool InodeLooksDeleted(const EXT4_INODE& inode);
    void ScanOrphanInodes(std::atomic<bool>* cancelFlag);

    // ── Time conversion ───────────────────────────────────────────────────
    static uint64_t UnixToFiletime(uint32_t unixTime, uint32_t extraBits = 0);

    // ── Recoverability assessment ─────────────────────────────────────────
    RecoverabilityLevel AssessRecoverability(uint32_t inodeNum);

    // ── State ─────────────────────────────────────────────────────────────
    SectorReader*      m_reader       = nullptr;
    Partition          m_part;
    EXT4_SUPERBLOCK    m_sb           = {};

    bool     m_valid        = false;
    bool     m_is64Bit      = false;
    bool     m_hasExtents   = false;
    bool     m_hasFiletype  = false;
    bool     m_hasInlineData = false;
    uint32_t m_blockSize    = 1024;
    // bigalloc: allocation unit is 1 << (s_log_cluster_size - s_log_block_size)
    // blocks.  Extent ee_start / ee_len are expressed in CLUSTERS, so they must
    // be scaled before a block-level read (m_clusterRatio == 1 otherwise).
    uint32_t m_clusterRatio = 1;
    uint32_t m_inodeSize    = 128;
    uint32_t m_descSize     = 32;
    uint32_t m_groupCount   = 0;
    uint64_t m_partStartByte = 0;

    // Inode bitmap cache for recoverability (loaded per-group)
    std::vector<uint8_t> m_inodeBitmap;
    uint32_t             m_inodeBitmapGroup = UINT32_MAX;

    std::vector<EXT4_GROUP_DESC> m_groupDescs;
    std::set<uint32_t>           m_visitedInodes;
    std::set<uint32_t>           m_seenInodes;

    // JBD2 replay result.  Empty when the volume has no journal, when it was
    // cleanly unmounted (s_start == 0) or when the log was unreadable.
    std::unordered_map<uint64_t, std::vector<uint8_t>> m_overlay;
    // block number -> sequence of the last revoke record naming it
    std::unordered_map<uint64_t, uint32_t>             m_revoke;
    uint32_t m_journalCommits = 0;
    std::unique_ptr<SectorReader> m_externalJournal;
    bool m_externalJournalReady = false;

    std::vector<FileNode> m_files;
    uint64_t              m_nextId      = 1;
    uint32_t              m_totalInodes = 0;
    uint32_t              m_inUseCount  = 0;

    FileFoundCallback     m_onFileFound;
};
