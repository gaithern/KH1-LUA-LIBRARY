---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")
local kh1_creature_data = require("kh1_lua_library.creature_data")

local native_log = kh1_native.log_debug or function(_) end
local function log(msg) native_log("[spawn_enemy] " .. msg) end

-- ############################################################ --
-- # Species-table redirect: persistent control block + caves  # --
-- ############################################################ --

local CTRL_KEY  = "kh1_species_redirect_v2"
local CTRL_SIZE = 0x4000
local MAGIC     = 0x52454432  -- 'RED2'

local OFF_MAGIC     = 0x00
local OFF_VERSION   = 0x04
local OFF_CAVE2     = 0x08
local OFF_COMP      = 0x10   -- address of the file-load completion cave
local OFF_BLOB_BASE = 0x18
local OFF_REG_COUNT = 0x28
local OFF_DONE_FLAG = 0x30   -- completion cave sets this to 1
local OFF_DONE_SIZE = 0x34   -- loaded size (ECX)
local OFF_DONE_DEST = 0x38   -- loaded dest (R8)

local ROWS_BASE  = 0x100
local ROW_STRIDE = 0x40
local ROW_BUFBASE = 0x00
local ROW_BUFEND  = 0x08
local ROW_MOTION  = 0x10   -- motion blob pointer (returned by cave2)
local ROW_ACTIVE  = 0x18
local ROW_TAG     = 0x1C
local MAX_ROWS = (CTRL_SIZE - ROWS_BASE) // ROW_STRIDE

local HOOK2_SPLICE_LEN = 14

local ctrl = nil
local module_base = nil

local function abs(rva) return module_base + rva end
local function ci(off) return ReadInt(ctrl + off, true) end
local function wi(off, v) WriteInt(ctrl + off, v, true) end
local function wl(off, v) WriteLong(ctrl + off, v, true) end
local function row_addr(i) return ctrl + ROWS_BASE + i * ROW_STRIDE end

local function rd_claim(buf_base, buf_end, tag)
    local count = ci(OFF_REG_COUNT)
    local free = nil
    for i = 0, count - 1 do
        if ReadInt(row_addr(i) + ROW_ACTIVE, true) == 0 then free = i break end
    end
    if not free then
        if count >= MAX_ROWS then return nil end
        free = count
        wi(OFF_REG_COUNT, count + 1)
    end
    local r = row_addr(free)
    WriteLong(r + ROW_BUFBASE, buf_base, true)
    WriteLong(r + ROW_BUFEND, buf_end, true)
    WriteLong(r + ROW_MOTION, 0, true)
    WriteInt(r + ROW_TAG, tag or 0, true)
    WriteInt(r + ROW_ACTIVE, 1, true)
    return free
end

local function rd_release(row)
    if row then WriteInt(row_addr(row) + ROW_ACTIVE, 0, true) end
end

local function rd_set_motion(row, ptr)
    if row then WriteLong(row_addr(row) + ROW_MOTION, ptr, true) end
end

local function rd_active_rows()
    local out = {}
    local count = ci(OFF_REG_COUNT)
    for i = 0, count - 1 do
        local r = row_addr(i)
        if ReadInt(r + ROW_ACTIVE, true) == 1 then
            out[#out + 1] = {
                row = i,
                buf_base = ReadLong(r + ROW_BUFBASE, true),
                buf_end = ReadLong(r + ROW_BUFEND, true),
                motion = ReadLong(r + ROW_MOTION, true),
                tag = ReadInt(r + ROW_TAG, true),
            }
        end
    end
    return out
end

local function rd_completion_addr() return ReadLong(ctrl + OFF_COMP, true) end
local function rd_clear_done() wi(OFF_DONE_FLAG, 0) end
local function rd_poll_done()
    if ci(OFF_DONE_FLAG) == 0 then return nil end
    return ReadLong(ctrl + OFF_DONE_DEST, true), ReadInt(ctrl + OFF_DONE_SIZE, true) & 0xFFFFFFFF
end

local sc = string.char
local function jmp_abs(target)
    return sc(0xFF, 0x25, 0x00, 0x00, 0x00, 0x00) .. string.pack("<I8", target)
