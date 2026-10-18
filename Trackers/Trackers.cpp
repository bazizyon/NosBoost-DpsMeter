#define NOMINMAX
#include <windows.h>

#include "ModContract.h"
#include "Features.h"
#include "RaidHistory.h"
#include "TLBSWidget.h"
#include "TEWLabel.h"
#include "TEWEditWidget.h"
#include "TEWGuageWidget.h"
#include "TEWCustomPanelWidget.h"
#include "TEWGraphicButtonWidget.h"
#include "TNTIconWidget.h"

#include <filesystem>
#include <iterator>

namespace {
    constexpr ModClassRequirement Requirements[] = {
        {TLBSWidget::ClassName, TLBSWidget::Version, TLBSWidget::ExpectedSize},
        {TEWCustomPanelWidget::ClassName, TEWCustomPanelWidget::Version, TEWCustomPanelWidget::ExpectedSize},
        {TEWGraphicButtonWidget::ClassName, TEWGraphicButtonWidget::Version, TEWGraphicButtonWidget::ExpectedSize},
        {TEWGuageWidget::ClassName, TEWGuageWidget::Version, TEWGuageWidget::ExpectedSize},
        {TEWControlWidget::ClassName, TEWControlWidget::Version, TEWControlWidget::ExpectedSize},
        {TEWLabel::ClassName, TEWLabel::Version, TEWLabel::ExpectedSize},
        {TNTIconWidget::ClassName, TNTIconWidget::Version, TNTIconWidget::ExpectedSize},
        {TEWEditWidget::ClassName, TEWEditWidget::Version, TEWEditWidget::ExpectedSize},
    };

    const ModHost* ActiveHost = nullptr;

    void MoveOldFiles() {
        wchar_t Exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, Exe, MAX_PATH);
        const auto Mods = std::filesystem::path(Exe).parent_path() / L"mods";
        std::error_code Error;
        std::filesystem::create_directories(Mods / L"Trackers", Error);
        if (std::filesystem::exists(Mods / L"DpsMeter.ini", Error) && !std::filesystem::exists(Mods / L"Trackers" / L"DpsMeter.ini", Error)) {
            std::filesystem::rename(Mods / L"DpsMeter.ini", Mods / L"Trackers" / L"DpsMeter.ini", Error);
        }
    }

    void OnRawPacket(const char* Packet, const bool IsSend) {
        RaidHistory::OnRawPacket(Packet, IsSend);
        ExpSession::OnRawPacket(Packet, IsSend);
    }
}

extern "C" {
    __declspec(dllexport) const ModClassRequirement* ModGetRequirements(size_t* OutCount) {
        *OutCount = std::size(Requirements);
        return Requirements;
    }

    __declspec(dllexport) void ModStartup(ImGuiContext* Context, const ImGuiMemAllocFunc AllocFunc,
                                          const ImGuiMemFreeFunc FreeFunc, void* AllocUserData, const ModHost* Host) {
        ImGui::SetCurrentContext(Context);
        ImGui::SetAllocatorFunctions(AllocFunc, FreeFunc, AllocUserData);
        MoveOldFiles();
        DpsMeter::Startup(Host);
        ExpSession::Startup(Host);
        RaidHistory::Startup(Host);
        Host->SetRawPacketObserver(&OnRawPacket);
        ActiveHost = Host;
    }

    __declspec(dllexport) void ModShutdown() {
        if (ActiveHost) ActiveHost->SetRawPacketObserver(nullptr);
        ActiveHost = nullptr;
        RaidHistory::Shutdown();
        ExpSession::Shutdown();
        DpsMeter::Shutdown();
    }

    __declspec(dllexport) void ModEarlyTick(const TLBSWidget* RootWidget, const TickContext tickContext) {
        DpsMeter::EarlyTick(const_cast<TLBSWidget*>(RootWidget), tickContext);
    }

    __declspec(dllexport) void ModTick(const TLBSWidget* RootWidget, const TickContext tickContext) {
        ExpSession::Tick(const_cast<TLBSWidget*>(RootWidget), tickContext);
    }

    __declspec(dllexport) void ModToggleMainWindow() {
        DpsMeter::ToggleWindow();
    }
}
