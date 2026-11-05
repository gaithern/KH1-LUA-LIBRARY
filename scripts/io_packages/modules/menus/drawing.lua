---@diagnostic disable: undefined-global

local cffi = require("cffi")
local khscii = require("helpers.khscii")
local game = require("modules.menus.game")

local TEXT_FONT = 2
local NO_MESSAGE = 0
local LABEL_TEXT_INDENT = 0x10
local TEXT_STYLE_SIZE = 0x14
local TEXT_STYLE_COLOR = 0x10
local VALUE_STYLE_NORMAL_OFFSET = 0x10
local LABEL_STYLE_NORMAL = 0
local LABEL_STYLE_SELECTED = 0xB
local LEFT_BAR_X = -0x40
local LEFT_BAR_WIDTH = 0x4B
local RIGHT_BAR_WIDTH = 0x17
local HELP_TEXT_X = 0xAE
local HELP_TEXT_COLOR = 0x80C87850
local MAXIMUM_TEXT_LENGTH = 255

local drawing = {
    FIRST_ROW_Y = 0x47,
    ALIGN_LEFT = 0,
    ALIGN_CENTER = 2,
}

local text_buffers = {}

local function text_buffer(name, value)
    if text_buffers[name] == nil then
        text_buffers[name] = cffi.new("uint8_t[?]", MAXIMUM_TEXT_LENGTH + 1)
    end
    local buffer = text_buffers[name]
    local bytes = khscii.GetKHSCII(value)
    local length = math.min(#bytes, MAXIMUM_TEXT_LENGTH)
    for i = 1, length do
        buffer[i - 1] = bytes[i]
    end
    buffer[length] = 0
    return buffer
end

function drawing.row_height()
    return ReadInt(g_MenuRowHeight)
end

function drawing.row_spacing()
    return drawing.row_height() + ReadInt(g_MenuRowGap)
end

function drawing.row_y(row)
    return drawing.FIRST_ROW_Y + row * drawing.row_spacing()
end

function drawing.text_size()
    return ReadInt(g_MenuTextSize)
end

function drawing.text_y_in_row(y)
    return y + (drawing.row_height() - drawing.text_size()) // 2
end

function drawing.style_color(style)
    return ReadInt(g_MenuTextStyles + style * TEXT_STYLE_SIZE + TEXT_STYLE_COLOR)
end

function drawing.draw_string(window, alignment, x, y, name, value, size, color)
    return cffi.tonumber(game.draw_string(window, alignment, x, y, text_buffer(name, value), size, color, 0))
end

local function draw_text_in_pill(window, style, alignment, x, y, width, anchor, name, value)
    game.draw_text(window, style, TEXT_FONT, alignment, x, y, width, anchor, NO_MESSAGE)
    return drawing.draw_string(window, alignment, anchor, drawing.text_y_in_row(y), name, value, drawing.text_size(), drawing.style_color(style))
end

function drawing.draw_label(window, style, x, y, width, name, value)
    return draw_text_in_pill(window, style, drawing.ALIGN_LEFT, x, y, width, x + LABEL_TEXT_INDENT, name, value)
end

function drawing.draw_value(window, style, x, y, width, name, value)
    return draw_text_in_pill(window, style, drawing.ALIGN_CENTER, x, y, width, x + width // 2, name, value)
end

function drawing.draw_help_text(window, window_y, value)
    local y = ReadInt(g_HelpTextY) + window_y
    local size = cffi.tonumber(game.help_text_size(BASE_ADDR + g_HelpTextSizeObject))
    drawing.draw_string(window, drawing.ALIGN_LEFT, HELP_TEXT_X, y, "help", value, size, HELP_TEXT_COLOR)
end

local function draw_selection_bars(window, layout, y)
    local bar_y = y + drawing.row_height() // 2 - 1
    local label_end_x = layout.label_x + layout.label_width
    local value_end_x = layout.value_x + layout.value_width
    game.draw_selection_bar(window, LEFT_BAR_X, bar_y, LEFT_BAR_WIDTH)
    game.draw_selection_bar(window, label_end_x, bar_y, layout.value_x - label_end_x)
    game.draw_selection_bar(window, value_end_x, bar_y, RIGHT_BAR_WIDTH)
end

function drawing.draw_pair_row(window, layout, y, selected, name, label, value)
    local label_style = LABEL_STYLE_NORMAL
    local value_style = ReadInt(g_ConfigValueColors + VALUE_STYLE_NORMAL_OFFSET)
    if selected then
        label_style = LABEL_STYLE_SELECTED
        value_style = ReadInt(g_ConfigValueColors)
        draw_selection_bars(window, layout, y)
    end
    drawing.draw_label(window, label_style, layout.label_x, y, layout.label_width, name .. "_label", label)
    local value_end_x = drawing.draw_value(window, value_style, layout.value_x, y, layout.value_width, name .. "_value", value)
    return value_end_x, value_style
end

return drawing
