---@diagnostic disable: undefined-global

local cffi = require("cffi")
local call_redirect = require("helpers.call_redirect")
local game = require("modules.menus.game")
local text = require("modules.menus.text")

cffi.cdef[[
    short GetAsyncKeyState(int key);
    int GetKeyboardState(uint8_t* state);
    int ToUnicode(uint32_t key, uint32_t scan_code, const uint8_t* state, uint16_t* text, int text_size, uint32_t flags);
    uint32_t MapVirtualKeyA(uint32_t code, uint32_t map_type);
    void* GetForegroundWindow(void);
    uint32_t GetWindowThreadProcessId(void* window, uint32_t* process_id);
    int OpenClipboard(void* owner);
    void* GetClipboardData(uint32_t format);
    int CloseClipboard(void);
    void* GlobalLock(void* memory);
    int GlobalUnlock(void* memory);
]]

local user32 = cffi.load("user32")

local KEY_BACKSPACE = 0x08
local KEY_ENTER = 0x0D
local KEY_CONTROL = 0x11
local KEY_ESCAPE = 0x1B
local KEY_V = 0x56
local FIRST_KEY = 0x08
local LAST_KEY = 0xFE
local KEY_DOWN = 0x8000
local MAP_KEY_TO_SCAN_CODE = 0
local TO_UNICODE_KEEP_KEYBOARD_STATE = 4
local TYPED_CHARACTER_SIZE = 8
local CLIPBOARD_UNICODE_TEXT = 13
local MAXIMUM_CLIPBOARD_LENGTH = 4096
local BACKSPACE_REPEAT_DELAY = 30
local BACKSPACE_REPEAT_RATE = 3

local editing = false
local waiting_for_buttons_release = false
local window_seen_this_frame = false
local previous_keys = {}
local backspace_held_frames = 0
local key_state = cffi.new("uint8_t[256]")
local typed_character = cffi.new("uint16_t[?]", TYPED_CHARACTER_SIZE)
local process_id = cffi.new("uint32_t[1]")

local keyboard = {}

local function game_is_foreground()
    user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), process_id)
    return cffi.tonumber(process_id[0]) == cffi.tonumber(cffi.C.GetCurrentProcessId())
end

local function key_is_down(key)
    return cffi.tonumber(user32.GetAsyncKeyState(key)) & KEY_DOWN ~= 0
end

local function is_typeable_code(code)
    if code < 0x20 then
        return false
    end
    if code >= 0x7F and code <= 0x9F then
        return false
    end
    if code >= 0xD800 and code <= 0xDFFF then
        return false
    end
    return true
end

local function utf16_units(pointer, count)
    local units = {}
    for index = 0, count - 1 do
        units[index] = cffi.tonumber(pointer[index])
    end
    return units
end

local function read_clipboard()
    if cffi.tonumber(user32.OpenClipboard(nil)) == 0 then
        return ""
    end
    local result = ""
    local clipboard_data = user32.GetClipboardData(CLIPBOARD_UNICODE_TEXT)
    if game.to_integer(clipboard_data) ~= 0 then
        local pointer = cffi.C.GlobalLock(clipboard_data)
        if game.to_integer(pointer) ~= 0 then
            local units = cffi.cast("const uint16_t*", pointer)
            local count = 0
            while count < MAXIMUM_CLIPBOARD_LENGTH and cffi.tonumber(units[count]) ~= 0 do
                count = count + 1
            end
            result = text.from_utf16(utf16_units(units, count), count)
            cffi.C.GlobalUnlock(clipboard_data)
        end
    end
    user32.CloseClipboard()
    return result
end

local function clean_pasted_text(pasted)
    local cleaned = string.gsub(pasted, "^%s+", "")
    cleaned = string.gsub(cleaned, "%s+$", "")
    cleaned = string.gsub(cleaned, "^/connect%s+", "")
    return cleaned
end

local function character_for_key(key)
    local scan_code = user32.MapVirtualKeyA(key, MAP_KEY_TO_SCAN_CODE)
    local count = cffi.tonumber(user32.ToUnicode(key, scan_code, key_state, typed_character, TYPED_CHARACTER_SIZE, TO_UNICODE_KEEP_KEYBOARD_STATE))
    if count < 1 then
        return ""
    end
    return text.from_utf16(utf16_units(typed_character, count), count)
end

local function add_text(value, added, maximum_length)
    for _, code in ipairs(text.character_codes(added)) do
        if is_typeable_code(code) and text.character_count(value) < maximum_length then
            value = value .. utf8.char(code)
        end
    end
    return value
end

local function remove_last_character(value)
    return text.first_characters(value, text.character_count(value) - 1)
end

local function stop_editing()
    editing = false
    waiting_for_buttons_release = true
    game.play_sound(game.SOUND_CONFIRM)
end

local function value_after_key(value, key, maximum_length)
    if key == KEY_ENTER or key == KEY_ESCAPE then
        stop_editing()
        return value
    end
    if key == KEY_BACKSPACE then
        return remove_last_character(value)
    end
    if key == KEY_V and key_is_down(KEY_CONTROL) then
        return add_text(value, clean_pasted_text(read_clipboard()), maximum_length)
    end
    return add_text(value, character_for_key(key), maximum_length)
end

local function value_after_backspace_repeat(value)
    if not key_is_down(KEY_BACKSPACE) then
        backspace_held_frames = 0
        return value
    end
    backspace_held_frames = backspace_held_frames + 1
    if backspace_held_frames > BACKSPACE_REPEAT_DELAY and backspace_held_frames % BACKSPACE_REPEAT_RATE == 0 then
        return remove_last_character(value)
    end
    return value
end

function keyboard.type_into(value, maximum_length)
    if not game_is_foreground() then
        return value
    end
    user32.GetKeyboardState(key_state)
    for key = FIRST_KEY, LAST_KEY do
        local down = key_is_down(key)
        if down and not previous_keys[key] then
            value = value_after_key(value, key, maximum_length)
            if not editing then
                return value
            end
        end
        previous_keys[key] = down
    end
    return value_after_backspace_repeat(value)
end

function keyboard.key_is_down(key)
    return game_is_foreground() and key_is_down(key)
end

function keyboard.start_editing()
    editing = true
    for key = FIRST_KEY, LAST_KEY do
        previous_keys[key] = key_is_down(key)
    end
    backspace_held_frames = 0
    game.play_sound(game.SOUND_CONFIRM)
end

function keyboard.cancel_editing()
    editing = false
end

function keyboard.is_editing()
    return editing
end

function keyboard.window_seen()
    window_seen_this_frame = true
end

local function filtered_buttons(buttons)
    if not window_seen_this_frame then
        editing = false
        waiting_for_buttons_release = false
    end
    window_seen_this_frame = false
    if editing then
        return 0
    end
    if waiting_for_buttons_release then
        if buttons == 0 then
            waiting_for_buttons_release = false
        end
        return 0
    end
    return buttons
end

local function pad_write_buttons(pad, buttons)
    local worked, result = pcall(filtered_buttons, cffi.tonumber(buttons))
    if worked then
        game.pad_write_buttons(pad, result)
    else
        game.pad_write_buttons(pad, buttons)
    end
end

function keyboard.install()
    if fnc_pad_write_buttons_call == nil then
        return false
    end
    return call_redirect.redirect_call(fnc_pad_write_buttons_call, fnc_pad_write_buttons, "MenuPadWriteButtons", pad_write_buttons)
end

return keyboard
