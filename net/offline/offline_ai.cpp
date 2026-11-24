// offline_ai.cpp — WoWVita: how the creatures of the offline world behave, from the CMaNGOS classic-db data.
//
//   * movement: MovementType 1 wanders at walking speed within spawndist of the spawn point: one to three moves,
//     then a pause of 3-10 s (CMaNGOS WanderMovementGenerator); MovementType 2 walks its waypoint path
//     (creature_movement by spawn, creature_movement_template by entry) and waits WaitTime at each point,
//     round and round. A move is sent to the client once, as one spline to its end point.
//   * combat start: remembered; when the fight ends (no one left on the threat list, or EventAI says so) or the
//     creature leashes (CMaNGOS CombatManager: not hurt for Pursuit - 15 s - and its enemy more than 30 yd from where
//     it was last hurt; or farther from the start than the creature's Leash) it evades: it runs back to where the
//     fight began, cannot be hurt on the way and is whole again when it arrives.
//   * assistance: the creatures of its faction within 5 yards (or its CallForHelp) that are not fighting and are
//     hostile to the enemy come 1.5 s after it is pulled (CMaNGOS CallAssistance / CanAssistTo).
//   * EventAI (creature_ai_scripts): timers in and out of combat, health / mana / range / target health / target
//     casting / a hurt friend / a missing aura, aggro, kill, death, evade, spawn and reached-home events with the
//     actions say/yell/emote, animation, cast, phases, combat movement, ranged mode, evade, flee for help
//     (CreatureFamilyFleeAssistanceRadius 30, CreatureFamilyFleeDelay 7000) and call for help. Anything else in
//     the database was left out by the exporter (tools/offline_data/export_world_sql.py).
#include "offline_core.hpp"

#include <algorithm>
#include <cmath>

