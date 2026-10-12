#ifndef KH1_NATIVE_H
#define KH1_NATIVE_H
/* Header for hook code that kh1_native compiles at load time with TinyCC.

   A mod ships a .c file under scripts/io_packages/ and installs it from Lua
   after VersionCheck has run:
       kh1_native.install_c("hooks/my_feature.c")
   The file includes only this header (there is no C library) and defines
       int install(void)
   which is called once after compiling; return nonzero on success.
   Compile errors and log lines go to kh1_native.log. */

#ifdef __TINYC__
typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long long uintptr_t;
typedef unsigned long long size_t;
#define NULL ((void*)0)
#else
#include <stddef.h>
#include <stdint.h>
#endif

typedef union KH1Xmm {
    uint8_t  u8[16];
    uint32_t u32[4];
    uint64_t u64[2];
    float    f32[4];
    double   f64[2];
} KH1Xmm;

/* Every register at a mid hook. Writes are applied when the hook returns;
   rip and rsp are read-only. */
typedef struct KH1Context {
    KH1Xmm   xmm[16];
    uint64_t rflags, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx, rax, rbp, rsp, trampoline_rsp, rip;
} KH1Context;

typedef void (*KH1MidHookFn)(KH1Context* ctx);

#ifdef __TINYC__
/* Absolute address of a name from the Steam/EGS globals files, 0 if unknown. */
uintptr_t kh1_symbol(const char* name);

/* Hooks are keyed by name; installing a name again is a no-op that succeeds.
   Inline: replaces the function at target; *original calls the unhooked code.
   Mid: calls fn before the instruction at address, with every register in ctx.
   Pointer: swaps a function pointer slot that must still hold expected. */
int kh1_hook_inline(const char* name, uintptr_t target, void* detour, void** original);
int kh1_hook_mid(const char* name, uintptr_t address, KH1MidHookFn fn);
int kh1_hook_pointer(const char* name, uintptr_t slot, void* expected, void* detour, void** original);

/* Same block Lua gets from kh1_native.persistent_block(key, size): zeroed on
   first use and kept for the life of the process, for state shared with Lua. */
void* kh1_persistent_block(const char* key, size_t size);

void kh1_log(const char* msg);

int   memcmp(const void* a, const void* b, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int value, size_t n);

#define KH1_FIELD(type, base, offset) (*(type*)((uintptr_t)(base) + (offset)))
#endif

#endif
