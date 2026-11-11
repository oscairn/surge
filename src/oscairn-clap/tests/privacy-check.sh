#!/bin/bash
# Runs a command with HOME set to a fresh folder holding the user's privacy-protected folders
# (Documents, Desktop, Downloads and others), and fails if the command touches any of them or
# leaves anything new under HOME outside the paths given with -a.
#
#   privacy-check.sh [-a <path under HOME the command may create>]... <command> [args...]
#
# macOS's NSFileManager finds the home folder from CFFIXED_USER_HOME, not HOME, so both are set.
# On macOS the command also runs under sandbox-exec, killed at its first access to a protected
# folder (or the real home's), reads and lookups of missing files in them included: each
# protected folder is a symbolic link, and resolving it counts as an access. Elsewhere only
# what the command leaves behind is checked.
set -euo pipefail
allow=()
while [ "${1-}" = -a ]; do allow+=("$2"); shift 2; done
[ $# -gt 0 ] || { echo "usage: $0 [-a path]... command [args...]" >&2; exit 2; }

realhome=$(eval echo "~$(id -un)")
home=$(mktemp -d "${TMPDIR:-/tmp}/privacy-home.XXXXXX")
home=$(cd "$home" && pwd -P)
trap 'rm -rf "$home"' EXIT
protected=(Documents Desktop Downloads Music Movies Pictures "Library/Mobile Documents")
mkdir -p "$home/Library" "$home/.protected"
for d in "${protected[@]}"; do
    t="$home/.protected/${d//\//-}"
    mkdir "$t"
    echo sentinel > "$t/sentinel"
    ln -s "$t" "$home/$d"
done

if [ "$(uname)" = Darwin ]; then fmt=(-f '%m %z %Sp ./%N'); else fmt=(-c '%Y %s %A ./%n'); fi
snapshot() { (cd "$home" && find . -mindepth 1 -exec stat "${fmt[@]}" {} + | sed 's| \./\./| ./|' | sort); }
before=$(snapshot)

rc=0
if [ "$(uname)" = Darwin ]; then
    rules=""
    for d in "${protected[@]}"; do rules+=" (subpath \"$home/$d\") (subpath \"$realhome/$d\")"; done
    profile="(version 1) (allow default) (deny file*$rules (subpath \"$home/.protected\") (with send-signal SIGKILL))"
    HOME="$home" CFFIXED_USER_HOME="$home" sandbox-exec -p "$profile" "$@" || rc=$?
else
    HOME="$home" "$@" || rc=$?
fi
if [ $rc -eq 137 ]; then
    echo "privacy-check: FAILED: killed at an access to a protected folder" >&2
    exit 1
fi
[ $rc -eq 0 ] || { echo "privacy-check: FAILED: the command exited with $rc" >&2; exit 1; }

# What appeared or changed under HOME, outside the allowed paths
changed=$(comm -13 <(echo "$before") <(snapshot) | while read -r line; do
    p=${line#* ./}
    ok=0
    for a in "${allow[@]+"${allow[@]}"}"; do
        case "$p/" in ("$a"/*) ok=1 ;; esac
        case "$a/" in ("$p"/*) ok=1 ;; esac # a parent the allowed path needed
    done
    [ $ok = 1 ] || echo "$p"
done)
if [ -n "$changed" ]; then
    echo "privacy-check: FAILED: changed or created under HOME:" >&2
    echo "$changed" >&2
    exit 1
fi
echo "privacy-check: passed"
