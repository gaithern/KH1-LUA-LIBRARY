---@diagnostic disable: undefined-global

local cffi = require("cffi")
local native = require("helpers.native")
local game = require("modules.menus.game")
local keyboard = require("modules.menus.keyboard")
local item_menu = require("modules.menus.item_menu")

local APP_MODE_TITLE = 3
local TITLE_MENU_STATE = 0x1B
local TITLE_IDLE_STATE = 0x18
local TITLE_IDLE_TIMER_START = 0x9C9
local CLEAR_COLOR_COMPONENTS = 4
local BLACK_CLEAR_COLOR = { 0, 0, 0, 0x80 }
local PAD_PRESSED = 0x08
local PAD_TRIANGLE = 0x1000
local JOB_PRIORITY = 0x2BFE7
local JOB_PARAMETER = 0x02
local JOB_FUNCTION = 0x10
local JOB_NO_PARAMETER = 0xFFFF
local JOB_KEEP_RUNNING = 0
local JOB_FINISHED = 4
local CURSOR_FADE_IN_FRAMES = 0x10
local JOB_FINISHED_STUB_SLOT_NAME = "menus_job_finished_stub"
local JOB_FINISHED_STUB = { 0xB8, 0x04, 0x00, 0x00, 0x00, 0xC3 }

local title_menu = {}

local open_key = 0
local key_was_down = false
local job_callback = nil
local job_slot = 0
local menu_open = false
local release_job_when_lua_closes = nil
local saved_clear_color = {}
local functions = {}

local function cast(function_type, rva)
    return cffi.cast(function_type, BASE_ADDR + rva)
end

local function on_title_menu()
    return ReadInt(g_nAppMode) == APP_MODE_TITLE and ReadInt(g_nTitleState) == TITLE_MENU_STATE
end

local function open_key_pressed()
    local key_down = false
    if open_key ~= 0 then
        key_down = keyboard.key_is_down(open_key)
    end
    local newly_pressed = key_down and not key_was_down
    key_was_down = key_down
    return newly_pressed
end

local function triangle_pressed()
    local pressed = ReadInt(ReadLong(g_MenuPadState) + PAD_PRESSED, true)
    return pressed & PAD_TRIANGLE ~= 0
end

local function set_clear_color(components)
    for index = 1, CLEAR_COLOR_COMPONENTS do
        WriteInt(g_dwScreenClearColor + (index - 1) * 4, components[index])
    end
end

local function hide_title_screen()
    for index = 1, CLEAR_COLOR_COMPONENTS do
        saved_clear_color[index] = ReadInt(g_dwScreenClearColor + (index - 1) * 4)
    end
    set_clear_color(BLACK_CLEAR_COLOR)
    WriteInt(g_nTitleState, TITLE_IDLE_STATE)
end

local function show_title_screen()
    set_clear_color(saved_clear_color)
    WriteInt(g_nTitleIdleTimer, TITLE_IDLE_TIMER_START)
    WriteInt(g_nTitleState, TITLE_MENU_STATE)
end

local function finish()
    functions.set_active(0)
    show_title_screen()
    menu_open = false
end

local function run_job()
    if cffi.tonumber(functions.open_window_count()) ~= 0 then
        functions.pad_replay()
        functions.update_windows()
        functions.cursor_draw()
        return JOB_KEEP_RUNNING
    end
    finish()
    return JOB_FINISHED
end

local function job(job_pointer)
    local worked, result = pcall(run_job)
    if worked then
        return result
    end
    native.log_debug("menus.title_menu: " .. tostring(result))
    finish()
    return JOB_FINISHED
end

local function job_finished_stub()
    local slot = native.persistent_slot(JOB_FINISHED_STUB_SLOT_NAME)
    if slot == 0 then
        return 0
    end
    if native.read_slot(slot) == 0 then
        local stub_address = native.allocate(#JOB_FINISHED_STUB, true)
        if stub_address == 0 then
            return 0
        end
        WriteArray(stub_address, JOB_FINISHED_STUB, true)
        native.write_slot(slot, stub_address)
    end
    return native.read_slot(slot)
end

local function release_job()
    if not menu_open or job_slot == 0 then
        return
    end
    if ReadLong(job_slot + JOB_FUNCTION, true) ~= game.to_integer(job_callback) then
        return
    end
    local stub = job_finished_stub()
    if stub ~= 0 then
        WriteLong(job_slot + JOB_FUNCTION, stub, true)
    end
    WriteInt(g_nMenuActive, 0)
    show_title_screen()
end

local function open()
    functions.reset_windows()
    functions.set_active(1)
    functions.cursor_init_colors()
    functions.cursor_reset()
    if not item_menu.open_without_parent() then
        functions.set_active(0)
        return
    end
    functions.cursor_set_position(0, 0)
    functions.cursor_fade_in(CURSOR_FADE_IN_FRAMES)
    functions.cursor_hide()
    job_slot = cffi.tonumber(functions.add_job(JOB_PRIORITY, game.to_integer(job_callback)))
    if job_slot ~= 0 then
        WriteShort(job_slot + JOB_PARAMETER, JOB_NO_PARAMETER, true)
    end
    menu_open = true
    hide_title_screen()
    game.play_sound(game.SOUND_CONFIRM)
end

function title_menu.frame()
    if menu_open or job_callback == nil then
        return
    end
    local key_pressed = open_key_pressed()
    if not on_title_menu() then
        return
    end
    if key_pressed or triangle_pressed() then
        open()
    end
end

function title_menu.install(key)
    if g_nTitleState == nil or fnc_add_menu_job == nil then
        return false
    end
    open_key = key or 0
    functions.add_job = cast("MenuAddJob", fnc_add_menu_job)
    functions.reset_windows = cast("MenuAction", fnc_menu_reset_windows)
    functions.set_active = cast("MenuOneNumber", fnc_menu_set_active)
    functions.update_windows = cast("MenuAction", fnc_menu_update_windows)
    functions.open_window_count = cast("MenuCount", fnc_menu_open_window_count)
    functions.pad_replay = cast("MenuAction", fnc_menu_pad_replay)
    functions.cursor_init_colors = cast("MenuAction", fnc_menu_cursor_init_colors)
    functions.cursor_reset = cast("MenuAction", fnc_menu_cursor_reset)
    functions.cursor_set_position = cast("MenuTwoNumbers", fnc_menu_cursor_set_position)
    functions.cursor_fade_in = cast("MenuOneNumber", fnc_menu_cursor_fade_in)
    functions.cursor_hide = cast("MenuAction", fnc_menu_cursor_hide)
    functions.cursor_draw = cast("MenuAction", fnc_menu_cursor_draw)
    job_callback = cffi.cast("MenuJob", job)
    release_job_when_lua_closes = setmetatable({}, { __gc = release_job })
    return true
end

return title_menu
