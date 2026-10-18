#define NOMINMAX
#include <windows.h>
#include "ModContract.h"
#include "GameHint.h"
#include "TEWButtonWidget.h"
#include <climits>
#include <cmath>
#include <ctime>
#include "Features.h"
#include "TEWLabel.h"
#include "LevPacket.h"
#include <array>
#include <execution>
#include <unordered_map>
#include <unordered_set>
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
#include <map>
#include <set>
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
    long LevCharacter = 0;
    float startAXP;
    float savedAXP;

    bool tempInvisible = false;

    struct ExpRecord {
        long Character = 0;
        int64_t Start = 0;
        int64_t End = 0;
        int64_t ActiveMs = 0;
        double LevelFrom = 0;
        double LevelGained = 0;
        double HeroFrom = 0;
        double HeroGained = 0;
        int64_t Damage = 0;
        int Kills = 0;
        int PartyKills = -1;
        int FullKills = 0;
        int Deaths = 0;
        int64_t Gold = 0;
        std::vector<std::wstring> Maps;
        std::vector<int> Morphs;
        std::map<int, int64_t> Picked;
        std::map<int, int64_t> Used;
        std::map<long, std::pair<std::wstring, int64_t>> DamageBy;
        std::map<std::array<int, 3>, int> Spots;
    };

    std::vector<ExpRecord> Sessions;
    ExpRecord Current;
    bool HasCurrent = false;
    int SelfMorph = -1;
    std::wstring CurrentMap;
    int64_t LastMapRead = 0;
    int Revision = 0;

    constexpr int GoldVnum = 1046;
    std::unordered_map<long, std::pair<int, int64_t>> Drops;
    std::unordered_map<int, std::pair<int, int64_t>> Bag;
    std::unordered_map<int, int64_t> LastUse;
    std::unordered_map<long, std::wstring> PartyNames;
    std::unordered_set<long> MyMates;
    std::unordered_map<long, std::pair<int, int>> MonsterAt;

    constexpr double FullXpShare = 0.10;
    struct MonsterHits {
        int64_t Hp = 0;
        int64_t MaxHp = 0;
        int64_t Mine = 0;
        bool Party = false;
    };
    std::unordered_map<long, MonsterHits> Hits;
    int CurrentMapId = -1;

    std::filesystem::path HistoryFile() { return Records::DataDir() / L"exp.jsonl"; }
    std::filesystem::path ActiveFile() { return Records::DataDir() / L"exp_active.json"; }

    std::string ToJson(const ExpRecord& R) {
        std::string J = "{\"character\":" + std::to_string(R.Character) + ",\"start\":" + std::to_string(R.Start) + ",\"end\":" + std::to_string(R.End)
            + ",\"activeMs\":" + std::to_string(R.ActiveMs) + ",\"levelFrom\":" + std::format("{:.6f}", R.LevelFrom)
            + ",\"levelGained\":" + std::format("{:.6f}", R.LevelGained) + ",\"heroFrom\":" + std::format("{:.6f}", R.HeroFrom)
            + ",\"heroGained\":" + std::format("{:.6f}", R.HeroGained) + ",\"damage\":" + std::to_string(R.Damage)
            + ",\"kills\":" + std::to_string(R.Kills) + ",\"partyKills\":" + std::to_string(R.PartyKills)
            + ",\"fullKills\":" + std::to_string(R.FullKills) + ",\"maps\":[";
        for (size_t i = 0; i < R.Maps.size(); i++) {
            if (i) J += ",";
            Records::Escape(J, Records::Utf8(R.Maps[i]));
        }
        J += "],\"morphs\":[";
        for (size_t i = 0; i < R.Morphs.size(); i++) J += (i ? "," : "") + std::to_string(R.Morphs[i]);
        J += "],\"deaths\":" + std::to_string(R.Deaths) + ",\"gold\":" + std::to_string(R.Gold);
        auto Counts = [&](const char* Key, const std::map<int, int64_t>& Values) {
            J += std::string(",\"") + Key + "\":[";
            bool First = true;
            for (const auto& [Vnum, Count] : Values) {
                J += (First ? "[" : ",[") + std::to_string(Vnum) + "," + std::to_string(Count) + "]";
                First = false;
            }
            J += "]";
        };
        Counts("picked", R.Picked);
        Counts("used", R.Used);
        J += ",\"damageBy\":[";
        bool First = true;
        for (const auto& [Id, Entry] : R.DamageBy) {
            J += (First ? "[" : ",[") + std::to_string(Id) + ",";
            Records::Escape(J, Records::Utf8(Entry.first));
            J += "," + std::to_string(Entry.second) + "]";
            First = false;
        }
        J += "],\"spots\":[";
        First = true;
        for (const auto& [Spot, Count] : R.Spots) {
            J += (First ? "[" : ",[") + std::to_string(Spot[0]) + "," + std::to_string(Spot[1]) + "," + std::to_string(Spot[2])
                + "," + std::to_string(Count) + "]";
            First = false;
        }
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
        R.PartyKills = J["partyKills"] ? static_cast<int>(J.Int("partyKills")) : -1;
        R.FullKills = static_cast<int>(J.Int("fullKills"));
        if (const Records::Json* Maps = J["maps"]) {
            for (const Records::Json& Map : Maps->Items) {
                if (Map.Type == Records::Json::String) R.Maps.push_back(Records::FromUtf8(Map.S));
            }
        }
        if (const Records::Json* Morphs = J["morphs"]) {
            for (const Records::Json& Morph : Morphs->Items) R.Morphs.push_back(static_cast<int>(Morph.N));
        }
        R.Deaths = static_cast<int>(J.Int("deaths"));
        R.Character = static_cast<long>(J.Int("character"));
        R.Gold = J.Int("gold");
        if (const Records::Json* Picked = J["picked"]) {
            for (const Records::Json& P : Picked->Items) R.Picked[static_cast<int>(P.At(0))] = P.At(1);
        }
        if (const Records::Json* Used = J["used"]) {
            for (const Records::Json& U : Used->Items) R.Used[static_cast<int>(U.At(0))] = U.At(1);
        }
        if (const Records::Json* By = J["damageBy"]) {
            for (const Records::Json& D : By->Items) {
                if (D.Items.size() == 3) R.DamageBy[static_cast<long>(D.At(0))] = {Records::FromUtf8(D.Items[1].S), D.At(2)};
            }
        }
        if (const Records::Json* Spots = J["spots"]) {
            for (const Records::Json& S : Spots->Items) {
                R.Spots[{static_cast<int>(S.At(0)), static_cast<int>(S.At(1)), static_cast<int>(S.At(2))}] = static_cast<int>(S.At(3));
            }
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
    void ToggleSessionImpl(const TEWGraphicButtonWidget* toggleButton);

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
        currentSessionState = STOPPED;
        toggleSessionLabel->SetText(L"Start");
        toggleSessionButton->color = Color(255, 0, 255, 51);
    }
    void ToggleSessionImpl(const TEWGraphicButtonWidget* toggleButton) {
        if (currentSessionState == STOPPED || currentSessionState == PAUSED) {
            if (!HasCurrent) {
                partyDamages.clear();
                partyDamages[localPlayerID] = {localPlayerName, 0};
                savedSeconds = 0;
                savedEXP = 0;
                savedAXP = 0;
                timerText->SetText(L"0s");
                expText->SetText(L"0.00%(0.00%/h)");
                axpText->SetText(L"0.00%(0.00%/h)");
                UpdateDamageDisplay();
                HasCurrent = true;
                Current = {};
                Current.Character = localPlayerID;
                Current.PartyKills = 0;
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
        resetSessionLabel->SetText(L"Stop");
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
        LevCharacter = localPlayerID;
        currentEXP = Packet.battleLevel + static_cast<float>(Packet.battleLevelXP) / Packet.battleLevelXPMax;
        currentAXP = Packet.heroLevel + static_cast<float>(Packet.heroLevelXP) / Packet.heroLevelXPMax;
        return true;
    }
    bool OnSuPacket(const Packet::SuPacket& Packet) {
        if (currentSessionState != RUNNING) {
            return true;
        }
        if (Packet.targetEntityID == localPlayerID && Packet.targetEntityType == EntityType::Player && !Packet.targetAlive) {
            Current.Deaths++;
        }
        const long User = Packet.userEntityID;
        const bool Mine = User == localPlayerID;
        const auto Member = PartyNames.find(User);
        if (!Mine && Member == PartyNames.end()) {
            return true;
        }
        const int64_t Damage = std::max(0, Packet.damage);
        const std::wstring& Name = Mine ? localPlayerName : Member->second;
        partyDamages[User].playerName = Name;
        partyDamages[User].damage += static_cast<int>(Damage);
        auto& By = Current.DamageBy[User];
        By.first = Name;
        By.second += Damage;
        if (Mine) Current.Damage += Damage;
        if (Packet.targetEntityType == EntityType::Monster && !Packet.targetAlive && Mine) Current.Kills++;
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

    constexpr int16_t Top = 44;
    constexpr int16_t SideX = 12;
    constexpr int16_t SideWidth = 160;
    constexpr int16_t MainX = SideX + SideWidth + 14;
    constexpr int CardCount = 5;
    constexpr int16_t CardHeight = 66;
    constexpr int16_t RowHeight = 22;
    constexpr int StatsRows = 14;
    constexpr int16_t IconSize = 16;
    constexpr int16_t ListStep = 20;
    constexpr int DamageRows = 4;
    constexpr int ItemRows = 7;
    constexpr int MapCells = 24;
    constexpr int16_t CellSize = 8;
    constexpr int16_t MapSize = MapCells * CellSize;
    constexpr int64_t CompareMinMs = 5 * 60'000;

    const Color StripGood(255, 40, 205, 60);
    const Color StripBad(255, 225, 45, 40);
    const Color StripPlain(255, 120, 120, 120);
    const Color BarGold(255, 255, 200, 0);
    const Color BarBlue(255, 40, 150, 255);
    const Color RuleColor(255, 80, 80, 80);
    const Color LevelColor(255, 255, 180, 6);
    const Color HeroColor(255, 239, 206, 215);
    constexpr int16_t TagWidth = 36;
    constexpr int16_t RateWidth = 128;
    const Color MapBack(255, 30, 30, 30);
    const Color NoHeat(0, 0, 0, 0);

    TLBSWidget* PageWidget = nullptr;
    int16_t MainWidth = 0;
    int16_t FullWidth = 0;
    std::vector<TLBSWidget*> Side;
    TEWLabel* Heading = nullptr;
    enum SideLine { SessionTitle, SessionState, SessionLevel, SessionHero, SessionKills, SessionFull, SessionGold,
                    TodayTitle, TodayLine, WeekTitle, WeekLine, BestLine, SideLineCount };
    TEWLabel* SideLines[SideLineCount]{};
    WidgetKit::Tab StartButton, StopButton, ToggleButton, ListButton, StatsButton, BackButton;
    WidgetKit::Dropdown* MapPicker = nullptr;
    std::vector<std::wstring> MapValues{L""};
    std::wstring MapFilter;
    size_t PickerFrom = static_cast<size_t>(-1);

    struct Card {
        TLBSWidget* Box = nullptr;
        TEWControlWidget* Strip = nullptr;
        TEWGuageWidget* Bar = nullptr;
        TNTIconWidget* Icon = nullptr;
        TEWGraphicButtonWidget* Hit = nullptr;
        TEWLabel* Name = nullptr;
        TEWLabel* Time = nullptr;
        TEWLabel* When = nullptr;
        TEWLabel* LevelTag = nullptr;
        TEWLabel* LevelText = nullptr;
        TEWLabel* HeroTag = nullptr;
        TEWLabel* HeroText = nullptr;
        TEWLabel* KillsText = nullptr;
        TEWLabel* Line3 = nullptr;
        TEWLabel* Compare = nullptr;
    };
    Card Cards[CardCount];
    Card HeaderCard;

    Table StatsGrid;
    Table DamageTable;
    TEWGuageWidget* DamageBars[DamageRows]{};
    struct ItemList {
        Table Grid;
        TNTIconWidget* Icons[ItemRows]{};
        int Scroll = 0;
        int Total = 0;
    };
    ItemList PickedList, UsedList;
    TEWLabel* MapCaption = nullptr;
    TEWButtonWidget* NextMapButton = nullptr;
    TEWControlWidget* MapBackground = nullptr;
    TEWControlWidget* MapImage = nullptr;

    constexpr int16_t MinimapImageSize = 128;
    constexpr int HeatCount = 96;

    std::map<int, int32_t> MinimapIds;

    int32_t MinimapImage(const int MapId) {
        if (const auto Found = MinimapIds.find(MapId); Found != MinimapIds.end()) return Found->second;
        int32_t Id = 0;
        const std::vector<uint8_t> Png = GameData::MinimapPng(MapId);
        uint16_t Width = 0, Height = 0;
        if (!Png.empty() && CachedHost && CachedHost->LoadUiImage) {
            Id = CachedHost->LoadUiImage(Png.data(), static_cast<uint32_t>(Png.size()), &Width, &Height);
        }
        MinimapIds[MapId] = Id;
        return Id;
    }

    std::map<int, std::pair<int, int>> MapSizes;
    bool MapSizesLoaded = false;

    std::filesystem::path MapSizeFile() { return Records::DataDir() / L"mapsizes.txt"; }

    void LoadMapSizes() {
        if (MapSizesLoaded) return;
        MapSizesLoaded = true;
        for (const std::string& Line : Records::ReadLines(MapSizeFile())) {
            const auto Parts = Records::Split(Line, ' ');
            if (Parts.size() == 3) MapSizes[static_cast<int>(Records::Num(Parts[0]))] = {static_cast<int>(Records::Num(Parts[1])), static_cast<int>(Records::Num(Parts[2]))};
        }
    }

    void NoteMapSize(const int MapId, const int Width, const int Height) {
        if (MapId < 0 || Width <= 0 || Height <= 0) return;
        LoadMapSizes();
        auto& Size = MapSizes[MapId];
        if (Size == std::pair{Width, Height}) return;
        Size = {Width, Height};
        Records::AppendLine(MapSizeFile(), std::to_string(MapId) + " " + std::to_string(Width) + " " + std::to_string(Height));
    }
    TEWControlWidget* Heat[HeatCount]{};
    int16_t MapLeft = 0;
    int16_t MapTop = 0;
    int MapIndex = 0;

    Pager Pages;
    Clicks Click;
    Clicks RightClick{VK_RBUTTON};
    int PageIndex = 0;
    std::vector<int> RowTargets;
    bool Dirty = true;
    int ShownRevision = -1;
    int64_t LastLiveRefresh = 0;
    bool HintShown = false;

    enum class ExpView { List, Stats, Details };
    ExpView View = ExpView::List;
    int Opened = -2;

    enum MenuItem { MenuOpen, MenuDelete, MenuCount };
    constexpr int16_t MenuWidth = 130;
    TLBSWidget* Menu = nullptr;
    TEWButtonWidget* MenuButtons[MenuCount]{};
    int MenuSession = -2;
    bool ConfirmDelete = false;

    std::wstring Levels(const double Gained) {
        return std::format(L"{:+.2f}%", Gained * 100);
    }

    double Rate(const double Gained, const int64_t Ms) {
        return Ms > 0 ? Gained * 3'600'000.0 / static_cast<double>(Ms) : 0.0;
    }

    std::wstring PerHour(const double Gained, const int64_t Ms) {
        return Ms > 0 ? std::format(L"{:.2f}%", Rate(Gained, Ms) * 100) : L"-";
    }

    std::wstring Thousands(const int64_t Value) {
        std::wstring Digits = std::to_wstring(Value < 0 ? -Value : Value);
        for (int At = static_cast<int>(Digits.size()) - 3; At > 0; At -= 3) Digits.insert(static_cast<size_t>(At), L",");
        return (Value < 0 ? L"-" : L"") + Digits;
    }

    std::wstring MainMap(const ExpRecord& R) {
        return R.Maps.empty() ? L"" : R.Maps.front();
    }

    std::wstring MapList(const ExpRecord& R) {
        std::wstring Maps;
        for (const std::wstring& Map : R.Maps) Maps += (Maps.empty() ? L"" : L", ") + Map;
        return Maps.empty() ? L"Unknown map" : Maps;
    }

    int64_t ItemCount(const std::map<int, int64_t>& Items) {
        int64_t Count = 0;
        for (const auto& [Vnum, Amount] : Items) Count += Amount;
        return Count;
    }

    int16_t SpIcon(const ExpRecord& R) {
        for (auto It = R.Morphs.rbegin(); It != R.Morphs.rend(); ++It) {
            if (const GameData::SpCard* Sp = GameData::FindSp(*It)) return Sp->Icon;
        }
        return -1;
    }

    bool OwnSession(const ExpRecord& R) {
        return R.Character == localPlayerID;
    }

    bool OnMap(const ExpRecord& R) {
        return OwnSession(R) && (MapFilter.empty() || std::find(R.Maps.begin(), R.Maps.end(), MapFilter) != R.Maps.end());
    }

    const ExpRecord* SessionAt(const int Index, const ExpRecord& Live) {
        if (Index == -1) return HasCurrent && OwnSession(Live) ? &Live : nullptr;
        return Index >= 0 && Index < static_cast<int>(Sessions.size()) ? &Sessions[Index] : nullptr;
    }

    int64_t LocalStart(const bool Week) {
        const std::time_t Now = std::time(nullptr);
        std::tm Local{};
        localtime_s(&Local, &Now);
        Local.tm_hour = Local.tm_min = Local.tm_sec = 0;
        if (Week) Local.tm_mday -= (Local.tm_wday + 6) % 7;
        Local.tm_isdst = -1;
        return static_cast<int64_t>(std::mktime(&Local)) * 1000;
    }

    bool HeroSession(const ExpRecord& R) {
        return R.HeroGained > 0;
    }

    double GainedXp(const ExpRecord& R) {
        return HeroSession(R) ? R.HeroGained : R.LevelGained;
    }

    struct RateTrack {
        int Count = 0;
        int64_t Ms = 0;
        double Gained = 0;
        double Best = 0;
    };

    int KillCount(const ExpRecord& R) {
        return R.PartyKills >= 0 ? R.PartyKills : R.Kills;
    }

    std::wstring FullXpText(const int Full, const int Kills) {
        return std::format(L"{:.0f}% full XP", Kills > 0 ? Full * 100.0 / Kills : 0.0);
    }

    std::wstring KillsLine(const ExpRecord& R) {
        std::wstring Line = Thousands(KillCount(R)) + L" kills";
        if (R.PartyKills > 0) Line += L" · " + FullXpText(R.FullKills, R.PartyKills);
        return Line;
    }

    struct MapStats {
        int Count = 0;
        int64_t Ms = 0;
        int Kills = 0;
        int PartyKills = 0;
        int FullKills = 0;
        double LevelGained = 0;
        double HeroGained = 0;
        RateTrack Level;
        RateTrack Hero;
        bool UsesHero() const { return Hero.Count > 0; }
        const RateTrack& Main() const { return UsesHero() ? Hero : Level; }
    };

    std::map<std::wstring, MapStats> StatsByMap(const int Skip = -2) {
        std::map<std::wstring, MapStats> Stats;
        for (int i = 0; i < static_cast<int>(Sessions.size()); i++) {
            const ExpRecord& R = Sessions[i];
            if (i == Skip || !OwnSession(R) || R.ActiveMs < CompareMinMs || R.Maps.empty()) continue;
            MapStats& S = Stats[MainMap(R)];
            S.Count++;
            S.Ms += R.ActiveMs;
            S.Kills += KillCount(R);
            if (R.PartyKills > 0) {
                S.PartyKills += R.PartyKills;
                S.FullKills += R.FullKills;
            }
            S.LevelGained += R.LevelGained;
            S.HeroGained += R.HeroGained;
            RateTrack& T = HeroSession(R) ? S.Hero : S.Level;
            T.Count++;
            T.Ms += R.ActiveMs;
            T.Gained += GainedXp(R);
            T.Best = std::max(T.Best, Rate(GainedXp(R), R.ActiveMs));
        }
        return Stats;
    }

    void Attach(TLBSWidget* Parent, TLBSWidget* Child) {
        if (!Parent || !Child) return;
        Child->parent = Parent;
        Parent->childrenList->push_back(Child);
    }

    WidgetKit::Tab AddButton(const int16_t X, const int16_t Y, const int16_t Width, const wchar_t* Text,
                             const WidgetKit::ClickFn OnClick, const intptr_t Argument) {
        WidgetKit::Tab Button = WidgetKit::CreateTab(CachedHost, X, Y, Width, Text, OnClick, reinterpret_cast<void*>(Argument));
        Attach(PageWidget, Button.Button);
        return Button;
    }

    TEWGuageWidget* AddBar(TLBSWidget* Parent, const int16_t X, const int16_t Y, const int16_t Width, const int16_t Height,
                           const Color& Tint, const float Fill = 1.0f) {
        auto* Bar = Widget::Create<TEWGuageWidget>(CachedHost);
        if (!Bar) return nullptr;
        delete[] Bar->imageData.atlasFrames;
        Bar->imageData.imageName = 1593835568;
        Bar->imageData.frameCount = 2;
        Bar->imageData.atlasFrames = new AtlasFrame[2]{{30, 4, 1, 1}, {510, 1, 1, 1}};
        Bar->rect = {X, Y, static_cast<int16_t>(X + Width), static_cast<int16_t>(Y + Height)};
        Bar->fillPercent = Fill;
        Bar->gaugeColor = Tint;
        Bar->isInteractable = false;
        Attach(Parent, Bar);
        return Bar;
    }

    bool Same(const Color& A, const Color& B) {
        return std::memcmp(&A, &B, sizeof(Color)) == 0;
    }

    void SetFill(TEWGuageWidget* Bar, const float Fill, const Color& Tint) {
        if (!Bar) return;
        const float Clamped = std::clamp(Fill, 0.0f, 1.0f);
        if (Bar->fillPercent != Clamped) Bar->fillPercent = Clamped;
        if (!Same(Bar->gaugeColor, Tint)) Bar->gaugeColor = Tint;
    }

    Card BuildCard(const int16_t X, const int16_t Y, const int16_t Width, const bool Clickable) {
        Card C;
        C.Box = Widget::Create<TLBSWidget>(CachedHost);
        if (!C.Box) return C;
        C.Box->rect = {X, Y, static_cast<int16_t>(X + Width), static_cast<int16_t>(Y + CardHeight)};
        C.Box->isInteractable = false;
        Attach(PageWidget, C.Box);
        constexpr int16_t TextX = 50;
        const auto TextWidth = static_cast<int16_t>(Width - TextX - 6);
        C.Strip = AddRect(CachedHost, C.Box, 0, 2, 4, CardHeight - 8, StripPlain);
        AddRect(CachedHost, C.Box, 0, CardHeight - 3, Width, 1, RuleColor);
        C.Icon = WidgetKit::CreateItemIcon(CachedHost, 12, 17, 28, -1);
        Attach(C.Box, C.Icon);
        C.Name = AddLabel(CachedHost, C.Box, TextX, 2, static_cast<int16_t>(TextWidth - 236));
        C.Time = AddLabel(CachedHost, C.Box, static_cast<int16_t>(Width - 234), 2, 110, 2);
        C.When = AddLabel(CachedHost, C.Box, static_cast<int16_t>(Width - 116), 2, 110, 2);
        C.LevelTag = AddLabel(CachedHost, C.Box, TextX, 19, TagWidth);
        C.LevelText = AddLabel(CachedHost, C.Box, TextX + TagWidth, 19, RateWidth);
        C.HeroTag = AddLabel(CachedHost, C.Box, TextX + TagWidth + RateWidth, 19, TagWidth);
        C.HeroText = AddLabel(CachedHost, C.Box, TextX + 2 * TagWidth + RateWidth, 19, RateWidth);
        C.KillsText = AddLabel(CachedHost, C.Box, static_cast<int16_t>(TextX + 2 * (TagWidth + RateWidth)), 19,
                               static_cast<int16_t>(TextWidth - 2 * (TagWidth + RateWidth)), 2);
        C.Line3 = AddLabel(CachedHost, C.Box, TextX, 35, static_cast<int16_t>(TextWidth - 210));
        C.Compare = AddLabel(CachedHost, C.Box, static_cast<int16_t>(TextX + TextWidth - 206), 35, 206, 2);
        C.Bar = AddBar(C.Box, TextX, 53, TextWidth, 4, BarGold, 0.0f);
        if (Clickable) {
            C.Box->isInteractable = true;
            C.Hit = AddHitArea(CachedHost, C.Box, 0, 0, Width, CardHeight);
        }
        return C;
    }

    void FillCard(Card& C, const ExpRecord& R, const bool Live, const int Index) {
        const int16_t Width = static_cast<int16_t>(C.Box ? C.Box->rect.right - C.Box->rect.left : 0);
        const auto TextWidth = static_cast<int16_t>(Width - 56);
        const double Mine = Rate(GainedXp(R), R.ActiveMs);
        const auto Stats = StatsByMap(Index);
        const auto Found = Stats.find(MainMap(R));
        const RateTrack* Here = nullptr;
        if (Found != Stats.end()) Here = HeroSession(R) ? &Found->second.Hero : &Found->second.Level;
        const double Average = Here && Here->Ms > 0 ? Rate(Here->Gained, Here->Ms) : 0.0;
        const bool Comparable = Here && Here->Count > 0 && Average > 0 && R.ActiveMs >= CompareMinMs;
        const double Diff = Comparable ? Mine / Average - 1.0 : 0.0;
        const bool Best = Comparable && Mine >= Here->Best;

        Color StripTint = StripPlain;
        if (Live) StripTint = Gold;
        else if (Comparable && Diff >= 0.1) StripTint = StripGood;
        else if (Comparable && Diff <= -0.1) StripTint = StripBad;
        SetColor(C.Strip, StripTint);

        SetText(C.Name, MapList(R), Plain, TextWidth - 236);
        const std::wstring State = Live ? (currentSessionState == RUNNING ? L"Running " : L"Paused ") : L"";
        SetText(C.Time, State + Duration(R.ActiveMs), Live ? Gold : Plain, 110);
        SetText(C.When, Ago(R.Start), Hint, 110);
        SetText(C.LevelTag, L"Level", LevelColor, TagWidth);
        SetText(C.LevelText, Levels(R.LevelGained) + L" (" + PerHour(R.LevelGained, R.ActiveMs) + L"/h)", Plain, RateWidth);
        SetText(C.HeroTag, L"Hero", HeroColor, TagWidth);
        SetText(C.HeroText, Levels(R.HeroGained) + L" (" + PerHour(R.HeroGained, R.ActiveMs) + L"/h)", Plain, RateWidth);
        SetText(C.KillsText, KillsLine(R), Plain, TextWidth - 2 * (TagWidth + RateWidth));
        SetText(C.Line3, L"Gold " + Compact(R.Gold) + L" · " + Thousands(ItemCount(R.Picked)) + L" picked up · "
                         + Thousands(ItemCount(R.Used)) + L" used · died " + std::to_wstring(R.Deaths) + L"x",
                Hint, TextWidth - 210);
        if (!Comparable) SetText(C.Compare, L"", Hint, 206);
        else if (Best) SetText(C.Compare, L"Best on this map", Gold, 206);
        else if (std::abs(Diff) < 0.1) SetText(C.Compare, L"About your average here", Hint, 206);
        else SetText(C.Compare, std::format(L"{:+.0f}% vs your average here", Diff * 100), Diff > 0 ? Good : Bad, 206);
        const double Peak = Here && Here->Count > 0 ? std::max(Here->Best, Mine) : Mine;
        SetFill(C.Bar, Peak > 0 ? static_cast<float>(Mine / Peak) : 0.0f, Best || (Live && Mine >= Peak) ? BarGold : BarBlue);
        WidgetKit::SetItemIcon(C.Icon, SpIcon(R));
    }

    void FillMapPicker() {
        PickerFrom = Sessions.size();
        if (!MapPicker) return;
        std::vector<std::wstring> Maps;
        for (const ExpRecord& R : Sessions) {
            if (!OwnSession(R)) continue;
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
        if (Opened == Index) View = ExpView::List;
        else if (Opened > Index) Opened--;
        PickerFrom = static_cast<size_t>(-1);
        Revision++;
    }

    std::set<long> Claimed;

    void ClaimSessions() {
        if (!localPlayerID || LevCharacter != localPlayerID || currentEXP <= 0 || Claimed.contains(localPlayerID)) return;
        Claimed.insert(localPlayerID);
        auto Fits = [](const ExpRecord& R) {
            if (R.Character || R.LevelFrom <= 0) return false;
            const double Level = currentEXP - (R.LevelFrom + R.LevelGained);
            const double Hero = currentAXP - (R.HeroFrom + R.HeroGained);
            return Level >= -0.0001 && Level < 1 && (R.HeroFrom <= 0 || (Hero >= -0.0001 && Hero < 1));
        };
        bool Changed = false;
        for (ExpRecord& R : Sessions) {
            if (!Fits(R)) continue;
            R.Character = localPlayerID;
            Changed = true;
        }
        if (HasCurrent && Fits(Current)) Current.Character = localPlayerID;
        if (Changed) {
            SaveAll();
            PickerFrom = static_cast<size_t>(-1);
            Revision++;
        }
    }

    void HideGrid(Table& Grid) {
        Grid.SetColumns({});
        Grid.ClearRows();
        Grid.SetActive(false);
    }

    std::wstring ShownTitles[3];

    void ShowDetails(const bool Visible) {
        Show(HeaderCard.Box, Visible);
        Show(BackButton.Button, Visible);
        Show(MapCaption, Visible);
        Show(MapBackground, Visible);
        if (!Visible) Show(MapImage, false);
        if (!Visible) Show(NextMapButton, false);
        if (!Visible) {
            for (TEWControlWidget* Cell : Heat) Show(Cell, false);
        }
        for (TEWGuageWidget* Bar : DamageBars) Show(Bar, false);
        for (ItemList* List : {&PickedList, &UsedList}) {
            for (TNTIconWidget* Icon : List->Icons) WidgetKit::SetItemIcon(Icon, -1);
        }
        HideGrid(DamageTable);
        HideGrid(PickedList.Grid);
        HideGrid(UsedList.Grid);
        for (std::wstring& Title : ShownTitles) Title.clear();
    }

    void ShowControls() {
        const bool Details = View == ExpView::Details;
        for (TLBSWidget* Widget : Side) Show(Widget, !Details);
        if (MapPicker) Show(MapPicker->Widget(), !Details);
        Show(Heading, !Details);
        for (Card& C : Cards) Show(C.Box, View == ExpView::List);
        WidgetKit::SetTabActive(ListButton, View == ExpView::List);
        WidgetKit::SetTabActive(StatsButton, View == ExpView::Stats);
        if (View != ExpView::Stats) HideGrid(StatsGrid);
        ShowDetails(Details);
    }

    void Go(const ExpView To) {
        View = To;
        PageIndex = 0;
        ShowControls();
        Dirty = true;
    }

    void RefreshSide(const ExpRecord& Live) {
        const int16_t W = SideWidth;
        SetText(SideLines[SessionTitle], L"Current session", Gold, W);
        if (!HasCurrent) {
            SetText(SideLines[SessionState], L"Not running", Hint, W);
            for (int i = SessionLevel; i <= SessionGold; i++) SetText(SideLines[i], L"", Hint, W);
        } else {
            const bool Running = currentSessionState == RUNNING;
            SetText(SideLines[SessionState], (Running ? L"Running · " : L"Paused · ") + Duration(Live.ActiveMs), Running ? Good : Gold, W);
            SetText(SideLines[SessionLevel], L"Level " + Levels(Live.LevelGained) + L" (" + PerHour(Live.LevelGained, Live.ActiveMs) + L"/h)", Plain, W);
            SetText(SideLines[SessionHero], L"Hero " + Levels(Live.HeroGained) + L" (" + PerHour(Live.HeroGained, Live.ActiveMs) + L"/h)", Plain, W);
            SetText(SideLines[SessionKills], Thousands(KillCount(Live)) + L" kills", Plain, W);
            SetText(SideLines[SessionFull], FullXpText(Live.FullKills, Live.PartyKills), Plain, W);
            SetText(SideLines[SessionGold], L"Gold " + Compact(Live.Gold), Plain, W);
        }
        if (StartButton.Caption) {
            const wchar_t* Caption = currentSessionState == RUNNING ? L"Pause" : currentSessionState == PAUSED ? L"Continue" : L"Start";
            StartButton.Caption->SetText(Caption);
        }

        const int64_t Today = LocalStart(false), Week = LocalStart(true);
        int64_t TodayMs = 0, WeekMs = 0;
        double TodayLevels = 0, WeekLevels = 0;
        auto Add = [&](const ExpRecord& R) {
            if (R.Start >= Week) { WeekMs += R.ActiveMs; WeekLevels += R.LevelGained; }
            if (R.Start >= Today) { TodayMs += R.ActiveMs; TodayLevels += R.LevelGained; }
        };
        for (const ExpRecord& R : Sessions) {
            if (OwnSession(R)) Add(R);
        }
        if (HasCurrent && OwnSession(Live)) Add(Live);
        SetText(SideLines[TodayTitle], L"Today", Gold, W);
        SetText(SideLines[TodayLine], Duration(TodayMs) + L" · level " + Levels(TodayLevels), TodayMs ? Plain : Hint, W);
        SetText(SideLines[WeekTitle], L"This week", Gold, W);
        SetText(SideLines[WeekLine], Duration(WeekMs) + L" · level " + Levels(WeekLevels), WeekMs ? Plain : Hint, W);
        std::wstring BestMap;
        double BestRate = 0;
        bool BestHero = false;
        for (const auto& [Map, S] : StatsByMap()) {
            const double R = Rate(S.Main().Gained, S.Main().Ms);
            if (S.UsesHero() != BestHero ? S.UsesHero() : R > BestRate) {
                BestRate = R;
                BestMap = Map;
                BestHero = S.UsesHero();
            }
        }
        SetText(SideLines[BestLine], BestMap.empty() ? L"" : L"Best map: " + BestMap, Hint, W);
    }

    void RefreshList(const ExpRecord& Live) {
        std::vector<int> Order;
        if (HasCurrent && OnMap(Live)) Order.push_back(-1);
        for (int i = static_cast<int>(Sessions.size()) - 1; i >= 0; i--) {
            if (OnMap(Sessions[i])) Order.push_back(i);
        }
        int64_t Played = 0;
        int Count = 0;
        const bool Any = std::any_of(Sessions.begin(), Sessions.end(), OwnSession);
        for (const int Index : Order) {
            if (Index < 0) continue;
            Count++;
            Played += Sessions[Index].ActiveMs;
        }
        if (Order.empty()) {
            SetText(Heading, !Any ? L"No sessions yet. Press Start to begin one, Stop saves it here." : L"No session on this map.",
                    Hint, MainWidth);
        } else {
            SetText(Heading, std::to_wstring(Count) + (Count == 1 ? L" session, " : L" sessions, ") + Duration(Played)
                             + (MapFilter.empty() ? L"" : L" on " + MapFilter), Gold, MainWidth);
        }
        const int PageCount = std::max(1, (static_cast<int>(Order.size()) + CardCount - 1) / CardCount);
        PageIndex = std::clamp(PageIndex, 0, PageCount - 1);
        RowTargets.assign(CardCount, -2);
        for (int Slot = 0; Slot < CardCount; Slot++) {
            const int At = PageIndex * CardCount + Slot;
            const bool Used = At < static_cast<int>(Order.size());
            Show(Cards[Slot].Box, Used);
            if (!Used) continue;
            RowTargets[Slot] = Order[At];
            FillCard(Cards[Slot], *SessionAt(Order[At], Live), Order[At] < 0, Order[At]);
        }
        Pages.Update(PageIndex, PageCount);
    }

    void RefreshStats() {
        StatsGrid.SetActive(true);
        StatsGrid.SetColumns({{L"Map", 136}, {L"Sessions", 56, 2}, {L"Time", 60, 2}, {L"Level/h", 60, 2}, {L"Hero/h", 60, 2},
                              {L"Best/h", 60, 2}, {L"Kills/h", 60, 2}, {L"Full XP", 0, 2}});
        auto Stats = StatsByMap();
        std::vector<std::pair<std::wstring, MapStats>> Rows(Stats.begin(), Stats.end());
        std::sort(Rows.begin(), Rows.end(), [](const auto& A, const auto& B) {
            if (A.second.UsesHero() != B.second.UsesHero()) return A.second.UsesHero();
            return Rate(A.second.Main().Gained, A.second.Main().Ms) > Rate(B.second.Main().Gained, B.second.Main().Ms);
        });
        SetText(Heading, Rows.empty() ? L"Sessions of 5 minutes or more show up here, by the map they started on."
                                      : L"By map, best rate first: hero XP where you gained it, level otherwise.",
                Rows.empty() ? Hint : Gold, MainWidth);
        const int PageCount = std::max(1, (static_cast<int>(Rows.size()) + StatsRows - 1) / StatsRows);
        PageIndex = std::clamp(PageIndex, 0, PageCount - 1);
        StatsGrid.ClearRows();
        for (int Slot = 0; Slot < StatsRows; Slot++) {
            const int At = PageIndex * StatsRows + Slot;
            if (At >= static_cast<int>(Rows.size())) continue;
            const auto& [Map, S] = Rows[At];
            const Color& Tint = At == 0 ? Gold : Plain;
            StatsGrid.Set(Slot, 0, Map, Tint);
            StatsGrid.Set(Slot, 1, std::to_wstring(S.Count), Tint);
            StatsGrid.Set(Slot, 2, Duration(S.Ms), Tint);
            StatsGrid.Set(Slot, 3, PerHour(S.LevelGained, S.Ms), S.UsesHero() ? Hint : Tint);
            StatsGrid.Set(Slot, 4, S.UsesHero() ? PerHour(S.Hero.Gained, S.Hero.Ms) : L"-", S.UsesHero() ? Tint : Hint);
            StatsGrid.Set(Slot, 5, std::format(L"{:.2f}%", S.Main().Best * 100), S.UsesHero() ? HeroColor : LevelColor);
            StatsGrid.Set(Slot, 6, Thousands(static_cast<int64_t>(S.Ms > 0 ? S.Kills * 3'600'000.0 / static_cast<double>(S.Ms) : 0)), Tint);
            StatsGrid.Set(Slot, 7, S.PartyKills > 0 ? std::format(L"{:.0f}%", S.FullKills * 100.0 / S.PartyKills) : L"-",
                          S.PartyKills > 0 ? Tint : Hint);
        }
        Pages.Update(PageIndex, PageCount);
    }

    int16_t ItemWidth = 0;

    void SetIcon(TNTIconWidget* Icon, const Table& Grid, const int Slot, const int16_t IconId) {
        if (!Icon) return;
        const int16_t X = Grid.ColumnX(0);
        const auto Y = static_cast<int16_t>(Grid.RowTop(Slot) + 2);
        if (Icon->rect.left != X || Icon->rect.top != Y) Icon->rect = {X, Y, static_cast<int16_t>(X + IconSize), static_cast<int16_t>(Y + IconSize)};
        WidgetKit::SetItemIcon(Icon, IconId);
    }

    void RefreshDamage(const ExpRecord& R) {
        std::vector<std::pair<long, std::pair<std::wstring, int64_t>>> Rows;
        int64_t Total = 0;
        for (const auto& [Id, Entry] : R.DamageBy) {
            if (Entry.first == L"Partner" || Entry.first == L"Pet") continue;
            Rows.push_back({Id, {Entry.first.empty() ? L"#" + std::to_wstring(Id) : Entry.first, Entry.second}});
            Total += Entry.second;
        }
        std::sort(Rows.begin(), Rows.end(), [](const auto& A, const auto& B) { return A.second.second > B.second.second; });
        if (ShownTitles[0].empty()) {
            ShownTitles[0] = L"Damage";
            DamageTable.SetActive(true);
            DamageTable.SetColumns({{L"Damage", 260}, {L"", 110, 2}, {L"Share", 0, 2}});
        }
        DamageTable.ClearRows();
        for (int Slot = 0; Slot < DamageRows; Slot++) {
            const bool Used = Slot < static_cast<int>(Rows.size());
            Show(DamageBars[Slot], Used);
            if (!Used) continue;
            const bool Owner = Rows[Slot].first == R.Character;
            const Color& Tint = Owner ? Gold : Plain;
            DamageTable.Set(Slot, 0, Rows[Slot].second.first, Tint);
            DamageTable.Set(Slot, 1, Compact(Rows[Slot].second.second), Tint);
            DamageTable.Set(Slot, 2, Percent(Rows[Slot].second.second, Total), Owner ? Gold : Hint);
            SetFill(DamageBars[Slot], Total > 0 ? static_cast<float>(Rows[Slot].second.second) / static_cast<float>(Total) : 0.0f,
                    Owner ? BarGold : BarBlue);
        }
        if (Rows.empty()) DamageTable.Set(0, 0, L"No damage recorded.", Hint);
    }

    void FillItems(ItemList& List, std::wstring& ShownTitle, const wchar_t* Name, std::vector<std::pair<int, int64_t>> Items,
                   const bool GoldFirst, const wchar_t* Empty) {
        std::stable_sort(Items.begin() + (GoldFirst ? 1 : 0), Items.end(), [](const auto& A, const auto& B) { return A.second > B.second; });
        List.Total = static_cast<int>(Items.size());
        List.Scroll = std::clamp(List.Scroll, 0, std::max(0, List.Total - ItemRows));
        std::wstring Title = Name;
        if (List.Total > ItemRows) {
            Title += L"  " + std::to_wstring(List.Scroll + 1) + L"-" + std::to_wstring(std::min(List.Total, List.Scroll + ItemRows)) + L" of "
                   + std::to_wstring(List.Total);
        }
        if (Title != ShownTitle) {
            ShownTitle = Title;
            List.Grid.SetActive(true);
            List.Grid.SetColumns({{Title.c_str(), static_cast<int16_t>(ItemWidth - 74), 1, IconSize + 4}, {L"", 0, 2}});
        }
        List.Grid.ClearRows();
        for (int Slot = 0; Slot < ItemRows; Slot++) {
            const int At = List.Scroll + Slot;
            const bool Used = At < List.Total;
            SetIcon(List.Icons[Slot], List.Grid, Slot, Used ? GameData::ItemIcon(Items[At].first) : static_cast<int16_t>(-1));
            if (!Used) continue;
            const bool IsGold = GoldFirst && At == 0;
            std::wstring ItemName = IsGold ? L"Gold" : GameData::ItemName(Items[At].first);
            if (ItemName.empty()) ItemName = L"Item #" + std::to_wstring(Items[At].first);
            List.Grid.Set(Slot, 0, ItemName, IsGold ? Gold : Plain);
            List.Grid.Set(Slot, 1, IsGold ? Compact(Items[At].second) : Thousands(Items[At].second), IsGold ? Gold : Plain);
        }
        if (!List.Total) List.Grid.Set(0, 0, Empty, Hint);
    }

    void RefreshKillMap(const ExpRecord& R) {
        std::map<int, int> PerMap;
        for (const auto& [Spot, Count] : R.Spots) PerMap[Spot[0]] += Count;
        std::vector<std::pair<int, int>> Maps(PerMap.begin(), PerMap.end());
        std::sort(Maps.begin(), Maps.end(), [](const auto& A, const auto& B) { return A.second > B.second; });
        Show(NextMapButton, Maps.size() > 1);
        if (Maps.empty()) {
            SetText(MapCaption, L"Kill map: no kill spots recorded", Hint, MapSize);
            Show(MapImage, false);
            for (TEWControlWidget* Cell : Heat) Show(Cell, false);
            return;
        }
        const int MapCount = static_cast<int>(Maps.size());
        MapIndex = (MapIndex % MapCount + MapCount) % MapCount;
        const int MapId = Maps[MapIndex].first;
        LoadMapSizes();
        const auto Known = MapSizes.find(MapId);
        const bool HasSize = Known != MapSizes.end();

        int OffsetX = 0, OffsetY = 0, SpanX = 1, SpanY = 1;
        if (HasSize) {
            SpanX = std::max(1, (Known->second.first + MapCells - 1) / MapCells);
            SpanY = std::max(1, (Known->second.second + MapCells - 1) / MapCells);
        } else {
            int MinX = INT_MAX, MinY = INT_MAX, MaxX = INT_MIN, MaxY = INT_MIN;
            for (const auto& [Spot, Count] : R.Spots) {
                if (Spot[0] != MapId) continue;
                MinX = std::min(MinX, Spot[1]);
                MaxX = std::max(MaxX, Spot[1]);
                MinY = std::min(MinY, Spot[2]);
                MaxY = std::max(MaxY, Spot[2]);
            }
            SpanX = SpanY = std::max({1, (MaxX - MinX + MapCells) / MapCells, (MaxY - MinY + MapCells) / MapCells});
            OffsetX = MinX - (MapCells * SpanX - (MaxX - MinX + 1)) / 2;
            OffsetY = MinY - (MapCells * SpanY - (MaxY - MinY + 1)) / 2;
        }
        if (MapImage) {
            const int32_t Picture = HasSize ? MinimapImage(MapId) : 0;
            Show(MapImage, Picture != 0);
            if (Picture && MapImage->imageData.imageName != Picture) MapImage->imageData.imageName = Picture;
        }
        std::vector<int> Counts(MapCells * MapCells, 0);
        int Hottest = 0;
        for (const auto& [Spot, Count] : R.Spots) {
            if (Spot[0] != MapId) continue;
            const int Col = std::clamp((Spot[1] - OffsetX) / SpanX, 0, MapCells - 1);
            const int Row = std::clamp((Spot[2] - OffsetY) / SpanY, 0, MapCells - 1);
            Hottest = std::max(Hottest, Counts[Row * MapCells + Col] += Count);
        }
        std::wstring Name = MapLabel(MapId);
        if (Name.empty()) Name = L"Map #" + std::to_wstring(MapId);
        SetText(MapCaption, Name + L", " + Thousands(Maps[MapIndex].second) + L" kills", Gold, MapSize - 86);
        std::vector<int> Hot;
        for (int i = 0; i < MapCells * MapCells; i++) {
            if (Counts[i] > 0) Hot.push_back(i);
        }
        std::sort(Hot.begin(), Hot.end(), [&](const int A, const int B) { return Counts[A] > Counts[B]; });
        for (int Slot = 0; Slot < HeatCount; Slot++) {
            TEWControlWidget* Cell = Heat[Slot];
            if (!Cell) continue;
            const bool Used = Slot < static_cast<int>(Hot.size());
            Show(Cell, Used);
            if (!Used) continue;
            const int At = Hot[Slot];
            const auto X = static_cast<int16_t>(MapLeft + At % MapCells * CellSize);
            const auto Y = static_cast<int16_t>(MapTop + At / MapCells * CellSize);
            if (Cell->rect.left != X || Cell->rect.top != Y) Cell->rect = {X, Y, static_cast<int16_t>(X + CellSize), static_cast<int16_t>(Y + CellSize)};
            const float T = std::sqrt(static_cast<float>(Counts[At]) / static_cast<float>(Hottest));
            const float Share = static_cast<float>(Counts[At]) / static_cast<float>(Hottest);
            SetColor(Cell, Color(static_cast<uint8_t>(5 + 215 * Share), 255, static_cast<uint8_t>(230 - 180 * T), static_cast<uint8_t>(90 - 60 * T)));
        }
    }

    void RefreshDetails(const ExpRecord& Live) {
        const ExpRecord* Found = SessionAt(Opened, Live);
        if (!Found) {
            Go(ExpView::List);
            return;
        }
        const ExpRecord& R = *Found;
        FillCard(HeaderCard, R, Opened < 0, Opened);
        std::wstring Sps;
        for (const int Morph : R.Morphs) Sps += (Sps.empty() ? L"" : L", ") + SpName(Morph);
        SetText(HeaderCard.Line3, L"SP: " + (Sps.empty() ? std::wstring(L"-") : Sps) + L" · gold " + Compact(R.Gold) + L" · died "
                                  + std::to_wstring(R.Deaths) + L"x", Hint, FullWidth - 56 - 210);
        SetText(HeaderCard.When, Opened < 0 ? Ago(R.Start) : WhenWithYear(R.Start), Hint, 110);

        RefreshDamage(R);
        std::vector<std::pair<int, int64_t>> Picked;
        if (R.Gold) Picked.emplace_back(GoldVnum, R.Gold);
        Picked.insert(Picked.end(), R.Picked.begin(), R.Picked.end());
        FillItems(PickedList, ShownTitles[1], L"Picked up", std::move(Picked), R.Gold > 0, L"Nothing picked up.");
        FillItems(UsedList, ShownTitles[2], L"Used", {R.Used.begin(), R.Used.end()}, false, L"Nothing used.");
        RefreshKillMap(R);
        Pages.Update(0, 1);
    }

    void RefreshPage() {
        const ExpRecord Live = Snapshot();
        if (PickerFrom != Sessions.size()) FillMapPicker();
        if (View == ExpView::Details) {
            RefreshDetails(Live);
            return;
        }
        RefreshSide(Live);
        if (View == ExpView::Stats) RefreshStats();
        else RefreshList(Live);
    }

    void CloseMenu() {
        if (Menu) Menu->isVisible = false;
        MenuSession = -2;
        ConfirmDelete = false;
    }

    void OpenDetails(const int Index) {
        Opened = Index;
        MapIndex = 0;
        PickedList.Scroll = UsedList.Scroll = 0;
        Go(ExpView::Details);
    }

    void __cdecl OnMenu(void* Argument) {
        const int Item = static_cast<int>(reinterpret_cast<intptr_t>(Argument));
        const int Index = MenuSession;
        if (Item == MenuOpen) {
            CloseMenu();
            OpenDetails(Index);
        } else if (Item == MenuDelete) {
            if (!ConfirmDelete) {
                ConfirmDelete = true;
                if (MenuButtons[MenuDelete]) MenuButtons[MenuDelete]->SetCaption(L"Click again to delete");
                return;
            }
            CloseMenu();
            Remove(Index);
            Dirty = true;
        }
    }

    void OpenMenu(const int Index, const TickContext& Context) {
        if (!Menu || !PageWidget) return;
        int32_t X = 0, Y = 0;
        for (const TLBSWidget* At = PageWidget; At; At = At->parent) {
            X += At->rect.left;
            Y += At->rect.top;
        }
        const int Count = Index >= 0 ? MenuCount : MenuDelete;
        const auto Height = static_cast<int16_t>(Count * TEWButtonWidget::RowHeight);
        const auto MenuLeft = static_cast<int16_t>(std::min<int32_t>(Context.mouseX - X, PageWidget->rect.right - PageWidget->rect.left - MenuWidth));
        const auto MenuTop = static_cast<int16_t>(Context.mouseY - Y);
        Menu->rect = {MenuLeft, MenuTop, static_cast<int16_t>(MenuLeft + MenuWidth), static_cast<int16_t>(MenuTop + Height)};
        const wchar_t* Captions[MenuCount] = {L"Open details", L"Delete session"};
        for (int i = 0; i < MenuCount; i++) {
            if (!MenuButtons[i]) continue;
            MenuButtons[i]->SetCaption(Captions[i]);
            MenuButtons[i]->isVisible = i < Count;
        }
        MenuSession = Index;
        ConfirmDelete = false;
        Menu->isVisible = true;
        Menu->BubbleUp();
    }

    void BuildMenu(TLBSWidget* Page) {
        Menu = Widget::Create<TLBSWidget>(CachedHost);
        if (!Menu) return;
        Menu->isVisible = false;
        Attach(Page, Menu);
        for (int i = 0; i < MenuCount; i++) {
            TEWButtonWidget* Button = Widget::Create<TEWButtonWidget>(CachedHost);
            if (!Button) continue;
            Button->rect.left = 0;
            Button->rect.top = static_cast<int16_t>(i * TEWButtonWidget::RowHeight);
            Button->SetWidth(MenuWidth);
            WidgetKit::SetOnClick(Button, &OnMenu, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            Attach(Menu, Button);
            MenuButtons[i] = Button;
        }
    }

    int CardAt() {
        if (View != ExpView::List) return -1;
        for (int Slot = 0; Slot < CardCount; Slot++) {
            if (Cards[Slot].Box && Cards[Slot].Box->isVisible && Hovered(Cards[Slot].Hit)) return Slot;
        }
        return -1;
    }

    void __cdecl OnToggleWindow(void*) {
        if (!sessionTracker) return;
        const bool Shown = !(sessionTracker->isVisible || tempInvisible);
        tempInvisible = false;
        sessionTracker->isVisible = Shown;
        if (Shown) sessionTracker->BubbleUp();
    }

    void __cdecl OnStartPause(void*) {
        if (!sessionTracker) return;
        ToggleSessionImpl(toggleSessionButton);
        Revision++;
        Dirty = true;
    }

    void __cdecl OnStop(void*) {
        if (!sessionTracker) return;
        ResetSessionImpl(resetSessionButton);
        Dirty = true;
    }

    void __cdecl OnView(void* Argument) {
        Go(Argument ? ExpView::Stats : ExpView::List);
    }

    void __cdecl OnBack(void*) {
        Go(ExpView::List);
    }

    void __cdecl OnNextMap(void*) {
        MapIndex++;
        Dirty = true;
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

    void OnMonsterHit(const std::vector<std::string>& T) {
        const auto Target = static_cast<long>(Num(T[4]));
        const int64_t MaxHp = Num(T[17]);
        if (MaxHp <= 0) return;
        const bool Alive = T[11] != "0";
        MonsterHits& M = Hits[Target];
        if (M.MaxHp != MaxHp) M = {MaxHp, MaxHp};
        const int64_t Damage = std::max<int64_t>(0, Num(T[13]));
        const int64_t Lost = Alive ? Damage : std::min(Damage, M.Hp);
        M.Hp = std::clamp<int64_t>(Num(T[16]), 0, MaxHp);
        const auto User = static_cast<long>(Num(T[2]));
        const bool Mine = T[1] == "1" ? User == localPlayerID : T[1] == "2" && MyMates.contains(User);
        if (Mine) M.Mine += Lost;
        if (Mine || (T[1] == "1" && PartyNames.contains(User))) M.Party = true;
        if (Alive) return;
        if (M.Party && currentSessionState == RUNNING && HasCurrent) {
            Current.PartyKills++;
            if (static_cast<double>(M.Mine) > FullXpShare * static_cast<double>(MaxHp)) Current.FullKills++;
            if (const auto At = MonsterAt.find(Target); At != MonsterAt.end() && CurrentMapId >= 0) {
                Current.Spots[{CurrentMapId, At->second.first, At->second.second}]++;
            }
            Revision++;
        }
        Hits.erase(Target);
        MonsterAt.erase(Target);
    }

    void OnRawPacket(const char* Packet, const bool IsSend) {
        if (!Packet) return;
        const auto T = Split(Packet, ' ');
        if (T.empty()) return;
        const std::string& Header = T[0];
        const bool Running = currentSessionState == RUNNING && HasCurrent;
        const int64_t Now = NowMs();
        if (IsSend) {
            if (Header == "u_i" && T.size() >= 5) LastUse[static_cast<int>(Num(T[3]) * 1000 + Num(T[4]))] = Now;
            return;
        }
        if (Header == "c_mode" && T.size() >= 4 && Num(T[2]) == localPlayerID) {
            SelfMorph = static_cast<int>(Num(T[3]));
            if (Running) {
                Note(Current.Morphs, SelfMorph);
                Revision++;
            }
        } else if (Header == "c_map" && T.size() >= 3) {
            CurrentMapId = static_cast<int>(Num(T[2]));
            Drops.clear();
            MonsterAt.clear();
            Hits.clear();
        } else if (Header == "su" && T.size() >= 18 && T[3] == "3") {
            OnMonsterHit(T);
        } else if (Header == "in" && T.size() >= 6 && T[1] == "3") {
            MonsterAt[static_cast<long>(Num(T[3]))] = {static_cast<int>(Num(T[4])), static_cast<int>(Num(T[5]))};
        } else if (Header == "mv" && T.size() >= 5 && T[1] == "3") {
            MonsterAt[static_cast<long>(Num(T[2]))] = {static_cast<int>(Num(T[3])), static_cast<int>(Num(T[4]))};
        } else if (Header == "out" && T.size() >= 3 && T[1] == "3") {
            MonsterAt.erase(static_cast<long>(Num(T[2])));
            Hits.erase(static_cast<long>(Num(T[2])));
        } else if (Header == "in" && T.size() >= 7 && T[1] == "9") {
            Drops[static_cast<long>(Num(T[3]))] = {static_cast<int>(Num(T[2])), Num(T[6])};
        } else if (Header == "drop" && T.size() >= 6) {
            Drops[static_cast<long>(Num(T[2]))] = {static_cast<int>(Num(T[1])), Num(T[5])};
        } else if (Header == "throw" && T.size() >= 8) {
            Drops[static_cast<long>(Num(T[2]))] = {static_cast<int>(Num(T[1])), Num(T[7])};
        } else if (Header == "get" && T.size() >= 4 && (T[1] == "1" || T[1] == "2")) {
            const auto Found = Drops.find(static_cast<long>(Num(T[3])));
            if (Found == Drops.end()) return;
            const long Picker = static_cast<long>(Num(T[2]));
            const bool Mine = T[1] == "1" ? Picker == localPlayerID : MyMates.contains(Picker);
            if (Running && Mine) {
                if (Found->second.first == GoldVnum) Current.Gold += Found->second.second;
                else Current.Picked[Found->second.first] += Found->second.second;
                Revision++;
            }
            Drops.erase(Found);
        } else if ((Header == "ivn" || Header == "inv") && T.size() >= 3 && T[1] != "0") {
            const int Type = static_cast<int>(Num(T[1]));
            for (size_t i = 2; i < T.size(); i++) {
                const auto Parts = Split(T[i], '.');
                if (Parts.size() < 3) continue;
                const int Key = Type * 1000 + static_cast<int>(Num(Parts[0]));
                const int Vnum = static_cast<int>(Num(Parts[1]));
                const int64_t Amount = Num(Parts[2]);
                const auto Old = Bag.find(Key);
                if (Header == "ivn" && Running && Old != Bag.end()) {
                    const auto Use = LastUse.find(Key);
                    const bool JustUsed = Use != LastUse.end() && Now - Use->second < 3000;
                    if (Vnum == Old->second.first && Amount < Old->second.second) {
                        Current.Used[Vnum] += Old->second.second - Amount;
                        Revision++;
                    } else if (Vnum != Old->second.first && JustUsed) {
                        Current.Used[Old->second.first] += Old->second.second;
                        Revision++;
                    }
                } else if (Header == "ivn" && Running && Vnum > 0) {
                    const auto Use = LastUse.find(Key);
                    if (Use != LastUse.end() && Now - Use->second < 3000) {
                        Current.Used[Vnum]++;
                        Revision++;
                    }
                }
                if (Vnum > 0 && Amount > 0) Bag[Key] = {Vnum, Amount};
                else Bag.erase(Key);
            }
        } else if (Header == "pinit" && T.size() >= 2) {
            PartyNames.clear();
            MyMates.clear();
            for (size_t i = 2; i < T.size(); i++) {
                const auto Fields = Split(T[i], '|');
                if (Fields.size() < 5) continue;
                const long Id = static_cast<long>(Num(Fields[1]));
                if (Id == localPlayerID) continue;
                if (Fields[0] == "2") MyMates.insert(Id);
                else if (Fields[0] == "1") PartyNames[Id] = Wide(Fields[4]);
            }
            for (auto It = partyDamages.begin(); It != partyDamages.end();) {
                if (It->first != localPlayerID && !PartyNames.contains(It->first)) It = partyDamages.erase(It);
                else ++It;
            }
            for (const auto& [Id, Name] : PartyNames) {
                auto& Bar = partyDamages[static_cast<int>(Id)];
                Bar.playerName = Name;
            }
            UpdateDamageDisplay();
        }
    }

    WidgetKit::WindowRaiser WindowRaiser;

    void Tick(TLBSWidget* RootWidget, const TickContext tickContext) {
        WindowRaiser.Tick(sessionTracker, tickContext);
        if (tickContext.isPlayerLoaded) NoteMapSize(tickContext.mapId, tickContext.mapWidth, tickContext.mapHeight);
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
        MainWidth = static_cast<int16_t>(Width - MainX - 12);
        FullWidth = static_cast<int16_t>(Width - 2 * SideX);
        Side.clear();

        Heading = AddLabel(CachedHost, Page, MainX, Top + 4, MainWidth);
        const int16_t CardsBottom = Top + 26 + CardCount * CardHeight;
        for (int Slot = 0; Slot < CardCount; Slot++) {
            Cards[Slot] = BuildCard(MainX, static_cast<int16_t>(Top + 26 + Slot * CardHeight), MainWidth, true);
        }
        StatsGrid.Build(CachedHost, Page, MainX, Top + 26, MainWidth, StatsRows, RowHeight, 8);

        BackButton = AddButton(static_cast<int16_t>(SideX + FullWidth - 80), Top, 80, L"Back", &OnBack, 0);
        HeaderCard = BuildCard(SideX, Top + 30, FullWidth, false);
        const int16_t ContentY = Top + 30 + CardHeight + 6;
        const auto MapX = static_cast<int16_t>(SideX + FullWidth - MapSize);
        const auto LeftWidth = static_cast<int16_t>(FullWidth - MapSize - 20);
        DamageTable.Build(CachedHost, Page, SideX, ContentY, LeftWidth, DamageRows, ListStep, 3);
        for (int i = 0; i < DamageRows; i++) {
            DamageBars[i] = AddBar(Page, SideX, static_cast<int16_t>(DamageTable.RowTop(i) + ListStep - 3), LeftWidth, 2, BarBlue, 0.0f);
        }
        const auto ListsY = static_cast<int16_t>(DamageTable.Bottom() + 8);
        ItemWidth = static_cast<int16_t>((LeftWidth - 16) / 2);
        PickedList.Grid.Build(CachedHost, Page, SideX, ListsY, ItemWidth, ItemRows, ListStep, 2);
        UsedList.Grid.Build(CachedHost, Page, static_cast<int16_t>(SideX + ItemWidth + 16), ListsY, ItemWidth, ItemRows, ListStep, 2);
        for (ItemList* List : {&PickedList, &UsedList}) {
            for (TNTIconWidget*& Icon : List->Icons) {
                Icon = WidgetKit::CreateItemIcon(CachedHost, 0, 0, IconSize, -1);
                Attach(Page, Icon);
            }
        }
        MapCaption = AddLabel(CachedHost, Page, MapX, ContentY, static_cast<int16_t>(MapSize - 86));
        NextMapButton = Widget::Create<TEWButtonWidget>(CachedHost);
        if (NextMapButton) {
            NextMapButton->rect.left = static_cast<int16_t>(MapX + MapSize - 80);
            NextMapButton->rect.top = ContentY;
            NextMapButton->SetWidth(80);
            NextMapButton->SetCaption(L"Next map");
            WidgetKit::SetOnClick(NextMapButton, &OnNextMap, nullptr);
            Attach(Page, NextMapButton);
        }
        MapLeft = MapX;
        MapTop = static_cast<int16_t>(ContentY + 20);
        MapBackground = AddRect(CachedHost, Page, MapX, MapTop, MapSize, MapSize, MapBack);
        MapImage = AddRect(CachedHost, Page, MapX, MapTop, MapSize, MapSize, Color(255, 255, 255, 255));
        if (MapImage) {
            delete[] MapImage->imageData.atlasFrames;
            MapImage->imageData.imageName = 0;
            MapImage->imageData.imageWidth = MinimapImageSize;
            MapImage->imageData.imageHeight = MinimapImageSize;
            MapImage->imageData.frameCount = 1;
            MapImage->imageData.atlasFrames = new AtlasFrame[1]{{0, 0, MinimapImageSize, MinimapImageSize}};
            Show(MapImage, false);
        }
        for (TEWControlWidget*& Cell : Heat) {
            Cell = AddRect(CachedHost, Page, MapX, MapTop, CellSize, CellSize, NoHeat);
            Show(Cell, false);
        }
        const auto Bottom = static_cast<int16_t>(std::max<int>({CardsBottom, StatsGrid.Bottom(), PickedList.Grid.Bottom(), MapTop + MapSize}));
        Pages.Build(CachedHost, Page, static_cast<int16_t>(MainX + MainWidth / 2), static_cast<int16_t>(Bottom + 6));

        Side.push_back(AddRect(CachedHost, Page, static_cast<int16_t>(SideX + SideWidth + 6), Top, 1, static_cast<int16_t>(Bottom - Top + 20), RuleColor));
        int16_t Y = Top;
        auto AddSideLine = [&](const SideLine Line) {
            SideLines[Line] = AddLabel(CachedHost, Page, SideX, Y, SideWidth);
            Side.push_back(SideLines[Line]);
            Y += 16;
        };
        for (int Line = SessionTitle; Line <= SessionGold; Line++) AddSideLine(static_cast<SideLine>(Line));
        Y += 4;
        StartButton = AddButton(SideX, Y, 77, L"Start", &OnStartPause, 0);
        StopButton = AddButton(SideX + 83, Y, 77, L"Stop", &OnStop, 0);
        Y += 30;
        ToggleButton = AddButton(SideX, Y, SideWidth, L"Show / hide window", &OnToggleWindow, 0);
        Y += 32;
        Side.insert(Side.end(), {StartButton.Button, StopButton.Button, ToggleButton.Button});
        Side.push_back(AddRect(CachedHost, Page, SideX, Y, SideWidth, 1, RuleColor));
        Y += 8;
        for (int Line = TodayTitle; Line <= BestLine; Line++) {
            if (Line == WeekTitle || Line == BestLine) Y += 4;
            AddSideLine(static_cast<SideLine>(Line));
        }
        Y += 8;
        TEWLabel* MapName = AddLabel(CachedHost, Page, SideX, Y, SideWidth);
        SetText(MapName, L"Map", Hint, SideWidth);
        Side.push_back(MapName);
        const int16_t PickerY = static_cast<int16_t>(Y + 16);
        Y += 44;
        ListButton = AddButton(SideX, Y, 77, L"Sessions", &OnView, 0);
        StatsButton = AddButton(SideX + 83, Y, 77, L"By map", &OnView, 1);
        Side.insert(Side.end(), {ListButton.Button, StatsButton.Button});

        WidgetKit::Dropdown::Desc Desc;
        Desc.X = SideX;
        Desc.Y = PickerY;
        Desc.Width = SideWidth - 17;
        Desc.Placeholder = L"All maps";
        Desc.OnPick = &OnPickMap;
        MapPicker = WidgetKit::Dropdown::Create(CachedHost, Desc);
        if (MapPicker) Attach(Page, MapPicker->Widget());

        BuildMenu(Page);
        View = ExpView::List;
        PickerFrom = static_cast<size_t>(-1);
        ShowControls();
        Dirty = true;
        return static_cast<int16_t>(Bottom + 30);
    }

    void ForgetPage() {
        PageWidget = nullptr;
        Heading = nullptr;
        for (auto& Line : SideLines) Line = nullptr;
        for (auto& C : Cards) C = {};
        HeaderCard = {};
        for (auto& Bar : DamageBars) Bar = nullptr;
        for (ItemList* List : {&PickedList, &UsedList}) {
            for (auto& Icon : List->Icons) Icon = nullptr;
            List->Grid.Forget();
        }
        for (auto& Cell : Heat) Cell = nullptr;
        MapBackground = nullptr;
        MapImage = nullptr;
        MapCaption = nullptr;
        NextMapButton = nullptr;
        Menu = nullptr;
        for (auto& Button : MenuButtons) Button = nullptr;
        MenuSession = -2;
        DamageTable.Forget();
        StatsGrid.Forget();
        Pages.Forget();
        StartButton = StopButton = ToggleButton = ListButton = StatsButton = BackButton = {};
        MapPicker = nullptr;
        Side.clear();
        if (HintShown) GameHint::Hide();
        HintShown = false;
    }

    void ResetView() {
        CloseMenu();
        if (View == ExpView::Details) Go(ExpView::List);
    }

    long ShownCharacter = 0;

    void TickPage(TLBSWidget* Root, const TickContext& Context) {
        if (!PageWidget) return;
        ClaimSessions();
        if (localPlayerID != ShownCharacter) {
            ShownCharacter = localPlayerID;
            PickerFrom = static_cast<size_t>(-1);
            if (View == ExpView::Details) Go(ExpView::List);
            Dirty = true;
        }
        if (MapPicker) MapPicker->Tick(Context);
        const bool PickerOpen = MapPicker && MapPicker->IsOpen();
        const int Hover = PickerOpen ? -1 : CardAt();
        if (Context.mouseWheel != 0 && !PickerOpen) {
            const int Step = Context.mouseWheel > 0 ? -1 : 1;
            if (View == ExpView::Details) {
                for (ItemList* List : {&PickedList, &UsedList}) {
                    if (List->Grid.Contains(Context)) {
                        List->Scroll += Step * 3;
                        Dirty = true;
                    }
                }
            } else if (View == ExpView::List ? Hover >= 0 : StatsGrid.Contains(Context)) {
                PageIndex += Step;
                Dirty = true;
            }
        }

        std::vector<std::wstring> HintLines;
        if (Hover >= 0 && Cards[Hover].When && Over(Cards[Hover].When, 110, 16, Context)) {
            const ExpRecord Snap = Snapshot();
            if (const ExpRecord* R = SessionAt(RowTargets[Hover], Snap)) HintLines = {WhenWithYear(R->Start)};
        }
        if (!HintLines.empty() && GameHint::Create(CachedHost, Root)) {
            GameHint::Show(HintLines, Context.mouseX, Context.mouseY);
            HintShown = true;
        } else if (HintShown) {
            GameHint::Hide();
            HintShown = false;
        }

        const bool MenuShown = Menu && Menu->isVisible;
        bool OverMenu = false;
        for (const TEWButtonWidget* Button : MenuButtons) {
            if (MenuShown && Button && Button->isVisible && Button->hovered) OverMenu = true;
        }
        if (MenuShown && (View != ExpView::List || (GetAsyncKeyState(VK_ESCAPE) & 0x8000))) CloseMenu();
        if (RightClick.Pressed() && !PickerOpen) {
            if (Hover >= 0 && RowTargets[Hover] != -2) OpenMenu(RowTargets[Hover], Context);
            else CloseMenu();
        }
        const bool Pressed = Click.Pressed();
        if (MenuShown) {
            if (Pressed && !OverMenu) CloseMenu();
        } else if (Pressed && !PickerOpen) {
            if (const int Step = Pages.Clicked(Context)) {
                PageIndex += Step;
                Dirty = true;
            } else if (Hover >= 0 && RowTargets[Hover] != -2) {
                OpenDetails(RowTargets[Hover]);
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
