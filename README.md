# KH1 Lua Library

A shared Lua library of helper functions for reading, writing, and manipulating
Kingdom Hearts 1 Final Mix (PC) memory, for use in [randomizer](https://github.com/tommadness/KH1Randomizer)
and other [LuaBackend](https://github.com/Sirius902/LuaBackend) mods. Supports both the
Steam and EGS builds.

## Layout

```
scripts/io_packages/
  kh1_lua_library.lua     Main library -- the public API. Returns a table of functions
                           (see below) for other scripts to require().
  VersionCheck.lua         Detects whether the running game is the Steam or EGS build
                           and requires the matching *Global lua file.
  SteamGlobal_*.lua        Per-version memory address tables (from KHPCSpeedrunTools).
  EGSGlobal_*.lua
  json.lua                 Third-party JSON encode/decode helper.
  kh1_native.dll            Native (C++) module, built from native/KH1Native, that bridges
                           into raw game function calls -- something the Lua host itself
                           has no primitive for.

native/KH1Native/          Visual Studio project for kh1_native.dll.

mod.yml                    OpenKH mod manifest listing the files this mod installs.
```

## Usage

In your own LuaBackend script:

```lua
require("VersionCheck")
local kh1 = require("kh1_lua_library")

local world = kh1.get_world()
kh1.give_sora_ability(some_ability_value)
```

`VersionCheck.lua` must run first -- it detects the game version and loads the matching
`SteamGlobal_*` / `EGSGlobal_*` address table that `kh1_lua_library.lua` depends on.

Only files directly under `scripts/` are auto-run and get `_OnFrame` called by
LuaBackend; everything in `scripts/io_packages/` is a library meant to be `require()`d
by those top-level scripts, not run on its own.

### What the library provides

- **Reads**: world/room, animation state, combo limits, stock counts, learned/equipped
  abilities, accessory slots, current inputs, spell effectiveness, Sora's position, gummi
  ship inventory, and more.
- **Writes**: animation speed, combo limits, movement speed, spell effectiveness/cost,
  attack/command data, gummi quantities, granting abilities, and more.
- **Gameplay tweaks**: force scan mode, force combo master, allow summoning anywhere,
  allow midair dodge roll guard, allow air items, multiply summon time.
- **UI**: on-screen input prompts, custom item popup text, spawning prizes, opening/closing
  custom text boxes, playing sound effects.
- **Bit/byte helpers**: `ReadBit`/`WriteBit`/`ReadBits`, KHSCII string conversion
  (`GetKHSCII`), table utilities (`contains`, `get_index`, `merge_tables`).

Anything that needs to call into real game code (rather than just read/write memory) is
routed through `kh1_native.dll` via `require("kh1_native")`.

## Building the native module

`kh1_native.dll` is prebuilt and committed to the repo (`scripts/io_packages/kh1_native.dll`),
so most contributors won't need to rebuild it. If you do change `native/KH1Native/dllmain.cpp`:

```powershell
native/KH1Native/build.ps1
```

This requires Visual Studio (with MSBuild) installed. The script locates `MSBuild.exe` via
`vswhere` and builds the `Release|x64` configuration, producing the DLL that gets copied
into `scripts/io_packages/`.

## Installing

This repo is structured as an OpenKH mod (see `mod.yml`) -- drop it into your mods folder
per your OpenKH/LuaBackend setup, or reference it as a dependency from another mod.
