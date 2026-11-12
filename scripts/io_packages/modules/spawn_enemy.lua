---@diagnostic disable: undefined-global

local cffi = require("cffi")
local native = require("helpers.native")
local call_redirect = require("helpers.call_redirect")
local creature_data = require("modules.creature_data")

cffi.cdef[[
    typedef uint64_t (*SpawnEnemyReverseBlob)(uint64_t blob, uint64_t second, uint64_t third, uint64_t fourth);
    typedef void (*SpawnEnemyLoadComplete)(uint32_t size, uint64_t unused, uint64_t destination);
    uint32_t GetCurrentThreadId(void);
]]

local function log(message)
    native.log_debug("[spawn_enemy] " .. message)
end

local function round_up(value, multiple)
    return ((value + multiple - 1) // multiple) * multiple
end

-- ############################################################ --
-- # Buffer registry: which of our buffers holds which motion   # --
-- ############################################################ --

local REGISTRY_KEY = "kh1_species_redirect_v3"
local REGISTRY_SIZE = 0x4000
local REGISTRY_ROW_COUNT_OFFSET = 0x28
local REGISTRY_FIRST_ROW_OFFSET = 0x100
local ROW_SIZE = 0x40
local ROW_BUFFER_START_OFFSET = 0x00
local ROW_BUFFER_END_OFFSET = 0x08
local ROW_MOTION_OFFSET = 0x10
local ROW_IN_USE_OFFSET = 0x18
local MAX_ROWS = (REGISTRY_SIZE - REGISTRY_FIRST_ROW_OFFSET) // ROW_SIZE

local registry = nil

local function row_address(row)
    return registry + REGISTRY_FIRST_ROW_OFFSET + row * ROW_SIZE
end

local function row_count()
    local count = ReadInt(registry + REGISTRY_ROW_COUNT_OFFSET, true)
    if count > MAX_ROWS then
        count = MAX_ROWS
    end
    return count
end

local function row_in_use(row)
    return ReadInt(row_address(row) + ROW_IN_USE_OFFSET, true) ~= 0
end

local function row_buffer_start(row)
    return ReadLong(row_address(row) + ROW_BUFFER_START_OFFSET, true)
end

local function row_buffer_end(row)
    return ReadLong(row_address(row) + ROW_BUFFER_END_OFFSET, true)
end

local function row_motion(row)
    return ReadLong(row_address(row) + ROW_MOTION_OFFSET, true)
end

local function set_row_motion(row, motion_address)
    WriteLong(row_address(row) + ROW_MOTION_OFFSET, motion_address, true)
end

local function claim_row(buffer_start, buffer_end)
    local count = row_count()
    local free_row = nil
    for row = 0, count - 1 do
        if not row_in_use(row) then
            free_row = row
            break
        end
    end
    if free_row == nil then
        if count >= MAX_ROWS then
            return nil
        end
        free_row = count
        WriteInt(registry + REGISTRY_ROW_COUNT_OFFSET, count + 1, true)
    end
    local address = row_address(free_row)
    WriteLong(address + ROW_BUFFER_START_OFFSET, buffer_start, true)
    WriteLong(address + ROW_BUFFER_END_OFFSET, buffer_end, true)
    WriteLong(address + ROW_MOTION_OFFSET, 0, true)
    WriteInt(address + ROW_IN_USE_OFFSET, 1, true)
    return free_row
end

local function release_row(row)
    if row ~= nil then
        WriteInt(row_address(row) + ROW_IN_USE_OFFSET, 0, true)
    end
end

local function rows_in_use()
    local rows = {}
    for row = 0, row_count() - 1 do
        if row_in_use(row) then
            table.insert(rows, row)
        end
    end
    return rows
end

local function row_containing(address)
    for row = 0, row_count() - 1 do
        if row_in_use(row) then
            if address >= row_buffer_start(row) and address < row_buffer_end(row) then
                return row
            end
        end
    end
    return nil
end

-- ############################################################ --
-- # Game hooks: motion lookup and file-load finished           # --
-- ############################################################ --

local original_reverse_blob = nil
local load_complete_callback = nil
local load_complete_address = 0
local finished_loads = {}
local load_thread_id = 0
local warned_about_thread = false
local installed = false

local function reverse_blob(blob, second, third, fourth)
    local row = row_containing(cffi.tonumber(blob))
    if row ~= nil then
        return row_motion(row)
    end
    return original_reverse_blob(blob, second, third, fourth)
end

local function on_file_loaded(size, unused, destination)
    finished_loads[cffi.tonumber(destination)] = size
    local thread_id = cffi.C.GetCurrentThreadId()
    if thread_id ~= load_thread_id and not warned_about_thread then
        warned_about_thread = true
        log(string.format("WARNING file load finished on thread %d, it was queued on thread %d", thread_id, load_thread_id))
    end
end

local function install()
    if installed then
        return true
    end
    if fnc_reverse_blob_to_slot_ptr_call_1 == nil or fnc_reverse_blob_to_slot_ptr_call_2 == nil then
        log("install failed: missing call site addresses")
        return false
    end
    registry = native.persistent_block(REGISTRY_KEY, REGISTRY_SIZE)
    if registry == nil or registry == 0 then
        log("install failed: persistent_block failed")
        return false
    end
    original_reverse_blob = cffi.cast("SpawnEnemyReverseBlob", BASE_ADDR + fnc_reverse_blob_to_slot_ptr)
    local call_sites = { fnc_reverse_blob_to_slot_ptr_call_1, fnc_reverse_blob_to_slot_ptr_call_2 }
    for _, call_site in ipairs(call_sites) do
        local redirected = call_redirect.redirect_call(call_site, fnc_reverse_blob_to_slot_ptr, "SpawnEnemyReverseBlob", reverse_blob)
        if not redirected then
            log(string.format("install failed: could not redirect call at 0x%X", call_site))
            return false
        end
    end
    load_complete_callback = cffi.cast("SpawnEnemyLoadComplete", on_file_loaded)
    load_complete_address = cffi.tonumber(cffi.cast("uintptr_t", load_complete_callback))
    installed = true
    return true
end

-- ############################################################ --
-- # Buffers: carved from a few 32MB arenas, then reused        # --
-- ############################################################ --

local BLOB_SIZE = 0x40000
local ARENA_SIZE = 0x2000000
local MAX_ARENAS = 6

local arenas = {}
local spare_buffers = {}

-- One 32MB-aligned arena uses one handle bucket, and the game never frees buckets.
local function carve_from_arenas(size_needed)
    local size = round_up(size_needed, BLOB_SIZE)
    if size > ARENA_SIZE then
        return nil
    end
    for _, arena in ipairs(arenas) do
        if arena.used + size <= ARENA_SIZE then
            local address = arena.base + arena.used
            arena.used = arena.used + size
            return address, size
        end
    end
    if #arenas >= MAX_ARENAS then
        return nil
    end
    local raw_address = native.allocate(ARENA_SIZE * 2)
    if raw_address == 0 then
        return nil
    end
    local base = round_up(raw_address, ARENA_SIZE)
    table.insert(arenas, { base = base, used = size })
    log(string.format("arena %d: base=0x%X", #arenas, base))
    return base, size
end

local function get_buffer(size_needed)
    local best_index = nil
    for index, buffer in ipairs(spare_buffers) do
        if buffer.size >= size_needed then
            if best_index == nil or buffer.size < spare_buffers[best_index].size then
                best_index = index
            end
        end
    end
    if best_index ~= nil then
        local buffer = table.remove(spare_buffers, best_index)
        return buffer.address, buffer.size
    end
    return carve_from_arenas(size_needed)
end

local function give_back_buffer(address, size)
    if address == nil or address == 0 or size == nil or size <= 0 then
        return
    end
    table.insert(spare_buffers, { address = address, size = size })
end

local function give_back_row_and_buffer(row)
    give_back_buffer(row_buffer_start(row), row_buffer_end(row) - row_buffer_start(row))
    release_row(row)
end

-- ############################################################ --
-- # Placement record: the room-table entry the game builds from # --
-- ############################################################ --

local RECORD_SIZE = 0x78
local RECORD_ID_OFFSET = 0x00
local RECORD_BLOB_HANDLE_OFFSET = 0x08
local RECORD_X_OFFSET = 0x1C
local RECORD_Y_OFFSET = 0x20
local RECORD_Z_OFFSET = 0x24
local RECORD_CHAR_ID_OFFSET = 0x4C
local RECORD_SPECIES_OFFSET = 0x55
local RECORD_RUN_LENGTH_OFFSET = 0x56
local RECORD_WEIGHT_OFFSET = 0x59
local RECORD_MODEL_HANDLE_OFFSET = 0x60
local RECORD_MOTION_HANDLE_OFFSET = 0x64
local RECORD_CATEGORY_ACTOR = 3
local SPAWN_WEIGHT = 1

local function template_run_length(template)
    local run_length = string.byte(template, RECORD_RUN_LENGTH_OFFSET + 1)
    if run_length == nil or run_length < 1 then
        return 1
    end
    return run_length
end

local function add_placement_record(template, char_id, x, y, z)
    if template == nil or #template ~= RECORD_SIZE then
        return nil
    end
    local old_table = GetPointer(placementTablePtr)
    local old_count = ReadInt(placementTableCount)
    if old_table == 0 or old_count <= 0 or old_count > 4096 then
        return nil
    end

    local new_table = native.allocate((old_count + 1) * RECORD_SIZE)
    if new_table == 0 then
        return nil
    end
    if not native.copy_memory(new_table, old_table, old_count * RECORD_SIZE) then
        native.free(new_table)
        return nil
    end

    local record = new_table + old_count * RECORD_SIZE
    local id = (RECORD_CATEGORY_ACTOR << 16) | (old_count & 0xFFFF)
    WriteArray(record, { template:byte(1, -1) }, true)
    WriteInt(record + RECORD_ID_OFFSET, id, true)
    WriteInt(record + RECORD_BLOB_HANDLE_OFFSET, 0, true)
    WriteFloat(record + RECORD_X_OFFSET, x, true)
    WriteFloat(record + RECORD_Y_OFFSET, y, true)
    WriteFloat(record + RECORD_Z_OFFSET, z, true)
    WriteShort(record + RECORD_CHAR_ID_OFFSET, char_id, true)
    WriteByte(record + RECORD_SPECIES_OFFSET, 0, true)
    WriteByte(record + RECORD_RUN_LENGTH_OFFSET, 1, true)
    WriteByte(record + RECORD_WEIGHT_OFFSET, SPAWN_WEIGHT, true)
    WriteInt(record + RECORD_MODEL_HANDLE_OFFSET, 0, true)
    WriteInt(record + RECORD_MOTION_HANDLE_OFFSET, 0, true)

    WriteLong(placementTablePtr, new_table)
    WriteInt(placementTableCount, old_count + 1)

    return { record = record, id = id, old_table = old_table, old_count = old_count }
end

local function remove_placement_record(placement)
    if placement == nil then
        return
    end
    WriteLong(placementTablePtr, placement.old_table)
    WriteInt(placementTableCount, placement.old_count)
end

-- ############################################################ --
-- # Loading files and fixing them up the way the game does     # --
-- ############################################################ --

local LOAD_TIMEOUT_SECONDS = 5.0
local MOTION_ALIGNMENT = 0x80

local permanent_strings = {}

local function permanent_string(text)
    if permanent_strings[text] ~= nil then
        return permanent_strings[text]
    end
    local address = native.allocate(#text + 1)
    if address == 0 then
        return 0
    end
    WriteArray(address, { (text .. "\0"):byte(1, -1) }, true)
    permanent_strings[text] = address
    return address
end

local function queue_file_load(filename, destination)
    local filename_address = permanent_string(filename)
    if filename_address == 0 then
        return false
    end
    finished_loads[destination] = nil
    load_thread_id = cffi.C.GetCurrentThreadId()
    local ok = native.call_function(fnc_queue_file_load_to_address, filename_address, destination, load_complete_address, 0)
    return ok
end

local function take_finished_load(destination)
    local size = finished_loads[destination]
    finished_loads[destination] = nil
    return size
end

local function call_game(function_address, argument)
    local ok = native.call_function(function_address, argument)
    return ok
end

local function blob_int(blob, offset)
    return ReadInt(blob + offset, true)
end

local function fix_up_model(blob)
    if not call_game(fnc_blob_reloc, blob + blob_int(blob, 0x04)) then
        return false
    end
    if blob_int(blob, 0x0C) > blob_int(blob, 0x08) then
        if not call_game(fnc_blob_fixup_a, blob + blob_int(blob, 0x08)) then
            return false
        end
    end
    if blob_int(blob, 0x18) > blob_int(blob, 0x14) then
        if not call_game(fnc_blob_fixup_b, blob + blob_int(blob, 0x14)) then
            return false
        end
    end
    return true
end

local function fix_up_motion(blob)
    if not call_game(fnc_blob_reloc, blob + blob_int(blob, 0x04)) then
        return false
    end
    if blob_int(blob, 0x10) <= blob_int(blob, 0x0C) then
        return true
    end
    local section_table = blob + blob_int(blob, 0x0C)
    for index = 0, 9 do
        local section_start = blob_int(section_table, 4 + index * 4)
        local section_end = blob_int(section_table, 8 + index * 4)
        if section_end > section_start then
            if not call_game(fnc_motion_fixup, section_table + section_start) then
                return false
            end
        end
    end
    return true
end

local function blob_header_looks_valid(blob)
    local previous_section = 0
    for index = 0, 4 do
        local section = blob_int(blob, 4 + index * 4)
        if index == 0 and section ~= 128 then
            return false
        end
        if section < previous_section then
            return false
        end
        previous_section = section
    end
    return true
end

-- ############################################################ --
-- # Game state checks                                          # --
-- ############################################################ --

local HANDLE_BUCKET_COUNT = 64
local HANDLE_BUCKET_HEADROOM = 6
local ENTITY_SIZE = 0x4B0
local ENTITY_COUNT = 96
local ENTITY_FLAGS_OFFSET = 0x374
local ENTITY_BLOB_HANDLE_OFFSETS = { 0x68, 0x134, 0x138, 0x13c, 0x154, 0x1d0, 0x1d4 }

local function sora_position()
    local sora = 0
    if g_SoraObjPtr then
        sora = GetPointer(g_SoraObjPtr)
    end
    if sora == 0 then
        return 0.0, 0.0, 0.0
    end
    return ReadFloat(sora + 0x10, true), ReadFloat(sora + 0x14, true), ReadFloat(sora + 0x18, true)
end

local function in_cutscene()
    return inCutscene ~= nil and ReadInt(inCutscene) ~= 0
end

local function boss_slowdown_active()
    if not g_GameSpeed then
        return false
    end
    local speed = ReadFloat(g_GameSpeed)
    return speed > 0.0 and speed < 0.9
end

local function handle_bucket_base(index)
    local address = resource_handle_bucket_table + index * 8
    local low = ReadInt(address) & 0xFFFFFFFF
    local high = ReadInt(address + 4) & 0xFFFFFFFF
    if low == 0xFFFFFFFF and high == 0xFFFFFFFF then
        return nil
    end
    return (high << 32) | low
end

local function handle_buckets_full()
    if not resource_handle_bucket_table then
        return false
    end
    local claimed = 0
    for index = 0, HANDLE_BUCKET_COUNT - 1 do
        if handle_bucket_base(index) ~= nil then
            claimed = claimed + 1
        end
    end
    return claimed >= HANDLE_BUCKET_COUNT - HANDLE_BUCKET_HEADROOM
end

local function resolve_handle(handle)
    if handle == 0 then
        return 0
    end
    handle = handle & 0x7FFFFFFF
    local bucket_base = handle_bucket_base(handle >> 25)
    if bucket_base == nil then
        return 0
    end
    return bucket_base | (handle & 0x1FFFFFF)
end

local function make_handle(address)
    local ok, handle = native.call_function(fnc_mint_resource_handle, address)
    if not ok then
        return nil
    end
    return handle & 0xFFFFFFFF
end

local function rows_used_by_live_entities()
    local used = {}
    for index = 0, ENTITY_COUNT - 1 do
        local entity = entityPoolBase + index * ENTITY_SIZE
        local occupied = (ReadInt(entity + ENTITY_FLAGS_OFFSET) & 1) ~= 0
        if occupied then
            for _, offset in ipairs(ENTITY_BLOB_HANDLE_OFFSETS) do
                local address = resolve_handle(ReadInt(entity + offset) & 0xFFFFFFFF)
                if address ~= 0 then
                    local row = row_containing(address)
                    if row ~= nil then
                        used[row] = true
                    end
                end
            end
        end
    end
    return used
end

-- ############################################################ --
-- # Spawns in progress                                         # --
-- ############################################################ --

local ABANDONED_SPAWN_GRACE_SECONDS = 2.0
local DEAD_CHECKS_BEFORE_FREEING = 2

local spawns_in_progress = {}
local dead_checks = {}
local current_room = nil

local function cancel_spawn(spawn)
    remove_placement_record(spawn.placement)
    release_row(spawn.row)
    give_back_buffer(spawn.buffer, spawn.buffer_size)
    spawns_in_progress[spawn.model_path] = nil
end

local function cancel_abandoned_spawns()
    local now = os.clock()
    for model_path, spawn in pairs(spawns_in_progress) do
        if now >= spawn.deadline + ABANDONED_SPAWN_GRACE_SECONDS then
            cancel_spawn(spawn)
            log("cancelled abandoned spawn " .. model_path)
        end
    end
end

local function free_rows_of_dead_enemies()
    local rows = rows_in_use()
    if #rows == 0 then
        return
    end
    local rows_still_loading = {}
    for _, spawn in pairs(spawns_in_progress) do
        rows_still_loading[spawn.row] = true
    end
    local rows_with_live_enemy = rows_used_by_live_entities()
    for _, row in ipairs(rows) do
        if rows_still_loading[row] or rows_with_live_enemy[row] then
            dead_checks[row] = 0
        else
            local checks = (dead_checks[row] or 0) + 1
            if checks >= DEAD_CHECKS_BEFORE_FREEING then
                give_back_row_and_buffer(row)
                dead_checks[row] = nil
            else
                dead_checks[row] = checks
            end
        end
    end
end

local function free_everything_if_room_changed()
    if not g_WorldNumber or not g_AreaNumber or not g_SetNumber then
        return
    end
    local room = ReadInt(g_WorldNumber) .. "-" .. ReadInt(g_AreaNumber) .. "-" .. ReadInt(g_SetNumber)
    if room == current_room then
        return
    end
    local first_check = current_room == nil
    current_room = room
    if first_check then
        return
    end
    local rows = rows_in_use()
    for _, row in ipairs(rows) do
        give_back_row_and_buffer(row)
    end
    if #rows > 0 then
        log(string.format("room change: freed %d row(s)", #rows))
    end
    spawns_in_progress = {}
    dead_checks = {}
end

local function keep_waiting(spawn)
    if os.clock() >= spawn.deadline then
        cancel_spawn(spawn)
        return false, "timeout"
    end
    return false, "loading"
end

local function start_spawn(model_path, data, x, y, z)
    local spawn = { model_path = model_path, motion_path = data.motionPath }

    local size_needed = (template_run_length(data.template) + 2) * BLOB_SIZE
    spawn.buffer, spawn.buffer_size = get_buffer(size_needed)
    if spawn.buffer == nil then
        return false, "buf_alloc_failed"
    end

    spawn.row = claim_row(spawn.buffer, spawn.buffer + spawn.buffer_size)
    if spawn.row == nil then
        cancel_spawn(spawn)
        return false, "registry_full"
    end

    spawn.placement = add_placement_record(data.template, data.charId, x, y, z)
    if spawn.placement == nil then
        cancel_spawn(spawn)
        return false, "splice_failed"
    end

    local buffer_handle = make_handle(spawn.buffer)
    if buffer_handle == nil then
        cancel_spawn(spawn)
        return false, "mint_failed"
    end
    WriteInt(spawn.placement.record + RECORD_BLOB_HANDLE_OFFSET, buffer_handle, true)

    if not queue_file_load(model_path, spawn.buffer) then
        cancel_spawn(spawn)
        return false, "load_failed"
    end

    spawn.step = "loading model"
    spawn.deadline = os.clock() + LOAD_TIMEOUT_SECONDS
    spawns_in_progress[model_path] = spawn
    log(string.format("%s -> buffer=0x%X size=0x%X row=%d (loading)", model_path, spawn.buffer, spawn.buffer_size, spawn.row))
    return false, "loading"
end

local function build_enemy(spawn)
    if not blob_header_looks_valid(spawn.buffer) then
        log("refusing to build: model header looks wrong")
        cancel_spawn(spawn)
        return false, "ctor_failed"
    end
    local ok, entity = native.call_function(fnc_spawn_world_gimmick_entity, spawn.placement.id)
    if not ok or entity == 0 then
        cancel_spawn(spawn)
        return false, "ctor_failed"
    end
    spawns_in_progress[spawn.model_path] = nil
    log(string.format("built entity=0x%X", entity))
    return true, entity
end

local function continue_after_model_loaded(spawn, model_size)
    if not fix_up_model(spawn.buffer) then
        cancel_spawn(spawn)
        return false, "load_failed"
    end
    spawn.motion_address = spawn.buffer + round_up(model_size, MOTION_ALIGNMENT)
    if spawn.motion_address + BLOB_SIZE > spawn.buffer + spawn.buffer_size then
        cancel_spawn(spawn)
        return false, "buf_alloc_failed"
    end
    set_row_motion(spawn.row, spawn.motion_address)
    if not queue_file_load(spawn.motion_path, spawn.motion_address) then
        cancel_spawn(spawn)
        return false, "load_failed"
    end
    spawn.step = "loading motion"
    spawn.deadline = os.clock() + LOAD_TIMEOUT_SECONDS
    return false, "loading"
end

local function continue_after_motion_loaded(spawn)
    if not fix_up_motion(spawn.motion_address) then
        cancel_spawn(spawn)
        return false, "load_failed"
    end
    return build_enemy(spawn)
end

local function continue_spawn(spawn)
    if spawn.step == "loading model" then
        local model_size = take_finished_load(spawn.buffer)
        if model_size == nil then
            return keep_waiting(spawn)
        end
        return continue_after_model_loaded(spawn, model_size)
    end
    local motion_size = take_finished_load(spawn.motion_address)
    if motion_size == nil then
        return keep_waiting(spawn)
    end
    return continue_after_motion_loaded(spawn)
end

-- ############################################################ --
-- # Public entry point                                         # --
-- ############################################################ --

local function try_spawn_enemy(model_path, x, y, z)
    if in_cutscene() then
        return false, "cutscene"
    end
    if boss_slowdown_active() then
        return false, "boss_slowdown"
    end
    if not install() then
        return false, "install_failed"
    end
    local data = creature_data[model_path]
    if data == nil then
        return false, "no_data"
    end

    free_everything_if_room_changed()
    cancel_abandoned_spawns()
    free_rows_of_dead_enemies()

    local spawn = spawns_in_progress[model_path]
    if spawn ~= nil then
        return continue_spawn(spawn)
    end

    if handle_buckets_full() then
        return false, "handles_full"
    end
    if x == nil or y == nil or z == nil then
        local sora_x, sora_y, sora_z = sora_position()
        x = x or sora_x
        y = y or sora_y
        z = z or sora_z
    end
    return start_spawn(model_path, data, x, y, z)
end

local function spawn_enemy(model_path, x, y, z)
    if model_path == nil then
        log("called with nil model")
        return false, "bad_args"
    end
    local ok, result = try_spawn_enemy(model_path, x, y, z)
    if not ok and result ~= "loading" then
        log(model_path .. " FAIL: " .. tostring(result))
    end
    return ok, result
end

return {
    spawn_enemy = spawn_enemy,
}
