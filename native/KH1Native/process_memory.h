#pragma once
#include <cstddef>

void* AllocateBlock(size_t size, bool executable);
bool FreeBlock(void* addr);

void* AllocateNear(void* target, size_t size);

void* PersistentBlock(const char* key, size_t keyLen, size_t size);

bool GuardedMemcpy(void* dst, const void* src, size_t len);

bool PatchCode(void* dest, const void* src, size_t len, bool suspendThreads);
