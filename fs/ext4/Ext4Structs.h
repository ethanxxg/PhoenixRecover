#pragma once
#include <cstdint>

#pragma pack(push, 1)

// ─────────────────────────────────────────────────────────────────────────────
// ext4 Superblock (located at byte offset 1024 in the filesystem)
// ─────────────────────────────────────────────────────────────────────────────
struct EXT4_SUPERBLOCK
{
    uint32_t s_inodes_count;          // 0x00: Total inode count
    uint32_t s_blocks_count_lo;       // 0x04: Total block count (lower 32 bits)
    uint32_t s_r_blocks_count_lo;     // 0x08: Reserved block count (lower 32)
    uint32_t s_free_blocks_count_lo;  // 0x0C: Free block count (lower 32)
    uint32_t s_free_inodes_count;     // 0x10: Free inode count
    uint32_t s_first_data_block;      // 0x14: First data block (0 or 1)
    uint32_t s_log_block_size;        // 0x18: Block size = 1024 << s_log_block_size
    uint32_t s_log_cluster_size;      // 0x1C: Cluster size (for bigalloc)
    uint32_t s_blocks_per_group;      // 0x20: Blocks per group
    uint32_t s_clusters_per_group;    // 0x24: Clusters per group (bigalloc)
    uint32_t s_inodes_per_group;      // 0x28: Inodes per group
    uint32_t s_mtime;                 // 0x2C: Mount time
    uint32_t s_wtime;                 // 0x30: Write time
    uint16_t s_mnt_count;             // 0x34: Mount count
    uint16_t s_max_mnt_count;         // 0x36: Maximum mount count
    uint16_t s_magic;                 // 0x38: Magic signature (0xEF53)
    uint16_t s_state;                 // 0x3A: File system state
    uint16_t s_errors;                // 0x3C: Behavior on errors
    uint16_t s_minor_rev_level;       // 0x3E: Minor revision level
    uint32_t s_lastcheck;             // 0x40: Time of last check
    uint32_t s_checkinterval;         // 0x44: Maximum time between checks
    uint32_t s_creator_os;            // 0x48: OS that created the filesystem
    uint32_t s_rev_level;             // 0x4C: Revision level (0 or 1)
    uint16_t s_def_resuid;            // 0x50: Default UID for reserved blocks
    uint16_t s_def_resgid;            // 0x52: Default GID for reserved blocks

    // Dynamic revision only (s_rev_level >= 1)
    uint32_t s_first_ino;             // 0x54: First non-reserved inode
    uint16_t s_inode_size;            // 0x58: Size of inode structure
    uint16_t s_block_group_nr;        // 0x5A: Block group number of this superblock
    uint32_t s_feature_compat;        // 0x5C: Compatible feature set
    uint32_t s_feature_incompat;      // 0x60: Incompatible feature set
    uint32_t s_feature_ro_compat;     // 0x64: Read-only compatible feature set
    uint8_t  s_uuid[16];              // 0x68: 128-bit filesystem UUID
    char     s_volume_name[16];       // 0x78: Volume name
    char     s_last_mounted[64];      // 0x88: Last mount path
    uint32_t s_algorithm_usage_bitmap;// 0xC8: Compression algorithms

    // Performance hints
    uint8_t  s_prealloc_blocks;       // 0xCC: Blocks to preallocate for files
    uint8_t  s_prealloc_dir_blocks;   // 0xCD: Blocks to preallocate for dirs
    uint16_t s_reserved_gdt_blocks;   // 0xCE: Reserved GDT blocks for growth

