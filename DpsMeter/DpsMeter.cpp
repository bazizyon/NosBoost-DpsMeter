#define NOMINMAX
#include <windows.h>

#include "ModContract.h"
#include "WidgetKit.h"
#include "TLBSWidget.h"
#include "TEWLabel.h"
#include "TEWGuageWidget.h"
#include "TEWCustomPanelWidget.h"
#include "TEWGraphicButtonWidget.h"
#include "CInfoPacket.h"
#include "InPacket.h"
#include "RBossPacket.h"
#include "SuPacket.h"

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr auto FightTimeout = std::chrono::seconds(5);
    constexpr auto RefreshInterval = std::chrono::milliseconds(250);
    constexpr double MinActiveSeconds = 3.0;
    constexpr int MaxRows = 10;
    constexpr int SelfSlot = MaxRows;
    constexpr int Slots = MaxRows + 1;
    constexpr int MaxTooltipSkills = 8;
    constexpr size_t MaxHistory = 5;

    constexpr uint16_t WindowWidth = 380;
    constexpr uint16_t TooltipWidth = 320;
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
    };

    std::wstring SkillName(const int SkillVNum) {
        return L"Skill " + std::to_wstring(SkillVNum);
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

    Fight Current;
    bool HasCurrent = false;
    bool EndCurrent = false;
    std::deque<Fight> History;
    size_t ViewIndex = 0;

    struct Settings {
        int16_t X = 20;
        int16_t Y = 200;
        bool Locked = false;
        bool BossOnly = false;
        bool ShowTotal = true;
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
        std::wstring ShownName, ShownDps, ShownTotal, ShownShare;
    };

    TLBSWidget* AttachedRoot = nullptr;



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
    std::wstring ShownBossLine, ShownFooter;
    int LaidOutRows = -1;
    bool LaidOutBossLine = false;
    bool LaidOutSelf = false;

    TEWCustomPanelWidget* Tooltip = nullptr;
    TEWLabel* TooltipTitle = nullptr;
    TEWLabel* TooltipStats = nullptr;
    TEWLabel* TooltipHeaderName = nullptr;
    TEWLabel* TooltipHeaderValue = nullptr;
    BarRow TooltipRows[MaxTooltipSkills];
    std::wstring ShownTooltipTitle, ShownTooltipStats;
    int LaidOutTooltipRows = -1;

    FightView LastView;
    int SlotPlayer[Slots] = {};
    int HoveredRow = -1;
    int Scroll = 0;
    Clock::time_point LastRefresh;
    bool HiddenForLoading = false;

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
        return Dir.substr(0, Dir.find_last_of("\\/") + 1) + "mods\\DpsMeter.ini";
    }

    void LoadSettings() {
        const std::string Path = SettingsPath();
        Config.X = static_cast<int16_t>(GetPrivateProfileIntA("DpsMeter", "X", Config.X, Path.c_str()));
        Config.Y = static_cast<int16_t>(GetPrivateProfileIntA("DpsMeter", "Y", Config.Y, Path.c_str()));
        Config.Locked = GetPrivateProfileIntA("DpsMeter", "Locked", 0, Path.c_str()) != 0;
        Config.BossOnly = GetPrivateProfileIntA("DpsMeter", "BossOnly", 0, Path.c_str()) != 0;
        Config.ShowTotal = GetPrivateProfileIntA("DpsMeter", "ShowTotal", 1, Path.c_str()) != 0;
    }

    void SaveSettings() {
        const std::string Path = SettingsPath();
        WritePrivateProfileStringA("DpsMeter", "X", std::to_string(Config.X).c_str(), Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "Y", std::to_string(Config.Y).c_str(), Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "Locked", Config.Locked ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "BossOnly", Config.BossOnly ? "1" : "0", Path.c_str());
        WritePrivateProfileStringA("DpsMeter", "ShowTotal", Config.ShowTotal ? "1" : "0", Path.c_str());
    }

    void StartNewFight(const Clock::time_point Now) {
        if (HasCurrent && !Current.Hits.empty()) {
            History.push_front(std::move(Current));
            if (History.size() > MaxHistory) History.pop_back();
            if (ViewIndex > 0) ViewIndex = std::min(ViewIndex + 1, History.size());
        }
        if (ViewIndex == 0) Scroll = 0;
        Current = Fight{};
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
        View.Live = IsCurrent && !EndCurrent && Now - Source.LastHit <= FightTimeout;
        View.End = View.Live ? Now : Source.LastHit;

        std::unordered_map<long, PlayerTotal> Totals;
        std::unordered_map<long, std::unordered_map<int, SkillTotal>> Skills;
        Clock::time_point FightStart = View.End;
        for (const Hit& H : Source.Hits) {
            if (Config.BossOnly && !Bosses.contains(H.Target)) continue;
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

    bool OnRBoss(const Packet::RBossPacket& Packet) {
        Bosses[Packet.entityID] = {Packet.HP, Packet.maxHP};
        return true;
    }

    bool OnSu(const Packet::SuPacket& Packet) {
        if (Packet.userEntityType != EntityType::Player || Packet.targetEntityType != EntityType::Monster) return true;
        if (Packet.hitMode == HitMode::SuccessfulBuff) return true;

        const auto Now = Clock::now();
        if (!HasCurrent || EndCurrent || Now - Current.LastHit > FightTimeout) {
            StartNewFight(Now);
        }
        Current.LastHit = Now;

        const bool Miss = Packet.hitMode == HitMode::Miss || Packet.hitMode == HitMode::LongRangeMiss
                       || Packet.hitMode == HitMode::UnknownMiss;
        const bool Crit = Packet.hitMode == HitMode::CriticalAttack || Packet.hitMode == HitMode::UnknownCriticalAttack;
        Current.Hits.push_back({Packet.userEntityID, Packet.targetEntityID, Packet.skillVNum,
                                std::max(0, Packet.damage), Crit, Miss, Now});
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

    void PlaceBarRow(BarRow& Row, const int16_t Y, const int16_t Width) {
        constexpr int16_t TextInset = 4;
        const int16_t TextY = static_cast<int16_t>(Y + (BarHeight - 12) / 2);
        Place(Row.Bar, Pad, Y, Width, BarHeight);
        Place(Row.Name, Pad + TextInset, static_cast<int16_t>(TextY + TextNudge), Width * 3 / 5, 30);
        Place(Row.Value, static_cast<int16_t>(Pad + Width * 2 / 5 - TextInset), static_cast<int16_t>(TextY + TextNudge),
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
        return Row;
    }

    void PlaceMeterRow(MeterRow& Row, const int16_t Y, const int16_t Width) {
        Place(Row.Bar, Pad, Y, Width, BarHeight);
        PlaceColumns(Row.Name, Row.Dps, Row.Total, Row.Share, static_cast<int16_t>(Y + (BarHeight - 12) / 2), Width);
    }

    void ShowMeterRow(MeterRow& Row, const bool Visible) {
        Show(Row.Bar, Visible);
        Show(Row.Name, Visible);
        Show(Row.Dps, Visible);
        Show(Row.Total, Visible && Config.ShowTotal);
        Show(Row.Share, Visible);
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

    void LayOutTooltip(const int SkillRows) {
        if (SkillRows == LaidOutTooltipRows) return;
        LaidOutTooltipRows = SkillRows;
        const int16_t Width = TooltipWidth - 2 * Pad;
        const int16_t RowsTop = ContentTop + 2 * LineHeight;
        PlaceHeaders(TooltipHeaderName, TooltipHeaderValue, ContentTop + LineHeight, Width);
        for (int i = 0; i < MaxTooltipSkills; i++) {
            PlaceBarRow(TooltipRows[i], static_cast<int16_t>(RowsTop + i * RowHeight), Width);
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
        Tooltip->ToggleVisibilityRecursively(false);
        ShownTooltipTitle.clear();
        ShownTooltipStats.clear();
        LaidOutTooltipRows = -1;
        Attach(Root, Tooltip);
    }

    void CreateMeterWindow(TLBSWidget* Root) {
        const WidgetKit::WindowDesc Desc{
            WidgetKit::WindowStyle::Plain, Config.X, Config.Y, WindowWidth, 200, L"DPS Meter", true
        };
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
        ShownFooter.clear();
        LaidOutRows = -1;
        ApplyLock();
        Window->isVisible = false;
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
        }
        if (PlayerCount > MaxRows) {
            std::swprintf(Buffer, std::size(Buffer), L" | %d-%d of %d", Scroll + 1, Scroll + VisibleRows, PlayerCount);
            Footer += Buffer;
        }
        SetText(FooterLabel, ShownFooter, Footer);
        Show(OlderButton, !Config.Locked && ViewIndex < History.size());
        Show(NewerButton, !Config.Locked && ViewIndex > 0);

        if (!Window->isBeingDragged && (Window->rect.left != Config.X || Window->rect.top != Config.Y)) {
            Config.X = Window->rect.left;
            Config.Y = Window->rect.top;
            SaveSettings();
        }

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
        if (Window && Window->isVisible && CursorInGame(Cursor)) {
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
        const POINT TopLeft = AbsoluteTopLeft(Window);
        if (Context.mouseX < TopLeft.x || Context.mouseX >= TopLeft.x + (Window->rect.right - Window->rect.left)
            || Context.mouseY < TopLeft.y || Context.mouseY >= TopLeft.y + (Window->rect.bottom - Window->rect.top)) {
            return;
        }
        const int MaxScroll = std::max(0, static_cast<int>(LastView.Players.size()) - MaxRows);
        const int NewScroll = std::clamp(Scroll + (Context.mouseWheel > 0 ? -1 : 1), 0, MaxScroll);
        if (NewScroll == Scroll) return;
        Scroll = NewScroll;
        RequestRefresh();
    }
}

extern "C" {
    __declspec(dllexport) const ModClassRequirement* ModGetRequirements(size_t* OutCount) {
        *OutCount = std::size(Requirements);
        return Requirements;
    }

    __declspec(dllexport) void ModStartup(
        ImGuiContext*, ImGuiMemAllocFunc, ImGuiMemFreeFunc, void*, const ModHost* Host
    ) {
        CachedHost = Host;
        LoadSettings();
        Packet::SubscribePacket(Host, &OnCInfo);
        Packet::SubscribePacket(Host, &OnIn);
        Packet::SubscribePacket(Host, &OnRBoss);
        Packet::SubscribePacket(Host, &OnSu);
    }

    __declspec(dllexport) void ModShutdown() {
        if (Window) {
            Config.X = Window->rect.left;
            Config.Y = Window->rect.top;
            SaveSettings();
        }
        DestroyWidgets();
        CachedHost = nullptr;
    }

    __declspec(dllexport) void ModEarlyTick(const TLBSWidget* RootWidget, const TickContext tickContext) {
        TLBSWidget* Root = const_cast<TLBSWidget*>(RootWidget);
        if (!CachedHost || !Root || !Root->childrenList) return;

        if (!tickContext.isPlayerLoaded) {
            if (Window && Window->isVisible) {
                Window->isVisible = false;
                HiddenForLoading = true;
            }
            Show(Tooltip, false);
            Show(LockToggle, false);
            Show(BossToggle, false);
            Show(TotalToggle, false);
            HoveredRow = -1;
            EndCurrent = HasCurrent;
            return;
        }

        if (Root != AttachedRoot) {
            Window = nullptr;
            Tooltip = nullptr;
            LockToggle = nullptr;
            BossToggle = nullptr;
            TotalToggle = nullptr;
            AttachedRoot = Root;
            CreateMeterWindow(Root);
        }
        if (!Window) {
            CachedHost->ReportStatus(ModHealthLevel::Broken, "Couldn't create the DPS window.");
            return;
        }
        CachedHost->ReportStatus(ModHealthLevel::Ok, "");

        if (HiddenForLoading) {
            HiddenForLoading = false;
            Window->isVisible = true;
        }
        SyncToggles();
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

    __declspec(dllexport) void ModTick(const TLBSWidget*, TickContext) {}

    __declspec(dllexport) void ModToggleMainWindow() {
        if (!Window) return;
        Window->isVisible = !Window->isVisible;
        HiddenForLoading = false;
    }
}
