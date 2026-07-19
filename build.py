#!/usr/bin/env python3
"""
build.py — full build: compile KH1Native, then regenerate mod.yml.

Runs the KH1Native MSBuild project (its Release|x64 output drops
kh1_native.dll straight into scripts/io_packages/), then
generate_mod_yml.py.

Usage:
  python build.py

Set MSBUILD_PATH to point at a specific MSBuild.exe if auto-detection via
vswhere doesn't find your Visual Studio install.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).parent
NATIVE_SLN = ROOT / 'native' / 'KH1Native' / 'KH1Native.sln'


def find_msbuild():
    """Locate MSBuild.exe: MSBUILD_PATH env var, vswhere, then PATH."""
    env_path = os.environ.get('MSBUILD_PATH')
    if env_path and Path(env_path).is_file():
        return env_path

    program_files_x86 = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    vswhere = Path(program_files_x86) / 'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    if vswhere.is_file():
        result = subprocess.run(
            [str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild',
             '-find', r'MSBuild\**\Bin\MSBuild.exe'],
            capture_output=True, text=True)
        candidates = [line for line in result.stdout.splitlines() if line.strip()]
        if candidates and Path(candidates[0]).is_file():
            return candidates[0]

    return shutil.which('MSBuild.exe') or shutil.which('msbuild')


def build_native():
    """Compile KH1Native (Release|x64). Returns False only on a real build failure;
    a missing MSBuild is reported as a warning and treated as non-fatal so contributors
    without Visual Studio can still regenerate mod.yml."""
    msbuild = find_msbuild()
    if not msbuild:
        print('\nWarning: MSBuild not found, skipping KH1Native build.', file=sys.stderr)
        print('Install Visual Studio (with the C++ workload) or set MSBUILD_PATH.', file=sys.stderr)
        return True

    result = subprocess.run([
        msbuild, str(NATIVE_SLN),
        '/p:Configuration=Release', '/p:Platform=x64', '/m',
    ])
    return result.returncode == 0


def main():
    if not build_native():
        print('\nKH1Native build failed.', file=sys.stderr)
        sys.exit(1)

    gen_yml = subprocess.run([sys.executable, str(ROOT / 'generate_mod_yml.py')])
    sys.exit(gen_yml.returncode)


if __name__ == '__main__':
    main()
