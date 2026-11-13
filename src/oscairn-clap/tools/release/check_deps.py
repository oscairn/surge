#!/usr/bin/env python3
"""Dependency check on a release .clap: it must load on the Oscairn device image.

    check_deps.py <file.clap> <platform>     platform: macos-* or linux-*

macOS (otool): every load command path is under /usr/lib or /System, and no LC_RPATH.
Linux (readelf): every DT_NEEDED is in the device allowlist (oscairn main
docs/plugin-porting.md "The Linux allowlist"); no RPATH/RUNPATH; no symbol version
newer than GLIBC_2.44 / GLIBCXX_3.4.34; no GLIBC_PRIVATE.

Exits non-zero naming the offending library or symbol.
"""
import os
import re
import subprocess
import sys

# The 19 names in plugin-porting.md; each is in both device images.
LINUX_NEEDED = {
    'libc.so.6', 'libm.so.6', 'libmvec.so.1', 'libdl.so.2', 'libpthread.so.0', 'librt.so.1',
    'ld-linux-aarch64.so.1', 'ld-linux-x86-64.so.2',
    'libgcc_s.so.1', 'libstdc++.so.6', 'libasound.so.2',
    'libX11.so.6', 'libXext.so.6', 'libXrandr.so.2', 'libXinerama.so.1',
    'libXcursor.so.1', 'libXrender.so.1', 'libfreetype.so.6', 'libfontconfig.so.1',
}
# Newest symbol versions in the device images' glibc and libstdc++ (plugin-porting.md table).
VERSION_CEILINGS = {'GLIBC': (2, 44), 'GLIBCXX': (3, 4, 34)}


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def fail(problems):
    for p in problems:
        print(f'check_deps: {p}', file=sys.stderr)
    sys.exit(1)


def dotted_version(text):
    return tuple(int(x) for x in text.split('.'))


def check_macos(path):
    problems = []
    out = run('otool', '-L', path)
    # Lines are tab-indented paths; the first is the file itself.
    for line in out.splitlines()[1:]:
        line = line.strip()
        lib = line.split(' (')[0].strip()
        if not (lib.startswith('/usr/lib/') or lib.startswith('/System/')):
            problems.append(f'load command outside /usr/lib and /System: {lib}')
    out = run('otool', '-l', path)
    if re.search(r'^\s*cmd LC_RPATH', out, re.M):
        rpath = re.search(r'path (\S+)', out)
        problems.append(f'LC_RPATH present: {rpath.group(1) if rpath else "?"}')
    return problems


def check_linux(path):
    problems = []
    dynamic = run('readelf', '-d', path)
    for m in re.finditer(r'\((NEEDED)\)\s+Shared library:\s+\[(\S+)\]', dynamic):
        if m.group(2) not in LINUX_NEEDED:
            problems.append(f'DT_NEEDED outside the allowlist: {m.group(2)}')
    for tag in ('RPATH', 'RUNPATH'):
        for m in re.finditer(r'\(' + tag + r'\)\s+(?:Library rpath|Library runpath):\s+\[(\S*)\]', dynamic):
            if m.group(1):
                problems.append(f'{tag} present: {m.group(1)}')
    versions = run('readelf', '-V', path)
    # Version needs look like "GLIBC_2.38", possibly flagged (*global). Only the plain names matter.
    for m in re.finditer(r'\b(GLIBC|GLIBCXX)_(\d+(?:\.\d+)*)\b', versions):
        ceiling = VERSION_CEILINGS[m.group(1)]
        got = dotted_version(m.group(2))
        if got > ceiling:
            problems.append(f'symbol version {m.group(0)}_{m.group(2)} above the image ceiling '
                            f'{m.group(1)}_{".".join(map(str, ceiling))}')
    if 'GLIBC_PRIVATE' in versions:
        problems.append('GLIBC_PRIVATE symbol version needed')
    return problems


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: check_deps.py <file.clap> <platform>')
    path, platform = sys.argv[1], sys.argv[2]
    if not os.path.isfile(path):
        sys.exit(f'check_deps: {path} not found')
    if platform.startswith('macos-'):
        problems = check_macos(path)
    elif platform.startswith('linux-'):
        problems = check_linux(path)
    else:
        sys.exit(f'check_deps: unknown platform {platform!r} (macos-* or linux-*)')
    if problems:
        fail(problems)
    print(f'check_deps: {os.path.basename(path)} passes the {platform} dependency check')


if __name__ == '__main__':
    main()
