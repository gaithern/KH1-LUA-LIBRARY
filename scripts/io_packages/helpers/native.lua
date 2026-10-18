---@diagnostic disable: undefined-global

-- Calls game functions and manages native memory through cffi.
-- Every function takes and returns plain Lua integers for addresses, like ReadLong/WriteLong.

local cffi = require("cffi")

cffi.cdef[[
    void* VirtualAlloc(void* address, size_t size, uint32_t allocation_type, uint32_t protect);
    int VirtualFree(void* address, size_t size, uint32_t free_type);
    void* CreateFileMappingA(void* file, void* attributes, uint32_t protect, uint32_t size_high, uint32_t size_low, const char* name);
    void* MapViewOfFile(void* mapping, uint32_t access, uint32_t offset_high, uint32_t offset_low, size_t size);
    uint32_t GetCurrentProcessId(void);
]]

local MEM_COMMIT_AND_RESERVE = 0x3000
local MEM_RELEASE = 0x8000
local PAGE_READWRITE = 0x04
local PAGE_EXECUTE_READWRITE = 0x40
local FILE_MAP_ALL_ACCESS = 0xF001F
local ALLOCATION_GRANULARITY = 0x10000
local NEAR_SEARCH_RANGE = 0x70000000
local SCRIPT_CONTEXT_SIZE = 4512
local SCRIPT_CONTEXT_STACK_INDEX = 404
local SCRIPT_CONTEXT_STACK = 408
local LOG_FILE_NAME = "kh1_lua_library.log"

local scratch_floats = nil
local script_context = nil
local log_path = nil

local function to_address(pointer)
    return cffi.tonumber(cffi.cast("uintptr_t", pointer))
end

local function function_type(argument_count)
    if argument_count == 0 then
        return "uint64_t (*)(void)"
    end
    local argument_types = {}
    for i = 1, argument_count do
        argument_types[i] = "uint64_t"
    end
    return "uint64_t (*)(" .. table.concat(argument_types, ", ") .. ")"
end

local function call_function(rva, ...)
    local arguments = {...}
    local game_function = cffi.cast(function_type(#arguments), BASE_ADDR + rva)
    local result = game_function(table.unpack(arguments))
    return true, cffi.tonumber(result)
end

local function call_evdl_syscall(rva, stack_values)
    if script_context == nil then
        script_context = cffi.new("uint8_t[?]", SCRIPT_CONTEXT_SIZE)
    end
    cffi.fill(script_context, SCRIPT_CONTEXT_SIZE, 0)
    local stack = cffi.cast("int32_t*", script_context + SCRIPT_CONTEXT_STACK)
    for i = 1, #stack_values do
        stack[i - 1] = stack_values[i]
    end
    local stack_index = cffi.cast("int32_t*", script_context + SCRIPT_CONTEXT_STACK_INDEX)
    if #stack_values > 0 then
        stack_index[0] = #stack_values - 1
    else
        stack_index[0] = 0
    end
    local syscall = cffi.cast("uint64_t (*)(void*)", BASE_ADDR + rva)
    local result = syscall(script_context)
    return true, cffi.tonumber(result)
end

local function write_floats(...)
    if scratch_floats == nil then
        scratch_floats = cffi.new("float[16]")
    end
    local values = {...}
    for i = 1, #values do
        scratch_floats[i - 1] = values[i]
    end
    return to_address(scratch_floats)
end

local function allocate(size, executable)
    local protect = PAGE_READWRITE
    if executable then
        protect = PAGE_EXECUTE_READWRITE
    end
    return to_address(cffi.C.VirtualAlloc(nil, size, MEM_COMMIT_AND_RESERVE, protect))
end

local function allocate_near(target, size)
    local offset = 0
    while offset < NEAR_SEARCH_RANGE do
        local candidates = { target + offset, target - offset }
        for i = 1, 2 do
            local address = candidates[i] - candidates[i] % ALLOCATION_GRANULARITY
            local block = cffi.C.VirtualAlloc(cffi.cast("void*", address), size, MEM_COMMIT_AND_RESERVE, PAGE_EXECUTE_READWRITE)
            if to_address(block) ~= 0 then
                return to_address(block)
            end
        end
        offset = offset + ALLOCATION_GRANULARITY
    end
    return 0
end

local function free(address)
    return cffi.C.VirtualFree(cffi.cast("void*", address), 0, MEM_RELEASE) ~= 0
end

local function copy_memory(destination, source, size)
    cffi.copy(cffi.cast("void*", destination), cffi.cast("void*", source), size)
    return true
end

local function persistent_slot(key)
    local name = "Local\\kh1_lua_library_" .. cffi.C.GetCurrentProcessId() .. "_" .. key
    local invalid_handle = cffi.cast("void*", -1)
    local mapping = cffi.C.CreateFileMappingA(invalid_handle, nil, PAGE_READWRITE, 0, 8, name)
    if to_address(mapping) == 0 then
        return 0
    end
    return to_address(cffi.C.MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 8))
end

local function persistent_block(key, size)
    local slot = persistent_slot(key)
    if slot == 0 then
        return 0
    end
    if ReadLong(slot, true) == 0 then
        WriteLong(slot, allocate(size, false), true)
    end
    return ReadLong(slot, true)
end

local function find_log_path()
    local source = debug.getinfo(1, "S").source
    local helpers_folder = string.match(source, "^@(.*)[/\\]helpers[/\\]native%.lua$")
    if helpers_folder == nil then
        return LOG_FILE_NAME
    end
    return helpers_folder .. "/" .. LOG_FILE_NAME
end

local function log_debug(message)
    if log_path == nil then
        log_path = find_log_path()
    end
    local file = io.open(log_path, "a")
    if file == nil then
        return
    end
    file:write("[" .. os.date("%H:%M:%S") .. "] " .. message .. "\n")
    file:close()
end

return {
    call_function = call_function,
    call_evdl_syscall = call_evdl_syscall,
    write_floats = write_floats,
    allocate = allocate,
    allocate_near = allocate_near,
    free = free,
    copy_memory = copy_memory,
    persistent_slot = persistent_slot,
    persistent_block = persistent_block,
    log_debug = log_debug,
}
