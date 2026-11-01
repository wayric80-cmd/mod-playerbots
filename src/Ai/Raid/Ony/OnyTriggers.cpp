/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "OnyTriggers.h"
#include "GenericTriggers.h"
#include "NearestNpcsValue.h"
#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

#include <mutex>
#include <unordered_map>

// wov: the flames of a Deep Breath sweep the room after the cast has ended, and bots went straight back to the
// whelps in its path (31/10: tanks died 40-55 yd in front of her). The breath is remembered per instance and the
// bots stay in the safe zone for kWovBreathHold seconds after the cast.
namespace
{
std::mutex g_wovBreathMx;
std::unordered_map<uint32, std::pair<uint32, time_t>> g_wovBreath;  // instance -> (breath spell, last seen casting)
time_t const kWovBreathHold = 6;
}

uint32 WovOnyRecentBreath(Unit* boss)
{
    if (!boss || !boss->GetMap())
        return 0;
    std::lock_guard<std::mutex> lk(g_wovBreathMx);
    auto it = g_wovBreath.find(boss->GetMap()->GetInstanceId());
    return it != g_wovBreath.end() && time(nullptr) <= it->second.second + kWovBreathHold ? it->second.first : 0;
}

OnyxiaDeepBreathTrigger::OnyxiaDeepBreathTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ony deep breath warning") {}

bool OnyxiaDeepBreathTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss)
        return false;
    if (!boss->HasUnitState(UNIT_STATE_CASTING))
        return WovOnyRecentBreath(boss) != 0;

    // Check if Onyxia is casting
    Spell* currentSpell = boss->GetCurrentSpell(CURRENT_GENERIC_SPELL);

    if (!currentSpell || !currentSpell->m_spellInfo)
        return false;

    uint32 spellId = currentSpell->m_spellInfo->Id;

    if (spellId == 17086 ||  // North to South
        spellId == 18351 ||  // South to North
        spellId == 18576 ||  // East to West
        spellId == 18609 ||  // West to East
        spellId == 18564 ||  // Southeast to Northwest
        spellId == 18584 ||  // Northwest to Southeast
        spellId == 18596 ||  // Southwest to Northeast
        spellId == 18617     // Northeast to Southwest
    )
    {
        std::lock_guard<std::mutex> lk(g_wovBreathMx);
        g_wovBreath[boss->GetMap()->GetInstanceId()] = {spellId, time(nullptr)};
        return true;
    }

    return false;
}

OnyxiaNearTailTrigger::OnyxiaNearTailTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ony near tail") {}

bool OnyxiaNearTailTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss || botAI->IsTank(bot))
        return false;

    // Skip if Onyxia is in air or transitioning
    if (!boss->IsInCombat() || boss->IsFlying() || !boss->GetVictim())
        return false;

    return true;
}
RaidOnyxiaFireballSplashTrigger::RaidOnyxiaFireballSplashTrigger(PlayerbotAI* botAI)
    : Trigger(botAI, "ony fireball splash incoming")
{
}

bool RaidOnyxiaFireballSplashTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss || !boss->HasUnitState(UNIT_STATE_CASTING))
        return false;

    // Check if Onyxia is casting Fireball
    Spell* currentSpell = boss->GetCurrentSpell(CURRENT_GENERIC_SPELL);
    if (!currentSpell || !currentSpell->m_spellInfo || currentSpell->m_spellInfo->Id != 18392)  // 18392 is the classic Fireball ID  // 18392 is the classic Fireball ID
        return false;

    GuidVector nearbyUnits = AI_VALUE(GuidVector, "nearest friendly players");

    for (ObjectGuid guid : nearbyUnits)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == bot || !unit->IsAlive())
            continue;

        if (bot->GetDistance(unit) < 8.0f)
            return true;
    }

    return false;
}

RaidOnyxiaWhelpsSpawnTrigger::RaidOnyxiaWhelpsSpawnTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ony whelps spawn") {}

bool RaidOnyxiaWhelpsSpawnTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "onyxia");
    if (!boss)
        return false;

    return !botAI->IsHeal(bot) && boss->IsFlying();  // DPS + Tanks only
}

OnyxiaAvoidEggsTrigger::OnyxiaAvoidEggsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "ony avoid eggs") {}

bool OnyxiaAvoidEggsTrigger::IsActive()
{
    Position botPos = Position(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());

    if (botPos.GetExactDist2d(-35.0f, -165.0f) <= 5.0f)
        return true;

    if (botPos.GetExactDist2d(-35.0f, -260.0f) <= 5.0f)
        return true;

    return false;
}