    // Journaling support
    uint8_t  s_journal_uuid[16];      // 0xD0: UUID of journal superblock
    uint32_t s_journal_inum;          // 0xE0: Inode number of journal file
    uint32_t s_journal_dev;           // 0xE4: Device number of journal file
    uint32_t s_last_orphan;           // 0xE8: Start of list of orphan inodes
    uint32_t s_hash_seed[4];          // 0xEC: HTREE hash seed
    uint8_t  s_def_hash_version;      // 0xFC: Default hash version
    uint8_t  s_jnl_backup_type;       // 0xFD: Journal backup type
    uint16_t s_desc_size;             // 0xFE: Size of group descriptors (64-bit)
    uint32_t s_default_mount_opts;    // 0x100: Default mount options
    uint32_t s_first_meta_bg;         // 0x104: First metablock block group
    uint32_t s_mkfs_time;             // 0x108: Filesystem creation time
    uint32_t s_jnl_blocks[17];        // 0x10C: Backup of journal inode

    // 64-bit support
    uint32_t s_blocks_count_hi;       // 0x150: High 32 bits of block count
    uint32_t s_r_blocks_count_hi;     // 0x154: High 32 bits of reserved blocks
    uint32_t s_free_blocks_count_hi;  // 0x158: High 32 bits of free blocks
    uint16_t s_min_extra_isize;       // 0x15C: All inodes have at least this size
    uint16_t s_want_extra_isize;      // 0x15E: New inodes should reserve this
    uint32_t s_flags;                 // 0x160: Miscellaneous flags
    uint16_t s_raid_stride;           // 0x164: RAID stride
    uint16_t s_mmp_interval;          // 0x166: MMP check wait seconds
    uint64_t s_mmp_block;             // 0x168: Block for multi-mount protection
    uint32_t s_raid_stripe_width;     // 0x170: Blocks on all data disks
    uint8_t  s_log_groups_per_flex;   // 0x174: FLEX_BG group size
    uint8_t  s_checksum_type;         // 0x175: Metadata checksum algorithm
    uint16_t s_reserved_pad;          // 0x176
    uint64_t s_kbytes_written;        // 0x178: KB written to this filesystem
    uint32_t s_snapshot_inum;         // 0x180: Snapshot inode
    uint32_t s_snapshot_id;           // 0x184: Sequential snapshot ID
    uint64_t s_snapshot_r_blocks_count; // 0x188: Reserved blocks for snapshot
    uint32_t s_snapshot_list;         // 0x190: Inode of snapshot list
    uint32_t s_error_count;           // 0x194: Number of errors
    uint32_t s_first_error_time;      // 0x198: First error time
    uint32_t s_first_error_ino;       // 0x19C: Inode involved in first error
    uint64_t s_first_error_block;     // 0x1A0: Block involved in first error
    uint8_t  s_first_error_func[32];  // 0x1A8: Function where first error happened
    uint32_t s_first_error_line;      // 0x1C8: Line number of first error
    uint32_t s_last_error_time;       // 0x1CC: Time of last error
    uint32_t s_last_error_ino;        // 0x1D0: Inode in last error
    uint32_t s_last_error_line;       // 0x1D4: Line number of last error
    uint64_t s_last_error_block;      // 0x1D8: Block in last error
    uint8_t  s_last_error_func[32];   // 0x1E0: Function where last error happened
    uint8_t  s_mount_opts[64];        // 0x200: Mount options string
    uint32_t s_usr_quota_inum;        // 0x240: Inode for user quota
    uint32_t s_grp_quota_inum;        // 0x244: Inode for group quota
    uint32_t s_overhead_blocks;       // 0x248: Overhead blocks
    uint32_t s_backup_bgs[2];         // 0x24C: Groups with sparse_super2 SBs
    uint8_t  s_encrypt_algos[4];      // 0x254: Encryption algorithms
    uint8_t  s_encrypt_pw_salt[16];   // 0x258: Salt for encryption
    uint32_t s_lpf_ino;               // 0x268: lost+found inode
    uint32_t s_prj_quota_inum;        // 0x26C: Project quota inode
    uint32_t s_checksum_seed;         // 0x270: crc32c seed for checksum
    uint8_t  s_wtime_hi;              // 0x274: Write time high bits
    uint8_t  s_mtime_hi;              // 0x275: Mount time high bits
    uint8_t  s_mkfs_time_hi;          // 0x276: mkfs time high bits
    uint8_t  s_lastcheck_hi;          // 0x277: Last check time high bits
    uint8_t  s_first_error_time_hi;   // 0x278
    uint8_t  s_last_error_time_hi;    // 0x279
    uint8_t  s_pad[2];                // 0x27A
    uint16_t s_encoding;              // 0x27C: Filename charset encoding
    uint16_t s_encoding_flags;        // 0x27E: Filename encoding flags
    uint32_t s_reserved[95];          // 0x280: Padding to 1024 bytes
    uint32_t s_checksum;              // 0x3FC: Superblock checksum
};
static_assert(sizeof(EXT4_SUPERBLOCK) == 1024, "EXT4_SUPERBLOCK must be 1024 bytes");

