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
  kh1_lua_library/         Standalone feature modules and their data, one file per
                           feature. The main library re-exports each module's public
                           functions, so requiring either one works.
    enemy_spawns.lua       Enemy spawning. Re-exported as kh1.spawn_enemy /
                           kh1.update_spawn_enemy.
    creature_data.lua      Per-creature char-id/weight/template data extracted offline
                           from room .ard files, used by enemy_spawns.lua.
  VersionCheck.lua         Detects whether the running game is the Steam or EGS build
                           and requires the matching *Global lua file.
  SteamGlobal_*.lua        Per-version memory address tables (from KHPCSpeedrunTools).
  EGSGlobal_*.lua
  json.lua                 Third-party JSON encode/decode helper.
  kh1_native.dll            Native (C++) module, built from native/KH1Native, that bridges
                           into raw game function calls -- something the Lua host itself
                           has no primitive for.

native/KH1Native/          Visual Studio project for kh1_native.dll.
  dllmain.cpp              Lua entry point and shared internals.
  kh1_native.h             Internals shared between dllmain.cpp and the feature files.
  packages/                Self-contained feature source, one file per feature, mirroring
                           the Lua kh1_lua_library/ split.
    spawn_enemy.cpp        Placement-table splice behind kh1.spawn_enemy.

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

### Standalone feature modules

Larger, self-contained features live in their own file under `scripts/io_packages/kh1_lua_library/`
rather than in the main library, and are required with a dotted path.
`kh1_lua_library/enemy_spawns.lua` is the first of these, and the shape to copy for new ones:

```lua
local spawns = require("kh1_lua_library.enemy_spawns")
spawns.spawn_enemy(model_path, motion_path, x, y, z, callback)
spawns.update_spawn_enemy()   -- every frame, from your own _OnFrame
spawns.can_spawn_enemy(model_path)  -- would it be refused? answers without spawning
```

The `kh1_lua_library.lua` file and the `kh1_lua_library/` folder sit side by side on
purpose -- `require("kh1_lua_library")` still resolves to the file, and
`require("kh1_lua_library.<feature>")` resolves into the folder.

The main library `require()`s each feature module and re-exports its public functions, so
`kh1.spawn_enemy(...)` keeps working and nothing downstream has to change. Because that
makes the dependency run library → feature, a feature module that needs something back
out of the main library (enemy spawns needs `get_sora_pos` / `is_in_gummi_garage`) has to
resolve it lazily at call time instead of `require`ing at load time, or the two modules
deadlock on each other mid-load. See the comment at the top of the enemy spawn module.

A feature with a native half puts that in `native/KH1Native/packages/` -- one file per
feature, e.g. `packages/spawn_enemy.cpp` -- leaving `dllmain.cpp` and `kh1_native.h` at the
project root as the shared entry point and internals. The project adds `$(ProjectDir)` to
`AdditionalIncludeDirectories` so sources in `packages/` still `#include "pch.h"` and
`#include "kh1_native.h"` with the same spelling as the root-level ones; changing that
spelling instead would break the `/Yu"pch.h"` precompiled-header match.

### What the library provides

- **Reads**: world/room, animation state, combo limits, stock counts, learned/equipped
  abilities, accessory slots, current inputs, spell effectiveness, Sora's position, gummi
  ship inventory, and more.
- **Writes**: animation speed, combo limits, movement speed, spell effectiveness/cost,
  attack/command data, gummi quantities, granting abilities, and more.
- **Gameplay tweaks**: force scan mode, force combo master, allow summoning anywhere,
  allow midair dodge roll guard, allow air items, multiply summon time,
  damage-cap control (on/off/fixed value, see below).
- **UI**: on-screen input prompts, custom item popup text, spawning prizes, opening/closing
  custom text boxes, playing sound effects.
- **Bit/byte helpers**: `ReadBit`/`WriteBit`/`ReadBits`, KHSCII string conversion
  (`GetKHSCII`), table utilities (`contains`, `get_index`, `merge_tables`).
- **Readiness check**: `can_spawn_enemy()` — ask whether a spawn would work *before*
  attempting it. See below.

### `can_spawn_enemy`

Answers "would `spawn_enemy` refuse right now?" without doing anything:

```lua
local verdict, code, message = kh1.can_spawn_enemy("xa_ex_2020.mdls")
-- "ready" | "retry" (transient) | "unavailable" (needs a restart, or unsupported)
```

It runs the real `spawn_enemy` gates — they're shared functions in
`packages/spawn_enemy.cpp`, not a reimplementation — but allocates nothing, triggers no
asset load and constructs nothing, so it's safe to poll. Called with no model path it
checks only the creature-independent gates and skips the per-creature slot search, which is
much cheaper. A `"ready"` verdict is not a guarantee: refusals that only surface mid-flight
(`blob_invalid`, `ctor_threw`, the room's concurrent-enemy budget) can still come back from
the real call.

`code` is the same short refusal code `spawn_enemy` itself returns (and now passes to its
callback as a fourth argument), so both paths can share one mapping table.

### `set_damage_cap`

Each enemy's `.bd` battle-param block carries a u16 "max damage per hit" cap (blob offset
0x10, runtime battleParams+0xC2, applied in the hit-damage formula before elemental
multipliers). `set_damage_cap(mode)` patches that clamp in place:

- `"on"` (or `true`) — restore normal behavior: each enemy's own cap applies.
- `"off"` (or `false`) — the clamp's JZ becomes a JMP; damage is never capped.
- integer `N` (1..0x7FFFFFFF) — the clamp always applies with `N` for every enemy,
  e.g. `set_damage_cap(1)` makes every hit deal 1 (before multipliers).

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
