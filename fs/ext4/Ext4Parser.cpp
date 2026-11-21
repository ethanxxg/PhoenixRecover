#include "Ext4Parser.h"
#include "../../common/Utils.h"
#include "../../log/Logger.h"
#include <algorithm>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// UnixToFiletime – convert Unix timestamp to Windows FILETIME (100-ns ticks)
// ─────────────────────────────────────────────────────────────────────────────
uint64_t Ext4Parser::UnixToFiletime(uint32_t unixTime, uint32_t extraBits)
{
    if (unixTime == 0) return 0;
    // Unix epoch = Jan 1, 1970; FILETIME epoch = Jan 1, 1601
    // Difference = 11644473600 seconds
    // 1 second = 10,000,000 100-ns intervals
    constexpr uint64_t kEpochDiff = 116444736000000000ULL;
    
    // extraBits contains nanoseconds in bits [30:2] if present
    uint64_t nanos = 0;
    if (extraBits) {
        nanos = (extraBits >> 2) & 0x3FFFFFFF;  // 30 bits of ns
    }
    
    uint64_t ticks = static_cast<uint64_t>(unixTime) * 10000000ULL + kEpochDiff;
    ticks += nanos / 100;  // Convert ns to 100-ns intervals
    return ticks;
}

// ─────────────────────────────────────────────────────────────────────────────
// Open – read and validate superblock
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::Open(SectorReader& reader, const Partition& partition)
{
    m_reader = &reader;
    m_part   = partition;
    m_valid  = false;
    m_files.clear();
    m_groupDescs.clear();
    m_visitedInodes.clear();
    m_seenInodes.clear();
    m_nextId = 1;
    m_totalInodes = 0;
    m_inUseCount  = 0;
    m_partStartByte = partition.startSector * reader.GetSectorSize();

    // Superblock is at byte offset 1024 from the start of the partition
    std::vector<uint8_t> buf(2048);
    if (!reader.ReadBytes(m_partStartByte, buf.data(), 2048)) {
        LOG_ERROR(L"Ext4Parser: cannot read superblock area");
        return false;
    }

    memcpy(&m_sb, buf.data() + 1024, sizeof(m_sb));

    // Validate magic number
    if (m_sb.s_magic != EXT4_SUPER_MAGIC) {
        LOG_WARN(L"Ext4Parser: invalid magic 0x" +
                 Utils::ToHexW(m_sb.s_magic) + L", expected 0xEF53");
        return false;
    }

    // Parse geometry
    m_blockSize = 1024u << m_sb.s_log_block_size;
    if (m_blockSize < 1024 || m_blockSize > 65536) {
        LOG_ERROR(L"Ext4Parser: invalid block size " + std::to_wstring(m_blockSize));
        return false;
    }

    m_inodeSize = (m_sb.s_rev_level >= 1) ? m_sb.s_inode_size : 128;
    if (m_inodeSize < 128 || m_inodeSize > 4096) {
        LOG_ERROR(L"Ext4Parser: invalid inode size " + std::to_wstring(m_inodeSize));
        return false;
    }

    // Check features
    m_hasExtents  = (m_sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)  != 0;
    m_is64Bit     = (m_sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)    != 0;
    m_hasFiletype = (m_sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_FILETYPE) != 0;

    // Group descriptor size
    m_descSize = (m_is64Bit && m_sb.s_desc_size >= 64) ? m_sb.s_desc_size : 32;

    // Calculate number of block groups
    uint64_t totalBlocks = m_sb.s_blocks_count_lo;
    if (m_is64Bit)
        totalBlocks |= (uint64_t)m_sb.s_blocks_count_hi << 32;

    m_groupCount = static_cast<uint32_t>(
        (totalBlocks + m_sb.s_blocks_per_group - 1) / m_sb.s_blocks_per_group);

    if (m_groupCount == 0 || m_groupCount > 1000000) {
        LOG_ERROR(L"Ext4Parser: invalid group count " + std::to_wstring(m_groupCount));
        return false;
    }

    // Read group descriptors
    // Group descriptors start at block 0 or 1 (depending on block size)
    uint64_t gdtBlock = (m_blockSize == 1024) ? 2 : 1;
    uint64_t gdtBytes = static_cast<uint64_t>(m_groupCount) * m_descSize;
    uint32_t gdtBlocks = static_cast<uint32_t>(
        (gdtBytes + m_blockSize - 1) / m_blockSize);

    std::vector<uint8_t> gdtBuf(static_cast<size_t>(gdtBlocks) * m_blockSize);
    if (!ReadBlocks(gdtBlock, gdtBlocks, gdtBuf.data())) {
        LOG_ERROR(L"Ext4Parser: cannot read group descriptors");
        return false;
    }

    m_groupDescs.resize(m_groupCount);
    for (uint32_t g = 0; g < m_groupCount; ++g) {
        const uint8_t* p = gdtBuf.data() + g * m_descSize;
        memcpy(&m_groupDescs[g], p, std::min((size_t)m_descSize, sizeof(EXT4_GROUP_DESC)));
    }

    m_valid = true;
    LOG_INFO(L"Ext4Parser::Open ok"
             L"  blockSize="   + std::to_wstring(m_blockSize) +
             L"  inodeSize="   + std::to_wstring(m_inodeSize) +
             L"  groups="      + std::to_wstring(m_groupCount) +
             L"  extents="     + (m_hasExtents ? L"yes" : L"no") +
             L"  64bit="       + (m_is64Bit ? L"yes" : L"no"));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadBlock / ReadBlocks
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ReadBlock(uint64_t blockNum, void* buffer)
{
    uint64_t offset = m_partStartByte + blockNum * m_blockSize;
    return m_reader->ReadBytes(offset, buffer, m_blockSize);
}

bool Ext4Parser::ReadBlocks(uint64_t startBlock, uint32_t count, void* buffer)
{
    uint64_t offset = m_partStartByte + startBlock * m_blockSize;
    return m_reader->ReadBytes(offset, buffer, count * m_blockSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadInode
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ReadInode(uint32_t inodeNum, EXT4_INODE& outInode)
{
    if (inodeNum == 0 || inodeNum > m_sb.s_inodes_count) return false;

    // Inode numbers are 1-based
    uint32_t idx = inodeNum - 1;
    uint32_t group = idx / m_sb.s_inodes_per_group;
    uint32_t localIdx = idx % m_sb.s_inodes_per_group;

    if (group >= m_groupCount) return false;

    // Get inode table block for this group
    uint64_t inodeTableBlock = m_groupDescs[group].bg_inode_table_lo;
    if (m_is64Bit)
        inodeTableBlock |= (uint64_t)m_groupDescs[group].bg_inode_table_hi << 32;

    // Calculate byte offset of the inode within the table
    uint64_t inodeOffset = m_partStartByte
                         + inodeTableBlock * m_blockSize
                         + static_cast<uint64_t>(localIdx) * m_inodeSize;

    if (!m_reader->ReadBytes(inodeOffset, &outInode,
                             std::min((uint32_t)sizeof(outInode), m_inodeSize))) {
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// GetExtentBlocks – parse extent tree to get physical blocks
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::pair<uint64_t, uint32_t>> Ext4Parser::GetExtentBlocks(const EXT4_INODE& inode)
{
    std::vector<std::pair<uint64_t, uint32_t>> result;

    const auto* hdr = reinterpret_cast<const EXT4_EXTENT_HEADER*>(inode.i_block);
    if (hdr->eh_magic != EXT4_EXT_MAGIC) {
        LOG_WARN(L"Ext4Parser: invalid extent magic");
        return result;
    }

    ParseExtentNode(inode.i_block, hdr->eh_depth, result);
    return result;
}

void Ext4Parser::ParseExtentNode(const uint8_t* data, uint16_t depth,
                                  std::vector<std::pair<uint64_t, uint32_t>>& out)
{
    const auto* hdr = reinterpret_cast<const EXT4_EXTENT_HEADER*>(data);
    if (hdr->eh_magic != EXT4_EXT_MAGIC) return;

    uint16_t entries = hdr->eh_entries;
    if (entries == 0) return;

    const uint8_t* entryPtr = data + sizeof(EXT4_EXTENT_HEADER);

    if (depth == 0) {
        // Leaf node: contains EXT4_EXTENT entries
        for (uint16_t i = 0; i < entries; ++i) {
            const auto* ext = reinterpret_cast<const EXT4_EXTENT*>(
                entryPtr + i * sizeof(EXT4_EXTENT));
            uint64_t startBlock = ext->ee_start_lo
                                | ((uint64_t)ext->ee_start_hi << 32);
            uint16_t len = ext->ee_len;
            if (len == 0)
                continue;
            // ee_len > 32768 encodes length as (len - 32768); do not skip.
            if (len > 32768)
                len = static_cast<uint16_t>(len - 32768);
            if (len == 0)
                continue;
            out.emplace_back(startBlock, len);
        }
    } else {
        // Index node: contains EXT4_EXTENT_IDX entries
        std::vector<uint8_t> childBuf(m_blockSize);
        for (uint16_t i = 0; i < entries; ++i) {
            const auto* idx = reinterpret_cast<const EXT4_EXTENT_IDX*>(
                entryPtr + i * sizeof(EXT4_EXTENT_IDX));
            uint64_t childBlock = idx->ei_leaf_lo
                                | ((uint64_t)idx->ei_leaf_hi << 32);
            if (!ReadBlock(childBlock, childBuf.data())) continue;
            ParseExtentNode(childBuf.data(), depth - 1, out);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GetIndirectBlocks – legacy block map (ext2/ext3 style)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::pair<uint64_t, uint32_t>> Ext4Parser::GetIndirectBlocks(const EXT4_INODE& inode)
{
    std::vector<std::pair<uint64_t, uint32_t>> result;
    const uint32_t* blocks = reinterpret_cast<const uint32_t*>(inode.i_block);

    // Calculate required block count
    uint64_t fileSize = inode.i_size_lo | ((uint64_t)inode.i_size_hi << 32);
    uint32_t reqBlocks = static_cast<uint32_t>((fileSize + m_blockSize - 1) / m_blockSize);
    uint32_t remaining = reqBlocks;

    // Direct blocks: i_block[0..11]
    for (int i = 0; i < 12 && remaining > 0; ++i) {
        if (blocks[i] != 0) {
            result.emplace_back(blocks[i], 1);
            --remaining;
        }
    }

    // Indirect block: i_block[12]
    if (remaining > 0 && blocks[12] != 0)
        ReadIndirectLevel(blocks[12], 1, result, remaining);

    // Double indirect: i_block[13]
    if (remaining > 0 && blocks[13] != 0)
        ReadIndirectLevel(blocks[13], 2, result, remaining);

    // Triple indirect: i_block[14]
    if (remaining > 0 && blocks[14] != 0)
        ReadIndirectLevel(blocks[14], 3, result, remaining);

    return result;
}

void Ext4Parser::ReadIndirectLevel(uint64_t blockNum, int level,
                                    std::vector<std::pair<uint64_t, uint32_t>>& out,
                                    uint32_t& remaining)
{
    if (remaining == 0 || blockNum == 0) return;

    std::vector<uint8_t> buf(m_blockSize);
    if (!ReadBlock(blockNum, buf.data())) return;

    uint32_t entriesPerBlock = m_blockSize / 4;  // 32-bit block numbers
    const uint32_t* entries = reinterpret_cast<const uint32_t*>(buf.data());

    for (uint32_t i = 0; i < entriesPerBlock && remaining > 0; ++i) {
        uint32_t entry = entries[i];
        if (entry == 0) continue;

        if (level == 1) {
            out.emplace_back(entry, 1);
            --remaining;
        } else {
            ReadIndirectLevel(entry, level - 1, out, remaining);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AssessRecoverability
// ─────────────────────────────────────────────────────────────────────────────
RecoverabilityLevel Ext4Parser::AssessRecoverability(uint32_t inodeNum)
{
    if (inodeNum == 0 || inodeNum > m_sb.s_inodes_count)
        return RecoverabilityLevel::Unknown;

    uint32_t idx = inodeNum - 1;
    uint32_t group = idx / m_sb.s_inodes_per_group;
    uint32_t localIdx = idx % m_sb.s_inodes_per_group;

    if (group >= m_groupCount) return RecoverabilityLevel::Unknown;

    // Load inode bitmap for this group if not cached
    if (m_inodeBitmapGroup != group) {
        uint64_t bitmapBlock = m_groupDescs[group].bg_inode_bitmap_lo;
        if (m_is64Bit)
            bitmapBlock |= (uint64_t)m_groupDescs[group].bg_inode_bitmap_hi << 32;

        m_inodeBitmap.resize(m_blockSize);
        if (!ReadBlock(bitmapBlock, m_inodeBitmap.data())) {
            m_inodeBitmap.clear();
            return RecoverabilityLevel::Unknown;
        }
        m_inodeBitmapGroup = group;
    }

    if (m_inodeBitmap.empty()) return RecoverabilityLevel::Unknown;

    // Check if inode is marked as free in the bitmap
    size_t byteIdx = localIdx / 8;
    uint8_t bitMask = 1u << (localIdx % 8);
    if (byteIdx >= m_inodeBitmap.size()) return RecoverabilityLevel::Unknown;

    bool isAllocated = (m_inodeBitmap[byteIdx] & bitMask) != 0;

    // For deleted files: if inode is now free, data might still be intact
    if (isAllocated)
        return RecoverabilityLevel::Lost;

    // Sample first data block to check if data is zeroed
    EXT4_INODE inode{};
    if (ReadInode(inodeNum, inode) && inode.i_size_lo > 0) {
        std::vector<std::pair<uint64_t, uint32_t>> extents;
        if (inode.i_flags & 0x80000) {
            extents = GetExtentBlocks(inode);
        } else {
            extents = GetIndirectBlocks(inode);
        }
        if (!extents.empty() && extents[0].first != 0) {
            std::vector<uint8_t> sample(m_blockSize);
            if (ReadBlocks(extents[0].first, 1, sample.data())) {
                bool allZero = true;
                uint32_t checkSize = (std::min)(512u, m_blockSize);
                for (uint32_t i = 0; i < checkSize; ++i) {
                    if (sample[i] != 0) { allZero = false; break; }
                }
                if (allZero)
                    return RecoverabilityLevel::Lost;
            }
        }
    }

    return RecoverabilityLevel::Excellent;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanMetadata
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ScanMetadata(std::function<void(int)> onProgress,
                               std::atomic<bool>* cancelFlag)
{
    if (!m_valid) return false;

    ScanDirectory(EXT4_ROOT_INO, L"\\", cancelFlag);
    ScanOrphanInodes(cancelFlag);

    if (onProgress) onProgress(100);

    size_t deletedCount = 0;
    for (const auto& f : m_files) {
        if (f.status == FileStatus::Deleted && !f.isDirectory)
            ++deletedCount;
    }

    LOG_INFO(L"Ext4Parser::ScanMetadata – total inodes=" +
             std::to_wstring(m_totalInodes) +
             L"  inUse=" + std::to_wstring(m_inUseCount) +
             L"  files=" + std::to_wstring(m_files.size()) +
             L"  deleted=" + std::to_wstring(deletedCount));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Directory block map (logical block index → physical block number)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint64_t> Ext4Parser::GetDirectoryLogicalBlocks(const EXT4_INODE& dirInode)
{
    std::vector<std::pair<uint64_t, uint32_t>> runs;
    if ((dirInode.i_flags & EXT4_EXTENTS_FL) && m_hasExtents)
        runs = GetExtentBlocks(dirInode);
    else
        runs = GetIndirectBlocks(dirInode);

    std::vector<uint64_t> logical;
    for (const auto& run : runs) {
        for (uint32_t i = 0; i < run.second; ++i)
            logical.push_back(run.first + i);
    }
    return logical;
}

bool Ext4Parser::ReadDirectoryLogicalBlock(const std::vector<uint64_t>& logicalBlocks,
                                           uint32_t logicalIdx, uint8_t* buffer)
{
    if (logicalIdx >= logicalBlocks.size())
        return false;
    return ReadBlock(logicalBlocks[logicalIdx], buffer);
}

bool Ext4Parser::IsDxRootBlock(const uint8_t* data, uint32_t blockSize)
{
    if (blockSize < EXT4_DX_ROOT_ENTRIES_OFF + 8)
        return false;

    const auto* info = reinterpret_cast<const EXT4_DX_ROOT_INFO*>(data + EXT4_DX_ROOT_INFO_OFF);
    if (info->reserved_zero != 0 || info->info_length != 8)
        return false;
    if (info->indirect_levels > 3)
        return false;

    const uint16_t limit = *reinterpret_cast<const uint16_t*>(data + EXT4_DX_ROOT_LIMIT_OFF);
    const uint16_t count = *reinterpret_cast<const uint16_t*>(data + EXT4_DX_ROOT_COUNT_OFF);
    if (count == 0 || count > limit)
        return false;

    const uint32_t maxEntries = (blockSize - EXT4_DX_ROOT_ENTRIES_OFF) / sizeof(EXT4_DX_ENTRY);
    if (count > maxEntries + 1)
        return false;

    return true;
}

bool Ext4Parser::InodeLooksDeleted(const EXT4_INODE& inode)
{
    if (inode.i_mode == 0)
        return false;
    if (inode.i_dtime != 0)
        return true;
    return inode.i_links_count == 0;
}

void Ext4Parser::ScanDxBlock(uint32_t dirInodeNum, const std::wstring& path,
                              const std::vector<uint64_t>& logicalBlocks,
                              uint32_t logicalBlock, uint8_t nodeDepth,
                              uint32_t parentInode, std::atomic<bool>* cancelFlag)
{
    if (cancelFlag && cancelFlag->load()) return;

    std::vector<uint8_t> buf(m_blockSize);
    if (!ReadDirectoryLogicalBlock(logicalBlocks, logicalBlock, buf.data()))
        return;

    if (nodeDepth == 0) {
        ParseDirBlock(buf.data(), m_blockSize, parentInode, path, cancelFlag);
        return;
    }

    if (m_blockSize < 12 + sizeof(EXT4_DX_ENTRY))
        return;

    const auto* meta = reinterpret_cast<const EXT4_DX_COUNT_LIMIT*>(buf.data() + 8);
    const uint16_t count = meta->count;
    const auto* entries = reinterpret_cast<const EXT4_DX_ENTRY*>(buf.data() + 12);

    const uint32_t maxEntries = (m_blockSize - 12) / sizeof(EXT4_DX_ENTRY);
    const uint16_t safeCount = (std::min)(count, static_cast<uint16_t>(maxEntries + 1));

    for (uint16_t i = 0; i + 1 < safeCount; ++i) {
        if (cancelFlag && cancelFlag->load()) return;
        ScanDxBlock(dirInodeNum, path, logicalBlocks, entries[i].block,
                    static_cast<uint8_t>(nodeDepth - 1), parentInode, cancelFlag);
    }
}

void Ext4Parser::ScanIndexedDirectory(uint32_t dirInodeNum, const std::wstring& path,
                                       const EXT4_INODE& dirInode,
                                       const std::vector<uint64_t>& logicalBlocks,
                                       std::atomic<bool>* cancelFlag)
{
    if (logicalBlocks.empty())
        return;

    std::vector<uint8_t> root(m_blockSize);
    if (!ReadDirectoryLogicalBlock(logicalBlocks, 0, root.data()))
        return;

    const auto* info = reinterpret_cast<const EXT4_DX_ROOT_INFO*>(root.data() + EXT4_DX_ROOT_INFO_OFF);
    const uint8_t indirect = info->indirect_levels;
    const uint16_t count = *reinterpret_cast<const uint16_t*>(root.data() + EXT4_DX_ROOT_COUNT_OFF);
    const uint32_t firstLeaf = *reinterpret_cast<const uint32_t*>(root.data() + EXT4_DX_ROOT_BLOCK_OFF);
    const auto* entries = reinterpret_cast<const EXT4_DX_ENTRY*>(root.data() + EXT4_DX_ROOT_ENTRIES_OFF);

    const uint32_t maxEntries = (m_blockSize - EXT4_DX_ROOT_ENTRIES_OFF) / sizeof(EXT4_DX_ENTRY);
    const uint16_t safeCount = (std::min)(count, static_cast<uint16_t>(maxEntries + 1));

    ScanDxBlock(dirInodeNum, path, logicalBlocks, firstLeaf, indirect, dirInodeNum, cancelFlag);
    for (uint16_t i = 0; i + 1 < safeCount; ++i) {
        if (cancelFlag && cancelFlag->load()) return;
        ScanDxBlock(dirInodeNum, path, logicalBlocks, entries[i].block,
                    indirect, dirInodeNum, cancelFlag);
    }

    (void)dirInode;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanDirectory
// ─────────────────────────────────────────────────────────────────────────────
void Ext4Parser::ScanDirectory(uint32_t inodeNum, const std::wstring& path,
                                std::atomic<bool>* cancelFlag)
{
    if (cancelFlag && cancelFlag->load()) return;
    if (m_visitedInodes.count(inodeNum)) return;
    m_visitedInodes.insert(inodeNum);

    EXT4_INODE inode;
    if (!ReadInode(inodeNum, inode)) return;

    if ((inode.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return;

    ++m_totalInodes;
    if (inode.i_links_count > 0) ++m_inUseCount;
    m_seenInodes.insert(inodeNum);

    const auto logicalBlocks = GetDirectoryLogicalBlocks(inode);
    if (logicalBlocks.empty())
        return;

    bool indexed = (inode.i_flags & EXT4_INDEX_FL) != 0;
    if (!indexed) {
        std::vector<uint8_t> first(m_blockSize);
        if (ReadDirectoryLogicalBlock(logicalBlocks, 0, first.data()))
            indexed = IsDxRootBlock(first.data(), m_blockSize);
    }

    if (indexed) {
        ScanIndexedDirectory(inodeNum, path, inode, logicalBlocks, cancelFlag);
        return;
    }

    std::vector<uint8_t> buf(m_blockSize);
    for (uint32_t i = 0; i < logicalBlocks.size(); ++i) {
        if (cancelFlag && cancelFlag->load()) return;
        if (!ReadDirectoryLogicalBlock(logicalBlocks, i, buf.data())) continue;
        ParseDirBlock(buf.data(), m_blockSize, inodeNum, path, cancelFlag);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ParseDirBlock
// ─────────────────────────────────────────────────────────────────────────────
void Ext4Parser::EmitDirEntry(uint32_t entryInode, uint8_t fileType, bool dentryDeleted,
                               const std::wstring& name, uint32_t parentInode,
                               const std::wstring& path, std::atomic<bool>* cancelFlag)
{
    if (name.empty()) return;

    bool isDir = (fileType == EXT4_FT_DIR);
    EXT4_INODE childInode{};
    bool gotInode = false;

    if (entryInode != 0) {
        gotInode = ReadInode(entryInode, childInode);
        if (gotInode) {
            m_seenInodes.insert(entryInode);
            if (fileType == EXT4_FT_UNKNOWN)
                isDir = ((childInode.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR);
        }
    }

    ++m_totalInodes;

    FileNode node;
    node.id           = m_nextId++;
    node.fsType       = L"ext4";
    node.name         = name;
    node.originalPath = path + name;
    node.isDirectory  = isDir;
    node.parentId     = parentInode;

    if (gotInode) {
        node.size = childInode.i_size_lo | ((uint64_t)childInode.i_size_hi << 32);
        node.createTime = UnixToFiletime(childInode.i_crtime, childInode.i_crtime_extra);
        node.modifyTime = UnixToFiletime(childInode.i_mtime, childInode.i_mtime_extra);
        node.accessTime = UnixToFiletime(childInode.i_atime, childInode.i_atime_extra);

        if (dentryDeleted || InodeLooksDeleted(childInode)) {
            node.status = FileStatus::Deleted;
        } else {
            node.status = FileStatus::Normal;
            ++m_inUseCount;
        }

        if (!isDir && node.size > 0) {
            std::vector<std::pair<uint64_t, uint32_t>> blocks;
            if ((childInode.i_flags & EXT4_EXTENTS_FL) && m_hasExtents)
                blocks = GetExtentBlocks(childInode);
            else
                blocks = GetIndirectBlocks(childInode);

            for (const auto& bc : blocks) {
                for (uint32_t j = 0; j < bc.second; ++j)
                    node.clusters.push_back(bc.first + j);
            }
        }
    } else if (dentryDeleted) {
        node.status = FileStatus::Deleted;
    } else {
        node.status = FileStatus::Normal;
    }

    {
        size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos && dot < name.size() - 1) {
            node.extension = name.substr(dot + 1);
            for (auto& c : node.extension)
                c = static_cast<wchar_t>(towlower(c));
        }
    }

    if (node.status == FileStatus::Deleted) {
        node.recoverability = (entryInode != 0)
            ? AssessRecoverability(entryInode)
            : RecoverabilityLevel::Good;
    } else {
        node.recoverability = RecoverabilityLevel::Excellent;
    }

    if (isDir) node.originalPath += L'\\';

    m_files.push_back(node);
    if (node.status == FileStatus::Deleted && m_onFileFound)
        m_onFileFound(node);

    if (isDir && !dentryDeleted && entryInode != 0 && entryInode != parentInode)
        ScanDirectory(entryInode, node.originalPath, cancelFlag);
}

void Ext4Parser::ParseDirBlock(const uint8_t* data, uint32_t size,
                                uint32_t parentInode, const std::wstring& path,
                                std::atomic<bool>* cancelFlag)
{
    uint32_t offset = 0;
    while (offset + sizeof(EXT4_DIR_ENTRY_2) <= size) {
        if (cancelFlag && cancelFlag->load()) return;

        const auto* entry = reinterpret_cast<const EXT4_DIR_ENTRY_2*>(data + offset);
        if (entry->rec_len == 0 || entry->rec_len > size - offset) break;
        if (offset + entry->rec_len > size) break;

        const uint32_t recLen         = entry->rec_len;
        const uint8_t  nameLen        = entry->name_len;
        const bool     dentryDeleted  = (entry->inode == 0);

        if (nameLen != 0) {
            const char* namePtr = reinterpret_cast<const char*>(
                data + offset + sizeof(EXT4_DIR_ENTRY_2));
            const std::string nameUtf8(namePtr, nameLen);
            if (nameUtf8 != "." && nameUtf8 != "..") {
                std::wstring name = Utils::Utf8ToWide(nameUtf8);
                if (name.empty() && !nameUtf8.empty())
                    name = Utils::AnsiToWide(nameUtf8.c_str());

                const uint8_t fileType = m_hasFiletype
                    ? entry->file_type : EXT4_FT_UNKNOWN;
                EmitDirEntry(entry->inode, fileType, dentryDeleted, name,
                             parentInode, path, cancelFlag);
            }
        }

        // W6: the standard rec_len walk above jumps over slack that still
        // contains deleted dirents. Recover their ORIGINAL names from there.
        ScanDirentSlack(data, size, offset, recLen, nameLen,
                        parentInode, path, cancelFlag);

        offset += recLen;
    }
}

void Ext4Parser::ScanDirentSlack(const uint8_t* data, uint32_t size,
                                 uint32_t entryOffset, uint32_t entryRecLen,
                                 uint32_t entryNameLen,
                                 uint32_t parentInode, const std::wstring& path,
                                 std::atomic<bool>* cancelFlag)
{
    const uint32_t used = static_cast<uint32_t>(
        (sizeof(EXT4_DIR_ENTRY_2) + entryNameLen + 3u) & ~3u);
    if (used >= entryRecLen) return;

    const uint32_t slackBegin = entryOffset + used;
    const uint32_t slackEnd   = entryOffset + entryRecLen;
    if (slackEnd > size || slackBegin >= slackEnd) return;

    uint32_t p = slackBegin;
    while (p + sizeof(EXT4_DIR_ENTRY_2) <= slackEnd) {
        if (cancelFlag && cancelFlag->load()) return;

        const auto* cand = reinterpret_cast<const EXT4_DIR_ENTRY_2*>(data + p);
        const uint32_t candIno = cand->inode;
        const uint32_t candRec = cand->rec_len;
        const uint32_t candLen = cand->name_len;
        const uint32_t minRec  = static_cast<uint32_t>(
            (sizeof(EXT4_DIR_ENTRY_2) + candLen + 3u) & ~3u);

        const bool plausible =
            candIno != 0 && candIno <= m_sb.s_inodes_count &&
            candLen >= 1 && candLen <= EXT4_NAME_LEN &&
            candRec >= minRec && (candRec & 3u) == 0 &&
            static_cast<uint64_t>(p) + candRec <= slackEnd &&
            m_seenInodes.count(candIno) == 0;

        if (plausible) {
            const uint8_t* namePtr = data + p + sizeof(EXT4_DIR_ENTRY_2);
            bool printable = true;
            for (uint32_t i = 0; i < candLen; ++i) {
                if (namePtr[i] < 0x20 || namePtr[i] > 0x7e) {
                    printable = false;
                    break;
                }
            }
            if (printable) {
                // Only emit when the backing inode really is deleted. This is
                // what keeps arbitrary slack bytes from becoming false files.
                EXT4_INODE ino{};
                if (ReadInode(candIno, ino) && InodeLooksDeleted(ino)) {
                    const std::string nameUtf8(
                        reinterpret_cast<const char*>(namePtr), candLen);
                    std::wstring name = Utils::Utf8ToWide(nameUtf8);
                    if (name.empty())
                        name = Utils::AnsiToWide(nameUtf8.c_str());
                    if (!name.empty() && name != L"." && name != L"..") {
                        // FT_UNKNOWN makes EmitDirEntry derive directory-ness
                        // from the inode mode rather than stale type bytes.
                        EmitDirEntry(candIno, EXT4_FT_UNKNOWN, true, name,
                                     parentInode, path, cancelFlag);
                        p += (std::max)(minRec, 4u);
                        continue;
                    }
                }
            }
        }
        p += 4;
    }
}

void Ext4Parser::ScanOrphanInodes(std::atomic<bool>* cancelFlag)
{
    const uint32_t firstInode = (m_sb.s_rev_level >= 1) ? m_sb.s_first_ino : 11;

    for (uint32_t ino = firstInode; ino <= m_sb.s_inodes_count; ++ino) {
        if (cancelFlag && cancelFlag->load()) return;
        if (m_seenInodes.count(ino)) continue;

        EXT4_INODE inode{};
        if (!ReadInode(ino, inode)) continue;
        if (!InodeLooksDeleted(inode)) continue;

        const uint16_t mode = inode.i_mode & EXT4_S_IFMT;
        if (mode != EXT4_S_IFREG && mode != EXT4_S_IFDIR) continue;

        m_seenInodes.insert(ino);
        ++m_totalInodes;

        FileNode node;
        node.id           = m_nextId++;
        node.fsType       = L"ext4";
        node.name         = L"inode_" + std::to_wstring(ino);
        node.originalPath = L"\\lost+found\\" + node.name;
        node.isDirectory  = (mode == EXT4_S_IFDIR);
        node.parentId     = 0;
        node.status       = FileStatus::Deleted;
        node.size         = inode.i_size_lo | ((uint64_t)inode.i_size_hi << 32);
        node.createTime   = UnixToFiletime(inode.i_crtime, inode.i_crtime_extra);
        node.modifyTime   = UnixToFiletime(inode.i_mtime, inode.i_mtime_extra);
        node.accessTime   = UnixToFiletime(inode.i_atime, inode.i_atime_extra);
        node.recoverability = AssessRecoverability(ino);

        if (!node.isDirectory && node.size > 0) {
            std::vector<std::pair<uint64_t, uint32_t>> blocks;
            if ((inode.i_flags & EXT4_EXTENTS_FL) && m_hasExtents)
                blocks = GetExtentBlocks(inode);
            else
                blocks = GetIndirectBlocks(inode);
            for (const auto& bc : blocks) {
                for (uint32_t j = 0; j < bc.second; ++j)
                    node.clusters.push_back(bc.first + j);
            }
        }

        if (node.isDirectory) node.originalPath += L'\\';

        m_files.push_back(node);
        if (m_onFileFound) m_onFileFound(node);
    }
}