constexpr uint16_t EXT4_SUPER_MAGIC = 0xEF53;

// ── Feature flags ────────────────────────────────────────────────────────────
constexpr uint32_t EXT4_FEATURE_INCOMPAT_FILETYPE   = 0x0002;
constexpr uint32_t EXT4_FEATURE_INCOMPAT_EXTENTS    = 0x0040;
constexpr uint32_t EXT4_FEATURE_INCOMPAT_64BIT      = 0x0080;
constexpr uint32_t EXT4_FEATURE_INCOMPAT_FLEX_BG    = 0x0200;
constexpr uint32_t EXT4_FEATURE_INCOMPAT_INLINE_DATA = 0x8000;
constexpr uint32_t EXT4_FEATURE_RO_COMPAT_BIGALLOC  = 0x0200;

// ── inline_data / in-inode xattr (ibody) ────────────────────────────────────
constexpr uint32_t EXT4_INLINE_DATA_FL      = 0x10000000;
constexpr uint32_t EXT4_XATTR_IBODY_MAGIC   = 0xEA020000;
constexpr uint8_t  EXT4_XATTR_INDEX_SYSTEM  = 7;   // "system.data"
constexpr uint8_t  EXT4_INLINE_DATA_HEAD    = 60;  // bytes carried by i_block[]

// ─────────────────────────────────────────────────────────────────────────────
// Block Group Descriptor (32 or 64 bytes)
// ─────────────────────────────────────────────────────────────────────────────
struct EXT4_GROUP_DESC
{
    uint32_t bg_block_bitmap_lo;      // 0x00: Block bitmap block (lower 32)
    uint32_t bg_inode_bitmap_lo;      // 0x04: Inode bitmap block (lower 32)
    uint32_t bg_inode_table_lo;       // 0x08: Inode table block (lower 32)
    uint16_t bg_free_blocks_count_lo; // 0x0C: Free blocks count (lower 16)
    uint16_t bg_free_inodes_count_lo; // 0x0E: Free inodes count (lower 16)
    uint16_t bg_used_dirs_count_lo;   // 0x10: Used directories count (lower 16)
    uint16_t bg_flags;                // 0x12: Flags
    uint32_t bg_exclude_bitmap_lo;    // 0x14: Exclude bitmap block (lower 32)
    uint16_t bg_block_bitmap_csum_lo; // 0x18: Block bitmap checksum (lower 16)
    uint16_t bg_inode_bitmap_csum_lo; // 0x1A: Inode bitmap checksum (lower 16)
    uint16_t bg_itable_unused_lo;     // 0x1C: Unused inodes count (lower 16)
    uint16_t bg_checksum;             // 0x1E: Group descriptor checksum

