/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Playerbots.h"
#include "BattleGroundTactics.h"
#include "BattlefieldScript.h"
#include "Channel.h"
#include "CheckMountStateAction.h"
#include "Config.h"
#include "BuiltInConfig.h"
#include "DBUpdater.h"
#include "DatabaseEnv.h"
#include "PlayerbotsDatabase.h"
#include <mysqld_error.h>
#include "GuildTaskMgr.h"
#include "PlayerScript.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotCommandScript.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotSpellRepository.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "RandomPlayerbotMgr.h"
#include "RandomBotLevelMgr.h"
#include "ScriptMgr.h"
#include "cmath"

class PlayerbotsDatabaseScript : public DatabaseScript
{
public:
    PlayerbotsDatabaseScript() : DatabaseScript("PlayerbotsDatabaseScript") {}

    bool OnModuleDatabasesLoading() override
    {
        std::string const dbString = sConfigMgr->GetOption<std::string>("PlayerbotsDatabaseInfo", "");
        if (dbString.empty())
        {
            LOG_ERROR("server.playerbots", "Playerbots database is not specified in configuration file");
            return false;
        }

        uint8 const synchThreads = sConfigMgr->GetOption<uint8>("PlayerbotsDatabase.SynchThreads", 2);
        PlayerbotsDatabase.SetConnectionInfo(dbString, synchThreads);

        bool const updatesEnabled = sConfigMgr->GetOption<bool>("Playerbots.Updates.EnableDatabases", true);
        if (updatesEnabled && !DBUpdaterUtil::CheckExecutable())
            return false;

        uint32 error = PlayerbotsDatabase.Open();
        if (error == ER_BAD_DB_ERROR && updatesEnabled)
        {
            // Database missing: create it through the mysql CLI and connect again
            if (!ModuleDBUpdater::Create(PlayerbotsDatabase))
                return false;

            error = PlayerbotsDatabase.Open();
        }

        if (error)
        {
            LOG_ERROR("server.playerbots", "Cannot connect to the playerbots database, error {}", error);
            return false;
        }

        if (updatesEnabled)
        {
            DBUpdaterInfo const info = {
                "Playerbots",
                BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbots",
                BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbots/data/sql/playerbots/base/",
                "db_playerbot"
            };

            if (!ModuleDBUpdater::Populate(PlayerbotsDatabase, info))
            {
                LOG_ERROR("server.playerbots", "Could not populate the playerbots database, see log for details.");
                return false;
            }

            if (!ModuleDBUpdater::Update(PlayerbotsDatabase, info))
            {
                LOG_ERROR("server.playerbots", "Could not update the playerbots database, see log for details.");
                return false;
            }
        }

        if (!PlayerbotsDatabase.PrepareStatements())
        {
            LOG_ERROR("server.playerbots", "Could not prepare statements of the playerbots database, see log for details.");
            return false;
        }

        return true;
    }

    void OnModuleDatabasesKeepAlive() override { PlayerbotsDatabase.KeepAlive(); }

    void OnModuleDatabasesClosing() override { PlayerbotsDatabase.Close(); }

    void OnDatabaseWarnAboutSyncQueries(bool apply) override { PlayerbotsDatabase.WarnAboutSyncQueries(apply); }

    // local: per-character logout (plan 34 section 4.1).
    //
    // Upstream dropped this override in #2793. Without it the core default applies --
    // CHAR_UPD_ACCOUNT_ONLINE, "UPDATE characters SET online = 0 WHERE account = ?" keyed on the
    // account id (WorldSession::LogoutPlayer). Our 158 bot accounts hold ~10 characters each, so one
    // bot logging out would mark every character on its account offline while they are still in the
    // world, and the flag is only set again at login. regard.py joins on online = 1 to decide who is
    // about, and add-bots.sh and lore-fill.sh count with it.
    //
    // CHAR_UPD_CHAR_OFFLINE is the same statement with "WHERE guid = ?", so only the character that
    // actually logged out is marked.
    void OnDatabaseSelectIndexLogout(Player* player, uint32& statementIndex, uint32& statementParam) override
    {
        statementIndex = CHAR_UPD_CHAR_OFFLINE;
        statementParam = player->GetGUID().GetCounter();
    }

    void OnDatabaseGetDBRevision(std::map<std::string, std::string>& revisions) override
    {
        std::string revision;
        if (QueryResult resultPlayerbot =
                PlayerbotsDatabase.Query("SELECT date FROM version_db_playerbots ORDER BY date DESC LIMIT 1"))
        {
            Field* fields = resultPlayerbot->Fetch();
            revision = fields[0].Get<std::string>();
        }

        if (revision.empty())
            revision = "Unknown Playerbots Database Revision";

        revisions["Playerbots"] = revision;
    }
};

