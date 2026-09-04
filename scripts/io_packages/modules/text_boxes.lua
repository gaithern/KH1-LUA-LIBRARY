---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")
local khscii = require("helpers.khscii")

local GetKHSCII = khscii.GetKHSCII

local STATE_KEY           = "kh1_textbox_state_v1"
local STATE_SIZE          = 0x220
local OFF_LEGACY_FLAG     = 0x000
local OFF_SLOT_COUNT      = 0x00C
local OFF_SLOT_VALID      = 0x018
local OFF_SLOT            = 0x01C
local OFF_BUF             = 0x020
local BUF_MAX             = 511
local EV_STRING_TABLE_MAX = 1536

-- Per-window box records, shared by every Lua state and kept across hot reloads, so the
-- library's driver script can close boxes that any script opened.
local BOXES_KEY          = "kh1_textbox_boxes_v1"
local BOXES_SIZE         = 0x100
local WINDOW_COUNT       = 8
local BOX_STRIDE         = 0x10
local BOX_VALID          = 0x0
local BOX_DEADLINE_MS    = 0x4
local BOX_WORLD          = 0x8
local BOX_ROOM           = 0xC
local OFF_LAST_WINDOW    = 0x80
local OFF_GHOST_SINCE_MS = 0x84
local OFF_BOXES_MAGIC    = 0x88
local BOXES_MAGIC        = 0x58425431

local GHOST_TIMEOUT_MS = 2000

local function state_block()
    local blk = kh1_native.persistent_block(STATE_KEY, STATE_SIZE)
    if blk == 0 or blk == nil then return nil end
    return blk
end

local function boxes_block()
    local blk = kh1_native.persistent_block(BOXES_KEY, BOXES_SIZE)
    if blk == 0 or blk == nil then return nil end
    if ReadInt(blk + OFF_BOXES_MAGIC, true) ~= BOXES_MAGIC then
        for w = 0, WINDOW_COUNT - 1 do
            WriteInt(blk + w * BOX_STRIDE + BOX_VALID, 0, true)
        end
        WriteInt(blk + OFF_LAST_WINDOW, -1, true)
        WriteInt(blk + OFF_GHOST_SINCE_MS, 0, true)
        WriteInt(blk + OFF_BOXES_MAGIC, BOXES_MAGIC, true)
    end
    return blk
end

local function now_ms()
    return math.floor(os.clock() * 1000)
end

local function get_world() return ReadByte(world) end
local function get_room() return ReadByte(room) end

-- ######################## --
-- # Custom message slot  # --
-- ######################## --

local function ev_string_slot(blk)
    local count = ReadInt(g_EVScriptLoaded)
    if ReadInt(blk + OFF_SLOT_VALID, true) == 1 then
        local slot = ReadInt(blk + OFF_SLOT, true)
        if count == ReadInt(blk + OFF_SLOT_COUNT, true) and slot == count - 1 then
            return slot
        end
    end
    if count >= EV_STRING_TABLE_MAX then return nil end
    WriteInt(g_EVScriptLoaded, count + 1)
    WriteInt(blk + OFF_SLOT, count, true)
    WriteInt(blk + OFF_SLOT_COUNT, count + 1, true)
    WriteInt(blk + OFF_SLOT_VALID, 1, true)
    return count
end

local function set_textbox_text(text)
    local blk = state_block()
    if not blk then return nil end
    local slot = ev_string_slot(blk)
    if not slot then return nil end
    local khs = GetKHSCII(text)
    local n = math.min(#khs, BUF_MAX)
    local bytes = {}
    for i = 1, n do bytes[i] = khs[i] end
    bytes[n + 1] = 0
    WriteArray(blk + OFF_BUF, bytes, true)
    WriteLong(g_pEVStringDataPtr + slot * 8, blk + OFF_BUF)
    WriteByte(blk + OFF_LEGACY_FLAG, 0, true)
    return slot
end

-- ######################## --
-- # Window helpers       # --
-- ######################## --

local function set_text_box_style(window_id, style)
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_005_set_window_type, {window_id, style})
end

local function set_text_box_position(window_id, x, y)
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_003_set_window_position, {window_id, x, y})
end

local function set_text_box_size(window_id, width, height)
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_004_set_window_size, {window_id, width, height})
end

local function set_text_box_tail_type(window_id, tail_type)
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_050_set_window_tail_type, {window_id, tail_type})
end

