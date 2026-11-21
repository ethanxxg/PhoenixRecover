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
        self.write_at(1024, bytes(sb))
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


VARIANTS = {
    "ext4-base": build_base,
    "ext4-uninit": build_uninit,
    "ext4-bigalloc": build_bigalloc,
    "ext4-inline": build_inline,
    "ext4-sparse": build_sparse_depth,
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
        with open(os.path.join(args.out, name + ".json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=1)
        print("%-16s %9d bytes  %s" % (name, len(data), path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
