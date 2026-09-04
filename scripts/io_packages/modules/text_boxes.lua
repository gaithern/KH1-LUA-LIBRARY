---@diagnostic disable: undefined-global

-- Notification text boxes on one shared window, queued by the game itself.
--
-- The EV window system keeps a per-window message queue and plays messages back to back. A
-- control byte 05 lo hi inside a message holds it on screen for lo|hi<<8 ticks (30 per second)
-- after it has been fully shown, immune to the confirm button (06 is the skippable variant), and a window opened with Open_window closes itself once its
-- queue is empty. So queue_text_box only appends: each message gets its own EV string slot and
-- buffer, and the game handles ordering, timing and closing. The library's driver script runs
-- update_text_boxes every frame to feed held messages, play per-message sounds and repair the
-- window when an EV script load wipes the game bookkeeping under it. All state lives in a
-- persistent block, so every Lua state shares one queue and nothing is lost on a hot reload.

local kh1_native = require("kh1_native")
local khscii = require("helpers.khscii")

local GetKHSCII = khscii.GetKHSCII

local NOTIFY_WINDOW    = 6
local SLOT_COUNT       = 32   -- EV string slots reserved at the top of the table = messages in flight
local BUF_SIZE         = 512
local HELD_COUNT       = 32   -- messages waiting while the window is busy or full
local TICKS_PER_SECOND = 30
local DEFAULT_DURATION = 2.5
local GHOST_GRACE_MS   = 500  -- an open window with nothing queued for this long belongs to nobody
local EV_STRING_TABLE_MAX = 1536
local SLOT_BASE        = EV_STRING_TABLE_MAX - SLOT_COUNT  -- 1504..1535: above any script, never refilled
local WINDOW_SLOTS     = 4
local MSG_QUEUE_STRIDE = 0x200
local MSG_QUEUE_ENTRIES = 0x100

local BLOCK_KEY  = "kh1_textbox_queue_v8"
local MAGIC      = 0x54425138
local TEMPLATE_SIZE = 0xD0
local INSTANCE_SIZE = 0xD0
local INST_CLOSE_SPEED = 0x12  -- u16 frames on a live instance (looked up from the index at open)
local TPL_CLOSE_SPEED_INDEX = 0xE  -- u16 index into the close speed table {0, 4, 8, 20} frames; 0 = instant
local INST_STATE = 0x20        -- 0 none, 1 opening, 2 open, 3 close requested, 4 closing, 5 finishing
local INST_CALLBACK = 0x18     -- completion callback pointer
local INST_STATE_FINISH = 5
local INST_TEXT_PTR = 0x88     -- KHSCII pointer of the message the instance is showing

local HDR_MAGIC       = 0x00
local HDR_NEXT_BUF    = 0x10
local HDR_HELD_HEAD   = 0x14
local HDR_HELD_COUNT  = 0x18
local HDR_LAST_POS    = 0x1C
local HDR_LAST_STATE  = 0x20
local HDR_LOOK_DIRTY  = 0x24  -- 1 while the window template holds our look
local HDR_OUR_WINDOW  = 0x28  -- 1 while a window we opened is alive
local HDR_LAST_COUNT  = 0x2C
local HDR_LAST_TABLE  = 0x30  -- g_EVScriptLoaded as last seen by the driver
local HDR_GOOD_POS    = 0x34  -- position/count last seen while the window had messages
local HDR_GOOD_COUNT  = 0x38
local HDR_GHOST_SINCE = 0x3C
local HDR_SHOWN_AT_MS = 0x40  -- last time a message became visible, for the timing log
local HDR_LOOK_FLAGS  = 0x44  -- which look fields the open window was opened with
local HDR_LOOK_STYLE  = 0x48
local HDR_LOOK_X      = 0x4C
local HDR_LOOK_Y      = 0x50
local HDR_LOOK_WIDTH  = 0x54
local HDR_LOOK_HEIGHT = 0x58
local HDR_LOOK_TAIL   = 0x5C
local HDR_LAST_TEXT   = 0x60  -- text pointer last seen on our window (8 bytes)
local OFF_SOUNDS      = 0x80                                -- int32[MSG_QUEUE_ENTRIES], by buffer index
local OFF_BUFFERS     = OFF_SOUNDS + MSG_QUEUE_ENTRIES * 4  -- SLOT_COUNT * BUF_SIZE
local OFF_HELD        = OFF_BUFFERS + SLOT_COUNT * BUF_SIZE
local HELD_TEXT_MAX   = 511
local HELD_DURATION   = 0x200
local HELD_SOUND      = 0x204
local HELD_STYLE      = 0x208
local HELD_X          = 0x20C
local HELD_Y          = 0x210
local HELD_WIDTH      = 0x214
local HELD_HEIGHT     = 0x218
local HELD_TAIL       = 0x21C
local HELD_FLAGS      = 0x220
local HELD_STRIDE     = 0x240
local OFF_TEMPLATE    = OFF_HELD + HELD_COUNT * HELD_STRIDE  -- snapshot of the vanilla template
local OFF_MIRROR      = OFF_TEMPLATE + TEMPLATE_SIZE          -- held-format copy of each in-flight message, by buffer
local OFF_POS_BUF     = OFF_MIRROR + SLOT_COUNT * HELD_STRIDE -- u8[MSG_QUEUE_ENTRIES]: buffer index by queue position
local BLOCK_SIZE      = OFF_POS_BUF + MSG_QUEUE_ENTRIES

