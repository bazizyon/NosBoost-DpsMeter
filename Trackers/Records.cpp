#define NOMINMAX
#include <windows.h>

#include "Records.h"
#include "WidgetKit.h"
#include "MapCatalog.h"
#include "SkillCatalog.h"
#include "GameData.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <ctime>
#include <cwctype>
#include <fstream>

namespace Records {
    int64_t NowMs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    std::filesystem::path DataDir() {
        wchar_t Exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, Exe, MAX_PATH);
        const auto Dir = std::filesystem::path(Exe).parent_path() / L"mods" / L"Trackers";
        std::error_code Error;
        std::filesystem::create_directories(Dir, Error);
        return Dir;
    }

    std::vector<std::string> Split(const std::string& Text, const char By) {
        std::vector<std::string> Parts;
        size_t From = 0;
        while (true) {
            const size_t At = Text.find(By, From);
            Parts.push_back(Text.substr(From, At == std::string::npos ? std::string::npos : At - From));
            if (At == std::string::npos) break;
            From = At + 1;
        }
        return Parts;
    }

    int64_t Num(const std::string& Text) {
        char* End = nullptr;
        const long long Value = std::strtoll(Text.c_str(), &End, 10);
        return End == Text.c_str() ? 0 : Value;
    }

    void Escape(std::string& Out, const std::string& Text) {
        Out += '"';
        for (const unsigned char C : Text) {
            if (C == '"' || C == '\\') {
                Out += '\\';
                Out += static_cast<char>(C);
            } else if (C < 0x20) {
                char Buffer[8];
                std::snprintf(Buffer, sizeof(Buffer), "\\u%04x", C);
                Out += Buffer;
            } else {
                Out += static_cast<char>(C);
            }
        }
        Out += '"';
    }

    const Json* Json::operator[](const char* Key) const {
        for (const auto& [Name, Value] : Fields) {
            if (Name == Key) return &Value;
        }
        return nullptr;
    }

    int64_t Json::Int(const char* Key, const int64_t Default) const {
        const Json* V = (*this)[Key];
        return V && V->Type == Number ? static_cast<int64_t>(V->N) : Default;
    }

    double Json::Real(const char* Key, const double Default) const {
        const Json* V = (*this)[Key];
        return V && V->Type == Number ? V->N : Default;
    }

    std::string Json::Str(const char* Key) const {
        const Json* V = (*this)[Key];
        return V && V->Type == String ? V->S : std::string{};
    }

    int64_t Json::At(const size_t Index) const {
        return Index < Items.size() && Items[Index].Type == Number ? static_cast<int64_t>(Items[Index].N) : 0;
    }

    namespace {
        struct Reader {
            const char* P;
            const char* End;

            void Space() {
                while (P < End && (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n')) P++;
            }

            bool Parse(Json& Out) {
                Space();
                if (P >= End) return false;
                if (*P == '{') {
                    Out.Type = Json::Object;
                    P++;
                    Space();
                    if (P < End && *P == '}') { P++; return true; }
                    while (P < End) {
                        Json Key;
                        Space();
                        if (!Parse(Key) || Key.Type != Json::String) return false;
                        Space();
                        if (P >= End || *P != ':') return false;
                        P++;
                        Json Value;
                        if (!Parse(Value)) return false;
                        Out.Fields.emplace_back(std::move(Key.S), std::move(Value));
                        Space();
                        if (P < End && *P == ',') { P++; continue; }
                        if (P < End && *P == '}') { P++; return true; }
                        return false;
                    }
                    return false;
                }
                if (*P == '[') {
                    Out.Type = Json::Array;
                    P++;
                    Space();
                    if (P < End && *P == ']') { P++; return true; }
                    while (P < End) {
                        Json Value;
                        if (!Parse(Value)) return false;
                        Out.Items.push_back(std::move(Value));
                        Space();
                        if (P < End && *P == ',') { P++; continue; }
                        if (P < End && *P == ']') { P++; return true; }
                        return false;
                    }
                    return false;
                }
                if (*P == '"') {
                    Out.Type = Json::String;
                    P++;
                    while (P < End && *P != '"') {
                        if (*P == '\\' && P + 1 < End) {
                            P++;
                            if (*P == 'u' && P + 4 < End) {
                                Out.S += static_cast<char>(std::strtol(std::string(P + 1, 4).c_str(), nullptr, 16));
                                P += 5;
                                continue;
                            }
                        }
                        Out.S += *P++;
                    }
                    if (P >= End) return false;
                    P++;
                    return true;
                }
                if (End - P >= 4 && std::strncmp(P, "null", 4) == 0) {
                    P += 4;
                    return true;
                }
                char* Stop = nullptr;
                Out.N = std::strtod(P, &Stop);
                if (Stop == P) return false;
                Out.Type = Json::Number;
                P = Stop;
                return true;
            }
        };
    }

    bool ParseJson(const std::string& Text, Json& Out) {
        Reader In{Text.data(), Text.data() + Text.size()};
        return In.Parse(Out);
    }

    std::vector<std::string> ReadLines(const std::filesystem::path& File) {
        std::vector<std::string> Lines;
        std::ifstream In(File, std::ios::binary);
        std::string Line;
        while (std::getline(In, Line)) {
            if (!Line.empty()) Lines.push_back(std::move(Line));
        }
        return Lines;
    }

    void AppendLine(const std::filesystem::path& File, const std::string& Line) {
        std::ofstream Out(File, std::ios::binary | std::ios::app);
        Out << Line << "\n";
    }

    void WriteFile(const std::filesystem::path& File, const std::string& Text) {
        std::ofstream Out(File, std::ios::binary | std::ios::trunc);
        Out << Text;
    }

    std::wstring Wide(const std::string& Text) {
        if (Text.empty()) return {};
        const int Size = MultiByteToWideChar(CP_ACP, 0, Text.c_str(), static_cast<int>(Text.size()), nullptr, 0);
        std::wstring Result(Size, L'\0');
        MultiByteToWideChar(CP_ACP, 0, Text.c_str(), static_cast<int>(Text.size()), Result.data(), Size);
        return Result;
    }

    std::string Utf8(const std::wstring& Text) {
        if (Text.empty()) return {};
        const int Size = WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()), nullptr, 0, nullptr, nullptr);
        std::string Result(Size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()), Result.data(), Size, nullptr, nullptr);
        return Result;
    }

    std::wstring FromUtf8(const std::string& Text) {
        if (Text.empty()) return {};
        const int Size = MultiByteToWideChar(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()), nullptr, 0);
        std::wstring Result(Size, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, Text.c_str(), static_cast<int>(Text.size()), Result.data(), Size);
        return Result;
    }

    std::wstring Compact(const int64_t Value) {
        wchar_t Buffer[32];
        if (Value >= 1'000'000'000) std::swprintf(Buffer, 32, L"%.2fB", Value / 1e9);
        else if (Value >= 1'000'000) std::swprintf(Buffer, 32, L"%.2fM", Value / 1e6);
        else if (Value >= 10'000) std::swprintf(Buffer, 32, L"%.1fK", Value / 1e3);
        else std::swprintf(Buffer, 32, L"%lld", static_cast<long long>(Value));
        return Buffer;
    }

    std::wstring Duration(const int64_t Ms) {
        const int64_t Seconds = std::max<int64_t>(0, Ms / 1000);
        wchar_t Buffer[32];
        if (Seconds >= 3600) std::swprintf(Buffer, 32, L"%lld:%02lld:%02lld", Seconds / 3600, Seconds / 60 % 60, Seconds % 60);
        else std::swprintf(Buffer, 32, L"%lld:%02lld", Seconds / 60, Seconds % 60);
        return Buffer;
    }

    std::wstring When(const int64_t Ms) {
        const std::time_t Seconds = static_cast<std::time_t>(Ms / 1000);
        std::tm Local{};
        localtime_s(&Local, &Seconds);
        wchar_t Buffer[32];
        std::wcsftime(Buffer, 32, L"%d.%m %H:%M", &Local);
        return Buffer;
    }

    std::wstring WhenWithYear(const int64_t Ms) {
        const std::time_t Seconds = static_cast<std::time_t>(Ms / 1000);
        std::tm Local{};
        localtime_s(&Local, &Seconds);
        wchar_t Buffer[32];
        std::wcsftime(Buffer, 32, L"%d.%m.%Y %H:%M", &Local);
        return Buffer;
    }

    std::wstring Ago(const int64_t Ms) {
        const auto Day = [](const int64_t At) {
            const std::time_t Seconds = static_cast<std::time_t>(At / 1000);
            std::tm Local{};
            localtime_s(&Local, &Seconds);
            Local.tm_hour = 12;
            Local.tm_min = Local.tm_sec = 0;
            return static_cast<int64_t>(std::mktime(&Local) / 86400);
        };
        const int64_t Days = Day(NowMs()) - Day(Ms);
        if (Days <= 0) return L"Today " + WhenWithYear(Ms).substr(11);
        if (Days == 1) return L"Yesterday";
        if (Days < 7) return std::to_wstring(Days) + L" days ago";
        if (Days < 14) return L"A week ago";
        return WhenWithYear(Ms).substr(0, 10);
    }

    void CopyToClipboard(const std::wstring& Text) {
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        const size_t Bytes = (Text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL Memory = GlobalAlloc(GMEM_MOVEABLE, Bytes)) {
            if (void* Data = GlobalLock(Memory)) {
                std::memcpy(Data, Text.c_str(), Bytes);
                GlobalUnlock(Memory);
                if (!SetClipboardData(CF_UNICODETEXT, Memory)) GlobalFree(Memory);
            } else {
                GlobalFree(Memory);
            }
        }
        CloseClipboard();
    }

    std::wstring Percent(const int64_t Part, const int64_t Whole) {
        if (Whole <= 0) return L"-";
        return std::to_wstring(Part * 100 / Whole) + L"%";
    }

    std::wstring Fit(const std::wstring& Text, const int Width) {
        if (WidgetKit::MeasureText(1, Text.c_str()) <= Width) return Text;
        std::wstring Cut = Text;
        while (!Cut.empty() && WidgetKit::MeasureText(1, (Cut + L"..").c_str()) > Width) Cut.pop_back();
        return Cut + L"..";
    }

    std::wstring SpName(const int Morph) {
        if (Morph <= 0) return L"-";
        if (GameData::IsSp(Morph)) {
            if (std::wstring Name = GameData::SpName(Morph); !Name.empty()) return Name;
        }
        if (const GameData::SpCard* Card = GameData::FindSp(Morph)) return Card->Name;
        return GameData::IsSp(Morph) ? L"SP #" + std::to_wstring(Morph) : L"Mount";
    }

    namespace {
        bool Loaded(bool& Done, std::chrono::steady_clock::time_point& LastAttempt, bool (*Load)()) {
            const auto Now = std::chrono::steady_clock::now();
            if (!Done && (LastAttempt == std::chrono::steady_clock::time_point{} || Now - LastAttempt > std::chrono::seconds(5))) {
                LastAttempt = Now;
                Done = Load();
            }
            return Done;
        }
    }

    int SkillKey(const int SkillVnum, const long Effect) {
        return SkillVnum >= 0 || Effect <= 0 ? SkillVnum : static_cast<int>(-Effect);
    }

    std::wstring SkillLabel(const int SkillVnum) {
        if (SkillVnum == -1) return L"Extra damage";
        if (SkillVnum < -1) return L"Extra damage (effect " + std::to_wstring(-SkillVnum) + L")";
        static bool Done = false;
        static std::chrono::steady_clock::time_point LastAttempt;
        Loaded(Done, LastAttempt, &SkillCatalog::Load);
        const SkillCatalog::Skill* Skill = SkillCatalog::Find(SkillVnum);
        if (Skill && !Skill->Name.empty()) return Skill->Name;
        return L"Skill " + std::to_wstring(SkillVnum);
    }

    int16_t SkillIcon(const int SkillVnum) {
        SkillLabel(SkillVnum);
        const SkillCatalog::Skill* Skill = SkillCatalog::Find(SkillVnum);
        return Skill ? Skill->Icon : static_cast<int16_t>(-1);
    }

    std::wstring MapLabel(const int MapId) {
        static bool Done = false;
        static std::chrono::steady_clock::time_point LastAttempt;
        Loaded(Done, LastAttempt, &MapCatalog::Load);
        std::wstring Name = MapCatalog::Name(MapId);
        return Name.empty() ? L"Map " + std::to_wstring(MapId) : Name;
    }

    namespace {
        constexpr ptrdiff_t VmtClassName = -0x2C;

        bool Copy(void* Out, const uintptr_t Address, const size_t Size) {
            __try {
                std::memcpy(Out, reinterpret_cast<const void*>(Address), Size);
                return true;
            } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
                return false;
            }
        }

        bool IsClass(const void* Object, const char* Name) {
            uintptr_t Vmt = 0, NamePtr = 0;
            uint8_t Length = 0;
            if (!Copy(&Vmt, reinterpret_cast<uintptr_t>(Object), 4)) return false;
            if (!Copy(&NamePtr, Vmt + VmtClassName, 4) || !Copy(&Length, NamePtr, 1)) return false;
            char Text[256];
            return Length == std::strlen(Name) && Copy(Text, NamePtr + 1, Length) && std::memcmp(Text, Name, Length) == 0;
        }

        void FindMapLabel(const TLBSWidget* Widget, const int32_t Y, const int Depth, std::wstring& Best, int32_t& BestY) {
            if (!Widget || !Widget->isVisible || Depth > 6) return;
            const int32_t Top = Y + Widget->rect.top;
            if (IsClass(Widget, TEWLabel::ClassName)) {
                const wchar_t* Text = static_cast<const TEWLabel*>(Widget)->labelText;
                if (Text && Top < BestY && std::any_of(Text, Text + wcslen(Text), [](const wchar_t C) { return iswalpha(C) != 0; })) {
                    Best = Text;
                    BestY = Top;
                }
            }
            const TLBSWidgetList* List = Widget->childrenList;
            if (!List || !List->list) return;
            for (uint32_t i = 0; i < List->count; i++) FindMapLabel(List->list[i], Top, Depth + 1, Best, BestY);
        }
    }

    std::wstring MinimapMapName(const TLBSWidget* Root) {
        const TLBSWidgetList* List = Root ? Root->childrenList : nullptr;
        if (!List || !List->list) return {};
        for (uint32_t i = 0; i < List->count; i++) {
            const TLBSWidget* Child = List->list[i];
            if (!Child || !IsClass(Child, "TNTMiniMapWidget")) continue;
            std::wstring Name;
            int32_t Y = INT32_MAX;
            FindMapLabel(Child, 0, 0, Name, Y);
            return Name;
        }
        return {};
    }

    TEWGraphicButtonWidget* AddHitArea(const ModHost* Host, TLBSWidget* Parent, const int16_t X, const int16_t Y,
                                       const int16_t Width, const int16_t Height) {
        auto* Area = Widget::Create<TEWGraphicButtonWidget>(Host);
        if (!Area || !Parent) return Area;
        delete[] Area->imageData.atlasFrames;
        Area->imageData.imageName = 1593835585;
        Area->imageData.frameCount = 3;
        Area->imageData.atlasFrames = new AtlasFrame[3]{{0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}};
        Area->drawMode = 0;
        Area->color = Color(0, 255, 255, 255);
        Area->rect = {X, Y, static_cast<int16_t>(X + Width), static_cast<int16_t>(Y + Height)};
        Area->parent = Parent;
        Parent->childrenList->push_back(Area);
        return Area;
    }

    TEWControlWidget* AddRect(const ModHost* Host, TLBSWidget* Parent, const int16_t X, const int16_t Y, const int16_t Width,
                              const int16_t Height, const Color& Tint) {
        auto* Rect = Widget::Create<TEWControlWidget>(Host);
        if (!Rect) return nullptr;
        delete[] Rect->imageData.atlasFrames;
        Rect->imageData.imageName = 1593835568;
        Rect->imageData.imageWidth = 512;
        Rect->imageData.imageHeight = 512;
        Rect->imageData.frameCount = 1;
        Rect->imageData.atlasFrames = new AtlasFrame[1]{{510, 1, 1, 1}};
        Rect->color = Tint;
        Rect->isInteractable = false;
        Rect->rect = {X, Y, static_cast<int16_t>(X + Width), static_cast<int16_t>(Y + Height)};
        if (Parent) {
            Rect->parent = Parent;
            Parent->childrenList->push_back(Rect);
        }
        return Rect;
    }

    void SetColor(TEWControlWidget* Rect, const Color& Tint) {
        if (Rect && std::memcmp(&Rect->color, &Tint, sizeof(Color)) != 0) Rect->color = Tint;
    }

    bool Hovered(const TEWGraphicButtonWidget* Area) {
        if (!Area || !Area->hovered) return false;
        for (const TLBSWidget* At = Area; At; At = At->parent) {
            if (!At->isVisible) return false;
        }
        return true;
    }

    TEWLabel* AddLabel(const ModHost* Host, TLBSWidget* Parent, const int16_t X, const int16_t Y, const int16_t Width,
                       const uint8_t Align) {
        TEWLabel* Label = Widget::Create<TEWLabel>(Host);
        if (!Label) return nullptr;
        Label->rect = {X, Y, static_cast<int16_t>(X + Width), static_cast<int16_t>(Y + 30)};
        Label->textAlignment = Align;
        Label->pxPerLine = Width;
        Label->SetText(L"");
        Label->parent = Parent;
        Parent->childrenList->push_back(Label);
        return Label;
    }

    void SetText(TEWLabel* Label, const std::wstring& Text, const Color& Tint, const int Width) {
        if (!Label) return;
        Label->SetText(Fit(Text, Width).c_str());
        Label->textColor = Tint;
    }

    void Show(TLBSWidget* Widget, const bool Visible) {
        if (Widget && Widget->isVisible != Visible) Widget->isVisible = Visible;
    }

    bool OnTop(const TLBSWidget* Widget, const int32_t X, const int32_t Y) {
        if (!Widget) return false;
        for (const TLBSWidget* At = Widget; At->parent; At = At->parent) {
            if (!At->isVisible) return false;
            const TLBSWidget* Parent = At->parent;
            const TLBSWidgetList* Siblings = Parent->childrenList;
            if (!Siblings) continue;
            int32_t Left = 0, Top = 0;
            for (const TLBSWidget* P = Parent; P; P = P->parent) {
                Left += P->rect.left;
                Top += P->rect.top;
            }
            const int32_t Index = Siblings->index_of(const_cast<TLBSWidget*>(At));
            if (Index < 0) continue;
            for (uint32_t i = static_cast<uint32_t>(Index) + 1; i < Siblings->count; i++) {
                const TLBSWidget* Other = Siblings->list[i];
                if (!Other || !Other->isVisible || !Other->isInteractable) continue;
                if (X >= Left + Other->rect.left && X < Left + Other->rect.right && Y >= Top + Other->rect.top && Y < Top + Other->rect.bottom) {
                    return false;
                }
            }
        }
        return true;
    }

    bool OnTop(const TLBSWidget* Widget, const TickContext& Context) {
        return OnTop(Widget, Context.mouseX, Context.mouseY);
    }

    bool OverWindow(const TLBSWidget* Inside, const TickContext& Context) {
        const TLBSWidget* Window = Inside;
        while (Window && Window->parent && Window->parent->parent) Window = Window->parent;
        if (!Window) return false;
        return Over(Window, static_cast<int16_t>(Window->rect.right - Window->rect.left),
                    static_cast<int16_t>(Window->rect.bottom - Window->rect.top), Context);
    }

    bool Over(const TLBSWidget* Widget, const int16_t Width, const int16_t Height, const TickContext& Context) {
        if (!Widget) return false;
        int32_t X = 0, Y = 0;
        for (const TLBSWidget* At = Widget; At; At = At->parent) {
            X += At->rect.left;
            Y += At->rect.top;
        }
        return Context.mouseX >= X && Context.mouseX < X + Width && Context.mouseY >= Y && Context.mouseY < Y + Height;
    }

    void Table::Build(const ModHost* Host, TLBSWidget* Parent, const int16_t X, const int16_t Y, const int16_t TableWidth,
                      const int RowCount, const int16_t RowStep, const int ColumnCount) {
        Left = X;
        Top = Y;
        Width = TableWidth;
        Step = RowStep;
        for (auto& Label : Header) Label = AddLabel(Host, Parent, X, Y, 10);
        Cells.assign(RowCount, std::vector<TEWLabel*>(std::clamp(ColumnCount, 1, MaxColumns), nullptr));
        for (int Row = 0; Row < RowCount; Row++) {
            const auto RowY = static_cast<int16_t>(RowTop(Row) + 2 + (Step - 17) / 2);
            for (auto& Label : Cells[Row]) Label = AddLabel(Host, Parent, X, RowY, 10);
        }
        Hits.assign(RowCount, nullptr);
        for (int Row = 0; Row < RowCount; Row++) Hits[Row] = AddHitArea(Host, Parent, X, RowTop(Row), TableWidth, Step);
    }

    void Table::SetColumns(const std::vector<Column>& Columns) {
        int16_t X = Left;
        for (int C = 0; C < MaxColumns; C++) {
            const bool Used = C < static_cast<int>(Columns.size());
            int16_t W = 0;
            int16_t Indent = 0;
            uint8_t Align = 1;
            if (Used) {
                W = Columns[C].Width ? Columns[C].Width : static_cast<int16_t>(std::max(10, Left + Width - X));
                Align = Columns[C].Align;
                Indent = std::min<int16_t>(Columns[C].Indent, static_cast<int16_t>(W - 1));
            }
            Widths[C] = W;
            Lefts[C] = X;
            Indents[C] = Indent;
            auto Move = [&](TEWLabel* Label, const int16_t Shift) {
                if (!Label) return;
                const int16_t Y = Label->rect.top;
                const auto CellX = static_cast<int16_t>(X + Shift);
                const auto CellW = std::max<int16_t>(static_cast<int16_t>(W - Shift), 1);
                Label->rect = {CellX, Y, static_cast<int16_t>(CellX + CellW), static_cast<int16_t>(Y + 30)};
                Label->textAlignment = Align;
                Label->pxPerLine = CellW;
                Show(Label, Used);
            };
            Move(Header[C], 0);
            for (auto& Row : Cells) {
                if (C < static_cast<int>(Row.size())) Move(Row[C], Indent);
            }
            SetText(Header[C], Used ? Columns[C].Name : L"", Hint, std::max<int16_t>(W, 1));
            if (Used) X = static_cast<int16_t>(X + W + 4);
        }
        for (int Row = 0; Row < static_cast<int>(Hits.size()); Row++) {
            if (!Hits[Row]) continue;
            const int16_t Y = RowTop(Row);
            Hits[Row]->rect = {Left, Y, static_cast<int16_t>(Left + Width), static_cast<int16_t>(Y + Step)};
        }
        ClearRows();
    }

    void Table::Set(const int Row, const int Column, const std::wstring& Text, const Color& Tint) {
        if (Row < 0 || Row >= Rows() || Column < 0 || Column >= MaxColumns || !Widths[Column]) return;
        if (Column >= static_cast<int>(Cells[Row].size())) return;
        SetText(Cells[Row][Column], Text, Tint, std::max(1, Widths[Column] - Indents[Column]));
    }

    void Table::ClearRows() {
        for (auto& Row : Cells) {
            for (TEWLabel* Label : Row) {
                if (Label) Label->SetText(L"");
            }
        }
    }

    int Table::RowAt(const TickContext&) const {
        if (!Active) return -1;
        for (int Row = 0; Row < static_cast<int>(Hits.size()); Row++) {
            if (Hovered(Hits[Row])) return Row;
        }
        return -1;
    }

    bool Table::OverCell(const int Row, const int Column, const TickContext& Context) const {
        if (Row < 0 || Row >= Rows() || Column < 0 || Column >= MaxColumns || !Widths[Column]) return false;
        if (Row < 0 || Row >= Rows() || Column < 0 || Column >= static_cast<int>(Cells[Row].size())) return false;
        return RowAt(Context) == Row && Over(Cells[Row][Column], Widths[Column], Step, Context);
    }

    bool Table::Contains(const TickContext& Context) const {
        return RowAt(Context) >= 0;
    }

    void Table::SetActive(const bool Value) {
        if (Active == Value) return;
        Active = Value;
        for (TEWGraphicButtonWidget* Hit : Hits) {
            if (Hit) Hit->isVisible = Value;
        }
    }

    void Table::Forget() {
        for (auto& Label : Header) Label = nullptr;
        Cells.clear();
        Hits.clear();
        Active = true;
    }

    void Pager::Build(const ModHost* Host, TLBSWidget* Parent, const int16_t CenterX, const int16_t Y) {
        Prev = AddLabel(Host, Parent, static_cast<int16_t>(CenterX - 50), Y, 20);
        Text = AddLabel(Host, Parent, static_cast<int16_t>(CenterX - 30), Y, 60, 3);
        Next = AddLabel(Host, Parent, static_cast<int16_t>(CenterX + 34), Y, 20);
        PrevHit = AddHitArea(Host, Parent, static_cast<int16_t>(CenterX - 50), Y, 20, 16);
        NextHit = AddHitArea(Host, Parent, static_cast<int16_t>(CenterX + 34), Y, 20, 16);
    }

    void Pager::Center(const int16_t CenterX) {
        const int16_t Offsets[] = {-50, -30, 34};
        TEWLabel* Parts[] = {Prev, Text, Next};
        for (int i = 0; i < 3; i++) {
            if (!Parts[i]) continue;
            const int16_t W = static_cast<int16_t>(Parts[i]->rect.right - Parts[i]->rect.left);
            Parts[i]->rect.left = static_cast<int16_t>(CenterX + Offsets[i]);
            Parts[i]->rect.right = static_cast<int16_t>(Parts[i]->rect.left + W);
        }
        if (PrevHit) PrevHit->MoveTo(static_cast<int16_t>(CenterX + Offsets[0]), PrevHit->rect.top);
        if (NextHit) NextHit->MoveTo(static_cast<int16_t>(CenterX + Offsets[2]), NextHit->rect.top);
    }

    void Pager::Update(const int Page, const int Pages) {
        SetText(Text, Pages > 1 ? std::to_wstring(Page + 1) + L" / " + std::to_wstring(Pages) : L"", Hint, 60);
        SetText(Prev, Page > 0 ? L"<" : L"", Plain, 20);
        SetText(Next, Page + 1 < Pages ? L">" : L"", Plain, 20);
        if (PrevHit && PrevHit->isVisible != (Page > 0)) PrevHit->isVisible = Page > 0;
        if (NextHit && NextHit->isVisible != (Page + 1 < Pages)) NextHit->isVisible = Page + 1 < Pages;
    }

    int Pager::Clicked(const TickContext&) const {
        if (Hovered(PrevHit)) return -1;
        if (Hovered(NextHit)) return 1;
        return 0;
    }

    void Pager::Forget() {
        Prev = Text = Next = nullptr;
        PrevHit = NextHit = nullptr;
    }

    bool Clicks::Pressed() {
        const bool Down = (GetAsyncKeyState(Key) & 0x8000) != 0;
        const bool Pressed = Down && !WasDown;
        WasDown = Down;
        return Pressed;
    }
}