void WovInit_OnWhisper(Player* from, Player* to);  // wov (RandomPlayerbotMgr.cpp)

class PlayerbotsPlayerScript : public PlayerScript
{
public:
    PlayerbotsPlayerScript() : PlayerScript("PlayerbotsPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_PVP_KILL,
        PLAYERHOOK_ON_AFTER_UPDATE,
        PLAYERHOOK_ON_BEFORE_CRITERIA_PROGRESS,
        PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_TELEPORT,
        PLAYERHOOK_CAN_GROUP_INVITE,
        PLAYERHOOK_ON_SEND_LIST_INVENTORY
    }) {}

    // Local patch (custom-wow, plans/25 item 65). Bots sell their greys when the person they are
    // travelling with opens a merchant window.
    //
    // Narrow deliberately, and the narrowness is the point. Selling in this module is keyed on the
    // BOT's own surroundings: SellAction::Sell resolves a vendor out of the bot's "nearest npcs"
    // value and sells through the bot's own session, never once reading the vendorGuid handed in
    // here (SellAction.cpp:150-184). So "the party sells when you open a window" cannot be honoured
    // as it was written down -- a bot across the zone would silently sell nothing, and the feature
    // would work or not work depending on where everyone happened to be standing. This fires only
    // for bots that can reach the SAME counter, which is the version that does what it looks like.
    //
    // Greys only, and the literal is hard-coded on purpose: "gray" resolves to ITEM_QUALITY_POOR
    // with allClasses false (SellAction.cpp:94-99), which keeps equipment and profession tools out
    // of it. Never route a caller-supplied quality string through here.
    void OnPlayerSendListInventory(Player* player, ObjectGuid vendorGuid, uint32& /*vendorEntry*/) override
    {
        if (!sPlayerbotAIConfig.sellGrayOnVendorOpen)
            return;

        // A person at a keyboard opening their own window. Nothing in the module sends
        // CMSG_LIST_INVENTORY on a bot's behalf today, but that is a fact about callers rather than
        // a guarantee, so it is checked rather than assumed.
        if (!player || GET_PLAYERBOT_AI(player))
            return;

        Group* group = player->GetGroup();
        if (!group)
            return;

        // This hook fires on EVERY CMSG_LIST_INVENTORY -- every reopen, and every client re-sync of
        // an open window -- and SellAction keeps no cooldown of its own, so without this a player
        // reopening a vendor would re-run the sweep each time. Keyed per bot; bounded by the number
        // of bots that have ever sold, and only ever touched from the world thread.
        static std::unordered_map<ObjectGuid, time_t> lastSweep;
        time_t const now = time(nullptr);

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player)
                continue;

            PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
            if (!botAI || !botAI->IsBotAI() || IsSelfBot(member))
                continue;               // a person, or someone at a keyboard: leave them alone

            // The same counter, not merely some vendor somewhere. This is the whole difference
            // between this and firing the existing `s gray` at the entire party.
            if (!member->GetNPCIfCanInteractWith(vendorGuid, UNIT_NPC_FLAG_VENDOR))
                continue;

            time_t& last = lastSweep[member->GetGUID()];
            if (last && now - last < 10)
                continue;
            last = now;

            botAI->DoSpecificAction("sell", Event("sell", "gray"), true);
        }
    }

    // Local patch (custom-wow, plans/31 §17). A person asking a bot to come along must always win.
    //
    // The core refuses an invite to anyone already in a group (GroupHandler.cpp, ERR_ALREADY_IN_GROUP_S),
    // and that refusal is several checks further down than this hook -- so this is the one place where a
    // bot's own company can be stood down before the invite is judged. Without it, switching bot grouping
    // on would make a share of the realm unrecruitable, which is the opposite of what it is for.
    //
    // Only bot-led companies are broken up: a party holding a person belongs to that person, and nobody
    // gets their group taken from them by someone else's invite. The bot leaves of its own accord rather
    // than being removed, so nothing reads as a kick -- regard.py charges kicked_by (-10) only when
    // group_leave carries a kicker, and leaving a party costs a bot nothing with its companions.
    [[nodiscard]] bool OnPlayerCanGroupInvite(Player* player, std::string& membername) override
    {
        if (!player)
            return true;

        Player* invited = ObjectAccessor::FindPlayerByName(membername, false);
        if (!invited || invited == player)
            return true;

        PlayerbotAI* invitedAI = GET_PLAYERBOT_AI(invited);
        if (!invitedAI || !invitedAI->IsBotAI() || IsSelfBot(invited))
            return true;            // a person, or someone at a keyboard: leave them alone

        if (GET_PLAYERBOT_AI(player) && !IsSelfBot(player))
            return true;            // bots inviting bots do not get to break up other companies

        Group* group = invited->GetGroup();
        if (!group || group->isBGGroup() || group->isLFGGroup())
            return true;

        // Someone real in there already? Then it is their company, not ours to dissolve.
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member)
                continue;
            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            if (!memberAI || IsSelfBot(member))
                return true;
        }

        invitedAI->LeaveOrDisbandGroup();

        LOG_DEBUG("playerbots", "[Playerbots] {} stood down from its company so {} could invite it",
                  invited->GetName(), player->GetName());

        return true;
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!player->GetSession()->IsBot())
        {
            PlayerbotsMgr::instance().AddPlayerbotData(player, false);
            sRandomPlayerbotMgr.OnPlayerLogin(player);

            // Before modifying the following messages, please make sure it does not violate the GNU GPLv2
            // license especially if you are distributing a repack or hosting a public server
            // e.g. you can replace the URL with your own repository,
            // but it should be publicly accessible and include all modifications you've made
            // wov: no login lines that reveal the bots (private realm, not distributed)
            if (false && sPlayerbotAIConfig.enabled)
            {
                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cff00ff00This server runs with |cff00ccffmod-playerbots|r "
                    "|cffcccccchttps://github.com/mod-playerbots/mod-playerbots|r");
            }

            if (false && (sPlayerbotAIConfig.enabled || sPlayerbotAIConfig.randomBotAutologin))
            {
                std::string maxAllowedBotCount = std::to_string(sRandomPlayerbotMgr.GetMaxAllowedBotCount());

                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cff00ff00Playerbots:|r The server is configured with " + maxAllowedBotCount + " bots.");
            }
        }
    }

    bool OnPlayerBeforeTeleport(Player* /*player*/, uint32 /*mapid*/, float /*x*/, float /*y*/, float /*z*/,
                                float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        /* for now commmented out until proven its actually required
        * havent seen any proof CleanVisibilityReferences() is needed

        // If the player is not safe to touch, do nothing
        if (!player)
            return true;

        // If same map or not in world do nothing
        if (!player->IsInWorld() || player->GetMapId() == mapid)
            return true;

        // If this is a selfbot, do nothing
        PlayerbotAI* ai = GET_PLAYERBOT_AI(player);
        if (!ai || IsSelfBot(player))
            return true;

        // Cross-map bot teleport: defer visibility reference cleanup.
        // CleanVisibilityReferences() erases this bot's GUID from other objects' visibility containers.
        // This is intentionally done via the event queue (instead of directly here) because erasing
        // from other players' visibility maps inside the teleport call stack can hit unsafe re-entrancy
        // or iterator invalidation while visibility updates are in progress
        ObjectGuid guid = player->GetGUID();
        player->m_Events.AddEventAtOffset(
            [guid, mapid]()
            {
                // do nothing, if the player is not safe to touch
                Player* p = ObjectAccessor::FindPlayer(guid);
                if (!p || !p->IsInWorld() || p->IsDuringRemoveFromWorld())
                    return;

                // do nothing if we are already on the target map
                if (p->GetMapId() == mapid)
                    return;

                p->GetObjectVisibilityContainer().CleanVisibilityReferences();
            },
            Milliseconds(0));

        */

        return true;
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override { WovWar_OnPvpKill(killer, killed); }  // wov

    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
        {
            botAI->UpdateAI(diff);
        }

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
        {
            playerbotMgr->UpdateAI(diff);
        }
    }

    using PlayerScript::OnPlayerCanUseChat;  // keep the base overloads visible

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER)
        {
            return true;
        }

        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);

        if (botAI == nullptr)
        {
            return true;
        }

        WovInit_OnWhisper(player, receiver);  // wov: answering an LFG/LFM post gets an invite

        botAI->HandleCommand(type, msg, player);

        // hotfix; otherwise the server will crash when whispering logout
        // https://github.com/mod-playerbots/mod-playerbots/pull/1838
        // TODO: find the root cause and solve it. (does not happen in party chat)
        if (msg == "logout")
            return false;

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Group* group) override
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* const member = itr->GetSource();

            if (member == nullptr)
                continue;

            PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);

            if (botAI == nullptr)
                continue;

            botAI->HandleCommand(type, msg, player);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Guild* /*guild*/) override
    {
        if (type != CHAT_MSG_GUILD)
            return true;

        PlayerbotMgr* playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);

        if (playerbotMgr == nullptr)
            return true;

        for (PlayerBotMap::const_iterator it = playerbotMgr->GetPlayerBotsBegin(); it != playerbotMgr->GetPlayerBotsEnd(); ++it)
        {
            Player* const bot = it->second;

            if (bot == nullptr)
                continue;

            if (bot->GetGuildId() != player->GetGuildId())
                continue;

            PlayerbotsMgr::instance().GetPlayerbotAI(bot)->HandleCommand(type, msg, player);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        PlayerbotMgr* const playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);

        if (playerbotMgr != nullptr && channel->GetFlags() & 0x18)
            playerbotMgr->HandleCommand(type, msg);

        sRandomPlayerbotMgr.HandleCommand(type, msg, player);

        return true;
    }

    bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if ((sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player)) &&
            (achievement->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_REACH | ACHIEVEMENT_FLAG_REALM_FIRST_KILL)))
        {
            return false;
        }

        return true;
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        // early return
        if (sPlayerbotAIConfig.randomBotXPRate == 1.0 || !player)
            return;

        // no XP multiplier, when player is no bot.
        if (!player->GetSession()->IsBot() || !sRandomPlayerbotMgr.IsRandomBot(player))
            return;

        // no XP multiplier, when bot is in a group with a real player.
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->GetSource();
                if (!member)
                    continue;

                if (!member->GetSession()->IsBot())
                    return;
            }
        }

        // otherwise apply bot XP multiplier.
        amount = static_cast<uint32>(std::round(static_cast<float>(amount) * sPlayerbotAIConfig.randomBotXPRate));
    }
};

