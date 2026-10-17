#define NOMINMAX
#include <windows.h>

#include "GameData.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

namespace GameData {
    namespace {
        constexpr char ListClassName[] = "TNTCItemDataList";
        constexpr uint32_t VmtSelfPtr = 0x4C;
        constexpr uint32_t VmtClassName = 0x2C;
        constexpr uint32_t ListItems = 0x04;
        constexpr uint32_t ListCount = 0x08;
        constexpr uint32_t RecordVnum = 0x30;
        constexpr uint32_t RecordName = 0x38;
        constexpr uint32_t RecordIcon = 0x4A;
        constexpr uint32_t RecordMorph = 0x4C;
        constexpr uint32_t RecordSlot = 0x5E;
        constexpr int16_t SpSlot = 4;
        constexpr int MaxItems = 100000;
        constexpr int MaxRaidId = 200;

        struct Image {
            const uint8_t* Start = nullptr;
            size_t Size = 0;
        };

        Image GameImage() {
            const auto Base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
            const auto* Dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
            const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(Base + Dos->e_lfanew);
            return {Base, Nt->OptionalHeader.SizeOfImage};
        }

        size_t ReadableSpan(const void* Address) {
            MEMORY_BASIC_INFORMATION Info{};
            if (!Address || !VirtualQuery(Address, &Info, sizeof(Info)) || Info.State != MEM_COMMIT) return 0;
            if (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
            const auto End = static_cast<const uint8_t*>(Info.BaseAddress) + Info.RegionSize;
            return static_cast<size_t>(End - static_cast<const uint8_t*>(Address));
        }

        bool Readable(const void* Address, const size_t Size) {
            return ReadableSpan(Address) >= Size;
        }

        uint32_t Dword(const uint8_t* At) {
            uint32_t Value;
            std::memcpy(&Value, At, sizeof(Value));
            return Value;
        }

        int16_t Word(const uint8_t* At) {
            int16_t Value;
            std::memcpy(&Value, At, sizeof(Value));
            return Value;
        }

        const uint8_t* FindPattern(const Image& Area, const int16_t* Pattern, const size_t Length, const uint8_t* From) {
            const uint8_t* End = Area.Start + Area.Size;
            const uint8_t* At = From;
            while (At < End) {
                MEMORY_BASIC_INFORMATION Info{};
                if (!VirtualQuery(At, &Info, sizeof(Info))) return nullptr;
                const uint8_t* RegionEnd = std::min(End, static_cast<const uint8_t*>(Info.BaseAddress) + Info.RegionSize);
                const bool Usable = Info.State == MEM_COMMIT && !(Info.Protect & (PAGE_NOACCESS | PAGE_GUARD));
                if (Usable) {
                    for (; At + Length <= RegionEnd; At++) {
                        size_t i = 0;
                        while (i < Length && (Pattern[i] < 0 || At[i] == Pattern[i])) i++;
                        if (i == Length) return At;
                    }
                }
                At = RegionEnd;
            }
            return nullptr;
        }

        const uint8_t* FindBytes(const Image& Area, const uint8_t* Bytes, const size_t Length, const uint8_t* From) {
            std::vector<int16_t> Pattern(Bytes, Bytes + Length);
            return FindPattern(Area, Pattern.data(), Length, From);
        }

        uintptr_t FindListVmt(const Image& Area) {
            uint8_t Name[sizeof(ListClassName)];
            Name[0] = static_cast<uint8_t>(sizeof(ListClassName) - 1);
            std::memcpy(Name + 1, ListClassName, sizeof(ListClassName) - 1);
            for (const uint8_t* Text = FindBytes(Area, Name, sizeof(Name), Area.Start); Text;
                 Text = FindBytes(Area, Name, sizeof(Name), Text + 1)) {
                const auto TextAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(Text));
                const auto* Needle = reinterpret_cast<const uint8_t*>(&TextAddress);
                for (const uint8_t* Ref = FindBytes(Area, Needle, 4, Area.Start); Ref;
                     Ref = FindBytes(Area, Needle, 4, Ref + 1)) {
                    const uintptr_t Vmt = reinterpret_cast<uintptr_t>(Ref) + VmtClassName;
                    const auto* Self = reinterpret_cast<const uint8_t*>(Vmt - VmtSelfPtr);
                    if (Readable(Self, 4) && Dword(Self) == Vmt) return Vmt;
                }
            }
            return 0;
        }

