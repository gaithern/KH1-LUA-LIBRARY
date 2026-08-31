#include "pch.h"
#include <cstdio>
#include "log.h"
#include "call_bridge.h"

typedef unsigned long long(__fastcall* Func0)();
typedef unsigned long long(__fastcall* Func1)(unsigned long long);
typedef unsigned long long(__fastcall* Func2)(unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func3)(unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func4)(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func5)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);
typedef unsigned long long(__fastcall* Func6)(unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long, unsigned long long);

// Gets the base address for the running game version.
unsigned long long GetGameBase() {
    return (unsigned long long)GetModuleHandleA(nullptr);
}


// Safely calls an in game function and grabs the return value.
// Handles different functions with varying amounts of input
// arguments.  Has exception handling for logging and other
// purposes.
bool SafeCall(unsigned long long address, const unsigned long long* args, int argCount, unsigned long long& outResult) {
    DWORD exceptionCode = 0;
    ULONG_PTR exceptionAddr = 0;
    __try {
        switch (argCount) {
        case 0: outResult = ((Func0)address)(); break;
        case 1: outResult = ((Func1)address)(args[0]); break;
        case 2: outResult = ((Func2)address)(args[0], args[1]); break;
        case 3: outResult = ((Func3)address)(args[0], args[1], args[2]); break;
        case 4: outResult = ((Func4)address)(args[0], args[1], args[2], args[3]); break;
        case 5: outResult = ((Func5)address)(args[0], args[1], args[2], args[3], args[4]); break;
        default: outResult = ((Func6)address)(args[0], args[1], args[2], args[3], args[4], args[5]); break;
        }
        return true;
    } __except (
        exceptionCode = GetExceptionCode(),
        exceptionAddr = (ULONG_PTR)((EXCEPTION_POINTERS*)GetExceptionInformation())->ExceptionRecord->ExceptionAddress,
        EXCEPTION_EXECUTE_HANDLER) {
        unsigned long long base = GetGameBase();
        char msg[192];
        snprintf(msg, sizeof(msg),
            "SafeCall: exception 0x%08X at KH1FM.exe RVA=0x%llX (calling target RVA=0x%llX)",
            (unsigned)exceptionCode, (unsigned long long)exceptionAddr - base, address - base);
        LogDebug(msg);
        return false;
    }
}
