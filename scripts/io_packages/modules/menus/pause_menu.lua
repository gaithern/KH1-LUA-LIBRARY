---@diagnostic disable: undefined-global

local call_redirect = require("helpers.call_redirect")
local game = require("modules.menus.game")
local drawing = require("modules.menus.drawing")

local NO_MESSAGE = 0
local CONFIG_ITEM = 6
local SAVE_MENU_ITEM = 7

local WINDOW_STATE = 0x48
local WINDOW_CLOSING_START_TIME = 0x4C
local WINDOW_ALPHA = 0xBC
local WINDOW_ANIMATION_TIME = 0xC0
local WINDOW_ROW_COUNT = 0xF8
local WINDOW_VISIBLE_ROWS = 0xFA
local WINDOW_CURSOR = 0xFE
local CURSOR_ROW_STEP = 0x10
local STATE_OPENING = 0
local STATE_ACTIVE = 1
local STATE_CLOSING = 2
local STATE_HIDDEN = 3
local FADE_FRAMES = 6
local FRAMES_BETWEEN_ROWS = 2

local ROW_X = 0xB
local ROW_WIDTH = 0x7A
local LABEL_STYLE_NORMAL = 0
local LABEL_STYLE_SELECTED = 1

local CALL_OPCODE = 0xE8
local CALL_LENGTH = 5
local MAIN_MENU_UPDATE_LENGTH = 0x985
local EXPECTED_ROW_DRAW_CALL_COUNT = 4

local pause_menu = {}

local row_label = ""
local row_help = ""
local header_descriptor = 0
local opener_address = 0
local callbacks = {}
local restore_when_lua_closes = nil

local inside_main_menu_update = false
local extra_row = -1
local config_row = -1
local extra_row_was_selected = false
local return_to_extra_row = false
local last_window = 0
local last_state = -1

local function row_draw_style(y, style)
    local row = (y - drawing.FIRST_ROW_Y) // drawing.row_spacing()
    if row == extra_row then
        return false, style
    end
    if row == config_row and ReadInt(g_MainMenuCursorRow) == extra_row then
        return true, BASE_ADDR + g_MenuTextStyles
    end
    return true, style
end

local function draw_row_filtered(window, font, alignment, x, y, width, anchor, style, message)
    local should_draw = true
    local new_style = style
    if inside_main_menu_update then
        local worked, filtered_should_draw, filtered_style = pcall(row_draw_style, y, style)
        if worked then
            should_draw = filtered_should_draw
            new_style = filtered_style
        end
    end
    if should_draw then
        game.draw_pill_text(window, font, alignment, x, y, width, anchor, new_style, message)
    end
end

local function find_item_row(item, item_count)
    for row = 0, item_count - 1 do
        if ReadInt(g_MainMenuItems + row * 4) == item then
            return row
        end
    end
    return -1
end

local function fade_in_alpha(time, full_alpha)
    local start = extra_row * FRAMES_BETWEEN_ROWS
    if time < start then
        return 0
    end
    if time - start < FADE_FRAMES then
        return math.floor((time - start) / FADE_FRAMES * full_alpha)
    end
    return full_alpha
end

local function fade_out_alpha(closing_time, row_count, full_alpha)
    local start = (row_count - extra_row - 1) * FRAMES_BETWEEN_ROWS
    if closing_time < start then
        return full_alpha
    end
    if closing_time - start < FADE_FRAMES then
        return math.floor((FADE_FRAMES - (closing_time - start)) / FADE_FRAMES * full_alpha)
    end
    return 0
end

local function extra_row_alpha(window, row_count)
    local state = ReadInt(window + WINDOW_STATE, true)
    local full_alpha = ReadInt(window + WINDOW_ALPHA, true)
    local time = ReadFloat(window + WINDOW_ANIMATION_TIME, true)
    if state == STATE_OPENING then
        return fade_in_alpha(time, full_alpha)
    end
    if state == STATE_CLOSING then
        local closing_time = time - ReadFloat(window + WINDOW_CLOSING_START_TIME, true)
        return fade_out_alpha(closing_time, row_count, full_alpha)
    end
    return full_alpha
end

local function draw_extra_row(window, row_count)
    if ReadInt(window + WINDOW_STATE, true) == STATE_HIDDEN then
        return
    end
    local style = LABEL_STYLE_NORMAL
    if game.cursor_row(window) == extra_row then
        style = LABEL_STYLE_SELECTED
    end
    local full_alpha = ReadInt(window + WINDOW_ALPHA, true)
    WriteInt(window + WINDOW_ALPHA, extra_row_alpha(window, row_count), true)
    drawing.draw_label(window, style, ROW_X, drawing.row_y(extra_row), ROW_WIDTH, "pause_menu_row", row_label)
    WriteInt(window + WINDOW_ALPHA, full_alpha, true)
end

local function move_cursor_back_to_extra_row(window, state_before)
    local menu_just_opened = state_before == STATE_OPENING and last_state ~= STATE_OPENING
    if window ~= last_window or menu_just_opened then
        last_window = window
        if extra_row_was_selected then
            return_to_extra_row = true
        end
    end
    if return_to_extra_row and (state_before == STATE_OPENING or state_before == STATE_ACTIVE) then
        WriteShort(window + WINDOW_CURSOR, extra_row * CURSOR_ROW_STEP, true)
        WriteInt(g_MainMenuCursorRow, extra_row)
        return_to_extra_row = false
    end
