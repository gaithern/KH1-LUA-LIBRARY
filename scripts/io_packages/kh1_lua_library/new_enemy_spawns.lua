---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")
local kh1_creature_data = require("kh1_lua_library.creature_data")
local redirect = require("kh1_lua_library.species_redirect")

local native_log = kh1_native.log_debug or function(_) end
local function log(msg) native_log("[new_spawn] " .. msg) end

local PLACEMENT_RECORD_SIZE       = 0x78
local PLACEMENT_ID_OFF            = 0x00
local PLACEMENT_HANDLE_OFF        = 0x08
local PLACEMENT_POS_X_OFF         = 0x1C
local PLACEMENT_POS_Y_OFF         = 0x20
local PLACEMENT_POS_Z_OFF         = 0x24
local PLACEMENT_CHARID_OFF        = 0x4C
local PLACEMENT_SPECIES_OFF       = 0x55
local PLACEMENT_RUNLEN_OFF        = 0x56
local PLACEMENT_WEIGHT_OFF        = 0x59
local PLACEMENT_MODEL_HANDLE_OFF  = 0x60
local PLACEMENT_MOTION_HANDLE_OFF = 0x64
local RECORD_CATEGORY_ACTOR       = 3

local SLOT_STRIDE                 = 0x50
local STATE_OFF                   = 0x03  -- state byte, relative to owner-table slot base
local SLOT_STATE_READY            = 6
local SLOT_MAX                    = 49    -- highest usable game species slot
local SLOT_NONE                   = 0xFF  -- row no longer holds a game slot (freed post-construct)
local RESOURCE_BLOB_SIZE          = 0x40000
local BLOB_HEADER_FIRST_SECTION   = 128
local SPAWN_LOAD_TIMEOUT          = 5.0

local REUSE_ENABLED = false
-- Weight written into our spliced record. The constructor sums live-entity weights and returns 0
-- ("budget full") once the room cap is exceeded, so a low value lets far more of our spawns fit.
local SPAWN_WEIGHT = 1

local pending_spawns = {}
local installed = false

local buffer_pool = {}

local function acquire_buffer(need_size)
    for i = #buffer_pool, 1, -1 do
        local b = buffer_pool[i]
        if b.size >= need_size then
            table.remove(buffer_pool, i)
            return b.addr, b.size, true
        end
    end
    local addr = kh1_native.allocate(need_size)
    if addr == 0 then return nil end
    return addr, need_size, false
end