local FLAG_STYLE = 1
local FLAG_POS   = 2
local FLAG_SIZE  = 4
local FLAG_TAIL  = 8
local FLAG_SOUND = 16

local function now_ms()
    return math.floor(os.clock() * 1000)
end

local function block()
    local blk = kh1_native.persistent_block(BLOCK_KEY, BLOCK_SIZE)
    if blk == 0 or blk == nil then return nil end
    if ReadInt(blk + HDR_MAGIC, true) ~= MAGIC then
        WriteInt(blk + HDR_NEXT_BUF, 0, true)
        WriteInt(blk + HDR_HELD_HEAD, 0, true)
        WriteInt(blk + HDR_HELD_COUNT, 0, true)
        WriteInt(blk + HDR_LAST_POS, -1, true)
        WriteInt(blk + HDR_LAST_STATE, 0, true)
        WriteInt(blk + HDR_LOOK_DIRTY, 0, true)
        WriteInt(blk + HDR_OUR_WINDOW, 0, true)
        WriteInt(blk + HDR_LAST_COUNT, 0, true)
        WriteInt(blk + HDR_LAST_TABLE, 0, true)
        WriteInt(blk + HDR_GOOD_POS, 0, true)
        WriteInt(blk + HDR_GOOD_COUNT, 0, true)
        WriteInt(blk + HDR_GHOST_SINCE, 0, true)
        WriteInt(blk + HDR_SHOWN_AT_MS, 0, true)
        WriteInt(blk + HDR_LOOK_FLAGS, 0, true)
        WriteLong(blk + HDR_LAST_TEXT, 0, true)
        WriteInt(blk + HDR_MAGIC, MAGIC, true)
    end
    return blk
end

-- ######################## --
-- # Game window state    # --
-- ######################## --

local function window_state()
    return ReadByte(g_EVWindowState + NOTIFY_WINDOW)
end

local function message_count()
    return ReadInt(g_EVWindowMsgCount + NOTIFY_WINDOW * 4)
end

local function message_position()
    return ReadInt(g_EVWindowMsgPosition)
end

local function last_message_id()
    local count = message_count()
    if count < 1 or count > MSG_QUEUE_ENTRIES then return nil end
    return ReadShort(g_EVWindowMsgQueue + NOTIFY_WINDOW * MSG_QUEUE_STRIDE + (count - 1) * 2)
end

local function window_closing()
    for slot = 0, WINDOW_SLOTS - 1 do
        if ReadByte(g_EVWindowStateArray + slot) == NOTIFY_WINDOW then
            return ReadByte(g_EVWindowActivity + slot) == 3
        end
    end
    return false
end

