#include "ModContract.h"
#include "Features.h"
#include "TEWLabel.h"
#include "LevPacket.h"
#include <array>
#include <execution>
#include <unordered_map>
#include "TEWGuageWidget.h"
#include <chrono>
#include "CInfoPacket.h"
#include "SuPacket.h"
#include "TEWGraphicButtonWidget.h"
#include "TNTIconWidget.h"
#include "Records.h"
#include "GameData.h"
#include <cstring>
#include <filesystem>
#include "WidgetKit.h"
#include <algorithm>
#include <format>
#include <vector>

// EXP Session Tracker Mod
// Tracks EXP gained, loot collected (gold and items)
// Tracks damage done by party members, amount of consumables used.
namespace {
    struct PlayerInfo {
        std::wstring playerName;
        int damage{};
    };
    enum SessionState {
        STOPPED,
        RUNNING,
        PAUSED
    };
    const ModHost* CachedHost = nullptr;
    constexpr ModClassRequirement Requirements[] = {
        {TNTIconWidget::ClassName, TNTIconWidget::Version, TNTIconWidget::ExpectedSize},
        {TEWGraphicButtonWidget::ClassName, TEWGraphicButtonWidget::Version, TEWGraphicButtonWidget::ExpectedSize},
        {TEWGuageWidget::ClassName, TEWGuageWidget::Version, TEWGuageWidget::ExpectedSize},
        {TEWLabel::ClassName, TEWLabel::Version, TEWLabel::ExpectedSize},

    };
    wchar_t windowTitle[] = L"EXP Session Tracker";
    TEWCustomPanelWidget* sessionTracker;

    TEWGraphicButtonWidget* toggleSessionButton;
    TEWLabel* toggleSessionLabel;

    TEWGraphicButtonWidget* resetSessionButton;

    TEWLabel* timerText;
    TEWLabel* expText;
    TEWLabel* axpText;

    TEWLabel* healthPotText;
    TEWLabel* manaPotText;
    TEWLabel* petGAText;
    TEWLabel* partnerGAText;

    TLBSWidget* player1Container;
    TEWGuageWidget* player1Guage;
    TEWLabel* player1Name;
    TEWLabel* player1DMG;

    TLBSWidget* player2Container;
    TEWGuageWidget* player2Guage;
    TEWLabel* player2Name;
    TEWLabel* player2DMG;

    TLBSWidget* player3Container;
    TEWGuageWidget* player3Guage;
    TEWLabel* player3Name;
    TEWLabel* player3DMG;

    SessionState currentSessionState = STOPPED;
    std::unordered_map<int, PlayerInfo>partyDamages;
    int localPlayerID;
    std::wstring localPlayerName;

    int64_t startSeconds;
    int64_t savedSeconds;

    float currentEXP;
    float startEXP;
    float savedEXP;

    float currentAXP;
    float startAXP;
    float savedAXP;

    bool tempInvisible = false;

    struct ExpRecord {
        int64_t Start = 0;
        int64_t End = 0;
        int64_t ActiveMs = 0;
        double LevelFrom = 0;
        double LevelGained = 0;
        double HeroFrom = 0;
        double HeroGained = 0;
        int64_t Damage = 0;
        int Kills = 0;
        std::vector<std::wstring> Maps;
        std::vector<int> Morphs;
    };

    std::vector<ExpRecord> Sessions;
    ExpRecord Current;
    bool HasCurrent = false;
    int SelfMorph = -1;
    std::wstring CurrentMap;
    int64_t LastMapRead = 0;
    int Revision = 0;

    std::filesystem::path HistoryFile() { return Records::DataDir() / L"exp.jsonl"; }
    std::filesystem::path ActiveFile() { return Records::DataDir() / L"exp_active.json"; }

    std::string ToJson(const ExpRecord& R) {
        std::string J = "{\"start\":" + std::to_string(R.Start) + ",\"end\":" + std::to_string(R.End)
            + ",\"activeMs\":" + std::to_string(R.ActiveMs) + ",\"levelFrom\":" + std::format("{:.6f}", R.LevelFrom)
            + ",\"levelGained\":" + std::format("{:.6f}", R.LevelGained) + ",\"heroFrom\":" + std::format("{:.6f}", R.HeroFrom)
            + ",\"heroGained\":" + std::format("{:.6f}", R.HeroGained) + ",\"damage\":" + std::to_string(R.Damage)
            + ",\"kills\":" + std::to_string(R.Kills) + ",\"maps\":[";
        for (size_t i = 0; i < R.Maps.size(); i++) {
            if (i) J += ",";
            Records::Escape(J, Records::Utf8(R.Maps[i]));
        }
        J += "],\"morphs\":[";
        for (size_t i = 0; i < R.Morphs.size(); i++) J += (i ? "," : "") + std::to_string(R.Morphs[i]);
        return J + "]}";
    }

    bool FromJson(const std::string& Line, ExpRecord& R) {
        Records::Json J;
        if (!Records::ParseJson(Line, J) || J.Type != Records::Json::Object) return false;
        R = {};
        R.Start = J.Int("start");
        R.End = J.Int("end");
        R.ActiveMs = J.Int("activeMs");
        R.LevelFrom = J.Real("levelFrom");
        R.LevelGained = J.Real("levelGained");
        R.HeroFrom = J.Real("heroFrom");
        R.HeroGained = J.Real("heroGained");
        R.Damage = J.Int("damage");
        R.Kills = static_cast<int>(J.Int("kills"));
        if (const Records::Json* Maps = J["maps"]) {
            for (const Records::Json& Map : Maps->Items) {
                if (Map.Type == Records::Json::String) R.Maps.push_back(Records::FromUtf8(Map.S));
            }
        }
        if (const Records::Json* Morphs = J["morphs"]) {
            for (const Records::Json& Morph : Morphs->Items) R.Morphs.push_back(static_cast<int>(Morph.N));
        }
        return true;
    }

    void Note(std::vector<int>& Seen, const int Value) {
        if (Value > 0 && std::find(Seen.begin(), Seen.end(), Value) == Seen.end()) Seen.push_back(Value);
    }

    void NoteMap(std::vector<std::wstring>& Seen, const std::wstring& Map) {
        if (!Map.empty() && std::find(Seen.begin(), Seen.end(), Map) == Seen.end()) Seen.push_back(Map);
    }

