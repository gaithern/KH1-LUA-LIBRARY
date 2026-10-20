#!/usr/bin/env python3
"""
build.py - regenerates mod.yml, optionally rebuilding cffi.dll first.

cffi.dll is committed, so a normal build only regenerates mod.yml.
Pass --cffi to rebuild cffi.dll from source first (see build_cffi.py for what that needs).

Usage:
  python build.py
  python build.py --cffi
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent


def main():
    if '--cffi' in sys.argv:
        cffi_build = subprocess.run([sys.executable, str(ROOT / 'build_cffi.py')])
        if cffi_build.returncode != 0:
            print('\ncffi.dll build failed.', file=sys.stderr)
            sys.exit(1)

    gen_yml = subprocess.run([sys.executable, str(ROOT / 'generate_mod_yml.py')])
    sys.exit(gen_yml.returncode)


if __name__ == '__main__':
    main()
