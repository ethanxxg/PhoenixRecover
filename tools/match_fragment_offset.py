#!/usr/bin/env python3
"""Match disk sectors to source file byte offsets (for fragmented layouts)."""
import os

ROOT = os.path.join(os.path.dirname(__file__), "..", "testdata", "file-carving-images")
ORIG = os.path.join(ROOT, "OriginalFiles", "Graphic")
IMAGES = os.path.join(ROOT, "Images")

# (level, name, sector from golden.h)
CASES = [
    ("L3", "100_0304crop.bmp", 49549),
    ("L3", "100_0183.gif", 75593),
    ("L3", "000_0021.png", 85767),
    ("L4", "02010026.jpg", 10059),
    ("L4", "100_0018.tif", 23567),
    ("L4", "100_0183.gif", 70248),
]


def longest_match_at(dd, dd_off, ref):
    best_off, best_len = 0, 0
    step = 512
    for start in range(0, len(ref), step):
        n = min(len(ref) - start, len(dd) - dd_off)
        lo, hi, m = 1, n, 0
        while lo <= hi:
            mid = (lo + hi) // 2
            if dd[dd_off : dd_off + mid] == ref[start : start + mid]:
                m = mid
                lo = mid + 1
            else:
                hi = mid - 1
        if m > best_len:
            best_len, best_off = m, start
    return best_off, best_len


def main():
    for lvl, name, sec in CASES:
        dd = open(os.path.join(IMAGES, f"{lvl}_Graphic.dd"), "rb").read()
        ref = open(os.path.join(ORIG, name), "rb").read()
        dd_off = sec * 512
        src_off, m = longest_match_at(dd, dd_off, ref)
        print(f"{lvl} {name} sec={sec} -> file_offset={src_off} match={m}")


if __name__ == "__main__":
    main()
