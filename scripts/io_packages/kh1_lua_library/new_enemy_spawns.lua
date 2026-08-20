---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")
local kh1_creature_data = require("kh1_lua_library.creature_data")

local native_log = kh1_native.log_debug or function(_) end
local function log(msg) native_log("[new_spawn] " .. msg) end

local SLOT_STRIDE                 = 0x50
local OWNER_OFF_FROM_PTR          = -0x48
local SLOT_OWNER_FREE             = 0xFF
local SLOT_ALLOC_MIN              = 0
local SLOT_ALLOC_MAX              = 49
local PLACEMENT_RECORD_SIZE       = 0x78
local PLACEMENT_SPECIES_OFF       = 0x55
local PLACEMENT_RUNLEN_OFF        = 0x56
local STATE_OFF_FROM_PTR          = -0x45
local MODEL_NAME_OFF_FROM_PTR     = -0x44
local SLOT_STATE_READY            = 6
local MODEL_NAME_SIZE             = 0x20
local RESOURCE_BLOB_SIZE          = 0x40000
local BLOB_HEADER_FIRST_SECTION   = 128
local BLOB_RELOC_MARKER_OFF       = 0xD0
local BLOB_RELOC_MARKER           = 0x96969696
local BLOB_RELOC_HANDLE_OFF       = 0xD4
local PLACEMENT_ID_OFF            = 0x00
local PLACEMENT_HANDLE_OFF        = 0x08
local PLACEMENT_POS_X_OFF         = 0x1C
local PLACEMENT_POS_Y_OFF         = 0x20
local PLACEMENT_POS_Z_OFF         = 0x24
local PLACEMENT_CHARID_OFF        = 0x4C
local PLACEMENT_WEIGHT_OFF        = 0x59
local PLACEMENT_MODEL_HANDLE_OFF  = 0x60
local PLACEMENT_MOTION_HANDLE_OFF = 0x64
local RECORD_CATEGORY_ACTOR       = 3
local SPAWN_LOAD_TIMEOUT          = 5.0
local RUNLEN_OFF_FROM_PTR         = -0x47
local ENTITY_POOL_STRIDE          = 0x4B0
local ENTITY_POOL_COUNT           = 96
local ENTITY_OCC_FLAG_OFF         = 0x374
local ENTITY_SPECIES_HND_OFF      = 0x134
local BLOB_TABLE_SLOT_COUNT       = 65

local pending_spawns = {}
local our_spawn_runs = {}

local function run_is_free(excluded, start, run_len)
    for k = 0, run_len - 1 do
        local s = start + k
        if s > SLOT_ALLOC_MAX then return false end
        if excluded and excluded[s] then return false end
        if ReadByte(loadedSpeciesPtrTable + OWNER_OFF_FROM_PTR + s * SLOT_STRIDE) ~= SLOT_OWNER_FREE then
            return false
        end
    end
    return true
end

local function find_free_species_run(run_len, excluded)
    if not loadedSpeciesPtrTable or not placementTablePtr then return nil end
    if run_len < 1 then run_len = 1 end
    for start = SLOT_ALLOC_MAX - run_len + 1, SLOT_ALLOC_MIN, -1 do
        if run_is_free(excluded, start, run_len) then return start end
    end
    return nil
end

