/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AcceptInvitationAction.h"
#include "CompanyStanding.h"
#include "Event.h"
#include "ObjectAccessor.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "WorldPacket.h"

uint32 WovDgLeaderOf(uint32 low);  // wov: RandomPlayerbotMgr.cpp

bool AcceptInvitationAction::Execute(Event event)
{
    Group* grp = bot->GetGroupInvite();
    if (!grp)
        return false;
    WorldPacket packet = event.getPacket();
    uint8 flag;
    std::string name;
    packet >> flag >> name;

    Player* inviter = ObjectAccessor::FindPlayer(grp->GetLeaderGUID());
    if (!inviter)
        return false;

    // wov: a bot on its way to a dungeon run only joins that run's tank
    if (uint32 lead = WovDgLeaderOf(bot->GetGUID().GetCounter()))
        if (inviter->GetGUID().GetCounter() != lead)
        {
            WorldPacket data(SMSG_GROUP_DECLINE, 10);
            data << bot->GetName();
            inviter->SendDirectMessage(&data);
            bot->UninviteFromGroup();
            return false;
        }

    if (!botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, false, inviter))
    {
        WorldPacket data(SMSG_GROUP_DECLINE, 10);
        data << bot->GetName();
        inviter->SendDirectMessage(&data);
        bot->UninviteFromGroup();
        return false;
    }

    // local: company rivals (custom wow plans/14, step C). A bot will not follow a bot of a rival company.
    // Real players are never refused.
    if (sPlayerbotAIConfig.companyRivalDeclineChance && GET_PLAYERBOT_AI(inviter) &&
        CompanyStanding::instance().AreRivals(bot->GetGuildId(), inviter->GetGuildId()) &&
        urand(0, 99) < sPlayerbotAIConfig.companyRivalDeclineChance)
    {
        WorldPacket data(SMSG_GROUP_DECLINE, 10);
        data << bot->GetName();
        inviter->SendDirectMessage(&data);
        bot->UninviteFromGroup();
        return false;
    }

    if (bot->isAFK() && !IsSelfBot(bot))
        bot->ToggleAFK();

    WorldPacket p;
    uint32 roles_mask = 0;
    p << roles_mask;
    bot->GetSession()->HandleGroupAcceptOpcode(p);

    if (!bot->GetGroup() || !bot->GetGroup()->IsMember(inviter->GetGUID()))
        return false;

    if (sRandomPlayerbotMgr.IsRandomBot(bot))
        botAI->SetMaster(inviter);
    // else
    // PlayerbotRepository::instance().Save(botAI);

    botAI->ResetStrategies();
    botAI->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
    botAI->Reset();

    botAI->TellMaster(PlayerbotTextMgr::instance().GetBotTextOrDefault("hello", "Hello", {}));

    // wov: a player says hi in party chat when they join, not a whisper
    if (sRandomPlayerbotMgr.IsRandomBot(bot) && urand(0, 99) < 75)
    {
        static char const* const kHi[] = {"hi", "hey", "hey all", "hi all", "yo", "o/", "hello", "ty for inv",
                                          "thx for the inv", "thanks for the invite", "ty", "sup", "hey guys",
                                          "hiya", "ty for invite", "heya", "hi guys", "yo ty", "thx", "sup all"};
        botAI->SayToParty(kHi[urand(0, sizeof(kHi) / sizeof(kHi[0]) - 1)]);
    }

    if (sPlayerbotAIConfig.summonWhenGroup && bot->GetDistance(inviter) > sPlayerbotAIConfig.sightDistance)
    {
        Teleport(inviter, bot, true);
    }
    return true;
}
