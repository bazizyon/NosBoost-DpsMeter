#define NOMINMAX
#include <windows.h>

#include "GameHint.h"
#include "GameCall.h"
#include "TEWLabels.h"
#include "WidgetKit.h"

#include <algorithm>
#include <cstring>

namespace GameHint {
    namespace {
        constexpr char HintClass[] = "TNTHintWidget";
        constexpr ptrdiff_t VmtClassName = -0x2C;
        // 0x74A39C
        constexpr uint8_t HintCreatePattern[] = {
            0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF4, 0x53, 0x56, 0x84, 0xD2, 0x74, 0x08, 0x83, 0xC4, 0xF0, 0xE8, 0, 0, 0, 0,
            0x88, 0x55, 0xFF, 0x8B, 0xD8, 0x33, 0xD2, 0x8B, 0xC3, 0xE8, 0, 0, 0, 0,
            0x33, 0xF6, 0x8B, 0xCB, 0xB2, 0x01, 0xA1, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0x89, 0x84, 0xB3, 0x9C, 0x00, 0x00, 0x00,
        };
        constexpr char HintCreateMask[] = "xxxxxxxxxxxxxxxx????xxxxxxxxxx????xxxxxxx????x????xxxxxxx";
        // 0x4A2090
        constexpr uint8_t SetLineCountPattern[] = {
            0x53, 0x56, 0x57, 0x55, 0x8B, 0xF2, 0x8B, 0xD8, 0x66, 0x3B, 0x73, 0x72, 0x74, 0,
            0x0F, 0xB7, 0xFE, 0x57, 0x8D, 0x83, 0x8C, 0x00, 0x00, 0x00, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x8B, 0x15,
        };
        constexpr char SetLineCountMask[] = "xxxxxxxxxxxxx?xxxxxxxxxxxxxxxxx";
        // 0x4A206C
        constexpr uint8_t SetLineTextPattern[] = {
            0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0xF2, 0x8B, 0xD8, 0x0F, 0xB7, 0xC6, 0x03, 0xC0,
            0x8B, 0x93, 0x8C, 0x00, 0x00, 0x00, 0x8D, 0x44, 0xC2, 0x08, 0x8B, 0xD7, 0xE8,
        };
        constexpr char SetLineTextMask[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxx";
        static_assert(sizeof(HintCreatePattern) == sizeof(HintCreateMask) - 1);
        static_assert(sizeof(SetLineCountPattern) == sizeof(SetLineCountMask) - 1);
        static_assert(sizeof(SetLineTextPattern) == sizeof(SetLineTextMask) - 1);
        constexpr size_t HintFollowsSource = 0x98;

        constexpr int16_t LineHeight = 16;
        constexpr int16_t Padding = 8;
        constexpr uint8_t Font = 1;

        uintptr_t SetLineCountFn = 0, SetLineTextFn = 0;
        TEWLabels* Hint = nullptr;
        TLBSWidget* HintRoot = nullptr;
        std::vector<std::wstring> ShownLines;
        uint32_t ShownColor = 0xFFFFFFFF;

        bool SafeCopy(void* Out, const uintptr_t Address, const size_t Size) {
            __try {
                std::memcpy(Out, reinterpret_cast<const void*>(Address), Size);
                return true;
            } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
                return false;
            }
        }

        bool IsHint(const TLBSWidget* Widget) {
            uintptr_t VTable = 0, NamePtr = 0;
            uint8_t Length = 0;
            if (!SafeCopy(&VTable, reinterpret_cast<uintptr_t>(Widget), 4)) return false;
            if (!SafeCopy(&NamePtr, VTable + VmtClassName, 4) || !SafeCopy(&Length, NamePtr, 1)) return false;
            char Text[sizeof(HintClass)];
            return Length == sizeof(HintClass) - 1 && SafeCopy(Text, NamePtr + 1, Length)
                && std::memcmp(Text, HintClass, Length) == 0;
        }

        uintptr_t HintClassRef(const TLBSWidget* Root) {
            const TLBSWidgetList* Children = Root->childrenList;
            for (uint32_t i = 0; i < Children->count; i++) {
                const TLBSWidget* Child = Children->list[i];
                uintptr_t ClassRef = 0;
                if (Child && IsHint(Child) && SafeCopy(&ClassRef, reinterpret_cast<uintptr_t>(Child), 4)) return ClassRef;
            }
            return 0;
        }

