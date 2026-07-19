#!/usr/bin/env python3
"""Generate mod.yml from the files this mod actually ships."""

from pathlib import Path

HEADER = """\
title: KH1 Lua Library
originalAuthor: Gicu
description: Library of Lua functions for use in rando and other mods
assets:
"""

ROOT = Path(__file__).parent
OUTPUT = ROOT / "mod.yml"

ASSET_DIRS = [
    "scripts/io_packages",
    "dll",
]


def relative_posix(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def main():
    files = []
    for asset_dir in ASSET_DIRS:
        d = ROOT / asset_dir
        if not d.is_dir():
            print(f"Warning: asset directory not found: {d}")
            continue
        files.extend(f for f in d.rglob("*") if f.is_file())

    if not files:
        print("No asset files found.")
        return

    files.sort(key=relative_posix)

    lines = [HEADER]
    for f in files:
        rel = relative_posix(f)
        lines.append(f"- name: {rel}\n  method: copy\n  source:\n  - name: {rel}\n")

    OUTPUT.write_text("".join(lines), encoding="utf-8")
    print(f"Generated {OUTPUT} with {len(files)} entries.")


if __name__ == "__main__":
    main()
