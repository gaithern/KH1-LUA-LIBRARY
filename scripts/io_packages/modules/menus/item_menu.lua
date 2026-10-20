---@diagnostic disable: undefined-global

local game = require("modules.menus.game")
local text = require("modules.menus.text")
local keyboard = require("modules.menus.keyboard")
local drawing = require("modules.menus.drawing")
local windows = require("modules.menus.windows")
local list_view = require("modules.menus.list_view")

local LAYOUT = { label_x = 0xB, label_width = 0x8E, value_x = 0x9F, value_width = 0x14A }
local HEADER_WIDTH = 0x7A
local HEADER_STYLE = 4
local BUTTON_STYLE_NORMAL = 0
local BUTTON_STYLE_SELECTED = 1
local HELP_WINDOW_Y = 0x12C
local CARET_BLINK_FRAMES = 20
local CURSOR_SPRITE_SPEED = 8
local PAD_PRESSED = 0x08
local PAD_LEFT_OR_RIGHT = 0xA00000A0
local MASK_CHARACTER = "*"
local EDITING_HELP = "Type, then press Enter. Ctrl+V pastes."
local TEXT_ROW_HELP = "Select a field and press confirm to type."
local EMPTY_LIST_TEXT = "Nothing to show."

local item_menu = {}

local title = ""
local rows = {}
local callbacks = {}
local list_descriptor = 0
local header_descriptor = 0
local header_window = 0
local return_row = 0
local notice_text = ""
local frame_count = 0

local function value_of(setting)
    if type(setting) == "function" then
        return setting()
    end
    return setting
end

local function toggle(row)
    row.set(not row.get())
    game.play_sound(game.SOUND_CURSOR)
end

local function open_list(row, row_index, list_window)
    local list_rows = value_of(row.rows) or {}
    if #list_rows == 0 then
        notice_text = row.empty_text or EMPTY_LIST_TEXT
        game.play_sound(game.SOUND_ERROR)
        return
    end
    return_row = row_index
    game.close_window(list_window)
    game.play_sound(game.SOUND_CONFIRM)
    list_view.open(list_window, list_rows)
end

local function press_button(row)
    notice_text = ""
    game.play_sound(game.SOUND_CONFIRM)
    row.on_confirm()
end

local function close_menu(list_window)
    if header_window ~= 0 then
        game.close_window(header_window)
    else
        game.close_window(list_window)
    end
    game.play_sound(game.SOUND_CANCEL)
end

local function confirm(row, row_index, list_window)
    if row.kind == "text" then
        keyboard.start_editing()
    elseif row.kind == "toggle" then
        toggle(row)
    elseif row.kind == "list" then
        open_list(row, row_index, list_window)
    elseif row.kind == "button" then
        press_button(row)
    end
end

local function handle_input(window)
    game.list_cursor_update(window)
    game.move_cursor_sprite(window, CURSOR_SPRITE_SPEED)
    local row_index = game.cursor_row(window)
    local row = rows[row_index + 1]
    local pressed = ReadInt(ReadLong(g_MenuPadState) + PAD_PRESSED, true)
    if pressed & ReadInt(g_MenuCancelMask) ~= 0 then
        close_menu(window)
    elseif row.kind == "toggle" and pressed & PAD_LEFT_OR_RIGHT ~= 0 then
        toggle(row)
    elseif pressed & ReadInt(g_MenuConfirmMask) ~= 0 then
        confirm(row, row_index, window)
    end
end

local function shown_value(row)
    if row.kind == "toggle" then
        if row.get() then
            return row.on_text or "On"
        end
        return row.off_text or "Off"
    end
    local value = row.get()
    if row.masked then
        return string.rep(MASK_CHARACTER, text.character_count(value))
    end
    return text.with_placeholders(value)
end

local function draw_row(window, row, row_index, selected)
    local y = drawing.row_y(row_index)
    local name = "row" .. row_index
    if row.kind ~= "text" and row.kind ~= "toggle" then
        local style = BUTTON_STYLE_NORMAL
        if selected then
            style = BUTTON_STYLE_SELECTED
        end
        drawing.draw_label(window, style, LAYOUT.label_x, y, LAYOUT.label_width, name, row.label)
        return
    end
    local text_end_x, value_style = drawing.draw_pair_row(window, LAYOUT, y, selected, name, row.label, shown_value(row))
    local caret_visible = (frame_count // CARET_BLINK_FRAMES) % 2 == 0
    if row.kind == "text" and selected and keyboard.is_editing() and caret_visible then
        drawing.draw_string(window, drawing.ALIGN_LEFT, text_end_x, drawing.text_y_in_row(y), "caret", "-", drawing.text_size(), drawing.style_color(value_style))
    end
end

local function help_text(row)
    if keyboard.is_editing() then
        return EDITING_HELP
    end
    if notice_text ~= "" then
        return notice_text
    end
    local help = value_of(row.help)
    if help == nil and row.kind == "text" then
        help = TEXT_ROW_HELP
    end
    return text.with_placeholders(text.wrapped(help or ""))
end

local function draw(window)
    local selected_index = game.cursor_row(window)
    for index, row in ipairs(rows) do
        draw_row(window, row, index - 1, index - 1 == selected_index)
    end
    drawing.draw_help_text(window, HELP_WINDOW_Y, help_text(rows[selected_index + 1]))
end

local function type_into_row(row)
    local value = row.get()
    local new_value = keyboard.type_into(value, row.max_length)
    if new_value ~= value then
        row.set(new_value)
    end
end

local function update_list(window)
    frame_count = frame_count + 1
    if game.window_is_active(window) then
        if keyboard.is_editing() then
            type_into_row(rows[game.cursor_row(window) + 1])
        else
            handle_input(window)
        end
    end
    draw(window)
end

local function list_update(window_pointer)
    keyboard.window_seen()
    game.run_safely("menus.item_menu", update_list, game.to_integer(window_pointer))
end

local function open_under_header(header, cursor_row)
    local window = windows.open_child(header, list_descriptor)
    if window == 0 then
        return
    end
    windows.set_cursor_row(window, cursor_row)
    game.bring_to_front(window)
    header_window = header
    keyboard.cancel_editing()
    notice_text = ""
end

local function update_header(header)
    if game.window_is_active(header) then
        open_under_header(header, return_row)
    end
    drawing.draw_label(header, HEADER_STYLE, 0, 0, HEADER_WIDTH, "header", title)
end

local function header_update(header_pointer)
    game.run_safely("menus.item_menu", update_header, game.to_integer(header_pointer))
end

local function opener(header_pointer)
    return_row = 0
    game.run_safely("menus.item_menu", open_under_header, game.to_integer(header_pointer), 0)
end

function item_menu.header_descriptor()
    return header_descriptor
end

function item_menu.opener_address()
    return game.to_integer(callbacks.opener)
end

function item_menu.install(definition)
    title = definition.title
    rows = definition.rows
    callbacks.list = game.make_callback(list_update)
    callbacks.header = game.make_callback(header_update)
    callbacks.opener = game.make_callback(opener)
    list_descriptor = windows.copy_descriptor(definition.id .. "_list_descriptor", g_ConfigMenuDescriptor, callbacks.list)
    header_descriptor = windows.copy_descriptor(definition.id .. "_header_descriptor", g_ConfigHeaderDescriptor, callbacks.header)
    if list_descriptor == 0 or header_descriptor == 0 then
        return false
    end
    windows.set_column_width(list_descriptor, LAYOUT.label_width)
    windows.set_row_count(list_descriptor, #rows)
    return true
end

return item_menu
