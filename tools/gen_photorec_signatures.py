#!/usr/bin/env python3
"""Extract declarative carving signatures from the PhotoRec 7.2 sources.

PhotoRec (the carving half of TestDisk) registers one C callback per format,
but almost every callback starts with a purely declarative statement:

    register_header_check(<offset>, <array>, sizeof(<array>), &cb, file_stat)

This tool resolves those <array> literals back to bytes and emits a signature
file that PhoenixRecover's production SignatureDB::LoadFromFile() can load
directly.  It deliberately extracts only the *declarative* layer: any extra
validation a callback performs (e.g. JPG's DHT walk) is not reproduced here,
so short magics are flagged so the caller can weight them accordingly.

Output schema matches testdata/custom_signatures.json:
    { "signatures": [ {extension, magic, magic_offset, footer, max_size, ...} ] }

Usage:
    python tools/gen_photorec_signatures.py --src D:/dev/testdisk-7.2/src \
        --out testdata/signatures/photorec_aligned.json
"""

import argparse
import json
import os
import re
import sys

# ── C literal helpers ────────────────────────────────────────────────────────

COMMENT_RE = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)
DEFINE_RE = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.+?)[ \t]*$', re.M)

ARRAY_RE = re.compile(
    r'(?:static\s+)?(?:const\s+)?unsigned\s+char\s+(\w+)\s*\[[^\]]*\]\s*=\s*'
    r'(?:\{([^}]*)\}|"((?:[^"\\]|\\.)*)")', re.S)

HINT_EXT_RE = re.compile(r'\.extension\s*=\s*"([^"]*)"')
HINT_MAX_RE = re.compile(r'\.max_filesize\s*=\s*([^,\n]+)')
HINT_DESC_RE = re.compile(r'\.description\s*=\s*"([^"]*)"')

REG_RE = re.compile(
    r'register_header_check\s*\(\s*([^,]+?)\s*,\s*(\w+)\s*,\s*'
    r'(?:sizeof\s*\(\s*\2\s*\)|\d+)\s*,', re.S)


def strip_comments(text):
    return COMMENT_RE.sub(' ', text)


def eval_int(expr, defines, depth=0):
    """Best-effort evaluation of a C integer constant expression."""
    if depth > 8:
        return None
    expr = expr.strip()
    if not expr:
        return None
    # Drop casts such as (uint64_t)
    expr = re.sub(r'\(\s*(?:u?int(?:8|16|32|64)_t|unsigned(?:\s+\w+)?|long|int|size_t)\s*\)', '', expr)
    expr = expr.replace('LL', '').replace('ULL', '').replace('UL', '').replace('L', '')
    tokens = re.findall(r'0[xX][0-9a-fA-F]+|\d+|\w+|<<|>>|\*\*|[()+\-*/%]', expr)
    if not tokens:
        return None
    for t in tokens:
        if re.fullmatch(r'\d+', t) or re.fullmatch(r'0[xX][0-9a-fA-F]+', t):
            continue
        if re.fullmatch(r'\w+', t):
            if t in defines:
                return eval_int(defines[t], defines, depth + 1)
            return None
    py = ' '.join(tokens).replace('<<', '<<').replace('>>', '>>')
    py = re.sub(r'\b0[xX]([0-9a-fA-F]+)\b', lambda m: str(int(m.group(1), 16)), py)
    try:
        return int(eval(py, {'__builtins__': {}}, {}))
    except Exception:
        return None