    int64_t SteadySeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    ExpRecord Snapshot() {
        ExpRecord R = Current;
        int64_t Seconds = savedSeconds;
        double Level = savedEXP;
        double Hero = savedAXP;
        if (currentSessionState == RUNNING) {
            Seconds += SteadySeconds() - startSeconds;
            Level += currentEXP - startEXP;
            Hero += currentAXP - startAXP;
        }
        R.ActiveMs = Seconds * 1000;
        R.LevelGained = Level;
        R.HeroGained = Hero;
        R.End = Records::NowMs();
        return R;
    }

    __declspec(naked) void HideWindow() {
        __asm {
            mov byte ptr [eax + 0x18], 0
            ret
        }
    }

    void StyleGraphicButton(TEWGraphicButtonWidget* graphicButton) {
        delete[] graphicButton->imageData.atlasFrames;
        graphicButton->imageData = ImageData(
            9,
            1593835585,
            512,
            512,
            new AtlasFrame[] {
                AtlasFrame(419, 308, 5, 24),
                AtlasFrame(424, 308, 10, 24),
                AtlasFrame(409, 308, 10, 24),
                AtlasFrame(445, 308, 5, 24),
                AtlasFrame(450, 308, 10, 24),
                AtlasFrame(435, 308, 10, 24),
                AtlasFrame(471, 308, 5, 24),
                AtlasFrame(476, 308, 10, 24),
                AtlasFrame(461, 308, 10, 24)
            });
        graphicButton->nineSliceInfo = {
            72,
            25,
            82,
            25,
            10,
            0,
            10,
            0
        };
        graphicButton->sliceCount = 3;
    }
    void AddLabelToWindow(TEWCustomPanelWidget* window, const wchar_t* windowName) {
        const auto windowLabel = Widget::Create<TEWLabel>(CachedHost);
        windowLabel->SetText(windowName);
        windowLabel->rect = Rect(12, 11, 200, 30);
        windowLabel->textColor = Color(255, 255, 255, 255);
        windowLabel->shadowColor = Color(255, 0, 0, 0);
        windowLabel->textAlignment = 1;
        windowLabel->fontStyle = 3;
        windowLabel->shadowStyle = 255;
        window->childrenList->push_back(windowLabel);
        windowLabel->parent = window;
    }
    void AddCloseButtonToWindow(TEWCustomPanelWidget* window) {
        const auto closeButton = Widget::Create<TEWGraphicButtonWidget>(CachedHost);
        closeButton->parent = window;
        closeButton->rect.right = 20;
        closeButton->rect.bottom = 20;
        closeButton->MoveTo(window->rect.right - 27, 8);
        closeButton->imageData.frameCount = 3;
        closeButton->imageData.imageName = 1593835585;
        delete[] closeButton->imageData.atlasFrames;
        closeButton->imageData.atlasFrames = new AtlasFrame[3];
        closeButton->imageData.atlasFrames[0] = AtlasFrame(244, 2, 20, 20);
        closeButton->imageData.atlasFrames[1] = AtlasFrame(264, 2, 20, 20);
        closeButton->imageData.atlasFrames[2] = AtlasFrame(244, 22, 20, 20);
        closeButton->drawMode = 0;
        closeButton->callbackFunction = reinterpret_cast<uint32_t>(HideWindow);
        closeButton->callbackArgument = reinterpret_cast<uint32_t>(window);
        window->childrenList->push_back(closeButton);
    }
    void MakeWindow(
        TEWCustomPanelWidget* widget,
        const uint16_t windowWidth,
        const uint16_t windowHeight,
        const wchar_t* windowName,
        const bool hasCloseButton)
    {
        widget->rect.right = widget->rect.left + windowWidth;
        widget->rect.bottom = widget->rect.top + windowHeight;
        delete[] widget->imageData.atlasFrames;
        widget->imageData = ImageData(
            9,
            1593835577,
            512,
            512,
            new AtlasFrame[] {
                AtlasFrame(59, 182, 40, 49),
                AtlasFrame( 1, 124, 58, 58),
                AtlasFrame(59, 124, 40, 58),
                AtlasFrame(99, 124, 14, 58),
                AtlasFrame(99, 182, 14, 49),
                AtlasFrame(99, 231, 14, 14),
                AtlasFrame(59, 231, 40, 14),
                AtlasFrame( 1, 231, 58, 14),
                AtlasFrame( 1, 182, 58, 49),
            });
        const uint16_t widthMiddle = windowWidth - 58 - 14;
        const uint16_t heightMiddle = windowHeight - 58 - 14;
        const uint16_t posRight = 58 + widthMiddle;
        const uint16_t posBot = 58 + heightMiddle;
        widget->nineSliceInfo = {
            widthMiddle,
            heightMiddle,
            posRight,
            posBot,
            58,
            58,
            14,
            14
        };
        widget->sliceCount = 1;
        widget->drawMode = 5;
        widget->isMoveable = true;

        widget->childrenList->clear();
        AddLabelToWindow(widget, windowName);
        if (hasCloseButton) {
            AddCloseButtonToWindow(widget);
        }
    }

