#!/usr/bin/env python3
"""Quick exFAT deleted-file inventory for DFR images (diagnostic only)."""
import struct
import sys
from pathlib import Path

EXFAT_ENTRY_EOD = 0x00
EXFAT_ENTRY_FILE = 0x85
EXFAT_ENTRY_STREAM_EXT = 0xC0
EXFAT_ENTRY_FILE_NAME = 0xC1


def read_boot(f, part_sector):
    f.seek(part_sector * 512)
    b = f.read(512)
    if b[3:11] != b"EXFAT   ":
        raise ValueError("not exFAT")
    return {
        "part": part_sector,
        "bps": 1 << b[108],
        "spc": 1 << b[109],
        "bpc": (1 << b[108]) * (1 << b[109]),
        "fat_off": struct.unpack_from("<I", b, 80)[0],
        "heap": struct.unpack_from("<I", b, 88)[0],
        "clusters": struct.unpack_from("<I", b, 92)[0],
        "root": struct.unpack_from("<I", b, 96)[0],
    }


def next_cl(f, vol, cl):
    off = (vol["part"] + vol["fat_off"]) * 512 + cl * 4
    f.seek(off)
    return struct.unpack("<I", f.read(4))[0]


def cl_byte_off(vol, cl):
    return (vol["part"] + vol["heap"] + (cl - 2) * vol["spc"]) * 512


def walk_dir(f, vol, cl, path, depth=0, visited=None):
    if visited is None:
        visited = set()
    if depth > 8 or cl < 2 or cl >= 0xFFFFFFF7 or cl in visited:
        return
    visited.add(cl)

    f.seek(cl_byte_off(vol, cl))
    data = f.read(vol["bpc"])
    o = 0
    while o + 32 <= len(data):
        et = data[o]
        if et == EXFAT_ENTRY_EOD:
            break
        if et in (0x85, 0x05):
            sc = data[o + 1]
            if o + 32 * (1 + sc) > len(data):
                break
            se = data[o + 32 : o + 64]
            if se[0] not in (0xC0, 0x40):
                o += 32 * (1 + sc)
                continue
            flags = se[1]
            nofat = flags & 1
            first = struct.unpack_from("<I", se, 20)[0]
            dlen = struct.unpack_from("<Q", se, 24)[0]
            ccnt = (dlen + vol["bpc"] - 1) // vol["bpc"] if dlen else 0
            attrs = struct.unpack_from("<H", data, o + 4)[0]
            is_dir = bool(attrs & 0x10)
            name = ""
            for i in range(2, sc + 1):
                ent = data[o + 32 * i : o + 32 * (i + 1)]
                if ent[0] not in (0xC1, 0x41):
                    break
                for j in range(2, 32, 2):
                    c = struct.unpack_from("<H", ent, j)[0]
                    if c == 0:
                        break
                    name += chr(c)
            deleted = et == 0x05
            full = path + name + ("\\" if is_dir else "")
            if deleted:
                print(
                    f"  DEL depth={depth} dir={is_dir} {full!r} size={dlen} "
                    f"first={first} nofat={nofat} need={ccnt}"
                )
            if is_dir and first >= 2:
                walk_dir(f, vol, first, full, depth + 1, visited)
            o += 32 * (1 + sc)
        else:
            o += 32

    nc = next_cl(f, vol, cl)
    if 2 <= nc < 0xFFFFFFF8 and nc not in visited:
        walk_dir(f, vol, nc, path, depth, visited)


def main():
    path = Path(sys.argv[1])
    part = int(sys.argv[2]) if len(sys.argv) > 2 else 2048
    print(path.name, "partition@", part)
    with path.open("rb") as f:
        vol = read_boot(f, part)
        print("root=", vol["root"], "bpc=", vol["bpc"])
        walk_dir(f, vol, vol["root"], "\\")


if __name__ == "__main__":
    main()
