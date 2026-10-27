/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RandomPlayerbotMgr.h"
#include "PlayerbotsDatabase.h"
#include "AiFactory.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "Cell.h"
#include "CellImpl.h"
#include "ChannelMgr.h"
#include "CompanyStanding.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "DatabaseEnv.h"
#include "Define.h"
#include "FleeManager.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LFGMgr.h"
#include "MapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "ObjectGuid.h"
#include "PerfMonitor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"

// ---- wov: play schedules -------------------------------------------------------------------------
// Every random bot is "played" by a person with a schedule (table wov_player, written by
// wov/gen_players.py). A bot only logs in while its player would be online. Reference implementation
// and simulator: wov/wov_schedule.py -- keep the two in sync.
#include "Config.h"
#include <unordered_map>
#include "CharacterCache.h"
namespace
{
struct WovSched { uint8 kind; uint8 startHour; uint16 mins; uint8 drive; };
std::unordered_map<uint32, WovSched> g_wovSched;
time_t g_wovSchedLoaded = 0;
bool   g_wovSchedEnable = false;
uint32 g_wovSessionPct = 100;
uint32 g_wovMinOnline = 0;
uint32 g_wovLogoutsPerMinute = 10;

uint64 WovH(uint64 a, uint64 b)
{
    uint64 x = a * 0x9E3779B97F4A7C15ULL + b * 0xBF58476D1CE4E5B9ULL + 0x94D049BB133111EBULL;
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 29;
    return x;
}

int32 WovTzOffsetHours(uint32 guid)
{
    uint64 b = WovH(guid, 7) % 100;
    if (b < 45) return 0;
    if (b < 60) return -1;
    if (b < 82) return -6;
    if (b < 94) return -9;
    return 8;
}

uint8 WovKind(std::string const& d)
{
    static char const* names[] = {"almost every day", "most evenings", "late at night", "irregularly",
                                  "weekends", "on weekday mornings", "in short sessions after work"};
    for (uint8 i = 0; i < 7; ++i)
        if (d == names[i])
            return i;
    return 7;
}

uint32 WovPlayProb(uint8 kind, int64 weekday)  // weekday 0=Sun .. 6=Sat
{
    bool we = weekday == 0 || weekday == 6;
    switch (kind)
    {
        case 0: return 90;
        case 1: return 70;
        case 2: return 60;
        case 3: return 40;
        case 4: return we ? 90 : (weekday == 5 ? 30 : 10);
        case 5: return we ? 20 : 80;
        case 6: return we ? 30 : 80;
        default: return 50;
    }
}

void WovLoadBands();

void WovLoadSchedules()
{
    time_t now = time(nullptr);
    if (g_wovSchedLoaded && now < g_wovSchedLoaded + 600)
        return;
    g_wovSchedLoaded = now;
    g_wovSchedEnable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.Schedule.Enable", false);
    g_wovSessionPct = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Schedule.SessionPercent", 100);
    g_wovMinOnline = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Schedule.MinOnline", 0);
    g_wovLogoutsPerMinute = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Schedule.LogoutsPerMinute", 10);
    WovLoadBands();
    if (!g_wovSchedEnable)
        return;
    QueryResult r = CharacterDatabase.Query(
        "SELECT guid, play_days, play_start_hour, session_minutes, drive FROM wov_player");
    g_wovSched.clear();
    if (r)
    {
        do
        {
            Field* f = r->Fetch();
            std::string const d = f[4].Get<std::string>();
            uint8 drive = d == "pvp" ? 1 : d == "dungeon" ? 2 : d == "gold" ? 3 : d == "level" ? 4 : d == "social" ? 5 : d == "explore" ? 6 : 0;
            g_wovSched[f[0].Get<uint32>()] = {WovKind(f[1].Get<std::string>()), f[2].Get<uint8>(),
                                              f[3].Get<uint16>(), drive};
        } while (r->NextRow());
    }
    LOG_INFO("playerbots", "WoV schedules: {} players loaded, session {}%, min online {}", g_wovSched.size(),
             g_wovSessionPct, g_wovMinOnline);
}

}  // namespace
bool WovRaidNightFor(uint32 guid);  // wov: raid nights (below)
namespace
{
bool WovScheduledOnline(uint32 guid)
{
    if (WovRaidNightFor(guid))  // raid night: the raiders come online for it and stay until it is over
        return true;
    auto it = g_wovSched.find(guid);
    if (it == g_wovSched.end())
        return false;
    WovSched const& w = it->second;
    time_t now = time(nullptr);
    tm lt{};
    localtime_r(&now, &lt);
    int64 sec = int64(now) + int64(lt.tm_gmtoff) + int64(WovTzOffsetHours(guid)) * 3600;
    int64 day = sec / 86400;
    int64 minute = (sec % 86400) / 60;
    for (int64 dd = day; dd >= day - 1; --dd)
    {
        int64 wd = (dd + 4) % 7;
        if (WovH(guid, uint64(dd * 8 + 1)) % 100 >= WovPlayProb(w.kind, wd))
            continue;
        int64 st = int64(w.startHour) * 60 + int64(WovH(guid, uint64(dd * 8 + 2)) % 81) - 40;
        int64 ln = int64(w.mins) * (70 + int64(WovH(guid, uint64(dd * 8 + 3)) % 71)) / 100;
        ln = ln * g_wovSessionPct / 100;
        int64 m = minute + (day - dd) * 1440;
        if (st <= m && m < st + ln)
            return true;
    }
    return false;
}

// wov: level bands. Levels are never changed by the server; the mix of who is ONLINE is steered instead,
// by which characters log in to keep the floor and which end their session. Bands: 1-10, 11-20, 21-30,
// 31-40, 41-50, 51-59, 60. Shares from AiPlayerbot.WoV.Bands ("12,12,12,12,12,12,28"); empty = off.
uint32 g_wovBandPct[7] = {0, 0, 0, 0, 0, 0, 0};
bool g_wovBands = false;

uint8 WovBandOf(uint32 level)
{
    if (level >= 60)
        return 6;
    if (level >= 51)
        return 5;
    return uint8((std::max<uint32>(level, 1) - 1) / 10);
}

void WovLoadBands()
{
    std::string v = sConfigMgr->GetOption<std::string>("AiPlayerbot.WoV.Bands", "");
    uint32 vals[7] = {0, 0, 0, 0, 0, 0, 0}, n = 0, sum = 0;
    std::string cur;
    for (char ch : v + ",")
    {
        if (ch == ',')
        {
            if (!cur.empty() && n < 7)
            {
                vals[n] = uint32(std::strtoul(cur.c_str(), nullptr, 10));
                sum += vals[n++];
            }
            cur.clear();
        }
        else if (std::isdigit(static_cast<unsigned char>(ch)))
            cur += ch;
    }
    g_wovBands = n == 7 && sum > 0;
    for (uint32 i = 0; i < 7; ++i)
        g_wovBandPct[i] = g_wovBands ? vals[i] * 100 / sum : 0;
}

uint8 WovBandOfGuid(uint32 guid)
{
    if (Player* p = ObjectAccessor::FindPlayerByLowGUID(guid))
        return WovBandOf(p->GetLevel());
    return WovBandOf(sCharacterCache->GetCharacterLevelByGuid(ObjectGuid::Create<HighGuid::Player>(guid)));
}

// online count per band for the given set of bot guids
template <class Set>
void WovBandCounts(Set const& bots, uint32 (&out)[7])
{
    for (uint32& o : out)
        o = 0;
    for (uint32 g : bots)
        ++out[WovBandOfGuid(g)];
}

bool WovLogoutBudget()
{
    static time_t window = 0;
    static uint32 used = 0;
    time_t now = time(nullptr);
    if (now >= window + 60)
    {
        window = now;
        used = 0;
    }
    if (used >= g_wovLogoutsPerMinute)
        return false;
    ++used;
    return true;
}
}  // namespace
// ---- end wov ---------------------------------------------------------------------------------------

// wov: what the player behind a bot wants to do (wov_player.drive, set by wov/drives.py)
// 1 pvp, 2 dungeon, 3 gold, 4 level, 5 social, 6 explore, 0 unknown
uint8 WovDriveOf(uint32 guidLow)
{
    auto it = g_wovSched.find(guidLow);
    return it == g_wovSched.end() ? 0 : it->second.drive;
}

// wov: Classic-like gold for a bot of this level (about level^3 / 500 gold, so ~2 g at 10, 54 g at 30, 430 g at 60),
// spread per bot; savers (drive gold) keep more. Used for new bots and the one-time trim (wov_wealth).
uint64 WovWealthTarget(Player* bot)
{
    uint64 level = bot ? bot->GetLevel() : 1;
    uint64 copper = level * level * level * 20;
    uint32 low = bot ? bot->GetGUID().GetCounter() : 0;
    uint32 spread = 40 + (low * 2654435761u >> 16) % 121;  // 40-160 %, fixed per bot
    copper = copper * spread / 100;
    if (WovDriveOf(low) == 3)
        copper = copper * 8 / 5;
    return std::max<uint64>(copper, 500);
}

// Percent multiplier on RpgStatusProbWeight (NewRpgBaseAction::RandomChangeStatus).
uint32 WovRpgWeightPct(Player* bot, int status)
{
    if (!g_wovSchedEnable || !bot)
        return 100;
    switch (WovDriveOf(bot->GetGUID().GetCounter()))
    {
        case 1: return status == RPG_OUTDOOR_PVP ? 300 : status == RPG_TRAVEL_FLIGHT ? 150 : 100;
        case 2: return status == RPG_DO_QUEST ? 120 : status == RPG_GO_CAMP ? 150 : 100;
        case 3: return status == RPG_GO_GRIND ? 300 : status == RPG_WANDER_NPC ? 150 : status == RPG_DO_QUEST ? 70 : 100;
        case 4: return status == RPG_DO_QUEST ? 160 : (status == RPG_REST || status == RPG_WANDER_NPC) ? 60 : 100;
        case 5: return (status == RPG_WANDER_NPC || status == RPG_GO_CAMP) ? 220 : status == RPG_REST ? 200 : status == RPG_DO_QUEST ? 70 : 100;
        case 6: return status == RPG_TRAVEL_FLIGHT ? 350 : status == RPG_WANDER_RANDOM ? 200 : 100;
        default: return 100;
    }
}

bool WovBotReserved(uint32 low);  // wov: below (dungeon runs, raid nights)

// ---- wov: Call to Arms (world PvP) -------------------------------------------------------------------
// A PvP kill in the open world can grow into a skirmish: the side that is losing people calls for help in
// General/LocalDefense, bots of the right level fly in on flight paths and the other side answers in kind.
// In the evening bots also organise raids on enemy towns. See wov/HANDOVER.md.
#include "Playerbots.h"
#include "TravelNode.h"
#include "WorldSessionMgr.h"
#include "WorldPacket.h"
#include <deque>
#include <mutex>
namespace
{
struct WovHotspot
{
    char const* name;
    uint32 fmEntry;
    TeamId owner;
    uint8 minL;
    uint8 maxL;
    uint32 map = 0;
    float x = 0, y = 0, z = 0;
    bool loaded = false;
};
std::vector<WovHotspot> g_wovHot = {
    {"Southshore", 2432, TEAM_ALLIANCE, 20, 34},  {"Tarren Mill", 2389, TEAM_HORDE, 20, 34},
    {"the Crossroads", 3615, TEAM_HORDE, 12, 25}, {"Astranaar", 4267, TEAM_ALLIANCE, 18, 30},
    {"Splintertree", 12616, TEAM_HORDE, 20, 32},  {"Lakeshire", 931, TEAM_ALLIANCE, 15, 27},
    {"Darkshire", 2409, TEAM_ALLIANCE, 20, 32},   {"Grom'gol", 1387, TEAM_HORDE, 30, 45},
    {"Refuge Pointe", 2835, TEAM_ALLIANCE, 30, 42}, {"Hammerfall", 2851, TEAM_HORDE, 30, 42},
    {"the Sepulcher", 2226, TEAM_HORDE, 10, 22}};

struct WovKill
{
    time_t t = 0;
    uint32 map = 0;
    float x = 0, y = 0, z = 0;
    TeamId killerTeam = TEAM_NEUTRAL;
    bool real = false;
    uint8 victimLevel = 1;
    uint32 killerLow = 0, victimLow = 0;
    bool killerBot = false, victimBot = false;
    std::string place;
};

struct WovSkirmish
{
    uint32 id = 0;
    uint32 map = 0;
    float x = 0, y = 0, z = 0;
    std::string place;
    uint8 minL = 1, maxL = 60;
    time_t start = 0, lastKill = 0, lastWave = 0;
    bool raid = false;
    TeamId raidTeam = TEAM_NEUTRAL;
    TeamId owner = TEAM_NEUTRAL;
    uint32 raidTarget = 0;
    bool announced = false;
    bool cried[2] = {false, false};
    std::set<uint32> members[2];
    std::set<uint32> invitedReal;
    std::set<uint32> realLows;   // real players fighting here
    std::set<uint32> arrived;    // members that reached the fight
    uint32 kills[2] = {0, 0};    // kills made by each side
    uint32 arriveSaid[2] = {0, 0};
    std::map<uint32, time_t> joined;  // when a recruit was sent
};

std::mutex g_wovWarMutex;
std::deque<WovKill> g_wovKillQueue;
std::deque<WovKill> g_wovRecent;
std::vector<WovSkirmish> g_wovSk;
uint32 g_wovSkSeq = 0;
std::unordered_map<uint32, uint32> g_wovBotSk;
struct WovCool { uint32 map; float x, y; time_t until; };
std::vector<WovCool> g_wovCool;  // places where a fight just ended: no new one there for a while
// Last resort against graveyard camping: a bot killed 4 times by players within 15 minutes gives up like a player would -- once it is
// alive again it hearths home and stays out of Call to Arms for half an hour.
std::unordered_map<uint32, std::vector<time_t>> g_wovPvpDeaths;
std::unordered_set<uint32> g_wovHearth;
std::unordered_map<uint32, time_t> g_wovNoRecruit;
// No fights next to a graveyard: the dead would come back right into it (graveyard camping).
float const kWovGyClear = 250.0f;
bool WovNearGraveyard(uint32 map, float x, float y)
{
    static std::vector<std::tuple<uint32, float, float>> gys;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (QueryResult r = WorldDatabase.Query("SELECT Map, x, y FROM game_graveyard WHERE Map IN (0, 1)"))
            do
                gys.emplace_back(r->Fetch()[0].Get<uint32>(), r->Fetch()[1].Get<float>(), r->Fetch()[2].Get<float>());
            while (r->NextRow());
    }
    for (auto const& [m, gx, gy] : gys)
        if (m == map && std::hypot(gx - x, gy - y) < kWovGyClear)
            return true;
    return false;
}

bool g_wovWarEnable = false;
uint32 g_wovWarMaxPerSide = 30, g_wovWarMaxActive = 2, g_wovWarSparkKills = 2, g_wovWarSparkWindow = 300,
       g_wovWarIdleEnd = 420, g_wovWarMaxMinutes = 40, g_wovWarRaidChance = 17, g_wovWarRaidFrom = 17,
       g_wovWarRaidTo = 24, g_wovWarAccept = 55;
time_t g_wovWarCfgT = 0;

char const* const kWovCry[] = {"need help at {place}!! {n} {enemy} here", "{enemy} raiding {place}, anyone around??",
                               "inc {place}, need ppl", "{place} getting camped by {enemy}, help pls",
                               "any {us} near {place}? {enemy} ganking", "{n} {enemy} at {place}, come help",
                               "help at {place} pls, {enemy} everywhere", "{enemy} at {place}!!",
                               "need ppl at {place} asap, {enemy} camping"};
char const* const kWovOmw[] = {"omw", "otw", "on my way", "omw 2 min", "coming", "flying in", "be there soon", "omw!"};
char const* const kWovInvite[] = {"{place} getting hit by {enemy}, u coming?", "yo, pvp at {place}, come",
                                  "we need ppl at {place}, {enemy} raiding", "wanna kill some {enemy}? {place}",
                                  "{enemy} at {place}, u around?"};
char const* const kWovRaidCall[] = {"raiding {place}, who's in?", "lets go hit {place}", "{place} raid, come",
                                    "anyone up for raiding {place}?", "going to {place} to kill some {enemy}, join"};
char const* const kWovArrive[] = {"here, where are they?", "im here", "made it, where they at?", "here, lets kill em",
                                  "landed, where?", "ok here", "who needs help?"};
char const* const kWovEnd[] = {"gg", "lol they ran", "ez", "gg wp", "that was fun", "they'll be back",
                               "gg, that was nuts", "ffs i died like 5 times", "good fight", "lol gg"};

template <size_t N>
char const* WovPick(char const* const (&a)[N])
{
    return a[urand(0, N - 1)];
}

TeamId WovOther(TeamId t) { return t == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE; }

std::string WovFill(std::string s, std::string const& place, uint32 n, TeamId enemy)
{
    auto rep = [&s](std::string const& k, std::string const& v)
    {
        for (size_t p = s.find(k); p != std::string::npos; p = s.find(k, p + v.size()))
            s.replace(p, k.size(), v);
    };
    rep("{place}", place);
    rep("{n}", std::to_string(std::max<uint32>(n, 2)));
    rep("{enemy}", enemy == TEAM_HORDE ? "hordes" : "allys");
    rep("{us}", enemy == TEAM_HORDE ? "ally" : "horde");
    return s;
}

void WovWarConfig(time_t now)
{
    if (g_wovWarCfgT && now < g_wovWarCfgT + 60)
        return;
    g_wovWarCfgT = now;
    g_wovWarEnable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.War.Enable", false);
    g_wovWarMaxPerSide = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.MaxPerSide", 30);
    g_wovWarMaxActive = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.MaxActive", 2);
    g_wovWarSparkKills = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.SparkKills", 2);
    g_wovWarSparkWindow = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.SparkWindow", 300);
    g_wovWarIdleEnd = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.IdleEnd", 420);
    g_wovWarMaxMinutes = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.MaxMinutes", 40);
    g_wovWarRaidChance = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.RaidChance", 17);
    g_wovWarRaidFrom = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.RaidFromHour", 17);
    g_wovWarRaidTo = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.RaidToHour", 24);
    g_wovWarAccept = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.War.AcceptChance", 55);
}

void WovHotLoad()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    for (WovHotspot& h : g_wovHot)
    {
        QueryResult r = WorldDatabase.Query("SELECT map, position_x, position_y, position_z FROM creature WHERE id = {} LIMIT 1",
                                            h.fmEntry);
        if (!r)
            continue;
        Field* f = r->Fetch();
        h.map = f[0].Get<uint32>();
        h.x = f[1].Get<float>();
        h.y = f[2].Get<float>();
        h.z = f[3].Get<float>();
        h.loaded = true;
    }
}

bool WovOpenWorld(Player* p)
{
    Map* m = p ? p->GetMap() : nullptr;
    return m && !m->Instanceable() && (p->GetMapId() == 0 || p->GetMapId() == 1);
}

float WovDist(Player* p, WovSkirmish const& s)
{
    if (!p || p->GetMapId() != s.map)
        return 1.0e9f;
    return p->GetExactDist(s.x, s.y, s.z);
}

void WovSend(Player* bot, WovSkirmish const& s)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai || bot->IsBeingTeleported())
        return;
    WorldPosition dest(s.map, s.x, s.y, s.z);
    if (bot->GetMapId() == s.map && bot->GetExactDist(s.x, s.y, s.z) < 450.0f)
    {
        ai->rpgInfo.ChangeToGoGrind(dest);
        return;
    }
    if (TravelMgr::FlightMasterInfo const* fm = bot->GetMapId() == s.map ? sTravelMgr.GetNearestFlightMasterInfo(bot) : nullptr)
    {
        uint32 to = sObjectMgr->GetNearestTaxiNode(s.x, s.y, s.z, s.map, bot->GetTeamId());
        if (fm->taxiNodeId && to && fm->taxiNodeId != to)
        {
            std::vector<uint32> path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, to);
            if (!path.empty())
            {
                ai->rpgInfo.ChangeToTravelFlight(fm->templateEntry, fm->pos, path);
                return;
            }
        }
        if (fm->taxiNodeId && fm->taxiNodeId == to)
        {
            ai->rpgInfo.ChangeToGoGrind(dest);  // our own flight master is the closest one: just ride there
            return;
        }
    }
    // no usable flight path: put the bot at its faction's flight master next to the fight, as if it had flown in
    uint32 to2 = sObjectMgr->GetNearestTaxiNode(s.x, s.y, s.z, s.map, bot->GetTeamId());
    if (TaxiNodesEntry const* n = to2 ? sTaxiNodesStore.LookupEntry(to2) : nullptr)
        if (n->map_id == s.map && (bot->GetMapId() != s.map || bot->GetExactDist(n->x, n->y, n->z) > 450.0f))
        {
            LOG_INFO("playerbots", "WoV war #{}: {} has no flight path, placed at flight master", s.id, bot->GetName());
            bot->TeleportTo(n->map_id, n->x + frand(-8.0f, 8.0f), n->y + frand(-8.0f, 8.0f), n->z + 0.5f, bot->GetOrientation());
        }
    if (bot->GetMapId() == s.map)
        ai->rpgInfo.ChangeToGoGrind(dest);
}

// A bot on an errand that has stood still for a while is stuck (unreachable flight master, broken path). If no
// real player is near either end, it is put where it was heading, as if it had walked there.
std::unordered_map<uint32, std::tuple<float, float, time_t>> g_wovStill;

bool WovStuck(Player* bot, time_t now, uint32 secs)
{
    auto& [x, y, t] = g_wovStill[bot->GetGUID().GetCounter()];
    if (!t || std::hypot(bot->GetPositionX() - x, bot->GetPositionY() - y) > 5.0f)
    {
        x = bot->GetPositionX();
        y = bot->GetPositionY();
        t = now;
        return false;
    }
    return now > t + time_t(secs);
}

bool WovRealNear(uint32 map, float px, float py, float range)
{
    for (auto const& [id, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* p = session ? session->GetPlayer() : nullptr;
        if (p && !session->IsBot() && p->IsInWorld() && p->GetMapId() == map &&
            std::hypot(p->GetPositionX() - px, p->GetPositionY() - py) < range)
            return true;
    }
    return false;
}

void WovUnstick(Player* bot, PlayerbotAI* ai, WorldPosition const& dest, char const* why)
{
    float tx = dest.GetPositionX(), ty = dest.GetPositionY(), tz = dest.GetPositionZ();
    uint32 tmap = dest.GetMapId();
    if (ai->rpgInfo.GetStatus() == RPG_TRAVEL_FLIGHT)
    {
        auto* f = std::get_if<NewRpgInfo::TravelFlight>(&ai->rpgInfo.data);
        if (!f || f->inFlight)
            return;
        tx = f->flightMasterPos.GetPositionX();  // stuck on the way to the flight master
        ty = f->flightMasterPos.GetPositionY();
        tz = f->flightMasterPos.GetPositionZ();
        tmap = f->flightMasterPos.GetMapId();
    }
    if (tmap != bot->GetMapId() || (!tx && !ty))
        return;
    if (WovRealNear(tmap, bot->GetPositionX(), bot->GetPositionY(), 150.0f) || WovRealNear(tmap, tx, ty, 150.0f))
        return;
    LOG_INFO("playerbots", "WoV unstick: {} ({}) {:.0f} yd", bot->GetName(), why, bot->GetExactDist(tx, ty, tz));
    g_wovStill.erase(bot->GetGUID().GetCounter());
    bot->NearTeleportTo(tx + frand(-2.0f, 2.0f), ty + frand(-2.0f, 2.0f), tz + 0.5f, bot->GetOrientation());
}

void WovSayChannel(Player* bot, std::string const& text, bool alsoDefense)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai || text.empty())
        return;
    ai->SayToChannel(text, ChatChannelId::GENERAL);
    if (alsoDefense)
        ai->SayToChannel(text, ChatChannelId::LOCAL_DEFENSE);
}

void WovCry(WovSkirmish& s, TeamId defender, uint32 enemies)
{
    if (s.cried[defender])
        return;
    for (uint32 low : s.members[defender])
    {
        Player* p = ObjectAccessor::FindPlayerByLowGUID(low);
        if (!p || WovDist(p, s) > 250.0f)
            continue;
        WovSayChannel(p, WovFill(WovPick(kWovCry), s.place, enemies, WovOther(defender)), urand(0, 1));
        s.cried[defender] = true;
        return;
    }
}

uint32 WovRecruit(WovSkirmish& s, TeamId team, uint32 want)
{
    if (!want)
        return 0;
    std::vector<Player*> cands;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetTeamId() != team)
            continue;
        if (bot->GetGroup() || bot->InBattleground() || bot->IsInFlight() || !WovOpenWorld(bot))
            continue;
        if (bot->GetLevel() < s.minL || bot->GetLevel() > s.maxL)
            continue;
        if (g_wovBotSk.count(bot->GetGUID().GetCounter()) || bot->IsBeingTeleported() || WovBotReserved(bot->GetGUID().GetCounter()))
            continue;
        auto nr = g_wovNoRecruit.find(bot->GetGUID().GetCounter());
        if (nr != g_wovNoRecruit.end() && time(nullptr) < nr->second)
            continue;
        cands.push_back(bot);
    }
    std::shuffle(cands.begin(), cands.end(), std::mt19937(std::random_device{}()));
    std::stable_partition(cands.begin(), cands.end(), [&s](Player* b) { return b->GetMapId() == s.map; });
    uint32 got = 0;
    Player* firstRecruit = nullptr;
    for (Player* bot : cands)
    {
        if (got >= want)
            break;
        uint32 accept = g_wovWarAccept;
        switch (WovDriveOf(bot->GetGUID().GetCounter()))
        {
            case 1: accept = std::min<uint32>(95, accept * 9 / 5); break;
            case 5:
            case 6: accept = accept * 3 / 5; break;
            case 2:
            case 4: accept = accept * 4 / 5; break;
            default: break;
        }
        if (urand(0, 99) >= accept)
            continue;
        uint32 low = bot->GetGUID().GetCounter();
        g_wovBotSk[low] = s.id;
        s.members[team].insert(low);
        s.joined[low] = time(nullptr);
        WovSend(bot, s);
        ++got;
        if (!firstRecruit)
            firstRecruit = bot;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (ai && bot->GetGuild() && urand(0, 99) < 30)
            ai->SayToGuild(s.raid && team == s.raidTeam && got == 1 ? WovFill(WovPick(kWovRaidCall), s.place, 0, WovOther(team))
                                                                    : std::string(WovPick(kWovOmw)));
    }
    if (firstRecruit)
    {
        for (auto const& [id, session] : sWorldSessionMgr->GetAllSessions())
        {
            Player* p = session ? session->GetPlayer() : nullptr;
            if (!p || session->IsBot() || !p->IsInWorld() || p->GetTeamId() != team || !WovOpenWorld(p))
                continue;
            if (p->GetLevel() < s.minL || p->GetLevel() > s.maxL + 5 || WovDist(p, s) < 300.0f)
                continue;
            if (s.invitedReal.count(p->GetGUID().GetCounter()) || urand(0, 99) >= 60)
                continue;
            s.invitedReal.insert(p->GetGUID().GetCounter());
            firstRecruit->Whisper(WovFill(WovPick(kWovInvite), s.place, 0, WovOther(team)), LANG_UNIVERSAL, p);
        }
    }
    if (got)
        LOG_INFO("playerbots", "WoV war #{} {}: {} {} recruited", s.id, s.place, got,
                 team == TEAM_ALLIANCE ? "alliance" : "horde");
    return got;
}

void WovAddMember(WovSkirmish& s, uint32 low, TeamId team)
{
    if (!low || g_wovBotSk.count(low))
        return;
    g_wovBotSk[low] = s.id;
    s.members[team].insert(low);
}

void WovStartRaid(time_t now)
{
    std::vector<WovHotspot*> options;
    for (WovHotspot& h : g_wovHot)
    {
        if (!h.loaded)
            continue;  // town raids are allowed next to a graveyard (kill caps and the time limit end them)
        TeamId att = WovOther(h.owner);
        uint32 attackers = 0, defenders = 0;
        for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
        {
            if (!bot || !bot->IsInWorld() || bot->GetLevel() < h.minL || bot->GetLevel() > h.maxL || !WovOpenWorld(bot))
                continue;
            if (bot->GetTeamId() == att)
                ++attackers;
            else
                ++defenders;
        }
        if (attackers >= 10 && defenders >= 6)
            options.push_back(&h);
    }
    if (options.empty())
        return;
    WovHotspot* h = options[urand(0, options.size() - 1)];
    for (WovSkirmish const& s : g_wovSk)
        if (s.map == h->map && std::hypot(s.x - h->x, s.y - h->y) < 600.0f)
            return;
    WovSkirmish s;
    s.id = ++g_wovSkSeq;
    s.map = h->map;
    s.x = h->x;
    s.y = h->y;
    s.z = h->z;
    s.place = h->name;
    s.minL = h->minL;
    s.maxL = h->maxL;
    s.start = now;
    s.lastKill = now + 480;  // grace while the raid flies in
    s.lastWave = now;
    s.raid = true;
    s.owner = h->owner;
    s.raidTeam = WovOther(h->owner);
    s.raidTarget = urand(12, std::max<uint32>(12, g_wovWarMaxPerSide));
    g_wovSk.push_back(s);
    LOG_INFO("playerbots", "WoV war #{}: {} raid on {} ({} attackers wanted)", s.id,
             s.raidTeam == TEAM_ALLIANCE ? "alliance" : "horde", s.place, s.raidTarget);
    WovRecruit(g_wovSk.back(), s.raidTeam, s.raidTarget);
}

void WovHandleKill(WovKill const& k, time_t now)
{
    if (k.victimBot)
    {
        auto& d = g_wovPvpDeaths[k.victimLow];
        d.erase(std::remove_if(d.begin(), d.end(), [now](time_t t) { return t + 900 < now; }), d.end());
        d.push_back(now);
        if (d.size() >= 4)
        {
            g_wovHearth.insert(k.victimLow);
            g_wovNoRecruit[k.victimLow] = now + 1800;
            d.clear();
        }
    }
    TeamId victimTeam = WovOther(k.killerTeam);
    for (WovSkirmish& s : g_wovSk)
    {
        float d = s.map == k.map ? std::hypot(s.x - k.x, s.y - k.y) : 1.0e9f;
        bool sameReal = k.real && ((!k.killerBot && s.realLows.count(k.killerLow)) || (!k.victimBot && s.realLows.count(k.victimLow)));
        if (d < 300.0f || (sameReal && d < 1500.0f))
        {
            if ((sameReal || k.real) && !WovNearGraveyard(k.map, k.x, k.y))
            {
                s.x = k.x;  // the fight moves with the real player
                s.y = k.y;
                s.z = k.z;
                if (!k.killerBot)
                    s.realLows.insert(k.killerLow);
                if (!k.victimBot)
                    s.realLows.insert(k.victimLow);
            }
            ++s.kills[k.killerTeam == TEAM_ALLIANCE ? 0 : 1];
            s.lastKill = std::max(s.lastKill, now);
            if (k.killerBot)
                WovAddMember(s, k.killerLow, k.killerTeam);
            if (k.victimBot)
                WovAddMember(s, k.victimLow, victimTeam);
            return;
        }
    }
    g_wovRecent.push_back(k);
    while (!g_wovRecent.empty() && g_wovRecent.front().t + time_t(g_wovWarSparkWindow) < now)
        g_wovRecent.pop_front();
    uint32 n = 0;
    for (WovKill const& r : g_wovRecent)
        if (r.map == k.map && std::hypot(r.x - k.x, r.y - k.y) < 220.0f)
            ++n;
    if (!(n >= g_wovWarSparkKills || k.real))
        return;
    for (WovCool const& c : g_wovCool)
        if (c.map == k.map && now < c.until && std::hypot(c.x - k.x, c.y - k.y) < 800.0f)
            return;  // a fight just ended here; the place cools down first
    if (WovNearGraveyard(k.map, k.x, k.y))
        return;
    if (g_wovSk.size() >= g_wovWarMaxActive + (k.real ? 2 : 0))
        return;
    WovSkirmish s;
    s.id = ++g_wovSkSeq;
    s.map = k.map;
    s.x = k.x;
    s.y = k.y;
    s.z = k.z;
    s.place = k.place.empty() ? std::string("here") : k.place;
    s.minL = uint8(std::max<int>(1, int(k.victimLevel) - 7));
    s.maxL = uint8(std::min<int>(60, int(k.victimLevel) + 7));
    s.start = now;
    s.lastKill = now;
    s.lastWave = 0;
    s.owner = victimTeam;
    s.kills[k.killerTeam == TEAM_ALLIANCE ? 0 : 1] = 1;
    if (!k.killerBot)
        s.realLows.insert(k.killerLow);
    if (!k.victimBot)
        s.realLows.insert(k.victimLow);
    g_wovSk.push_back(s);
    WovSkirmish& ref = g_wovSk.back();
    if (k.killerBot)
        WovAddMember(ref, k.killerLow, k.killerTeam);
    if (k.victimBot)
        WovAddMember(ref, k.victimLow, victimTeam);
    LOG_INFO("playerbots", "WoV war #{}: skirmish at {} (lvl {}-{})", ref.id, ref.place, ref.minL, ref.maxL);
    WovCry(ref, victimTeam, n);
    if (!k.killerBot)
        WovRecruit(ref, k.killerTeam, urand(2, 3));  // a real player is never alone for long
}

void WovEnd(WovSkirmish& s)
{
    uint32 said = 0;
    for (int t = 0; t < 2; ++t)
    {
        for (uint32 low : s.members[t])
        {
            g_wovBotSk.erase(low);
            Player* p = ObjectAccessor::FindPlayerByLowGUID(low);
            if (!p)
                continue;
            PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
            if (!ai)
                continue;
            if (said < 3 && p->IsAlive() && WovDist(p, s) < 200.0f && urand(0, 99) < 15)
            {
                WovSayChannel(p, WovPick(kWovEnd), false);
                ++said;
            }
            if (p->IsAlive() && !p->GetGroup() && ai->rpgInfo.GetStatus() != RPG_TRAVEL_FLIGHT)
                ai->rpgInfo.ChangeToIdle();  // everyone goes back to their own business
        }
    }
    g_wovCool.push_back({s.map, s.x, s.y, time(nullptr) + 1800});
    g_wovCool.erase(std::remove_if(g_wovCool.begin(), g_wovCool.end(),
                                   [](WovCool const& c) { return c.until < time(nullptr); }),
                    g_wovCool.end());
    {   // wov: record the battle for the web portal (acore_characters.wov_battle)
        uint32 zone = 0;
        for (int t = 0; t < 2 && !zone; ++t)
            for (uint32 low : s.members[t])
                if (Player* p = ObjectAccessor::FindPlayerByLowGUID(low))
                    if (p->GetMapId() == s.map && WovDist(p, s) < 300.0f)
                    {
                        zone = p->GetZoneId();
                        break;
                    }
        std::string place = s.place;
        CharacterDatabase.EscapeString(place);
        char const* winner = s.kills[0] > s.kills[1] ? "alliance" : (s.kills[1] > s.kills[0] ? "horde" : "draw");
        std::string raidTeam = s.raid ? (s.raidTeam == TEAM_ALLIANCE ? "0" : "1") : "NULL";
        CharacterDatabase.Execute(
            "INSERT INTO wov_battle (started, ended, map, zone, place, kind, raid_team, alliance_size, horde_size, alliance_kills, horde_kills, winner) "
            "VALUES (FROM_UNIXTIME({}), NOW(), {}, {}, '{}', '{}', {}, {}, {}, {}, {}, '{}')",
            uint64(s.start), s.map, zone, place, s.raid ? "raid" : "skirmish", raidTeam, uint32(s.members[0].size()),
            uint32(s.members[1].size()), s.kills[0], s.kills[1], winner);
    }
    LOG_INFO("playerbots", "WoV war #{} at {} over ({} vs {})", s.id, s.place, s.members[0].size(), s.members[1].size());
}

void WovWar_Tick(time_t now)
{
    WovWarConfig(now);
    if (!g_wovWarEnable)
        return;
    static time_t last = 0;
    if (now < last + 5)
        return;
    last = now;
    WovHotLoad();

    std::deque<WovKill> kills;
    {
        std::lock_guard<std::mutex> lock(g_wovWarMutex);
        kills.swap(g_wovKillQueue);
    }
    for (WovKill const& k : kills)
        WovHandleKill(k, now);

    for (auto it = g_wovHearth.begin(); it != g_wovHearth.end();)
    {
        Player* p = ObjectAccessor::FindPlayerByLowGUID(*it);
        if (!p || !p->IsInWorld())
        {
            it = g_wovHearth.erase(it);
            continue;
        }
        if (WovBotReserved(*it))  // on a dungeon run or raid night: it runs back to its group instead
        {
            it = g_wovHearth.erase(it);
            continue;
        }
        if (!p->IsAlive() || p->IsInCombat() || p->IsBeingTeleported() || p->GetGroup())
        {
            ++it;
            continue;
        }
        if (auto sk = g_wovBotSk.find(*it); sk != g_wovBotSk.end())
        {
            for (WovSkirmish& sm : g_wovSk)
                if (sm.id == sk->second)
                {
                    sm.members[0].erase(*it);
                    sm.members[1].erase(*it);
                }
            g_wovBotSk.erase(sk);
        }
        LOG_INFO("playerbots", "WoV war: {} died 4 times to players, hearths home", p->GetName());
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(p))
            ai->rpgInfo.ChangeToIdle();
        p->TeleportTo(p->m_homebindMapId, p->m_homebindX, p->m_homebindY, p->m_homebindZ, p->GetOrientation());
        it = g_wovHearth.erase(it);
    }

    static time_t lastRaidCheck = now;
    if (now >= lastRaidCheck + 600)
    {
        lastRaidCheck = now;
        tm lt{};
        localtime_r(&now, &lt);
        uint32 hour = uint32(lt.tm_hour);
        if (hour >= g_wovWarRaidFrom && hour < g_wovWarRaidTo && g_wovSk.size() < g_wovWarMaxActive &&
            urand(0, 99) < g_wovWarRaidChance)
            WovStartRaid(now);
    }

    for (size_t i = 0; i < g_wovSk.size();)
    {
        WovSkirmish& s = g_wovSk[i];
        uint32 nearCount[2] = {0, 0};
        uint32 aliveCount[2] = {0, 0};
        WorldPosition dest(s.map, s.x, s.y, s.z);
        for (int t = 0; t < 2; ++t)
        {
            for (auto it = s.members[t].begin(); it != s.members[t].end();)
            {
                Player* p = ObjectAccessor::FindPlayerByLowGUID(*it);
                if (!p || !p->IsInWorld())
                {
                    Player* c = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(*it));
                    if (c && c->IsBeingTeleportedFar())
                    {
                        ++it;  // on the way from the other continent
                        continue;
                    }
                    g_wovBotSk.erase(*it);
                    it = s.members[t].erase(it);
                    continue;
                }
                float d = WovDist(p, s);
                auto jn = s.joined.find(*it);
                if (!s.arrived.count(*it) && d > 450.0f && jn != s.joined.end() && now > jn->second + 480)
                {
                    g_wovBotSk.erase(*it);  // never made it: free the slot for someone closer
                    s.joined.erase(jn);
                    it = s.members[t].erase(it);
                    continue;
                }
                if (p->IsAlive())
                    ++aliveCount[t];
                if (p->IsAlive() && d < 160.0f)
                    ++nearCount[t];
                if (p->IsAlive() && d < 120.0f && !s.arrived.count(*it))
                {
                    s.arrived.insert(*it);
                    if (s.arriveSaid[t] < 3 && urand(0, 99) < 30)
                    {
                        ++s.arriveSaid[t];
                        WovSayChannel(p, WovPick(kWovArrive), false);
                    }
                }
                PlayerbotAI* ai = GET_PLAYERBOT_AI(p);
                if (ai && p->IsAlive() && !p->IsInFlight() && !p->GetGroup() &&
                    ai->rpgInfo.GetStatus() != RPG_TRAVEL_FLIGHT && d > 70.0f)
                {
                    if (d > 450.0f)
                        WovSend(p, s);
                    else
                        ai->rpgInfo.ChangeToGoGrind(dest);
                }
                if (ai && p->IsAlive() && !p->IsInFlight() && !p->IsInCombat() && !p->GetGroup() && d > 70.0f &&
                    WovStuck(p, now, 120))
                    WovUnstick(p, ai, dest, "war");
                ++it;
            }
        }
        for (auto const& [id, session] : sWorldSessionMgr->GetAllSessions())
        {
            Player* p = session ? session->GetPlayer() : nullptr;
            if (p && !session->IsBot() && p->IsInWorld() && p->IsAlive() && WovDist(p, s) < 160.0f)
                ++nearCount[p->GetTeamId() == TEAM_ALLIANCE ? 0 : 1];
        }

        if (s.raid && !s.announced && nearCount[s.raidTeam] >= 3)
        {
            s.announced = true;
            s.lastKill = std::max(s.lastKill, now);
            for (uint32 low : s.members[s.raidTeam])
            {
                Player* p = ObjectAccessor::FindPlayerByLowGUID(low);
                if (p && WovDist(p, s) < 160.0f)
                {
                    WorldPacket data(SMSG_ZONE_UNDER_ATTACK, 4);
                    data << uint32(p->GetAreaId());
                    sWorldSessionMgr->SendGlobalMessage(&data, nullptr, s.owner);
                    break;
                }
            }
            WovCry(s, s.owner, nearCount[s.raidTeam]);
        }

        if (now >= s.lastWave + 60)
        {
            s.lastWave = now;
            LOG_INFO("playerbots", "WoV war #{} {}: alliance {} ({} here), horde {} ({} here), kills {}-{}", s.id, s.place,
                     s.members[0].size(), nearCount[0], s.members[1].size(), nearCount[1], s.kills[0], s.kills[1]);
            for (int t = 0; t < 2; ++t)
            {
                TeamId team = TeamId(t);
                uint32 have = aliveCount[t];  // the dead do not count: send more
                uint32 want = 0;
                if (s.raid && team == s.raidTeam)
                    want = have < s.raidTarget ? std::min<uint32>(s.raidTarget - have, 10) : 0;
                else if (!(s.raid && !s.announced))
                {
                    uint32 enemies = nearCount[1 - t];
                    uint32 lost = s.kills[1 - t];
                    uint32 target = std::min<uint32>(g_wovWarMaxPerSide, enemies * 3 / 2 + 3 + lost * 2);
                    if (!enemies && !lost)
                        target = 0;
                    want = target > have ? std::min<uint32>(target - have, 10) : 0;
                }
                // A side that is only being farmed stops feeding bodies in one by one, and nobody trickles in
                // alone against a crowd: they come as a group or not at all.
                uint32 lostT = s.kills[1 - t], madeT = s.kills[t];
                if (lostT > madeT + 12 || s.members[t].size() >= g_wovWarMaxPerSide * 2)
                    want = 0;
                else if (want && want < 4 && nearCount[t] * 2 < nearCount[1 - t])
                    want = 0;
                WovRecruit(s, team, want);
            }
        }

        bool timeout = now > s.start + time_t(g_wovWarMaxMinutes) * 60 || s.kills[0] + s.kills[1] >= 80 ||
                       std::max(s.kills[0], s.kills[1]) >= std::min(s.kills[0], s.kills[1]) + 30;  // over: rout or bloodbath
        bool idle = now > s.lastKill + time_t(g_wovWarIdleEnd) && now > s.start + 300;
        if (timeout || idle)
        {
            WovEnd(s);
            g_wovSk.erase(g_wovSk.begin() + i);
            continue;
        }
        ++i;
    }
}
}  // namespace

// Called from the PvP kill hook (map threads): only queues.
void WovWar_OnPvpKill(Player* killer, Player* killed)
{
    if (!g_wovWarEnable || !killer || !killed || killer == killed || killer->GetTeamId() == killed->GetTeamId())
        return;
    if (!WovOpenWorld(killed))
        return;
    WovKill k;
    k.t = time(nullptr);
    k.map = killed->GetMapId();
    k.x = killed->GetPositionX();
    k.y = killed->GetPositionY();
    k.z = killed->GetPositionZ();
    k.killerTeam = killer->GetTeamId();
    k.killerBot = sRandomPlayerbotMgr.IsRandomBot(killer);
    k.victimBot = sRandomPlayerbotMgr.IsRandomBot(killed);
    k.real = !k.killerBot || !k.victimBot;
    k.victimLevel = killed->GetLevel();
    k.killerLow = killer->GetGUID().GetCounter();
    k.victimLow = killed->GetGUID().GetCounter();
    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(killed->GetAreaId()))
        k.place = area->area_name[0];
    std::lock_guard<std::mutex> lock(g_wovWarMutex);
    g_wovKillQueue.push_back(std::move(k));
}
// ---- end wov: Call to Arms ----------------------------------------------------------------------------

// ---- wov: initiative -----------------------------------------------------------------------------------
uint32 WovMarketPriceOf(uint32 entry, TeamId team);  // wov: market (below)
// Bots act like players who want things: LFG/LFM, WTS/WTB and guild recruitment in the channels real players
// can see, asking real players nearby to group ("inv?"), inviting them when they answer an LFM, and guild
// leaders inviting unguilded real players. See wov/HANDOVER.md.
#include "GuildMgr.h"
#include <unordered_set>
#include <tuple>
#include <cstring>
#include <cctype>
namespace
{
bool g_wovInitEnable = false;
uint32 g_wovInitLfgSec = 150, g_wovInitTradeSec = 140, g_wovInitRecruitSec = 600, g_wovInitGroupMin = 12,
       g_wovInitGuildMin = 60;
time_t g_wovInitCfgT = 0;

struct WovDungeon
{
    char const* name;
    uint8 minL, maxL;
    int team;  // -1 both, 0 alliance, 1 horde
};
WovDungeon const kWovDungeons[] = {
    {"RFC", 13, 18, 1},      {"deadmines", 17, 26, 0}, {"WC", 17, 25, -1},     {"SFK", 22, 30, -1},
    {"BFD", 24, 32, -1},     {"stockades", 24, 32, 0}, {"gnomer", 29, 38, -1}, {"RFK", 29, 38, -1},
    {"SM GY", 28, 37, -1},   {"SM lib", 30, 39, -1},   {"SM arm", 32, 42, -1}, {"SM cath", 35, 45, -1},
    {"RFD", 37, 46, -1},     {"uldaman", 41, 51, -1},  {"ZF", 44, 54, -1},     {"mara", 46, 55, -1},
    {"ST", 50, 56, -1},      {"BRD", 52, 60, -1},      {"LBRS", 55, 60, -1},   {"DM east", 55, 60, -1},
    {"strat", 58, 60, -1},   {"scholo", 58, 60, -1},   {"UBRS", 58, 60, -1}};

char const* const kWovLfg[] = {"LFG {dg}", "lfg {dg}", "{role} LFG {dg}", "lfg {dg} {role}", "LFG {dg} as {role}",
                               "{role} lf {dg} grp", "anyone doing {dg}? {role} here", "lfg {dg} pst"};
char const* const kWovLfm[] = {"LF{n}M {dg} need {need}", "LFM {dg} need {need}", "{dg} LF{n}M", "LF1M {dg} {need} pst",
                               "LF{n}M {dg}, {need} then go", "need {need} for {dg}, pst", "LF{n}M {dg} pst"};
char const* const kWovNeed[] = {"tank", "heals", "dps", "tank and heals", "healer", "1 dps", "a tank"};
char const* const kWovWts[] = {"WTS {item} {price}", "WTS {item} pst", "selling {item} {price}", "WTS {item}, {price} obo",
                               "{item} for sale, pst", "WTS {item} cheap, pst"};
char const* const kWovWtb[] = {"WTB {item} pst", "WTB {item} x{n}", "buying {item}, pst", "WTB {item}, paying well",
                               "anyone selling {item}? pst"};
char const* const kWovRecruit[] = {"<{guild}> recruiting all lvls, pst for inv", "{guild} is recruiting, chill ppl, all classes",
                                   "{guild} lf members for dungeons and pvp, /w me", "looking for a guild? {guild} is recruiting, whisper me",
                                   "<{guild}> is recruiting! lvl 10+ pst"};
char const* const kWovAskGroup[] = {"wanna group? doing the same quests", "hey u wanna duo for a bit?", "group up? inv me",
                                    "inv? we're on the same quests lol", "want to team up? inv me", "yo wanna group"};
char const* const kWovAskGuild[] = {"hey, want a guild? we're chill, sent u an inv", "u need a guild? {guild} is recruiting, sent inv",
                                    "yo, sent u a guild inv, we run dungeons and stuff", "want to join {guild}? invited u"};
char const* const kWovInvOk[] = {"inv sent", "sent", "invited", "inv'd", "sure, sent"};

uint32 const kWovWtbItems[][2] = {{2589, 1}, {2592, 15}, {4306, 25}, {4338, 35}, {14047, 48},
                                  {2840, 1}, {3576, 12}, {3575, 25}, {3860, 35}, {12359, 48},
                                  {858, 10}, {929, 20}, {1710, 30}, {3928, 40}, {13446, 50}};

struct WovLfx
{
    time_t t = 0;   // last post (0 once a real player answered it)
    bool lfm = false;
    WovDungeon const* dg = nullptr;
    time_t t0 = 0;  // when the bot decided on this dungeon; it sticks to it for a while, then gives up
    std::string role, need;
};
std::unordered_map<uint32, time_t> g_wovLastPost;  // bot -> last initiative post (world thread)
uint32 const kWovIntentSec = 1500, kWovGiveUpQuietSec = 1800, kWovPostGapSec = 300;
std::mutex g_wovInitMutex;
std::unordered_map<uint32, WovLfx> g_wovLfx;
std::vector<std::pair<uint32, uint32>> g_wovInviteQueue;
std::vector<std::tuple<uint32, uint32, WovDungeon const*>> g_wovJoinTrack;
std::unordered_map<uint32, time_t> g_wovNextGroupAsk, g_wovNextGuildAsk;
struct WovGuildApp
{
    time_t due;
    uint32 applicant, recruiter, guildId;
};
std::vector<WovGuildApp> g_wovGuildApps;
char const* const kWovApply[] = {"saw {guild} recruiting, can i get an inv?", "hey, still recruiting? inv pls", "inv to {guild}? pls",
                                 "looking for a guild, can i join?", "{guild} recruiting? id like an inv"};

void WovInitConfig(time_t now)
{
    if (g_wovInitCfgT && now < g_wovInitCfgT + 60)
        return;
    g_wovInitCfgT = now;
    g_wovInitEnable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.Init.Enable", false);
    g_wovInitLfgSec = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Init.LfgSeconds", 150);
    g_wovInitTradeSec = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Init.TradeSeconds", 140);
    g_wovInitRecruitSec = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Init.RecruitSeconds", 600);
    g_wovInitGroupMin = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Init.GroupAskMinutes", 12);
    g_wovInitGuildMin = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Init.GuildAskMinutes", 60);
}

bool WovInCapital(Player* p)
{
    AreaTableEntry const* zone = sAreaTableStore.LookupEntry(p->GetZoneId());
    return zone && (zone->flags & AREA_FLAG_CAPITAL);
}

std::string WovRep(std::string s, std::string const& k, std::string const& v)
{
    for (size_t p = s.find(k); p != std::string::npos; p = s.find(k, p + v.size()))
        s.replace(p, k.size(), v);
    return s;
}

std::string WovRole(Player* bot)
{
    switch (bot->getClass())
    {
        case CLASS_WARRIOR: return urand(0, 1) ? "tank" : "dps";
        case CLASS_PRIEST: return urand(0, 3) ? "healer" : "heals";
        case CLASS_DRUID:
        case CLASS_SHAMAN:
        case CLASS_PALADIN: return urand(0, 1) ? "healer" : "dps";
        default: return "dps";
    }
}

std::string WovPrice(uint32 copper)
{
    uint32 g = copper / 10000, s = (copper % 10000) / 100;
    if (g && s >= 10)
        return std::to_string(g) + "g " + std::to_string(s) + "s";
    if (g)
        return std::to_string(g) + "g";
    return std::to_string(std::max<uint32>(s, 1)) + "s";
}

WovDungeon const* WovDungeonFor(Player* bot)
{
    std::vector<WovDungeon const*> fit;
    int team = bot->GetTeamId() == TEAM_ALLIANCE ? 0 : 1;
    for (WovDungeon const& d : kWovDungeons)
        if (bot->GetLevel() >= d.minL && bot->GetLevel() <= d.maxL && (d.team < 0 || d.team == team))
            fit.push_back(&d);
    return fit.empty() ? nullptr : fit[urand(0, fit.size() - 1)];
}

Player* WovPickPoster(Player* real, bool needCapital, uint8 preferDrive = 0)
{
    std::vector<Player*> c;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!bot || !bot->IsInWorld() || bot->GetTeamId() != real->GetTeamId() || bot->GetZoneId() != real->GetZoneId())
            continue;
        if (needCapital && !WovInCapital(bot))
            continue;
        if (bot->GetGroup() || (bot->GetMap() && bot->GetMap()->Instanceable()))
            continue;
        if (g_wovBotSk.count(bot->GetGUID().GetCounter()))
            continue;
        auto lp = g_wovLastPost.find(bot->GetGUID().GetCounter());
        if (lp != g_wovLastPost.end() && time(nullptr) < lp->second + kWovPostGapSec)
            continue;
        c.push_back(bot);
        if (preferDrive && WovDriveOf(bot->GetGUID().GetCounter()) == preferDrive)
            for (int k = 0; k < 3; ++k)
                c.push_back(bot);
    }
    return c.empty() ? nullptr : c[urand(0, c.size() - 1)];
}

void WovPostLfx(Player* bot, Player* real)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai || bot->GetGroup())
        return;
    uint32 low = bot->GetGUID().GetCounter();
    time_t now = time(nullptr);
    WovLfx cur;
    {
        std::lock_guard<std::mutex> lock(g_wovInitMutex);
        auto it = g_wovLfx.find(low);
        if (it != g_wovLfx.end())
            cur = it->second;
    }
    if (cur.dg && now >= cur.t0 + kWovIntentSec && now < cur.t0 + kWovIntentSec + kWovGiveUpQuietSec)
        return;  // gave up on its group; quiet for a while
    if (!cur.dg || now >= cur.t0 + kWovIntentSec)
    {
        // a new plan: one dungeon, looking for a group OR building one, and it sticks to it
        cur = WovLfx();
        cur.dg = WovDungeonFor(bot);
        if (!cur.dg)
            return;
        cur.lfm = urand(0, 99) < 45;
        cur.t0 = now;
        cur.role = WovRole(bot);
        cur.need = WovPick(kWovNeed);
    }
    bool lfm = cur.lfm;
    WovDungeon const* d = cur.dg;
    std::string text = lfm ? WovPick(kWovLfm) : WovPick(kWovLfg);
    text = WovRep(text, "{dg}", d->name);
    text = WovRep(text, "{role}", cur.role);
    text = WovRep(text, "{need}", cur.need);
    text = WovRep(text, "{n}", std::to_string(urand(1, 3)));
    if (WovInCapital(real))
        ai->SayToChannel(text, urand(0, 1) ? ChatChannelId::LOOKING_FOR_GROUP : ChatChannelId::TRADE);
    else
        ai->SayToChannel(text, ChatChannelId::GENERAL);
    g_wovLastPost[low] = now;
    cur.t = now;
    std::lock_guard<std::mutex> lock(g_wovInitMutex);
    g_wovLfx[low] = cur;
}

bool WovSellable(ItemTemplate const* p)
{
    return p && p->Bonding != BIND_WHEN_PICKED_UP && p->Bonding != BIND_QUEST_ITEM && p->SellPrice > 0 &&
           (p->Quality >= ITEM_QUALITY_UNCOMMON || p->Class == ITEM_CLASS_TRADE_GOODS);
}

bool WovCraftTradeAsk(Player* asker);  // wov: crafting orders (below)

void WovPostTrade(Player* bot)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai)
        return;
    if (urand(0, 99) < 22 && WovCraftTradeAsk(bot))  // wov: "LF tailor to make [item]", a crafter answers
    {
        g_wovLastPost[bot->GetGUID().GetCounter()] = time(nullptr);
        return;
    }
    std::string text;
    if (urand(0, 99) < 55)
    {
        std::vector<Item*> sellable;
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* b = bot->GetBagByPos(bag))
                for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                    if (Item* it = b->GetItemByPos(slot))
                        if (WovSellable(it->GetTemplate()))
                            sellable.push_back(it);
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* it = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (WovSellable(it->GetTemplate()))
                    sellable.push_back(it);
        if (!sellable.empty())
        {
            Item* it = sellable[urand(0, sellable.size() - 1)];
            ItemTemplate const* p = it->GetTemplate();
            uint32 price = std::max<uint32>(p->SellPrice * urand(3, 6) * std::max<uint32>(1, it->GetCount()), 500);
            if (uint32 unit = WovMarketPriceOf(p->ItemId, bot->GetTeamId()))  // what it goes for on the AH right now
                price = std::max<uint32>(unit * std::max<uint32>(1, it->GetCount()) * urand(85, 105) / 100, p->SellPrice + 1);
            text = WovRep(WovRep(WovPick(kWovWts), "{item}", ChatHelper::FormatItem(p, it->GetCount() > 1 ? it->GetCount() : 0)),
                          "{price}", WovPrice(price));
        }
    }
    if (text.empty())
    {
        std::vector<uint32> wants;
        for (auto const& w : kWovWtbItems)
            if (bot->GetLevel() >= w[1] && bot->GetLevel() <= w[1] + 20)
                wants.push_back(w[0]);
        if (wants.empty())
            return;
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(wants[urand(0, wants.size() - 1)]);
        if (!p)
            return;
        text = WovRep(WovRep(WovPick(kWovWtb), "{item}", ChatHelper::FormatItem(p)), "{n}", std::to_string(urand(1, 4) * 5));
    }
    g_wovLastPost[bot->GetGUID().GetCounter()] = time(nullptr);
    ai->SayToChannel(text, ChatChannelId::TRADE);
}

void WovPostRecruit(Player* real)
{
    std::vector<Player*> c;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
        if (bot && bot->IsInWorld() && bot->GetGuild() && bot->GetTeamId() == real->GetTeamId() &&
            bot->GetZoneId() == real->GetZoneId() && !bot->GetGroup() && bot->GetMap() && !bot->GetMap()->Instanceable())
            c.push_back(bot);
    c.erase(std::remove_if(c.begin(), c.end(), [](Player* b) {
                auto lp = g_wovLastPost.find(b->GetGUID().GetCounter());
                return lp != g_wovLastPost.end() && time(nullptr) < lp->second + kWovPostGapSec;
            }), c.end());
    if (c.empty())
        return;
    Player* bot = c[urand(0, c.size() - 1)];
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai)
        return;
    g_wovLastPost[bot->GetGUID().GetCounter()] = time(nullptr);
    std::string text = WovRep(WovPick(kWovRecruit), "{guild}", bot->GetGuild()->GetName());
    ai->SayToChannel(text, WovInCapital(bot) ? ChatChannelId::GUILD_RECRUITMENT : ChatChannelId::GENERAL);
    // a few unguilded players around answer the post
    time_t now = time(nullptr);
    uint32 apps = 0;
    for (auto const& [guid, a] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (apps >= 2)
            break;
        if (!a || a == bot || !a->IsInWorld() || a->GetGuild() || a->GetTeamId() != bot->GetTeamId() || a->GetLevel() < 5)
            continue;
        if (a->GetZoneId() != bot->GetZoneId() || urand(0, 99) >= 25)
            continue;
        g_wovGuildApps.push_back({now + time_t(urand(20, 120)), a->GetGUID().GetCounter(), bot->GetGUID().GetCounter(),
                                  bot->GetGuild()->GetId()});
        ++apps;
    }
}

void WovAskGroup(Player* real)
{
    if (real->GetGroup() || !WovOpenWorld(real))
        return;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetGroup() || bot->GetTeamId() != real->GetTeamId())
            continue;
        if (bot->GetMapId() != real->GetMapId() || bot->GetExactDist(real) > 60.0f)
            continue;
        if (std::abs(int(bot->GetLevel()) - int(real->GetLevel())) > 3 || g_wovBotSk.count(bot->GetGUID().GetCounter()))
            continue;
        bot->Whisper(WovPick(kWovAskGroup), LANG_UNIVERSAL, real);
        LOG_INFO("playerbots", "WoV init: {} asks {} to group", bot->GetName(), real->GetName());
        return;
    }
}

void WovAskGuild(Player* real)
{
    if (real->GetGuild() || real->GetLevel() < 5)
        return;
    std::vector<Guild*> guilds;
    std::set<uint32> seen;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        Guild* g = bot ? bot->GetGuild() : nullptr;
        if (!g || bot->GetTeamId() != real->GetTeamId() || seen.count(g->GetId()))
            continue;
        seen.insert(g->GetId());
        Player* leader = ObjectAccessor::FindPlayer(g->GetLeaderGUID());
        if (leader && leader->IsInWorld() && GET_PLAYERBOT_AI(leader))
            guilds.push_back(g);
    }
    if (guilds.empty())
        return;
    Guild* g = guilds[urand(0, guilds.size() - 1)];
    Player* leader = ObjectAccessor::FindPlayer(g->GetLeaderGUID());
    if (!leader)
        return;
    g->HandleInviteMember(leader->GetSession(), real->GetName());
    leader->Whisper(WovRep(WovPick(kWovAskGuild), "{guild}", g->GetName()), LANG_UNIVERSAL, real);
    LOG_INFO("playerbots", "WoV init: {} invites {} to <{}>", leader->GetName(), real->GetName(), g->GetName());
}

void WovInit_Tick(time_t now)
{
    WovInitConfig(now);
    if (!g_wovInitEnable)
        return;
    static time_t last = 0;
    if (now < last + 5)
        return;
    last = now;

    std::vector<std::pair<uint32, uint32>> invites;
    {
        std::lock_guard<std::mutex> lock(g_wovInitMutex);
        invites.swap(g_wovInviteQueue);
    }
    for (auto const& [botLow, playerLow] : invites)
    {
        Player* bot = ObjectAccessor::FindPlayerByLowGUID(botLow);
        Player* p = ObjectAccessor::FindPlayerByLowGUID(playerLow);
        if (!bot || !p || p->GetGroup() || (bot->GetGroup() && bot->GetGroup()->IsFull()))
            continue;
        bot->Whisper(WovPick(kWovInvOk), LANG_UNIVERSAL, p);
        WorldPacket pk;
        pk << p->GetName();
        pk << uint32(0);
        bot->GetSession()->HandleGroupInviteOpcode(pk);
    }

    for (size_t i = 0; i < g_wovGuildApps.size();)
    {
        WovGuildApp app = g_wovGuildApps[i];
        if (now < app.due)
        {
            ++i;
            continue;
        }
        g_wovGuildApps.erase(g_wovGuildApps.begin() + i);
        Player* a = ObjectAccessor::FindPlayerByLowGUID(app.applicant);
        Player* r = ObjectAccessor::FindPlayerByLowGUID(app.recruiter);
        Guild* g = sGuildMgr->GetGuildById(app.guildId);
        if (!a || a->GetGuild() || !g)
            continue;
        Player* leader = ObjectAccessor::FindPlayer(g->GetLeaderGUID());
        std::string text = WovRep(WovPick(kWovApply), "{guild}", g->GetName());
        bool realLeader = g->GetLeaderGUID() && !sRandomPlayerbotMgr.IsRandomBot(g->GetLeaderGUID().GetCounter());
        // a bot member whose rank may invite (e.g. Veteran/Officer set by a real guild master) handles it
        Player* officer = nullptr;
        if (realLeader)
            for (auto const& [og, ob] : sRandomPlayerbotMgr.GetAllBots())
                if (ob && ob->IsInWorld() && ob->GetGuildId() == g->GetId() && g->HasRankRight(ob, GR_RIGHT_INVITE))
                {
                    officer = ob;
                    break;
                }
        if (officer)
        {
            static char const* const kOk[] = {"sure, sent", "inv sent", "welcome, sent u an inv", "sent, welcome", "np, inv sent"};
            a->Whisper(text, LANG_UNIVERSAL, officer);
            g->HandleInviteMember(officer->GetSession(), a->GetName());
            officer->Whisper(WovPick(kOk), LANG_UNIVERSAL, a);
            LOG_INFO("playerbots", "WoV init: officer {} invites {} to <{}>", officer->GetName(), a->GetName(), g->GetName());
        }
        else if (leader && leader->IsInWorld() && !GET_PLAYERBOT_AI(leader))
            a->Whisper(text, LANG_UNIVERSAL, leader);  // a real guild master decides
        else if (r && !realLeader)
        {
            a->Whisper(text, LANG_UNIVERSAL, r);
            g->AddMember(a->GetGUID());
            LOG_INFO("playerbots", "WoV init: {} joined <{}> after {}'s recruitment post", a->GetName(), g->GetName(), r->GetName());
        }
    }

    static time_t nextLfg[2] = {0, 0}, nextTrade[2] = {0, 0}, nextRecruit[2] = {0, 0};
    std::vector<Player*> reals[2];
    for (auto const& [id, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* p = session ? session->GetPlayer() : nullptr;
        if (p && !session->IsBot() && p->IsInWorld())
            reals[p->GetTeamId() == TEAM_ALLIANCE ? 0 : 1].push_back(p);
    }
    for (int t = 0; t < 2; ++t)
    {
        if (reals[t].empty())
            continue;
        Player* real = reals[t][urand(0, reals[t].size() - 1)];
        if (now >= nextLfg[t])
        {
            nextLfg[t] = now + urand(g_wovInitLfgSec / 2, g_wovInitLfgSec * 3 / 2);
            if (Player* bot = WovPickPoster(real, false, 2))
                WovPostLfx(bot, real);
        }
        if (now >= nextTrade[t])
        {
            nextTrade[t] = now + urand(g_wovInitTradeSec / 2, g_wovInitTradeSec * 3 / 2);
            if (WovInCapital(real))
                if (Player* bot = WovPickPoster(real, true, 3))
                    WovPostTrade(bot);
        }
        if (now >= nextRecruit[t])
        {
            nextRecruit[t] = now + urand(g_wovInitRecruitSec / 2, g_wovInitRecruitSec * 3 / 2);
            WovPostRecruit(real);
        }
        for (Player* p : reals[t])
        {
            uint32 low = p->GetGUID().GetCounter();
            time_t& ng = g_wovNextGroupAsk[low];
            if (!ng)
                ng = now + urand(3, 8) * 60;
            else if (now >= ng)
            {
                ng = now + urand(g_wovInitGroupMin * 60 / 2, g_wovInitGroupMin * 60 * 3 / 2);
                if (urand(0, 99) < 40)
                    WovAskGroup(p);
            }
            time_t& nu = g_wovNextGuildAsk[low];
            if (!nu)
                nu = now + urand(10, 25) * 60;
            else if (now >= nu)
            {
                nu = now + urand(g_wovInitGuildMin * 60 / 2, g_wovInitGuildMin * 60 * 3 / 2);
                if (urand(0, 99) < 50)
                    WovAskGuild(p);
            }
        }
    }
}
}  // namespace

// Whisper hook (Playerbots.cpp): a real player answering a recent LFG/LFM post gets an invite.
void WovInit_OnWhisper(Player* from, Player* to)
{
    if (!g_wovInitEnable || !from || !to || from->GetTeamId() != to->GetTeamId())
        return;
    if (sRandomPlayerbotMgr.IsRandomBot(from) || !sRandomPlayerbotMgr.IsRandomBot(to))
        return;
    std::lock_guard<std::mutex> lock(g_wovInitMutex);
    auto it = g_wovLfx.find(to->GetGUID().GetCounter());
    if (it == g_wovLfx.end() || time(nullptr) > it->second.t + 600)
        return;
    g_wovInviteQueue.emplace_back(to->GetGUID().GetCounter(), from->GetGUID().GetCounter());
    g_wovJoinTrack.emplace_back(to->GetGUID().GetCounter(), from->GetGUID().GetCounter(), it->second.dg);
    it->second.t = 0;  // answered; the plan itself stays, so the bot does not switch dungeon
}

// For mod-ollama-chat's prompts: what this bot has said it is doing in the channels, so its talk stays consistent.
std::string WovIntentOf(uint32 low)
{
    if (!g_wovInitEnable)
        return "";
    Player* bot = ObjectAccessor::FindPlayerByLowGUID(low);
    std::lock_guard<std::mutex> lock(g_wovInitMutex);
    auto it = g_wovLfx.find(low);
    if (it == g_wovLfx.end() || !it->second.dg || time(nullptr) >= it->second.t0 + kWovIntentSec)
        return "";
    std::string dg = it->second.dg->name;
    if (bot && bot->GetGroup())
        return "Right now you are in a group for " + dg + ". Talk is about that group and run; do not look for other "
               "groups or quest partners.";
    if (it->second.lfm)
        return "Right now you are putting a group together for " + dg + " and still need " + it->second.need +
               " (you posted it in the channel). If you mention what you are doing, it is that; do not look for other "
               "groups, dungeons or quest partners.";
    return "Right now you are looking for a group for " + dg + " as " + it->second.role +
           " (you posted it in the channel). If you mention what you are doing, it is that; do not ask for other "
           "groups, dungeons or quest partners.";
}
// ---- end wov: initiative --------------------------------------------------------------------------------

// ---- wov: play together --------------------------------------------------------------------------------
// A real player who posts LFM/LFG in a channel gets answers like on a real server: bots of the right level and
// role whisper "inv, healer" (LFM), or a bot forms a group and invites them (LFG). A bot-led group fills its
// open slots with bots, then hands the lead to the real player so the bots follow them into the dungeon.
namespace
{
struct WovParty
{
    uint32 leaderBot = 0, player = 0;
    WovDungeon const* dg = nullptr;
    time_t start = 0, nextInvite = 0;
    uint32 invited = 0;
};
struct WovReply
{
    time_t due = 0;
    uint32 bot = 0, player = 0;
    std::string text;
    bool invitePlayer = false;
    WovDungeon const* dg = nullptr;
};
struct WovChatEvt
{
    uint32 player = 0;
    std::string msg;
};
std::vector<WovParty> g_wovParties;
std::vector<WovReply> g_wovReplies;
std::vector<WovChatEvt> g_wovChatQueue;
std::unordered_map<uint32, time_t> g_wovLastLfReply;
std::unordered_map<uint32, uint32> g_wovBusyBot;

char const* const kWovTank[] = {"tank here, inv", "can tank, inv pls", "{lvl} warrior, can tank", "inv, tank", "tank inv?"};
char const* const kWovHeal[] = {"healer here, inv", "can heal, inv", "{lvl} {cls} heals, inv pls", "inv heals", "heals here"};
char const* const kWovDps[] = {"inv dps", "{lvl} {cls} dps inv?", "inv pls", "dps here, inv", "can i come? {cls} {lvl}", "inv"};
char const* const kWovLead[] = {"need {role}? doing {dg}, sent inv", "we need a {role} for {dg}, inv sent", "inv for {dg}?",
                                "{dg}? sent u an inv"};
char const* const kWovHand[] = {"u lead pls, havent done {dg} in ages", "ok we're full, u lead", "full, lead the way",
                                "u lead? i'll follow", "gogo, u lead", "made u lead, lets go"};

int WovRoleOf(Player* p)
{
    // wov: the role its talents actually give it (a holy paladin heals, a fury warrior is dps)
    if (PlayerbotAI::IsTank(p, true))
        return 0;
    if (PlayerbotAI::IsHeal(p, true))
        return 1;
    return 2;
}

char const* WovClassName(Player* p)
{
    switch (p->getClass())
    {
        case CLASS_WARRIOR: return "warrior";
        case CLASS_PALADIN: return "pala";
        case CLASS_HUNTER: return "hunter";
        case CLASS_ROGUE: return "rogue";
        case CLASS_PRIEST: return "priest";
        case CLASS_SHAMAN: return "shaman";
        case CLASS_MAGE: return "mage";
        case CLASS_WARLOCK: return "lock";
        case CLASS_DRUID: return "druid";
        default: return "dps";
    }
}

WovDungeon const* WovDungeonByName(char const* n)
{
    for (WovDungeon const& d : kWovDungeons)
        if (!strcmp(d.name, n))
            return &d;
    return nullptr;
}

std::vector<std::string> WovTokens(std::string const& m)
{
    std::vector<std::string> t;
    std::string cur;
    for (char c : m)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
            cur += char(std::tolower(static_cast<unsigned char>(c)));
        else if (!cur.empty())
        {
            t.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty())
        t.push_back(cur);
    return t;
}

WovDungeon const* WovParseDungeon(std::vector<std::string> const& tok, uint8 level)
{
    static std::pair<char const*, char const*> const al[] = {
        {"rfc", "RFC"}, {"ragefire", "RFC"}, {"deadmines", "deadmines"}, {"vc", "deadmines"}, {"wc", "WC"},
        {"wailing", "WC"}, {"sfk", "SFK"}, {"shadowfang", "SFK"}, {"bfd", "BFD"}, {"blackfathom", "BFD"},
        {"stocks", "stockades"}, {"stockade", "stockades"}, {"stockades", "stockades"}, {"gnomer", "gnomer"},
        {"gnomeregan", "gnomer"}, {"rfk", "RFK"}, {"kraul", "RFK"}, {"gy", "SM GY"}, {"graveyard", "SM GY"},
        {"lib", "SM lib"}, {"library", "SM lib"}, {"arm", "SM arm"}, {"armory", "SM arm"}, {"cath", "SM cath"},
        {"cathedral", "SM cath"}, {"rfd", "RFD"}, {"downs", "RFD"}, {"ulda", "uldaman"}, {"uldaman", "uldaman"},
        {"zf", "ZF"}, {"mara", "mara"}, {"maraudon", "mara"}, {"st", "ST"}, {"sunken", "ST"}, {"brd", "BRD"},
        {"lbrs", "LBRS"}, {"ubrs", "UBRS"}, {"strat", "strat"}, {"stratholme", "strat"}, {"scholo", "scholo"},
        {"scholomance", "scholo"}};
    for (std::string const& w : tok)
    {
        if (w == "dm")
            return WovDungeonByName(level >= 45 ? "DM east" : "deadmines");
        if (w == "sm")
            return WovDungeonByName(level < 33 ? "SM GY" : level < 36 ? "SM lib" : level < 40 ? "SM arm" : "SM cath");
        for (auto const& [k, v] : al)
            if (w == k)
                return WovDungeonByName(v);
    }
    return nullptr;
}

int WovIntent(std::vector<std::string> const& tok)
{
    for (size_t i = 0; i < tok.size(); ++i)
    {
        std::string const& w = tok[i];
        if (w == "lfm" || (w.size() == 4 && w[0] == 'l' && w[1] == 'f' && std::isdigit(static_cast<unsigned char>(w[2])) && w[3] == 'm'))
            return 1;
        if (w == "lfg" || (w == "lf" && i + 1 < tok.size() && (tok[i + 1] == "group" || tok[i + 1] == "grp")))
            return 2;
    }
    return 0;
}

bool WovFreeBot(Player* bot, TeamId team, time_t now)
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->GetTeamId() != team || bot->GetGroup() || bot->InBattleground())
        return false;
    if (g_wovBotSk.count(bot->GetGUID().GetCounter()))
        return false;
    auto it = g_wovBusyBot.find(bot->GetGUID().GetCounter());
    return it == g_wovBusyBot.end() || now >= time_t(it->second);
}

bool WovLevelFits(Player* bot, WovDungeon const* dg, uint8 around)
{
    uint8 l = bot->GetLevel();
    return l + 1 >= dg->minL && l <= dg->maxL + 2 && std::abs(int(l) - int(around)) <= 6;
}

std::string WovRoleLine(Player* bot)
{
    int r = WovRoleOf(bot);
    std::string t = r == 0 ? WovPick(kWovTank) : r == 1 ? WovPick(kWovHeal) : WovPick(kWovDps);
    t = WovRep(t, "{lvl}", std::to_string(bot->GetLevel()));
    return WovRep(t, "{cls}", WovClassName(bot));
}

void WovHandleChat(WovChatEvt const& e, time_t now)
{
    Player* p = ObjectAccessor::FindPlayerByLowGUID(e.player);
    if (!p || !p->IsInWorld() || sRandomPlayerbotMgr.IsRandomBot(p))
        return;
    std::vector<std::string> tok = WovTokens(e.msg);
    int intent = WovIntent(tok);
    if (!intent)
        return;
    time_t& last = g_wovLastLfReply[e.player];
    if (now < last + 90)
        return;
    last = now;
    WovDungeon const* dg = WovParseDungeon(tok, p->GetLevel());
    if (!dg)
        dg = WovDungeonFor(p);
    if (!dg)
        return;
    bool wantRole[3] = {false, false, false};
    for (std::string const& w : tok)
    {
        if (w == "tank")
            wantRole[0] = true;
        if (w == "heal" || w == "heals" || w == "healer" || w == "healz")
            wantRole[1] = true;
        if (w == "dps" || w == "dd")
            wantRole[2] = true;
    }
    bool anyRole = !wantRole[0] && !wantRole[1] && !wantRole[2];

    std::vector<Player*> cands;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
        if (WovFreeBot(bot, p->GetTeamId(), now) && WovLevelFits(bot, dg, p->GetLevel()))
            cands.push_back(bot);
    std::shuffle(cands.begin(), cands.end(), std::mt19937(std::random_device{}()));

    if (intent == 1)
    {
        uint32 freeSlots = p->GetGroup() ? (p->GetGroup()->IsFull() ? 0 : 5 - p->GetGroup()->GetMembersCount()) : 4;
        uint32 want = std::min<uint32>(freeSlots + 1, 4);
        uint32 sent = 0;
        for (Player* bot : cands)
        {
            if (sent >= want)
                break;
            if (!anyRole && !wantRole[WovRoleOf(bot)])
                continue;
            g_wovReplies.push_back({now + time_t(urand(3, 25)), bot->GetGUID().GetCounter(), e.player, WovRoleLine(bot), false, dg});
            g_wovBusyBot[bot->GetGUID().GetCounter()] = uint32(now + 300);
            ++sent;
        }
        LOG_INFO("playerbots", "WoV together: {} LFM {} -> {} answers", p->GetName(), dg->name, sent);
    }
    else if (!p->GetGroup())
    {
        for (Player* bot : cands)
        {
            std::string t = WovRep(WovRep(WovPick(kWovLead), "{dg}", dg->name), "{role}",
                                   WovRoleOf(p) == 0 ? "tank" : WovRoleOf(p) == 1 ? "healer" : "dps");
            g_wovReplies.push_back({now + time_t(urand(4, 15)), bot->GetGUID().GetCounter(), e.player, t, true, dg});
            g_wovBusyBot[bot->GetGUID().GetCounter()] = uint32(now + 600);
            LOG_INFO("playerbots", "WoV together: {} LFG {} -> {} forms a group", p->GetName(), dg->name, bot->GetName());
            break;
        }
    }
}

void WovInviteFrom(Player* inviter, Player* who)
{
    WorldPacket pk;
    pk << who->GetName();
    pk << uint32(0);
    inviter->GetSession()->HandleGroupInviteOpcode(pk);
}

void WovTrackParty(uint32 botLow, uint32 playerLow, WovDungeon const* dg, time_t now)
{
    for (WovParty const& p : g_wovParties)
        if (p.leaderBot == botLow)
            return;
    g_wovParties.push_back({botLow, playerLow, dg, now, now + 5, 0});
}

void WovTogether_Tick(time_t now)
{
    if (!g_wovInitEnable)
        return;
    std::vector<WovChatEvt> evts;
    {
        std::lock_guard<std::mutex> lock(g_wovInitMutex);
        evts.swap(g_wovChatQueue);
    }
    for (WovChatEvt const& e : evts)
        WovHandleChat(e, now);

    std::vector<std::tuple<uint32, uint32, WovDungeon const*>> joins;
    {
        std::lock_guard<std::mutex> lock(g_wovInitMutex);
        joins.swap(g_wovJoinTrack);
    }
    for (auto const& [b, pl, d] : joins)
        WovTrackParty(b, pl, d, now);

    for (size_t i = 0; i < g_wovReplies.size();)
    {
        WovReply r = g_wovReplies[i];
        if (now < r.due)
        {
            ++i;
            continue;
        }
        g_wovReplies.erase(g_wovReplies.begin() + i);
        Player* bot = ObjectAccessor::FindPlayerByLowGUID(r.bot);
        Player* p = ObjectAccessor::FindPlayerByLowGUID(r.player);
        if (!bot || !p || bot->GetGroup())
            continue;
        bot->Whisper(r.text, LANG_UNIVERSAL, p);
        if (r.invitePlayer && !p->GetGroup())
        {
            WovInviteFrom(bot, p);
            WovTrackParty(r.bot, r.player, r.dg, now);
        }
    }

    for (size_t i = 0; i < g_wovParties.size();)
    {
        WovParty& w = g_wovParties[i];
        Player* leader = ObjectAccessor::FindPlayerByLowGUID(w.leaderBot);
        Player* p = ObjectAccessor::FindPlayerByLowGUID(w.player);
        Group* g = leader ? leader->GetGroup() : nullptr;
        bool joined = g && p && g->IsMember(p->GetGUID());
        if (!leader || !p || (!joined && now > w.start + 120) || (g && !g->IsLeader(leader->GetGUID())))
        {
            g_wovParties.erase(g_wovParties.begin() + i);
            continue;
        }
        if (!joined)
        {
            ++i;
            continue;
        }
        uint32 members = g->GetMembersCount();
        if (members < 5 && w.invited < 6 && now >= w.nextInvite && now <= w.start + 150)
        {
            bool have[3] = {false, false, false};
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                    have[WovRoleOf(m)] = true;
            int need = !have[0] ? 0 : !have[1] ? 1 : 2;
            if (w.dg)
                for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
                {
                    if (!WovFreeBot(bot, p->GetTeamId(), now) || !WovLevelFits(bot, w.dg, p->GetLevel()) || WovRoleOf(bot) != need)
                        continue;
                    WovInviteFrom(leader, bot);
                    g_wovBusyBot[bot->GetGUID().GetCounter()] = uint32(now + 600);
                    break;
                }
            ++w.invited;
            w.nextInvite = now + urand(5, 12);
            ++i;
            continue;
        }
        if (members >= 5 || now > w.start + 150)
        {
            g->ChangeLeader(p->GetGUID());
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            {
                Player* m = ref->GetSource();
                PlayerbotAI* ai = m ? GET_PLAYERBOT_AI(m) : nullptr;
                if (!ai)
                    continue;
                ai->SetMaster(p);
                ai->ResetStrategies();
                ai->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
                ai->Reset();
                if (m->GetMapId() != p->GetMapId() || m->GetExactDist(p) > 250.0f)
                    m->TeleportTo(p->GetMapId(), p->GetPositionX() + frand(-3.0f, 3.0f), p->GetPositionY() + frand(-3.0f, 3.0f),
                                  p->GetPositionZ(), p->GetOrientation());
            }
            if (PlayerbotAI* lai = GET_PLAYERBOT_AI(leader))
                lai->SayToParty(WovRep(WovPick(kWovHand), "{dg}", w.dg ? w.dg->name : "this"));
            LOG_INFO("playerbots", "WoV together: group of {} handed to {} for {}", members, p->GetName(), w.dg ? w.dg->name : "?");
            g_wovParties.erase(g_wovParties.begin() + i);
            continue;
        }
        ++i;
    }
}
}  // namespace

void WovTogether_OnChannel(Player* player, std::string const& msg)
{
    if (!g_wovInitEnable || !player || msg.size() > 255 || sRandomPlayerbotMgr.IsRandomBot(player))
        return;
    std::lock_guard<std::mutex> lock(g_wovInitMutex);
    g_wovChatQueue.push_back({player->GetGUID().GetCounter(), msg});
}
// ---- end wov: play together ----------------------------------------------------------------------------

// ---- wov: dungeon talk --------------------------------------------------------------------------------
// Short party lines a pug says that no model is needed for: a sorry when a bot's need beats a real player's,
// gz on a real player's blue, "wipe" and "run back?" after a wipe, "r"/"gogo" on zoning in. Only in groups with
// a real player in them, with chances and cooldowns, so it reads like people and not like a feed.
#include "Group.h"
#include <unordered_set>
namespace
{
struct WovRollEvt
{
    uint32 winner = 0, item = 0;
    uint8 vote = 0;
    std::vector<std::pair<uint32, uint8>> votes;  // (low guid, RollVote)
};
struct WovPartyLine
{
    time_t due = 0;
    uint32 bot = 0;
    std::string text;
};
std::mutex g_wovDgMutex;
std::vector<WovRollEvt> g_wovRollEvts;
std::vector<WovPartyLine> g_wovPartyLines;
std::unordered_map<uint32, time_t> g_wovWipeCd;    // group low guid -> next allowed
std::unordered_map<uint64, time_t> g_wovZoneSeen;  // (player low << 32 | instance id) -> seen until
time_t g_wovDgNext = 0;

char const* const kWovSorry[] = {"sry, was an upgrade for me", "oh sry, big upgrade", "sry man", "sry, needed that one",
                                 "oops sry, upgrade", "sry, ill pass the next one", "my bad, was an upgrade tho"};
char const* const kWovMine[] = {"nice", "finally", "yes!!", "omg finally", "sweet", "been farming that lol", "ty ty"};
char const* const kWovGz[] = {"gz", "grats", "gz!", "nice gz", "gz, nice drop", "grats man", "gz gz"};
char const* const kWovUnlucky[] = {"unlucky", "rip", "next time", "unlucky man"};
char const* const kWovWipe[] = {"wipe", "welp", "rip", "lol wipe", "rip us", "oops", "that went well", "gg"};
char const* const kWovAfter[] = {"run back?", "who pulled that lol", "ok run back", "rez up and rebuff",
                                 "pull slower pls", "lets wait for mana next time", "my bad", "again?", "brb corpse run"};
char const* const kWovEnter[] = {"r", "ready", "gogo", "lets go", "1 sec drinking", "pull when ready", "buffs?",
                                 "need to repair after this lol", "havent been here in ages", "r, gogo"};

bool WovRealIn(Group* g)
{
    if (!g)
        return false;
    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
        if (Player* m = ref->GetSource())
            if (m->GetSession() && !GET_PLAYERBOT_AI(m) && !sRandomPlayerbotMgr.IsRandomBot(m))
                return true;
    return false;
}

bool WovIsReal(Player* m) { return m && m->GetSession() && !GET_PLAYERBOT_AI(m) && !sRandomPlayerbotMgr.IsRandomBot(m); }

void WovPartySay(Player* bot, std::string const& text, time_t due)
{
    if (bot && sRandomPlayerbotMgr.IsRandomBot(bot))
        g_wovPartyLines.push_back({due, bot->GetGUID().GetCounter(), text});
}

// a random bot of the group other than `not1`, or nullptr
Player* WovOtherBot(Group* g, Player* not1)
{
    std::vector<Player*> c;
    for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
        if (Player* m = ref->GetSource())
            if (m != not1 && m->IsInWorld() && sRandomPlayerbotMgr.IsRandomBot(m))
                c.push_back(m);
    return c.empty() ? nullptr : c[urand(0, c.size() - 1)];
}

void WovDgRoll(WovRollEvt const& e, time_t now)
{
    Player* w = ObjectAccessor::FindPlayerByLowGUID(e.winner);
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(e.item);
    Group* g = w ? w->GetGroup() : nullptr;
    if (!w || !proto || !g || !WovRealIn(g) || proto->Quality < ITEM_QUALITY_UNCOMMON)
        return;
    time_t due = now + urand(2, 6);
    if (sRandomPlayerbotMgr.IsRandomBot(w))
    {
        bool realNeeded = false;
        for (auto const& [low, v] : e.votes)
            if (v == NEED && low != e.winner && WovIsReal(ObjectAccessor::FindPlayerByLowGUID(low)))
                realNeeded = true;
        if (e.vote == NEED && realNeeded)
        {
            if (urand(0, 99) < 75)
                WovPartySay(w, WovPick(kWovSorry), due);
            if (urand(0, 99) < 20)
                WovPartySay(WovOtherBot(g, w), WovPick(kWovUnlucky), due + urand(2, 5));
        }
        else if (proto->Quality >= ITEM_QUALITY_RARE)
        {
            if (urand(0, 99) < 50)
                WovPartySay(w, WovPick(kWovMine), due);
            if (urand(0, 99) < 40)
                WovPartySay(WovOtherBot(g, w), WovPick(kWovGz), due + urand(2, 6));
        }
        else if (e.vote == NEED && urand(0, 99) < 10)
            WovPartySay(w, WovPick(kWovMine), due);
    }
    else if (WovIsReal(w))
    {
        uint32 chance = proto->Quality >= ITEM_QUALITY_RARE ? 80 : e.vote == NEED ? 25 : 0;
        if (urand(0, 99) < chance)
            WovPartySay(WovOtherBot(g, nullptr), WovPick(kWovGz), due);
        if (proto->Quality >= ITEM_QUALITY_EPIC && urand(0, 99) < 60)
            WovPartySay(WovOtherBot(g, nullptr), WovPick(kWovGz), due + urand(2, 5));
    }
    LOG_INFO("playerbots", "WoV dungeon: roll {} won by {} ({})", proto->Name1, w->GetName(), e.vote == NEED ? "need" : "greed");
}

void WovDgScan(time_t now)
{
    std::unordered_set<Group*> done;
    for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
    {
        if (!p || !p->IsInWorld() || !WovIsReal(p))
            continue;
        Group* g = p->GetGroup();
        Map* map = p->GetMap();
        if (!g || !map || !map->IsDungeon() || map->IsBattlegroundOrArena() || g->isRaidGroup() || !done.insert(g).second)
            continue;
        uint32 here = 0, dead = 0, bots = 0;
        for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
            if (Player* m = ref->GetSource())
                if (m->IsInWorld() && m->GetMap() == map)
                {
                    ++here;
                    dead += m->IsAlive() ? 0 : 1;
                    bots += sRandomPlayerbotMgr.IsRandomBot(m) ? 1 : 0;
                }
        if (!bots)
            continue;
        uint32 gl = g->GetGUID().GetCounter();
        if (here >= 3 && dead == here && now >= g_wovWipeCd[gl])
        {
            g_wovWipeCd[gl] = now + 240;
            Player* a = WovOtherBot(g, nullptr);
            if (urand(0, 99) < 85)
                WovPartySay(a, WovPick(kWovWipe), now + urand(2, 5));
            if (urand(0, 99) < 60)
                WovPartySay(WovOtherBot(g, a), WovPick(kWovAfter), now + urand(10, 22));
            LOG_INFO("playerbots", "WoV dungeon: wipe in {} (group of {}, {} with {})", map->GetMapName(), here,
                     p->GetName(), bots);
        }
        uint64 key = (uint64(p->GetGUID().GetCounter()) << 32) | map->GetInstanceId();
        auto it = g_wovZoneSeen.find(key);
        bool fresh = it == g_wovZoneSeen.end() || now >= it->second;
        g_wovZoneSeen[key] = now + 1800;
        if (fresh && bots >= 2 && p->IsAlive() && urand(0, 99) < 55)
            WovPartySay(WovOtherBot(g, nullptr), WovPick(kWovEnter), now + urand(5, 12));
    }
    for (auto it = g_wovZoneSeen.begin(); it != g_wovZoneSeen.end();)
        it = now >= it->second ? g_wovZoneSeen.erase(it) : std::next(it);
    for (auto it = g_wovWipeCd.begin(); it != g_wovWipeCd.end();)
        it = now >= it->second ? g_wovWipeCd.erase(it) : std::next(it);
}
}  // namespace

void WovDungeon_OnRoll(Player* winner, uint32 item, uint8 vote, Roll* roll)
{
    if (!g_wovInitEnable || !winner || !roll)
        return;
    WovRollEvt e;
    e.winner = winner->GetGUID().GetCounter();
    e.item = item;
    e.vote = vote;
    for (auto const& [g, v] : roll->playerVote)
        e.votes.push_back({uint32(g.GetCounter()), uint8(v)});
    std::lock_guard<std::mutex> lock(g_wovDgMutex);
    if (g_wovRollEvts.size() < 64)
        g_wovRollEvts.push_back(std::move(e));
}

void WovDungeon_Tick(time_t now)
{
    if (!g_wovInitEnable)
        return;
    std::vector<WovRollEvt> evts;
    {
        std::lock_guard<std::mutex> lock(g_wovDgMutex);
        evts.swap(g_wovRollEvts);
    }
    for (WovRollEvt const& e : evts)
        WovDgRoll(e, now);
    if (now >= g_wovDgNext)
    {
        g_wovDgNext = now + 2;
        WovDgScan(now);
    }
    for (size_t i = 0; i < g_wovPartyLines.size();)
    {
        if (now < g_wovPartyLines[i].due)
        {
            ++i;
            continue;
        }
        WovPartyLine l = g_wovPartyLines[i];
        g_wovPartyLines.erase(g_wovPartyLines.begin() + i);
        Player* bot = ObjectAccessor::FindPlayerByLowGUID(l.bot);
        PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (ai && bot->IsInWorld() && bot->GetGroup())
            ai->SayToParty(l.text);
    }
}
// ---- end wov: dungeon talk ----------------------------------------------------------------------------

// ---- wov: dungeon runs ----------------------------------------------------------------------------------
// Bots run dungeons on their own, the way a pug does: a tank picks a dungeon that fits its level near where it
// plays, a healer and three dps of the right level and faction who are around answer, everyone walks (or flies)
// to the entrance, the party forms at the stone, walks through the portal and the tank leads the clear
// (mod-dungeon-clear). When the last boss is dead they say thanks and go their own way. No teleports except the
// instance portal itself, no summons, nothing a player could not do.
#include "MapMgr.h"
namespace
{
struct WovDgDef
{
    char const* name;
    uint32 trigger;  // areatrigger_teleport id of the outdoor entrance
    uint8 minL, maxL;
    int team;  // -1 both, 0 alliance, 1 horde
};
WovDgDef const kWovDgDefs[] = {
    {"RFC", 2230, 13, 18, 1},       {"WC", 228, 18, 25, -1},     {"deadmines", 78, 19, 26, 0},
    {"SFK", 145, 22, 30, -1},       {"BFD", 257, 24, 32, -1},    {"stockades", 101, 24, 32, 0},
    {"RFK", 244, 29, 38, -1},       {"gnomer", 324, 29, 38, -1}, {"RFD", 442, 37, 46, -1},
    {"mara", 3134, 46, 55, -1},     {"ST", 446, 50, 56, -1},     {"scholo", 2567, 58, 60, -1},
    {"strat", 2214, 58, 60, -1}};
struct WovDgSpot
{
    WovDgDef const* def = nullptr;
    uint32 map = 0, dmap = 0;
    float x = 0, y = 0, z = 0, dx = 0, dy = 0, dz = 0, dO = 0;
    // where the party meets: the stone itself, or dry land next to it when the entrance is under water (BFD)
    mutable float mx = 0, my = 0, mz = 0;
    mutable bool meetSet = false;
};
enum WovDgPhase : uint8 { DG_GATHER, DG_ENTER, DG_RUN, DG_DONE };
struct WovDgMember
{
    uint32 low = 0;
    float lastDist = 1e9f;  // unused (kept for layout)
    time_t lastProgress = 0;
    bool flew = false;
    float px = 0, py = 0;  // last position that counted as progress
    time_t joined = 0;
    time_t closerT = 0;  // last time it got 30 yd closer to the stone
    time_t deadT = 0;    // dead since (on the way)
    time_t outT = 0;     // alive but outside the instance since (revived at the graveyard or at the portal)
};
struct WovDgRunState
{
    uint32 id = 0;
    WovDgSpot const* spot = nullptr;
    std::vector<WovDgMember> m;  // m[0] is the tank
    WovDgPhase phase = DG_GATHER;
    time_t start = 0, phaseT = 0, tankThere = 0;
    bool finalKilled = false;
    float tx = 0, ty = 0;               // tank position the stall watchdog compares with
    time_t moveT = 0, logT = 0, rezT = 0;
    uint8 stalls = 0, retries = 0;
    bool swum = false;
    uint32 real = 0;          // a real player who answered the tank's LFM (takes the last dps spot)
    time_t realT = 0, lfmT = 0, limboT = 0;
    uint8 lfmN = 0;
};
std::vector<WovDgSpot> g_wovDgSpots;
std::vector<WovDgRunState> g_wovDgRuns;
std::unordered_map<uint32, time_t> g_wovDgDropped;  // replaced for not showing up: not picked again for a while
std::unordered_set<uint32> g_wovDgFinal;  // creature entries of last bosses
std::unordered_set<uint32> g_wovDgEnabled;  // enabled entrance triggers (config)
std::mutex g_wovDgKillMutex;
std::vector<std::pair<uint32, uint32>> g_wovDgKills;  // (killer low, creature entry), from the kill hook
std::vector<std::pair<uint32, uint32>> g_wovDgJoins;  // (bot low, real player low): whispers to bots, same mutex
bool g_wovDgEnable = false;
uint32 g_wovDgMax = 6, g_wovDgEvery = 120, g_wovDgNextId = 1;
float g_wovDgTankRange = 2600.0f, g_wovDgMemberRange = 2300.0f;
time_t g_wovDgNextStart = 0, g_wovDgCfgT = 0;

char const* const kWovDgOmw[] = {"omw", "coming", "on my way", "otw", "omw, 2 min", "k coming"};
char const* const kWovDgBye[] = {"ty for group", "gg ty", "ty all", "thx for the run", "gg", "ty gl", "nice run ty", "cya"};
char const* const kWovDgThanks[] = {"gj all", "ez", "gg", "nice", "that went well", "gz on loot"};

bool WovOnMarketTrip(uint32 low);  // wov: market (below)

void WovDgConfig(time_t now)
{
    if (now < g_wovDgCfgT + 60)
        return;
    g_wovDgCfgT = now;
    g_wovDgEnable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.DgRuns.Enable", false);
    g_wovDgMax = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.DgRuns.MaxActive", 6);
    g_wovDgEvery = std::max<uint32>(30, sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.DgRuns.StartEverySec", 120));
    g_wovDgTankRange = sConfigMgr->GetOption<float>("AiPlayerbot.WoV.DgRuns.TankRange", 2600.0f);
    g_wovDgMemberRange = sConfigMgr->GetOption<float>("AiPlayerbot.WoV.DgRuns.MemberRange", 2300.0f);
    std::string list = sConfigMgr->GetOption<std::string>("AiPlayerbot.WoV.DgRuns.Triggers", "2230,228,78,145,257,101,244,324,442");
    g_wovDgEnabled.clear();
    std::string cur;
    for (char ch : list + ",")
        if (std::isdigit(static_cast<unsigned char>(ch)))
            cur += ch;
        else if (!cur.empty())
        {
            g_wovDgEnabled.insert(uint32(std::strtoul(cur.c_str(), nullptr, 10)));
            cur.clear();
        }
    if (g_wovDgSpots.empty())
    {
        for (WovDgDef const& d : kWovDgDefs)
        {
            AreaTrigger const* at = sObjectMgr->GetAreaTrigger(d.trigger);
            AreaTriggerTeleport const* tp = sObjectMgr->GetAreaTriggerTeleport(d.trigger);
            if (!at || !tp)
                continue;
            WovDgSpot s;
            s.def = &d;
            s.map = at->map;
            s.x = at->x;
            s.y = at->y;
            s.z = at->z;
            s.dmap = tp->target_mapId;
            s.dx = tp->target_X;
            s.dy = tp->target_Y;
            s.dz = tp->target_Z;
            s.dO = tp->target_Orientation;
            s.mx = s.x;
            s.my = s.y;
            s.mz = s.z;
            g_wovDgSpots.push_back(s);
        }
        if (QueryResult r = WorldDatabase.Query("SELECT creditEntry FROM instance_encounters WHERE creditType=0 AND lastEncounterDungeon>0"))
            do
                g_wovDgFinal.insert(r->Fetch()[0].Get<uint32>());
            while (r->NextRow());
        LOG_INFO("playerbots", "WoV dungeon runs: {} entrances, {} last bosses", g_wovDgSpots.size(), g_wovDgFinal.size());
    }
}

bool WovDgBusy(uint32 low)
{
    for (WovDgRunState const& r : g_wovDgRuns)
        for (WovDgMember const& mm : r.m)
            if (mm.low == low)
                return true;
    return false;
}

bool WovDgFree(Player* b, TeamId team, time_t now)
{
    if (!WovFreeBot(b, team, now) || b->InBattlegroundQueue() || b->IsInCombat() || b->IsInFlight())
        return false;
    if (!b->GetMap() || b->GetMap()->Instanceable() || WovOnMarketTrip(b->GetGUID().GetCounter()) ||
        WovDgBusy(b->GetGUID().GetCounter()) || WovRaidNightFor(b->GetGUID().GetCounter()))  // raid night: not now
        return false;
    auto dr = g_wovDgDropped.find(b->GetGUID().GetCounter());
    if (dr != g_wovDgDropped.end() && now < dr->second)
        return false;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(b);
    return ai && !ai->GetMaster();
}

bool WovDgFits(Player* b, WovDgDef const* d, uint8 around)
{
    uint8 l = b->GetLevel();
    return l >= d->minL && l <= d->maxL && std::abs(int(l) - int(around)) <= 5;
}

// an entrance under water: the party waits on the nearest dry ground and swims in together (waiting at the stone
// itself drowns them). Needs the grid loaded, so it is worked out when the first member is close.
void WovDgMeetFix(WovDgSpot const* s, Player* near)
{
    s->meetSet = true;
    Map* m = near->GetMap();
    uint32 ph = near->GetPhaseMask();
    float wl = m->GetWaterLevel(s->x, s->y);
    bool wet = m->IsInWater(ph, s->x, s->y, s->z + 0.5f, 2.0f) || (wl > INVALID_HEIGHT && wl > s->z + 1.0f);
    if (!wet)
        return;
    for (float rad = 15.0f; rad <= 220.0f; rad += 5.0f)
    {
        float bestZ = INVALID_HEIGHT, bx = 0, by = 0;
        for (int a = 0; a < 32; ++a)
        {
            float ang = float(a) * float(M_PI) / 16.0f;
            float x = s->x + rad * std::cos(ang), y = s->y + rad * std::sin(ang);
            float gz = m->GetHeight(ph, x, y, s->z + 80.0f, true, 120.0f);
            if (gz <= INVALID_HEIGHT || std::fabs(gz - s->z) > 60.0f || m->IsInWater(ph, x, y, gz + 0.5f, 2.0f))
                continue;
            float lw = m->GetWaterLevel(x, y);
            if (lw > INVALID_HEIGHT && lw > gz + 0.3f)
                continue;
            if (bestZ <= INVALID_HEIGHT || gz < bestZ)  // the lowest dry spot: the shore, not a hill top
            {
                bestZ = gz;
                bx = x;
                by = y;
            }
        }
        if (bestZ > INVALID_HEIGHT)
        {
            s->mx = bx;
            s->my = by;
            s->mz = bestZ;
            LOG_INFO("playerbots", "WoV dungeon runs: {} entrance is under water, the party meets on the shore {:.0f} yd away ({:.0f},{:.0f},{:.0f})",
                     s->def->name, rad, bx, by, bestZ);
            return;
        }
    }
    LOG_INFO("playerbots", "WoV dungeon runs: {} entrance is under water and no dry ground was found near it", s->def->name);
}

void WovDgGoTo(Player* b, PlayerbotAI* ai, WovDgSpot const* s, WovDgMember& mm, time_t now)
{
    float d = b->GetExactDist(s->mx, s->my, s->mz);
    if (b->GetMapId() != s->map)
        return;
    if (false && d > 900.0f && !mm.flew && !b->IsInFlight())  // wov: walking proved more reliable than the taxi flow
    {
        mm.flew = true;
        TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(b);
        uint32 to = sObjectMgr->GetNearestTaxiNode(s->mx, s->my, s->mz, s->map, b->GetTeamId());
        if (fm && fm->taxiNodeId && to && fm->taxiNodeId != to)
        {
            std::vector<uint32> path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, to);
            if (!path.empty())
            {
                ai->rpgInfo.ChangeToTravelFlight(fm->templateEntry, fm->pos, path);
                return;
            }
        }
    }
    if (b->IsInFlight())
        return;  // already on a taxi: let it land
    if (d <= 12.0f)
    {
        if (ai->rpgInfo.GetStatus() != RPG_REST)
            ai->rpgInfo.ChangeToRest();
        return;
    }
    auto* gc = std::get_if<NewRpgInfo::GoCamp>(&ai->rpgInfo.data);
    if (!gc || gc->pos.distance(WorldPosition(s->map, s->mx, s->my, s->mz)) > 5.0f)
        ai->rpgInfo.ChangeToGoCamp(WorldPosition(s->map, s->mx, s->my, s->mz));
}

void WovDgRelease(WovDgRunState& r, char const* why, bool sayBye)
{
    uint32 said = 0;
    for (WovDgMember const& mm : r.m)
    {
        Player* b = ObjectAccessor::FindPlayerByLowGUID(mm.low);
        PlayerbotAI* ai = b ? GET_PLAYERBOT_AI(b) : nullptr;
        if (!ai)
            continue;
        if (sayBye && b->GetGroup() && said < 2 && urand(0, 99) < 60)
        {
            ai->SayToParty(WovPick(kWovDgBye));
            ++said;
        }
        if (b->GetGroup())
            ai->LeaveOrDisbandGroup();
        ai->ResetStrategies();  // back to its own spec and normal life
        ai->rpgInfo.ChangeToIdle();
    }
    LOG_INFO("playerbots", "WoV dungeon run #{} {}: {}", r.id, r.spot->def->name, why);
}

// in a levelling pug any warrior tanks and any priest heals, whatever the talents
bool WovDgCanTank(Player* b) { return b->getClass() == CLASS_WARRIOR || PlayerbotAI::IsTank(b, true); }
bool WovDgCanHeal(Player* b) { return b->getClass() == CLASS_PRIEST || PlayerbotAI::IsHeal(b, true); }

// the pug roles for this run: a warrior tanks in defensive stance whatever its talents, a priest heals
void WovDgRoles(std::vector<Player*> const& ps)
{
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(ps[0]))
        if (!PlayerbotAI::IsTank(ps[0]) && ps[0]->getClass() == CLASS_WARRIOR)
            ai->ChangeStrategy("-arms,-fury,-dps assist,+tank,+tank assist,+pull,+pull back", BOT_STATE_COMBAT);
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(ps[1]))
        if (!PlayerbotAI::IsHeal(ps[1]))
            switch (ps[1]->getClass())
            {
                case CLASS_PRIEST: ai->ChangeStrategy("-dps,-shadow debuff,-shadow aoe,-dps assist,+heal", BOT_STATE_COMBAT); break;
                case CLASS_PALADIN: ai->ChangeStrategy("-dps,-tank,-tank assist,-bthreat,+heal,+bcast", BOT_STATE_COMBAT); break;
                case CLASS_SHAMAN: ai->ChangeStrategy("-ele,-enh,+resto", BOT_STATE_COMBAT); break;
                case CLASS_DRUID: ai->ChangeStrategy("-cat,-bear,-balance,-tank assist,+resto", BOT_STATE_COMBAT); break;
                default: break;
            }
}

bool WovDgStart(time_t now)
{
    std::vector<Player*> tanks;
    for (auto const& [g, b] : sRandomPlayerbotMgr.GetAllBots())
        if (b && b->IsInWorld() && b->GetLevel() >= 13 && WovDgFree(b, b->GetTeamId(), now) && WovDgCanTank(b))
            tanks.push_back(b);
    std::shuffle(tanks.begin(), tanks.end(), std::mt19937(std::random_device{}()));
    uint32 nSpot = 0, nHeal = 0, nDps = 0;
    for (Player* t : tanks)
    {
        // nearest enabled entrance that fits the tank, on its continent, within reach
        WovDgSpot const* best = nullptr;
        float bestD = g_wovDgTankRange;
        for (WovDgSpot const& s : g_wovDgSpots)
        {
            if (!g_wovDgEnabled.count(s.def->trigger) || s.map != t->GetMapId() || !WovDgFits(t, s.def, t->GetLevel()))
                continue;
            bool taken = false;  // one party at a stone at a time
            for (WovDgRunState const& o : g_wovDgRuns)
                taken = taken || o.spot == &s;
            if (taken)
                continue;
            if (s.def->team >= 0 && int(t->GetTeamId()) != s.def->team)
                continue;
            float d = t->GetExactDist(s.x, s.y, s.z);
            if (d < bestD)
            {
                bestD = d;
                best = &s;
            }
        }
        if (!best)
            continue;
        ++nSpot;
        Player* heal = nullptr;
        std::vector<Player*> dps;
        std::vector<std::pair<float, Player*>> cands;
        for (auto const& [g, b] : sRandomPlayerbotMgr.GetAllBots())
        {
            if (!b || b == t || !b->IsInWorld() || b->GetMapId() != best->map || !WovDgFits(b, best->def, t->GetLevel()) ||
                !WovDgFree(b, t->GetTeamId(), now))
                continue;
            float d = b->GetExactDist(best->x, best->y, best->z);
            if (d > g_wovDgMemberRange)
                continue;
            if (b->GetGuildId() && b->GetGuildId() == t->GetGuildId())
                d *= 0.5f;  // guildmates answer first
            cands.push_back({d, b});
        }
        std::sort(cands.begin(), cands.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
        for (auto const& [d, b] : cands)
        {
            if (PlayerbotAI::IsTank(b, true))
                continue;
            if (!heal && WovDgCanHeal(b))
                heal = b;
            else if (!PlayerbotAI::IsHeal(b, true) && b->getClass() != CLASS_PRIEST && dps.size() < 3)
                dps.push_back(b);
            if (heal && dps.size() == 3)
                break;
        }
        // no healer by spec or priest around: a paladin, shaman or druid heals this one
        if (!heal)
            for (auto const& [d, b] : cands)
            {
                uint8 c = b->getClass();
                if ((c == CLASS_PALADIN || c == CLASS_SHAMAN || c == CLASS_DRUID) &&
                    std::find(dps.begin(), dps.end(), b) == dps.end() && !PlayerbotAI::IsTank(b, true))
                {
                    heal = b;
                    break;
                }
            }
        if (heal)
            ++nHeal;
        if (dps.size() >= 3)
            ++nDps;
        if (!heal || dps.size() < 3)
            continue;
        WovDgRunState r;
        r.id = g_wovDgNextId++;
        r.spot = best;
        r.start = r.phaseT = now;
        for (Player* p : std::vector<Player*>{t, heal, dps[0], dps[1], dps[2]})
        {
            r.m.push_back({p->GetGUID().GetCounter(), 1e9f, now, false, 0.0f, 0.0f, now});
            g_wovBusyBot[p->GetGUID().GetCounter()] = uint32(now + 3 * HOUR);
            if (p != t && urand(0, 99) < 35)
                p->Whisper(WovPick(kWovDgOmw), LANG_UNIVERSAL, t);
        }
        LOG_INFO("playerbots", "WoV dungeon run #{} {}: tank {} (lvl {}), heal {}, dps {} {} {}, {:.0f} yd to the stone", r.id,
                 best->def->name, t->GetName(), t->GetLevel(), heal->GetName(), dps[0]->GetName(), dps[1]->GetName(),
                 dps[2]->GetName(), bestD);
        g_wovDgRuns.push_back(r);
        return true;
    }
    LOG_INFO("playerbots", "WoV dungeon runs: no party this time ({} free tanks, {} near an entrance, {} with a healer, {} with 3 dps)",
             tanks.size(), nSpot, nHeal, nDps);
    return false;
}

// a member who does not show up is replaced by someone of the same role close to the stone ("anyone else?")
bool WovDgReplace(WovDgRunState& r, size_t idx, Player* tank, time_t now)
{
    if (idx == 0)
        return false;
    Player* best = nullptr;
    float bestD = g_wovDgMemberRange;
    for (auto const& [g, b] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!b || !b->IsInWorld() || b->GetMapId() != r.spot->map || !WovDgFits(b, r.spot->def, tank->GetLevel()) ||
            !WovDgFree(b, tank->GetTeamId(), now) || PlayerbotAI::IsTank(b, true))
            continue;
        bool healer = WovDgCanHeal(b) || b->getClass() == CLASS_PALADIN || b->getClass() == CLASS_SHAMAN || b->getClass() == CLASS_DRUID;
        if (idx == 1 ? !healer : (PlayerbotAI::IsHeal(b, true) || b->getClass() == CLASS_PRIEST))
            continue;
        float d = b->GetExactDist(r.spot->x, r.spot->y, r.spot->z);
        if (d < bestD)
        {
            bestD = d;
            best = b;
        }
    }
    if (!best)
        return false;
    uint32 old = r.m[idx].low;
    if (Player* o = ObjectAccessor::FindPlayerByLowGUID(old))
        if (PlayerbotAI* oai = GET_PLAYERBOT_AI(o))
            oai->rpgInfo.ChangeToIdle();
    g_wovBusyBot.erase(old);
    g_wovDgDropped[old] = now + 20 * MINUTE;
    r.m[idx] = {best->GetGUID().GetCounter(), 1e9f, now, false, best->GetPositionX(), best->GetPositionY(), now};
    g_wovBusyBot[best->GetGUID().GetCounter()] = uint32(now + 3 * HOUR);
    if (urand(0, 99) < 35)
        best->Whisper(WovPick(kWovDgOmw), LANG_UNIVERSAL, tank);
    LOG_INFO("playerbots", "WoV dungeon run #{} {}: {} replaces a member who did not show up ({:.0f} yd away)", r.id,
             r.spot->def->name, best->GetName(), bestD);
    return true;
}

// a dead member gets back like anyone: a released ghost zones in again, a corpse inside gets up (after the run back)
void WovDgRunBack(WovDgRunState const& r, Player* b)
{
    if (b->IsAlive() || b->IsBeingTeleported())
        return;
    if (b->GetMapId() != r.spot->dmap)
    {
        b->TeleportTo(r.spot->dmap, r.spot->dx + frand(-1.5f, 1.5f), r.spot->dy + frand(-1.5f, 1.5f), r.spot->dz, r.spot->dO);
        return;
    }
    b->ResurrectPlayer(0.5f);
    b->SpawnCorpseBones();
}

// LFM for the run: only when a real player who could come is in the tank's zone (same side, level fits, no group).
// Whoever whispers the tank within 10 minutes gets the last dps spot (WovDungeonRun_OnWhisper).
void WovDgPostLfm(WovDgRunState& r, Player* tank, time_t now)
{
    if (r.real || r.lfmN >= 2 || now < r.start + 20 || now > r.start + 12 * MINUTE || (r.lfmT && now < r.lfmT + 4 * MINUTE))
        return;
    static time_t lastScan = 0;
    if (now < lastScan + 1 && r.lfmT)
        return;
    lastScan = now;
    bool any = false;
    for (auto const& [id, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* p = session ? session->GetPlayer() : nullptr;
        if (!p || !WovIsReal(p) || !p->IsInWorld() || p->GetGroup() || p->GetTeamId() != tank->GetTeamId() ||
            p->GetZoneId() != tank->GetZoneId())
            continue;
        if (p->GetLevel() < r.spot->def->minL || p->GetLevel() > r.spot->def->maxL ||
            std::abs(int(p->GetLevel()) - int(tank->GetLevel())) > 5)
            continue;
        any = true;
        break;
    }
    if (!any)
        return;
    static char const* const lfm[] = {"LF1M {dg}, meet at the stone", "LF1M {dg} pst", "{dg} LF1M then go",
                                      "need 1 more for {dg}, w/ me", "LF1M {dg}, any dps", "LF1M {dg} /w me"};
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(tank))
        ai->SayToChannel(WovRep(lfm[urand(0, 5)], "{dg}", r.spot->def->name), ChatChannelId::GENERAL);
    r.lfmT = now;
    ++r.lfmN;
    LOG_INFO("playerbots", "WoV dungeon run #{} {}: LFM posted in General (a real player is in the zone)", r.id, r.spot->def->name);
}

// returns false when the run is over and should be removed
bool WovDgStep(WovDgRunState& r, time_t now)
{
    std::vector<Player*> ps;
    for (WovDgMember const& mm : r.m)
    {
        // connected, in the world or not (FindPlayerByLowGUID misses a bot that is between maps)
        Player* b = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(mm.low));
        if (!b || !GET_PLAYERBOT_AI(b))
        {
            WovDgRelease(r, "a member logged off", r.phase == DG_RUN);
            return false;
        }
        if (!b->IsInWorld() || b->IsBeingTeleportedFar())
        {
            // between maps (portal, a ghost zoning in): look again next tick, but not for ever
            if (!r.limboT)
                r.limboT = now;
            if (now > r.limboT + 2 * MINUTE)
            {
                WovDgRelease(r, std::string(b->GetName() + " vanished between maps").c_str(), r.phase == DG_RUN);
                return false;
            }
            return true;
        }
        ps.push_back(b);
    }
    r.limboT = 0;
    Player* tank = ps[0];
    PlayerbotAI* tai = GET_PLAYERBOT_AI(tank);
    Player* real = nullptr;
    bool realIn = false;  // in the tank's group
    if (r.real)
    {
        real = ObjectAccessor::FindPlayerByLowGUID(r.real);
        realIn = real && tank->GetGroup() && tank->GetGroup()->IsMember(real->GetGUID());
        if (!real || (!realIn && (r.phase != DG_GATHER || now > r.realT + 2 * MINUTE || real->GetGroup())))
        {
            LOG_INFO("playerbots", "WoV dungeon run #{} {}: the real player is no longer with the run", r.id, r.spot->def->name);
            r.real = 0;
            real = nullptr;
            realIn = false;
        }
    }
    if (now > r.start + 150 * MINUTE)
    {
        WovDgRelease(r, "timed out", true);
        return false;
    }
    if (r.phase == DG_GATHER)
    {
        uint32 here = 0;
        if (!r.spot->meetSet)
            for (Player* b : ps)
                if (b->GetMapId() == r.spot->map && b->GetExactDist(r.spot->x, r.spot->y, r.spot->z) < 250.0f)
                {
                    WovDgMeetFix(r.spot, b);
                    break;
                }
        for (size_t i = 0; i < ps.size(); ++i)
        {
            Player* b = ps[i];
            WovDgMember& mm = r.m[i];
            if (b->GetMapId() != r.spot->map || b->InBattleground() || b->InBattlegroundQueue())
            {
                WovDgRelease(r, std::string(b->GetName() + (b->InBattlegroundQueue() || b->InBattleground() ? " queued for a battleground" : " left the continent")).c_str(), false);
                return false;
            }
            // dead on the way (a mob, a fall, drowning): it runs back like anyone, but not for ever
            if (!b->IsAlive())
            {
                if (!mm.deadT)
                    mm.deadT = now;
                if (now > mm.deadT + 4 * MINUTE)
                {
                    if (i == 0 || !WovDgReplace(r, i, tank, now))
                    {
                        g_wovDgDropped[mm.low] = now + 20 * MINUTE;
                        WovDgRelease(r, std::string(b->GetName() + " kept dying on the way").c_str(), false);
                        return false;
                    }
                    return true;
                }
            }
            else
                mm.deadT = 0;
            float d = b->GetExactDist(r.spot->mx, r.spot->my, r.spot->mz);
            // progress = it is moving (walking to a flight master counts) or fighting or already there
            float moved = std::hypot(b->GetPositionX() - mm.px, b->GetPositionY() - mm.py);
            if (moved > 15.0f || b->IsInFlight() || b->IsInCombat() || d <= 30.0f)
            {
                mm.px = b->GetPositionX();
                mm.py = b->GetPositionY();
                mm.lastProgress = now;
            }
            if (d < mm.lastDist - 30.0f || !mm.closerT)
            {
                mm.lastDist = d;
                mm.closerT = now;
            }
            // walking in circles for 4 minutes without getting closer (no path, a wall, a river): it is
            // placed further along the way, never where a real player could see it (WovUnstick checks)
            if (d > 60.0f && !mm.flew && now > mm.closerT + 4 * MINUTE && !b->IsInCombat() && !b->IsInFlight())
            {
                float k = std::min(1.0f, 400.0f / d);  // a stretch of the way, at most 400 yd at a time
                float tx = b->GetPositionX() + (r.spot->mx - b->GetPositionX()) * k;
                float ty = b->GetPositionY() + (r.spot->my - b->GetPositionY()) * k;
                float tz = k >= 1.0f ? r.spot->mz : b->GetMap()->GetHeight(b->GetPhaseMask(), tx, ty, MAX_HEIGHT);
                if (tz > INVALID_HEIGHT)
                    WovUnstick(b, GET_PLAYERBOT_AI(b), WorldPosition(r.spot->map, tx, ty, tz), "dungeon run");
                mm.closerT = now;
            }
            if (d <= 40.0f)
                mm.flew = true;  // reused as "has arrived at the stone"
            bool late = i > 0 && !mm.flew && r.tankThere && now > std::max(r.tankThere, mm.joined) + 8 * MINUTE;
            if (now > mm.lastProgress + 5 * MINUTE || late)
            {
                if (i == 0 || !WovDgReplace(r, i, tank, now))
                {
                    g_wovDgDropped[mm.low] = now + 20 * MINUTE;  // not picked again right away for the next party
                    WovDgRelease(r, std::string(b->GetName() + (late ? " never showed up" : " got stuck on the way")).c_str(), false);
                    return false;
                }
                return true;  // the new member is picked up next tick
            }
            if (d <= 60.0f && mm.flew && b->IsAlive())
                ++here;
            if (!b->IsInCombat() && b->IsAlive())
                WovDgGoTo(b, GET_PLAYERBOT_AI(b), r.spot, mm, now);
            // in someone else's group (picked up on the way): it leaves it for the run
            if (Group* og = b->GetGroup())
                if (!og->IsMember(tank->GetGUID()) || (i == 0 && !og->IsLeader(tank->GetGUID())))
                    GET_PLAYERBOT_AI(b)->LeaveOrDisbandGroup();
        }
        if (now >= r.phaseT + 60)
        {
            r.phaseT = now;
            std::string st;
            for (size_t i = 0; i < ps.size(); ++i)
                st += " " + ps[i]->GetName() + ":" + std::to_string(int(ps[i]->GetExactDist(r.spot->mx, r.spot->my, r.spot->mz))) +
                      (ps[i]->IsInFlight() ? "f" : "") + "/" + std::to_string(uint32(GET_PLAYERBOT_AI(ps[i])->rpgInfo.GetStatus()));
            LOG_INFO("playerbots", "WoV dungeon run #{} {} gathering:{}", r.id, r.spot->def->name, st);
        }
        WovDgPostLfm(r, tank, now);
        if (real && realIn && real->IsInWorld() && real->IsAlive() && real->GetMapId() == r.spot->map &&
            real->GetExactDist(r.spot->mx, r.spot->my, r.spot->mz) <= 60.0f)
            ++here;
        else if (real && r.tankThere && now > std::max(r.tankThere, r.realT) + 12 * MINUTE)
        {
            // the real player does not come: the run goes on with four
            tank->Whisper(urand(0, 1) ? "sry we cant wait any longer, maybe next time" : "gotta go, sry. next time", LANG_UNIVERSAL, real);
            if (realIn && tank->GetGroup()->IsLeader(tank->GetGUID()))
                tank->GetGroup()->RemoveMember(real->GetGUID(), GROUP_REMOVEMETHOD_KICK);
            LOG_INFO("playerbots", "WoV dungeon run #{} {}: {} did not come, going with four", r.id, r.spot->def->name, real->GetName());
            r.real = 0;
            real = nullptr;
            realIn = false;
        }
        // the party forms at the stone as people arrive
        if (!r.tankThere && tank->GetExactDist(r.spot->mx, r.spot->my, r.spot->mz) <= 40.0f)
            r.tankThere = now;
        if (tank->GetExactDist(r.spot->mx, r.spot->my, r.spot->mz) <= 60.0f)
            for (size_t i = 1; i < ps.size(); ++i)
            {
                Player* b = ps[i];
                if (b->GetExactDist(r.spot->mx, r.spot->my, r.spot->mz) <= 60.0f && !b->GetGroup() &&
                    (!tank->GetGroup() || (tank->GetGroup()->IsLeader(tank->GetGUID()) && !tank->GetGroup()->IsFull())))
                    WovInviteFrom(tank, b);
            }
        Group* g = tank->GetGroup();
        if (g && g->IsLeader(tank->GetGUID()))  // someone else got in: the run's own people come first
        {
            std::vector<ObjectGuid> extra;
            for (GroupReference* ref = g->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                {
                    bool ours = r.real && m->GetGUID().GetCounter() == r.real;
                    for (Player* b : ps)
                        ours = ours || b == m;
                    if (!ours)
                        extra.push_back(m->GetGUID());
                }
            for (ObjectGuid const& x : extra)
                g->RemoveMember(x, GROUP_REMOVEMETHOD_KICK);
            g = tank->GetGroup();
        }
        bool all = g && g->IsLeader(tank->GetGUID());
        for (Player* b : ps)
            all = all && g && g->IsMember(b->GetGUID());
        all = all && (!real || realIn);
        if (all && here == ps.size() + (real ? 1 : 0))
        {
            r.phase = DG_ENTER;
            r.phaseT = now;
            if (real)
                tai->SayToParty(urand(0, 1) ? "ok all here, zone in" : "going in, zone in");
            else if (urand(0, 99) < 60)
                tai->SayToParty(urand(0, 1) ? "ok everyone here, lets go" : "all here? going in");
        }
        else if (now > r.start + 40 * MINUTE)
        {
            WovDgRelease(r, "never got the party together", false);
            return false;
        }
        return true;
    }
    if (r.phase == DG_ENTER)
    {
        if (now < r.phaseT + 4)
            return true;
        if (!r.swum)  // meeting point on the shore: swim to the entrance together first
        {
            bool shore = std::hypot(r.spot->mx - r.spot->x, r.spot->my - r.spot->y) > 8.0f;
            if (shore && now < r.phaseT + 100 && tank->GetExactDist(r.spot->x, r.spot->y, r.spot->z) > 12.0f)
            {
                WorldPosition stone(r.spot->map, r.spot->x, r.spot->y, r.spot->z);
                for (Player* b : ps)
                    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(b))
                    {
                        auto* gc = std::get_if<NewRpgInfo::GoCamp>(&ai->rpgInfo.data);
                        if (!gc || gc->pos.distance(stone) > 5.0f)
                            ai->rpgInfo.ChangeToGoCamp(stone);
                    }
                return true;
            }
            r.swum = true;
            r.phaseT = now - 4;
        }
        // the portal: everyone steps through together
        for (Player* b : ps)
            if (b->GetMapId() == r.spot->map && !b->IsBeingTeleported())
            {
                if (sMapMgr->PlayerCannotEnter(r.spot->dmap, b, false) != Map::CAN_ENTER)
                {
                    WovDgRelease(r, std::string(b->GetName() + " cannot enter the instance").c_str(), true);
                    return false;
                }
                b->TeleportTo(r.spot->dmap, r.spot->dx + frand(-1.5f, 1.5f), r.spot->dy + frand(-1.5f, 1.5f), r.spot->dz,
                              r.spot->dO);
            }
        bool inside = true;
        for (Player* b : ps)
            inside = inside && b->GetMapId() == r.spot->dmap && b->IsInWorld() && !b->IsBeingTeleported() &&
                     b->GetInstanceId() == tank->GetInstanceId();
        bool realInside = !real || (real->IsInWorld() && real->GetMapId() == r.spot->dmap && real->GetInstanceId() == tank->GetInstanceId());
        if (inside && now >= r.phaseT + 10 && (realInside || now > r.phaseT + 150))
        {
            WovDgRoles(ps);
            tai->DoSpecificAction("dc on", Event("wov", "", nullptr), true);
            r.phase = DG_RUN;
            r.phaseT = now;
            LOG_INFO("playerbots", "WoV dungeon run #{} {}: inside (instance {}), clear started", r.id, r.spot->def->name,
                     tank->GetInstanceId());
        }
        else if (now > r.phaseT + 3 * MINUTE)
        {
            std::string who;
            for (Player* b : ps)
                who += " " + b->GetName() + ":" + std::to_string(b->GetMapId()) + "/" + std::to_string(b->GetInstanceId()) +
                       (b->IsAlive() ? "" : "(dead)") + (b->IsBeingTeleported() ? "(tp)" : "") +
                       ":" + std::to_string(uint32(sMapMgr->PlayerCannotEnter(r.spot->dmap, b, false)));
            WovDgRelease(r, std::string("could not get everyone inside:" + who).c_str(), true);
            return false;
        }
        return true;
    }
    if (r.phase == DG_RUN)
    {
        WovDgRoles(ps);  // a strategy reset (group or talent change) must not take the roles away mid-run
        bool anyAlive = false, left = false;
        for (Player* b : ps)
        {
            anyAlive = anyAlive || b->IsAlive();
            left = left || !b->GetGroup() || b->GetGroup() != tank->GetGroup();
        }
        for (size_t i = 0; i < ps.size(); ++i)
        {
            Player* b = ps[i];
            WovDgMember& mm = r.m[i];
            if (b->IsAlive())
            {
                mm.deadT = 0;
                // up again but outside (graveyard, or got up at the portal): it walks back in like anyone
                if (b->GetMapId() != r.spot->dmap)
                {
                    if (!mm.outT)
                        mm.outT = now;
                    bool atPortal = b->GetMapId() == r.spot->map && b->GetExactDist(r.spot->x, r.spot->y, r.spot->z) < 200.0f;
                    if (!b->IsBeingTeleported() && !b->IsInCombat() && (atPortal ? now > mm.outT + 15 : now > mm.outT + 3 * MINUTE))
                    {
                        b->TeleportTo(r.spot->dmap, r.spot->dx + frand(-1.5f, 1.5f), r.spot->dy + frand(-1.5f, 1.5f), r.spot->dz, r.spot->dO);
                        mm.outT = 0;
                    }
                    else if (now > mm.outT + 10 * MINUTE)
                    {
                        WovDgRelease(r, std::string(b->GetName() + " did not come back in").c_str(), true);
                        return false;
                    }
                }
                else
                    mm.outT = 0;
                continue;
            }
            if (!mm.deadT)
                mm.deadT = now;
            // released and outside: it runs back as a ghost and zones in; a corpse nobody rezzed gets up after the run back
            if (anyAlive && now > mm.deadT + (b->GetMapId() != r.spot->dmap ? 90 : 180))
                WovDgRunBack(r, b);
        }
        if (r.finalKilled)
        {
            r.phase = DG_DONE;
            r.phaseT = now;
            if (urand(0, 99) < 70)
                GET_PLAYERBOT_AI(ps[urand(0, ps.size() - 1)])->SayToParty(WovPick(kWovDgThanks));
            LOG_INFO("playerbots", "WoV dungeon run #{} {}: cleared in {} min", r.id, r.spot->def->name, (now - r.start) / 60);
            return true;
        }
        if (left)
        {
            std::string who;
            for (Player* b : ps)
                if (!b->GetGroup() || b->GetGroup() != tank->GetGroup())
                    who += " " + b->GetName() + (!b->GetGroup() ? "(no group)" : "(other group)");
            WovDgRelease(r, std::string(anyAlive ? "the party broke up:" : "wiped, the party broke up:").append(who).c_str(), true);
            return false;
        }
        if (r.rezT && now >= r.rezT + 25)  // back on their feet, ate and drank: second try
        {
            r.rezT = 0;
            for (Player* b : ps)
                WovDgRunBack(r, b);  // the ghosts that zoned in meanwhile
            for (Player* b : ps)
                if (b->IsAlive())
                {
                    b->SetFullHealth();
                    if (b->getPowerType() == POWER_MANA)
                        b->SetPower(POWER_MANA, b->GetMaxPower(POWER_MANA));
                }
            tai->DoSpecificAction("dc on", Event("wov", "", nullptr), true);
            r.moveT = 0;
            LOG_INFO("playerbots", "WoV dungeon run #{} {}: second try after the wipe", r.id, r.spot->def->name);
        }
        if (!anyAlive)
        {
            if (r.retries == 0 && now > r.phaseT + 3 * MINUTE)  // the corpse run took its time
            {
                r.retries = 1;
                for (Player* b : ps)
                    WovDgRunBack(r, b);
                r.rezT = now;
                r.phaseT = now;
                static char const* const again[] = {"ok again, slower this time", "my bad, again?", "lets try that again",
                                                    "wait for mana before pulls pls", "one more go"};
                if (urand(0, 99) < 70)
                    GET_PLAYERBOT_AI(ps[urand(0, ps.size() - 1)])->SayToParty(again[urand(0, 4)]);
                LOG_INFO("playerbots", "WoV dungeon run #{} {}: wiped, they run back and try again", r.id, r.spot->def->name);
            }
            else if (now > r.phaseT + 4 * MINUTE)
            {
                WovDgRelease(r, r.retries ? "wiped twice and gave up" : "wiped and gave up", true);
                return false;
            }
            return true;
        }
        r.phaseT = now;
        if (now >= r.logT + 10 * MINUTE)
        {
            r.logT = now;
            uint32 alive = 0;
            for (Player* b : ps)
                alive += b->IsAlive() ? 1 : 0;
            LOG_INFO("playerbots", "WoV dungeon run #{} {} inside: tank at {:.0f},{:.0f},{:.0f}, {} alive, {} min", r.id,
                     r.spot->def->name, tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(), alive, (now - r.start) / 60);
        }
        // stall watchdog: the tank has not moved 20 yd in 7 minutes out of combat (no path, an event it cannot do)
        if (!r.rezT)
        {
            float mv = std::hypot(tank->GetPositionX() - r.tx, tank->GetPositionY() - r.ty);
            // in combat a tank may stand still for a boss, but not for a quarter of an hour (a fight that never ends)
            if (!r.moveT || mv > 20.0f || !tank->IsAlive())
            {
                r.tx = tank->GetPositionX();
                r.ty = tank->GetPositionY();
                r.moveT = now;
            }
            else if (now > r.moveT + (tank->IsInCombat() ? 15 : 7) * MINUTE)
            {
                r.moveT = now;
                ++r.stalls;
                LOG_INFO("playerbots", "WoV dungeon run #{} {}: stalled at {:.0f},{:.0f},{:.0f} (stall {})", r.id, r.spot->def->name,
                         tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(), r.stalls);
                if (r.stalls == 1)
                    tai->DoSpecificAction("dc on", Event("wov", "", nullptr), true);  // fresh plan from here
                else if (r.stalls <= 3)
                {
                    tai->DoSpecificAction("dc skip", Event("wov", "", nullptr), true);
                    tai->DoSpecificAction("dc on", Event("wov", "", nullptr), true);
                    if (urand(0, 99) < 50)
                        tai->SayToParty(urand(0, 1) ? "cant get there, skip it" : "eh stuck, lets skip that one");
                }
                else
                {
                    WovDgRelease(r, "got stuck inside", true);
                    return false;
                }
            }
        }
        return true;
    }
    // DG_DONE: loot a minute, then thanks and part ways
    if (now >= r.phaseT + 70)
    {
        WovDgRelease(r, "done", true);
        return false;
    }
    return true;
}
}  // namespace


#include "InstanceSaveMgr.h"
#include <map>
#include <set>
// ---- wov: raid nights -----------------------------------------------------------------------------------
// A bot guild raids Molten Core on its raid nights (two evenings a week, 20:00-20:45 server time, picked per guild).
// The level-60 members online come, the officers post in guild chat, everyone heads for the Molten Core window in
// Blackrock Mountain, a warlock summons those who are far away (the classic "sum pls"), the main tank forms the
// raid, they step through the portal and the main tank leads the clear (mod-dungeon-clear in raid mode, the
// playerbots Molten Core strategy for the bosses). Up to two wipes are run back, then they call it. When Ragnaros
// dies (or they call it) it is "gg" in guild chat and everyone goes back to their own evening.
namespace
{
struct WovRaid
{
    uint32 id = 0, guild = 0;
    std::string gname;
    std::vector<WovDgMember> m;  // m[0] is the main tank and raid leader
    std::vector<uint8> role;     // 0 tank, 1 healer, 2 dps
    WovDgPhase phase = DG_GATHER;
    time_t start = 0, phaseT = 0, sumT = 0, moveT = 0, logT = 0, rezT = 0, roleT = 0, limboT = 0;
    float tx = 0, ty = 0;
    uint8 wipes = 0, stalls = 0;
    bool finalKilled = false;
    std::vector<uint32> reals;  // real players of the guild who were asked along
    time_t realInvT = 0;
};
std::vector<WovRaid> g_wovRaids;
std::unordered_map<uint32, time_t> g_wovRaidLast;  // guild -> last raid start
std::unordered_set<uint32> g_wovRaidRealGuilds;    // guilds led by a real player: they plan their own raids
bool g_wovRaidEnable = false;
uint32 g_wovRaidPug = 30;  // a guild short of people fills up to this with level 60s from outside ("LF5M MC")
uint32 g_wovRaidMin = 20, g_wovRaidMax = 40, g_wovRaidMaxActive = 2, g_wovRaidHour = 20, g_wovRaidTest = 0, g_wovRaidNextId = 1;
time_t g_wovRaidCfgT = 0, g_wovRaidRealT = 0;
WovDgDef const kWovMcDef = {"MC", 3528, 60, 60, -1};
WovDgSpot g_wovMcSpot;
bool g_wovMcSpotOk = false;
uint32 const kWovRagnaros = 11502;

char const* const kWovRnCall[] = {"MC tonight! invites going out, come to the window in BRM",
                                     "raid time, MC. head to blackrock mountain, sums at the window",
                                     "MC inv now, be at the window in BRM. lock will sum",
                                     "raid starting, MC. bring pots and come to the window"};
char const* const kWovRnSumAsk[] = {"sum pls", "can i get a sum?", "sum plz", "anyone summoning?", "need sum"};
char const* const kWovRnSumThx[] = {"ty for sum", "thx lock", "ty", "thanks for the sum"};
char const* const kWovRnGo[] = {"ok everyone in, lets go", "zone in", "all here, buffs and go", "going in, follow the MT"};
char const* const kWovRnWipe[] = {"wipe, run back", "run back pls", "ok again, focus", "rez up and lets go again"};
char const* const kWovRnEnd[] = {"gg all, good raid tonight", "thanks all, same time next week", "gj everyone",
                                   "gg, see you next raid"};
char const* const kWovRnCallIt[] = {"ok calling it for tonight, ty all", "thats it for tonight, gj everyone",
                                      "calling it, we'll get it next week"};

void WovRaidConfig(time_t now)
{
    if (now < g_wovRaidCfgT + 60)
        return;
    g_wovRaidCfgT = now;
    g_wovRaidEnable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.Raids.Enable", false);
    g_wovRaidMin = std::max<uint32>(10, sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Raids.MinSize", 20));
    g_wovRaidMax = std::min<uint32>(40, std::max(g_wovRaidMin, sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Raids.MaxSize", 40)));
    g_wovRaidMaxActive = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Raids.MaxActive", 2);
    g_wovRaidPug = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Raids.PugFillTo", 30);
    g_wovRaidHour = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Raids.StartHour", 20) % 24;
    g_wovRaidTest = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Raids.TestGuild", 0);
    if (!g_wovMcSpotOk)
    {
        AreaTrigger const* at = sObjectMgr->GetAreaTrigger(kWovMcDef.trigger);
        AreaTriggerTeleport const* tp = sObjectMgr->GetAreaTriggerTeleport(kWovMcDef.trigger);
        if (at && tp)
        {
            g_wovMcSpot.def = &kWovMcDef;
            g_wovMcSpot.map = at->map;
            // the raid meets at Lothos Riftwaker, beside the window on solid floor (the window ledge drops into the lava)
            g_wovMcSpot.x = g_wovMcSpot.mx = -7506.0f;
            g_wovMcSpot.y = g_wovMcSpot.my = -1042.0f;
            g_wovMcSpot.z = g_wovMcSpot.mz = 180.9f;
            g_wovMcSpot.meetSet = true;
            g_wovMcSpot.dmap = tp->target_mapId;
            g_wovMcSpot.dx = tp->target_X;
            g_wovMcSpot.dy = tp->target_Y;
            g_wovMcSpot.dz = tp->target_Z;
            g_wovMcSpot.dO = tp->target_Orientation;
            g_wovMcSpotOk = true;
        }
    }
    g_wovDgFinal.insert(kWovRagnaros);  // the kill hook reports Ragnaros too
    if (now >= g_wovRaidRealT + 10 * MINUTE)
    {
        g_wovRaidRealT = now;
        g_wovRaidRealGuilds.clear();
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT g.guildid FROM guild g JOIN characters c ON c.guid=g.leaderguid "
                "WHERE c.account NOT IN (SELECT id FROM acore_auth.account WHERE username LIKE 'RNDBOT%')"))
            do
                g_wovRaidRealGuilds.insert(r->Fetch()[0].Get<uint32>());
            while (r->NextRow());
    }
}

// a guild's raid nights: two weekdays and a start time fixed per guild
time_t WovRaidStartToday(uint32 guild, time_t now)
{
    tm lt;
    localtime_r(&now, &lt);
    int d1 = int(guild * 3 % 7), d2 = int((guild * 3 + 3) % 7);
    if (lt.tm_wday != d1 && lt.tm_wday != d2)
        return 0;
    lt.tm_hour = int(g_wovRaidHour);
    lt.tm_min = int(guild % 4) * 15;
    lt.tm_sec = 0;
    return mktime(&lt);
}

void WovRaidRole(Player* b, uint8 role)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(b);
    if (!ai)
        return;
    if (role == 0 && b->getClass() == CLASS_WARRIOR && !PlayerbotAI::IsTank(b))
        ai->ChangeStrategy("-arms,-fury,-dps assist,+tank,+tank assist", BOT_STATE_COMBAT);
    else if (role == 0 && b->getClass() == CLASS_DRUID && !PlayerbotAI::IsTank(b))
        ai->ChangeStrategy("-cat,-balance,-resto,-dps assist,+bear,+tank assist", BOT_STATE_COMBAT);
    else if (role == 1 && !PlayerbotAI::IsHeal(b))
        switch (b->getClass())
        {
            case CLASS_PRIEST: ai->ChangeStrategy("-dps,-shadow debuff,-shadow aoe,-dps assist,+heal", BOT_STATE_COMBAT); break;
            case CLASS_PALADIN: ai->ChangeStrategy("-dps,-tank,-tank assist,-bthreat,+heal,+bcast", BOT_STATE_COMBAT); break;
            case CLASS_SHAMAN: ai->ChangeStrategy("-ele,-enh,+resto", BOT_STATE_COMBAT); break;
            case CLASS_DRUID: ai->ChangeStrategy("-cat,-bear,-balance,-tank assist,+resto", BOT_STATE_COMBAT); break;
            default: break;
        }
}

void WovRaidSay(WovRaid const& r, char const* text, bool guild)
{
    Player* lead = r.m.empty() ? nullptr : ObjectAccessor::FindPlayerByLowGUID(r.m[0].low);
    PlayerbotAI* ai = lead ? GET_PLAYERBOT_AI(lead) : nullptr;
    if (!ai)
        return;
    if (guild)
        ai->SayToGuild(text);
    else if (lead->GetGroup())
        ai->SayToRaid(text);
}

void WovRaidFree(uint32 low)
{
    g_wovBusyBot.erase(low);
    if (Player* b = ObjectAccessor::FindPlayerByLowGUID(low))
        if (PlayerbotAI* ai = GET_PLAYERBOT_AI(b))
        {
            if (b->GetGroup())
                ai->LeaveOrDisbandGroup();
            ai->ResetStrategies();
            ai->rpgInfo.ChangeToIdle();
        }
}

void WovRaidDrop(WovRaid& r, size_t i, char const* why)
{
    uint32 low = r.m[i].low;
    Player* b = ObjectAccessor::FindPlayerByLowGUID(low);
    LOG_INFO("playerbots", "WoV raid #{} <{}> MC: {} drops out ({})", r.id, r.gname, b ? b->GetName() : std::to_string(low), why);
    r.m.erase(r.m.begin() + i);
    r.role.erase(r.role.begin() + i);
    WovRaidFree(low);
}

void WovRaidEnd(WovRaid& r, char const* why, bool guildBye, char const* const* lines, size_t n)
{
    if (guildBye && n)
        WovRaidSay(r, lines[urand(0, n - 1)], true);
    for (WovDgMember const& mm : r.m)
        WovRaidFree(mm.low);
    LOG_INFO("playerbots", "WoV raid #{} <{}> MC: {} ({} min, {} wipes)", r.id, r.gname, why, (time(nullptr) - r.start) / 60, r.wipes);
}

void WovRaidRunBack(Player* b)
{
    WovDgSpot const& s = g_wovMcSpot;
    if (b->IsAlive() || b->IsBeingTeleported())
        return;
    if (b->GetMapId() != s.dmap)
    {
        b->TeleportTo(s.dmap, s.dx + frand(-2.0f, 2.0f), s.dy + frand(-2.0f, 2.0f), s.dz, s.dO);
        return;
    }
    b->ResurrectPlayer(0.5f);
    b->SpawnCorpseBones();
}

// free for raid night: in the world, not busy elsewhere; a group of bots is left for the raid, one with a real player is not
bool WovRaidAvail(Player* b, time_t now)
{
    if (!b->IsInWorld() || !b->IsAlive() || b->IsInFlight() || b->InBattleground())  // a battleground queue is left for the raid
        return false;
    if (!b->GetMap() || b->GetMap()->Instanceable() || WovOnMarketTrip(b->GetGUID().GetCounter()) || WovDgBusy(b->GetGUID().GetCounter()))
        return false;
    if (b->GetGroup() && WovRealIn(b->GetGroup()))
        return false;
    auto it = g_wovBusyBot.find(b->GetGUID().GetCounter());
    if (it != g_wovBusyBot.end() && now < time_t(it->second))
        return false;
    PlayerbotAI* ai = GET_PLAYERBOT_AI(b);
    return ai && !ai->GetMaster();
}

// the Molten Core lockout a character has this week (0 = none). A raid can only take people with no lockout or the
// same one as its main tank, as for real players.
uint32 WovMcBind(Player* p)
{
    InstanceSave* sv = sInstanceSaveMgr->PlayerGetInstanceSave(p->GetGUID(), 409, Difficulty(0));
    return sv ? sv->GetInstanceId() : 0;
}

void WovLeaveBgQueues(Player* b)
{
    for (uint32 i = 0; i < PLAYER_MAX_BATTLEGROUND_QUEUES; ++i)
    {
        BattlegroundQueueTypeId q = b->GetBattlegroundQueueTypeId(i);
        if (q == BATTLEGROUND_QUEUE_NONE)
            continue;
        sBattlegroundMgr->GetBattlegroundQueue(q).RemovePlayer(b->GetGUID(), true);
        b->RemoveBattlegroundQueueId(q);
    }
}

bool WovRaidStart(uint32 guild, time_t now)
{
    std::vector<Player*> tanks, heals, dps, hyb;
    std::string gname;
    for (auto const& [g, b] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!b || !b->IsInWorld() || b->GetGuildId() != guild || b->GetLevel() < 60 || !WovRaidAvail(b, now))
            continue;
        if (gname.empty())
            if (Guild* gg = sGuildMgr->GetGuildById(guild))
                gname = gg->GetName();
        uint8 c = b->getClass();
        if (PlayerbotAI::IsTank(b, true) || c == CLASS_WARRIOR)
            tanks.push_back(b);
        else if (c == CLASS_PRIEST || PlayerbotAI::IsHeal(b, true))
            heals.push_back(b);
        else if (c == CLASS_PALADIN || c == CLASS_SHAMAN || c == CLASS_DRUID)
            hyb.push_back(b);
        else
            dps.push_back(b);
    }
    // this week's lockout: the raid goes to the instance most of the guild is saved to (if a tank is saved there too),
    // and only takes people who are saved there or nowhere
    uint32 bind = 0;
    {
        std::map<uint32, uint32> n;
        std::set<uint32> tankBinds;
        for (auto* v : {&tanks, &heals, &dps, &hyb})
            for (Player* p : *v)
                if (uint32 b = WovMcBind(p))
                    ++n[b];
        for (Player* t : tanks)
            tankBinds.insert(WovMcBind(t));
        uint32 best = 0;
        for (auto const& [b, c] : n)
            if (tankBinds.count(b) && c > best)
            {
                best = c;
                bind = b;
            }
        if (!bind && !tankBinds.count(0))
            bind = tanks.empty() ? 0 : WovMcBind(tanks[0]);
        for (auto* v : {&tanks, &heals, &dps, &hyb})
            v->erase(std::remove_if(v->begin(), v->end(), [&](Player* p) { uint32 b = WovMcBind(p); return b && b != bind; }), v->end());
    }
    // short of people: the raid leader asks around ("LF5M MC, need lock and heals") and level 60s from outside the
    // guild of the same side fill up, the roles the raid lacks first (warlocks for banishes, healers, tanks)
    uint32 have = tanks.size() + heals.size() + dps.size() + hyb.size();
    if (have >= g_wovRaidMin / 2 && have < g_wovRaidPug && !tanks.empty())
    {
        TeamId team = tanks[0]->GetTeamId();
        std::vector<Player*> pool;
        for (auto const& [g, b] : sRandomPlayerbotMgr.GetAllBots())
            if (b && b->IsInWorld() && b->GetTeamId() == team && b->GetGuildId() != guild && b->GetLevel() >= 60 &&
                !g_wovRaidRealGuilds.count(b->GetGuildId()) && !WovRaidNightFor(b->GetGUID().GetCounter()) && WovRaidAvail(b, now))
                if (uint32 pb = WovMcBind(b); !pb || pb == bind)
                    pool.push_back(b);
        std::shuffle(pool.begin(), pool.end(), std::mt19937(std::random_device{}()));
        auto count = [&](uint8 cls) {
            uint32 n = 0;
            for (auto* v : {&tanks, &heals, &dps, &hyb})
                for (Player* p : *v)
                    n += p->getClass() == cls ? 1 : 0;
            return n;
        };
        uint32 pugs = 0;
        auto take = [&](auto pred, std::vector<Player*>& into, uint32 upto) {
            for (auto it = pool.begin(); it != pool.end() && into.size() < upto && have < g_wovRaidPug;)
                if (pred(*it))
                {
                    into.push_back(*it);
                    it = pool.erase(it);
                    ++have;
                    ++pugs;
                }
                else
                    ++it;
        };
        uint32 locks = count(CLASS_WARLOCK);
        if (locks < 3)
            take([](Player* p) { return p->getClass() == CLASS_WARLOCK; }, dps, dps.size() + (3 - locks));
        take([](Player* p) { return p->getClass() == CLASS_PRIEST || PlayerbotAI::IsHeal(p, true); }, heals, std::max<uint32>(5, g_wovRaidPug / 4));
        take([](Player* p) { return p->getClass() == CLASS_WARRIOR; }, tanks, 4);
        take([](Player* p) { return true; }, dps, 1000);
        if (pugs)
            LOG_INFO("playerbots", "WoV raid <{}>: {} from outside the guild fill up the raid", gname, pugs);
    }
    // too few warriors: a druid bear-tanks (an off-tank, rarely the main tank)
    while (tanks.size() < 2)
    {
        auto it = std::find_if(hyb.begin(), hyb.end(), [](Player* p) { return p->getClass() == CLASS_DRUID && !PlayerbotAI::IsHeal(p, true); });
        if (it == hyb.end())
            it = std::find_if(hyb.begin(), hyb.end(), [](Player* p) { return p->getClass() == CLASS_DRUID; });
        if (it == hyb.end())
            break;
        tanks.push_back(*it);
        hyb.erase(it);
    }
    // main tank first: a tank by spec, then the warrior with the most health
    std::sort(tanks.begin(), tanks.end(), [bind](Player* a, Player* b) {
        bool la = WovMcBind(a) == bind, lb = WovMcBind(b) == bind;  // the main tank carries the raid's lockout
        if (la != lb)
            return la;
        bool ta = PlayerbotAI::IsTank(a, true) || a->getClass() == CLASS_WARRIOR, tb = PlayerbotAI::IsTank(b, true) || b->getClass() == CLASS_WARRIOR;
        return ta != tb ? ta : a->GetMaxHealth() > b->GetMaxHealth();
    });
    uint32 total = tanks.size() + heals.size() + dps.size() + hyb.size();
    if (total < g_wovRaidMin || tanks.size() < 2)
    {
        LOG_INFO("playerbots", "WoV raid <{}>: not enough people for MC tonight ({} level 60 online, {} tanks)", gname, total, tanks.size());
        return false;
    }
    uint32 size = std::min(g_wovRaidMax, total);
    uint32 wantHeal = std::max<uint32>(4, size / 4), wantTank = std::min<uint32>(size >= 25 ? 4 : 3, tanks.size());
    WovRaid r;
    r.id = g_wovRaidNextId++;
    r.guild = guild;
    r.gname = gname;
    r.start = r.phaseT = now;
    auto add = [&](Player* p, uint8 role) {
        if (r.m.size() >= size)
            return;
        r.m.push_back({p->GetGUID().GetCounter(), 1e9f, now, false, p->GetPositionX(), p->GetPositionY(), now});
        r.role.push_back(role);
        g_wovBusyBot[p->GetGUID().GetCounter()] = uint32(now + 5 * HOUR);
        if (p->InBattlegroundQueue())
            WovLeaveBgQueues(p);
    };
    for (uint32 i = 0; i < wantTank; ++i)
        add(tanks[i], 0);
    uint32 h = 0;
    for (Player* p : heals)
        if (h < wantHeal)
            add(p, 1), ++h;
    for (Player* p : hyb)
        if (h < wantHeal)
            add(p, 1), ++h;
    for (size_t i = wantTank; i < tanks.size(); ++i)
        add(tanks[i], 2);
    for (Player* p : dps)
        add(p, 2);
    for (Player* p : hyb)
        if (std::find_if(r.m.begin(), r.m.end(), [&](WovDgMember const& x) { return x.low == p->GetGUID().GetCounter(); }) == r.m.end())
            add(p, 2);
    for (Player* p : heals)
        if (std::find_if(r.m.begin(), r.m.end(), [&](WovDgMember const& x) { return x.low == p->GetGUID().GetCounter(); }) == r.m.end())
            add(p, 1);
    g_wovRaidLast[guild] = now;
    WovRaidSay(r, kWovRnCall[urand(0, 3)], true);
    // real players in the guild are asked along (a whisper from the raid leader); they are invited at the window
    for (auto const& [id, session] : sWorldSessionMgr->GetAllSessions())
    {
        Player* p = session ? session->GetPlayer() : nullptr;
        if (!p || !WovIsReal(p) || !p->IsInWorld() || p->GetGuildId() != guild || p->GetLevel() < 55)
            continue;
        r.reals.push_back(p->GetGUID().GetCounter());
        static char const* const ask[] = {"MC tonight, u coming? meet at the window in BRM, ill inv",
                                          "raiding MC now, want in? come to blackrock mountain, lothos",
                                          "hey we're doing MC, come to the BRM window and ill invite u"};
        tanks[0]->Whisper(ask[urand(0, 2)], LANG_UNIVERSAL, p);
    }
    LOG_INFO("playerbots", "WoV raid #{} <{}> MC: {} going ({} tanks, {} healers), main tank {}", r.id, gname, r.m.size(), wantTank, h,
             tanks[0]->GetName());
    g_wovRaids.push_back(r);
    return true;
}

// returns false when the raid is over
bool WovRaidStep(WovRaid& r, time_t now)
{
    WovDgSpot const& s = g_wovMcSpot;
    std::vector<Player*> ps;
    for (size_t i = 0; i < r.m.size();)
    {
        Player* b = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(r.m[i].low));
        if (!b || !GET_PLAYERBOT_AI(b))
        {
            if (i == 0)
            {
                WovRaidEnd(r, "the main tank logged off", true, kWovRnCallIt, 3);
                return false;
            }
            WovRaidDrop(r, i, "logged off");
            continue;
        }
        if (!b->IsInWorld() || b->IsBeingTeleportedFar())
        {
            if (!r.limboT)
                r.limboT = now;
            if (now < r.limboT + 2 * MINUTE)
                return true;
            if (i == 0)
            {
                WovRaidEnd(r, "the main tank vanished between maps", true, kWovRnCallIt, 3);
                return false;
            }
            WovRaidDrop(r, i, "vanished between maps");
            continue;
        }
        ps.push_back(b);
        ++i;
    }
    r.limboT = 0;
    Player* mt = ps[0];
    PlayerbotAI* mai = GET_PLAYERBOT_AI(mt);
    if (now > r.start + 4 * HOUR)
    {
        WovRaidEnd(r, "out of time", true, kWovRnCallIt, 3);
        return false;
    }
    if (r.phase == DG_GATHER)
    {
        uint32 here = 0;
        Player* lock = nullptr;
        std::vector<size_t> away;
        for (size_t i = 0; i < ps.size(); ++i)
        {
            Player* b = ps[i];
            WovDgMember& mm = r.m[i];
            PlayerbotAI* ai = GET_PLAYERBOT_AI(b);
            bool onMap = b->GetMapId() == s.map && !b->GetMap()->Instanceable();
            float d = onMap ? b->GetExactDist(s.x, s.y, s.z) : 1e9f;
            if (b->IsAlive() && onMap && d <= 60.0f)
            {
                ++here;
                if (b->getClass() == CLASS_WARLOCK)
                    lock = b;
            }
            else if (b->IsAlive() && !b->IsInCombat() && !b->IsInFlight() && !b->InBattleground())
                away.push_back(i);
            if (!b->IsAlive() || b->IsInCombat())
                continue;
            if (onMap && d <= 2500.0f)
                WovDgGoTo(b, ai, &s, mm, now);  // close enough to walk (Searing Gorge, Burning Steppes)
            // further away: it goes on with its evening until the summon comes
            if (Group* og = b->GetGroup())  // someone else's group: leaves it for the raid
                if (!og->IsMember(mt->GetGUID()) || (i == 0 && !og->IsLeader(mt->GetGUID())))
                    ai->LeaveOrDisbandGroup();
        }
        // the summoning: a warlock (or the meeting stone in Blackrock Mountain) and two more at the window bring the
        // others, one at a time
        Player* helper = lock;
        if (!helper)
            for (size_t i = 0; i < ps.size() && !helper; ++i)
                if (ps[i]->IsAlive() && ps[i]->GetMapId() == s.map && ps[i]->GetExactDist(s.x, s.y, s.z) <= 60.0f)
                    helper = ps[i];
        if (helper && here >= 2 && !away.empty() && now >= r.sumT + 12)
        {
            r.sumT = now;
            size_t i = away[urand(0, away.size() - 1)];
            Player* b = ps[i];
            if (urand(0, 99) < 40)
                b->Whisper(WovPick(kWovRnSumAsk), LANG_UNIVERSAL, helper);
            b->TeleportTo(s.map, s.x + frand(-1.5f, 1.5f), s.y + frand(-1.5f, 1.5f), s.z + 0.5f, frand(0.0f, 6.28f));
            if (urand(0, 99) < 30)
                b->Whisper(WovPick(kWovRnSumThx), LANG_UNIVERSAL, helper);
            LOG_INFO("playerbots", "WoV raid #{} <{}> MC: {} summons {}", r.id, r.gname, lock ? lock->GetName() : "the meeting stone",
                     b->GetName());
        }
        // the raid forms at the window: the main tank invites, and makes it a raid
        if (mt->IsAlive() && mt->GetMapId() == s.map && mt->GetExactDist(s.x, s.y, s.z) <= 60.0f)
        {
            Group* g = mt->GetGroup();
            if (g && g->IsLeader(mt->GetGUID()) && !g->isRaidGroup() && g->GetMembersCount() >= 2)
                g->ConvertToRaid();
            g = mt->GetGroup();
            for (size_t i = 1; i < ps.size(); ++i)
            {
                Player* b = ps[i];
                if (b->GetGroup() || !b->IsAlive() || b->GetMapId() != s.map || b->GetExactDist(s.x, s.y, s.z) > 60.0f)
                    continue;
                if (g && (!g->IsLeader(mt->GetGUID()) || g->IsFull() || (!g->isRaidGroup() && g->GetMembersCount() >= 5)))
                    break;
                WovInviteFrom(mt, b);
            }
        }
        // real guild members who came to the window get an invite (they are not waited for)
        if (!r.reals.empty() && now >= r.realInvT + 20)
        {
            r.realInvT = now;
            Group* g = mt->GetGroup();
            for (uint32 rl : r.reals)
                if (Player* p = ObjectAccessor::FindPlayerByLowGUID(rl))
                    if (p->IsInWorld() && !p->GetGroup() && p->GetMapId() == s.map && p->GetExactDist(s.x, s.y, s.z) <= 60.0f &&
                        g && g->IsLeader(mt->GetGUID()) && g->isRaidGroup() && !g->IsFull())
                        WovInviteFrom(mt, p);
        }
        if (now >= r.phaseT + 60)
        {
            r.phaseT = now;
            uint32 grouped = 0;
            for (Player* b : ps)
                grouped += b->GetGroup() && b->GetGroup() == mt->GetGroup() ? 1 : 0;
            std::string st;
            for (Player* b : ps)
            {
                bool onMap = b->GetMapId() == s.map;
                st += " " + b->GetName() + ":" + (onMap ? std::to_string(int(b->GetExactDist(s.x, s.y, s.z))) : "m" + std::to_string(b->GetMapId())) +
                      (b->IsAlive() ? "" : "d") + (b->IsInCombat() ? "c" : "") + "/" + std::to_string(uint32(GET_PLAYERBOT_AI(b)->rpgInfo.GetStatus()));
            }
            LOG_INFO("playerbots", "WoV raid #{} <{}> MC gathering: {} of {} at the window, {} in the raid, warlock there: {} |{}", r.id,
                     r.gname, here, ps.size(), grouped, lock ? "yes" : "no", st);
        }
        Group* g = mt->GetGroup();
        uint32 ready = 0;
        for (Player* b : ps)
            ready += (g && b->GetGroup() == g && b->IsAlive() && b->GetMapId() == s.map && b->GetExactDist(s.x, s.y, s.z) <= 60.0f) ? 1 : 0;
        bool timeUp = now > r.start + 30 * MINUTE;
        if (g && g->isRaidGroup() && (ready == ps.size() || (timeUp && ready >= g_wovRaidMin)))
        {
            for (size_t i = ps.size(); i-- > 1;)  // the ones who did not make it are not waited for
                if (!(ps[i]->GetGroup() == g && ps[i]->IsAlive() && ps[i]->GetMapId() == s.map &&
                      ps[i]->GetExactDist(s.x, s.y, s.z) <= 60.0f))
                    WovRaidDrop(r, i, "not at the window in time");
            g->SetGroupMemberFlag(mt->GetGUID(), true, MEMBER_FLAG_MAINTANK);
            r.phase = DG_ENTER;
            r.phaseT = now;
            WovRaidSay(r, WovPick(kWovRnGo), false);
            return true;
        }
        if (now > r.start + 45 * MINUTE)
        {
            WovRaidEnd(r, "not enough people showed up", true, kWovRnCallIt, 3);
            return false;
        }
        return true;
    }
    if (r.phase == DG_ENTER)
    {
        if (now < r.phaseT + 5)
            return true;
        for (size_t i = ps.size(); i-- > 0;)
        {
            Player* b = ps[i];
            if (b->GetMapId() != s.map || b->IsBeingTeleported())
                continue;
            if (sMapMgr->PlayerCannotEnter(s.dmap, b, false) != Map::CAN_ENTER)
            {
                if (i == 0)
                {
                    WovRaidEnd(r, "the main tank cannot enter Molten Core", true, kWovRnCallIt, 3);
                    return false;
                }
                WovRaidDrop(r, i, "cannot enter");
                return true;
            }
            b->TeleportTo(s.dmap, s.dx + frand(-3.0f, 3.0f), s.dy + frand(-3.0f, 3.0f), s.dz, s.dO);
        }
        bool inside = true;
        for (Player* b : ps)
            inside = inside && b->GetMapId() == s.dmap && b->GetInstanceId() == mt->GetInstanceId();
        if (inside && now >= r.phaseT + 15)
        {
            for (size_t i = 0; i < ps.size(); ++i)
                WovRaidRole(ps[i], r.role[i]);
            if (Group* g = mt->GetGroup())
                g->SetGroupMemberFlag(mt->GetGUID(), true, MEMBER_FLAG_MAINTANK);
            mai->DoSpecificAction("dc on", Event("wov", "", nullptr), true);
            r.phase = DG_RUN;
            r.phaseT = r.roleT = now;
            LOG_INFO("playerbots", "WoV raid #{} <{}> MC: {} inside (instance {}), clear started", r.id, r.gname, ps.size(), mt->GetInstanceId());
        }
        else if (now > r.phaseT + 3 * MINUTE)
        {
            for (size_t i = ps.size(); i-- > 1;)
                if (ps[i]->GetMapId() != s.dmap || ps[i]->GetInstanceId() != mt->GetInstanceId())
                    WovRaidDrop(r, i, "did not get inside");
            if (mt->GetMapId() != s.dmap || r.m.size() < g_wovRaidMin / 2)
            {
                WovRaidEnd(r, "could not get the raid inside", true, kWovRnCallIt, 3);
                return false;
            }
            r.phaseT = now - 15;  // the rest are in: start
        }
        return true;
    }
    if (r.phase == DG_RUN)
    {
        if (r.finalKilled)
        {
            WovRaidEnd(r, "Ragnaros is dead, MC cleared", true, kWovRnEnd, 4);
            return false;
        }
        // Garr: each warlock keeps one Firesworn banished (the classic way to take him with a small raid)
        if (Creature* garr = mt->FindNearestCreature(12057, 120.0f))
            if (garr->IsAlive() && garr->IsInCombat())
            {
                std::list<Creature*> adds;
                mt->GetCreatureListWithEntryInGrid(adds, 12099, 120.0f);
                auto banished = [](Creature* c) { return c->HasAura(710) || c->HasAura(18647); };
                for (Player* b : ps)
                {
                    if (b->getClass() != CLASS_WARLOCK || !b->IsAlive() || b->GetMapId() != s.dmap || b->IsNonMeleeSpellCast(false))
                        continue;
                    bool mine = false;
                    for (Creature* c : adds)
                        mine = mine || (c->IsAlive() && (c->HasAura(710, b->GetGUID()) || c->HasAura(18647, b->GetGUID())));
                    if (mine)
                        continue;
                    Creature* pick = nullptr;
                    for (Creature* c : adds)
                        if (c->IsAlive() && !banished(c) && b->GetExactDist(c) <= 30.0f && b->IsWithinLOSInMap(c) &&
                            (!pick || b->GetExactDist(c) < b->GetExactDist(pick)))
                            pick = c;
                    if (pick && GET_PLAYERBOT_AI(b)->CastSpell("banish", pick))
                        LOG_INFO("playerbots", "WoV raid #{} <{}> MC: {} banishes a Firesworn", r.id, r.gname, b->GetName());
                }
            }
        if (now >= r.roleT + 30)  // a strategy reset must not take the roles away mid-raid
        {
            r.roleT = now;
            for (size_t i = 0; i < ps.size(); ++i)
                WovRaidRole(ps[i], r.role[i]);
        }
        Group* g = mt->GetGroup();
        for (size_t i = ps.size(); i-- > 1;)
            if (!ps[i]->GetGroup() || ps[i]->GetGroup() != g)
            {
                WovRaidDrop(r, i, "left the raid");
                return true;
            }
        if (r.m.size() < g_wovRaidMin / 2)
        {
            WovRaidEnd(r, "too many left", true, kWovRnCallIt, 3);
            return false;
        }
        bool anyAlive = false;  // anywhere: one who got up at the graveyard is on the way back, not a wipe
        for (Player* b : ps)
            anyAlive = anyAlive || b->IsAlive();
        if (r.rezT && now >= r.rezT + 30)  // back up after the run back: drink, buff, again
        {
            r.rezT = 0;
            for (Player* b : ps)
                WovRaidRunBack(b);
            for (Player* b : ps)
                if (b->IsAlive())
                {
                    b->SetFullHealth();
                    if (b->getPowerType() == POWER_MANA)
                        b->SetPower(POWER_MANA, b->GetMaxPower(POWER_MANA));
                }
            mai->DoSpecificAction("dc on", Event("wov", "", nullptr), true);
            r.moveT = 0;
        }
        if (!anyAlive)
        {
            if (now > r.phaseT + 3 * MINUTE)
            {
                if (++r.wipes > 2)
                {
                    WovRaidEnd(r, "called it after three wipes", true, kWovRnCallIt, 3);
                    return false;
                }
                for (Player* b : ps)
                    WovRaidRunBack(b);
                r.rezT = now;
                r.phaseT = now;
                WovRaidSay(r, WovPick(kWovRnWipe), false);
                LOG_INFO("playerbots", "WoV raid #{} <{}> MC: wipe {}, running back", r.id, r.gname, r.wipes);
            }
            return true;
        }
        r.phaseT = now;
        // single deaths: a released ghost zones in again, a corpse nobody rezzed gets up after the run back
        for (size_t i = 0; i < ps.size(); ++i)
        {
            Player* b = ps[i];
            WovDgMember& mm = r.m[i];
            if (b->IsAlive())
            {
                mm.deadT = 0;
                if (b->GetMapId() != s.dmap && !b->IsBeingTeleported() && !b->IsInCombat())
                {
                    if (!mm.outT)
                        mm.outT = now;
                    if (now > mm.outT + 2 * MINUTE)
                    {
                        b->TeleportTo(s.dmap, s.dx + frand(-2.0f, 2.0f), s.dy + frand(-2.0f, 2.0f), s.dz, s.dO);
                        mm.outT = 0;
                    }
                }
                continue;
            }
            if (!mm.deadT)
                mm.deadT = now;
            if (now > mm.deadT + (b->GetMapId() != s.dmap ? 90 : 240))
                WovRaidRunBack(b);
        }
        if (now >= r.logT + 10 * MINUTE)
        {
            r.logT = now;
            uint32 alive = 0;
            for (Player* b : ps)
                alive += b->IsAlive() ? 1 : 0;
            LOG_INFO("playerbots", "WoV raid #{} <{}> MC inside: MT at {:.0f},{:.0f},{:.0f}, {}/{} alive, {} min, {} wipes", r.id, r.gname,
                     mt->GetPositionX(), mt->GetPositionY(), mt->GetPositionZ(), alive, ps.size(), (now - r.start) / 60, r.wipes);
        }
        if (!r.rezT)  // stall watchdog, as in the dungeons
        {
            float mv = std::hypot(mt->GetPositionX() - r.tx, mt->GetPositionY() - r.ty);
            if (!r.moveT || mv > 20.0f || !mt->IsAlive())
            {
                r.tx = mt->GetPositionX();
                r.ty = mt->GetPositionY();
                r.moveT = now;
            }
            else if (now > r.moveT + (mt->IsInCombat() ? 15 : 8) * MINUTE)
            {
                r.moveT = now;
                ++r.stalls;
                LOG_INFO("playerbots", "WoV raid #{} <{}> MC: stalled at {:.0f},{:.0f},{:.0f} (stall {})", r.id, r.gname, mt->GetPositionX(),
                         mt->GetPositionY(), mt->GetPositionZ(), r.stalls);
                if (r.stalls == 1)
                    mai->DoSpecificAction("dc on", Event("wov", "", nullptr), true);
                else if (r.stalls <= 3)
                {
                    mai->DoSpecificAction("dc skip", Event("wov", "", nullptr), true);
                    mai->DoSpecificAction("dc on", Event("wov", "", nullptr), true);
                }
                else
                {
                    WovRaidEnd(r, "stuck, called it", true, kWovRnCallIt, 3);
                    return false;
                }
            }
        }
        return true;
    }
    return false;
}

void WovRaid_Tick(time_t now)
{
    WovRaidConfig(now);
    for (size_t i = 0; i < g_wovRaids.size();)
    {
        if (WovRaidStep(g_wovRaids[i], now))
            ++i;
        else
            g_wovRaids.erase(g_wovRaids.begin() + i);
    }
    if (!g_wovRaidEnable || !g_wovMcSpotOk || g_wovRaids.size() >= g_wovRaidMaxActive)
        return;
    static time_t lastPlan = 0;
    if (now < lastPlan + 60)
        return;
    lastPlan = now;
    std::unordered_set<uint32> guilds;
    for (auto const& [g, b] : sRandomPlayerbotMgr.GetAllBots())
        if (b && b->GetGuildId() && b->GetLevel() >= 60)
            guilds.insert(b->GetGuildId());
    for (uint32 guild : guilds)
    {
        if (g_wovRaids.size() >= g_wovRaidMaxActive)
            break;
        if (g_wovRaidRealGuilds.count(guild))
            continue;
        bool running = false;
        for (WovRaid const& r : g_wovRaids)
            running = running || r.guild == guild;
        if (running)
            continue;
        time_t last = g_wovRaidLast.count(guild) ? g_wovRaidLast[guild] : 0;
        bool test = g_wovRaidTest == guild && now > last + 30 * MINUTE;
        time_t at = WovRaidStartToday(guild, now);
        bool night = at && now >= at && now <= at + 20 * MINUTE && last < at;
        if (!test && !night)
            continue;
        static std::unordered_map<uint32, time_t> retryAt;
        if (night && now < retryAt[guild])
            continue;
        if (!WovRaidStart(guild, now))
        {
            // not enough yet: people still finishing a battleground or a dungeon. Ask again every 5 minutes while the
            // start window is open, then no raid tonight (test: again in 30 min)
            retryAt[guild] = now + 5 * MINUTE;
            if (!night || now > at + 15 * MINUTE)
                g_wovRaidLast[guild] = night ? at : now;
        }
    }
}
}  // namespace

// wov: level-60 members of a bot guild whose raid night is now (from half an hour before the start until three and
// a half hours after it). They come online for it (play schedule) and log in first. Recomputed every 5 minutes.
bool WovRaidNightFor(uint32 guid)
{
    static std::unordered_set<uint32> night;
    static time_t calcT = 0;
    time_t now = time(nullptr);
    if (now >= calcT + 300)
    {
        calcT = now;
        night.clear();
        if (g_wovRaidEnable)
            if (QueryResult r = CharacterDatabase.Query(
                    "SELECT m.guildid, m.guid FROM guild_member m JOIN characters c ON c.guid=m.guid "
                    "JOIN acore_auth.account a ON a.id=c.account WHERE a.username LIKE 'RNDBOT%' AND c.level>=60"))
                do
                {
                    uint32 g = r->Fetch()[0].Get<uint32>();
                    if (g_wovRaidRealGuilds.count(g))
                        continue;
                    time_t at = WovRaidStartToday(g, now);
                    if (at && now >= at - 30 * MINUTE && now <= at + 210 * MINUTE)
                        night.insert(r->Fetch()[1].Get<uint32>());
                } while (r->NextRow());
    }
    return night.count(guid) != 0;
}
// ---- end wov: raid nights -------------------------------------------------------------------------------

// wov: the tank a dungeon-run member (or the main tank a raider) waits for (0 when it is in no run). World thread.
uint32 WovDgLeaderOf(uint32 low)
{
    for (WovDgRunState const& r : g_wovDgRuns)
        for (size_t i = 1; i < r.m.size(); ++i)
            if (r.m[i].low == low)
                return r.m[0].low;
    for (WovRaid const& r : g_wovRaids)
        for (size_t i = 1; i < r.m.size(); ++i)
            if (r.m[i].low == low)
                return r.m[0].low;
    return 0;
}

// wov: true while a bot is taken by a planned activity (dungeon run, group forming, ...)
bool WovBotReserved(uint32 low)
{
    auto it = g_wovBusyBot.find(low);
    return it != g_wovBusyBot.end() && time(nullptr) < time_t(it->second);
}

void WovDungeonRun_OnKill(Player* killer, Creature* killed)
{
    if (!g_wovDgEnable || !killer || !killed || !g_wovDgFinal.count(killed->GetEntry()) || !killer->GetMap() ||
        !killer->GetMap()->IsDungeon())
        return;
    std::lock_guard<std::mutex> lock(g_wovDgKillMutex);
    g_wovDgKills.push_back({killer->GetGUID().GetCounter(), killed->GetEntry()});
}

// whisper hook (Playerbots.cpp): a real player answering a run's LFM. Filtered on the world thread.
void WovDungeonRun_OnWhisper(Player* from, Player* to)
{
    if (!g_wovDgEnable || !from || !to || sRandomPlayerbotMgr.IsRandomBot(from) || !sRandomPlayerbotMgr.IsRandomBot(to))
        return;
    std::lock_guard<std::mutex> lock(g_wovDgKillMutex);
    if (g_wovDgJoins.size() < 64)
        g_wovDgJoins.push_back({to->GetGUID().GetCounter(), from->GetGUID().GetCounter()});
}

void WovDungeonRun_Tick(time_t now)
{
    WovDgConfig(now);
    static time_t last = 0;
    if (now < last + 5)
        return;
    last = now;
    std::vector<std::pair<uint32, uint32>> kills;
    {
        std::lock_guard<std::mutex> lock(g_wovDgKillMutex);
        kills.swap(g_wovDgKills);
    }
    std::vector<std::pair<uint32, uint32>> joins;
    {
        std::lock_guard<std::mutex> lock(g_wovDgKillMutex);
        joins.swap(g_wovDgJoins);
    }
    for (auto const& [bl, pl] : joins)
        for (WovDgRunState& r : g_wovDgRuns)
        {
            if (r.phase != DG_GATHER || r.real || !r.lfmT || now > r.lfmT + 10 * MINUTE || r.m.empty() || r.m[0].low != bl ||
                r.m.size() < 5)
                continue;
            Player* tank = ObjectAccessor::FindPlayerByLowGUID(bl);
            Player* p = ObjectAccessor::FindPlayerByLowGUID(pl);
            if (!tank || !p || p->GetGroup() || p->GetTeamId() != tank->GetTeamId() || p->GetLevel() < r.spot->def->minL ||
                p->GetLevel() > r.spot->def->maxL)
                break;
            if (tank->GetGroup() && (!tank->GetGroup()->IsLeader(tank->GetGUID()) || tank->GetGroup()->IsFull()))
                break;
            // the last dps steps aside before the group is made; it goes back to its own business
            uint32 drop = r.m.back().low;
            r.m.pop_back();
            g_wovBusyBot.erase(drop);
            g_wovDgDropped[drop] = now + 20 * MINUTE;
            if (Player* d = ObjectAccessor::FindPlayerByLowGUID(drop))
                if (PlayerbotAI* dai = GET_PLAYERBOT_AI(d))
                {
                    if (d->GetGroup() && d->GetGroup()->IsMember(tank->GetGUID()))
                        dai->LeaveOrDisbandGroup();
                    dai->rpgInfo.ChangeToIdle();
                }
            r.real = pl;
            r.realT = now;
            static char const* const ok[] = {"sure, inv", "inv sent, meet at the stone", "k inv", "invited, come to the stone", "sure, sent"};
            tank->Whisper(ok[urand(0, 4)], LANG_UNIVERSAL, p);
            WovInviteFrom(tank, p);
            LOG_INFO("playerbots", "WoV dungeon run #{} {}: real player {} joins (lvl {})", r.id, r.spot->def->name, p->GetName(), p->GetLevel());
            break;
        }
    for (auto const& [k, e] : kills)
    {
        for (WovDgRunState& r : g_wovDgRuns)
            for (WovDgMember const& mm : r.m)
                if (mm.low == k)
                    r.finalKilled = true;
        for (WovRaid& r : g_wovRaids)
            for (WovDgMember const& mm : r.m)
                if (mm.low == k && e == kWovRagnaros)
                    r.finalKilled = true;
    }
    WovRaid_Tick(now);  // wov: raid nights
    for (size_t i = 0; i < g_wovDgRuns.size();)
    {
        if (WovDgStep(g_wovDgRuns[i], now))
            ++i;
        else
        {
            for (WovDgMember const& mm : g_wovDgRuns[i].m)
                g_wovBusyBot.erase(mm.low);
            g_wovDgRuns.erase(g_wovDgRuns.begin() + i);
        }
    }
    if (g_wovDgEnable && g_wovDgRuns.size() < g_wovDgMax && now >= g_wovDgNextStart)
    {
        g_wovDgNextStart = now + g_wovDgEvery;
        WovDgStart(now);
    }
}
// ---- end wov: dungeon runs ------------------------------------------------------------------------------

// ---- wov: crafting orders -----------------------------------------------------------------------------
// A real player links an item and asks for it ("can u make [Heavy Linen Bandage]?" in a whisper, or
// "LF tailor [Red Linen Shirt]" in Trade). A bot that knows a recipe for it whispers back what mats it needs and
// asks for them by mail. When the mail arrives the bot crafts (reagents consumed, real skill-up), mails the
// result and any leftovers back and keeps the money as a tip. Item mail from real players that no order covers
// is sent back. Open orders are kept in wov_craft_order and expire after three days.
#include "CharacterCache.h"
#include "ChatHelper.h"
#include "DBCStores.h"
#include "Mail.h"
#include "SpellMgr.h"
#include "World.h"
#include <atomic>
#include <mutex>
namespace
{
struct WovCraftOrder
{
    uint32 item = 0, spell = 0;
    time_t t = 0;
};
struct WovCraftAsk
{
    uint32 player = 0, bot = 0;
    std::string msg;
};
struct WovCraftSay
{
    time_t due = 0;
    uint32 bot = 0, player = 0;
    std::string text;
};
std::mutex g_wovCraftMutex;
std::vector<WovCraftAsk> g_wovCraftAsks;
std::vector<WovCraftSay> g_wovCraftSays;
std::map<std::pair<uint32, uint32>, WovCraftOrder> g_wovCraftOrders;  // (bot, player)
std::unordered_map<uint32, time_t> g_wovCraftLastAsk;
std::once_flag g_wovCraftOnce;
std::unordered_map<uint32, std::vector<uint32>> g_wovCraftRecipes;  // crafted item -> recipe spells
std::atomic<bool> g_wovCraftEnable{false};
time_t const kWovCraftOrderSec = 3 * DAY;

char const* const kWovCraftYes[] = {"i can make {item}. mail me {mats} and ill send it back",
                                     "sure, mail me {mats} and ill craft {item} for u",
                                     "got the recipe for {item}. mats are {mats}, just mail em to me",
                                     "yeah i can do {item}. send {mats} in the mail, tips welcome"};
char const* const kWovCraftNo[] = {"dont have that recipe sry", "cant make that one", "nah not my profession sry",
                                    "no recipe for that, sry"};
char const* const kWovCraftDone[] = {"sent u {item}, enjoy", "{item} done, its in ur mailbox", "done, check ur mail",
                                      "made it, sent it back to u"};
char const* const kWovCraftMiss[] = {"got ur mail but im missing {mats}, sent it all back",
                                      "u forgot {mats}, mailed ur stuff back"};

void WovCraftBuild()
{
    std::call_once(g_wovCraftOnce, [] {
        static uint32 const kSkills[] = {SKILL_ALCHEMY,      SKILL_BLACKSMITHING, SKILL_COOKING,   SKILL_ENGINEERING,
                                         SKILL_FIRST_AID,    SKILL_LEATHERWORKING, SKILL_TAILORING, SKILL_ENCHANTING,
                                         SKILL_MINING};
        for (uint32 i = 0; i < sSkillLineAbilityStore.GetNumRows(); ++i)
        {
            SkillLineAbilityEntry const* e = sSkillLineAbilityStore.LookupEntry(i);
            if (!e || std::find(std::begin(kSkills), std::end(kSkills), e->SkillLine) == std::end(kSkills))
                continue;
            SpellInfo const* s = sSpellMgr->GetSpellInfo(e->Spell);
            if (!s)
                continue;
            for (uint8 k = 0; k < MAX_SPELL_EFFECTS; ++k)
                if (s->Effects[k].Effect == SPELL_EFFECT_CREATE_ITEM && s->Effects[k].ItemType)
                {
                    std::vector<uint32>& v = g_wovCraftRecipes[s->Effects[k].ItemType];
                    if (std::find(v.begin(), v.end(), e->Spell) == v.end())
                        v.push_back(e->Spell);
                }
        }
    });
}

uint32 WovCraftItemOf(std::string const& msg)
{
    size_t p = msg.find("|Hitem:");
    if (p == std::string::npos)
        return 0;
    return uint32(strtoul(msg.c_str() + p + 7, nullptr, 10));
}

bool WovCraftWords(std::string const& msg, bool whisper)
{
    std::string m;
    // drop the item links so item names cannot count as words
    for (size_t i = 0; i < msg.size(); ++i)
    {
        if (msg[i] == '|' && i + 1 < msg.size() && msg[i + 1] == 'H')
        {
            size_t e = msg.find("|h|r", i);
            if (e == std::string::npos)
                break;
            i = e + 3;
            m += ' ';
            continue;
        }
        m += char(std::tolower((unsigned char)msg[i]));
    }
    m = " " + m + " ";
    if (m.find("wts") != std::string::npos || m.find("selling") != std::string::npos)
        return false;
    static char const* const kW[] = {"make", "craft", "cook", "forge", "smelt", "sew", "brew", "can u", "can you",
                                     "could you", "recipe", "do u ", "do you "};
    static char const* const kC[] = {"craft", "make", "maker", "cook", "smelt", " lf ", " lf1", " lfw", "wtb",
                                     "anyone", "someone", "who can", " need"};
    if (whisper)
    {
        for (char const* w : kW)
            if (m.find(w) != std::string::npos)
                return true;
        return false;
    }
    for (char const* w : kC)
        if (m.find(w) != std::string::npos)
            return true;
    return false;
}

std::string WovCraftMats(SpellInfo const* si, uint32 times)
{
    std::string out;
    for (uint8 x = 0; x < MAX_SPELL_REAGENTS; ++x)
    {
        if (si->Reagent[x] <= 0)
            continue;
        ItemTemplate const* r = sObjectMgr->GetItemTemplate(uint32(si->Reagent[x]));
        if (!r)
            continue;
        if (!out.empty())
            out += ", ";
        out += ChatHelper::FormatItem(r, si->ReagentCount[x] * times);
    }
    return out;
}

void WovCraftAskHandle(WovCraftAsk const& ask, time_t now)
{
    Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(ask.player));
    if (!p)
        return;
    time_t& lastAsk = g_wovCraftLastAsk[ask.player];
    if (now < lastAsk + (ask.bot ? 5 : 60))
        return;
    lastAsk = now;
    uint32 item = WovCraftItemOf(ask.msg);
    auto rit = g_wovCraftRecipes.find(item);
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
    if (rit == g_wovCraftRecipes.end() || !proto)
        return;
    auto knows = [&](Player* b) -> uint32 {
        for (uint32 s : rit->second)
            if (b->HasSpell(s))
                return s;
        return 0;
    };
    Player* bot = nullptr;
    uint32 spell = 0;
    if (ask.bot)
    {
        bot = ObjectAccessor::FindPlayerByLowGUID(ask.bot);
        if (!bot || !bot->IsInWorld())
            return;
        spell = knows(bot);
        if (!spell)
        {
            g_wovCraftSays.push_back({now + urand(4, 10), ask.bot, ask.player, WovPick(kWovCraftNo)});
            return;
        }
    }
    else
    {
        std::unordered_map<uint32, uint32> open;
        for (auto const& [k, o] : g_wovCraftOrders)
            ++open[k.first];
        int best = -1;
        for (auto const& [guid, b] : sRandomPlayerbotMgr.GetAllBots())
        {
            if (!b || !b->IsInWorld() || !b->IsAlive() || b->IsInCombat() || b->InBattleground() ||
                b->GetTeamId() != p->GetTeamId())
                continue;
            if (b->GetMap() && b->GetMap()->Instanceable())
                continue;
            if (open[b->GetGUID().GetCounter()] >= 3)
                continue;
            uint32 s = knows(b);
            if (!s)
                continue;
            int score = (b->GetZoneId() == p->GetZoneId() ? 100 : 0) + (b->GetMapId() == p->GetMapId() ? 20 : 0) +
                        int(urand(0, 30));
            if (score > best)
            {
                best = score;
                bot = b;
                spell = s;
            }
        }
        if (!bot)
            return;  // nobody online can make it: the channel stays quiet, like a real one
    }
    SpellInfo const* si = sSpellMgr->GetSpellInfo(spell);
    if (!si)
        return;
    std::string mats = WovCraftMats(si, 1);
    if (mats.empty())
        return;
    uint32 blow = bot->GetGUID().GetCounter();
    std::string text = WovRep(WovRep(WovPick(kWovCraftYes), "{item}", ChatHelper::FormatItem(proto)), "{mats}", mats);
    g_wovCraftSays.push_back({now + (ask.bot ? time_t(urand(5, 12)) : time_t(urand(20, 60))), blow, ask.player, text});
    g_wovCraftOrders[{blow, ask.player}] = {item, spell, now};
    CharacterDatabase.Execute("UPDATE wov_craft_order SET status='cancelled', updated=NOW() WHERE bot={} AND player={} AND "
                              "status='open'",
                              blow, ask.player);
    CharacterDatabase.Execute("INSERT INTO wov_craft_order (player, bot, item, spell, status) VALUES ({}, {}, {}, {}, 'open')",
                              ask.player, blow, item, spell);
    LOG_INFO("playerbots", "WoV craft: {} offers item {} (spell {}) to {}", bot->GetName(), item, spell, p->GetName());
}

void WovCraftOneMail(Player* bot, Mail* m, time_t now)
{
    uint32 blow = bot->GetGUID().GetCounter(), plow = m->sender;
    ObjectGuid pguid = ObjectGuid::Create<HighGuid::Player>(plow);
    Player* p = ObjectAccessor::FindConnectedPlayer(pguid);
    auto oit = g_wovCraftOrders.find({blow, plow});
    bool order = oit != g_wovCraftOrders.end() && now < oit->second.t + kWovCraftOrderSec && bot->HasSpell(oit->second.spell);
    SpellInfo const* si = order ? sSpellMgr->GetSpellInfo(oit->second.spell) : nullptr;
    std::vector<Item*> items;
    for (MailItemInfo const& mi : m->items)
        if (Item* it = bot->GetMItem(mi.item_guid))
            items.push_back(it);
    uint32 n = 0;
    std::string missing;
    if (si)
    {
        n = 5;
        for (uint8 x = 0; x < MAX_SPELL_REAGENTS; ++x)
        {
            if (si->Reagent[x] <= 0 || !si->ReagentCount[x])
                continue;
            uint32 have = 0;
            for (Item* it : items)
                if (it->GetEntry() == uint32(si->Reagent[x]))
                    have += it->GetCount();
            n = std::min(n, have / si->ReagentCount[x]);
            if (have < si->ReagentCount[x])
                if (ItemTemplate const* r = sObjectMgr->GetItemTemplate(uint32(si->Reagent[x])))
                    missing += (missing.empty() ? "" : ", ") + ChatHelper::FormatItem(r, si->ReagentCount[x] - have);
        }
    }
    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
    std::vector<Item*> back;
    uint32 money = m->money, crafted = 0, itemId = order ? oit->second.item : 0;
    if (n > 0)
    {
        for (uint8 x = 0; x < MAX_SPELL_REAGENTS; ++x)
        {
            if (si->Reagent[x] <= 0)
                continue;
            uint32 need = si->ReagentCount[x] * n;
            for (Item*& it : items)
            {
                if (!it || !need || it->GetEntry() != uint32(si->Reagent[x]))
                    continue;
                uint32 c = it->GetCount();
                if (c <= need)
                {
                    need -= c;
                    bot->RemoveMItem(it->GetGUID().GetCounter());
                    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ITEM_INSTANCE);
                    stmt->SetData(0, it->GetGUID().GetCounter());
                    trans->Append(stmt);
                    delete it;
                    it = nullptr;
                }
                else
                {
                    it->SetCount(c - need);
                    need = 0;
                }
            }
        }
        int32 per = 1;
        for (uint8 k = 0; k < MAX_SPELL_EFFECTS; ++k)
            if (si->Effects[k].Effect == SPELL_EFFECT_CREATE_ITEM && si->Effects[k].ItemType == itemId)
                per = std::max<int32>(1, si->Effects[k].CalcValue(bot));
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        uint32 total = uint32(per) * n, stack = proto ? std::max<uint32>(1, proto->GetMaxStackSize()) : 1;
        while (total)
        {
            uint32 c = std::min(stack, total);
            Item* ni = Item::CreateItem(itemId, c, bot);
            if (!ni)
                break;
            back.push_back(ni);
            crafted += c;
            total -= c;
        }
        for (uint32 k = 0; k < n; ++k)
            bot->UpdateCraftSkill(oit->second.spell);
        if (money)
        {
            bot->ModifyMoney(money);  // the tip
            money = 0;
        }
    }
    for (Item* it : items)
        if (it)
        {
            bot->RemoveMItem(it->GetGUID().GetCounter());
            back.push_back(it);
        }
    std::string subject, body;
    if (n > 0)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        subject = proto ? proto->Name1 : "Your order";
        body = "here u go\n\n" + bot->GetName();
    }
    else if (order)
    {
        subject = "Missing mats";
        body = "missing some mats, sending it all back\n\n" + bot->GetName();
    }
    else
    {
        subject = "Returned";
        body = "think u sent this to the wrong person?\n\n" + bot->GetName();
    }
    size_t i = 0;
    bool first = true;
    while (i < back.size() || (first && money))
    {
        MailDraft draft(subject, body);
        if (first && money)
            draft.AddMoney(money);
        for (uint32 k = 0; k < MAX_MAIL_ITEMS && i < back.size(); ++k, ++i)
        {
            Item* it = back[i];
            if (it->GetState() == ITEM_UNCHANGED)
                it->FSetState(ITEM_CHANGED);
            it->SetOwnerGUID(pguid);
            it->SaveToDB(trans);
            draft.AddItem(it);
        }
        draft.SendMailTo(trans, MailReceiver(p, plow), MailSender(bot), MAIL_CHECK_MASK_HAS_BODY, 0);
        first = false;
    }
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_BY_ID);
    stmt->SetData(0, m->messageID);
    trans->Append(stmt);
    stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_MAIL_ITEM_BY_ID);
    stmt->SetData(0, m->messageID);
    trans->Append(stmt);
    if (n > 0)
        bot->SaveInventoryAndGoldToDB(trans);
    bot->RemoveMail(m->messageID);
    delete m;
    CharacterDatabase.CommitTransaction(trans);
    if (n > 0)
    {
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
            if (p)
                g_wovCraftSays.push_back({now + urand(2, 6), blow, plow, WovRep(WovPick(kWovCraftDone), "{item}", ChatHelper::FormatItem(proto))});
        CharacterDatabase.Execute("UPDATE wov_craft_order SET status='done', crafted={}, updated=NOW() WHERE bot={} AND "
                                  "player={} AND status='open'",
                                  crafted, blow, plow);
        g_wovCraftOrders.erase(oit);
        LOG_INFO("playerbots", "WoV craft: {} crafted {}x item {} for guid {}", bot->GetName(), crafted, itemId, plow);
    }
    else if (order && p)
        g_wovCraftSays.push_back({now + urand(2, 6), blow, plow, WovRep(WovPick(kWovCraftMiss), "{mats}", missing.empty() ? "some mats" : missing)});
    else
        LOG_INFO("playerbots", "WoV craft: {} returned unrequested mail from guid {}", bot->GetName(), plow);
}

void WovCraftMail(time_t now)
{
    time_t delay = time_t(sWorld->getIntConfig(CONFIG_MAIL_DELIVERY_DELAY));
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (!bot || !bot->IsInWorld() || !bot->GetMailSize())
            continue;
        std::vector<Mail*> todo;
        for (Mail* m : bot->GetMails())
        {
            if (!m || m->state == MAIL_STATE_DELETED || m->messageType != MAIL_NORMAL || m->COD)
                continue;
            if (m->items.empty() && !m->money)
                continue;
            // item mail between accounts is held for an hour; the bot "checks its mail" a minute after it was sent
            if (now + delay < m->deliver_time + 60)
                continue;
            uint32 acc = sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid::Create<HighGuid::Player>(m->sender));
            if (!acc || PlayerbotAIConfig::instance().IsInRandomAccountList(acc))
                continue;
            todo.push_back(m);
        }
        for (Mail* m : todo)
            WovCraftOneMail(bot, m, now);
    }
}

// wov: bots ask each other for crafts in Trade. The asker posts "LF tailor to make [item], have mats"; a bot in a
// capital that really knows the recipe answers in Trade a little later. Only items the asker could use soon.
struct WovCraftPub
{
    time_t due = 0;
    uint32 bot = 0;
    std::string text;
};
std::vector<WovCraftPub> g_wovCraftPub;
char const* const kWovCraftLf[] = {"LF {prof} to make {item}, have mats", "any {prof} can craft {item}? got mats",
                                   "LF {prof} for {item}, will tip", "need {item} made, have mats, tipping",
                                   "LF {prof} {item} pst", "anyone make {item}? have the mats"};
char const* const kWovCraftCan[] = {"{prof} here, can do {item}, w me", "i can make {item}, pst", "can craft {item}, w me w mats",
                                    "{asker} i can make that, w me", "i got {item}, whisper me", "can do it, w me"};

char const* WovProfName(uint32 spell)
{
    auto bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spell);
    for (auto it = bounds.first; it != bounds.second; ++it)
        switch (it->second->SkillLine)
        {
            case SKILL_ALCHEMY: return "alch";
            case SKILL_BLACKSMITHING: return "bs";
            case SKILL_COOKING: return "cook";
            case SKILL_ENGINEERING: return "engi";
            case SKILL_FIRST_AID: return "first aid";
            case SKILL_LEATHERWORKING: return "lw";
            case SKILL_TAILORING: return "tailor";
            default: break;
        }
    return nullptr;
}

bool WovCraftTradeAsk(Player* asker)
{
    if (!g_wovCraftEnable || !asker || asker->GetLevel() < 12)
        return false;
    WovCraftBuild();
    std::vector<Player*> crafters;
    for (auto const& [guid, b] : sRandomPlayerbotMgr.GetAllBots())
        if (b && b != asker && b->IsInWorld() && b->GetTeamId() == asker->GetTeamId() && WovInCapital(b) && !b->GetGroup() &&
            b->GetLevel() >= 10)
            crafters.push_back(b);
    std::shuffle(crafters.begin(), crafters.end(), std::mt19937(std::random_device{}()));
    if (crafters.size() > 12)
        crafters.resize(12);
    uint8 lvl = asker->GetLevel();
    for (Player* c : crafters)
    {
        std::vector<std::pair<uint32, uint32>> opts;  // (item, spell)
        for (auto const& [item, spells] : g_wovCraftRecipes)
        {
            ItemTemplate const* p = sObjectMgr->GetItemTemplate(item);
            if (!p || p->Bonding == BIND_WHEN_PICKED_UP || p->RequiredLevel > lvl || p->RequiredLevel + 12 < lvl)
                continue;
            bool gear = (p->Class == ITEM_CLASS_WEAPON || p->Class == ITEM_CLASS_ARMOR) && p->Quality >= ITEM_QUALITY_UNCOMMON &&
                        p->RequiredLevel >= 5;
            bool use = p->Class == ITEM_CLASS_CONSUMABLE && p->RequiredLevel >= 5;
            if (!gear && !use)
                continue;
            for (uint32 sp : spells)
                if (c->HasSpell(sp) && WovProfName(sp))
                {
                    opts.push_back({item, sp});
                    break;
                }
        }
        if (opts.empty())
            continue;
        auto [item, sp] = opts[urand(0, opts.size() - 1)];
        ItemTemplate const* p = sObjectMgr->GetItemTemplate(item);
        PlayerbotAI* ai = GET_PLAYERBOT_AI(asker);
        if (!ai)
            return false;
        std::string link = ChatHelper::FormatItem(p);
        std::string prof = WovProfName(sp);
        ai->SayToChannel(WovRep(WovRep(WovPick(kWovCraftLf), "{prof}", prof), "{item}", link), ChatChannelId::TRADE);
        if (urand(0, 99) < 70)
        {
            std::string t = WovRep(WovRep(WovRep(WovPick(kWovCraftCan), "{prof}", prof), "{item}", link), "{asker}", asker->GetName());
            g_wovCraftPub.push_back({time(nullptr) + time_t(urand(15, 70)), c->GetGUID().GetCounter(), t});
        }
        LOG_INFO("playerbots", "WoV craft: {} asks Trade for {} ({} {} can make it)", asker->GetName(), p->Name1, prof, c->GetName());
        return true;
    }
    return false;
}

void WovCraftPubTick(time_t now)
{
    for (size_t i = 0; i < g_wovCraftPub.size();)
    {
        if (now < g_wovCraftPub[i].due)
        {
            ++i;
            continue;
        }
        WovCraftPub e = g_wovCraftPub[i];
        g_wovCraftPub.erase(g_wovCraftPub.begin() + i);
        Player* b = ObjectAccessor::FindPlayerByLowGUID(e.bot);
        PlayerbotAI* ai = b ? GET_PLAYERBOT_AI(b) : nullptr;
        if (ai && b->IsInWorld() && WovInCapital(b))
            ai->SayToChannel(e.text, ChatChannelId::TRADE);
    }
}

void WovCraft_Tick(time_t now)
{
    static time_t last = 0, cfgT = 0, mailT = 0;
    static bool loaded = false;
    if (now >= cfgT + 60)
    {
        cfgT = now;
        g_wovCraftEnable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.Craft.Enable", false);
    }
    if (!g_wovCraftEnable || now < last + 3)
        return;
    last = now;
    WovCraftPubTick(now);
    if (!loaded)
    {
        loaded = true;
        WovCraftBuild();
        if (QueryResult r = CharacterDatabase.Query("SELECT bot, player, item, spell, UNIX_TIMESTAMP(created) FROM wov_craft_order "
                                                    "WHERE status='open' AND created > NOW() - INTERVAL 3 DAY"))
            do
            {
                Field* f = r->Fetch();
                g_wovCraftOrders[{f[0].Get<uint32>(), f[1].Get<uint32>()}] = {f[2].Get<uint32>(), f[3].Get<uint32>(),
                                                                               time_t(f[4].Get<uint64>())};
            } while (r->NextRow());
        LOG_INFO("playerbots", "WoV craft: {} craftable items, {} open orders", g_wovCraftRecipes.size(), g_wovCraftOrders.size());
    }
    std::vector<WovCraftAsk> asks;
    {
        std::lock_guard<std::mutex> lock(g_wovCraftMutex);
        asks.swap(g_wovCraftAsks);
    }
    for (WovCraftAsk const& a : asks)
        WovCraftAskHandle(a, now);
    for (auto it = g_wovCraftSays.begin(); it != g_wovCraftSays.end();)
    {
        if (now < it->due)
        {
            ++it;
            continue;
        }
        Player* b = ObjectAccessor::FindPlayerByLowGUID(it->bot);
        Player* p = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(it->player));
        if (b && p && b->IsInWorld())
            b->Whisper(it->text, LANG_UNIVERSAL, p);
        it = g_wovCraftSays.erase(it);
    }
    if (now >= mailT + 20)
    {
        mailT = now;
        WovCraftMail(now);
        for (auto it = g_wovCraftOrders.begin(); it != g_wovCraftOrders.end();)
        {
            if (now >= it->second.t + kWovCraftOrderSec)
            {
                CharacterDatabase.Execute("UPDATE wov_craft_order SET status='expired', updated=NOW() WHERE bot={} AND "
                                          "player={} AND status='open'",
                                          it->first.first, it->first.second);
                it = g_wovCraftOrders.erase(it);
            }
            else
                ++it;
        }
    }
}
}  // namespace

// Used by mod-ollama-chat too: such a message is answered by the craft code, not by the language model.
bool WovCraft_Claims(std::string const& msg, bool whisper)
{
    if (!g_wovCraftEnable || msg.size() > 255)
        return false;
    uint32 item = WovCraftItemOf(msg);
    if (!item)
        return false;
    WovCraftBuild();
    if (!g_wovCraftRecipes.count(item))
        return false;
    return WovCraftWords(msg, whisper);
}

// Whisper hook (Playerbots.cpp). True when the whisper is a crafting request; it is then not a bot command.
bool WovCraft_OnWhisper(Player* from, Player* to, std::string const& msg)
{
    if (!from || !to || sRandomPlayerbotMgr.IsRandomBot(from) || !sRandomPlayerbotMgr.IsRandomBot(to))
        return false;
    if (!WovCraft_Claims(msg, true))
        return false;
    std::lock_guard<std::mutex> lock(g_wovCraftMutex);
    g_wovCraftAsks.push_back({from->GetGUID().GetCounter(), to->GetGUID().GetCounter(), msg});
    return true;
}

void WovCraft_OnChannel(Player* player, std::string const& msg)
{
    if (!player || sRandomPlayerbotMgr.IsRandomBot(player) || !WovCraft_Claims(msg, false))
        return;
    std::lock_guard<std::mutex> lock(g_wovCraftMutex);
    g_wovCraftAsks.push_back({player->GetGUID().GetCounter(), 0, msg});
}
// ---- end wov: crafting orders -------------------------------------------------------------------------

// ---- wov: regear once ----------------------------------------------------------------------------------
// Bots used to get a fresh set of blues at every level. Each random bot is re-geared ONCE with ordinary
// leveling gear (greens at most, often worse; RandomGearLoweringChance) a few at a time; after that, gear
// comes only from what they loot and quest (AutoUpgradeEquip = 0). Done bots are listed in wov_regear.
#include "PlayerbotFactory.h"
namespace
{
void WovRegear_Tick(time_t now)
{
    static time_t last = 0, cfgT = 0;
    static bool enable = false;
    static uint32 perTick = 4;
    static std::unordered_set<uint32> done;
    static bool loaded = false;
    if (now >= cfgT + 60)
    {
        cfgT = now;
        enable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.RegearOnce.Enable", false);
        perTick = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.RegearOnce.PerTick", 4);
    }
    if (!enable || now < last + 10)
        return;
    last = now;
    if (!loaded)
    {
        loaded = true;
        if (QueryResult r = CharacterDatabase.Query("SELECT guid FROM wov_regear"))
            do
                done.insert(r->Fetch()[0].Get<uint32>());
            while (r->NextRow());
    }
    uint32 n = 0;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (n >= perTick)
            break;
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->GetGroup() || bot->IsInFlight())
            continue;
        if (bot->GetMap() && bot->GetMap()->Instanceable())
            continue;
        uint32 low = bot->GetGUID().GetCounter();
        if (done.count(low))
            continue;
        done.insert(low);
        PlayerbotFactory factory(bot, bot->GetLevel(), ITEM_QUALITY_UNCOMMON);
        factory.InitEquipment(false);
        CharacterDatabase.Execute("INSERT IGNORE INTO wov_regear (guid) VALUES ({})", low);
        ++n;
    }
}
// wov: bracket regear. Regear-once left many 50+ bots in grey/white items, which no real player wears.
// Each random bot now gets ONE fresh set per level bracket (30-39, 40-49, 50-59, 60) at the quality a real
// player of that level typically has: mostly greens with some blues while levelling, blues (some epics) at 60.
// Between brackets gear still only comes from loot and quests. Done brackets are listed in wov_regear_bracket.
void WovRegearBracket_Tick(time_t now)
{
    static time_t last = 0, cfgT = 0;
    static bool enable = false;
    static uint32 perTick = 3, minLevel = 30;
    static std::set<std::pair<uint32, uint32>> done;
    static bool loaded = false;
    if (now >= cfgT + 60)
    {
        cfgT = now;
        enable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.RegearBracket.Enable", false);
        perTick = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.RegearBracket.PerTick", 3);
        minLevel = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.RegearBracket.MinLevel", 30);
    }
    if (!enable || now < last + 10)
        return;
    last = now;
    if (!loaded)
    {
        loaded = true;
        if (QueryResult r = CharacterDatabase.Query("SELECT guid, bracket FROM wov_regear_bracket"))
            do
            {
                Field* f = r->Fetch();
                done.insert({f[0].Get<uint32>(), uint32(f[1].Get<uint8>())});
            } while (r->NextRow());
    }
    uint32 n = 0;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (n >= perTick)
            break;
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->GetGroup() || bot->IsInFlight())
            continue;
        if (bot->GetMap() && bot->GetMap()->Instanceable())
            continue;
        uint32 level = bot->GetLevel();
        if (level < minLevel)
            continue;
        uint32 bracket = std::min<uint32>(level, 60) / 10;
        uint32 low = bot->GetGUID().GetCounter();
        if (done.count({low, bracket}))
            continue;
        done.insert({low, bracket});
        uint32 roll = urand(0, 99);
        uint32 quality = ITEM_QUALITY_UNCOMMON;
        if (level >= 60)
            quality = roll < 15 ? ITEM_QUALITY_EPIC : ITEM_QUALITY_RARE;
        else if (bracket == 5)
            quality = roll < 70 ? ITEM_QUALITY_RARE : ITEM_QUALITY_UNCOMMON;
        else if (bracket == 4)
            quality = roll < 40 ? ITEM_QUALITY_RARE : ITEM_QUALITY_UNCOMMON;
        else if (roll < 15)
            quality = ITEM_QUALITY_RARE;
        PlayerbotFactory factory(bot, level, quality);
        // second_chance: clear each slot first, otherwise the old grey/white item can stay when the new one
        // cannot be equipped over it (measured: 25-45 % grey/white right after a plain InitEquipment).
        factory.InitEquipment(false, true);
        CharacterDatabase.Execute("INSERT IGNORE INTO wov_regear_bracket (guid, bracket, quality) VALUES ({}, {}, {})", low, bracket,
                                  quality);
        ++n;
    }
}
}  // namespace
// ---- end wov: regear once ------------------------------------------------------------------------------
// ---- wov: market ---------------------------------------------------------------------------------------
// Bots use the auction house the way players do. Now and then a bot makes a trip to its faction's auction
// house (walks when close, else flies), puts surplus loot up (AuctionPosting, in the npc-reach hook), buys one
// gear upgrade it can afford, then walks to the nearest mailbox and collects what the house mailed it (won items,
// sale proceeds, expired listings). Every packet goes through the bot's own session, so the core does all checks.
// Bot gold is brought down once per bot to a Classic-like curve (wov_wealth) and new bots start on that curve.
#include "AuctionHouseMgr.h"
#include "ItemUsageValue.h"
#include "GameTime.h"
#include <atomic>
#include "Mail.h"
namespace
{
struct WovAuctioneer
{
    uint32 entry = 0, map = 0;
    float x = 0, y = 0, z = 0;
    TeamId team = TEAM_NEUTRAL;
};
struct WovAhLot
{
    uint32 id = 0, entry = 0, count = 0, buyout = 0, ownerLow = 0;
};
struct WovTrip
{
    int phase = 1;  // 1 to the auctioneer, 2 at the auction house, 3 to the mailbox
    uint32 entry = 0, map = 0;
    float x = 0, y = 0, z = 0;
    time_t start = 0, phaseT = 0;
    ObjectGuid target;
};
struct WovReach
{
    uint32 low = 0;
    int kind = 0;  // 1 auctioneer, 2 mailbox
    ObjectGuid obj;
};

std::vector<WovAuctioneer> g_wovAuctioneers;
std::mutex g_wovAhMutex;
std::unordered_map<AuctionHouseObject const*, std::vector<WovAhLot>> g_wovAhLots;
std::unordered_map<uint32, time_t> g_wovAhBought;  // bot -> last purchase (under g_wovAhMutex)
std::unordered_map<uint32, uint32> g_wovAhBoughtEntry;  // bot -> item entry it bought (under g_wovAhMutex)
std::unordered_map<uint32, WovTrip> g_wovTrips;     // world thread only
bool WovOnMarketTrip(uint32 low) { return g_wovTrips.count(low) != 0; }  // wov: dungeon runs
std::mutex g_wovReachMutex;
std::vector<WovReach> g_wovReached;
std::vector<std::tuple<uint32, uint32, time_t>> g_wovEquipLater;  // (bot, item low guid, when): world thread
std::atomic<bool> g_wovMarketEnable{false};
std::atomic<uint32> g_wovMarketBudgetPct{35};
uint32 g_wovMarketTrips = 40;  // expected trips per bot per hour, in hundredths
bool g_wovWealthEnable = false;

void WovMarketConfig(time_t now)
{
    static time_t cfgT = 0;
    if (now < cfgT + 60)
        return;
    cfgT = now;
    g_wovMarketEnable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.Market.Enable", false);
    g_wovMarketTrips = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Market.TripsPerHourPct", 40);
    g_wovMarketBudgetPct = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.Market.BudgetPct", 35);
    g_wovWealthEnable = sConfigMgr->GetOption<bool>("AiPlayerbot.WoV.Wealth.Enable", false);
}

void WovAuctioneersLoad()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    QueryResult r = WorldDatabase.Query(
        "SELECT c.id, c.map, c.position_x, c.position_y, c.position_z, t.faction FROM creature c JOIN creature_template t "
        "ON t.entry = c.id WHERE (t.npcflag & 2097152) <> 0 AND c.map IN (0, 1)");
    if (!r)
        return;
    AuctionHouseObject* ally = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Alliance);
    AuctionHouseObject* horde = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Horde);
    do
    {
        Field* f = r->Fetch();
        WovAuctioneer a;
        a.entry = f[0].Get<uint32>();
        a.map = f[1].Get<uint32>();
        a.x = f[2].Get<float>();
        a.y = f[3].Get<float>();
        a.z = f[4].Get<float>();
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(f[5].Get<uint32>());
        a.team = house == ally ? TEAM_ALLIANCE : house == horde ? TEAM_HORDE : TEAM_NEUTRAL;
        if (a.team != TEAM_NEUTRAL)
            g_wovAuctioneers.push_back(a);
    } while (r->NextRow());
    LOG_INFO("playerbots", "WoV market: {} faction auctioneers", g_wovAuctioneers.size());
}

void WovAhSnapshot()
{
    std::unordered_map<AuctionHouseObject const*, std::vector<WovAhLot>> lots;
    for (AuctionHouseId id : {AuctionHouseId::Alliance, AuctionHouseId::Horde})
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(id);
        if (!house)
            continue;
        std::vector<WovAhLot>& v = lots[house];
        for (auto const& [aid, a] : house->GetAuctions())
            if (a && a->buyout && a->itemCount == 1)
                v.push_back({a->Id, a->item_template, a->itemCount, a->buyout, uint32(a->owner.GetCounter())});
    }
    std::lock_guard<std::mutex> lock(g_wovAhMutex);
    g_wovAhLots.swap(lots);
}

// The bot (map thread, its own AI) is standing at an auctioneer: buy one upgrade it can afford.
void WovAhBuy(PlayerbotAI* ai, Player* bot, Creature* auctioneer)
{
    uint32 low = bot->GetGUID().GetCounter();
    time_t now = time(nullptr);
    std::vector<WovAhLot> lots;
    {
        std::lock_guard<std::mutex> lock(g_wovAhMutex);
        auto b = g_wovAhBought.find(low);
        if (b != g_wovAhBought.end() && now < b->second + 1800)
            return;
        auto it = g_wovAhLots.find(sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction()));
        if (it == g_wovAhLots.end())
            return;
        lots = it->second;
    }
    uint64 budget = uint64(bot->GetMoney()) * g_wovMarketBudgetPct / 100;
    std::vector<std::pair<WovAhLot, ItemTemplate const*>> cands;
    for (WovAhLot const& lot : lots)
    {
        if (lot.ownerLow == low || lot.buyout > budget)
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(lot.entry);
        if (!proto || proto->InventoryType == INVTYPE_NON_EQUIP || proto->RequiredLevel > bot->GetLevel())
            continue;
        if (proto->Quality < ITEM_QUALITY_NORMAL || proto->Quality > ITEM_QUALITY_RARE)
            continue;
        if (bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        cands.push_back({lot, proto});
    }
    if (cands.empty())
        return;
    std::shuffle(cands.begin(), cands.end(), std::mt19937(std::random_device{}()));
    if (cands.size() > 80)
        cands.resize(80);
    WovAhLot const* best = nullptr;
    ItemTemplate const* bestProto = nullptr;
    for (auto const& [lot, proto] : cands)
    {
        ItemUsage usage = ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", int32(lot.entry))->Get();
        if (usage != ITEM_USAGE_EQUIP && usage != ITEM_USAGE_REPLACE)
            continue;
        if (!best || proto->ItemLevel > bestProto->ItemLevel ||
            (proto->ItemLevel == bestProto->ItemLevel && lot.buyout < best->buyout))
        {
            best = &lot;
            bestProto = proto;
        }
    }
    if (!best)
        return;
    {
        std::lock_guard<std::mutex> lock(g_wovAhMutex);
        g_wovAhBought[low] = now;
        g_wovAhBoughtEntry[low] = best->entry;
    }
    WorldPacket* p = new WorldPacket(CMSG_AUCTION_PLACE_BID);  // QueuePacket takes ownership
    *p << auctioneer->GetGUID();
    *p << best->id;
    *p << best->buyout;
    bot->GetSession()->QueuePacket(p);
    LOG_INFO("playerbots", "WoV market: {} (lvl {}) buys {} for {}c", bot->GetName(), bot->GetLevel(), bestProto->Name1,
             best->buyout);
}

// World thread: the bot stands at a mailbox; collect everything the auction house sent.
void WovTakeMail(Player* bot, ObjectGuid mailbox)
{
    time_t now = GameTime::GetGameTime().count();
    uint32 n = 0;
    for (Mail* m : bot->GetMails())
    {
        if (!m || m->state == MAIL_STATE_DELETED || m->deliver_time > now || m->COD || m->messageType != MAIL_AUCTION)
            continue;
        for (MailItemInfo const& it : m->items)
        {
            g_wovEquipLater.emplace_back(bot->GetGUID().GetCounter(), it.item_guid, time(nullptr) + 5);
            WorldPacket* p = new WorldPacket(CMSG_MAIL_TAKE_ITEM);
            *p << mailbox << m->messageID << it.item_guid;
            bot->GetSession()->QueuePacket(p);
        }
        if (m->money)
        {
            WorldPacket* p = new WorldPacket(CMSG_MAIL_TAKE_MONEY);
            *p << mailbox << m->messageID;
            bot->GetSession()->QueuePacket(p);
        }
        WorldPacket* d = new WorldPacket(CMSG_MAIL_DELETE);
        *d << mailbox << m->messageID << uint32(m->mailTemplateId);
        bot->GetSession()->QueuePacket(d);
        if (++n >= 20)
            break;
    }
    if (n)
        LOG_INFO("playerbots", "WoV market: {} empties {} auction mails", bot->GetName(), n);
}

bool WovAimAt(Player* bot, PlayerbotAI* ai, ObjectGuid target)
{
    if (ai->rpgInfo.GetStatus() != RPG_WANDER_NPC)
        ai->rpgInfo.ChangeToWanderNpc();
    auto* data = std::get_if<NewRpgInfo::WanderNpc>(&ai->rpgInfo.data);
    if (!data)
        return false;
    if (data->npcOrGo != target)
    {
        data->npcOrGo = target;
        data->lastReach = 0;
    }
    return true;
}

bool WovTripStart(Player* bot, PlayerbotAI* ai, time_t now)
{
    WovAuctioneer const* best = nullptr;
    float bestD = 1.0e9f;
    for (WovAuctioneer const& a : g_wovAuctioneers)
    {
        if (a.team != bot->GetTeamId() || a.map != bot->GetMapId())
            continue;
        float d = bot->GetExactDist(a.x, a.y, a.z);
        if (d < bestD)
        {
            bestD = d;
            best = &a;
        }
    }
    if (!best)
        return false;
    WovTrip t;
    t.entry = best->entry;
    t.map = best->map;
    t.x = best->x;
    t.y = best->y;
    t.z = best->z;
    t.start = t.phaseT = now;
    if (bestD > 600.0f)
    {
        TravelMgr::FlightMasterInfo const* fm = sTravelMgr.GetNearestFlightMasterInfo(bot);
        uint32 to = sObjectMgr->GetNearestTaxiNode(t.x, t.y, t.z, t.map, bot->GetTeamId());
        if (!fm || !fm->taxiNodeId || !to || fm->taxiNodeId == to)
            return false;
        std::vector<uint32> path = sTravelNodeMap.FindTaxiPath(fm->taxiNodeId, to);
        if (path.empty())
            return false;
        ai->rpgInfo.ChangeToTravelFlight(fm->templateEntry, fm->pos, path);
    }
    g_wovTrips[bot->GetGUID().GetCounter()] = t;
    LOG_INFO("playerbots", "WoV market trip: {} (lvl {}) heads to the auction house, {:.0f} yd{}", bot->GetName(),
             bot->GetLevel(), bestD, bestD > 600.0f ? " by air" : "");
    return true;
}

void WovWealthTick()
{
    static std::unordered_set<uint32> done;
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        if (QueryResult r = CharacterDatabase.Query("SELECT guid FROM wov_wealth"))
            do
                done.insert(r->Fetch()[0].Get<uint32>());
            while (r->NextRow());
    }
    uint32 n = 0;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (n >= 25)
            break;
        if (!bot || !bot->IsInWorld())
            continue;
        uint32 low = bot->GetGUID().GetCounter();
        if (done.count(low))
            continue;
        done.insert(low);
        ++n;
        uint64 target = WovWealthTarget(bot);
        if (bot->GetMoney() > target * 2)
            bot->SetMoney(uint32(target * urand(60, 140) / 100));
        CharacterDatabase.Execute("INSERT IGNORE INTO wov_wealth (guid) VALUES ({})", low);
    }
}

std::mutex g_wovForceMutex;
std::unordered_set<uint32> g_wovForce;  // bots on an errand: kept active although no real player is near

void WovMarket_Tick(time_t now)
{
    WovMarketConfig(now);
    static time_t last = 0, lastSnap = 0;
    if (now < last + 10)
        return;
    last = now;
    {
        std::unordered_set<uint32> force;
        for (auto const& [low, t] : g_wovTrips)
            force.insert(low);
        for (auto const& [low, sk] : g_wovBotSk)
            force.insert(low);
        for (auto const& [low, until] : g_wovBusyBot)  // wov: dungeon runs and groups being formed
            if (now < time_t(until))
                force.insert(low);
        std::lock_guard<std::mutex> lock(g_wovForceMutex);
        g_wovForce.swap(force);
    }
    if (g_wovWealthEnable)
        WovWealthTick();
    // AiPlayerbot.WoV.Debug.Bots = "Name1,Name2": log what these bots are doing every 10 s (reload config to change)
    {
        static time_t dbgCfgT = 0;
        static std::vector<std::string> dbgNames;
        if (now >= dbgCfgT + 60)
        {
            dbgCfgT = now;
            dbgNames.clear();
            std::string list = sConfigMgr->GetOption<std::string>("AiPlayerbot.WoV.Debug.Bots", "");
            size_t p = 0;
            while (p < list.size())
            {
                size_t q = list.find(',', p);
                std::string n = list.substr(p, q == std::string::npos ? std::string::npos : q - p);
                n.erase(0, n.find_first_not_of(' '));
                n.erase(n.find_last_not_of(' ') + 1);
                if (!n.empty())
                    dbgNames.push_back(n);
                if (q == std::string::npos)
                    break;
                p = q + 1;
            }
        }
        for (std::string const& n : dbgNames)
        {
            Player* d = ObjectAccessor::FindPlayerByName(n, false);
            PlayerbotAI* dai = d ? GET_PLAYERBOT_AI(d) : nullptr;
            if (!d || !dai)
                continue;
            std::string strat;
            for (std::string const& st : dai->GetStrategies(BOT_STATE_NON_COMBAT))
                strat += st + " ";
            LOG_INFO("playerbots", "WoV debug {}: rpg {} pos {:.0f},{:.0f},{:.0f} area {} moving {} motion {} combat {} form {} "
                     "group {} master {} | {}", d->GetName(), uint32(dai->rpgInfo.GetStatus()), d->GetPositionX(),
                     d->GetPositionY(), d->GetPositionZ(), d->GetAreaId(), d->isMoving(),
                     uint32(d->GetMotionMaster()->GetCurrentMovementGeneratorType()), d->IsInCombat(),
                     uint32(d->GetShapeshiftForm()), d->GetGroup() != nullptr,
                     dai->GetMaster() ? dai->GetMaster()->GetName() : "-", strat);
        }
    }
    if (!g_wovMarketEnable)
        return;
    WovAuctioneersLoad();
    if (now >= lastSnap + 60)
    {
        lastSnap = now;
        WovAhSnapshot();
    }

    // items taken from the mail: tell the bot it got them, so its own AI considers equipping them
    for (size_t i = 0; i < g_wovEquipLater.size();)
    {
        auto [botLow, itemLow, when] = g_wovEquipLater[i];
        if (now < when)
        {
            ++i;
            continue;
        }
        if (Player* bot = ObjectAccessor::FindPlayerByLowGUID(botLow))
            if (Item* item = bot->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemLow)))
            {
                bool bought = false;
                {
                    std::lock_guard<std::mutex> lock(g_wovAhMutex);
                    auto b = g_wovAhBoughtEntry.find(botLow);
                    if (b != g_wovAhBoughtEntry.end() && b->second == item->GetEntry())
                    {
                        bought = true;
                        g_wovAhBoughtEntry.erase(b);
                    }
                }
                if (bought && !item->IsEquipped())
                {
                    WorldPacket* p = new WorldPacket(CMSG_AUTOEQUIP_ITEM);  // put on what it bought, as a player would
                    *p << uint8(item->GetBagSlot()) << uint8(item->GetSlot());
                    bot->GetSession()->QueuePacket(p);
                    LOG_INFO("playerbots", "WoV market: {} puts on {}", bot->GetName(), item->GetTemplate()->Name1);
                }
            }
        g_wovEquipLater.erase(g_wovEquipLater.begin() + i);
    }

    std::vector<WovReach> reached;
    {
        std::lock_guard<std::mutex> lock(g_wovReachMutex);
        reached.swap(g_wovReached);
    }
    for (WovReach const& r : reached)
    {
        Player* bot = ObjectAccessor::FindPlayerByLowGUID(r.low);
        auto t = g_wovTrips.find(r.low);
        if (r.kind == 1 && bot)
            LOG_INFO("playerbots", "WoV market trip: {} at the auctioneer", bot->GetName());
        if (r.kind == 1 && t != g_wovTrips.end() && t->second.phase == 1)
        {
            t->second.phase = 2;
            t->second.phaseT = now;
        }
        else if (r.kind == 2)
        {
            if (bot && bot->IsInWorld())
                WovTakeMail(bot, r.obj);
            if (t != g_wovTrips.end())
                g_wovTrips.erase(t);
        }
    }

    static time_t lastDump = 0;
    bool dump = now >= lastDump + 120;
    if (dump)
        lastDump = now;
    for (auto it = g_wovTrips.begin(); it != g_wovTrips.end();)
    {
        WovTrip& t = it->second;
        if (dump)
            if (Player* db = ObjectAccessor::FindPlayerByLowGUID(it->first))
                if (PlayerbotAI* dai = GET_PLAYERBOT_AI(db))
                    LOG_INFO("playerbots", "WoV market dbg: {} phase {} dist {:.0f} status {} flying {} moving {} age {}",
                             db->GetName(), t.phase, db->GetMapId() == t.map ? db->GetExactDist(t.x, t.y, t.z) : -1.0f,
                             uint32(dai->rpgInfo.GetStatus()), db->IsInFlight(), db->isMoving(), uint32(now - t.start));
        Player* bot = ObjectAccessor::FindPlayerByLowGUID(it->first);
        PlayerbotAI* ai = bot ? GET_PLAYERBOT_AI(bot) : nullptr;
        if (!bot || !ai || !bot->IsInWorld() || now > t.start + 2400 || bot->GetGroup() || bot->InBattleground() ||
            g_wovBotSk.count(it->first))
        {
            if (bot)
                LOG_INFO("playerbots", "WoV market trip: {} gave up (phase {})", bot->GetName(), t.phase);
            it = g_wovTrips.erase(it);
            continue;
        }
        if (bot->IsInFlight() || ai->rpgInfo.GetStatus() == RPG_TRAVEL_FLIGHT || !bot->IsAlive() || bot->IsInCombat())
        {
            if (!bot->IsInFlight() && bot->IsAlive() && !bot->IsInCombat() && ai->rpgInfo.GetStatus() == RPG_TRAVEL_FLIGHT &&
                WovStuck(bot, now, 90))
                WovUnstick(bot, ai, WorldPosition(t.map, t.x, t.y, t.z), "market flight");
            ++it;
            continue;
        }
        if (bot->GetMapId() != t.map)
        {
            LOG_INFO("playerbots", "WoV market trip: {} left the map", bot->GetName());
            it = g_wovTrips.erase(it);
            continue;
        }
        if (t.phase == 1)
        {
            float d = bot->GetExactDist(t.x, t.y, t.z);
            if (d > 1500.0f)
            {
                LOG_INFO("playerbots", "WoV market trip: {} landed too far away ({:.0f} yd)", bot->GetName(), d);
                it = g_wovTrips.erase(it);  // the flight did not bring it here
                continue;
            }
            if (d > 40.0f)
            {
                if (ai->rpgInfo.GetStatus() != RPG_GO_CAMP)
                    ai->rpgInfo.ChangeToGoCamp(WorldPosition(t.map, t.x, t.y, t.z));
                else if (WovStuck(bot, now, 120))
                    WovUnstick(bot, ai, WorldPosition(t.map, t.x, t.y, t.z), "market walk");
            }
            else
            {
                if (!t.target)
                    if (Creature* c = bot->FindNearestCreature(t.entry, 60.0f))
                        t.target = c->GetGUID();
                if (!t.target || !WovAimAt(bot, ai, t.target))
                {
                    LOG_INFO("playerbots", "WoV market trip: {} cannot find or aim at the auctioneer", bot->GetName());
                    it = g_wovTrips.erase(it);
                    continue;
                }
            }
        }
        else if (t.phase == 2 && now >= t.phaseT + 8)
        {
            GameObject* mb = bot->FindNearestGameObjectOfType(GAMEOBJECT_TYPE_MAILBOX, 150.0f);
            LOG_INFO("playerbots", "WoV market trip: {} to the mailbox ({:.0f} yd, {} mails)", bot->GetName(),
                     mb ? bot->GetExactDist(mb) : -1.0f, bot->GetMailSize());
            if (!mb)
            {
                it = g_wovTrips.erase(it);
                continue;
            }
            t.phase = 3;
            t.phaseT = now;
            t.target = mb->GetGUID();
            WovAimAt(bot, ai, t.target);
        }
        else if (t.phase == 3)
        {
            if (!bot->GetMailSize() || now > t.phaseT + 120)
            {
                LOG_INFO("playerbots", "WoV market trip: {} leaves the mailbox ({} mails, {} s, status {})", bot->GetName(),
                         bot->GetMailSize(), uint32(now - t.phaseT), uint32(ai->rpgInfo.GetStatus()));
                it = g_wovTrips.erase(it);  // nothing to fetch, or could not get there
                continue;
            }
            GameObject* mb = ObjectAccessor::GetGameObject(*bot, t.target);
            if (mb && bot->GetExactDist(mb) < 4.5f)
            {
                WovTakeMail(bot, t.target);  // standing at it; the npc-reach hook did not fire
                it = g_wovTrips.erase(it);
                continue;
            }
            if (mb && bot->GetExactDist(mb) < 25.0f && now > t.phaseT + 30 && !bot->isMoving())
                bot->GetMotionMaster()->MovePoint(0, mb->GetPositionX(), mb->GetPositionY(), mb->GetPositionZ());
            else
                WovAimAt(bot, ai, t.target);
        }
        ++it;
    }

    // new trips
    uint32 started = 0;
    for (auto const& [guid, bot] : sRandomPlayerbotMgr.GetAllBots())
    {
        if (started >= 6)
            break;
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsInFlight() || bot->GetGroup())
            continue;
        if (bot->GetLevel() < 10 || !WovOpenWorld(bot) || bot->InBattleground())
            continue;
        uint32 low = bot->GetGUID().GetCounter();
        if (g_wovTrips.count(low) || g_wovBotSk.count(low))
            continue;
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai || ai->rpgInfo.GetStatus() == RPG_TRAVEL_FLIGHT)
            continue;
        uint32 rate = g_wovMarketTrips;  // hundredths of a trip per hour; one roll per 10 s = 360 per hour
        if (WovDriveOf(low) == 3)
            rate *= 2;
        if (bot->GetFreeInventorySpace() < 4)
            rate *= 3;
        if (urand(0, 35999) >= rate)
            continue;
        if (WovTripStart(bot, ai, now))
            ++started;
    }
}
}  // namespace

// Median buyout of one item in this faction's auction house right now (0 when nobody sells it).
uint32 WovMarketPriceOf(uint32 entry, TeamId team)
{
    AuctionHouseObject const* house =
        sAuctionMgr->GetAuctionsMapByHouseId(team == TEAM_ALLIANCE ? AuctionHouseId::Alliance : AuctionHouseId::Horde);
    std::vector<uint32> prices;
    {
        std::lock_guard<std::mutex> lock(g_wovAhMutex);
        auto it = g_wovAhLots.find(house);
        if (it == g_wovAhLots.end())
            return 0;
        for (WovAhLot const& lot : it->second)
            if (lot.entry == entry)
                prices.push_back(lot.buyout);
    }
    if (prices.empty())
        return 0;
    std::nth_element(prices.begin(), prices.begin() + prices.size() / 2, prices.end());
    return prices[prices.size() / 2];
}

// Called from PlayerbotAI::AllowActive (map threads).
bool WovForceActive(uint32 low)
{
    std::lock_guard<std::mutex> lock(g_wovForceMutex);
    return g_wovForce.count(low) > 0;
}

// Called from NewRpgWanderNpcAction when a random bot reaches an npc or a game object (map thread).
void WovMarket_OnReach(PlayerbotAI* ai, Player* bot, WorldObject* obj)
{
    if (!g_wovMarketEnable || !ai || !bot || !obj || !sRandomPlayerbotMgr.IsRandomBot(bot))
        return;
    WovReach r;
    r.low = bot->GetGUID().GetCounter();
    r.obj = obj->GetGUID();
    if (Creature* c = obj->ToCreature())
    {
        if (!c->HasNpcFlag(UNIT_NPC_FLAG_AUCTIONEER))
            return;
        WovAhBuy(ai, bot, c);
        r.kind = 1;
    }
    else if (GameObject* go = obj->ToGameObject())
    {
        if (go->GetGoType() != GAMEOBJECT_TYPE_MAILBOX || !bot->GetMailSize())
            return;
        r.kind = 2;
    }
    else
        return;
    std::lock_guard<std::mutex> lock(g_wovReachMutex);
    g_wovReached.push_back(r);
}
// ---- end wov: market -----------------------------------------------------------------------------------
#include "PlayerbotFactory.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "Position.h"
#include "RaceMgr.h"
#include "RandomBotLevelMgr.h"
#include "Random.h"
#include "RandomPlayerbotFactory.h"
#include "ServerFacade.h"
#include "SharedDefines.h"
#include "TravelMgr.h"
#include "Unit.h"
#include "World.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <boost/thread/thread.hpp>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <set>
#include <utility>

struct GuidClassRaceInfo
{
    ObjectGuid::LowType guid;
    uint32 rClass;
    uint32 rRace;
};

void PrintStatsThread() { sRandomPlayerbotMgr.PrintStats(); }

void activatePrintStatsThread()
{
    boost::thread t(PrintStatsThread);
    t.detach();
}

void CheckBgQueueThread() { sRandomPlayerbotMgr.CheckBgQueue(); }

void activateCheckBgQueueThread()
{
    boost::thread t(CheckBgQueueThread);
    t.detach();
}

void CheckLfgQueueThread() { sRandomPlayerbotMgr.CheckLfgQueue(); }

void activateCheckLfgQueueThread()
{
    boost::thread t(CheckLfgQueueThread);
    t.detach();
}

void CheckPlayersThread() { sRandomPlayerbotMgr.CheckPlayers(); }

void activateCheckPlayersThread()
{
    boost::thread t(CheckPlayersThread);
    t.detach();
}

class botPIDImpl
{
public:
    botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd);
    ~botPIDImpl();
    double calculate(double setpoint, double pv);
    void adjust(double Kp, double Ki, double Kd)
    {
        _Kp = Kp;
        _Ki = Ki;
        _Kd = Kd;
    }
    void reset() { _integral = 0; }

private:
    double _dt;
    double _max;
    double _min;
    double _Kp;
    double _Ki;
    double _Kd;
    double _pre_error;
    double _integral;
};

botPID::botPID(double dt, double max, double min, double Kp, double Ki, double Kd)
{
    pimpl = new botPIDImpl(dt, max, min, Kp, Ki, Kd);
}
void botPID::adjust(double Kp, double Ki, double Kd) { pimpl->adjust(Kp, Ki, Kd); }
void botPID::reset() { pimpl->reset(); }
double botPID::calculate(double setpoint, double pv) { return pimpl->calculate(setpoint, pv); }
botPID::~botPID() { delete pimpl; }

/**
 * Implementation
 */
botPIDImpl::botPIDImpl(double dt, double max, double min, double Kp, double Ki, double Kd)
    : _dt(dt), _max(max), _min(min), _Kp(Kp), _Ki(Ki), _Kd(Kd), _pre_error(0), _integral(0)
{
}

double botPIDImpl::calculate(double setpoint, double pv)
{
    // Calculate error
    double error = setpoint - pv;

    // Proportional term
    double Pout = _Kp * error;

    // Integral term
    _integral += error * _dt;
    double Iout = _Ki * _integral;

    // Derivative term
    double derivative = (error - _pre_error) / _dt;
    double Dout = _Kd * derivative;

    // Calculate total output
    double output = Pout + Iout + Dout;

    // Restrict to max/min
    if (output > _max)
    {
        output = _max;
        _integral -= error * _dt;  // Stop integral buildup at max
    }
    else if (output < _min)
    {
        output = _min;
        _integral -= error * _dt;  // Stop integral buildup at min
    }

    // Save error to previous error
    _pre_error = error;

    return output;
}

botPIDImpl::~botPIDImpl() {}

uint32 RandomPlayerbotMgr::GetMaxAllowedBotCount() { return GetEventValue(0, "bot_count"); }

void RandomPlayerbotMgr::LogPlayerLocation()
{
    activeBots = 0;

    try
    {
        sPlayerbotAIConfig.openLog("player_location.csv", "w");

        if (sPlayerbotAIConfig.randomBotAutologin)
        {
            for (auto i : GetAllBots())
            {
                Player* bot = i.second;
                if (!bot)
                    continue;

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << "RND"
                    << ",";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(bot).printWKT(out);
                out << bot->GetOrientation() << ",";
                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                out << bot->GetMapId() << ",";
                out << bot->GetLevel() << ",";
                out << bot->GetHealth() << ",";
                out << bot->GetPowerPct(bot->getPowerType()) << ",";
                out << bot->GetMoney() << ",";

                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                {
                    out << std::to_string(uint8(botAI->GetGrouperType())) << ",";
                    out << std::to_string(uint8(botAI->GetGuilderType())) << ",";
                    out << (botAI->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                    out << (botAI->IsActive() ? "active" : "delay") << ",";
                    out << botAI->HandleRemoteCommand("state") << ",";

                    if (botAI->AllowActivity(ALL_ACTIVITY))
                        activeBots++;
                }
                else
                {
                    out << 0 << "," << 0 << ",err,err,err,";
                }

                out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                out << (bot->isDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive");

                sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());
            }

            for (auto i : GetPlayers())
            {
                Player* bot = i;
                if (!bot)
                    continue;

                std::ostringstream out;
                out << sPlayerbotAIConfig.GetTimestampStr() << "+00,";
                out << "PLR"
                    << ",";
                out << bot->GetName() << ",";
                out << std::fixed << std::setprecision(2);
                WorldPosition(bot).printWKT(out);
                out << bot->GetOrientation() << ",";
                out << std::to_string(bot->getRace()) << ",";
                out << std::to_string(bot->getClass()) << ",";
                out << bot->GetMapId() << ",";
                out << bot->GetLevel() << ",";
                out << bot->GetHealth() << ",";
                out << bot->GetPowerPct(bot->getPowerType()) << ",";
                out << bot->GetMoney() << ",";

                if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
                {
                    out << std::to_string(uint8(botAI->GetGrouperType())) << ",";
                    out << std::to_string(uint8(botAI->GetGuilderType())) << ",";
                    out << (botAI->AllowActivity(ALL_ACTIVITY) ? "active" : "inactive") << ",";
                    out << (botAI->IsActive() ? "active" : "delay") << ",";
                    out << botAI->HandleRemoteCommand("state") << ",";

                    if (botAI->AllowActivity(ALL_ACTIVITY))
                        activeBots++;
                }
                else
                {
                    out << 0 << "," << 0 << ",player,player,player,";
                }

                out << (bot->IsInCombat() ? "combat" : "safe") << ",";
                out << (bot->isDead() ? (bot->GetCorpse() ? "ghost" : "dead") : "alive");

                sPlayerbotAIConfig.log("player_location.csv", out.str().c_str());
            }
        }
    }
    catch (...)
    {
        return;
        // This is to prevent some thread-unsafeness. Crashes would happen if bots get added or removed.
        // We really don't care here. Just skip a log. Making this thread-safe is not worth the effort.
    }
}

void RandomPlayerbotMgr::UpdateAIInternal(uint32 /*elapsed*/, bool /*minimal*/)
{
    if (totalPmo)
        totalPmo->finish();

    totalPmo = sPerfMonitor.start(PERF_MON_TOTAL, "RandomPlayerbotMgr::FullTick");

    if (!sPlayerbotAIConfig.randomBotAutologin || !sPlayerbotAIConfig.enabled)
        return;

    CompanyStanding::instance().Update();  // local: company standing (custom wow plans/14, step C)
    WovWar_Tick(time(nullptr));  // wov: Call to Arms
    WovInit_Tick(time(nullptr));  // wov: initiative
    WovTogether_Tick(time(nullptr));  // wov: play together
    WovDungeon_Tick(time(nullptr));  // wov: dungeon talk
    WovDungeonRun_Tick(time(nullptr));  // wov: dungeon runs
    WovRegear_Tick(time(nullptr));  // wov: regear once
    WovRegearBracket_Tick(time(nullptr));  // wov: one fresh set per level bracket
    WovMarket_Tick(time(nullptr));  // wov: market
    WovCraft_Tick(time(nullptr));  // wov: crafting orders

    /*if (sPlayerbotAIConfig.enablePrototypePerformanceDiff)
    {
        LOG_INFO("playerbots", "---------------------------------------");
        LOG_INFO("playerbots",
                 "PROTOTYPE: Playerbot performance enhancements are active. Issues and instability may occur.");
        LOG_INFO("playerbots", "---------------------------------------");
        ScaleBotActivity();
    }*/

    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    if (!maxAllowedBotCount || (maxAllowedBotCount < sPlayerbotAIConfig.minRandomBots ||
                                maxAllowedBotCount > sPlayerbotAIConfig.maxRandomBots))
    {
        maxAllowedBotCount = urand(sPlayerbotAIConfig.minRandomBots, sPlayerbotAIConfig.maxRandomBots);
        SetEventValue(0, "bot_count", maxAllowedBotCount,
                      urand(sPlayerbotAIConfig.randomBotCountChangeMinInterval,
                            sPlayerbotAIConfig.randomBotCountChangeMaxInterval));
    }

    GetBots();
    // Copied deliberately: ProcessBot() erases from currentBots while this is
    // being iterated below, so the loops must run over a snapshot.
    std::unordered_set<uint32> availableBots = currentBots;
    uint32 availableBotCount = availableBots.size();
    uint32 onlineBotCount = playerBots.size();

    uint32 onlineBotFocus = 75;
    if (onlineBotCount < (uint32)(sPlayerbotAIConfig.minRandomBots * 90 / 100))
        onlineBotFocus = 25;

    // only keep updating till initializing time has completed,
    // which prevents unneeded expensive GameTime calls.
    if (_isBotInitializing)
    {
        _isBotInitializing = GameTime::GetUptime().count() < sPlayerbotAIConfig.maxRandomBots * (0.11 + 0.4);
    }

    uint32 updateIntervalTurboBoost = _isBotInitializing ? 1 : sPlayerbotAIConfig.randomBotUpdateInterval;
    SetNextCheckDelay(updateIntervalTurboBoost * (onlineBotFocus + 25) * 10);

    PerfMonitorOperation* pmo = sPerfMonitor.start(
        PERF_MON_TOTAL,
        onlineBotCount < maxAllowedBotCount ? "RandomPlayerbotMgr::Login" : "RandomPlayerbotMgr::UpdateAIInternal");

    bool realPlayerIsLogged = false;
    if (sPlayerbotAIConfig.disabledWithoutRealPlayer)
    {
        if (sWorldSessionMgr->GetActiveAndQueuedSessionCount() > 0)
        {
            RealPlayerLastTimeSeen = time(nullptr);
            realPlayerIsLogged = true;

            if (DelayLoginBotsTimer == 0)
            {
                DelayLoginBotsTimer = time(nullptr) + sPlayerbotAIConfig.disabledWithoutRealPlayerLoginDelay;
            }
        }
        else
        {
            if (DelayLoginBotsTimer)
            {
                DelayLoginBotsTimer = 0;
            }

            if (RealPlayerLastTimeSeen != 0 && onlineBotCount > 0 &&
                time(nullptr) > RealPlayerLastTimeSeen + sPlayerbotAIConfig.disabledWithoutRealPlayerLogoutDelay)
            {
                LogoutAllBots();
                LOG_INFO("playerbots", "Logout all bots due no real player session.");
            }
        }

        if (availableBotCount < maxAllowedBotCount &&
            (sPlayerbotAIConfig.disabledWithoutRealPlayer == false ||
             (realPlayerIsLogged && DelayLoginBotsTimer != 0 && time(nullptr) >= DelayLoginBotsTimer)))
        {
            AddRandomBots();
        }
    }
    else if (availableBotCount < maxAllowedBotCount)
    {
        AddRandomBots();
    }

    if (sPlayerbotAIConfig.syncLevelWithPlayers && !players.empty())
    {
        if (time(nullptr) > (PlayersCheckTimer + 60))
            sRandomPlayerbotMgr.CheckPlayers();
    }

    if (sPlayerbotAIConfig.randomBotJoinBG /* && !players.empty()*/)
    {
        if (time(nullptr) > (BgCheckTimer + 35))
            sRandomPlayerbotMgr.CheckBgQueue();
    }

    if (sPlayerbotAIConfig.randomBotJoinLfg /* && !players.empty()*/)
    {
        if (time(nullptr) > (LfgCheckTimer + 30))
            sRandomPlayerbotMgr.CheckLfgQueue();
    }

    if (sPlayerbotAIConfig.randomBotAutologin && sPlayerbotAIConfig.randomBotPrintStatsInterval &&
        time(nullptr) > (printStatsTimer + sPlayerbotAIConfig.randomBotPrintStatsInterval))
    {
        if (!printStatsTimer)
        {
            printStatsTimer = time(nullptr);
        }
        else
        {
            sRandomPlayerbotMgr.PrintStats();
            // activatePrintStatsThread();
        }
    }
    uint32 updateBots = sPlayerbotAIConfig.randomBotsPerInterval * onlineBotFocus / 100;
    uint32 maxNewBots =
        onlineBotCount < maxAllowedBotCount &&
                (sPlayerbotAIConfig.disabledWithoutRealPlayer == false ||
                 (realPlayerIsLogged && DelayLoginBotsTimer != 0 && time(nullptr) >= DelayLoginBotsTimer))
            ? maxAllowedBotCount - onlineBotCount
            : 0;
    uint32 loginBots = std::min(sPlayerbotAIConfig.randomBotsPerInterval - updateBots, maxNewBots);

    if (!availableBots.empty())
    {
        // Update bots
        for (auto bot : availableBots)
        {
            if (!GetPlayerBot(bot))
                continue;

            if (ProcessBot(bot))
            {
                updateBots--;
            }

            if (!updateBots)
                break;
        }

        if (loginBots && botLoading.empty())
        {
            loginBots += updateBots;
            loginBots = std::min(loginBots, maxNewBots);

            LOG_DEBUG("playerbots", "{} new bots prepared to login", loginBots);

            // Log in bots
            for (auto bot : availableBots)
            {
                if (GetPlayerBot(bot))
                    continue;

                if (ProcessBot(bot))
                {
                    loginBots--;
                }

                if (!loginBots)
                    break;
            }

            DelayLoginBotsTimer = 0;
        }
    }

    if (pmo)
        pmo->finish();

    if (sPlayerbotAIConfig.hasLog("player_location.csv"))
    {
        LogPlayerLocation();
    }
}

// void RandomPlayerbotMgr::ScaleBotActivity()
//{
//     float activityPercentage = getActivityPercentage();
//
//     // if (activityPercentage >= 100.0f || activityPercentage <= 0.0f) pid.reset(); //Stop integer buildup during
//     // max/min activity
//
//     //    % increase/decrease                   wanted diff                                         , avg diff
//     float activityPercentageMod = pid.calculate(
//         sRandomPlayerbotMgr.GetPlayers().empty() ? sPlayerbotAIConfig.diffEmpty :
//         sPlayerbotAIConfig.diffWithPlayer, sWorldUpdateTime.GetAverageUpdateTime());
//
//     activityPercentage = activityPercentageMod + 50;
//
//     // Cap the percentage between 0 and 100.
//     activityPercentage = std::max(0.0f, std::min(100.0f, activityPercentage));
//
//     setActivityPercentage(activityPercentage);
// }

// Assigns accounts as RNDbot accounts (type 1) based on MaxRandomBots and EnablePeriodicOnlineOffline and its ratio,
// and assigns accounts as AddClass accounts (type 2) based AddClassAccountPoolSize. Type 1 and 2 assignments are
// permenant, unless MaxRandomBots or AddClassAccountPoolSize are set to 0. If so, their associated accounts will
// be unassigned (type 0)
void RandomPlayerbotMgr::AssignAccountTypes()
{
    LOG_INFO("playerbots", "Assigning account types for random bot accounts...");

    // Clear existing filtered lists
    rndBotTypeAccounts.clear();
    addClassTypeAccounts.clear();

    // First, get ALL randombot accounts from the database
    std::vector<uint32> allRandomBotAccounts;
    QueryResult allAccounts = LoginDatabase.Query(
        "SELECT id FROM account WHERE username LIKE '{}%%' ORDER BY id",
        sPlayerbotAIConfig.randomBotAccountPrefix.c_str());

    if (allAccounts)
    {
        do
        {
            Field* fields = allAccounts->Fetch();
            uint32 accountId = fields[0].Get<uint32>();
            allRandomBotAccounts.push_back(accountId);
        } while (allAccounts->NextRow());
    }

    LOG_INFO("playerbots", "Found {} total randombot accounts in database", allRandomBotAccounts.size());

    // Check existing assignments
    PlayerbotsDatabasePreparedStatement* assignmentsStmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_ACCOUNT_TYPE);
    PreparedQueryResult existingAssignments = PlayerbotsDatabase.Query(assignmentsStmt);
    std::map<uint32, uint8> currentAssignments;

    if (existingAssignments)
    {
        do
        {
            Field* fields = existingAssignments->Fetch();
            uint32 accountId = fields[0].Get<uint32>();
            uint8 accountType = fields[1].Get<uint8>();
            currentAssignments[accountId] = accountType;
        } while (existingAssignments->NextRow());
    }

    // Mark ALL randombot accounts as unassigned if not already assigned
    for (uint32 accountId : allRandomBotAccounts)
    {
        if (currentAssignments.find(accountId) == currentAssignments.end())
        {
            PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_ACCOUNT_TYPE);
            stmt->SetData(0, accountId);
            stmt->SetData(1, uint8(0));
            PlayerbotsDatabase.Execute(stmt);
            currentAssignments[accountId] = 0;
        }
    }

    // Calculate needed RNDbot accounts
    uint32 neededRndBotAccounts = 0;
    if (sPlayerbotAIConfig.maxRandomBots > 0)
    {
        int divisor = RandomPlayerbotFactory::CalculateAvailableCharsPerAccount();
        int maxBots = sPlayerbotAIConfig.maxRandomBots;

        // Take periodic online-offline into account
        if (sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        {
            maxBots *= sPlayerbotAIConfig.periodicOnlineOfflineRatio;
        }

        // Calculate base accounts needed for RNDbots, ensuring round up for maxBots not cleanly divisible by the divisor
        neededRndBotAccounts = (maxBots + divisor - 1) / divisor;
    }

    // Count existing assigned accounts
    uint32 existingRndBotAccounts = 0;
    uint32 existingAddClassAccounts = 0;

    for (auto const& [accountId, accountType] : currentAssignments)
    {
        if (accountType == 1) existingRndBotAccounts++;
        else if (accountType == 2) existingAddClassAccounts++;
    }

    // Assign RNDbot accounts from lowest position if needed
    if (existingRndBotAccounts < neededRndBotAccounts)
    {
        uint32 toAssign = neededRndBotAccounts - existingRndBotAccounts;
        uint32 assigned = 0;

        for (uint32 i = 0; i < allRandomBotAccounts.size() && assigned < toAssign; i++)
        {
            uint32 accountId = allRandomBotAccounts[i];
            if (currentAssignments[accountId] == 0) // Unassigned
            {
                PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_ACCOUNT_TYPE);
                stmt->SetData(0, uint8(1));
                stmt->SetData(1, accountId);
                PlayerbotsDatabase.Execute(stmt);
                currentAssignments[accountId] = 1;
                assigned++;
            }
        }

        if (assigned < toAssign)
        {
            LOG_ERROR("playerbots", "Not enough unassigned accounts to fulfill RNDbot requirements. Need {} more accounts.", toAssign - assigned);
        }
    }

    // Assign AddClass accounts from highest position if needed
    uint32 neededAddClassAccounts = sPlayerbotAIConfig.addClassAccountPoolSize;

    if (existingAddClassAccounts < neededAddClassAccounts)
    {
        uint32 toAssign = neededAddClassAccounts - existingAddClassAccounts;
        uint32 assigned = 0;

        for (size_t idx = allRandomBotAccounts.size(); idx-- > 0 && assigned < toAssign;)
        {
            uint32 accountId = allRandomBotAccounts[idx];
            if (currentAssignments[accountId] == 0) // Unassigned
            {
                PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_ACCOUNT_TYPE);
                stmt->SetData(0, uint8(2));
                stmt->SetData(1, accountId);
                PlayerbotsDatabase.Execute(stmt);
                currentAssignments[accountId] = 2;
                assigned++;
            }
        }

        if (assigned < toAssign)
        {
            LOG_ERROR("playerbots", "Not enough unassigned accounts to fulfill AddClass requirements. Need {} more accounts.", toAssign - assigned);
        }
    }

    // Populate filtered account lists with ALL accounts of each type
    for (auto const& [accountId, accountType] : currentAssignments)
    {
        if (accountType == 1) rndBotTypeAccounts.push_back(accountId);
        else if (accountType == 2) addClassTypeAccounts.push_back(accountId);
    }

    LOG_INFO("playerbots", "Account type assignment complete: {} RNDbot accounts, {} AddClass accounts, {} unassigned",
             rndBotTypeAccounts.size(), addClassTypeAccounts.size(),
             currentAssignments.size() - rndBotTypeAccounts.size() - addClassTypeAccounts.size());
}

bool RandomPlayerbotMgr::IsAccountType(uint32 accountId, uint8 accountType)
{
    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_ACCOUNT_TYPE_BY_ACCOUNT_AND_TYPE);
    stmt->SetData(0, accountId);
    stmt->SetData(1, accountType);
    return PlayerbotsDatabase.Query(stmt) != nullptr;
}

// Logs-in bots in 4 phases. Phase 1 logs Alliance bots up to how much is expected according to the faction ratio,
// and Phase 2 logs-in the remainder Horde bots to reach the total maxAllowedBotCount. If maxAllowedBotCount is not
// reached after Phase 2, the function goes back to log-in Alliance bots and reach maxAllowedBotCount. This is done
// because not every account is guaranteed 5A/5H bots, so the true ratio might be skewed by few percentages. Finally,
// Phase 4 is reached if and only if the value of RandomBotAccountCount is lower than it should.
uint32 RandomPlayerbotMgr::AddRandomBots()
{
    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    WovLoadSchedules();  // wov
    if (g_wovSchedEnable)
    {
        static time_t wovLastScan = 0;
        if (time(nullptr) < wovLastScan + 60)
            return currentBots.size();
        wovLastScan = time(nullptr);
    }
    static time_t missingBotsTimer = 0;

    if (currentBots.size() < maxAllowedBotCount)
    {
        // Calculate how many bots to add
        maxAllowedBotCount -= currentBots.size();
        maxAllowedBotCount = std::min(sPlayerbotAIConfig.randomBotsPerInterval, maxAllowedBotCount);

        // Single RNG instance for all shuffling
        std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());

        // Only need to track the Alliance count, as it's in Phase 1
        uint32 totalRatio = sPlayerbotAIConfig.randomBotAllianceRatio + sPlayerbotAIConfig.randomBotHordeRatio;
        uint32 allowedAllianceCount = maxAllowedBotCount * (sPlayerbotAIConfig.randomBotAllianceRatio) / totalRatio;

        uint32 remainder = maxAllowedBotCount * (sPlayerbotAIConfig.randomBotAllianceRatio) % totalRatio;

        // Fix #1082: Randomly add one based on reminder
        if (remainder && urand(1, totalRatio) <= remainder)
        {
            allowedAllianceCount++;
        }

        // Determine which accounts to use based on EnablePeriodicOnlineOffline
        std::vector<uint32> accountsToUse;
        if (sPlayerbotAIConfig.enablePeriodicOnlineOffline)
        {

            // Calculate how many accounts can be used
            // With enablePeriodicOnlineOffline, don't use all of rndBotTypeAccounts right away. Fraction results are rounded up
            uint32 accountsToUseCount = (rndBotTypeAccounts.size() + sPlayerbotAIConfig.periodicOnlineOfflineRatio - 1)
                                        / sPlayerbotAIConfig.periodicOnlineOfflineRatio;

            // Randomly select accounts
            std::vector<uint32> shuffledAccounts = rndBotTypeAccounts;
            std::shuffle(shuffledAccounts.begin(), shuffledAccounts.end(), rng);

            for (uint32 i = 0; i < accountsToUseCount && i < shuffledAccounts.size(); i++)
            {
                accountsToUse.push_back(shuffledAccounts[i]);
            }
        }
        else
        {
            accountsToUse = rndBotTypeAccounts;
        }

        // Pre-map all characters from selected accounts
        struct CharacterInfo
        {
            uint32 guid;
            uint8 rClass;
            uint8 rRace;
            uint32 accountId;
        };
        std::vector<CharacterInfo> allCharacters;

        for (uint32 accountId : accountsToUse)
        {
            CharacterDatabasePreparedStatement* stmt =
                CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARS_BY_ACCOUNT_ID);
            stmt->SetData(0, accountId);
            PreparedQueryResult result = CharacterDatabase.Query(stmt);
            if (!result)
                continue;

            do
            {
                Field* fields = result->Fetch();
                CharacterInfo info;
                info.guid = fields[0].Get<uint32>();
                info.rClass = fields[1].Get<uint8>();
                info.rRace = fields[2].Get<uint8>();
                info.accountId = accountId;
                allCharacters.push_back(info);
            } while (result->NextRow());
        }

        // Shuffle for class balance
        std::shuffle(allCharacters.begin(), allCharacters.end(), rng);

        // wov: fill the bands that are short of their share first (levels are never changed, only who logs in)
        if (g_wovSchedEnable && g_wovBands)
        {
            uint32 online[7];
            WovBandCounts(currentBots, online);
            uint32 target = uint32(currentBots.size()) + maxAllowedBotCount;
            std::vector<CharacterInfo> byBand[7];
            for (auto const& ci : allCharacters)
                byBand[WovBandOfGuid(ci.guid)].push_back(ci);
            std::vector<CharacterInfo> ordered;
            ordered.reserve(allCharacters.size());
            size_t idx[7] = {0, 0, 0, 0, 0, 0, 0};
            while (ordered.size() < allCharacters.size())
            {
                int best = -1;
                double bestGap = -1e18;
                for (int b = 0; b < 7; ++b)
                {
                    if (idx[b] >= byBand[b].size())
                        continue;
                    double gap = double(g_wovBandPct[b]) * target / 100.0 - double(online[b]);
                    if (gap > bestGap)
                    {
                        bestGap = gap;
                        best = b;
                    }
                }
                if (best < 0)
                    break;
                ordered.push_back(byBand[best][idx[best]++]);
                ++online[best];  // as if it logged in; scheduled-off characters later in the list still fill in
            }
            allCharacters.swap(ordered);
        }

        // wov: raid night: the guild's raiders log in first
        std::stable_partition(allCharacters.begin(), allCharacters.end(),
                              [](CharacterInfo const& ci) { return WovRaidNightFor(ci.guid); });

        // Separate characters by faction for phased login
        std::vector<CharacterInfo> allianceChars;
        std::vector<CharacterInfo> hordeChars;

        for (auto const& charInfo : allCharacters)
        {
            if (IsAlliance(charInfo.rRace))
                allianceChars.push_back(charInfo);

            else
                hordeChars.push_back(charInfo);
        }

        // Lambda to handle bot login logic
        auto tryLoginBot = [&](CharacterInfo const& charInfo) -> bool
        {
            if (GetEventValue(charInfo.guid, "add") ||
                GetEventValue(charInfo.guid, "logout") ||
                GetPlayerBot(charInfo.guid) ||
                currentBots.contains(charInfo.guid) ||
                (sPlayerbotAIConfig.disableDeathKnightLogin && charInfo.rClass == CLASS_DEATH_KNIGHT) ||
                // local: release-schedule era gate
                (sPlayerbotAIConfig.eraExpansion < 2 && charInfo.rClass == CLASS_DEATH_KNIGHT) ||
                (sPlayerbotAIConfig.eraExpansion < 1 &&
                 (charInfo.rRace == RACE_BLOODELF || charInfo.rRace == RACE_DRAENEI)))
            {
                return false;
            }

            // wov: only while the player behind this character would be playing (or to keep the floor)
            if (g_wovSchedEnable && !WovScheduledOnline(charInfo.guid) && currentBots.size() >= g_wovMinOnline)
                return false;

            uint32 add_time = sPlayerbotAIConfig.enablePeriodicOnlineOffline
                                ? urand(sPlayerbotAIConfig.minRandomBotInWorldTime,
                                        sPlayerbotAIConfig.maxRandomBotInWorldTime)
                                : sPlayerbotAIConfig.permanentlyInWorldTime;

            SetEventValue(charInfo.guid, "add", 1, add_time);
            SetEventValue(charInfo.guid, "logout", 0, 0);
            currentBots.insert(charInfo.guid);

            return true;
        };

        // PHASE 1: Log-in Alliance bots up to allowedAllianceCount
        for (auto const& charInfo : allianceChars)
        {
            if (!allowedAllianceCount)
                break;

            if (tryLoginBot(charInfo))
            {
                maxAllowedBotCount--;
                allowedAllianceCount--;
            }
        }

        // PHASE 2: Log-in Horde bots up to maxAllowedBotCount
        for (auto const& charInfo : hordeChars)
        {
            if (!maxAllowedBotCount)
                break;

            if (tryLoginBot(charInfo))
                maxAllowedBotCount--;
        }

        // PHASE 3: If maxAllowedBotCount wasn't reached, log-in more Alliance bots
        for (auto const& charInfo : allianceChars)
        {
            if (!maxAllowedBotCount)
                break;

            if (tryLoginBot(charInfo))
                maxAllowedBotCount--;
        }

        // PHASE 4: An error is given if maxAllowedBotCount is still not reached
        if (maxAllowedBotCount)
        {
            if (missingBotsTimer == 0)
                missingBotsTimer = time(nullptr);

            if (time(nullptr) - missingBotsTimer >= 10)
            {
                int divisor = RandomPlayerbotFactory::CalculateAvailableCharsPerAccount();
                uint32 moreAccountsNeeded = (maxAllowedBotCount + divisor - 1) / divisor;
                LOG_ERROR("playerbots",
                          "Can't log-in all the requested bots. Try increasing RandomBotAccountCount in your conf file.\n"
                          "{} more accounts needed.", moreAccountsNeeded);
                missingBotsTimer = 0;    // Reset timer so error is not spammed every tick
            }
        }
        else
        {
            missingBotsTimer = 0;       // Reset timer if logins for this interval were successful
        }
    }
    else
    {
        missingBotsTimer = 0;           // Reset timer if there's enough bots
    }

    return currentBots.size();
}

void RandomPlayerbotMgr::LoadBattleMastersCache()
{
    BattleMastersCache.clear();

    LOG_INFO("playerbots", "Loading Battlemasters Cache...");

    QueryResult result = WorldDatabase.Query("SELECT `entry`,`bg_template` FROM `battlemaster_entry`");

    uint32 count = 0;

    if (!result)
    {
        return;
    }

    do
    {
        ++count;

        Field* fields = result->Fetch();

        uint32 entry = fields[0].Get<uint32>();
        uint32 bgTypeId = fields[1].Get<uint32>();

        CreatureTemplate const* bmaster = sObjectMgr->GetCreatureTemplate(entry);
        if (!bmaster)
            continue;

        FactionTemplateEntry const* bmFaction = sFactionTemplateStore.LookupEntry(bmaster->faction);
        uint32 bmFactionId = bmFaction->faction;
        FactionEntry const* bmParentFaction = sFactionStore.LookupEntry(bmFactionId);
        uint32 bmParentTeam = bmParentFaction->team;
        TeamId bmTeam = TEAM_NEUTRAL;
        if (bmParentTeam == 891)
            bmTeam = TEAM_ALLIANCE;

        if (bmFactionId == 189)
            bmTeam = TEAM_ALLIANCE;

        if (bmParentTeam == 892)
            bmTeam = TEAM_HORDE;

        if (bmFactionId == 66)
            bmTeam = TEAM_HORDE;

        BattleMastersCache[bmTeam][BattlegroundTypeId(bgTypeId)].insert(
            BattleMastersCache[bmTeam][BattlegroundTypeId(bgTypeId)].end(), entry);
        LOG_DEBUG("playerbots", "Cached Battlemaster #{} for BG Type {} ({})", entry, bgTypeId,
                  bmTeam == TEAM_ALLIANCE ? "Alliance"
                  : bmTeam == TEAM_HORDE  ? "Horde"
                                          : "Neutral");

    } while (result->NextRow());

    LOG_INFO("playerbots", ">> Loaded {} battlemaster entries", count);
}

std::vector<uint32> parseBrackets(std::string const& str)
{
    std::vector<uint32> brackets;
    std::stringstream ss(str);
    std::string item;

    while (std::getline(ss, item, ','))
    {
        brackets.push_back(static_cast<uint32>(std::stoi(item)));
    }

    return brackets;
}

void RandomPlayerbotMgr::CheckBgQueue()
{
    if (!BgCheckTimer)
    {
        BgCheckTimer = time(nullptr);
        return;  // Exit immediately after initializing the timer
    }

    if (time(nullptr) < BgCheckTimer)
    {
        return;  // No need to proceed if the current time is less than the timer
    }

    // Update the timer to the current time
    BgCheckTimer = time(nullptr);

    LOG_DEBUG("playerbots", "Checking BG Queue...");

    // Initialize Battleground Data (do not clear here)

    for (int bracket = BG_BRACKET_ID_FIRST; bracket < MAX_BATTLEGROUND_BRACKETS; ++bracket)
    {
        for (int queueType = BATTLEGROUND_QUEUE_AV; queueType < MAX_BATTLEGROUND_QUEUE_TYPES; ++queueType)
        {
            BattlegroundData[queueType][bracket] = BattlegroundInfo();
        }
    }

    // A participant may level past the queue's bracket while waiting or inside the match.
    // Use the bracket assigned by the queue or the instance, never their current level.
    auto getQueueBracket = [&](Player* participant, Battleground* bg, uint8 slot,
                               BattlegroundQueueTypeId queueTypeId, uint32 mapId) -> PvPDifficultyEntry const*
    {
        if (participant->InBattleground() && participant->GetCurrentBattlegroundQueueSlot() == slot)
            return bg ? GetBattlegroundBracketById(mapId, bg->GetBracketId()) : nullptr;

        GroupQueueInfo groupInfo;
        BattlegroundQueue& queue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
        if (!queue.GetPlayerGroupInfoData(participant->GetGUID(), &groupInfo))
            return nullptr;
        return GetBattlegroundBracketById(mapId, BattlegroundBracketId(groupInfo.BracketId));
    };

    // Process real players and populate Battleground Data with player/queue count
    // Opens a queue for bots to join
    for (Player* player : players)
    {
        // Skip player if not currently in a queue
        if (!player->InBattlegroundQueue())
            continue;

        Battleground* bg = player->GetBattleground(true);
        if (bg && bg->GetStatus() == STATUS_WAIT_LEAVE)
            continue;

        TeamId teamId = player->GetTeamId();

        for (uint8 queueType = 0; queueType < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueType)
        {
            BattlegroundQueueTypeId queueTypeId = player->GetBattlegroundQueueTypeId(queueType);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            // Check if real player is able to create/join this queue
            BattlegroundTypeId bgTypeId = sBattlegroundMgr->BGTemplateId(queueTypeId);
            uint32 mapId = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId)->GetMapId();
            PvPDifficultyEntry const* pvpDiff = getQueueBracket(player, bg, queueType, queueTypeId, mapId);
            if (!pvpDiff)
                continue;

            // If player is allowed, populate the BattlegroundData with the appropriate level requirements
            BattlegroundBracketId bracketId = pvpDiff->GetBracketId();
            BattlegroundData[queueTypeId][bracketId].minLevel = pvpDiff->minLevel;
            BattlegroundData[queueTypeId][bracketId].maxLevel = pvpDiff->maxLevel;

            // Arena logic
            bool isRated = false;
            if (BattlegroundMgr::BGArenaType(queueTypeId))
            {
                BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
                GroupQueueInfo ginfo;

                if (bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
                {
                    isRated = ginfo.IsRated;
                }

                if (bgQueue.IsPlayerInvitedToRatedArena(player->GetGUID()) ||
                    (player->InArena() && bg->isRated()))
                    isRated = true;

                if (isRated)
                    BattlegroundData[queueTypeId][bracketId].ratedArenaPlayerCount++;
                else
                    BattlegroundData[queueTypeId][bracketId].skirmishArenaPlayerCount++;
            }
            // BG Logic
            else
            {
                if (teamId == TEAM_ALLIANCE)
                    BattlegroundData[queueTypeId][bracketId].bgAlliancePlayerCount++;
                else
                    BattlegroundData[queueTypeId][bracketId].bgHordePlayerCount++;

                // If a player has joined the BG, update the instance count in BattlegroundData (for consistency)
                if (player->InBattleground() && player->GetCurrentBattlegroundQueueSlot() == queueType)
                {
                    std::vector<uint32>* instanceIds = nullptr;
                    uint32 instanceId = bg->GetInstanceID();

                    instanceIds = &BattlegroundData[queueTypeId][bracketId].bgInstances;
                    if (instanceIds &&
                        std::find(instanceIds->begin(), instanceIds->end(), instanceId) == instanceIds->end())
                        instanceIds->push_back(instanceId);

                    BattlegroundData[queueTypeId][bracketId].bgInstanceCount = instanceIds->size();
                }
            }

            if (!player->IsInvitedForBattlegroundInstance() && !player->InBattleground())
            {
                if (BattlegroundMgr::BGArenaType(queueTypeId))
                {
                    if (isRated)
                        BattlegroundData[queueTypeId][bracketId].activeRatedArenaQueue = 1;
                    else
                        BattlegroundData[queueTypeId][bracketId].activeSkirmishArenaQueue = 1;
                }
                else
                {
                    BattlegroundData[queueTypeId][bracketId].activeBgQueue = 1;
                }
            }
        }
    }

    // Process player bots
    for (auto& [guid, bot] : playerBots)
    {
        if (!bot || !bot->InBattlegroundQueue() || !bot->IsInWorld() || !IsRandomBot(bot))
            continue;

        Battleground* bg = bot->GetBattleground(true);
        if (bg && bg->GetStatus() == STATUS_WAIT_LEAVE)
            continue;

        TeamId teamId = bot->GetTeamId();

        for (uint8 queueType = 0; queueType < PLAYER_MAX_BATTLEGROUND_QUEUES; ++queueType)
        {
            BattlegroundQueueTypeId queueTypeId = bot->GetBattlegroundQueueTypeId(queueType);
            if (queueTypeId == BATTLEGROUND_QUEUE_NONE)
                continue;

            BattlegroundTypeId bgTypeId = sBattlegroundMgr->BGTemplateId(queueTypeId);
            uint32 mapId = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId)->GetMapId();
            PvPDifficultyEntry const* pvpDiff = getQueueBracket(bot, bg, queueType, queueTypeId, mapId);
            if (!pvpDiff)
                continue;

            BattlegroundBracketId bracketId = pvpDiff->GetBracketId();
            BattlegroundData[queueTypeId][bracketId].minLevel = pvpDiff->minLevel;
            BattlegroundData[queueTypeId][bracketId].maxLevel = pvpDiff->maxLevel;

            if (BattlegroundMgr::BGArenaType(queueTypeId))
            {
                bool isRated = false;
                BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(queueTypeId);
                GroupQueueInfo ginfo;

                if (bgQueue.GetPlayerGroupInfoData(guid, &ginfo))
                {
                    isRated = ginfo.IsRated;
                }

                if (bgQueue.IsPlayerInvitedToRatedArena(guid) || (bot->InArena() && bg->isRated()))
                    isRated = true;

                if (isRated)
                    BattlegroundData[queueTypeId][bracketId].ratedArenaBotCount++;
                else
                    BattlegroundData[queueTypeId][bracketId].skirmishArenaBotCount++;
            }
            else
            {
                if (teamId == TEAM_ALLIANCE)
                    BattlegroundData[queueTypeId][bracketId].bgAllianceBotCount++;
                else
                    BattlegroundData[queueTypeId][bracketId].bgHordeBotCount++;
            }

            if (bot->InBattleground() && bot->GetCurrentBattlegroundQueueSlot() == queueType)
            {
                std::vector<uint32>* instanceIds = nullptr;
                uint32 instanceId = bg->GetInstanceID();
                bool isArena = false;
                bool isRated = false;

                // Arena logic
                if (bot->InArena())
                {
                    isArena = true;
                    if (bg->isRated())
                    {
                        isRated = true;
                        instanceIds = &BattlegroundData[queueTypeId][bracketId].ratedArenaInstances;
                    }
                    else
                    {
                        instanceIds = &BattlegroundData[queueTypeId][bracketId].skirmishArenaInstances;
                    }
                }
                // BG Logic
                else
                {
                    instanceIds = &BattlegroundData[queueTypeId][bracketId].bgInstances;
                }

                if (instanceIds &&
                    std::find(instanceIds->begin(), instanceIds->end(), instanceId) == instanceIds->end())
                    instanceIds->push_back(instanceId);

                if (isArena)
                {
                    if (isRated)
                        BattlegroundData[queueTypeId][bracketId].ratedArenaInstanceCount = instanceIds->size();
                    else
                        BattlegroundData[queueTypeId][bracketId].skirmishArenaInstanceCount = instanceIds->size();
                }
                else
                {
                    BattlegroundData[queueTypeId][bracketId].bgInstanceCount = instanceIds->size();
                }
            }
        }
    }

    // If enabled, wait for all bots to have logged in before queueing for Arena's / BG's
    // wov: the WoV schedule keeps about 900 of the 1000 bots online, so "all bots logged in" never happens;
    // start once AiPlayerbot.WoV.BgMinBots (default 400) are online instead.
    if (sPlayerbotAIConfig.randomBotAutoJoinBG &&
        playerBots.size() >= std::min<uint32>(GetMaxAllowedBotCount(),
                                               sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.BgMinBots", 400)))
    {
        uint32 randomBotAutoJoinArenaBracket = sPlayerbotAIConfig.randomBotAutoJoinArenaBracket;
        uint32 randomBotAutoJoinBGRatedArena2v2Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena2v2Count;
        uint32 randomBotAutoJoinBGRatedArena3v3Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena3v3Count;
        uint32 randomBotAutoJoinBGRatedArena5v5Count = sPlayerbotAIConfig.randomBotAutoJoinBGRatedArena5v5Count;

        uint32 randomBotAutoJoinBGICCount = sPlayerbotAIConfig.randomBotAutoJoinBGICCount;
        uint32 randomBotAutoJoinBGEYCount = sPlayerbotAIConfig.randomBotAutoJoinBGEYCount;
        uint32 randomBotAutoJoinBGAVCount = sPlayerbotAIConfig.randomBotAutoJoinBGAVCount;
        uint32 randomBotAutoJoinBGABCount = sPlayerbotAIConfig.randomBotAutoJoinBGABCount;
        uint32 randomBotAutoJoinBGWSCount = sPlayerbotAIConfig.randomBotAutoJoinBGWSCount;

        std::vector<uint32> icBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinICBrackets);
        std::vector<uint32> eyBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinEYBrackets);
        std::vector<uint32> avBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinAVBrackets);
        std::vector<uint32> abBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinABBrackets);
        std::vector<uint32> wsBrackets = parseBrackets(sPlayerbotAIConfig.randomBotAutoJoinWSBrackets);

        // Check both bgInstanceCount / bgInstances.size
        // to help counter against potentional inconsistencies
        auto updateRatedArenaInstanceCount = [&](uint32 queueType, uint32 bracket, uint32 minCount)
        {
            if (BattlegroundData[queueType][bracket].activeRatedArenaQueue == 0 &&
                BattlegroundData[queueType][bracket].ratedArenaInstanceCount < minCount &&
                BattlegroundData[queueType][bracket].ratedArenaInstances.size() < minCount)
                BattlegroundData[queueType][bracket].activeRatedArenaQueue = 1;
        };

        auto updateBGInstanceCount = [&](uint32 queueType, std::vector<uint32> brackets, uint32 minCount)
        {
            for (uint32 bracket : brackets)
            {
                if (BattlegroundData[queueType][bracket].activeBgQueue == 0 &&
                    BattlegroundData[queueType][bracket].bgInstanceCount < minCount &&
                    BattlegroundData[queueType][bracket].bgInstances.size() < minCount)
                    BattlegroundData[queueType][bracket].activeBgQueue = 1;
            }
        };

        // Update rated arena instance counts
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_2v2, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena2v2Count);
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_3v3, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena3v3Count);
        updateRatedArenaInstanceCount(BATTLEGROUND_QUEUE_5v5, randomBotAutoJoinArenaBracket,
                                      randomBotAutoJoinBGRatedArena5v5Count);

        // Update battleground instance counts
        updateBGInstanceCount(BATTLEGROUND_QUEUE_IC, icBrackets, randomBotAutoJoinBGICCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_EY, eyBrackets, randomBotAutoJoinBGEYCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_AV, avBrackets, randomBotAutoJoinBGAVCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_AB, abBrackets, randomBotAutoJoinBGABCount);
        updateBGInstanceCount(BATTLEGROUND_QUEUE_WS, wsBrackets, randomBotAutoJoinBGWSCount);
    }

    LogBattlegroundInfo();
}

void RandomPlayerbotMgr::LogBattlegroundInfo()
{
    for (auto const& queueTypePair : BattlegroundData)
    {
        uint8 queueType = queueTypePair.first;

        BattlegroundQueueTypeId queueTypeId = BattlegroundQueueTypeId(queueType);

        if (uint8 type = BattlegroundMgr::BGArenaType(queueTypeId))
        {
            for (auto const& bracketIdPair : queueTypePair.second)
            {
                auto& bgInfo = bracketIdPair.second;
                if (bgInfo.minLevel == 0)
                    continue;
                LOG_INFO("playerbots",
                         "ARENA:{} {}: Player (Skirmish:{}, Rated:{}) Bots (Skirmish:{}, Rated:{}) Total (Skirmish:{} "
                         "Rated:{}), Instances (Skirmish:{} Rated:{})",
                         type == ARENA_TYPE_2v2   ? "2v2"
                         : type == ARENA_TYPE_3v3 ? "3v3"
                                                  : "5v5",
                         std::to_string(bgInfo.minLevel) + "-" + std::to_string(bgInfo.maxLevel),
                         bgInfo.skirmishArenaPlayerCount, bgInfo.ratedArenaPlayerCount, bgInfo.skirmishArenaBotCount,
                         bgInfo.ratedArenaBotCount, bgInfo.skirmishArenaPlayerCount + bgInfo.skirmishArenaBotCount,
                         bgInfo.ratedArenaPlayerCount + bgInfo.ratedArenaBotCount, bgInfo.skirmishArenaInstanceCount,
                         bgInfo.ratedArenaInstanceCount);
            }
            continue;
        }

        BattlegroundTypeId bgTypeId = BattlegroundMgr::BGTemplateId(queueTypeId);
        std::string _bgType;
        switch (bgTypeId)
        {
            case BATTLEGROUND_AV:
                _bgType = "AV";
                break;
            case BATTLEGROUND_WS:
                _bgType = "WSG";
                break;
            case BATTLEGROUND_AB:
                _bgType = "AB";
                break;
            case BATTLEGROUND_EY:
                _bgType = "EotS";
                break;
            case BATTLEGROUND_RB:
                _bgType = "Random";
                break;
            case BATTLEGROUND_SA:
                _bgType = "SotA";
                break;
            case BATTLEGROUND_IC:
                _bgType = "IoC";
                break;
            default:
                _bgType = "Other";
                break;
        }

        for (auto const& bracketIdPair : queueTypePair.second)
        {
            auto& bgInfo = bracketIdPair.second;
            if (bgInfo.minLevel == 0)
                continue;

            LOG_INFO("playerbots",
                     "BG:{} {}: Player ({}:{}) Bot ({}:{}) Total (A:{} H:{}), Instances {}, Active Queue: {}", _bgType,
                     std::to_string(bgInfo.minLevel) + "-" + std::to_string(bgInfo.maxLevel),
                     bgInfo.bgAlliancePlayerCount, bgInfo.bgHordePlayerCount, bgInfo.bgAllianceBotCount,
                     bgInfo.bgHordeBotCount, bgInfo.bgAlliancePlayerCount + bgInfo.bgAllianceBotCount,
                     bgInfo.bgHordePlayerCount + bgInfo.bgHordeBotCount, bgInfo.bgInstanceCount, bgInfo.activeBgQueue);
        }
    }
    LOG_DEBUG("playerbots", "BG Queue check finished");
}

void RandomPlayerbotMgr::CheckLfgQueue()
{
    if (!LfgCheckTimer || time(nullptr) > (LfgCheckTimer + 30))
        LfgCheckTimer = time(nullptr);

    LOG_DEBUG("playerbots", "Checking LFG Queue...");

    // Clear LFG list
    LfgDungeons[TEAM_ALLIANCE].clear();
    LfgDungeons[TEAM_HORDE].clear();

    for (std::vector<Player*>::iterator i = players.begin(); i != players.end(); ++i)
    {
        Player* player = *i;
        if (!player || !player->IsInWorld())
            continue;

        Group* group = player->GetGroup();
        ObjectGuid guid = group ? group->GetGUID() : player->GetGUID();

        lfg::LfgState gState = sLFGMgr->GetState(guid);
        if (gState != lfg::LFG_STATE_NONE && gState < lfg::LFG_STATE_DUNGEON)
        {
            lfg::LfgDungeonSet const& dList = sLFGMgr->GetSelectedDungeons(player->GetGUID());
            for (lfg::LfgDungeonSet::const_iterator itr = dList.begin(); itr != dList.end(); ++itr)
            {
                lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(*itr);
                if (!dungeon)
                    continue;

                LfgDungeons[player->GetTeamId()].push_back(dungeon->id);
            }
        }
    }

    LOG_DEBUG("playerbots", "LFG Queue check finished");
}

void RandomPlayerbotMgr::CheckPlayers()
{
    if (!PlayersCheckTimer || time(nullptr) > (PlayersCheckTimer + 60))
        PlayersCheckTimer = time(nullptr);

    LOG_INFO("playerbots", "Checking Players...");

    if (!playersLevel)
        playersLevel = sPlayerbotAIConfig.randombotStartingLevel;

    for (std::vector<Player*>::iterator i = players.begin(); i != players.end(); ++i)
    {
        Player* player = *i;

        if (player->IsGameMaster())
            continue;

        // if (player->GetSession()->GetSecurity() > SEC_PLAYER)
        //     continue;

        if (player->GetLevel() > playersLevel)
            playersLevel = player->GetLevel() + 3;
    }

    LOG_INFO("playerbots", "Max player level is {}, max bot level set to {}", playersLevel - 3, playersLevel);
}

void RandomPlayerbotMgr::ScheduleRandomize(uint32 bot, uint32 time) { SetEventValue(bot, "randomize", 1, time); }

void RandomPlayerbotMgr::ScheduleTeleport(uint32 bot, uint32 time)
{
    if (!time)
        time = 60 + urand(sPlayerbotAIConfig.randomBotUpdateInterval, sPlayerbotAIConfig.randomBotUpdateInterval * 3);

    SetEventValue(bot, "teleport", 1, time);
}

void RandomPlayerbotMgr::ScheduleChangeStrategy(uint32 bot, uint32 time)
{
    if (!time)
        time = urand(sPlayerbotAIConfig.minRandomBotChangeStrategyTime,
                     sPlayerbotAIConfig.maxRandomBotChangeStrategyTime);

    SetEventValue(bot, "change_strategy", 1, time);
}

bool RandomPlayerbotMgr::ProcessBot(uint32 bot)
{
    ObjectGuid botGUID = ObjectGuid::Create<HighGuid::Player>(bot);
    Player* player = GetPlayerBot(botGUID);
    PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;

    uint32 isValid = GetEventValue(bot, "add");
    if (!isValid)
    {
        if (!player || (!player->GetGroup() && !WovBotReserved(bot)))  // wov: not while on its way to a dungeon run
        {
            if (player)
                LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: log out", bot, IsAlliance(player->getRace()) ? "A" : "H",
                          player->GetLevel(), player->GetName().c_str());
            else
                LOG_DEBUG("playerbots", "Bot #{}: log out", bot);

            SetEventValue(bot, "add", 0, 0);
            currentBots.erase(bot);

            if (player)
                LogoutPlayerBot(botGUID);
        }

        return false;
    }

    // wov: session over -> log out, a few per minute, never in a group or next to a real player
    int wovBandSide = 0;  // -1 short of its share, +1 well over it
    if (g_wovSchedEnable && g_wovBands && player)
    {
        static time_t bandT = 0;
        static uint32 bandOnline[7] = {0, 0, 0, 0, 0, 0, 0};
        if (time(nullptr) >= bandT + 30)
        {
            bandT = time(nullptr);
            WovBandCounts(currentBots, bandOnline);
        }
        uint8 b = WovBandOf(player->GetLevel());
        double want = double(g_wovBandPct[b]) * double(currentBots.size()) / 100.0;
        wovBandSide = bandOnline[b] < want * 0.9 ? -1 : (bandOnline[b] > want * 1.3 ? 1 : 0);
    }
    if (g_wovSchedEnable && player && player->IsInWorld() && !player->GetGroup() && !WovBotReserved(bot) &&
        currentBots.size() > g_wovMinOnline && wovBandSide >= 0 && (!WovScheduledOnline(bot) || wovBandSide > 0) &&
        !(botAI && botAI->HasPlayerNearby(40.0f)) && WovLogoutBudget())
    {
        LOG_DEBUG("playerbots", "WoV: {} logs off (session over)", player->GetName());
        SetEventValue(bot, "add", 0, 0);
        currentBots.erase(bot);
        LogoutPlayerBot(botGUID);
        return false;
    }

    uint32 randomTime;
    if (!player)
    {
        AddPlayerBot(botGUID, 0);
        randomTime = urand(1, 2);

        uint32 randomBotUpdateInterval = _isBotInitializing ? 1 : sPlayerbotAIConfig.randomBotUpdateInterval;
        randomTime = urand(std::max(5, static_cast<int>(randomBotUpdateInterval * 0.5)),
                           std::max(12, static_cast<int>(randomBotUpdateInterval * 2)));
        SetEventValue(bot, "update", 1, randomTime);

        // do not randomize or teleport immediately after server start (prevent lagging)
        if (!GetEventValue(bot, "randomize"))
        {
            randomTime = urand(3, std::max(4, static_cast<int>(randomBotUpdateInterval * 0.4)));
            ScheduleRandomize(bot, randomTime);
        }
        if (!GetEventValue(bot, "teleport"))
        {
            randomTime = urand(std::max(7, static_cast<int>(randomBotUpdateInterval * 0.7)),
                               std::max(14, static_cast<int>(randomBotUpdateInterval * 1.4)));
            ScheduleTeleport(bot, randomTime);
        }

        return true;
    }

    if (!player->IsInWorld())
        return false;

    if (player->GetGroup() || player->HasUnitState(UNIT_STATE_IN_FLIGHT))
        return false;

    uint32 update = GetEventValue(bot, "update");
    if (!update)
    {
        if (botAI)
            botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Set(true);

        bool update = true;
        if (botAI)
        {
            // botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Set(true);
            if (!sRandomPlayerbotMgr.IsRandomBot(player))
                update = false;

            if (player->GetGroup() && botAI->GetGroupLeader())
            {
                PlayerbotAI* groupLeaderBotAI = GET_PLAYERBOT_AI(botAI->GetGroupLeader());
                if (!groupLeaderBotAI || IsSelfBot(botAI->GetGroupLeader()))
                    update = false;
            }

            // if (botAI->HasPlayerNearby(sPlayerbotAIConfig.grindDistance))
            //     update = false;
        }

        if (update)
            ProcessBot(player);

        randomTime = urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
        SetEventValue(bot, "update", 1, randomTime);

        return true;
    }

    uint32 logout = GetEventValue(bot, "logout");
    if (player && !logout && !isValid && !WovBotReserved(bot))  // wov: not in the middle of a dungeon run or raid
    {
        LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: log out", bot, IsAlliance(player->getRace()) ? "A" : "H",
                  player->GetLevel(), player->GetName().c_str());
        LogoutPlayerBot(botGUID);
        currentBots.erase(bot);
        SetEventValue(bot, "logout", 1,
                      urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime));
        return true;
    }

    return false;
}

bool RandomPlayerbotMgr::ProcessBot(Player* bot)
{

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

     uint32 botId = bot->GetGUID().GetCounter();

    // wov: a bot on a planned activity (dungeon run) is the coordinator's: no revive-teleport, no refresh that drops
    // its group, no "leader is a random bot" leave
    if (WovBotReserved(botId))
        return false;

    // wov: fresh start (launch day, wov/fresh_start.py). With AiPlayerbot.WoV.FreshStart = N every random bot that has
    // not had launch N yet is provisioned once more at level 1 (death knights at the heroic start level) by the factory
    // (spells, skills, talents, gear, bags) and put in its race's starting area with its hearth there. Done once per
    // bot per N; 0 = off.
    {
        static uint32 freshId = 0;
        static time_t freshCfgT = 0;
        static std::unordered_set<uint32> freshOnly;  // test: only these bots (empty = all)
        if (time(nullptr) > freshCfgT + 60)
        {
            freshCfgT = time(nullptr);
            freshId = sConfigMgr->GetOption<uint32>("AiPlayerbot.WoV.FreshStart", 0);
            freshOnly.clear();
            std::string cur;
            for (char ch : sConfigMgr->GetOption<std::string>("AiPlayerbot.WoV.FreshStart.Only", "") + ",")
                if (std::isdigit(static_cast<unsigned char>(ch)))
                    cur += ch;
                else if (!cur.empty())
                {
                    freshOnly.insert(uint32(std::strtoul(cur.c_str(), nullptr, 10)));
                    cur.clear();
                }
        }
        if (freshId && (freshOnly.empty() || freshOnly.count(botId)) && bot->IsInWorld() && !bot->IsInCombat() && !bot->IsBeingTeleported() &&
            GetEventValue(botId, "wov_fresh") != freshId)
        {
            uint32 level = bot->getClass() == CLASS_DEATH_KNIGHT
                               ? std::max<uint32>(1, sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))
                               : 1;
            if (bot->GetGroup())
                botAI->LeaveOrDisbandGroup();
            SetValue(bot, "level", level);
            PlayerbotFactory factory(bot, level);
            factory.Randomize(false);
            MarkFactoryInitialized(bot);
            if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(bot->getRace(), bot->getClass()))
            {
                WorldLocation start(info->mapId, info->positionX, info->positionY, info->positionZ, info->orientation);
                bot->SetHomebind(start, info->areaId);
                bot->TeleportTo(start);
            }
            SetEventValue(botId, "wov_fresh", freshId, 0);
            LOG_INFO("playerbots", "WoV fresh start {}: {} is level {} in its starting area", freshId, bot->GetName(), level);
            return true;
        }
    }

    // if death revive
    if (bot->isDead())
    {
        if (!GetEventValue(botId, "dead"))
        {
            uint32 randomTime =
                urand(sPlayerbotAIConfig.minRandomBotReviveTime, sPlayerbotAIConfig.maxRandomBotReviveTime);
            LOG_DEBUG("playerbots", "Mark bot {} as dead, will be revived in {}s.", bot->GetName().c_str(),
                      randomTime);
            SetEventValue(botId, "dead", 1, sPlayerbotAIConfig.maxRandomBotInWorldTime);
            SetEventValue(botId, "revive", 1, randomTime);
            return false;
        }

        if (!GetEventValue(botId, "revive"))
        {
            Revive(bot);
            return true;
        }

        return false;
    }

    // leave group if leader is rndbot
    Group* group = bot->GetGroup();
    if (group && !group->isLFGGroup() && IsRandomBot(group->GetLeader()))
    {
        botAI->LeaveOrDisbandGroup();
        LOG_INFO("playerbots", "Bot {} remove from group since leader is random bot.", bot->GetName().c_str());
    }

    // only randomize and teleport idle bots
    bool idleBot = false;
    if (TravelTarget* target = botAI->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
    {
        if (target->getTravelState() == TravelState::TRAVEL_STATE_IDLE)
        {
            idleBot = true;
        }
    }
    else
    {
        idleBot = true;
    }

    if (idleBot)
    {
        // randomize
        uint32 randomize = GetEventValue(botId, "randomize");
        // A kept bot is provisioned once and never rerolled after that, but its timer keeps running: a bot
        // that stops being kept (the switch goes off, or it loses its anchor) then waits a normal interval
        // instead of every such bot rerolling on its next update.
        bool const preserveMeeting = sPlayerbotAIConfig.persistentProgression == PersistentProgressionMode::ANCHORED &&
            sPlayerbotAIConfig.persistentProgressionAnchorOnMeeting;
        if (!randomize && RandomBotLevelMgr::instance().IsProgressionKept(bot) &&
            (preserveMeeting || HasFactoryInitialization(bot)))
        {
            ScheduleRandomize(botId, urand(sPlayerbotAIConfig.minRandomBotRandomizeTime,
                                           sPlayerbotAIConfig.maxRandomBotRandomizeTime));
            randomize = 1;
        }

        if (!randomize)
        {
            // bool randomiser = true;
            // if (player->GetGuildId())
            // {
            //     if (Guild* guild = sGuildMgr->GetGuildById(player->GetGuildId()))
            //     {
            //         if (guild->GetLeaderGUID() == player->GetGUID())
            //         {
            //             for (std::vector<Player*>::iterator i = players.begin(); i != players.end(); ++i)
            //                 GuildTaskMgr::instance().Update(*i, player);
            //         }

            //         uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guild->GetLeaderGUID());
            //         if (!sPlayerbotAIConfig.IsInRandomAccountList(accountId))
            //         {
            //             uint8 rank = player->GetRank();
            //             randomiser = rank < 4 ? false : true;
            //         }
            //     }
            // }
            // if (randomiser)
            // {
            Randomize(bot);
            LOG_DEBUG("playerbots", "Bot #{} {}:{} <{}>: randomized", botId,
                      bot->GetTeamId() == TEAM_ALLIANCE ? "A" : "H", bot->GetLevel(), bot->GetName());
            uint32 randomTime =
                urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
            ScheduleRandomize(botId, randomTime);
            return true;
        }

        // uint32 changeStrategy = GetEventValue(bot, "change_strategy");
        // if (!changeStrategy)
        // {
        //     LOG_INFO("playerbots", "Changing strategy for bot  #{} <{}>", bot, player->GetName().c_str());
        //     ChangeStrategy(player);
        //     return true;
        // }

        uint32 teleport = GetEventValue(botId, "teleport");
        if (!teleport)
        {
            LOG_DEBUG("playerbots", "Bot #{} <{}>: teleport for level and refresh", botId, bot->GetName());
            Refresh(bot);
            RandomTeleportForLevel(bot);
            uint32 time = urand(sPlayerbotAIConfig.minRandomBotTeleportInterval,
                                sPlayerbotAIConfig.maxRandomBotTeleportInterval);
            ScheduleTeleport(botId, time);
            return true;
        }
    }

    return false;
}

void RandomPlayerbotMgr::Revive(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    // LOG_INFO("playerbots", "Bot {} revived", player->GetName().c_str());
    SetEventValue(bot, "dead", 0, 0);
    SetEventValue(bot, "revive", 0, 0);

    Refresh(player);
    RandomTeleportGrindForLevel(player);
}

bool WovBotReserved(uint32 low);  // fwd

void RandomPlayerbotMgr::RandomTeleport(Player* bot, std::vector<WorldLocation>& locs, bool hearth)
{
    // ignore when alrdy teleported or not in the world yet.
    if (bot->IsBeingTeleported() || !bot->IsInWorld())
        return;

    // no teleport / movement update when rooted.
    if (bot->IsRooted())
        return;

    // ignore when in queue for battle grounds.
    if (bot->InBattlegroundQueue())
        return;

    // ignore when in battle grounds or arena.
    if (bot->InBattleground() || bot->InArena())
        return;

    // ignore when in group (e.g. world, dungeons, raids) and leader is not a player.
    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetGUID()))
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (botAI)
    {
        // ignore when in when taxi with boat/zeppelin and has players nearby
        if (bot->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT) && bot->HasUnitState(UNIT_STATE_IGNORE_PATHFINDING) &&
            botAI->HasPlayerNearby())
            return;
        // wov: a player never vanishes in front of someone (level-up and periodic teleports); it walks or flies on
        if (!hearth && botAI->HasPlayerNearby(200.0f))
            return;
        // wov: nor while it is on its way to a dungeon run or in one
        if (WovBotReserved(bot->GetGUID().GetCounter()))
            return;
    }

    // if (sPlayerbotAIConfig.randomBotRpgChance < 0)
    //     return;

    if (locs.empty())
    {
        LOG_DEBUG("playerbots", "Cannot teleport bot {} - no locations available", bot->GetName().c_str());
        return;
    }

    std::vector<WorldPosition> tlocs;
    for (auto& loc : locs)
        tlocs.push_back(WorldPosition(loc));
    // Do not teleport to maps disabled in config
    tlocs.erase(std::remove_if(tlocs.begin(), tlocs.end(),
                               [](WorldPosition l)
                               {
                                   std::vector<uint32>::iterator i =
                                       find(sPlayerbotAIConfig.randomBotMaps.begin(),
                                            sPlayerbotAIConfig.randomBotMaps.end(), l.GetMapId());
                                   return i == sPlayerbotAIConfig.randomBotMaps.end();
                               }),
                tlocs.end());
    if (tlocs.empty())
    {
        LOG_DEBUG("playerbots", "Cannot teleport bot {} - all locations removed by filter", bot->GetName().c_str());
        return;
    }

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomTeleportByLocations");

    std::shuffle(std::begin(tlocs), std::end(tlocs), RandomEngine::Instance());
    for (uint32 i = 0; i < tlocs.size(); i++)
    {
        WorldLocation loc = tlocs[i];

        float x = loc.GetPositionX();  // + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) -
                                       // sPlayerbotAIConfig.grindDistance / 2 : 0);
        float y = loc.GetPositionY();  // + (attemtps > 0 ? urand(0, sPlayerbotAIConfig.grindDistance) -
                                       // sPlayerbotAIConfig.grindDistance / 2 : 0);
        float z = loc.GetPositionZ();

        Map* map = sMapMgr->FindMap(loc.GetMapId(), 0);
        if (!map)
            continue;

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(map->GetZoneId(bot->GetPhaseMask(), x, y, z));
        if (!zone)
            continue;

        AreaTableEntry const* area = sAreaTableStore.LookupEntry(map->GetAreaId(bot->GetPhaseMask(), x, y, z));
        if (!area)
            continue;

        // Do not teleport to enemy zones if level is low
        if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            continue;

        if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            continue;

        if (map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            continue;

        float ground = map->GetHeight(bot->GetPhaseMask(), x, y, z + 0.5f);
        if (ground <= INVALID_HEIGHT)
            continue;

        z = 0.05f + ground;

        if (!botAI->StarterLevelDistanceCheck(bot, loc, true))
            continue;

        LocaleConstant const& locale = sWorld->GetDefaultDbcLocale();
        LOG_DEBUG("playerbots",
                  "Random teleporting bot {} (level {}) to Map: {} ({}) Zone: {} ({}) Area: {} ({}) ZoneLevel: {} "
                  "AreaLevel: {} {},{},{} ({}/{} "
                  "locations)",
                  bot->GetName().c_str(), bot->GetLevel(), map->GetId(), map->GetMapName(), zone->ID,
                  zone->area_name[locale], area->ID, area->area_name[locale], zone->area_level, area->area_level, x, y,
                  z, i + 1, tlocs.size());

        if (hearth)
        {
            bot->SetHomebind(loc, zone->ID);
        }

        // Prevent blink to be detected by visible real players
        if (botAI->HasPlayerNearby(150.0f))
        {
            break;
        }

        bot->GetMotionMaster()->Clear();
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI)
            botAI->Reset(true);
        bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        bot->TeleportTo(loc.GetMapId(), x, y, z, 0);
        bot->SendMovementFlagUpdate();

        if (pmo)
            pmo->finish();

        return;
    }

    if (pmo)
        pmo->finish();

    // LOG_ERROR("playerbots", "Cannot teleport bot {} - no locations available ({} locations)", bot->GetName().c_str(),
    //           tlocs.size());
}

void RandomPlayerbotMgr::PrepareAddclassCache()
{
    // Using accounts marked as type 2 (AddClass)
    int32 collected = 0;

    for (uint32 accountId : addClassTypeAccounts)
    {
        for (uint8 claz = CLASS_WARRIOR; claz <= CLASS_DRUID; claz++)
        {
            if (claz == 10)
                continue;

            QueryResult results = CharacterDatabase.Query(
                "SELECT guid, race FROM characters "
                "WHERE account = {} AND class = '{}' AND online = 0",
                accountId, claz);

            if (results)
            {
                do
                {
                    Field* fields = results->Fetch();
                    ObjectGuid guid = ObjectGuid(HighGuid::Player, fields[0].Get<uint32>());
                    uint32 race = fields[1].Get<uint32>();
                    bool isAlliance = race == 1 || race == 3 || race == 4 || race == 7 || race == 11;
                    addclassCache[GetTeamClassIdx(isAlliance, claz)].insert(guid);
                    collected++;
                } while (results->NextRow());
            }
        }
    }

    LOG_INFO("playerbots", ">> {} characters collected for addclass command from {} AddClass accounts.", collected, addClassTypeAccounts.size());
}

void RandomPlayerbotMgr::Init()
{
    if (sPlayerbotAIConfig.addClassCommand)
        sRandomPlayerbotMgr.PrepareAddclassCache();

    if (sPlayerbotAIConfig.randomBotJoinBG)
        sRandomPlayerbotMgr.LoadBattleMastersCache();

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_EVENT);
    stmt->SetData(0, std::string("add"));
    PlayerbotsDatabase.Execute(stmt);
}

void RandomPlayerbotMgr::InitArenaTeams()
{
    if (sPlayerbotAIConfig.deleteRandomBotArenaTeams)
    {
        RandomPlayerbotFactory::DeleteBotArenaTeams();
        return;
    }

    RandomPlayerbotFactory::LoadArenaTeamData();

    LOG_INFO("playerbots", "Bot arena teams: 2v2={}/{}, 3v3={}/{}, 5v5={}/{}",
             RandomPlayerbotFactory::GetBotArenaTeamCount(ARENA_TYPE_2v2), sPlayerbotAIConfig.randomBotArenaTeam2v2Count,
             RandomPlayerbotFactory::GetBotArenaTeamCount(ARENA_TYPE_3v3), sPlayerbotAIConfig.randomBotArenaTeam3v3Count,
             RandomPlayerbotFactory::GetBotArenaTeamCount(ARENA_TYPE_5v5), sPlayerbotAIConfig.randomBotArenaTeam5v5Count);
}

void RandomPlayerbotMgr::RandomTeleportForLevel(Player* bot)
{
    if (bot->InBattleground())
        return;

    if (bot->GetLevel() >= 10 && urand(0, 100) < sPlayerbotAIConfig.probTeleToBankers * 100)
    {
        std::vector<WorldLocation> locs = sTravelMgr.GetCityLocations(bot);
        if (!locs.empty())
        {
            RandomTeleport(bot, locs, true);
            return;
        }
    }
    std::vector<WorldLocation> locs = sTravelMgr.GetTeleportLocations(bot);

    if (sPlayerbotAIConfig.randomBotConcentrateInPlayerZone && !locs.empty())
    {
        std::vector<WorldLocation> playerZoneLocs = GetPlayerZoneTeleportLocations(locs, bot);
        if (!playerZoneLocs.empty())
            locs = std::move(playerZoneLocs);
    }

    // local: company seats (custom wow plans/14, step C). A guild bot sometimes goes where its company keeps
    // a seat or fights for ground. The locations already fit its level; with none left, it travels as usual.
    if (sPlayerbotAIConfig.companySeatTeleportChance && bot->GetGuildId() && !locs.empty() &&
        urand(0, 99) < sPlayerbotAIConfig.companySeatTeleportChance)
    {
        std::vector<WorldLocation> companyLocs;
        for (WorldLocation const& loc : locs)
        {
            uint32 zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, loc);
            if (!CompanyStanding::instance().PullsToward(bot->GetGuildId(), zoneId))
                continue;

            // Same enemy-zone rule as RandomTeleport, so the set can't drain downstream.
            if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
            {
                if ((zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE) ||
                    (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE))
                    continue;
            }

            companyLocs.push_back(loc);
        }

        if (!companyLocs.empty())
            locs = std::move(companyLocs);
    }

    if (!locs.empty())
    {
        RandomTeleport(bot, locs, false);
        return;
    }
}

// Returns the subset of teleport locations that lie in a zone currently occupied by a real
// (non-GM) player, so random bots can be gathered where players actually are. Returns an empty
// vector when no player is online or none of the locations match, letting the caller fall back
// to the default world-wide behaviour.
std::vector<WorldLocation> RandomPlayerbotMgr::GetPlayerZoneTeleportLocations(std::vector<WorldLocation> const& locs,
                                                                              Player* bot)
{
    std::set<uint32> playerMaps;
    std::set<std::pair<uint32, uint32>> playerMapZones;

    // players only ever holds real (non random bot) players and is maintained on login/logout, so
    // this is a pass over the online player list, not a world-wide scan.
    for (Player* player : players)
    {
        if (!player || !player->IsInWorld() || player->IsGameMaster())
            continue;

        Map* map = player->GetMap();
        if (!map)
            continue;

        // Instanceable maps (dungeons, raids, battlegrounds, arenas) are never valid targets: a
        // WorldLocation carries no instance id, so a bot would be sent to another instance of the
        // same map rather than to the player.
        if (map->Instanceable())
            continue;

        // Resolve the player zone the same way as the candidate locations below (unphased terrain),
        // so a player standing in a phased area still matches its underlying zone.
        uint32 zoneId = map->GetZoneId(PHASEMASK_NORMAL, player->GetPositionX(), player->GetPositionY(),
                                       player->GetPositionZ());
        playerMaps.insert(map->GetId());
        playerMapZones.insert(std::make_pair(map->GetId(), zoneId));
    }

    std::vector<WorldLocation> filtered;
    if (playerMapZones.empty())
        return filtered;

    for (WorldLocation const& loc : locs)
    {
        if (playerMaps.find(loc.GetMapId()) == playerMaps.end())
            continue;

        uint32 zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, loc);
        if (playerMapZones.find(std::make_pair(loc.GetMapId(), zoneId)) == playerMapZones.end())
            continue;

        // Skip enemy-faction zones, matching the check in RandomTeleport. This has to be done here:
        // the caller only falls back to normal teleporting when the filtered set is empty, so keeping
        // a hostile location would let the downstream team check drain the set and strand the bot.
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
        {
            if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
                continue;

            if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
                continue;
        }

        filtered.push_back(loc);
    }

    return filtered;
}

void RandomPlayerbotMgr::RandomTeleportGrindForLevel(Player* bot)
{
    if (bot->InBattleground())
        return;

    std::vector<WorldLocation> locs = sTravelMgr.GetTeleportLocations(bot);
    LOG_DEBUG("playerbots", "Random teleporting bot {} for level {} ({} locations available)", bot->GetName().c_str(),
              bot->GetLevel(), locs.size());

    RandomTeleport(bot, locs);
}

void RandomPlayerbotMgr::RandomTeleport(Player* bot)
{
    if (bot->InBattleground())
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomTeleport");
    std::vector<WorldLocation> locs;

    std::list<Unit*> targets;
    float range = sPlayerbotAIConfig.randomBotTeleportDistance;
    Acore::AnyUnitInObjectRangeCheck u_check(bot, range);
    Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, targets, u_check);
    Cell::VisitObjects(bot, searcher, range);

    if (!targets.empty())
    {
        for (Unit* unit : targets)
        {
            bot->UpdatePosition(*unit);
            FleeManager manager(bot, sPlayerbotAIConfig.sightDistance, 0, true);
            float rx, ry, rz;
            if (manager.CalculateDestination(&rx, &ry, &rz))
            {
                WorldLocation loc(bot->GetMapId(), rx, ry, rz);
                locs.push_back(loc);
            }
        }
    }
    else
    {
        RandomTeleportForLevel(bot);
    }

    if (pmo)
        pmo->finish();

    Refresh(bot);
}

void RandomPlayerbotMgr::Randomize(Player* bot)
{
    if (bot->InBattleground())
        return;

    if (bot->GetLevel() < 3 || (bot->GetLevel() < 56 && bot->getClass() == CLASS_DEATH_KNIGHT))
    {
        RandomizeFirst(bot);
    }
    else if (bot->GetLevel() < sPlayerbotAIConfig.randomBotMaxLevel || !sPlayerbotAIConfig.downgradeMaxLevelBot)
    {
        uint8 level = bot->GetLevel();
        PlayerbotFactory factory(bot, level);
        factory.Randomize(true);
        MarkFactoryInitialized(bot);
        // IncreaseLevel(bot);
    }
    else
    {
        RandomizeFirst(bot);
    }
}

void RandomPlayerbotMgr::IncreaseLevel(Player* bot)
{
    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "IncreaseLevel");
    uint32 lastLevel = GetValue(bot, "level");
    uint8 level = bot->GetLevel() + 1;
    if (level > maxLevel)
    {
        level = maxLevel;
    }
    if (lastLevel != level)
    {
        PlayerbotFactory factory(bot, level);
        factory.Randomize(true);
    }

    if (pmo)
        pmo->finish();
}

void RandomPlayerbotMgr::RandomizeFirst(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    uint32 maxLevel = sPlayerbotAIConfig.randomBotMaxLevel;
    if (maxLevel > sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))
        maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    // if lvl sync is enabled, max level is limited by online players lvl
    if (sPlayerbotAIConfig.syncLevelWithPlayers)
        maxLevel = std::max(sPlayerbotAIConfig.randomBotMinLevel,
                            std::min(playersLevel, sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL)));

    uint32 minLevel = sPlayerbotAIConfig.randomBotMinLevel;
    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        maxLevel = std::max(maxLevel, sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
        minLevel = std::max(minLevel, sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
    }

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomizeFirst");

    uint32 level;

    if (sPlayerbotAIConfig.downgradeMaxLevelBot && bot->GetLevel() >= sPlayerbotAIConfig.randomBotMaxLevel)
    {
        if (bot->getClass() == CLASS_DEATH_KNIGHT)
        {
            level = sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL);
        }
        else
        {
            level = sPlayerbotAIConfig.randomBotMinLevel;
        }
    }
    else
    {
        uint32 roll = urand(1, 100);
        if (roll <= 100 * sPlayerbotAIConfig.randomBotMaxLevelChance)
        {
            level = maxLevel;
        }
        else if (roll <=
                 (100 * (sPlayerbotAIConfig.randomBotMaxLevelChance + sPlayerbotAIConfig.randomBotMinLevelChance)))
        {
            level = minLevel;
        }
        else
        {
            level = urand(minLevel, maxLevel);
        }
    }

    if (sPlayerbotAIConfig.disableRandomLevels)
    {
        level = bot->getClass() == CLASS_DEATH_KNIGHT ? std::max(sPlayerbotAIConfig.randombotStartingLevel,
                                                                 sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))
                                                      : sPlayerbotAIConfig.randombotStartingLevel;
    }

    SetValue(bot, "level", level);
    PlayerbotFactory factory(bot, level);
    factory.Randomize(false);
    MarkFactoryInitialized(bot);

    uint32 randomTime =
        urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    uint32 inworldTime =
        urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime);

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, randomTime);
    stmt->SetData(1, "bot_delete");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, inworldTime);
    stmt->SetData(1, "logout");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    // teleport to a random inn for bot level
    botAI->Reset(true);

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();

    RandomTeleportForLevel(bot);
}

void RandomPlayerbotMgr::RandomizeMin(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "RandomizeMin");
    uint32 level = sPlayerbotAIConfig.randomBotMinLevel;
    SetValue(bot, "level", level);
    PlayerbotFactory factory(bot, level);
    factory.Randomize(false);
    MarkFactoryInitialized(bot);

    uint32 randomTime =
        urand(sPlayerbotAIConfig.minRandomBotRandomizeTime, sPlayerbotAIConfig.maxRandomBotRandomizeTime);
    uint32 inworldTime =
        urand(sPlayerbotAIConfig.minRandomBotInWorldTime, sPlayerbotAIConfig.maxRandomBotInWorldTime);

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, randomTime);
    stmt->SetData(1, "bot_delete");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_RANDOM_BOTS);
    stmt->SetData(0, inworldTime);
    stmt->SetData(1, "logout");
    stmt->SetData(2, bot->GetGUID().GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    // teleport to a random inn for bot level
    botAI->Reset(true);

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();
}

void RandomPlayerbotMgr::Clear(Player* bot)
{
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.ClearEverything();
}

uint32 RandomPlayerbotMgr::GetZoneLevel(uint16 mapId, float teleX, float teleY, float /*teleZ*/)
{
    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);

    uint32 level = 0;
    QueryResult results = WorldDatabase.Query(
        "SELECT AVG(t.minlevel) minlevel, AVG(t.maxlevel) maxlevel FROM creature c "
        "INNER JOIN creature_template t ON c.id = t.entry WHERE map = {} AND minlevel > 1 AND ABS(position_x - {}) < "
        "{} AND ABS(position_y - {}) < {}",
        mapId, teleX, sPlayerbotAIConfig.randomBotTeleportDistance / 2, teleY,
        sPlayerbotAIConfig.randomBotTeleportDistance / 2);

    if (results)
    {
        Field* fields = results->Fetch();
        uint8 minLevel = fields[0].Get<uint8>();
        uint8 maxLevel = fields[1].Get<uint8>();
        level = urand(minLevel, maxLevel);
        if (level > maxLevel)
            level = maxLevel;
    }
    else
    {
        level = urand(1, maxLevel);
    }

    return level;
}

void RandomPlayerbotMgr::Refresh(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    if (bot->isDead())
    {
        bot->ResurrectPlayer(1.0f);
        bot->SpawnCorpseBones();
        botAI->ResetStrategies(false);
    }

    // if (sPlayerbotAIConfig.disableRandomLevels)
    //     return;

    if (bot->InBattleground())
        return;

    LOG_DEBUG("playerbots", "Refreshing bot {} <{}>", bot->GetGUID().ToString().c_str(), bot->GetName().c_str());

    PerfMonitorOperation* pmo = sPerfMonitor.start(PERF_MON_RNDBOT, "Refresh");

    botAI->Reset();

    bot->DurabilityRepairAll(false, 1.0f, false);
    bot->SetFullHealth();
    bot->SetPvP(sWorld->IsPvPRealm());
    // A kept bot is restocked but keeps its belongings.
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.Refresh(RandomBotLevelMgr::instance().IsProgressionKept(bot));

    if (bot->GetMaxPower(POWER_MANA) > 0)
        bot->SetPower(POWER_MANA, bot->GetMaxPower(POWER_MANA));

    if (bot->GetMaxPower(POWER_ENERGY) > 0)
        bot->SetPower(POWER_ENERGY, bot->GetMaxPower(POWER_ENERGY));

    uint32 money = bot->GetMoney();
    bot->SetMoney(money + 500 * sqrt(urand(1, bot->GetLevel() * 5)));

    if (bot->GetGroup())
        botAI->LeaveOrDisbandGroup();

    if (pmo)
        pmo->finish();
}

bool RandomPlayerbotMgr::IsRandomBot(Player* bot)
{
    if (bot && GET_PLAYERBOT_AI(bot))
    {
        if (IsSelfBot(bot))
            return false;
    }
    if (bot)
    {
        return IsRandomBot(bot->GetGUID().GetCounter());
    }

    return false;
}

bool RandomPlayerbotMgr::IsRandomBot(ObjectGuid::LowType bot)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(bot);
    if (!sPlayerbotAIConfig.IsInRandomAccountList(sCharacterCache->GetCharacterAccountIdByGuid(guid)))
        return false;

    return currentBots.contains(bot);
}

bool RandomPlayerbotMgr::IsAddclassBot(Player* bot)
{
    if (bot && GET_PLAYERBOT_AI(bot))
    {
        if (IsSelfBot(bot))
            return false;
    }
    if (bot)
    {
        return IsAddclassBot(bot->GetGUID().GetCounter());
    }

    return false;
}

bool RandomPlayerbotMgr::IsAddclassBot(ObjectGuid::LowType bot)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(bot);

    // Check the cache with faction considerations
    for (uint8 claz = CLASS_WARRIOR; claz <= CLASS_DRUID; claz++)
    {
        if (claz == 10)
            continue;

        for (uint8 isAlliance = 0; isAlliance <= 1; isAlliance++)
        {
            if (addclassCache[GetTeamClassIdx(isAlliance, claz)].find(guid) !=
                addclassCache[GetTeamClassIdx(isAlliance, claz)].end())
            {
                return true;
            }
        }
    }

    // If not in cache, check the account type
    uint32 accountId = sCharacterCache->GetCharacterAccountIdByGuid(guid);
    if (accountId && IsAccountType(accountId, 2)) // Type 2 = AddClass
    {
        return true;
    }

    return false;
}

void RandomPlayerbotMgr::GetBots()
{
    if (!currentBots.empty())
        return;

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_OWNER_AND_EVENT);
    stmt->SetData(0, 0);
    stmt->SetData(1, "add");
    uint32 maxAllowedBotCount = GetEventValue(0, "bot_count");
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 bot = fields[0].Get<uint32>();
            if (GetEventValue(bot, "add"))
                currentBots.insert(bot);

            if (currentBots.size() >= maxAllowedBotCount)
                break;
        } while (result->NextRow());
    }
}

std::vector<uint32> RandomPlayerbotMgr::GetBgBots(uint32 bracket)
{
    // if (!currentBgBots.empty()) return currentBgBots;

    std::vector<uint32> BgBots;

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_EVENT_AND_VALUE);
    stmt->SetData(0, "bg");
    stmt->SetData(1, bracket);
    if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 bot = fields[0].Get<uint32>();
            BgBots.push_back(bot);
        } while (result->NextRow());
    }

    return BgBots;
}

bool RandomPlayerbotMgr::HasFactoryInitialization(Player* bot)
{
    uint32 botId = bot->GetGUID().GetCounter();
    if (GetEventValue(botId, "factory_initialized"))
        return true;

    // Older realms recorded only a timed factory level. Read that historical
    // evidence without expiring it, then migrate once to a permanent marker.
    if (CachedEvent* level = FindEvent(botId, "level", true); level && level->value)
    {
        MarkFactoryInitialized(bot);
        return true;
    }
    return false;
}

void RandomPlayerbotMgr::PreserveCurrentState(uint32 botId)
{
    // Load the event cache before a write, so unrelated events are not hidden by
    // SetEventValue's create-on-write behavior. This must run on the world thread.
    if (!GetEventValue(botId, "factory_initialized"))
        SetEventValue(botId, "factory_initialized", 1, 0);
}

void RandomPlayerbotMgr::MarkFactoryInitialized(Player* bot)
{
    if (sPlayerbotAIConfig.persistentProgression != PersistentProgressionMode::OFF)
        SetEventValue(bot->GetGUID().GetCounter(), "factory_initialized", 1, 0);
}

CachedEvent* RandomPlayerbotMgr::FindEvent(uint32 bot, std::string const& event, bool allowExpired)
{
    BotEventCache& cache = eventCache[bot];

    // Load once
    if (!cache.loaded)
    {
        cache.events.clear();

        PlayerbotsDatabasePreparedStatement* stmt =
            PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_RANDOM_BOTS_BY_OWNER_AND_BOT);
        stmt->SetData(0, 0);
        stmt->SetData(1, bot);

        if (PreparedQueryResult result = PlayerbotsDatabase.Query(stmt))
        {
            do
            {
                Field* fields = result->Fetch();

                CachedEvent e;
                e.value = fields[1].Get<uint32>();
                e.lastChangeTime = fields[2].Get<uint32>();
                e.validIn = fields[3].Get<uint32>();
                e.data = fields[4].Get<std::string>();

                cache.events.emplace(fields[0].Get<std::string>(), std::move(e));
            } while (result->NextRow());
        }

        cache.loaded = true;
    }

    auto it = cache.events.find(event);
    if (it == cache.events.end())
        return nullptr;

    CachedEvent& e = it->second;

    // remove expired events
    if (!allowExpired && e.validIn && (NowSeconds() - e.lastChangeTime) >= e.validIn && event != "specNo" &&
        event != "specLink")
    {
        cache.events.erase(it);
        return nullptr;
    }

    return &e;
}

bool RandomPlayerbotMgr::IsSpecPvp(uint32 bot, uint8 cls)
{
    uint32 stored = GetValue(bot, "specNo");
    if (!stored)
        return false;
    uint32 specIndex = stored - 1;
    std::string const& name = sPlayerbotAIConfig.premadeSpecName[cls][specIndex];
    return !name.empty() && name.find("pvp") != std::string::npos;
}

uint32 RandomPlayerbotMgr::GetEventValue(uint32 bot, std::string const& event)
{
    if (CachedEvent* e = FindEvent(bot, event))
        return e->value;

    return 0;
}

std::string RandomPlayerbotMgr::GetEventData(uint32 bot, std::string const& event)
{
    if (CachedEvent* e = FindEvent(bot, event))
        return e->data;

    return "";
}

uint32 RandomPlayerbotMgr::SetEventValue(uint32 bot, std::string const& event, uint32 value, uint32 validIn,
                                         std::string const& data)
{
    PlayerbotsDatabaseTransaction trans = PlayerbotsDatabase.BeginTransaction();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_OWNER_AND_EVENT);
    stmt->SetData(0, 0);
    stmt->SetData(1, bot);
    stmt->SetData(2, event.c_str());
    trans->Append(stmt);

    if (value)
    {
        stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_RANDOM_BOTS);
        stmt->SetData(0, 0);
        stmt->SetData(1, bot);
        stmt->SetData(2, NowSeconds());
        stmt->SetData(3, validIn);
        stmt->SetData(4, event.c_str());
        stmt->SetData(5, value);

        if (!data.empty())
            stmt->SetData(6, data.c_str());
        else
            stmt->SetData(6);  // NULL

        trans->Append(stmt);
    }

    PlayerbotsDatabase.CommitTransaction(trans);

    // Update in-memory cache
    BotEventCache& cache = eventCache[bot];
    cache.loaded = true;

    if (!value)
    {
        cache.events.erase(event);
        return 0;
    }

    CachedEvent& e = cache.events[event];  // create-on-write is OK here
    e.value = value;
    e.lastChangeTime = NowSeconds();
    e.validIn = validIn;
    e.data = data;

    return value;
}

uint32 RandomPlayerbotMgr::GetValue(uint32 bot, std::string const& type) { return GetEventValue(bot, type); }

uint32 RandomPlayerbotMgr::GetValue(Player* bot, std::string const& type)
{
    return GetValue(bot->GetGUID().GetCounter(), type);
}

std::string RandomPlayerbotMgr::GetData(uint32 bot, std::string const& type) { return GetEventData(bot, type); }

void RandomPlayerbotMgr::SetValue(uint32 bot, std::string const& type, uint32 value, std::string const& data)
{
    SetEventValue(bot, type, value, sPlayerbotAIConfig.maxRandomBotInWorldTime, data);
}

void RandomPlayerbotMgr::SetValue(Player* bot, std::string const& type, uint32 value, std::string const& data)
{
    SetValue(bot->GetGUID().GetCounter(), type, value, data);
}

bool RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(ChatHandler* /*handler*/, char const* args)
{
    if (!sPlayerbotAIConfig.enabled)
    {
        LOG_ERROR("playerbots", "Playerbots system is currently disabled!");
        return false;
    }

    if (!args || !*args)
    {
        LOG_ERROR("playerbots", "Usage: rndbot stats/update/reset/init/refresh/add/remove");
        return false;
    }

    std::string const cmd = args;

    if (cmd == "reset")
    {
        PlayerbotsDatabase.Execute(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS));
        sRandomPlayerbotMgr.eventCache.clear();
        LOG_INFO("playerbots", "Random bots were reset for all players. Please restart the Server.");
        return true;
    }

    if (cmd == "stats")
    {
        sRandomPlayerbotMgr.PrintStats();
        // activatePrintStatsThread();
        return true;
    }

    if (cmd == "reload")
    {
        sPlayerbotAIConfig.Initialize();
        return true;
    }

    if (cmd == "update")
    {
        sRandomPlayerbotMgr.UpdateAIInternal(0);
        return true;
    }

    std::map<std::string, ConsoleCommandHandler> handlers;
    // handlers["initmin"] = &RandomPlayerbotMgr::RandomizeMin;
    handlers["init"] = &RandomPlayerbotMgr::RandomizeFirst;
    handlers["clear"] = &RandomPlayerbotMgr::Clear;
    handlers["levelup"] = handlers["level"] = &RandomPlayerbotMgr::IncreaseLevel;
    handlers["refresh"] = &RandomPlayerbotMgr::Refresh;
    handlers["teleport"] = &RandomPlayerbotMgr::RandomTeleportForLevel;
    // handlers["rpg"] = &RandomPlayerbotMgr::RandomTeleportForRpg;
    handlers["revive"] = &RandomPlayerbotMgr::Revive;
    handlers["grind"] = &RandomPlayerbotMgr::RandomTeleport;
    handlers["change_strategy"] = &RandomPlayerbotMgr::ChangeStrategy;

    for (std::map<std::string, ConsoleCommandHandler>::iterator j = handlers.begin(); j != handlers.end(); ++j)
    {
        std::string const prefix = j->first;
        if (cmd.find(prefix) != 0)
            continue;

        std::string const name = cmd.size() > prefix.size() + 1 ? cmd.substr(1 + prefix.size()) : "%";

        std::vector<uint32> botIds;
        for (std::vector<uint32>::iterator i = sPlayerbotAIConfig.randomBotAccounts.begin();
             i != sPlayerbotAIConfig.randomBotAccounts.end(); ++i)
        {
            uint32 account = *i;
            if (QueryResult results = CharacterDatabase.Query(
                    "SELECT guid FROM characters WHERE account = {} AND name like '{}'", account, name.c_str()))
            {
                do
                {
                    Field* fields = results->Fetch();

                    uint32 botId = fields[0].Get<uint32>();
                    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(botId);
                    if (!sRandomPlayerbotMgr.IsRandomBot(guid.GetCounter()))
                    {
                        continue;
                    }
                    Player* bot = ObjectAccessor::FindPlayer(guid);
                    if (!bot)
                        continue;

                    botIds.push_back(botId);
                } while (results->NextRow());
            }
        }

        if (botIds.empty())
        {
            LOG_INFO("playerbots", "Nothing to do");
            return false;
        }

        uint32 processed = 0;
        for (std::vector<uint32>::iterator i = botIds.begin(); i != botIds.end(); ++i)
        {
            ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(*i);
            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot)
                continue;

            LOG_INFO("playerbots", "[{}/{}] Processing command {} for bot {}", processed++, botIds.size(), cmd.c_str(),
                     bot->GetName().c_str());

            ConsoleCommandHandler handler = j->second;
            (sRandomPlayerbotMgr.*handler)(bot);
        }

        return true;
    }

    // std::vector<std::string> messages = sRandomPlayerbotMgr.HandlePlayerbotCommand(args);
    // for (std::vector<std::string>::iterator i = messages.begin(); i != messages.end(); ++i)
    // {
    //     LOG_INFO("playerbots", "{}", i->c_str());
    // }
    return true;
}

void RandomPlayerbotMgr::HandleCommand(uint32 type, std::string const text, Player* fromPlayer, std::string channelName)
{
    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (!bot)
            continue;

        if (!channelName.empty())
        {
            if (ChannelMgr* cMgr = ChannelMgr::forTeam(bot->GetTeamId()))
            {
                Channel* chn = cMgr->GetChannel(channelName, bot);
                if (!chn)
                    continue;
            }
        }

        if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
            botAI->HandleCommand(type, text, fromPlayer);
    }
}

void RandomPlayerbotMgr::OnPlayerLogout(Player* player)
{
    DisablePlayerBot(player->GetGUID());

    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (botAI && player == botAI->GetMaster())
        {
            botAI->SetMaster(nullptr);
            if (!bot->InBattleground())
            {
                botAI->ResetStrategies();
            }
        }
    }

    std::vector<Player*>::iterator i = std::find(players.begin(), players.end(), player);
    if (i != players.end())
        players.erase(i);
}

void RandomPlayerbotMgr::OnBotLoginInternal(Player* const bot)
{
    if (_isBotLogging)
    {
        LOG_INFO("playerbots", "{}/{} Bot {} logged in", playerBots.size(),
                 sRandomPlayerbotMgr.GetMaxAllowedBotCount(), bot->GetName().c_str());

        if (playerBots.size() == sRandomPlayerbotMgr.GetMaxAllowedBotCount())
        {
            _isBotLogging = false;
        }
    }

    // Run guild recovery/assignment at login to handle empty guild tables after restart.
    if (sPlayerbotAIConfig.randomBotGuildCount > 0)
    {
        PlayerbotFactory factory(bot, bot->GetLevel());
        factory.InitGuild();
    }

    RandomPlayerbotFactory::AssignBotToArenaTeam(bot);

    if (sPlayerbotAIConfig.randomBotFixedLevel)
    {
        bot->SetPlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    }
    else
    {
        bot->RemovePlayerFlag(PLAYER_FLAGS_NO_XP_GAIN);
    }
}

void RandomPlayerbotMgr::OnPlayerLogin(Player* player)
{
    uint32 botsNearby = 0;

    for (PlayerBotMap::const_iterator it = GetPlayerBotsBegin(); it != GetPlayerBotsEnd(); ++it)
    {
        Player* const bot = it->second;
        if (player == bot /* || GET_PLAYERBOT_AI(player)*/)  // TEST
            continue;

        Cell playerCell(player->GetPositionX(), player->GetPositionY());
        Cell botCell(bot->GetPositionX(), bot->GetPositionY());

        // if (playerCell == botCell)
        // botsNearby++;

        Group* group = bot->GetGroup();
        if (!group)
            continue;

        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (botAI && member == player && (!botAI->GetMaster() || GET_PLAYERBOT_AI(botAI->GetMaster())))
            {
                if (!bot->InBattleground())
                {
                    botAI->SetMaster(player);
                    botAI->ResetStrategies();
                    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                        "hello", "Hello", {}));
                }

                break;
            }
        }
    }

    if (botsNearby > 100 && false)
    {
        WorldPosition botPos(player);

        // botPos.GetReachableRandomPointOnGround(player, sPlayerbotAIConfig.reactDistance * 2, true);

        // player->TeleportTo(botPos);
        // player->Relocate(botPos.coord_x, botPos.coord_y, botPos.coord_z, botPos.orientation);

        if (!player->GetFactionTemplateEntry())
        {
            botPos.GetReachableRandomPointOnGround(player, sPlayerbotAIConfig.reactDistance * 2, true);
        }
        else
        {
            std::vector<TravelDestination*> dests = TravelMgr::instance().getRpgTravelDestinations(player, true, true, 200000.0f);

            do
            {
                RpgTravelDestination* dest = (RpgTravelDestination*)dests[urand(0, dests.size() - 1)];
                CreatureTemplate const* cInfo = dest->GetCreatureTemplate();
                if (!cInfo)
                    continue;

                FactionTemplateEntry const* factionEntry = sFactionTemplateStore.LookupEntry(cInfo->faction);
                ReputationRank reaction = Unit::GetFactionReactionTo(player->GetFactionTemplateEntry(), factionEntry);

                if (reaction > REP_NEUTRAL && dest->nearestPoint(&botPos)->GetMapId() == player->GetMapId())
                {
                    botPos = *dest->nearestPoint(&botPos);
                    break;
                }
            } while (true);
        }

        player->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
        player->TeleportTo(botPos);

        // player->Relocate(botPos.getX(), botPos.getY(), botPos.getZ(), botPos.getO());
    }

    if (IsRandomBot(player))
    {
        // ObjectGuid::LowType guid = player->GetGUID().GetCounter(); //not used, conditional could be rewritten for
        // simplicity. line marked for removal.
        player->SetPvP(sWorld->IsPvPRealm());
    }
    else
    {
        players.push_back(player);
        LOG_DEBUG("playerbots", "Including non-random bot player {} into random bot update", player->GetName().c_str());
    }
}

void RandomPlayerbotMgr::OnPlayerLoginError(uint32 bot)
{
    SetEventValue(bot, "add", 0, 0);
    currentBots.erase(bot);
}

Player* RandomPlayerbotMgr::GetRandomPlayer()
{
    if (players.empty())
        return nullptr;

    uint32 index = urand(0, players.size() - 1);
    return players[index];
}

void RandomPlayerbotMgr::PrintStats()
{
    printStatsTimer = time(nullptr);
    LOG_INFO("playerbots", "Random Bots Stats: {} online", playerBots.size());

    std::map<uint8, uint32> alliance, horde;
    for (uint32 i = 0; i < 10; ++i)
    {
        alliance[i] = 0;
        horde[i] = 0;
    }

    std::map<uint8, uint32> perRace;
    std::map<uint8, uint32> perClass;

    std::map<uint8, uint32> lvlPerRace;
    std::map<uint8, uint32> lvlPerClass;
    for (uint8 race = RACE_HUMAN; race < sRaceMgr->GetMaxRaces(); ++race)
    {
        perRace[race] = 0;
        lvlPerRace[race] = 0;
    }

    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        perClass[cls] = 0;
        lvlPerClass[cls] = 0;
    }

    uint32 dps = 0;
    uint32 heal = 0;
    uint32 tank = 0;
    uint32 active = 0;
/*    uint32 update = 0;
    uint32 randomize = 0;
    uint32 teleport = 0;
    uint32 changeStrategy = 0;*/
    uint32 dead = 0;
    uint32 combat = 0;
    // uint32 revive = 0; //not used, line marked for removal.
    uint32 inFlight = 0;
    uint32 moving = 0;
    uint32 mounted = 0;
    uint32 inBg = 0;
    uint32 rest = 0;
    uint32 engine_noncombat = 0;
    uint32 engine_combat = 0;
    uint32 engine_dead = 0;
    std::unordered_map<NewRpgStatus, int> rpgStatusCount;
    // static NewRpgStatistic rpgStasticTotal;
    std::unordered_map<uint32, int> zoneCount;
    uint8 maxBotLevel = 0;
    for (PlayerBotMap::iterator i = playerBots.begin(); i != playerBots.end(); ++i)
    {
        Player* bot = i->second;
        if (IsAlliance(bot->getRace()))
            ++alliance[bot->GetLevel()];
        else
            ++horde[bot->GetLevel()];
        maxBotLevel = std::max(maxBotLevel, bot->GetLevel());

        ++perRace[bot->getRace()];
        ++perClass[bot->getClass()];

        lvlPerClass[bot->getClass()] += bot->GetLevel();
        lvlPerRace[bot->getRace()] += bot->GetLevel();

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
        {
            LOG_ERROR("playerbots", "Player/Bot {} is registered in sRandomPlayerbotMgr playerBots and has no bot AI!", bot->GetName().c_str());
            continue;
        }

        if (botAI->IsActivityAllowedCached())
            ++active;
        /* TODO: Review statistics on rpg merge
        if (botAI->GetAiObjectContext()->GetValue<bool>("random bot update")->Get())
            ++update;

        uint32 botId = bot->GetGUID().GetCounter();
        if (!GetEventValue(botId, "randomize"))
            ++randomize;

        if (!GetEventValue(botId, "teleport"))
            ++teleport;

        if (!GetEventValue(botId, "change_strategy"))
            ++changeStrategy;
        */
        if (bot->isDead())
        {
            ++dead;
            // if (!GetEventValue(botId, "dead"))
            //++revive;
        }
        if (bot->IsInCombat())
            ++combat;

        if (bot->isMoving())
            ++moving;

        if (bot->IsInFlight())
            ++inFlight;

        if (bot->IsMounted())
            ++mounted;

        if (bot->InBattleground() || bot->InArena())
            ++inBg;

        if (bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING))
            ++rest;

        if (botAI->GetState() == BOT_STATE_NON_COMBAT)
            ++engine_noncombat;

        else if (botAI->GetState() == BOT_STATE_COMBAT)
            ++engine_combat;

        else
            ++engine_dead;

        if (botAI->IsHeal(bot, false))
            ++heal;

        else if (botAI->IsTank(bot, false))
            ++tank;

        else
            ++dps;

        zoneCount[bot->GetZoneId()]++;

        if (sPlayerbotAIConfig.enableNewRpgStrategy)
        {
            rpgStatusCount[botAI->rpgInfo.GetStatus()]++;
            rpgStasticTotal += botAI->rpgStatistic;
            botAI->rpgStatistic = NewRpgStatistic();
        }
    }

    LOG_INFO("playerbots", "Bots level:");
    // uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    uint32_t currentAlliance = 0, currentHorde = 0;
    uint32_t step = std::max(1, static_cast<int>((maxBotLevel + 4) / 8));
    uint32_t from = 1;

    for (uint8 i = 1; i <= maxBotLevel; ++i)
    {
        currentAlliance += alliance[i];
        currentHorde += horde[i];

        if (((i + 1) % step == 0) || i == maxBotLevel)
        {
            if (currentAlliance || currentHorde)
                LOG_INFO("playerbots", "    {}..{}: {} alliance, {} horde", from, i, currentAlliance, currentHorde);
            currentAlliance = 0;
            currentHorde = 0;
            from = i + 1;
        }
    }

    LOG_INFO("playerbots", "Bots race:");
    for (uint8 race = RACE_HUMAN; race < sRaceMgr->GetMaxRaces(); ++race)
    {
        if (perRace[race])
        {
            uint32 lvl = lvlPerRace[race] * 10 / perRace[race];
            float flvl = lvl / 10.0f;
            LOG_INFO("playerbots", "    {}: {}, avg lvl: {}", ChatHelper::FormatRace(race).c_str(), perRace[race],
                     flvl);
        }
    }

    LOG_INFO("playerbots", "Bots class:");
    for (uint8 cls = CLASS_WARRIOR; cls < MAX_CLASSES; ++cls)
    {
        if (perClass[cls])
        {
            uint32 lvl = lvlPerClass[cls] * 10 / perClass[cls];
            float flvl = lvl / 10.0f;
            LOG_INFO("playerbots", "    {}: {}, avg lvl: {}", ChatHelper::FormatClass(cls).c_str(), perClass[cls],
                     flvl);
        }
    }

    LOG_INFO("playerbots", "Bots role:");
    LOG_INFO("playerbots", "    tank: {}, heal: {}, dps: {}", tank, heal, dps);

    LOG_INFO("playerbots", "Bots status:");
    LOG_INFO("playerbots", "    Active: {}", active);
    LOG_INFO("playerbots", "    Moving: {}", moving);

    // LOG_INFO("playerbots", "Bots to:");
    // LOG_INFO("playerbots", "    update: {}", update);
    // LOG_INFO("playerbots", "    randomize: {}", randomize);
    // LOG_INFO("playerbots", "    teleport: {}", teleport);
    // LOG_INFO("playerbots", "    change_strategy: {}", changeStrategy);
    // LOG_INFO("playerbots", "    revive: {}", revive);

    LOG_INFO("playerbots", "    In flight: {}", inFlight);
    LOG_INFO("playerbots", "    On mount: {}", mounted);
    LOG_INFO("playerbots", "    In combat: {}", combat);
    LOG_INFO("playerbots", "    In BG: {}", inBg);
    LOG_INFO("playerbots", "    In Rest: {}", rest);
    LOG_INFO("playerbots", "    Dead: {}", dead);

    if (sPlayerbotAIConfig.enableNewRpgStrategy)
    {
        LOG_INFO("playerbots", "Bots rpg status:");
        LOG_INFO("playerbots",
                 "    Idle: {}, Rest: {}, GoGrind: {}, GoCamp: {}, MoveRandom: {}, MoveNpc: {}, DoQuest: {}, "
                 "TravelFlight: {}, OutdoorPvP: {}",
                 rpgStatusCount[RPG_IDLE], rpgStatusCount[RPG_REST], rpgStatusCount[RPG_GO_GRIND],
                 rpgStatusCount[RPG_GO_CAMP], rpgStatusCount[RPG_WANDER_RANDOM], rpgStatusCount[RPG_WANDER_NPC],
                 rpgStatusCount[RPG_DO_QUEST], rpgStatusCount[RPG_TRAVEL_FLIGHT], rpgStatusCount[RPG_OUTDOOR_PVP]);

        LOG_INFO("playerbots", "Bots total quests:");
        LOG_INFO("playerbots", "    Accepted: {}, Rewarded: {}, Dropped: {}", rpgStasticTotal.questAccepted,
                 rpgStasticTotal.questRewarded, rpgStasticTotal.questDropped);
    }

    LOG_INFO("playerbots", "Bots engine:", dead);
    LOG_INFO("playerbots", "    Non-combat: {}, Combat: {}, Dead: {}", engine_noncombat, engine_combat, engine_dead);
}

double RandomPlayerbotMgr::GetBuyMultiplier(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    uint32 value = GetEventValue(id, "buymultiplier");
    if (!value)
    {
        value = urand(50, 120);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval,
                               sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "buymultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

double RandomPlayerbotMgr::GetSellMultiplier(Player* bot)
{
    uint32 id = bot->GetGUID().GetCounter();
    uint32 value = GetEventValue(id, "sellmultiplier");
    if (!value)
    {
        value = urand(80, 250);
        uint32 validIn = urand(sPlayerbotAIConfig.minRandomBotsPriceChangeInterval,
                               sPlayerbotAIConfig.maxRandomBotsPriceChangeInterval);
        SetEventValue(id, "sellmultiplier", value, validIn);
    }

    return (double)value / 100.0;
}

void RandomPlayerbotMgr::AddTradeDiscount(Player* bot, Player* master, int32 value)
{
    if (!master)
        return;

    uint32 discount = GetTradeDiscount(bot, master);
    int32 result = (int32)discount + value;
    discount = (result < 0 ? 0 : result);

    SetTradeDiscount(bot, master, discount);
}

void RandomPlayerbotMgr::SetTradeDiscount(Player* bot, Player* master, uint32 value)
{
    if (!master)
        return;

    uint32 botId = bot->GetGUID().GetCounter();
    uint32 masterId = master->GetGUID().GetCounter();

    std::ostringstream name;
    name << "trade_discount_" << masterId;
    SetEventValue(botId, name.str(), value, sPlayerbotAIConfig.maxRandomBotInWorldTime);
}

uint32 RandomPlayerbotMgr::GetTradeDiscount(Player* bot, Player* master)
{
    if (!master)
        return 0;

    uint32 botId = bot->GetGUID().GetCounter();
    uint32 masterId = master->GetGUID().GetCounter();

    std::ostringstream name;
    name << "trade_discount_" << masterId;
    return GetEventValue(botId, name.str());
}

std::string const RandomPlayerbotMgr::HandleRemoteCommand(std::string const request)
{
    std::string::const_iterator pos = std::find(request.begin(), request.end(), ',');
    if (pos == request.end())
    {
        std::ostringstream out;
        out << "invalid request: " << request;
        return out.str();
    }

    std::string const command = std::string(request.begin(), pos);
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(atoi(std::string(pos + 1, request.end()).c_str()));
    Player* bot = GetPlayerBot(guid);
    if (!bot)
        return "invalid guid";

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return "invalid guid";

    return botAI->HandleRemoteCommand(command);
}

void RandomPlayerbotMgr::ChangeStrategy(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    if (frand(0.f, 100.f) > sPlayerbotAIConfig.randomBotRpgChance)
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to grind spot", bot, player->GetName().c_str());
        ScheduleTeleport(bot, 30);
    }
    else
    {
        LOG_INFO("playerbots", "Changing strategy for bot #{} <{}> to RPG", bot, player->GetName().c_str());
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to inn", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
        SetEventValue(bot, "teleport", 1, sPlayerbotAIConfig.maxRandomBotInWorldTime);
    }

    ScheduleChangeStrategy(bot);
}

void RandomPlayerbotMgr::ChangeStrategyOnce(Player* player)
{
    uint32 bot = player->GetGUID().GetCounter();

    if (frand(0.f, 100.f) > sPlayerbotAIConfig.randomBotRpgChance)  // select grind / pvp
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to grind spot", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
        Refresh(player);
    }
    else
    {
        LOG_INFO("playerbots", "Bot #{} <{}>: sent to inn", bot, player->GetName().c_str());
        RandomTeleportForLevel(player);
    }
}

void RandomPlayerbotMgr::RandomTeleportForRpg(Player* bot)
{
    uint32 race = bot->getRace();
    uint32 level = bot->GetLevel();
    LOG_DEBUG("playerbots", "Random teleporting bot {} for RPG ({} locations available)", bot->GetName().c_str(),
              rpgLocsCacheLevel[race].size());
    RandomTeleport(bot, rpgLocsCacheLevel[race][level], true);
}

void RandomPlayerbotMgr::Remove(Player* bot)
{
    ObjectGuid owner = bot->GetGUID();

    PlayerbotsDatabasePreparedStatement* stmt =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_DEL_RANDOM_BOTS_BY_OWNER);
    stmt->SetData(0, 0);
    stmt->SetData(1, owner.GetCounter());
    PlayerbotsDatabase.Execute(stmt);

    uint32 botId = owner.GetCounter();
    eventCache.erase(botId);

    LogoutPlayerBot(owner);
}

CreatureData const* RandomPlayerbotMgr::GetCreatureDataByEntry(uint32 entry)
{
    if (entry != 0)
    {
        for (auto const& itr : sObjectMgr->GetAllCreatureData())
            if (itr.second.id == entry)
                return &itr.second;
    }

    return nullptr;
}

ObjectGuid RandomPlayerbotMgr::GetBattleMasterGUID(Player* bot, BattlegroundTypeId bgTypeId)
{
    ObjectGuid battleMasterGUID = ObjectGuid::Empty;

    TeamId team = bot->GetTeamId();
    std::vector<uint32> Bms;

    for (auto i = std::begin(BattleMastersCache[team][bgTypeId]); i != std::end(BattleMastersCache[team][bgTypeId]);
         ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    for (auto i = std::begin(BattleMastersCache[TEAM_NEUTRAL][bgTypeId]);
         i != std::end(BattleMastersCache[TEAM_NEUTRAL][bgTypeId]); ++i)
    {
        Bms.insert(Bms.end(), *i);
    }

    if (Bms.empty())
        return battleMasterGUID;

    float dist1 = FLT_MAX;

    for (auto i = begin(Bms); i != end(Bms); ++i)
    {
        CreatureData const* data = sRandomPlayerbotMgr.GetCreatureDataByEntry(*i);
        if (!data)
            continue;

        Unit* Bm = PlayerbotAI::GetUnit(data);
        if (!Bm)
            continue;

        if (bot->GetMapId() != Bm->GetMapId())
            continue;

        // return first available guid on map if queue from anywhere
        if (!BattlegroundMgr::IsArenaType(bgTypeId))
        {
            battleMasterGUID = Bm->GetGUID();
            break;
        }

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(Bm->GetZoneId());
        if (!zone)
            continue;

        if (zone->team == 4 && bot->GetTeamId() == TEAM_ALLIANCE)
            continue;

        if (zone->team == 2 && bot->GetTeamId() == TEAM_HORDE)
            continue;

        if (Bm->getDeathState() == DeathState::Dead)
            continue;

        float dist2 = ServerFacade::instance().GetDistance2d(bot, data->posX, data->posY);
        if (dist2 < dist1)
        {
            dist1 = dist2;
            battleMasterGUID = Bm->GetGUID();
        }
    }

    return battleMasterGUID;
}
