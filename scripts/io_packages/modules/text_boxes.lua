---@diagnostic disable: undefined-global

local kh1_native = require("kh1_native")
local khscii = require("helpers.khscii")
local notify_window = require("modules.text_boxes.notify_window")
local store = require("modules.text_boxes.message_store")

local EV_STRING_TABLE_MAX   = 1536
local FIRST_STRING_ID       = EV_STRING_TABLE_MAX - store.BUFFER_COUNT
local TICKS_PER_SECOND      = 30
local MAX_DWELL_TICKS       = 0xFFFF
local DEFAULT_DURATION      = 2.5
local EMPTY_WINDOW_GRACE_MS = 500
local LOOK_FIELDS           = { "style", "x", "y", "width", "height", "tail" }

local function now_ms()
    return math.floor(os.clock() * 1000)
end

local function clean_message(options)
    local message = {}
    message.text = options.text
    message.duration = options.duration or DEFAULT_DURATION
    message.sound = options.sound
    message.style = options.style
    message.tail = options.tail
    if options.x ~= nil and options.y ~= nil then
        message.x = options.x
        message.y = options.y
    end
    if options.width ~= nil and options.height ~= nil then
        message.width = options.width
        message.height = options.height
    end
    if options.auto_width then
        message.auto_width = true
    end
    return message
end

local function reserve_string_slots()
    if ReadInt(g_EVScriptLoaded) > FIRST_STRING_ID then
        return false
    end
    local first_slot_address = g_pEVStringDataPtr + FIRST_STRING_ID * 8
    if ReadLong(first_slot_address) == store.get_buffer_address(0) then
        return true
    end
    for index = 0, store.BUFFER_COUNT - 1 do
        local slot_address = g_pEVStringDataPtr + (FIRST_STRING_ID + index) * 8
        WriteLong(slot_address, store.get_buffer_address(index))
    end
    local last_string_id = FIRST_STRING_ID + store.BUFFER_COUNT - 1
    ConsolePrint(string.format("kh1_lua_library: EV string slots %d..%d pointed at the notification buffers", FIRST_STRING_ID, last_string_id))
    return true
end

local function is_our_string_id(string_id)
    return string_id >= FIRST_STRING_ID and string_id < FIRST_STRING_ID + store.BUFFER_COUNT
end

local function owns_window()
    if not notify_window.is_window_busy() then
        return false
    end
    local last_message_id = notify_window.get_last_message_id()
    if last_message_id == nil then
        return false
    end
    return is_our_string_id(last_message_id)
end

local function get_shown_text_address()
    if notify_window.get_window_state() ~= notify_window.WINDOW_SHOWING then
        return 0
    end
    local box = notify_window.find_screen_box()
    if box == nil then
        return 0
    end
    return notify_window.get_screen_box_text(box)
end

local function find_showing_buffer()
    return store.find_buffer_index(get_shown_text_address())
end

local function find_queue_position_of_buffer(buffer_index, message_count)
    for position = message_count - 1, 0, -1 do
        if store.get_buffer_at_position(position) == buffer_index then
            return position
        end
    end
    return nil
end

local function count_in_flight()
    local message_count = notify_window.get_message_count()
    if notify_window.get_window_state() == notify_window.WINDOW_IDLE then
        return 0
    end
    local showing_buffer = find_showing_buffer()
    if showing_buffer == nil then
        return message_count
    end
    local position = find_queue_position_of_buffer(showing_buffer, message_count)
    if position == nil then
        return message_count
    end
    return message_count - position
end

local function make_dwell_code(duration_seconds)
    local ticks = math.floor(duration_seconds * TICKS_PER_SECOND)
    if ticks < 1 then
        ticks = 1
    end
    if ticks > MAX_DWELL_TICKS then
        ticks = MAX_DWELL_TICKS
    end
    local low_byte = ticks % 256
    local high_byte = math.floor(ticks / 256)
    return string.format("{0x05}{0x%02X}{0x%02X}", low_byte, high_byte)
end

local function write_buffer_text(buffer_index, message)
    local text = message.text .. make_dwell_code(message.duration)
    store.write_buffer(buffer_index, khscii.GetKHSCII(text))
end

local function look_matches_open_window(message)
    local current_look = store.read_look()
    for _, name in ipairs(LOOK_FIELDS) do
        if message[name] ~= nil and message[name] ~= current_look[name] then
            return false
        end
    end
    return true
end

local function can_show(message)
    if not notify_window.window_syscalls_enabled() then
        return false
    end
    if notify_window.is_event_active() or notify_window.is_summon_active() then
        return false
    end
    if count_in_flight() >= store.BUFFER_COUNT then
        return false
    end
    if not notify_window.is_window_busy() then
        return true
    end
    if message.auto_width then
        return false
    end
    if not owns_window() or notify_window.is_screen_box_closing() then
        return false
    end
    return look_matches_open_window(message)
