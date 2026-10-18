#pragma once
#include "ModContract.h"

#include <string>
#include <vector>

namespace GameHint {
    bool Create(const ModHost* Host, TLBSWidget* Root);
    void Destroy();
    void Show(const std::vector<std::wstring>& Lines, int32_t MouseX, int32_t MouseY, uint32_t FirstLineColor = 0xFFFFFFFF);
    void Hide();
}
