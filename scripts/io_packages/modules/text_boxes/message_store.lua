---@diagnostic disable: undefined-global

local native = require("helpers.native")
local notify_window = require("modules.text_boxes.notify_window")

local BLOCK_KEY = "kh1_textbox_queue_v9"

local NEXT_BUFFER       = 0x00
local HELD_FIRST        = 0x04
local HELD_COUNT        = 0x08
local TEMPLATE_SAVED    = 0x0C
local OUR_WINDOW        = 0x10
local LAST_SCRIPT_TABLE = 0x14
local REQUEUE_FIRST     = 0x18
local REQUEUE_END       = 0x1C
local EMPTY_SINCE_MS    = 0x20
local LAST_SHOWN_AT_MS  = 0x24
local LAST_SHOWN_TEXT   = 0x28
local HEADER_SIZE       = 0x40

local RECORD_TEXT_MAX   = 511
local RECORD_DURATION   = 0x200
local RECORD_AUTO_WIDTH = 0x204
local RECORD_SIZE       = 0x240
local RECORD_OPTIONAL_FIELDS = {
    { name = "sound",  offset = 0x208 },
    { name = "style",  offset = 0x210 },
    { name = "x",      offset = 0x218 },
    { name = "y",      offset = 0x220 },
    { name = "width",  offset = 0x228 },
    { name = "height", offset = 0x230 },
    { name = "tail",   offset = 0x238 },
}

local BUFFER_COUNT    = 32
local BUFFER_SIZE     = 512
local HELD_CAPACITY   = 32
local TEMPLATE_SIZE   = notify_window.TEMPLATE_SIZE
local QUEUE_POSITIONS = notify_window.MESSAGE_QUEUE_LENGTH

local BUFFERS_START         = HEADER_SIZE
local HELD_START            = BUFFERS_START + BUFFER_COUNT * BUFFER_SIZE
local SENT_START            = HELD_START + HELD_CAPACITY * RECORD_SIZE
local LOOK_START            = SENT_START + BUFFER_COUNT * RECORD_SIZE
local TEMPLATE_BACKUP_START = LOOK_START + RECORD_SIZE
local QUEUE_POSITIONS_START = TEMPLATE_BACKUP_START + TEMPLATE_SIZE
local BLOCK_SIZE            = QUEUE_POSITIONS_START + QUEUE_POSITIONS

local block_address = nil

local function open()
    if block_address ~= nil then
        return true
    end
    local address = native.persistent_block(BLOCK_KEY, BLOCK_SIZE)
    if address == nil or address == 0 then
        return false
    end
    block_address = address
    return true
end

local function read_header(field)
    return ReadInt(block_address + field, true)
end

local function write_header(field, value)
    WriteInt(block_address + field, value, true)
end

local function read_last_shown_text()
    return ReadLong(block_address + LAST_SHOWN_TEXT, true)
end

local function write_last_shown_text(text_address)
    WriteLong(block_address + LAST_SHOWN_TEXT, text_address, true)
end

local function read_signed_int(address)
    local value = ReadInt(address, true)
    if value >= 0x80000000 then
        value = value - 0x100000000
    end
    return value
end

