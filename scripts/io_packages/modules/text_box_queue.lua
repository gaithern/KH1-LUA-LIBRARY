---@diagnostic disable: undefined-global

local kh1_native     = require("kh1_native")
local kh1_text_boxes = require("modules.text_boxes")

local BLOCK_KEY = "kh1_textbox_queue_v1"
local MAGIC     = 0x4B545851
local VERSION   = 1
local CAPACITY  = 32

local HDR_MAGIC        = 0x00
local HDR_VERSION      = 0x04
local HDR_CAPACITY     = 0x08
local HDR_HEAD         = 0x0C
local HDR_COUNT        = 0x10
local HDR_HEARTBEAT_MS = 0x14
local HDR_CUR_VALID    = 0x18
local HDR_CUR_WINDOW   = 0x1C
local HDR_CUR_DEADLINE = 0x20
local HDR_CUR_WORLD    = 0x24
local HDR_CUR_ROOM     = 0x28
local HDR_CUR_SLOT     = 0x2C
local HDR_GHOST_SINCE  = 0x30
local HDR_SIZE         = 0x40

local REC_TEXT      = 0x000
local REC_TEXT_MAX  = 1023
local REC_WINDOW    = 0x400
local REC_DURATION  = 0x404
local REC_STYLE     = 0x408
local REC_X         = 0x40C
local REC_Y         = 0x410
local REC_WIDTH     = 0x414
local REC_HEIGHT    = 0x418
local REC_TAIL      = 0x41C
local REC_SOUND     = 0x420
local REC_PRIORITY  = 0x424
local REC_FLAGS     = 0x428
local REC_SIZE      = 0x440

local FLAG_HAS_STYLE = 1
local FLAG_HAS_POS   = 2
local FLAG_HAS_SIZE  = 4
local FLAG_HAS_TAIL  = 8
local FLAG_HAS_SOUND = 16

local BLOCK_SIZE = HDR_SIZE + CAPACITY * REC_SIZE

local DEFAULT_WINDOW   = 6
local DEFAULT_DURATION = 2.5
local GHOST_TIMEOUT_MS = 2000

local WINDOW_SLOTS      = 4
local MSG_QUEUE_STRIDE  = 0x200
local MSG_QUEUE_ENTRIES = 0x100

local function now_ms()
    return math.floor(os.clock() * 1000)
end

local function block()
    local blk = kh1_native.persistent_block(BLOCK_KEY, BLOCK_SIZE)
    if blk == 0 or blk == nil then return nil end
    if ReadInt(blk + HDR_MAGIC, true) ~= MAGIC or ReadInt(blk + HDR_VERSION, true) ~= VERSION then
        WriteInt(blk + HDR_MAGIC, MAGIC, true)
        WriteInt(blk + HDR_VERSION, VERSION, true)
        WriteInt(blk + HDR_CAPACITY, CAPACITY, true)
        WriteInt(blk + HDR_HEAD, 0, true)
        WriteInt(blk + HDR_COUNT, 0, true)
        WriteInt(blk + HDR_HEARTBEAT_MS, 0, true)
        WriteInt(blk + HDR_CUR_VALID, 0, true)
        WriteInt(blk + HDR_GHOST_SINCE, 0, true)
    end
    return blk
end

local function record_addr(blk, index)
    return blk + HDR_SIZE + (index % CAPACITY) * REC_SIZE
end

-- ######################## --
-- # Producer side        # --
-- ######################## --

