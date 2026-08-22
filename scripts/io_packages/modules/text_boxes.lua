---@diagnostic disable: undefined-global

-- Text box feature: custom-text code-cave hooks (ported byte-for-byte from the old kh1_native
-- C++) plus the open/close/update driver. EVDL syscalls go through kh1_native.call_evdl_syscall.

local kh1_native = require("kh1_native")
local code_cave = require("helpers.code_cave")
local khscii = require("helpers.khscii")

local sc = code_cave.sc
local install_splice = code_cave.install_splice
local GetKHSCII = khscii.GetKHSCII

-- Persistent state: stub-embedded addresses (flag/buffer) and pending-box tracking must stay
-- stable across script reloads. LuaBackend reads are unsigned, so use a 0/1 valid flag.
local STATE_KEY  = "kh1_textbox_state_v1"
local STATE_SIZE = 0x220
local OFF_FLAG          = 0x00  -- active flag byte (the stub clears it after consuming the text)
local OFF_PENDING_VALID = 0x04
local OFF_PENDING_WID   = 0x08
local OFF_PENDING_RVA   = 0x10
local OFF_BUF           = 0x20  -- 512-byte text buffer
local BUF_MAX = 511

local function state_block()
    local blk = kh1_native.persistent_block(STATE_KEY, STATE_SIZE)
    if blk == 0 or blk == nil then return nil end
    return blk
end

-- ######################## --
-- # Hook stubs           # --
-- ######################## --

-- Text box body hook: point rdx at our buffer for the life of the box while the flag is set.
local function textbox_chunks(flag, buf, resume_abs)
    return function(orig, cave)
        return {
            orig,                                       -- replay mov rdx,[r11+r10*8+disp32]
            sc(0x50),                                   -- push rax
            sc(0x48, 0xB8) .. string.pack("<I8", flag), -- mov rax, flagAddr
            sc(0x80, 0x38, 0x01),                       -- cmp byte [rax],1
            sc(0x0F, 0x85), {rel32 = "skip"},           -- jne skip
            sc(0xC6, 0x00, 0x00),                       -- mov byte [rax],0
            sc(0x48, 0xBA) .. string.pack("<I8", buf),  -- mov rdx, bufAddr
            {label = "skip"},
            sc(0x58),                                   -- pop rax
            sc(0xE9), {rel32 = resume_abs},             -- jmp resume
        }
    end
end

-- Per-character animation call site: substitute rdx before the game's call while flagged.
local function textbox_anim_chunks(flag, buf, resume_abs, call_target_abs)
    return function(orig, cave)
        return {
            sc(0x50),                                   -- push rax
            sc(0x48, 0xB8) .. string.pack("<I8", flag), -- mov rax, flagAddr
            sc(0x80, 0x38, 0x01),                       -- cmp byte [rax],1
            sc(0x0F, 0x85), {rel32 = "use_orig"},       -- jne use_orig
            sc(0xC6, 0x00, 0x00),                       -- mov byte [rax],0
            sc(0x48, 0xBA) .. string.pack("<I8", buf),  -- mov rdx, bufAddr
            sc(0x58),                                   -- pop rax
            sc(0xE9), {rel32 = "continue_call"},        -- jmp continue_call
            {label = "use_orig"},
            sc(0x58),                                   -- pop rax
            orig:sub(1, 4),                             -- replay mov rdx,[r12+rdx*8]
            {label = "continue_call"},
            sc(0xE8), {rel32 = call_target_abs},        -- call original target
            sc(0xE9), {rel32 = resume_abs},             -- jmp resume
        }
    end
end

local function install_textbox_hook(hook_rva, resume_rva)
    local base = kh1_native.get_module_base()
    local blk = state_block()
    if not blk then return false end
    return install_splice("InstallTextBoxHook", base + hook_rva, 8,
        function(o) return o[1] == 0x4B and o[2] == 0x8B and o[3] == 0x94 end,
        textbox_chunks(blk + OFF_FLAG, blk + OFF_BUF, base + resume_rva))
end

local function install_textbox_anim_hook(hook_rva, resume_rva, call_target_rva)
    local base = kh1_native.get_module_base()
    local blk = state_block()
    if not blk then return false end
    return install_splice("InstallTextBoxAnimHook", base + hook_rva, 9,
        function(o) return o[1] == 0x49 and o[2] == 0x8B and o[3] == 0x14 and o[4] == 0xD4 and o[5] == 0xE8 end,
        textbox_anim_chunks(blk + OFF_FLAG, blk + OFF_BUF, base + resume_rva, base + call_target_rva))
end

local function set_textbox_text(text)
    local blk = state_block()
    if not blk then return end
    local khs = GetKHSCII(text)
    local n = math.min(#khs, BUF_MAX)
    local bytes = {}
    for i = 1, n do bytes[i] = khs[i] end
    bytes[n + 1] = 0
    WriteArray(blk + OFF_BUF, bytes, true)
    WriteByte(blk + OFF_FLAG, 1, true)
end

-- ######################## --
-- # Pending-box tracking # --
-- ######################## --

-- Tracks the most recent box so it can be closed after a script reload without an id table.
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
    -- Controls the style of text box.
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_005_set_window_type, {window_id, style})
end

local function set_text_box_position(window_id, x, y)
    -- Controls text box position on screen.
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_003_set_window_position, {window_id, x, y})
end

local function set_text_box_size(window_id, width, height)
    -- Controls box width/height.
    window_id = window_id or 1
    return kh1_native.call_evdl_syscall(fnc_004_set_window_size, {window_id, width, height})
end

local function open_text_box(text, window_id, duration_seconds, style, x, y, width, height)
    --[[ Opens a text box. Boxes normally reference predefined text, so ASM
    and text memory are injected to allow custom text.]]
    window_id = window_id or 1
    if style ~= nil then
        set_text_box_style(window_id, style)
    end
    if x ~= nil and y ~= nil then
        set_text_box_position(window_id, x, y)
    end
    if width ~= nil and height ~= nil then
        set_text_box_size(window_id, width, height)
    end
    install_textbox_hook(fnc_display_message_text_hook, fnc_display_message_text_resume)
    install_textbox_anim_hook(fnc_display_message_anim_hook, fnc_display_message_anim_resume, fnc_display_message_anim_call_target)
    set_textbox_text(text)
    local opened = kh1_native.call_evdl_syscall(fnc_0B1_open_window_no_close, {window_id})
    local displayed = kh1_native.call_evdl_syscall(fnc_001_display_message, {window_id, 0})
    set_pending_text_box(window_id, fnc_002_close_window)
    open_text_boxes[window_id] = {
        deadline = (duration_seconds and duration_seconds > 0) and (os.clock() + duration_seconds) or nil,
        open_world = get_world(),
        open_room = get_room(),
    }
    return opened and displayed
end

local function close_text_box(window_id)
    -- Closes text box
    window_id = window_id or 1
    open_text_boxes[window_id] = nil
    clear_pending_text_box()
    return kh1_native.call_evdl_syscall(fnc_002_close_window, {window_id})
end

local function update_text_boxes()
    -- Handles the timing mechanism for open text boxes.
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

-- If the script is refreshed, clean up any box left open by the previous instance.
close_pending_text_box()

return {
    set_text_box_style = set_text_box_style,
    set_text_box_position = set_text_box_position,
    set_text_box_size = set_text_box_size,
    open_text_box = open_text_box,
    close_text_box = close_text_box,
    update_text_boxes = update_text_boxes,
    -- exposed for offline stub verification only
    _test = {
        textbox_chunks = textbox_chunks,
        textbox_anim_chunks = textbox_anim_chunks,
    },
}
