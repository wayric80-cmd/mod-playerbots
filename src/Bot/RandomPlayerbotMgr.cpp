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

bool WovScheduledOnline(uint32 guid)
{
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
        if (g_wovBotSk.count(bot->GetGUID().GetCounter()) || bot->IsBeingTeleported())
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
        if (!h.loaded || WovNearGraveyard(h.map, h.x, h.y))
            continue;
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

void WovPostTrade(Player* bot)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
    if (!ai)
        return;
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
    switch (p->getClass())
    {
        case CLASS_WARRIOR: return 0;
        case CLASS_PRIEST: return 1;
        case CLASS_DRUID:
        case CLASS_SHAMAN:
        case CLASS_PALADIN: return (p->GetGUID().GetCounter() % 2) ? 1 : 2;
        default: return 2;
    }
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
    WovRegear_Tick(time(nullptr));  // wov: regear once
    WovMarket_Tick(time(nullptr));  // wov: market

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

    // Process real players and populate Battleground Data with player/queue count
    // Opens a queue for bots to join
    for (Player* player : players)
    {
        // Skip player if not currently in a queue
        if (!player->InBattlegroundQueue())
            continue;

        Battleground* bg = player->GetBattleground();
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
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, player->GetLevel());
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
                    (player->InArena() && player->GetBattleground()->isRated()))
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
                if (player->InBattleground())
                {
                    std::vector<uint32>* instanceIds = nullptr;
                    uint32 instanceId = player->GetBattleground()->GetInstanceID();

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

        Battleground* bg = bot->GetBattleground();
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
            PvPDifficultyEntry const* pvpDiff = GetBattlegroundBracketByLevel(mapId, bot->GetLevel());
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

                if (bgQueue.IsPlayerInvitedToRatedArena(guid) || (bot->InArena() && bot->GetBattleground()->isRated()))
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

            if (bot->InBattleground())
            {
                std::vector<uint32>* instanceIds = nullptr;
                uint32 instanceId = bot->GetBattleground()->GetInstanceID();
                bool isArena = false;
                bool isRated = false;

                // Arena logic
                if (bot->InArena())
                {
                    isArena = true;
                    if (bot->GetBattleground()->isRated())
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
    if (sPlayerbotAIConfig.randomBotAutoJoinBG && playerBots.size() >= GetMaxAllowedBotCount())
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
        if (!player || !player->GetGroup())
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
    if (g_wovSchedEnable && player && player->IsInWorld() && !player->GetGroup() &&
        currentBots.size() > g_wovMinOnline && !WovScheduledOnline(bot) &&
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
    if (player && !logout && !isValid)
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
