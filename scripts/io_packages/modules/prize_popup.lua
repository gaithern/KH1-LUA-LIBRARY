---@diagnostic disable: undefined-global

-- Prize pickup popup feature: custom-text code-cave hooks (ported byte-for-byte from the old
-- kh1_native C++) plus show_custom_item_popup, which forces a map-prize pickup box with our text.

local kh1_native = require("kh1_native")
local code_cave = require("helpers.code_cave")
local khscii = require("helpers.khscii")

local sc = code_cave.sc
local install_splice = code_cave.install_splice
local GetKHSCII = khscii.GetKHSCII

-- Persistent state: stub-embedded flag/prev/buffer addresses must stay stable across reloads.
local STATE_KEY  = "kh1_popup_state_v1"
local STATE_SIZE = 0x220
local OFF_FLAG = 0x00   -- active flag byte (the stub clears it when the popup closes)
local OFF_PREV = 0x04   -- previous popup state dword (completion hook 1->0 detector)
local OFF_BUF  = 0x20   -- 512-byte text buffer
local BUF_MAX  = 511

local function state_block()
    local blk = kh1_native.persistent_block(STATE_KEY, STATE_SIZE)
    if blk == 0 or blk == nil then return nil end
    return blk
end

-- ######################## --
-- # Hook stubs           # --
-- ######################## --

-- Swap the rdi text pointer for our buffer while the flag is set.
local function popup_text_chunks(flag, buf, resume_abs, call_target_abs)
    return function(orig, cave)
        return {
            sc(0x50),                                   -- push rax
            sc(0x48, 0xB8) .. string.pack("<I8", flag), -- mov rax, flagAddr
            sc(0x80, 0x38, 0x01),                       -- cmp byte [rax],1
            sc(0x0F, 0x85), {rel32 = "use_orig"},       -- jne use_orig
            sc(0x48, 0xBF) .. string.pack("<I8", buf),  -- mov rdi, bufAddr
            sc(0x58),                                   -- pop rax
            sc(0xE9), {rel32 = "continue_call"},        -- jmp continue_call
            {label = "use_orig"},
            sc(0x58),                                   -- pop rax
            sc(0x48, 0x8B, 0xF8),                       -- mov rdi, rax
            {label = "continue_call"},
            sc(0xE8), {rel32 = call_target_abs},        -- call original target
            sc(0xE9), {rel32 = resume_abs},             -- jmp resume
        }
    end
end

-- Watch the state dword and clear the custom-text flag on the 1->0 (closed) transition.
local function popup_completion_chunks(flag, prev, state_abs, resume_abs)
    return function(orig, cave)
        return {
            sc(0x50),                                        -- push rax
            sc(0x41, 0x52),                                  -- push r10
            sc(0x41, 0x53),                                  -- push r11
            sc(0x49, 0xBA) .. string.pack("<I8", state_abs), -- mov r10, stateAddr
            sc(0x41, 0x8B, 0x02),                            -- mov eax, [r10]
            sc(0x49, 0xBB) .. string.pack("<I8", prev),      -- mov r11, prevAddr
            sc(0x45, 0x8B, 0x13),                            -- mov r10d, [r11]
            sc(0x41, 0x83, 0xFA, 0x00),                      -- cmp r10d, 0
            sc(0x0F, 0x84), {rel32 = "skip_clear"},          -- je skip_clear
            sc(0x83, 0xF8, 0x00),                            -- cmp eax, 0
            sc(0x0F, 0x85), {rel32 = "skip_clear"},          -- jne skip_clear
            sc(0x49, 0xBA) .. string.pack("<I8", flag),      -- mov r10, flagAddr
            sc(0x41, 0xC6, 0x02, 0x00),                      -- mov byte [r10], 0
            {label = "skip_clear"},
            sc(0x49, 0xBA) .. string.pack("<I8", prev),      -- mov r10, prevAddr
            sc(0x41, 0x89, 0x02),                            -- mov [r10], eax
            sc(0x41, 0x5B),                                  -- pop r11
            sc(0x41, 0x5A),                                  -- pop r10
            sc(0x58),                                        -- pop rax
            sc(0x48, 0x83, 0xEC, 0x28),                      -- replay sub rsp, 0x28
            sc(0x33, 0xD2),                                  -- replay xor edx, edx
            sc(0xE9), {rel32 = resume_abs},                  -- jmp resume
        }
    end
end

local function install_popup_text_hook(hook_rva, resume_rva, call_target_rva)
    local base = kh1_native.get_module_base()
    local blk = state_block()
    if not blk then return false end
    return install_splice("InstallPopupTextHook", base + hook_rva, 8,
        function(o)
            return o[1] == 0x48 and o[4] == 0xE8
                and ((o[2] == 0x8B and o[3] == 0xF8) or (o[2] == 0x89 and o[3] == 0xC7))
        end,
        popup_text_chunks(blk + OFF_FLAG, blk + OFF_BUF, base + resume_rva, base + call_target_rva))
end

local function install_popup_completion_hook(tick_rva, resume_rva, state_rva)
    local base = kh1_native.get_module_base()
    local blk = state_block()
    if not blk then return false end
    return install_splice("InstallPopupCompletionHook", base + tick_rva, 6,
        function(o)
            return o[1] == 0x48 and o[2] == 0x83 and o[3] == 0xEC and o[4] == 0x28
                and o[5] == 0x33 and o[6] == 0xD2
        end,
        popup_completion_chunks(blk + OFF_FLAG, blk + OFF_PREV, base + state_rva, base + resume_rva))
end

local function set_custom_popup_text(text)
    local blk = state_block()
    if not blk then return end
    local khs = GetKHSCII(text)
    local n = math.min(#khs, BUF_MAX)
    local bytes = {}
    for i = 1, n do bytes[i] = khs[i] end
    bytes[n + 1] = 0
    WriteArray(blk + OFF_BUF, bytes, true)
    WriteByte(blk + OFF_FLAG, 1, true)
end

-- ######################## --
-- # Public                # --
-- ######################## --

local function show_custom_item_popup(text)
    --[[ Uses the in game function to force a map prize pickup box, with
    injected bytes/memory so the text can be custom.]]
    install_popup_text_hook(fnc_item_popup_text_hook, fnc_item_popup_text_resume, fnc_item_popup_text_call_target)
    install_popup_completion_hook(fnc_item_popup_tick, fnc_item_popup_tick_resume, g_item_popup_state)
    set_custom_popup_text(text)
    return kh1_native.call_function(fnc_show_item_message, 1, 1)
end

return {
    show_custom_item_popup = show_custom_item_popup,
    -- exposed for offline stub verification only
    _test = {
        popup_text_chunks = popup_text_chunks,
        popup_completion_chunks = popup_completion_chunks,
    },
}