end

local function open_window_for(message, string_id)
    store.save_template()
    notify_window.apply_look(message, string_id)
    store.write_look(message)
    notify_window.make_closing_instant()
    if not notify_window.open_without_width_pad() then
        return false
    end
    store.write_header(store.OUR_WINDOW, 1)
    return true
end

local function show(message)
    if not reserve_string_slots() then
        return false
    end
    local buffer_index = store.read_header(store.NEXT_BUFFER)
    local string_id = FIRST_STRING_ID + buffer_index
    write_buffer_text(buffer_index, message)
    store.write_sent(buffer_index, message)

    if not notify_window.is_window_busy() then
        if not open_window_for(message, string_id) then
            return false
        end
    end

    local position = notify_window.get_message_count()
    store.set_buffer_at_position(position, buffer_index)
    notify_window.display(string_id)
    if notify_window.get_message_count() ~= position + 1 then
        return false
    end

    local next_buffer = (buffer_index + 1) % store.BUFFER_COUNT
    store.write_header(store.NEXT_BUFFER, next_buffer)
    return true
end

local function requeue_in_flight()
    local first_position = store.read_header(store.REQUEUE_FIRST)
    local last_position = store.read_header(store.REQUEUE_END) - 1
    store.write_header(store.REQUEUE_END, 0)
    if last_position < first_position or last_position >= notify_window.MESSAGE_QUEUE_LENGTH then
        return 0
    end

    local in_flight_messages = {}
    for position = first_position, last_position do
        local buffer_index = store.get_buffer_at_position(position)
        in_flight_messages[#in_flight_messages + 1] = store.read_sent(buffer_index)
    end
    local held_messages = store.take_all_held()

    for _, message in ipairs(in_flight_messages) do
        store.push_held(message)
    end
    for _, message in ipairs(held_messages) do
        store.push_held(message)
    end
    return #in_flight_messages
end

local function requeue_if_ours(opened_by_us)
    if opened_by_us then
        return requeue_in_flight()
    end
    return 0
end

local function close_box_left_by_wipe(box, box_state, opened_by_us)
    local requeued = requeue_if_ours(opened_by_us)
    ConsolePrint(string.format("kh1_lua_library: window %d bookkeeping wiped under a live box (screen box state %d, script table %d, ours %s), closing it, %d requeued",
        notify_window.NOTIFY_WINDOW, box_state, ReadInt(g_EVScriptLoaded), tostring(opened_by_us), requeued))
    notify_window.close_immediately(box, box_state)
end

local function reset_window_without_box(box, window_state)
    ConsolePrint(string.format("kh1_lua_library: window %d screen box is gone but bookkeeping says state %d, resetting",
        notify_window.NOTIFY_WINDOW, window_state))
    notify_window.reset_bookkeeping(box)
    store.write_header(store.OUR_WINDOW, 0)
    store.write_header(store.EMPTY_SINCE_MS, 0)
end

local function close_window_if_empty_too_long(box, box_state, window_state, opened_by_us)
    local empty_since = store.read_header(store.EMPTY_SINCE_MS)
    if empty_since == 0 then
        store.write_header(store.EMPTY_SINCE_MS, now_ms())
        return
    end
    if now_ms() - empty_since < EMPTY_WINDOW_GRACE_MS then
        return
    end
    local requeued = requeue_if_ours(opened_by_us)
    ConsolePrint(string.format("kh1_lua_library: window %d is open with no messages (state %d, screen box state %d, position %d, ours %s), closing it, %d requeued",
        notify_window.NOTIFY_WINDOW, window_state, box_state, notify_window.get_message_position(), tostring(opened_by_us), requeued))
    notify_window.close_immediately(box, box_state)
    store.write_header(store.OUR_WINDOW, 0)
    store.write_header(store.EMPTY_SINCE_MS, 0)
end

local function repair_window()
    local opened_by_us = store.read_header(store.OUR_WINDOW) == 1
    local box = notify_window.find_screen_box()
    local box_state = notify_window.get_screen_box_state(box)
    local window_state = notify_window.get_window_state()
    local message_count = notify_window.get_message_count()

    if window_state == notify_window.WINDOW_CLOSED then
        store.write_header(store.EMPTY_SINCE_MS, 0)
        if box_state ~= notify_window.BOX_IDLE then
            close_box_left_by_wipe(box, box_state, opened_by_us)
        end
        store.write_header(store.OUR_WINDOW, 0)
        return
    end

    local window_is_opening = window_state == notify_window.WINDOW_OPENING
    if not window_is_opening and box_state == notify_window.BOX_IDLE then
        reset_window_without_box(box, window_state)
        return
    end

    local box_is_opening = box_state == notify_window.BOX_OPENING
    if message_count == 0 and not window_is_opening and not box_is_opening then
        close_window_if_empty_too_long(box, box_state, window_state, opened_by_us)
        return
    end
    store.write_header(store.EMPTY_SINCE_MS, 0)
end

local function close_window_for_summon()
    if not notify_window.is_summon_active() then
        return
    end
    if not owns_window() or notify_window.is_screen_box_closing() then
        return
    end
    local message_count = notify_window.get_message_count()
    store.write_header(store.REQUEUE_FIRST, message_count - count_in_flight())
    store.write_header(store.REQUEUE_END, message_count)
    local requeued = requeue_in_flight()
    ConsolePrint(string.format("kh1_lua_library: summon active, closing window %d, %d requeued", notify_window.NOTIFY_WINDOW, requeued))
    local box = notify_window.find_screen_box()
    notify_window.close_immediately(box, notify_window.get_screen_box_state(box))
    store.write_header(store.OUR_WINDOW, 0)
end

local function log_script_table_changes()
    local table_count = ReadInt(g_EVScriptLoaded)
    local last_table_count = store.read_header(store.LAST_SCRIPT_TABLE)
    if table_count == last_table_count then
        return
    end
    if store.read_header(store.OUR_WINDOW) == 1 then
        ConsolePrint(string.format("kh1_lua_library: EV string table count %d -> %d while our window is open (state %d, count %d)",
            last_table_count, table_count, notify_window.get_window_state(), notify_window.get_message_count()))
    end
    store.write_header(store.LAST_SCRIPT_TABLE, table_count)
end

local function show_held_messages()
    while store.count_held() > 0 do
        local message = store.peek_held()
        if not can_show(message) then
            return
        end
        if not show(message) then
            return
        end
        store.drop_held()
    end
end

local function restore_template_when_idle()
    if notify_window.is_window_busy() then
        return
    end
    if store.count_held() > 0 then
        return
    end
    store.restore_template()
end

local function play_sound_for_new_message()
    local text_address = get_shown_text_address()
    if text_address == store.read_last_shown_text() then
        return
    end
    store.write_last_shown_text(text_address)

    local buffer_index = store.find_buffer_index(text_address)
    if buffer_index == nil then
        return
    end

    local last_shown_at = store.read_header(store.LAST_SHOWN_AT_MS)
    local ms_since_last = 0
    if last_shown_at > 0 then
        ms_since_last = now_ms() - last_shown_at
    end
    ConsolePrint(string.format("kh1_lua_library: window %d showing a message (%d ms after the previous, %d queued)",
        notify_window.NOTIFY_WINDOW, ms_since_last, notify_window.get_message_count()))
    store.write_header(store.LAST_SHOWN_AT_MS, now_ms())

    local message = store.read_sent(buffer_index)
    if message.sound ~= nil and fnc_play_se2 ~= nil then
        kh1_native.call_function(fnc_play_se2, message.sound, 0)
    end
end

local function remember_in_flight_range()
    local message_count = notify_window.get_message_count()
    if message_count == 0 then
        return
    end
    local showing_buffer = find_showing_buffer()
    if showing_buffer == nil then
        return
    end
    local position = find_queue_position_of_buffer(showing_buffer, message_count)
    if position == nil then
        return
    end
    store.write_header(store.REQUEUE_FIRST, position)
    store.write_header(store.REQUEUE_END, message_count)
end

local function queue_text_box(options)
    if type(options) ~= "table" or type(options.text) ~= "string" then
        return false
    end
    if not store.open() then
        return false
    end
    local message = clean_message(options)
    if store.count_held() == 0 and can_show(message) and show(message) then
        return true
    end
    store.push_held(message)
    return true
end

local function close_text_box()
    return notify_window.close()
end

local function update_text_boxes()
    if not store.open() then
        return
    end
    log_script_table_changes()
    repair_window()
    close_window_for_summon()
    show_held_messages()
    restore_template_when_idle()
    play_sound_for_new_message()
    remember_in_flight_range()
end

local function is_text_box_busy()
    return notify_window.is_window_busy()
end

local function pending_count()
    if not store.open() then
        return 0
    end
    return store.count_held() + count_in_flight()
end

return {
    queue_text_box = queue_text_box,
    close_text_box = close_text_box,
    update_text_boxes = update_text_boxes,
    is_text_box_busy = is_text_box_busy,
    pending_count = pending_count,
}