    // 64-bit extension (only if INCOMPAT_64BIT is set and s_desc_size >= 64)
    uint32_t bg_block_bitmap_hi;      // 0x20
    uint32_t bg_inode_bitmap_hi;      // 0x24
    uint32_t bg_inode_table_hi;       // 0x28
    uint16_t bg_free_blocks_count_hi; // 0x2C
    uint16_t bg_free_inodes_count_hi; // 0x2E
    uint16_t bg_used_dirs_count_hi;   // 0x30
    uint16_t bg_itable_unused_hi;     // 0x32
    uint32_t bg_exclude_bitmap_hi;    // 0x34
    uint16_t bg_block_bitmap_csum_hi; // 0x38
    uint16_t bg_inode_bitmap_csum_hi; // 0x3A
    uint32_t bg_reserved;             // 0x3C
};
static_assert(sizeof(EXT4_GROUP_DESC) == 64, "EXT4_GROUP_DESC must be 64 bytes");

// ─────────────────────────────────────────────────────────────────────────────
// Inode structure (128 or 256 bytes for ext4)
// ─────────────────────────────────────────────────────────────────────────────
struct EXT4_INODE
{
    uint16_t i_mode;                  // 0x00: File mode
    uint16_t i_uid;                   // 0x02: Owner UID (lower 16)
    uint32_t i_size_lo;               // 0x04: File size (lower 32)
    uint32_t i_atime;                 // 0x08: Last access time
    uint32_t i_ctime;                 // 0x0C: Inode change time
    uint32_t i_mtime;                 // 0x10: Last modification time
    uint32_t i_dtime;                 // 0x14: Deletion time
    uint16_t i_gid;                   // 0x18: Owner GID (lower 16)
    uint16_t i_links_count;           // 0x1A: Hard link count
    uint32_t i_blocks_lo;             // 0x1C: Block count (512-byte units, lower 32)
    uint32_t i_flags;                 // 0x20: File flags
    uint32_t i_osd1;                  // 0x24: OS-dependent value 1
    uint8_t  i_block[60];             // 0x28: Block map or extent tree
    uint32_t i_generation;            // 0x64: File version (for NFS)
    uint32_t i_file_acl_lo;           // 0x68: File ACL (lower 32)
    uint32_t i_size_hi;               // 0x6C: File size (upper 32)
    uint32_t i_obso_faddr;            // 0x70: Obsolete fragment address
    uint16_t i_blocks_hi;             // 0x74: Block count (upper 16)
    uint16_t i_file_acl_hi;           // 0x76: File ACL (upper 16)
    uint16_t i_uid_hi;                // 0x78: UID (upper 16)
    uint16_t i_gid_hi;                // 0x7A: GID (upper 16)
    uint16_t i_checksum_lo;           // 0x7C: Inode checksum (lower 16)
    uint16_t i_reserved;              // 0x7E

    // Extra inode fields (if s_inode_size > 128)
    uint16_t i_extra_isize;           // 0x80: Size of extra inode fields
    uint16_t i_checksum_hi;           // 0x82: Inode checksum (upper 16)
    uint32_t i_ctime_extra;           // 0x84: Extra change time bits
    uint32_t i_mtime_extra;           // 0x88: Extra mod time bits
    uint32_t i_atime_extra;           // 0x8C: Extra access time bits
    uint32_t i_crtime;                // 0x90: File creation time
    uint32_t i_crtime_extra;          // 0x94: Extra creation time bits
    uint32_t i_version_hi;            // 0x98: High 32 bits of file version
    uint32_t i_projid;                // 0x9C: Project ID
};
static_assert(sizeof(EXT4_INODE) >= 128, "EXT4_INODE base must be >= 128 bytes");

// ── Inode mode bits ──────────────────────────────────────────────────────────
constexpr uint16_t EXT4_S_IFMT   = 0xF000;  // Format mask
constexpr uint16_t EXT4_S_IFSOCK = 0xC000;  // Socket
constexpr uint16_t EXT4_S_IFLNK  = 0xA000;  // Symbolic link
constexpr uint16_t EXT4_S_IFREG  = 0x8000;  // Regular file
constexpr uint16_t EXT4_S_IFBLK  = 0x6000;  // Block device
constexpr uint16_t EXT4_S_IFDIR  = 0x4000;  // Directory
constexpr uint16_t EXT4_S_IFCHR  = 0x2000;  // Character device
constexpr uint16_t EXT4_S_IFIFO  = 0x1000;  // FIFO

