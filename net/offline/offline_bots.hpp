// offline_bots.hpp — the Party Director: exactly four companions (tank, healer, two damage dealers),
// their deterministic Utility AI (the "teacher" of the imitation-learning pipeline) and the legal-action
// mask that always guards the optional neural policy.
//
// The bots never write game state directly. They call the same World actions a player does
// (startAttack / tryCast / moveEntity), so threat, cooldowns, range and resources are enforced by the
// authoritative simulation in exactly one place.
#pragma once

#include <cstdint>

#include "offline_core.hpp"

namespace w3n {
namespace offline {

enum class TacticalAction : uint8_t {
    BasicAttack = 0,    // auto-attack / primary damage filler
    HighPriority = 1,   // the role's main ability (Heroic Strike, Eviscerate, Fire Blast ...)
    Defensive = 2,      // Taunt / Battle Shout / Power Word: Fortitude
    HealSupport = 3,    // a heal on the most hurt ally
    InterruptCc = 4,    // Kick an enemy cast
    Reposition = 5,     // close to the target or the formation slot
    Retreat = 6,        // break off, recover, avoid a pull
    Hold = 7,           // conserve resources / no legal action
};
constexpr int kActionCount = 8;
constexpr int kFeatureCount = 24;

// 24 normalized tactical features, int8 in [0,127] (= 0.0 .. 1.0). Feature 20 is scaled by 127/3,
// feature 21 by 127/11 and feature 23 by 127/7.
struct TacticalFeatures {
    int8_t v[kFeatureCount] = {};
    void set(int i, float f01) {
        if (i < 0 || i >= kFeatureCount) return;
        if (!(f01 > 0.0f)) f01 = 0.0f;
        if (f01 > 1.0f) f01 = 1.0f;
        v[i] = (int8_t)(f01 * 127.0f + 0.5f);
    }
};

// Fill the fixed feature contract from the current world state. 'bot' must be alive.
void buildFeatures(const World& w, const Entity& bot, TacticalFeatures& out, uint32_t nowMs);

// Which of the eight categories are currently legal for this bot (bit i = TacticalAction i).
uint32_t legalActionMask(const World& w, const Entity& bot, uint32_t nowMs);

// The deterministic teacher: scores the legal actions and returns the best one.
TacticalAction utilityChoose(const World& w, const Entity& bot, uint32_t legalMask, uint32_t nowMs);

// One tactical decision + the actions it implies. Called by World::tickBot every 200 ms.
void botThink(World& w, Entity& bot, uint32_t nowMs);

// Director helpers used by the world/server: formation offsets and command mapping.
void formationSlot(const World& w, const Entity& bot, float* x, float* y);
const char* actionName(TacticalAction a);

}  // namespace offline
}  // namespace w3n