def parse_bytes(body, defines):
    body = body.strip()
    if not body:
        return None
    # Plain "PK\003\004" style string literal.
    if body.startswith('"') and body.endswith('"'):
        s = body[1:-1]
        out, i = [], 0
        while i < len(s):
            if s[i] == '\\':
                i += 1
                if i >= len(s):
                    return None
                c = s[i]
                if c == 'x':
                    m = re.match(r'[0-9a-fA-F]{1,2}', s[i + 1:])
                    if not m:
                        return None
                    out.append(int(m.group(0), 16))
                    i += 1 + len(m.group(0))
                    continue
                esc = {'n': 10, 'r': 13, 't': 9, '0': 0, '\\': 92, '"': 34, "'": 39}
                if c in esc:
                    out.append(esc[c])
                elif c.isdigit():
                    m = re.match(r'[0-7]{1,3}', s[i:])
                    out.append(int(m.group(0), 8) & 0xFF)
                    i += len(m.group(0))
                    continue
                else:
                    return None
                i += 1
            else:
                out.append(ord(s[i]) & 0xFF)
                i += 1
        return out

    out = []
    for item in body.split(','):
        item = item.strip()
        if not item:
            continue
        if item.startswith("'") and item.endswith("'") and len(item) >= 3:
            ch = item[1:-1]
            if len(ch) == 1:
                out.append(ord(ch))
                continue
            if ch.startswith('\\') and len(ch) >= 2:
                out.append({'n': 10, 'r': 13, 't': 9, '0': 0, '\\': 92, "'": 39}.get(ch[1], -1) & 0xFF)
                continue
            return None
        v = eval_int(item, defines)
        if v is None:
            return None
        if v < 0 or v > 255:
            return None
        out.append(v)
    return out or None


# ── extraction ───────────────────────────────────────────────────────────────

def scan_file(path, defines):
    """Return (hints, regs, arrays) for one translation unit."""
    raw = open(path, 'r', encoding='utf-8', errors='replace').read()
    text = strip_comments(raw)
    arrays = {}
    for m in ARRAY_RE.finditer(text):
        name = m.group(1)
        body = m.group(2) if m.group(2) is not None else '"' + (m.group(3) or '') + '"'
        b = parse_bytes(body, defines)
        if b:
            arrays.setdefault(name, b)
    hints = re.findall(HINT_EXT_RE, text)
    maxes = HINT_MAX_RE.findall(text)
    descs = HINT_DESC_RE.findall(text)
    regs = []
    for m in REG_RE.finditer(text):
        off_expr, arr = m.group(1), m.group(2)
        off = eval_int(off_expr, defines)
        regs.append((off, arr, off_expr.strip()))
    return hints, regs, arrays, maxes, descs, text