class PlayerbotsMiscScript : public MiscScript
{
public:
    PlayerbotsMiscScript() : MiscScript("PlayerbotsMiscScript", {MISCHOOK_ON_DESTRUCT_PLAYER}) {}

    void OnDestructPlayer(Player* player) override
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
            delete botAI;

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
            delete playerbotMgr;
    }
};

class PlayerbotsServerScript : public ServerScript
{
public:
    PlayerbotsServerScript() : ServerScript("PlayerbotsServerScript", {
        SERVERHOOK_CAN_PACKET_RECEIVE
    }) {}

    void OnPacketReceived(WorldSession* session, WorldPacket const& packet) override
    {
        if (Player* player = session->GetPlayer())
            if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
                playerbotMgr->HandleMasterIncomingPacket(packet);
    }
};

class PlayerbotsWorldScript : public WorldScript
{
public:
    PlayerbotsWorldScript() : WorldScript("PlayerbotsWorldScript", {
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
        WORLDHOOK_ON_UPDATE
    }) {}

    void OnBeforeWorldInitialized() override
    {
        // Before modifying the following messages, please make sure it does not violate the GNU GPLv2
        // license especially if you are distributing a repack or hosting a public server
        // e.g. you can replace the URL with your own repository,
        // but it should be publicly accessible and include all modifications you've made
        LOG_INFO("server.loading", "╔══════════════════════════════════════════════════════════╗");
        LOG_INFO("server.loading", "║                                                          ║");
        LOG_INFO("server.loading", "║              AzerothCore Playerbots Module               ║");
        LOG_INFO("server.loading", "║                                                          ║");
        LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
        LOG_INFO("server.loading", "║     mod-playerbots is a community-driven open-source     ║");
        LOG_INFO("server.loading", "║  project based on AzerothCore, licensed under GNU GPLv2  ║");
        LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
        LOG_INFO("server.loading", "║     https://github.com/mod-playerbots/mod-playerbots     ║");
        LOG_INFO("server.loading", "╚══════════════════════════════════════════════════════════╝");

        uint32 oldMSTime = getMSTime();

        LOG_INFO("server.loading", " ");
        LOG_INFO("server.loading", "Load Playerbots Config...");

        sPlayerbotAIConfig.Initialize();

        LOG_INFO("server.loading", ">> Loaded playerbots config in {} ms", GetMSTimeDiffToNow(oldMSTime));
        LOG_INFO("server.loading", " ");

        PlayerbotSpellRepository::Instance().Initialize();
        CheckMountStateAction::LoadPreferredMounts();

        LOG_INFO("server.loading", "Playerbots World Thread Processor initialized");
    }

