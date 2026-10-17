#pragma once
#include <cstdint>
#include <string>

namespace GameData {
    struct SpCard {
        int Vnum = 0;
        int16_t Icon = -1;
        std::wstring Name;
    };

    std::wstring RaidName(int RaidId);
    std::wstring SpName(int Morph);
    const SpCard* FindSp(int Morph);
    bool IsSp(int Morph);
    long PlayerId();
}
