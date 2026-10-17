#pragma once
#include "ModContract.h"

namespace RaidHistory {
    void Startup(const ModHost* Host);
    void Shutdown();
    void OnRawPacket(const char* Packet, bool IsSend);

    int16_t BuildPage(TLBSWidget* Page, int16_t Width);
    void ForgetPage();
    void TickPage(TLBSWidget* Root, const TickContext& Context);
}
