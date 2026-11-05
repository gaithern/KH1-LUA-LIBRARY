---@diagnostic disable: undefined-global

-- Ends a cutscene skip right before a gift popup opens, so the reward is shown.
-- Replaces the open window syscalls in g_EVSyscallTable with Lua functions through cffi.

local cffi = require("cffi")

cffi.cdef[[
    typedef uint64_t (*ShowSkippedRewardsSyscall)(void* thread);
    typedef uint64_t (*ShowSkippedRewardsEndSkip)(void);
]]

local SYSCALL_OPEN_WINDOW = 0x000
local SYSCALL_OPEN_WINDOW_NO_CLOSE = 0x0B1
local SYSCALL_RETRY_NEXT_FRAME = 4
local GIFT_POPUP_INSTRUCTION = 0x1800025D
local THREAD_CODE_OFFSET = 0x80
local THREAD_PC_OFFSET = 0x190

local enabled = false
local open_window_replacement = nil
local open_window_no_close_replacement = nil
local restore_when_lua_closes = nil

local function syscall_slot(syscall_number)
    return g_EVSyscallTable + syscall_number * 8
end

local function is_cutscene_skipping()
    local flags = ReadByte(inCutscene)
    if flags & 1 == 1 then
        return true
    end
    return false
end

local function is_gift_popup_coming(thread)
    local thread_address = cffi.tonumber(cffi.cast("uintptr_t", thread))
    local code_address = ReadLong(thread_address + THREAD_CODE_OFFSET, true)
    local pc = ReadInt(thread_address + THREAD_PC_OFFSET, true)
    local instruction = ReadInt(code_address + (pc + 3) * 4, true)
    if instruction == GIFT_POPUP_INSTRUCTION then
        return true
    end
    return false
end

local function end_event_skip()
    local end_skip = cffi.cast("ShowSkippedRewardsEndSkip", BASE_ADDR + fnc_end_event_skip)
    end_skip()
end

local function ended_skip_for_gift_popup(thread)
    if not is_cutscene_skipping() then
        return false
    end
    if not is_gift_popup_coming(thread) then
        return false
    end
    end_event_skip()
    if is_cutscene_skipping() then
        return false
    end
    return true
end

local function make_replacement(original_address)
    local original = cffi.cast("ShowSkippedRewardsSyscall", original_address)
    local function replacement(thread)
        local worked, ended_skip = pcall(ended_skip_for_gift_popup, thread)
        if worked and ended_skip then
            return SYSCALL_RETRY_NEXT_FRAME
        end
        return original(thread)
    end
    return cffi.cast("ShowSkippedRewardsSyscall", replacement)
end

local function point_slot_at(syscall_number, replacement)
    local replacement_address = cffi.tonumber(cffi.cast("uintptr_t", replacement))
    WriteLong(syscall_slot(syscall_number), replacement_address)
end

local function disable()
    if not enabled then
        return true
    end
    WriteLong(syscall_slot(SYSCALL_OPEN_WINDOW), BASE_ADDR + fnc_000_open_window)
    WriteLong(syscall_slot(SYSCALL_OPEN_WINDOW_NO_CLOSE), BASE_ADDR + fnc_0B1_open_window_no_close)
    enabled = false
    return true
end

local function enable()
    if enabled then
        return true
    end
    if g_EVSyscallTable == nil or inCutscene == nil or fnc_end_event_skip == nil
        or fnc_000_open_window == nil or fnc_0B1_open_window_no_close == nil then
        return false
    end
    open_window_replacement = make_replacement(BASE_ADDR + fnc_000_open_window)
    open_window_no_close_replacement = make_replacement(BASE_ADDR + fnc_0B1_open_window_no_close)
    point_slot_at(SYSCALL_OPEN_WINDOW, open_window_replacement)
    point_slot_at(SYSCALL_OPEN_WINDOW_NO_CLOSE, open_window_no_close_replacement)
    restore_when_lua_closes = setmetatable({}, { __gc = disable })
    enabled = true
    return true
end

local function set_show_skipped_rewards(turn_on)
    if turn_on then
        return enable()
    end
    return disable()
end

return {
    set_show_skipped_rewards = set_show_skipped_rewards,
}
