local MISSING_GLYPH_PLACEHOLDER = "{0x81}"
local SYMBOLS_WITH_GLYPHS = " -!?%/.,;:'()[]&+*~\""
local LINE_BREAK_CODE = 0x0A
local HELP_LINE_LENGTH = 42
local HELP_LINE_COUNT = 3

local text = {}

function text.character_codes(value)
    local codes = {}
    if utf8.len(value) == nil then
        for i = 1, #value do
            table.insert(codes, string.byte(value, i))
        end
        return codes
    end
    for _, code in utf8.codes(value) do
        table.insert(codes, code)
    end
    return codes
end

function text.character_count(value)
    return #text.character_codes(value)
end

function text.first_characters(value, count)
    local codes = text.character_codes(value)
    local shortened = ""
    for i = 1, math.min(count, #codes) do
        shortened = shortened .. utf8.char(codes[i])
    end
    return shortened
end

function text.shortened(value, maximum_length)
    if text.character_count(value) <= maximum_length then
        return value
    end
    return text.first_characters(value, maximum_length - 3) .. "..."
end

local function has_glyph(code)
    if code >= 0x80 then
        return false
    end
    local character = string.char(code)
    if string.find(character, "%w") ~= nil then
        return true
    end
    return string.find(SYMBOLS_WITH_GLYPHS, character, 1, true) ~= nil
end

function text.with_placeholders(value)
    local shown = ""
    for _, code in ipairs(text.character_codes(value)) do
        if code == LINE_BREAK_CODE then
            shown = shown .. "\n"
        elseif has_glyph(code) then
            shown = shown .. string.char(code)
        else
            shown = shown .. MISSING_GLYPH_PLACEHOLDER
        end
    end
    return shown
end

function text.wrapped(value)
    local lines = {}
    local line = ""
    for word in string.gmatch(value, "%S+") do
        if line == "" then
            line = word
        elseif text.character_count(line) + 1 + text.character_count(word) <= HELP_LINE_LENGTH then
            line = line .. " " .. word
        else
            table.insert(lines, line)
            line = word
        end
    end
    if line ~= "" then
        table.insert(lines, line)
    end
    while #lines > HELP_LINE_COUNT do
        table.remove(lines)
    end
    return table.concat(lines, "\n")
end

function text.from_utf16(units, count)
    local result = ""
    local index = 0
    while index < count do
        local code = units[index]
        if code >= 0xD800 and code <= 0xDBFF and index + 1 < count then
            local next_code = units[index + 1]
            if next_code >= 0xDC00 and next_code <= 0xDFFF then
                code = 0x10000 + (code - 0xD800) * 0x400 + (next_code - 0xDC00)
                index = index + 1
            end
        end
        if code < 0xD800 or code > 0xDFFF then
            result = result .. utf8.char(code)
        end
        index = index + 1
    end
    return result
end

return text