local function cached_name_matches(name_rva, model_path)
    if #model_path >= MODEL_NAME_SIZE then return false end
    for i = 1, #model_path do
        if ReadByte(name_rva + i - 1) ~= string.byte(model_path, i) then return false end
    end
    return ReadByte(name_rva + #model_path) == 0
end

local function blob_is_healthy(species)
    local blob = speciesResourceTable + species * RESOURCE_BLOB_SIZE
    if ReadInt(blob + 4) ~= BLOB_HEADER_FIRST_SECTION then return false end
    local marker = ReadInt(blob + BLOB_RELOC_MARKER_OFF) & 0xFFFFFFFF
    if marker == BLOB_RELOC_MARKER and ReadInt(blob + BLOB_RELOC_HANDLE_OFF) == 0 then return false end
    return true
end

local function blob_construct_safe(species)
    local blob = speciesResourceTable + species * RESOURCE_BLOB_SIZE
    local s = {}
    for i = 0, 8 do s[i] = ReadInt(blob + 4 + i * 4) end
    if s[0] ~= BLOB_HEADER_FIRST_SECTION then return false, string.format("hdr sec0=%d", s[0]) end
    local prev = 0
    for i = 0, 4 do
        if s[i] < 0 or s[i] < prev then return false, string.format("hdr not monotonic at %d", i) end
        prev = s[i]
    end
    return true
end

local function find_loaded_slot_by_filename(model_path)
    if not loadedSpeciesPtrTable or not speciesResourceTable or not model_path then return nil end
    for s = SLOT_ALLOC_MIN, SLOT_ALLOC_MAX do
        if ReadByte(loadedSpeciesPtrTable + STATE_OFF_FROM_PTR + s * SLOT_STRIDE) == SLOT_STATE_READY then
            local name_rva = loadedSpeciesPtrTable + MODEL_NAME_OFF_FROM_PTR + s * SLOT_STRIDE
            if cached_name_matches(name_rva, model_path) and blob_is_healthy(s)
                and blob_construct_safe(s) then
                return s
            end
        end
    end
    return nil
end

local function get_creature_data(model_path)
    return model_path and kh1_creature_data[model_path] or nil
end

local function resolve_handle(h)
    if h == 0 then return 0 end
    h = h & 0x7FFFFFFF
    local addr = resource_handle_bucket_table + (h >> 25) * 8
    local lo = ReadInt(addr) & 0xFFFFFFFF
    local hi = ReadInt(addr + 4) & 0xFFFFFFFF
    if lo == 0xFFFFFFFF and hi == 0xFFFFFFFF then return 0 end
    return ((hi << 32) | lo) | (h & 0x1FFFFFF)
end

local function mark_run(occ, sp)
    if sp < 0 or sp > SLOT_ALLOC_MAX then return end
    local primary = ReadByte(loadedSpeciesPtrTable + OWNER_OFF_FROM_PTR + sp * SLOT_STRIDE)
    if primary == SLOT_OWNER_FREE or primary > SLOT_ALLOC_MAX then primary = sp end
    local rl = ReadByte(loadedSpeciesPtrTable + RUNLEN_OFF_FROM_PTR + primary * SLOT_STRIDE)
    if rl < 1 then rl = 1 end
    for k = 0, rl - 1 do occ[primary + k] = true end
end

local function live_referenced_slots()
    local occ = {}
    if not entityPoolBase or not resource_handle_bucket_table or not speciesResourceTable
        or not loadedSpeciesPtrTable then return occ end
    local base = kh1_native.get_module_base()
    local blob_lo = base + speciesResourceTable
    local blob_hi = blob_lo + BLOB_TABLE_SLOT_COUNT * RESOURCE_BLOB_SIZE
    local table_ptr = GetPointer(placementTablePtr)
    local count = ReadInt(placementTableCount)
    local table_ok = table_ptr ~= 0 and count > 0 and count <= 4096
    for i = 0, ENTITY_POOL_COUNT - 1 do
        local e = entityPoolBase + i * ENTITY_POOL_STRIDE
        if (ReadInt(e + ENTITY_OCC_FLAG_OFF) & 1) ~= 0 then
            local resolved = resolve_handle(ReadInt(e + ENTITY_SPECIES_HND_OFF) & 0xFFFFFFFF)
            if resolved >= blob_lo and resolved < blob_hi then mark_run(occ, (resolved - blob_lo) >> 18) end
            if table_ok then
                local ridx = ReadInt(e + 0x04) & 0xFFFF
                if ridx < count then mark_run(occ, ReadByte(table_ptr + ridx * PLACEMENT_RECORD_SIZE + PLACEMENT_SPECIES_OFF, true)) end
            end
        end
    end
    return occ
end

local function release_our_runs()
    if fnc_release_species_slot_run and #our_spawn_runs > 0 then
        local occ = live_referenced_slots()
        local released, kept = 0, 0
        for _, r in ipairs(our_spawn_runs) do
            local sp = r.species
            local name_ok = cached_name_matches(loadedSpeciesPtrTable + MODEL_NAME_OFF_FROM_PTR + sp * SLOT_STRIDE, r.model)
            local token_ok = ReadLong(loadedSpeciesPtrTable + sp * SLOT_STRIDE) == r.token
            local in_use = false
            for k = 0, (r.run_len or 1) - 1 do if occ[sp + k] then in_use = true break end end
            if name_ok and token_ok and not in_use then
                kh1_native.call_function(fnc_release_species_slot_run, sp, r.run_len)
                released = released + 1
            else
                kept = kept + 1
            end
        end
        log(string.format("room change: released %d of %d spawned runs (%d re-taken/alive, kept)", released, #our_spawn_runs, kept))
    end
    our_spawn_runs = {}
end

local function record_our_spawn(species, run_len, model_path)
    our_spawn_runs[#our_spawn_runs + 1] = {
        species = species, run_len = run_len, model = model_path,
        token = ReadLong(loadedSpeciesPtrTable + species * SLOT_STRIDE),
    }
end

local function resolve_species_slot(model_path, run_len, excluded)
    local reused = find_loaded_slot_by_filename(model_path)
    if reused then return reused, false end
    local free = find_free_species_run(run_len, excluded)
    if free then return free, true end
    return nil, nil
end

local function splice_placement_record(species, template, char_id, weight, x, y, z)
    if not template or #template ~= PLACEMENT_RECORD_SIZE then return nil end
    local old_ptr = GetPointer(placementTablePtr)
    local old_count = ReadInt(placementTableCount)
    if old_ptr == 0 or old_count <= 0 or old_count > 4096 then return nil end

    local new_count = old_count + 1
    local buffer = kh1_native.allocate(new_count * PLACEMENT_RECORD_SIZE)
    if buffer == 0 then return nil end
    if not kh1_native.copy_memory(buffer, old_ptr, old_count * PLACEMENT_RECORD_SIZE) then
        kh1_native.free(buffer)
        return nil
    end

    local record = buffer + old_count * PLACEMENT_RECORD_SIZE
    kh1_native.write_bytes(record, template)

    local id = (RECORD_CATEGORY_ACTOR << 16) | (old_count & 0xFFFF)
    WriteInt(record + PLACEMENT_ID_OFF, id, true)
    WriteInt(record + PLACEMENT_HANDLE_OFF, 0, true)
    WriteFloat(record + PLACEMENT_POS_X_OFF, x, true)
    WriteFloat(record + PLACEMENT_POS_Y_OFF, y, true)
    WriteFloat(record + PLACEMENT_POS_Z_OFF, z, true)
    WriteShort(record + PLACEMENT_CHARID_OFF, char_id, true)
    WriteByte(record + PLACEMENT_SPECIES_OFF, species, true)
    WriteByte(record + PLACEMENT_WEIGHT_OFF, weight, true)
    WriteInt(record + PLACEMENT_MODEL_HANDLE_OFF, 0, true)
    WriteInt(record + PLACEMENT_MOTION_HANDLE_OFF, 0, true)

    WriteLong(placementTablePtr, buffer)
    WriteInt(placementTableCount, new_count)

    return {
        buffer = buffer,
        record = record,
        id = id,
        species = species,
        old_ptr = old_ptr,
        old_count = old_count,
    }
end

local function rollback_splice(ctx)
    if not ctx then return end
    WriteLong(placementTablePtr, ctx.old_ptr)
    WriteInt(placementTableCount, ctx.old_count)
end

local interned_paths = {}
local function intern_path(path)
    if interned_paths[path] then return interned_paths[path] end
    local addr = kh1_native.allocate(#path + 1)
    if addr == 0 then return 0 end
    kh1_native.write_bytes(addr, path .. "\0")
    interned_paths[path] = addr
    return addr
end

local function mint_string_handle(path)
    local addr = intern_path(path)
    if addr == 0 then return nil end
    local ok, handle = kh1_native.call_function(fnc_mint_resource_handle, addr)
    if not ok then return nil end
    return handle & 0xFFFFFFFF
end

local function trigger_asset_load(ctx, model_path, motion_path)
    local model_handle = mint_string_handle(model_path)
    local motion_handle = mint_string_handle(motion_path)
    if not model_handle or not motion_handle then return false end
    WriteInt(ctx.record + PLACEMENT_MODEL_HANDLE_OFF, model_handle, true)
    WriteInt(ctx.record + PLACEMENT_MOTION_HANDLE_OFF, motion_handle, true)
    return kh1_native.call_function(fnc_load_gimmick_assets, ctx.id, 0)
end

local function slot_is_ready(species)
    return ReadByte(loadedSpeciesPtrTable + STATE_OFF_FROM_PTR + species * SLOT_STRIDE) == SLOT_STATE_READY
end

local function construct_entity(ctx)
    local safe, why = blob_construct_safe(ctx.species)
    if not safe then
        log(string.format("REFUSE construct species=%d id=0x%X: blob unsafe (%s)", ctx.species, ctx.id, why))
        return nil
    end
    log(string.format("construct species=%d id=0x%X", ctx.species, ctx.id))
    local ok, entity = kh1_native.call_function(fnc_spawn_world_gimmick_entity, ctx.id)
    if not ok then log("construct CRASHED (SafeCall caught)"); return nil end
    if entity == 0 then log("construct returned null (budget?)"); return nil end
    log(string.format("construct OK entity=0x%X", entity))
    return entity
end

local function sora_position()
    local sora = g_SoraObjPtr and GetPointer(g_SoraObjPtr) or 0
    if sora == 0 then return 0.0, 0.0, 0.0 end
    return ReadFloat(sora + 0x10, true), ReadFloat(sora + 0x14, true), ReadFloat(sora + 0x18, true)
end

local HANDLE_BUCKET_COUNT = 64
local HANDLE_BUCKET_HEADROOM = 6

local function boss_slowdown_active()
    if not g_GameSpeed then return false end
    local speed = ReadFloat(g_GameSpeed)
    return speed > 0.0 and speed < 0.9
end

local function handle_buckets_full()
    if not resource_handle_bucket_table then return false end
    local claimed = 0
    for i = 0, HANDLE_BUCKET_COUNT - 1 do
        local addr = resource_handle_bucket_table + i * 8
        local lo = ReadInt(addr) & 0xFFFFFFFF
        local hi = ReadInt(addr + 4) & 0xFFFFFFFF
        if not (lo == 0xFFFFFFFF and hi == 0xFFFFFFFF) then claimed = claimed + 1 end
    end
    return claimed >= HANDLE_BUCKET_COUNT - HANDLE_BUCKET_HEADROOM
end

local last_room_world, last_room_area, last_room_set = nil, nil, nil

local function drop_pending_on_room_change()
    if not g_WorldNumber or not g_AreaNumber or not g_SetNumber then return end
    local w, a, s = ReadInt(g_WorldNumber), ReadInt(g_AreaNumber), ReadInt(g_SetNumber)
    if w == last_room_world and a == last_room_area and s == last_room_set then return end
    last_room_world, last_room_area, last_room_set = w, a, s
    if next(pending_spawns) then pending_spawns = {} end
    release_our_runs()
end

local function spawn_enemy(model_path, x, y, z)
    if not model_path then log("called with nil model"); return false, "bad_args" end
    if inCutscene and ReadInt(inCutscene) ~= 0 then log(model_path .. " -> cutscene"); return false, "cutscene" end
    if boss_slowdown_active() then log(model_path .. " -> boss_slowdown"); return false, "boss_slowdown" end

    local data = get_creature_data(model_path)
    if not data then log(model_path .. " -> no_data"); return false, "no_data" end

    drop_pending_on_room_change()

    local pending = pending_spawns[model_path]
    if pending then
        if slot_is_ready(pending.species) then
            log(string.format("%s load ready (species=%d), constructing", model_path, pending.species))
            local entity = construct_entity(pending.ctx)
            pending_spawns[model_path] = nil
            if entity then
                record_our_spawn(pending.species, pending.run_len, model_path)
                return true, entity
            end
            rollback_splice(pending.ctx)
            return false, "ctor_failed"
        end
        if os.clock() >= pending.deadline then
            pending_spawns[model_path] = nil
            return false, "timeout"
        end
        return false, "loading"
    end

    if handle_buckets_full() then log(model_path .. " -> handles_full"); return false, "handles_full" end

    local run_len = string.byte(data.template, PLACEMENT_RUNLEN_OFF + 1) or 1
    if run_len < 1 then run_len = 1 end

    local excluded = {}
    for _, p in pairs(pending_spawns) do
        for k = 0, (p.run_len or 1) - 1 do excluded[p.species + k] = true end
    end
    local species, needs_load = resolve_species_slot(model_path, run_len, excluded)
    if not species then log(model_path .. " -> slots_full (run_len=" .. run_len .. ")"); return false, "slots_full" end
    log(string.format("%s resolved species=%d run_len=%d needs_load=%s", model_path, species, run_len, tostring(needs_load)))

    if x == nil or y == nil or z == nil then
        local sx, sy, sz = sora_position()
        x, y, z = x or sx, y or sy, z or sz
    end

    local ctx = splice_placement_record(species, data.template, data.charId, data.weight, x, y, z)
    if not ctx then return false, "splice_failed" end
    log(string.format("%s spliced id=0x%X buffer=0x%X pos=(%.0f,%.0f,%.0f)", model_path, ctx.id, ctx.buffer, x, y, z))

    if not needs_load then
        local entity = construct_entity(ctx)
        if entity then
            record_our_spawn(species, run_len, model_path)
            return true, entity
        end
        rollback_splice(ctx)
        return false, "ctor_failed"
    end

    do
        local diag = {}
        for k = 0, run_len - 1 do
            local sl = species + k
            local own = ReadByte(loadedSpeciesPtrTable + OWNER_OFF_FROM_PTR + sl * SLOT_STRIDE)
            local st = ReadByte(loadedSpeciesPtrTable + STATE_OFF_FROM_PTR + sl * SLOT_STRIDE)
            diag[#diag + 1] = string.format("s%d:own%02X/st%d", sl, own, st)
        end
        log(string.format("%s pre-load target run %d..%d: %s", model_path, species, species + run_len - 1, table.concat(diag, " ")))
    end

    if not trigger_asset_load(ctx, model_path, data.motionPath) then
        rollback_splice(ctx)
        return false, "load_failed"
    end
    log(string.format("%s load triggered, waiting for species=%d run_len=%d", model_path, species, run_len))
    pending_spawns[model_path] = { ctx = ctx, species = species, run_len = run_len, deadline = os.clock() + SPAWN_LOAD_TIMEOUT }
    return false, "loading"
end

return {
    spawn_enemy = spawn_enemy,
}