    std::wstring FormatDuration(const int64_t seconds) {
        const int64_t h = seconds / 3600;
        const int64_t m = (seconds % 3600) / 60;
        const int64_t s = seconds % 60;
        if (h > 0) return std::format(L"{}h{}m{}s", h, m, s);
        if (m > 0) return std::format(L"{}m{}s", m, s);
        return std::format(L"{}s", s);
    }
    std::wstring CompactDamageValue(int64_t totalDamage) {
        if (totalDamage < 1'000) {
            return std::to_wstring(totalDamage);
        }
        double damageInDouble = totalDamage;
        if (totalDamage < 10'000) {
            damageInDouble /= 1000.0;
            return std::format(L"{:.2f}K", damageInDouble);
        }
        if (totalDamage < 1'000'000) {
            totalDamage /= 1000;
            return std::to_wstring(totalDamage) + L"K";
        }
        if (totalDamage < 10'000'000) {
            damageInDouble /= 1000000.0;
            return std::format(L"{:.2f}M", damageInDouble);
        }
        if (totalDamage < 1'000'000'000) {
            totalDamage /= 1000000;
            return std::to_wstring(totalDamage) + L"M";
        }
        if (totalDamage < 10'000'000'000LL) {
            damageInDouble /= 1000000000.0;
            return std::format(L"{:.1f}B", damageInDouble);
        }
        if (totalDamage < 1'000'000'000'000LL) {
            const int64_t roundedDamage = totalDamage / 1000000000;
            return std::to_wstring(roundedDamage) + L"B";
        }
        if (totalDamage < 10'000'000'000'000LL) {
            damageInDouble /= 1000000000000.0;
            return std::format(L"{:.1f}T", damageInDouble);
        }
        if (totalDamage < 1'000'000'000'000'000LL) {
            const int64_t roundedDamage = totalDamage / 1000000000000;
            return std::to_wstring(roundedDamage) + L"T";
        }
        return L"TOO LARGE.";
    }
    void UpdateDamageDisplay() {
        std::vector<std::pair<int, PlayerInfo>> sortedDamages(partyDamages.begin(), partyDamages.end());
        if (sortedDamages.empty()) {
            return;
        }
        std::ranges::sort(sortedDamages, [](const auto& a, const auto& b) {
            return a.second.damage > b.second.damage;
        });

        const std::array<std::tuple<TLBSWidget*, TEWLabel*, TEWLabel*, TEWGuageWidget*>, 3> slots = {{
            {player1Container, player1Name, player1DMG, player1Guage},
            {player2Container, player2Name, player2DMG, player2Guage},
            {player3Container, player3Name, player3DMG, player3Guage},
        }};

        for (auto& [container, nameLabel, dmgLabel, gauge] : slots) {
            container->isVisible = false;
        }

        const int topDamage = sortedDamages[0].second.damage;

        for (int i = 0; i < (std::min)(static_cast<int>(sortedDamages.size()), 3); i++) {
            const auto& [id, info] = sortedDamages[i];
            auto& [container, nameLabel, dmgLabel, gauge] = slots[i];

            container->isVisible = true;

            gauge->fillPercent = static_cast<float>(info.damage) / static_cast<float>(topDamage);

            nameLabel->SetText(info.playerName.c_str());
            dmgLabel->SetText(CompactDamageValue(info.damage).c_str());
        }
    }
    void ResetSessionImpl(const TEWGraphicButtonWidget* resetButton) {
        if (HasCurrent) {
            const ExpRecord Done = Snapshot();
            if (Done.ActiveMs > 0 || Done.LevelGained != 0 || Done.HeroGained != 0) {
                Records::AppendLine(HistoryFile(), ToJson(Done));
                Sessions.push_back(Done);
            }
            HasCurrent = false;
            std::error_code Error;
            std::filesystem::remove(ActiveFile(), Error);
            Revision++;
        }
        partyDamages.clear();
        partyDamages[localPlayerID] = {localPlayerName, 0};
        savedSeconds = 0;
        savedEXP = 0;
        savedAXP = 0;
        timerText->SetText(L"0s");
        currentSessionState = STOPPED;
        toggleSessionLabel->SetText(L"Start");
        toggleSessionButton->color = Color(255, 0, 255, 51);
        expText->SetText(L"0.00%(0.00%/h)");
        axpText->SetText(L"0.00%(0.00%/h)");
        UpdateDamageDisplay();
    }
    void ToggleSessionImpl(const TEWGraphicButtonWidget* toggleButton) {
        if (currentSessionState == STOPPED || currentSessionState == PAUSED) {
            if (!HasCurrent) {
                HasCurrent = true;
                Current = {};
                Current.Start = Records::NowMs();
                Current.LevelFrom = currentEXP;
                Current.HeroFrom = currentAXP;
                NoteMap(Current.Maps, CurrentMap);
                Note(Current.Morphs, SelfMorph);
            }
            Revision++;
            currentSessionState = RUNNING;
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            startSeconds = seconds;
            startEXP = currentEXP;
            startAXP = currentAXP;
            toggleSessionLabel->SetText(L"Pause");
            toggleSessionButton->color = Color(255, 255, 255, 255);
        } else if (currentSessionState == RUNNING) {
            currentSessionState = PAUSED;
            Revision++;
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            savedSeconds += seconds - startSeconds;
            timerText->SetText(FormatDuration(savedSeconds).c_str());
            savedEXP += currentEXP - startEXP;
            savedAXP += currentAXP - startAXP;
            toggleSessionLabel->SetText(L"Continue");
            toggleSessionButton->color = Color(255, 0, 255, 51);
        }
    }
    __declspec(naked) void ResetSession() {
        __asm {
            push eax
            call ResetSessionImpl
            add esp, 4
            ret
        }
    }
    __declspec(naked) void ToggleSession() {
        __asm {
            push eax
            call ToggleSessionImpl
            add esp, 4
            ret
        }
    }
    TEWGuageWidget* CreateGaugeMarked(const int16_t x1, const int16_t y1, const int16_t x2, const int16_t y2, const float fill) {
        auto* gauge = Widget::Create<TEWGuageWidget>(CachedHost);
        if (!gauge) {
            return nullptr;
        }
        delete[] gauge->imageData.atlasFrames;
        gauge->imageData.imageName = 1593835568;
        gauge->imageData.frameCount = 2;
        gauge->imageData.atlasFrames = new AtlasFrame[2]{
            {30, 4, 1, 1},
            {249, 43, 1, 1}
        };
        gauge->fillPercent = fill;
        gauge->rect = {x1, y1, x2, y2};

        const std::vector<Rect> markPositions = {
            {42,4,43,6},
            {85,2,87,6},
            {128,4,129,6}
        };
        for (const auto& markPosition : markPositions) {
            auto* mark = Widget::Create<TEWCustomPanelWidget>(CachedHost);
            if (!mark) {
                continue;
            }
            mark->rect = markPosition;
            mark->parent = gauge;
            gauge->childrenList->push_back(mark);
        }
        return gauge;
    }
    void CreateWidget(TLBSWidget* RootWidget) {
        sessionTracker =  Widget::Create<TEWCustomPanelWidget>(CachedHost);
        MakeWindow(sessionTracker, 270, 200, windowTitle, true);
        sessionTracker->isRestrictedToScreen = true;
        sessionTracker->isVisible = false;

        timerText = Widget::Create<TEWLabel>(CachedHost);
        timerText->textAlignment = 2;
        timerText->rect = {143, 12, 242, 32};
        timerText->SetText(L"0s");
        timerText->textColor = Color(255, 138, 138, 138);

        const auto expIcon = Widget::Create<TNTIconWidget>(CachedHost);
        expIcon->rect = {13,  42, 43, 72};
        expIcon->image->imageID = 1089;
        expText =  Widget::Create<TEWLabel>(CachedHost);
        expText->textAlignment = 1;
        expText->pxPerLine = 120;
        expText->xPosition = 0;
        expText->rect = {47, 50, 167, 100};
        expText->SetText(L"0.00%(0.00%/h)");

        const auto axpIcon = Widget::Create<TNTIconWidget>(CachedHost);
        axpIcon->rect = {13,  79, 43, 109};
        axpIcon->image->imageID = 4279;
        axpText = Widget::Create<TEWLabel>(CachedHost);
        axpText->textAlignment = 1;
        axpText->pxPerLine = 120;
        axpText->xPosition = 0;
        axpText->rect = {47, 87, 167, 137};
        axpText->SetText(L"0.00%(0.00%/h)");

        const auto healthPotIcon = Widget::Create<TNTIconWidget>(CachedHost);
        healthPotIcon->rect = {192,  110, 222, 140};
        healthPotIcon->image->imageID = 7098;
        healthPotText = Widget::Create<TEWLabel>(CachedHost);
        healthPotText->textAlignment = 3;
        healthPotText->pxPerLine = 30;
        healthPotText->xPosition = 0;
        healthPotText->rect = {195, 130, 225, 160};
        healthPotText->SetText(L"0");

        const auto manaPotIcon = Widget::Create<TNTIconWidget>(CachedHost);
        manaPotIcon->rect = {227,  110, 257, 140};
        manaPotIcon->image->imageID = 7099;
        manaPotText = Widget::Create<TEWLabel>(CachedHost);
        manaPotText->textAlignment = 3;
        manaPotText->pxPerLine = 30;
        manaPotText->xPosition = 0;
        manaPotText->rect = {230, 130, 260, 160};
        manaPotText->SetText(L"0");

        const auto petGAIcon = Widget::Create<TNTIconWidget>(CachedHost);
        petGAIcon->rect = {196,  155,  226, 185};
        petGAIcon->image->imageID = 4814;
        petGAText = Widget::Create<TEWLabel>(CachedHost);
        petGAText->textAlignment = 3;
        petGAText->pxPerLine = 30;
        petGAText->xPosition = 0;
        petGAText->rect = {195, 175, 225, 205};
        petGAText->SetText(L"0");

        const auto partnerGAIcon = Widget::Create<TNTIconWidget>(CachedHost);
        partnerGAIcon->rect = {231,  155, 261, 185};
        partnerGAIcon->image->imageID = 4813;
        partnerGAText = Widget::Create<TEWLabel>(CachedHost);
        partnerGAText->textAlignment = 3;
        partnerGAText->pxPerLine = 30;
        partnerGAText->xPosition = 0;
        partnerGAText->rect = {230, 175, 260, 205};
        partnerGAText->SetText(L"0");

        toggleSessionButton = Widget::Create<TEWGraphicButtonWidget>(CachedHost);
        toggleSessionButton->rect = {193, 40, 258, 90};
        toggleSessionButton->color = Color(255, 0, 255, 51);
        toggleSessionButton->imageData.imageName = 1593835620;
        delete[] toggleSessionButton->imageData.atlasFrames;
        toggleSessionButton->imageData.frameCount = 9;
        toggleSessionButton->imageData.atlasFrames = new AtlasFrame[9];
        toggleSessionButton->imageData.atlasFrames[0] = AtlasFrame{4,   420, 3, 24};
        toggleSessionButton->imageData.atlasFrames[1] = AtlasFrame{66,  420, 4, 24};
        toggleSessionButton->imageData.atlasFrames[2] = AtlasFrame{0,   420, 5, 24};
        toggleSessionButton->imageData.atlasFrames[3] = AtlasFrame{75,  420, 3, 24};
        toggleSessionButton->imageData.atlasFrames[4] = AtlasFrame{137, 420, 4, 24};
        toggleSessionButton->imageData.atlasFrames[5] = AtlasFrame{71,  420, 5, 24};
        toggleSessionButton->imageData.atlasFrames[6] = AtlasFrame{146, 420, 3, 24};
        toggleSessionButton->imageData.atlasFrames[7] = AtlasFrame{208, 420, 4, 24};
        toggleSessionButton->imageData.atlasFrames[8] = AtlasFrame{142, 420, 5, 24};
        toggleSessionButton->nineSliceInfo = NineSliceInfo(
        55,
        25,
        60,
        25,
        5,
        0,
        5,
        0);
        toggleSessionButton->sliceCount = 3;
        toggleSessionButton->drawMode = 4;
        toggleSessionButton->callbackFunction = reinterpret_cast<uint32_t>(ToggleSession);
        toggleSessionButton->callbackArgument = reinterpret_cast<uint32_t>(toggleSessionButton);

        toggleSessionLabel = Widget::Create<TEWLabel>(CachedHost);
        toggleSessionLabel->rect = {0, 7, 66, 57};
        toggleSessionLabel->textAlignment = 3;
        toggleSessionLabel->pxPerLine = 65;
        toggleSessionLabel->SetText(L"Start");
        toggleSessionLabel->parent = toggleSessionButton;
        toggleSessionButton->childrenList->push_back(toggleSessionLabel);

        resetSessionButton = Widget::Create<TEWGraphicButtonWidget>(CachedHost);
        resetSessionButton->rect = {193, 75, 258, 125};
        resetSessionButton->color = Color(255, 132, 12, 35);
        resetSessionButton->imageData.imageName = 1593835620;
        delete[] resetSessionButton->imageData.atlasFrames;
        resetSessionButton->imageData.frameCount = 9;
        resetSessionButton->imageData.atlasFrames = new AtlasFrame[9];
        resetSessionButton->imageData.atlasFrames[0] = AtlasFrame{4,   420, 3, 24};
        resetSessionButton->imageData.atlasFrames[1] = AtlasFrame{66,  420, 4, 24};
        resetSessionButton->imageData.atlasFrames[2] = AtlasFrame{0,   420, 5, 24};
        resetSessionButton->imageData.atlasFrames[3] = AtlasFrame{75,  420, 3, 24};
        resetSessionButton->imageData.atlasFrames[4] = AtlasFrame{137, 420, 4, 24};
        resetSessionButton->imageData.atlasFrames[5] = AtlasFrame{71,  420, 5, 24};
        resetSessionButton->imageData.atlasFrames[6] = AtlasFrame{146, 420, 3, 24};
        resetSessionButton->imageData.atlasFrames[7] = AtlasFrame{208, 420, 4, 24};
        resetSessionButton->imageData.atlasFrames[8] = AtlasFrame{142, 420, 5, 24};
        resetSessionButton->nineSliceInfo = NineSliceInfo(
        55,
        25,
        60,
        25,
        5,
        0,
        5,
        0);
        resetSessionButton->sliceCount = 3;
        resetSessionButton->drawMode = 4;
        resetSessionButton->callbackFunction = reinterpret_cast<uint32_t>(ResetSession);
        resetSessionButton->callbackArgument = reinterpret_cast<uint32_t>(resetSessionButton);

        const auto resetSessionLabel = Widget::Create<TEWLabel>(CachedHost);
        resetSessionLabel->rect = {0, 7, 66, 57};
        resetSessionLabel->textAlignment = 3;
        resetSessionLabel->pxPerLine = 65;
        resetSessionLabel->SetText(L"Reset");
        resetSessionLabel->parent = resetSessionButton;
        resetSessionButton->childrenList->push_back(resetSessionLabel);

        player1Container = Widget::Create<TLBSWidget>(CachedHost);
        player1Container->rect = sessionTracker->rect;
        player1Container->isInteractable = false;
        player1Guage = CreateGaugeMarked(13, 127, 185, 133, 1.0f);
        if (!player1Guage) {
            sessionTracker = nullptr;
            return;
        }
        player1Name = Widget::Create<TEWLabel>(CachedHost);
        player1Name->SetText(L"Player1");
        player1Name->textAlignment = 1;
        player1Name->rect = {13, 113, 63, 163};
        player1DMG = Widget::Create<TEWLabel>(CachedHost);
        player1DMG->SetText(L"0");
        player1DMG->textAlignment = 2;
        player1DMG->pxPerLine = 40;
        player1DMG->rect = {145, 113, 185, 163};
        player1Guage->parent = player1Container;
        player1DMG->parent = player1Container;
        player1Name->parent = player1Container;
        player1Container->childrenList->push_back(player1Guage);
        player1Container->childrenList->push_back(player1DMG);
        player1Container->childrenList->push_back(player1Name);

        player2Container = Widget::Create<TLBSWidget>(CachedHost);
        player2Container->rect = sessionTracker->rect;
        player2Container->isVisible = false;
        player2Container->isInteractable = false;
        player2Guage = CreateGaugeMarked(13, 154, 185, 160, 0.5f);
        if (!player2Guage) {
            sessionTracker = nullptr;
            return;
        }
        player2Name = Widget::Create<TEWLabel>(CachedHost);
        player2Name->SetText(L"Player2");
        player2Name->textAlignment = 1;
        player2Name->rect = {13, 140, 63, 190};
        player2DMG = Widget::Create<TEWLabel>(CachedHost);
        player2DMG->SetText(L"0");
        player2DMG->textAlignment = 2;
        player2DMG->pxPerLine = 40;
        player2DMG->rect = {145, 140, 185, 190};
        player2Guage->parent = player2Container;
        player2DMG->parent = player2Container;
        player2Name->parent = player2Container;
        player2Container->childrenList->push_back(player2Guage);
        player2Container->childrenList->push_back(player2DMG);
        player2Container->childrenList->push_back(player2Name);

        player3Container = Widget::Create<TLBSWidget>(CachedHost);
        player3Container->rect = sessionTracker->rect;
        player3Container->isVisible = false;
        player3Container->isInteractable = false;
        player3Guage = CreateGaugeMarked(13, 181, 185, 187, 0.3f);
        if (!player3Guage) {
            sessionTracker = nullptr;
            return;
        }
        player3Name = Widget::Create<TEWLabel>(CachedHost);
        player3Name->SetText(L"Player3");
        player3Name->textAlignment = 1;
        player3Name->rect = {13, 167, 63, 217};
        player3DMG = Widget::Create<TEWLabel>(CachedHost);
        player3DMG->SetText(L"0");
        player3DMG->textAlignment = 2;
        player3DMG->pxPerLine = 40;
        player3DMG->rect = {145, 167, 185, 217};
        player3Guage->parent = player3Container;
        player3DMG->parent = player3Container;
        player3Name->parent = player3Container;
        player3Container->childrenList->push_back(player3Guage);
        player3Container->childrenList->push_back(player3DMG);
        player3Container->childrenList->push_back(player3Name);

        timerText->parent = sessionTracker;
        expIcon->parent = sessionTracker;
        expText->parent = sessionTracker;
        axpIcon->parent = sessionTracker;
        axpText->parent = sessionTracker;
        healthPotIcon->parent = sessionTracker;
        healthPotText->parent = sessionTracker;
        manaPotIcon->parent = sessionTracker;
        manaPotText->parent = sessionTracker;
        petGAIcon->parent = sessionTracker;
        petGAText->parent = sessionTracker;
        partnerGAIcon->parent = sessionTracker;
        partnerGAText->parent = sessionTracker;
        toggleSessionButton->parent = sessionTracker;
        resetSessionButton->parent = sessionTracker;

        sessionTracker->childrenList->push_back(timerText);
        sessionTracker->childrenList->push_back(expIcon);
        sessionTracker->childrenList->push_back(expText);
        sessionTracker->childrenList->push_back(axpIcon);
        sessionTracker->childrenList->push_back(axpText);
        sessionTracker->childrenList->push_back(healthPotIcon);
        sessionTracker->childrenList->push_back(healthPotText);
        sessionTracker->childrenList->push_back(manaPotIcon);
        sessionTracker->childrenList->push_back(manaPotText);
        sessionTracker->childrenList->push_back(petGAIcon);
        sessionTracker->childrenList->push_back(petGAText);
        sessionTracker->childrenList->push_back(partnerGAIcon);
        sessionTracker->childrenList->push_back(partnerGAText);
        sessionTracker->childrenList->push_back(toggleSessionButton);
        sessionTracker->childrenList->push_back(resetSessionButton);
        sessionTracker->childrenList->push_back(player1Container);
        sessionTracker->childrenList->push_back(player2Container);
        sessionTracker->childrenList->push_back(player3Container);

        sessionTracker->parent = RootWidget;
        RootWidget->childrenList->push_back(sessionTracker);
    }
    void DestroyWidgetTree(TLBSWidget* Widget) {
        if (!Widget) {
            return;
        }
        if (Widget->childrenList) {
            if (Widget->childrenList->list) {
                for (uint32_t i = 0; i < Widget->childrenList->count; i++) {
                    DestroyWidgetTree(Widget->childrenList->list[i]);
                }
            }
            delete Widget->childrenList;
        }
        delete Widget;
    }
    void UpdateDisplay(TLBSWidget* RootWidget) {
        if (!sessionTracker) {
            CreateWidget(RootWidget);
            if (sessionTracker && currentSessionState == PAUSED) {
                toggleSessionLabel->SetText(L"Continue");
                timerText->SetText(FormatDuration(savedSeconds).c_str());
                expText->SetText(std::format(L"{:.2f}%", savedEXP * 100).c_str());
                axpText->SetText(std::format(L"{:.2f}%", savedAXP * 100).c_str());
            }
        }
        if (currentSessionState == RUNNING) {
            const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()
            ).count();
            const int64_t totalSeconds = savedSeconds + seconds - startSeconds;
            timerText->SetText(FormatDuration(totalSeconds).c_str());
            const float gainedEXP = currentEXP - startEXP + savedEXP;
            const float EXPPerHour = totalSeconds > 0 ? gainedEXP / totalSeconds * 3600 : 0.0f;
            expText->SetText(std::format(L"{:.2f}%({:.2f}%/h)", gainedEXP * 100, EXPPerHour * 100).c_str());
            const float gainedAXP = currentAXP - startAXP + savedAXP;
            const float AXPPerHour = totalSeconds > 0 ? gainedAXP / totalSeconds * 3600 : 0.0f;
            axpText->SetText(std::format(L"{:.2f}%({:.2f}%/h)", gainedAXP * 100, AXPPerHour * 100).c_str());
        }
    }
    bool OnLevPacket(const Packet::LevPacket& Packet) {
        currentEXP = Packet.battleLevel + static_cast<float>(Packet.battleLevelXP) / Packet.battleLevelXPMax;
        currentAXP = Packet.heroLevel + static_cast<float>(Packet.heroLevelXP) / Packet.heroLevelXPMax;
        return true;
    }
    bool OnSuPacket(const Packet::SuPacket& Packet) {
        if (Packet.userEntityID != localPlayerID) {
            return true;
        }
        if (currentSessionState != RUNNING) {
            return true;
        }
        partyDamages[Packet.userEntityID].damage += Packet.damage;
        Current.Damage += std::max(0, Packet.damage);
        if (Packet.targetEntityType == EntityType::Monster && !Packet.targetAlive) Current.Kills++;
        UpdateDamageDisplay();
        return true;
    }
    bool OnCInfoPacket(const Packet::CInfoPacket& Packet) {
        if (!partyDamages.contains(Packet.characterID)) {
            partyDamages[Packet.characterID] = {std::wstring(Packet.name.begin(), Packet.name.end()), 0};
        }
        localPlayerID = Packet.characterID;
        SelfMorph = static_cast<int>(Packet.morphVNum);
        if (currentSessionState == RUNNING) Note(Current.Morphs, SelfMorph);
        localPlayerName = std::wstring(Packet.name.begin(), Packet.name.end());
        UpdateDamageDisplay();
        return true;
    }
}