local function pool_buffer(addr, size)
    if addr and addr ~= 0 and size and size > 0 then
        buffer_pool[#buffer_pool + 1] = { addr = addr, size = size }
    end
end

local function ensure_installed()
    if installed then return true end
    local ok, why = redirect.install()
    if not ok then log("redirect install failed: " .. tostring(why)); return false end
    installed = true
    return true
end

local function model_hash(path)
    local h = 2166136261
    for i = 1, #path do
        h = (h ~ string.byte(path, i)) & 0xFFFFFFFF
        h = (h * 16777619) & 0xFFFFFFFF
    end
    return h
end

local function get_creature_data(model_path)
    return model_path and kh1_creature_data[model_path] or nil
end

local function sora_position()
    local sora = g_SoraObjPtr and GetPointer(g_SoraObjPtr) or 0
    if sora == 0 then return 0.0, 0.0, 0.0 end
    return ReadFloat(sora + 0x10, true), ReadFloat(sora + 0x14, true), ReadFloat(sora + 0x18, true)
end

local function boss_slowdown_active()
    if not g_GameSpeed then return false end
    local speed = ReadFloat(g_GameSpeed)
    return speed > 0.0 and speed < 0.9
end

local HANDLE_BUCKET_COUNT = 64
local HANDLE_BUCKET_HEADROOM = 6

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

local function slot_state(slot)
    return ReadByte(species_owner_table + slot * SLOT_STRIDE + STATE_OFF)
end
local function slot_is_ready(slot)
    return slot_state(slot) == SLOT_STATE_READY
end

local function blob_construct_safe(buf)
    local s = {}
    for i = 0, 4 do s[i] = ReadInt(buf + 4 + i * 4, true) end
    if s[0] ~= BLOB_HEADER_FIRST_SECTION then return false, string.format("hdr sec0=%d", s[0]) end
    local prev = 0
    for i = 0, 4 do
        if s[i] < 0 or s[i] < prev then return false, string.format("hdr not monotonic at %d", i) end
        prev = s[i]
    end
    return true
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

local function mint_ptr_handle(addr)
    local ok, handle = kh1_native.call_function(fnc_mint_resource_handle, addr)
    if not ok then return nil end
    return handle & 0xFFFFFFFF
end

-- Find a model we already loaded this session (from the persistent registry, so reuse survives F1).
local function find_our_loaded(hash)
    for _, r in ipairs(redirect.active_rows()) do
        if r.tag == hash and slot_is_ready(r.slot_n) then return r end
    end
    return nil
end

local function splice_placement_record(slot, template, char_id, weight, x, y, z)
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
    WriteByte(record + PLACEMENT_SPECIES_OFF, slot, true)
    WriteByte(record + PLACEMENT_RUNLEN_OFF, 1, true)  -- 1 game slot for bookkeeping; data is ours
    WriteByte(record + PLACEMENT_WEIGHT_OFF, SPAWN_WEIGHT, true)
    WriteInt(record + PLACEMENT_MODEL_HANDLE_OFF, 0, true)
    WriteInt(record + PLACEMENT_MOTION_HANDLE_OFF, 0, true)

    WriteLong(placementTablePtr, buffer)
    WriteInt(placementTableCount, new_count)

    return { buffer = buffer, record = record, id = id, slot = slot,
             old_ptr = old_ptr, old_count = old_count }
end

local function rollback_splice(ctx)
    if not ctx then return end
    WriteLong(placementTablePtr, ctx.old_ptr)
    WriteInt(placementTableCount, ctx.old_count)
end

local function trigger_asset_load(ctx, model_path, motion_path, buf)
    local model_handle = mint_string_handle(model_path)
    local motion_handle = mint_string_handle(motion_path)
    local buf_handle = mint_ptr_handle(buf)
    if not model_handle or not motion_handle or not buf_handle then return false end
    WriteInt(ctx.record + PLACEMENT_MODEL_HANDLE_OFF, model_handle, true)
    WriteInt(ctx.record + PLACEMENT_MOTION_HANDLE_OFF, motion_handle, true)
    WriteInt(ctx.record + PLACEMENT_HANDLE_OFF, buf_handle, true)  -- dest = our buffer
    WriteByte(species_owner_table + ctx.slot * SLOT_STRIDE + STATE_OFF, 0)  -- RVA global

    local base = kh1_native.get_module_base()
    local ok, job = kh1_native.call_function(fnc_alloc_async_job_slot,
        base + gimmick_job_pool, 0, base + fnc_async_load_job_callback, 0)
    if not ok or job == 0 then return false end
    WriteLong(job + 0x20, ctx.record, true)
    WriteLong(job + 0x28, 0, true)
    return true
end

local function construct_entity(ctx, buf)
    local safe, why = blob_construct_safe(buf)
    if not safe then
        log(string.format("REFUSE construct slot=%d id=0x%X: blob unsafe (%s)", ctx.slot, ctx.id, why))
        return nil
    end
    local ok, entity = kh1_native.call_function(fnc_spawn_world_gimmick_entity, ctx.id)
    if not ok then log("construct CRASHED (SafeCall caught)"); return nil end
    if entity == 0 then log("construct returned null (budget?)"); return nil end
    log(string.format("construct OK slot=%d entity=0x%X", ctx.slot, entity))
    return entity
end

local ENTITY_POOL_STRIDE = 0x4B0
local ENTITY_POOL_COUNT = 96
local ENTITY_OCC_FLAG_OFF = 0x374
local ENTITY_BLOB_HND_OFFS = { 0x68, 0x134, 0x138, 0x13c, 0x154, 0x1d0, 0x1d4 }
local RECLAIM_GRACE = 2
local dead_streak = {}

local function resolve_handle(h)
    if h == 0 then return 0 end
    h = h & 0x7FFFFFFF
    local addr = resource_handle_bucket_table + (h >> 25) * 8
    local lo = ReadInt(addr) & 0xFFFFFFFF
    local hi = ReadInt(addr + 4) & 0xFFFFFFFF
    if lo == 0xFFFFFFFF and hi == 0xFFFFFFFF then return 0 end
    return ((hi << 32) | lo) | (h & 0x1FFFFFF)
end

local function free_slot(slot_n)
    local base = species_owner_table + slot_n * SLOT_STRIDE
    WriteByte(base, 0xFF)
    WriteByte(base + 2, 0)
    WriteByte(base + 3, 0)
    WriteLong(loadedSpeciesPtrTable + slot_n * SLOT_STRIDE, 0)
end

local function live_rows(rows)
    local alive = {}
    for i = 0, ENTITY_POOL_COUNT - 1 do
        local e = entityPoolBase + i * ENTITY_POOL_STRIDE
        if (ReadInt(e + ENTITY_OCC_FLAG_OFF) & 1) ~= 0 then
            for _, off in ipairs(ENTITY_BLOB_HND_OFFS) do
                local resolved = resolve_handle(ReadInt(e + off) & 0xFFFFFFFF)
                if resolved ~= 0 then
                    for _, r in ipairs(rows) do
                        if resolved >= r.buf_base and resolved < r.buf_end then alive[r.row] = true break end
                    end
                end
            end
        end
    end
    return alive
end

local function reclaim_dead_rows()
    local rows = redirect.active_rows()
    if #rows == 0 then return end
    local pending_rows = {}
    for _, p in pairs(pending_spawns) do pending_rows[p.row] = true end
    local alive = live_rows(rows)
    local reclaimed, harvested = 0, 0
    for _, r in ipairs(rows) do
        if pending_rows[r.row] then
            dead_streak[r.row] = 0
        elseif alive[r.row] then
            dead_streak[r.row] = 0
            if r.slot_n <= SLOT_MAX then
                free_slot(r.slot_n)
                redirect.set_row_slot(r.row, SLOT_NONE)
                harvested = harvested + 1
            end
        elseif (dead_streak[r.row] or 0) + 1 >= RECLAIM_GRACE then
            if r.slot_n <= SLOT_MAX then
                local slot_ptr = ReadLong(loadedSpeciesPtrTable + r.slot_n * SLOT_STRIDE)
                if slot_ptr >= r.buf_base and slot_ptr < r.buf_end then free_slot(r.slot_n) end
            end
            pool_buffer(r.buf_base, r.buf_end - r.buf_base)
            redirect.release(r.row)
            dead_streak[r.row] = nil
            reclaimed = reclaimed + 1
            log(string.format("reclaim slot=%d buf=0x%X (dead)", r.slot_n, r.buf_base))
        else
            dead_streak[r.row] = (dead_streak[r.row] or 0) + 1
        end
    end
    if harvested > 0 then log(string.format("harvested %d slot(s) for reuse", harvested)) end
end

local last_room_world, last_room_area, last_room_set = nil, nil, nil

local function release_all_on_room_change()
    if not g_WorldNumber or not g_AreaNumber or not g_SetNumber then return end
    local w, a, s = ReadInt(g_WorldNumber), ReadInt(g_AreaNumber), ReadInt(g_SetNumber)
    if w == last_room_world and a == last_room_area and s == last_room_set then return end
    last_room_world, last_room_area, last_room_set = w, a, s
    local rows = redirect.active_rows()
    local released = 0
    for _, r in ipairs(rows) do
        if r.slot_n <= SLOT_MAX then
            local slot_ptr = ReadLong(loadedSpeciesPtrTable + r.slot_n * SLOT_STRIDE)
            if slot_ptr >= r.buf_base and slot_ptr < r.buf_end then
                free_slot(r.slot_n)
                released = released + 1
            end
        end
        pool_buffer(r.buf_base, r.buf_end - r.buf_base)
        redirect.release(r.row)
    end
    if #rows > 0 then log(string.format("room change: %d row(s), released %d still-holding slot(s)", #rows, released)) end
    pending_spawns = {}
end

local function spawn_enemy(model_path, x, y, z)
    if not model_path then log("called with nil model"); return false, "bad_args" end
    local function fail(reason) log(model_path .. " FAIL: " .. reason); return false, reason end
    if inCutscene and ReadInt(inCutscene) ~= 0 then return fail("cutscene") end
    if boss_slowdown_active() then return fail("boss_slowdown") end
    if not ensure_installed() then return fail("install_failed") end

    local data = get_creature_data(model_path)
    if not data then return fail("no_data") end

    release_all_on_room_change()
    reclaim_dead_rows()

    if x == nil or y == nil or z == nil then
        local sx, sy, sz = sora_position()
        x, y, z = x or sx, y or sy, z or sz
    end

    local hash = model_hash(model_path)

    local pending = pending_spawns[model_path]
    if pending then
        if slot_is_ready(pending.ctx.slot) then
            local entity = construct_entity(pending.ctx, pending.buf)
            pending_spawns[model_path] = nil
            if entity then return true, entity end
            rollback_splice(pending.ctx)
            return fail("ctor_failed")
        end
        if os.clock() >= pending.deadline then
            pending_spawns[model_path] = nil
            return fail("timeout")
        end
        return false, "loading"
    end

    local loaded = REUSE_ENABLED and find_our_loaded(hash)
    if loaded then
        local ctx = splice_placement_record(loaded.slot_n, data.template, data.charId, data.weight, x, y, z)
        if not ctx then return fail("splice_failed") end
        local buf_handle = mint_ptr_handle(loaded.buf_base)
        if not buf_handle then rollback_splice(ctx); return fail("mint_failed") end
        WriteInt(ctx.record + PLACEMENT_HANDLE_OFF, buf_handle, true)
        local entity = construct_entity(ctx, loaded.buf_base)
        if entity then return true, entity end
        rollback_splice(ctx)
        return fail("ctor_failed")
    end

    if handle_buckets_full() then return fail("handles_full") end

    local run_len = string.byte(data.template, PLACEMENT_RUNLEN_OFF + 1) or 1
    if run_len < 1 then run_len = 1 end
    local buf_size = (run_len + 2) * RESOURCE_BLOB_SIZE

    local slot = redirect.find_free_slot()
    if not slot then return fail("slots_full") end
    local buf, buf_actual, reused = acquire_buffer(buf_size)
    if not buf then return fail("buf_alloc_failed") end
    local row = redirect.claim(buf, buf + buf_actual, slot, hash)
    if not row then pool_buffer(buf, buf_actual); return fail("registry_full") end
    log(string.format("%s -> slot=%d buf=0x%X size=0x%X row=%d %s", model_path, slot, buf, buf_actual,
        row, reused and "(pooled)" or "(new)"))

    local ctx = splice_placement_record(slot, data.template, data.charId, data.weight, x, y, z)
    if not ctx then
        redirect.release(row); pool_buffer(buf, buf_actual)
        return fail("splice_failed")
    end

    if not trigger_asset_load(ctx, model_path, data.motionPath, buf) then
        rollback_splice(ctx); redirect.release(row); pool_buffer(buf, buf_actual)
        return fail("load_failed")
    end
    pending_spawns[model_path] = { ctx = ctx, buf = buf, row = row, deadline = os.clock() + SPAWN_LOAD_TIMEOUT }
    return false, "loading"
end

return {
    spawn_enemy = spawn_enemy,
}
