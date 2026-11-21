#!/usr/bin/env python3
"""Compare UI type-tree extensions vs SignatureDB built-in signatures."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAGE = (ROOT / "pages" / "RecoveryScanPage.cpp").read_text(encoding="utf-8", errors="ignore")
SIG = (ROOT / "scan" / "SignatureDB.cpp").read_text(encoding="utf-8", errors="ignore")


def extract_csv(name: str) -> list[str]:
    anchor = f"const QString &{name}()"
    start = PAGE.find(anchor)
    if start < 0:
        return []
    brace = PAGE.find("{", start)
    depth = 0
    end = brace
    for i in range(brace, len(PAGE)):
        if PAGE[i] == "{":
            depth += 1
        elif PAGE[i] == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break
    block = PAGE[brace:end]
    lit = re.search(r"QStringLiteral\(\s*(.*?)\s*\)\s*;", block, re.S)
    if lit:
        parts = re.findall(r'"([^"]*)"', lit.group(1))
    else:
        parts = re.findall(r'"([^"]*)"', block)
    csv = "".join(parts)
    return [x.strip().lower() for x in csv.split(",") if x.strip()]


PICS_CSV = (
    "jpg,png,jpeg,jfif,jpe,gif,tif,tiff,eps,psd,bmp,dib,ico,3mf,ai,ani,apm,avif,arw,cr2,crw,cur,"
    "dds,djvu,dsc,dng,emf,exr,gmp,gpd,gpr,hdr,heic,heif,hp,icns,icon,jp2,j2c,j2k,jpf,jxr,mnw,nef,"
    "orf,pam,pbm,pgm,pm,ppm,pef,ps,raf,raw,rw2,sci,scp,scr,sgi,shp,srw,stl,svg,tga,tpl,webp,wmf,"
    "xar,xbm,xcf,xpm,3fr,mrw"
).split(",")

IMG_FIXED = (
    "jpg png jpeg gif tif eps psd bmp ico 3mf ai ani apm crw cur dds djvu dsc emf exr gmp gpd gpr "
    "hdr heic heif hp icns icon jp2 jpf jxr mnw pbm pgm pm ppm ps raw sci scp scr sgi shp stl svg "
    "tga tiff tpl webp wmf xar xbm xcf xpm"
).split()

VID_FIXED = (
    "mp4 mov avi swf m4v wmv 3g2 3gp 3gpp 3gpp2 aaf asf bin dat dir dv fbx flv h264 m2t m2ts m3u "
    "mkv mod mpeg mts mxf ogv rms rmvb rpm rt scm ts webm"
).split()

ui_images = sorted(set(x.strip().lower() for x in PICS_CSV + IMG_FIXED if x.strip()))
ui_videos = sorted(set(x.strip().lower() for x in VID_FIXED if x.strip()))
ui_docs = sorted(set(extract_csv("documentExtensionsCsv")))
ui_audio = sorted(set(extract_csv("audioExtensionsCsv")))
ui_arch = sorted(set(extract_csv("archiveExtensionsCsv")))

sig_exts: set[str] = set()
for m in re.finditer(r'(?:mk|riff|ftyp)\(L"([^"]+)"', SIG):
    sig_exts.add(m.group(1).lower())

ALIASES = {
    "jpeg": "jpg",
    "jfif": "jpg",
    "jpe": "jpg",
    "tiff": "tif",
    "aiff": "aif",
    "aifc": "aif",
    "mpeg": "mpg",
    "mpga": "mp3",
    "3gpp": "3gp",
    "3gpp2": "3g2",
    "m2ts": "mts",
    "rmvb": "rm",
    "acc": "aac",
    "icon": "ico",
    "dib": "bmp",
    "j2c": "jp2",
    "j2k": "jp2",
    "jpf": "jp2",
    "pef": "tif",
    "nef": "tif",
    "arw": "tif",
    "dng": "tif",
    "srw": "tif",
    "3fr": "tif",
    "sci": "tif",
    "scp": "tif",
    "scr": "tif",
    "mnw": "tif",
    "dsc": "tif",
    "gpd": "tif",
    "gpr": "tif",
    "gmp": "tif",
    "tpl": "tif",
    "pam": "pbm",
    "pm": "ppm",
    "rpm": "rpm",  # placeholder
}

ZIP_OOXML = {
    "docx", "xlsx", "pptx", "odt", "ods", "odp", "epub", "numbers", "key", "xps",
    "apk", "jar", "ipa", "odg", "ott", "dotx", "potx", "thmx", "xlf", "xmind",
    "dotm", "dox",
}
CFB_OLE = {
    "doc", "xls", "ppt", "mdb", "msg", "pub", "wps", "msi", "vsd", "accdb", "dot",
    "pot", "sam", "wpt",
}
RIFF_EXT = {"avi", "wav", "webp"}
FTYP_EXT = {"mp4", "m4v", "m4a", "mov", "3gp", "3g2", "heic", "heif", "avif", "cr3", "f4v"}


def covered(ext: str) -> str | bool:
    e = ext.lower()
    if e in sig_exts:
        return True
    if e in ALIASES and ALIASES[e] in sig_exts:
        return f"alias:{ALIASES[e]}"
    if e in ZIP_OOXML and "zip" in sig_exts:
        return "zip-container"
    if e in CFB_OLE and "doc" in sig_exts:
        return "cfb-container"
    if e in RIFF_EXT:
        return "riff-family"
    if e in FTYP_EXT:
        return "ftyp-family"
    return False


def report(name: str, exts: list[str]) -> list[str]:
    missing = [e for e in exts if covered(e) is False]
    print(f"=== {name}: UI={len(exts)} missing={len(missing)} ===")
    if missing:
        print("  " + ",".join(missing))
    return missing


all_missing = []
all_missing += report("图片", ui_images)
all_missing += report("视频", ui_videos)
all_missing += report("文档", ui_docs)
all_missing += report("音频", ui_audio)
all_missing += report("压缩", ui_arch)
print(f"\nTOTAL unique missing: {len(set(all_missing))}")