// ── Inode flags ──────────────────────────────────────────────────────────────
constexpr uint32_t EXT4_INDEX_FL     = 0x00001000;  // Hash-tree (htree) directory
constexpr uint32_t EXT4_EXTENTS_FL    = 0x00080000;  // Uses extents

// ── ext4 htree (indexed directory) ───────────────────────────────────────────
constexpr uint32_t EXT4_DX_ROOT_INFO_OFF   = 0x18;
constexpr uint32_t EXT4_DX_ROOT_LIMIT_OFF  = 0x20;
constexpr uint32_t EXT4_DX_ROOT_COUNT_OFF  = 0x22;
constexpr uint32_t EXT4_DX_ROOT_BLOCK_OFF  = 0x24;
constexpr uint32_t EXT4_DX_ROOT_ENTRIES_OFF = 0x28;

struct EXT4_DX_ROOT_INFO
{
    uint32_t reserved_zero;
    uint8_t  hash_version;
    uint8_t  info_length;
    uint8_t  indirect_levels;
    uint8_t  unused_flags;
};

struct EXT4_DX_COUNT_LIMIT
{
    uint16_t limit;
    uint16_t count;
};

struct EXT4_DX_ENTRY
{
    uint32_t hash;
    uint32_t block;   // Logical block within directory inode
};

// ─────────────────────────────────────────────────────────────────────────────
// Extent structures
// ─────────────────────────────────────────────────────────────────────────────
struct EXT4_EXTENT_HEADER
{
    uint16_t eh_magic;                // 0xF30A
    uint16_t eh_entries;              // Number of valid entries
    uint16_t eh_max;                  // Capacity of entries
    uint16_t eh_depth;                // Depth of tree (0 = leaf)
    uint32_t eh_generation;           // Generation
};
constexpr uint16_t EXT4_EXT_MAGIC = 0xF30A;

struct EXT4_EXTENT
{
    uint32_t ee_block;                // First logical block covered
    uint16_t ee_len;                  // Number of blocks covered
    uint16_t ee_start_hi;             // High 16 bits of physical block
    uint32_t ee_start_lo;             // Low 32 bits of physical block
};

struct EXT4_EXTENT_IDX
{
    uint32_t ei_block;                // First logical block this index covers
    uint32_t ei_leaf_lo;              // Low 32 bits of child node block
    uint16_t ei_leaf_hi;              // High 16 bits of child node block
    uint16_t ei_unused;               // Unused
};

// ─────────────────────────────────────────────────────────────────────────────
// Directory entry (ext4 with FILETYPE feature)
// ─────────────────────────────────────────────────────────────────────────────
struct EXT4_DIR_ENTRY_2
{
    uint32_t inode;                   // Inode number (0 if deleted)
    uint16_t rec_len;                 // Directory entry length
    uint8_t  name_len;                // Name length
    uint8_t  file_type;               // File type
    // char name[EXT4_NAME_LEN];      // File name (variable length)
};

// ── File type values ─────────────────────────────────────────────────────────
constexpr uint8_t EXT4_FT_UNKNOWN  = 0;
constexpr uint8_t EXT4_FT_REG_FILE = 1;
constexpr uint8_t EXT4_FT_DIR      = 2;
constexpr uint8_t EXT4_FT_CHRDEV   = 3;
constexpr uint8_t EXT4_FT_BLKDEV   = 4;
constexpr uint8_t EXT4_FT_FIFO     = 5;
constexpr uint8_t EXT4_FT_SOCK     = 6;
constexpr uint8_t EXT4_FT_SYMLINK  = 7;

constexpr uint32_t EXT4_NAME_LEN   = 255;
constexpr uint32_t EXT4_ROOT_INO   = 2;

