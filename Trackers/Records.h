#pragma once
#include "ModContract.h"
#include "TEWLabel.h"
#include "TEWGraphicButtonWidget.h"
#include "TEWControlWidget.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Records {
    int64_t NowMs();
    std::filesystem::path DataDir();
    std::vector<std::string> Split(const std::string& Text, char By);
    int64_t Num(const std::string& Text);

    void Escape(std::string& Out, const std::string& Text);

    struct Json {
        enum Kind { Null, Number, String, Array, Object } Type = Null;
        double N = 0;
        std::string S;
        std::vector<Json> Items;
        std::vector<std::pair<std::string, Json>> Fields;

        const Json* operator[](const char* Key) const;
        int64_t Int(const char* Key, int64_t Default = 0) const;
        double Real(const char* Key, double Default = 0) const;
        std::string Str(const char* Key) const;
        int64_t At(size_t Index) const;
    };
    bool ParseJson(const std::string& Text, Json& Out);

    std::vector<std::string> ReadLines(const std::filesystem::path& File);
    void AppendLine(const std::filesystem::path& File, const std::string& Line);
    void WriteFile(const std::filesystem::path& File, const std::string& Text);

    std::wstring Wide(const std::string& Text);
    std::string Utf8(const std::wstring& Text);
    std::wstring FromUtf8(const std::string& Text);
    std::wstring Compact(int64_t Value);
    std::wstring Duration(int64_t Ms);
    std::wstring When(int64_t Ms);
    std::wstring WhenWithYear(int64_t Ms);
    std::wstring Ago(int64_t Ms);
    void CopyToClipboard(const std::wstring& Text);
    std::wstring Percent(int64_t Part, int64_t Whole);
    std::wstring Fit(const std::wstring& Text, int Width);
    std::wstring SpName(int Morph);
    std::wstring SkillLabel(int SkillVnum);
    int SkillKey(int SkillVnum, long Effect);
    int16_t SkillIcon(int SkillVnum);
    std::wstring MapLabel(int MapId);
    std::wstring MinimapMapName(const TLBSWidget* Root);

    inline const Color Plain(255, 255, 255, 255);
    inline const Color Hint(255, 190, 190, 190);
    inline const Color Gold(255, 255, 214, 92);
    inline const Color Good(255, 140, 220, 120);
    inline const Color Bad(255, 255, 110, 90);
    inline const Color Picked(255, 255, 240, 180);

    TEWLabel* AddLabel(const ModHost* Host, TLBSWidget* Parent, int16_t X, int16_t Y, int16_t Width, uint8_t Align = 1);
    void SetText(TEWLabel* Label, const std::wstring& Text, const Color& Tint, int Width);
    void Show(TLBSWidget* Widget, bool Visible);
    bool OverWindow(const TLBSWidget* Inside, const TickContext& Context);
    TEWGraphicButtonWidget* AddHitArea(const ModHost* Host, TLBSWidget* Parent, int16_t X, int16_t Y, int16_t Width, int16_t Height);
    bool Hovered(const TEWGraphicButtonWidget* Area);
    TEWControlWidget* AddRect(const ModHost* Host, TLBSWidget* Parent, int16_t X, int16_t Y, int16_t Width, int16_t Height, const Color& Tint);
    void SetColor(TEWControlWidget* Rect, const Color& Tint);
    bool OnTop(const TLBSWidget* Widget, int32_t X, int32_t Y);
    bool OnTop(const TLBSWidget* Widget, const TickContext& Context);

    bool Over(const TLBSWidget* Widget, int16_t Width, int16_t Height, const TickContext& Context);

    struct Column {
        const wchar_t* Name;
        int16_t Width;
        uint8_t Align = 1;
        int16_t Indent = 0;
    };

    class Table {
    public:
        static constexpr int MaxColumns = 12;
        void Build(const ModHost* Host, TLBSWidget* Parent, int16_t X, int16_t Y, int16_t Width, int Rows, int16_t RowStep = 17,
                   int ColumnCount = MaxColumns);
        void SetColumns(const std::vector<Column>& Columns);
        void Place(const int16_t X, const int16_t TableWidth) { Left = X; Width = TableWidth; }
        void Set(int Row, int Column, const std::wstring& Text, const Color& Tint = Plain);
        void ClearRows();
        int Rows() const { return static_cast<int>(Cells.size()); }
        int RowAt(const TickContext& Context) const;
        bool OverCell(int Row, int Column, const TickContext& Context) const;
        int16_t RowTop(int Row) const { return static_cast<int16_t>(Top + (Row + 1) * Step); }
        int16_t ColumnX(int Column) const { return Column >= 0 && Column < MaxColumns ? Lefts[Column] : Left; }
        int16_t Bottom() const { return RowTop(Rows()); }
        bool Contains(const TickContext& Context) const;
        void SetActive(bool Active);
        void Forget();

    private:
        int16_t Left = 0;
        int16_t Top = 0;
        int16_t Width = 0;
        int16_t Step = 17;
        int16_t Widths[MaxColumns]{};
        int16_t Lefts[MaxColumns]{};
        int16_t Indents[MaxColumns]{};
        TEWLabel* Header[MaxColumns]{};
        std::vector<std::vector<TEWLabel*>> Cells;
        std::vector<TEWGraphicButtonWidget*> Hits;
        bool Active = true;
    };

    class Pager {
    public:
        void Build(const ModHost* Host, TLBSWidget* Parent, int16_t CenterX, int16_t Y);
        void Update(int Page, int Pages);
        void Center(int16_t CenterX);
        int Clicked(const TickContext& Context) const;
        void Forget();

    private:
        TEWLabel* Prev = nullptr;
        TEWLabel* Text = nullptr;
        TEWLabel* Next = nullptr;
        TEWGraphicButtonWidget* PrevHit = nullptr;
        TEWGraphicButtonWidget* NextHit = nullptr;
    };

    class Clicks {
    public:
        explicit Clicks(int Key = 1) : Key(Key) {}
        bool Pressed();

    private:
        int Key;
        bool WasDown = false;
    };
}
