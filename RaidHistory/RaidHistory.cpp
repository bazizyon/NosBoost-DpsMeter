#define NOMINMAX
#include <windows.h>
#include <compressapi.h>

#include "RaidHistory.h"
#include "Records.h"
#include "GameData.h"
#include "GameHint.h"
#include "WidgetKit.h"
#include "TLBSWidget.h"
#include "TEWLabel.h"
#include "TEWGuageWidget.h"
#include "TNTIconWidget.h"
#include "TEWButtonWidget.h"
#include "SuPacket.h"
#include "CInfoPacket.h"
#include "InPacket.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <deque>
#include <fstream>
#include <unordered_map>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace RaidHistory {
    namespace {
        using namespace Records;


        struct SkillStat {
            int64_t Damage = 0;
            int Hits = 0;
            int Crits = 0;
            int Misses = 0;
        };

        struct Member {
            long Id = 0;
            std::string Name;
            int Level = 0;
            int HeroLevel = 0;
            int Class = -1;
            int Gender = -1;
            int Morph = -1;
            std::string Listing;
            std::string In;
            int64_t Damage = 0;
            int64_t BossDamage = 0;
            int Hits = 0;
            int Crits = 0;
            int Misses = 0;
            int Deaths = 0;
            int64_t LeftAt = 0;
            int64_t Gold = 0;
            std::map<int, int64_t> Picked;
            std::vector<std::pair<int64_t, int>> MorphChanges;
            std::map<int, SkillStat> Skills;
        };

        enum Result { Running = 0, Won = 1, Failed = 2 };

        using Locks = std::array<int64_t, 5>;

        struct Raid {
            int64_t Start = 0;
            int64_t End = 0;
            int64_t WonAt = 0;
            int64_t FailedAt = 0;
            int Type = 0;
            int RaidId = -1;
            bool Hardcore = false;
            int Result = Running;
            long Self = 0;
            int BossVnum = 0;
            long BossId = 0;
            int64_t BossMaxHp = 0;
            int64_t BossSeenAt = 0;
            int64_t BossKilledAt = 0;
            int64_t BossFirstHitAt = 0;
            int LivesLeft = -1;
            int MaxLives = -1;
            std::vector<int> Maps;
            std::vector<Locks> LockTimes;
            std::vector<Member> Members;
            std::vector<long> Party;
            std::string Log;
            int Scan = 0;
            int TriesLeft = -1;
            int64_t RewardAt = 0;
            int64_t RewardDamage = 0;
            int64_t RewardLow = 0;
            int64_t RewardMateDamage = 0;
            int RewardPlayers = 0;
            int64_t MateBossDamage = 0;
            int64_t RewardBossLost = 0;
            int64_t BossTaken = 0;
            std::vector<std::pair<int64_t, std::string>> Packets;
        };

        struct Seen {
            std::string Name;
            int Class = -1;
            int Gender = -1;
            int Sp = -1;
            std::string In;
        };
        std::unordered_map<long, Seen> Known;

        struct Logged {
            int64_t At;
            bool Sent;
            std::string Text;
        };
        std::deque<Logged> Recent;
        constexpr int64_t RecentMs = 3 * 60 * 1000;
        std::ofstream LogOut;

        bool IsSp(const int Morph) {
            return GameData::IsSp(Morph);
        }

        const ModHost* Host = nullptr;
        bool HasActive = false;
        Raid Active;
        std::vector<Raid> History;
        std::string LastListing;
        long SelfId = 0;
        int SelfMorph = -1;
        int Revision = 0;

        std::string ToJson(const Raid& R) {
            std::string J = "{\"start\":" + std::to_string(R.Start) + ",\"end\":" + std::to_string(R.End)
                + ",\"wonAt\":" + std::to_string(R.WonAt) + ",\"failedAt\":" + std::to_string(R.FailedAt) + ",\"type\":" + std::to_string(R.Type)
                + ",\"raidId\":" + std::to_string(R.RaidId) + ",\"hardcore\":" + (R.Hardcore ? "1" : "0")
                + ",\"result\":" + std::to_string(R.Result) + ",\"self\":" + std::to_string(R.Self)
                + ",\"bossVnum\":" + std::to_string(R.BossVnum) + ",\"bossId\":" + std::to_string(R.BossId)
                + ",\"bossMaxHp\":" + std::to_string(R.BossMaxHp) + ",\"bossSeenAt\":" + std::to_string(R.BossSeenAt)
                + ",\"bossKilledAt\":" + std::to_string(R.BossKilledAt)
                + ",\"livesLeft\":" + std::to_string(R.LivesLeft) + ",\"maxLives\":" + std::to_string(R.MaxLives)
                + ",\"bossFirstHitAt\":" + std::to_string(R.BossFirstHitAt) + ",\"loot\":" + std::to_string(R.Scan) + ",\"triesLeft\":" + std::to_string(R.TriesLeft) + ",\"rewardAt\":" + std::to_string(R.RewardAt)
                + ",\"rewardDamage\":" + std::to_string(R.RewardDamage) + ",\"rewardLow\":" + std::to_string(R.RewardLow)
                + ",\"rewardMateDamage\":" + std::to_string(R.RewardMateDamage) + ",\"rewardPlayers\":" + std::to_string(R.RewardPlayers)
                + ",\"mateBossDamage\":" + std::to_string(R.MateBossDamage) + ",\"rewardBossLost\":" + std::to_string(R.RewardBossLost)
                + ",\"log\":";
            Escape(J, R.Log);
            J += ",\"maps\":[";
            for (size_t i = 0; i < R.Maps.size(); i++) J += (i ? "," : "") + std::to_string(R.Maps[i]);
            J += "],\"party\":[";
            for (size_t i = 0; i < R.Party.size(); i++) J += (i ? "," : "") + std::to_string(R.Party[i]);
            J += "],\"locks\":[";
            for (size_t i = 0; i < R.LockTimes.size(); i++) {
                J += i ? ",[" : "[";
                for (size_t k = 0; k < R.LockTimes[i].size(); k++) J += (k ? "," : "") + std::to_string(R.LockTimes[i][k]);
                J += "]";
            }
            J += "],\"members\":[";
            for (size_t i = 0; i < R.Members.size(); i++) {
                const Member& M = R.Members[i];
                if (i) J += ',';
                J += "{\"id\":" + std::to_string(M.Id) + ",\"name\":";
                Escape(J, M.Name);
                J += ",\"level\":" + std::to_string(M.Level) + ",\"heroLevel\":" + std::to_string(M.HeroLevel)
                    + ",\"class\":" + std::to_string(M.Class) + ",\"gender\":" + std::to_string(M.Gender)
                    + ",\"morph\":" + std::to_string(M.Morph) + ",\"listing\":";
                Escape(J, M.Listing);
                J += ",\"in\":";
                Escape(J, M.In);
                J += ",\"damage\":" + std::to_string(M.Damage) + ",\"bossDamage\":" + std::to_string(M.BossDamage)
                    + ",\"hits\":" + std::to_string(M.Hits) + ",\"crits\":" + std::to_string(M.Crits)
                    + ",\"misses\":" + std::to_string(M.Misses) + ",\"deaths\":" + std::to_string(M.Deaths)
                    + ",\"leftAt\":" + std::to_string(M.LeftAt) + ",\"gold\":" + std::to_string(M.Gold) + ",\"picked\":[";
                bool FirstPick = true;
                for (const auto& [Vnum, Count] : M.Picked) {
                    J += (FirstPick ? "[" : ",[") + std::to_string(Vnum) + "," + std::to_string(Count) + "]";
                    FirstPick = false;
                }
                J += "],\"morphChanges\":[";
                for (size_t k = 0; k < M.MorphChanges.size(); k++) {
                    J += (k ? ",[" : "[") + std::to_string(M.MorphChanges[k].first) + "," + std::to_string(M.MorphChanges[k].second) + "]";
                }
                J += "],\"skills\":[";
                bool First = true;
                for (const auto& [Skill, S] : M.Skills) {
                    J += (First ? "[" : ",[") + std::to_string(Skill) + "," + std::to_string(S.Damage) + "," + std::to_string(S.Hits)
                        + "," + std::to_string(S.Crits) + "," + std::to_string(S.Misses) + "]";
                    First = false;
                }
                J += "]}";
            }
            J += "],\"packets\":[";
            for (size_t i = 0; i < R.Packets.size(); i++) {
                J += (i ? ",[" : "[") + std::to_string(R.Packets[i].first) + ",";
                Escape(J, R.Packets[i].second);
                J += "]";
            }
            J += "]}";
            return J;
        }

        bool FromJson(const std::string& Line, Raid& R) {
            Json J;
            if (!ParseJson(Line, J) || J.Type != Json::Object) return false;
            R = {};
            R.Start = J.Int("start");
            R.End = J.Int("end");
            R.WonAt = J.Int("wonAt");
            R.FailedAt = J.Int("failedAt");
            R.Type = static_cast<int>(J.Int("type"));
            R.RaidId = static_cast<int>(J.Int("raidId", -1));
            R.Hardcore = J.Int("hardcore") != 0;
            R.Result = static_cast<int>(J.Int("result"));
            R.Self = static_cast<long>(J.Int("self"));
            R.BossVnum = static_cast<int>(J.Int("bossVnum"));
            R.BossId = static_cast<long>(J.Int("bossId"));
            R.BossMaxHp = J.Int("bossMaxHp");
            R.BossSeenAt = J.Int("bossSeenAt");
            R.BossKilledAt = J.Int("bossKilledAt");
            R.LivesLeft = static_cast<int>(J.Int("livesLeft", -1));
            R.MaxLives = static_cast<int>(J.Int("maxLives", -1));
            R.Log = J.Str("log");
            R.Scan = static_cast<int>(J.Int("loot"));
            R.TriesLeft = static_cast<int>(J.Int("triesLeft", -1));
            R.RewardAt = J.Int("rewardAt");
            R.RewardDamage = J.Int("rewardDamage");
            R.RewardLow = J.Int("rewardLow");
            R.RewardMateDamage = J.Int("rewardMateDamage");
            R.RewardPlayers = static_cast<int>(J.Int("rewardPlayers"));
            R.MateBossDamage = J.Int("mateBossDamage");
            R.RewardBossLost = J.Int("rewardBossLost");
            R.BossFirstHitAt = J.Int("bossFirstHitAt");
            if (const Json* Maps = J["maps"]) {
                for (const Json& Map : Maps->Items) R.Maps.push_back(static_cast<int>(Map.N));
            }
            if (const Json* Party = J["party"]) {
                for (const Json& Id : Party->Items) R.Party.push_back(static_cast<long>(Id.N));
            }
            if (const Json* Times = J["locks"]) {
                for (const Json& T : Times->Items) R.LockTimes.push_back({T.At(0), T.At(1), T.At(2), T.At(3), T.At(4)});
            }
            if (const Json* Members = J["members"]) {
                for (const Json& M : Members->Items) {
                    Member Entry;
                    Entry.Id = static_cast<long>(M.Int("id"));
                    Entry.Name = M.Str("name");
                    Entry.Level = static_cast<int>(M.Int("level"));
                    Entry.HeroLevel = static_cast<int>(M.Int("heroLevel"));
                    Entry.Class = static_cast<int>(M.Int("class", -1));
                    Entry.Gender = static_cast<int>(M.Int("gender", -1));
                    Entry.In = M.Str("in");
                    Entry.Morph = static_cast<int>(M.Int("morph", -1));
                    Entry.Listing = M.Str("listing");
                    Entry.Damage = M.Int("damage");
                    Entry.BossDamage = M.Int("bossDamage");
                    Entry.Hits = static_cast<int>(M.Int("hits"));
                    Entry.Crits = static_cast<int>(M.Int("crits"));
                    Entry.Misses = static_cast<int>(M.Int("misses"));
                    Entry.Deaths = static_cast<int>(M.Int("deaths"));
                    Entry.LeftAt = M.Int("leftAt");
                    Entry.Gold = M.Int("gold");
                    if (const Json* Picked = M["picked"]) {
                        for (const Json& P : Picked->Items) Entry.Picked[static_cast<int>(P.At(0))] = P.At(1);
                    }
                    if (const Json* Changes = M["morphChanges"]) {
                        for (const Json& C : Changes->Items) Entry.MorphChanges.emplace_back(C.At(0), static_cast<int>(C.At(1)));
                    }
                    if (const Json* Skills = M["skills"]) {
                        for (const Json& S : Skills->Items) {
                            Entry.Skills[static_cast<int>(S.At(0))] = {S.At(1), static_cast<int>(S.At(2)), static_cast<int>(S.At(3)),
                                                                        static_cast<int>(S.At(4))};
                        }
                    }
                    R.Members.push_back(std::move(Entry));
                }
            }
            if (const Json* Packets = J["packets"]) {
                for (const Json& P : Packets->Items) {
                    if (P.Items.size() == 2) R.Packets.emplace_back(P.At(0), P.Items[1].S);
                }
            }
            return true;
        }

        constexpr int GoldVnum = 1046;
        std::unordered_map<long, std::pair<int, int64_t>> Drops;
        std::set<long> InStatus;

        bool ReadDrop(const std::string& Header, const std::vector<std::string>& T, long& Id, int& Vnum, int64_t& Amount) {
            if (Header == "throw" && T.size() >= 8) {
                Vnum = static_cast<int>(Num(T[1]));
                Id = static_cast<long>(Num(T[2]));
                Amount = Num(T[7]);
            } else if (Header == "drop" && T.size() >= 6) {
                Vnum = static_cast<int>(Num(T[1]));
                Id = static_cast<long>(Num(T[2]));
                Amount = Num(T[5]);
            } else if (Header == "in" && T.size() >= 7 && T[1] == "9") {
                Vnum = static_cast<int>(Num(T[2]));
                Id = static_cast<long>(Num(T[3]));
                Amount = Num(T[6]);
            } else {
                return false;
            }
            return true;
        }

        Member* FindIn(Raid& R, const long Id) {
            for (Member& M : R.Members) {
                if (M.Id == Id) return &M;
            }
            return nullptr;
        }

        void AddPickup(Member& M, const int Vnum, const int64_t Amount) {
            if (Vnum == GoldVnum) M.Gold += Amount;
            else M.Picked[Vnum] += Amount;
        }

        void NoteStatus(Raid& R, std::set<long>& Before, const std::vector<std::string>& T, const int64_t At) {
            std::set<long> Now;
            for (size_t i = 2; i < T.size(); i++) {
                if (const long Id = static_cast<long>(Num(Split(T[i], '.')[0]))) Now.insert(Id);
            }
            for (const long Id : Before) {
                if (Now.contains(Id)) continue;
                if (Member* M = FindIn(R, Id); M && !M->LeftAt) M->LeftAt = At;
            }
            for (const long Id : Now) {
                if (Member* M = FindIn(R, Id)) M->LeftAt = 0;
            }
            Before = std::move(Now);
        }

        bool LeftEarly(const Raid& R, const Member& M) {
            if (!M.LeftAt) return false;
            const int64_t Over = R.WonAt ? R.WonAt : R.FailedAt;
            return !Over || M.LeftAt < Over;
        }

        std::filesystem::path HistoryFile() { return DataDir() / L"raids.jsonl"; }
        std::filesystem::path ActiveFile() { return DataDir() / L"raid_active.json"; }

        std::filesystem::path LogDir();

        std::filesystem::path Packed(std::filesystem::path Raw) {
            Raw += L".z";
            return Raw;
        }

        std::string ReadAll(const std::filesystem::path& File) {
            std::ifstream In(File, std::ios::binary);
            return {std::istreambuf_iterator<char>(In), std::istreambuf_iterator<char>()};
        }

        bool PackFile(const std::filesystem::path& Raw) {
            const std::string Data = ReadAll(Raw);
            if (Data.empty()) return false;
            COMPRESSOR_HANDLE Compressor = nullptr;
            if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &Compressor)) return false;
            SIZE_T Needed = 0;
            Compress(Compressor, Data.data(), Data.size(), nullptr, 0, &Needed);
            std::string Out(Needed, '\0');
            SIZE_T Size = 0;
            const bool Done = Needed && Compress(Compressor, Data.data(), Data.size(), Out.data(), Out.size(), &Size);
            CloseCompressor(Compressor);
            if (!Done) return false;
            Out.resize(Size);
            auto Temp = Raw;
            Temp += L".tmp";
            WriteFile(Temp, Out);
            std::error_code Error;
            std::filesystem::rename(Temp, Packed(Raw), Error);
            if (Error) return false;
            std::filesystem::remove(Raw, Error);
            return true;
        }

        std::string Unpack(const std::string& Data) {
            DECOMPRESSOR_HANDLE Decompressor = nullptr;
            if (Data.empty() || !CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &Decompressor)) return {};
            SIZE_T Needed = 0;
            Decompress(Decompressor, Data.data(), Data.size(), nullptr, 0, &Needed);
            std::string Out(Needed, '\0');
            SIZE_T Size = 0;
            const bool Done = Needed && Decompress(Decompressor, Data.data(), Data.size(), Out.data(), Out.size(), &Size);
            CloseDecompressor(Decompressor);
            if (!Done) return {};
            Out.resize(Size);
            return Out;
        }

        std::vector<std::string> LogLines(const std::string& Name) {
            const auto Raw = LogDir() / Name;
            std::error_code Error;
            const std::string Text = std::filesystem::exists(Raw, Error) ? ReadAll(Raw) : Unpack(ReadAll(Packed(Raw)));
            std::vector<std::string> Lines;
            size_t From = 0;
            while (From < Text.size()) {
                size_t To = Text.find('\n', From);
                if (To == std::string::npos) To = Text.size();
                if (To > From) Lines.emplace_back(Text, From, To - From);
                From = To + 1;
            }
            return Lines;
        }

        std::thread Packer;
        std::atomic<bool> StopPacking{false};

        void PackLater(std::vector<std::filesystem::path> Files) {
            if (Files.empty()) return;
            if (Packer.joinable()) Packer.join();
            Packer = std::thread([Files = std::move(Files)] {
                for (const auto& File : Files) {
                    if (StopPacking) break;
                    PackFile(File);
                }
            });
        }

        void AddParty(Raid& R, const long Id) {
            if (Id && std::find(R.Party.begin(), R.Party.end(), Id) == R.Party.end()) R.Party.push_back(Id);
        }

        void AddListingParty(Raid& R, const std::string& Packet) {
            const auto Tokens = Split(Packet, ' ');
            for (size_t i = 4; i < Tokens.size(); i++) {
                const auto Parts = Split(Tokens[i], '.');
                if (Parts.size() >= 8) AddParty(R, static_cast<long>(Num(Parts[6])));
            }
        }

        void AddStatusParty(Raid& R, const std::vector<std::string>& Tokens) {
            for (size_t i = 2; i < Tokens.size(); i++) AddParty(R, static_cast<long>(Num(Split(Tokens[i], '.')[0])));
        }

        void KeepParty(Raid& R) {
            if (R.Party.empty()) return;
            std::erase_if(R.Members, [&](const Member& M) {
                return std::find(R.Party.begin(), R.Party.end(), M.Id) == R.Party.end();
            });
        }

        void FillFromLog(Raid& R) {
            if (R.RaidId >= 0 || R.Log.empty()) return;
            std::string Listing;
            bool Started = false;
            const auto Lines = LogLines(R.Log);
            for (const std::string& Line : Lines) {
                const auto Fields = Split(Line, '\t');
                if (Fields.size() < 3 || Fields[1] != "<") continue;
                if (Num(Fields[0]) >= 0) Started = true;
                if (!Started && (Fields[2] == "raid 2 -1" || Fields[2] == "raidf 2 -1")) Listing.clear();
                if (!Fields[2].starts_with("rdlst")) continue;
                if (Started && !Listing.empty()) break;
                Listing = Fields[2];
                if (Started) break;
            }
            R.Party.clear();
            if (!Listing.empty()) AddListingParty(R, Listing);
            for (const std::string& Line : Lines) {
                const auto Fields = Split(Line, '\t');
                if (Fields.size() < 3 || Fields[1] != "<" || Num(Fields[0]) < 0) continue;
                if (Fields[2].starts_with("rdlst")) AddListingParty(R, Fields[2]);
                else if (Fields[2].starts_with("raid 3 ") || Fields[2].starts_with("raidfhp 3 ")) AddStatusParty(R, Split(Fields[2], ' '));
            }
            KeepParty(R);
            {
                const auto Tokens = Split(Listing, ' ');
                if (Tokens.size() < 4) return;
                R.RaidId = static_cast<int>(Num(Tokens[3]));
                R.Hardcore = Tokens[0].ends_with("hd");
                for (size_t i = 4; i < Tokens.size(); i++) {
                    const auto Parts = Split(Tokens[i], '.');
                    if (Parts.size() < 8) continue;
                    for (Member& M : R.Members) {
                        if (M.Id != Num(Parts[6])) continue;
                        M.Level = static_cast<int>(Num(Parts[0]));
                        if (IsSp(static_cast<int>(Num(Parts[1])))) M.Morph = static_cast<int>(Num(Parts[1]));
                        M.Class = static_cast<int>(Num(Parts[2]));
                        M.Name = Parts[4];
                        M.Gender = static_cast<int>(Num(Parts[5]));
                        M.HeroLevel = static_cast<int>(Num(Parts[7]));
                        M.Listing = Tokens[i];
                    }
                }
            }
        }

        constexpr int CurrentScan = 6;
        std::set<long> MyMates;
        bool RewardPending = false;

        constexpr int64_t SameBatchMs = 100;
        bool IsRewardMessage(const std::vector<std::string>& T, const long Self) {
            return T.size() >= 5 && T[0] == "sayi" && T[4] == "2734" && static_cast<long>(Num(T[2])) == Self;
        }

        bool ReadTriesLeft(const std::vector<std::string>& T, const long Self, int& Left) {
            if (T.size() < 8 || T[0] != "sayi" || T[4] != "2725" || static_cast<long>(Num(T[2])) != Self) return false;
            Left = static_cast<int>(Num(T[7]));
            return true;
        }

        bool FillLoot(Raid& R) {
            if (R.Scan >= CurrentScan || R.Log.empty()) return false;
            for (Member& M : R.Members) {
                M.LeftAt = 0;
                M.Gold = 0;
                M.Picked.clear();
            }
            std::unordered_map<long, std::pair<int, int64_t>> Ground;
            std::set<long> StatusIds;
            int64_t MyBoss = 0, BossTaken = 0;
            bool Pending = false;
            std::set<long> Mates;
            R.RewardAt = R.RewardDamage = R.RewardLow = R.RewardMateDamage = R.MateBossDamage = R.RewardBossLost = 0;
            R.RewardPlayers = 0;
            for (const std::string& Line : LogLines(R.Log)) {
                const auto Fields = Split(Line, '\t');
                if (Fields.size() < 3 || Fields[1] != "<") continue;
                const int64_t At = Num(Fields[0]);
                const auto T = Split(Fields[2], ' ');
                if (T.empty()) continue;
                long Id = 0;
                int Vnum = 0;
                int64_t Amount = 0;
                if (ReadDrop(T[0], T, Id, Vnum, Amount)) {
                    Ground[Id] = {Vnum, Amount};
                } else if (T[0] == "get" && T.size() >= 4 && T[1] == "1" && At >= 0) {
                    const auto Found = Ground.find(static_cast<long>(Num(T[3])));
                    Member* M = FindIn(R, static_cast<long>(Num(T[2])));
                    if (Found != Ground.end() && M) AddPickup(*M, Found->second.first, Found->second.second);
                    if (Found != Ground.end()) Ground.erase(Found);
                } else if ((T[0] == "raid" || T[0] == "raidfhp") && T.size() >= 2 && T[1] == "3" && At >= 0) {
                    NoteStatus(R, StatusIds, T, At);
                } else if (T[0] == "pinit") {
                    Mates.clear();
                    for (size_t i = 2; i < T.size(); i++) {
                        const auto F = Split(T[i], '|');
                        if (F.size() > 1 && F[0] == "2") Mates.insert(static_cast<long>(Num(F[1])));
                    }
                } else if (At >= 0 && !R.RewardAt && IsRewardMessage(T, R.Self)) {
                    R.RewardAt = At;
                    R.RewardDamage = R.RewardLow = MyBoss;
                    R.RewardMateDamage = R.MateBossDamage;
                    R.RewardPlayers = static_cast<int>(StatusIds.size());
                    R.RewardBossLost = BossTaken;
                    Pending = true;
                } else if (int Left = 0; R.Hardcore && At >= 0 && ReadTriesLeft(T, R.Self, Left)) {
                    R.TriesLeft = Left;
                } else if (T[0] == "su" && T.size() >= 12 && R.BossId && Num(T[4]) == R.BossId && At >= 0) {
                    const int64_t Hit = T.size() > 13 ? std::max<int64_t>(0, Num(T[13])) : 0;
                    if (T[1] == "1" && static_cast<long>(Num(T[2])) == R.Self && Hit) {
                        if (Pending && At - R.RewardAt <= SameBatchMs) {
                            R.RewardLow = MyBoss;
                            R.RewardDamage = MyBoss + Hit;
                        }
                        Pending = false;
                        MyBoss += Hit;
                    }
                    if (!R.RewardAt) BossTaken += Hit;
                    if (T[1] == "2" && Mates.contains(static_cast<long>(Num(T[2]))) && T.size() > 13) {
                        R.MateBossDamage += std::max<int64_t>(0, Num(T[13]));
                    }
                    if (!R.BossFirstHitAt) R.BossFirstHitAt = At;
                    if (T[11] == "0" && !R.BossKilledAt) R.BossKilledAt = At;
                }
            }
            if (!R.BossKilledAt && R.WonAt) R.BossKilledAt = R.WonAt;
            R.Scan = CurrentScan;
            return true;
        }

        void LoadHistory() {
            History.clear();
            bool Changed = false;
            for (const std::string& Line : ReadLines(HistoryFile())) {
                Raid R;
                if (!FromJson(Line, R)) continue;
                if (R.Scan < CurrentScan) FillFromLog(R);
                if (FillLoot(R)) Changed = true;
                History.push_back(std::move(R));
            }
            if (Changed) {
                std::string Text;
                for (const Raid& R : History) Text += ToJson(R) + "\n";
                WriteFile(HistoryFile(), Text);
            }
            Revision++;
        }


        Member* FindMember(const long Id) {
            for (Member& M : Active.Members) {
                if (M.Id == Id) return &M;
            }
            return nullptr;
        }

        void Fill(Member& M, const Seen& S) {
            if (!S.Name.empty()) M.Name = S.Name;
            if (S.Class >= 0) M.Class = S.Class;
            if (S.Gender >= 0) M.Gender = S.Gender;
            if (S.Sp > 0 && M.Morph <= 0) M.Morph = S.Sp;
            if (!S.In.empty()) M.In = S.In;
        }

        Member& MemberFor(const long Id) {
            if (Member* M = FindMember(Id)) return *M;
            Member Added;
            Added.Id = Id;
            if (Id == SelfId && IsSp(SelfMorph)) Added.Morph = SelfMorph;
            if (const auto Found = Known.find(Id); Found != Known.end()) Fill(Added, Found->second);
            Active.Members.push_back(Added);
            return Active.Members.back();
        }

        void Learn(const long Id, const Seen& S) {
            if (HasActive) {
                if (Member* M = FindMember(Id)) Fill(*M, S);
            }
        }

        std::filesystem::path LogDir() {
            const auto Dir = DataDir() / L"raids";
            std::error_code Error;
            std::filesystem::create_directories(Dir, Error);
            return Dir;
        }

        bool WorthLogging(const std::string& Text) {
            static const std::set<std::string> Skipped = {
                "mv", "tp", "walk", "ptctl", "st", "ct", "ct_n", "eff_t", "eff_s", "eff_g", "cond",
                "qslot", "mslot", "qset", "sc_p", "sc_n", "sr", "ms_c", "sp", "ncif",
            };
            const std::string Header = Text.substr(0, Text.find(' '));
            if (Header == "eff") return Text.starts_with("eff 1 ");
            return !Skipped.contains(Header);
        }

        void WriteLog(const Logged& P) {
            if (LogOut) LogOut << (P.At - Active.Start) << '\t' << (P.Sent ? '>' : '<') << '\t' << P.Text << '\n';
        }

        void OpenLog(const bool WithRecent) {
            if (LogOut.is_open()) LogOut.close();
            if (Active.Log.empty()) Active.Log = std::to_string(Active.Start) + ".log";
            LogOut.open(LogDir() / Active.Log, std::ios::binary | std::ios::app);
            if (WithRecent) {
                for (const Logged& P : Recent) WriteLog(P);
            }
            Recent.clear();
        }

        void ApplyListing(const std::string& Packet) {
            const auto Tokens = Split(Packet, ' ');
            if (HasActive && Active.RaidId < 0 && Tokens.size() >= 4) {
                Active.RaidId = static_cast<int>(Num(Tokens[3]));
                Active.Hardcore = Tokens[0].size() >= 2 && Tokens[0].ends_with("hd");
            }
            for (size_t i = 4; i < Tokens.size(); i++) {
                const auto Parts = Split(Tokens[i], '.');
                if (Parts.size() < 8) continue;
                const long Id = static_cast<long>(Num(Parts[6]));
                const int Morph = static_cast<int>(Num(Parts[1]));
                Seen& S = Known[Id];
                S.Name = Parts[4];
                S.Class = static_cast<int>(Num(Parts[2]));
                S.Gender = static_cast<int>(Num(Parts[5]));
                if (IsSp(Morph)) S.Sp = Morph;
                if (!HasActive) continue;
                Member& M = MemberFor(Id);
                M.Level = static_cast<int>(Num(Parts[0]));
                if (IsSp(Morph)) M.Morph = Morph;
                M.Class = S.Class;
                M.Gender = S.Gender;
                M.Name = S.Name;
                M.HeroLevel = static_cast<int>(Num(Parts[7]));
                M.Listing = Tokens[i];
            }
        }

        int OpenRaid = -2;

        void Finish(const int Result) {
            if (!HasActive) return;
            Active.End = NowMs();
            if (Active.Result == Running) Active.Result = Result;
            KeepParty(Active);
            LastListing.clear();
            AppendLine(HistoryFile(), ToJson(Active));
            History.push_back(Active);
            if (OpenRaid == -1) OpenRaid = static_cast<int>(History.size()) - 1;
            HasActive = false;
            Active = {};
            LogOut.close();
            std::error_code Error;
            std::filesystem::remove(ActiveFile(), Error);
            if (!History.back().Log.empty()) PackLater({LogDir() / History.back().Log});
            Revision++;
        }

        void Begin(const int Type) {
            if (HasActive) Finish(Failed);
            Active = {};
            HasActive = true;
            Active.Start = NowMs();
            Active.Type = Type;
            Active.Scan = CurrentScan;
            InStatus.clear();
            RewardPending = false;
            if (const long Id = GameData::PlayerId()) SelfId = Id;
            Active.Self = SelfId;
            OpenLog(true);
            if (!LastListing.empty()) {
                ApplyListing(LastListing);
                AddListingParty(Active, LastListing);
            }
            if (SelfId) MemberFor(SelfId);
            Revision++;
        }

        bool OnSu(const Packet::SuPacket& Packet) {
            if (!HasActive) return true;
            if (Packet.targetEntityType == EntityType::Player && !Packet.targetAlive) {
                MemberFor(Packet.targetEntityID).Deaths++;
            }
            if (Packet.userEntityType == EntityType::NPC && MyMates.contains(Packet.userEntityID) && Active.BossId
                && Packet.targetEntityID == Active.BossId) {
                Active.MateBossDamage += std::max(0, Packet.damage);
            }
            if (Active.BossId && Packet.targetEntityID == Active.BossId && !Active.RewardAt) {
                Active.BossTaken += std::max(0, Packet.damage);
            }
            if (Packet.userEntityType != EntityType::Player || Packet.targetEntityType != EntityType::Monster) return true;
            if (Packet.hitMode == HitMode::SuccessfulBuff) return true;
            const bool Miss = Packet.hitMode == HitMode::Miss || Packet.hitMode == HitMode::LongRangeMiss
                           || Packet.hitMode == HitMode::UnknownMiss;
            const bool Crit = Packet.hitMode == HitMode::CriticalAttack || Packet.hitMode == HitMode::UnknownCriticalAttack;
            const int64_t Damage = Miss ? 0 : std::max(0, Packet.damage);
            Member& M = MemberFor(Packet.userEntityID);
            SkillStat& S = M.Skills[SkillKey(Packet.skillVNum, Packet.skillEffect)];
            M.Damage += Damage;
            S.Damage += Damage;
            M.Hits++;
            S.Hits++;
            if (Crit) { M.Crits++; S.Crits++; }
            if (Miss) { M.Misses++; S.Misses++; }
            if (Active.BossId && Packet.targetEntityID == Active.BossId) {
                if (Packet.userEntityID == Active.Self && Damage) {
                    if (RewardPending && NowMs() - Active.Start - Active.RewardAt <= SameBatchMs) {
                        Active.RewardLow = M.BossDamage;
                        Active.RewardDamage = M.BossDamage + Damage;
                        Revision++;
                    }
                    RewardPending = false;
                }
                M.BossDamage += Damage;
                if (!Active.BossFirstHitAt) Active.BossFirstHitAt = NowMs() - Active.Start;
                if (!Packet.targetAlive && !Active.BossKilledAt) Active.BossKilledAt = NowMs() - Active.Start;
            }
            return true;
        }

        bool OnCInfo(const Packet::CInfoPacket& Packet) {
            if (SelfId != Packet.characterID) Revision++;
            SelfId = Packet.characterID;
            SelfMorph = static_cast<int>(Packet.morphVNum);
            Seen& S = Known[SelfId];
            S.Name = Packet.name;
            S.Class = static_cast<int>(Packet.characterClass);
            S.Gender = Packet.sex;
            if (IsSp(SelfMorph)) S.Sp = SelfMorph;
            if (HasActive && !Active.Self) Active.Self = SelfId;
            Learn(SelfId, S);
            return true;
        }

        bool OnIn(const Packet::InPacket& Packet) {
            if (Packet.entityType != EntityType::Player) return true;
            Seen& S = Known[Packet.entityID];
            S.Name = Packet.entityName;
            S.Class = static_cast<int>(Packet.inPlayerSubPacket.playerClass);
            S.Gender = Packet.inPlayerSubPacket.sex;
            if (IsSp(static_cast<int>(Packet.inPlayerSubPacket.morphVNum))) S.Sp = static_cast<int>(Packet.inPlayerSubPacket.morphVNum);
            Learn(Packet.entityID, S);
            return true;
        }

        void SaveActive() {
            if (HasActive) WriteFile(ActiveFile(), ToJson(Active));
        }

        void LoadActive() {
            const auto Lines = ReadLines(ActiveFile());
            Raid R;
            if (!Lines.empty() && FromJson(Lines[0], R) && NowMs() - R.Start < 3 * 60 * 60 * 1000) {
                Active = std::move(R);
                HasActive = true;
                OpenLog(false);
            }
        }


        const Member* SelfIn(const Raid& R) {
            for (const Member& M : R.Members) {
                if (M.Id == R.Self) return &M;
            }
            return nullptr;
        }

        int64_t TotalDamage(const Raid& R) {
            int64_t Total = 0;
            for (const Member& M : R.Members) Total += M.Damage;
            return Total;
        }

        int64_t Length(const Raid& R) {
            return (R.End ? R.End : NowMs()) - R.Start;
        }

        int64_t WinTime(const Raid& R) {
            return R.WonAt ? R.WonAt : Length(R);
        }

        int64_t RaidTime(const Raid& R) {
            if (R.WonAt) return R.WonAt;
            if (R.FailedAt) return R.FailedAt;
            return Length(R);
        }

        int64_t BossTime(const Raid& R) {
            const int64_t From = R.BossFirstHitAt ? R.BossFirstHitAt : R.BossSeenAt;
            return From && R.BossKilledAt > From ? R.BossKilledAt - From : 0;
        }

        int SelfMorphOf(const Raid& R) {
            const Member* Me = SelfIn(R);
            return Me ? Me->Morph : -1;
        }

        constexpr int UnknownRaid = 100000;

        int RaidKey(const Raid& R) {
            return R.RaidId >= 0 ? R.RaidId * 2 + (R.Hardcore ? 1 : 0) : UnknownRaid + R.Type;
        }

        std::wstring RaidName(const int Key) {
            if (Key >= UnknownRaid) return L"Unknown raid";
            std::wstring Name = GameData::RaidName(Key / 2);
            if (Name.empty()) Name = L"Raid " + std::to_wstring(Key / 2);
            return Key % 2 ? Name + L" (Hardcore)" : Name;
        }

        const wchar_t* ClassName(const int Class) {
            switch (Class) {
                case 0: return L"Adventurer";
                case 1: return L"Swordsman";
                case 2: return L"Archer";
                case 3: return L"Mage";
                case 4: return L"Martial Artist";
                default: return L"?";
            }
        }

        std::wstring MemberName(const Member& M) {
            return M.Name.empty() ? L"#" + std::to_wstring(M.Id) : Wide(M.Name);
        }

        const Raid* RaidAt(const int Index) {
            if (Index == -1) return HasActive ? &Active : nullptr;
            return Index >= 0 && Index < static_cast<int>(History.size()) ? &History[Index] : nullptr;
        }


        enum class View { List, Stats, Raid, Player, Weekly };
        enum WeeklyKind { Weeklies, Dailies };
        int WeeklyTab = Weeklies;
        WidgetKit::Tab WeeklyButton;
        WidgetKit::Tab WeeklyTabs[2];
        constexpr int HardcoreTries = 35;

        int64_t GermanOffsetMs(const int64_t UtcMs) {
            const std::time_t Seconds = static_cast<std::time_t>(UtcMs / 1000);
            std::tm Utc{};
            gmtime_s(&Utc, &Seconds);
            auto LastSunday = [&](const int Month) {
                std::tm Day{};
                Day.tm_year = Utc.tm_year;
                Day.tm_mon = Month;
                Day.tm_mday = 31;
                std::time_t At = _mkgmtime(&Day);
                std::tm Check{};
                gmtime_s(&Check, &At);
                Day.tm_mday = 31 - Check.tm_wday;
                Day.tm_hour = 1;
                return static_cast<int64_t>(_mkgmtime(&Day));
            };
            const bool Summer = Seconds >= LastSunday(2) && Seconds < LastSunday(9);
            return (Summer ? 2 : 1) * 3600'000LL;
        }

        int64_t LastReset(const int64_t NowMs, const bool Weekly) {
            constexpr int64_t Day = 86'400'000LL;
            const int64_t Offset = GermanOffsetMs(NowMs);
            const int64_t Local = NowMs + Offset;
            int64_t Reset = Local - Local % Day + 2 * 3600'000LL;
            if (Weekly) {
                const int64_t Weekday = (Local / Day + 4) % 7;
                Reset -= ((Weekday + 6) % 7) * Day;
                if (Reset > Local) Reset -= 7 * Day;
            } else if (Reset > Local) {
                Reset -= Day;
            }
            return Reset - Offset;
        }

        std::wstring Until(const int64_t Ms) {
            const int64_t Minutes = std::max<int64_t>(0, Ms / 60'000);
            if (Minutes >= 1440) return std::to_wstring(Minutes / 1440) + L"d " + std::to_wstring(Minutes % 1440 / 60) + L"h";
            return std::to_wstring(Minutes / 60) + L"h " + std::to_wstring(Minutes % 60) + L"m";
        }
        enum StatsKind { ByRaid, BySp, ByPlayer };

        constexpr int TableRows = 15;
        constexpr int CardCount = 5;
        constexpr int16_t Top = 44;
        constexpr int16_t SideX = 12;
        constexpr int16_t SideWidth = 160;
        constexpr int16_t MainX = SideX + SideWidth + 14;
        constexpr int16_t CardHeight = 66;
        constexpr int SummaryLines = 7;

        TLBSWidget* PageWidget = nullptr;
        int16_t MainWidth = 0;
        int16_t FullWidth = 0;
        int16_t LineWidth = 0;
        std::vector<TLBSWidget*> Side;
        TEWLabel* Heading = nullptr;
        TEWLabel* Lines[3]{};
        TEWLabel* Summary[SummaryLines]{};
        Table Grid;
        Pager Pages;
        Clicks Click;
        Clicks RightClick{VK_RBUTTON};

        enum MenuItem { MenuOpen, MenuCopy, MenuDelete, MenuCount };
        constexpr int16_t MenuWidth = 130;
        TLBSWidget* Menu = nullptr;
        TEWButtonWidget* MenuButtons[MenuCount]{};
        int MenuRaid = -2;
        bool ConfirmDelete = false;
        WidgetKit::Tab ListButton, StatsButton, BackButton;
        WidgetKit::Tab KindButtons[3];

        struct Card {
            TLBSWidget* Box = nullptr;
            TEWControlWidget* Strip = nullptr;
            TEWGuageWidget* Bar = nullptr;
            TEWControlWidget* Rule = nullptr;
            TNTIconWidget* Icon = nullptr;
            TNTIconWidget* RaidIcon = nullptr;
            TEWGraphicButtonWidget* Hit = nullptr;
            TEWLabel* Name = nullptr;
            TEWLabel* Result = nullptr;
            TEWLabel* When = nullptr;
            TEWLabel* Line2 = nullptr;
            TEWLabel* Line3 = nullptr;
            TEWLabel* Top = nullptr;
        };
        Card Cards[CardCount];

        constexpr int FilterCount = 5;
        enum FilterKind { CharacterFilter, RaidFilter, SpFilter, PlayerFilter, ResultFilter };
        constexpr int ThisCharacter = -2;
        WidgetKit::Dropdown* Pickers[FilterCount]{};
        std::vector<int> PickerValues[FilterCount];
        int Filters[FilterCount] = {ThisCharacter, -1, -1, -1, -1};
        std::vector<std::wstring> RowNames;
        TNTIconWidget* RowIcons[TableRows]{};
        constexpr int16_t IconSize = 16;

        void HideRowIcons() {
            for (TNTIconWidget* Icon : RowIcons) WidgetKit::SetItemIcon(Icon, -1);
        }

        void SetRowIcon(const int Slot, const int Column, const int16_t IconId) {
            TNTIconWidget* Icon = Slot >= 0 && Slot < TableRows ? RowIcons[Slot] : nullptr;
            if (!Icon) return;
            const int16_t X = Grid.ColumnX(Column);
            const auto Y = static_cast<int16_t>(Grid.RowTop(Slot) + 1);
            Icon->rect = {X, Y, static_cast<int16_t>(X + IconSize), static_cast<int16_t>(Y + IconSize)};
            WidgetKit::SetItemIcon(Icon, IconId);
        }
        int CopiedRow = -1;
        size_t FilteredFrom = static_cast<size_t>(-1);
        std::map<int, std::wstring> PlayerNames;

        View Current = View::List;
        int Kind = ByRaid;
        long OpenMember = 0;
        int PageIndex = 0;
        std::vector<int> RowTargets;
        bool Dirty = true;
        int ShownRevision = -1;
        int64_t LastLiveRefresh = 0;

        bool HasMember(const Raid& R, const long Id) {
            return std::any_of(R.Members.begin(), R.Members.end(), [&](const Member& M) { return M.Id == Id; });
        }

        bool Matches(const Raid& R) {
            if (Filters[CharacterFilter] == ThisCharacter && SelfId && R.Self != SelfId) return false;
            if (Filters[CharacterFilter] >= 0 && R.Self != Filters[CharacterFilter]) return false;
            if (Filters[RaidFilter] >= 0 && RaidKey(R) != Filters[RaidFilter]) return false;
            if (Filters[SpFilter] >= 0) {
                const Member* Me = SelfIn(R);
                if (!Me) return false;
                bool Used = Me->Morph == Filters[SpFilter];
                for (const auto& [At, Morph] : Me->MorphChanges) Used = Used || Morph == Filters[SpFilter];
                if (!Used) return false;
            }
            if (Filters[PlayerFilter] >= 0 && !HasMember(R, Filters[PlayerFilter])) return false;
            if (Filters[ResultFilter] >= 0 && R.Result != Filters[ResultFilter]) return false;
            return true;
        }

        std::vector<int> Filtered(const bool WithActive) {
            std::vector<int> Order;
            if (WithActive && HasActive && Matches(Active)) Order.push_back(-1);
            for (int i = static_cast<int>(History.size()) - 1; i >= 0; i--) {
                if (Matches(History[i])) Order.push_back(i);
            }
            return Order;
        }

        std::wstring PlayerName(const int Id) {
            const auto Found = PlayerNames.find(Id);
            return Found == PlayerNames.end() || Found->second.empty() ? L"#" + std::to_wstring(Id) : Found->second;
        }

        std::wstring ResultName(const int Result) {
            return Result == Won ? L"Won" : Result == Failed ? L"Failed" : L"Running";
        }

        void FillPicker(const int Which, const wchar_t* All, const std::vector<int>& Found, std::wstring (*Name)(int)) {
            WidgetKit::Dropdown* Picker = Pickers[Which];
            if (!Picker) return;
            std::vector<std::wstring> Items{All};
            PickerValues[Which] = {-1};
            int Selected = 0;
            for (const int Value : Found) {
                if (Value == Filters[Which]) Selected = static_cast<int>(Items.size());
                Items.push_back(Name(Value));
                PickerValues[Which].push_back(Value);
            }
            if (!Selected) Filters[Which] = -1;
            Picker->SetItems(std::move(Items));
            Picker->SetSelected(Selected);
        }

        void FillPickers() {
            FilteredFrom = History.size();
            std::set<int> Types, Morphs;
            std::map<int, int> Together;
            PlayerNames.clear();
            for (const Raid& R : History) {
                Types.insert(RaidKey(R));
                if (const Member* Me = SelfIn(R)) {
                    if (IsSp(Me->Morph)) Morphs.insert(Me->Morph);
                    for (const auto& [At, Morph] : Me->MorphChanges) {
                        if (IsSp(Morph)) Morphs.insert(Morph);
                    }
                }
                for (const Member& M : R.Members) {
                    if (M.Id == R.Self) continue;
                    Together[static_cast<int>(M.Id)]++;
                    if (!M.Name.empty()) PlayerNames[static_cast<int>(M.Id)] = Wide(M.Name);
                }
            }
            std::vector<int> Players;
            for (const auto& [Id, Count] : Together) Players.push_back(Id);
            std::stable_sort(Players.begin(), Players.end(), [&](const int A, const int B) { return Together[A] > Together[B]; });
            FillPicker(RaidFilter, L"All raids", {Types.begin(), Types.end()}, &RaidName);
            FillPicker(SpFilter, L"All specialists", {Morphs.begin(), Morphs.end()}, &SpName);
            FillPicker(PlayerFilter, L"Anyone", Players, &PlayerName);
            FillPicker(ResultFilter, L"Any result", {Won, Failed}, &ResultName);

            std::map<int, std::wstring> Characters;
            for (const Raid& R : History) {
                if (const Member* Me = SelfIn(R); Me && !Me->Name.empty()) Characters[static_cast<int>(R.Self)] = Wide(Me->Name);
                else Characters.try_emplace(static_cast<int>(R.Self), L"#" + std::to_wstring(R.Self));
            }
            if (WidgetKit::Dropdown* Picker = Pickers[CharacterFilter]) {
                std::vector<std::wstring> Items{L"This character", L"All characters"};
                PickerValues[CharacterFilter] = {ThisCharacter, -1};
                int Selected = Filters[CharacterFilter] == -1 ? 1 : 0;
                for (const auto& [Id, Name] : Characters) {
                    if (Id == Filters[CharacterFilter]) Selected = static_cast<int>(Items.size());
                    Items.push_back(Name);
                    PickerValues[CharacterFilter].push_back(Id);
                }
                if (!Selected) Filters[CharacterFilter] = ThisCharacter;
                Picker->SetItems(std::move(Items));
                Picker->SetSelected(Selected);
            }
        }

        bool AnyPickerOpen() {
            for (WidgetKit::Dropdown* Picker : Pickers) {
                if (Picker && Picker->IsOpen()) return true;
            }
            return false;
        }

        void SetLine(const int Index, const std::wstring& Text, const Color& Tint = Hint) {
            SetText(Lines[Index], Text, Tint, LineWidth);
        }

        void ShowCards(const bool Visible) {
            for (Card& C : Cards) Show(C.Box, Visible);
        }

        void ShowControls() {
            const bool Browsing = Current == View::List || Current == View::Stats;
            WidgetKit::SetTabActive(ListButton, Current == View::List);
            WidgetKit::SetTabActive(StatsButton, Current == View::Stats);
            for (int i = 0; i < 3; i++) {
                Show(KindButtons[i].Button, Current == View::Stats);
                WidgetKit::SetTabActive(KindButtons[i], Kind == i);
            }
            Show(BackButton.Button, !Browsing);
            for (int i = 0; i < 2; i++) {
                Show(WeeklyTabs[i].Button, Current == View::Weekly);
                WidgetKit::SetTabActive(WeeklyTabs[i], WeeklyTab == i);
            }
            for (WidgetKit::Dropdown* Picker : Pickers) {
                if (Picker && !Browsing) Picker->SetOpen(false);
            }
            for (TLBSWidget* Part : Side) Show(Part, Browsing);
            const int16_t X = Browsing ? MainX : SideX;
            LineWidth = Browsing ? MainWidth : FullWidth;
            for (TEWLabel* Line : Lines) {
                if (!Line) continue;
                Line->rect = {X, Line->rect.top, static_cast<int16_t>(X + LineWidth), Line->rect.bottom};
                Line->pxPerLine = LineWidth;
            }
            if (BackButton.Button) {
                const int16_t W = static_cast<int16_t>(BackButton.Button->rect.right - BackButton.Button->rect.left);
                BackButton.Button->rect = {X, BackButton.Button->rect.top, static_cast<int16_t>(X + W), BackButton.Button->rect.bottom};
            }
            Grid.Place(X, LineWidth);
            Pages.Center(static_cast<int16_t>(X + LineWidth / 2));
            Show(Heading, Current == View::List);
            for (TEWLabel* Line : Lines) Show(Line, Current != View::List);
            ShowCards(Current == View::List);
            if (Current == View::List) Grid.SetColumns({});
        }

        void Go(const View Next) {
            Current = Next;
            PageIndex = 0;
            Dirty = true;
            ShowControls();
        }

        int PageCountFor(const int Count, const int PerPage) {
            return std::max(1, (Count + PerPage - 1) / PerPage);
        }

        template <typename RowFn>
        void FillRows(const int Count, RowFn&& Row) {
            const int PageCount = PageCountFor(Count, TableRows);
            PageIndex = std::clamp(PageIndex, 0, PageCount - 1);
            Grid.ClearRows();
            for (int Slot = 0; Slot < TableRows; Slot++) {
                const int At = PageIndex * TableRows + Slot;
                if (At < Count) Row(Slot, At);
            }
            Pages.Update(PageIndex, PageCount);
        }

        int64_t BossTotal(const Raid& R) {
            int64_t Total = 0;
            for (const Member& M : R.Members) Total += M.BossDamage;
            return Total;
        }

        void RefreshSummary() {
            const auto Order = Filtered(false);
            int WonCount = 0;
            int64_t Best = 0, WonTotal = 0, Played = 0, MyBoss = 0, MyBest = 0;
            for (const int Index : Order) {
                const Raid& R = History[Index];
                Played += RaidTime(R);
                if (R.Result == Won) {
                    WonCount++;
                    WonTotal += WinTime(R);
                    Best = Best ? std::min(Best, WinTime(R)) : WinTime(R);
                }
                if (const Member* Me = SelfIn(R)) {
                    MyBoss += Me->BossDamage;
                    MyBest = std::max(MyBest, Me->BossDamage);
                }
            }
            const int Done = static_cast<int>(Order.size());
            const std::wstring Texts[SummaryLines] = {
                std::to_wstring(Done) + (Done == 1 ? L" raid" : L" raids"),
                L"Won " + std::to_wstring(WonCount) + L" (" + Percent(WonCount, Done) + L")",
                L"Best time " + (Best ? Duration(Best) : L"-"),
                L"Average time " + (WonCount ? Duration(WonTotal / WonCount) : L"-"),
                L"Boss damage avg " + (Done ? Compact(MyBoss / Done) : L"-"),
                L"Boss damage best " + (MyBest ? Compact(MyBest) : L"-"),
                L"In raids " + Duration(Played),
            };
            for (int i = 0; i < SummaryLines; i++) SetText(Summary[i], Texts[i], i ? Hint : Gold, SideWidth);
        }

        void FillCard(Card& C, const Raid& R) {
            const Member* Me = SelfIn(R);
            const Color& Tint = R.Result == Won ? Good : R.Result == Failed ? Bad : Gold;
            static const Color StripWon(255, 40, 205, 60);
            static const Color StripFailed(255, 225, 45, 40);
            SetColor(C.Strip, R.Result == Won ? StripWon : R.Result == Failed ? StripFailed : Gold);
            const int16_t TextWidth = static_cast<int16_t>(MainWidth - 50);
            SetText(C.Name, RaidName(RaidKey(R)), Gold, TextWidth - 216);
            SetText(C.Result, ResultName(R.Result) + L"  " + Duration(RaidTime(R)), Tint, 90);
            SetText(C.When, Ago(R.Start), Hint, 110);

            std::vector<const Member*> ByBoss;
            for (const Member& M : R.Members) ByBoss.push_back(&M);
            std::sort(ByBoss.begin(), ByBoss.end(), [](const Member* A, const Member* B) { return A->BossDamage > B->BossDamage; });
            const int64_t Total = BossTotal(R);
            const int64_t TopBoss = ByBoss.empty() ? 0 : ByBoss.front()->BossDamage;
            std::wstring Line2 = L"Not in this raid's records";
            if (Me) {
                const auto Rank = std::find(ByBoss.begin(), ByBoss.end(), Me) - ByBoss.begin() + 1;
                Line2 = SpName(Me->Morph) + L"   Boss " + Compact(Me->BossDamage) + L" (" + Percent(Me->BossDamage, Total) + L"), #"
                      + std::to_wstring(Rank) + L" of " + std::to_wstring(R.Members.size());
                if (const int64_t Fight = BossTime(R); Fight > 0 && Me->BossDamage > 0) {
                    Line2 += L"   " + Compact(Me->BossDamage * 1000 / Fight) + L" boss DPS";
                }
            }
            SetText(C.Line2, Line2, Plain, TextWidth);

            SetText(C.Line3, BossTime(R) ? L"Boss fight " + Duration(BossTime(R)) : L"", Hint, TextWidth - 230);
            SetText(C.Top, !ByBoss.empty() && TopBoss > 0 ? L"Top: " + MemberName(*ByBoss.front()) + L" (" + Compact(TopBoss) + L" DMG)" : L"",
                    !ByBoss.empty() && ByBoss.front() == Me ? Gold : Hint, 226);

            if (C.Bar) {
                C.Bar->fillPercent = Me && TopBoss > 0 ? std::clamp(static_cast<float>(Me->BossDamage) / static_cast<float>(TopBoss), 0.0f, 1.0f) : 0.0f;
                C.Bar->gaugeColor = Me && Me == (ByBoss.empty() ? nullptr : ByBoss.front()) ? Color(255, 255, 200, 0) : Color(255, 40, 150, 255);
            }
            const GameData::SpCard* Sp = Me ? GameData::FindSp(Me->Morph) : nullptr;
            WidgetKit::SetItemIcon(C.Icon, Sp ? Sp->Icon : static_cast<int16_t>(-1));
            WidgetKit::SetItemIcon(C.RaidIcon, R.RaidId >= 0 ? GameData::RaidIcon(R.RaidId, R.Hardcore) : static_cast<int16_t>(-1));
        }

        void RefreshList() {
            const auto Order = Filtered(true);
            const int PageCount = PageCountFor(static_cast<int>(Order.size()), CardCount);
            PageIndex = std::clamp(PageIndex, 0, PageCount - 1);
            SetText(Heading, Order.empty() ? (History.empty() && !HasActive ? L"No raids yet. Raids are recorded from when they start."
                                                                             : L"No raid matches these filters.")
                                           : L"Newest first. Click a raid to see every player in it.", Hint, MainWidth);
            RowTargets.assign(CardCount, -2);
            for (int Slot = 0; Slot < CardCount; Slot++) {
                const int At = PageIndex * CardCount + Slot;
                const bool Used = At < static_cast<int>(Order.size());
                Show(Cards[Slot].Box, Used);
                if (!Used) continue;
                RowTargets[Slot] = Order[At];
                FillCard(Cards[Slot], *RaidAt(Order[At]));
            }
            Pages.Update(PageIndex, PageCount);
        }

        struct Tally {
            int Runs = 0;
            int Wins = 0;
            int64_t Best = 0;
            int64_t WonTime = 0;
            int64_t Damage = 0;
            int64_t Hits = 0;
            int64_t Crits = 0;
            int64_t LastSeen = 0;
            std::wstring Name;

            void Add(const Raid& R, const Member* M) {
                Runs++;
                if (R.Result == Won) {
                    Wins++;
                    WonTime += WinTime(R);
                    Best = Best ? std::min(Best, WinTime(R)) : WinTime(R);
                }
                if (M) {
                    Damage += M->BossDamage;
                    Hits += M->Hits;
                    Crits += M->Crits;
                }
                LastSeen = std::max(LastSeen, R.Start);
            }
        };

        void RefreshStats() {
            const auto Order = Filtered(false);
            std::map<int, Tally> Groups;
            for (const int Index : Order) {
                const Raid& R = History[Index];
                const Member* Me = SelfIn(R);
                if (Kind == ByRaid) Groups[RaidKey(R)].Add(R, Me);
                else if (Kind == BySp) Groups[SelfMorphOf(R)].Add(R, Me);
                else {
                    for (const Member& M : R.Members) {
                        if (M.Id == R.Self) continue;
                        Tally& T = Groups[static_cast<int>(M.Id)];
                        T.Add(R, &M);
                        if (!M.Name.empty()) T.Name = Wide(M.Name);
                    }
                }
            }
            std::vector<std::pair<int, Tally>> Sorted(Groups.begin(), Groups.end());
            std::stable_sort(Sorted.begin(), Sorted.end(), [](const auto& A, const auto& B) { return A.second.Runs > B.second.Runs; });

            if (Kind == ByRaid) {
                Grid.SetColumns({{L"Raid", 150}, {L"Runs", 40, 2}, {L"Won", 40, 2}, {L"Win %", 44, 2}, {L"Best", 50, 2},
                                 {L"Average", 56, 2}, {L"Your avg boss damage", 0, 2}});
                SetLine(2, L"Times are for won raids. Click a raid to list its runs.");
            } else if (Kind == BySp) {
                Grid.SetColumns({{L"Your specialist", 150}, {L"Runs", 40, 2}, {L"Won", 40, 2}, {L"Win %", 44, 2}, {L"Average", 56, 2},
                                 {L"Avg boss damage", 90, 2}, {L"Crit %", 0, 2}});
                SetLine(2, L"Grouped by the specialist you ended the raid in. Click one to list its runs.");
            } else {
                Grid.SetColumns({{L"Player", 130}, {L"Raids", 44, 2}, {L"Won", 44, 2}, {L"Win %", 44, 2}, {L"Their avg boss damage", 130, 2},
                                 {L"Last raid", 0, 2}});
                SetLine(2, L"The players you raided with, most raids first. Click one to list your raids together.");
            }
            SetLine(0, std::to_wstring(Order.size()) + L" finished raids match the filters.", Gold);
            SetLine(1, L"");

            RowTargets.assign(TableRows, -2);
            FillRows(static_cast<int>(Sorted.size()), [&](const int Slot, const int At) {
                const auto& [Key, T] = Sorted[At];
                RowTargets[Slot] = Key;
                const std::wstring Name = Kind == ByRaid ? RaidName(Key)
                                        : Kind == BySp   ? SpName(Key)
                                        : T.Name.empty() ? L"#" + std::to_wstring(Key) : T.Name;
                Grid.Set(Slot, 0, Name);
                Grid.Set(Slot, 1, std::to_wstring(T.Runs));
                Grid.Set(Slot, 2, std::to_wstring(T.Wins));
                Grid.Set(Slot, 3, Percent(T.Wins, T.Runs), T.Wins * 2 >= T.Runs ? Good : Bad);
                if (Kind == ByRaid) {
                    Grid.Set(Slot, 4, T.Best ? Duration(T.Best) : L"-");
                    Grid.Set(Slot, 5, T.Wins ? Duration(T.WonTime / T.Wins) : L"-");
                    Grid.Set(Slot, 6, Compact(T.Damage / T.Runs));
                } else if (Kind == BySp) {
                    Grid.Set(Slot, 4, T.Wins ? Duration(T.WonTime / T.Wins) : L"-");
                    Grid.Set(Slot, 5, Compact(T.Damage / T.Runs));
                    Grid.Set(Slot, 6, Percent(T.Crits, T.Hits));
                } else {
                    Grid.Set(Slot, 4, Compact(T.Damage / T.Runs));
                    Grid.Set(Slot, 5, When(T.LastSeen));
                }
            });
        }

        std::wstring LockLine(const Raid& R) {
            if (R.LockTimes.empty()) return L"No lock data for this raid.";
            std::wstring Line;
            int64_t MonstersDone = 0;
            for (const Locks& L : R.LockTimes) {
                if (L[2] > 0 && L[1] <= 0) {
                    MonstersDone = L[0];
                    break;
                }
            }
            const Locks& First = R.LockTimes.front();
            if (First[2] > 0) {
                Line = L"Monsters " + std::to_wstring(First[2]) + L": " + (MonstersDone ? L"done at " + Duration(MonstersDone) : L"not done");
            }
            std::wstring Levers;
            for (size_t i = 1; i < R.LockTimes.size(); i++) {
                if (R.LockTimes[i][3] < R.LockTimes[i - 1][3]) Levers += (Levers.empty() ? L"" : L", ") + Duration(R.LockTimes[i][0]);
            }
            if (First[4] > 0) {
                Line += (Line.empty() ? L"" : L". ") + std::wstring(L"Levers ") + std::to_wstring(First[4]) + L": "
                      + (Levers.empty() ? L"none pulled" : Levers);
            }
            return Line.empty() ? L"No lock data for this raid." : Line;
        }

        void RefreshRaid() {
            const Raid* R = RaidAt(OpenRaid);
            if (!R) {
                Go(View::List);
                return;
            }
            Grid.SetColumns({{L"Player", 100}, {L"Lv", 62}, {L"Class", 66}, {L"Specialist", 106, 1, IconSize + 4}, {L"Boss damage", 76, 2}, {L"Share", 44, 2},
                             {L"Total damage", 80, 2}, {L"Gold", 64, 2}, {L"Crit", 36, 2}, {L"Miss", 36, 2}, {L"Died", 0, 2}});
            std::wstring Top = RaidName(RaidKey(*R)) + L", " + WhenWithYear(R->Start) + L", "
                + (R->Result == Won ? L"won" : R->Result == Failed ? L"failed" : L"running") + L" in " + Duration(RaidTime(*R));
            if (R->MaxLives > 0) Top += L", lives " + std::to_wstring(std::max(0, R->LivesLeft)) + L"/" + std::to_wstring(R->MaxLives);
            SetLine(0, Top, R->Result == Won ? Good : R->Result == Failed ? Bad : Gold);
            std::wstring Boss = L"No boss seen.";
            if (R->BossVnum) {
                Boss = L"Boss #" + std::to_wstring(R->BossVnum) + L", " + Compact(R->BossMaxHp) + L" HP";
                if (R->BossFirstHitAt) Boss += L", first hit at " + Duration(R->BossFirstHitAt);
                if (R->RewardAt) {
                    Boss += L", reward unlocked at " + Compact(R->RewardLow) + L" - " + Compact(R->RewardDamage) + L" boss damage";
                    if (R->RewardMateDamage) Boss += L" (+" + Compact(R->RewardMateDamage) + L" partner)";
                    Boss += L" at " + Duration(R->RewardAt);
                    if (R->BossMaxHp) Boss += L", boss " + std::to_wstring(R->RewardBossLost * 100 / R->BossMaxHp) + L"% down";
                }
                if (R->BossKilledAt) Boss += L", down at " + Duration(R->BossKilledAt);
                if (BossTime(*R)) Boss += L", fight " + Duration(BossTime(*R));
            }
            if (!R->Maps.empty()) {
                Boss += L". Maps: ";
                for (size_t i = 0; i < R->Maps.size(); i++) Boss += (i ? L", " : L"") + MapLabel(R->Maps[i]);
            }
            SetLine(1, Boss);
            SetLine(2, LockLine(*R));

            std::vector<const Member*> Sorted;
            for (const Member& M : R->Members) Sorted.push_back(&M);
            std::sort(Sorted.begin(), Sorted.end(), [](const Member* A, const Member* B) {
                return A->BossDamage != B->BossDamage ? A->BossDamage > B->BossDamage : A->Damage > B->Damage;
            });
            const int64_t Total = BossTotal(*R);
            RowTargets.assign(TableRows, -2);
            RowNames.assign(TableRows, L"");
            FillRows(static_cast<int>(Sorted.size()), [&](const int Slot, const int At) {
                const Member& M = *Sorted[At];
                RowTargets[Slot] = static_cast<int>(M.Id);
                static const Color Faded(255, 95, 95, 95);
                const bool Left = LeftEarly(*R, M);
                const Color& Tint = Left ? Faded : M.Id == R->Self ? Picked : Plain;
                Grid.Set(Slot, 0, MemberName(M), Tint);
                RowNames[Slot] = M.Name.empty() ? L"" : Wide(M.Name);
                std::wstring Level = M.Level ? std::to_wstring(M.Level) : L"-";
                if (M.HeroLevel > 0) Level += L" (+" + std::to_wstring(M.HeroLevel) + L")";
                Grid.Set(Slot, 1, Level, Tint);
                Grid.Set(Slot, 2, ClassName(M.Class), Tint);
                Grid.Set(Slot, 3, SpName(M.Morph), Tint);
                const GameData::SpCard* Card = GameData::FindSp(M.Morph);
                SetRowIcon(Slot, 3, Card ? Card->Icon : static_cast<int16_t>(-1));
                Grid.Set(Slot, 4, Compact(M.BossDamage), Tint);
                Grid.Set(Slot, 5, Percent(M.BossDamage, Total), Tint);
                Grid.Set(Slot, 6, Compact(M.Damage), Tint);
                Grid.Set(Slot, 7, M.Gold || !M.Picked.empty() ? Compact(M.Gold) : L"-", Tint);
                Grid.Set(Slot, 8, Percent(M.Crits, M.Hits), Tint);
                Grid.Set(Slot, 9, Percent(M.Misses, M.Hits), Tint);
                Grid.Set(Slot, 10, std::to_wstring(M.Deaths), M.Deaths && !Left ? Bad : Tint);
            });
        }

        void RefreshWeekly() {
            long Character = Filters[CharacterFilter] >= 0 ? Filters[CharacterFilter] : SelfId;
            std::wstring Name = L"#" + std::to_wstring(Character);
            for (const Raid& R : History) {
                if (R.Self != Character) continue;
                if (const Member* Me = SelfIn(R); Me && !Me->Name.empty()) Name = Wide(Me->Name);
            }
            const int64_t Now = NowMs();
            const int64_t Week = LastReset(Now, true);
            const int64_t Today = LastReset(Now, false);
            SetLine(0, Name + L":  weekly reset in " + Until(Week + 7 * 86'400'000LL - Now) + L", daily reset in "
                       + Until(Today + 86'400'000LL - Now), Gold);
            std::vector<const Raid*> Runs;
            for (const Raid& R : History) {
                if (R.Self == Character) Runs.push_back(&R);
            }
            if (HasActive && Active.Self == Character) Runs.push_back(&Active);
            RowTargets.assign(TableRows, -2);

            if (WeeklyTab == Weeklies) {
                SetLine(1, L"Hardcore raids, " + std::to_wstring(HardcoreTries) + L" tries a week. The game sends the count after each run.");
                SetLine(2, L"Until the game sends a count this week, a raid shows all its tries.");
                Grid.SetColumns({{L"Hardcore raid", 280, 1, IconSize + 4}, {L"Tries left", 90, 2}, {L"Runs this week", 120, 2}, {L"Last run", 0, 2}});
                const auto& Raids = GameData::HardcoreRaids();
                FillRows(static_cast<int>(Raids.size()), [&](const int Slot, const int At) {
                    const int Id = Raids[At];
                    int Count = 0;
                    int Left = -1;
                    int64_t Last = 0, LeftAt = 0;
                    for (const Raid* R : Runs) {
                        if (R->RaidId != Id || !R->Hardcore) continue;
                        Last = std::max(Last, R->Start);
                        if (R->Start < Week) continue;
                        Count++;
                        if (R->TriesLeft >= 0 && R->Start >= LeftAt) {
                            Left = R->TriesLeft;
                            LeftAt = R->Start;
                        }
                    }
                    if (Left < 0) Left = HardcoreTries;
                    const std::wstring LeftText = std::to_wstring(Left) + L" / " + std::to_wstring(HardcoreTries);
                    Grid.Set(Slot, 0, RaidName(Id * 2 + 1), Count ? Plain : Hint);
                    SetRowIcon(Slot, 0, GameData::RaidIcon(Id, true));
                    Grid.Set(Slot, 1, LeftText, Left == 0 ? Bad : Left < HardcoreTries ? Gold : Plain);
                    Grid.Set(Slot, 2, std::to_wstring(Count), Count ? Plain : Hint);
                    Grid.Set(Slot, 3, Last ? Ago(Last) : L"-", Hint);
                });
            } else {
                SetLine(1, L"Daily raid quests from the game data. Done means you won that raid enough times since the daily reset.");
                SetLine(2, L"Whether you took the quest is not known yet.");
                Grid.SetColumns({{L"Daily quest", 300}, {L"Raid", 240, 1, IconSize + 4}, {L"Today", 0, 2}});
                const auto& Quests = GameData::DailyRaidQuests();
                FillRows(static_cast<int>(Quests.size()), [&](const int Slot, const int At) {
                    const GameData::RaidQuest& Q = Quests[At];
                    int Wins = 0;
                    for (const Raid* R : Runs) {
                        if (R->RaidId == Q.RaidId && R->Result == Won && R->Start >= Today) Wins++;
                    }
                    const bool Done = Wins >= Q.Count;
                    Grid.Set(Slot, 0, Q.Name, Done ? Hint : Plain);
                    Grid.Set(Slot, 1, RaidName(Q.RaidId * 2), Done ? Hint : Plain);
                    SetRowIcon(Slot, 1, GameData::RaidIcon(Q.RaidId, false));
                    Grid.Set(Slot, 2, Done ? L"Done" : std::to_wstring(Wins) + L" / " + std::to_wstring(Q.Count), Done ? Good : Plain);
                });
                if (Quests.empty()) SetLine(2, L"The daily quests could not be read from the game data.", Bad);
            }
        }

        void RefreshPlayer() {
            const Raid* R = RaidAt(OpenRaid);
            const Member* M = nullptr;
            if (R) {
                for (const Member& Each : R->Members) {
                    if (Each.Id == OpenMember) M = &Each;
                }
            }
            if (!M) {
                Go(R ? View::Raid : View::List);
                return;
            }
            Grid.SetColumns({{L"Skill", 200, 1, IconSize + 4}, {L"Damage", 80, 2}, {L"Share", 54, 2}, {L"Hits", 54, 2}, {L"Avg hit", 70, 2},
                             {L"Crits", 54, 2}, {L"Crit %", 54, 2}, {L"Miss", 0, 2}});
            std::wstring Top = MemberName(*M) + L", " + ClassName(M->Class);
            if (M->Level) Top += L", level " + std::to_wstring(M->Level);
            if (M->HeroLevel) Top += L" (+" + std::to_wstring(M->HeroLevel) + L")";
            Top += L", " + SpName(M->Morph);
            SetLine(0, Top, M->Id == R->Self ? Picked : Gold);
            SetLine(1, Compact(M->BossDamage) + L" boss damage (" + Percent(M->BossDamage, BossTotal(*R)) + L"), " + Compact(M->Damage)
                       + L" total damage, " + std::to_wstring(M->Hits) + L" hits, " + Percent(M->Crits, M->Hits) + L" crits, "
                       + Percent(M->Misses, M->Hits) + L" missed, died " + std::to_wstring(M->Deaths) + L"x");
            std::wstring Changes;
            for (const auto& [At, Morph] : M->MorphChanges) {
                Changes += (Changes.empty() ? L"Changes: " : L", ") + SpName(Morph) + L" at " + Duration(At);
            }
            SetLine(2, Changes.empty() ? L"No SP change during the raid." : Changes);

            std::vector<std::pair<int, SkillStat>> Sorted(M->Skills.begin(), M->Skills.end());
            std::sort(Sorted.begin(), Sorted.end(), [](const auto& A, const auto& B) { return A.second.Damage > B.second.Damage; });
            RowTargets.assign(TableRows, -2);
            FillRows(static_cast<int>(Sorted.size()), [&](const int Slot, const int At) {
                const auto& [Skill, S] = Sorted[At];
                const int Landed = S.Hits - S.Misses;
                Grid.Set(Slot, 0, SkillLabel(Skill));
                SetRowIcon(Slot, 0, SkillIcon(Skill));
                Grid.Set(Slot, 1, Compact(S.Damage));
                Grid.Set(Slot, 2, Percent(S.Damage, M->Damage));
                Grid.Set(Slot, 3, std::to_wstring(S.Hits));
                Grid.Set(Slot, 4, Landed > 0 ? Compact(S.Damage / Landed) : L"-");
                Grid.Set(Slot, 5, std::to_wstring(S.Crits));
                Grid.Set(Slot, 6, Percent(S.Crits, S.Hits));
                Grid.Set(Slot, 7, std::to_wstring(S.Misses));
            });
        }

        void Refresh() {
            if (!PageWidget) return;
            if (FilteredFrom != History.size()) FillPickers();
            RefreshSummary();
            HideRowIcons();
            Grid.SetActive(Current != View::List);
            switch (Current) {
                case View::List: RefreshList(); break;
                case View::Stats: RefreshStats(); break;
                case View::Raid: RefreshRaid(); break;
                case View::Player: RefreshPlayer(); break;
                case View::Weekly: RefreshWeekly(); break;
            }
        }

        void SetFilter(const int Which, const int Value) {
            Filters[Which] = Value;
            const auto& Values = PickerValues[Which];
            const auto Found = std::find(Values.begin(), Values.end(), Value);
            if (Pickers[Which]) Pickers[Which]->SetSelected(Found == Values.end() ? 0 : static_cast<int>(Found - Values.begin()));
        }

        void OpenRow(const int Slot) {
            if (Slot < 0 || Slot >= static_cast<int>(RowTargets.size()) || RowTargets[Slot] == -2) return;
            const int Target = RowTargets[Slot];
            if (Current == View::List) {
                OpenRaid = Target;
                Go(View::Raid);
            } else if (Current == View::Raid) {
                OpenMember = Target;
                Go(View::Player);
            } else if (Current == View::Stats) {
                SetFilter(Kind == ByRaid ? RaidFilter : Kind == BySp ? SpFilter : PlayerFilter, Target);
                Go(View::List);
            }
        }

        void Attach(TLBSWidget* Parent, TLBSWidget* Child);

        void CloseMenu() {
            if (Menu) Menu->isVisible = false;
            MenuRaid = -2;
            ConfirmDelete = false;
        }

        void DeleteRaid(const int Index) {
            if (Index < 0 || Index >= static_cast<int>(History.size())) return;
            const Raid Gone = History[Index];
            std::string Text;
            for (const std::string& Line : ReadLines(HistoryFile())) {
                Raid R;
                if (FromJson(Line, R) && R.Start == Gone.Start) continue;
                Text += Line + "\n";
            }
            WriteFile(HistoryFile(), Text);
            if (!Gone.Log.empty()) {
                std::error_code Error;
                std::filesystem::remove(LogDir() / Gone.Log, Error);
                std::filesystem::remove(Packed(LogDir() / Gone.Log), Error);
            }
            History.erase(History.begin() + Index);
            if (OpenRaid == Index) Current = View::List;
            if (OpenRaid > Index) OpenRaid--;
            FilteredFrom = static_cast<size_t>(-1);
            Revision++;
            Dirty = true;
        }

        void __cdecl OnMenu(void* Argument) {
            const int Item = static_cast<int>(reinterpret_cast<intptr_t>(Argument));
            const int Index = MenuRaid;
            const Raid* R = RaidAt(Index);
            if (!R) {
                CloseMenu();
                return;
            }
            if (Item == MenuOpen) {
                CloseMenu();
                OpenRaid = Index;
                Go(View::Raid);
            } else if (Item == MenuCopy) {
                std::wstring Names;
                for (const Member& M : R->Members) {
                    if (M.Name.empty()) continue;
                    if (!Names.empty()) Names += L", ";
                    Names += Wide(M.Name);
                }
                CopyToClipboard(Names);
                CloseMenu();
            } else if (Item == MenuDelete) {
                if (!ConfirmDelete) {
                    ConfirmDelete = true;
                    if (MenuButtons[MenuDelete]) MenuButtons[MenuDelete]->SetCaption(L"Click again to delete");
                    return;
                }
                CloseMenu();
                DeleteRaid(Index);
            }
        }

        void OpenMenu(const int Index, const TickContext& Context) {
            if (!Menu || !PageWidget) return;
            int32_t X = 0, Y = 0;
            for (const TLBSWidget* At = PageWidget; At; At = At->parent) {
                X += At->rect.left;
                Y += At->rect.top;
            }
            const bool CanDelete = Index >= 0;
            const int Count = CanDelete ? MenuCount : MenuDelete;
            const auto Height = static_cast<int16_t>(Count * TEWButtonWidget::RowHeight);
            const auto Left = static_cast<int16_t>(std::min<int32_t>(Context.mouseX - X, PageWidget->rect.right - PageWidget->rect.left - MenuWidth));
            const auto Top = static_cast<int16_t>(Context.mouseY - Y);
            Menu->rect = {Left, Top, static_cast<int16_t>(Left + MenuWidth), static_cast<int16_t>(Top + Height)};
            const wchar_t* Captions[MenuCount] = {L"Open details", L"Copy player names", L"Delete raid"};
            for (int i = 0; i < MenuCount; i++) {
                if (!MenuButtons[i]) continue;
                MenuButtons[i]->SetCaption(Captions[i]);
                MenuButtons[i]->isVisible = i < Count;
            }
            MenuRaid = Index;
            ConfirmDelete = false;
            Menu->isVisible = true;
        }

        void BuildMenu(TLBSWidget* Page) {
            Menu = Widget::Create<TLBSWidget>(Host);
            if (!Menu) return;
            Menu->isVisible = false;
            Attach(Page, Menu);
            for (int i = 0; i < MenuCount; i++) {
                TEWButtonWidget* Button = Widget::Create<TEWButtonWidget>(Host);
                if (!Button) continue;
                Button->rect.left = 0;
                Button->rect.top = static_cast<int16_t>(i * TEWButtonWidget::RowHeight);
                Button->SetWidth(MenuWidth);
                WidgetKit::SetOnClick(Button, &OnMenu, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
                Attach(Menu, Button);
                MenuButtons[i] = Button;
            }
        }

        const Member* RowMember(const int Row) {
            const Raid* R = RaidAt(OpenRaid);
            if (!R || Row < 0 || Row >= static_cast<int>(RowTargets.size())) return nullptr;
            for (const Member& M : R->Members) {
                if (M.Id == RowTargets[Row]) return &M;
            }
            return nullptr;
        }

        std::vector<std::wstring> PickupLines(const Member& M) {
            if (!M.Gold && M.Picked.empty()) return {};
            std::vector<std::wstring> Lines{MemberName(M) + L" picked up:"};
            if (M.Gold) Lines.push_back(L"Gold " + Compact(M.Gold));
            std::vector<std::pair<int, int64_t>> Items(M.Picked.begin(), M.Picked.end());
            std::sort(Items.begin(), Items.end(), [](const auto& A, const auto& B) { return A.second > B.second; });
            constexpr size_t MaxLines = 20;
            for (size_t i = 0; i < Items.size() && i < MaxLines; i++) {
                std::wstring Name = GameData::ItemName(Items[i].first);
                if (Name.empty()) Name = L"Item #" + std::to_wstring(Items[i].first);
                Lines.push_back(Name + L" x" + std::to_wstring(Items[i].second));
            }
            if (Items.size() > MaxLines) Lines.push_back(L"and " + std::to_wstring(Items.size() - MaxLines) + L" more");
            return Lines;
        }

        int CardAt(const TickContext& Context) {
            for (int Slot = 0; Slot < CardCount; Slot++) {
                if (Hovered(Cards[Slot].Hit)) return Slot;
            }
            return -1;
        }

        void __cdecl OnPick(void* Argument, const int Index) {
            const int Which = static_cast<int>(reinterpret_cast<intptr_t>(Argument));
            const auto& Values = PickerValues[Which];
            Filters[Which] = Index >= 0 && Index < static_cast<int>(Values.size()) ? Values[Index] : -1;
            PageIndex = 0;
            if (Current != View::Stats) Current = View::List;
            Go(Current);
        }

        void __cdecl OnView(void* Argument) {
            Go(Argument ? View::Stats : View::List);
        }

        void __cdecl OnKind(void* Argument) {
            Kind = static_cast<int>(reinterpret_cast<intptr_t>(Argument));
            Go(View::Stats);
        }

        void __cdecl OnBack(void*) {
            Go(Current == View::Player ? View::Raid : View::List);
        }

        void __cdecl OnWeekly(void*) {
            Go(View::Weekly);
        }

        void __cdecl OnWeeklyTab(void* Argument) {
            WeeklyTab = static_cast<int>(reinterpret_cast<intptr_t>(Argument));
            Go(View::Weekly);
        }

        void Attach(TLBSWidget* Parent, TLBSWidget* Child) {
            if (!Parent || !Child) return;
            Child->parent = Parent;
            Parent->childrenList->push_back(Child);
        }

        WidgetKit::Tab AddButton(const int16_t X, const int16_t Y, const int16_t Width, const wchar_t* Text,
                                 const WidgetKit::ClickFn OnClick, const intptr_t Argument) {
            WidgetKit::Tab Button = WidgetKit::CreateTab(Host, X, Y, Width, Text, OnClick, reinterpret_cast<void*>(Argument));
            Attach(PageWidget, Button.Button);
            return Button;
        }

        TEWGuageWidget* AddBar(TLBSWidget* Parent, const int16_t X, const int16_t Y, const int16_t Width, const int16_t Height,
                               const Color& Tint, const float Fill = 1.0f) {
            auto* Bar = Widget::Create<TEWGuageWidget>(Host);
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

        Card BuildCard(const int16_t Y) {
            Card C;
            C.Box = Widget::Create<TLBSWidget>(Host);
            if (!C.Box) return C;
            C.Box->rect = {MainX, Y, static_cast<int16_t>(MainX + MainWidth), static_cast<int16_t>(Y + CardHeight)};
            C.Box->isInteractable = false;
            Attach(PageWidget, C.Box);
            const int16_t TextX = 50;
            const auto TextWidth = static_cast<int16_t>(MainWidth - TextX - 6);
            C.Strip = AddRect(Host, C.Box, 0, 2, 4, CardHeight - 8, Good);
            C.Rule = AddRect(Host, C.Box, 0, CardHeight - 3, MainWidth, 1, Color(255, 80, 80, 80));
            C.RaidIcon = WidgetKit::CreateItemIcon(Host, 12, 3, 28, -1);
            if (C.RaidIcon) C.RaidIcon->keepTransparency = true;
            Attach(C.Box, C.RaidIcon);
            C.Icon = WidgetKit::CreateItemIcon(Host, 12, 33, 28, -1);
            Attach(C.Box, C.Icon);
            C.Name = AddLabel(Host, C.Box, TextX, 2, static_cast<int16_t>(TextWidth - 216));
            C.Result = AddLabel(Host, C.Box, static_cast<int16_t>(MainWidth - 214), 2, 90, 2);
            C.When = AddLabel(Host, C.Box, static_cast<int16_t>(MainWidth - 116), 2, 110, 2);
            C.Line2 = AddLabel(Host, C.Box, TextX, 19, TextWidth);
            C.Line3 = AddLabel(Host, C.Box, TextX, 35, static_cast<int16_t>(TextWidth - 230));
            C.Top = AddLabel(Host, C.Box, static_cast<int16_t>(TextX + TextWidth - 226), 35, 226, 2);
            C.Bar = AddBar(C.Box, TextX, 53, TextWidth, 4, Gold, 0.0f);
            C.Box->isInteractable = true;
            C.Hit = AddHitArea(Host, C.Box, 0, 0, MainWidth, CardHeight);
            return C;
        }
    }

    void Startup(const ModHost* ModHost) {
        Host = ModHost;
        LoadHistory();
        LoadActive();
        SelfId = GameData::PlayerId();
        StopPacking = false;
        std::vector<std::filesystem::path> Unpacked;
        std::error_code Error;
        for (const auto& Entry : std::filesystem::directory_iterator(LogDir(), Error)) {
            const auto File = Entry.path();
            if (File.extension() != L".log" || (HasActive && File.filename() == Active.Log)) continue;
            Unpacked.push_back(File);
        }
        PackLater(std::move(Unpacked));
        Packet::SubscribePacket(Host, &OnSu);
        Packet::SubscribePacket(Host, &OnCInfo);
        Packet::SubscribePacket(Host, &OnIn);
    }

    void Shutdown() {
        StopPacking = true;
        if (Packer.joinable()) Packer.join();
        SaveActive();
        LogOut.close();
        ForgetPage();
        Host = nullptr;
    }

    void OnRawPacket(const char* Packet, const bool IsSend) {
        if (!Packet) return;
        std::string Text(Packet);
        while (!Text.empty() && (Text.back() == '\n' || Text.back() == '\r' || Text.back() == ' ')) Text.pop_back();
        if (Text.empty()) return;

        const Logged Entry{NowMs(), IsSend, Text};
        if (!WorthLogging(Text)) {
        } else if (HasActive) {
            WriteLog(Entry);
        } else {
            Recent.push_back(Entry);
            while (!Recent.empty() && (Recent.front().At < Entry.At - RecentMs || Recent.size() > 50000)) Recent.pop_front();
        }
        if (IsSend) return;

        const size_t Space = Text.find(' ');
        std::string Header = Text.substr(0, Space);
        if (Header == "raidf") Header = "raid";
        const bool HardcoreStatus = Header == "raidfhp";
        const auto T = Split(Text, ' ');
        if (Header == "c_map") Drops.clear();
        {
            long Id = 0;
            int Vnum = 0;
            int64_t Amount = 0;
            if (ReadDrop(Header, T, Id, Vnum, Amount)) {
                if (Drops.size() > 20000) Drops.clear();
                Drops[Id] = {Vnum, Amount};
                return;
            }
        }
        if (Header == "get" && T.size() >= 4 && T[1] == "1") {
            const auto Found = Drops.find(static_cast<long>(Num(T[3])));
            if (Found == Drops.end()) return;
            if (HasActive) {
                const long Id = static_cast<long>(Num(T[2]));
                Member* M = FindMember(Id);
                if (!M && std::find(Active.Party.begin(), Active.Party.end(), Id) != Active.Party.end()) M = &MemberFor(Id);
                if (M) {
                    AddPickup(*M, Found->second.first, Found->second.second);
                    Revision++;
                }
            }
            Drops.erase(Found);
            return;
        }
        if (Header == "in" && T.size() >= 5 && T[1] == "1") {
            const long Id = static_cast<long>(Num(T[4]));
            Known[Id].In = Text;
            Learn(Id, Known[Id]);
            return;
        }
        if (Header == "c_mode" && T.size() >= 4 && T[1] == "1") {
            const long Id = static_cast<long>(Num(T[2]));
            const int Morph = static_cast<int>(Num(T[3]));
            if (IsSp(Morph)) Known[Id].Sp = Morph;
            if (Id == SelfId) SelfMorph = Morph;
        }
        const bool Listing = Header.starts_with("rdlst");
        if (Header == "pinit") {
            MyMates.clear();
            for (size_t i = 2; i < T.size(); i++) {
                const auto F = Split(T[i], '|');
                if (F.size() > 1 && F[0] == "2") MyMates.insert(static_cast<long>(Num(F[1])));
            }
            return;
        }
        if (Header == "sayi") {
            if (int Left = 0; HasActive && Active.Hardcore && ReadTriesLeft(T, Active.Self, Left)) {
                Active.TriesLeft = Left;
                Revision++;
            }
            if (HasActive && !Active.RewardAt && IsRewardMessage(T, Active.Self)) {
                Active.RewardAt = NowMs() - Active.Start;
                const Member* Me = FindMember(Active.Self);
                Active.RewardDamage = Active.RewardLow = Me ? Me->BossDamage : 0;
                Active.RewardMateDamage = Active.MateBossDamage;
                Active.RewardPlayers = static_cast<int>(InStatus.size());
                Active.RewardBossLost = Active.BossTaken;
                RewardPending = true;
                Revision++;
            }
            return;
        }
        if (Header != "raidbf" && Header != "raid" && !HardcoreStatus && !Listing && Header != "raidmbf" && Header != "rboss"
            && Header != "c_map" && Header != "c_mode") return;

        if (Header == "raidbf" && T.size() >= 4) {
            const int Step = static_cast<int>(Num(T[2]));
            if (Step == 0) Begin(static_cast<int>(Num(T[3])));
            if (Step == 1 && HasActive) {
                Active.Result = Won;
                Active.WonAt = NowMs() - Active.Start;
                if (Active.BossId && !Active.BossKilledAt) Active.BossKilledAt = Active.WonAt;
                Revision++;
            } else if (Step != 0 && HasActive) {
                Active.Result = Failed;
                if (!Active.FailedAt) Active.FailedAt = NowMs() - Active.Start;
                Revision++;
            }
            return;
        }
        if (Listing) {
            LastListing = Text;
            ApplyListing(Text);
            if (HasActive) AddListingParty(Active, Text);
            if (HasActive) Revision++;
            return;
        }
        if (Header == "raid" && T.size() >= 3 && T[1] == "2" && T[2] == "-1") LastListing.clear();
        if (!HasActive) return;
        if (Header == "raid" || HardcoreStatus) {
            if (T.size() >= 2 && T[1] == "3") {
                for (size_t i = 2; i < T.size(); i++) {
                    const auto Parts = Split(T[i], '.');
                    if (Parts.size() >= 1 && Num(Parts[0])) MemberFor(static_cast<long>(Num(Parts[0])));
                }
                AddStatusParty(Active, T);
                NoteStatus(Active, InStatus, T, NowMs() - Active.Start);
                return;
            }
            if (Header == "raid" && T.size() >= 3 && T[1] == "2" && T[2] == "-1") Finish(Failed);
            return;
        }
        if (Header == "raidmbf" && T.size() >= 7) {
            Active.LivesLeft = static_cast<int>(Num(T[5]));
            Active.MaxLives = static_cast<int>(Num(T[6]));
            const Locks Now{NowMs() - Active.Start, Num(T[2]), Num(T[1]), Num(T[4]), Num(T[3])};
            const Locks* Last = Active.LockTimes.empty() ? nullptr : &Active.LockTimes.back();
            if (!Last || (*Last)[1] != Now[1] || (*Last)[2] != Now[2] || (*Last)[3] != Now[3] || (*Last)[4] != Now[4]) {
                Active.LockTimes.push_back(Now);
            }
        } else if (Header == "rboss" && T.size() >= 6 && T[1] == "3") {
            Active.BossId = static_cast<long>(Num(T[2]));
            Active.BossMaxHp = std::max(Active.BossMaxHp, Num(T[4]));
            Active.BossVnum = static_cast<int>(Num(T[5]));
            if (Num(T[3]) > 0 && !Active.BossSeenAt) Active.BossSeenAt = NowMs() - Active.Start;
            if (Num(T[3]) <= 0 && !Active.BossKilledAt) Active.BossKilledAt = NowMs() - Active.Start;
        } else if (Header == "c_map" && T.size() >= 3) {
            if (Active.WonAt || Active.FailedAt) {
                Finish(Active.Result);
                return;
            }
            Active.Maps.push_back(static_cast<int>(Num(T[2])));
        } else if (Header == "c_mode" && T.size() >= 4 && T[1] == "1") {
            if (Member* M = FindMember(static_cast<long>(Num(T[2])))) {
                const int Morph = static_cast<int>(Num(T[3]));
                const int Last = M->MorphChanges.empty() ? M->Morph : M->MorphChanges.back().second;
                if (Morph != Last) M->MorphChanges.emplace_back(NowMs() - Active.Start, Morph);
                if (IsSp(Morph)) M->Morph = Morph;
            }
        }
        Revision++;
    }

    int16_t BuildPage(TLBSWidget* Page, const int16_t Width) {
        PageWidget = Page;
        MainWidth = static_cast<int16_t>(Width - MainX - 12);
        FullWidth = static_cast<int16_t>(Width - 2 * SideX);
        LineWidth = MainWidth;
        Side.clear();

        Heading = AddLabel(Host, Page, MainX, Top + 4, MainWidth);
        BackButton = AddButton(MainX, Top, 80, L"Back", &OnBack, 0);
        const wchar_t* KindNames[] = {L"By raid", L"By specialist", L"Players"};
        const int16_t KindWidths[] = {90, 110, 90};
        int16_t X = MainX;
        for (int i = 0; i < 3; i++) {
            KindButtons[i] = AddButton(X, Top, KindWidths[i], KindNames[i], &OnKind, i);
            X = static_cast<int16_t>(X + KindWidths[i] + 6);
        }
        int16_t Y = Top + 32;
        for (TEWLabel*& Line : Lines) {
            Line = AddLabel(Host, Page, MainX, Y, MainWidth);
            Y += 16;
        }
        Y += 6;
        Grid.Build(Host, Page, MainX, Y, MainWidth, TableRows);
        for (auto& Icon : RowIcons) {
            Icon = WidgetKit::CreateItemIcon(Host, 0, 0, IconSize, -1);
            Attach(Page, Icon);
        }
        for (int Slot = 0; Slot < CardCount; Slot++) Cards[Slot] = BuildCard(static_cast<int16_t>(Top + 26 + Slot * CardHeight));
        const auto Bottom = static_cast<int16_t>(std::max<int>(Grid.Bottom(), Top + 26 + CardCount * CardHeight));
        Pages.Build(Host, Page, static_cast<int16_t>(MainX + MainWidth / 2), static_cast<int16_t>(Bottom + 6));

        Side.push_back(AddRect(Host, Page, static_cast<int16_t>(SideX + SideWidth + 6), Top, 1, static_cast<int16_t>(Bottom - Top + 20),
                              Color(255, 80, 80, 80)));
        const wchar_t* FilterNames[FilterCount] = {L"Character", L"Raid", L"Specialist", L"Played with", L"Result"};
        Y = Top;
        int16_t PickerY[FilterCount];
        for (int i = 0; i < FilterCount; i++) {
            TEWLabel* Name = AddLabel(Host, Page, SideX, Y, SideWidth);
            SetText(Name, FilterNames[i], Hint, SideWidth);
            Side.push_back(Name);
            PickerY[i] = static_cast<int16_t>(Y + 16);
            Y += 40;
        }
        ListButton = AddButton(SideX, Y, 77, L"Raids", &OnView, 0);
        StatsButton = AddButton(SideX + 83, Y, 77, L"Stats", &OnView, 1);
        Side.push_back(ListButton.Button);
        Side.push_back(StatsButton.Button);
        Y += 36;
        for (TEWLabel*& Line : Summary) {
            Line = AddLabel(Host, Page, SideX, Y, SideWidth);
            Side.push_back(Line);
            Y += 16;
        }
        WeeklyButton = AddButton(SideX, static_cast<int16_t>(Y + 2), SideWidth, L"Dailies & weeklies", &OnWeekly, 0);
        Side.push_back(WeeklyButton.Button);
        WeeklyTabs[0] = AddButton(static_cast<int16_t>(SideX + 90), Top, 100, L"Weeklies", &OnWeeklyTab, Weeklies);
        WeeklyTabs[1] = AddButton(static_cast<int16_t>(SideX + 196), Top, 100, L"Dailies", &OnWeeklyTab, Dailies);

        const wchar_t* Placeholders[FilterCount] = {L"This character", L"All raids", L"All specialists", L"Anyone", L"Any result"};
        for (int i = FilterCount - 1; i >= 0; i--) {
            WidgetKit::Dropdown::Desc Desc;
            Desc.X = SideX;
            Desc.Y = PickerY[i];
            Desc.Width = SideWidth - 17;
            Desc.Placeholder = Placeholders[i];
            Desc.Filterable = i == PlayerFilter;
            Desc.OnPick = &OnPick;
            Desc.Argument = reinterpret_cast<void*>(static_cast<intptr_t>(i));
            Pickers[i] = WidgetKit::Dropdown::Create(Host, Desc);
            if (Pickers[i]) {
                Attach(Page, Pickers[i]->Widget());
                Side.push_back(Pickers[i]->Widget());
            }
        }
        BuildMenu(Page);
        FilteredFrom = static_cast<size_t>(-1);
        ShowControls();
        Dirty = true;
        return static_cast<int16_t>(Bottom + 30);
    }

    void ForgetPage() {
        PageWidget = nullptr;
        Heading = nullptr;
        for (auto& Line : Lines) Line = nullptr;
        for (auto& Line : Summary) Line = nullptr;
        for (auto& C : Cards) C = {};
        for (auto& Icon : RowIcons) Icon = nullptr;
        Menu = nullptr;
        for (auto& Button : MenuButtons) Button = nullptr;
        MenuRaid = -2;
        Grid.Forget();
        Pages.Forget();
        ListButton = StatsButton = BackButton = WeeklyButton = {};
        for (auto& Button : WeeklyTabs) Button = {};
        for (auto& Button : KindButtons) Button = {};
        for (auto& Picker : Pickers) Picker = nullptr;
        Side.clear();
    }

    void ResetView() {
        if (Current == View::Raid || Current == View::Player || Current == View::Weekly) Go(View::List);
        CloseMenu();
    }

    void TickPage(TLBSWidget* Root, const TickContext& Context) {
        if (!PageWidget) return;
        if (const long Id = GameData::PlayerId(); Id && Id != SelfId) {
            SelfId = Id;
            Revision++;
        }
        for (WidgetKit::Dropdown* Picker : Pickers) {
            if (Picker) Picker->Tick(Context);
        }
        const bool PickerOpen = AnyPickerOpen();
        const bool Blocked = PickerOpen;
        const bool OverList = Current == View::List ? CardAt(Context) >= 0 : Grid.Contains(Context);
        if (Context.mouseWheel != 0 && !Blocked && OverList) {
            PageIndex += Context.mouseWheel > 0 ? -1 : 1;
            Dirty = true;
        }
        const int Card = !Blocked && Current == View::List ? CardAt(Context) : -1;
        const int Row = !Blocked && Current == View::Raid ? Grid.RowAt(Context) : -1;
        const bool OverName = Row >= 0 && Row < static_cast<int>(RowNames.size()) && !RowNames[Row].empty() && Grid.OverCell(Row, 0, Context);
        if (!OverName || Row != CopiedRow) CopiedRow = -1;
        std::vector<std::wstring> HintLines;
        uint32_t HintTitle = 0xFFFFFFFF;
        if (Card >= 0 && Cards[Card].When && Over(Cards[Card].When, 110, 16, Context)) {
            if (const Raid* R = RaidAt(RowTargets[Card])) HintLines = {WhenWithYear(R->Start)};
        } else if (OverName) {
            if (const Member* M = RowMember(Row); M && LeftEarly(*RaidAt(OpenRaid), *M)) {
                HintLines.push_back(L"Left the raid at " + Duration(M->LeftAt));
            }
            HintLines.push_back(Row == CopiedRow ? L"Copied!" : L"Alt+click to copy name");
        } else if (Row >= 0 && Grid.OverCell(Row, 7, Context)) {
            if (const Member* M = RowMember(Row)) HintLines = PickupLines(*M);
            HintTitle = 0xFFFFD65C;
        }
        if (HintLines.empty()) GameHint::Hide();
        else if (GameHint::Create(Host, Root)) GameHint::Show(HintLines, Context.mouseX, Context.mouseY, HintTitle);

        const bool MenuShown = Menu && Menu->isVisible;
        bool OverMenu = false;
        for (const TEWButtonWidget* Button : MenuButtons) {
            if (MenuShown && Button && Button->isVisible && Button->hovered) OverMenu = true;
        }
        if (MenuShown && (Current != View::List || (GetAsyncKeyState(VK_ESCAPE) & 0x8000))) CloseMenu();
        if (RightClick.Pressed() && !Blocked) {
            if (Card >= 0 && RowTargets[Card] != -2) OpenMenu(RowTargets[Card], Context);
            else CloseMenu();
        }
        const bool Pressed = Click.Pressed();
        if (MenuShown) {
            if (Pressed && !OverMenu) CloseMenu();
        } else if (Pressed && !Blocked) {
            if (const int Step = Pages.Clicked(Context)) {
                PageIndex += Step;
                Dirty = true;
            } else if (OverName && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
                CopyToClipboard(RowNames[Row]);
                CopiedRow = Row;
            } else {
                OpenRow(Current == View::List ? Card : Grid.RowAt(Context));
            }
        }
        const int64_t Now = NowMs();
        const bool ShowsActive = HasActive && (Current == View::List || OpenRaid == -1);
        const bool Live = ShowsActive && Now - LastLiveRefresh > 1000;
        if (!Dirty && Revision == ShownRevision && !Live) return;
        if (Live) LastLiveRefresh = Now;
        Dirty = false;
        ShownRevision = Revision;
        Refresh();
    }
}
