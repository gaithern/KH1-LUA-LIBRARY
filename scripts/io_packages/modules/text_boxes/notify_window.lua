---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")

local NOTIFY_WINDOW = 6

local WINDOW_CLOSED  = 0
local WINDOW_OPENING = 1
local WINDOW_SHOWING = 2
local WINDOW_IDLE    = 4

local SCREEN_BOX_COUNT = 4
local SCREEN_BOX_SIZE  = 0xD0

local SCREEN_BOX_FREE    = 0
local SCREEN_BOX_SHOWING = 2
local SCREEN_BOX_CLOSING = 3

local BOX_IDLE            = 0
local BOX_OPENING         = 1
local BOX_CLOSE_REQUESTED = 3
local BOX_CLOSING         = 4
local BOX_FINISHED        = 5

local BOX_CLOSE_FRAMES  = 0x12
local BOX_DONE_CALLBACK = 0x18
local BOX_STATE         = 0x20
local BOX_TEXT          = 0x88

local TEMPLATE_SIZE              = 0xD0
local TEMPLATE_CLOSE_SPEED_INDEX = 0xE

local MESSAGE_QUEUE_STRIDE = 0x200
local MESSAGE_QUEUE_LENGTH = 0x100

local OPEN_WINDOW_SUCCESS = 2
local SUMMON_ACTIVE_FLAG  = 0x2000
local SYSCALLS_BLOCKED_FLAG = 1
local EVENT_MODE_FLAG       = 2

local function is_bit_set(value, bit_value)
    return math.floor(value / bit_value) % 2 == 1
end

local function get_window_state()
    return ReadByte(g_EVWindowState + NOTIFY_WINDOW)
end

local function set_window_state(state)
    WriteByte(g_EVWindowState + NOTIFY_WINDOW, state)
end

local function is_window_busy()
    return get_window_state() ~= WINDOW_CLOSED
end

local function get_message_count()
    return ReadInt(g_EVWindowMsgCount + NOTIFY_WINDOW * 4)
end

local function set_message_count(count)
    WriteInt(g_EVWindowMsgCount + NOTIFY_WINDOW * 4, count)
end

local function get_message_position()
    return ReadInt(g_EVWindowMsgPosition)
end

local function get_last_message_id()
    local count = get_message_count()
    if count < 1 or count > MESSAGE_QUEUE_LENGTH then
        return nil
    end
    local queue_address = g_EVWindowMsgQueue + NOTIFY_WINDOW * MESSAGE_QUEUE_STRIDE
    return ReadShort(queue_address + (count - 1) * 2)
end

local function find_screen_box()
    for box = 0, SCREEN_BOX_COUNT - 1 do
        if ReadByte(g_EVWindowStateArray + box) == NOTIFY_WINDOW then
            return box
        end
    end
    return nil
end

local function get_screen_box_address(box)
    return g_EVScreenBoxes + box * SCREEN_BOX_SIZE
end

local function get_screen_box_state(box)
    if box == nil then
        return BOX_IDLE
    end
    return ReadInt(get_screen_box_address(box) + BOX_STATE)
end

local function get_screen_box_text(box)
    return ReadLong(get_screen_box_address(box) + BOX_TEXT)
end

local function set_screen_box_status(box, status)
    WriteByte(g_EVWindowActivity + box, status)
end

local function is_screen_box_closing()
    local box = find_screen_box()
    if box == nil then
        return false
    end
    return ReadByte(g_EVWindowActivity + box) == SCREEN_BOX_CLOSING
end

local function window_syscalls_enabled()
    local syscalls_blocked = is_bit_set(ReadByte(inCutscene), SYSCALLS_BLOCKED_FLAG)
    return not syscalls_blocked and ReadInt(g_EVCutsceneCheck) == 0
end

local function is_event_active()
    return is_bit_set(ReadByte(inCutscene), EVENT_MODE_FLAG)
end

local function is_summon_active()
    return is_bit_set(ReadInt(stateFlag), SUMMON_ACTIVE_FLAG)
end

local function get_template_address()
    return g_EVWindowTemplates + NOTIFY_WINDOW * TEMPLATE_SIZE
end

local function make_closing_instant()
    WriteShort(get_template_address() + TEMPLATE_CLOSE_SPEED_INDEX, 0)
end

