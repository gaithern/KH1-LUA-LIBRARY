---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")
local kh1_creature_data = require("kh1_lua_library.creature_data")

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
local SLOT_RUN_LEN                = 1
local SPAWN_LOAD_TIMEOUT          = 5.0

local pending_spawns = {}

local function get_room_roster()
    local roster = {}
    local table_ptr = ReadLong(placementTablePtr)
    local count = ReadInt(placementTableCount)
    if table_ptr == 0 or count <= 0 or count > 4096 then return roster end
    for i = 0, count - 1 do
        local rec = table_ptr + i * PLACEMENT_RECORD_SIZE
        local species = ReadByte(rec + PLACEMENT_SPECIES_OFF, true)
        local run_len = ReadByte(rec + PLACEMENT_RUNLEN_OFF, true)
        if run_len < 1 then run_len = 1 end
        for k = 0, run_len - 1 do
            local s = species + k
            if s >= 0 and s <= SLOT_ALLOC_MAX then roster[s] = true end
        end
    end
    return roster
end

local function find_free_species_slot(excluded)
    if not loadedSpeciesPtrTable or not placementTablePtr then return nil end
    local roster = get_room_roster()
    for s = SLOT_ALLOC_MAX, SLOT_ALLOC_MIN, -1 do
        if not roster[s] and not (excluded and excluded[s]) then
            local owner = ReadByte(loadedSpeciesPtrTable + OWNER_OFF_FROM_PTR + s * SLOT_STRIDE)
            if owner == SLOT_OWNER_FREE then
                return s
            end
        end
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

local function find_loaded_slot_by_filename(model_path)
    if not loadedSpeciesPtrTable or not speciesResourceTable or not model_path then return nil end
    for s = SLOT_ALLOC_MIN, SLOT_ALLOC_MAX do
        if ReadByte(loadedSpeciesPtrTable + STATE_OFF_FROM_PTR + s * SLOT_STRIDE) == SLOT_STATE_READY then
            local name_rva = loadedSpeciesPtrTable + MODEL_NAME_OFF_FROM_PTR + s * SLOT_STRIDE
            if cached_name_matches(name_rva, model_path) and blob_is_healthy(s) then
                return s
            end
        end
    end
    return nil
end

local function get_creature_data(model_path)
    return model_path and kh1_creature_data[model_path] or nil
end

local function resolve_species_slot(model_path, excluded)
    local reused = find_loaded_slot_by_filename(model_path)
    if reused then return reused, false end
    local free = find_free_species_slot(excluded)
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
    WriteByte(record + PLACEMENT_RUNLEN_OFF, SLOT_RUN_LEN, true)
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
    local ok, entity = kh1_native.call_function(fnc_spawn_world_gimmick_entity, ctx.id)
    if not ok or entity == 0 then return nil end
    return entity
end

local function sora_position()
    local sora = g_SoraObjPtr and GetPointer(g_SoraObjPtr) or 0
    if sora == 0 then return 0.0, 0.0, 0.0 end
    return ReadFloat(sora + 0x10, true), ReadFloat(sora + 0x14, true), ReadFloat(sora + 0x18, true)
end

local function spawn_enemy(model_path, x, y, z)
    if not model_path then return false, "bad_args" end
    if inCutscene and ReadInt(inCutscene) ~= 0 then return false, "cutscene" end

    local data = get_creature_data(model_path)
    if not data then return false, "no_data" end

    local pending = pending_spawns[model_path]
    if pending then
        if slot_is_ready(pending.species) then
            local entity = construct_entity(pending.ctx)
            pending_spawns[model_path] = nil
            if entity then return true, entity end
            rollback_splice(pending.ctx)
            return false, "ctor_failed"
        end
        if os.clock() >= pending.deadline then
            pending_spawns[model_path] = nil
            return false, "timeout"
        end
        return false, "loading"
    end

    local excluded = {}
    for _, p in pairs(pending_spawns) do excluded[p.species] = true end
    local species, needs_load = resolve_species_slot(model_path, excluded)
    if not species then return false, "slots_full" end

    if x == nil or y == nil or z == nil then
        local sx, sy, sz = sora_position()
        x, y, z = x or sx, y or sy, z or sz
    end

    local ctx = splice_placement_record(species, data.template, data.charId, data.weight, x, y, z)
    if not ctx then return false, "splice_failed" end

    if not needs_load then
        local entity = construct_entity(ctx)
        if entity then return true, entity end
        rollback_splice(ctx)
        return false, "ctor_failed"
    end

    if not trigger_asset_load(ctx, model_path, data.motionPath) then
        rollback_splice(ctx)
        return false, "load_failed"
    end
    pending_spawns[model_path] = { ctx = ctx, species = species, deadline = os.clock() + SPAWN_LOAD_TIMEOUT }
    return false, "loading"
end

return {
    spawn_enemy = spawn_enemy,
}
