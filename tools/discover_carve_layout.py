#!/usr/bin/env python3
import os

ROOT = os.path.join(os.path.dirname(__file__), "..", "testdata", "file-carving-images")
ORIG = os.path.join(ROOT, "OriginalFiles", "Graphic")
IMAGES = os.path.join(ROOT, "Images")

FILES6 = [
    "09260002.jpg",
    "100_0018.tif",
    "100_0304crop.bmp",
    "02010025.pcx",
    "100_0183.gif",
    "000_0021.png",
]
FILES7 = FILES6 + ["02010026.jpg"]


def find_header(dd, ref):
    sig = ref[: min(32, len(ref))]
    for sec in range(len(dd) // 512):
        off = sec * 512
        if dd[off : off + len(sig)] == sig:
            m = 0
            while off + m < len(dd) and m < len(ref) and dd[off + m] == ref[m]:
                m += 1
            return sec, m
    return None, 0


def main():
    for lvl in ["L1", "L2", "L3", "L4", "L5"]:
        dd = open(os.path.join(IMAGES, f"{lvl}_Graphic.dd"), "rb").read()
        flist = FILES7 if lvl == "L4" else FILES6
        print(f"// {lvl}")
        for name in flist:
            ref = open(os.path.join(ORIG, name), "rb").read()
            sec, m = find_header(dd, ref)
            print(f'  {{"{name}", {sec if sec else 0:6d}, {len(ref):8d}, {m:8d}, false}},')
        print()


if __name__ == "__main__":
    main()