        void SetLine(const int Index, const std::wstring& Text, const int16_t Width, const uint32_t TextColor) {
            const BSTR Copy = SysAllocString(Text.c_str());
            GameCall::Call(SetLineTextFn, reinterpret_cast<uint32_t>(Hint), static_cast<uint32_t>(Index), reinterpret_cast<uint32_t>(Copy));
            SysFreeString(Copy);
            TextData& Line = Hint->textDatas[Index];
            Line.xOffset = static_cast<uint16_t>(Padding);
            Line.yOffset = static_cast<uint16_t>(Padding / 2 + Index * LineHeight);
            Line.pxPerLine = static_cast<uint16_t>(Width);
            Line.isVisible = true;
            TextStyle& Style = Hint->textStyles[Index];
            Style.textColor = TextColor;
            Style.textAlignment = 1;
        }

        void Fill(const std::vector<std::wstring>& Lines, const uint32_t FirstLineColor) {
            if (Lines == ShownLines && FirstLineColor == ShownColor) return;
            ShownLines = Lines;
            ShownColor = FirstLineColor;
            int Inner = 60;
            for (const std::wstring& Line : Lines) Inner = std::max(Inner, WidgetKit::MeasureText(Font, Line.c_str()));
            const auto Count = static_cast<uint32_t>(Lines.size());
            GameCall::Call(SetLineCountFn, reinterpret_cast<uint32_t>(Hint), Count, 0);
            if (Hint->lineCount != Count) return;
            for (size_t i = 0; i < Lines.size(); i++) {
                SetLine(static_cast<int>(i), Lines[i], static_cast<int16_t>(Inner + 2), i ? 0xFFFFFFFF : FirstLineColor);
            }
            Hint->maxTextWidthPx = static_cast<uint16_t>(Inner);
            Hint->rect.right = static_cast<int16_t>(Hint->rect.left + Inner + 2 * Padding);
            Hint->rect.bottom = static_cast<int16_t>(Hint->rect.top + Count * LineHeight + Padding);
        }
    }

    bool Create(const ModHost* Host, TLBSWidget* Root) {
        static const uintptr_t CreateFn = GameCall::Find(HintCreatePattern, HintCreateMask);
        if (!SetLineCountFn) SetLineCountFn = GameCall::Find(SetLineCountPattern, SetLineCountMask);
        if (!SetLineTextFn) SetLineTextFn = GameCall::Find(SetLineTextPattern, SetLineTextMask);
        if (Hint || !Host || !CreateFn || !SetLineCountFn || !SetLineTextFn || !Root || !Root->childrenList) return Hint != nullptr;
        const uintptr_t ClassRef = HintClassRef(Root);
        if (!ClassRef) return false;
        const uint32_t ChildrenBefore = Root->childrenList->count;
        Hint = reinterpret_cast<TEWLabels*>(GameCall::Call(CreateFn, static_cast<uint32_t>(ClassRef), 1, reinterpret_cast<uint32_t>(Root)));
        if (!Hint) return false;
        Hint->isVisible = false;
        Hint->isInteractable = false;
        *(reinterpret_cast<uint8_t*>(Hint) + HintFollowsSource) = 0;
        if (Root->childrenList->count == ChildrenBefore || Root->childrenList->index_of(Hint) < 0) {
            Hint->parent = Root;
            Root->childrenList->push_back(Hint);
        }
        HintRoot = Root;
        ShownLines.clear();
        return true;
    }

    void Destroy() {
        if (Hint) {
            Hint->isVisible = false;
            if (TLBSWidget* Parent = Hint->parent; Parent && Parent->childrenList && Parent->childrenList->index_of(Hint) >= 0) {
                Parent->childrenList->remove(Hint);
            }
        }
        Hint = nullptr;
        HintRoot = nullptr;
        ShownLines.clear();
    }

    void Show(const std::vector<std::wstring>& Lines, const int32_t MouseX, const int32_t MouseY, const uint32_t FirstLineColor) {
        if (!Hint || Lines.empty()) return;
        Fill(Lines, FirstLineColor);
        const int16_t Width = static_cast<int16_t>(Hint->rect.right - Hint->rect.left);
        const int16_t Height = static_cast<int16_t>(Hint->rect.bottom - Hint->rect.top);
        auto Left = static_cast<int16_t>(MouseX + 12);
        auto Top = static_cast<int16_t>(MouseY + 20);
        if (HintRoot && Left + Width > HintRoot->rect.right) Left = static_cast<int16_t>(HintRoot->rect.right - Width);
        if (HintRoot && Top + Height > HintRoot->rect.bottom) Top = static_cast<int16_t>(MouseY - Height - 4);
        if (Hint->rect.left != Left || Hint->rect.top != Top) {
            Hint->rect = {Left, Top, static_cast<int16_t>(Left + Width), static_cast<int16_t>(Top + Height)};
        }
        if (!Hint->isVisible) {
            Hint->isVisible = true;
            Hint->BubbleUp();
        }
    }

    void Hide() {
        if (Hint && Hint->isVisible) Hint->isVisible = false;
    }
}