    void OnUpdate(uint32 diff) override
    {
        PlayerbotWorldThreadProcessor::instance().Update(diff);
        sRandomPlayerbotMgr.UpdateAI(diff);  // World thread only
    }
};

class PlayerbotsScript : public PlayerbotScript
{
public:
    PlayerbotsScript() : PlayerbotScript("PlayerbotsScript") {}

    bool OnPlayerbotCheckLFGQueue(lfg::Lfg5Guids const& guidsList) override
    {
        bool nonBotFound = false;

        for (ObjectGuid const& guid : guidsList.guids)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);

            if (guid.IsGroup() || IsRealPlayer(player) || IsSelfBot(player))
            {
                nonBotFound = true;
                break;
            }
        }

        return nonBotFound;
    }

    void OnPlayerbotCheckKillTask(Player* player, Unit* victim) override
    {
        if (player)
            GuildTaskMgr::instance().CheckKillTask(player, victim);
    }

    void OnPlayerbotCheckPetitionAccount(Player* player, bool& found) override
    {
        if (!found)
            return;

        if (PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr)
            found = false;
    }

    bool OnPlayerbotCheckUpdatesToSend(Player* player) override
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI == nullptr)
            return true;

        return IsSelfBot(player);
    }

    void OnPlayerbotPacketSent(Player* player, WorldPacket const* packet) override
    {
        if (player == nullptr)
            return;

        if (packet && sPlayerbotAIConfig.persistentProgressionAnchorOnMeeting)
            RandomBotLevelMgr::instance().ObserveMeetingTradePacket(player, *packet);

        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
            botAI->HandleBotOutgoingPacket(*packet);

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
            playerbotMgr->HandleMasterOutgoingPacket(*packet);
    }

    void OnPlayerbotUpdate(uint32 /*diff*/) override
    {
        sRandomPlayerbotMgr.UpdateSessions();  // Per-bot updates only
    }

    void OnPlayerbotUpdateSessions(Player* player) override
    {
        if (player)
            if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
                playerbotMgr->UpdateSessions();
    }

    void OnPlayerbotLogout(Player* player) override
    {
        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
        {
            PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

            if (botAI == nullptr || IsSelfBot(player))
                playerbotMgr->LogoutAllBots();
        }

        sRandomPlayerbotMgr.OnPlayerLogout(player);
    }

    void OnPlayerbotLogoutBots() override
    {
        LOG_INFO("playerbots", "Logging out all bots...");
        sRandomPlayerbotMgr.LogoutAllBots();
    }
};