-- The drawn window instance slot for our window id, or nil. The slot map survives a wipe.
local function instance_slot()
    for slot = 0, WINDOW_SLOTS - 1 do
        if ReadByte(g_EVWindowStateArray + slot) == NOTIFY_WINDOW then return slot end
    end
    return nil
end

local function instance_state(slot)
    return ReadInt(g_EVWindowInstances + slot * INSTANCE_SIZE + INST_STATE)
end

-- Every window syscall is a silent no-op while the cutscene bit or the cutscene check is set.
local function window_syscalls_enabled()
    return ReadByte(inCutscene) % 2 == 0 and ReadInt(g_EVCutsceneCheck) == 0
end

-- Bit 1 of the same byte is event mode: Enter_event_mode sets it, Exit_event_mode clears it.
-- Notifications wait it out so they never sit on top of story dialogue; the held ring keeps them.
local function event_active()
    return math.floor(ReadByte(inCutscene) / 2) % 2 == 1
end

-- The buffer index our window is currently showing, or nil. Read from the instance itself, so
-- other windows opening or advancing cannot confuse it (the position global is shared).
local function showing_index(blk)
    local slot = instance_slot()
    if not slot or window_state() ~= 2 then return nil end
    local ptr = ReadLong(g_EVWindowInstances + slot * INSTANCE_SIZE + INST_TEXT_PTR)
    local rel = ptr - (blk + OFF_BUFFERS)
    if rel < 0 or rel % BUF_SIZE ~= 0 or rel / BUF_SIZE >= SLOT_COUNT then return nil end
    return rel / BUF_SIZE
end

local function is_text_box_busy(window_id)
    return ReadByte(g_EVWindowState + (window_id or NOTIFY_WINDOW)) ~= 0
end

-- ######################## --
-- # EV string slots      # --
-- ######################## --

-- Our messages use the top 32 entries of the EV string table. Script loads refill indices
-- 0..count-1 only and the game appends its own strings at count, so nothing else ever writes
-- there as long as the loaded script has fewer than 1504 strings. We never touch the count. The
-- pointers are checked on every show and rewritten if anything clobbered them.
local function ensure_slots(blk)
    if ReadInt(g_EVScriptLoaded) > SLOT_BASE then return nil end
    if ReadLong(g_pEVStringDataPtr + SLOT_BASE * 8) ~= blk + OFF_BUFFERS then
        for i = 0, SLOT_COUNT - 1 do
            WriteLong(g_pEVStringDataPtr + (SLOT_BASE + i) * 8, blk + OFF_BUFFERS + i * BUF_SIZE)
        end
        ConsolePrint(string.format("kh1_lua_library: EV string slots %d..%d pointed at the notification buffers", SLOT_BASE, SLOT_BASE + SLOT_COUNT - 1))
    end
    return SLOT_BASE
end

local function owns_window(blk)
    if window_state() == 0 then return false end
    local id = last_message_id()
    return id ~= nil and id >= SLOT_BASE and id < SLOT_BASE + SLOT_COUNT
end

-- Messages queued on the window that have not finished showing. The position global is only
-- meaningful while the window is displaying or idle; while it is closed or still opening,
-- everything queued is still ahead.
local function in_flight(blk)
    local count = message_count()
    local state = window_state()
    if state == 4 then return 0 end
    if state ~= 2 then return count end
    local index = showing_index(blk)
    if index == nil then return count end
    for pos = count - 1, 0, -1 do
        if ReadByte(blk + OFF_POS_BUF + pos, true) == index then return count - pos end
    end
    return count
end

-- ######################## --
-- # Message records      # --
-- ######################## --

local function held_addr(blk, index)
    return blk + OFF_HELD + (index % HELD_COUNT) * HELD_STRIDE
end