end

-- hook2 (FUN_140285db0) splice. For a blob addr inside one of our rows, return that row's motion
-- pointer and RET; otherwise recompute the reverse-math index and fall through to the game's own
-- species-table lookup at hook2_resume. Byte layout verified against capstone (jump offsets fixed).
local function build_cave2()
    return
        sc(0x4C,0x8B,0xD1) ..                                   -- mov r10, rcx
        sc(0x48,0xB8) .. string.pack("<I8", ctrl) ..            -- mov rax, ctrl
        sc(0x44,0x8B,0x58,0x28) ..                              -- mov r11d, [rax+0x28]  (regCount)
        sc(0x45,0x85,0xDB) ..                                   -- test r11d, r11d
        sc(0x0F,0x84,0x3D,0x00,0x00,0x00) ..                    -- jz FALLBACK
        sc(0x4C,0x8D,0x80,0x00,0x01,0x00,0x00) ..               -- lea r8, [rax+0x100] (rows)
        sc(0x41,0x83,0x78,0x18,0x00) ..                         -- LOOP: cmp dword [r8+0x18], 0 (active)
        sc(0x0F,0x84,0x1E,0x00,0x00,0x00) ..                    -- jz NEXT
        sc(0x49,0x8B,0x00) ..                                   -- mov rax, [r8]      (bufBase)
        sc(0x49,0x39,0xC2) ..                                   -- cmp r10, rax
        sc(0x0F,0x82,0x12,0x00,0x00,0x00) ..                    -- jb NEXT
        sc(0x49,0x8B,0x40,0x08) ..                              -- mov rax, [r8+8]    (bufEnd)
        sc(0x49,0x39,0xC2) ..                                   -- cmp r10, rax
        sc(0x0F,0x83,0x05,0x00,0x00,0x00) ..                    -- jae NEXT
        sc(0x49,0x8B,0x40,0x10) ..                              -- mov rax, [r8+0x10] (motionPtr)
        sc(0xC3) ..                                             -- ret
        sc(0x49,0x83,0xC0,0x40) ..                              -- NEXT: add r8, 0x40
        sc(0x41,0xFF,0xCB) ..                                   -- dec r11d
        sc(0x0F,0x85,0xCA,0xFF,0xFF,0xFF) ..                    -- jnz LOOP
        sc(0x48,0xB8) .. string.pack("<I8", abs(speciesResourceTable)) .. -- FALLBACK: mov rax, blob_base
        sc(0x48,0x2B,0xC8) ..                                   -- sub rcx, rax
        sc(0x48,0xC1,0xE9,0x12) ..                              -- shr rcx, 0x12
        jmp_abs(abs(fnc_reverse_blob_to_slot_ptr + HOOK2_SPLICE_LEN)) -- jmp hook2_resume
end

-- File-load completion callback: (ECX=size, R8=dest). Stash both and raise a flag the driver polls.
local function build_completion()
    return
        sc(0x48,0xB8) .. string.pack("<I8", ctrl + OFF_DONE_DEST) .. sc(0x4C,0x89,0x00) ..  -- mov [dest], r8
        sc(0x48,0xB8) .. string.pack("<I8", ctrl + OFF_DONE_SIZE) .. sc(0x89,0x08) ..        -- mov [size], ecx
        sc(0x48,0xB8) .. string.pack("<I8", ctrl + OFF_DONE_FLAG) .. sc(0xC7,0x00,0x01,0x00,0x00,0x00) .. -- mov [flag], 1
        sc(0xC3)
end

local function rd_is_installed()
    return ctrl ~= nil and ci(OFF_MAGIC) == MAGIC
end

