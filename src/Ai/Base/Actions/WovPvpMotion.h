#pragma once
namespace WovPvp
{
enum class Intent { None, Flank, Kite, Spread, Retreat, Cover };
inline Intent Choose(float health, bool ranged, float distance, bool crowded, bool enemyCasting)
{
    if (distance > 35.0f)
        return Intent::None;
    if (health < 25.0f && distance < 32.0f)
        return Intent::Retreat;
    if (ranged && distance < 12.0f)
        return Intent::Kite;
    if (distance > (ranged ? 28.0f : 6.0f))
        return Intent::None;
    if (ranged && enemyCasting)
        return Intent::Cover;
    if (crowded)
        return Intent::Spread;
    return Intent::Flank;
}
}
