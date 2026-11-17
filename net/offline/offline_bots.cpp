// offline_bots.cpp — Party Director + deterministic Utility AI (teacher) + neural policy arbitration.
#include "offline_bots.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "offline_nn.hpp"

namespace w3n {
namespace offline {

namespace {

constexpr uint32_t kDecisionMs = 200;      // 5 tactical decisions per second per bot
constexpr float kPi = 3.14159265f;

const Entity* botAt(const World& w, int i) { return w.bot(i); }
const Entity* tankOf(const World& w) { return w.bot(0); }
const Entity* healerOf(const World& w) { return w.bot(1); }

float dist2d(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

int classOf(const World& w, const Entity& bot) {
    const WorldData* d = w.data();
    if (!d || bot.botIndex < 0 || bot.botIndex >= kMaxParty) return 1;
    return d->bots[(size_t)bot.botIndex].cls;
}

const SpellDef* defOf(const World& w, uint32_t id) {
    return w.data() ? w.data()->spell(id) : nullptr;
}

bool knows(const Entity& e, uint32_t id) {
    for (uint8_t i = 0; i < e.spellCount; ++i)
        if (e.spells[i] == id) return true;
    return false;
}

bool canUse(const World& w, const Entity& bot, const SpellDef& s, const Entity* target, uint32_t nowMs) {
    if (!knows(bot, s.id)) return false;
    if (w.cooldownLeft(bot, s.id, nowMs) > 0) return false;
    if (s.powerCost > (uint32_t)std::max<int32_t>(0, bot.power)) return false;
    if (target) {
        const float maxR = s.maxRange > 0 ? s.maxRange : 5.0f;
        if (dist2d(bot.x, bot.y, target->x, target->y) > maxR + target->boundingRadius) return false;
    }
    return true;
}

// The first spell of a kind this bot can use right now (skipping the Attack filler 6603).
const SpellDef* readySpell(const World& w, const Entity& bot, SpellKind kind, const Entity* target, uint32_t nowMs,
                           uint32_t skip = 6603) {
    for (uint8_t i = 0; i < bot.spellCount; ++i) {
        const uint32_t id = bot.spells[i];
        if (id == skip) continue;
        const SpellDef* s = defOf(w, id);
        if (!s || s->kind != kind) continue;
        if (canUse(w, bot, *s, target, nowMs)) return s;
    }
    return nullptr;
}

const SpellDef* mainAbility(const World& w, const Entity& bot, const Entity* target, uint32_t nowMs) {
    const SpellDef* s = readySpell(w, bot, SpellKind::Melee, target, nowMs);
    if (s) return s;
    s = readySpell(w, bot, SpellKind::Damage, target, nowMs);
    if (s && bot.botRole == (uint8_t)Role::Healer && bot.power * 100 < (int)bot.maxPower * 55) return nullptr;   // keep mana for heals
    return s;
}

const SpellDef* healSpell(const World& w, const Entity& bot, uint32_t nowMs) {
    const SpellDef* big = readySpell(w, bot, SpellKind::Heal, nullptr, nowMs);
    if (big) return big;
    return nullptr;
}

const Entity* lowestAliveAlly(const World& w, const Entity& except) {
    const Entity* best = nullptr;
    float bestF = 1.01f;
    auto consider = [&](const Entity* e) {
        if (!e || !e->alive() || e->guid == except.guid || !e->maxHp) return;
        const float f = (float)e->hp / (float)e->maxHp;
        if (f < bestF) { bestF = f; best = e; }
    };
    consider(w.player());
    for (int i = 0; i < kMaxParty; ++i) consider(botAt(w, i));
    return best;
}

bool healerUnderAttack(const World& w) {
    const Entity* h = healerOf(w);
    if (!h || !h->alive()) return false;
    if (h->victim) return true;
    for (int i = 0; i < kMaxEntities; ++i) {
        const Entity* e = w.byIndex(i);
        if (!e || !e->used || e->objType != 3 || !e->alive()) continue;
        if (w.threatOf(*e, h->guid) > 0) return true;
    }
    return false;
}

// Pick what this bot should be fighting. Never starts a voluntary fight while the player is out of
// combat unless the creature has already engaged the party (the "avoid unintended pulls" rule).
uint64_t chooseTarget(const World& w, const Entity& bot, uint32_t nowMs) {
    (void)nowMs;
    if (bot.victim) {
        const Entity* t = w.entity(bot.victim);
        if (t && t->alive()) return t->guid;
    }
    const bool assist = bot.tacticalMode == 1;
    if (!assist) return 0;
    if (const Entity* p = w.player()) {
        if (p->victim) {
            const Entity* t = w.entity(p->victim);
            if (t && t->alive() && w.hostileTowards(bot, *t)) return t->guid;
        }
    }
    // The tank leads; the others assist the tank's target.
    if (bot.botRole != (uint8_t)Role::Tank) {
        if (const Entity* t = tankOf(w)) {
            if (t->victim) {
                const Entity* v = w.entity(t->victim);
                if (v && v->alive() && w.hostileTowards(bot, *v)) return v->guid;
            }
        }
    }
    const bool playerInCombat = w.player() && w.player()->victim != 0;
    const float reach = bot.botRole == (uint8_t)Role::Tank ? 14.0f : 24.0f;
    uint64_t best = 0;
    float bestD = reach;
    for (int i = 0; i < kMaxEntities; ++i) {
        const Entity* e = w.byIndex(i);
        if (!e || !e->used || e->objType != 3 || !e->alive()) continue;
        if (!w.hostileTowards(bot, *e)) continue;
        const float d = dist2d(bot.x, bot.y, e->x, e->y);
        if (d >= bestD) continue;
        // Pull discipline: do not grab a fresh pack unless it is the only one nearby and the party
        // is already fighting (or it is already close to the player).
        if (!playerInCombat) {
            const Entity* p = w.player();
            if (p && w.nearbyHostileCount(*e, 9.0f) > 2 && dist2d(p->x, p->y, e->x, e->y) > 10.0f) continue;
        }
        bestD = d;
        best = e->guid;
    }
    return best;
}

}  // namespace

const char* actionName(TacticalAction a) {
    switch (a) {
        case TacticalAction::BasicAttack: return "attack";
        case TacticalAction::HighPriority: return "ability";
        case TacticalAction::Defensive: return "defensive";
        case TacticalAction::HealSupport: return "heal";
        case TacticalAction::InterruptCc: return "interrupt";
        case TacticalAction::Reposition: return "reposition";
        case TacticalAction::Retreat: return "retreat";
        case TacticalAction::Hold: return "hold";
    }
    return "?";
}

void formationSlot(const World& w, const Entity& bot, float* x, float* y) {
    const Entity* p = w.player();
    if (!p) { *x = bot.x; *y = bot.y; return; }
    const int role = bot.botRole;
    float back = 5.0f, side = 0.0f;
    if (role == (uint8_t)Role::Tank) { back = -2.0f; side = 0.0f; }          // ahead
    else if (role == (uint8_t)Role::Healer) { back = 6.5f; side = 1.5f; }
    else if (bot.botIndex == 2) { back = 5.0f; side = -2.5f; }
    else { back = 5.0f; side = 2.5f; }
    const float c = std::cos(p->o), s = std::sin(p->o);
    *x = p->x - c * back - s * side;
    *y = p->y - s * back + c * side;
}

void buildFeatures(const World& w, const Entity& bot, TacticalFeatures& f, uint32_t nowMs) {
    const Entity* target = w.entity(chooseTarget(w, bot, nowMs));
    const Entity* tank = tankOf(w);
    const Entity* healer = healerOf(w);
    const float hpF = bot.maxHp ? (float)bot.hp / (float)bot.maxHp : 0.0f;
    const float powF = bot.maxPower ? (float)bot.power / (float)bot.maxPower : 0.0f;
    uint64_t lowWho = 0;
    const float lowAlly = w.lowestPartyHealthFraction(&lowWho);
    const float targetHp = target && target->maxHp ? (float)target->hp / (float)target->maxHp : 0.0f;
    const float distT = target ? dist2d(bot.x, bot.y, target->x, target->y) : 40.0f;
    const int32_t topThreat = target ? [&] {
        int32_t t = 0;
        for (int i = 0; i < kMaxEntities; ++i) {
            const Entity* e = w.byIndex(i);
            if (!e || !e->used) continue;
            t = std::max(t, w.threatOf(*target, e->guid));
        }
        return t;
    }() : 0;
    const int32_t myThreat = target ? w.threatOf(*target, bot.guid) : 0;
    const bool interruptReady = [] {
        return true;   // refined below by legalActionMask; the feature says "a tool exists"
    }();
    const SpellDef* main = target ? mainAbility(w, bot, target, nowMs) : nullptr;
    const SpellDef* heal = healSpell(w, bot, nowMs);
    const bool enemyCasting = target && target->castSpell != 0;

    f.set(0, hpF);
    f.set(1, powF);
    f.set(2, tank && tank->maxHp ? (float)tank->hp / (float)tank->maxHp : 0.0f);
    f.set(3, lowAlly);
    f.set(4, (float)w.injuredPartyMembers(80) / 4.0f);
    f.set(5, targetHp);
    f.set(6, distT / 40.0f);
    f.set(7, topThreat > 0 ? (float)myThreat / (float)(topThreat + 1) : 0.0f);
    f.set(8, (float)w.nearbyHostileCount(bot, 12.0f) / 5.0f);
    f.set(9, (float)[&] {
        int n = 0;
        if (w.player() && w.player()->victim) ++n;
        for (int i = 0; i < kMaxParty; ++i) { const Entity* b = w.bot(i); if (b && b->alive() && b->victim) ++n; }
        return n;
    }() / 4.0f);
    f.set(10, enemyCasting ? 1.0f : 0.0f);
    f.set(11, (interruptReady && enemyCasting && readySpell(w, bot, SpellKind::Interrupt, target, nowMs)) ? 1.0f : 0.0f);
    f.set(12, (heal || readySpell(w, bot, SpellKind::Taunt, target, nowMs)) ? 1.0f : 0.0f);
    f.set(13, main ? 1.0f : 0.0f);
    f.set(14, 1.0f);   // line of sight is not simulated offline (see docs); the client still renders walls
    f.set(15, (target && distT > (bot.botRole == (uint8_t)Role::Tank ? 4.5f : 24.0f)) ? 1.0f : 0.0f);
    f.set(16, healerUnderAttack(w) ? 1.0f : 0.0f);
    f.set(17, (target && (target->victim == (healer ? healer->guid : 0) || targetHp < 0.35f)) ? 1.0f : 0.0f);
    f.set(18, 0.0f);   // crowd-control restrictions: not implemented in the first subset
    f.set(19, (float)bot.tacticalMode / 3.0f);
    f.set(20, (float)bot.botRole / 3.0f);
    f.set(21, (float)classOf(w, bot) / 11.0f);
    f.set(22, std::min(1.0f, (float)bot.incomingDamage / 100.0f));
    f.set(23, (float)bot.lastAction / 7.0f);
}

uint32_t legalActionMask(const World& w, const Entity& bot, uint32_t nowMs) {
    uint32_t m = 0;
    const Entity* target = w.entity(chooseTarget(w, bot, nowMs));
    const bool alive = bot.alive();
    if (!alive) return 1u << (uint32_t)TacticalAction::Hold;
    const bool assist = bot.tacticalMode == 1 || bot.victim != 0;
    if (target && assist) {
        m |= 1u << (uint32_t)TacticalAction::BasicAttack;
        if (mainAbility(w, bot, target, nowMs)) m |= 1u << (uint32_t)TacticalAction::HighPriority;
        if (readySpell(w, bot, SpellKind::Taunt, target, nowMs) || readySpell(w, bot, SpellKind::Buff, &bot, nowMs))
            m |= 1u << (uint32_t)TacticalAction::Defensive;
        if (target->castSpell && readySpell(w, bot, SpellKind::Interrupt, target, nowMs))
            m |= 1u << (uint32_t)TacticalAction::InterruptCc;
    }
    if (healSpell(w, bot, nowMs)) {
        const Entity* hurt = lowestAliveAlly(w, bot);
        if (hurt && hurt->maxHp && hurt->hp < (int32_t)(hurt->maxHp * 95 / 100) &&
            dist2d(bot.x, bot.y, hurt->x, hurt->y) <= 40.0f)
            m |= 1u << (uint32_t)TacticalAction::HealSupport;
    }
    m |= 1u << (uint32_t)TacticalAction::Reposition;
    if (bot.hp * 100 < (int)bot.maxHp * 35 || (bot.powerType == 0 && bot.power * 100 < (int)bot.maxPower * 10 && w.injuredPartyMembers(60) > 2))
        m |= 1u << (uint32_t)TacticalAction::Retreat;
    m |= 1u << (uint32_t)TacticalAction::Hold;
    return m;
}

TacticalAction utilityChoose(const World& w, const Entity& bot, uint32_t legalMask, uint32_t nowMs) {
    const Entity* target = w.entity(chooseTarget(w, bot, nowMs));
    const int32_t hpPct = bot.maxHp ? (int)bot.hp * 100 / (int)bot.maxHp : 0;
    const float lowAlly = w.lowestPartyHealthFraction();
    int bestScore = -1000;
    TacticalAction best = TacticalAction::Hold;
    auto consider = [&](TacticalAction a, int score) {
        if (!(legalMask & (1u << (uint32_t)a))) return;
        if (score > bestScore) { bestScore = score; best = a; }
    };
    const bool isHealer = bot.botRole == (uint8_t)Role::Healer;
    const bool isTank = bot.botRole == (uint8_t)Role::Tank;

    consider(TacticalAction::Retreat, hpPct < 25 ? 140 : 100);
    if (isHealer) {
        consider(TacticalAction::HealSupport, lowAlly < 0.35f ? 130 : lowAlly < 0.6f ? 100 : lowAlly < 0.85f ? 60 : 15);
        // A healer only spends damage mana when everyone is comfortable.
        consider(TacticalAction::HighPriority, lowAlly > 0.75f && bot.power * 100 > (int)bot.maxPower * 60 ? 35 : 0);
    } else {
        consider(TacticalAction::HighPriority, 70);
    }
    consider(TacticalAction::Defensive, isTank && healerUnderAttack(w) ? 110 : 20);
    consider(TacticalAction::InterruptCc, 95);
    if (target) {
        const int32_t top = [&] {
            int32_t t = 0;
            for (int i = 0; i < kMaxEntities; ++i) { const Entity* e = w.byIndex(i); if (e && e->used) t = std::max(t, w.threatOf(*target, e->guid)); }
            return t;
        }();
        // DPS respect the tank's threat: at 80% they hold instead of pulling aggro.
        const bool threatCapped = !isTank && !isHealer && top > 0 && w.threatOf(*target, bot.guid) * 100 > top * 80;
        if (!threatCapped) consider(TacticalAction::BasicAttack, 50);
        else consider(TacticalAction::Hold, 45);
        consider(TacticalAction::Reposition, 30);
    } else {
        consider(TacticalAction::Reposition, 40);
    }
    consider(TacticalAction::Hold, 5);
    return best;
}

namespace {

void followSlot(World& w, Entity& bot, uint32_t nowMs) {
    float fx = bot.x, fy = bot.y;
    formationSlot(w, bot, &fx, &fy);
    const float d = dist2d(bot.x, bot.y, fx, fy);
    const float dx = fx - bot.moveX, dy = fy - bot.moveY;
    const bool slotMoved = dx * dx + dy * dy > 6.25f;   // the player moved / turned enough to matter
    if (d > 2.6f && (slotMoved || !bot.moving)) w.moveEntity(bot, fx, fy, nowMs, 0.35f);
    else if (d < 1.4f) bot.moving = false;
}

void orderMoveToTarget(World& w, Entity& bot, const Entity* target, uint32_t nowMs) {
    if (!target) return;
    const float melee = bot.botRole == (uint8_t)Role::Tank || bot.botRole == (uint8_t)Role::Dps ? 4.0f : 0.0f;
    if (melee > 0) {
        if (dist2d(bot.x, bot.y, target->x, target->y) > 4.2f)
            w.moveEntity(bot, target->x, target->y, nowMs, 3.8f);
        else
            bot.moving = false;
        return;
    }
    // Ranged roles keep a comfortable distance.
    const float d = dist2d(bot.x, bot.y, target->x, target->y);
    if (d > 28.0f) {
        w.moveEntity(bot, target->x, target->y, nowMs, 24.0f);
    } else if (d < 10.0f) {
        const float dx = bot.x - target->x, dy = bot.y - target->y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.1f) w.moveEntity(bot, target->x + dx / len * 16.0f, target->y + dy / len * 16.0f, nowMs, 0.2f);
    } else {
        bot.moving = false;
    }
}

bool castAt(World& w, Entity& bot, const SpellDef* s, const Entity* target, uint32_t nowMs) {
    if (!s || !target) return false;
    if (w.tryCast(bot, s->id, target->guid, nowMs)) {
        bot.lastAction = (uint8_t)(s->kind == SpellKind::Heal ? TacticalAction::HealSupport
                                  : s->kind == SpellKind::Interrupt ? TacticalAction::InterruptCc
                                  : s->kind == SpellKind::Taunt ? TacticalAction::Defensive
                                  : TacticalAction::HighPriority);
        return true;
    }
    return false;
}

void executeAction(World& w, Entity& bot, TacticalAction action, uint32_t nowMs) {
    const Entity* target = w.entity(chooseTarget(w, bot, nowMs));
    const bool isMelee = bot.botRole == (uint8_t)Role::Tank || bot.botRole == (uint8_t)Role::Dps;
    switch (action) {
        case TacticalAction::BasicAttack:
            if (!target) { bot.moving = false; break; }
            if (isMelee) {
                if (!bot.victim) w.startAttack(bot, target->guid, nowMs);
                orderMoveToTarget(w, bot, target, nowMs);
                bot.lastAction = (uint8_t)action;
            } else {
                const SpellDef* s = readySpell(w, bot, SpellKind::Damage, target, nowMs);
                if (!castAt(w, bot, s, target, nowMs)) {
                    // No damage spell: step in and swing (a healer out of mana still defends itself).
                    if (!bot.victim) w.startAttack(bot, target->guid, nowMs);
                    orderMoveToTarget(w, bot, target, nowMs);
                } else if (bot.victim) {
                    w.stopAttack(bot, nowMs);
                }
            }
            break;
        case TacticalAction::HighPriority: {
            const SpellDef* s = mainAbility(w, bot, target, nowMs);
            if (s && target) {
                castAt(w, bot, s, target, nowMs);
                if (s->startsAutoAttack && !bot.victim) w.startAttack(bot, target->guid, nowMs);
            }
            orderMoveToTarget(w, bot, target, nowMs);
            break;
        }
        case TacticalAction::Defensive: {
            const SpellDef* taunt = readySpell(w, bot, SpellKind::Taunt, target, nowMs);
            if (taunt && target) castAt(w, bot, taunt, target, nowMs);
            else {
                const SpellDef* buff = readySpell(w, bot, SpellKind::Buff, &bot, nowMs);
                if (buff) castAt(w, bot, buff, &bot, nowMs);
            }
            bot.lastAction = (uint8_t)action;
            break;
        }
        case TacticalAction::HealSupport: {
            const SpellDef* heal = healSpell(w, bot, nowMs);
            const Entity* hurt = lowestAliveAlly(w, bot);
            if (heal && hurt && hurt->alive()) {
                if (bot.victim) w.stopAttack(bot, nowMs);
                castAt(w, bot, heal, hurt, nowMs);
            }
            break;
        }
        case TacticalAction::InterruptCc: {
            const SpellDef* kick = readySpell(w, bot, SpellKind::Interrupt, target, nowMs);
            if (kick && target) castAt(w, bot, kick, target, nowMs);
            break;
        }
        case TacticalAction::Reposition: {
            if (target) {
                orderMoveToTarget(w, bot, target, nowMs);
            } else {
                followSlot(w, bot, nowMs);
            }
            bot.lastAction = (uint8_t)action;
            break;
        }
        case TacticalAction::Retreat:
            bot.recoveringUntilMs = nowMs + 5000;
            if (bot.victim) w.stopAttack(bot, nowMs);
            bot.moving = false;
            bot.lastAction = (uint8_t)action;
            break;
        case TacticalAction::Hold:
            if (bot.victim && bot.botRole == (uint8_t)Role::Healer) w.stopAttack(bot, nowMs);
            bot.moving = false;
            bot.lastAction = (uint8_t)action;
            break;
    }
}

}  // namespace

void botThink(World& w, Entity& bot, uint32_t nowMs) {
    if (!bot.alive() || !w.data()) return;
    if (bot.tacticalMode == 0 || bot.tacticalMode == 2) {
        // Follow / stay: defend only when a creature has already engaged this bot.
        if (!bot.victim) {
            if (bot.tacticalMode == 0) {
                followSlot(w, bot, nowMs);
            } else {
                bot.moving = false;
            }
            bot.lastAction = (uint8_t)TacticalAction::Hold;
            return;
        }
    }
    if (bot.tacticalMode == 3) { bot.moving = false; bot.lastAction = (uint8_t)TacticalAction::Retreat; return; }

    const Entity* target = w.entity(chooseTarget(w, bot, nowMs));
    if (bot.victim && !target) w.stopAttack(bot, nowMs);

    TacticalFeatures features;
    buildFeatures(w, bot, features, nowMs);
    const uint32_t legal = legalActionMask(w, bot, nowMs);

    // ---- emergency rules: deterministic, always ahead of the model (never bypassed)
    TacticalAction action = TacticalAction::Hold;
    const float lowAlly = w.lowestPartyHealthFraction();
    const bool isHealer = bot.botRole == (uint8_t)Role::Healer;
    if (isHealer && lowAlly < 0.30f && (legal & (1u << (uint32_t)TacticalAction::HealSupport)))
        action = TacticalAction::HealSupport;
    else if (bot.botRole == (uint8_t)Role::Tank && healerUnderAttack(w) &&
             (legal & (1u << (uint32_t)TacticalAction::Defensive)))
        action = TacticalAction::Defensive;
    else if (bot.hp * 100 < (int)bot.maxHp * 20 && (legal & (1u << (uint32_t)TacticalAction::Retreat)))
        action = TacticalAction::Retreat;
    else {
        // ---- optional learned policy
        NeuralPolicy* nn = neuralPolicy();
        int chosen = -1;
        if (nn && nn->ready()) chosen = nn->infer(features.v);
        if (chosen >= 0 && chosen < kActionCount && (legal & (1u << (uint32_t)chosen)))
            action = (TacticalAction)chosen;
        else
            action = utilityChoose(w, bot, legal, nowMs);
    }
    executeAction(w, bot, action, nowMs);
    if (action != TacticalAction::Reposition) {
        // Keep following the group when there is nothing to fight.
        if (!target && !bot.victim) followSlot(w, bot, nowMs);
    }
}

}  // namespace offline
}  // namespace w3n