namespace {

    using namespace Records;

    constexpr int TableRows = 12;
    constexpr int16_t Left = 12;
    constexpr int16_t Top = 44;

    TLBSWidget* PageWidget = nullptr;
    int16_t LineWidth = 0;
    TEWLabel* Lines[3]{};
    Table Grid;
    Pager Pages;
    Clicks Click;
    WidgetKit::Tab ToggleButton;
    WidgetKit::Dropdown* MapPicker = nullptr;
    std::vector<std::wstring> MapValues{L""};
    std::wstring MapFilter;
    TEWGraphicButtonWidget* RemoveButtons[TableRows]{};
    size_t PickerFrom = static_cast<size_t>(-1);
    int PageIndex = 0;
    int Selected = -2;
    std::vector<int> RowTargets;
    bool Dirty = true;
    int ShownRevision = -1;
    int64_t LastLiveRefresh = 0;

    std::wstring Levels(const double Gained) {
        return std::format(L"{:.2f}%", Gained * 100);
    }

    std::wstring PerHour(const double Gained, const int64_t Ms) {
        return Ms > 0 ? std::format(L"{:.2f}%", Gained * 100 * 3'600'000.0 / static_cast<double>(Ms)) : L"-";
    }

    std::wstring Names(const std::vector<int>& Values, std::wstring (*Name)(int)) {
        std::wstring Text;
        for (const int Value : Values) Text += (Text.empty() ? L"" : L", ") + Name(Value);
        return Text.empty() ? L"-" : Text;
    }

