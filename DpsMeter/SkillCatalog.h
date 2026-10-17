#pragma once
#include <cstdint>
#include <string>

namespace SkillCatalog {
    struct Skill {
        int Vnum = 0;
        int16_t Icon = -1;
        std::wstring Name;
    };

    const Skill* Find(int Vnum);
    bool Load();
}
