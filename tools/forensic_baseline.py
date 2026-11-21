#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
P0-1 forensic baseline for the real damaged disk (\\.\PhysicalDrive2).

HARD RULES (do not weaken):
  * The source disk handle is opened with GENERIC_READ ONLY.
    GENERIC_WRITE is never requested, no write IOCTL is ever issued.
  * No chkdsk / diskpart import / initialize / convert-to-basic / mount RW.
  * SMART raw IOCTLs are SKIPPED on purpose: they need GENERIC_READ|GENERIC_WRITE.
  * Device identity is re-checked on every run; any mismatch aborts immediately.

Usage:
    python tools/forensic_baseline.py            # capture -> baseline.json
    python tools/forensic_baseline.py --verify   # re-capture and compare to baseline.json
"""
import base64
import ctypes
import datetime
import hashlib
import json
import os
import struct
import subprocess
import sys
from ctypes import wintypes

DEVICE = r"\\.\PhysicalDrive2"
DISK_INDEX = 2
EXPECT_SERIAL = "0AD132601218"
EXPECT_SIZE = 1000202273280
OUT_DIR = r"D:\dev\PhoenixRecover\docs\reports\real_disk_recovery_2026-11-14"
BASELINE = os.path.join(OUT_DIR, "baseline.json")

GENERIC_READ = 0x80000000
SHARE_RW = 0x1 | 0x2
OPEN_EXISTING = 3
FILE_BEGIN = 0
INVALID = ctypes.c_void_p(-1).value
IOCTL_DISK_GET_LENGTH_INFO = 0x0007405C
SECTOR = 512

SIGS = {
    b"PRIVHEAD": "LDM_PRIVHEAD",
    b"TOCBLOCK": "LDM_TOCBLOCK",
    b"VMDB": "LDM_VMDB",
    b"VBLK": "LDM_VBLK",
    b"EFI PART": "GPT_HEADER",
    b"EXFAT   ": "EXFAT_BOOT",
    b"NTFS    ": "NTFS_BOOT",
    b"FAT32   ": "FAT32_BOOT",
    b"FAT16   ": "FAT16_BOOT",
    b"MSDOS5.0": "FAT_BOOT",
    b"MSWIN4.1": "FAT_BOOT",
}

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
adv = ctypes.WinDLL("advapi32", use_last_error=True)
k32.GetCurrentProcess.restype = wintypes.HANDLE
k32.CreateFileW.restype = wintypes.HANDLE
k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                            wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
k32.ReadFile.restype = wintypes.BOOL
k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                         ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.SetFilePointerEx.restype = wintypes.BOOL
k32.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong,
                                 ctypes.POINTER(ctypes.c_longlong), wintypes.DWORD]
k32.DeviceIoControl.restype = wintypes.BOOL
k32.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID,
                                wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
k32.CloseHandle.argtypes = [wintypes.HANDLE]
adv.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                 ctypes.POINTER(wintypes.HANDLE)]
adv.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID,
                                    wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]


def die(msg, code=2):
    print("ABORT: " + msg)
    sys.exit(code)


def token_state():
    h = wintypes.HANDLE()
    if not adv.OpenProcessToken(k32.GetCurrentProcess(), 0x0008, ctypes.byref(h)):
        die("cannot query process token (err=%d)" % ctypes.get_last_error())
    buf = ctypes.create_string_buffer(4)
    ret = wintypes.DWORD(0)
    adv.GetTokenInformation(h, 18, buf, 4, ctypes.byref(ret))
    et = struct.unpack_from("<I", buf)[0]
    adv.GetTokenInformation(h, 20, buf, 4, ctypes.byref(ret))
    ev = struct.unpack_from("<I", buf)[0]
    k32.CloseHandle(h)
    return {"isElevated": ev, "elevationType": et}


def disk_identity():
    """Bind to a *specific* physical device; abort on any drift."""
    ps = ("Get-CimInstance Win32_DiskDrive | Where-Object { $_.Index -eq %d } | "
          "Select-Object Index,DeviceID,Model,SerialNumber,Size,Partitions,Status,InterfaceType | "
          "ConvertTo-Json -Compress" % DISK_INDEX)
    raw = subprocess.check_output(["powershell.exe", "-NoProfile", "-Command", ps], text=True)
    d = json.loads(raw)
    if isinstance(d, list):
        d = d[0] if d else {}
    serial = str(d.get("SerialNumber", "")).strip()
    size = int(d.get("Size", 0))
    if serial != EXPECT_SERIAL or size != EXPECT_SIZE:
        die("device identity drift: serial=%r size=%r (expected %r / %r)" %
            (serial, size, EXPECT_SERIAL, EXPECT_SIZE))
    return {
        "index": d.get("Index"), "deviceId": d.get("DeviceID"),
        "model": d.get("Model"), "serialNumber": serial, "sizeBytes": size,
        "partitions": d.get("Partitions"), "status": d.get("Status"),
        "interfaceType": d.get("InterfaceType"),
    }


def read_at(handle, offset, size):
    """Aligned synchronous read. Returns (bytes|None, err)."""
    newpos = ctypes.c_longlong(0)
    if not k32.SetFilePointerEx(handle, ctypes.c_longlong(offset),
                                ctypes.byref(newpos), FILE_BEGIN):
        return None, ctypes.get_last_error()
    if newpos.value != offset:
        return None, -1
    buf = ctypes.create_string_buffer(size)
    got = wintypes.DWORD(0)
    ctypes.set_last_error(0)
    ok = k32.ReadFile(handle, buf, size, ctypes.byref(got), None)
    err = ctypes.get_last_error()
    if not ok:
        return None, err
    return buf.raw[:got.value], err


def hits(data):
    out = []
    for sig, label in SIGS.items():
        pos = 0
        while True:
            i = data.find(sig, pos)
            if i < 0:
                break
            out.append({"sig": label, "offsetInSample": i})
            pos = i + 1
            if len(out) > 40:
                return out
    return out


def capture():
    tok = token_state()
    if not tok["isElevated"]:
        die("process is NOT elevated (token=%r). Read-only physical-disk access "
            "requires an elevated token." % tok)

    ident = disk_identity()

    handle = k32.CreateFileW(DEVICE, GENERIC_READ, SHARE_RW, None, OPEN_EXISTING, 0, None)
    if handle is None or handle == INVALID:
        die("CreateFileW(GENERIC_READ) failed err=%d" % ctypes.get_last_error())

    length_buf = ctypes.create_string_buffer(8)
    bret = wintypes.DWORD(0)
    if not k32.DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, None, 0,
                               length_buf, 8, ctypes.byref(bret), None):
        die("IOCTL_DISK_GET_LENGTH_INFO failed err=%d" % ctypes.get_last_error())
    total = struct.unpack_from("<Q", length_buf)[0]
    total_sectors = total // SECTOR

    samples = []

    def sample(name, lba, size, keep=0):
        off = lba * SECTOR
        data, err = read_at(handle, off, size)
        rec = {"name": name, "lba": lba, "offsetBytes": off, "requestedSize": size}
        if data is None:
            rec.update({"readError": err, "readOk": False, "sha256": None})
            print("[WARN] %-24s LBA=%-12d READ FAILED err=%s" % (name, lba, err))
        else:
            rec.update({
                "readOk": True, "actualSize": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "head64Hex": data[:64].hex(),
                "signatures": hits(data),
            })
            if keep:
                rec["base64"] = base64.b64encode(data[:keep]).decode("ascii")
            print("[ OK ] %-24s LBA=%-12d %-8d bytes sha256=%s" %
                  (name, lba, len(data), rec["sha256"][:16]))
        samples.append(rec)
        return data

    mbr = sample("LBA0_MBR", 0, SECTOR, keep=SECTOR)
    sample("LBA1_GPT_or_PRIVHEAD", 1, SECTOR, keep=SECTOR)
    sample("LBA2_GPT_entries", 2, 8 * SECTOR, keep=512)
    sample("HEAD_64_SECTORS", 0, 64 * SECTOR, keep=SECTOR)

    tail_bytes = 1024 * 1024
    tail_lba = total_sectors - tail_bytes // SECTOR
    sample("TAIL_1MiB_LDM_DB", tail_lba, tail_bytes, keep=SECTOR * 8)

    mbr_parts = []
    if mbr and len(mbr) >= 512:
        for i in range(4):
            e = mbr[446 + 16 * i:446 + 16 * i + 16]
            typ = e[4]
            start = struct.unpack_from("<I", e, 8)[0]
            count = struct.unpack_from("<I", e, 12)[0]
            mbr_parts.append({"index": i, "type": "0x%02x" % typ,
                              "startLBA": start, "sectorCount": count,
                              "sizeGiB": round(count * SECTOR / 1024.0 ** 3, 3)})
            if typ and count:
                sample("PART%d_BOOT_LBA%d" % (i, start), start, 8 * SECTOR, keep=SECTOR)

    if mbr and len(mbr) >= 512 and mbr[510:512] == b"\x55\xaa":
        mbr_valid = True
    else:
        mbr_valid = False

    k32.CloseHandle(handle)

    all_sigs = sorted({h["sig"] for s in samples for h in s.get("signatures", [])})

    # Damage-layer assessment (evidence based, not speculation).
    assessment = []
    if mbr_valid:
        assessment.append("MBR signature 0x55AA valid -> physical sector 0 readable, "
                          "partition table NOT wiped")
    else:
        assessment.append("MBR signature invalid -> boot/partition layer damaged")
    if any(p["type"] == "0x42" for p in mbr_parts):
        assessment.append("0x42 LDM (Windows dynamic disk) partitions present -> "
                          "volume layer depends on the LDM database, not just the MBR")
    if "LDM_PRIVHEAD" in all_sigs or "LDM_TOCBLOCK" in all_sigs:
        assessment.append("LDM private header / TOC block found -> LDM database still on disk")
    else:
        assessment.append("NO LDM PRIVHEAD/TOCBLOCK signature in sampled regions -> "
                          "LDM database may be damaged or located elsewhere")
    if any(s.startswith("NTFS") or s.startswith("FAT") or s.startswith("EXFAT")
           for s in all_sigs):
        assessment.append("filesystem boot signature found in sampled regions")

    return {
        "schema": "phoenix.forensic.baseline/v1",
        "generatedAtUtc": datetime.datetime.now(datetime.timezone.utc)
                          .isoformat().replace("+00:00", "Z"),
        "target": DEVICE,
        "deviceIdentity": ident,
        "token": tok,
        "access": {
            "desiredAccess": "GENERIC_READ",
            "shareMode": "FILE_SHARE_READ|FILE_SHARE_WRITE",
            "genericWriteRequested": False,
            "writeOperationsPerformed": 0,
            "smartRawIoctlSkipped": True,
            "smartSkipReason": "raw SMART IOCTL requires GENERIC_WRITE; forbidden by zero-write rule",
        },
        "geometry": {"totalBytes": total, "totalSectors": total_sectors,
                     "sectorSize": SECTOR},
        "mbrValid": mbr_valid,
        "mbrPartitions": mbr_parts,
        "signaturesFound": all_sigs,
        "samples": samples,
        "damageLayerAssessment": assessment,
    }


def verify(current, saved):
    a = saved.get("samples", [])
    b = current.get("samples", [])
    diffs = []
    by_name_a = {s["name"]: s for s in a}
    for s in b:
        old = by_name_a.get(s["name"])
        if not old:
            diffs.append("%s: missing in baseline" % s["name"])
            continue
        if old.get("sha256") != s.get("sha256"):
            diffs.append("%s: sha256 %s -> %s" %
                         (s["name"], old.get("sha256"), s.get("sha256")))
    ident_ok = (saved.get("deviceIdentity", {}).get("serialNumber") ==
                current.get("deviceIdentity", {}).get("serialNumber") and
                saved.get("geometry", {}).get("totalBytes") ==
                current.get("geometry", {}).get("totalBytes"))
    return {"identityUnchanged": ident_ok,
            "samplesCompared": len(b),
            "changed": diffs,
            "conclusion": ("SOURCE DISK UNCHANGED" if ident_ok and not diffs
                           else "SOURCE DISK CHANGED - INVESTIGATE")}


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    mode = "--verify" in sys.argv
    result = capture()

    if mode:
        if not os.path.exists(BASELINE):
            die("baseline.json not found; capture first")
        with open(BASELINE, "r", encoding="utf-8") as f:
            saved = json.load(f)
        v = verify(result, saved)
        print("\n=== VERIFY ===")
        print(json.dumps(v, indent=2, ensure_ascii=False))
        with open(os.path.join(OUT_DIR, "baseline_verify.json"), "w", encoding="utf-8") as f:
            json.dump({"verifiedAtUtc": datetime.datetime.now(datetime.timezone.utc)
                       .isoformat().replace("+00:00", "Z"),
                       "result": v}, f, indent=2, ensure_ascii=False)
    else:
        with open(BASELINE, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2, ensure_ascii=False)
        print("\n=== BASELINE -> %s ===" % BASELINE)
        print(json.dumps({"deviceIdentity": result["deviceIdentity"],
                          "geometry": result["geometry"],
                          "mbrValid": result["mbrValid"],
                          "mbrPartitions": result["mbrPartitions"],
                          "signaturesFound": result["signaturesFound"],
                          "damageLayerAssessment": result["damageLayerAssessment"]},
                         indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