    bool OnMap(const ExpRecord& R) {
        return MapFilter.empty() || std::find(R.Maps.begin(), R.Maps.end(), MapFilter) != R.Maps.end();
    }

    void FillMapPicker() {
        PickerFrom = Sessions.size();
        if (!MapPicker) return;
        std::vector<std::wstring> Maps;
        for (const ExpRecord& R : Sessions) {
            for (const std::wstring& Map : R.Maps) NoteMap(Maps, Map);
        }
        std::sort(Maps.begin(), Maps.end());
        std::vector<std::wstring> Items{L"All maps"};
        MapValues = {L""};
        int Picked = 0;
        for (const std::wstring& Map : Maps) {
            if (Map == MapFilter) Picked = static_cast<int>(Items.size());
            Items.push_back(Map);
            MapValues.push_back(Map);
        }
        if (!Picked) MapFilter.clear();
        MapPicker->SetItems(std::move(Items));
        MapPicker->SetSelected(Picked);
    }

    void SaveAll() {
        std::string Text;
        for (const ExpRecord& R : Sessions) Text += ToJson(R) + "\n";
        WriteFile(HistoryFile(), Text);
    }

    void Remove(const int Index) {
        if (Index < 0 || Index >= static_cast<int>(Sessions.size())) return;
        Sessions.erase(Sessions.begin() + Index);
        SaveAll();
        if (Selected == Index) Selected = -2;
        else if (Selected > Index) Selected--;
        Revision++;
    }

