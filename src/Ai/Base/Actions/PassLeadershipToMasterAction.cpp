/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PassLeadershipToMasterAction.h"
#include "Event.h"
#include "PlayerbotOperations.h"
#include "PlayerbotWorldThreadProcessor.h"

bool WovBotReserved(uint32 low);  // wov: RandomPlayerbotMgr.cpp (dungeon runs, raid nights)

bool PassLeadershipToMasterAction::Execute(Event /*event*/)
{
    if (Player* master = GetMaster())
        if (master && master != bot && bot->GetGroup() && bot->GetGroup()->IsMember(master->GetGUID()))
        {
            auto setLeaderOp = std::make_unique<GroupSetLeaderOperation>(bot->GetGUID(), master->GetGUID());
            PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(setLeaderOp));

            if (!message.empty())
                botAI->TellMasterNoFacing(message);

            if (sRandomPlayerbotMgr.IsRandomBot(bot))
            {
                botAI->ResetStrategies();
                botAI->Reset();
            }

            return true;
        }

    return false;
}

bool PassLeadershipToMasterAction::isUseful()
{
    return botAI->IsAltBot() && bot->GetGroup() && bot->GetGroup()->IsLeader(bot->GetGUID());
}

bool GiveLeaderAction::isUseful()
{
    // wov 2/11: in a dungeon run or raid the bot tank leads and keeps the lead (it handed it to the real player after
    // every pull - "give leader in dungeon" - and reset its own strategies each time)
    if (WovBotReserved(bot->GetGUID().GetCounter()))
        return false;
    return (IsRealPlayer(botAI->GetMaster()) || IsSelfBot(botAI->GetMaster())) && bot->GetGroup() &&
           bot->GetGroup()->IsLeader(bot->GetGUID());
}
