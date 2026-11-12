#!/bin/sh
# Collect every licence/notice file the binary needs into <outdir>/licenses/<name>
# and bundle them as <outdir>/licenses.tar.gz (the layout the manifest's licenseFiles use).
set -eu
here=$(dirname "$0")
cd "$(git -C "$here" rev-parse --show-toplevel)"
. "$here/release.conf"

out=${1:?usage: licenses.sh <outdir>}
mkdir -p "$out/licenses"
for pair in $LICENSE_FILES; do
    src=${pair%%=*}
    dst=${pair#*=}
    [ -f "$src" ] || { echo "licenses.sh: $src not found" >&2; exit 1; }
    cp "$src" "$out/licenses/$dst"
done
# COPYFILE_DISABLE: bsdtar would otherwise store AppleDouble ._ files for copied xattrs.
COPYFILE_DISABLE=1 tar -czf "$out/licenses.tar.gz" -C "$out" licenses
echo "licenses.sh: wrote $out/licenses.tar.gz from $LICENSE_FILES"
