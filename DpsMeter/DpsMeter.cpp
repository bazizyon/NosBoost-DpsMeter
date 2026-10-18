#define NOMINMAX
#include <windows.h>

#include "ModContract.h"
#include "WidgetKit.h"
#include "TLBSWidget.h"
#include "TEWLabel.h"
#include "TEWGuageWidget.h"
#include "TEWCustomPanelWidget.h"
#include "TEWGraphicButtonWidget.h"
#include "TNTIconWidget.h"
#include "CInfoPacket.h"
#include "CMapPacket.h"
#include "NcifPacket.h"
#include "InPacket.h"
#include "RBossPacket.h"
#include "SuPacket.h"
#include "SkillCatalog.h"
#include "MapCatalog.h"
#include "GameHint.h"
#include "Records.h"
#include "Features.h"
#include "RaidHistory.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr auto RefreshInterval = std::chrono::milliseconds(250);
    constexpr double MinActiveSeconds = 3.0;
    constexpr int MaxRows = 10;
    constexpr int SelfSlot = MaxRows;
    constexpr int Slots = MaxRows + 1;
    constexpr int MaxTooltipSkills = 8;
    constexpr size_t MaxHistory = 5;

    constexpr uint16_t WindowWidth = 380;
    constexpr uint16_t TooltipWidth = 340;
    constexpr int16_t Pad = 14;
    constexpr int16_t ContentTop = 40;
    constexpr int16_t LineHeight = 18;
    constexpr int16_t RowHeight = 20;
    constexpr int16_t BarHeight = 16;
    constexpr int16_t TextNudge = -1;
    constexpr int16_t ArrowWidth = 12;
    constexpr int16_t ArrowHeight = 19;
    constexpr uint16_t SkinTop = 58;
    constexpr uint16_t SkinBottom = 14;
    constexpr int16_t SelfGap = 8;
    constexpr int16_t ColumnInset = 4;
    constexpr int16_t ColumnGap = 6;
    constexpr int16_t DpsWidth = 64;
    constexpr int16_t TotalWidth = 58;
    constexpr int16_t ShareWidth = 36;

    constexpr int32_t GaugeImageName = 1593835568;
    constexpr int32_t ArrowImageName = 1593835585;

    constexpr ModClassRequirement Requirements[] = {
        {TLBSWidget::ClassName, TLBSWidget::Version, TLBSWidget::ExpectedSize},
        {TEWCustomPanelWidget::ClassName, TEWCustomPanelWidget::Version, TEWCustomPanelWidget::ExpectedSize},
        {TEWGraphicButtonWidget::ClassName, TEWGraphicButtonWidget::Version, TEWGraphicButtonWidget::ExpectedSize},
        {TEWGuageWidget::ClassName, TEWGuageWidget::Version, TEWGuageWidget::ExpectedSize},
        {TEWLabel::ClassName, TEWLabel::Version, TEWLabel::ExpectedSize},
        {TNTIconWidget::ClassName, TNTIconWidget::Version, TNTIconWidget::ExpectedSize},
        {TEWEditWidget::ClassName, TEWEditWidget::Version, TEWEditWidget::ExpectedSize},
    };

    const SkillCatalog::Skill* FindSkill(const int SkillVNum) {
        static bool Loaded = false;
        static std::chrono::steady_clock::time_point LastAttempt;
        const auto Now = std::chrono::steady_clock::now();
        if (!Loaded && (LastAttempt == std::chrono::steady_clock::time_point{} || Now - LastAttempt > std::chrono::seconds(5))) {
            LastAttempt = Now;
            Loaded = SkillCatalog::Load();
        }
        return SkillCatalog::Find(SkillVNum);
    }

    std::wstring MapName(const int MapId) {
        static bool Loaded = false;
        static std::chrono::steady_clock::time_point LastAttempt;
        const auto Now = std::chrono::steady_clock::now();
        if (!Loaded && (LastAttempt == std::chrono::steady_clock::time_point{} || Now - LastAttempt > std::chrono::seconds(5))) {
            LastAttempt = Now;
            Loaded = MapCatalog::Load();
        }
        return MapCatalog::Name(MapId);
    }

    std::wstring SkillName(const int SkillVNum) {
        if (SkillVNum < 0) return Records::SkillLabel(SkillVNum);
        const SkillCatalog::Skill* Skill = FindSkill(SkillVNum);
        if (Skill && !Skill->Name.empty()) return Skill->Name;
        return L"Skill " + std::to_wstring(SkillVNum);
    }

    int16_t SkillIcon(const int SkillVNum) {
        const SkillCatalog::Skill* Skill = FindSkill(SkillVNum);
        return Skill ? Skill->Icon : static_cast<int16_t>(-1);
    }

    struct Hit {
        long Player;
        long Target;
        int Skill;
        int Damage;
        bool Crit;
        bool Miss;
        Clock::time_point Time;
    };

    struct Fight {
        std::vector<Hit> Hits;
        Clock::time_point LastHit;
        bool BossMap = false;
        int MapId = 0;
    };

    struct PlayerInfo {
        std::wstring Name;
        PlayerClass Class = PlayerClass::Adventurer;
    };

    struct BossInfo {
        int HP = 0;
        int MaxHP = 0;
    };

    const ModHost* CachedHost = nullptr;

    long OwnCharacterId = 0;
    std::unordered_map<long, PlayerInfo> Players;
    std::unordered_map<long, BossInfo> Bosses;
    bool BossOnMap = false;
    int CurrentMapId = 0;
    std::unordered_map<long, std::unordered_map<long, long long>> MobDamage;

    Fight Current;
    bool HasCurrent = false;
    bool EndCurrent = false;
    std::deque<Fight> History;
    size_t ViewIndex = 0;

    constexpr int MobRowChoices[] = {3, 5, 10};

    struct Settings {
        int Corner = 0;
        int CornerX = 20;
        int CornerY = 200;
        bool ShowMeter = false;
        bool ShowMobDamage = true;
        int MobRows = 3;
        bool Locked = false;
        bool BossOnly = false;
        bool ShowTotal = true;
        bool SplitFights = true;
        int FightSeconds = 5;
        bool SplitOnMap = true;
        int Page = 0;
        int SettingsX = -1;
        int SettingsY = -1;
    } Config;

    struct SkillTotal {
        int Skill = 0;
        long long Damage = 0;
        int Hits = 0;
    };

    struct PlayerTotal {
        long Id = 0;
        long long Damage = 0;
        int Casts = 0;
        int Hits = 0;
        int Crits = 0;
        int MaxHit = 0;
        Clock::time_point FirstHit;
        std::vector<SkillTotal> Skills;
    };

    struct FightView {
        std::vector<PlayerTotal> Players;
        long long Total = 0;
        double Seconds = 0;
        Clock::time_point End;
        bool Live = false;
        bool BossFiltered = false;
    };

    struct BarRow {
        TEWGuageWidget* Bar = nullptr;
        TEWLabel* Name = nullptr;
        TEWLabel* Value = nullptr;
        std::wstring ShownName;
        std::wstring ShownValue;
    };

    struct MeterRow {
        TEWGuageWidget* Bar = nullptr;
        TEWLabel* Name = nullptr;
        TEWLabel* Dps = nullptr;
        TEWLabel* Total = nullptr;
        TEWLabel* Share = nullptr;
        TEWGraphicButtonWidget* Hit = nullptr;
        std::wstring ShownName, ShownDps, ShownTotal, ShownShare;
    };

    TLBSWidget* AttachedRoot = nullptr;

    constexpr char TargetClassName[] = "TNTMonsterSummaryInfoWidget";
    constexpr ptrdiff_t VmtClassName = -0x2C;
    constexpr int TargetMonster = 3;
    constexpr auto TargetRescanInterval = std::chrono::milliseconds(500);
    constexpr int MaxMobRows = 10;
    constexpr int MobSelfSlot = MaxMobRows;
    constexpr int MobSlots = MaxMobRows + 1;
    constexpr int16_t MobPanelWidth = 190;
    constexpr int16_t MobPanelGap = 4;
    constexpr int16_t MobTitleHeight = 16;
    constexpr int16_t MobRowHeight = 16;
    constexpr int16_t MobBarHeight = 14;
    constexpr int16_t MobSelfGap = 5;
    constexpr int16_t MobTotalWidth = 56;

    struct MobRow {
        TEWGuageWidget* Bar = nullptr;
        TEWLabel* Name = nullptr;
        TEWLabel* Total = nullptr;
        std::wstring ShownName, ShownTotal;
    };

    TLBSWidget* TargetWidget = nullptr;
    uintptr_t TargetVTable = 0;
    Clock::time_point LastTargetScan;
    int SelectedType = 0;
    long SelectedId = 0;

    TLBSWidget* MobPanel = nullptr;
    TEWGraphicButtonWidget* MobHit = nullptr;
    TEWLabel* MobTitle = nullptr;
    std::wstring ShownMobTitle;
    MobRow MobRows[MobSlots];
    TEWGuageWidget* MobSeparator = nullptr;
    int LaidOutMobRows = -1;
    bool LaidOutMobSelf = false;
    long ShownMobId = 0;
    int MobScroll = 0;
    Clock::time_point LastMobRefresh;

    constexpr uint16_t SettingsWidth = 760;
    constexpr int PageCount = 3;
    constexpr int16_t PageTabsTop = 42;
    constexpr int16_t PageShift = 36;
    constexpr int16_t HelpWidth = 22;
    TEWCustomPanelWidget* SettingsWindow = nullptr;
    TLBSWidget* Pages[PageCount]{};
    WidgetKit::Tab PageTabs[PageCount]{};
    uint16_t PageHeights[PageCount]{};
    int ActivePage = 0;
    TLBSWidget* DpsPage = nullptr;
    TEWGraphicButtonWidget* ShowMeterBox = nullptr;
    TEWGraphicButtonWidget* ShowMobBox = nullptr;
    TEWGraphicButtonWidget* MobRowBoxes[std::size(MobRowChoices)] = {};
    bool MobRowChecked[std::size(MobRowChoices)] = {};
    TEWLabel* ShowMeterHelp = nullptr;
    TEWLabel* ShowMobHelp = nullptr;
    TEWLabel* SplitHelp = nullptr;
    TEWLabel* SplitMapHelp = nullptr;
    TEWGraphicButtonWidget* HelpHits[4]{};
    TEWGraphicButtonWidget* SplitMapBox = nullptr;
    TEWGraphicButtonWidget* SplitBox = nullptr;
    WidgetKit::EditBox SecondsBox{};
    std::wstring ShownSeconds;
    bool SettingsPlaced = false;
    bool MeterShown = false;
    bool WasLoading = true;
    int16_t PlacedX = 0, PlacedY = 0;
    int PlacedScreenWidth = 0, PlacedScreenHeight = 0;

    TEWCustomPanelWidget* Window = nullptr;
    TEWLabel* TitleLabel = nullptr;
    TLBSWidget* CloseButton = nullptr;
    TLBSWidget* BossToggle = nullptr;
    TLBSWidget* TotalToggle = nullptr;
    TLBSWidget* LockToggle = nullptr;
    TEWLabel* BossLineLabel = nullptr;
    TEWLabel* HeaderName = nullptr;
    TEWLabel* HeaderDps = nullptr;
    TEWLabel* HeaderTotal = nullptr;
    TEWLabel* HeaderShare = nullptr;
    MeterRow Rows[Slots];
    TEWGuageWidget* SelfSeparator = nullptr;
    TEWGraphicButtonWidget* OlderButton = nullptr;
    TEWGraphicButtonWidget* NewerButton = nullptr;
    TEWLabel* FooterLabel = nullptr;
    std::wstring ShownBossLine, ShownFooter, ShownHeaderName = L"Player";
    int LaidOutRows = -1;
    bool LaidOutBossLine = false;
    bool LaidOutSelf = false;

    TEWCustomPanelWidget* Tooltip = nullptr;
    TEWLabel* TooltipTitle = nullptr;
    TEWLabel* TooltipStats = nullptr;
    TEWLabel* TooltipHeaderName = nullptr;
    TEWLabel* TooltipHeaderValue = nullptr;
    BarRow TooltipRows[MaxTooltipSkills];
    TNTIconWidget* TooltipIcons[MaxTooltipSkills] = {};
    std::wstring ShownTooltipTitle, ShownTooltipStats;
    int LaidOutTooltipRows = -1;

    FightView LastView;
    int SlotPlayer[Slots] = {};
    int HoveredRow = -1;
    int Scroll = 0;
    Clock::time_point LastRefresh;

    std::wstring ToWide(const std::string& Text) {
        if (Text.empty()) return {};
        const int Length = MultiByteToWideChar(1252, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
        std::wstring Result(Length, L'\0');
        MultiByteToWideChar(1252, 0, Text.data(), static_cast<int>(Text.size()), Result.data(), Length);
        return Result;
    }

    std::wstring FormatAmount(const double Value) {
        wchar_t Buffer[32];
        if (Value >= 1'000'000'000) {
            std::swprintf(Buffer, std::size(Buffer), L"%.2fB", Value / 1'000'000'000);
        } else if (Value >= 1'000'000) {
            std::swprintf(Buffer, std::size(Buffer), L"%.2fM", Value / 1'000'000);
        } else if (Value >= 10'000) {
            std::swprintf(Buffer, std::size(Buffer), L"%.1fk", Value / 1'000);
        } else {
            std::swprintf(Buffer, std::size(Buffer), L"%.0f", Value);
        }
        return Buffer;
    }

    std::wstring FormatDuration(const double Seconds) {
        const int Total = static_cast<int>(Seconds);
        wchar_t Buffer[16];
        std::swprintf(Buffer, std::size(Buffer), L"%d:%02d", Total / 60, Total % 60);
        return Buffer;
    }

    std::wstring NameOf(const long Id) {
        if (const auto It = Players.find(Id); It != Players.end() && !It->second.Name.empty()) {
            return It->second.Name;
        }
        return L"#" + std::to_wstring(Id);
    }

    const wchar_t* ClassName(const PlayerClass Class) {
        switch (Class) {
            case PlayerClass::Swordsman:     return L"Swordsman";
            case PlayerClass::Archer:        return L"Archer";
            case PlayerClass::Mage:          return L"Mage";
            case PlayerClass::MartialArtist: return L"Martial Artist";
            default:                         return L"Adventurer";
        }
    }

    Color BarColor(const long Id) {
        if (Id == OwnCharacterId) return Color(255, 255, 215, 0);
        const auto It = Players.find(Id);
        switch (It == Players.end() ? PlayerClass::Adventurer : It->second.Class) {
            case PlayerClass::Swordsman:     return Color(255, 35, 110, 209);
            case PlayerClass::Archer:        return Color(255, 22, 173, 65);
            case PlayerClass::Mage:          return Color(255, 186, 34, 34);
            case PlayerClass::MartialArtist: return Color(255, 235, 165, 45);
            default:                         return Color(255, 210, 125, 35);
        }
    }

    std::string SettingsPath() {
        char Path[MAX_PATH];
        GetModuleFileNameA(nullptr, Path, MAX_PATH);
        std::string Dir(Path);
        return Dir.substr(0, Dir.find_last_of("\\/") + 1) + "mods\\Trackers\\DpsMeter.ini";
    }

    void LoadSettings() {
        const std::string Path = SettingsPath();
        Config.Corner = static_cast<int>(GetPrivateProfileIntA("DpsMeter", "Corner", -1, Path.c_str()));
        if (Config.Corner < 0 || Config.Corner > 3) {
            Config.Corner = 0;
            Config.CornerX = static_cast<int>(GetPrivateProfileIntA("DpsMeter", "X", Config.CornerX, Path.c_str()));
            Config.CornerY = static_cast<int>(GetPrivateProfileIntA("DpsMeter", "Y", Config.CornerY, Path.c_str()));
        } else {
            Config.CornerX = static_cast<int>(GetPrivateProfileIntA("DpsMeter", "CornerX", Config.CornerX, Path.c_str()));
            Config.CornerY = static_cast<int>(GetPrivateProfileIntA("DpsMeter", "CornerY", Config.CornerY, Path.c_str()));
        }
        Config.ShowMeter = GetPrivateProfileIntA("DpsMeter", "ShowMeter", 0, Path.c_str()) != 0;
        Config.ShowMobDamage = GetPrivateProfileIntA("DpsMeter", "ShowMobDamage", 1, Path.c_str()) != 0;
        Config.MobRows = static_cast<int>(GetPrivateProfileIntA("DpsMeter", "MobRows", 3, Path.c_str()));
        if (std::find(std::begin(MobRowChoices), std::end(MobRowChoices), Config.MobRows) == std::end(MobRowChoices)) Config.MobRows = 3;
        Config.Locked = GetPrivateProfileIntA("DpsMeter", "Locked", 0, Path.c_str()) != 0;
        Config.BossOnly = GetPrivateProfileIntA("DpsMeter", "BossOnly", 0, Path.c_str()) != 0;
        Config.ShowTotal = GetPrivateProfileIntA("DpsMeter", "ShowTotal", 1, Path.c_str()) != 0;
        Config.SplitFights = GetPrivateProfileIntA("DpsMeter", "SplitFights", 1, Path.c_str()) != 0;
        Config.FightSeconds = std::max(1, static_cast<int>(GetPrivateProfileIntA("DpsMeter", "FightSeconds", 5, Path.c_str())));
        Config.SplitOnMap = GetPrivateProfileIntA("DpsMeter", "SplitOnMap", 1, Path.c_str()) != 0;
        Config.Page = std::clamp(static_cast<int>(GetPrivateProfileIntA("Trackers", "Page", 0, Path.c_str())), 0, 2);
        Config.SettingsX = static_cast<int>(GetPrivateProfileIntA("Trackers", "X", -1, Path.c_str()));
        Config.SettingsY = static_cast<int>(GetPrivateProfileIntA("Trackers", "Y", -1, Path.c_str()));
    }

    void SaveSettings() {
        const std::string Path = SettingsPath();
        WritePrivateProfileStringA("DpsMeter", "X", nullptr, Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "Y", nullptr, Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "Corner", std::to_string(Config.Corner).c_str(), Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "CornerX", std::to_string(Config.CornerX).c_str(), Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "CornerY", std::to_string(Config.CornerY).c_str(), Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "ShowMeter", Config.ShowMeter ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "ShowMobDamage", Config.ShowMobDamage ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "MobRows", std::to_string(Config.MobRows).c_str(), Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "Locked", Config.Locked ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "BossOnly", Config.BossOnly ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "ShowTotal", Config.ShowTotal ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "SplitFights", Config.SplitFights ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "FightSeconds", std::to_string(Config.FightSeconds).c_str(), Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "SplitOnMap", Config.SplitOnMap ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("Trackers", "Page", std::to_string(Config.Page).c_str(), Path.c_str());
        WritePrivateProfileStringA("Trackers", "X", std::to_string(Config.SettingsX).c_str(), Path.c_str());
        WritePrivateProfileStringA("Trackers", "Y", std::to_string(Config.SettingsY).c_str(), Path.c_str());
    }

    void RememberSettingsPlace() {
        if (!SettingsWindow || !SettingsPlaced) return;
        if (Config.SettingsX == SettingsWindow->rect.left && Config.SettingsY == SettingsWindow->rect.top) return;
        Config.SettingsX = SettingsWindow->rect.left;
        Config.SettingsY = SettingsWindow->rect.top;
        SaveSettings();
    }

    bool FightIdle(const Clock::time_point LastHit, const Clock::time_point Now) {
        return Config.SplitFights && Now - LastHit > std::chrono::seconds(Config.FightSeconds);
    }

    void StartNewFight(const Clock::time_point Now) {
        if (HasCurrent && !Current.Hits.empty()) {
            History.push_front(std::move(Current));
            if (History.size() > MaxHistory) History.pop_back();
            if (ViewIndex > 0) ViewIndex = std::min(ViewIndex + 1, History.size());
        }
        if (ViewIndex == 0) Scroll = 0;
        Current = Fight{};
        Current.BossMap = BossOnMap;
        Current.MapId = CurrentMapId;
        Current.LastHit = Now;
        HasCurrent = true;
        EndCurrent = false;
    }

    const Fight* ViewedFight() {
        if (ViewIndex == 0) return HasCurrent ? &Current : nullptr;
        return ViewIndex <= History.size() ? &History[ViewIndex - 1] : nullptr;
    }

    FightView BuildView(const Fight& Source, const bool IsCurrent, const Clock::time_point Now) {
        FightView View;
        View.Live = IsCurrent && !EndCurrent && !FightIdle(Source.LastHit, Now);
        View.BossFiltered = Config.BossOnly && Source.BossMap;
        View.End = View.Live ? Now : Source.LastHit;

        std::unordered_map<long, PlayerTotal> Totals;
        std::unordered_map<long, std::unordered_map<int, SkillTotal>> Skills;
        Clock::time_point FightStart = View.End;
        for (const Hit& H : Source.Hits) {
            if (View.BossFiltered && !Bosses.contains(H.Target)) continue;
            PlayerTotal& P = Totals[H.Player];
            if (P.Casts == 0) {
                P.Id = H.Player;
                P.FirstHit = H.Time;
            }
            FightStart = std::min(FightStart, H.Time);
            P.Casts++;
            if (H.Miss) continue;
            P.Hits++;
            P.Crits += H.Crit;
            P.Damage += H.Damage;
            P.MaxHit = std::max(P.MaxHit, H.Damage);
            SkillTotal& S = Skills[H.Player][H.Skill];
            S.Skill = H.Skill;
            S.Damage += H.Damage;
            S.Hits++;
        }

        for (auto& [Id, P] : Totals) {
            if (P.Damage <= 0) continue;
            for (const auto& [Skill, S] : Skills[Id]) P.Skills.push_back(S);
            std::sort(P.Skills.begin(), P.Skills.end(), [](const auto& A, const auto& B) { return A.Damage > B.Damage; });
            View.Total += P.Damage;
            View.Players.push_back(std::move(P));
        }
        std::sort(View.Players.begin(), View.Players.end(), [](const auto& A, const auto& B) { return A.Damage > B.Damage; });
        View.Seconds = std::chrono::duration<double>(View.End - FightStart).count();
        return View;
    }

    double PlayerDps(const PlayerTotal& P, const FightView& View) {
        const double Active = std::chrono::duration<double>(View.End - P.FirstHit).count();
        return static_cast<double>(P.Damage) / std::max(MinActiveSeconds, Active);
    }

    std::wstring BossLine(const Clock::time_point Now) {
        if (!HasCurrent || ViewIndex != 0) return {};
        std::unordered_map<long, long long> BossDamage;
        std::unordered_map<long, Clock::time_point> FirstBossHit;
        for (const Hit& H : Current.Hits) {
            const auto It = Bosses.find(H.Target);
            if (It == Bosses.end() || It->second.MaxHP <= 0 || H.Miss) continue;
            BossDamage[H.Target] += H.Damage;
            FirstBossHit.try_emplace(H.Target, H.Time);
        }
        if (BossDamage.empty()) return {};

        const auto Top = std::max_element(BossDamage.begin(), BossDamage.end(),
            [](const auto& A, const auto& B) { return A.second < B.second; });
        const BossInfo& Boss = Bosses[Top->first];
        wchar_t Buffer[96];
        if (Boss.HP <= 0) {
            std::swprintf(Buffer, std::size(Buffer), L"Boss down");
            return Buffer;
        }
        const int Percent = static_cast<int>(static_cast<long long>(Boss.HP) * 100 / Boss.MaxHP);
        const double Seconds = std::max(1.0, std::chrono::duration<double>(Now - FirstBossHit[Top->first]).count());
        const double GroupDps = static_cast<double>(Top->second) / Seconds;
        if (GroupDps <= 0) {
            std::swprintf(Buffer, std::size(Buffer), L"Boss %d%%", Percent);
        } else {
            std::swprintf(Buffer, std::size(Buffer), L"Boss %d%% | ~%ls left", Percent,
                          FormatDuration(Boss.HP / GroupDps).c_str());
        }
        return Buffer;
    }

    bool OnCInfo(const Packet::CInfoPacket& Packet) {
        OwnCharacterId = Packet.characterID;
        Players[Packet.characterID] = {ToWide(Packet.name), Packet.characterClass};
        return true;
    }

    bool OnIn(const Packet::InPacket& Packet) {
        if (Packet.entityType == EntityType::Player) {
            Players[Packet.entityID] = {ToWide(Packet.entityName), Packet.inPlayerSubPacket.playerClass};
        }
        return true;
    }

    bool OnCMap(const Packet::CMapPacket& Packet) {
        CurrentMapId = Packet.mapId;
        if (Config.SplitOnMap && HasCurrent) EndCurrent = true;
        BossOnMap = false;
        MobDamage.clear();
        SelectedType = 0;
        SelectedId = 0;
        return true;
    }

    bool OnNcif(const Packet::NcifPacket& Packet) {
        SelectedType = Packet.entityType;
        SelectedId = Packet.entityID;
        return true;
    }

    bool OnRBoss(const Packet::RBossPacket& Packet) {
        Bosses[Packet.entityID] = {Packet.HP, Packet.maxHP};
        BossOnMap = true;
        if (HasCurrent) Current.BossMap = true;
        return true;
    }

    bool OnSu(const Packet::SuPacket& Packet) {
        if (Packet.userEntityType != EntityType::Player || Packet.targetEntityType != EntityType::Monster) return true;
        if (Packet.hitMode == HitMode::SuccessfulBuff) return true;

        const auto Now = Clock::now();
        if (!HasCurrent || EndCurrent || FightIdle(Current.LastHit, Now)) {
            StartNewFight(Now);
        }
        Current.LastHit = Now;

        const bool Miss = Packet.hitMode == HitMode::Miss || Packet.hitMode == HitMode::LongRangeMiss
                       || Packet.hitMode == HitMode::UnknownMiss;
        const bool Crit = Packet.hitMode == HitMode::CriticalAttack || Packet.hitMode == HitMode::UnknownCriticalAttack;
        Current.Hits.push_back({Packet.userEntityID, Packet.targetEntityID, Records::SkillKey(Packet.skillVNum, Packet.skillEffect),
                                std::max(0, Packet.damage), Crit, Miss, Now});
        if (!Packet.targetAlive) MobDamage.erase(Packet.targetEntityID);
        else if (!Miss && Packet.damage > 0) MobDamage[Packet.targetEntityID][Packet.userEntityID] += Packet.damage;
        return true;
    }

    void Attach(TLBSWidget* Parent, TLBSWidget* Child) {
        if (!Parent || !Child) return;
        Child->parent = Parent;
        Parent->childrenList->push_back(Child);
    }

    void Detach(TLBSWidget* Widget) {
        if (!Widget) return;
        Widget->isVisible = false;
        if (TLBSWidget* Parent = Widget->parent; Parent && Parent->childrenList
            && Parent->childrenList->index_of(Widget) >= 0) {
            Parent->childrenList->remove(Widget);
        }
    }

    void Place(TLBSWidget* Widget, const int16_t X, const int16_t Y, const int16_t Width, const int16_t Height) {
        if (!Widget) return;
        Widget->rect = {X, Y, static_cast<int16_t>(X + Width), static_cast<int16_t>(Y + Height)};
    }

    void Show(TLBSWidget* Widget, const bool Visible) {
        if (Widget && Widget->isVisible != Visible) Widget->isVisible = Visible;
    }

    void SetText(TEWLabel* Label, std::wstring& Shown, const std::wstring& Text) {
        if (!Label || Shown == Text) return;
        Shown = Text;
        Label->SetText(Text.c_str());
    }

    TEWLabel* AddLabel(TLBSWidget* Parent, const int16_t X, const int16_t TextY, const int16_t Width,
                       const uint8_t Alignment) {
        TEWLabel* Label = Widget::Create<TEWLabel>(CachedHost);
        if (!Label) return nullptr;
        Place(Label, X, static_cast<int16_t>(TextY + TextNudge), Width, 30);
        Label->textAlignment = Alignment;
        Label->pxPerLine = Width;
        Label->SetText(L"");
        Attach(Parent, Label);
        return Label;
    }

    TEWGuageWidget* AddBar(TLBSWidget* Parent) {
        TEWGuageWidget* Bar = Widget::Create<TEWGuageWidget>(CachedHost);
        if (!Bar) return nullptr;
        delete[] Bar->imageData.atlasFrames;
        Bar->imageData.imageName = GaugeImageName;
        Bar->imageData.frameCount = 2;
        Bar->imageData.atlasFrames = new AtlasFrame[2]{
            {30, 4, 1, 1},
            {249, 43, 1, 1},
        };
        Bar->fillPercent = 0;
        Bar->isInteractable = false;
        Attach(Parent, Bar);
        return Bar;
    }

    BarRow AddBarRow(TLBSWidget* Parent) {
        BarRow Row;
        Row.Bar = AddBar(Parent);
        Row.Name = AddLabel(Parent, 0, 0, 10, 1);
        Row.Value = AddLabel(Parent, 0, 0, 10, 2);
        return Row;
    }

    void PlaceBarRow(BarRow& Row, const int16_t Y, const int16_t Width, const int16_t X = Pad) {
        constexpr int16_t TextInset = 4;
        const int16_t TextY = static_cast<int16_t>(Y + (BarHeight - 12) / 2);
        Place(Row.Bar, X, Y, Width, BarHeight);
        Place(Row.Name, static_cast<int16_t>(X + TextInset), static_cast<int16_t>(TextY + TextNudge), Width * 3 / 5, 30);
        Place(Row.Value, static_cast<int16_t>(X + Width * 2 / 5 - TextInset), static_cast<int16_t>(TextY + TextNudge),
              static_cast<int16_t>(Width * 3 / 5), 30);
        if (Row.Name) Row.Name->pxPerLine = Width * 3 / 5;
        if (Row.Value) Row.Value->pxPerLine = static_cast<int16_t>(Width * 3 / 5);
    }

    void ShowBarRow(BarRow& Row, const bool Visible) {
        Show(Row.Bar, Visible);
        Show(Row.Name, Visible);
        Show(Row.Value, Visible);
    }

    void UpdateBarRow(BarRow& Row, const std::wstring& Name, const std::wstring& Value, const float Fill,
                      const Color& BarTint) {
        if (Row.Bar) {
            Row.Bar->fillPercent = std::clamp(Fill, 0.0f, 1.0f);
            Row.Bar->gaugeColor = BarTint;
        }
        SetText(Row.Name, Row.ShownName, Name);
        SetText(Row.Value, Row.ShownValue, Value);
    }

    struct ColumnLayout {
        int16_t NameWidth;
        int16_t DpsX;
        int16_t TotalX;
        int16_t ShareX;
    };

    ColumnLayout MeterColumns(const int16_t Width) {
        ColumnLayout Columns{};
        Columns.ShareX = static_cast<int16_t>(Pad + Width - ColumnInset - ShareWidth);
        Columns.TotalX = static_cast<int16_t>(Columns.ShareX - ColumnGap - TotalWidth);
        Columns.DpsX = static_cast<int16_t>((Config.ShowTotal ? Columns.TotalX : Columns.ShareX) - ColumnGap - DpsWidth);
        Columns.NameWidth = static_cast<int16_t>(Columns.DpsX - ColumnGap - Pad - ColumnInset);
        return Columns;
    }

    void PlaceLabel(TEWLabel* Label, const int16_t X, const int16_t TextY, const int16_t Width) {
        if (!Label) return;
        Place(Label, X, static_cast<int16_t>(TextY + TextNudge), Width, 30);
        Label->pxPerLine = Width;
    }

    void PlaceColumns(TEWLabel* Name, TEWLabel* Dps, TEWLabel* Total, TEWLabel* Share, const int16_t TextY,
                      const int16_t Width) {
        const ColumnLayout Columns = MeterColumns(Width);
        PlaceLabel(Name, Pad + ColumnInset, TextY, Columns.NameWidth);
        PlaceLabel(Dps, Columns.DpsX, TextY, DpsWidth);
        PlaceLabel(Total, Columns.TotalX, TextY, TotalWidth);
        PlaceLabel(Share, Columns.ShareX, TextY, ShareWidth);
    }

    MeterRow AddMeterRow(TLBSWidget* Parent) {
        MeterRow Row;
        Row.Bar = AddBar(Parent);
        Row.Name = AddLabel(Parent, 0, 0, 10, 1);
        Row.Dps = AddLabel(Parent, 0, 0, 10, 2);
        Row.Total = AddLabel(Parent, 0, 0, 10, 2);
        Row.Share = AddLabel(Parent, 0, 0, 10, 2);
        Row.Hit = Records::AddHitArea(CachedHost, Parent, 0, 0, 1, 1);
        return Row;
    }

    void PlaceMeterRow(MeterRow& Row, const int16_t Y, const int16_t Width) {
        Place(Row.Bar, Pad, Y, Width, BarHeight);
        Place(Row.Hit, Pad, Y, Width, BarHeight);
        PlaceColumns(Row.Name, Row.Dps, Row.Total, Row.Share, static_cast<int16_t>(Y + (BarHeight - 12) / 2), Width);
    }

    void ShowMeterRow(MeterRow& Row, const bool Visible) {
        Show(Row.Bar, Visible);
        Show(Row.Name, Visible);
        Show(Row.Dps, Visible);
        Show(Row.Total, Visible && Config.ShowTotal);
        Show(Row.Share, Visible);
        Show(Row.Hit, Visible);
    }

    void SetPanelHeight(TEWCustomPanelWidget* Panel, uint16_t Height) {
        Height = std::max<uint16_t>(Height, SkinTop + SkinBottom + 1);
        Panel->rect.bottom = static_cast<int16_t>(Panel->rect.top + Height);
        Panel->nineSliceInfo.heightMiddle = Height - SkinTop - Panel->nineSliceInfo.heightBot;
        Panel->nineSliceInfo.posBot = SkinTop + Panel->nineSliceInfo.heightMiddle;
    }

    TEWGraphicButtonWidget* AddArrowButton(TLBSWidget* Parent, const bool PointsRight, const WidgetKit::ClickFn OnClick) {
        TEWGraphicButtonWidget* Button = Widget::Create<TEWGraphicButtonWidget>(CachedHost);
        if (!Button) return nullptr;
        const int16_t X = PointsRight ? 495 : 476;
        const AtlasFrame Dark{X, 71, ArrowWidth, ArrowHeight};
        const AtlasFrame Light{X, 49, ArrowWidth, ArrowHeight};
        delete[] Button->imageData.atlasFrames;
        Button->imageData.imageName = ArrowImageName;
        Button->imageData.frameCount = 3;
        Button->imageData.atlasFrames = new AtlasFrame[3]{Dark, Light, Dark};
        Button->drawMode = 0;
        Place(Button, 0, 0, ArrowWidth, ArrowHeight);
        WidgetKit::SetOnClick(Button, OnClick, nullptr);
        Attach(Parent, Button);
        return Button;
    }

    POINT AbsoluteTopLeft(const TLBSWidget* Widget) {
        POINT P{0, 0};
        for (const TLBSWidget* W = Widget; W; W = W->parent) {
            P.x += W->rect.left;
            P.y += W->rect.top;
        }
        return P;
    }

    bool CursorInGame(POINT& Out) {
        const HWND Foreground = GetForegroundWindow();
        DWORD Pid = 0;
        GetWindowThreadProcessId(Foreground, &Pid);
        if (!Foreground || Pid != GetCurrentProcessId() || !GetCursorPos(&Out)) return false;
        return ScreenToClient(Foreground, &Out) != FALSE;
    }

    void RequestRefresh() {
        LastRefresh = {};
    }

    void __cdecl OnOlder(void*) {
        ViewIndex = std::min(ViewIndex + 1, History.size());
        Scroll = 0;
        RequestRefresh();
    }

    void __cdecl OnNewer(void*) {
        if (ViewIndex > 0) ViewIndex--;
        Scroll = 0;
        RequestRefresh();
    }

    void __cdecl OnBossOnlyChanged(void*) {
        SaveSettings();
        RequestRefresh();
    }

    constexpr int16_t ToggleY = 11;
    constexpr int16_t ToggleTextWidth = 30;
    constexpr int16_t ToggleWidth = 15 + 4 + ToggleTextWidth;
    constexpr int16_t LockX = WindowWidth - 33 - ToggleWidth;
    constexpr int16_t BossX = LockX - 8 - ToggleWidth;
    constexpr int16_t TotalX = BossX - 8 - ToggleWidth;
    const Color HeaderColor(255, 160, 160, 160);

    void ApplyLock() {
        if (!Window) return;
        const bool Chrome = !Config.Locked;
        Show(TitleLabel, Chrome);
        Show(CloseButton, Chrome);
        Show(OlderButton, Chrome);
        Show(NewerButton, Chrome);
        Show(FooterLabel, Chrome);
        Window->color.alpha = Chrome ? 255 : 0;
        Window->isMoveable = Chrome;
        Window->isInteractable = Chrome;
        LaidOutRows = -1;
    }

    void __cdecl OnShowTotalChanged(void*) {
        LaidOutRows = -1;
        SaveSettings();
        RequestRefresh();
    }

    void __cdecl OnLockChanged(void*) {
        ApplyLock();
        SaveSettings();
        RequestRefresh();
    }

    void SyncToggle(TLBSWidget* Toggle, const int16_t OffsetX) {
        if (!Toggle || !Window || !AttachedRoot) return;
        Show(Toggle, Window->isVisible);
        const int16_t X = static_cast<int16_t>(Window->rect.left + OffsetX);
        const int16_t Y = static_cast<int16_t>(Window->rect.top + ToggleY);
        if (Toggle->rect.left != X || Toggle->rect.top != Y) Place(Toggle, X, Y, ToggleWidth, 15);
        const TLBSWidgetList* Siblings = AttachedRoot->childrenList;
        if (Siblings->index_of(Toggle) < Siblings->index_of(Window)) Toggle->BubbleUp();
    }

    void SyncToggles() {
        SyncToggle(TotalToggle, TotalX);
        SyncToggle(BossToggle, BossX);
        SyncToggle(LockToggle, LockX);
    }

    void PlaceHeaders(TEWLabel* Name, TEWLabel* Value, const int16_t Y, const int16_t Width) {
        Place(Name, Pad + 4, static_cast<int16_t>(Y + TextNudge), Width / 2, 30);
        Place(Value, static_cast<int16_t>(Pad + Width / 2 - 4), static_cast<int16_t>(Y + TextNudge), Width / 2, 30);
    }

    TEWLabel* AddHeader(TLBSWidget* Parent, const wchar_t* Text, const int16_t Width, const uint8_t Alignment) {
        TEWLabel* Label = AddLabel(Parent, 0, 0, Width, Alignment);
        if (!Label) return nullptr;
        Label->textColor = HeaderColor;
        Label->SetText(Text);
        return Label;
    }

    void LayOutWindow(const int VisibleRows, const bool HasBossLine, const bool HasSelf) {
        if (VisibleRows == LaidOutRows && HasBossLine == LaidOutBossLine && HasSelf == LaidOutSelf) return;
        LaidOutRows = VisibleRows;
        LaidOutBossLine = HasBossLine;
        LaidOutSelf = HasSelf;

        const int16_t Width = WindowWidth - 2 * Pad;
        int16_t Y = ContentTop;
        Show(BossLineLabel, HasBossLine);
        if (HasBossLine) {
            Place(BossLineLabel, Pad, static_cast<int16_t>(Y + TextNudge), Width, 30);
            Y += LineHeight;
        }
        Show(HeaderName, VisibleRows > 0);
        Show(HeaderDps, VisibleRows > 0);
        Show(HeaderTotal, VisibleRows > 0 && Config.ShowTotal);
        Show(HeaderShare, VisibleRows > 0);
        if (VisibleRows > 0) {
            PlaceColumns(HeaderName, HeaderDps, HeaderTotal, HeaderShare, Y, Width);
            Y += LineHeight;
        }
        for (int i = 0; i < MaxRows; i++) {
            PlaceMeterRow(Rows[i], static_cast<int16_t>(Y + i * RowHeight), Width);
            ShowMeterRow(Rows[i], i < VisibleRows);
        }
        Y += static_cast<int16_t>(VisibleRows * RowHeight);
        Show(SelfSeparator, HasSelf);
        ShowMeterRow(Rows[SelfSlot], HasSelf);
        if (HasSelf) {
            Place(SelfSeparator, Pad, static_cast<int16_t>(Y + (SelfGap - 2) / 2 - 2), Width, 2);
            Y += SelfGap;
            PlaceMeterRow(Rows[SelfSlot], Y, Width);
            Y += RowHeight;
        }
        if (!Config.Locked) {
            Y += 4;
            Place(OlderButton, Pad, Y, ArrowWidth, ArrowHeight);
            Place(NewerButton, static_cast<int16_t>(WindowWidth - Pad - ArrowWidth), Y, ArrowWidth, ArrowHeight);
            Place(FooterLabel, Pad + ArrowWidth, static_cast<int16_t>(Y + (ArrowHeight - 12) / 2 + TextNudge),
                  Width - 2 * ArrowWidth, 30);
            if (FooterLabel) FooterLabel->pxPerLine = Width - 2 * ArrowWidth;
            Y += ArrowHeight;
        }
        SetPanelHeight(Window, static_cast<uint16_t>(Y + 10));
    }

    TNTIconWidget* CreateSkillIcon() {
        TNTIconWidget* Icon = Widget::Create<TNTIconWidget>(CachedHost);
        if (!Icon) return nullptr;
        auto* Raw = reinterpret_cast<uint8_t*>(Icon);
        Icon->keepTransparency = false;
        Icon->resized = true;
        *reinterpret_cast<float*>(Raw + 0x90) = 1.0f;
        Icon->imageIf = 4;
        Raw[0xAB] = 1;
        Raw[0xAC] = 1;
        Raw[0xAE] = 10;
        Raw[0xD3] = 1;
        Icon->isInteractable = false;
        Icon->color = Color(254, 255, 255, 255);
        delete Icon->image;
        Icon->image = reinterpret_cast<IconProperties*>(new uint8_t[0x40]{});
        Icon->isVisible = false;
        return Icon;
    }

    void SetSkillIcon(TNTIconWidget* Icon, const int16_t IconId) {
        if (!Icon || !Icon->image) return;
        const bool Visible = IconId >= 0;
        if (Visible && Icon->image->imageID != IconId) Icon->image->imageID = IconId;
        if (Icon->isVisible != Visible) Icon->isVisible = Visible;
    }

    void LayOutTooltip(const int SkillRows) {
        if (SkillRows == LaidOutTooltipRows) return;
        LaidOutTooltipRows = SkillRows;
        const int16_t Width = TooltipWidth - 2 * Pad;
        const int16_t RowsTop = ContentTop + 2 * LineHeight;
        PlaceHeaders(TooltipHeaderName, TooltipHeaderValue, ContentTop + LineHeight, Width);
        for (int i = 0; i < MaxTooltipSkills; i++) {
            const int16_t Y = static_cast<int16_t>(RowsTop + i * RowHeight);
            Place(TooltipIcons[i], Pad, Y, BarHeight, BarHeight);
            PlaceBarRow(TooltipRows[i], Y, static_cast<int16_t>(Width - BarHeight - 4), static_cast<int16_t>(Pad + BarHeight + 4));
            ShowBarRow(TooltipRows[i], i < SkillRows);
        }
        SetPanelHeight(Tooltip, static_cast<uint16_t>(RowsTop + SkillRows * RowHeight + 10));
    }

    void CreateTooltip(TLBSWidget* Root) {
        const WidgetKit::WindowDesc Desc{WidgetKit::WindowStyle::Plain, 0, 0, TooltipWidth, 120, L"", false};
        Tooltip = WidgetKit::CreateGameWindow(CachedHost, Desc);
        if (!Tooltip) return;
        Tooltip->isMoveable = false;
        Tooltip->isInteractable = false;

        TooltipTitle = Tooltip->childrenList->count > 0 ? static_cast<TEWLabel*>(Tooltip->childrenList->list[0]) : nullptr;
        if (TooltipTitle) {
            TooltipTitle->rect.right = TooltipWidth - 12;
            TooltipTitle->pxPerLine = TooltipWidth - 24;
        }
        TooltipStats = AddLabel(Tooltip, Pad, ContentTop, TooltipWidth - 2 * Pad, 1);
        TooltipHeaderName = AddHeader(Tooltip, L"Skill", (TooltipWidth - 2 * Pad) / 2, 1);
        TooltipHeaderValue = AddHeader(Tooltip, L"Share | Hits", (TooltipWidth - 2 * Pad) / 2, 2);
        for (auto& Row : TooltipRows) Row = AddBarRow(Tooltip);
        for (auto& Icon : TooltipIcons) {
            Icon = CreateSkillIcon();
            Attach(Tooltip, Icon);
        }
        Tooltip->ToggleVisibilityRecursively(false);
        ShownTooltipTitle.clear();
        ShownTooltipStats.clear();
        LaidOutTooltipRows = -1;
        Attach(Root, Tooltip);
    }

    int ScreenWidth() {
        return AttachedRoot ? AttachedRoot->rect.right - AttachedRoot->rect.left : 1920;
    }

    int ScreenHeight() {
        return AttachedRoot ? AttachedRoot->rect.bottom - AttachedRoot->rect.top : 1080;
    }

    void PlaceMeter() {
        const int Width = ScreenWidth(), Height = ScreenHeight();
        const int WindowW = Window->rect.right - Window->rect.left;
        const int WindowH = Window->rect.bottom - Window->rect.top;
        int X = Config.Corner & 1 ? Width - Config.CornerX : Config.CornerX;
        int Y = Config.Corner & 2 ? Height - Config.CornerY : Config.CornerY;
        X = std::clamp(X, 0, std::max(0, Width - WindowW));
        Y = std::clamp(Y, 0, std::max(0, Height - WindowH));
        Place(Window, static_cast<int16_t>(X), static_cast<int16_t>(Y), static_cast<int16_t>(WindowW), static_cast<int16_t>(WindowH));
        PlacedX = Window->rect.left;
        PlacedY = Window->rect.top;
        PlacedScreenWidth = Width;
        PlacedScreenHeight = Height;
    }

    void RememberMeterPosition() {
        const int Width = ScreenWidth(), Height = ScreenHeight();
        const bool Right = (Window->rect.left + Window->rect.right) / 2 > Width / 2;
        const bool Bottom = (Window->rect.top + Window->rect.bottom) / 2 > Height / 2;
        Config.Corner = (Right ? 1 : 0) | (Bottom ? 2 : 0);
        Config.CornerX = Right ? Width - Window->rect.left : Window->rect.left;
        Config.CornerY = Bottom ? Height - Window->rect.top : Window->rect.top;
        PlacedX = Window->rect.left;
        PlacedY = Window->rect.top;
    }

    void SyncMeterPosition() {
        if (!Window || Window->isBeingDragged) return;
        if (ScreenWidth() != PlacedScreenWidth || ScreenHeight() != PlacedScreenHeight) {
            PlaceMeter();
        } else if (Window->rect.left != PlacedX || Window->rect.top != PlacedY) {
            RememberMeterPosition();
            SaveSettings();
        }
    }

    void CreateMeterWindow(TLBSWidget* Root) {
        const WidgetKit::WindowDesc Desc{WidgetKit::WindowStyle::Plain, 0, 0, WindowWidth, 200, L"DPS Meter", true};
        Window = WidgetKit::CreateGameWindow(CachedHost, Desc);
        if (!Window) return;
        Window->isRestrictedToScreen = true;

        TitleLabel = Window->childrenList->count > 0 ? static_cast<TEWLabel*>(Window->childrenList->list[0]) : nullptr;
        CloseButton = Window->childrenList->count > 1 ? Window->childrenList->list[1] : nullptr;


        BossLineLabel = AddLabel(Window, Pad, ContentTop, WindowWidth - 2 * Pad, 1);
        if (BossLineLabel) BossLineLabel->textColor = Color(255, 255, 120, 120);
        HeaderName = AddHeader(Window, L"Player", 10, 1);
        HeaderDps = AddHeader(Window, L"DPS", 10, 2);
        HeaderTotal = AddHeader(Window, L"Total", 10, 2);
        HeaderShare = AddHeader(Window, L"Share", 10, 2);
        for (auto& Row : Rows) Row = AddMeterRow(Window);
        SelfSeparator = AddBar(Window);
        if (SelfSeparator) {
            SelfSeparator->fillPercent = 1.0f;
            SelfSeparator->gaugeColor = Color(255, 120, 120, 120);
        }
        OlderButton = AddArrowButton(Window, false, &OnOlder);
        NewerButton = AddArrowButton(Window, true, &OnNewer);
        FooterLabel = AddLabel(Window, 0, 0, 10, 3);

        ShownBossLine.clear();
        ShownHeaderName = L"Player";
        ShownFooter.clear();
        LaidOutRows = -1;
        ApplyLock();
        Window->isVisible = false;
        PlaceMeter();
        Attach(Root, Window);

        LockToggle = WidgetKit::CreateLabeledCheckbox(CachedHost, 0, 0, L"Lock", ToggleTextWidth,
                                                      &Config.Locked, &OnLockChanged);
        Attach(Root, LockToggle);
        BossToggle = WidgetKit::CreateLabeledCheckbox(CachedHost, 0, 0, L"Boss", ToggleTextWidth,
                                                      &Config.BossOnly, &OnBossOnlyChanged);
        Attach(Root, BossToggle);
        TotalToggle = WidgetKit::CreateLabeledCheckbox(CachedHost, 0, 0, L"Total", ToggleTextWidth,
                                                       &Config.ShowTotal, &OnShowTotalChanged);
        Attach(Root, TotalToggle);
        SyncToggles();
        CreateTooltip(Root);
    }

    void DestroyWidgets() {
        GameHint::Destroy();
        Detach(MobPanel);
        RememberSettingsPlace();
        Detach(SettingsWindow);
        MobPanel = nullptr;
        MobHit = nullptr;
        for (auto& Hit : HelpHits) Hit = nullptr;
        SettingsWindow = nullptr;
        TargetWidget = nullptr;
        Detach(Tooltip);
        Detach(LockToggle);
        Detach(BossToggle);
        Detach(TotalToggle);
        Detach(Window);
        Window = nullptr;
        Tooltip = nullptr;
        LockToggle = nullptr;
        BossToggle = nullptr;
        TotalToggle = nullptr;
        AttachedRoot = nullptr;
        HoveredRow = -1;
    }

    void RefreshWindow(const Clock::time_point Now) {
        const Fight* Viewed = ViewedFight();
        LastView = Viewed ? BuildView(*Viewed, ViewIndex == 0, Now) : FightView{};

        const std::wstring Boss = BossLine(Now);
        const int PlayerCount = static_cast<int>(LastView.Players.size());
        const int VisibleRows = std::min(MaxRows, PlayerCount);
        Scroll = std::clamp(Scroll, 0, std::max(0, PlayerCount - MaxRows));
        const auto Self = std::find_if(LastView.Players.begin(), LastView.Players.end(),
                                       [](const PlayerTotal& P) { return P.Id == OwnCharacterId; });
        const int SelfIndex = Self == LastView.Players.end() ? -1 : static_cast<int>(Self - LastView.Players.begin());
        LayOutWindow(VisibleRows, !Boss.empty(), SelfIndex >= 0);
        SetText(BossLineLabel, ShownBossLine, Boss);
        SetText(HeaderName, ShownHeaderName, LastView.BossFiltered ? L"Player (boss)" : L"Player");

        wchar_t Buffer[128];
        const double TopDamage = LastView.Players.empty() ? 1.0 : static_cast<double>(LastView.Players.front().Damage);
        const auto UpdateSlot = [&](const int Slot, const int Index) {
            const PlayerTotal& P = LastView.Players[Index];
            MeterRow& Row = Rows[Slot];
            SlotPlayer[Slot] = Index;
            if (Row.Bar) {
                Row.Bar->fillPercent = std::clamp(static_cast<float>(P.Damage / TopDamage), 0.0f, 1.0f);
                Row.Bar->gaugeColor = BarColor(P.Id);
            }
            std::swprintf(Buffer, std::size(Buffer), L"%lld%%", P.Damage * 100 / std::max(1LL, LastView.Total));
            SetText(Row.Name, Row.ShownName, std::to_wstring(Index + 1) + L". " + NameOf(P.Id));
            SetText(Row.Dps, Row.ShownDps, FormatAmount(PlayerDps(P, LastView)) + L"/s");
            SetText(Row.Total, Row.ShownTotal, FormatAmount(static_cast<double>(P.Damage)));
            SetText(Row.Share, Row.ShownShare, Buffer);
        };
        for (int i = 0; i < Slots; i++) SlotPlayer[i] = -1;
        for (int i = 0; i < VisibleRows; i++) UpdateSlot(i, Scroll + i);
        if (SelfIndex >= 0) UpdateSlot(SelfSlot, SelfIndex);

        std::wstring Footer;
        if (ViewIndex == 0) {
            Footer = L"Live";
        } else {
            std::swprintf(Buffer, std::size(Buffer), L"%zu fight%ls ago", ViewIndex, ViewIndex == 1 ? L"" : L"s");
            Footer = Buffer;
            if (const Fight* Viewed = ViewedFight(); Config.SplitOnMap && Viewed) {
                if (const std::wstring Map = MapName(Viewed->MapId); !Map.empty()) Footer += L" | " + Map;
            }
        }
        if (PlayerCount > MaxRows) {
            std::swprintf(Buffer, std::size(Buffer), L" | %d-%d of %d", Scroll + 1, Scroll + VisibleRows, PlayerCount);
            Footer += Buffer;
        }
        SetText(FooterLabel, ShownFooter, Footer);
        Show(OlderButton, !Config.Locked && ViewIndex < History.size());
        Show(NewerButton, !Config.Locked && ViewIndex > 0);

    }

    void RefreshTooltip() {
        if (!Tooltip) return;
        const int Index = HoveredRow < 0 ? -1 : SlotPlayer[HoveredRow];
        if (Index < 0 || Index >= static_cast<int>(LastView.Players.size())) {
            if (Tooltip->isVisible) Tooltip->ToggleVisibilityRecursively(false);
            return;
        }
        const PlayerTotal& P = LastView.Players[Index];
        const auto Info = Players.find(P.Id);
        const PlayerClass Class = Info == Players.end() ? PlayerClass::Adventurer : Info->second.Class;

        wchar_t Buffer[128];
        SetText(TooltipTitle, ShownTooltipTitle, NameOf(P.Id) + L"'s damage");
        std::swprintf(Buffer, std::size(Buffer), L"%ls | hit %d%% | crit %d%% | max %ls", ClassName(Class),
                      P.Hits * 100 / std::max(1, P.Casts), P.Crits * 100 / std::max(1, P.Hits),
                      FormatAmount(P.MaxHit).c_str());
        SetText(TooltipStats, ShownTooltipStats, Buffer);

        const int SkillRows = std::min(MaxTooltipSkills, static_cast<int>(P.Skills.size()));
        const bool WasHidden = !Tooltip->isVisible;
        if (WasHidden) {
            Tooltip->isVisible = true;
            Show(TooltipTitle, true);
            Show(TooltipStats, true);
            Show(TooltipHeaderName, true);
            Show(TooltipHeaderValue, true);
            LaidOutTooltipRows = -1;
        }
        LayOutTooltip(SkillRows);
        for (int i = 0; i < SkillRows; i++) {
            const SkillTotal& S = P.Skills[i];
            const double Share = static_cast<double>(S.Damage) / static_cast<double>(std::max(1LL, P.Damage));
            std::swprintf(Buffer, std::size(Buffer), L"%d%% | %dx",static_cast<int>(Share * 100), S.Hits);
            UpdateBarRow(TooltipRows[i], SkillName(S.Skill), Buffer, static_cast<float>(Share), BarColor(P.Id));
        }
        for (int i = 0; i < MaxTooltipSkills; i++) {
            SetSkillIcon(TooltipIcons[i], i < SkillRows ? SkillIcon(P.Skills[i].Skill) : static_cast<int16_t>(-1));
        }

        const POINT Meter = AbsoluteTopLeft(Window);
        const POINT Bar = AbsoluteTopLeft(Rows[HoveredRow].Bar);
        const int16_t Height = static_cast<int16_t>(Tooltip->rect.bottom - Tooltip->rect.top);
        const int ScreenRight = AttachedRoot ? AttachedRoot->rect.right : 1920;
        const int ScreenBottom = AttachedRoot ? AttachedRoot->rect.bottom : 1080;
        int X = Meter.x + WindowWidth + 4;
        if (X + TooltipWidth > ScreenRight) X = Meter.x - TooltipWidth - 4;
        X = std::clamp(X, 0, std::max(0, ScreenRight - TooltipWidth));
        const int Y = std::clamp(static_cast<int>(Bar.y) - ContentTop, 0, std::max(0, ScreenBottom - Height));
        Place(Tooltip, static_cast<int16_t>(X), static_cast<int16_t>(Y), TooltipWidth, Height);
        if (WasHidden) Tooltip->BubbleUp();
    }

    void UpdateHover() {
        int Hovered = -1;
        POINT Cursor;
        if (Window && Window->isVisible && Window->isInteractable) {
            for (int i = 0; i < Slots; i++) {
                if (Records::Hovered(Rows[i].Hit)) {
                    Hovered = i;
                    break;
                }
            }
        } else if (Window && Window->isVisible && CursorInGame(Cursor) && Records::OnTop(Window, Cursor.x, Cursor.y)) {
            for (int i = 0; i < Slots; i++) {
                const TEWGuageWidget* Bar = Rows[i].Bar;
                if (!Bar || !Bar->isVisible) continue;
                const POINT TopLeft = AbsoluteTopLeft(Bar);
                if (Cursor.x >= TopLeft.x && Cursor.x < TopLeft.x + (Bar->rect.right - Bar->rect.left)
                    && Cursor.y >= TopLeft.y && Cursor.y < TopLeft.y + (Bar->rect.bottom - Bar->rect.top)) {
                    Hovered = i;
                    break;
                }
            }
        }
        if (Hovered != HoveredRow) {
            HoveredRow = Hovered;
            RefreshTooltip();
        }
        if (Window && Config.Locked && Window->isInteractable != (Hovered >= 0)) Window->isInteractable = Hovered >= 0;
    }

    void ScrollRows(const TickContext& Context) {
        if (HoveredRow < 0) return;
        const int MaxScroll = std::max(0, static_cast<int>(LastView.Players.size()) - MaxRows);
        const int NewScroll = std::clamp(Scroll + (Context.mouseWheel > 0 ? -1 : 1), 0, MaxScroll);
        if (NewScroll == Scroll) return;
        Scroll = NewScroll;
        RequestRefresh();
    }


    bool SafeCopy(void* Out, const uintptr_t Address, const size_t Size) {
        __try {
            std::memcpy(Out, reinterpret_cast<const void*>(Address), Size);
            return true;
        } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            return false;
        }
    }

    template <typename T>
    bool SafeRead(const uintptr_t Address, T& Out) {
        return SafeCopy(&Out, Address, sizeof(T));
    }

    bool IsTargetClass(const TLBSWidget* Widget) {
        uintptr_t VTable = 0, NamePtr = 0;
        uint8_t Length = 0;
        if (!SafeRead(reinterpret_cast<uintptr_t>(Widget), VTable)) return false;
        if (!SafeRead(VTable + VmtClassName, NamePtr) || !SafeRead(NamePtr, Length)) return false;
        char Text[sizeof(TargetClassName)];
        return Length == sizeof(TargetClassName) - 1 && SafeCopy(Text, NamePtr + 1, Length)
            && std::memcmp(Text, TargetClassName, Length) == 0;
    }

    TLBSWidget* FindTarget(TLBSWidget* Root, const Clock::time_point Now) {
        const TLBSWidgetList* Children = Root->childrenList;
        if (TargetWidget && Children->index_of(TargetWidget) >= 0) return TargetWidget;
        TargetWidget = nullptr;
        if (!TargetVTable && Now - LastTargetScan < TargetRescanInterval) return nullptr;
        LastTargetScan = Now;
        for (uint32_t i = 0; i < Children->count; i++) {
            TLBSWidget* Child = Children->list[i];
            if (!Child) continue;
            if (TargetVTable ? Child->vTable == TargetVTable : IsTargetClass(Child)) {
                TargetVTable = Child->vTable;
                TargetWidget = Child;
                break;
            }
        }
        return TargetWidget;
    }

    bool CursorOver(const TLBSWidget* Widget, const int16_t Width, const int16_t Height) {
        POINT Cursor;
        if (!Widget || !CursorInGame(Cursor)) return false;
        const POINT At = AbsoluteTopLeft(Widget);
        return Cursor.x >= At.x && Cursor.x < At.x + Width && Cursor.y >= At.y && Cursor.y < At.y + Height;
    }

    MobRow AddMobRow(TLBSWidget* Parent) {
        MobRow Row;
        Row.Bar = AddBar(Parent);
        Row.Name = AddLabel(Parent, 0, 0, 10, 1);
        Row.Total = AddLabel(Parent, 0, 0, 10, 2);
        return Row;
    }

    void PlaceMobRow(MobRow& Row, const int16_t Y) {
        const auto TextY = static_cast<int16_t>(Y + (MobBarHeight - 12) / 2);
        Place(Row.Bar, 0, Y, MobPanelWidth, MobBarHeight);
        PlaceLabel(Row.Name, 4, TextY, MobPanelWidth - MobTotalWidth - 8);
        PlaceLabel(Row.Total, MobPanelWidth - MobTotalWidth - 4, TextY, MobTotalWidth);
    }

    void ShowMobRow(MobRow& Row, const bool Visible) {
        Show(Row.Bar, Visible);
        Show(Row.Name, Visible);
        Show(Row.Total, Visible);
    }

    void CreateMobPanel(TLBSWidget* Root) {
        MobPanel = Widget::Create<TLBSWidget>(CachedHost);
        if (!MobPanel) return;
        Place(MobPanel, 0, 0, MobPanelWidth, MobTitleHeight);
        MobPanel->isInteractable = false;
        MobTitle = AddLabel(MobPanel, 0, 0, MobPanelWidth, 1);
        if (MobTitle) {
            PlaceLabel(MobTitle, 2, static_cast<int16_t>((MobBarHeight - 12) / 2), MobPanelWidth - 4);
            MobTitle->shadowStyle = 255;
            MobTitle->shadowColor = Color(255, 0, 0, 0);
            MobTitle->textColor = Color(255, 255, 215, 0);
        }
        for (auto& Row : MobRows) Row = AddMobRow(MobPanel);
        MobSeparator = AddBar(MobPanel);
        if (MobSeparator) {
            MobSeparator->fillPercent = 1.0f;
            MobSeparator->gaugeColor = Color(255, 120, 120, 120);
        }
        MobHit = Records::AddHitArea(CachedHost, MobPanel, 0, 0, MobPanelWidth, MobTitleHeight);
        ShownMobTitle.clear();
        LaidOutMobRows = -1;
        ShownMobId = 0;
        MobPanel->isVisible = false;
        Attach(Root, MobPanel);
    }

    void HideMobPanel() {
        if (!MobPanel) return;
        Show(MobPanel, false);
        if (MobPanel->isInteractable) MobPanel->isInteractable = false;
    }

    void LayOutMobPanel(const int Rows, const bool HasSelf) {
        if (Rows == LaidOutMobRows && HasSelf == LaidOutMobSelf) return;
        LaidOutMobRows = Rows;
        LaidOutMobSelf = HasSelf;
        int16_t Y = MobTitleHeight;
        for (int i = 0; i < MaxMobRows; i++) {
            PlaceMobRow(MobRows[i], static_cast<int16_t>(Y + i * MobRowHeight));
            ShowMobRow(MobRows[i], i < Rows);
        }
        Y = static_cast<int16_t>(Y + Rows * MobRowHeight);
        Show(MobSeparator, HasSelf);
        ShowMobRow(MobRows[MobSelfSlot], HasSelf);
        if (HasSelf) {
            Place(MobSeparator, 0, Y, MobPanelWidth, 2);
            Y = static_cast<int16_t>(Y + MobSelfGap);
            PlaceMobRow(MobRows[MobSelfSlot], Y);
            Y = static_cast<int16_t>(Y + MobRowHeight);
        }
        MobPanel->rect.bottom = static_cast<int16_t>(MobPanel->rect.top + Y);
    }

    void RefreshMobPanel(const std::unordered_map<long, long long>& Damage) {
        std::vector<std::pair<long, long long>> Dealt(Damage.begin(), Damage.end());
        std::sort(Dealt.begin(), Dealt.end(), [](const auto& A, const auto& B) { return A.second > B.second; });

        const int Count = static_cast<int>(Dealt.size());
        const int Rows = std::min(Config.MobRows, Count);
        MobScroll = std::clamp(MobScroll, 0, std::max(0, Count - Config.MobRows));
        const auto Self = std::find_if(Dealt.begin(), Dealt.end(), [](const auto& Entry) { return Entry.first == OwnCharacterId; });
        const int SelfIndex = Self == Dealt.end() ? -1 : static_cast<int>(Self - Dealt.begin());
        const bool HasSelf = SelfIndex >= 0 && (SelfIndex < MobScroll || SelfIndex >= MobScroll + Rows);
        LayOutMobPanel(Rows, HasSelf);

        const double Top = static_cast<double>(std::max(1LL, Dealt.front().second));
        const auto Update = [&](const int Slot, const int Index) {
            const auto& [Player, Total] = Dealt[Index];
            MobRow& Row = MobRows[Slot];
            if (Row.Bar) {
                Row.Bar->fillPercent = std::clamp(static_cast<float>(Total / Top), 0.0f, 1.0f);
                Row.Bar->gaugeColor = BarColor(Player);
            }
            SetText(Row.Name, Row.ShownName, std::to_wstring(Index + 1) + L". " + NameOf(Player));
            SetText(Row.Total, Row.ShownTotal, FormatAmount(static_cast<double>(Total)));
        };
        for (int i = 0; i < Rows; i++) Update(i, MobScroll + i);
        if (HasSelf) Update(MobSelfSlot, SelfIndex);

        std::wstring Title = L"Damage";
        if (Count > Config.MobRows) {
            wchar_t Buffer[48];
            std::swprintf(Buffer, std::size(Buffer), L"  %d-%d of %d", MobScroll + 1, MobScroll + Rows, Count);
            Title += Buffer;
        }
        SetText(MobTitle, ShownMobTitle, Title);
    }

    void SyncMobPanel(TLBSWidget* Root, const TickContext& Context, const Clock::time_point Now) {
        if (!MobPanel) return;
        TLBSWidget* Target = FindTarget(Root, Now);
        const bool Targeted = Config.ShowMobDamage && Target && Target->isVisible && SelectedType == TargetMonster;
        const auto Damage = Targeted ? MobDamage.find(SelectedId) : MobDamage.end();
        if (Damage == MobDamage.end() || Damage->second.empty()) {
            HideMobPanel();
            return;
        }
        if (SelectedId != ShownMobId) {
            ShownMobId = SelectedId;
            MobScroll = 0;
            LastMobRefresh = {};
        }

        const POINT At = AbsoluteTopLeft(Target);
        const auto X = static_cast<int16_t>(std::max(0L, At.x - MobPanelGap - MobPanelWidth));
        const auto Y = static_cast<int16_t>(At.y);
        if (MobPanel->rect.left != X || MobPanel->rect.top != Y) {
            Place(MobPanel, X, Y, MobPanelWidth, static_cast<int16_t>(MobPanel->rect.bottom - MobPanel->rect.top));
        }

        const auto PanelHeight = static_cast<int16_t>(MobPanel->rect.bottom - MobPanel->rect.top);
        if (MobHit && (MobHit->rect.right != MobPanelWidth || MobHit->rect.bottom != PanelHeight)) {
            MobHit->rect = {0, 0, MobPanelWidth, PanelHeight};
        }
        const bool Hovered = MobPanel->isVisible
            && (MobPanel->isInteractable ? Records::Hovered(MobHit)
                                         : CursorOver(MobPanel, MobPanelWidth, PanelHeight) && Records::OnTop(MobPanel, Context));
        if (MobPanel->isInteractable != Hovered) MobPanel->isInteractable = Hovered;
        if (Hovered && Context.mouseWheel != 0) {
            MobScroll += Context.mouseWheel > 0 ? -1 : 1;
            LastMobRefresh = {};
        }
        if (Now - LastMobRefresh >= RefreshInterval) {
            LastMobRefresh = Now;
            RefreshMobPanel(Damage->second);
        }
        if (!MobPanel->isVisible) Show(MobPanel, true);
        const TLBSWidgetList* Siblings = Root->childrenList;
        if (Siblings->index_of(MobPanel) < Siblings->index_of(Target)) MobPanel->BubbleUp();
    }

    TEWGraphicButtonWidget* CheckboxOf(TLBSWidget* LabeledCheckbox) {
        if (!LabeledCheckbox || !LabeledCheckbox->childrenList || LabeledCheckbox->childrenList->count == 0) return nullptr;
        return static_cast<TEWGraphicButtonWidget*>(LabeledCheckbox->childrenList->list[0]);
    }

    void SyncSettingsChecks() {
        WidgetKit::SetChecked(ShowMeterBox, Config.ShowMeter);
        WidgetKit::SetChecked(ShowMobBox, Config.ShowMobDamage);
        WidgetKit::SetChecked(SplitBox, Config.SplitFights);
        WidgetKit::SetChecked(SplitMapBox, Config.SplitOnMap);
        for (size_t i = 0; i < std::size(MobRowChoices); i++) {
            MobRowChecked[i] = Config.MobRows == MobRowChoices[i];
            WidgetKit::SetChecked(MobRowBoxes[i], MobRowChecked[i]);
        }
    }

    void __cdecl OnShowMeterChanged(void*) {
        MeterShown = Config.ShowMeter;
        if (Window) {
            Show(Window, Config.ShowMeter);
            if (Config.ShowMeter) Window->BubbleUp();
        }
        SaveSettings();
        RequestRefresh();
    }

    void __cdecl OnSplitChanged(void*) {
        SaveSettings();
        RequestRefresh();
    }

    void ReadSecondsBox() {
        if (!SecondsBox.Edit) return;
        const std::wstring Text = SecondsBox.Edit->GetText();
        if (Text == ShownSeconds) return;
        ShownSeconds = Text;
        int Seconds = 0;
        for (const wchar_t Ch : Text) {
            if (Ch < L'0' || Ch > L'9') return;
            Seconds = std::min(Seconds * 10 + (Ch - L'0'), 24 * 60 * 60);
        }
        if (Seconds < 1 || Seconds == Config.FightSeconds) return;
        Config.FightSeconds = Seconds;
        SaveSettings();
        RequestRefresh();
    }

    void __cdecl OnShowMobChanged(void*) {
        SaveSettings();
    }

    void __cdecl OnMobRowsChanged(void* Argument) {
        Config.MobRows = MobRowChoices[reinterpret_cast<uintptr_t>(Argument)];
        SyncSettingsChecks();
        LastMobRefresh = {};
        SaveSettings();
    }

    TEWLabel* AddHelpLabel(TLBSWidget* Parent, const int16_t X, const int16_t Y) {
        TEWLabel* Label = Widget::Create<TEWLabel>(CachedHost);
        if (!Label) return nullptr;
        Place(Label, X, Y, HelpWidth, 30);
        Label->textAlignment = 1;
        Label->pxPerLine = HelpWidth;
        Label->textColor = Color(255, 150, 200, 255);
        Label->SetText(L"(?)");
        Attach(Parent, Label);
        for (auto& Hit : HelpHits) {
            if (Hit) continue;
            Hit = Records::AddHitArea(CachedHost, Parent, X, Y, HelpWidth, 15);
            break;
        }
        return Label;
    }

    TLBSWidget* AddSetting(const int16_t X, const int16_t Y, const wchar_t* Text, bool* Value, const WidgetKit::ClickFn OnChange,
                           void* Argument, int16_t& Right) {
        const auto TextWidth = static_cast<int16_t>(std::max(10, WidgetKit::MeasureText(1, Text) + 4));
        TLBSWidget* Setting = WidgetKit::CreateLabeledCheckbox(CachedHost, X, Y, Text, TextWidth, Value, OnChange, Argument);
        Attach(DpsPage, Setting);
        Right = static_cast<int16_t>(X + 15 + 4 + TextWidth);
        return Setting;
    }

    void ShowPage(const int Index) {
        ActivePage = Index;
        if (Config.Page != Index) {
            Config.Page = Index;
            SaveSettings();
        }
        for (int i = 0; i < PageCount; i++) {
            Show(Pages[i], i == Index);
            WidgetKit::SetTabActive(PageTabs[i], i == Index);
        }
        if (SettingsWindow && PageHeights[Index]) SetPanelHeight(SettingsWindow, PageHeights[Index]);
        GameHint::Hide();
    }

    void __cdecl OnPageTab(void* Argument) {
        ShowPage(static_cast<int>(reinterpret_cast<intptr_t>(Argument)));
    }

    void CreateSettingsWindow(TLBSWidget* Root) {
        const WidgetKit::WindowDesc Desc{WidgetKit::WindowStyle::Plain, 0, 0, SettingsWidth, 130, L"Trackers", true};
        SettingsWindow = WidgetKit::CreateGameWindow(CachedHost, Desc);
        if (!SettingsWindow) return;
        SettingsWindow->isRestrictedToScreen = true;

        for (int i = 0; i < PageCount; i++) {
            Pages[i] = Widget::Create<TLBSWidget>(CachedHost);
            if (!Pages[i]) continue;
            Place(Pages[i], 0, PageShift, SettingsWidth, 600);
            Attach(SettingsWindow, Pages[i]);
        }
        const wchar_t* PageNames[PageCount] = {L"DPS Meter", L"EXP", L"Raids"};
        constexpr int16_t TabWidth = (SettingsWidth - 2 * Pad - 2 * 6) / PageCount;
        for (int i = 0; i < PageCount; i++) {
            PageTabs[i] = WidgetKit::CreateTab(CachedHost, static_cast<int16_t>(Pad + i * (TabWidth + 6)), PageTabsTop, TabWidth,
                                               PageNames[i], &OnPageTab, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
            Attach(SettingsWindow, PageTabs[i].Button);
        }
        DpsPage = Pages[0] ? Pages[0] : SettingsWindow;

        int16_t Right = 0;
        int16_t Y = ContentTop + 4;
        ShowMeterBox = CheckboxOf(AddSetting(Pad, Y, L"Show meter", &Config.ShowMeter, &OnShowMeterChanged, nullptr, Right));
        ShowMeterHelp = AddHelpLabel(DpsPage, static_cast<int16_t>(Right + 2), Y);
        Y += 24;
        ShowMobBox = CheckboxOf(AddSetting(Pad, Y, L"Show damage per mob", &Config.ShowMobDamage, &OnShowMobChanged, nullptr, Right));
        ShowMobHelp = AddHelpLabel(DpsPage, static_cast<int16_t>(Right + 2), Y);
        Y += 24;

        if (TEWLabel* RowsLabel = Widget::Create<TEWLabel>(CachedHost)) {
            constexpr wchar_t RowsText[] = L"Players per mob:";
            const auto Width = static_cast<int16_t>(std::max(10, WidgetKit::MeasureText(1, RowsText) + 4));
            Place(RowsLabel, static_cast<int16_t>(Pad + 19), Y, Width, 30);
            RowsLabel->textAlignment = 1;
            RowsLabel->pxPerLine = Width;
            RowsLabel->SetText(RowsText);
            Attach(DpsPage, RowsLabel);
            Right = static_cast<int16_t>(Pad + 19 + Width + 8);
        }
        for (size_t i = 0; i < std::size(MobRowChoices); i++) {
            const std::wstring Text = std::to_wstring(MobRowChoices[i]);
            int16_t Next = 0;
            MobRowBoxes[i] = CheckboxOf(AddSetting(Right, Y, Text.c_str(), &MobRowChecked[i], &OnMobRowsChanged,
                                                   reinterpret_cast<void*>(i), Next));
            Right = static_cast<int16_t>(Next + 10);
        }
        Y += 24;

        SplitBox = CheckboxOf(AddSetting(Pad, Y, L"Split fights after", &Config.SplitFights, &OnSplitChanged, nullptr, Right));
        SecondsBox = WidgetKit::CreateEditBox(CachedHost, static_cast<int16_t>(Right + 4), static_cast<int16_t>(Y - 3), 44, 21);
        if (SecondsBox.Frame) Attach(DpsPage, SecondsBox.Frame);
        ShownSeconds = std::to_wstring(Config.FightSeconds);
        if (SecondsBox.Edit) SecondsBox.Edit->SetText(ShownSeconds.c_str());
        Right = static_cast<int16_t>(Right + 4 + 44 + 6);
        if (TEWLabel* Unit = Widget::Create<TEWLabel>(CachedHost)) {
            constexpr wchar_t UnitText[] = L"s idle";
            const auto Width = static_cast<int16_t>(std::max(10, WidgetKit::MeasureText(1, UnitText) + 4));
            Place(Unit, Right, Y, Width, 30);
            Unit->textAlignment = 1;
            Unit->pxPerLine = Width;
            Unit->SetText(UnitText);
            Attach(DpsPage, Unit);
            Right = static_cast<int16_t>(Right + Width);
        }
        SplitHelp = AddHelpLabel(DpsPage, static_cast<int16_t>(Right + 2), Y);
        Y += 24;

        SplitMapBox = CheckboxOf(AddSetting(Pad, Y, L"Split fights on map change", &Config.SplitOnMap, &OnSplitChanged, nullptr, Right));
        SplitMapHelp = AddHelpLabel(DpsPage, static_cast<int16_t>(Right + 2), Y);
        Y += 24;

        PageHeights[0] = static_cast<uint16_t>(PageShift + Y + 6);
        if (Pages[1]) PageHeights[1] = static_cast<uint16_t>(PageShift + ExpSession::BuildPage(Pages[1], SettingsWidth));
        if (Pages[2]) PageHeights[2] = static_cast<uint16_t>(PageShift + RaidHistory::BuildPage(Pages[2], SettingsWidth));
        ShowPage(Config.Page);
        SyncSettingsChecks();
        SettingsPlaced = false;
        SettingsWindow->isVisible = false;
        Attach(Root, SettingsWindow);
    }

    WidgetKit::WindowRaiser SettingsRaiser;

    void SyncSettings(TLBSWidget* Root, const TickContext& Context) {
        if (!SettingsWindow || !SettingsWindow->isVisible) {
            GameHint::Hide();
            return;
        }
        if (ActivePage == 1) {
            ExpSession::TickPage(Root, Context);
            return;
        }
        if (ActivePage == 2) {
            RaidHistory::TickPage(Root, Context);
            return;
        }
        if (ActivePage != 0) {
            GameHint::Hide();
            return;
        }
        ReadSecondsBox();
        static const std::vector<std::wstring> SplitHelpLines = {
            L"On: a new fight starts once nobody has hit",
            L"anything for this many seconds.",
            L"Off: idle time never ends a fight.",
        };
        static const std::vector<std::wstring> SplitMapLines = {
            L"On: changing maps ends the fight, and older",
            L"fights show the map they were on.",
            L"Off: a fight keeps going across maps.",
        };
        static const std::vector<std::wstring> MeterHelp = {
            L"Shows the DPS meter window.",
            L"Lock, Boss and Total sit in its title bar;",
            L"hover a player to see their skills.",
        };
        static const std::vector<std::wstring> MobHelp = {
            L"While you target a monster that has taken damage,",
            L"a small list left of its window shows who dealt",
            L"the most to it. Scroll it with the mouse wheel.",
        };
        const std::vector<std::wstring>* Lines = nullptr;
        const std::vector<std::wstring>* HelpLines[4] = {&MeterHelp, &MobHelp, &SplitHelpLines, &SplitMapLines};
        for (int i = 0; i < 4; i++) {
            if (Records::Hovered(HelpHits[i])) Lines = HelpLines[i];
        }
        if (!Lines) {
            GameHint::Hide();
            return;
        }
        if (GameHint::Create(CachedHost, Root)) GameHint::Show(*Lines, Context.mouseX, Context.mouseY);
    }

    void ToggleSettings() {
        if (!SettingsWindow) return;
        if (SettingsWindow->isVisible) {
            RememberSettingsPlace();
            Show(SettingsWindow, false);
            GameHint::Hide();
            return;
        }
        if (!SettingsPlaced && AttachedRoot) {
            SettingsPlaced = true;
            const int16_t Width = static_cast<int16_t>(SettingsWindow->rect.right - SettingsWindow->rect.left);
            const int16_t Height = static_cast<int16_t>(SettingsWindow->rect.bottom - SettingsWindow->rect.top);
            const bool Saved = Config.SettingsX >= 0 && Config.SettingsY >= 0 && Config.SettingsX < ScreenWidth() - 40
                && Config.SettingsY < ScreenHeight() - 40;
            Place(SettingsWindow, static_cast<int16_t>(Saved ? Config.SettingsX : (ScreenWidth() - Width) / 2),
                  static_cast<int16_t>(Saved ? Config.SettingsY : (ScreenHeight() - Height) / 2), Width, Height);
        }
        ExpSession::ResetView();
        RaidHistory::ResetView();
        SyncSettingsChecks();
        Show(SettingsWindow, true);
        SettingsWindow->BubbleUp();
    }
}

namespace DpsMeter {
    void Startup(const ModHost* Host) {
        CachedHost = Host;
        LoadSettings();
        Packet::SubscribePacket(Host, &OnCInfo);
        Packet::SubscribePacket(Host, &OnIn);
        Packet::SubscribePacket(Host, &OnCMap);
        Packet::SubscribeSentPacket(Host, &OnNcif);
        Packet::SubscribePacket(Host, &OnRBoss);
        Packet::SubscribePacket(Host, &OnSu);
    }

    void Shutdown() {
        if (Window && !Window->isBeingDragged) {
            RememberMeterPosition();
            SaveSettings();
        }
        DestroyWidgets();
        CachedHost = nullptr;
    }

    void EarlyTick(TLBSWidget* Root, const TickContext tickContext) {
        if (!CachedHost || !Root || !Root->childrenList) return;

        if (!tickContext.isPlayerLoaded) {
            Show(Window, false);
            Show(Tooltip, false);
            Show(LockToggle, false);
            Show(BossToggle, false);
            Show(TotalToggle, false);
            HideMobPanel();
            GameHint::Hide();
            WasLoading = true;
            HoveredRow = -1;
            if (Config.SplitOnMap) EndCurrent = HasCurrent;
            return;
        }

        if (Root != AttachedRoot) {
            Window = nullptr;
            Tooltip = nullptr;
            LockToggle = nullptr;
            BossToggle = nullptr;
            TotalToggle = nullptr;
            MobPanel = nullptr;
            MobHit = nullptr;
            for (auto& Hit : HelpHits) Hit = nullptr;
            SettingsWindow = nullptr;
            for (int i = 0; i < PageCount; i++) {
                Pages[i] = nullptr;
                PageTabs[i] = {};
            }
            DpsPage = nullptr;
            ExpSession::ForgetPage();
            RaidHistory::ForgetPage();
            TargetWidget = nullptr;
            GameHint::Destroy();
            AttachedRoot = Root;
            CreateMeterWindow(Root);
            CreateMobPanel(Root);
            CreateSettingsWindow(Root);
            MeterShown = false;
            WasLoading = true;
        }
        if (!Window) {
            CachedHost->ReportStatus(ModHealthLevel::Broken, "Couldn't create the DPS window.");
            return;
        }
        CachedHost->ReportStatus(ModHealthLevel::Ok, "");

        if (WasLoading) {
            WasLoading = false;
            Show(Window, Config.ShowMeter);
            MeterShown = Config.ShowMeter;
        } else if (MeterShown && !Window->isVisible) {
            MeterShown = false;
            Config.ShowMeter = false;
            SyncSettingsChecks();
            SaveSettings();
        }
        SyncToggles();
        SyncMeterPosition();
        SyncMobPanel(Root, tickContext, Clock::now());
        SyncSettings(Root, tickContext);
        SettingsRaiser.Tick(SettingsWindow, tickContext);
        if (!Window->isVisible) {
            if (Tooltip && Tooltip->isVisible) Tooltip->ToggleVisibilityRecursively(false);
            HoveredRow = -1;
            return;
        }

        UpdateHover();
        if (tickContext.mouseWheel != 0) ScrollRows(tickContext);
        const auto Now = Clock::now();
        if (Now - LastRefresh < RefreshInterval) return;
        LastRefresh = Now;
        RefreshWindow(Now);
        RefreshTooltip();
    }

    void ToggleWindow() {
        ToggleSettings();
    }
}
