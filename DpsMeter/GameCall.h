#pragma once
#include <cstdint>

namespace GameCall {
    uintptr_t Find(const uint8_t* Pattern, const char* Mask);
    uint32_t __stdcall Call(uintptr_t Function, uint32_t Eax, uint32_t Edx, uint32_t Ecx);
}
