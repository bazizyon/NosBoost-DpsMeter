#define NOMINMAX
#include <windows.h>

#include "GameCall.h"

#include <cstring>

namespace GameCall {
    uintptr_t Find(const uint8_t* Pattern, const char* Mask) {
        const auto Base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
        const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(Base + Dos->e_lfanew);
        const IMAGE_SECTION_HEADER* Section = IMAGE_FIRST_SECTION(Nt);
        const size_t Length = std::strlen(Mask);
        for (WORD s = 0; s < Nt->FileHeader.NumberOfSections; s++, Section++) {
            if (!(Section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const auto* Start = reinterpret_cast<const uint8_t*>(Base + Section->VirtualAddress);
            const size_t Size = Section->Misc.VirtualSize;
            for (size_t i = 0; i + Length <= Size; i++) {
                size_t j = 0;
                while (j < Length && (Mask[j] == '?' || Start[i + j] == Pattern[j])) j++;
                if (j == Length) return reinterpret_cast<uintptr_t>(Start + i);
            }
        }
        return 0;
    }

    __declspec(naked) uint32_t __stdcall Call(uintptr_t Function, uint32_t Eax, uint32_t Edx, uint32_t Ecx) {
        __asm {
            push ebx
            mov eax, [esp + 12]
            mov edx, [esp + 16]
            mov ecx, [esp + 20]
            mov ebx, [esp + 8]
            call ebx
            pop ebx
            ret 16
        }
    }
}
