#pragma once
#include "ModContract.h"

namespace DpsMeter {
    void Startup(const ModHost* Host);
    void Shutdown();
    void EarlyTick(TLBSWidget* Root, TickContext tickContext);
    void ToggleWindow();
}

namespace ExpSession {
    void Startup(const ModHost* Host);
    void Shutdown();
    void Tick(TLBSWidget* Root, TickContext tickContext);
    void OnRawPacket(const char* Packet, bool IsSend);

    int16_t BuildPage(TLBSWidget* Page, int16_t Width);
    void ForgetPage();
    void TickPage(TLBSWidget* Root, const TickContext& Context);
    void ResetView();
}