    void RefreshPage() {
        if (PickerFrom != Sessions.size()) FillMapPicker();
        const ExpRecord Live = Snapshot();
        std::vector<int> Order;
        if (HasCurrent && OnMap(Live)) Order.push_back(-1);
        for (int i = static_cast<int>(Sessions.size()) - 1; i >= 0; i--) {
            if (OnMap(Sessions[i])) Order.push_back(i);
        }
        auto At = [&](const int Index) -> const ExpRecord& { return Index < 0 ? Live : Sessions[Index]; };

        int64_t Played = 0;
        double LevelTotal = 0, BestRate = 0;
        int Count = 0;
        for (const int Index : Order) {
            if (Index < 0) continue;
            const ExpRecord& R = Sessions[Index];
            Count++;
            Played += R.ActiveMs;
            LevelTotal += R.LevelGained;
            if (R.ActiveMs >= 60'000) BestRate = std::max(BestRate, R.LevelGained * 3'600'000.0 / static_cast<double>(R.ActiveMs));
        }
        if (Order.empty()) {
            SetText(Lines[0], Sessions.empty() ? L"No sessions yet. Start one in the EXP window; Reset saves it here."
                                               : L"No session on this map.", Hint, LineWidth);
        } else {
            SetText(Lines[0], std::to_wstring(Count) + L" sessions, " + Duration(Played) + L", " + Levels(LevelTotal)
                              + L" levels in all, best " + Levels(BestRate) + L"/h", Gold, LineWidth);
        }
        if (Selected == -1 && !HasCurrent) Selected = Sessions.empty() ? -2 : static_cast<int>(Sessions.size()) - 1;
        if (Selected == -1 || (Selected >= 0 && Selected < static_cast<int>(Sessions.size()))) {
            const ExpRecord& R = At(Selected);
            std::wstring Maps;
            for (const std::wstring& Map : R.Maps) Maps += (Maps.empty() ? L"" : L", ") + Map;
            SetText(Lines[1], L"Maps: " + (Maps.empty() ? L"-" : Maps) + L". SP: " + Names(R.Morphs, &SpName), Plain, LineWidth);
            SetText(Lines[2], std::format(L"Level {:.2f} to {:.2f}, hero {:.2f} to {:.2f}, ", R.LevelFrom, R.LevelFrom + R.LevelGained,
                                          R.HeroFrom, R.HeroFrom + R.HeroGained)
                              + std::to_wstring(R.Kills) + L" kills, " + Compact(R.Damage) + L" damage", Plain, LineWidth);
        } else {
            SetText(Lines[1], L"Click a session to see its maps, specialists and kills.", Hint, LineWidth);
            SetText(Lines[2], L"", Hint, LineWidth);
        }

        const int Rows = static_cast<int>(Order.size());
        const int PageCount = std::max(1, (Rows + TableRows - 1) / TableRows);
        PageIndex = std::clamp(PageIndex, 0, PageCount - 1);
        Grid.ClearRows();
        RowTargets.assign(TableRows, -2);
        for (int Slot = 0; Slot < TableRows; Slot++) {
            const int Index = PageIndex * TableRows + Slot;
            Show(RemoveButtons[Slot], Index < Rows && Order[Index] >= 0);
            if (Index >= Rows) continue;
            const int Target = Order[Index];
            const ExpRecord& R = At(Target);
            RowTargets[Slot] = Target;
            const Color& Tint = Target == Selected ? Records::Picked : Target < 0 ? Gold : Plain;
            Grid.Set(Slot, 0, Target < 0 ? (currentSessionState == RUNNING ? L"Running" : L"Paused") : When(R.Start), Tint);
            Grid.Set(Slot, 1, Duration(R.ActiveMs), Tint);
            Grid.Set(Slot, 2, R.Maps.empty() ? L"-"
                              : R.Maps.front() + (R.Maps.size() > 1 ? L" +" + std::to_wstring(R.Maps.size() - 1) : L""), Tint);
            Grid.Set(Slot, 3, Levels(R.LevelGained), Tint);
            Grid.Set(Slot, 4, PerHour(R.LevelGained, R.ActiveMs), Tint);
            Grid.Set(Slot, 5, Levels(R.HeroGained), Tint);
            Grid.Set(Slot, 6, PerHour(R.HeroGained, R.ActiveMs), Tint);
            Grid.Set(Slot, 7, Compact(R.Damage), Tint);
        }
        Pages.Update(PageIndex, PageCount);
    }

    void __cdecl OnToggleWindow(void*) {
        if (!sessionTracker) return;
        const bool Shown = !(sessionTracker->isVisible || tempInvisible);
        tempInvisible = false;
        sessionTracker->isVisible = Shown;
        if (Shown) sessionTracker->BubbleUp();
    }

    void __cdecl OnRemove(void* Argument) {
        const auto Slot = static_cast<size_t>(reinterpret_cast<intptr_t>(Argument));
        if (Slot < RowTargets.size() && RowTargets[Slot] >= 0) Remove(RowTargets[Slot]);
        Dirty = true;
    }

    TEWGraphicButtonWidget* CreateRemoveButton(TLBSWidget* Parent, const int16_t X, const int16_t Y, const int Slot) {
        auto* Button = Widget::Create<TEWGraphicButtonWidget>(CachedHost);
        if (!Button) return nullptr;
        delete[] Button->imageData.atlasFrames;
        Button->imageData.imageName = 1593835585;
        Button->imageData.frameCount = 3;
        Button->imageData.atlasFrames = new AtlasFrame[3]{{244, 2, 20, 20}, {264, 2, 20, 20}, {244, 22, 20, 20}};
        Button->drawMode = 0;
        Button->rect = {X, Y, static_cast<int16_t>(X + 20), static_cast<int16_t>(Y + 20)};
        Button->isVisible = false;
        WidgetKit::SetOnClick(Button, &OnRemove, reinterpret_cast<void*>(static_cast<intptr_t>(Slot)));
        Button->parent = Parent;
        Parent->childrenList->push_back(Button);
        return Button;
    }

    void __cdecl OnPickMap(void*, const int Index) {
        MapFilter = Index > 0 && Index < static_cast<int>(MapValues.size()) ? MapValues[Index] : L"";
        PageIndex = 0;
        Dirty = true;
    }
}

namespace ExpSession {
    void Startup(const ModHost* Host) {
        CachedHost = Host;
        Sessions.clear();
        for (const std::string& Line : ReadLines(HistoryFile())) {
            ExpRecord R;
            if (FromJson(Line, R)) Sessions.push_back(std::move(R));
        }
        const auto Active = ReadLines(ActiveFile());
        if (!Active.empty() && FromJson(Active[0], Current)) {
            HasCurrent = true;
            currentSessionState = PAUSED;
            savedSeconds = Current.ActiveMs / 1000;
            savedEXP = static_cast<float>(Current.LevelGained);
            savedAXP = static_cast<float>(Current.HeroGained);
        }
        localPlayerID = static_cast<int>(GameData::PlayerId());
        Packet::SubscribePacket(Host, &OnLevPacket);
        Packet::SubscribePacket(Host, &OnSuPacket);
        Packet::SubscribePacket(Host, &OnCInfoPacket);
    }

