#!/usr/bin/env python3
"""
build_deps.py - fetch the third-party pieces KH1Native needs.

- SafetyHook (amalgamated, with Zydis): compiled into kh1_native.dll. Extracted
  into native/KH1Native/external/safetyhook/ (gitignored).
- TinyCC: libtcc.dll, which kh1_native loads at runtime to compile hook .c
  files. It ships with the mod, so it is copied into scripts/io_packages/
  (committed, like kh1_native.dll) together with its LGPL license.

Re-runs are no-ops unless a pinned version or file list changes; a stamp file
per dependency records what the current tree was built from. build.py runs
this before MSBuild, and KH1Native's pre-build step runs it too.

Usage:
  python build_deps.py
  python build_deps.py --force    # re-download everything
"""
import argparse
import hashlib
import io
import json
import shutil
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).parent
EXTERNAL = ROOT / 'native' / 'KH1Native' / 'external'
IO_PACKAGES = ROOT / 'scripts' / 'io_packages'

# Each file maps a path inside the archive to a destination under ROOT.
DEPENDENCIES = [
    {
        'name': 'SafetyHook',
        'version': 'v0.7.0',
        'url': 'https://github.com/cursey/safetyhook/releases/download/v0.7.0/safetyhook-amalgamated-zydis.zip',
        'sha256': None,
        'stamp': EXTERNAL / 'safetyhook' / '.build_deps.json',
        'files': {
            'safetyhook.hpp': EXTERNAL / 'safetyhook' / 'safetyhook.hpp',
            'safetyhook.cpp': EXTERNAL / 'safetyhook' / 'safetyhook.cpp',
            'Zydis.h': EXTERNAL / 'safetyhook' / 'Zydis.h',
            'Zydis.c': EXTERNAL / 'safetyhook' / 'Zydis.c',
        },
        'extra': {},
    },
    {
        'name': 'TinyCC',
        'version': '0.9.27',
        'url': 'https://download.savannah.gnu.org/releases/tinycc/tcc-0.9.27-win64-bin.zip',
        'sha256': '34a721949a2583fdff725312da092fa0f5f1f284b702e6f811c6954714faabb2',
        'stamp': EXTERNAL / 'tinycc' / '.build_deps.json',
        'files': {
            'tcc/libtcc.dll': IO_PACKAGES / 'libtcc.dll',
        },
        # The release zip carries no license file; fetched alongside it.
        'extra': {
            'https://raw.githubusercontent.com/TinyCC/tinycc/release_0_9_27/COPYING': IO_PACKAGES / 'libtcc_LICENSE.txt',
        },
    },
]


def fingerprint(dep):
    """Hash of the pinned version, sources and file list; changing any re-fetches."""
    payload = json.dumps({
        'version': dep['version'], 'url': dep['url'], 'sha256': dep['sha256'],
        'files': {k: str(v.relative_to(ROOT)) for k, v in dep['files'].items()},
        'extra': {k: str(v.relative_to(ROOT)) for k, v in dep['extra'].items()},
    }, sort_keys=True).encode()
    return hashlib.sha256(payload).hexdigest()


def outputs(dep):
    return list(dep['files'].values()) + list(dep['extra'].values())


def is_current(dep):
    """True if the outputs already match the pinned version."""
    stamp = dep['stamp']
    if not stamp.is_file():
        return False
    try:
        data = json.loads(stamp.read_text(encoding='utf-8'))
    except (json.JSONDecodeError, OSError):
        return False
    return data.get('fingerprint') == fingerprint(dep) and all(p.is_file() for p in outputs(dep))


def download(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'KH1-LUA-LIBRARY-build'})
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read()


def ensure(dep, force=False):
    """Make the dependency's outputs match its pinned version."""
    name, version = dep['name'], dep['version']
    if not force and is_current(dep):
        print(f'{name} {version} is up to date')
        return

    print(f'Fetching {name} {version} from {dep["url"]}')
    try:
        archive_bytes = download(dep['url'])
        extras = {dest: download(url) for url, dest in dep['extra'].items()}
    except urllib.error.URLError as error:
        raise RuntimeError(
            f'Could not download {name} {version}: {error.reason}\n'
            'The first build needs network access; after that the extracted copy is reused.') from error

    if dep['sha256'] and hashlib.sha256(archive_bytes).hexdigest() != dep['sha256']:
        raise RuntimeError(f'{name} {version}: download does not match the pinned SHA-256')

    with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
        names = set(archive.namelist())
        missing = [inner for inner in dep['files'] if inner not in names]
        if missing:
            raise RuntimeError(
                f'Not present in {name} {version}: {", ".join(missing)}\n'
                'Upstream may have moved or renamed them; update DEPENDENCIES.')
        for inner, dest in dep['files'].items():
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(archive.read(inner))
    for dest, data in extras.items():
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)

    dep['stamp'].parent.mkdir(parents=True, exist_ok=True)
    dep['stamp'].write_text(json.dumps({'version': version, 'fingerprint': fingerprint(dep)}, indent=2),
                            encoding='utf-8')
    print(f'Extracted {name} {version}: {", ".join(str(p.relative_to(ROOT)) for p in outputs(dep))}')


def main():
    parser = argparse.ArgumentParser(description='Fetch the third-party pieces KH1Native needs.')
    parser.add_argument('--force', action='store_true', help='re-download even if already up to date')
    args = parser.parse_args()

    try:
        for dep in DEPENDENCIES:
            ensure(dep, force=args.force)
    except (RuntimeError, OSError) as error:
        print(f'\nbuild_deps.py: {error}', file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
