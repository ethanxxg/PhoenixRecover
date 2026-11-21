#!/usr/bin/env python3
"""List duplicate extensions in UiFileTypeCatalog.cpp."""
import re
from collections import defaultdict
from pathlib import Path

text = Path(__file__).resolve().parents[1].joinpath(
    "common", "UiFileTypeCatalog.cpp"
).read_text(encoding="utf-8")

cats = re.findall(r'\{ L"([^"]+)", EXT\((.*?)\)\s*\}', text, re.S)


def exts(block: str) -> list[str]:
    return [x.strip().lower() for x in re.findall(r'L"([^"]+)"', block)]


by_ext: dict[str, list[str]] = defaultdict(list)
intra: list[tuple[str, str]] = []

for name, block in cats:
    seen: set[str] = set()
    for e in exts(block):
        if e in seen:
            intra.append((name, e))
        seen.add(e)
        by_ext[e].append(name)

print("=== Intra-category duplicates ===")
for n, e in intra:
    print(f"  {n}: {e}")
if not intra:
    print("  (none)")

print("\n=== Cross-category duplicates ===")
for e, names in sorted(by_ext.items()):
    if len(names) > 1:
        print(f"  {e}: {', '.join(names)}")

print(f"\nTotal extensions (with dupes): {sum(len(exts(b)) for _, b in cats)}")
print(f"Unique extensions: {len(by_ext)}")
