---@diagnostic disable: undefined-global

-- Notification text boxes on one shared window, queued by the game itself.
--
-- The EV window system keeps a per-window message queue and plays messages back to back. A
-- control byte 06 lo hi inside a message holds it on screen for lo|hi<<8 ticks (30 per second)
-- after it has been fully shown, and a window opened with Open_window closes itself once its
-- queue is empty. So queue_text_box only appends: each message gets its own EV string slot and
-- buffer, and the game handles ordering, timing and closing. The library's driver script runs
-- update_text_boxes every frame to feed held messages, play per-message sounds and clean up a
-- ghost window left behind by an EV script load. All state lives in a persistent block, so
-- every Lua state shares one queue and nothing is lost on a hot reload.

local kh1_native = require("kh1_native")
local khscii = require("helpers.khscii")

local GetKHSCII = khscii.GetKHSCII

local NOTIFY_WINDOW    = 6
local SLOT_COUNT       = 32   -- EV string slots reserved at the top of the table = messages in flight
local BUF_SIZE         = 512
local HELD_COUNT       = 32   -- messages waiting while the window is busy or full
local TICKS_PER_SECOND = 30
local DEFAULT_DURATION = 2.5
local GHOST_TIMEOUT_MS = 2000
local EV_STRING_TABLE_MAX = 1536
local WINDOW_SLOTS     = 4
local MSG_QUEUE_STRIDE = 0x200
local MSG_QUEUE_ENTRIES = 0x100

local BLOCK_KEY  = "kh1_textbox_queue_v2"
local MAGIC      = 0x54425132

local HDR_MAGIC       = 0x00
local HDR_SLOT_VALID  = 0x04
local HDR_SLOT_BASE   = 0x08
local HDR_SLOT_TABLE  = 0x0C  -- g_EVScriptLoaded value after our reservation
local HDR_NEXT_BUF    = 0x10
local HDR_HELD_HEAD   = 0x14
local HDR_HELD_COUNT  = 0x18
local HDR_LAST_POS    = 0x1C
local HDR_LAST_STATE  = 0x20
local HDR_GHOST_SINCE = 0x24
local OFF_SOUNDS      = 0x40                                -- int32[MSG_QUEUE_ENTRIES], by queue position
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
local BLOCK_SIZE      = OFF_HELD + HELD_COUNT * HELD_STRIDE

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
        WriteInt(blk + HDR_SLOT_VALID, 0, true)
        WriteInt(blk + HDR_NEXT_BUF, 0, true)
        WriteInt(blk + HDR_HELD_HEAD, 0, true)
        WriteInt(blk + HDR_HELD_COUNT, 0, true)
        WriteInt(blk + HDR_LAST_POS, -1, true)
        WriteInt(blk + HDR_LAST_STATE, 0, true)
        WriteInt(blk + HDR_GHOST_SINCE, 0, true)
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

local function is_text_box_busy(window_id)
    return ReadByte(g_EVWindowState + (window_id or NOTIFY_WINDOW)) ~= 0
end

-- ######################## --
-- # EV string slots      # --
-- ######################## --

local function ensure_slots(blk)
    local count = ReadInt(g_EVScriptLoaded)
    if ReadInt(blk + HDR_SLOT_VALID, true) == 1 then
        local base = ReadInt(blk + HDR_SLOT_BASE, true)
        if count == ReadInt(blk + HDR_SLOT_TABLE, true) and base == count - SLOT_COUNT then
            return base
        end
    end
    if count + SLOT_COUNT > EV_STRING_TABLE_MAX then return nil end
    for i = 0, SLOT_COUNT - 1 do
        WriteLong(g_pEVStringDataPtr + (count + i) * 8, blk + OFF_BUFFERS + i * BUF_SIZE)
    end
    WriteInt(g_EVScriptLoaded, count + SLOT_COUNT)
    WriteInt(blk + HDR_SLOT_BASE, count, true)
    WriteInt(blk + HDR_SLOT_TABLE, count + SLOT_COUNT, true)
    WriteInt(blk + HDR_SLOT_VALID, 1, true)
    WriteInt(blk + HDR_NEXT_BUF, 0, true)
    return count
end

local function owns_window(blk)
    if window_state() == 0 then return false end
    local id = last_message_id()
    if id == nil then return false end
    local base = ReadInt(blk + HDR_SLOT_BASE, true)
    return ReadInt(blk + HDR_SLOT_VALID, true) == 1 and id >= base and id < base + SLOT_COUNT
end

-- Messages queued on the window that have not finished showing. The position global is only
-- meaningful while the window is displaying or idle; while it is closed or still opening,
-- everything queued is still ahead.
local function in_flight()
    local count = message_count()
    local state = window_state()
    if state ~= 2 and state ~= 4 then return count end
    local pos = message_position()
    if pos < 0 or pos > count then return count end
    return count - pos
end

-- ######################## --
-- # Showing a message    # --
-- ######################## --

