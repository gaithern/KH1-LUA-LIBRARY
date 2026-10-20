---@diagnostic disable: undefined-global

local cffi = require("cffi")
local native = require("helpers.native")

cffi.cdef[[
    typedef void (*MenuWindowUpdate)(void* window);
    typedef void (*MenuDrawText)(uint64_t window, int style, int font, int alignment, int x, int y, int width, int anchor, int message);
    typedef void (*MenuDrawPillText)(uint64_t window, uint64_t font, int alignment, int x, int y, int width, int anchor, uint64_t style, int message);
    typedef void (*MenuDrawSelectionBar)(uint64_t window, int x, int y, int width);
    typedef int (*MenuDrawString)(uint64_t window, int alignment, int x, int y, const uint8_t* text, int size, uint32_t color, int unknown);
    typedef uint64_t (*MenuCreateWindow)(uint64_t descriptor, int layer);
    typedef int (*MenuCloseWindow)(uint64_t window);
    typedef void (*MenuLinkWindows)(uint64_t window, uint64_t other_window);
    typedef void (*MenuWindowAction)(uint64_t window);
    typedef int (*MenuListCursorRow)(uint64_t window);
    typedef void (*MenuMoveCursorSprite)(uint64_t window, int speed);
    typedef uint32_t (*MenuWindowIsActive)(uint64_t window);
    typedef void (*MenuPlaySound)(int sound);
    typedef int (*MenuHelpTextSize)(uint64_t object);
    typedef void (*MenuPadWriteButtons)(void* pad, uint32_t buttons);
    typedef void (*MenuAction)(void);
    typedef int (*MenuCount)(void);
    typedef void (*MenuOneNumber)(int value);
    typedef void (*MenuTwoNumbers)(int first, int second);
    typedef uint64_t (*MenuAddJob)(int priority, uint64_t job_function);
    typedef uint64_t (*MenuJob)(uint64_t job);
]]

local game = {
    SOUND_CURSOR = 1,
    SOUND_CONFIRM = 2,
    SOUND_CANCEL = 3,
    SOUND_ERROR = 4,
}

local installed = false

local function cast(function_type, rva)
    return cffi.cast(function_type, BASE_ADDR + rva)
end

function game.to_integer(pointer)
    return cffi.tonumber(cffi.cast("uintptr_t", pointer))
end

function game.make_callback(lua_function)
    return cffi.cast("MenuWindowUpdate", lua_function)
end

function game.run_safely(label, action, ...)
    local worked, message = pcall(action, ...)
    if not worked then
        native.log_debug(label .. ": " .. tostring(message))
    end
end

function game.window_is_active(window)
    return cffi.tonumber(game.is_active(window)) & 1 == 1
end

function game.cursor_row(window)
    return cffi.tonumber(game.list_cursor_row(window))
end

function game.install()
    if installed then
        return true
    end
    if fnc_menu_draw_string == nil or g_HelpTextSizeObject == nil or fnc_menu_draw_pill_text == nil then
        return false
    end
    game.main_menu_update = cast("MenuWindowUpdate", fnc_main_menu_update)
    game.main_menu_help_update = cast("MenuWindowUpdate", fnc_main_menu_help_update)
    game.draw_text = cast("MenuDrawText", fnc_menu_draw_text)
    game.draw_pill_text = cast("MenuDrawPillText", fnc_menu_draw_pill_text)
    game.draw_selection_bar = cast("MenuDrawSelectionBar", fnc_menu_draw_selection_bar)
    game.draw_string = cast("MenuDrawString", fnc_menu_draw_string)
    game.create_window = cast("MenuCreateWindow", fnc_menu_create_window)
    game.close_window = cast("MenuCloseWindow", fnc_menu_close_window)
    game.set_child = cast("MenuLinkWindows", fnc_menu_set_child)
    game.set_parent = cast("MenuLinkWindows", fnc_menu_set_parent)
    game.bring_to_front = cast("MenuWindowAction", fnc_menu_bring_to_front)
    game.list_cursor_update = cast("MenuWindowAction", fnc_menu_list_cursor_update)
    game.list_cursor_row = cast("MenuListCursorRow", fnc_menu_list_cursor_row)
    game.move_cursor_sprite = cast("MenuMoveCursorSprite", fnc_menu_move_cursor_sprite)
    game.is_active = cast("MenuWindowIsActive", fnc_menu_window_is_active)
    game.play_sound = cast("MenuPlaySound", fnc_menu_play_sound)
    game.pad_write_buttons = cast("MenuPadWriteButtons", fnc_pad_write_buttons)
    local help_text_vtable = ReadLong(g_HelpTextSizeObject)
    game.help_text_size = cffi.cast("MenuHelpTextSize", ReadLong(help_text_vtable + 8, true))
    installed = true
    return true
end

return game
