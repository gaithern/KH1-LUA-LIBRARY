local kh1_native = require("kh1_native")

local M = {}

local CTRL_KEY  = "kh1_species_redirect_v2"
local CTRL_SIZE = 0x4000
local MAGIC     = 0x52454432

local OFF_MAGIC     = 0x00
local OFF_VERSION   = 0x04
local OFF_CAVE2     = 0x08
local OFF_COMP      = 0x10
local OFF_BLOB_BASE = 0x18
local OFF_REG_COUNT = 0x28
local OFF_DONE_FLAG = 0x30
local OFF_DONE_SIZE = 0x34
local OFF_DONE_DEST = 0x38
local ROWS_BASE  = 0x100
local ROW_STRIDE = 0x40
local ROW_BUFBASE = 0x00
local ROW_BUFEND  = 0x08
local ROW_MOTION  = 0x10
local ROW_ACTIVE  = 0x18
local ROW_TAG     = 0x1C
local MAX_ROWS = (CTRL_SIZE - ROWS_BASE) // ROW_STRIDE

local RVA = {
    hook2_entry  = 0x285db0,
    hook2_resume = 0x285dbe,
    blob_base    = 0xD2ADA0,
}

local ctrl = nil
local module_base = nil

local function abs(rva) return module_base + rva end
local function ci(off) return ReadInt(ctrl + off, true) end
local function wi(off, v) WriteInt(ctrl + off, v, true) end
local function wl(off, v) WriteLong(ctrl + off, v, true) end
local function row_addr(i) return ctrl + ROWS_BASE + i * ROW_STRIDE end

function M.claim(buf_base, buf_end, tag)
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
    WriteLong(r + ROW_MOTION, 0, true)
    WriteInt(r + ROW_TAG, tag or 0, true)
    WriteInt(r + ROW_ACTIVE, 1, true)
    return free
end

function M.release(row)
    if row then WriteInt(row_addr(row) + ROW_ACTIVE, 0, true) end
end

function M.set_motion(row, ptr)
    if row then WriteLong(row_addr(row) + ROW_MOTION, ptr, true) end
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
                motion = ReadLong(r + ROW_MOTION, true),
                tag = ReadInt(r + ROW_TAG, true),
            }
        end
    end
    return out
end

function M.completion_addr() return ReadLong(ctrl + OFF_COMP, true) end
function M.clear_done() wi(OFF_DONE_FLAG, 0) end
function M.poll_done()
    if ci(OFF_DONE_FLAG) == 0 then return nil end
    return ReadLong(ctrl + OFF_DONE_DEST, true), ReadInt(ctrl + OFF_DONE_SIZE, true) & 0xFFFFFFFF
end

local sc = string.char
local function jmp_abs(target)
    return sc(0xFF, 0x25, 0x00, 0x00, 0x00, 0x00) .. string.pack("<I8", target)
end

local function build_cave2()
    return
        sc(0x4C,0x8B,0xD1) ..                                   -- mov r10, rcx
        sc(0x48,0xB8) .. string.pack("<I8", ctrl) ..            -- mov rax, ctrl
        sc(0x44,0x8B,0x58,0x28) ..                              -- mov r11d, [rax+0x28]  (regCount)
        sc(0x45,0x85,0xDB) ..                                   -- test r11d, r11d
        sc(0x0F,0x84,0x3D,0x00,0x00,0x00) ..                    -- jz FALLBACK
        sc(0x4C,0x8D,0x80,0x00,0x01,0x00,0x00) ..               -- lea r8, [rax+0x100] (rows)
        sc(0x41,0x83,0x78,0x18,0x00) ..                         -- LOOP: cmp dword [r8+0x18], 0 (active)
        sc(0x0F,0x84,0x1E,0x00,0x00,0x00) ..                    -- jz NEXT
        sc(0x49,0x8B,0x00) ..                                   -- mov rax, [r8]      (bufBase)
        sc(0x49,0x39,0xC2) ..                                   -- cmp r10, rax
        sc(0x0F,0x82,0x12,0x00,0x00,0x00) ..                    -- jb NEXT
        sc(0x49,0x8B,0x40,0x08) ..                              -- mov rax, [r8+8]    (bufEnd)
        sc(0x49,0x39,0xC2) ..                                   -- cmp r10, rax
        sc(0x0F,0x83,0x05,0x00,0x00,0x00) ..                    -- jae NEXT
        sc(0x49,0x8B,0x40,0x10) ..                              -- mov rax, [r8+0x10] (motionPtr)
        sc(0xC3) ..                                             -- ret
        sc(0x49,0x83,0xC0,0x40) ..                              -- NEXT: add r8, 0x40
        sc(0x41,0xFF,0xCB) ..                                   -- dec r11d
        sc(0x0F,0x85,0xCA,0xFF,0xFF,0xFF) ..                    -- jnz LOOP
        sc(0x48,0xB8) .. string.pack("<I8", abs(RVA.blob_base)) .. -- FALLBACK: mov rax, blob_base
        sc(0x48,0x2B,0xC8) ..                                   -- sub rcx, rax
        sc(0x48,0xC1,0xE9,0x12) ..                              -- shr rcx, 0x12
        jmp_abs(abs(RVA.hook2_resume))                          -- jmp hook2_resume
end

local function build_completion()
    return
        sc(0x48,0xB8) .. string.pack("<I8", ctrl + OFF_DONE_DEST) .. sc(0x4C,0x89,0x00) ..  -- mov [dest], r8
        sc(0x48,0xB8) .. string.pack("<I8", ctrl + OFF_DONE_SIZE) .. sc(0x89,0x08) ..        -- mov [size], ecx
        sc(0x48,0xB8) .. string.pack("<I8", ctrl + OFF_DONE_FLAG) .. sc(0xC7,0x00,0x01,0x00,0x00,0x00) .. -- mov [flag], 1
        sc(0xC3)                                                                             -- ret
end

function M.is_installed()
    return ctrl ~= nil and ci(OFF_MAGIC) == MAGIC
end

function M.install()
    module_base = kh1_native.get_module_base()
    ctrl = kh1_native.persistent_block(CTRL_KEY, CTRL_SIZE)
    if ctrl == 0 or ctrl == nil then return false, "persistent_block failed" end
    if M.is_installed() then return true, "already installed" end

    local cave2 = kh1_native.allocate(0x200, 1)
    local comp = kh1_native.allocate(0x80, 1)
    if cave2 == 0 or comp == 0 then return false, "cave alloc failed" end

    wl(OFF_BLOB_BASE, abs(RVA.blob_base))
    wl(OFF_CAVE2, cave2)
    wl(OFF_COMP, comp)
    wi(OFF_REG_COUNT, 0)
    wi(OFF_DONE_FLAG, 0)

    kh1_native.write_bytes(cave2, build_cave2())
    kh1_native.write_bytes(comp, build_completion())
    kh1_native.patch_code(abs(RVA.hook2_entry), jmp_abs(cave2))

    wi(OFF_VERSION, 2)
    wi(OFF_MAGIC, MAGIC)
    return true, "installed"
end

return M
