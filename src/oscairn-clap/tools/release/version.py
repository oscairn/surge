#!/usr/bin/env python3
"""Versions read from this repo's source (no duplicated constant).

descriptor(file) reads a clap_plugin_descriptor_t out of one plugin's source file;
each plugin's own version lives there. package_version() reads the package version
from wherever release.conf's PACKAGE_VERSION_FROM points: "descriptor:<file>" takes
it from that plugin's descriptor (repos that publish release == plugin version),
"cmake:<file>" from a project(<name> VERSION X.Y.Z) line. Used by manifest.py (to
cross-check the tag) and by the release workflow (a dry run has no tag, so it must
derive one from the package version).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from release_conf import REPO, conf

DESCRIPTOR = re.compile(
    r'CLAP_VERSION(?:_INIT)?,\s*'
    r'"(?P<id>[^"]*)",\s*"(?P<name>[^"]*)",\s*"(?P<vendor>[^"]*)",\s*"(?P<url>[^"]*)",\s*'
    r'"[^"]*",\s*"[^"]*",\s*"(?P<version>[^"]*)"')

PROJECT = re.compile(r'project\([\w-]+ VERSION ([0-9]+\.[0-9]+\.[0-9]+)')


def descriptor(path):
    text = open(os.path.join(REPO, path), encoding='utf-8').read()
    m = DESCRIPTOR.search(text)
    if not m:
        sys.exit(f'version.py: no clap_plugin_descriptor_t found in {path}')
    return m.groupdict()


def package_version():
    kind, _, target = conf()['PACKAGE_VERSION_FROM'].partition(':')
    if kind == 'descriptor':
        return descriptor(target)['version']
    if kind == 'cmake':
        m = PROJECT.search(open(os.path.join(REPO, target), encoding='utf-8').read())
        if not m:
            sys.exit(f'version.py: no project(<name> VERSION X.Y.Z) found in {target}')
        return m.group(1)
    sys.exit(f"version.py: PACKAGE_VERSION_FROM must be descriptor:<file> or cmake:<file>, "
             f"not {conf()['PACKAGE_VERSION_FROM']!r}")


if __name__ == '__main__':
    print(package_version())
