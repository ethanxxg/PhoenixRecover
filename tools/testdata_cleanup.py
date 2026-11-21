#!/usr/bin/env python3
"""Testdata cleanup: reclaim space from E01-derived .dd expansions and true duplicates.

Never deletes E01/E02/E03/AFF/RAW/ZIP originals, nor any dataset that has no
same-directory source. Default is dry-run; pass --apply to unlink.

Usage:
  py -3 tools/testdata_cleanup.py                 # dry-run report
  py -3 tools/testdata_cleanup.py --apply         # execute
  py -3 tools/testdata_cleanup.py --apply --keep domexusers   # exclude by substring

Regenerate the deleted raw images later:
  powershell -File testdata/tools/Expand-EwfToRaw.ps1 -RepoRoot <repo> -Roots digital-corpora-disk-images
"""
import argparse
import hashlib
import json
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CD_ROOT = os.path.join(REPO, "testdata", "digital-corpora-disk-images")
SRC_EXT = (".E01", ".E02", ".E03", ".aff", ".raw", ".zip")
# A wiped-disk expansion is ~262 GiB of zeros; re-expanding it is never worth it.
NEVER_REEXPAND = ("wiped_disk.dd",)


def md5(path, chunk=1 << 20):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(chunk), b""):
            h.update(blk)
    return h.hexdigest()


def walk(root):
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            yield os.path.join(dirpath, name)


def collect(keep_patterns, only_patterns):
    cands = []
    for path in walk(CD_ROOT):
        name = os.path.basename(path)
        if any(p and p.lower() in name.lower() for p in keep_patterns):
            continue
        rel = os.path.relpath(path, REPO)

        if name.lower().endswith(".dd"):
            if only_patterns and not any(
                p.lower() in name.lower() for p in only_patterns
            ):
                continue
            base = path[: -len(".dd")]
            srcs = [base + e for e in SRC_EXT if os.path.exists(base + e)]
            if not srcs:
                continue  # original download, keep
            st = os.stat(path)
            c = {
                "path": rel,
                "bytes": st.st_size,
                "reason": "e01-derived",
                "sources": [os.path.relpath(s, REPO) for s in srcs],
                "mtime": time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime)),
                "src_mtime": time.strftime(
                    "%Y-%m-%d %H:%M", time.localtime(os.stat(srcs[0]).st_mtime)
                ),
            }
            if name in NEVER_REEXPAND:
                c["note"] = "all-zero wiped-disk expansion; do not regenerate"
            cands.append(c)
            continue

        if name.endswith(".old"):
            twin = path[: -len(".old")]
            if os.path.exists(twin) and os.path.getsize(twin) == os.path.getsize(path):
                if md5(twin) == md5(path):
                    cands.append(
                        {
                            "path": rel,
                            "bytes": os.path.getsize(path),
                            "reason": "byte-identical duplicate of "
                            + os.path.relpath(twin, REPO),
                        }
                    )
            continue

        if name.endswith(".linco"):
            cands.append(
                {
                    "path": rel,
                    "bytes": os.path.getsize(path),
                    "reason": "download-tool cache residue",
                }
            )
    return cands


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--keep", action="append", default=[])
    ap.add_argument(
        "--only",
        action="append",
        default=[],
        help="restrict derived .dd removal to names containing this substring "
        "(duplicates and junk are always handled)",
    )
    args = ap.parse_args()

    cands = collect(args.keep, args.only)
    cands.sort(key=lambda c: -c["bytes"])
    total = sum(c["bytes"] for c in cands)

    print(f"{'SIZE':>10}  {'MTIME':<16} {'SRC MTIME':<16} REASON / PATH")
    for c in cands:
        print(
            f"{human(c['bytes']):>10}  {c.get('mtime','-'):<16} "
            f"{c.get('src_mtime','-'):<16} {c['reason']}"
        )
        print(f"{'':>10}  {c['path']}")
        if c.get("note"):
            print(f"{'':>10}  ! {c['note']}")
    print(f"\ncandidates={len(cands)} reclaim={human(total)}")

    if not args.apply:
        print("\n(dry-run; re-run with --apply)")
        return 0

    freed = 0
    failed = []
    for c in cands:
        full = os.path.join(REPO, c["path"])
        try:
            size = os.path.getsize(full)
            os.remove(full)
            freed += size
            print(f"removed {human(size):>10}  {c['path']}")
        except OSError as exc:
            failed.append({"path": c["path"], "error": str(exc)})
            print(f"FAILED  {c['path']}: {exc}", file=sys.stderr)

    out_dir = os.path.join(REPO, "docs", "reports")
    os.makedirs(out_dir, exist_ok=True)
    stamp = time.strftime("%Y-%m-%d")
    log = {
        "date": stamp,
        "root": os.path.relpath(CD_ROOT, REPO),
        "applied": True,
        "candidates": cands,
        "freedBytes": freed,
        "freedHuman": human(freed),
        "failed": failed,
        "regenerate": (
            "powershell -File testdata/tools/Expand-EwfToRaw.ps1 "
            "-RepoRoot <repo> -Roots digital-corpora-disk-images"
        ),
        "warnings": [
            "wiped_disk.dd was a zero-fill expansion of a 54 MB E01; "
            "regenerating it costs ~262 GiB of writes with no test value."
        ],
    }
    json_path = os.path.join(out_dir, f"testdata_cleanup_{stamp}.json")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(log, f, ensure_ascii=False, indent=2)

    print(f"\nfreed={human(freed)} failed={len(failed)}")
    print(f"log: {os.path.relpath(json_path, REPO)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