local function rd_install()
    module_base = kh1_native.get_module_base()
    ctrl = kh1_native.persistent_block(CTRL_KEY, CTRL_SIZE)
    if ctrl == 0 or ctrl == nil then return false, "persistent_block failed" end
    if rd_is_installed() then return true, "already installed" end

    local cave2 = kh1_native.allocate(0x200, 1)
    local comp = kh1_native.allocate(0x80, 1)
    if cave2 == 0 or comp == 0 then return false, "cave alloc failed" end

    wl(OFF_BLOB_BASE, abs(speciesResourceTable))
    wl(OFF_CAVE2, cave2)
    wl(OFF_COMP, comp)
    wi(OFF_REG_COUNT, 0)
    wi(OFF_DONE_FLAG, 0)

    kh1_native.write_bytes(cave2, build_cave2())
    kh1_native.write_bytes(comp, build_completion())
    kh1_native.patch_code(abs(fnc_reverse_blob_to_slot_ptr), jmp_abs(cave2))

    wi(OFF_VERSION, 2)
    wi(OFF_MAGIC, MAGIC)
    return true, "installed"
end

-- ############################################################ --
-- # Spawn driver                                             # --
-- ############################################################ --

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

local RESOURCE_BLOB_SIZE          = 0x40000
local BLOB_HEADER_FIRST_SECTION   = 128
local SPAWN_LOAD_TIMEOUT          = 5.0
local SPAWN_WEIGHT                = 1  -- low weight so the room's concurrent-enemy budget fits many

local pending_spawns = {}
local installed = false

-- Handle-bucket-bounded arena allocator: every buffer is carved from a few 32MB-aligned arenas so
-- all buffers in one arena share ONE handle bucket (the game never frees buckets).
local ARENA_SIZE = 0x2000000
local ARENA_CELL = RESOURCE_BLOB_SIZE
local ARENA_CAP  = 6
local arenas = {}
local buffer_pool = {}

