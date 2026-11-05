---@diagnostic disable: undefined-global

local game = require("modules.menus.game")
local text = require("modules.menus.text")
local drawing = require("modules.menus.drawing")
local windows = require("modules.menus.windows")

local LAYOUT = { label_x = 0xB, label_width = 0x118, value_x = 0x129, value_width = 0xC0 }
local VISIBLE_ROWS = 8
local MAXIMUM_NAME_LENGTH = 32
local MAXIMUM_VALUE_LENGTH = 20
local HELP_WINDOW_Y = 0x12C
local CURSOR_SPRITE_SPEED = 8
local PAD_PRESSED = 0x08
local PAD_REPEAT = 0x10
local PAD_UP = 0x10000010
local PAD_DOWN = 0x40000040
local PAD_PAGE_UP = 0x400
local PAD_PAGE_DOWN = 0x800

local list_view = {}

local descriptor = 0
local update_callback = nil
local rows = {}
local selected = 1
local first_visible = 1

local function shown_rows(source_rows)
    local shown = {}
    for _, row in ipairs(source_rows) do
        local name = tostring(row.name)
        local value = tostring(row.value)
        local help = row.help or (name .. ": " .. value)
        table.insert(shown, {
            name = text.with_placeholders(text.shortened(name, MAXIMUM_NAME_LENGTH)),
            value = text.with_placeholders(text.shortened(value, MAXIMUM_VALUE_LENGTH)),
            help = text.with_placeholders(text.wrapped(help)),
        })
    end
    return shown
end

local function select_row(new_selection)
    if new_selection ~= selected then
        selected = new_selection
        game.play_sound(game.SOUND_CURSOR)
    end
end

local function handle_input(window)
    local pad = ReadLong(g_MenuPadState)
    local pressed = ReadInt(pad + PAD_PRESSED, true)
    local repeated = ReadInt(pad + PAD_REPEAT, true)
    local count = #rows
    if pressed & ReadInt(g_MenuCancelMask) ~= 0 then
        game.close_window(window)
        game.play_sound(game.SOUND_CANCEL)
    elseif repeated & PAD_UP ~= 0 then
        if selected > 1 then
            select_row(selected - 1)
        elseif pressed & PAD_UP ~= 0 then
            select_row(count)
        end
    elseif repeated & PAD_DOWN ~= 0 then
        if selected < count then
            select_row(selected + 1)
        elseif pressed & PAD_DOWN ~= 0 then
            select_row(1)
        end
    elseif repeated & PAD_PAGE_UP ~= 0 then
        select_row(math.max(1, selected - VISIBLE_ROWS))
    elseif repeated & PAD_PAGE_DOWN ~= 0 then
        select_row(math.min(count, selected + VISIBLE_ROWS))
    end
end

local function scroll_to_selection()
    if selected < first_visible then
        first_visible = selected
    end
    if selected > first_visible + VISIBLE_ROWS - 1 then
        first_visible = selected - VISIBLE_ROWS + 1
    end
end

local function draw(window)
    local visible_rows = math.min(#rows, VISIBLE_ROWS)
    for screen_row = 0, visible_rows - 1 do
        local row_number = first_visible + screen_row
        local row = rows[row_number]
        drawing.draw_pair_row(window, LAYOUT, drawing.row_y(screen_row), row_number == selected, "list" .. screen_row, row.name, row.value)
    end
    drawing.draw_help_text(window, HELP_WINDOW_Y, rows[selected].help)
end

local function update(window)
    if #rows == 0 then
        return
    end
    local active = game.window_is_active(window)
    if active then
        handle_input(window)
    end
    scroll_to_selection()
    windows.set_cursor_row(window, selected - first_visible)
    if active then
        game.move_cursor_sprite(window, CURSOR_SPRITE_SPEED)
    end
    draw(window)
end

local function window_update(window_pointer)
    game.run_safely("menus.list_view", update, game.to_integer(window_pointer))
end

function list_view.open(parent, source_rows)
    rows = shown_rows(source_rows)
    selected = 1
    first_visible = 1
    windows.set_row_count(descriptor, math.min(#rows, VISIBLE_ROWS))
    local window = windows.open_child(parent, descriptor)
    if window ~= 0 then
        game.bring_to_front(window)
    end
end

function list_view.install(id)
    update_callback = game.make_callback(window_update)
    descriptor = windows.copy_descriptor(id .. "_list_view_descriptor", g_ConfigMenuDescriptor, update_callback)
    if descriptor == 0 then
        return false
    end
    windows.set_column_width(descriptor, LAYOUT.label_width)
    return true
end

return list_view
