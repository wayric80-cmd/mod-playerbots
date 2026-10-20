/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuildAcceptAction.h"
#include "RandomPlayerbotMgr.h"
#include "DatabaseEnv.h"
#include "CompanyStanding.h"
#include "Event.h"
#include "GuildPackets.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotSecurity.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"

bool GuildAcceptAction::Execute(Event event)
{
    WorldPacket p(event.getPacket());
    p.rpos(0);
    Player* inviter = nullptr;
    std::string Invitedname;
    p >> Invitedname;

    if (normalizePlayerName(Invitedname))
        inviter = ObjectAccessor::FindPlayerByName(Invitedname.c_str());

    if (!inviter)
        return false;

    // wov: a random bot that asked to join (recruitment post) accepts an invite from a guild officer bot
    if (sRandomPlayerbotMgr.IsRandomBot(bot) && sRandomPlayerbotMgr.IsRandomBot(inviter) && !bot->GetGuildId() &&
        inviter->GetGuildId())
    {
        WorldPackets::Guild::AcceptGuildInvite data = WorldPacket(CMSG_GUILD_ACCEPT);
        bot->GetSession()->HandleGuildAcceptOpcode(data);
        return true;
    }

    // wov: a random bot answers a real player's guild invite like a player would: it depends on how much it likes
    // them (mod-ollama-chat sentiment, 0..1, 0.5 = never met). Leaving its own guild for someone is rare.
    if (sRandomPlayerbotMgr.IsRandomBot(bot) && !GET_PLAYERBOT_AI(inviter) && inviter->GetGuildId())
    {
        float like = 0.5f;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT sentiment_value FROM mod_ollama_chat_bot_player_sentiments WHERE bot_guid = {} AND player_guid = {}",
                bot->GetGUID().GetRawValue(), inviter->GetGUID().GetRawValue()))
            like = r->Fetch()[0].Get<float>();
        uint32 chance = bot->GetGuildId() ? (like >= 0.7f ? 15 : 0) : (like >= 0.6f ? 85 : (like <= 0.35f ? 5 : 40));
        bool wovAccept = urand(0, 99) < chance;
        if (wovAccept && bot->GetGuildId())
        {
            WorldPackets::Guild::GuildLeave leave = WorldPacket(CMSG_GUILD_LEAVE);
            bot->GetSession()->HandleGuildLeaveOpcode(leave);
            wovAccept = !bot->GetGuildId();
        }
        if (wovAccept)
        {
            WorldPackets::Guild::AcceptGuildInvite data = WorldPacket(CMSG_GUILD_ACCEPT);
            bot->GetSession()->HandleGuildAcceptOpcode(data);
        }
        else
        {
            WorldPackets::Guild::GuildDeclineInvitation data = WorldPacket(CMSG_GUILD_DECLINE);
            bot->GetSession()->HandleGuildDeclineOpcode(data);
        }
        LOG_INFO("playerbots", "WoV guild invite: {} -> {} (like {:.2f}, chance {}%) {}", inviter->GetName(), bot->GetName(),
                 like, chance, wovAccept ? "accepted" : "declined");
        return true;
    }

    // local: company regard gate (custom wow plans/18, step P2). An unguilded bot joins a real player's company
    // only as far as it trusts them and the people already inside; it says none of the stock lines, and its
    // answer is written to company_answer. Invites from bots keep the stock answer.
    bool const weigh = sPlayerbotAIConfig.companyRegardGate && !GET_PLAYERBOT_AI(inviter);
    char const* reason = nullptr;

    bool accept = true;
    uint32 guildId = inviter->GetGuildId();
    if (!guildId)
    {
        if (!weigh)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "guild_accept_inviter_not_in_guild", "You are not in a guild!", {}));
        reason = "unwilling";
        accept = false;
    }
    else if (bot->GetGuildId())
    {
        // local: company defection (custom wow plans/18, step P7). A guilded bot may leave its company for a real
        // player's; the security check runs first, and the core's own leaving and joining do the rest.
        CompanyStanding::Verdict verdict{false, "in_company"};
        if (weigh && sPlayerbotAIConfig.companyDefectChance &&
            botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, true, inviter, true))
            verdict = CompanyStanding::instance().JudgeDefection(bot, inviter, guildId);

        if (verdict.accept)
        {
            WorldPackets::Guild::GuildLeave leave = WorldPacket(CMSG_GUILD_LEAVE);
            bot->GetSession()->HandleGuildLeaveOpcode(leave);
            if (bot->GetGuildId())   // the core would not let it go
                verdict = {false, "needed"};
        }
        else if (!weigh)
        {
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "guild_accept_already_in_guild", "Sorry, I am in a guild already", {}));
        }

        reason = verdict.reason;
        accept = verdict.accept;
    }
    else if (!botAI->GetSecurity()->CheckLevelFor(PLAYERBOT_SECURITY_INVITE, weigh, inviter, true))
    {
        if (!weigh)
            botAI->TellError(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "guild_accept_declined", "Sorry, I don't want to join your guild :(", {}));
        reason = "unwilling";
        accept = false;
    }
    else if (weigh)
    {
        CompanyStanding::Verdict const verdict = CompanyStanding::instance().JudgeInvite(bot, inviter, guildId);
        accept = verdict.accept;
        reason = verdict.reason;
    }

    if (weigh)
        CompanyStanding::instance().RecordAnswer(bot, inviter, "join", guildId, accept, reason);

    if (accept)
    {
        WorldPackets::Guild::AcceptGuildInvite data = WorldPacket(CMSG_GUILD_ACCEPT);
        bot->GetSession()->HandleGuildAcceptOpcode(data);
    }
    else
    {
        WorldPackets::Guild::GuildDeclineInvitation data = WorldPacket(CMSG_GUILD_DECLINE);
        bot->GetSession()->HandleGuildDeclineOpcode(data);
    }

    return true;
}