// ─────────────────────────────────────────────────────────────────────────────
// JBD2 journal
//
// Every field is big-endian and the structures are variable length, so they are
// decoded by offset through Ext4Parser::Be32/Be16 instead of being memcpy'd
// into a packed struct.
//
// Journal header (12 bytes, shared by every log block):
//   +0x00  h_magic      be32  0xC03B3998
//   +0x04  h_blocktype  be32  1..5
//   +0x08  h_sequence   be32  transaction sequence (0 for superblock/revoke)
//
// Superblock (block 0 of the journal):
//   +0x0C  s_blocksize        be32  must equal the filesystem block size
//   +0x10  s_maxlen           be32  journal length in blocks
//   +0x14  s_first            be32  first legal log block (normally 1)
//   +0x18  s_sequence         be32  sequence of the first log block
//   +0x1C  s_start            be32  0 = cleanly unmounted, nothing to replay
//   +0x28  s_feature_incompat be32  bit1 = 64-bit block numbers (tag = 12 B)
//
// Descriptor block: 12-byte header then journal_block_tag_s records:
//   be32 t_blocknr, be16 t_checksum, be16 t_flags [, be32 t_blocknr_high]
// The data blocks follow the descriptor block, one per non-DELETED tag, in tag
// order.  A transaction too large for one descriptor continues with another
// descriptor block after the data blocks and ends at its commit block.
//
// Revoke block: 12-byte header (r_count at +0x08 counts bytes used, header
// included) followed by be32 block numbers, or be64 when 64BIT is set.
//
// Replay rule (jbd2_journal_test_revoke): a logged copy survives only when its
// transaction sequence is GREATER than the sequence of the last revoke record
// for that block.  A revoke therefore cancels the copy logged in the same
// transaction and every earlier one, but not a later re-log.
// ─────────────────────────────────────────────────────────────────────────────
constexpr uint32_t JBD2_MAGIC                = 0xC03B3998;
constexpr uint32_t JBD2_BLOCKTYPE_DESCRIPTOR = 1;
constexpr uint32_t JBD2_BLOCKTYPE_COMMIT     = 2;
constexpr uint32_t JBD2_BLOCKTYPE_SB_V1      = 3;
constexpr uint32_t JBD2_BLOCKTYPE_SB_V2      = 4;
constexpr uint32_t JBD2_BLOCKTYPE_REVOKE     = 5;

constexpr uint16_t JBD2_FLAG_ESCAPE    = 0x0001;  // first 4 B zeroed on disk
constexpr uint16_t JBD2_FLAG_SAME_UUID = 0x0002;  // v1 only
constexpr uint16_t JBD2_FLAG_DELETED   = 0x0004;  // no data block follows
constexpr uint16_t JBD2_FLAG_LAST_TAG  = 0x0008;

constexpr uint32_t JBD2_FEATURE_INCOMPAT_REVOKE    = 0x0001;
constexpr uint32_t JBD2_FEATURE_INCOMPAT_64BIT     = 0x0002;
constexpr uint32_t JBD2_FEATURE_INCOMPAT_CSUM_V2   = 0x0008;
constexpr uint32_t JBD2_FEATURE_INCOMPAT_CSUM_V3   = 0x0010;
constexpr uint32_t JBD2_CRC32C_CHKSUM              = 4;
constexpr uint32_t JBD2_JOURNAL_BLOCK_TAIL_SIZE    = 4;  // struct jbd2_journal_block_tail{be32 t_checksum}
// journal superblock field offsets (big-endian)
constexpr uint32_t JBD2_SB_UUID_OFF       = 0x30;
constexpr uint32_t JBD2_SB_CHECKSUM_OFF   = 0xFC;
constexpr uint32_t JBD2_COMMIT_CSUM_OFF   = 0x10;  // commit_header.h_chksum[0]
constexpr uint32_t JBD2_COMMIT_CSUM_TYPE_OFF = 0x0C;

#pragma pack(pop)
