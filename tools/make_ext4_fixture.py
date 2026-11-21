#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Synthetic ext4 fixture generator (independent oracle, zero external deps).

Why hand-rolled: WSL / e2fsprogs (`mkfs.ext4`, `debugfs`) are unavailable in
this environment, and the project needs a *repeatable* gold sample whose
expected bytes we can compute without any ext4 implementation.  Every field
written here follows the on-disk layout documented in
Documentation/filesystems/ext4/ (superblock 1024 B at offset 1024, group
descriptor table at block 2 for 1 KiB blocks / block 1 for larger blocks,
inode table per group, extent tree header magic 0xF30A, dirent v2 layout).

Variants (all single block group, 1 KiB blocks unless stated):

  ext4-base       extents + legacy indirect block map + deleted dirent in
                  slack + orphan (no dirent) inode
  ext4-uninit     preallocated file with an UNINITIALIZED extent
                  (ee_len = 32768 + n): content must recover as zeros, not as
                  the stale 0xAA bytes that sit in those blocks
  ext4-bigalloc   bigalloc volume (s_log_cluster_size = s_log_block_size + 1):
                  extent ee_start/ee_len are CLUSTERS, not blocks
  ext4-inline     inline_data: small file and small directory carried inside
                  the inode body (EXT4_INLINE_DATA_FL)

Usage:
    py -3 tools/make_ext4_fixture.py [--out DIR] [--only NAME]

Outputs, per variant:
    <out>/<name>.img    raw filesystem image (partition starts at sector 0)
    <out>/<name>.json   manifest: geometry + expected file bytes (sha256)