class PlayerBotsBGScript : public BGScript
{
public:
    PlayerBotsBGScript() : BGScript("PlayerBotsBGScript") {}

    void OnBattlegroundStart(Battleground* bg) override
    {
        BGStrategyData data;

        switch (bg->GetBgTypeID())
        {
            case BATTLEGROUND_WS:
                data.allianceStrategy = urand(0, WS_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, WS_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_AB:
                data.allianceStrategy = urand(0, AB_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, AB_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_AV:
                data.allianceStrategy = urand(0, AV_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, AV_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_EY:
                data.allianceStrategy = urand(0, EY_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, EY_STRATEGY_MAX - 1);
                break;
            default:
                break;
        }

        bgStrategies[bg->GetInstanceID()] = data;
    }

    void OnBattlegroundEnd(Battleground* bg, TeamId /*winnerTeam*/) override { bgStrategies.erase(bg->GetInstanceID()); }
};

// Workaround for missing InitEnabledHooksIfNeeded for new BattlefieldScript in ScriptMgr
class PlayerbotsBattlefieldScript : public BattlefieldScript
{
public:
    PlayerbotsBattlefieldScript() : BattlefieldScript("PlayerbotsBattlefieldScript") { }
};

void AddPlayerbotsSecureLoginScripts();
void AddPlayerbotsSelfBotAfkScripts();
void AddPlayerbotEncounterScripts();

void AddSC_MagtheridonBotScripts();
void AddSC_TempestKeepBotScripts();
void AddSC_HyjalBotScripts();
void AddSC_IcecrownBotScripts();
void AddSC_RubySanctumBotScripts();
void AddSC_randombot_level_mgr();

void AddPlayerbotsScripts()
{
    new PlayerbotsBattlefieldScript();
    new PlayerbotsDatabaseScript();
    new PlayerbotsPlayerScript();
    new PlayerbotsMiscScript();
    new PlayerbotsServerScript();
    new PlayerbotsWorldScript();
    new PlayerbotsScript();
    new PlayerBotsBGScript();
    AddPlayerbotsSecureLoginScripts();
    AddPlayerbotsSelfBotAfkScripts();
    AddPlayerbotEncounterScripts();
    AddPlayerbotsCommandscripts();
    PlayerBotsGuildValidationScript();
    AddSC_MagtheridonBotScripts();
    AddSC_TempestKeepBotScripts();
    AddSC_HyjalBotScripts();
    AddSC_IcecrownBotScripts();
    AddSC_RubySanctumBotScripts();
    AddSC_randombot_level_mgr();
}
