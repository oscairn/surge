#!/usr/bin/env python3
"""Shared plumbing for the release scripts: read release.conf, hash files."""
import hashlib
import os
import re
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
# Ask git instead of counting '..' hops: repos keep the release scripts at different
# depths (setBfree oscairn/tools/release, surge src/oscairn-clap/tools/release).
REPO = subprocess.run(('git', '-C', HERE, 'rev-parse', '--show-toplevel'),
                      capture_output=True, text=True, check=True).stdout.strip()


def conf():
    """Parse release.conf's KEY=VALUE lines (the same file the sh scripts source)."""
    out = {}
    with open(os.path.join(HERE, 'release.conf'), encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            key, _, value = line.partition('=')
            out[key] = value.strip().strip('"')
    return out


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()