    void Shutdown() {
        if (HasCurrent) WriteFile(ActiveFile(), ToJson(Snapshot()));
        ForgetPage();
        if (sessionTracker) {
            sessionTracker->parent->childrenList->remove(sessionTracker);
            DestroyWidgetTree(sessionTracker);
            sessionTracker = nullptr;
        }
        CachedHost = nullptr;
    }

    void OnRawPacket(const char* Packet, const bool IsSend) {
        if (IsSend || !Packet || std::strncmp(Packet, "c_mode 1 ", 9) != 0) return;
        const auto T = Split(Packet, ' ');
        if (T.size() < 4 || Num(T[2]) != localPlayerID) return;
        SelfMorph = static_cast<int>(Num(T[3]));
        if (currentSessionState == RUNNING) {
            Note(Current.Morphs, SelfMorph);
            Revision++;
        }
    }

    void Tick(TLBSWidget* RootWidget, const TickContext tickContext) {
        if (const long Id = GameData::PlayerId()) localPlayerID = static_cast<int>(Id);
        if (const int64_t Now = NowMs(); tickContext.isPlayerLoaded && Now - LastMapRead > 500) {
            LastMapRead = Now;
            const std::wstring Map = MinimapMapName(RootWidget);
            if (!Map.empty() && Map != CurrentMap) {
                CurrentMap = Map;
                if (currentSessionState == RUNNING) {
                    NoteMap(Current.Maps, CurrentMap);
                    Revision++;
                }
            }
        }
        UpdateDisplay(RootWidget);
        if (!sessionTracker) return;
        if (!tickContext.isPlayerLoaded) {
            if (sessionTracker->isVisible) {
                sessionTracker->isVisible = false;
                tempInvisible = true;
            }
        } else if (tempInvisible) {
            tempInvisible = false;
            sessionTracker->isVisible = true;
        }
    }

