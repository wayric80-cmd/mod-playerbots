/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgTriggers.h"
#include "PlayerbotAI.h"

#include "Map.h"
#include "Player.h"

// wov: no open-world RPG inside a dungeon (it walked and "unstuck"-teleported bots out of instances mid-run)
bool NewRpgStatusTrigger::IsActive()
{
    if (bot->GetMap() && bot->GetMap()->Instanceable() && !bot->InBattleground() && !bot->InArena())
        return false;
    return status == botAI->rpgInfo.GetStatus();
}