local function write_bytes_with_terminator(address, bytes, max_length)
    local length = math.min(#bytes, max_length)
    local bytes_to_write = {}
    for i = 1, length do
        bytes_to_write[i] = bytes[i]
    end
    bytes_to_write[length + 1] = 0
    WriteArray(address, bytes_to_write, true)
end

local function write_message(address, message)
    local text_bytes = {}
    for i = 1, #message.text do
        text_bytes[i] = message.text:byte(i)
    end
    write_bytes_with_terminator(address, text_bytes, RECORD_TEXT_MAX)

    WriteInt(address + RECORD_DURATION, math.floor(message.duration * 1000), true)

    local auto_width = 0
    if message.auto_width then
        auto_width = 1
    end
    WriteInt(address + RECORD_AUTO_WIDTH, auto_width, true)

    for _, field in ipairs(RECORD_OPTIONAL_FIELDS) do
        local value = message[field.name]
        if value == nil then
            WriteInt(address + field.offset, 0, true)
        else
            WriteInt(address + field.offset, 1, true)
            WriteInt(address + field.offset + 4, value, true)
        end
    end
end

local function read_message(address)
    local text_bytes = ReadArray(address, RECORD_TEXT_MAX + 1, true)
    local characters = {}
    for i = 1, #text_bytes do
        if text_bytes[i] == 0 then
            break
        end
        characters[#characters + 1] = string.char(text_bytes[i])
    end

    local message = {}
    message.text = table.concat(characters)
    message.duration = ReadInt(address + RECORD_DURATION, true) / 1000
    if ReadInt(address + RECORD_AUTO_WIDTH, true) == 1 then
        message.auto_width = true
    end

    for _, field in ipairs(RECORD_OPTIONAL_FIELDS) do
        local has_value = ReadInt(address + field.offset, true) == 1
        if has_value then
            message[field.name] = read_signed_int(address + field.offset + 4)
        end
    end
    return message
end

local function get_buffer_address(index)
    return block_address + BUFFERS_START + index * BUFFER_SIZE
end

local function write_buffer(index, khscii_bytes)
    write_bytes_with_terminator(get_buffer_address(index), khscii_bytes, BUFFER_SIZE - 1)
end

local function find_buffer_index(text_address)
    local offset = text_address - get_buffer_address(0)
    if offset < 0 then
        return nil
    end
    if offset % BUFFER_SIZE ~= 0 then
        return nil
    end
    local index = math.floor(offset / BUFFER_SIZE)
    if index >= BUFFER_COUNT then
        return nil
    end
    return index
end

local function write_sent(index, message)
    write_message(block_address + SENT_START + index * RECORD_SIZE, message)
end

local function read_sent(index)
    return read_message(block_address + SENT_START + index * RECORD_SIZE)
end

local function write_look(message)
    write_message(block_address + LOOK_START, message)
end

local function read_look()
    return read_message(block_address + LOOK_START)
end

local function set_buffer_at_position(position, index)
    if position < 0 or position >= QUEUE_POSITIONS then
        return
    end
    WriteByte(block_address + QUEUE_POSITIONS_START + position, index, true)
end

local function get_buffer_at_position(position)
    return ReadByte(block_address + QUEUE_POSITIONS_START + position, true)
end

local function get_held_address(position)
    local slot = position % HELD_CAPACITY
    return block_address + HELD_START + slot * RECORD_SIZE
end

local function count_held()
    return read_header(HELD_COUNT)
end

local function push_held(message)
    local first = read_header(HELD_FIRST)
    local count = read_header(HELD_COUNT)
    if count >= HELD_CAPACITY then
        first = (first + 1) % HELD_CAPACITY
        count = HELD_CAPACITY - 1
        write_header(HELD_FIRST, first)
    end
    write_message(get_held_address(first + count), message)
    write_header(HELD_COUNT, count + 1)
end

local function peek_held()
    if count_held() == 0 then
        return nil
    end
    return read_message(get_held_address(read_header(HELD_FIRST)))
end

local function drop_held()
    local count = count_held()
    if count == 0 then
        return
    end
    local first = read_header(HELD_FIRST)
    write_header(HELD_FIRST, (first + 1) % HELD_CAPACITY)
    write_header(HELD_COUNT, count - 1)
end

local function take_all_held()
    local messages = {}
    while count_held() > 0 do
        messages[#messages + 1] = peek_held()
        drop_held()
    end
    return messages
end

local function save_template()
    if read_header(TEMPLATE_SAVED) == 1 then
        return
    end
    local template = ReadArray(notify_window.get_template_address(), TEMPLATE_SIZE)
    WriteArray(block_address + TEMPLATE_BACKUP_START, template, true)
    write_header(TEMPLATE_SAVED, 1)
end

local function restore_template()
    if read_header(TEMPLATE_SAVED) ~= 1 then
        return
    end
    local template = ReadArray(block_address + TEMPLATE_BACKUP_START, TEMPLATE_SIZE, true)
    WriteArray(notify_window.get_template_address(), template)
    write_header(TEMPLATE_SAVED, 0)
end

return {
    BUFFER_COUNT = BUFFER_COUNT,

    NEXT_BUFFER = NEXT_BUFFER,
    OUR_WINDOW = OUR_WINDOW,
    LAST_SCRIPT_TABLE = LAST_SCRIPT_TABLE,
    REQUEUE_FIRST = REQUEUE_FIRST,
    REQUEUE_END = REQUEUE_END,
    EMPTY_SINCE_MS = EMPTY_SINCE_MS,
    LAST_SHOWN_AT_MS = LAST_SHOWN_AT_MS,

    open = open,
    read_header = read_header,
    write_header = write_header,
    read_last_shown_text = read_last_shown_text,
    write_last_shown_text = write_last_shown_text,

    get_buffer_address = get_buffer_address,
    write_buffer = write_buffer,
    find_buffer_index = find_buffer_index,

    write_sent = write_sent,
    read_sent = read_sent,
    write_look = write_look,
    read_look = read_look,

    set_buffer_at_position = set_buffer_at_position,
    get_buffer_at_position = get_buffer_at_position,

    count_held = count_held,
    push_held = push_held,
    peek_held = peek_held,
    drop_held = drop_held,
    take_all_held = take_all_held,

    save_template = save_template,
    restore_template = restore_template,
}