local function write_text(addr, text)
    local n = math.min(#text, REC_TEXT_MAX)
    local bytes = {}
    for i = 1, n do bytes[i] = text:byte(i) end
    bytes[n + 1] = 0
    WriteArray(addr, bytes, true)
end

local function read_text(addr)
    local bytes = ReadArray(addr, REC_TEXT_MAX + 1, true)
    local chars = {}
    for i = 1, #bytes do
        if bytes[i] == 0 then break end
        chars[#chars + 1] = string.char(bytes[i])
    end
    return table.concat(chars)
end

local function enqueue(opts)
    if type(opts) ~= "table" or type(opts.text) ~= "string" then return false end
    local blk = block()
    if not blk then return false end
    local head = ReadInt(blk + HDR_HEAD, true)
    local count = ReadInt(blk + HDR_COUNT, true)
    if count >= CAPACITY then
        head = (head + 1) % CAPACITY
        count = CAPACITY - 1
        WriteInt(blk + HDR_HEAD, head, true)
        WriteInt(blk + HDR_COUNT, count, true)
    end

    local rec = record_addr(blk, head + count)
    local flags = 0
    write_text(rec + REC_TEXT, opts.text)
    WriteInt(rec + REC_WINDOW, opts.window or DEFAULT_WINDOW, true)
    WriteInt(rec + REC_DURATION, math.floor((opts.duration or DEFAULT_DURATION) * 1000), true)
    if opts.style ~= nil then flags = flags + FLAG_HAS_STYLE; WriteInt(rec + REC_STYLE, opts.style, true) end
    if opts.x ~= nil and opts.y ~= nil then
        flags = flags + FLAG_HAS_POS
        WriteInt(rec + REC_X, opts.x, true)
        WriteInt(rec + REC_Y, opts.y, true)
    end
    if opts.width ~= nil and opts.height ~= nil then
        flags = flags + FLAG_HAS_SIZE
        WriteInt(rec + REC_WIDTH, opts.width, true)
        WriteInt(rec + REC_HEIGHT, opts.height, true)
    end
    if opts.tail ~= nil then flags = flags + FLAG_HAS_TAIL; WriteInt(rec + REC_TAIL, opts.tail, true) end
    if opts.sound ~= nil then flags = flags + FLAG_HAS_SOUND; WriteInt(rec + REC_SOUND, opts.sound, true) end
    WriteInt(rec + REC_PRIORITY, opts.priority or 0, true)
    WriteInt(rec + REC_FLAGS, flags, true)

    WriteInt(blk + HDR_COUNT, count + 1, true)
    return true
end

local function pending_count()
    local blk = block()
    if not blk then return 0 end
    return ReadInt(blk + HDR_COUNT, true)
end

local function driver_alive()
    local blk = block()
    if not blk then return false end
    local beat = ReadInt(blk + HDR_HEARTBEAT_MS, true)
    return beat > 0 and (now_ms() - beat) < 1000
end

-- ######################## --
-- # Window ownership     # --
-- ######################## --

local function window_state(window_id)
    return ReadByte(g_EVWindowState + window_id)
end

local function any_window_active()
    for slot = 0, WINDOW_SLOTS - 1 do
        if ReadByte(g_EVWindowActivity + slot) ~= 0 then return true end
    end
    return false
end

local function last_message_id(window_id)
    local count = ReadInt(g_EVWindowMsgCount + window_id * 4)
    if count < 1 or count > MSG_QUEUE_ENTRIES then return nil end
    return ReadShort(g_EVWindowMsgQueue + window_id * MSG_QUEUE_STRIDE + (count - 1) * 2)
end

local function owns_window(window_id, slot)
    if window_state(window_id) == 0 then return false end
    return last_message_id(window_id) == slot
end

local function message_count(window_id)
    return ReadInt(g_EVWindowMsgCount + window_id * 4)
end

local function can_open()
    if ReadInt(g_EVScriptLoaded) <= 0 then return false end
    local w = ReadByte(world)
    return w ~= 0 and w ~= 0xFF
end

-- ######################## --
-- # Driver side          # --
-- ######################## --

local function clear_current(blk)
    WriteInt(blk + HDR_CUR_VALID, 0, true)
end

local function tend_current(blk)
    if ReadInt(blk + HDR_CUR_VALID, true) == 0 then return false end
    local window_id = ReadInt(blk + HDR_CUR_WINDOW, true)
    local slot = ReadInt(blk + HDR_CUR_SLOT, true)
    local state = window_state(window_id)
    local last = last_message_id(window_id)
    if state ~= 0 and last ~= nil and last ~= slot then
        clear_current(blk)
        return false
    end
    local expired = now_ms() >= ReadInt(blk + HDR_CUR_DEADLINE, true)
    local moved = ReadByte(world) ~= ReadInt(blk + HDR_CUR_WORLD, true)
        or ReadByte(room) ~= ReadInt(blk + HDR_CUR_ROOM, true)
    if expired or moved then
        if state ~= 0 then
            kh1_text_boxes.close_text_box(window_id)
        end
        clear_current(blk)
        return false
    end
    return true
end

local function sweep_ghost(blk)
    local window_id = ReadInt(blk + HDR_CUR_WINDOW, true)
    if window_state(window_id) ~= 0 and message_count(window_id) == 0 then
        local since = ReadInt(blk + HDR_GHOST_SINCE, true)
        if since == 0 then
            WriteInt(blk + HDR_GHOST_SINCE, now_ms(), true)
        elseif now_ms() - since >= GHOST_TIMEOUT_MS then
            kh1_text_boxes.close_text_box(window_id)
            WriteInt(blk + HDR_GHOST_SINCE, 0, true)
        end
    else
        WriteInt(blk + HDR_GHOST_SINCE, 0, true)
    end
end

local function show_next(blk)
    local head = ReadInt(blk + HDR_HEAD, true)
    local rec = record_addr(blk, head)
    local flags = ReadInt(rec + REC_FLAGS, true)
    local function opt(flag, off) if flags % (flag * 2) >= flag then return ReadInt(rec + off, true) end return nil end

    local window_id = ReadInt(rec + REC_WINDOW, true)
    local text = read_text(rec + REC_TEXT)
    local style = opt(FLAG_HAS_STYLE, REC_STYLE)
    local x, y = opt(FLAG_HAS_POS, REC_X), opt(FLAG_HAS_POS, REC_Y)
    local width, height = opt(FLAG_HAS_SIZE, REC_WIDTH), opt(FLAG_HAS_SIZE, REC_HEIGHT)
    local tail = opt(FLAG_HAS_TAIL, REC_TAIL)
    local sound = opt(FLAG_HAS_SOUND, REC_SOUND)

    local ok, message_id = kh1_text_boxes.open_text_box(text, window_id, nil, style, x, y, width, height, tail)
    if not ok then return false end
    kh1_text_boxes.forget_text_box(window_id)

    WriteInt(blk + HDR_HEAD, (head + 1) % CAPACITY, true)
    WriteInt(blk + HDR_COUNT, ReadInt(blk + HDR_COUNT, true) - 1, true)
    WriteInt(blk + HDR_CUR_WINDOW, window_id, true)
    WriteInt(blk + HDR_CUR_DEADLINE, now_ms() + ReadInt(rec + REC_DURATION, true), true)
    WriteInt(blk + HDR_CUR_WORLD, ReadByte(world), true)
    WriteInt(blk + HDR_CUR_ROOM, ReadByte(room), true)
    WriteInt(blk + HDR_CUR_SLOT, message_id or -1, true)
    WriteInt(blk + HDR_CUR_VALID, 1, true)

    if sound ~= nil and fnc_play_se2 ~= nil then
        kh1_native.call_function(fnc_play_se2, sound, 0)
    end
    return true
end

local function driver_frame()
    local blk = block()
    if not blk then return end
    WriteInt(blk + HDR_HEARTBEAT_MS, now_ms(), true)

    if tend_current(blk) then return end
    sweep_ghost(blk)
    if ReadInt(blk + HDR_COUNT, true) == 0 then return end
    if any_window_active() then return end
    if not can_open() then return end
    show_next(blk)
end

local function clear()
    local blk = block()
    if not blk then return end
    WriteInt(blk + HDR_HEAD, 0, true)
    WriteInt(blk + HDR_COUNT, 0, true)
end

return {
    enqueue = enqueue,
    pending_count = pending_count,
    driver_alive = driver_alive,
    driver_frame = driver_frame,
    clear = clear,
    owns_window = owns_window,
    any_window_active = any_window_active,
}