local function apply_look(message, string_id)
    if message.style ~= nil then
        kh1_native.call_evdl_syscall(fnc_005_set_window_type, {NOTIFY_WINDOW, message.style})
    end
    if message.tail ~= nil then
        kh1_native.call_evdl_syscall(fnc_050_set_window_tail_type, {NOTIFY_WINDOW, message.tail})
    end
    if message.width ~= nil then
        kh1_native.call_evdl_syscall(fnc_004_set_window_size, {NOTIFY_WINDOW, message.width, message.height})
    end
    local can_auto_width = fnc_16D_set_window_width_auto ~= nil
    if message.auto_width and can_auto_width then
        local x = message.x or 0
        local y = message.y or 0
        kh1_native.call_evdl_syscall(fnc_16D_set_window_width_auto, {NOTIFY_WINDOW, string_id, x, y})
    elseif message.x ~= nil then
        kh1_native.call_evdl_syscall(fnc_003_set_window_position, {NOTIFY_WINDOW, message.x, message.y})
    end
end

local function open_without_width_pad()
    local saved_pad = nil
    if g_EVWindowWidthPad ~= nil then
        saved_pad = ReadFloat(g_EVWindowWidthPad)
        WriteFloat(g_EVWindowWidthPad, 0)
    end
    local ok, result = kh1_native.call_evdl_syscall(fnc_000_open_window, {NOTIFY_WINDOW})
    if saved_pad ~= nil then
        WriteFloat(g_EVWindowWidthPad, saved_pad)
    end
    if not ok or result ~= OPEN_WINDOW_SUCCESS then
        return false
    end
    return is_window_busy()
end

local function display(string_id)
    kh1_native.call_evdl_syscall(fnc_001_display_message, {NOTIFY_WINDOW, string_id})
end

local function close()
    return kh1_native.call_evdl_syscall(fnc_002_close_window, {NOTIFY_WINDOW})
end

local function reset_bookkeeping(box)
    if box ~= nil then
        set_screen_box_status(box, SCREEN_BOX_FREE)
    end
    set_window_state(WINDOW_CLOSED)
    set_message_count(0)
end

local function finish_screen_box(box)
    local box_address = get_screen_box_address(box)
    WriteLong(box_address + BOX_DONE_CALLBACK, 0)
    WriteInt(box_address + BOX_STATE, BOX_FINISHED)
    reset_bookkeeping(box)
end

local function close_immediately(box, box_state)
    if box ~= nil then
        local already_closing = box_state == BOX_CLOSE_REQUESTED or box_state == BOX_CLOSING or box_state == BOX_FINISHED
        if already_closing then
            ConsolePrint("kh1_lua_library: close did not progress (screen box state " .. box_state .. "), finishing it")
            finish_screen_box(box)
            return
        end
        set_screen_box_status(box, SCREEN_BOX_SHOWING)
        WriteShort(get_screen_box_address(box) + BOX_CLOSE_FRAMES, 0)
    end
    set_window_state(WINDOW_SHOWING)
    close()
end

return {
    NOTIFY_WINDOW = NOTIFY_WINDOW,
    MESSAGE_QUEUE_LENGTH = MESSAGE_QUEUE_LENGTH,
    TEMPLATE_SIZE = TEMPLATE_SIZE,

    WINDOW_CLOSED = WINDOW_CLOSED,
    WINDOW_OPENING = WINDOW_OPENING,
    WINDOW_SHOWING = WINDOW_SHOWING,
    WINDOW_IDLE = WINDOW_IDLE,

    BOX_IDLE = BOX_IDLE,
    BOX_OPENING = BOX_OPENING,

    get_window_state = get_window_state,
    is_window_busy = is_window_busy,
    get_message_count = get_message_count,
    get_message_position = get_message_position,
    get_last_message_id = get_last_message_id,

    find_screen_box = find_screen_box,
    get_screen_box_state = get_screen_box_state,
    get_screen_box_text = get_screen_box_text,
    is_screen_box_closing = is_screen_box_closing,

    window_syscalls_enabled = window_syscalls_enabled,
    is_event_active = is_event_active,
    is_summon_active = is_summon_active,

    get_template_address = get_template_address,
    make_closing_instant = make_closing_instant,
    apply_look = apply_look,

    open_without_width_pad = open_without_width_pad,
    display = display,
    close = close,
    close_immediately = close_immediately,
    reset_bookkeeping = reset_bookkeeping,
}
