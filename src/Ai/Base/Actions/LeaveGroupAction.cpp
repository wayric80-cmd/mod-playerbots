/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LeaveGroupAction.h"
#include "Event.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotTextMgr.h"
#include "ServerFacade.h"
#include "Playerbots.h"

bool LeaveGroupAction::Execute(Event event)
{
    Player* player = event.getOwner();
    if (player == botAI->GetMaster())
        return Leave();

    return false;
}

bool PartyCommandAction::Execute(Event event)
{
    WorldPacket& p = event.getPacket();
    p.rpos(0);
    uint32 operation;
    std::string member;

    p >> operation >> member;

    if (operation != PARTY_OP_LEAVE)
        return false;
    // Only leave if master has left the party, and randombot cannot set new master.
    Player* master = GetMaster();
    if (master && member == master->GetName())
    {
        if (sRandomPlayerbotMgr.IsRandomBot(bot))
        {
            Player* newMaster = botAI->FindNewMaster();
            if (newMaster || bot->InBattleground())
            {
                botAI->SetMaster(newMaster);
                return false;
            }
        }
        return Leave();
    }
    return false;
}

bool UninviteAction::Execute(Event event)
{
    WorldPacket& p = event.getPacket();
    if (p.GetOpcode() == CMSG_GROUP_UNINVITE)
    {
        p.rpos(0);
        std::string memberName;
        p >> memberName;

        // player not found
        if (!normalizePlayerName(memberName))
        {
            return false;
        }

        if (bot->GetName() == memberName)
            return Leave();
    }

    if (p.GetOpcode() == CMSG_GROUP_UNINVITE_GUID)
    {
        p.rpos(0);
        ObjectGuid guid;
        p >> guid;

        if (bot->GetGUID() == guid)
            return Leave();
    }

    return false;
}

bool LeaveGroupAction::Leave()
{
    if (!botAI)
        return false;

    Player* master = botAI -> GetMaster();
    if (master)
        botAI->TellMaster(
            PlayerbotTextMgr::instance().GetBotTextOrDefault("goodbye", "Goodbye!", {}),
            PLAYERBOT_SECURITY_TALK);

    // wov: a player says bye in party chat when they leave, not a whisper
    if (sRandomPlayerbotMgr.IsRandomBot(bot) && bot->GetGroup() && urand(0, 99) < 70)
    {
        static char const* const kBye[] = {"ty for group", "thx for group", "ty all", "gg ty", "cya", "thx all gl",
                                           "ty gl", "bye all", "gtg ty", "thanks for the group", "ty ty", "gg",
                                           "thx guys", "ty, gl all", "cya guys", "ty for the run", "later all"};
        botAI->SayToParty(kBye[urand(0, sizeof(kBye) / sizeof(kBye[0]) - 1)]);
    }

    botAI->LeaveOrDisbandGroup();
    return true;
}

bool LeaveFarAwayAction::Execute(Event /*event*/)
{
    // allow bot to leave party when they want
    return Leave();
}

bool LeaveFarAwayAction::isUseful()
{
    if (bot->InBattleground())
        return false;

    if (bot->InBattlegroundQueue())
        return false;

    if (!bot->GetGroup())
        return false;

    Player* groupLeader = botAI->GetGroupLeader();
    Player* trueMaster = botAI->GetMaster();
    if (!groupLeader || (bot == groupLeader && !IsSelfBot(bot)))
        return false;

    PlayerbotAI* groupLeaderBotAI = nullptr;
    if (groupLeader)
        groupLeaderBotAI = GET_PLAYERBOT_AI(groupLeader);
    if (groupLeader && !groupLeaderBotAI)
        return false;

    if (trueMaster && !GET_PLAYERBOT_AI(trueMaster))
        return false;

    if (botAI->IsAltBot() &&
        (!groupLeaderBotAI || IsSelfBot(groupLeader)))  // Don't leave when an altbot is grouped under a regular real player or a selfbot.
        return false;

    // Local patch (custom-wow, plans/31 §15 H3). A person who walks up wanting hands cannot recruit anyone
    // already in a company: the core refuses the invite with ERR_ALREADY_IN_GROUP_S (GroupHandler.cpp:164)
    // before any bot code runs, so nothing here can react to being asked. The only way a bot can be
    // available is to be free before the asking. Everything above has established this is a bot-led company
    // with no person in it, so breaking it costs nobody anything -- and company dispersing when someone
    // comes looking for help is what people do.
    //
    // Chance-gated and off by default: with grouping on, an unbounded rule would dissolve every company
    // within sight of the player and make the world empty around him, which is the opposite of the point.
    if (sPlayerbotAIConfig.yieldGroupToPlayerChance && urand(0, 99) < sPlayerbotAIConfig.yieldGroupToPlayerChance)
    {
        GuidVector nearby = botAI->GetAiObjectContext()->GetValue<GuidVector>("nearest friendly players")->Get();
        for (ObjectGuid const& guid : nearby)
        {
            Player* person = ObjectAccessor::FindPlayer(guid);
            if (!person || person == bot || GET_PLAYERBOT_AI(person))
                continue;                       // only someone at a keyboard
            if (person->GetGroup() || person->IsBeingTeleported() || person->isDND())
                continue;                       // already has company, or not asking for any
            if (person->GetMapId() != bot->GetMapId())
                continue;
            if (ServerFacade::instance().GetDistance2d(bot, person) > sPlayerbotAIConfig.rpgDistance)
                continue;                       // walked up to, not merely in the same field
            return true;
        }
    }

    if (botAI->GetGrouperType() == GrouperType::SOLO)
        return true;

    uint32 dCount = AI_VALUE(uint32, "death count");

    if (dCount > 9)
        return true;

    if (dCount > 4 && !botAI->HasGameClientMaster())
        return true;

    if (bot->GetGuildId() == groupLeader->GetGuildId())
    {
        if (bot->GetLevel() > groupLeader->GetLevel() + 5)
        {
            if (AI_VALUE(bool, "should get money"))
                return false;
        }
    }

    if (abs(int32(groupLeader->GetLevel() - bot->GetLevel())) > 4)
        return true;

    if (bot->GetMapId() != groupLeader->GetMapId() || bot->GetDistance2d(groupLeader) >= 2 * sPlayerbotAIConfig.rpgDistance)
    {
        return true;
    }

    return false;
}
