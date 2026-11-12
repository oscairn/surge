#!/usr/bin/env python3
"""Write oscairn-release.json (the release manifest) from the files in a dist directory.

    manifest.py <tag> <distdir> [outfile]        outfile: <distdir>/oscairn-release.json

<tag> is this repo's release tag, oscairn-vX.Y.Z (upstream v* tags exist, hence the
prefix; until the packages schema adopts it, source.tag stays the real tag name).
The package version comes from the tag and must equal the CMake project version
(version.py). The plugins[] rows come from every plugin source file in release.conf's
PLUGINS; each plugin's own version and name are read from its clap_plugin_descriptor_t.
The builds rows come from every <NAME>-<platform>.clap found in <distdir>.
"""
import datetime
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from release_conf import REPO, conf, sha256
from version import descriptor, package_version

PLATFORMS = {'linux-arm64', 'linux-x86_64', 'macos-arm64', 'macos-x86_64', 'windows-x86_64'}


def git(*args):
    return subprocess.run(('git', '-C', REPO) + args, capture_output=True, text=True,
                          check=True).stdout.strip()


def clap_version():
    text = open(os.path.join(REPO, conf()['CLAP_VERSION_FILE']), encoding='utf-8').read()
    v = {k: re.search(rf'#define CLAP_VERSION_{k} (\d+)', text).group(1)
         for k in ('MAJOR', 'MINOR', 'REVISION')}
    return f"{v['MAJOR']}.{v['MINOR']}.{v['REVISION']}"


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit('usage: manifest.py <tag> <distdir> [outfile]')
    c = conf()
    tag, dist = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) == 4 else os.path.join(dist, 'oscairn-release.json')

    m = re.fullmatch(re.escape(c['TAG_PREFIX']) + r'v([0-9]+\.[0-9]+\.[0-9]+)', tag)
    if not m:
        sys.exit(f"manifest.py: tag {tag!r} is not {c['TAG_PREFIX']}vX.Y.Z")
    version = m.group(1)

    package = package_version()
    if package != version:
        sys.exit(f"manifest.py: tag {tag} says {version} but the package "
                 f"({c['PACKAGE_VERSION_FROM']}) says {package}")

    plugins = []
    for pair in c['PLUGINS'].split():
        path, kind = pair.split('=', 1)
        plugin = descriptor(path)
        plugins.append({'format': 'clap', 'id': plugin['id'], 'version': plugin['version'],
                        'name': plugin['name'], 'kind': kind})

    builds = []
    for name in sorted(os.listdir(dist)):
        m = re.fullmatch(re.escape(c['NAME']) + r'-([a-z0-9_]+-[a-z0-9_]+)\.clap', name)
        if not m:
            continue
        platform = m.group(1)
        if platform not in PLATFORMS:
            sys.exit(f'manifest.py: {name}: platform {platform!r} is not in the accepted set '
                     f'({", ".join(sorted(PLATFORMS))})')
        path = os.path.join(dist, name)
        builds.append({'platform': platform, 'file': name,
                       'sha256': sha256(path), 'size': os.path.getsize(path)})
    if not builds:
        sys.exit(f"manifest.py: no {c['NAME']}-<platform>.clap found in {dist}")

    license_files = []
    for pair in c['LICENSE_FILES'].split():
        path = os.path.join(dist, 'licenses', pair.split('=', 1)[1])
        if not os.path.isfile(path):
            sys.exit(f'manifest.py: {path} missing; run licenses.sh first')
        license_files.append({'file': 'licenses/' + pair.split('=', 1)[1], 'sha256': sha256(path)})

    assets = []
    for dep in c.get('ASSET_DEPS', '').split():
        dep_id, _, dep_version = dep.partition('@')
        assets.append({'id': dep_id, 'version': dep_version})

    revision = git('rev-parse', 'HEAD')
    if not re.fullmatch(r'[0-9a-f]{40}', revision):
        sys.exit(f'manifest.py: HEAD is not a full commit hash: {revision!r}')
    oscairn_clap = json.load(open(os.path.join(REPO, c['OSCAIRN_CLAP_UPSTREAM']), encoding='utf-8'))['commit']

    manifest = {
        'format': 'oscairn.plugin-release',
        'version': 1,
        'package': {'id': c['PACKAGE_ID'], 'version': version, 'name': c['PACKAGE_NAME'],
                    'summary': c['PACKAGE_SUMMARY']},
        'plugins': plugins,
        'builds': builds,
        'source': {'url': c['SOURCE_URL'], 'revision': revision, 'tag': tag},
        'license': c['LICENSE'],
        'licenseFiles': license_files,
        'dependencies': {'assets': assets},
        'clap': clap_version(),
        'oscairnClap': oscairn_clap,
    }
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=2)
        f.write('\n')
    print(f"manifest.py: wrote {out} ({len(builds)} build(s), {len(plugins)} plugin(s), "
          f"package {version}, commit {revision[:12]}, "
          f"built {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%d})")


if __name__ == '__main__':
    main()