    int16_t BuildPage(TLBSWidget* Page, const int16_t Width) {
        PageWidget = Page;
        LineWidth = static_cast<int16_t>(Width - 2 * Left);
        ToggleButton = WidgetKit::CreateTab(CachedHost, Left, Top, 130, L"Show / Hide window", &OnToggleWindow, nullptr);
        if (ToggleButton.Button) {
            ToggleButton.Button->parent = Page;
            Page->childrenList->push_back(ToggleButton.Button);
        }
        int16_t Y = Top + 32;
        for (TEWLabel*& Line : Lines) {
            Line = AddLabel(CachedHost, Page, Left, Y, LineWidth);
            Y += 16;
        }
        Y += 4;
        constexpr int16_t RowHeight = 22;
        Grid.Build(CachedHost, Page, Left, Y, static_cast<int16_t>(LineWidth - 28), TableRows, RowHeight);
        Grid.SetColumns({{L"Date", 64}, {L"Time", 50, 2}, {L"Map", 100}, {L"Level", 56, 2}, {L"Per hour", 56, 2},
                         {L"Hero", 50, 2}, {L"Per hour", 56, 2}, {L"Damage", 0, 2}});
        for (int Slot = 0; Slot < TableRows; Slot++) {
            RemoveButtons[Slot] = CreateRemoveButton(Page, static_cast<int16_t>(Left + LineWidth - 20),
                                                     static_cast<int16_t>(Grid.RowTop(Slot) + (RowHeight - 20) / 2 + 1), Slot);
        }
        Pages.Build(CachedHost, Page, static_cast<int16_t>(Width / 2), static_cast<int16_t>(Grid.Bottom() + 6));

        WidgetKit::Dropdown::Desc Desc;
        Desc.X = Left + 140;
        Desc.Y = Top + 3;
        Desc.Width = 180;
        Desc.Placeholder = L"All maps";
        Desc.OnPick = &OnPickMap;
        MapPicker = WidgetKit::Dropdown::Create(CachedHost, Desc);
        if (MapPicker) {
            MapPicker->Widget()->parent = Page;
            Page->childrenList->push_back(MapPicker->Widget());
        }
        PickerFrom = static_cast<size_t>(-1);
        Dirty = true;
        return static_cast<int16_t>(Grid.Bottom() + 30);
    }

    void ForgetPage() {
        PageWidget = nullptr;
        for (auto& Line : Lines) Line = nullptr;
        Grid.Forget();
        Pages.Forget();
        ToggleButton = {};
        MapPicker = nullptr;
        for (auto& Button : RemoveButtons) Button = nullptr;
    }

    void TickPage(const TickContext& Context) {
        if (!PageWidget) return;
        if (MapPicker) MapPicker->Tick(Context);
        const bool PickerOpen = MapPicker && MapPicker->IsOpen();
        if (Context.mouseWheel != 0 && !PickerOpen) {
            PageIndex += Context.mouseWheel > 0 ? -1 : 1;
            Dirty = true;
        }
        if (Click.Pressed() && !PickerOpen) {
            if (const int Step = Pages.Clicked(Context)) {
                PageIndex += Step;
                Dirty = true;
            } else if (const int Row = Grid.RowAt(Context); Row >= 0 && Row < static_cast<int>(RowTargets.size()) && RowTargets[Row] != -2) {
                Selected = RowTargets[Row] == Selected ? -2 : RowTargets[Row];
                Dirty = true;
            }
        }
        const int64_t Now = NowMs();
        const bool Live = currentSessionState == RUNNING && Now - LastLiveRefresh > 1000;
        if (!Dirty && Revision == ShownRevision && !Live) return;
        if (Live) LastLiveRefresh = Now;
        Dirty = false;
        ShownRevision = Revision;
        RefreshPage();
    }
}