local function arena_carve(need)
    local sz = (need + (ARENA_CELL - 1)) & ~(ARENA_CELL - 1)
    if sz > ARENA_SIZE then return nil end
    for _, a in ipairs(arenas) do
        if a.bump + sz <= ARENA_SIZE then
            local addr = a.base + a.bump
            a.bump = a.bump + sz
            return addr, sz
        end
    end
    if #arenas >= ARENA_CAP then return nil end
    local raw = kh1_native.allocate(ARENA_SIZE * 2)
    if raw == 0 then return nil end
    local base = (raw + (ARENA_SIZE - 1)) & ~(ARENA_SIZE - 1)
    arenas[#arenas + 1] = { base = base, bump = sz }
    log(string.format("arena %d: base=0x%X (1 handle bucket)", #arenas, base))
    return base, sz
end

local function acquire_buffer(need_size)
    local best_i, best
    for i = #buffer_pool, 1, -1 do
        local b = buffer_pool[i]
        if b.size >= need_size and (not best or b.size < best.size) then best_i, best = i, b end
    end
    if best then
        table.remove(buffer_pool, best_i)
        return best.addr, best.size, true
    end
    local addr, sz = arena_carve(need_size)
    if not addr then return nil end
    return addr, sz, false
end

local function pool_buffer(addr, size)
    if addr and addr ~= 0 and size and size > 0 then
        buffer_pool[#buffer_pool + 1] = { addr = addr, size = size }
    end
end

local function ensure_installed()
    if installed then return true end
    local ok, why = rd_install()
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

-- Blob header sanity: first section marker present and section offsets monotonic. This is our
-- readiness gate now that no species slot tracks load state.
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

local function mint_ptr_handle(addr)
    local ok, handle = kh1_native.call_function(fnc_mint_resource_handle, addr)
    if not ok then return nil end
    return handle & 0xFFFFFFFF
end

local function splice_placement_record(template, char_id, x, y, z)
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
    WriteByte(record + PLACEMENT_SPECIES_OFF, 0, true)   -- ignored: constructor uses record+0x08
    WriteByte(record + PLACEMENT_RUNLEN_OFF, 1, true)
    WriteByte(record + PLACEMENT_WEIGHT_OFF, SPAWN_WEIGHT, true)
    WriteInt(record + PLACEMENT_MODEL_HANDLE_OFF, 0, true)
    WriteInt(record + PLACEMENT_MOTION_HANDLE_OFF, 0, true)

    WriteLong(placementTablePtr, buffer)
    WriteInt(placementTableCount, new_count)

    return { buffer = buffer, record = record, id = id, old_ptr = old_ptr, old_count = old_count }
end

local function rollback_splice(ctx)
    if not ctx then return end
    WriteLong(placementTablePtr, ctx.old_ptr)
    WriteInt(placementTableCount, ctx.old_count)
end

-- Blob relocation, replicating exactly what hook1/motion-callback do (same functions, same args),
-- minus the species-slot writes. If any fault, SafeCall returns false and we refuse the spawn.
local function call_ok(fn, arg)
    return (kh1_native.call_function(fn, arg))
end

local function model_fixup(dest)
    if not call_ok(fnc_blob_reloc, dest + ReadInt(dest + 4, true)) then return false end
    if ReadInt(dest + 0xC, true) - ReadInt(dest + 8, true) > 0 then
        if not call_ok(fnc_blob_fixup_a, dest + ReadInt(dest + 8, true)) then return false end
    end
    if ReadInt(dest + 0x18, true) - ReadInt(dest + 0x14, true) > 0 then
        if not call_ok(fnc_blob_fixup_b, dest + ReadInt(dest + 0x14, true)) then return false end
    end
    return true
end

local function motion_fixup(dest)
    if not call_ok(fnc_blob_reloc, dest + ReadInt(dest + 4, true)) then return false end
    if ReadInt(dest + 0x10, true) - ReadInt(dest + 0xC, true) > 0 then
        local base2 = dest + ReadInt(dest + 0xC, true)
        for k = 0, 9 do
            local lo = ReadInt(base2 + 4 + k * 4, true)
            local hi = ReadInt(base2 + 8 + k * 4, true)
            if hi - lo > 0 then
                if not call_ok(fnc_motion_fixup, base2 + lo) then return false end
            end
        end
    end
    return true
end

local function construct_entity(ctx, buf)
    local safe, why = blob_construct_safe(buf)
    if not safe then
        log(string.format("REFUSE construct id=0x%X: blob unsafe (%s)", ctx.id, why))
        return nil
    end
    local ok, entity = kh1_native.call_function(fnc_spawn_world_gimmick_entity, ctx.id)
    if not ok then log("construct CRASHED (SafeCall caught)"); return nil end
    if entity == 0 then log("construct returned null (budget?)"); return nil end
    log(string.format("construct OK entity=0x%X", entity))
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

local function cleanup_pending(model_path, p)
    rollback_splice(p.ctx)
    rd_release(p.row)
    pool_buffer(p.buf, p.size)
    pending_spawns[model_path] = nil
end

local function reclaim_dead_rows()
    local now_c = os.clock()
    for model, p in pairs(pending_spawns) do
        if now_c >= p.deadline + 2.0 then
            rollback_splice(p.ctx)
            rd_release(p.row)
            pool_buffer(p.buf, p.size)
            pending_spawns[model] = nil
            log("swept stale load " .. model)
        end
    end
    local rows = rd_active_rows()
    if #rows == 0 then return end
    local pending_rows = {}
    for _, p in pairs(pending_spawns) do pending_rows[p.row] = true end
    local alive = live_rows(rows)
    for _, r in ipairs(rows) do
        if pending_rows[r.row] or alive[r.row] then
            dead_streak[r.row] = 0
        elseif (dead_streak[r.row] or 0) + 1 >= RECLAIM_GRACE then
            pool_buffer(r.buf_base, r.buf_end - r.buf_base)
            rd_release(r.row)
            dead_streak[r.row] = nil
        else
            dead_streak[r.row] = (dead_streak[r.row] or 0) + 1
        end
    end
end

local last_room_world, last_room_area, last_room_set = nil, nil, nil

local function release_all_on_room_change()
    if not g_WorldNumber or not g_AreaNumber or not g_SetNumber then return end
    local w, a, s = ReadInt(g_WorldNumber), ReadInt(g_AreaNumber), ReadInt(g_SetNumber)
    if w == last_room_world and a == last_room_area and s == last_room_set then return end
    last_room_world, last_room_area, last_room_set = w, a, s
    local rows = rd_active_rows()
    for _, r in ipairs(rows) do
        pool_buffer(r.buf_base, r.buf_end - r.buf_base)
        rd_release(r.row)
    end
    if #rows > 0 then log(string.format("room change: released %d row(s)", #rows)) end
    pending_spawns = {}
end

local function queue_load(filename, dest)
    local fname = intern_path(filename)
    if fname == 0 then return false end
    rd_clear_done()
    return (kh1_native.call_function(fnc_queue_file_load_to_address, fname, dest, rd_completion_addr(), 0))
end

-- Drive a spawn already loading: model -> motion -> construct, one file at a time. The CC driver
-- polls the same request every frame until it succeeds/fails, so only one load is ever in flight.
local function drive_pending(model_path, p)
    local dest = rd_poll_done()
    if p.stage == 0 then
        if dest ~= p.buf then
            if dest then rd_clear_done() end                       -- stale completion, ignore
            if os.clock() >= p.deadline then cleanup_pending(model_path, p); return false, "timeout" end
            return false, "loading"
        end
        local _, size = rd_poll_done()
        rd_clear_done()
        if not model_fixup(p.buf) then cleanup_pending(model_path, p); return false, "load_failed" end
        local motion_dest = p.buf + ((size + 0x7F) & ~0x7F)
        if motion_dest + RESOURCE_BLOB_SIZE > p.buf + p.size then
            cleanup_pending(model_path, p); return false, "buf_alloc_failed"
        end
        p.motion_dest = motion_dest
        rd_set_motion(p.row, motion_dest)
        if not queue_load(p.motion_path, motion_dest) then cleanup_pending(model_path, p); return false, "load_failed" end
        p.stage = 1
        p.deadline = os.clock() + SPAWN_LOAD_TIMEOUT
        return false, "loading"
    elseif p.stage == 1 then
        if dest ~= p.motion_dest then
            if dest then rd_clear_done() end
            if os.clock() >= p.deadline then cleanup_pending(model_path, p); return false, "timeout" end
            return false, "loading"
        end
        rd_clear_done()
        if not motion_fixup(p.motion_dest) then cleanup_pending(model_path, p); return false, "load_failed" end
        p.stage = 2
    end
    -- stage 2: construct (motion resolves through cave2 -> row.motion)
    local entity = construct_entity(p.ctx, p.buf)
    pending_spawns[model_path] = nil
    if entity then return true, entity end
    rollback_splice(p.ctx); rd_release(p.row); pool_buffer(p.buf, p.size)
    return false, "ctor_failed"
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

    local p = pending_spawns[model_path]
    if p then return drive_pending(model_path, p) end

    if handle_buckets_full() then return fail("handles_full") end

    local run_len = string.byte(data.template, PLACEMENT_RUNLEN_OFF + 1) or 1
    if run_len < 1 then run_len = 1 end
    local buf_size = (run_len + 2) * RESOURCE_BLOB_SIZE

    local buf, buf_actual = acquire_buffer(buf_size)
    if not buf then return fail("buf_alloc_failed") end
    local hash = model_hash(model_path)
    local row = rd_claim(buf, buf + buf_actual, hash)
    if not row then pool_buffer(buf, buf_actual); return fail("registry_full") end

    local ctx = splice_placement_record(data.template, data.charId, x, y, z)
    if not ctx then rd_release(row); pool_buffer(buf, buf_actual); return fail("splice_failed") end

    local buf_handle = mint_ptr_handle(buf)
    if not buf_handle then
        rollback_splice(ctx); rd_release(row); pool_buffer(buf, buf_actual); return fail("mint_failed")
    end
    WriteInt(ctx.record + PLACEMENT_HANDLE_OFF, buf_handle, true)

    if not queue_load(model_path, buf) then
        rollback_splice(ctx); rd_release(row); pool_buffer(buf, buf_actual); return fail("load_failed")
    end
    log(string.format("%s -> buf=0x%X size=0x%X row=%d (loading)", model_path, buf, buf_actual, row))
    pending_spawns[model_path] = { ctx = ctx, buf = buf, size = buf_actual, row = row,
        stage = 0, motion_path = data.motionPath, deadline = os.clock() + SPAWN_LOAD_TIMEOUT }
    return false, "loading"
end

return {
    spawn_enemy = spawn_enemy,
}