"""

import argparse
import hashlib
import json
import os
import struct
import sys

# ── constants (must match fs/ext4/Ext4Structs.h) ────────────────────────────
S_IFREG = 0x8000
S_IFDIR = 0x4000
EXTENTS_FL = 0x00080000
INLINE_DATA_FL = 0x10000000
INCOMPAT_FILETYPE = 0x0002
INCOMPAT_EXTENTS = 0x0040
INCOMPAT_INLINE_DATA = 0x8000
RO_COMPAT_BIGALLOC = 0x0200
EXT_MAGIC = 0xF30A
FT_REG = 1
FT_DIR = 2

FIRST_INO = 11            # s_first_ino for dynamic-rev filesystems
ROOT_INO = 2

LOG_BY_SIZE = {1024: 0, 2048: 1, 4096: 2, 8192: 3, 16384: 4}


def align4(n):
    return (n + 3) & ~3


def dirent(ino, name, ftype, rec_len=None):
    """ext4_dir_entry_2: inode u32, rec_len u16, name_len u8, type u8."""
    nb = name.encode("utf-8") if isinstance(name, str) else name
    need = align4(8 + len(nb))
    rec = need if rec_len is None else rec_len
    assert rec >= need and rec % 4 == 0, (name, rec, need)
    return struct.pack("<IHBB", ino, rec, len(nb), ftype) + nb + b"\x00" * (rec - 8 - len(nb))


def extent_tree(extents, blocks_per_cluster=1):
    """Leaf-only extent tree.

    extents: list of (logical_block, start, length, uninit)
    With bigalloc the on-disk ee_start/ee_len are in clusters, and the logical
    block is still a block index.
    """
    assert len(extents) <= 4, "leaf fits at most 4 extents in 60 bytes"
    body = b""
    for logical, start, length, uninit in extents:
        ee_len = length if not uninit else (length + 32768)
        body += struct.pack("<IHHI", logical, ee_len, (start >> 32) & 0xFFFF, start & 0xFFFFFFFF)
    hdr = struct.pack("<HHHHI", EXT_MAGIC, len(extents), 4, 0, 0)
    return hdr + body


def inode_bytes(mode, size, links, flags, iblock, blocks512,
                atime=1700000000, ctime=1700000000, mtime=1700000000,
                dtime=0, inode_size=256, crtime=1700000000):
    b = bytearray(inode_size)
    struct.pack_into("<H", b, 0x00, mode)
    struct.pack_into("<H", b, 0x02, 0)              # uid
    struct.pack_into("<I", b, 0x04, size & 0xFFFFFFFF)
    struct.pack_into("<I", b, 0x08, atime)
    struct.pack_into("<I", b, 0x0C, ctime)
    struct.pack_into("<I", b, 0x10, mtime)
    struct.pack_into("<I", b, 0x14, dtime)
    struct.pack_into("<H", b, 0x18, 0)              # gid
    struct.pack_into("<H", b, 0x1A, links)
    struct.pack_into("<I", b, 0x1C, blocks512)
    struct.pack_into("<I", b, 0x20, flags)
    assert len(iblock) <= 60
    b[0x28:0x28 + len(iblock)] = iblock
    struct.pack_into("<I", b, 0x6C, (size >> 32) & 0xFFFFFFFF)   # i_size_hi
    if inode_size > 128:
        struct.pack_into("<H", b, 0x80, 32)         # i_extra_isize
        struct.pack_into("<I", b, 0x90, crtime)     # i_crtime
    return bytes(b)


class Ext4Image:
    def __init__(self, bs=1024, total_blocks=8192, inodes=128, inode_size=256,
                 incompat=0, ro_compat=0, cluster_ratio=1):
        self.bs = bs
        self.total_blocks = total_blocks
        self.inodes = inodes
        self.inode_size = inode_size
        self.incompat = incompat
        self.ro_compat = ro_compat
        self.cluster_ratio = cluster_ratio          # blocks per cluster (bigalloc)
        self.journal_inum = 0                       # s_journal_inum (0xE0)
        # Post-assembly patches.  finish() writes the inode table from
        # inodes_data, so a variant that needs a *stale home copy* (the JBD2
        # fixture does) registers a hook instead of fighting the order.
        self.hooks = []

        self.log_block_size = LOG_BY_SIZE[bs]
        self.log_cluster_size = self.log_block_size + (cluster_ratio.bit_length() - 1)
        # sanity: cluster_ratio must be a power of two
        assert cluster_ratio & (cluster_ratio - 1) == 0

        self.img = bytearray(bs * total_blocks)

        self.gdt_block = 2 if bs == 1024 else 1
        self.bbitmap_block = self.gdt_block + 1
        self.ibitmap_block = self.gdt_block + 2
        self.itable_block = self.gdt_block + 3
        itable_blocks = (inodes * inode_size + bs - 1) // bs
        self.next_block = self.itable_block + itable_blocks

        self.inodes_data = {}
        self.used_blocks = set()
        for b in range(0, self.next_block):
            self.used_blocks.add(b)

    # ── low level ──────────────────────────────────────────────────────────
    def alloc(self, nblocks):
        # bigalloc: allocations are whole clusters, and the start must be
        # cluster aligned (mkfs never hands out a misaligned cluster).
        if self.cluster_ratio > 1:
            nclust = (nblocks + self.cluster_ratio - 1) // self.cluster_ratio
            nblocks = nclust * self.cluster_ratio
            padded = (self.next_block + self.cluster_ratio - 1) // self.cluster_ratio
            start = padded * self.cluster_ratio
            for b in range(self.next_block, start):
                self.used_blocks.add(b)
        else:
            start = self.next_block
        for b in range(start, start + nblocks):
            self.used_blocks.add(b)
        self.next_block = start + nblocks
        assert self.next_block <= self.total_blocks, "image too small"
        return start

    def write_block(self, blk, data):
        assert len(data) <= self.bs
        off = blk * self.bs
        self.img[off:off + len(data)] = data

    def write_at(self, off, data):
        self.img[off:off + len(data)] = data

    def put_inode(self, ino, raw):
        assert len(raw) == self.inode_size
        self.inodes_data[ino] = raw

    # ── finalize ───────────────────────────────────────────────────────────
    def finish(self):
        bs = self.bs
        # inode table
        for ino, raw in self.inodes_data.items():
            idx = ino - 1
            off = self.itable_block * bs + idx * self.inode_size
            self.img[off:off + self.inode_size] = raw

        # block bitmap: mark used blocks
        bitmap = bytearray(bs)
        for b in self.used_blocks:
            if b < bs * 8:
                bitmap[b // 8] |= 1 << (b % 8)
        self.write_block(self.bbitmap_block, bytes(bitmap))

        # inode bitmap: mark inodes 1..max as used (all created inodes are
        # allocated on disk even when the file was deleted; a deleted inode is
        # cleared in the bitmap, which is exactly what we want to exercise)
        ibitmap = bytearray(bs)
        for ino in self.inodes_data:
            idx = ino - 1
            if idx < bs * 8:
                ibitmap[idx // 8] |= 1 << (idx % 8)
        self.write_block(self.ibitmap_block, bytes(ibitmap))

        # group descriptor
        gd = bytearray(32)
        struct.pack_into("<I", gd, 0x00, self.bbitmap_block)
        struct.pack_into("<I", gd, 0x04, self.ibitmap_block)
        struct.pack_into("<I", gd, 0x08, self.itable_block)
        free_blocks = self.total_blocks - len(self.used_blocks)
        struct.pack_into("<H", gd, 0x0C, min(free_blocks, 0xFFFF))
        free_inodes = self.inodes - len(self.inodes_data)
        struct.pack_into("<H", gd, 0x0E, max(free_inodes, 0))
        struct.pack_into("<H", gd, 0x10, 1)          # used dirs
        self.write_at(self.gdt_block * bs, bytes(gd))

        # superblock
        sb = bytearray(1024)
        struct.pack_into("<I", sb, 0x00, self.inodes)
        struct.pack_into("<I", sb, 0x04, self.total_blocks & 0xFFFFFFFF)
        struct.pack_into("<I", sb, 0x08, 0)
        struct.pack_into("<I", sb, 0x0C, free_blocks & 0xFFFFFFFF)
        struct.pack_into("<I", sb, 0x10, max(free_inodes, 0))
        struct.pack_into("<I", sb, 0x14, 1 if bs == 1024 else 0)   # first_data_block
        struct.pack_into("<I", sb, 0x18, self.log_block_size)
        struct.pack_into("<I", sb, 0x1C, self.log_cluster_size)
        struct.pack_into("<I", sb, 0x20, self.total_blocks)        # blocks_per_group
        struct.pack_into("<I", sb, 0x24, self.total_blocks // self.cluster_ratio)
        struct.pack_into("<I", sb, 0x28, self.inodes)              # inodes_per_group
        struct.pack_into("<I", sb, 0x2C, 1700000000)               # mtime
        struct.pack_into("<I", sb, 0x30, 1700000000)               # wtime
        struct.pack_into("<H", sb, 0x34, 0)
        struct.pack_into("<H", sb, 0x36, 0xFFFF)
        struct.pack_into("<H", sb, 0x38, 0xEF53)                   # magic
        struct.pack_into("<H", sb, 0x3A, 1)                        # clean
        struct.pack_into("<H", sb, 0x3C, 1)
        struct.pack_into("<H", sb, 0x3E, 0)
        struct.pack_into("<I", sb, 0x48, 0)                        # creator_os = Linux
        struct.pack_into("<I", sb, 0x4C, 1)                        # rev_level = dynamic
        struct.pack_into("<H", sb, 0x54, FIRST_INO)
        struct.pack_into("<H", sb, 0x58, self.inode_size)
        struct.pack_into("<H", sb, 0x5A, 0)            # s_block_group_nr
        struct.pack_into("<I", sb, 0x5C, 0)                        # feature_compat
        struct.pack_into("<I", sb, 0x60, self.incompat)
        struct.pack_into("<I", sb, 0x64, self.ro_compat)
        struct.pack_into("<I", sb, 0xE0, self.journal_inum)        # s_journal_inum
        self.write_at(1024, bytes(sb))

        for hook in self.hooks:
            hook(self.img)
        return bytes(self.img)


# ── payload helpers ─────────────────────────────────────────────────────────
def pattern(tag, size):
    """Deterministic, non-repeating-friendly payload."""
    seed = hashlib.sha256(tag.encode("utf-8")).digest()
    out = bytearray()
    counter = 0
    while len(out) < size:
        out += hashlib.sha256(seed + counter.to_bytes(4, "little")).digest()
        counter += 1
    return bytes(out[:size])


def sha(b):
    return hashlib.sha256(b).hexdigest()


def manifest_entry(path, name, size, payload, status, kind="data"):
    return {
        "path": path,
        "name": name,
        "size": size,
        "sha256": sha(payload) if payload is not None else None,
        "status": status,
        "kind": kind,
    }


def dirblock(entries, bs):
    """Assemble one directory block.

    entries: list of (inode, name, file_type).  The last entry's rec_len runs
    to the end of the block, exactly like a real ext4 directory.
    """
    body = b""
    for i, (ino, name, ftype) in enumerate(entries):
        last = (i == len(entries) - 1)
        nb = name.encode("utf-8")
        need = align4(8 + len(nb))
        rec = (bs - len(body)) if last else need
        assert rec >= need and rec % 4 == 0, (name, rec, need)
        body += dirent(ino, name, ftype, rec)
    assert len(body) == bs, (len(body), bs)
    return body


def itable_block_bytes(img, ino, override_raw):
    """Serialise the inode-table block that holds `ino`, with `override_raw`
    substituted for that slot.  Used to build a journal image of the inode
    table from *before* the on-disk copy went stale."""
    bs = img.bs
    blk = img.itable_block + ((ino - 1) * img.inode_size) // bs
    base = blk * bs - img.itable_block * bs          # offset within the table
    buf = bytearray(bs)
    for i, raw in img.inodes_data.items():
        o = (i - 1) * img.inode_size
        if base <= o and o + img.inode_size <= base + bs:
            buf[o - base:o - base + img.inode_size] = raw
    o = (ino - 1) * img.inode_size
    buf[o - base:o - base + img.inode_size] = override_raw
    return blk, bytes(buf)


# ── CRC32C (Castagnoli, reflected 0x82F63B78) ──────────────────────────────
# Independent oracle implementation.  Linux jbd2 uses crc32c() with the same
# polynomial; check vector: CRC32C("123456789") == 0xE3069283, matching the
# C++ implementation in fs/refs/RefsCrypto.h.
def _crc32c_table():
    tbl = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ (0x82F63B78 if (c & 1) else 0)
        tbl.append(c & 0xFFFFFFFF)
    return tbl


_CRC32C_TBL = _crc32c_table()


def crc32c_init():
    return 0xFFFFFFFF


def crc32c_update(state, data):
    tbl = _CRC32C_TBL
    for b in data:
        state = tbl[(state ^ b) & 0xFF] ^ (state >> 8)
    return state & 0xFFFFFFFF


def crc32c(data, state=0xFFFFFFFF):
    """Final CRC32C value (init 0xFFFFFFFF, xorout 0xFFFFFFFF)."""
    return crc32c_update(state, data) ^ 0xFFFFFFFF


# ── JBD2 record writers (all fields big-endian) ─────────────────────────────
JBD2_MAGIC = 0xC03B3998
JBD2_DESCRIPTOR, JBD2_COMMIT = 1, 2
JBD2_SB_V1, JBD2_SB_V2, JBD2_REVOKE = 3, 4, 5
JBD2_FLAG_LAST_TAG = 0x0008
JBD2_FLAG_SAME_UUID = 0x0002
JBD2_INCOMPAT_REVOKE = 0x0001
JBD2_INCOMPAT_CSUM_V2 = 0x0008
JBD2_INCOMPAT_CSUM_V3 = 0x0010
JBD2_CRC32C_CHKSUM = 4

# Fixed journal UUID.  The kernel derives j_csum_seed = crc32c(~0, uuid, 16).
JBD_UUID = bytes(range(0x10, 0x20))


def jbd_crc(data, seed=0xFFFFFFFF):
    """Linux jbd2_chksum(): raw CHAINED crc32c state, NO xorout."""
    return crc32c_update(seed, data)


def jbd_seed():
    return jbd_crc(JBD_UUID)


def jbd_super(bs, maxlen, first, seq, start, csum=0):
    """csum: 0 = no checksum, 2 = CSUM_V2, 3 = CSUM_V3."""
    b = struct.pack(">III", JBD2_MAGIC, JBD2_SB_V2, 0)          # h_sequence = 0
    b += struct.pack(">IIIIII", bs, maxlen, first, seq, start, 0)  # ...s_errno
    incompat = JBD2_INCOMPAT_REVOKE
    if csum == 2:
        incompat |= JBD2_INCOMPAT_CSUM_V2
    elif csum == 3:
        incompat |= JBD2_INCOMPAT_CSUM_V3
    b += struct.pack(">III", 0, incompat, 0)                     # compat/incompat/ro
    b += JBD_UUID                                               # s_uuid @ 0x30
    b += struct.pack(">IIII", 1, 0, 0, 0)                       # nr_users, ...
    if csum:
        # s_checksum_type @ 0x50
        b = b[:0x50] + bytes([JBD2_CRC32C_CHKSUM]) + b[0x51:]
        # s_checksum @ 0xFC = jbd2_chksum(seed, WHOLE sb block with that
        # 4-byte field zeroed) — same rule as commit/desc tails, not a prefix.
        buf = bytearray(b + b"\x00" * (bs - len(b)))
        struct.pack_into(">I", buf, 0xFC, 0)
        struct.pack_into(">I", buf, 0xFC, jbd_crc(bytes(buf), jbd_seed()))
        return bytes(buf)
    return b + b"\x00" * (bs - len(b))


def jbd_super_v1(bs, maxlen, first, seq, start):
    """JBD (v1) journal superblock: blocktype 3, no feature/checksum fields.

    The service is still read by the same offsets as JBD2 for blocksize,
    maxlen, first, sequence and start; bytes from 0x20 onward are reserved.
    """
    b = struct.pack(">III", JBD2_MAGIC, JBD2_SB_V1, 0)
    b += struct.pack(">IIIIII", bs, maxlen, first, seq, start, 0)
    b += JBD_UUID                                               # s_uuid @ 0x30
    return b + b"\x00" * (bs - len(b))


def jbd_desc_v1(bs, seq, targets, uuid=JBD_UUID):
    """JBD v1 descriptor: be32 blocknr + be32 flags, then a 16-byte UUID.

    The first tag writes the UUID; later tags set JBD2_FLAG_SAME_UUID so the
    reader reuses it.  LAST_TAG terminates the array exactly as in v2.
    """
    body = bytearray(struct.pack(">III", JBD2_MAGIC, JBD2_DESCRIPTOR, seq))
    for i, (bn, flags) in enumerate(targets):
        if i == len(targets) - 1:
            flags |= JBD2_FLAG_LAST_TAG
        if i > 0:
            flags |= JBD2_FLAG_SAME_UUID
        body += struct.pack(">II", bn, flags)
        if not (flags & JBD2_FLAG_SAME_UUID):
            body += uuid
    assert len(body) <= bs
    return bytes(body + b"\x00" * (bs - len(body)))


def jbd_desc(bs, seq, targets, csum=0, data_blocks=None, bad_tchecksum=False,
             bad_desc_csum=False):
    """Descriptor block.  Targets are (blocknr, flags) in data-block order.

    csum=0: 8-byte legacy tag (be32 blocknr, be16 csum=0, be16 flags).
    csum=2: 8-byte v2 tag; t_checksum = low16 of crc32c(seed, be32(seq), data).
    csum=3: 16-byte v3 tag (be32 blocknr, be32 flags, be32 high=0, be32 csum).
    `data_blocks[i]` is the raw data-block bytes for target i, used to compute
    the per-tag checksum exactly as jbd2_block_tag_csum_set() does.
    """
    body = bytearray(struct.pack(">III", JBD2_MAGIC, JBD2_DESCRIPTOR, seq))
    seed = jbd_seed()
    seq_be = struct.pack(">I", seq)
    for i, (bn, flags) in enumerate(targets):
        if i == len(targets) - 1:
            flags |= JBD2_FLAG_LAST_TAG
        if csum == 0:
            body += struct.pack(">IHH", bn, 0, flags)
        else:
            raw = data_blocks[i] if data_blocks else b""
            # two chained raw jbd2_chksum calls: (seed, seq) then (state, data)
            c = jbd_crc(raw, jbd_crc(seq_be, jbd_seed()))
            if bad_tchecksum and i == 0:
                c ^= 0xFFFF
            if csum == 2:
                body += struct.pack(">IHH", bn, c & 0xFFFF, flags)
            else:  # csum == 3
                body += struct.pack(">IIII", bn, flags, 0, c)
    assert len(body) <= bs - (4 if csum else 0)
    if csum:
        # descriptor/revoke tail: be32 csum over whole block with tail zeroed
        buf = bytearray(body + b"\x00" * (bs - len(body)))
        struct.pack_into(">I", buf, bs - 4, 0)
        d = jbd_crc(bytes(buf), seed)
        if bad_desc_csum:
            d ^= 0xFFFFFFFF
        struct.pack_into(">I", buf, bs - 4, d)
        return bytes(buf)
    return bytes(body + b"\x00" * (bs - len(body)))


def jbd_commit(bs, seq, csum=0, bad_csum=False):
    b = bytearray(struct.pack(">III", JBD2_MAGIC, JBD2_COMMIT, seq))
    if csum:
        # h_chksum_type @ 0x0C, h_chksum_size @ 0x0D, h_chksum[0] @ 0x10
        b += bytes([JBD2_CRC32C_CHKSUM, 4, 0, 0])
        b += b"\x00" * (0x10 - len(b))
        b += b"\x00" * 4                                         # h_chksum[0]=0
        b += b"\x00" * (bs - len(b))
        struct.pack_into(">I", b, 0x10, 0)
        c = jbd_crc(bytes(b), jbd_seed())
        if bad_csum:
            c ^= 0xFFFFFFFF
        struct.pack_into(">I", b, 0x10, c)
        return bytes(b)
    return bytes(b + b"\x00" * (bs - len(b)))


def jbd_revoke(bs, blocks, csum=0):
    # r_count counts the bytes used including the 12-byte header (tail excluded
    # from r_count when csum is on, matching the kernel).
    used = 12 + 4 * len(blocks)
    body = struct.pack(">III", JBD2_MAGIC, JBD2_REVOKE, used)
    body += b"".join(struct.pack(">I", b) for b in blocks)
    assert len(body) <= bs - (4 if csum else 0)
    if csum:
        buf = bytearray(body + b"\x00" * (bs - len(body)))
        struct.pack_into(">I", buf, bs - 4, 0)
        struct.pack_into(">I", buf, bs - 4, jbd_crc(bytes(buf), jbd_seed()))
        return bytes(buf)
    return body + b"\x00" * (bs - len(body))


# ── variants ────────────────────────────────────────────────────────────────
def build_base():
    bs = 1024
    img = Ext4Image(bs=bs, total_blocks=8192,
                    incompat=INCOMPAT_FILETYPE | INCOMPAT_EXTENTS)
    files = []

    # root directory block
    root_blk = img.alloc(1)

    # hello.txt: 3000 B -> 3 extents' worth of blocks (contiguous run)
    hello = pattern("hello", 3000)
    hello_blk = img.alloc(3)
    img.write_at(hello_blk * bs, hello + b"\x00" * (3 * bs - len(hello)))
    img.put_inode(12, inode_bytes(S_IFREG | 0o644, len(hello), 1, EXTENTS_FL,
                                  extent_tree([(0, hello_blk, 3, False)]),
                                  3 * bs // 512))
    files.append(manifest_entry("\\hello.txt", "hello.txt", len(hello), hello, "normal"))

    # big.bin: 5 blocks + tail, single extent
    big = pattern("big", 5 * bs + 321)
    big_blk = img.alloc(6)
    img.write_at(big_blk * bs, big + b"\x00" * (6 * bs - len(big)))
    img.put_inode(13, inode_bytes(S_IFREG | 0o644, len(big), 1, EXTENTS_FL,
                                  extent_tree([(0, big_blk, 6, False)]),
                                  6 * bs // 512))
    files.append(manifest_entry("\\big.bin", "big.bin", len(big), big, "normal"))

    # gone.txt: deleted dirent living in the SLACK of a zero-length entry, the
    # inode is still intact (dtime set, links 0) -> the classic "deleted but
    # not yet overwritten" case.
    gone = pattern("gone", 2500)
    gone_blk = img.alloc(3)
    img.write_at(gone_blk * bs, gone + b"\x00" * (3 * bs - len(gone)))
    img.put_inode(14, inode_bytes(S_IFREG | 0o644, len(gone), 0, EXTENTS_FL,
                                  extent_tree([(0, gone_blk, 3, False)]),
                                  3 * bs // 512, dtime=1700000900))
    files.append(manifest_entry("\\gone.txt", "gone.txt", len(gone), gone, "deleted"))

    # indirect.bin: legacy (non-extent) mapping, 13 blocks -> 12 direct + 1
    # single-indirect data block.  Exercises GetIndirectBlocks().
    ind_size = 13 * bs
    indirect_payload = pattern("indirect", ind_size)
    data_blocks = [img.alloc(1) for _ in range(13)]
    for i, blk in enumerate(data_blocks):
        img.write_at(blk * bs, indirect_payload[i * bs:(i + 1) * bs])
    ind_ptr = img.alloc(1)
    img.write_block(ind_ptr, b"".join(struct.pack("<I", b) for b in data_blocks[12:]))
    iblock = b"".join(struct.pack("<I", b) for b in data_blocks[:12])
    iblock += struct.pack("<I", ind_ptr) + struct.pack("<I", 0) + struct.pack("<I", 0)
    img.put_inode(15, inode_bytes(S_IFREG | 0o644, ind_size, 1, 0, iblock,
                                  (13 + 1) * bs // 512))
    files.append(manifest_entry("\\indirect.bin", "indirect.bin", ind_size,
                                indirect_payload, "normal"))

    # orphan.bin: no dirent at all -> only reachable through ScanOrphanInodes.
    orphan = pattern("orphan", 1500)
    orphan_blk = img.alloc(2)
    img.write_at(orphan_blk * bs, orphan + b"\x00" * (2 * bs - len(orphan)))
    img.put_inode(16, inode_bytes(S_IFREG | 0o644, len(orphan), 0, EXTENTS_FL,
                                  extent_tree([(0, orphan_blk, 2, False)]),
                                  2 * bs // 512, dtime=1700000950))
    files.append(manifest_entry("\\lost+found\\inode_16", "inode_16",
                                len(orphan), orphan, "deleted"))

    # ── root directory content ──
    entries = dirent(ROOT_INO, ".", FT_DIR) + dirent(ROOT_INO, "..", FT_DIR)
    entries += dirent(12, "hello.txt", FT_REG)
    entries += dirent(13, "big.bin", FT_REG)
    entries += dirent(15, "indirect.bin", FT_REG)
    slack_off = len(entries)
    slack_len = bs - slack_off
    # zero-length slot whose rec_len swallows the rest of the block; the
    # residual dirent for gone.txt is left at slack_off + 8 (the parser starts
    # scanning slack right after the 8-byte header of this slot).
    entries += struct.pack("<IHBB", 0, slack_len, 0, 0)
    entries += dirent(14, "gone.txt", FT_REG)
    entries += b"\x00" * (bs - len(entries))
    img.write_block(root_blk, entries[:bs])

    img.put_inode(ROOT_INO, inode_bytes(S_IFDIR | 0o755, bs, 2, EXTENTS_FL,
                                        extent_tree([(0, root_blk, 1, False)]),
                                        bs // 512))
    # inode 1 = bad blocks inode, present on every real ext4
    img.put_inode(1, inode_bytes(0, 0, 1, 0, b"", 0, dtime=0))

    return img, {
        "variant": "ext4-base",
        "purpose": "extents + indirect map + dirent-slack deletion + orphan inode",
        "partitionStartSector": 0,
        "blockSize": bs,
        "clusterRatio": 1,
        "files": files,
    }


def build_uninit():
    bs = 1024
    img = Ext4Image(bs=bs, total_blocks=8192,
                    incompat=INCOMPAT_FILETYPE | INCOMPAT_EXTENTS)

    root_blk = img.alloc(1)

    # live file so the volume is not degenerate
    live = pattern("live", 1024)
    live_blk = img.alloc(1)
    img.write_at(live_blk * bs, live)
    img.put_inode(12, inode_bytes(S_IFREG | 0o644, len(live), 1, EXTENTS_FL,
                                  extent_tree([(0, live_blk, 1, False)]), bs // 512))

    # preallocated file: 2 blocks, extent marked UNINITIALIZED (ee_len = 32768+2).
    # Those blocks contain 0xAA garbage on disk; the kernel would read zeros.
    size = 2 * bs
    pre_blk = img.alloc(2)
    img.write_at(pre_blk * bs, b"\xAA" * (2 * bs))
    img.put_inode(13, inode_bytes(S_IFREG | 0o644, size, 1, EXTENTS_FL,
                                  extent_tree([(0, pre_blk, 2, True)]),
                                  2 * bs // 512))

    entries = dirent(ROOT_INO, ".", FT_DIR) + dirent(ROOT_INO, "..", FT_DIR)
    entries += dirent(12, "live.bin", FT_REG)
    entries += dirent(13, "prealloc.bin", FT_REG)
    entries += b"\x00" * (bs - len(entries))
    img.write_block(root_blk, entries[:bs])
    img.put_inode(ROOT_INO, inode_bytes(S_IFDIR | 0o755, bs, 2, EXTENTS_FL,
                                        extent_tree([(0, root_blk, 1, False)]),
                                        bs // 512))
    img.put_inode(1, inode_bytes(0, 0, 1, 0, b"", 0))

    return img, {
        "variant": "ext4-uninit",
        "purpose": "uninitialized extent must recover as zeros, not stale 0xAA bytes",
        "partitionStartSector": 0,
        "blockSize": bs,
        "clusterRatio": 1,
        "files": [
            manifest_entry("\\live.bin", "live.bin", len(live), live, "normal"),
            manifest_entry("\\prealloc.bin", "prealloc.bin", size,
                           b"\x00" * size, "normal", kind="zeros"),
        ],
    }


def build_bigalloc():
    bs = 1024
    ratio = 2                                   # 2 KiB clusters
    img = Ext4Image(bs=bs, total_blocks=8192, cluster_ratio=ratio,
                    incompat=INCOMPAT_FILETYPE | INCOMPAT_EXTENTS,
                    ro_compat=RO_COMPAT_BIGALLOC)

    root_blk = img.alloc(1)

    # span.bin: 2 clusters = 4 KiB.  ee_start / ee_len are CLUSTERS.
    size = 2 * ratio * bs
    payload = pattern("span", size)
    start_blk = img.alloc(2 * ratio)
    assert start_blk % ratio == 0, "bigalloc data must be cluster aligned"
    img.write_at(start_blk * bs, payload)
    img.put_inode(12, inode_bytes(S_IFREG | 0o644, size, 1, EXTENTS_FL,
                                  extent_tree([(0, start_blk // ratio, 2, False)]),
                                  size // 512))

    # small.bin: 100 bytes inside one cluster
    small = pattern("small", 100)
    s_blk = img.alloc(1)
    img.write_at(s_blk * bs, small)
    img.put_inode(13, inode_bytes(S_IFREG | 0o644, len(small), 1, EXTENTS_FL,
                                  extent_tree([(0, s_blk // ratio, 1, False)]),
                                  ratio * bs // 512))

    entries = dirent(ROOT_INO, ".", FT_DIR) + dirent(ROOT_INO, "..", FT_DIR)
    entries += dirent(12, "span.bin", FT_REG)
    entries += dirent(13, "small.bin", FT_REG)
    entries += b"\x00" * (bs - len(entries))
    img.write_block(root_blk, entries[:bs])
    img.put_inode(ROOT_INO, inode_bytes(S_IFDIR | 0o755, bs, 2, EXTENTS_FL,
                                        extent_tree([(0, root_blk // ratio, 1, False)]),
                                        ratio * bs // 512))
    img.put_inode(1, inode_bytes(0, 0, 1, 0, b"", 0))

    return img, {
        "variant": "ext4-bigalloc",
        "purpose": "bigalloc: extent ee_start/ee_len are clusters (ratio 2)",
        "partitionStartSector": 0,
        "blockSize": bs,
        "clusterRatio": ratio,
        "files": [
            manifest_entry("\\span.bin", "span.bin", len(payload), payload, "normal"),
            manifest_entry("\\small.bin", "small.bin", len(small), small, "normal"),
        ],
    }


def build_inline():
    bs = 1024
    inode_size = 256
    extra_isize = 32
    img = Ext4Image(bs=bs, total_blocks=8192, inode_size=inode_size,
                    incompat=INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_INLINE_DATA)

    root_blk = img.alloc(1)

    def put_inline(ino, mode, payload, links=1):
        """inline_data inode: first 60 B in i_block[], remainder in the
        system.data ibody xattr (entries start at 128+extra_isize, values grow
        down towards the 0xEA020000 magic in the last 4 bytes)."""
        assert len(payload) <= 60 + (inode_size - 4 - (128 + extra_isize) - 20), len(payload)
        head = payload[:60]
        iblock = head + b"\x00" * (60 - len(head))
        raw = bytearray(inode_bytes(mode, len(payload), links, INLINE_DATA_FL,
                                    iblock, 0, inode_size=inode_size))
        if len(payload) > 60:
            tail = payload[60:]
            value_offs = inode_size - 4 - len(tail)
            entry_off = 128 + extra_isize
            ent_size = 16 + 4                      # header + "data"
            assert value_offs >= entry_off + ent_size, (value_offs, entry_off + ent_size)
            raw[entry_off + 0] = 4                 # e_name_len
            raw[entry_off + 1] = 7                 # e_name_index = system
            struct.pack_into("<H", raw, entry_off + 2, value_offs)
            struct.pack_into("<I", raw, entry_off + 4, len(tail))
            struct.pack_into("<I", raw, entry_off + 8, 0)      # hash
            raw[entry_off + 16:entry_off + 20] = b"data"
            raw[value_offs:value_offs + len(tail)] = tail
            struct.pack_into("<I", raw, inode_size - 4, 0xEA020000)
        img.put_inode(ino, bytes(raw))

    # tiny.txt: 40 B, fully inside i_block[]
    tiny = pattern("tiny", 40)
    put_inline(12, S_IFREG | 0o644, tiny)

    # medium.txt: 120 B -> 60 B in i_block[] + 60 B in the system.data xattr.
    # (256-byte inodes leave exactly 72 B of ibody space, so 120 is the
    # realistic upper bound before ext4 converts the file to block mapping.)
    medium = pattern("medium", 120)
    put_inline(13, S_IFREG | 0o644, medium)

    # three children so the inline directory body is 88 B: 4 B parent inode +
    # 84 B of dirents, i.e. 28 B spill into the ibody xattr.
    notes = []
    for idx, (ino, nm, sz) in enumerate(((14, "note1.txt", 300),
                                         (15, "note2.txt", 200),
                                         (16, "note3.txt", 100))):
        payload = pattern(nm, sz)
        blk = img.alloc(1)
        img.write_at(blk * bs, payload + b"\x00" * (bs - len(payload)))
        img.put_inode(ino, inode_bytes(S_IFREG | 0o644, len(payload), 1, EXTENTS_FL,
                                       extent_tree([(0, blk, 1, False)]), bs // 512))
        notes.append(manifest_entry("\\indir\\" + nm, nm, len(payload), payload, "normal"))

    d = dirent(17, ".", FT_DIR) + dirent(ROOT_INO, "..", FT_DIR)
    for ino, nm, _ in ((14, "note1.txt", 0), (15, "note2.txt", 0), (16, "note3.txt", 0)):
        d += dirent(ino, nm, FT_REG)
    dir_body = struct.pack("<I", ROOT_INO) + d
    assert len(dir_body) > 60, "fixture must spill into the ibody xattr"
    put_inline(17, S_IFDIR | 0o755, dir_body, links=2)

    entries = dirent(ROOT_INO, ".", FT_DIR) + dirent(ROOT_INO, "..", FT_DIR)
    entries += dirent(12, "tiny.txt", FT_REG)
    entries += dirent(13, "medium.txt", FT_REG)
    entries += dirent(17, "indir", FT_DIR)
    entries += b"\x00" * (bs - len(entries))
    img.write_block(root_blk, entries[:bs])
    img.put_inode(ROOT_INO, inode_bytes(S_IFDIR | 0o755, bs, 3, EXTENTS_FL,
                                        extent_tree([(0, root_blk, 1, False)]),
                                        bs // 512, inode_size=inode_size))
    img.put_inode(1, inode_bytes(0, 0, 1, 0, b"", 0, inode_size=inode_size))

    return img, {
        "variant": "ext4-inline",
        "purpose": "inline_data: in-inode body, ibody xattr spill, inline directory",
        "partitionStartSector": 0,
        "blockSize": bs,
        "clusterRatio": 1,
        "files": [
            manifest_entry("\\tiny.txt", "tiny.txt", len(tiny), tiny, "normal"),
            manifest_entry("\\medium.txt", "medium.txt", len(medium), medium, "normal"),
            manifest_entry("\\indir", "indir", 0, None, "normal", kind="dir"),
        ] + notes,
    }


def build_sparse_depth():
    """Depth-1 extent tree with holes + a legacy indirect map with a hole.

    A sparse file is a sequence of extents separated by gaps.  The extent tree
    only names the allocated ranges, so the parser must synthesise zero markers
    for the gaps; otherwise every byte after the first hole shifts forward and
    the recovered file is silently wrong.
    """
    bs = 1024
    img = Ext4Image(bs=bs, total_blocks=8192,
                    incompat=INCOMPAT_FILETYPE | INCOMPAT_EXTENTS)
    root_blk = img.alloc(1)
    files = []

    # ── sparse.img: depth=1 index node -> two leaves, 3 extents, 3 holes ──
    seg = 4                      # blocks per extent
    total = 24                   # logical blocks, 6 segments of 4
    segA = pattern("sparseA", seg * bs)
    segB = pattern("sparseB", seg * bs)
    segC = pattern("sparseC", seg * bs)

    blkA = img.alloc(seg); img.write_at(blkA * bs, segA)
    blkB = img.alloc(seg); img.write_at(blkB * bs, segB)
    blkC = img.alloc(seg); img.write_at(blkC * bs, segC)

    leaf1 = img.alloc(1)
    img.write_block(leaf1, extent_tree([(0, blkA, seg, False),
                                        (8, blkB, seg, False)]))
    leaf2 = img.alloc(1)
    img.write_block(leaf2, extent_tree([(16, blkC, seg, False)]))

    idx_hdr = struct.pack("<HHHHI", EXT_MAGIC, 2, 4, 1, 0)     # eh_depth = 1
    idx_body = (struct.pack("<IIHH", 0, leaf1, 0, 0) +
                struct.pack("<IIHH", 16, leaf2, 0, 0))
    size = total * bs
    payload = (segA + b"\x00" * (seg * bs) +
               segB + b"\x00" * (seg * bs) +
               segC + b"\x00" * (seg * bs))
    assert len(payload) == size
    img.put_inode(12, inode_bytes(S_IFREG | 0o644, size, 1, EXTENTS_FL,
                                  idx_hdr + idx_body,
                                  (3 * seg + 2) * bs // 512))
    files.append(manifest_entry("\\sparse.img", "sparse.img", size, payload,
                                "normal"))

    # ── sparseind.bin: legacy indirect map, hole at logical block 5 ──
    nblocks = 13                  # 12 direct + 1 single-indirect
    raw = pattern("sparseind", nblocks * bs)
    hole_at = 5
    data_blocks = []
    for i in range(nblocks):
        if i == hole_at:
            data_blocks.append(0)             # HOLE inside the direct range
            continue
        b = img.alloc(1)
        img.write_at(b * bs, raw[i * bs:(i + 1) * bs])
        data_blocks.append(b)

    ind_ptr = img.alloc(1)
    img.write_block(ind_ptr, struct.pack("<I", data_blocks[12]))
    iblock = b"".join(struct.pack("<I", b) for b in data_blocks[:12])
    iblock += struct.pack("<I", ind_ptr) + struct.pack("<I", 0) + struct.pack("<I", 0)

    size2 = nblocks * bs
    expect = bytearray(raw)
    expect[hole_at * bs:(hole_at + 1) * bs] = b"\x00" * bs
    img.put_inode(13, inode_bytes(S_IFREG | 0o644, size2, 1, 0, iblock,
                                  (nblocks + 1) * bs // 512))
    files.append(manifest_entry("\\sparseind.bin", "sparseind.bin", size2,
                                bytes(expect), "normal"))

    # ── root directory ──
    entries = dirent(ROOT_INO, ".", FT_DIR) + dirent(ROOT_INO, "..", FT_DIR)
    entries += dirent(12, "sparse.img", FT_REG)
    entries += dirent(13, "sparseind.bin", FT_REG)
    entries += b"\x00" * (bs - len(entries))
    img.write_block(root_blk, entries[:bs])

    img.put_inode(ROOT_INO, inode_bytes(S_IFDIR | 0o755, bs, 2, EXTENTS_FL,
                                        extent_tree([(0, root_blk, 1, False)]),
                                        bs // 512))
    img.put_inode(1, inode_bytes(0, 0, 1, 0, b"", 0, dtime=0))

    return img, {
        "variant": "ext4-sparse",
        "purpose": "depth-1 extent tree with holes + legacy indirect map hole",
        "partitionStartSector": 0,
        "blockSize": bs,
        "clusterRatio": 1,
        "files": files,
    }


JOURNAL_BLOCKS = 24


def build_journal(clean=False, csum=0, corrupt=None, v1=False, torn=False):
    """JBD/JBD2 replay: metadata committed to the journal but never
    checkpointed to its home location.

    v1=True uses the original JBD descriptor (8-byte tag + inline UUID),
    which must not be mistaken for a JBD2 v2 tag.  Otherwise csum=0 legacy
    v2, 2 = CSUM_V2 (16-bit tag csum), 3 = CSUM_V3
    (32-bit tag csum).  `corrupt` may be 'tag' / 'commit' / 'desc' to poison
    exactly one checksum and prove the engine refuses the whole transaction.

    This is the crash window a recovery tool exists for.  The file
    `rescue.bin` was created, the kernel logged its inode-table block and the
    root directory block, and then the machine died before either reached its
    home block.  On disk the inode slot is zeroed and the directory has no
    entry, so without replay the file is simply gone; with replay both come
    back and the name is recovered instead of `inode_15`.

    Log contents (24-block journal, s_start = 1):

      jb[1..4]   txn 100: descriptor -> [inode-table block, dir block 0]
                          data, data, commit
      jb[5..8]   txn 101: descriptor -> [dir block 0]   (second version, adds
                          later.txt), then a REVOKE for dir block 1, commit
      jb[9..11]  txn 102: descriptor -> [dir block 1] (poison: poison.txt)
                          data, commit
      jb[12..13] txn 103: REVOKE dir block 1, commit

    Expected outcome:
      * inode table block 0  <- txn 100  -> rescue.bin exists
      * dir block 0          <- txn 101  (last copy wins) -> rescue.bin AND
                                            later.txt both carry names
      * dir block 1          <- txn 102 is suppressed by the txn-103 revoke,
                               so keep.txt survives and poison.txt must NOT
                               appear.  A replayed-but-revoked block is the one
                               way a bug here turns into a *wrong* file list.

    `clean=True` writes the identical journal but with s_start = 0, i.e. the
    log was checkpointed.  Nothing may be replayed: rescue.bin must stay gone.
    That is the guard against "just read the journal area" instead of
    "replay what s_start says is pending".
    """
    bs = 1024
    inode_size = 256
    img = Ext4Image(bs=bs, total_blocks=8192, inode_size=inode_size,
                    incompat=INCOMPAT_FILETYPE | INCOMPAT_EXTENTS)

    root_l0 = img.alloc(1)
    root_l1 = img.alloc(1)
    jblk    = img.alloc(JOURNAL_BLOCKS)

    rescue  = pattern("rescue", 2048)
    rb      = img.alloc(2)
    img.write_at(rb * bs, rescue)
    revoked = pattern("revoked", 1200)
    vb      = img.alloc(2)
    img.write_at(vb * bs, revoked + b"\x00" * (2 * bs - len(revoked)))
    keep    = pattern("keep", 500)
    kb      = img.alloc(1)
    img.write_at(kb * bs, keep + b"\x00" * (bs - len(keep)))
    later   = pattern("later", 700)
    lb      = img.alloc(1)
    img.write_at(lb * bs, later + b"\x00" * (bs - len(later)))

    # inode 15 = rescue.bin: written to the image (so the bitmaps and counts
    # stay sane) and then zeroed at home by the hook below.
    img.put_inode(15, inode_bytes(S_IFREG | 0o644, len(rescue), 1, EXTENTS_FL,
                                  extent_tree([(0, rb, 2, False)]),
                                  2 * bs // 512))
    img.put_inode(16, inode_bytes(S_IFREG | 0o644, len(revoked), 0, EXTENTS_FL,
                                  extent_tree([(0, vb, 2, False)]),
                                  2 * bs // 512, dtime=1700000900))
    img.put_inode(17, inode_bytes(S_IFREG | 0o644, len(keep), 1, EXTENTS_FL,
                                  extent_tree([(0, kb, 1, False)]), bs // 512))
    img.put_inode(18, inode_bytes(S_IFREG | 0o644, len(later), 1, EXTENTS_FL,
                                  extent_tree([(0, lb, 1, False)]), bs // 512))
    img.put_inode(8, inode_bytes(S_IFREG | 0o600, JOURNAL_BLOCKS * bs, 1, EXTENTS_FL,
                                 extent_tree([(0, jblk, JOURNAL_BLOCKS, False)]),
                                 JOURNAL_BLOCKS * bs // 512))
    img.journal_inum = 8

    # ── home state: the interrupted transaction never landed here ──────────
    img.write_block(root_l0, dirblock([(ROOT_INO, ".", FT_DIR),
                                       (ROOT_INO, "..", FT_DIR),
                                       (16, "revoked.bin", FT_REG)], bs))
    img.write_block(root_l1, dirblock([(17, "keep.txt", FT_REG)], bs))

    def zero_home_rescue(buf):
        off = img.itable_block * bs + (15 - 1) * inode_size
        buf[off:off + inode_size] = b"\x00" * inode_size
    img.hooks.append(zero_home_rescue)

    # ── journal images ─────────────────────────────────────────────────────
    itbl_blk, itbl_img = itable_block_bytes(img, 15, img.inodes_data[15])

    dir_v1 = dirblock([(ROOT_INO, ".", FT_DIR), (ROOT_INO, "..", FT_DIR),
                       (16, "revoked.bin", FT_REG),
                       (15, "rescue.bin", FT_REG)], bs)
    dir_v2 = dirblock([(ROOT_INO, ".", FT_DIR), (ROOT_INO, "..", FT_DIR),
                       (16, "revoked.bin", FT_REG),
                       (15, "rescue.bin", FT_REG),
                       (18, "later.txt", FT_REG)], bs)
    dir_poison = dirblock([(19, "poison.txt", FT_REG)], bs)

    # For csum modes, each descriptor's tag checksums are computed over the
    # matching data blocks.  'corrupt' poisons the FIRST transaction only so
    # the expected outcome is deterministic (later txns would otherwise still
    # restore dir block 0 and hide the refusal).
    def desc(seq, targets, data, first=False):
        if v1:
            return jbd_desc_v1(bs, seq, targets)
        bad_tag = (corrupt == 'tag' and first)
        bad_desc = (corrupt == 'desc' and first)
        return jbd_desc(bs, seq, targets, csum=csum,
                        data_blocks=data, bad_tchecksum=bad_tag,
                        bad_desc_csum=bad_desc)

    def commit(seq, first=False):
        return jbd_commit(bs, seq, csum=csum,
                          bad_csum=(corrupt == 'commit' and first))

    log = {
        0:  (jbd_super_v1(bs, JOURNAL_BLOCKS, 1, 100, 0 if clean else 1)
             if v1 else
             jbd_super(bs, JOURNAL_BLOCKS, 1, 100, 0 if clean else 1, csum=csum)),
        1:  desc(100, [(itbl_blk, 0), (root_l0, 0)], [itbl_img, dir_v1], first=True),
        2:  itbl_img,
        3:  dir_v1,
        4:  commit(100, first=True),
        5:  desc(101, [(root_l0, 0)], [dir_v2]),
        6:  dir_v2,
        7:  jbd_revoke(bs, [root_l1], csum=csum),
        8:  (b"\x00" * bs if torn else commit(101)),
        9:  desc(102, [(root_l1, 0)], [dir_poison]),
        10: dir_poison,
        11: commit(102),
        12: jbd_revoke(bs, [root_l1], csum=csum),
        13: commit(103),
    }
    for idx, data in log.items():
        img.write_block(jblk + idx, data)

    # ── root directory inode (2 blocks, no holes) ──────────────────────────
    img.put_inode(ROOT_INO, inode_bytes(S_IFDIR | 0o755, 2 * bs, 3, EXTENTS_FL,
                                        extent_tree([(0, root_l0, 2, False)]),
                                        2 * bs // 512, inode_size=inode_size))
    img.put_inode(1, inode_bytes(0, 0, 1, 0, b"", 0, inode_size=inode_size))

    files = []
    absent = ["poison.txt"]
    if clean:
        name = "ext4-journal-clean"
        purpose = ("journal that was cleanly checkpointed (s_start = 0): "
                   "nothing may be replayed")
        journal = {"start": 0, "appliedTransactions": 0, "overlayBlocks": 0,
                   "csum": csum}
        absent += ["rescue.bin", "later.txt"]
    elif corrupt:
        name = "ext4-journal-csum-bad"
        purpose = ("JBD2 checksum v3 journal whose first commit block checksum "
                   "is corrupted: the engine must refuse to replay ANY "
                   "transaction (a wrong overlay is worse than no replay)")
        journal = {"start": 1, "appliedTransactions": 0, "overlayBlocks": 0,
                   "csum": csum, "corrupt": corrupt}
        absent += ["rescue.bin", "later.txt"]
    elif torn:
        name = "ext4-journal-torn"
        purpose = ("synthetic power loss: transaction 101 has descriptor and "
                   "data but its commit block was never written; only txn 100 "
                   "may be replayed")
        journal = {"start": 1, "appliedTransactions": 1, "overlayBlocks": 2,
                   "csum": 0, "torn": True}
        files.append(manifest_entry("\\rescue.bin", "rescue.bin", len(rescue),
                                    rescue, "normal"))
        absent += ["later.txt"]
    elif v1:
        name = "ext4-journal-v1"
        purpose = ("JBD v1 replay: descriptor tags carry an inline UUID and "
                   "be32 flags; parsing them as JBD2 tags must be refused")
        journal = {"start": 1, "appliedTransactions": 2, "overlayBlocks": 2,
                   "csum": 0, "v1": True}
        files.append(manifest_entry("\\rescue.bin", "rescue.bin", len(rescue),
                                    rescue, "normal"))
        files.append(manifest_entry("\\later.txt", "later.txt", len(later),
                                    later, "normal"))
    elif csum:
        name = "ext4-journal-csum2" if csum == 2 else "ext4-journal-csum3"
        purpose = ("JBD2 checksum v%s journal: tag/commit/descriptor CRC32C "
                   "must verify and the same bytes replay as the legacy log"
                   % csum)
        journal = {"start": 1, "appliedTransactions": 2, "overlayBlocks": 2,
                   "csum": csum}
        files.append(manifest_entry("\\rescue.bin", "rescue.bin", len(rescue),
                                    rescue, "normal"))
        files.append(manifest_entry("\\later.txt", "later.txt", len(later),
                                    later, "normal"))
    else:
        name = "ext4-journal"
        purpose = ("JBD2 replay: inode + dirent only exist in the committed log; "
                   "a revoked block must stay at its home content")
        journal = {"start": 1, "appliedTransactions": 2, "overlayBlocks": 2,
                   "csum": 0}
        files.append(manifest_entry("\\rescue.bin", "rescue.bin", len(rescue),
                                    rescue, "normal"))
        files.append(manifest_entry("\\later.txt", "later.txt", len(later),
                                    later, "normal"))

    files.append(manifest_entry("\\keep.txt", "keep.txt", len(keep), keep, "normal"))
    files.append(manifest_entry("\\revoked.bin", "revoked.bin", len(revoked),
                                revoked, "deleted"))

    return img, {
        "variant": name,
        "purpose": purpose,
        "partitionStartSector": 0,
        "blockSize": bs,
        "clusterRatio": 1,
        "journal": journal,
        "absent": absent,
        "files": files,
    }


INCOMPAT_FLEX_BG = 0x0200


def build_flex_bg_gdt():
    """FLEX_BG + journal-replayed cross-group GDT.

    Two 4096-block groups.  Group 1's bitmaps and inode table live in group 0
    (the FLEX_BG layout); `far.txt` is inode 65, i.e. local index 0 of group 1.
    The *home* GDT deliberately points group 1 at block 0.  Only the committed
    journal block holds the correct group descriptor.  After replay the engine
    must reload the GDT before resolving inode 65; without that reload the file
    is silently lost even though its name is in the root directory.
    """
    bs = 1024
    inode_size = 256
    incompat = INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_FLEX_BG
    img = Ext4Image(bs=bs, total_blocks=8192, inodes=128, inode_size=inode_size,
                    incompat=incompat)

    root_l0 = img.alloc(1)
    jblk    = img.alloc(JOURNAL_BLOCKS)
    payload = pattern("far", 900)
    data_blk = img.alloc(1)
    img.write_at(data_blk * bs, payload + b"\x00" * (bs - len(payload)))
    # Group 1 metadata, deliberately packed into group 0 (FLEX_BG semantics).
    bmap1 = img.alloc(1)
    imap1 = img.alloc(1)
    itab1 = img.alloc(32)

    img.put_inode(65, inode_bytes(S_IFREG | 0o644, len(payload), 1, EXTENTS_FL,
                                  extent_tree([(0, data_blk, 1, False)]),
                                  bs // 512, inode_size=inode_size))
    img.put_inode(8, inode_bytes(S_IFREG | 0o600, JOURNAL_BLOCKS * bs, 1,
                                 EXTENTS_FL,
                                 extent_tree([(0, jblk, JOURNAL_BLOCKS, False)]),
                                 JOURNAL_BLOCKS * bs // 512,
                                 inode_size=inode_size))
    img.journal_inum = 8
    img.write_block(root_l0, dirblock([(ROOT_INO, ".", FT_DIR),
                                       (ROOT_INO, "..", FT_DIR),
                                       (65, "far.txt", FT_REG)], bs))
    img.put_inode(ROOT_INO, inode_bytes(S_IFDIR | 0o755, bs, 2, EXTENTS_FL,
                                        extent_tree([(0, root_l0, 1, False)]),
                                        bs // 512, inode_size=inode_size))
    img.put_inode(1, inode_bytes(0, 0, 1, 0, b"", 0, inode_size=inode_size))

    def patch(buf):
        sb = 1024
        struct.pack_into("<I", buf, sb + 0x20, 4096)   # blocks_per_group
        struct.pack_into("<I", buf, sb + 0x28, 64)     # inodes_per_group
        struct.pack_into("<I", buf, sb + 0x60, incompat)
        buf[sb + 0x174] = 1                            # 2 groups per flex

        gdt_off = img.gdt_block * bs
        home = bytearray(buf[gdt_off:gdt_off + 64])
        # group 1 descriptor in the home copy points at a block with no inode.
        struct.pack_into("<I", home, 32 + 0x08, 0)
        buf[gdt_off:gdt_off + 64] = home

        logged = bytearray(home)
        struct.pack_into("<I", logged, 0x00, img.bbitmap_block)
        struct.pack_into("<I", logged, 0x04, img.ibitmap_block)
        struct.pack_into("<I", logged, 0x08, img.itable_block)
        struct.pack_into("<I", logged, 32 + 0x00, bmap1)
        struct.pack_into("<I", logged, 32 + 0x04, imap1)
        struct.pack_into("<I", logged, 32 + 0x08, itab1)

        # Copy inode 65 into the relocated group-1 table (the table written by
        # finish() is the home copy at block 5).
        src = img.itable_block * bs + 64 * inode_size
        dst = itab1 * bs
        buf[dst:dst + inode_size] = buf[src:src + inode_size]
        buf[imap1 * bs] |= 1                           # inode 65 is allocated

        # The journal data block must be a FULL block: assigning a 64-byte
        # descriptor into a 1024-byte slice would shrink the bytearray and
        # shift everything after it.
        logged_block = bytearray(bs)
        logged_block[:64] = logged
        for idx, blk in enumerate((jbd_super(bs, JOURNAL_BLOCKS, 1, 100, 1),
                                   jbd_desc(bs, 100, [(img.gdt_block, 0)],
                                            data_blocks=[bytes(logged_block)]),
                                   bytes(logged_block),
                                   jbd_commit(bs, 100))):
            off = (jblk + idx) * bs
            buf[off:off + bs] = blk

    img.hooks.append(patch)

    return img, {
        "variant": "ext4-flex-bg-gdt",
        "purpose": ("FLEX_BG: group 1 metadata sits in group 0; the committed "
                    "journal carries the group descriptor that points at it"),
        "partitionStartSector": 0,
        "blockSize": bs,
        "clusterRatio": 1,
        "journal": {"start": 1, "appliedTransactions": 1, "overlayBlocks": 1,
                    "csum": 0, "flexBg": True},
        "absent": [],
        "files": [manifest_entry("\\far.txt", "far.txt", len(payload),
                                 payload, "normal")],
    }


def build_external_journal():
    """s_journal_dev: the committed log lives on a separate raw image.

    The volume superblock has s_journal_inum = 0 and a non-zero
    s_journal_dev; the only way to recover `rescue.bin` is to hand the engine
    the external journal image and have it match the 16-byte journal UUID.
    """
    bs = 1024
    inode_size = 256
    img = Ext4Image(bs=bs, total_blocks=8192, inode_size=inode_size,
                    incompat=INCOMPAT_FILETYPE | INCOMPAT_EXTENTS)

    root_l0 = img.alloc(1)
    rescue = pattern("extjournal", 1500)
    rb = img.alloc(2)
    img.write_at(rb * bs, rescue + b"\x00" * (2 * bs - len(rescue)))
    img.put_inode(15, inode_bytes(S_IFREG | 0o644, len(rescue), 1, EXTENTS_FL,
                                  extent_tree([(0, rb, 2, False)]),
                                  2 * bs // 512, inode_size=inode_size))
    img.put_inode(ROOT_INO, inode_bytes(S_IFDIR | 0o755, bs, 2, EXTENTS_FL,
                                        extent_tree([(0, root_l0, 1, False)]),
                                        bs // 512, inode_size=inode_size))
    img.put_inode(1, inode_bytes(0, 0, 1, 0, b"", 0, inode_size=inode_size))
    img.write_block(root_l0, dirblock([(ROOT_INO, ".", FT_DIR),
                                       (ROOT_INO, "..", FT_DIR)], bs))

    def zero_home(buf):
        off = img.itable_block * bs + 14 * inode_size
        buf[off:off + inode_size] = b"\x00" * inode_size
        sb = 1024
        buf[sb + 0xD0:sb + 0xE0] = JBD_UUID
        struct.pack_into("<I", buf, sb + 0xE0, 0)   # s_journal_inum
        struct.pack_into("<I", buf, sb + 0xE4, 1)   # s_journal_dev

    img.hooks.append(zero_home)

    itbl_blk, itbl_img = itable_block_bytes(img, 15, img.inodes_data[15])
    jimg = bytearray(bs * JOURNAL_BLOCKS)
    records = (
        jbd_super(bs, JOURNAL_BLOCKS, 1, 100, 1),
        jbd_desc(bs, 100, [(itbl_blk, 0), (root_l0, 0)],
                 data_blocks=[itbl_img,
                              dirblock([(ROOT_INO, ".", FT_DIR),
                                        (ROOT_INO, "..", FT_DIR),
                                        (15, "rescue.bin", FT_REG)], bs)]),
        itbl_img,
        dirblock([(ROOT_INO, ".", FT_DIR), (ROOT_INO, "..", FT_DIR),
                  (15, "rescue.bin", FT_REG)], bs),
        jbd_commit(bs, 100),
    )
    for idx, data in enumerate(records):
        jimg[idx * bs:(idx + 1) * bs] = data

    return img, {
        "variant": "ext4-journal-external",
        "purpose": ("external journal device: s_journal_dev + matching UUID; "
                    "replay requires an explicitly supplied journal image"),
        "partitionStartSector": 0,
        "blockSize": bs,
        "clusterRatio": 1,
        "journal": {"start": 1, "appliedTransactions": 1, "overlayBlocks": 2,
                    "csum": 0, "external": True},
        "absent": [],
        "files": [manifest_entry("\\rescue.bin", "rescue.bin", len(rescue),
                                 rescue, "normal")],
        "externalJournalImage": bytes(jimg),
    }


VARIANTS = {
    "ext4-base": build_base,
    "ext4-uninit": build_uninit,
    "ext4-bigalloc": build_bigalloc,
    "ext4-inline": build_inline,
    "ext4-sparse": build_sparse_depth,
    "ext4-journal": lambda: build_journal(False),
    "ext4-journal-v1": lambda: build_journal(False, v1=True),
    "ext4-journal-clean": lambda: build_journal(True),
    "ext4-journal-torn": lambda: build_journal(False, torn=True),
    "ext4-journal-csum2": lambda: build_journal(False, csum=2),
    "ext4-journal-csum3": lambda: build_journal(False, csum=3),
    "ext4-journal-csum-bad": lambda: build_journal(False, csum=3, corrupt='commit'),
    "ext4-flex-bg-gdt": build_flex_bg_gdt,
    "ext4-journal-external": build_external_journal,
}


def main():
    ap = argparse.ArgumentParser()
    default_out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                               "testdata", "ext4-synthetic")
    ap.add_argument("--out", default=default_out)
    ap.add_argument("--only", action="append", default=[])
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    names = args.only or list(VARIANTS)
    for name in names:
        if name not in VARIANTS:
            sys.stderr.write("unknown variant %s\n" % name)
            return 2
        img, manifest = VARIANTS[name]()
        data = img.finish()
        path = os.path.join(args.out, name + ".img")
        with open(path, "wb") as f:
            f.write(data)
        manifest["imageBytes"] = len(data)
        manifest["imageSha256"] = sha(data)
        # Some variants need a second raw image (e.g. an external journal
        # device).  Write it next to the volume and keep only its path in JSON.
        aux = manifest.pop("externalJournalImage", None)
        if aux is not None:
            aux_path = os.path.join(args.out, name + ".journal.img")
            with open(aux_path, "wb") as f:
                f.write(aux)
            manifest["externalJournalPath"] = name + ".journal.img"
        with open(os.path.join(args.out, name + ".json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=1)
        print("%-16s %9d bytes  %s" % (name, len(data), path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
