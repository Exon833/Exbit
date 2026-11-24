#!/usr/bin/env python3
# Builds data/fonts: the fonts shipped for the symbols the interface draws (▾ ✕ ♩ ⟲ ⏮ …), so they show on a machine
# without DejaVu or Noto (Windows, a minimal Linux). Run from the repository's root when the interface gets a symbol
# the bundle lacks (tests/test_gfx.cpp says which):
#   python3 tools/symbol_fonts.py [--dejavu DejaVuSans.ttf] [--noto NotoSansSymbols-Regular.ttf ...]
# The code points: every non-ASCII character inside a string in src/ (not the terminal's: it draws with the
# terminal's own font) and in data/'s stylesheets. DejaVu Sans gives what it has, plus ASCII and Latin-1 (text even
# when the system has no font at all); the Noto symbol fonts the rest, merged into one file. Needs fontTools.
import argparse, glob, os, re, sys, tempfile

from fontTools import subset
from fontTools.merge import Merger
from fontTools.ttLib import TTFont

FLATPAK = '/var/lib/flatpak/runtime/org.gnome.Platform/x86_64/*/*/files/share/fonts/dejavu/DejaVuSans.ttf'
DEJAVU = ['/usr/share/fonts/TTF/DejaVuSans.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
          '/usr/share/fonts/dejavu/DejaVuSans.ttf'] + sorted(glob.glob(FLATPAK))
NOTO = ['/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf', '/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf',
        '/usr/share/fonts/noto/NotoSansMath-Regular.ttf']


def literals(src):  # the strings and characters of a C++ file, comments skipped
    i, n = 0, len(src)
    while i < n:
        if src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif src.startswith('R"', i) and re.match(r'R"([^(\s]*)\(', src[i:]):
            m = re.match(r'R"([^(\s]*)\(', src[i:])
            end = ')' + m.group(1) + '"'
            j = src.find(end, i)
            yield src[i:j]
            i = n if j < 0 else j + len(end)
        elif src[i] in '"\'':
            q, j = src[i], i + 1
            while j < n and src[j] != q and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            s = re.sub(r'(?:\\x[0-9a-fA-F]{2})+', lambda m: bytes.fromhex(m.group(0).replace('\\x', '')).decode('utf-8', 'replace'), src[i + 1:j])
            yield re.sub(r'\\u([0-9a-fA-F]{4})', lambda m: chr(int(m.group(1), 16)), s)
            i = j + 1
        else:
            i += 1


def used_code_points():
    cps = set()
    for d, _, files in os.walk('src'):
        if d.startswith(('src/tui', 'src/winehost')):
            continue
        for f in files:
            if f.endswith(('.cpp', '.h')):
                for s in literals(open(os.path.join(d, f), encoding='utf-8').read()):
                    cps.update(ord(c) for c in s if ord(c) > 127)
    for d, _, files in os.walk('data'):
        for f in files:
            if f.endswith('.css'):
                t = re.sub(r'/\*.*?\*/', '', open(os.path.join(d, f), encoding='utf-8').read(), flags=re.S)
                cps.update(ord(c) for c in t if ord(c) > 127)
    cps -= {0xFEFF}  # (a byte order mark compared, never drawn)
    return cps


def cut(path, unicodes, out):
    opts = subset.Options()
    opts.hinting = False  # (the rasterizer doesn't hint)
    opts.drop_tables += ['MATH', 'DSIG', 'FFTM', 'vhea', 'vmtx']
    opts.name_IDs = ['*']  # (the copyright and licence entries kept)
    font = subset.load_font(path, opts)
    s = subset.Subsetter(opts)
    s.populate(unicodes=unicodes)
    s.subset(font)
    subset.save_font(font, out, opts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dejavu', default=next((p for p in DEJAVU if os.path.exists(p)), None))
    ap.add_argument('--noto', nargs='*', default=[p for p in NOTO if os.path.exists(p)])
    a = ap.parse_args()
    if not a.dejavu:
        sys.exit('DejaVu Sans not found: --dejavu path/to/DejaVuSans.ttf')
    used = used_code_points()
    os.makedirs('data/fonts', exist_ok=True)
    dejavu = TTFont(a.dejavu).getBestCmap()
    base = set(range(0x20, 0x7F)) | set(range(0xA0, 0x100))
    mine = (used & set(dejavu)) | base
    cut(a.dejavu, sorted(mine), 'data/fonts/DejaVuSans-Symbols.ttf')
    left, parts, notices = used - set(dejavu), [], []
    with tempfile.TemporaryDirectory() as tmp:
        for p in a.noto:
            font = TTFont(p)
            have = left & set(font.getBestCmap())
            if have:
                out = os.path.join(tmp, '%d.ttf' % len(parts))
                cut(p, sorted(have), out)
                parts.append(out)
                notices.append(font['name'].getDebugName(0))
                left -= have
        if parts:
            merged = Merger().merge(parts)
            for rec in merged['name'].names:  # (one family: what the merged file is; every part's copyright)
                if rec.nameID == 0:
                    rec.string = '; '.join(dict.fromkeys(notices))
                elif rec.nameID in (1, 4, 16):
                    rec.string = 'Noto Sans Symbols Tiny DAW'
                elif rec.nameID == 6:
                    rec.string = 'NotoSansSymbolsTinyDAW-Regular'
            merged.save('data/fonts/NotoSansSymbols-Symbols.ttf')
    print('%d code points: %d from DejaVu Sans, %d from Noto, missing: %s' % (
        len(used), len(used & set(dejavu)), len(used - set(dejavu) - left),
        ' '.join('U+%04X' % c for c in sorted(left)) or 'none'))
    return 1 if left else 0


if __name__ == '__main__':
    sys.exit(main())