        uintptr_t FindListGlobal(const Image& Area, const uintptr_t Vmt) {
            const auto SelfPtr = static_cast<uint32_t>(Vmt - VmtSelfPtr);
            uint8_t Load[5] = {0xA1};
            std::memcpy(Load + 1, &SelfPtr, 4);
            for (const uint8_t* At = FindBytes(Area, Load, sizeof(Load), Area.Start); At;
                 At = FindBytes(Area, Load, sizeof(Load), At + 1)) {
                if (!Readable(At, 15) || At[5] != 0xE8 || At[10] != 0xA3) continue;
                return Dword(At + 11);
            }
            return 0;
        }

        uintptr_t FindItemList() {
            static uintptr_t Global = 0;
            static uintptr_t Vmt = 0;
            if (!Global) {
                const Image Area = GameImage();
                Vmt = FindListVmt(Area);
                if (Vmt) Global = FindListGlobal(Area, Vmt);
                if (!Global) return 0;
            }
            const auto* Slot = reinterpret_cast<const uint8_t*>(Global);
            if (!Readable(Slot, 4)) return 0;
            const uintptr_t List = Dword(Slot);
            const auto* Object = reinterpret_cast<const uint8_t*>(List);
            if (!List || !Readable(Object, 0x10) || Dword(Object) != Vmt) return 0;
            return List;
        }

        std::wstring Widen(const char* Text, const size_t Length) {
            if (!Length) return {};
            const int Size = MultiByteToWideChar(CP_ACP, 0, Text, static_cast<int>(Length), nullptr, 0);
            std::wstring Result(Size, L'\0');
            MultiByteToWideChar(CP_ACP, 0, Text, static_cast<int>(Length), Result.data(), Size);
            return Result;
        }

        std::wstring ReadCString(const char* Text) {
            const size_t Limit = std::min<size_t>(256, ReadableSpan(Text));
            size_t Length = 0;
            while (Length < Limit && Text[Length]) Length++;
            return Widen(Text, Length);
        }


        std::unordered_map<int, SpCard> Cards;
        bool CardsLoaded = false;

        bool LoadCards() {
            const uintptr_t List = FindItemList();
            if (!List) return false;
            const auto* Object = reinterpret_cast<const uint8_t*>(List);
            const auto* Entries = reinterpret_cast<const uint8_t*>(Dword(Object + ListItems));
            const int Count = static_cast<int>(Dword(Object + ListCount));
            if (Count <= 0 || Count > MaxItems || !Readable(Entries, Count * 4)) return false;
            std::unordered_map<int, SpCard> Loaded;
            for (int i = 0; i < Count; i++) {
                const auto* Record = reinterpret_cast<const uint8_t*>(Dword(Entries + i * 4));
                if (!Readable(Record, RecordSlot + 2) || Word(Record + RecordSlot) != SpSlot) continue;
                const int Morph = Word(Record + RecordMorph);
                if (Morph <= 0) continue;
                SpCard Card{static_cast<int>(Dword(Record + RecordVnum)), Word(Record + RecordIcon),
                            ReadCString(reinterpret_cast<const char*>(Dword(Record + RecordName)))};
                const auto Found = Loaded.find(Morph);
                if (Found == Loaded.end() || (Found->second.Name.find(L'(') != std::wstring::npos
                                              && Card.Name.find(L'(') == std::wstring::npos)) {
                    Loaded[Morph] = std::move(Card);
                }
            }
            if (Loaded.empty()) return false;
            Cards = std::move(Loaded);
            return true;
        }

        template <typename LoadFn>
        bool Ready(bool& Done, std::chrono::steady_clock::time_point& LastAttempt, LoadFn&& Load) {
            const auto Now = std::chrono::steady_clock::now();
            if (!Done && (LastAttempt == std::chrono::steady_clock::time_point{} || Now - LastAttempt > std::chrono::seconds(5))) {
                LastAttempt = Now;
                Done = Load();
            }
            return Done;
        }

        bool CardsReady() {
            static std::chrono::steady_clock::time_point LastAttempt;
            return Ready(CardsLoaded, LastAttempt, &LoadCards);
        }


        uintptr_t RaidTable = 0;
        bool TableFound = false;