namespace w3n {
namespace offline {

namespace {
constexpr float kReach = 5.0f;              // melee reach (offline_world.cpp kMeleeReach)
// mangosd.conf defaults: LeashRadius 30, CreatureFamilyAssistanceRadius 5, CreatureFamilyAssistanceDelay 1500,
// CreatureFamilyFleeAssistanceRadius 30, CreatureFamilyFleeDelay 10000
constexpr float kLeashRadius = 30.0f;
constexpr float kAssistRadius = 5.0f;
constexpr uint32_t kAssistDelayMs = 1500;
constexpr float kFleeRadius = 30.0f;
constexpr uint32_t kFleeMs = 10000;

float d2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}
float heading(float ax, float ay, float bx, float by) { return std::atan2(by - ay, bx - ax); }
int pct(int32_t v, uint32_t max) { return max ? (int)((int64_t)std::max<int32_t>(0, v) * 100 / (int64_t)max) : 100; }
}  // namespace

// ---------------------------------------------------------------- setup
void World::initBehaviour(Entity& e, int spawnIndex, uint32_t nowMs) {
    const CreatureAi* ai = data_ ? data_->ai(e.entry) : nullptr;
    e.spawnIndex = spawnIndex;
    e.walkSpeed = ai ? ai->walkSpeed : 2.5f;
    e.legActive = false;
    e.wpIndex = 0;
    e.nextIdleMoveMs = nowMs + 500 + rng_.below(6000);   // not all at once
    e.aiCombat = false;
    e.fleeUntilMs = 0;
    e.fleeTo = 0;
    e.assistAtMs = 0;
    e.assistTarget = 0;
    e.aiPhase = 0;
    e.combatMove = true;
    e.rangedDist = 0;
    e.castPushes = 0;
    AiTimers& t = timersOf(e);
    t = AiTimers();
    if (!ai) return;
    for (int i = 0; i < (int)ai->events.size() && i < 48; ++i)
        if (ai->events[i].type == 1 || ai->events[i].type == 29) aiArm(e, i, ai->events[i], true, nowMs);
    aiEvent(e, 11, nowMs, e.summonerGuid);   // spawned (the invoker: who summoned it)
}

// ---------------------------------------------------------------- movement
void World::startLeg(Entity& e, float x, float y, float z, bool walk) {
    e.legX = x; e.legY = y; e.legZ = z;
    e.legFromZ = e.z;
    e.legLen = d2(e.x, e.y, x, y);
    e.legWalk = walk;
    e.legActive = true;
    e.moving = true;
    e.o = heading(e.x, e.y, x, y);
    ++e.moveLeg;
}

bool World::stepLeg(Entity& e, float speed, uint32_t nowMs) {
    (void)nowMs;
    const float dx = e.legX - e.x, dy = e.legY - e.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    const float step = speed * ((float)kSimStepMs / 1000.0f);
    if (d <= step || d < 0.05f) {
        e.x = e.legX; e.y = e.legY; e.z = e.legZ;
        applyGround(e);
        return true;
    }
    e.x += dx / d * step;
    e.y += dy / d * step;
    // the path's own heights (bridges, buildings: the ground query knows only the terrain), the ground on a wander
    if (e.legLen > 0.01f) e.z = e.legZ + (e.legFromZ - e.legZ) * (d - step) / e.legLen;
    const SpawnMove* sm = data_ ? data_->spawnMove(e.spawnIndex) : nullptr;
    if (!sm || sm->type != 2) applyGround(e);
    return false;
}

void World::tickIdleMovement(Entity& e, uint32_t nowMs) {
    if (followMaster(e, nowMs)) return;   // a linked creature walks with its master
    const SpawnMove* sm = data_ ? data_->spawnMove(e.spawnIndex) : nullptr;
    if (e.legActive) {
        if (!stepLeg(e, e.legWalk ? e.walkSpeed : e.speed, nowMs)) return;
        e.legActive = false;
        e.moving = false;
        if (sm && sm->type == 2) {
            const std::vector<MovePoint>& pts = data_->paths[sm->path];
            const MovePoint& p = pts[e.wpIndex % pts.size()];
            if (p.o < 7.0f) e.o = p.o;
            e.nextIdleMoveMs = nowMs + p.waitMs;
            e.wpIndex = (uint16_t)((e.wpIndex + 1) % pts.size());
        } else if (sm && sm->type == 1) {
            // one to three moves in a row, then a pause (wpIndex counts the moves left: a wanderer has no path)
            if (e.wpIndex > 1) { --e.wpIndex; e.nextIdleMoveMs = nowMs; }
            else { e.wpIndex = (uint16_t)(1 + rng_.below(3)); e.nextIdleMoveMs = nowMs + 3000 + rng_.below(7001); }
        } else {
            e.o = e.homeO;   // back at its place after a fight
        }
        return;
    }
    if (nowMs < e.nextIdleMoveMs) return;
    if (sm && sm->type == 2) {
        const std::vector<MovePoint>& pts = data_->paths[sm->path];
        const MovePoint& p = pts[e.wpIndex % pts.size()];
        if (d2(e.x, e.y, p.x, p.y) < 0.3f) {   // (already there: the next one)
            e.wpIndex = (uint16_t)((e.wpIndex + 1) % pts.size());
            return;
        }
        startLeg(e, p.x, p.y, p.z, true);
    } else if (sm && sm->type == 1) {
        const float a = (float)rng_.below(6283) / 1000.0f;
        const float r = sm->wander * std::sqrt((float)rng_.below(1001) / 1000.0f);
        if (r < 0.5f) { e.nextIdleMoveMs = nowMs + 1000; return; }
        startLeg(e, e.homeX + std::cos(a) * r, e.homeY + std::sin(a) * r, e.homeZ, true);
    } else if (d2(e.x, e.y, e.homeX, e.homeY) > 0.5f) {
        startLeg(e, e.homeX, e.homeY, e.homeZ, true);
    } else {
        e.nextIdleMoveMs = nowMs + 5000;
    }
}

// ---------------------------------------------------------------- combat start / end
void World::enterCombat(Entity& e, uint32_t nowMs) {
    e.aiCombat = true;
    e.combatX = e.x; e.combatY = e.y; e.combatZ = e.z;
    e.refreshMs = nowMs; e.refreshX = e.x; e.refreshY = e.y;
    e.legActive = false;
    e.assistAtMs = 0;
    e.assistTarget = 0;
    e.castPushes = 0;
    const CreatureAi* ai = data_ ? data_->ai(e.entry) : nullptr;
    callAssistance(e, ai && ai->callForHelp > 0 ? ai->callForHelp : kAssistRadius, nowMs, kAssistDelayMs);
    if (!ai) return;
    AiTimers& t = timersOf(e);
    for (int i = 0; i < (int)ai->events.size() && i < 48; ++i) {
        const AiEvent& ev = ai->events[i];
        if (ev.type == 1) continue;
        t.done &= ~(1ull << i);
        if (ev.type == 0) aiArm(e, i, ev, true, nowMs);
        else t.next[i] = 0;
    }
    aiEvent(e, 4, nowMs, e.victim);   // aggro
    linkedAggro(e, nowMs);
}

void World::startEvade(Entity& e, uint32_t nowMs) {
    const uint64_t was = e.victim;
    if (e.castSpell) cancelCast(e, 35, nowMs);
    e.victim = e.target = 0;
    e.threatCount = 0;
    e.auraCount = 0;
    e.aiCombat = false;
    e.fleeUntilMs = 0;
    e.fleeTo = 0;
    e.assistAtMs = 0;
    e.rangedDist = 0;
    e.combatMove = true;
    e.aiState = 3;
    e.legActive = false;
    if (was) emit(SimEvent::AttackStopped, e.guid, was, 0, 0);
    emit(SimEvent::ThreatChanged, e.guid, 0, 0, 0);
    aiEvent(e, 7, nowMs);   // evade
    linkedEvade(e, nowMs);
}

bool World::tickEvade(Entity& e, uint32_t nowMs) {
    if (d2(e.x, e.y, e.combatX, e.combatY) > 0.6f) {
        moveToward(e, e.combatX, e.combatY, nowMs, 0.5f);
        return true;
    }
    e.moving = false;
    e.hp = (int32_t)e.maxHp;
    e.power = (int32_t)e.maxPower;
    e.aiState = 0;
    e.nextIdleMoveMs = nowMs + 1000 + rng_.below(2000);
    const AiTimers& t = timersOf(e);
    (void)t;
    aiEvent(e, 21, nowMs);   // reached home
    return false;
}

// ---------------------------------------------------------------- help
void World::callAssistance(Entity& e, float radius, uint32_t nowMs, uint32_t delayMs) {
    Entity* enemy = entity(e.victim);
    if (!enemy || !enemy->alive()) return;
    for (Entity& c : pool_) {
        if (!c.used || c.objType != 3 || &c == &e || !c.alive() || c.victim || c.aiState == 3) continue;
        if (c.faction != e.faction || d2(c.x, c.y, e.x, e.y) > radius) continue;
        const CreatureAi* ai = data_ ? data_->ai(c.entry) : nullptr;
        if (ai && ai->civilian) continue;
        if (!hostileTowards(c, *enemy)) continue;   // (a neutral one does not help)
        if (delayMs) {   // CMaNGOS AssistDelayEvent: the ones near it now come a moment later
            c.assistTarget = enemy->guid;
            c.assistAtMs = nowMs + delayMs;
            continue;
        }
        c.victim = c.target = enemy->guid;
        c.aiState = 1;
        addThreat(c, enemy->guid, 1);
        c.lastDamageMs = nowMs;
    }
}

bool World::tickFlee(Entity& e, uint32_t nowMs) {
    if (nowMs >= e.fleeUntilMs) { e.fleeUntilMs = 0; e.fleeTo = 0; return false; }
    Entity* friendE = e.fleeTo ? entity(e.fleeTo) : nullptr;
    if (friendE && friendE->alive() && !friendE->victim) {
        if (d2(e.x, e.y, friendE->x, friendE->y) <= 3.0f) {
            callAssistance(e, 6.0f, nowMs);       // the friend (and whoever stands with it) joins in
            e.fleeUntilMs = 0;
            e.fleeTo = 0;
            return false;
        }
        moveToward(e, friendE->x, friendE->y, nowMs, 2.5f);
        return true;
    }
    e.fleeTo = 0;
    // nobody to run to: away from the enemy
    if (Entity* t = entity(e.victim)) {
        const float dx = e.x - t->x, dy = e.y - t->y;
        const float d = std::max(0.1f, std::sqrt(dx * dx + dy * dy));
        moveToward(e, e.x + dx / d * 4.0f, e.y + dy / d * 4.0f, nowMs, 0.0f);
    }
    return true;
}

// ---------------------------------------------------------------- EventAI
bool World::aiCanRun(const Entity& e, const AiEvent& ev) const {
    return !(ev.phaseMask & (1u << (e.aiPhase & 31)));
}

void World::aiArm(Entity& e, int index, const AiEvent& ev, bool initial, uint32_t nowMs) {
    AiTimers& t = timersOf(e);
    int lo, hi;
    if (ev.type == 0 || ev.type == 1 || ev.type == 29) {   // (29: EVENT_T_TIMER_GENERIC, in and out of combat)
        lo = initial ? ev.p[0] : ev.p[2];
        hi = initial ? ev.p[1] : ev.p[3];
        if (!initial && !lo && !hi) { t.done |= 1ull << index; return; }
    } else if (ev.type == 5 || ev.type == 13) {
        lo = ev.p[0]; hi = ev.p[1];
    } else {
        lo = ev.p[2]; hi = ev.p[3];
    }
    if (!initial && !(ev.flags & 1) && ev.type != 0 && ev.type != 1) { t.done |= 1ull << index; return; }
    if (!initial && !lo && !hi && ev.type != 0 && ev.type != 1) { t.done |= 1ull << index; return; }
    t.next[index] = nowMs + (uint32_t)std::max(0, rng_.range(std::max(0, lo), std::max(lo, hi)));
}

void World::aiTick(Entity& e, uint32_t nowMs) {
    const CreatureAi* ai = data_ ? data_->ai(e.entry) : nullptr;
    if (!ai || ai->events.empty() || !e.alive()) return;
    AiTimers& t = timersOf(e);
    Entity* vic = e.victim ? entity(e.victim) : nullptr;
    for (int i = 0; i < (int)ai->events.size() && i < 48; ++i) {
        const AiEvent& ev = ai->events[i];
        if ((t.done >> i) & 1) continue;
        if (t.next[i] > nowMs) continue;
        if (!aiCanRun(e, ev)) continue;
        bool fire = false;
        uint64_t other = 0;
        switch (ev.type) {
            case 0: fire = e.aiCombat; break;
            case 1: fire = !e.aiCombat && e.aiState != 3; break;
            case 2: { const int h = pct(e.hp, e.maxHp); fire = e.aiCombat && h <= ev.p[0] && h >= ev.p[1]; break; }
            case 3: { const int m = pct(e.power, e.maxPower); fire = e.aiCombat && e.maxPower && m <= ev.p[0] && m >= ev.p[1]; break; }
            case 9: if (e.aiCombat && vic) { const float d = d2(e.x, e.y, vic->x, vic->y); fire = d >= ev.p[0] && d <= ev.p[1]; } break;
            case 12: if (e.aiCombat && vic) { const int h = pct(vic->hp, vic->maxHp); fire = h <= ev.p[0] && h >= ev.p[1]; } break;
            case 13: fire = e.aiCombat && vic && vic->castSpell; break;
            case 14:
                for (Entity& c : pool_) {
                    if (!c.used || c.objType != 3 || !c.alive() || c.faction != e.faction) continue;
                    if ((int32_t)c.maxHp - c.hp < ev.p[0] || d2(c.x, c.y, e.x, e.y) > (float)ev.p[1]) continue;
                    fire = true; other = c.guid; break;
                }
                break;
            case 27: fire = !hasAura(e, (uint32_t)ev.p[0]); other = e.guid; break;
            case 29: fire = e.aiState != 3; other = e.summonerGuid; break;   // EVENT_T_TIMER_GENERIC
            default: continue;   // the ones that happen (aggro, death, ...) run from aiEvent
        }
        if (fire && (ev.flags & 0x300) && vic) {   // ranged / melee mode only
            const bool melee = inMeleeRange(e, *vic) || e.rangedDist <= 0;
            if (((ev.flags & 0x100) && melee) || ((ev.flags & 0x200) && !melee)) fire = false;
        }
        if (!fire) continue;
        bool ok = true;
        if ((int)rng_.below(100) < ev.chance) ok = aiRun(e, i, ev, nowMs, other);
        if (!e.used || !e.alive()) return;
        if (!ok && (ev.flags & 0x400)) { t.next[i] = nowMs + 500; continue; }   // a combat action that could not run: soon again
        aiArm(e, i, ev, false, nowMs);
    }
}

void World::aiEvent(Entity& e, uint8_t type, uint32_t nowMs, uint64_t other) {
    const CreatureAi* ai = data_ ? data_->ai(e.entry) : nullptr;
    if (!ai) return;
    AiTimers& t = timersOf(e);
    for (int i = 0; i < (int)ai->events.size() && i < 48; ++i) {
        const AiEvent& ev = ai->events[i];
        if (ev.type != type || !aiCanRun(e, ev)) continue;
        if (type == 5 && t.next[i] > nowMs) continue;   // kill: with its own repeat time
        if ((int)rng_.below(100) >= ev.chance) continue;
        aiRun(e, i, ev, nowMs, other);
        if (type == 5) t.next[i] = nowMs + (uint32_t)std::max(0, rng_.range(ev.p[0], std::max(ev.p[0], ev.p[1])));
        if (!e.used) return;
    }
}

bool World::aiRun(Entity& e, int index, const AiEvent& ev, uint32_t nowMs, uint64_t other) {
    (void)index;
    if (ev.flags & 0x20) {   // one random action of those set
        int n = 0, pick[3];
        for (int k = 0; k < 3; ++k) if (ev.act[k].type) pick[n++] = k;
        return n ? aiAction(e, ev.act[pick[rng_.below((uint32_t)n)]], nowMs, other) : true;
    }
    bool ok = true;
    for (const AiAction& a : ev.act)
        if (a.type && !aiAction(e, a, nowMs, other)) ok = false;
    return ok;
}

bool World::aiAction(Entity& e, const AiAction& a, uint32_t nowMs, uint64_t other) {
    auto randomOf = [&](const int32_t* p) -> int32_t {
        int n = 0;
        int32_t v[3];
        for (int k = 0; k < 3; ++k) if (p[k]) v[n++] = p[k];
        return n ? v[rng_.below((uint32_t)n)] : 0;
    };
    switch (a.type) {
        case 2:     // ACTION_T_SET_FACTION: p1 the faction template (0: its own again)
            if (const CreatureTemplate* t = data_ ? data_->creature(e.entry) : nullptr) e.faction = a.p[0] ? (uint32_t)a.p[0] : t->faction;
            emit(SimEvent::Message, e.guid, e.guid, 0, 3);   // (the client's faction field)
            break;
        case 55: {  // ACTION_T_ATTACK_START: p1 the target (11 TARGET_T_SPAWNER, 6 the invoker, 1 the victim)
            uint64_t t = a.p[0] == 11 ? e.summonerGuid : a.p[0] == 6 ? other : e.victim;
            Entity* v = t ? entity(t) : nullptr;
            if (!v || !v->alive() || !canAttack(e, *v)) break;
            e.victim = e.target = v->guid;
            e.aiState = 1;
            addThreat(e, v->guid, 1);
            if (!e.aiCombat) enterCombat(e, nowMs);
            break;
        }
        case 1: {   // text
            const int32_t id = randomOf(a.p);
            if (id && data_ && data_->aiTexts.count(id)) emit(SimEvent::CreatureText, e.guid, e.victim ? e.victim : other, 0, id);
            break;
        }
        case 5:     // emote
            if (a.p[0]) emit(SimEvent::CreatureEmote, e.guid, 0, 0, a.p[0]);
            break;
        case 10: {  // random emote (-1: none)
            const int32_t id = randomOf(a.p);
            if (id > 0) emit(SimEvent::CreatureEmote, e.guid, 0, 0, id);
            break;
        }
        case 11: {  // cast: p1 spell, p2 target (0 self, 1 victim, 2-5 others on the threat list, 6 the invoker), p3 flags
            const SpellDef* s = data_ ? data_->spell((uint32_t)a.p[0]) : nullptr;
            if (!s || !e.alive()) break;
            uint64_t target = e.victim;
            if (a.p[1] == 0 || (a.p[2] & 0x10)) target = e.guid;
            else if (a.p[1] == 6 && other) target = other;
            else if ((a.p[1] == 4 || a.p[1] == 5) && e.threatCount > 1) {
                const uint8_t k = (uint8_t)rng_.below(e.threatCount);
                if (entity(e.threat[k].who)) target = e.threat[k].who;
            }
            if (s->friendly || s->kind == SpellKind::Buff || s->kind == SpellKind::Heal) {
                if (other && entity(other) && entity(other)->objType == 3) target = other;
                else target = e.guid;
            }
            if (!target) break;
            if ((a.p[2] & 0x20) && entity(target) && hasAura(*entity(target), s->id)) break;   // aura not present only
            if (e.castSpell) {
                if (!(a.p[2] & 0x01)) return false;   // busy (CAST_INTERRUPT_PREVIOUS not set): a combat action waits
                cancelCast(e, 35, nowMs);
            }
            e.gcdEndMs = 0;   // EventAI keeps its own timers
            return tryCast(e, s->id, target, nowMs) || !e.aiCombat;
        }
        case 21: e.combatMove = a.p[0] != 0; break;
        case 22: e.aiPhase = (uint8_t)std::max(0, std::min(31, a.p[0])); break;
        case 23: e.aiPhase = (uint8_t)std::max(0, std::min(31, (int)e.aiPhase + a.p[0])); break;
        case 24: if (e.aiCombat) startEvade(e, nowMs); break;
        case 25: {  // flee for assistance
            if (!e.aiCombat || e.fleeUntilMs) break;
            if (e.castSpell) cancelCast(e, 35, nowMs);
            Entity* best = nullptr;
            float bestD = kFleeRadius;
            for (Entity& c : pool_) {
                if (!c.used || c.objType != 3 || &c == &e || !c.alive() || c.victim || c.faction != e.faction) continue;
                const CreatureAi* ai = data_->ai(c.entry);
                if (ai && ai->civilian) continue;
                const float d = d2(c.x, c.y, e.x, e.y);
                if (d < bestD) { bestD = d; best = &c; }
            }
            e.fleeTo = best ? best->guid : 0;
            e.fleeUntilMs = nowMs + kFleeMs;
            break;
        }
        case 30: e.aiPhase = (uint8_t)std::max(0, std::min(31, randomOf(a.p))); break;
        case 31: e.aiPhase = (uint8_t)std::max(0, std::min(31, rng_.range(a.p[0], std::max(a.p[0], a.p[1])))); break;
        case 39: callAssistance(e, a.p[0] > 0 ? (float)a.p[0] : kAssistRadius, nowMs); break;
        case 57: e.rangedDist = a.p[1] > 0 ? std::min(40.0f, (float)a.p[1]) : 0.0f; break;
        default: break;
    }
    return true;
}

// ---------------------------------------------------------------- control auras (SPFX)
void World::applyControl(Entity& t, const SpellControl& c, uint32_t nowMs) {
    const uint32_t until = nowMs + c.durationMs;
    bool breaksCast = false;
    for (const auto& [type, value] : c.effects) {
        switch ((Control)type) {
            case Control::Stun: t.stunnedUntilMs = std::max(t.stunnedUntilMs, until); breaksCast = true; break;
            case Control::Root: t.rootUntilMs = std::max(t.rootUntilMs, until); break;
            case Control::Slow:
                if (value < 0 && (t.slowUntilMs <= nowMs || value < t.slowPct)) t.slowPct = std::max(-95, value);   // the strongest
                if (value < 0) t.slowUntilMs = std::max(t.slowUntilMs, until);
                break;
            case Control::Fear:
            case Control::Confuse:
                if (t.objType == 3) { t.fearUntilMs = std::max(t.fearUntilMs, until); t.nextFearMoveMs = 0; }
                else t.stunnedUntilMs = std::max(t.stunnedUntilMs, until);   // a player: loses control (no server-driven run yet)
                breaksCast = true;
                break;
            case Control::Silence: t.silenceUntilMs = std::max(t.silenceUntilMs, until); breaksCast = true; break;
            case Control::Pacify: t.pacifyUntilMs = std::max(t.pacifyUntilMs, until); break;
            case Control::PacifySilence:
                t.pacifyUntilMs = std::max(t.pacifyUntilMs, until);
                t.silenceUntilMs = std::max(t.silenceUntilMs, until);
                breaksCast = true;
                break;
        }
    }
    if (breaksCast && t.castSpell) cancelCast(t, 35, nowMs);
    if (t.stunned(nowMs) || t.rooted(nowMs)) { t.moving = false; t.legActive = false; }
}

// ---------------------------------------------------------------- pools and linked creatures
void World::choosePools() {
    poolChosen_.clear();
    if (!data_ || data_->pools.empty()) return;
    poolChosen_.assign(data_->spawns.size(), 1);
    for (const SpawnPool& p : data_->pools) {
        std::vector<uint32_t> m;
        for (uint32_t i : p.members) if (i < poolChosen_.size()) { poolChosen_[i] = 0; m.push_back(i); }
        for (uint16_t k = 0; k < p.limit && !m.empty(); ++k) {   // 'limit' of them, at random
            const uint32_t at = rng_.below((uint32_t)m.size());
            poolChosen_[m[at]] = 1;
            m.erase(m.begin() + at);
        }
    }
}

Entity* World::spawnEntity(uint32_t idx) {
    if (!data_ || idx >= data_->spawns.size()) return nullptr;
    return entity(0xF130000000000000ull | ((uint64_t)(data_->spawns[idx].entry & 0xFFFFFF) << 24) | (uint64_t)(idx + 1));
}

void World::releaseCreature(Entity& e, uint32_t nowMs) {   // back to the spawn list (like a far creature)
    (void)nowMs;
    const uint32_t idx = (uint32_t)(e.guid & 0xFFFFFF);
    if (idx >= 1 && idx <= spawnActive_.size()) spawnActive_[idx - 1] = 0;
    emit(SimEvent::Vanish, e.guid, 0, 0, 0);
    for (Entity& o : pool_)
        if (o.used) {
            if (o.victim == e.guid) o.victim = 0;
            if (o.target == e.guid) o.target = 0;
        }
    e = Entity();
}

void World::linkedAggro(Entity& e, uint32_t nowMs) {
    if (!data_ || e.spawnIndex < 0 || !e.victim) return;
    Entity* enemy = entity(e.victim);
    if (!enemy) return;
    auto join = [&](Entity* c) {
        if (!c || !c->alive() || c->victim || c->aiState == 3 || !canAttack(*c, *enemy)) return;
        c->victim = c->target = enemy->guid;
        c->aiState = 1;
        addThreat(*c, enemy->guid, 1);
        c->lastDamageMs = nowMs;
    };
    auto range = data_->slavesOf.equal_range((uint32_t)e.spawnIndex);   // FLAG_AGGRO_ON_AGGRO: its slaves come
    for (auto it = range.first; it != range.second; ++it)
        if (const SpawnLink* l = data_->link((int)it->second))
            if (l->flags & 0x0001) join(spawnEntity(it->second));
    if (const SpawnLink* l = data_->link(e.spawnIndex))                // FLAG_TO_AGGRO_ON_AGGRO: and its master
        if (l->flags & 0x0002) join(spawnEntity(l->master));
}

void World::linkedEvade(Entity& e, uint32_t nowMs) {
    if (!data_ || e.spawnIndex < 0) return;
    auto range = data_->slavesOf.equal_range((uint32_t)e.spawnIndex);   // FLAG_EVADE_ON_EVADE
    for (auto it = range.first; it != range.second; ++it) {
        const SpawnLink* l = data_->link((int)it->second);
        Entity* c = spawnEntity(it->second);
        if (l && (l->flags & 0x4000) && c && c->alive() && c->aiCombat) startEvade(*c, nowMs);
    }
}

bool World::followMaster(Entity& e, uint32_t nowMs) {   // FLAG_FOLLOW: keeps its place beside the master
    const SpawnLink* l = data_ && e.spawnIndex >= 0 ? data_->link(e.spawnIndex) : nullptr;
    if (!l || !(l->flags & 0x0200)) return false;
    Entity* m = spawnEntity(l->master);
    if (!m || !m->alive() || m->victim || m->aiState == 3) return false;
    const Spawn& ss = data_->spawns[(uint32_t)e.spawnIndex];
    const Spawn& ms = data_->spawns[l->master];
    const float ox = ss.x - ms.x, oy = ss.y - ms.y;
    const float tx = (m->legActive ? m->legX : m->x) + ox, ty = (m->legActive ? m->legY : m->y) + oy;
    const float tz = m->legActive ? m->legZ : m->z;
    const bool walk = m->legActive ? m->legWalk : true;
    if (e.legActive) {
        if (d2(e.legX, e.legY, tx, ty) > 0.5f) startLeg(e, tx, ty, tz, walk);
        const float sp = (walk ? m->walkSpeed : m->speed) * 1.1f;
        if (stepLeg(e, sp, nowMs)) { e.legActive = false; e.moving = false; e.o = m->o; }
        return true;
    }
    if (d2(e.x, e.y, tx, ty) > 0.7f) startLeg(e, tx, ty, tz, walk);
    return true;
}

// ---------------------------------------------------------------- the creature's step
void World::tickBehaviour(Entity& e, uint32_t nowMs) {
    const CreatureAi* ai = data_->ai(e.entry);
    if (e.stunned(nowMs)) { e.moving = false; return; }   // stunned: nothing at all
    if (e.feared(nowMs)) {                                // feared / confused: runs about, does nothing else
        if (nowMs >= e.nextFearMoveMs || !e.moving) {
            const float a = (float)rng_.below(6283) / 1000.0f;
            e.moveX = e.x + std::cos(a) * 8.0f;
            e.moveY = e.y + std::sin(a) * 8.0f;
            e.nextFearMoveMs = nowMs + 1500;
        }
        moveToward(e, e.moveX, e.moveY, nowMs, 0.3f);
        return;
    }
    aiTick(e, nowMs);
    if (!e.used || !e.alive()) return;
    if (e.aiState == 3) { tickEvade(e, nowMs); return; }

    if (e.victim) {
        Entity* t = entity(e.victim);
        if (!t || !t->alive() || t->untargetable) {
            // the next one on the threat list, if any
            uint8_t out = 0;
            for (uint8_t i = 0; i < e.threatCount; ++i) {
                const Entity* w = entity(e.threat[i].who);
                if (w && w->alive() && !w->untargetable && w->guid != e.victim) e.threat[out++] = e.threat[i];
            }
            e.threatCount = out;
            e.victim = e.target = highestThreatTarget(e);
        }
    }
    if (e.victim && e.aiCombat) {   // leashing
        Entity* t = entity(e.victim);
        const uint32_t pursuit = ai ? ai->pursuitMs : 15000;
        const bool tooLong = nowMs - e.refreshMs > pursuit && t && d2(t->x, t->y, e.refreshX, e.refreshY) > kLeashRadius;
        const bool tooFar = ai && ai->leash > 0 && d2(e.x, e.y, e.combatX, e.combatY) > ai->leash;
        if (tooLong || tooFar) { startEvade(e, nowMs); return; }
    }
    if (!e.victim && e.assistTarget && nowMs >= e.assistAtMs) {   // called for help a moment ago
        Entity* enemy = entity(e.assistTarget);
        if (enemy && enemy->alive() && !enemy->untargetable && hostileTowards(e, *enemy)) {
            e.victim = e.target = enemy->guid;
            addThreat(e, enemy->guid, 1);
            e.lastDamageMs = nowMs;
        }
        e.assistTarget = 0;
        e.assistAtMs = 0;
    }
    if (!e.victim && !(ai && ai->civilian)) selectCreatureTarget(e, nowMs);
    if (!e.victim) {
        if (e.aiCombat) { startEvade(e, nowMs); return; }
        tickIdleMovement(e, nowMs);
        return;
    }
    if (!e.aiCombat) enterCombat(e, nowMs);
    if (!e.used || !e.alive() || !e.victim) return;
    if (e.fleeUntilMs && tickFlee(e, nowMs)) return;
    Entity* t = entity(e.victim);
    if (!t) return;
    e.o = heading(e.x, e.y, t->x, t->y);
    if (e.castSpell) { e.moving = false; return; }   // stands while it casts
    if (!e.combatMove) { e.moving = false; return; }
    const float dist = d2(e.x, e.y, t->x, t->y);
    if (e.rangedDist > 0 && dist <= e.rangedDist) { e.moving = false; return; }
    if (!inMeleeRange(e, *t)) {
        moveToward(e, t->x, t->y, nowMs, e.rangedDist > 0 ? e.rangedDist * 0.9f : kReach * 0.9f);
        return;
    }
    e.moving = false;
    // creatures without EventAI keep the sample's filler abilities
    if ((!ai || ai->events.empty()) && e.nextAbilityMs && nowMs >= e.nextAbilityMs && e.spellCount && !e.castSpell) {
        const uint32_t sp = e.spells[(uint32_t)rng_.below(e.spellCount)];
        if (const SpellDef* sd = data_->spell(sp))
            if (sd->kind == SpellKind::Damage || sd->kind == SpellKind::Dot || sd->kind == SpellKind::Melee) tryCast(e, sp, t->guid, nowMs);
        e.nextAbilityMs = nowMs + 6000 + rng_.below(4000);
    }
}

}  // namespace offline
}  // namespace w3n
