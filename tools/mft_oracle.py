#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
Independent NTFS MFT oracle for the real damaged disk (\.\PhysicalDrive2).

This is deliberately INDEPENDENT of the PhoenixRecover engine: it parses the
MFT with its own minimal implementation and reads file payload directly from
the physical disk by LCN. Its output is the ground truth used to compare
against whatever RecoverEngine produces.

READ-ONLY: the handle is GENERIC_READ only. No write is ever issued.

Usage:
  python tools/mft_oracle.py scan --boot 63                 # enumerate volume 0
  python tools/mft_oracle.py scan --boot 209728638         # enumerate volume 1
  python tools/mft_oracle.py hash --boot 63 --ids ids.txt  # ground-truth hashes
"""
import argparse
import base64
import csv
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import forensic_baseline as fb

SECTOR = 512
CHUNK = 4 * 1024 * 1024
OUT_DIR = r"D:\dev\PhoenixRecover\docs\reports\real_disk_recovery_2026-11-14"


class Disk:
    def __init__(self):
        # P0-14: when the physical disk is out of reach, run the same oracle
        # against a raw volume image. Offsets then address the image, so callers
        # pass --boot 0 for a volume dump (image byte 0 == volume boot sector).
        img = os.environ.get("PHOENIX_ORACLE_IMAGE", "")
        if img:
            self.ident = {"image": img, "mode": "image"}
            self.handle = fb.k32.CreateFileW(
                img, fb.GENERIC_READ, fb.SHARE_RW, None, 3, 0, None)
            if self.handle is None or self.handle == fb.INVALID:
                raise RuntimeError("CreateFileW(%s) failed err=%d" %
                                   (img, fb.ctypes.get_last_error()))
            return
        self.ident = fb.disk_identity()
        self.handle = fb.k32.CreateFileW(
            fb.DEVICE, fb.GENERIC_READ, fb.SHARE_RW, None, 3, 0, None)
        if self.handle is None or self.handle == fb.INVALID:
            raise RuntimeError("CreateFileW(GENERIC_READ) failed err=%d" %
                               fb.ctypes.get_last_error())

    def close(self):
        fb.k32.CloseHandle(self.handle)

    def read(self, byte_offset, size):
        """Aligned read; returns bytes or raises. size must be sector-multiple."""
        assert byte_offset % SECTOR == 0 and size % SECTOR == 0
        out = bytearray()
        pos = 0
        while pos < size:
            n = min(CHUNK, size - pos)
            data, err = fb.read_at(self.handle, byte_offset + pos, n)
            if data is None:
                raise IOError("read failed at byte %d err=%s" %
                              (byte_offset + pos, err))
            out += data
            pos += n
        return bytes(out)


class WindowReader:
    """Sequential-friendly read cache.

    Files within a batch are processed in first-LCN order, so a 32 MiB sliding
    window turns hundreds of thousands of tiny 4 KiB reads into a handful of
    large sequential reads. Essential on a USB HDD where every seek costs ~10 ms.
    """
    WINDOW = 32 * 1024 * 1024

    def __init__(self, disk):
        self.disk = disk
        self.base = 0
        self.buf = b""

    def read(self, byte_offset, size):
        if size <= 0:
            return b""
        if (byte_offset >= self.base and
                byte_offset + size <= self.base + len(self.buf)):
            i = byte_offset - self.base
            return self.buf[i:i + size]
        self.base = (byte_offset // SECTOR) * SECTOR
        want = max(self.WINDOW,
                   ((size + SECTOR - 1) // SECTOR) * SECTOR)
        self.buf = self.disk.read(self.base, want)
        return self.buf[byte_offset - self.base:byte_offset - self.base + size]


def decode_runlist(buf, pos, end):
    """NTFS data-run list -> [(lcn, clusterCount), ...]; lcn=0 means sparse."""
    runs = []
    lcn = 0
    while pos < end:
        hdr = buf[pos]
        if hdr == 0:
            break
        len_bytes = hdr & 0x0F
        off_bytes = hdr >> 4
        pos += 1
        if len_bytes == 0 or pos + len_bytes > end:
            break
        length = int.from_bytes(buf[pos:pos + len_bytes], "little")
        pos += len_bytes
        if off_bytes == 0:
            runs.append((0, length))
            continue
        if pos + off_bytes > end:
            break
        delta = int.from_bytes(buf[pos:pos + off_bytes], "little", signed=True)
        pos += off_bytes
        lcn += delta
        runs.append((lcn, length))
    return runs


def apply_usa(rec, record_size):
    """Apply the Update Sequence Array fix-ups to one MFT record."""
    try:
        usa_off = struct.unpack_from("<H", rec, 0x04)[0]
        usa_cnt = struct.unpack_from("<H", rec, 0x06)[0]
    except struct.error:
        return rec
    if not (0 < usa_off < record_size - 2 and 1 < usa_cnt <= (record_size // SECTOR) + 1):
        return rec
    buf = bytearray(rec)
    try:
        usn = struct.unpack_from("<H", buf, usa_off)[0]
        for i in range(usa_cnt - 1):
            sector_end = (i + 1) * SECTOR - 2
            if sector_end + 2 > record_size or usa_off + 2 + 2 * i + 2 > record_size:
                break
            replacement = struct.unpack_from("<H", buf, usa_off + 2 + 2 * i)[0]
            if struct.unpack_from("<H", buf, sector_end)[0] != usn:
                # Mismatched USN: record may be damaged; still apply best effort.
                pass
            struct.pack_into("<H", buf, sector_end, replacement)
    except struct.error:
        return rec
    return bytes(buf)


def parse_record(raw, record_size):
    """Return a dict describing one MFT record, or None."""
    if len(raw) < record_size or raw[:4] != b"FILE":
        return None
    rec = apply_usa(raw, record_size)
    try:
        flags = struct.unpack_from("<H", rec, 0x16)[0]
        attr_off = struct.unpack_from("<H", rec, 0x14)[0]
    except struct.error:
        return None
    if attr_off < 0x18 or attr_off >= record_size:
        return None

    names = []
    data_runs = None
    data_size = 0
    resident = None
    compressed = False
    compression_unit = 0
    ads_names = []

    pos = attr_off
    while pos + 16 <= record_size:
        try:
            atype = struct.unpack_from("<I", rec, pos)[0]
        except struct.error:
            break
        if atype == 0xFFFFFFFF or atype == 0:
            break
        try:
            alen = struct.unpack_from("<I", rec, pos + 4)[0]
        except struct.error:
            break
        if alen < 16 or pos + alen > record_size:
            break
        non_resident = rec[pos + 8]

        if atype == 0x30 and not non_resident:  # $FILE_NAME
            try:
                vlen = struct.unpack_from("<I", rec, pos + 0x10)[0]
                voff = struct.unpack_from("<H", rec, pos + 0x14)[0]
            except struct.error:
                pos += alen
                continue
            v = rec[pos + voff:pos + voff + vlen]
            if len(v) >= 0x42:
                try:
                    parent = struct.unpack_from("<Q", v, 0x00)[0] & 0xFFFFFFFFFFFF
                    name_len = v[0x40]
                    namespace = v[0x41]
                    raw_name = v[0x42:0x42 + name_len * 2]
                    name = raw_name.decode("utf-16-le", errors="replace")
                    if name:
                        names.append((namespace, name, parent))
                except (struct.error, UnicodeDecodeError):
                    pass

        elif atype == 0x80:  # $DATA
            # P0-5: a NAMED $DATA attribute is an alternate data stream (ADS),
            # e.g. "Zone.Identifier" on downloaded files. It is NOT the default
            # stream and must never override size / payload / runlist.
            # The old parser ignored nameLength, so a 26-byte resident ADS
            # replaced the real non-resident $DATA, and a second non-resident
            # ADS overwrote realSize. Both produced FALSE engine mismatches on
            # the real disk (batch_003): oracle expected 26..277 bytes while
            # the engine correctly recovered the full unnamed $DATA.
            name_len = rec[pos + 9]
            if name_len:
                name_off = struct.unpack_from("<H", rec, pos + 0x0A)[0]
                raw_name = rec[pos + name_off:pos + name_off + name_len * 2]
                ads_names.append(raw_name.decode("utf-16-le", errors="replace"))
                pos += alen
                continue
            if non_resident:
                try:
                    run_off = struct.unpack_from("<H", rec, pos + 0x20)[0]
                    comp_unit = struct.unpack_from("<H", rec, pos + 0x22)[0]
                    attr_flags = struct.unpack_from("<H", rec, pos + 0x0C)[0]
                    real_size = struct.unpack_from("<Q", rec, pos + 0x30)[0]
                except struct.error:
                    pos += alen
                    continue
                if data_runs is None:  # first UNNAMED $DATA only
                    if run_off >= alen:
                        pos += alen
                        continue
                    data_runs = decode_runlist(rec, pos + run_off, pos + alen)
                    data_size = real_size
                    compressed = bool(attr_flags & 0x0001)
                    compression_unit = comp_unit
            else:
                try:
                    vlen = struct.unpack_from("<I", rec, pos + 0x10)[0]
                    voff = struct.unpack_from("<H", rec, pos + 0x14)[0]
                except struct.error:
                    pos += alen
                    continue
                if resident is None and data_runs is None:
                    if voff + vlen > alen:
                        pos += alen
                        continue
                    resident = rec[pos + voff:pos + voff + vlen]
                    data_size = len(resident)

        pos += alen

    if not names:
        return None
    # Prefer Win32 / Win32&DOS namespaces over DOS short names.
    best = sorted(names, key=lambda t: (0 if t[0] in (1, 3) else 1, -len(t[1])))[0]
    return {
        "name": best[1], "parent": best[2],
        "inUse": bool(flags & 0x01), "isDir": bool(flags & 0x02),
        "runs": data_runs or [], "size": data_size,
        "resident": resident, "compressed": compressed,
        "compressionUnit": compression_unit,
        "adsNames": ads_names,
    }


class Volume:
    def __init__(self, disk, boot_lba):
        self.disk = disk
        self.win = WindowReader(disk)
        self.bootLBA = boot_lba
        boot = disk.read(boot_lba * SECTOR, SECTOR)
        if boot[3:11] != b"NTFS    ":
            raise RuntimeError("LBA %d is not an NTFS boot sector" % boot_lba)
        self.bps = struct.unpack_from("<H", boot, 0x0B)[0]
        self.spc = boot[0x0D]
        self.bytesPerCluster = self.bps * self.spc
        self.totalSectors = struct.unpack_from("<Q", boot, 0x28)[0]
        self.mftLCN = struct.unpack_from("<Q", boot, 0x30)[0]
        cpr = struct.unpack_from("<b", boot, 0x40)[0]
        self.recordSize = (1 << (-cpr)) if cpr < 0 else cpr * self.bytesPerCluster
        if not (256 <= self.recordSize <= 65536):
            raise RuntimeError("bad MFT record size %d" % self.recordSize)
        self.mftLBA = boot_lba + self.mftLCN * self.spc

    def lcn_to_byte(self, lcn):
        return (self.bootLBA + lcn * self.spc) * SECTOR

    def init_mft_runs(self):
        if getattr(self, "mftRuns", None):
            return self.mftRuns
        rec0_raw = self.disk.read(self.mftLBA * SECTOR, max(SECTOR, self.recordSize))
        rec0 = parse_record(rec0_raw, self.recordSize)
        if rec0 is None or not rec0["runs"]:
            raise RuntimeError("cannot read $MFT record 0 / its data runs")
        self.mftRuns = rec0["runs"]
        return self.mftRuns

    def record_byte_offset(self, idx):
        """Map an MFT record index to an absolute disk byte offset."""
        runs = self.init_mft_runs()
        off = idx * self.recordSize
        for lcn, nclus in runs:
            nbytes = nclus * self.bytesPerCluster
            if off < nbytes:
                return None if lcn == 0 else self.lcn_to_byte(lcn) + off
            off -= nbytes
        return None

    def iter_mft_records(self):
        self.init_mft_runs()
        idx = 0
        for lcn, nclus in self.mftRuns:
            nbytes = nclus * self.bytesPerCluster
            if lcn == 0:  # sparse
                idx += nbytes // self.recordSize
                continue
            base = self.lcn_to_byte(lcn)
            for off in range(0, nbytes, CHUNK):
                n = min(CHUNK, nbytes - off)
                data = self.disk.read(base + off, n)
                for i in range(0, n, self.recordSize):
                    yield idx, data[i:i + self.recordSize]
                    idx += 1

    def read_file_payload(self, runs, size):
        """Read logical file bytes directly from disk, independently of engine."""
        out = bytearray()
        remaining = size
        for lcn, nclus in runs:
            if remaining <= 0:
                break
            nbytes = nclus * self.bytesPerCluster
            take = min(nbytes, remaining)
            if lcn == 0:  # sparse hole
                out += b"\x00" * take
            else:
                data = self.win.read(self.lcn_to_byte(lcn), take)
                out += data
            remaining -= take
        return bytes(out[:size])


def encode_runs(runs):
    return ";".join("%d:%d" % (l, n) for l, n in runs)


def decode_runs(s):
    out = []
    for part in s.split(";"):
        if not part:
            continue
        a, b = part.split(":")
        out.append((int(a), int(b)))
    return out


def wname(s):
    return base64.b64encode(s.encode("utf-8", errors="replace")).decode("ascii")


def scan(args):
    disk = Disk()
    try:
        for boot_lba in args.boot:
            vol = Volume(disk, boot_lba)
            print("=== volume bootLBA=%d bps=%d spc=%d bytesPerCluster=%d "
                  "totalSectors=%d mftLBA=%d recordSize=%d ===" %
                  (boot_lba, vol.bps, vol.spc, vol.bytesPerCluster,
                   vol.totalSectors, vol.mftLBA, vol.recordSize))
            records = {}
            order = []
            for idx, raw in vol.iter_mft_records():
                info = parse_record(raw, vol.recordSize)
                if info is None:
                    continue
                records[idx] = info
                order.append(idx)
            print("parsed %d MFT records" % len(records))

            def resolve_path(idx):
                seen = set()
                parts = []
                cur = idx
                while cur in records and cur not in seen and len(parts) < 64:
                    seen.add(cur)
                    r = records[cur]
                    parts.append(r["name"])
                    p = r["parent"]
                    if p == cur or p == 5 or p not in records:
                        break
                    cur = p
                return "\\" + "\\".join(reversed(parts))

            csv_path = os.path.join(OUT_DIR, "candidates_vol%d.csv" % boot_lba)
            files = []
            with open(csv_path, "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(["id", "volumeBootLBA", "name", "path", "size",
                            "isDir", "inUse", "resident", "compressed",
                            "compressionUnit", "runs", "firstLCN", "ext",
                            "isSystem", "residentHex"])
                system_count = 0
                system_bytes = 0
                for idx in order:
                    r = records[idx]
                    path = resolve_path(idx)
                    ext = os.path.splitext(r["name"])[1].lstrip(".").lower()
                    first = r["runs"][0][0] if r["runs"] else -1
                    # NTFS system files ($MFT, $BadClus:$Bad, ...) are not user
                    # data. $BadClus:$Bad is a volume-sized sparse stream and
                    # would otherwise dominate the batch budget with zeros.
                    is_system = idx < 16 or r["name"].startswith("$")
                    w.writerow([
                        idx, boot_lba, wname(r["name"]), wname(path), r["size"],
                        int(r["isDir"]), int(r["inUse"]),
                        int(r["resident"] is not None), int(r["compressed"]),
                        r["compressionUnit"], encode_runs(r["runs"]), first, ext,
                        int(is_system),
                        r["resident"].hex() if r["resident"] is not None else ""])
                    if not r["isDir"] and r["size"] > 0:
                        if is_system:
                            system_count += 1
                            system_bytes += r["size"]
                        else:
                            files.append({"id": idx, "size": r["size"],
                                          "inUse": r["inUse"],
                                          "resident": r["resident"] is not None,
                                          "compressed": r["compressed"],
                                          "runs": len(r["runs"])})

            summary = {
                "volumeBootLBA": boot_lba,
                "geometry": {"bytesPerCluster": vol.bytesPerCluster,
                             "mftLBA": vol.mftLBA,
                             "recordSize": vol.recordSize,
                             "totalSectors": vol.totalSectors},
                "mftRuns": encode_runs(vol.mftRuns),
                "recordsParsed": len(records),
                "recoverableFileCount": len(files),
                "recoverableBytes": sum(x["size"] for x in files),
                "excludedSystemFiles": system_count,
                "excludedSystemBytes": system_bytes,
                "inUseFiles": sum(1 for x in files if x["inUse"]),
                "deletedFiles": sum(1 for x in files if not x["inUse"]),
                "residentFiles": sum(1 for x in files if x["resident"]),
                "compressedFiles": sum(1 for x in files if x["compressed"]),
                "fragmentedFiles": sum(1 for x in files if x["runs"] > 1),
                "maxFileBytes": max((x["size"] for x in files), default=0),
                "deviceIdentity": disk.ident,
            }
            sp = os.path.join(OUT_DIR, "oracle_summary_vol%d.json" % boot_lba)
            with open(sp, "w", encoding="utf-8") as f:
                json.dump(summary, f, indent=2, ensure_ascii=False)
            print(json.dumps(summary, indent=2, ensure_ascii=False))
            print("-> %s" % csv_path)
    finally:
        disk.close()


def hash_ids(args):
    disk = Disk()
    try:
        vol = Volume(disk, args.boot[0])
        csv_path = os.path.join(OUT_DIR, "candidates_vol%d.csv" % args.boot[0])
        want = set()
        with open(args.ids, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    want.add(int(line))
        out = {}
        matched = []
        with open(csv_path, "r", newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                if int(row["id"]) in want:
                    matched.append(row)
        # Process in first-LCN order so the window cache yields sequential I/O.
        # Resident files have no LCN and are hashed last (no disk read needed).
        matched.sort(key=lambda r: (int(r.get("firstLCN") or -1) < 0,
                                    int(r.get("firstLCN") or 0)))
        for row in matched:
            rid = int(row["id"])
            size = int(row["size"])
            resident = row["resident"] == "1"
            runs = decode_runs(row["runs"])
            if resident:
                # `candidates_vol*.csv` has NO residentHex column (it was
                # dropped when the ADS-safe parser replaced the old scan
                # output). Trusting that column made EVERY resident file hash
                # as b"" -> e3b0c442... and produced 133,902 false mismatches
                # in batch 7, whose id set is dominated by small resident
                # system files. Re-parse the MFT record from the read-only
                # source disk instead, exactly like `verify-ids` does.
                pos = vol.record_byte_offset(rid)
                if pos is None:
                    out[str(rid)] = {"error": "MFT record sparse or out of range"}
                    continue
                info = parse_record(disk.read(pos, vol.recordSize),
                                    vol.recordSize)
                if info is None or info["resident"] is None:
                    out[str(rid)] = {"error": "resident $DATA not found on disk"}
                    continue
                payload = info["resident"]
            else:
                try:
                    payload = vol.read_file_payload(runs, size)
                except IOError as e:
                    out[str(rid)] = {"error": str(e)}
                    continue
            out[str(rid)] = {"sha256": hashlib.sha256(payload).hexdigest(),
                             "bytes": len(payload), "expectedSize": size}
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2)
        print("hashed %d ids -> %s" % (len(out), args.out))
    finally:
        disk.close()


def verify_ids(args):
    """Recompute truth DIRECTLY from each MFT record on disk.

    Unlike `hash`, this never reads the (possibly stale) candidate CSV: it
    re-parses every record with the ADS-safe parser. Usable to audit a
    mismatch even after the staged recovered files have been released.
    """
    disk = Disk()
    try:
        vol = Volume(disk, args.boot[0])
        want = []
        with open(args.ids, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    want.append(int(line))
        out = {}
        for rid in want:
            pos = vol.record_byte_offset(rid)
            if pos is None:
                out[str(rid)] = {"error": "MFT record sparse or out of range"}
                continue
            info = parse_record(disk.read(pos, vol.recordSize), vol.recordSize)
            if info is None:
                out[str(rid)] = {"error": "unparsable MFT record"}
                continue
            if info["resident"] is not None:
                payload = info["resident"]
                kind = "resident"
            else:
                kind = "non-resident"
                try:
                    payload = vol.read_file_payload(info["runs"], info["size"])
                except IOError as e:
                    out[str(rid)] = {"error": str(e)}
                    continue
            out[str(rid)] = {
                "sha256": hashlib.sha256(payload).hexdigest(),
                "bytes": len(payload), "expectedSize": info["size"],
                "streamKind": kind, "runCount": len(info["runs"]),
                "adsSkipped": info["adsNames"],
            }
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=2, ensure_ascii=False)
        print("verified %d ids -> %s" % (len(out), args.out))
    finally:
        disk.close()


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("--boot", type=int, nargs="+", required=True)
    s.set_defaults(func=scan)
    h = sub.add_parser("hash")
    h.add_argument("--boot", type=int, nargs=1, required=True)
    h.add_argument("--ids", required=True)
    h.add_argument("--out", required=True)
    h.set_defaults(func=hash_ids)
    v = sub.add_parser("verify-ids")
    v.add_argument("--boot", type=int, nargs=1, required=True)
    v.add_argument("--ids", required=True)
    v.add_argument("--out", required=True)
    v.set_defaults(func=verify_ids)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
