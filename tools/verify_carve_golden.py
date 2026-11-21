#!/usr/bin/env python3
"""Verify / derive embedBytes for file_carving_golden.h from local dd images."""
import hashlib
import os

ROOT = os.path.join(os.path.dirname(__file__), "..", "testdata", "file-carving-images")
ORIG = os.path.join(ROOT, "OriginalFiles", "Graphic")
IMAGES = os.path.join(ROOT, "Images")

LEVELS = {
    "L2": {
        "100_0018.tif": 10000,
        "09260002.jpg": 49237,
        "100_0304crop.bmp": 59356,
        "02010025.pcx": 79341,
        "100_0183.gif": 90917,
        "000_0021.png": 101177,
    },
    "L3": {
        "09260002.jpg": 10000,
        "100_0018.tif": 20059,
        "100_0304crop.bmp": 49549,
        "02010025.pcx": 64542,
        "100_0183.gif": 75593,
        "000_0021.png": 85767,
    },
    "L4": {
        "09260002.jpg": 10000,
        "02010026.jpg": 10059,
        "100_0304crop.bmp": 20239,
        "100_0018.tif": 23567,
        "02010025.pcx": 69461,
        "100_0183.gif": 70248,
        "000_0021.png": 70420,
    },
    "L5": {
        "09260002.jpg": 10000,
        "100_0304crop.bmp": 10059,
        "100_0018.tif": 30104,
        "02010025.pcx": 39849,
        "100_0183.gif": 70917,
        "000_0021.png": 80917,
    },
}


def longest_prefix_match(hay, needle):
    lo, hi, best = 1, min(len(hay), len(needle)), 0
    while lo <= hi:
        mid = (lo + hi) // 2
        if hay[:mid] == needle[:mid]:
            best = mid
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    for lvl, files in LEVELS.items():
        dd_path = os.path.join(IMAGES, f"{lvl}_Graphic.dd")
        dd = open(dd_path, "rb").read()
        print(f"// {lvl}")
        for name, sec in files.items():
            ref = open(os.path.join(ORIG, name), "rb").read()
            off = sec * 512
            chunk = dd[off : off + len(ref)]
            embed = longest_prefix_match(chunk, ref)
            ok = embed > 0
            tag = "OK" if ok else "FAIL"
            print(
                f'  {{"{name}", {sec:6d}, {len(ref):8d}, {embed:8d}, false}},  // {tag}'
            )
        print()


if __name__ == "__main__":
    main()