end

local function run_original_with_extra_row(window_pointer, cursor_on_extra_row)
    local extra_slot = g_MainMenuItems + extra_row * 4
    local config_descriptor_slot = g_MainMenuItemDescriptors + CONFIG_ITEM * 8
    local config_opener_slot = g_MainMenuItemOpeners + CONFIG_ITEM * 8
    local saved_extra_slot = ReadInt(extra_slot)
    local saved_config_descriptor = ReadLong(config_descriptor_slot)
    local saved_config_opener = ReadLong(config_opener_slot)
    WriteInt(extra_slot, CONFIG_ITEM)
    if cursor_on_extra_row then
        WriteLong(config_descriptor_slot, header_descriptor)
        WriteLong(config_opener_slot, opener_address)
    end
    inside_main_menu_update = true
    game.main_menu_update(window_pointer)
    inside_main_menu_update = false
    WriteInt(extra_slot, saved_extra_slot)
    WriteLong(config_descriptor_slot, saved_config_descriptor)
    WriteLong(config_opener_slot, saved_config_opener)
end

local function main_menu_update(window_pointer)
    local window = game.to_integer(window_pointer)
    local item_count = ReadInt(g_MainMenuItemCount)
    if find_item_row(SAVE_MENU_ITEM, item_count) ~= -1 then
        game.main_menu_update(window_pointer)
        return
    end
    local row_count = item_count + 1
    extra_row = item_count
    config_row = find_item_row(CONFIG_ITEM, item_count)
    WriteShort(window + WINDOW_ROW_COUNT, row_count, true)
    WriteShort(window + WINDOW_VISIBLE_ROWS, row_count, true)
    local state_before = ReadInt(window + WINDOW_STATE, true)
    move_cursor_back_to_extra_row(window, state_before)
    local cursor_on_extra_row = game.cursor_row(window) == extra_row

    run_original_with_extra_row(window_pointer, cursor_on_extra_row)

    local state_after = ReadInt(window + WINDOW_STATE, true)
    last_state = state_after
    if cursor_on_extra_row and state_before == STATE_ACTIVE and state_after == STATE_CLOSING then
        return_to_extra_row = true
    end
    if state_after == STATE_ACTIVE then
        extra_row_was_selected = game.cursor_row(window) == extra_row
    end
    game.run_safely("menus.pause_menu", draw_extra_row, window, row_count)
end

local function help_update(window_pointer)
    if ReadInt(g_MainMenuCursorRow) ~= ReadInt(g_MainMenuItemCount) then
        game.main_menu_help_update(window_pointer)
        return
    end
    local help_slot = g_MainMenuItemHelpMsgTable + CONFIG_ITEM * 4
    local saved_message = ReadInt(help_slot)
    WriteInt(help_slot, NO_MESSAGE)
    game.main_menu_help_update(window_pointer)
    WriteInt(help_slot, saved_message)
    game.run_safely("menus.pause_menu", drawing.draw_help_text, game.to_integer(window_pointer), 0, row_help)
end

local function find_row_draw_calls()
    local call_sites = {}
    for offset = 0, MAIN_MENU_UPDATE_LENGTH - CALL_LENGTH do
        local call_site = fnc_main_menu_update + offset
        if ReadByte(call_site) == CALL_OPCODE and call_redirect.call_target(call_site) == BASE_ADDR + fnc_menu_draw_pill_text then
            table.insert(call_sites, call_site)
        end
    end
    return call_sites
end

local function redirect_row_draw_calls()
    local call_sites = find_row_draw_calls()
    if #call_sites ~= EXPECTED_ROW_DRAW_CALL_COUNT then
        return false
    end
    for _, call_site in ipairs(call_sites) do
        if not call_redirect.redirect_call(call_site, fnc_menu_draw_pill_text, "MenuDrawPillText", draw_row_filtered) then
            return false
        end
    end
    return true
end

local function uninstall()
    WriteLong(g_MainMenuDescriptor, BASE_ADDR + fnc_main_menu_update)
    WriteLong(g_MainMenuHelpDescriptor, BASE_ADDR + fnc_main_menu_help_update)
end

function pause_menu.install(label, help, item_header_descriptor, item_opener_address)
    if g_MainMenuDescriptor == nil or g_MainMenuHelpDescriptor == nil then
        return false
    end
    row_label = label
    row_help = help
    header_descriptor = item_header_descriptor
    opener_address = item_opener_address
    if not redirect_row_draw_calls() then
        return false
    end
    callbacks.main_menu_update = game.make_callback(main_menu_update)
    callbacks.help_update = game.make_callback(help_update)
    WriteLong(g_MainMenuDescriptor, game.to_integer(callbacks.main_menu_update))
    WriteLong(g_MainMenuHelpDescriptor, game.to_integer(callbacks.help_update))
    restore_when_lua_closes = setmetatable({}, { __gc = uninstall })
    return true
end

return pause_menu
