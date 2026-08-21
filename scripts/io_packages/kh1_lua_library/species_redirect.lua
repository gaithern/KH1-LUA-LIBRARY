local kh1_native = require("kh1_native")

local M = {}

local CTRL_KEY = "kh1_species_redirect_v1"
local CTRL_SIZE = 0x2000
local MAGIC = 0x52454431

local OFF_MAGIC       = 0x00
local OFF_VERSION     = 0x04
local OFF_CAVE1       = 0x08
local OFF_CAVE2       = 0x10
local OFF_BLOB_BASE   = 0x18
local OFF_SPECIES_TAB = 0x20
local OFF_REG_COUNT   = 0x28
local ROWS_BASE  = 0x100
local ROW_STRIDE = 0x20
local ROW_BUFBASE = 0x00
local ROW_BUFEND  = 0x08
local ROW_SLOTN   = 0x10
local ROW_ACTIVE  = 0x14
local ROW_TAG     = 0x18
local MAX_ROWS = (CTRL_SIZE - ROWS_BASE) // ROW_STRIDE

local SLOT_STRIDE = 0x50
local SLOT_OWNER_OFF = 0x00
local FIRST_USABLE_SLOT = 0
local LAST_USABLE_SLOT = 49
local OWNER_FREE = 0xFF

local RVA = {
    hook1_splice = 0x2862d7,
    hook1_resume = 0x2862ef,
    hook2_entry  = 0x285db0,
    hook2_resume = 0x285dbe,
    blob_base    = 0xD2ADA0,
    species_tab  = 0x2869DD0,
}

local ctrl = nil
local module_base = nil

local function abs(rva) return module_base + rva end

local function ci(off) return ReadInt(ctrl + off, true) end
local function wi(off, v) WriteInt(ctrl + off, v, true) end
local function wl(off, v) WriteLong(ctrl + off, v, true) end
local function row_addr(i) return ctrl + ROWS_BASE + i * ROW_STRIDE end

local function find_free_slot()
    local owner_base = abs(RVA.species_tab)
    local claimed = {}
    local count = ci(OFF_REG_COUNT)
    for i = 0, count - 1 do
        local r = row_addr(i)
        if ReadInt(r + ROW_ACTIVE, true) == 1 then claimed[ReadInt(r + ROW_SLOTN, true)] = true end
    end
    for s = LAST_USABLE_SLOT, FIRST_USABLE_SLOT, -1 do
        if not claimed[s] and ReadByte(owner_base + s * SLOT_STRIDE + SLOT_OWNER_OFF, true) == OWNER_FREE then
            return s
        end
    end
    return nil
end
M.find_free_slot = find_free_slot

function M.claim(buf_base, buf_end, slot_n, tag)
    local count = ci(OFF_REG_COUNT)
    local free = nil
    for i = 0, count - 1 do
        if ReadInt(row_addr(i) + ROW_ACTIVE, true) == 0 then free = i break end
    end
    if not free then
        if count >= MAX_ROWS then return nil end
        free = count
        wi(OFF_REG_COUNT, count + 1)
    end
    local r = row_addr(free)
    WriteLong(r + ROW_BUFBASE, buf_base, true)
    WriteLong(r + ROW_BUFEND, buf_end, true)
    WriteInt(r + ROW_SLOTN, slot_n, true)
    WriteInt(r + ROW_TAG, tag or 0, true)
    WriteInt(r + ROW_ACTIVE, 1, true)
    return free
end

function M.release(row)
    if row then WriteInt(row_addr(row) + ROW_ACTIVE, 0, true) end
end

function M.set_row_slot(row, slot_n)
    if row then WriteInt(row_addr(row) + ROW_SLOTN, slot_n, true) end
end

function M.active_rows()
    local out = {}
    local count = ci(OFF_REG_COUNT)
    for i = 0, count - 1 do
        local r = row_addr(i)
        if ReadInt(r + ROW_ACTIVE, true) == 1 then
            out[#out + 1] = {
                row = i,
                buf_base = ReadLong(r + ROW_BUFBASE, true),
                buf_end = ReadLong(r + ROW_BUFEND, true),
                slot_n = ReadInt(r + ROW_SLOTN, true),
                tag = ReadInt(r + ROW_TAG, true),
            }
        end
    end
    return out
end

local sc = string.char
local function jmp_abs(target)
    return sc(0xFF,0x25,0x00,0x00,0x00,0x00) .. string.pack("<I8", target)
