#pragma once
#include <cstddef>
#include <cstdint>

static const size_t MAX_SYSCALL_STACK = 32;

bool CallEvdlSyscall(unsigned long long rva, const int32_t* stack, size_t len, unsigned long long& outResult);
