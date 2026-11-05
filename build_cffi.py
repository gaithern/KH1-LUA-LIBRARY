#!/usr/bin/env python3
"""
build_cffi.py - rebuilds scripts/io_packages/cffi.dll from source.

cffi.dll is cffi-lua (https://github.com/q66/cffi-lua) built against the lua54.dll this mod
ships in dll/, with libffi linked in statically. The built dll is committed, so this only needs
to run when changing the cffi-lua version.

Needs MSYS2 (https://www.msys2.org) with the UCRT64 toolchain and libffi:
  pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libffi mingw-w64-ucrt-x86_64-tools-git mingw-w64-ucrt-x86_64-ninja
and meson for this Python:
  pip install meson

Set MSYS2_ROOT if MSYS2 is not installed in C:\\msys64.

Usage:
  python build_cffi.py
"""
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).parent
WORK = ROOT / 'build' / 'cffi'
LUA_DLL = ROOT / 'dll' / 'lua54.dll'
OUTPUT = ROOT / 'scripts' / 'io_packages' / 'cffi.dll'

CFFI_REPOSITORY = 'https://github.com/q66/cffi-lua.git'
CFFI_COMMIT = '2621884230c9072ad77f7379609b74c9d6dbeb86'
LUA_SOURCE_URL = 'https://www.lua.org/ftp/lua-5.4.4.tar.gz'


def run(command, working_folder, environment):
    print('>', ' '.join(str(part) for part in command))
    result = subprocess.run(command, cwd=working_folder, env=environment)
    if result.returncode != 0:
        print('\nCommand failed.', file=sys.stderr)
        sys.exit(1)


def find_ucrt64_bin():
    msys2_root = Path(os.environ.get('MSYS2_ROOT', r'C:\msys64'))
    ucrt64_bin = msys2_root / 'ucrt64' / 'bin'
    if not (ucrt64_bin / 'gcc.exe').is_file():
        return None
    return ucrt64_bin


def build_environment(ucrt64_bin, lua_include):
    environment = dict(os.environ)
    environment['PATH'] = str(ucrt64_bin) + os.pathsep + environment['PATH']
    environment['PKG_CONFIG_PATH'] = str(ucrt64_bin.parent / 'lib' / 'pkgconfig')
    environment['CXXFLAGS'] = '-I' + str(lua_include)
    environment['LDFLAGS'] = '-static-libgcc -static-libstdc++'
    return environment


def get_cffi_source(environment):
    source = WORK / 'cffi-lua'
    if not source.is_dir():
        run(['git', 'clone', CFFI_REPOSITORY, str(source)], WORK, environment)
    run(['git', 'checkout', CFFI_COMMIT], source, environment)
    return source


def get_lua_headers():
    archive = WORK / 'lua-5.4.4.tar.gz'
    if not archive.is_file():
        print('> download', LUA_SOURCE_URL)
        urllib.request.urlretrieve(LUA_SOURCE_URL, archive)
    with tarfile.open(archive) as tar:
        tar.extractall(WORK)
    return WORK / 'lua-5.4.4' / 'src'


def make_lua_import_library(source, ucrt64_bin, environment):
    library_folder = WORK / 'lua_import'
    library_folder.mkdir(exist_ok=True)
    shutil.copy(LUA_DLL, library_folder / 'lua54.dll')
    run([ucrt64_bin / 'gendef.exe', 'lua54.dll'], library_folder, environment)
    run([ucrt64_bin / 'dlltool.exe', '-d', 'lua54.def', '-l', 'liblua54.dll.a', '-D', 'lua54.dll'],
        library_folder, environment)
    deps = source / 'deps'
    deps.mkdir(exist_ok=True)
    shutil.copy(library_folder / 'liblua54.dll.a', deps / 'liblua54.dll.a')
    shutil.copy(library_folder / 'liblua54.dll.a', deps / 'liblua54.a')


def main():
    ucrt64_bin = find_ucrt64_bin()
    if ucrt64_bin is None:
        print('MSYS2 UCRT64 gcc not found. Install MSYS2 or set MSYS2_ROOT.', file=sys.stderr)
        sys.exit(1)

    WORK.mkdir(parents=True, exist_ok=True)
    lua_include = get_lua_headers()
    environment = build_environment(ucrt64_bin, lua_include)
    source = get_cffi_source(environment)
    make_lua_import_library(source, ucrt64_bin, environment)

    build_folder = source / 'build'
    if build_folder.is_dir():
        shutil.rmtree(build_folder)
    meson = [sys.executable, '-m', 'mesonbuild.mesonmain']
    run(meson + ['setup', 'build', '-Dlua_version=custom', '-Dtests=false',
                 '-Dbuildtype=release', '--prefer-static'], source, environment)
    run(meson + ['compile', '-C', 'build'], source, environment)

    shutil.copy(build_folder / 'cffi.dll', OUTPUT)
    print('\nWrote', OUTPUT)


if __name__ == '__main__':
    main()