def parse_existing(path):
    """Read the built-in table in scan/SignatureDB.cpp: mk(L"ext", "HEX", ..., off, ...)."""
    text = open(path, 'r', encoding='utf-8', errors='replace').read()
    out = set()
    for m in re.finditer(r'mk\(\s*L"(\w+)"\s*,\s*"([0-9A-Fa-f]*)"\s*,[^;]*?,\s*(\d+)\s*,', text):
        out.add((m.group(1).lower(), m.group(2).upper(), int(m.group(3))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True, help='Path to testdisk src/ directory')
    ap.add_argument('--out', required=True, help='Output JSON path (main library)')
    ap.add_argument('--lowconf-out', help='Optional JSON for magics shorter than --min-magic-bytes')
    ap.add_argument('--emit-header', help='Optional C++ header to generate')
    ap.add_argument('--exclude', help='SignatureDB.cpp to exclude signatures it already ships')
    ap.add_argument('--report', help='Optional coverage report (text)')
    ap.add_argument('--min-magic-bytes', type=int, default=3,
                    help='Magics shorter than this go to --lowconf-out, not the main library')
    ap.add_argument('--min-distinct-bytes', type=int, default=2,
                    help='Reject magics built from fewer distinct byte values (zero/erased fill)')
    ap.add_argument('--max-size-cap', type=int, default=4 * 1024 * 1024 * 1024,
                    help='Clamp absurd PhotoRec max_filesize values (default 4 GiB)')
    args = ap.parse_args()

    src = args.src
    files = sorted(f for f in os.listdir(src) if re.fullmatch(r'file_\w+\.c', f))
    if not files:
        print('ERROR: no file_*.c under %s' % src, file=sys.stderr)
        return 2

    # Global #define table (headers included transitively by the .c files are
    # not parsed; per-file defines cover the magic arrays in practice).
    global_defines = {}
    for name in ('filegen.h', 'common.h'):
        p = os.path.join(src, name)
        if os.path.exists(p):
            for m in DEFINE_RE.finditer(open(p, encoding='utf-8', errors='replace').read()):
                global_defines.setdefault(m.group(1), m.group(2))

    sigs, stats = [], {
        'files': len(files),
        'hints': 0,
        'registrations': 0,
        'resolved': 0,
        'unresolved_array': [],
        'unresolved_offset': [],
        'no_hint': [],
        'empty_extension': [],
    }

    for fname in files:
        path = os.path.join(src, fname)
        raw = open(path, 'r', encoding='utf-8', errors='replace').read()
        defines = dict(global_defines)
        for m in DEFINE_RE.finditer(raw):
            defines.setdefault(m.group(1), m.group(2))

        hints, regs, arrays, maxes, descs, text = scan_file(path, defines)
        if not hints:
            stats['no_hint'].append(fname)
            continue
        stats['hints'] += len(hints)
        stats['registrations'] += len(regs)

        max_size = None
        if maxes:
            max_size = eval_int(maxes[0], defines)

        for ext in hints:
            if not ext:
                stats['empty_extension'].append(fname)
                continue
            for off, arr, off_expr in regs:
                if arr not in arrays:
                    stats['unresolved_array'].append('%s:%s' % (fname, arr))
                    continue
                if off is None:
                    stats['unresolved_offset'].append('%s:%s(%s)' % (fname, arr, off_expr))
                    continue
                magic = arrays[arr]
                if not magic:
                    continue
                ms = max_size if max_size and max_size > 0 else None
                if ms and ms > args.max_size_cap:
                    ms = args.max_size_cap
                sigs.append({
                    'extension': ext.lower(),
                    'category': 'PhotoRec',
                    'description': (descs[0] if descs else ext) + ' (PhotoRec declarative magic)',
                    'magic': ''.join('%02X' % b for b in magic),
                    'magic_offset': off,
                    'footer': '',
                    'max_size': ms if ms else args.max_size_cap,
                    'secondary_magic': '',
                    'secondary_offset': -1,
                    '_source': fname,
                })
                stats['resolved'] += 1

    # Deduplicate on (extension, offset, magic) but keep distinct variants.
    seen, uniq = set(), []
    for s in sigs:
        key = (s['extension'], s['magic_offset'], s['magic'])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(s)

    existing = parse_existing(args.exclude) if args.exclude else set()
    kept, dup_of_builtin, bad_ext, low_entropy = [], [], [], []
    EXT_OK = re.compile(r'^[a-z0-9_]{1,12}$')
    for s in uniq:
        if not EXT_OK.match(s['extension']):
            # PhotoRec uses display wildcards such as "gm*" or "tx?" for families
            # that share one hint; they are not real extensions and would make
            # the emitted node type meaningless.
            bad_ext.append(s['extension'])
            continue
        if (s['extension'], s['magic'], s['magic_offset']) in existing:
            dup_of_builtin.append(s)
            continue
        # Entropy floor for the main library.  PhotoRec can afford an all-zero
        # magic (dovecot is 48 zero bytes) because its callback then validates
        # the surrounding structure.  We only carry the declarative layer, so a
        # magic that a zero-filled or erased region satisfies would match every
        # 512-byte slot and drown the result list.  Short magics are handled
        # separately below (they go to the low-confidence file, never the main
        # library).
        raw = bytes.fromhex(s['magic'])
        if len(raw) >= args.min_magic_bytes and len(set(raw)) < args.min_distinct_bytes:
            low_entropy.append((s['extension'], s['magic']))
            continue
        kept.append(s)

    main_lib, lowconf = [], []
    for s in kept:
        (main_lib if len(s['magic']) // 2 >= args.min_magic_bytes else lowconf).append(s)

    def dump(path, rows):
        os.makedirs(os.path.dirname(os.path.abspath(path)) or '.', exist_ok=True)
        with open(path, 'w', encoding='utf-8') as f:
            json.dump({'signatures': rows}, f, indent=2, ensure_ascii=False)

    dump(args.out, main_lib)
    if args.lowconf_out:
        dump(args.lowconf_out, lowconf)

    if args.emit_header:
        lines = [
            '#pragma once',
            '// ---------------------------------------------------------------------------',
            '// PhotoRecSignatures.h - generated by tools/gen_photorec_signatures.py',
            '//',
            '// Declarative carving magics extracted from TestDisk 7.2 PhotoRec',
            '// (src/file_*.c: register_header_check).  PhotoRec pairs each magic with a',
            '// C callback that performs additional structural validation; only the',
            '// declarative layer is reproduced here, so every entry is a *header hint*',
            '// rather than a full format validator.  Magics shorter than %d bytes are'
            % args.min_magic_bytes,
            '// deliberately excluded: on random data a 1-2 byte magic matches every few',
            '// hundred bytes and the false-positive rate becomes unusable.',
            '//',
            '// DO NOT EDIT BY HAND - re-run the generator instead.',
            '// ---------------------------------------------------------------------------',
            '#include "SignatureDB.h"',
            '',
            'namespace phoenix {',
            '',
            'inline void RegisterPhotoRecAlignSignatures(SignatureDB& db)',
            '{',
            '    auto add = [&db](const wchar_t* ext, std::initializer_list<uint8_t> magic,',
            '                     int off, uint64_t maxBytes) {',
            '        FileSignature s;',
            '        s.extension    = ext;',
            '        s.header.assign(magic.begin(), magic.end());',
            '        s.headerOffset = off;',
            '        s.maxSize      = maxBytes;',
            '        db.Add(std::move(s));',
            '    };',
            '',
        ]
        for s in main_lib:
            bs = ', '.join('0x' + s['magic'][i:i + 2] for i in range(0, len(s['magic']), 2))
            lines.append('    add(L"%s", {%s}, %d, %dull);' %
                         (s['extension'], bs, s['magic_offset'], s['max_size']))
        lines += ['}', '', '} // namespace phoenix', '']
        os.makedirs(os.path.dirname(os.path.abspath(args.emit_header)) or '.', exist_ok=True)
        with open(args.emit_header, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines))

    exts_main = sorted({s['extension'] for s in main_lib})
    exts_low = sorted({s['extension'] for s in lowconf})
    lines = [
        'PhotoRec declarative signature extraction',
        '  source files          : %d' % stats['files'],
        '  file_hint_t entries   : %d' % stats['hints'],
        '  register_header_check : %d' % stats['registrations'],
        '  resolved magic        : %d' % stats['resolved'],
        '  unique (ext,off,magic): %d' % len(uniq),
        '  duplicate of built-in : %d' % len(dup_of_builtin),
        '  wildcard/invalid ext  : %d (%s)' % (len(bad_ext), ' '.join(sorted(set(bad_ext))[:8])),
        '  low entropy (rejected): %d (%s)' % (
            len(low_entropy),
            ' '.join('%s=%s' % (e, m[:16]) for e, m in low_entropy[:6])),
        '  main library          : %d sigs / %d extensions (magic >= %d bytes)'
        % (len(main_lib), len(exts_main), args.min_magic_bytes),
        '  low-confidence        : %d sigs / %d extensions (magic <  %d bytes)'
        % (len(lowconf), len(exts_low), args.min_magic_bytes),
        '  files without hint    : %d' % len(stats['no_hint']),
        '  unresolved arrays     : %d' % len(stats['unresolved_array']),
        '  unresolved offsets    : %d' % len(stats['unresolved_offset']),
    ]
    if stats['unresolved_array']:
        lines.append('  -- unresolved arrays (first 20) --')
        lines += ['     ' + x for x in stats['unresolved_array'][:20]]
    if stats['unresolved_offset']:
        lines.append('  -- unresolved offsets (first 20) --')
        lines += ['     ' + x for x in stats['unresolved_offset'][:20]]
    report = '\n'.join(lines)
    print(report)
    if args.report:
        with open(args.report, 'w', encoding='utf-8') as f:
            f.write(report + '\n\n')
            f.write('Main-library extensions (%d):\n%s\n\n' % (len(exts_main), ' '.join(exts_main)))
            f.write('Low-confidence extensions (%d):\n%s\n' % (len(exts_low), ' '.join(exts_low)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