-- Per-window state byte kept by the EV window system: 0 = closed, 1 = opening/open,
-- 2 = displaying a message, 4 = open and idle. Non-zero also covers the close animation,
-- during which fnc_0B1_open_window_no_close refuses to open the window again.
local function is_text_box_busy(window_id)
    window_id = window_id or 1
    return ReadByte(g_EVWindowState + window_id) ~= 0
end

local function record_box(window_id, duration_seconds)
    local blk = boxes_block()
    if not blk then return end
    local rec = blk + window_id * BOX_STRIDE
    local deadline = 0
    if duration_seconds and duration_seconds > 0 then
        deadline = now_ms() + math.floor(duration_seconds * 1000)
    end
    WriteInt(rec + BOX_DEADLINE_MS, deadline, true)
    WriteInt(rec + BOX_WORLD, get_world(), true)
    WriteInt(rec + BOX_ROOM, get_room(), true)
    WriteInt(rec + BOX_VALID, 1, true)
    WriteInt(blk + OFF_LAST_WINDOW, window_id, true)
end

local function forget_box(window_id)
    local blk = boxes_block()
    if not blk then return end
    WriteInt(blk + window_id * BOX_STRIDE + BOX_VALID, 0, true)
end

-- Opens a text box. When the window is already open the new message replaces the current one
-- in place (the native multi-message path), so it never has to wait for a close animation.
-- The box is closed by update_text_boxes, which the library's driver script runs every frame.
local function open_text_box(text, window_id, duration_seconds, style, x, y, width, height, tail_type)
    window_id = window_id or 1
    local already_open = is_text_box_busy(window_id)
    if not already_open then
        if style ~= nil then
            set_text_box_style(window_id, style)
        end
        if tail_type ~= nil then
            set_text_box_tail_type(window_id, tail_type)
        end
        if x ~= nil and y ~= nil then
            set_text_box_position(window_id, x, y)
        end
        if width ~= nil and height ~= nil then
            set_text_box_size(window_id, width, height)
        end
    end
    local message_id = set_textbox_text(text)
    if message_id == nil then return false end
    local opened = true
    if not already_open then
        opened = kh1_native.call_evdl_syscall(fnc_0B1_open_window_no_close, {window_id})
    end
    local displayed = kh1_native.call_evdl_syscall(fnc_001_display_message, {window_id, message_id})
    record_box(window_id, duration_seconds)
    return opened and displayed, message_id
end

local function close_text_box(window_id)
    window_id = window_id or 1
    forget_box(window_id)
    return kh1_native.call_evdl_syscall(fnc_002_close_window, {window_id})
end

-- An EV script load wipes the window bookkeeping but not the drawn window. A box left open at
-- that moment later wakes as an open window with an empty message queue that nothing owns.
local function close_ghost_window(blk)
    local window_id = ReadInt(blk + OFF_LAST_WINDOW, true)
    if window_id < 0 or window_id >= WINDOW_COUNT then return end
    local open = ReadByte(g_EVWindowState + window_id) ~= 0
    local empty = ReadInt(g_EVWindowMsgCount + window_id * 4) == 0
    if not (open and empty) then
        WriteInt(blk + OFF_GHOST_SINCE_MS, 0, true)
        return
    end
    local since = ReadInt(blk + OFF_GHOST_SINCE_MS, true)
    if since == 0 then
        WriteInt(blk + OFF_GHOST_SINCE_MS, now_ms(), true)
    elseif now_ms() - since >= GHOST_TIMEOUT_MS then
        close_text_box(window_id)
        WriteInt(blk + OFF_GHOST_SINCE_MS, 0, true)
    end
end

local function update_text_boxes()
    local blk = boxes_block()
    if not blk then return end
    local now = now_ms()
    local current_world = get_world()
    local current_room = get_room()
    for window_id = 0, WINDOW_COUNT - 1 do
        local rec = blk + window_id * BOX_STRIDE
        if ReadInt(rec + BOX_VALID, true) == 1 then
            local deadline = ReadInt(rec + BOX_DEADLINE_MS, true)
            local timed_out = deadline > 0 and now >= deadline
            local transitioned = current_world ~= ReadInt(rec + BOX_WORLD, true)
                or current_room ~= ReadInt(rec + BOX_ROOM, true)
            if timed_out or transitioned then
                close_text_box(window_id)
            end
        end
    end
    close_ghost_window(blk)
end

return {
    set_text_box_style = set_text_box_style,
    set_text_box_position = set_text_box_position,
    set_text_box_size = set_text_box_size,
    set_text_box_tail_type = set_text_box_tail_type,
    is_text_box_busy = is_text_box_busy,
    open_text_box = open_text_box,
    close_text_box = close_text_box,
    update_text_boxes = update_text_boxes,
}
