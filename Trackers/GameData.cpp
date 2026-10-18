#define NOMINMAX
#include <windows.h>

#include "GameData.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
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
        std::unordered_map<int, const uint8_t*> ItemRecords;
        bool CardsLoaded = false;

        bool LoadCards() {
            const uintptr_t List = FindItemList();
            if (!List) return false;
            const auto* Object = reinterpret_cast<const uint8_t*>(List);
            const auto* Entries = reinterpret_cast<const uint8_t*>(Dword(Object + ListItems));
            const int Count = static_cast<int>(Dword(Object + ListCount));
            if (Count <= 0 || Count > MaxItems || !Readable(Entries, Count * 4)) return false;
            std::unordered_map<int, SpCard> Loaded;
            std::unordered_map<int, const uint8_t*> Records;
            for (int i = 0; i < Count; i++) {
                const auto* Record = reinterpret_cast<const uint8_t*>(Dword(Entries + i * 4));
                if (!Readable(Record, RecordSlot + 2)) continue;
                Records[static_cast<int>(Dword(Record + RecordVnum))] = Record;
                if (Word(Record + RecordSlot) != SpSlot) continue;
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
            ItemRecords = std::move(Records);
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

    namespace {
        constexpr char Digits[] = {0x00, 0x20, 0x2D, 0x2E, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x0A, 0x00};
        constexpr int SealEffect = 301;

        std::string DecryptDat(const std::string& Data) {
            std::string Result;
            size_t i = 0;
            while (i < Data.size()) {
                const auto Control = static_cast<uint8_t>(Data[i++]);
                if (Control == 0xFF) {
                    Result += '\r';
                    continue;
                }
                int Count = Control & 0x7F;
                if (Control & 0x80) {
                    while (Count > 0 && i < Data.size()) {
                        const auto Byte = static_cast<uint8_t>(Data[i++]);
                        Result += Digits[Byte >> 4];
                        if (--Count == 0) break;
                        Result += Digits[Byte & 0x0F];
                        Count--;
                    }
                } else {
                    for (; Count > 0 && i < Data.size(); Count--) Result += static_cast<char>(Data[i++] ^ 0x33);
                }
            }
            return Result;
        }

        std::string DataFolder() {
            char Path[MAX_PATH];
            GetModuleFileNameA(nullptr, Path, MAX_PATH);
            const std::string Exe(Path);
            return Exe.substr(0, Exe.find_last_of("\\/") + 1) + "NostaleData\\";
        }

        std::string ReadNtEntry(const std::string& Archive, const uint32_t Id) {
            std::ifstream File(DataFolder() + Archive, std::ios::binary);
            uint32_t Count = 0;
            File.seekg(0x10);
            File.read(reinterpret_cast<char*>(&Count), 4);
            if (!File || Count > 100000) return {};
            File.seekg(0x15);
            std::vector<uint32_t> Index(static_cast<size_t>(Count) * 2);
            File.read(reinterpret_cast<char*>(Index.data()), static_cast<std::streamsize>(Index.size() * 4));
            if (!File) return {};
            for (uint32_t i = 0; i < Count; i++) {
                if (Index[i * 2] != Id) continue;
                File.seekg(Index[i * 2 + 1]);
                uint32_t Head[3]{};
                uint8_t Packed = 0;
                File.read(reinterpret_cast<char*>(Head), 12);
                File.read(reinterpret_cast<char*>(&Packed), 1);
                if (!File || Packed || Head[2] > 16 * 1024 * 1024) return {};
                std::string Data(Head[2], '\0');
                File.read(Data.data(), static_cast<std::streamsize>(Data.size()));
                return File ? Data : std::string{};
            }
            return {};
        }

        uint32_t Crc32(const uint8_t* Data, const size_t Size) {
            uint32_t Crc = 0xFFFFFFFFu;
            for (size_t i = 0; i < Size; i++) {
                Crc ^= Data[i];
                for (int Bit = 0; Bit < 8; Bit++) Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1u)));
            }
            return ~Crc;
        }

        void PushBig(std::vector<uint8_t>& Out, const uint32_t Value) {
            for (int Shift = 24; Shift >= 0; Shift -= 8) Out.push_back(static_cast<uint8_t>(Value >> Shift));
        }

        std::vector<uint8_t> EncodePng(const int Width, const int Height, const std::vector<uint8_t>& Rgba) {
            std::vector<uint8_t> Raw;
            Raw.reserve(static_cast<size_t>(Width * 4 + 1) * Height);
            for (int Y = 0; Y < Height; Y++) {
                Raw.push_back(0);
                Raw.insert(Raw.end(), Rgba.begin() + static_cast<std::ptrdiff_t>(Y) * Width * 4,
                           Rgba.begin() + static_cast<std::ptrdiff_t>(Y + 1) * Width * 4);
            }
            std::vector<uint8_t> Z{0x78, 0x01};
            uint32_t A = 1, B = 0;
            for (const uint8_t Byte : Raw) {
                A = (A + Byte) % 65521;
                B = (B + A) % 65521;
            }
            for (size_t At = 0; At < Raw.size(); At += 65535) {
                const auto Len = static_cast<uint16_t>(std::min<size_t>(65535, Raw.size() - At));
                Z.push_back(At + Len >= Raw.size() ? 1 : 0);
                Z.push_back(static_cast<uint8_t>(Len));
                Z.push_back(static_cast<uint8_t>(Len >> 8));
                Z.push_back(static_cast<uint8_t>(~Len));
                Z.push_back(static_cast<uint8_t>(~Len >> 8));
                Z.insert(Z.end(), Raw.begin() + static_cast<std::ptrdiff_t>(At), Raw.begin() + static_cast<std::ptrdiff_t>(At + Len));
            }
            PushBig(Z, (B << 16) | A);

            std::vector<uint8_t> Png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            auto Chunk = [&](const char* Type, const std::vector<uint8_t>& Body) {
                PushBig(Png, static_cast<uint32_t>(Body.size()));
                const size_t Start = Png.size();
                Png.insert(Png.end(), Type, Type + 4);
                Png.insert(Png.end(), Body.begin(), Body.end());
                PushBig(Png, Crc32(Png.data() + Start, Png.size() - Start));
            };
            std::vector<uint8_t> Header;
            PushBig(Header, static_cast<uint32_t>(Width));
            PushBig(Header, static_cast<uint32_t>(Height));
            Header.insert(Header.end(), {8, 6, 0, 0, 0});
            Chunk("IHDR", Header);
            Chunk("IDAT", Z);
            Chunk("IEND", {});
            return Png;
        }

        std::string ReadNosFile(const std::string& Archive, const std::string& Wanted) {
            std::ifstream File(DataFolder() + Archive, std::ios::binary);
            int32_t Files = 0;
            File.read(reinterpret_cast<char*>(&Files), 4);
            for (int i = 0; i < Files && File; i++) {
                int32_t Header[2]{};
                File.read(reinterpret_cast<char*>(Header), 8);
                std::string Name(static_cast<size_t>(std::max(0, Header[1])), '\0');
                File.read(Name.data(), static_cast<std::streamsize>(Name.size()));
                int32_t Info[2]{};
                File.read(reinterpret_cast<char*>(Info), 8);
                if (Name != Wanted) {
                    File.seekg(Info[1], std::ios::cur);
                    continue;
                }
                std::string Data(static_cast<size_t>(std::max(0, Info[1])), '\0');
                File.read(Data.data(), static_cast<std::streamsize>(Data.size()));
                return Info[0] ? DecryptDat(Data) : Data;
            }
            return {};
        }

        std::string ReadItemDat() {
            return ReadNosFile("NSgtdData.NOS", "Item.dat");
        }

        std::vector<std::string> Fields(const std::string& Line) {
            std::vector<std::string> Result;
            std::istringstream Stream(Line);
            std::string Token;
            while (std::getline(Stream, Token, '\t')) Result.push_back(Token);
            return Result;
        }

        std::map<std::pair<int, bool>, int16_t> LoadSeals() {
            std::map<std::pair<int, bool>, int16_t> Seals;
            std::istringstream Content(ReadItemDat());
            std::string Line;
            int16_t Icon = -1;
            while (std::getline(Content, Line, '\r')) {
                const auto Parts = Fields(Line);
                if (Parts.size() < 2) continue;
                if (Parts[1] == "VNUM") Icon = -1;
                else if (Parts[1] == "INDEX" && Parts.size() > 6) Icon = static_cast<int16_t>(std::atoi(Parts[6].c_str()));
                else if (Parts[1] == "DATA" && Parts.size() > 5 && std::atoi(Parts[2].c_str()) == SealEffect && Icon >= 0) {
                    Seals.try_emplace({std::atoi(Parts[4].c_str()), std::atoi(Parts[5].c_str()) != 0}, Icon);
                }
            }
            return Seals;
        }
    }

    std::wstring ItemName(const int Vnum) {
        if (!CardsReady()) return {};
        const auto Found = ItemRecords.find(Vnum);
        if (Found == ItemRecords.end() || !Readable(Found->second + RecordName, 4)) return {};
        return ReadCString(reinterpret_cast<const char*>(Dword(Found->second + RecordName)));
    }

    int16_t ItemIcon(const int Vnum) {
        if (!CardsReady()) return -1;
        const auto Found = ItemRecords.find(Vnum);
        if (Found == ItemRecords.end() || !Readable(Found->second + RecordIcon, 2)) return -1;
        return Word(Found->second + RecordIcon);
    }

    namespace {
        const std::map<std::pair<int, bool>, int16_t>& Seals() {
            static const std::map<std::pair<int, bool>, int16_t> Loaded = LoadSeals();
            return Loaded;
        }

        std::vector<std::string> SplitLines(const std::string& Text) {
            std::vector<std::string> Lines;
            std::string Line;
            for (const char C : Text) {
                if (C == '\r' || C == '\n') {
                    if (!Line.empty()) Lines.push_back(std::move(Line));
                    Line.clear();
                } else {
                    Line += C;
                }
            }
            if (!Line.empty()) Lines.push_back(std::move(Line));
            return Lines;
        }

        std::map<std::string, std::string> ReadTexts(const std::string& Language, const std::string& Kind) {
            std::map<std::string, std::string> Texts;
            for (const std::string& Line : SplitLines(ReadNosFile("NSlangData_" + Language + ".NOS", "_code_" + Language + "_" + Kind + ".txt"))) {
                const size_t Tab = Line.find('\t');
                if (Tab != std::string::npos) Texts[Line.substr(0, Tab)] = Line.substr(Tab + 1);
            }
            return Texts;
        }

        std::string ClientLanguage() {
            std::string NameKey;
            int Vnum = -1;
            for (const std::string& Line : SplitLines(ReadItemDat())) {
                const auto Parts = Fields(Line);
                if (Parts.size() > 2 && Parts[1] == "VNUM") Vnum = std::atoi(Parts[2].c_str());
                if (Parts.size() > 2 && Parts[1] == "NAME" && Vnum > 0 && !ItemName(Vnum).empty()) {
                    NameKey = Parts[2];
                    break;
                }
            }
            if (NameKey.empty()) return "";
            const std::wstring Loaded = ItemName(Vnum);
            for (const char* Language : {"UK", "DE", "FR", "PL", "IT", "ES", "CZ", "TR"}) {
                std::string Lower = Language;
                for (char& C : Lower) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
                const auto Texts = ReadTexts(Lower, "Item");
                const auto Found = Texts.find(NameKey);
                if (Found != Texts.end() && Widen(Found->second.data(), Found->second.size()) == Loaded) return Lower;
            }
            return "";
        }

        bool QuestsLoaded = false;
        std::vector<RaidQuest> Quests;

        bool LoadQuests() {
            const std::string Language = ClientLanguage();
            if (Language.empty()) return false;
            const auto Titles = ReadTexts(Language, "quest");
            std::vector<RaidQuest> Loaded;
            RaidQuest Quest;
            int Type = 0;
            std::string Title;
            for (const std::string& Line : SplitLines(ReadNosFile("NSgtdData.NOS", "quest.dat"))) {
                const auto Parts = Fields(Line);
                if (Parts.empty()) continue;
                const std::string& Key = Parts[0].empty() && Parts.size() > 1 ? Parts[1] : Parts[0];
                const size_t At = Parts[0].empty() ? 2 : 1;
                if (Key == "VNUM" && Parts.size() > At + 1) {
                    Quest = {};
                    Quest.Quest = std::atoi(Parts[At].c_str());
                    Type = std::atoi(Parts[At + 1].c_str());
                    Title.clear();
                } else if (Key == "TITLE" && Parts.size() > At) {
                    const auto Found = Titles.find(Parts[At]);
                    Title = Found == Titles.end() ? "" : Found->second;
                } else if (Key == "DATA" && Type == 25 && Parts.size() > At + 1) {
                    std::string Lower = Title;
                    for (char& C : Lower) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
                    if (Lower.find("daily") == std::string::npos) continue;
                    Quest.RaidId = std::atoi(Parts[At].c_str());
                    Quest.Count = std::max(1, std::atoi(Parts[At + 1].c_str()));
                    Quest.Name = Widen(Title.data(), Title.size());
                    Loaded.push_back(Quest);
                }
            }
            if (Loaded.empty()) return false;
            Quests = std::move(Loaded);
            return true;
        }
    }

    const std::vector<int>& HardcoreRaids() {
        static std::vector<int> Raids;
        if (Raids.empty()) {
            for (const auto& [Key, Icon] : Seals()) {
                if (Key.second) Raids.push_back(Key.first);
            }
        }
        return Raids;
    }

    const std::vector<RaidQuest>& DailyRaidQuests() {
        static std::chrono::steady_clock::time_point LastAttempt;
        Ready(QuestsLoaded, LastAttempt, &LoadQuests);
        return Quests;
    }

    int16_t RaidIcon(const int RaidId, const bool Hardcore) {
        const auto& Seals = GameData::Seals();
        auto Found = Seals.find({RaidId, Hardcore});
        if (Found == Seals.end()) Found = Seals.find({RaidId, false});
        return Found == Seals.end() ? static_cast<int16_t>(-1) : Found->second;
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

    std::vector<uint8_t> MinimapPng(const int MapId) {
        if (MapId < 0) return {};
        const uint32_t Id = MinimapImageBase + static_cast<uint32_t>(MapId);
        char Archive[32];
        std::snprintf(Archive, sizeof(Archive), "NStpData%02X.NOS", Id % 32);
        const std::string Data = ReadNtEntry(Archive, Id);
        if (Data.size() < 8) return {};
        const auto* Bytes = reinterpret_cast<const uint8_t*>(Data.data());
        const int Width = Bytes[0] | Bytes[1] << 8;
        const int Height = Bytes[2] | Bytes[3] << 8;
        const uint8_t Format = Bytes[4];
        const size_t Pixels = static_cast<size_t>(Width) * Height;
        const size_t Need = Pixels * (Format == 2 ? 4 : 2);
        if (Width <= 0 || Height <= 0 || Width > 1024 || Height > 1024 || Format > 2 || Data.size() < 8 + Need) return {};
        std::vector<uint8_t> Rgba(Pixels * 4);
        for (size_t i = 0; i < Pixels; i++) {
            uint8_t* Out = &Rgba[i * 4];
            Out[3] = 255;
            if (Format == 2) {
                const uint8_t* In = Bytes + 8 + i * 4;
                Out[0] = In[2];
                Out[1] = In[1];
                Out[2] = In[0];
                continue;
            }
            const auto V = static_cast<uint16_t>(Bytes[8 + i * 2] | Bytes[9 + i * 2] << 8);
            if (Format == 0) {
                Out[0] = static_cast<uint8_t>((V >> 8 & 15) * 17);
                Out[1] = static_cast<uint8_t>((V >> 4 & 15) * 17);
                Out[2] = static_cast<uint8_t>((V & 15) * 17);
            } else {
                Out[0] = static_cast<uint8_t>((V >> 10 & 31) * 255 / 31);
                Out[1] = static_cast<uint8_t>((V >> 5 & 31) * 255 / 31);
                Out[2] = static_cast<uint8_t>((V & 31) * 255 / 31);
            }
        }
        return EncodePng(Width, Height, Rgba);
    }
}
