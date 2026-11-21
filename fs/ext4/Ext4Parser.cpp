#include "Ext4Parser.h"
#include "../../common/Utils.h"
#include "../../log/Logger.h"
#include "../refs/RefsCrypto.h"   // phoenix::refs::Crc32C{Update,Finish} — shared CRC32C implementation
#include <algorithm>
#include <cstring>

// JBD2 uses the same CRC32C polynomial (Castagnoli, reflected 0x82F63B78) as
// ReFS, BUT the kernel's jbd2_chksum() == crc32c_le() returns the RAW
// (non-xorout) chained state, which is fed straight into the next call.
// j_csum_seed = jbd2_chksum(~0, journal_uuid, 16) is such a raw state, and
// every jbd2_*_csum_verify() compares against a raw state too.  So we must use
// Crc32CUpdate ONLY here — applying Crc32CFinish (xorout) would desync every
// checksum.  (ReFS deliberately uses the xorout form; JBD2 does not.)
namespace {
inline uint32_t Jbd2Crc(uint32_t seed, const uint8_t* p, size_t n)
{
    return phoenix::refs::Crc32CUpdate(seed, p, n);   // no xorout
}
}

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
// SetExternalJournalImage – supply the separate journal device explicitly
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::SetExternalJournalImage(const std::wstring& imagePath)
{
    auto reader = std::make_unique<SectorReader>();
    if (!reader->OpenImage(imagePath) || !reader->IsOpen()) return false;
    m_externalJournal      = std::move(reader);
    m_externalJournalReady = true;
    return true;
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
    m_overlay.clear();
    m_revoke.clear();
    m_journalCommits = 0;
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
    m_hasInlineData = (m_sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA) != 0;

    // bigalloc: s_log_cluster_size describes the allocation cluster.  When it
    // is larger than s_log_block_size every extent's ee_start / ee_len counts
    // CLUSTERS, not blocks.  Without this scaling a whole bigalloc volume is
    // read at the wrong offset (and dir trees come out empty).
    m_clusterRatio = 1;
    if (m_sb.s_log_cluster_size > m_sb.s_log_block_size) {
        const uint32_t shift = m_sb.s_log_cluster_size - m_sb.s_log_block_size;
        if (shift <= 8)
            m_clusterRatio = 1u << shift;
    }

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
    if (!ReloadGroupDescriptors()) {
        LOG_ERROR(L"Ext4Parser: cannot read group descriptors");
        return false;
    }

    // ── JBD2 replay ───────────────────────────────────────────────────────
    // Metadata that the kernel committed to the journal but never wrote back
    // to its home location ("crash before checkpoint").  Replaying it is what
    // makes a file that looks deleted on disk come back with its name and
    // extent tree.  The overlay is memory-only; the scan source is untouched.
    if ((m_sb.s_journal_inum != 0 || m_sb.s_journal_dev != 0) &&
        ReplayJournal() && !m_overlay.empty())
        ReloadGroupDescriptors();   // the GDT blocks may themselves be logged

    m_valid = true;
    LOG_INFO(L"Ext4Parser::Open ok"
             L"  blockSize="   + std::to_wstring(m_blockSize) +
             L"  inodeSize="   + std::to_wstring(m_inodeSize) +
             L"  groups="      + std::to_wstring(m_groupCount) +
             L"  extents="     + (m_hasExtents ? L"yes" : L"no") +
             L"  64bit="       + (m_is64Bit ? L"yes" : L"no") +
             L"  jnlCommits="  + std::to_wstring(m_journalCommits) +
             L"  jnlBlocks="   + std::to_wstring(m_overlay.size()));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReloadGroupDescriptors – (re)read the group descriptor table
// Called once during Open() and again after journal replay, because a logged
// group descriptor can move the inode table or change its free counts.
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ReloadGroupDescriptors()
{
    const uint64_t gdtBlock = (m_blockSize == 1024) ? 2 : 1;
    const uint64_t gdtBytes = static_cast<uint64_t>(m_groupCount) * m_descSize;
    const uint32_t gdtBlocks = static_cast<uint32_t>(
        (gdtBytes + m_blockSize - 1) / m_blockSize);

    std::vector<uint8_t> gdtBuf(static_cast<size_t>(gdtBlocks) * m_blockSize);
    if (!ReadBlocks(gdtBlock, gdtBlocks, gdtBuf.data()))
        return false;

    m_groupDescs.assign(m_groupCount, EXT4_GROUP_DESC{});
    for (uint32_t g = 0; g < m_groupCount; ++g) {
        const uint8_t* p = gdtBuf.data() + g * m_descSize;
        memcpy(&m_groupDescs[g], p, std::min((size_t)m_descSize, sizeof(EXT4_GROUP_DESC)));
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadFsBytes – the single choke point for metadata reads.
//
// ReadBlock/ReadBlocks/ReadInode/ReadInodeRaw all come through here, so the
// journal overlay is honoured everywhere (ReadInode addresses the inode table
// arithmetically and never called ReadBlock).  A range may straddle blocks, so
// the copy walks block by block and only falls back to the device for blocks
// the journal did not override.
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ReadFsBytes(uint64_t byteOffset, void* buffer, uint32_t len)
{
    if (m_overlay.empty())
        return m_reader->ReadBytes(m_partStartByte + byteOffset, buffer, len);

    uint8_t* dst   = static_cast<uint8_t*>(buffer);
    uint64_t pos   = byteOffset;
    uint32_t left  = len;

    while (left > 0) {
        const uint64_t blk   = pos / m_blockSize;
        const uint32_t inBlk = static_cast<uint32_t>(pos % m_blockSize);
        const uint32_t chunk = std::min<uint32_t>(left, m_blockSize - inBlk);

        auto it = m_overlay.find(blk);
        if (it != m_overlay.end()) {
            memcpy(dst, it->second.data() + inBlk, chunk);
        } else if (!m_reader->ReadBytes(m_partStartByte + pos, dst, chunk)) {
            return false;
        }
        dst  += chunk;
        pos  += chunk;
        left -= chunk;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadBlock / ReadBlocks
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ReadBlock(uint64_t blockNum, void* buffer)
{
    return ReadFsBytes(blockNum * m_blockSize, buffer, m_blockSize);
}

bool Ext4Parser::ReadBlocks(uint64_t startBlock, uint32_t count, void* buffer)
{
    return ReadFsBytes(startBlock * m_blockSize, buffer, count * m_blockSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// JournalBlockMap – journal logical block index -> absolute filesystem block
//
// The journal is an ordinary regular file (s_journal_inum, usually inode 8) so
// its own mapping is read like any other file.  Holes are kept as 0 and are
// rejected by ReadJournalAt, never silently mapped to block 0.
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::JournalBlockMap(const EXT4_INODE& jinode, std::vector<uint64_t>& map)
{
    map.clear();

    std::vector<std::pair<uint64_t, uint32_t>> runs;
    if (jinode.i_flags & EXT4_EXTENTS_FL) runs = GetExtentBlocks(jinode);
    else                                  runs = GetIndirectBlocks(jinode);
    if (runs.empty()) return false;

    const uint64_t jsize = static_cast<uint64_t>(jinode.i_size_lo)
                         | (static_cast<uint64_t>(jinode.i_size_hi) << 32);
    const uint64_t jblocks = (jsize + m_blockSize - 1) / m_blockSize;
    if (jblocks < 2) return false;
    // 4M blocks = 4 GiB of log at 1 KiB blocks; anything larger is corruption.
    if (jblocks > (1ull << 22)) return false;

    map.reserve(static_cast<size_t>(jblocks));
    for (const auto& r : runs) {
        for (uint32_t i = 0; i < r.second && map.size() < jblocks; ++i)
            map.push_back(r.first ? (r.first + i) : 0);
        if (map.size() >= jblocks) break;
    }
    return !map.empty() && map[0] != 0;
}

bool Ext4Parser::ReadJournalAt(const std::vector<uint64_t>& map, uint32_t index, void* out)
{
    if (index >= map.size()) return false;
    const uint64_t blk = map[index];
    if (blk == 0) return false;
    // Replay runs while m_overlay is still empty, so this is the raw device.
    return ReadBlock(blk, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReplayJournal – build the read-only overlay from the JBD2 log
//
// Two phases, mirroring jbd2_journal_recover():
//   1. Walk the whole log once, collecting committed transactions (descriptor
//      tag + data-block position) and every revoke record.  A revoke may be
//      logged AFTER the descriptor it cancels, so replaying as we walk would
//      get the order wrong — hence the separate collection phase.
//   2. Apply the collected transactions in log order, skipping any copy that a
//      revoke suppresses (t_seq <= revoke_seq), keeping the last copy of a
//      block that was logged twice.
//
// Only committed transactions reach the overlay: a torn tail (bad magic, a
// missing commit block, an out-of-sequence header) simply ends the walk, which
// is exactly what the kernel does after a crash.
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ReplayJournal()
{
    m_overlay.clear();
    m_revoke.clear();
    m_journalCommits = 0;

    // An external journal (s_journal_dev != 0) is a separate device.  It can
    // only be replayed when the caller supplied that device explicitly; if
    // both dev and inum are set, the external device wins (that is the
    // on-disk meaning of a non-zero s_journal_dev).
    const bool useExternal = (m_sb.s_journal_dev != 0 &&
                              m_externalJournalReady && m_externalJournal != nullptr);
    if (!useExternal && m_sb.s_journal_inum == 0) return false;

    std::vector<uint64_t> jmap;
    if (!useExternal) {
        EXT4_INODE jinode{};
        if (!ReadInode(m_sb.s_journal_inum, jinode)) return false;
        if ((jinode.i_mode & EXT4_S_IFMT) != EXT4_S_IFREG) return false;
        if (!JournalBlockMap(jinode, jmap)) return false;
    }
    const uint64_t jcapacity = useExternal
        ? (m_externalJournal->GetTotalBytes() / m_blockSize)
        : static_cast<uint64_t>(jmap.size());
    auto readJb = [&](uint32_t index, void* out) -> bool {
        if (useExternal)
            return m_externalJournal->ReadBytes(
                static_cast<uint64_t>(index) * m_blockSize, out, m_blockSize);
        return ReadJournalAt(jmap, index, out);
    };

    std::vector<uint8_t> buf(m_blockSize);
    if (!readJb(0, buf.data())) return false;

    // Never replay a log from a different volume: the UUID is the only stable
    // identity linking an external journal device to this filesystem.
    if (useExternal &&
        std::memcmp(buf.data() + JBD2_SB_UUID_OFF, m_sb.s_journal_uuid, 16) != 0) {
        LOG_INFO(L"Ext4Parser: external journal UUID mismatch; refusing replay");
        return false;
    }

    if (Be32(buf.data()) != JBD2_MAGIC) return false;
    // JBD (v1, very old ext3) and JBD2 (v2) share the superblock prefix but
    // NOT the descriptor tag layout:
    //   v1: 8-byte tag (be32 blocknr, be32 flags), followed by a 16-byte UUID
    //       unless JBD_FLAG_SAME_UUID is set;
    //   v2: 8-byte tag (be32 blocknr, be16 checksum, be16 flags), no inline
    //       UUID (the UUID lives once in the journal superblock).
    // Parsing v1 with the v2 rule desyncs the tag array and overlays the wrong
    // blocks, so the two variants are dispatched explicitly below.  Anything
    // else is still refused: no replay is the pre-existing behaviour, whereas
    // a wrong overlay is silent corruption.
    const uint32_t sbType = Be32(buf.data() + 4);
    const bool isV1 = (sbType == JBD2_BLOCKTYPE_SB_V1);
    if (!isV1 && sbType != JBD2_BLOCKTYPE_SB_V2) return false;

    const uint32_t jbs      = Be32(buf.data() + 0x0C);
    const uint32_t maxlen   = Be32(buf.data() + 0x10);
    const uint32_t first    = Be32(buf.data() + 0x14);
    uint32_t       seq      = Be32(buf.data() + 0x18);
    const uint32_t start    = Be32(buf.data() + 0x1C);
    // The feature flags do not exist in a v1 superblock (that area is zero);
    // treating them as 0 is both accurate and keeps the v2 path unchanged.
    const uint32_t incompat = isV1 ? 0u : Be32(buf.data() + 0x28);

    if (jbs != m_blockSize) return false;
    // s_start == 0 means the log was checkpointed and the journal is empty:
    // there is nothing newer than the home locations, so replay is a no-op.
    if (start == 0) return false;
    if (maxlen < 2 || maxlen > jcapacity) return false;
    if (first == 0 || first >= maxlen) return false;
    if (start < first || start >= maxlen) return false;

    const bool has64  = (incompat & JBD2_FEATURE_INCOMPAT_64BIT) != 0;
    const bool hasCsum2 = (incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) != 0;
    const bool hasCsum3 = (incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3) != 0;
    const bool hasCsum  = hasCsum2 || hasCsum3;

    // Tag layout: legacy/v2-without-64bit = 8 B (be32 blocknr, be16 csum,
    // be16 flags); v2-with-64bit = 12 B (adds be32 high); v3 = 16 B
    // (be32 blocknr, be32 flags, be32 high, be32 csum) regardless of 64BIT.
    // We refuse the 64-bit variants: this formatter writes block numbers
    // below 2^32, and silently mis-parsing a foreign 64-bit tag would build a
    // wrong overlay.  Refusing = no replay, the safe direction.
    uint32_t tagSize;
    if (isV1)          tagSize = 8u;    // + 16-byte UUID unless SAME_UUID
    else if (hasCsum3) tagSize = 16u;
    else if (has64)    tagSize = 12u;   // v2 + 64bit (handled conservatively below)
    else               tagSize = 8u;
    const uint32_t revSize = has64 ? 8u : 4u;
    if (has64) return false;    // out of scope: avoid wrong overlay over silent acceptance

    // j_csum_seed = jbd2_chksum(~0, s_uuid, 16).  The checksum field itself
    // is a plain CRC32C final value; the UUID lives at journal SB +0x30.
    uint32_t csumSeed = 0;
    if (hasCsum) {
        csumSeed = Jbd2Crc(0xFFFFFFFFu, buf.data() + JBD2_SB_UUID_OFF, 16);
        // The superblock must carry the CRC32C checksum type; anything else
        // is a format we don't implement → refuse rather than mis-verify.
        if (buf.data()[0x50] != JBD2_CRC32C_CHKSUM) {
            LOG_INFO(L"Ext4Parser JBD2 sb csum type FAIL val=" + std::to_wstring(buf.data()[0x50]));
            return false;
        }
        // Verify the superblock's own checksum (s_checksum at 0xFC, computed
        // over the block with that field zeroed).  A corrupt journal SB must
        // never drive replay.
        uint32_t provided = Be32(buf.data() + JBD2_SB_CHECKSUM_OFF);
        std::vector<uint8_t> sbCopy(buf.begin(), buf.end());
        std::memset(sbCopy.data() + JBD2_SB_CHECKSUM_OFF, 0, 4);
        uint32_t sbCalc = Jbd2Crc(csumSeed, sbCopy.data(), m_blockSize);
        if (provided != sbCalc) {
            LOG_INFO(L"Ext4Parser JBD2 sb csum FAIL provided=" + std::to_wstring(provided)
                     + L" calc=" + std::to_wstring(sbCalc)
                     + L" seed=" + std::to_wstring(csumSeed)
                     + L" type=" + std::to_wstring(buf.data()[0x50]));
            return false;
        }
    }

    // Verify a descriptor/revoke block tail: be32 at (blocksize - 4), computed
    // over the whole block with that trailing field zeroed.
    auto tailOk = [&](const uint8_t* p) -> bool {
        if (!hasCsum) return true;
        const uint32_t provided = Be32(p + m_blockSize - JBD2_JOURNAL_BLOCK_TAIL_SIZE);
        std::vector<uint8_t> copy(p, p + m_blockSize);
        std::memset(copy.data() + m_blockSize - JBD2_JOURNAL_BLOCK_TAIL_SIZE, 0, 4);
        uint32_t calc = Jbd2Crc(csumSeed, copy.data(), m_blockSize);
        if (provided != calc)
            LOG_INFO(L"Ext4Parser JBD2 tail csum FAIL provided=" + std::to_wstring(provided)
                     + L" calc=" + std::to_wstring(calc));
        return provided == calc;
    };
    // Verify a data block's tag checksum: csum = crc32c(seed, be32(seq)) then
    // crc32c(csum, data_block).  v3 stores be32, v2 stores low be16.
    auto tagOk = [&](const uint8_t* tagp, uint32_t seq, const uint8_t* data,
                     uint16_t storeCsumV2) -> bool {
        if (!hasCsum) return true;
        uint8_t seqBe[4] = { static_cast<uint8_t>(seq >> 24),
                             static_cast<uint8_t>(seq >> 16),
                             static_cast<uint8_t>(seq >> 8),
                             static_cast<uint8_t>(seq) };
        uint32_t state = phoenix::refs::Crc32CUpdate(csumSeed, seqBe, 4);
        uint32_t c = Jbd2Crc(state, data, m_blockSize);
        if (hasCsum3)
            return Be32(tagp + 12) == c;
        return storeCsumV2 == static_cast<uint16_t>(c & 0xFFFFu);
    };

    struct Item { uint64_t block; uint32_t jidx; bool escape; };
    struct Txn  { uint32_t seq; std::vector<Item> items; };

    std::vector<Txn> transaction;
    std::vector<Item> pending;
    uint32_t cur = seq;
    uint32_t pos = start;
    const uint32_t stepCap = maxlen * 2 + 8;   // guards against log wraparound

    auto advance = [&](uint32_t i) -> uint32_t {
        return (i + 1 >= maxlen) ? first : i + 1;
    };

    for (uint32_t step = 0; step < stepCap; ++step) {
        if (!readJb(pos, buf.data())) break;
        if (Be32(buf.data()) != JBD2_MAGIC) break;          // torn tail

        const uint32_t type = Be32(buf.data() + 4);

        if (type == JBD2_BLOCKTYPE_DESCRIPTOR) {
            if (Be32(buf.data() + 8) != cur) break;         // out of sequence
            // A checksummed descriptor carries a 4-byte trailer; a corrupt
            // trailer means the tag array cannot be trusted → end the walk.
            if (!tailOk(buf.data())) break;

            // Collect the tag array first: the data blocks follow the whole
            // descriptor block, in tag order.
            struct Tag { uint64_t bn; uint16_t flags; uint16_t csumV2; size_t off; };
            std::vector<Tag> tags;
            const size_t tagLimit = m_blockSize - (hasCsum ? JBD2_JOURNAL_BLOCK_TAIL_SIZE : 0);
            size_t off = 12;
            while (off + tagSize <= tagLimit) {
                uint64_t bn = Be32(buf.data() + off);
                uint16_t flags, csumV2 = 0;
                if (isV1) {
                    // v1: t_flags is be32; a 16-byte UUID follows every tag
                    // unless the writer set SAME_UUID (subsequent tags reuse
                    // the UUID written by the first tag of this descriptor).
                    flags = static_cast<uint16_t>(Be32(buf.data() + off + 4));
                    if (!(flags & JBD2_FLAG_SAME_UUID)) {
                        if (off + 8 + 16 > tagLimit) break;
                    }
                } else if (hasCsum3) {
                    flags = static_cast<uint16_t>(Be32(buf.data() + off + 4));
                } else {
                    csumV2 = Be16(buf.data() + off + 4);        // t_checksum (v2, low16)
                    flags  = Be16(buf.data() + off + 6);        // t_flags
                }
                tags.emplace_back(Tag{bn, flags, csumV2, off});
                off += tagSize;
                if (isV1 && !(flags & JBD2_FLAG_SAME_UUID)) off += 16;
                if (flags & JBD2_FLAG_LAST_TAG) break;
            }
            if (tags.empty()) break;

            for (const auto& t : tags) {
                uint32_t dataIdx = pos;
                if (!(t.flags & JBD2_FLAG_DELETED)) {
                    dataIdx = advance(pos);     // consumed even if later revoked
                    pos     = dataIdx;
                }
                if (t.bn == 0) continue;
                // Verify the data block's per-tag checksum against the bytes
                // we just located.  A bad tag csum skips only this block
                // (jbd2_do_replay's `goto skip_write`), never corrupts others.
                if (hasCsum && !(t.flags & JBD2_FLAG_DELETED)) {
                    if (dataIdx >= jmap.size() || jmap[dataIdx] == 0) continue;
                    std::vector<uint8_t> dblock(m_blockSize);
                    if (!readJb(dataIdx, dblock.data())) continue;
                    if (!tagOk(buf.data() + t.off, cur,
                               dblock.data(), t.csumV2)) {
                        LOG_INFO(L"Ext4Parser JBD2 tag checksum mismatch for block "
                                 + std::to_wstring(t.bn) + L"; skipping data block");
                        continue;   // this data block is corrupt → skip it
                    }
                }
                pending.push_back(Item{t.bn, dataIdx,
                                       (t.flags & JBD2_FLAG_ESCAPE) != 0});
            }
        } else if (type == JBD2_BLOCKTYPE_COMMIT) {
            if (Be32(buf.data() + 8) != cur) break;
            // A checksummed commit block's h_chksum[0] must verify; a torn or
            // corrupted commit is exactly the crash window we must NOT replay.
            if (hasCsum) {
                if (buf.data()[JBD2_COMMIT_CSUM_TYPE_OFF] != JBD2_CRC32C_CHKSUM) {
                    LOG_INFO(L"Ext4Parser JBD2 commit type FAIL val=" + std::to_wstring(buf.data()[JBD2_COMMIT_CSUM_TYPE_OFF]));
                    break;
                }
                const uint32_t provided = Be32(buf.data() + JBD2_COMMIT_CSUM_OFF);
                std::vector<uint8_t> cpy(buf.begin(), buf.end());
                std::memset(cpy.data() + JBD2_COMMIT_CSUM_OFF, 0, 4);
                uint32_t cc = Jbd2Crc(csumSeed, cpy.data(), m_blockSize);
                if (provided != cc) {
                    LOG_INFO(L"Ext4Parser JBD2 commit csum FAIL seq=" + std::to_wstring(cur)
                             + L" provided=" + std::to_wstring(provided)
                             + L" calc=" + std::to_wstring(cc));
                    break;
                }
            }
            if (!pending.empty()) {
                Txn t;
                t.seq = cur;
                t.items.swap(pending);
                transaction.push_back(std::move(t));
            }
            pending.clear();
            ++cur;
        } else if (type == JBD2_BLOCKTYPE_REVOKE) {
            const uint32_t rcount = Be32(buf.data() + 8);
            if (rcount < 12 || rcount > m_blockSize) break;
            if (!tailOk(buf.data())) break;
            for (uint32_t off = 12; off + revSize <= rcount; off += revSize) {
                uint64_t bn = Be32(buf.data() + off);
                auto it = m_revoke.find(bn);
                if (it == m_revoke.end() || it->second < cur)
                    m_revoke[bn] = cur;
            }
        } else {
            break;
        }
        pos = advance(pos);
    }

    // ── phase 2: apply ────────────────────────────────────────────────────
    uint64_t totalBlocks = m_sb.s_blocks_count_lo;
    if (m_is64Bit)
        totalBlocks |= static_cast<uint64_t>(m_sb.s_blocks_count_hi) << 32;

    // The superblock is deliberately never overlaid: its geometry is what the
    // rest of this parser was configured from, and swapping it mid-flight
    // would re-interpret the volume under the code that is reading it.
    const uint64_t sbBlock = (m_blockSize == 1024) ? 1 : 0;

    constexpr size_t kMaxOverlayBytes = 256u << 20;
    size_t bytes = 0;

    // Built into a local map and swapped in at the end: if a transaction ever
    // logs one of the journal's own blocks, reading it mid-apply must still
    // return the raw device content, not the half-built overlay.
    std::unordered_map<uint64_t, std::vector<uint8_t>> overlay;

    for (const Txn& t : transaction) {
        bool applied = false;
        for (const Item& it : t.items) {
            if (it.block == 0 || it.block >= totalBlocks) continue;
            if (it.block == sbBlock) continue;

            auto rv = m_revoke.find(it.block);
            if (rv != m_revoke.end() && t.seq <= rv->second) continue;

            if (bytes + m_blockSize > kMaxOverlayBytes) break;

            std::vector<uint8_t> data(m_blockSize);
            if (!readJb(it.jidx, data.data())) continue;
            if (it.escape) {   // writer zeroed a leading magic, restore it
                data[0] = static_cast<uint8_t>(JBD2_MAGIC >> 24);
                data[1] = static_cast<uint8_t>(JBD2_MAGIC >> 16);
                data[2] = static_cast<uint8_t>(JBD2_MAGIC >>  8);
                data[3] = static_cast<uint8_t>(JBD2_MAGIC);
            }
            overlay[it.block] = std::move(data);
            bytes += m_blockSize;
            applied = true;
        }
        if (applied) ++m_journalCommits;
    }

    m_overlay.swap(overlay);

    if (!m_overlay.empty()) {
        LOG_INFO(std::wstring(L"Ext4Parser: ") + (isV1 ? L"JBD" : L"JBD2") + L" replay applied "
                 + std::to_wstring(m_journalCommits) + L" transaction(s), "
                 + std::to_wstring(m_overlay.size()) + L" block(s) overlaid, "
                 + std::to_wstring(m_revoke.size()) + L" revoke record(s)");
    }
    return !m_overlay.empty();
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
    const uint64_t inodeOffset = inodeTableBlock * m_blockSize
                               + static_cast<uint64_t>(localIdx) * m_inodeSize;

    if (!ReadFsBytes(inodeOffset, &outInode,
                     std::min((uint32_t)sizeof(outInode), m_inodeSize))) {
        return false;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadInodeRaw – same addressing as ReadInode but keeps the whole inode so
// that the in-inode xattr area (ibody) survives for inline_data payloads.
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ReadInodeRaw(uint32_t inodeNum, std::vector<uint8_t>& raw)
{
    raw.clear();
    if (inodeNum == 0 || inodeNum > m_sb.s_inodes_count) return false;

    const uint32_t idx      = inodeNum - 1;
    const uint32_t group    = idx / m_sb.s_inodes_per_group;
    const uint32_t localIdx = idx % m_sb.s_inodes_per_group;
    if (group >= m_groupCount) return false;
    if (m_inodeSize < 128) return false;

    uint64_t inodeTableBlock = m_groupDescs[group].bg_inode_table_lo;
    if (m_is64Bit)
        inodeTableBlock |= (uint64_t)m_groupDescs[group].bg_inode_table_hi << 32;

    const uint64_t off = inodeTableBlock * m_blockSize
                       + static_cast<uint64_t>(localIdx) * m_inodeSize;
    raw.resize(m_inodeSize);
    return ReadFsBytes(off, raw.data(), m_inodeSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// ParseInodeXattrData – find the "system.data" entry in the in-inode xattr
// area and return its value.  This is where inline_data keeps everything that
// does not fit into the 60 bytes of i_block[].
//
// Layout (Documentation/filesystems/ext4/inodes.rst, "In-inode xattrs"):
//   entries start at 128 + i_extra_isize
//   each entry: u8 name_len, u8 name_index, u16 value_offs, u32 value_size,
//               u32 hash, then name[] (padded to 4 bytes)
//   value lives at (inode start + value_offs), value_size bytes
//   the area ends at inode_size - 4, which holds the magic 0xEA020000
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ParseInodeXattrData(const std::vector<uint8_t>& raw,
                                     const EXT4_INODE& inode, uint64_t wantBytes,
                                     std::vector<uint8_t>& out)
{
    out.clear();
    if (raw.size() < 132) return false;

    const uint16_t extraIsize = inode.i_extra_isize;
    if (extraIsize == 0 || extraIsize > 128) return false;

    const uint32_t start = 128u + extraIsize;
    if (start + 16 > raw.size()) return false;

    const uint32_t magicOff = static_cast<uint32_t>(raw.size()) - 4;
    const uint32_t magic = static_cast<uint32_t>(raw[magicOff])
                         | (static_cast<uint32_t>(raw[magicOff + 1]) << 8)
                         | (static_cast<uint32_t>(raw[magicOff + 2]) << 16)
                         | (static_cast<uint32_t>(raw[magicOff + 3]) << 24);
    if (magic != EXT4_XATTR_IBODY_MAGIC) return false;

    uint32_t pos = start;
    for (int guard = 0; guard < 32; ++guard) {
        if (pos + 16 > magicOff) return false;

        const uint8_t  nameLen   = raw[pos];
        const uint8_t  nameIndex = raw[pos + 1];
        const uint16_t valueOffs = static_cast<uint16_t>(raw[pos + 2])
                                 | (static_cast<uint16_t>(raw[pos + 3]) << 8);
        const uint32_t valueSize = static_cast<uint32_t>(raw[pos + 4])
                                 | (static_cast<uint32_t>(raw[pos + 5]) << 8)
                                 | (static_cast<uint32_t>(raw[pos + 6]) << 16)
                                 | (static_cast<uint32_t>(raw[pos + 7]) << 24);

        if (nameLen == 0) break;                  // end of entries
        if (pos + 16 + nameLen > magicOff) return false;

        const bool isSystem = (nameIndex == EXT4_XATTR_INDEX_SYSTEM);
        const bool isData   = (nameLen == 4 &&
                              raw[pos + 16] == 'd' && raw[pos + 17] == 'a' &&
                              raw[pos + 18] == 't' && raw[pos + 19] == 'a');
        if (isSystem && isData) {
            if (valueSize == 0) return false;
            if (static_cast<uint64_t>(valueOffs) + valueSize > raw.size()) return false;
            const uint64_t take = (std::min)(wantBytes,
                                             static_cast<uint64_t>(valueSize));
            out.assign(raw.begin() + valueOffs,
                       raw.begin() + valueOffs + static_cast<std::ptrdiff_t>(take));
            return !out.empty();
        }

        pos += static_cast<uint32_t>(16 + ((nameLen + 3u) & ~3u));
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadInlineData – assemble an inline_data file/directory body:
//   0..59   bytes from i_block[]
//   60..    bytes from the system.data ibody xattr
// Returns false when i_size claims more bytes than we can account for, so the
// caller can mark the node instead of emitting a silently truncated file.
// ─────────────────────────────────────────────────────────────────────────────
bool Ext4Parser::ReadInlineData(uint32_t inodeNum, const EXT4_INODE& inode,
                                uint64_t size, std::vector<uint8_t>& out)
{
    out.clear();
    if (size == 0) return true;
    if (size > (1u << 20)) return false;          // inline data is tiny by design

    const uint32_t head = static_cast<uint32_t>((std::min)(size, uint64_t(60)));
    out.assign(inode.i_block, inode.i_block + head);

    if (size <= 60) return true;

    std::vector<uint8_t> raw;
    if (!ReadInodeRaw(inodeNum, raw)) return false;

    std::vector<uint8_t> tail;
    if (!ParseInodeXattrData(raw, inode, size - 60, tail)) return false;
    out.insert(out.end(), tail.begin(), tail.end());
    return out.size() >= size;
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

    // Walk the tree in logical order, emitting (0, n) for every gap.  A sparse
    // file is nothing but gaps between extents: without these markers the
    // later extents would be written back-to-back and every byte after the
    // first hole would land at the wrong offset.
    uint64_t nextLogicalBlock = 0;
    ParseExtentNode(inode.i_block, hdr->eh_depth, result, nextLogicalBlock);
    return result;
}

void Ext4Parser::ParseExtentNode(const uint8_t* data, uint16_t depth,
                                  std::vector<std::pair<uint64_t, uint32_t>>& out,
                                  uint64_t& nextLogicalBlock)
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
            // ee_len > 32768 means the extent is UNINITIALIZED: the logical
            // range reads as zeros, whatever stale bytes the blocks still
            // hold must never be emitted as recovered content.  Emit zero
            // markers (cluster 0 == sparse/zero, honoured by RecoverEngine)
            // instead of the on-disk block numbers.
            const bool uninit = (len > 32768);
            if (uninit)
                len = static_cast<uint16_t>(len - 32768);
            if (len == 0)
                continue;

            // ee_block is always a logical BLOCK index; only ee_start/ee_len
            // switch to clusters under bigalloc.
            const uint64_t logical = ext->ee_block;
            uint32_t spanBlocks = len;
            if (m_clusterRatio > 1) {
                startBlock *= m_clusterRatio;
                spanBlocks = std::min<uint32_t>(
                    static_cast<uint32_t>(len) * m_clusterRatio, 0xFFFFu);
                len = static_cast<uint16_t>(spanBlocks);
            }

            if (logical > nextLogicalBlock) {
                const uint64_t gap = logical - nextLogicalBlock;
                out.emplace_back(0, static_cast<uint32_t>(
                    std::min<uint64_t>(gap, 0xFFFFFFFFull)));
            }

            if (uninit) {
                for (uint16_t j = 0; j < len; ++j)
                    out.emplace_back(0, 1);
            } else {
                out.emplace_back(startBlock, len);
            }
            nextLogicalBlock = (std::max)(nextLogicalBlock, logical + spanBlocks);
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
            ParseExtentNode(childBuf.data(), depth - 1, out, nextLogicalBlock);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// AppendBlockRun – expand a (start, length) run into per-block cluster numbers
//
// start == 0 is the HOLE marker produced by GetExtentBlocks/GetIndirectBlocks:
// it means "length logical blocks that read as zeros", so it must expand into
// `length` zero entries.  Treating it as block 0 would emit 0,1,2,3 and shift
// every following byte of a sparse file.  (Block 0 is never allocated to a
// file, so there is no ambiguity.)
// ─────────────────────────────────────────────────────────────────────────────
namespace {
void AppendBlockRun(std::vector<uint64_t>& dst, uint64_t start, uint32_t length)
{
    if (length == 0) return;
    if (start == 0) {
        dst.insert(dst.end(), length, 0);
        return;
    }
    for (uint32_t j = 0; j < length; ++j)
        dst.push_back(start + j);
}
} // namespace

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

    // Direct blocks: i_block[0..11].  A zero slot inside the file's logical
    // range is a HOLE, not "unused": it must still emit a zero marker, or the
    // blocks that follow it would be written back-to-back and every byte after
    // the hole would land at the wrong offset.  `remaining` is what separates
    // a hole from the trailing unused slots.
    for (int i = 0; i < 12 && remaining > 0; ++i) {
        result.emplace_back(blocks[i], 1);   // 0 == sparse/zero marker
        --remaining;
    }

    // Indirect trees.  A NULL root pointer with logical blocks still pending
    // means the whole subtree is a hole; emit zero markers for it so the
    // recovered file keeps its offsets.
    const uint64_t perBlock = m_blockSize / 4;   // entries per indirect block
    for (int lvl = 1; lvl <= 3 && remaining > 0; ++lvl) {
        const int slot = 11 + lvl;               // i_block[12], [13], [14]
        if (blocks[slot] != 0) {
            ReadIndirectLevel(blocks[slot], lvl, result, remaining);
            continue;
        }
        uint64_t capacity = 1;
        for (int k = 0; k < lvl; ++k) capacity *= perBlock;
        const uint64_t take = (std::min)(capacity,
                                         static_cast<uint64_t>(remaining));
        result.emplace_back(0, static_cast<uint32_t>(take));
        remaining -= static_cast<uint32_t>(take);
    }

    return result;
}

void Ext4Parser::ReadIndirectLevel(uint64_t blockNum, int level,
                                    std::vector<std::pair<uint64_t, uint32_t>>& out,
                                    uint32_t& remaining)
{
    if (remaining == 0 || blockNum == 0) return;

    std::vector<uint8_t> buf(m_blockSize);
    if (!ReadBlock(blockNum, buf.data())) return;

    const uint32_t entriesPerBlock = m_blockSize / 4;  // 32-bit block numbers
    const uint32_t* entries = reinterpret_cast<const uint32_t*>(buf.data());
    // How many logical blocks one slot at this level covers.
    uint64_t slotCapacity = 1;
    for (int k = 0; k < level - 1; ++k) slotCapacity *= entriesPerBlock;

    for (uint32_t i = 0; i < entriesPerBlock && remaining > 0; ++i) {
        const uint32_t entry = entries[i];
        if (entry == 0) {
            // Zero inside the file's logical range is a HOLE.  Consume the
            // whole subtree worth of zero markers, otherwise the data blocks
            // after the hole would shift forward in the recovered file.
            const uint64_t take = (std::min)(slotCapacity,
                                             static_cast<uint64_t>(remaining));
            out.emplace_back(0, static_cast<uint32_t>(take));
            remaining -= static_cast<uint32_t>(take);
            continue;
        }

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

    // ── inline_data directory ────────────────────────────────────────────
    // The dirent array lives inside the inode: 4-byte parent inode, then the
    // usual dirent stream.  It is never reachable through i_block[] pointers,
    // so the block-map path below must not run for these inodes.
    if (m_hasInlineData && (inode.i_flags & EXT4_INLINE_DATA_FL)) {
        const uint64_t dirSize = inode.i_size_lo | ((uint64_t)inode.i_size_hi << 32);
        std::vector<uint8_t> body;
        if (!ReadInlineData(inodeNum, inode, dirSize, body) || body.size() <= 4) {
            LOG_WARN(L"Ext4Parser: inline directory body unreadable for inode " +
                     std::to_wstring(inodeNum));
            return;
        }
        ParseDirBlock(body.data() + 4, static_cast<uint32_t>(body.size() - 4),
                      inodeNum, path, cancelFlag);
        return;
    }

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
    bool inlineBodyLost = false;

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

        if (!isDir && node.size > 0) {            if (m_hasInlineData && (childInode.i_flags & EXT4_INLINE_DATA_FL)) {
                // inline_data: the body lives inside the inode (i_block[] plus
                // the system.data ibody xattr).  Reading block pointers here
                // would interpret payload bytes as addresses.
                std::vector<uint8_t> body;
                if (ReadInlineData(entryInode, childInode, node.size, body)) {
                    node.residentData = body;
                } else {
                    inlineBodyLost = true;
                    LOG_WARN(L"Ext4Parser: inline_data body incomplete for inode " +
                             std::to_wstring(entryInode));
                }
            } else {
                std::vector<std::pair<uint64_t, uint32_t>> blocks;
                if ((childInode.i_flags & EXT4_EXTENTS_FL) && m_hasExtents)
                    blocks = GetExtentBlocks(childInode);
                else
                    blocks = GetIndirectBlocks(childInode);

                for (const auto& bc : blocks)
                    AppendBlockRun(node.clusters, bc.first, bc.second);
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
    // An inline payload we could not fully assemble must never be advertised
    // as recoverable: report it as lost instead of writing partial bytes.
    if (inlineBodyLost) node.recoverability = RecoverabilityLevel::Lost;

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
            if (m_hasInlineData && (inode.i_flags & EXT4_INLINE_DATA_FL)) {
                std::vector<uint8_t> body;
                if (ReadInlineData(ino, inode, node.size, body)) {
                    node.residentData = body;
                } else {
                    node.recoverability = RecoverabilityLevel::Lost;
                }
            } else {
                std::vector<std::pair<uint64_t, uint32_t>> blocks;
                if ((inode.i_flags & EXT4_EXTENTS_FL) && m_hasExtents)
                    blocks = GetExtentBlocks(inode);
                else
                    blocks = GetIndirectBlocks(inode);
                for (const auto& bc : blocks)
                    AppendBlockRun(node.clusters, bc.first, bc.second);
            }
        }

        if (node.isDirectory) node.originalPath += L'\\';

        m_files.push_back(node);
        if (m_onFileFound) m_onFileFound(node);
    }
}