end
local function build_cave2_bytes()
    local body =
        sc(0x4C,0x8B,0xD1) ..
        sc(0x48,0xB8) .. string.pack("<I8", abs(RVA.blob_base)) ..
        sc(0x48,0x29,0xC1,0x48,0xC1,0xE9,0x12) ..
        sc(0x48,0xB8) .. string.pack("<I8", ctrl) ..
        sc(0x44,0x8B,0x58,0x28,0x45,0x85,0xDB,0x0F,0x84,0x3D,0x00,0x00,0x00) ..
        sc(0x4C,0x8D,0x80,0x00,0x01,0x00,0x00) ..
        sc(0x41,0x83,0x78,0x14,0x00,0x0F,0x84,0x22,0x00,0x00,0x00) ..
        sc(0x49,0x8B,0x00,0x49,0x39,0xC2,0x0F,0x82,0x16,0x00,0x00,0x00) ..
        sc(0x49,0x8B,0x40,0x08,0x49,0x39,0xC2,0x0F,0x83,0x09,0x00,0x00,0x00) ..
        sc(0x41,0x8B,0x48,0x10,0xE9,0x09,0x00,0x00,0x00) ..
        sc(0x49,0x83,0xC0,0x20,0x41,0xFF,0xCB,0x75,0xCA)
    return body .. sc(0xFF,0x25,0x00,0x00,0x00,0x00) .. string.pack("<I8", abs(RVA.hook2_resume))
end
local function build_cave1_bytes()
    local body =
        sc(0x48,0x8B,0xC3) ..
        sc(0x48,0xB9) .. string.pack("<I8", abs(RVA.blob_base)) ..
        sc(0x48,0x29,0xC8,0x48,0xC1,0xE8,0x12) ..
        sc(0x48,0xB9) .. string.pack("<I8", ctrl) ..
        sc(0x44,0x8B,0x59,0x28,0x45,0x85,0xDB,0x0F,0x84,0x3D,0x00,0x00,0x00) ..
        sc(0x4C,0x8D,0x81,0x00,0x01,0x00,0x00) ..
        sc(0x41,0x83,0x78,0x14,0x00,0x0F,0x84,0x22,0x00,0x00,0x00) ..
        sc(0x49,0x8B,0x08,0x48,0x39,0xCB,0x0F,0x82,0x16,0x00,0x00,0x00) ..
        sc(0x49,0x8B,0x48,0x08,0x48,0x39,0xCB,0x0F,0x83,0x09,0x00,0x00,0x00) ..
        sc(0x41,0x8B,0x40,0x10,0xE9,0x09,0x00,0x00,0x00) ..
        sc(0x49,0x83,0xC0,0x20,0x41,0xFF,0xCB,0x75,0xCA) ..
        sc(0x48,0xBA) .. string.pack("<I8", abs(RVA.species_tab))
    return body .. sc(0xFF,0x25,0x00,0x00,0x00,0x00) .. string.pack("<I8", abs(RVA.hook1_resume))
end

function M.is_installed()
    return ctrl ~= nil and ci(OFF_MAGIC) == MAGIC
end

function M.install()
    module_base = kh1_native.get_module_base()
    ctrl = kh1_native.persistent_block(CTRL_KEY, CTRL_SIZE)
    if ctrl == 0 or ctrl == nil then return false, "persistent_block failed" end

    if M.is_installed() then
        return true, "already installed"
    end

    local cave1 = kh1_native.allocate(0x400, 1)
    local cave2 = kh1_native.allocate(0x400, 1)
    if cave1 == 0 or cave2 == 0 then return false, "cave alloc failed" end

    local c1 = build_cave1_bytes()
    local c2 = build_cave2_bytes()
    kh1_native.write_bytes(cave1, c1)
    kh1_native.write_bytes(cave2, c2)

    wl(OFF_BLOB_BASE, abs(RVA.blob_base))
    wl(OFF_SPECIES_TAB, abs(RVA.species_tab))
    wl(OFF_CAVE1, cave1)
    wl(OFF_CAVE2, cave2)
    if ci(OFF_REG_COUNT) == 0 then wi(OFF_REG_COUNT, 0) end

    local h1 = abs(RVA.hook1_splice)
    local p1 = jmp_abs(cave1) .. string.rep("\x90", (RVA.hook1_resume - RVA.hook1_splice) - 14)
    kh1_native.patch_code(h1, p1)
    local h2 = abs(RVA.hook2_entry)
    local p2 = jmp_abs(cave2)
    kh1_native.patch_code(h2, p2)

    wi(OFF_VERSION, 1)
    wi(OFF_MAGIC, MAGIC)
    return true, "installed"
end

return M
