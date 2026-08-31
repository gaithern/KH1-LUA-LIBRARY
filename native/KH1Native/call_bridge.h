#pragma once

static const int MAX_CALL_ARGS = 6;

unsigned long long GetGameBase();

bool SafeCall(unsigned long long address, const unsigned long long* args, int argCount,
              unsigned long long& outResult);
