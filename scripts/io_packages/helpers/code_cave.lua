---@diagnostic disable: undefined-global

-- Shared code-cave + splice infrastructure for the text_boxes / prize_popup hooks.
-- Built on the kh1_native primitives allocate_near / write_bytes / patch_code.

local kh1_native = require("kh1_native")

local native_log = kh1_native.log_debug or function(_) end

local sc = string.char

-- Chunks: byte strings, {label="name"} markers, and 4-byte rel32 slots: {rel32="name"}
-- for stub-internal targets or {rel32=abs_addr} for absolute ones (base = cave address).
local function assemble(chunks, base)
    local labels, items, off = {}, {}, 0
    for _, c in ipairs(chunks) do
        if type(c) == "string" then
            items[#items + 1] = { bytes = c }
            off = off + #c
        elseif c.label then
            labels[c.label] = off
        else
            items[#items + 1] = { target = c.rel32, off = off }
            off = off + 4
        end
    end
    local out = {}
    for _, it in ipairs(items) do
        if it.bytes then
            out[#out + 1] = it.bytes
        elseif type(it.target) == "number" then
            out[#out + 1] = string.pack("<i4", it.target - (base + it.off + 4))
        else
            out[#out + 1] = string.pack("<i4", labels[it.target] - (it.off + 4))
        end
    end
    return table.concat(out)
end

-- Validates the window's original bytes, writes the stub to a nearby cave, and splices an
-- E9 jmp (NOP-padded) over the window with all other threads suspended. make_chunks receives
-- (original_bytes_string, cave_addr) and returns the chunk list for assemble().
local function install_splice(name, hook_addr, window, validate, make_chunks)
    local function log(msg) native_log("[" .. name .. "] " .. msg) end
    if ReadByte(hook_addr, true) == 0xE9 then return true end  -- already installed
    local orig = {}
    for i = 1, window do orig[i] = ReadByte(hook_addr + i - 1, true) end
    if not validate(orig) then
        log("unexpected original bytes at hook address, aborting")
        return false
    end
    local cave = kh1_native.allocate_near(hook_addr, 4096)
    if cave == 0 then
        log("failed to allocate a nearby code cave")
        return false
    end
    local ok, stub = pcall(assemble, make_chunks(sc(table.unpack(orig)), cave), cave)
    if not ok then
        log("stub assembly failed: " .. tostring(stub))
        return false
    end
    kh1_native.write_bytes(cave, stub)
    local patch = sc(0xE9) .. string.pack("<i4", cave - (hook_addr + 5)) .. string.rep(sc(0x90), window - 5)
    if not kh1_native.patch_code(hook_addr, patch, 1) then
        log("patch_code failed")
        return false
    end
    log("installed successfully")
    return true
end

return {
    sc = sc,
    assemble = assemble,
    install_splice = install_splice,
}
