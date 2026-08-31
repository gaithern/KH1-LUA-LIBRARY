# KH1 Lua Library

A shared Lua library of helper functions for reading, writing, and manipulating
Kingdom Hearts 1 Final Mix (PC) memory, for use in [randomizer](https://github.com/tommadness/KH1Randomizer)
and other [LuaBackend](https://github.com/Sirius902/LuaBackend) mods. Supports both the
Steam and EGS builds.

## Repository Layout
Below you'll find key components of the repository and their descriptions.

- `dll/*`
| File | Description |
| :--- | ----------: |
| `lua54.dll` | Bundled lua module |
- `native/KH1Native/*`
| File | Description |
| :--- | ----------: |
| `call_bridge.cpp`| Handler for calling in-exe functions.  Handles varying numbers of input arugments. |
| `dllmain.cpp`| Main entry point. |
| `evdl_syscall.cpp` | Supports calls EVDL Syscalls for C++/Lua.  Creates a dummy scriptCtx for execution. |
| `log.cpp` | Handles writing to a shared log for debugging purposes. |
| `lua_api.cpp` | Identifies the Lua module and populate Lua C function pointers. |
| `lua_bindings.cpp` | Exposes C++ to be used on the Lua side. |
| `process_memory.cpp` | Toolset for working with game memory, including memory allocation, safe writes, etc. |
- `scripts/io_packages/*`
| File | Description |
| :--- | ----------: |
| `helpers/*` | Generic helper lua modules. |
| `modules/*` | Containers for complex lua function(s) pertaining to one subject area. |
| `EGSGlobal_1_0_0_10.lua` | Mapped RVAs for KH1 Epic Game Store v1.0.0.10. |
| `json.lua` | Generic lua helper.  From [RXI's json.lua](https://github.com/rxi/json.lua). |
| `kh1_lua_library.lua` | Main script to be used for outside mods.  Contains callers for all functionality. |
| `kh1_native.dll` | Compiled binary from `native/KH1Native/*`. |
| `SteamGlobal_1_0_0_2.lua` | Mapped RVAs for KH1 Epic Game Store v1.0.0.2. |
| `VersionCheck.lua` | [KHPCSpeedrunTools](https://github.com/Denhonator/KHPCSpeedrunTools/blob/main/1FMMods/scripts/io_packages/VersionCheck.lua)'s methodology of determining active game version. |
- `build.py`
  - Compiles `kh1_native.dll` from its source in `native/KH1Native/*`.
  - Creates relevant `mod.yml` by calling `generate_mod_yml.py`
- `generate_mod_yml.py`
  - Generates `mod.yml` by iterating through `scripts/io_packages/`.
- `icon.png`
  - Icon for the mod manager.
- `LICENSE`
  - License for the project
- `mod.yml`
  - OpenKH configuration file for what relevant files are here to compile the mod for end users.
- `README.md`
  - This doc.

## Building mod/making changes
If any script or CPP code needs to be changed, those changes should be accurately picked up and compiled by running `python build.py`.

## Currently shipped features for modders using this project
All functions below are available on the table returned by `require("kh1_lua_library")`.

### Helpers
| Function | Arguments | Returns | Description |
| :------- | :-------- | :------ | :---------- |
| `byte_to_bits` | `byte` | array of 8 bits | Converts a byte to an array of 8 bits (LSB first). |
| `bits_to_byte` | `bits` | byte | Converts an array of 8 bits (LSB first) back to a byte. |
| `ReadBits` | `address, absolute` | array of 8 bits | Reads the byte at `address` and returns it as a bit array. |
| `ReadBit` | `address, bit_num, absolute` | `0` or `1` | Reads a single bit from the byte at `address`. |
| `WriteBit` | `address, bit_num, value, absolute` | none | Writes a single bit into the byte at `address`. |
| `contains` | `tbl, val` | boolean | Returns true if `val` exists in table `tbl`. |
| `get_index` | `tbl, val` | index or `nil` | Returns the index of `val` in `tbl`, if present. |
| `merge_tables` | `t1, t2` | table | Appends the values of `t2` onto `t1` and returns it. |
| `GetKHSCII` | `string` | byte array | Converts a string to its KHSCII byte encoding (null-terminated). |

### Getters
| Function | Arguments | Returns | Description |
| :------- | :-------- | :------ | :---------- |
| `get_world` | none | byte | Current world ID. |
| `get_room` | none | byte | Current room ID. |
| `get_gummi_select` | none | byte | Current gummi menu selection. |
| `get_animation_speed` | none | float | Sora's current animation speed. |
| `get_current_animation` | none | byte | Sora's current animation ID. |
| `get_animation_time` | none | float | How long Sora has been in his current animation. |
| `get_ground_combo_length_limit` | none | byte | Sora's current ground combo length limit. |
| `get_air_combo_length_limit` | none | byte | Sora's current air combo length limit. |
| `get_stock` | none | array of 255 bytes | The full item inventory. |
| `get_stock_at_index` | `index` | byte | Item quantity at inventory index (1-based). |
| `get_sora_abilities` | none | array | All ability IDs Sora has acquired. |
| `get_shared_abilities` | none | array | All shared ability IDs the party has acquired. |
| `get_equipped_sora_abilities` | none | array | Only the Sora abilities currently equipped. |
| `get_equipped_shared_abilities` | none | array | Only the shared abilities currently equipped. |
| `get_soras_accessory_slots` | none | byte | Sora's max accessory slot count. |
| `get_soras_equipped_accessories` | none | array | Sora's currently equipped accessories. |
| `get_inputs` | none | array of 4 bytes | Raw input device state. |
| `get_spell_effectiveness` | `spell` | byte | Effectiveness of a spell by name (e.g. `"Fire"`, `"Curaga"`). |
| `get_current_hits` | none | byte | Sora's current combo hit count. |
| `get_sora_pos` | none | table `{X, Y, Z}` | Sora's current position. |
| `get_gummi_qty_at_index` | `index` | byte | Gummi item quantity at inventory index (1-based). |

### Setters
| Function | Arguments | Returns | Description |
| :------- | :-------- | :------ | :---------- |
| `set_animation_speed` | `animation_speed` | none | Overwrites Sora's current animation speed. |
| `set_ground_combo_length_limit` | `limit` | none | Overwrites Sora's ground combo length limit. |
| `set_air_combo_length_limit` | `limit` | none | Overwrites Sora's air combo length limit. |
| `set_stock_at_index` | `index, qty` | none | Sets item quantity at inventory index (capped at 99). |
| `set_sora_walk_speed` | `walk_speed` | none | Sets Sora's walk speed. |
| `set_sora_run_speed` | `run_speed` | none | Sets Sora's run speed. |
| `set_spell_effectiveness` | `spell, value` | none | Sets a spell's effectiveness by name. |
| `set_spell_cost` | `spell, value` | none | Sets a spell's cost. Valid values: 15 = 1/2 CP, 30 = 1 CP, 100/200/300 = 1/2/3 MP. |
| `set_attack_animation_data` | `index, animation_data` | none | Overwrites an attack animation data entry (byte array). |
| `set_command_data` | `index, command_data` | none | Overwrites a command menu data entry (byte array). |
| `set_gummi_qty_at_index` | `index, qty` | none | Sets gummi item quantity at inventory index (capped at 99). |

### Advanced
| Function | Arguments | Returns | Description |
| :------- | :-------- | :------ | :---------- |
| `make_sora_actionable` | none | none | Clears the flag on Sora's object that locks him out of acting. |
| `calculate_ground_combo_limit` | none | integer | What Sora's ground combo limit should be, based on equipped Combo Plus abilities. |
| `calculate_air_combo_limit` | none | integer | What Sora's air combo limit should be, based on equipped Air Combo Plus abilities. |
| `enable_ability` | `ability` | none | Force-enables an ability by name (e.g. `"Dodge Roll"`, `"Guard"`) even if unowned/unequipped. |
| `give_sora_ability` | `ability_value` | none | Grants Sora an ability by ID (unequipped). |
| `give_shared_ability` | `shared_ability_value` | none | Grants the party a shared ability by ID (unequipped). |
| `force_scan` | `on` | none | ASM patch that forces Scan on/off. Credits to KSX. |
| `force_combo_master` | `on` | none | ASM patch that forces Combo Master on/off. Credits to KSX. |
| `allow_summon_anywhere` | `on` | none | ASM patch that allows summons outside of combat. Credits to KSX. |
| `allow_midair_dodge_roll_guard` | `on` | none | ASM patch that allows dodging/guarding in mid air. Credits to KSX. |
| `allow_air_items` | `on` | none | ASM patch that allows item use in mid air (currently affects mid-air spells too). Credits to KSX. |
| `multiply_summon_time` | `mult` | none | Multiplies the summon duration factor. Credits to KSX. |
| `is_pressed` | `button_array, only` | boolean | True if all buttons in `button_array` (e.g. `{"L1", "Triangle"}`) are pressed. With `only`, no other buttons may be held. |
| `is_in_gummi_garage` | none | boolean | True anywhere in the Gummi ship menus (needs a rename — not just the garage). |
| `is_in_gummi` | none | boolean | True while in a Gummi mission. |
| `sora_koed` | none | boolean | True if Sora's current HP is 0. |
| `ko_sora` | none | none | Triggers the in-game event script that KOs Sora. |
| `heartless_angel_sora` | none | none | Sets Sora's HP to 1 and MP to 0. |
| `set_damage_cap` | `mode` | boolean | `"on"`/`true`: normal per-enemy damage caps. `"off"`/`false`: never cap. Integer N: cap every hit at N. |
| `spawn_prize` | `item_id` | boolean | Calls the in-game function to spawn a map prize at Sora's position. |
| `spawn_enemy` | `model_path, x, y, z` | `boolean, reason` | Spawns an enemy by model path. Asynchronous: returns `false, "loading"` while assets load — call again each frame until it returns `true`. Coordinates default to Sora's position. |
| `play_se2` | `se_id, param_2` | boolean | Plays a sound effect using the in-game function. |
| `show_prompt` | `input_title, input_party, duration, colour` | boolean | Shows level-up-style prompts over party members. `input_title` and `input_party` are tables indexed 1-3 (Sora/ally 1/ally 2); each party entry is 1-2 lines of text. |
| `show_custom_item_popup` | `text` | boolean | Forces a map-prize pickup popup with custom text. |
| `open_text_box` | `text, window_id, duration_seconds, style, x, y, width, height` | boolean | Opens a text box with custom text. Only `text` is required; `duration_seconds` auto-closes it (requires `update_text_boxes`). |
| `close_text_box` | `window_id` | boolean | Closes an open text box (`window_id` defaults to 1). |
| `update_text_boxes` | none | none | Handles timed closing of open text boxes; call every frame from `_OnFrame`. |
| `set_text_box_style` | `window_id, style` | boolean | Sets a text box's style. |
| `set_text_box_position` | `window_id, x, y` | boolean | Sets a text box's on-screen position. |
| `set_text_box_size` | `window_id, width, height` | boolean | Sets a text box's width/height. |
