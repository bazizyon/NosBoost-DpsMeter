#pragma once
#include <cstdint>
#include <string>
#include <vector>

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
    std::wstring ItemName(int Vnum);
    int16_t ItemIcon(int Vnum);
    int16_t RaidIcon(int RaidId, bool Hardcore);
    const std::vector<int>& HardcoreRaids();

    struct RaidQuest {
        int Quest = 0;
        std::wstring Name;
        int RaidId = -1;
        int Count = 1;
    };
    const std::vector<RaidQuest>& DailyRaidQuests();

    constexpr uint32_t MinimapImageBase = 0x5551D000;
    std::vector<uint8_t> MinimapPng(int MapId);
}