local function write_record(rec, m)
    local n = math.min(#m.text, HELD_TEXT_MAX)
    local bytes = {}
    for i = 1, n do bytes[i] = m.text:byte(i) end
    bytes[n + 1] = 0
    WriteArray(rec, bytes, true)
    local flags = 0
    WriteInt(rec + HELD_DURATION, math.floor((m.duration or DEFAULT_DURATION) * 1000), true)
    if m.sound ~= nil then flags = flags + FLAG_SOUND; WriteInt(rec + HELD_SOUND, m.sound, true) end
    if m.style ~= nil then flags = flags + FLAG_STYLE; WriteInt(rec + HELD_STYLE, m.style, true) end
    if m.x ~= nil and m.y ~= nil then
        flags = flags + FLAG_POS
        WriteInt(rec + HELD_X, m.x, true)
        WriteInt(rec + HELD_Y, m.y, true)
    end
    if m.width ~= nil and m.height ~= nil then
        flags = flags + FLAG_SIZE
        WriteInt(rec + HELD_WIDTH, m.width, true)
        WriteInt(rec + HELD_HEIGHT, m.height, true)
    end
    if m.tail ~= nil then flags = flags + FLAG_TAIL; WriteInt(rec + HELD_TAIL, m.tail, true) end
    WriteInt(rec + HELD_FLAGS, flags, true)
end

local function read_record(rec)
    local bytes = ReadArray(rec, HELD_TEXT_MAX + 1, true)
    local chars = {}
    for i = 1, #bytes do
        if bytes[i] == 0 then break end
        chars[#chars + 1] = string.char(bytes[i])
    end
    local flags = ReadInt(rec + HELD_FLAGS, true)
    local function opt(flag, off)
        if flags % (flag * 2) >= flag then return ReadInt(rec + off, true) end
        return nil
    end
    local m = {
        text = table.concat(chars),
        duration = ReadInt(rec + HELD_DURATION, true) / 1000,
        sound = opt(FLAG_SOUND, HELD_SOUND),
        style = opt(FLAG_STYLE, HELD_STYLE),
        x = opt(FLAG_POS, HELD_X),
        y = opt(FLAG_POS, HELD_Y),
        width = opt(FLAG_SIZE, HELD_WIDTH),
        height = opt(FLAG_SIZE, HELD_HEIGHT),
        tail = opt(FLAG_TAIL, HELD_TAIL),
    }
    return m
end

-- ######################## --
-- # Showing a message    # --
-- ######################## --

local function dwell_escape(duration_seconds)
    local ticks = math.floor((duration_seconds or DEFAULT_DURATION) * TICKS_PER_SECOND)
    if ticks < 1 then ticks = 1 end
    if ticks > 0xFFFF then ticks = 0xFFFF end
    -- 05 lo hi is the timed wait the pad cannot skip (06 lo hi is the skippable one the tutorials use)
    return string.format("{0x05}{0x%02X}{0x%02X}", ticks % 256, math.floor(ticks / 256))
end

local function write_buffer(blk, index, text, duration_seconds)
    local khs = GetKHSCII(text .. dwell_escape(duration_seconds))
    local n = math.min(#khs, BUF_SIZE - 1)
    local bytes = {}
    for i = 1, n do bytes[i] = khs[i] end
    bytes[n + 1] = 0
    WriteArray(blk + OFF_BUFFERS + index * BUF_SIZE, bytes, true)
end

-- Set_window_* calls change window 6's TEMPLATE, which every later open of window 6 copies,
-- including vanilla prompts that rely on the defaults. Snapshot it before our look goes in and
-- put it back once our window has closed.
local function template_addr()
    return g_EVWindowTemplates + NOTIFY_WINDOW * TEMPLATE_SIZE
end

local function save_template(blk)
    if ReadInt(blk + HDR_LOOK_DIRTY, true) == 1 then return end
    WriteArray(blk + OFF_TEMPLATE, ReadArray(template_addr(), TEMPLATE_SIZE), true)
    WriteInt(blk + HDR_LOOK_DIRTY, 1, true)
end

local function restore_template(blk)
    if ReadInt(blk + HDR_LOOK_DIRTY, true) ~= 1 then return end
    WriteArray(template_addr(), ReadArray(blk + OFF_TEMPLATE, TEMPLATE_SIZE, true))
    WriteInt(blk + HDR_LOOK_DIRTY, 0, true)
end

local function apply_look(m)
    if m.style ~= nil then
        kh1_native.call_evdl_syscall(fnc_005_set_window_type, {NOTIFY_WINDOW, m.style})
    end
    if m.tail ~= nil then
        kh1_native.call_evdl_syscall(fnc_050_set_window_tail_type, {NOTIFY_WINDOW, m.tail})
    end
    if m.x ~= nil and m.y ~= nil then
        kh1_native.call_evdl_syscall(fnc_003_set_window_position, {NOTIFY_WINDOW, m.x, m.y})
    end
    if m.width ~= nil and m.height ~= nil then
        kh1_native.call_evdl_syscall(fnc_004_set_window_size, {NOTIFY_WINDOW, m.width, m.height})
    end
end

-- A window keeps the look it was opened with, so a message that asks for a different style,
-- position, size or tail waits for the window to drain and close, then opens its own.
local function record_look(blk, m)
    local flags = 0
    if m.style ~= nil then flags = flags + FLAG_STYLE; WriteInt(blk + HDR_LOOK_STYLE, m.style, true) end
    if m.x ~= nil and m.y ~= nil then
        flags = flags + FLAG_POS
        WriteInt(blk + HDR_LOOK_X, m.x, true)
        WriteInt(blk + HDR_LOOK_Y, m.y, true)
    end
    if m.width ~= nil and m.height ~= nil then
        flags = flags + FLAG_SIZE
        WriteInt(blk + HDR_LOOK_WIDTH, m.width, true)
        WriteInt(blk + HDR_LOOK_HEIGHT, m.height, true)
    end
    if m.tail ~= nil then flags = flags + FLAG_TAIL; WriteInt(blk + HDR_LOOK_TAIL, m.tail, true) end
    WriteInt(blk + HDR_LOOK_FLAGS, flags, true)
end

local function look_matches(blk, m)
    local flags = ReadInt(blk + HDR_LOOK_FLAGS, true)
    local function has(flag) return flags % (flag * 2) >= flag end
    if m.style ~= nil and (not has(FLAG_STYLE) or ReadInt(blk + HDR_LOOK_STYLE, true) ~= m.style) then return false end
    if m.x ~= nil and m.y ~= nil and (not has(FLAG_POS)
        or ReadInt(blk + HDR_LOOK_X, true) ~= m.x or ReadInt(blk + HDR_LOOK_Y, true) ~= m.y) then return false end
    if m.width ~= nil and m.height ~= nil and (not has(FLAG_SIZE)
        or ReadInt(blk + HDR_LOOK_WIDTH, true) ~= m.width or ReadInt(blk + HDR_LOOK_HEIGHT, true) ~= m.height) then return false end
    if m.tail ~= nil and (not has(FLAG_TAIL) or ReadInt(blk + HDR_LOOK_TAIL, true) ~= m.tail) then return false end
    return true
end

-- The window is free for a message when it is closed, or open with our own messages on it, not
-- already closing, and opened with the same look. In-flight messages must not exceed our ring.
local function can_show(blk, m)
    if not window_syscalls_enabled() or event_active() then return false end
    if in_flight(blk) >= SLOT_COUNT then return false end
    if window_state() == 0 then return true end
    return owns_window(blk) and not window_closing() and look_matches(blk, m)
end

local function show(blk, m)
    local base = ensure_slots(blk)
    if base == nil then return false end
    if window_state() == 0 then
        save_template(blk)
        apply_look(m)
        record_look(blk, m)
        -- close speed index 0 = 0 frames: the window closes in one or two frames instead of animating
        WriteShort(template_addr() + TPL_CLOSE_SPEED_INDEX, 0)
        -- the bridge returns (called ok, syscall result); the syscall returns 2 when it acted, 4 when refused
        local ok, result = kh1_native.call_evdl_syscall(fnc_000_open_window, {NOTIFY_WINDOW})
        if not ok or result ~= 2 or window_state() == 0 then
            return false
        end
        WriteInt(blk + HDR_OUR_WINDOW, 1, true)
    end
    local index = ReadInt(blk + HDR_NEXT_BUF, true)
    write_buffer(blk, index, m.text, m.duration)
    write_record(blk + OFF_MIRROR + index * HELD_STRIDE, m)
    local position = message_count()
    WriteInt(blk + OFF_SOUNDS + index * 4, m.sound or -1, true)
    if position >= 0 and position < MSG_QUEUE_ENTRIES then
        WriteByte(blk + OFF_POS_BUF + position, index, true)
    end
    kh1_native.call_evdl_syscall(fnc_001_display_message, {NOTIFY_WINDOW, base + index})
    if message_count() ~= position + 1 then
        -- the game ignored it (cutscene state): keep the message, the caller holds it
        return false
    end
    WriteInt(blk + HDR_NEXT_BUF, (index + 1) % SLOT_COUNT, true)
    return true
end

-- ######################## --
-- # Held messages        # --
-- ######################## --

local function push_held(blk, m)
    local head = ReadInt(blk + HDR_HELD_HEAD, true)
    local count = ReadInt(blk + HDR_HELD_COUNT, true)
    if count >= HELD_COUNT then
        head = (head + 1) % HELD_COUNT
        count = HELD_COUNT - 1
        WriteInt(blk + HDR_HELD_HEAD, head, true)
    end
    write_record(held_addr(blk, head + count), m)
    WriteInt(blk + HDR_HELD_COUNT, count + 1, true)
end

local function peek_held(blk)
    local count = ReadInt(blk + HDR_HELD_COUNT, true)
    if count == 0 then return nil end
    return read_record(held_addr(blk, ReadInt(blk + HDR_HELD_HEAD, true)))
end

local function drop_held(blk)
    local head = ReadInt(blk + HDR_HELD_HEAD, true)
    local count = ReadInt(blk + HDR_HELD_COUNT, true)
    if count == 0 then return end
    WriteInt(blk + HDR_HELD_HEAD, (head + 1) % HELD_COUNT, true)
    WriteInt(blk + HDR_HELD_COUNT, count - 1, true)
end

-- ######################## --
-- # Public API           # --
-- ######################## --

-- opts: text (required), duration (seconds), sound, style, x, y, width, height, tail.
-- The look is applied only when the window has to be opened; queued messages share it.
local function queue_text_box(opts)
    if type(opts) ~= "table" or type(opts.text) ~= "string" then return false end
    local blk = block()
    if not blk then return false end
    if ReadInt(blk + HDR_HELD_COUNT, true) == 0 and can_show(blk, opts) and show(blk, opts) then
        return true
    end
    push_held(blk, opts)
    return true
end

local function open_text_box(text, window_id, duration_seconds, style, x, y, width, height, tail_type)
    return queue_text_box{
        text = text, duration = duration_seconds, style = style,
        x = x, y = y, width = width, height = height, tail = tail_type,
    }
end

local function close_text_box(window_id)
    return kh1_native.call_evdl_syscall(fnc_002_close_window, {window_id or NOTIFY_WINDOW})
end

local function pending_count()
    local blk = block()
    if not blk then return 0 end
    return ReadInt(blk + HDR_HELD_COUNT, true) + in_flight(blk)
end

-- Put the messages that were still ahead on the wiped window back at the front of the held ring,
-- oldest first, so they show again on the next window. The one on screen is included.
local function requeue_in_flight(blk)
    local first = ReadInt(blk + HDR_GOOD_POS, true)
    local last = ReadInt(blk + HDR_GOOD_COUNT, true) - 1
    WriteInt(blk + HDR_GOOD_COUNT, 0, true)
    if first < 0 then first = 0 end
    if last < first or last >= MSG_QUEUE_ENTRIES then return 0 end
    local records = {}
    for p = first, last do
        records[#records + 1] = read_record(blk + OFF_MIRROR + ReadByte(blk + OFF_POS_BUF + p, true) * HELD_STRIDE)
    end
    local held = {}
    while ReadInt(blk + HDR_HELD_COUNT, true) > 0 do
        held[#held + 1] = peek_held(blk)
        drop_held(blk)
    end
    for _, m in ipairs(records) do push_held(blk, m) end
    for _, m in ipairs(held) do push_held(blk, m) end
    return #records
end

-- An EV script load wipes the window bookkeeping (state, queue, count, slot activity) but not
-- the drawn instance. Left alone, the instance wakes as a window nothing owns, showing message 0
-- of the new script. The instance state field survives the wipe, so the driver sees the mismatch
-- the same frame and closes the window through the normal game path. The opposite case,
-- bookkeeping alive but the instance gone (the game kills window jobs when its cutscene check
-- fires), is repaired by resetting the bookkeeping the way the close callback would.
local function job_pool_report()
    if g_EVWindowJobPool == nil then return "" end
    local parts = {}
    for j = 0, WINDOW_SLOTS - 1 do
        local inst = ReadLong(g_EVWindowJobPool + j * 0x38 + 0x28)
        local idx = "none"
        if inst ~= 0 then
            local rel = inst - (kh1_native.get_module_base() + g_EVWindowInstances)
            idx = (rel >= 0 and rel % INSTANCE_SIZE == 0) and ("inst" .. (rel / INSTANCE_SIZE)) or string.format("%X", inst)
        end
        parts[#parts + 1] = string.format("job%d=%s", j, idx)
    end
    return " [" .. table.concat(parts, " ") .. "]"
end

-- Finish a close the game will not finish for us: no completion callback, state 5 so a live job
-- frees itself silently on its next tick, and the slot bookkeeping reset the way the close
-- callback would have done it. A dead job leaves the instance inert, which draws nothing.
local function force_close(slot)
    if slot then
        local inst = g_EVWindowInstances + slot * INSTANCE_SIZE
        WriteLong(inst + INST_CALLBACK, 0)
        WriteInt(inst + INST_STATE, INST_STATE_FINISH)
        WriteByte(g_EVWindowActivity + slot, 0)
    end
    WriteByte(g_EVWindowState + NOTIFY_WINDOW, 0)
    WriteInt(g_EVWindowMsgCount + NOTIFY_WINDOW * 4, 0)
end

-- Ask the game to close the window. If a close is already pending and has not progressed, the
-- window job is gone: finish it ourselves.
local function close_now(slot, inst_state)
    if slot and (inst_state == 3 or inst_state == 4 or inst_state == 5) then
        ConsolePrint("kh1_lua_library: close did not progress (instance " .. inst_state .. "), finishing it" .. job_pool_report())
        force_close(slot)
        return
    end
    if slot then
        WriteByte(g_EVWindowActivity + slot, 2)
        WriteShort(g_EVWindowInstances + slot * INSTANCE_SIZE + INST_CLOSE_SPEED, 0)
    end
    WriteByte(g_EVWindowState + NOTIFY_WINDOW, 2)
    close_text_box(NOTIFY_WINDOW)
end

local function repair_window(blk)
    local ours = ReadInt(blk + HDR_OUR_WINDOW, true) == 1
    local slot = instance_slot()
    local book = window_state()
    local inst = slot and instance_state(slot) or 0
    local count = message_count()

    if book == 0 then
        WriteInt(blk + HDR_GHOST_SINCE, 0, true)
        if inst ~= 0 then
            local requeued = ours and requeue_in_flight(blk) or 0
            ConsolePrint(string.format("kh1_lua_library: window %d bookkeeping wiped under a live box (instance %d, script table %d, ours %s), closing it, %d requeued",
                NOTIFY_WINDOW, inst, ReadInt(g_EVScriptLoaded), tostring(ours), requeued))
            close_now(slot, inst)
        end
        WriteInt(blk + HDR_OUR_WINDOW, 0, true)
        return
    end

    if book ~= 1 and inst == 0 then
        ConsolePrint(string.format("kh1_lua_library: window %d instance is gone but bookkeeping says state %d, resetting", NOTIFY_WINDOW, book))
        if slot then WriteByte(g_EVWindowActivity + slot, 0) end
        WriteByte(g_EVWindowState + NOTIFY_WINDOW, 0)
        WriteInt(g_EVWindowMsgCount + NOTIFY_WINDOW * 4, 0)
        WriteInt(blk + HDR_OUR_WINDOW, 0, true)
        WriteInt(blk + HDR_GHOST_SINCE, 0, true)
        return
    end

    -- Open, past the opening animation, with nothing queued: a woken ghost. A real window gets its
    -- message within a frame or two of opening, so a short grace period is enough.
    if count == 0 and book ~= 1 and inst ~= 1 then
        local since = ReadInt(blk + HDR_GHOST_SINCE, true)
        if since == 0 then
            WriteInt(blk + HDR_GHOST_SINCE, now_ms(), true)
        elseif now_ms() - since >= GHOST_GRACE_MS then
            local requeued = ours and requeue_in_flight(blk) or 0
            ConsolePrint(string.format("kh1_lua_library: window %d is open with no messages (state %d, instance %d, position %d, ours %s), closing it, %d requeued",
                NOTIFY_WINDOW, book, inst, message_position(), tostring(ours), requeued))
            close_now(slot, inst)
            WriteInt(blk + HDR_OUR_WINDOW, 0, true)
            WriteInt(blk + HDR_GHOST_SINCE, 0, true)
        end
        return
    end
    WriteInt(blk + HDR_GHOST_SINCE, 0, true)
end

local function log_script_loads(blk)
    local table_count = ReadInt(g_EVScriptLoaded)
    local last = ReadInt(blk + HDR_LAST_TABLE, true)
    if table_count ~= last then
        if ReadInt(blk + HDR_OUR_WINDOW, true) == 1 then
            ConsolePrint(string.format("kh1_lua_library: EV string table count %d -> %d while our window is open (state %d, count %d)",
                last, table_count, window_state(), message_count()))
        end
        WriteInt(blk + HDR_LAST_TABLE, table_count, true)
    end
end

local function play_message_sound(blk)
    local slot = instance_slot()
    local state = window_state()
    local ptr = 0
    if slot and state == 2 then
        ptr = ReadLong(g_EVWindowInstances + slot * INSTANCE_SIZE + INST_TEXT_PTR)
    end
    local last = ReadLong(blk + HDR_LAST_TEXT, true)
    if ptr ~= last then
        WriteLong(blk + HDR_LAST_TEXT, ptr, true)
        local index = showing_index(blk)
        if index ~= nil then
            local shown_at = ReadInt(blk + HDR_SHOWN_AT_MS, true)
            ConsolePrint(string.format("kh1_lua_library: window %d showing a message (%d ms after the previous, %d queued)",
                NOTIFY_WINDOW, shown_at > 0 and (now_ms() - shown_at) or 0, message_count()))
            WriteInt(blk + HDR_SHOWN_AT_MS, now_ms(), true)
            local sound = ReadInt(blk + OFF_SOUNDS + index * 4, true)
            if sound >= 0 and fnc_play_se2 ~= nil then
                kh1_native.call_function(fnc_play_se2, sound, 0)
            end
        end
    end
    WriteInt(blk + HDR_LAST_POS, message_position(), true)
    WriteInt(blk + HDR_LAST_STATE, state, true)
    local count = message_count()
    WriteInt(blk + HDR_LAST_COUNT, count, true)
    if count > 0 and state == 2 then
        local index = showing_index(blk)
        if index ~= nil then
            for pos = count - 1, 0, -1 do
                if ReadByte(blk + OFF_POS_BUF + pos, true) == index then
                    WriteInt(blk + HDR_GOOD_POS, pos, true)
                    WriteInt(blk + HDR_GOOD_COUNT, count, true)
                    break
                end
            end
        end
    end
end

local function feed_held(blk)
    while ReadInt(blk + HDR_HELD_COUNT, true) > 0 do
        local m = peek_held(blk)
        if not m or not can_show(blk, m) or not show(blk, m) then break end
        drop_held(blk)
    end
end

local function update_text_boxes()
    local blk = block()
    if not blk then return end
    log_script_loads(blk)
    repair_window(blk)
    feed_held(blk)
    if window_state() == 0 and ReadInt(blk + HDR_HELD_COUNT, true) == 0 then
        restore_template(blk)
    end
    play_message_sound(blk)
end

return {
    queue_text_box = queue_text_box,
    open_text_box = open_text_box,
    close_text_box = close_text_box,
    update_text_boxes = update_text_boxes,
    is_text_box_busy = is_text_box_busy,
    pending_count = pending_count,
}