local function dwell_escape(duration_seconds)
    local ticks = math.floor((duration_seconds or DEFAULT_DURATION) * TICKS_PER_SECOND)
    if ticks < 1 then ticks = 1 end
    if ticks > 0xFFFF then ticks = 0xFFFF end
    return string.format("{0x06}{0x%02X}{0x%02X}", ticks % 256, math.floor(ticks / 256))
end

local function write_buffer(blk, index, text, duration_seconds)
    local khs = GetKHSCII(text .. dwell_escape(duration_seconds))
    local n = math.min(#khs, BUF_SIZE - 1)
    local bytes = {}
    for i = 1, n do bytes[i] = khs[i] end
    bytes[n + 1] = 0
    WriteArray(blk + OFF_BUFFERS + index * BUF_SIZE, bytes, true)
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

-- The window is free for us when it is closed, or open with our own message on it and not
-- already closing. In-flight messages must not exceed our buffer ring.
local function can_show(blk)
    if in_flight() >= SLOT_COUNT then return false end
    if window_state() == 0 then return true end
    return owns_window(blk) and not window_closing()
end

local function show(blk, m)
    local base = ensure_slots(blk)
    if base == nil then return false end
    if window_state() == 0 then
        apply_look(m)
        if not kh1_native.call_evdl_syscall(fnc_000_open_window, {NOTIFY_WINDOW}) then
            return false
        end
    end
    local index = ReadInt(blk + HDR_NEXT_BUF, true)
    write_buffer(blk, index, m.text, m.duration)
    local position = message_count()
    if position >= 0 and position < MSG_QUEUE_ENTRIES then
        WriteInt(blk + OFF_SOUNDS + position * 4, m.sound or -1, true)
    end
    kh1_native.call_evdl_syscall(fnc_001_display_message, {NOTIFY_WINDOW, base + index})
    WriteInt(blk + HDR_NEXT_BUF, (index + 1) % SLOT_COUNT, true)
    return true
end

-- ######################## --
-- # Held messages        # --
-- ######################## --

local function held_addr(blk, index)
    return blk + OFF_HELD + (index % HELD_COUNT) * HELD_STRIDE
end

local function push_held(blk, m)
    local head = ReadInt(blk + HDR_HELD_HEAD, true)
    local count = ReadInt(blk + HDR_HELD_COUNT, true)
    if count >= HELD_COUNT then
        head = (head + 1) % HELD_COUNT
        count = HELD_COUNT - 1
        WriteInt(blk + HDR_HELD_HEAD, head, true)
    end
    local rec = held_addr(blk, head + count)
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
    WriteInt(blk + HDR_HELD_COUNT, count + 1, true)
end

local function peek_held(blk)
    local head = ReadInt(blk + HDR_HELD_HEAD, true)
    local count = ReadInt(blk + HDR_HELD_COUNT, true)
    if count == 0 then return nil end
    local rec = held_addr(blk, head)
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
    if ReadInt(blk + HDR_HELD_COUNT, true) == 0 and can_show(blk) and show(blk, opts) then
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
    return ReadInt(blk + HDR_HELD_COUNT, true) + in_flight()
end

-- An EV script load wipes the window bookkeeping but not the drawn window. A box left open at
-- that moment later wakes as an open window with an empty message queue that nothing owns.
local function close_ghost_window(blk)
    if window_state() ~= 0 and message_count() == 0 then
        local since = ReadInt(blk + HDR_GHOST_SINCE, true)
        if since == 0 then
            WriteInt(blk + HDR_GHOST_SINCE, now_ms(), true)
        elseif now_ms() - since >= GHOST_TIMEOUT_MS then
            close_text_box(NOTIFY_WINDOW)
            WriteInt(blk + HDR_GHOST_SINCE, 0, true)
        end
    else
        WriteInt(blk + HDR_GHOST_SINCE, 0, true)
    end
end

local function play_message_sound(blk)
    local state = window_state()
    local pos = message_position()
    local last_pos = ReadInt(blk + HDR_LAST_POS, true)
    local last_state = ReadInt(blk + HDR_LAST_STATE, true)
    if state == 2 and (pos ~= last_pos or last_state ~= 2) and pos >= 0 and pos < message_count() then
        local sound = ReadInt(blk + OFF_SOUNDS + pos * 4, true)
        if sound >= 0 and fnc_play_se2 ~= nil then
            kh1_native.call_function(fnc_play_se2, sound, 0)
        end
    end
    WriteInt(blk + HDR_LAST_POS, pos, true)
    WriteInt(blk + HDR_LAST_STATE, state, true)
end

local function feed_held(blk)
    while ReadInt(blk + HDR_HELD_COUNT, true) > 0 and can_show(blk) do
        local m = peek_held(blk)
        if not m or not show(blk, m) then break end
        drop_held(blk)
    end
end

local function update_text_boxes()
    local blk = block()
    if not blk then return end
    close_ghost_window(blk)
    feed_held(blk)
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
