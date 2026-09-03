---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")
local khscii = require("helpers.khscii")

local GetKHSCII = khscii.GetKHSCII

local STATE_KEY           = "kh1_textbox_state_v1"
local STATE_SIZE          = 0x220
local OFF_LEGACY_FLAG     = 0x000
local OFF_PENDING_VALID   = 0x004
local OFF_PENDING_WID     = 0x008
local OFF_SLOT_COUNT      = 0x00C
local OFF_PENDING_RVA     = 0x010
local OFF_SLOT_VALID      = 0x018
local OFF_SLOT            = 0x01C
local OFF_BUF             = 0x020
local BUF_MAX             = 511
local EV_STRING_TABLE_MAX = 1536

local function state_block()
    local blk = kh1_native.persistent_block(STATE_KEY, STATE_SIZE)
    if blk == 0 or blk == nil then return nil end
    return blk
end

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
-- # Pending-box tracking # --
-- ######################## --

local function set_pending_text_box(window_id, close_rva)
    local blk = state_block()
    if not blk then return end
    WriteInt(blk + OFF_PENDING_WID, window_id, true)
    WriteLong(blk + OFF_PENDING_RVA, close_rva, true)
    WriteInt(blk + OFF_PENDING_VALID, 1, true)
end

local function clear_pending_text_box()
    local blk = state_block()
    if blk then WriteInt(blk + OFF_PENDING_VALID, 0, true) end
end

local function close_pending_text_box()
    local blk = state_block()
    if not blk or ReadInt(blk + OFF_PENDING_VALID, true) == 0 then return false end
    local wid = ReadInt(blk + OFF_PENDING_WID, true)
    local rva = ReadLong(blk + OFF_PENDING_RVA, true)
    WriteInt(blk + OFF_PENDING_VALID, 0, true)
    return (kh1_native.call_evdl_syscall(rva, {wid}))
end

-- ######################## --
-- # Public driver        # --
-- ######################## --

local open_text_boxes = {}

local function get_world() return ReadByte(world) end
local function get_room() return ReadByte(room) end

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

local function open_text_box(text, window_id, duration_seconds, style, x, y, width, height, tail_type)
    window_id = window_id or 1
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
    local message_id = set_textbox_text(text)
    if message_id == nil then return false end
    local opened = kh1_native.call_evdl_syscall(fnc_0B1_open_window_no_close, {window_id})
    local displayed = kh1_native.call_evdl_syscall(fnc_001_display_message, {window_id, message_id})
    set_pending_text_box(window_id, fnc_002_close_window)
    open_text_boxes[window_id] = {
        deadline = (duration_seconds and duration_seconds > 0) and (os.clock() + duration_seconds) or nil,
        open_world = get_world(),
        open_room = get_room(),
    }
    return opened and displayed
end

local function close_text_box(window_id)

    window_id = window_id or 1
    open_text_boxes[window_id] = nil
    clear_pending_text_box()
    return kh1_native.call_evdl_syscall(fnc_002_close_window, {window_id})
end

local function update_text_boxes()
    local now = os.clock()
    local current_world = get_world()
    local current_room = get_room()
    for window_id, box in pairs(open_text_boxes) do
        local timed_out = box.deadline and now >= box.deadline
        local transitioned = current_world ~= box.open_world or current_room ~= box.open_room
        if timed_out or transitioned then
            close_text_box(window_id)
        end
    end
end

close_pending_text_box()

return {
    set_text_box_style = set_text_box_style,
    set_text_box_position = set_text_box_position,
    set_text_box_size = set_text_box_size,
    set_text_box_tail_type = set_text_box_tail_type,
    open_text_box = open_text_box,
    close_text_box = close_text_box,
    update_text_boxes = update_text_boxes,
}