        bool FindRaidTable() {
            constexpr int16_t Pattern[] = {
                0xC7, 0x45, 0xF0, -1, -1, -1, -1, 0x8B, 0x16, 0x81, 0xC2, 0xEB, 0x08, 0x00, 0x00,
                0x8B, 0xC3, 0xE8, -1, -1, -1, -1, 0x8B, 0xD0, 0x8B, 0x45, 0xF0, 0xE8,
            };
            const Image Area = GameImage();
            const uint8_t* At = FindPattern(Area, Pattern, std::size(Pattern), Area.Start);
            if (!At) return false;
            RaidTable = Dword(At + 3);
            return Readable(reinterpret_cast<const void*>(RaidTable), 4);
        }

        std::wstring ReadAnsiString(const uint8_t* Text) {
            if (!Text || !Readable(Text - 8, 8)) return {};
            const uint32_t Length = Dword(Text - 4);
            if (!Length || Length > 256 || !Readable(Text, Length)) return {};
            return Widen(reinterpret_cast<const char*>(Text), Length);
        }
    }

    namespace {
        constexpr int MaxSpMorph = 64;
        uintptr_t SpTable = 0;
        bool SpTableFound = false;

        bool FindSpTable() {
            constexpr int16_t Pattern[] = {
                0xC7, 0x06, 0x01, 0x00, 0x00, 0x00, 0xC7, 0x45, 0xF4, -1, -1, -1, -1,
                0x8B, 0x16, 0x4A, 0x81, 0xC2, 0xF1, 0x07, 0x00, 0x00, 0x8B, 0xC3, 0xE8,
            };
            const Image Area = GameImage();
            const uint8_t* At = FindPattern(Area, Pattern, std::size(Pattern), Area.Start);
            if (!At) return false;
            SpTable = Dword(At + 9);
            return Readable(reinterpret_cast<const void*>(SpTable), 4);
        }
    }

    namespace {
        uintptr_t SceneGlobal = 0;
        bool SceneFound = false;

        bool FindScene() {
            constexpr int16_t Pattern[] = {0xA1, -1, -1, -1, -1, -1, -1, -1, -1, 0x58, 0x00, 0x0F, 0x84, 0xBF};
            const Image Area = GameImage();
            const uint8_t* At = FindPattern(Area, Pattern, std::size(Pattern), Area.Start);
            if (!At) return false;
            SceneGlobal = Dword(At + 1);
            return Readable(reinterpret_cast<const void*>(SceneGlobal), 4);
        }

        uintptr_t Follow(const uintptr_t Address) {
            const auto* At = reinterpret_cast<const uint8_t*>(Address);
            return Address && Readable(At, 4) ? Dword(At) : 0;
        }
    }

    long PlayerId() {
        static std::chrono::steady_clock::time_point LastAttempt;
        if (!Ready(SceneFound, LastAttempt, &FindScene)) return 0;
        const uintptr_t Scene = Follow(Follow(SceneGlobal));
        const uintptr_t Player = Scene ? Follow(Scene + 0x30) : 0;
        return Player ? static_cast<long>(Follow(Player + 0x08)) : 0;
    }

    std::wstring SpName(const int Morph) {
        static std::chrono::steady_clock::time_point LastAttempt;
        if (Morph < 1 || Morph > MaxSpMorph || !Ready(SpTableFound, LastAttempt, &FindSpTable)) return {};
        const auto* Slot = reinterpret_cast<const uint8_t*>(SpTable + (Morph - 1) * 4);
        if (!Readable(Slot, 4)) return {};
        return ReadAnsiString(reinterpret_cast<const uint8_t*>(Dword(Slot)));
    }

    std::wstring RaidName(const int RaidId) {
        static std::chrono::steady_clock::time_point LastAttempt;
        if (RaidId < 0 || RaidId > MaxRaidId || !Ready(TableFound, LastAttempt, &FindRaidTable)) return {};
        const auto* Slot = reinterpret_cast<const uint8_t*>(RaidTable + RaidId * 4);
        if (!Readable(Slot, 4)) return {};
        return ReadAnsiString(reinterpret_cast<const uint8_t*>(Dword(Slot)));
    }

    const SpCard* FindSp(const int Morph) {
        if (!CardsReady()) return nullptr;
        const auto Found = Cards.find(Morph);
        return Found == Cards.end() ? nullptr : &Found->second;
    }

    bool IsSp(const int Morph) {
        if (Morph <= 0) return false;
        if (CardsReady()) return Cards.contains(Morph);
        return Morph <= 58 && (Morph < 42 || Morph > 44);
    }
}
