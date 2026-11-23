// offline_spells.cpp — WoWVita: the player's class spells, run from spell_template (the pack's SPX2 section).
//
// The rules are those of CMaNGOS / vMaNGOS for 1.12 where the offline world has what they need:
//   * a cast: known spell, not passive, no cast already, the global cooldown (StartRecoveryTime) and the spell's
//     own / category cooldowns, the stance (Stances / StancesNot against the shapeshift form), the power (ManaCost,
//     + ManaCostPerlevel, or ManaCostPercentage of the base; rage in tenths as the client shows it), reagents,
//     a weapon / shield it needs (EquippedItemClass + subclass mask), combo points for finishers, the target
//     (an enemy, a friend, none) and the range; cast time, then the effects; channels keep going until their end
//   * values: BasePoints + RealPointsPerLevel * (level - SpellLevel, up to MaxLevel) + BaseDice..DieSides
//     (+ DicePerLevel), + PointsPerComboPoint * combo points; durations: SpellDuration base + per level, up to max
//   * targets (ImplicitTargetA / B): the caster, the target (enemy / friend / any), enemies around the caster,
//     the target or the ground point (radius), the party around the caster, a cone in front, chained targets
//   * effects: school damage, weapon damage (+ bonus, % - one strike for all of a spell's weapon effects), heal,
//     auras, energize, create item (conjured food / water), trigger spell, charge, combo points, interrupt,
//     dispel, threat, taunt, power burn, health leech; "on next swing" (Heroic Strike); persistent area auras
//     (Blizzard, Consecration); channelled spells
//   * auras: periodic damage / heal / leech / trigger, the control auras (stun, root, slow, fear, silence ...),
//     armour (resistance), speed, absorb shields, damage shields (Thorns), damage done / taken %, attack power,
//     shapeshift (stances, forms); anything else is shown on its target for its duration
//   * rage: from damage dealt and taken (7.5 x and 2.5 x damage / the level's rage conversion)
// What the offline world has no data for (stats, spell power, talents' modifiers, pets, totems) is left out.
#include "offline_core.hpp"

#include <algorithm>
#include <cmath>

namespace w3n {
namespace offline {

namespace {
constexpr float kReach = 5.0f;
constexpr uint32_t kSlotCategoryBit = 0x80000000u;   // a category cooldown kept in the cooldown list

float dist(const Entity& a, const Entity& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// SPELL_EFFECT_* (1.12)
enum : uint8_t {
    E_INSTAKILL = 1, E_SCHOOL_DAMAGE = 2, E_DUMMY = 3, E_TELEPORT = 5, E_APPLY_AURA = 6, E_POWER_DRAIN = 8,
    E_HEALTH_LEECH = 9, E_HEAL = 10, E_WEAPON_PCT = 31, E_WEAPON_NOSCHOOL = 17, E_ENERGIZE = 30, E_CREATE_ITEM = 24,
    E_PERSISTENT_AURA = 27, E_AREA_AURA = 35, E_LEARN_SPELL = 36, E_DISPEL = 38, E_WEAPON_DAMAGE = 58,
    E_POWER_BURN = 62, E_THREAT = 63, E_TRIGGER = 64, E_HEAL_MAX = 67, E_INTERRUPT = 68, E_COMBO = 80,
    E_CHARGE = 96, E_ATTACK_ME = 114, E_NORMALIZED_WEAPON = 121,
};
// SPELL_AURA_* (1.12)
enum : uint8_t {
    A_PERIODIC_DAMAGE = 3, A_CONFUSE = 5, A_FEAR = 7, A_PERIODIC_HEAL = 8, A_STUN = 12, A_DMG_DONE = 13,
    A_DAMAGE_SHIELD = 15, A_RESISTANCE = 22, A_PERIODIC_TRIGGER = 23, A_PACIFY = 25, A_ROOT = 26, A_SILENCE = 27,
    A_INC_SPEED = 31, A_DEC_SPEED = 33, A_SHAPESHIFT = 36, A_SCHOOL_ABSORB = 69, A_PERIODIC_LEECH = 53,
    A_PACIFY_SILENCE = 60, A_DMG_PCT_DONE = 79, A_DMG_PCT_TAKEN = 87, A_ATTACK_POWER = 99, A_MOUNTED_SPEED = 32,
};
bool weaponEffect(uint8_t e) { return e == E_WEAPON_NOSCHOOL || e == E_WEAPON_DAMAGE || e == E_NORMALIZED_WEAPON || e == E_WEAPON_PCT; }
bool areaEnemy(uint8_t t) { return t == 15 || t == 16 || t == 24 || t == 28; }
bool partyTarget(uint8_t t) { return t == 20 || t == 30 || t == 31 || t == 33 || t == 37; }
// the target of the cast: 1 enemy, 2 friend, 0 none
int unitTarget(const SpellX& s) {
    int need = 0;
    for (const SpellEffectX& e : s.eff) {
        if (!e.effect) continue;
        if (e.targetA == 6 || e.targetA == 25 || (e.targetA == 0 && (e.effect == E_SCHOOL_DAMAGE || weaponEffect(e.effect)))) need = 1;
        else if ((e.targetA == 21 || e.targetA == 57) && need == 0) need = 2;
    }
    return need;
}
}  // namespace

// ---------------------------------------------------------------- values
int32_t World::effValue(const Entity& c, const SpellX& s, int k, int combo) {
    const SpellEffectX& e = s.eff[k];
    int lvl = c.level;
    if (s.maxLevel && lvl > s.maxLevel) lvl = s.maxLevel;
    const int over = std::max(0, lvl - (int)s.spellLevel);
    int32_t v = e.base + (int32_t)(e.perLevel * (float)over);
    const int32_t dice = e.die + (int32_t)(e.dicePerLevel * (float)over);
    const int32_t lo = std::max(1, e.baseDice);
    if (dice >= lo) v += rng_.range(lo, dice);
    else if (dice > 0) v += dice;
    else v += 1;   // (BaseDice 1, DieSides 0: the base and one)
    if (combo > 0 && e.perCombo != 0) v += (int32_t)(e.perCombo * (float)combo);
    return v;
}

uint32_t World::durationX(const Entity& c, const SpellX& s) const {
    if (s.durBase < 0) return 0;   // until cancelled
    int32_t d = s.durBase;
    if (s.durPerLevel) {
        const int over = std::max(0, (int)c.level - (int)s.spellLevel);
        d += s.durPerLevel * over;
        if (s.durMax > 0) d = std::min(d, s.durMax);
    }
    return (uint32_t)std::max(0, d);
}

bool World::positiveX(const SpellX& s) const {
    for (const SpellEffectX& e : s.eff) {
        if (!e.effect) continue;
        if (e.effect == E_SCHOOL_DAMAGE || weaponEffect(e.effect) || e.effect == E_INTERRUPT || e.effect == E_POWER_BURN ||
            e.effect == E_HEALTH_LEECH || e.effect == E_POWER_DRAIN)
            return false;
        if (e.targetA == 6 || areaEnemy(e.targetA) || areaEnemy(e.targetB)) return false;
        if (e.aura == A_PERIODIC_DAMAGE || e.aura == A_STUN || e.aura == A_ROOT || e.aura == A_FEAR || e.aura == A_CONFUSE ||
            e.aura == A_DEC_SPEED || e.aura == A_SILENCE || e.aura == A_PACIFY || e.aura == A_PERIODIC_LEECH ||
            e.aura == A_DMG_PCT_TAKEN)
            return false;
    }
    return true;
}

uint32_t World::powerCostX(const Entity& e, const SpellX& s) const {
    uint32_t cost = s.manaCost + s.manaPerLevel * (uint32_t)std::max<int>(0, (int)e.level - (int)s.spellLevel);
    if (s.manaPct) cost += e.maxPower * s.manaPct / 100;
    if (s.powerType == 1) cost = (cost + 5) / 10;   // rage: the database keeps tenths (Heroic Strike 150 = 15 rage)
    return cost;
}

int32_t World::auraSum(const Entity& e, uint8_t type, int32_t miscMask) const {
    int32_t n = 0;
    for (uint8_t i = 0; i < e.auraCount; ++i)
        if (e.auras[i].type == type && (miscMask < 0 || (e.auras[i].misc & miscMask))) n += e.auras[i].amount;
    return n;
}

// 1.12 rage conversion (Player::RewardRage): c = 0.0091107836 L^2 + 3.225598133 L + 4.2652911;
// dealing 7.5 * damage / c, taking 2.5 * damage / c (rage points as the client shows them)
void World::gainRage(Entity& e, int32_t damage, bool dealt) {
    if (e.powerType != 1 || damage <= 0 || !e.alive()) return;
    const float L = (float)e.level;
    const float c = 0.0091107836f * L * L + 3.225598133f * L + 4.2652911f;
    const float r = (dealt ? 7.5f : 2.5f) * (float)damage / c;
    e.power = std::min<int32_t>((int32_t)e.maxPower, e.power + std::max<int32_t>(1, (int32_t)std::lround(r)));
}

int32_t World::absorb(Entity& dst, int32_t dmg, uint8_t school) {
    for (int i = 0; i < dst.auraCount && dmg > 0;) {
        Aura& a = dst.auras[i];
        if (a.type == A_SCHOOL_ABSORB && (a.misc & (1 << school)) && a.amount > 0) {
            const int32_t take = std::min(a.amount, dmg);
            a.amount -= take;
            dmg -= take;
            if (a.amount <= 0) { removeAura(dst, i); continue; }
        }
        ++i;
    }
    return dmg;
}

void World::cancelAuraX(Entity& e, uint32_t spell) {
    const SpellX* s = data_ ? data_->sx(spell) : nullptr;
    for (int i = 0; i < e.auraCount;) {
        if (e.auras[i].spell == spell && (e.auras[i].positive || !s)) { removeAura(e, i); continue; }
        ++i;
    }
}

// ---------------------------------------------------------------- cast
bool World::castX(Entity& e, uint32_t id, uint64_t targetGuid, uint32_t now, bool triggered) {
    const SpellX* sp = data_ ? data_->sx(id) : nullptr;
    if (!sp || !e.alive()) return false;
    const SpellX& s = *sp;
    auto fail = [&](uint8_t why, uint64_t t = 0) { if (!triggered) emit(SimEvent::CastFailed, e.guid, t, id, why, 0, 0); return false; };
    // (the reasons are SPELL_FAILED_* of 1.12: net/result_text.cpp words them)
    if (!triggered) {
        if (s.passive()) return fail(56);                                   // SPELL_FAILED_NOT_KNOWN (no casting a passive)
        if (e.castSpell) return fail(97);                                   // SPELL_FAILED_SPELL_IN_PROGRESS
        if (e.stunned(now) || e.feared(now)) return fail(100);               // SPELL_FAILED_STUNNED
        if (e.silenced(now) && s.dmgClass != 2 && s.school != 0) return fail(96);   // SPELL_FAILED_SILENCED
        if (e.pacified(now) && s.dmgClass == 2) return fail(90);           // SPELL_FAILED_PACIFIED
        if (s.gcdMs && e.gcdEndMs > now) return fail(60);                    // SPELL_FAILED_NOT_READY
        if (cooldownLeft(e, id, now) || (s.category && cooldownLeft(e, kSlotCategoryBit | s.category, now))) return fail(60);
        if (s.stances && !(e.form && (s.stances & (1u << (e.form - 1))))) return fail(86);   // SPELL_FAILED_ONLY_SHAPESHIFT
        if (s.stancesNot && e.form && (s.stancesNot & (1u << (e.form - 1)))) return fail(61);  // SPELL_FAILED_NOT_SHAPESHIFT
        const uint32_t cost = powerCostX(e, s);
        if (cost && (s.powerType == e.powerType || s.powerType > 3) && e.power < (int32_t)cost) return fail(77);   // SPELL_FAILED_NO_POWER
        if (s.powerType == 0xFE && e.hp <= (int32_t)cost) return fail(77);   // (health)
        for (const auto& [item, n] : s.reagents)
            if (countPlayerItem(item) < n) return fail(92);                 // SPELL_FAILED_REAGENTS
        if (s.equipClass >= 0) {   // a weapon (2) or a shield (4 / 6) worn
            const CharacterProfile& p = profiles_[0];
            bool ok = false;
            for (int slot : {15, 16, 17}) {
                const ItemTemplate* it = p.equipment[(size_t)slot] ? data_->item(p.equipment[(size_t)slot]) : nullptr;
                if (it && (int32_t)it->cls == s.equipClass && (!s.equipSubMask || (s.equipSubMask & (1u << it->subclass)))) ok = true;
            }
            if (!ok) return fail(25);                                       // SPELL_FAILED_EQUIPPED_ITEM_CLASS
        }
        bool usesCombo = false;
        for (const SpellEffectX& x : s.eff) usesCombo = usesCombo || x.perCombo != 0;
        if (usesCombo && (!e.comboPoints || e.comboTarget != targetGuid)) return fail(70);   // SPELL_FAILED_CANT_DO_THAT_YET...
    }
    Entity* t = entity(targetGuid);
    const int need = unitTarget(s);
    if (need == 1) {
        if (!t || t == &e || !t->alive()) return fail(10, targetGuid);     // SPELL_FAILED_BAD_TARGETS
        if (!canAttack(e, *t)) return fail(10, targetGuid);
    } else if (need == 2) {
        if (!t || !t->alive() || canAttack(e, *t)) t = &e;
    } else if (!t) {
        t = &e;
    }
    if (need == 1 && !triggered) {
        const float maxR = (s.rangeMax > 0 ? s.rangeMax : kReach) + t->boundingRadius + e.boundingRadius;
        const float d = dist(e, *t);
        if (d > maxR) return fail(89, t->guid);                            // SPELL_FAILED_OUT_OF_RANGE
        if (s.rangeMin > 0 && d < s.rangeMin) return fail(118, t->guid);    // SPELL_FAILED_TOO_CLOSE
        if (s.targetAuraState == 1 && t->hp * 5 > (int32_t)t->maxHp) return fail(103, t->guid);   // (Execute: under 20 %)
    }
    // "on the next swing": queued, paid when it lands (Heroic Strike, Cleave, Raptor Strike, Maul)
    if ((s.attr & 0x4) && !triggered) {
        e.nextSwingSpell = id;
        if (!e.victim && t && t != &e) startAttack(e, t->guid, now);
        return true;
    }
    if (s.gcdMs && !triggered) e.gcdEndMs = now + s.gcdMs;
    e.castTarget = t ? t->guid : e.guid;
    e.castTriggered = triggered;
    if (s.castMs > 0 && !triggered) {
        e.castSpell = id;
        e.castStartMs = now;
        e.castEndMs = now + s.castMs;
        e.castInterruptible = 1;
        e.channeling = false;
        emit(SimEvent::CastStarted, e.guid, e.castTarget, id, (int32_t)s.castMs, s.school);
        return true;
    }
    if (s.channeled() && !triggered) {
        const uint32_t d = durationX(e, s);
        if (!finishX(e, s, e.castTarget, now)) return false;
        e.castSpell = id;
        e.castStartMs = now;
        e.castEndMs = now + std::max<uint32_t>(d, 500);
        e.channeling = true;
        return true;
    }
    return finishX(e, s, e.castTarget, now);
}

void World::endChannel(Entity& e, uint32_t now) {
    (void)now;
    if (!e.channeling) return;
    const uint32_t id = e.castSpell;
    e.channeling = false;
    for (AreaFx& a : areas_) if (a.used && a.caster == e.guid && a.spell == id) a.used = false;
    for (Entity& o : pool_) {
        if (!o.used) continue;
        for (int i = 0; i < o.auraCount;) {
            if (o.auras[i].spell == id && o.auras[i].caster == e.guid) { removeAura(o, i); continue; }
            ++i;
        }
    }
}

bool World::finishX(Entity& e, const SpellX& s, uint64_t targetGuid, uint32_t now) {
    Entity* t = entity(targetGuid);
    const int need = unitTarget(s);
    if (need == 1 && (!t || !t->alive())) { emit(SimEvent::CastFailed, e.guid, targetGuid, s.id, 10, 0, 0); return false; }
    if (!t) t = &e;
    if (!e.castTriggered) {   // the price
        const uint32_t cost = powerCostX(e, s);
        if (cost && (s.powerType == e.powerType)) {
            if (e.power < (int32_t)cost) { emit(SimEvent::CastFailed, e.guid, targetGuid, s.id, 77, 0, 0); return false; }
            e.power -= (int32_t)cost;
            if (e.powerType == 0) e.lastManaSpendMs = now;
            emit(SimEvent::PowerChanged, e.guid, 0, 0, e.power);
        }
        for (const auto& [item, n] : s.reagents) removePlayerItem(item, n);
        auto cd = [&](uint32_t key, uint32_t ms) {
            if (!ms) return;
            for (uint8_t i = 0; i < e.cooldownCount; ++i)
                if (e.cooldowns[i].spell == key) { e.cooldowns[i] = Cooldown{key, now + ms, ms}; return; }
            if (e.cooldownCount < kMaxCooldowns) e.cooldowns[e.cooldownCount++] = Cooldown{key, now + ms, ms};
            else e.cooldowns[0] = Cooldown{key, now + ms, ms};
        };
        cd(s.id, s.cooldownMs);
        if (s.category) cd(kSlotCategoryBit | s.category, s.catCooldownMs);
    }
    emit(SimEvent::CastFinished, e.guid, t->guid, s.id, 0, s.school);
    int combo = 0;
    bool usesCombo = false;
    for (const SpellEffectX& x : s.eff) usesCombo = usesCombo || x.perCombo != 0;
    if (usesCombo) combo = e.comboPoints;
    int32_t weaponBonus = 0, weaponPct = 100;
    bool weapon = false;
    for (int k = 0; k < 3; ++k) {
        if (!s.eff[k].effect) continue;
        if (weaponEffect(s.eff[k].effect)) {   // one strike for all the weapon effects
            if (s.eff[k].effect == E_WEAPON_PCT) weaponPct = weaponPct * effValue(e, s, k, combo) / 100;
            else weaponBonus += effValue(e, s, k, combo);
            weapon = true;
            continue;
        }
        applyEffectX(e, s, k, t, now, combo, &weaponBonus, &weaponPct, &weapon);
        if (!e.used) return true;
    }
    if (weapon && t != &e && t->alive()) {
        int32_t dmg = rng_.range((int32_t)e.dmgMin, (int32_t)e.dmgMax) + weaponBonus;
        dmg = dmg * weaponPct / 100;
        dmg += auraSum(e, A_ATTACK_POWER) * (int32_t)e.attackMs / 14000;
        if ((int32_t)rng_.below(100) < 5) dmg *= 2;   // a crit
        applyDamage(e, *t, std::max(1, dmg), s.id, s.school, now);
        gainRage(e, dmg, true);
        if (!e.victim) startAttack(e, t->guid, now);
    }
    if (usesCombo) { e.comboPoints = 0; e.comboTarget = 0; }
    return true;
}

// ---------------------------------------------------------------- targets
void World::targetsX(Entity& c, const SpellX& s, int k, Entity* t, std::vector<Entity*>& out) {
    out.clear();
    const SpellEffectX& e = s.eff[k];
    const float r = e.radius > 0 ? e.radius : 8.0f;
    auto enemiesAround = [&](float x, float y, bool cone) {
        for (Entity& o : pool_) {
            if (!o.used || !o.alive() || &o == &c || o.untargetable || !canAttack(c, o)) continue;
            const float d = std::hypot(o.x - x, o.y - y);
            if (d > r + o.boundingRadius) continue;
            if (cone) {
                float a = std::atan2(o.y - c.y, o.x - c.x) - c.o;
                while (a > 3.14159265f) a -= 6.2831853f;
                while (a < -3.14159265f) a += 6.2831853f;
                if (std::fabs(a) > 0.5236f) continue;   // 60 degrees in front
            }
            out.push_back(&o);
        }
    };
    auto party = [&]() {
        out.push_back(&c);
        for (Entity* b : bots_) if (b && b->alive() && dist(*b, c) <= r) out.push_back(b);
    };
    const uint8_t A = e.targetA, B = e.targetB;
    if (B == 15 || B == 16 || A == 15 || A == 16) {
        if (A == 22 || A == 1) enemiesAround(c.x, c.y, false);
        else if (c.castHasDest && (A == 18 || A == 17 || A == 16 || A == 15 || B == 16)) enemiesAround(c.castDestX, c.castDestY, false);
        else if (t && t != &c) enemiesAround(t->x, t->y, false);
        else enemiesAround(c.x, c.y, false);
        return;
    }
    if (A == 24 || B == 24) { enemiesAround(c.x, c.y, true); return; }
    if (partyTarget(A) || partyTarget(B)) { party(); return; }
    if (A == 1 || A == 5 || (A == 0 && e.effect != E_SCHOOL_DAMAGE)) { out.push_back(&c); return; }
    if (A == 21 || A == 57) { out.push_back(t && t->alive() && !canAttack(c, *t) ? t : &c); return; }
    if (t) out.push_back(t);
    if (e.chain > 1 && t && t != &c) {   // chained to the nearest others
        Entity* last = t;
        for (int n = 1; n < e.chain; ++n) {
            Entity* best = nullptr;
            float bd = 10.0f;
            for (Entity& o : pool_) {
                if (!o.used || !o.alive() || &o == &c || std::find(out.begin(), out.end(), &o) != out.end()) continue;
                if (!canAttack(c, o)) continue;
                const float d = dist(o, *last);
                if (d < bd) { bd = d; best = &o; }
            }
            if (!best) break;
            out.push_back(best);
            last = best;
        }
    }
}

// ---------------------------------------------------------------- effects
void World::applyEffectX(Entity& c, const SpellX& s, int k, Entity* t, uint32_t now, int combo, int32_t*, int32_t*, bool*) {
    const SpellEffectX& e = s.eff[k];
    std::vector<Entity*> ts;
    targetsX(c, s, k, t, ts);
    switch (e.effect) {
        case E_SCHOOL_DAMAGE:
            for (Entity* x : ts) {
                if (!x->alive() || x == &c) continue;
                int32_t dmg = effValue(c, s, k, combo);
                if ((int32_t)rng_.below(100) < 5) dmg = dmg * 3 / 2;   // a spell crit
                applyDamage(c, *x, std::max(1, dmg), s.id, s.school, now);
            }
            break;
        case E_HEAL: case E_HEAL_MAX:
            for (Entity* x : ts) {
                if (!x->alive()) continue;
                const int32_t v = e.effect == E_HEAL_MAX ? (int32_t)x->maxHp : effValue(c, s, k, combo);
                applyHeal(c, *x, std::max(1, v), s.id, now);
            }
            break;
        case E_APPLY_AURA: case E_AREA_AURA:
            for (Entity* x : ts) if (x->alive()) applyAuraX(c, *x, s, k, now, combo);
            break;
        case E_PERSISTENT_AURA: {   // Blizzard, Consecration, Rain of Fire: a patch of ground that works on its own
            AreaFx* slot = nullptr;
            for (AreaFx& a : areas_) if (!a.used) { slot = &a; break; }
            if (!slot) break;
            AreaFx a;
            a.used = true; a.caster = c.guid; a.spell = s.id; a.eff = (uint8_t)k;
            if (c.castHasDest && (e.targetA == 18 || e.targetA == 16 || e.targetA == 17)) { a.x = c.castDestX; a.y = c.castDestY; }
            else if (e.targetA == 22 || e.targetA == 1 || !t || t == &c) { a.x = c.x; a.y = c.y; }
            else { a.x = t->x; a.y = t->y; }
            a.radius = e.radius > 0 ? e.radius : 8.0f;
            a.periodMs = e.amplitude > 0 ? (uint32_t)e.amplitude : 1000;
            a.endMs = now + std::max<uint32_t>(durationX(c, s), a.periodMs);
            a.nextMs = now + a.periodMs;
            a.value = effValue(c, s, k, combo);
            *slot = a;
            break;
        }
        case E_ENERGIZE:
            for (Entity* x : ts) {
                if (!x->alive() || (int32_t)x->powerType != e.misc) continue;
                int32_t v = effValue(c, s, k, combo);
                if (x->powerType == 1) v = (v + 5) / 10;   // tenths of rage
                x->power = std::min<int32_t>((int32_t)x->maxPower, x->power + v);
                emit(SimEvent::PowerChanged, x->guid, 0, 0, x->power);
            }
            break;
        case E_CREATE_ITEM:
            if (c.self && e.itemType) addItemToPlayer(e.itemType, (uint16_t)std::max(1, effValue(c, s, k, combo)), now);
            break;
        case E_TRIGGER:
            if (e.trigger) castX(c, e.trigger, t ? t->guid : c.guid, now, true);
            break;
        case E_COMBO:
            if (t && t != &c) {
                if (c.comboTarget != t->guid) { c.comboTarget = t->guid; c.comboPoints = 0; }
                c.comboPoints = (uint8_t)std::min(5, (int)c.comboPoints + std::max(1, effValue(c, s, k)));
            }
            break;
        case E_CHARGE:
            if (t && t != &c) {   // to the target's side (the session moves the client from where it was)
                c.castDestX = c.x; c.castDestY = c.y; c.castDestZ = c.z;
                const float a = std::atan2(c.y - t->y, c.x - t->x);
                const float stop = t->boundingRadius + c.boundingRadius + 1.0f;
                c.x = t->x + std::cos(a) * stop;
                c.y = t->y + std::sin(a) * stop;
                c.z = t->z;
                applyGround(c);
                emit(SimEvent::Message, c.guid, t->guid, s.id, 1);   // (value 1: the session sends the charge)
                if (!c.victim) startAttack(c, t->guid, now);
            }
            break;
        case E_INTERRUPT:
            for (Entity* x : ts) if (x->castSpell && x != &c) cancelCast(*x, 35, now);
            break;
        case E_DISPEL:   // one magic aura: a harmful one from a friend, a helpful one from an enemy
            for (Entity* x : ts) {
                const bool friendT = !canAttack(c, *x);
                for (int i = x->auraCount - 1; i >= 0; --i)
                    if (x->auras[i].positive != friendT && x->auras[i].school != 0) { removeAura(*x, i); break; }
            }
            break;
        case E_THREAT:
            for (Entity* x : ts) if (x->objType == 3) addThreat(*x, c.guid, effValue(c, s, k, combo));
            break;
        case E_ATTACK_ME:
            for (Entity* x : ts) if (x->objType == 3) taunt(*x, c.guid);
            break;
        case E_POWER_BURN: case E_POWER_DRAIN:
            for (Entity* x : ts) {
                if (!x->alive() || x->powerType != 0) continue;
                const int32_t v = std::min(x->power, effValue(c, s, k, combo));
                x->power -= v;
                if (e.effect == E_POWER_BURN) applyDamage(c, *x, std::max(1, (int32_t)(v * std::max(0.1f, e.multiple))), s.id, s.school, now);
                else if (c.powerType == 0) c.power = std::min<int32_t>((int32_t)c.maxPower, c.power + v);
            }
            break;
        case E_HEALTH_LEECH:
            for (Entity* x : ts) {
                if (!x->alive() || x == &c) continue;
                const int32_t v = effValue(c, s, k, combo);
                applyDamage(c, *x, std::max(1, v), s.id, s.school, now);
                applyHeal(c, c, std::max(1, (int32_t)(v * (e.multiple > 0 ? e.multiple : 1.0f))), s.id, now);
            }
            break;
        case E_INSTAKILL:
            for (Entity* x : ts) if (x->alive() && x != &c) applyDamage(c, *x, x->hp, s.id, s.school, now);
            break;
        default:
            break;   // (dummy / script effects of single spells, summons, totems, teleports: not yet)
    }
}

void World::applyAuraX(Entity& c, Entity& t, const SpellX& s, int k, uint32_t now, int combo) {
    const SpellEffectX& e = s.eff[k];
    const bool positive = positiveX(s) && !canAttack(c, t);
    Aura a;
    a.spell = s.id;
    a.caster = c.guid;
    a.type = e.aura;
    a.effIndex = (uint8_t)k;
    a.misc = e.misc;
    a.school = s.school;
    a.positive = positive;
    a.kind = (uint8_t)(positive ? SpellKind::Buff : SpellKind::Debuff);
    a.amount = effValue(c, s, k, combo);
    const uint32_t d = durationX(c, s);
    a.endMs = d ? now + d : 0;
    a.amplitudeMs = e.amplitude > 0 ? (uint32_t)e.amplitude : 0;
    a.nextTickMs = a.amplitudeMs ? now + a.amplitudeMs : 0;
    a.charges = (uint8_t)std::min<uint32_t>(255, s.procCharges);
    switch (e.aura) {
        case A_SHAPESHIFT:   // a stance / form: the others go
            for (int i = t.auraCount - 1; i >= 0; --i)
                if (t.auras[i].type == A_SHAPESHIFT && t.auras[i].spell != s.id) removeAura(t, i);
            t.form = (uint8_t)e.misc;
            break;
        case A_STUN: case A_ROOT: case A_DEC_SPEED: case A_FEAR: case A_CONFUSE: case A_SILENCE: case A_PACIFY: case A_PACIFY_SILENCE: {
            SpellControl ctl;
            ctl.durationMs = d ? d : 10000;
            const Control type = e.aura == A_STUN ? Control::Stun : e.aura == A_ROOT ? Control::Root : e.aura == A_DEC_SPEED ? Control::Slow
                               : e.aura == A_FEAR ? Control::Fear : e.aura == A_CONFUSE ? Control::Confuse : e.aura == A_SILENCE ? Control::Silence
                               : e.aura == A_PACIFY ? Control::Pacify : Control::PacifySilence;
            ctl.effects.emplace_back((uint8_t)type, a.amount);
            applyControl(t, ctl, now);
            break;
        }
        case A_PERIODIC_DAMAGE: case A_PERIODIC_LEECH:
            a.kind = (uint8_t)SpellKind::Debuff;   // (ticks in tickAuraX, not as the sample's DoTs)
            if (!a.amplitudeMs) { a.amplitudeMs = 3000; a.nextTickMs = now + 3000; }
            break;
        default:
            break;
    }
    if (!positive && t.objType == 3 && !t.victim && canAttack(t, c)) { t.victim = t.target = c.guid; addThreat(t, c.guid, 1); }
    addAura(t, a);
}

void World::tickAuraX(Entity& e, Aura& a, uint32_t now) {
    if (!a.amplitudeMs || !a.nextTickMs || now < a.nextTickMs) return;
    a.nextTickMs += a.amplitudeMs;
    Entity* c = entity(a.caster);
    switch (a.type) {
        case A_PERIODIC_DAMAGE: case A_PERIODIC_LEECH:
            if (c && c->alive()) {
                const uint32_t spell = a.spell;
                const int32_t amount = std::max<int32_t>(1, a.amount);
                applyDamage(*c, e, amount, spell, a.school, now);
                if (a.type == A_PERIODIC_LEECH && c->alive()) applyHeal(*c, *c, amount, spell, now);
            }
            break;
        case A_PERIODIC_HEAL:
            if (e.alive()) applyHeal(c ? *c : e, e, std::max<int32_t>(1, a.amount), a.spell, now);
            break;
        case A_PERIODIC_TRIGGER: {   // (Arcane Missiles: the caster's aura fires at what it casts at)
            const SpellX* s = data_ ? data_->sx(a.spell) : nullptr;
            const uint32_t tr = s ? s->eff[a.effIndex].trigger : 0;
            if (tr && c && c->alive()) {
                uint64_t tgt = e.guid;
                if (&e == c) tgt = (c->castTarget && c->castTarget != c->guid) ? c->castTarget : (c->victim ? c->victim : c->guid);
                castX(*c, tr, tgt, now, true);
            }
            break;
        }
        default:
            break;
    }
}

void World::tickAreas(uint32_t now) {
    for (AreaFx& a : areas_) {
        if (!a.used) continue;
        if (now >= a.endMs) { a.used = false; continue; }
        if (now < a.nextMs) continue;
        a.nextMs += a.periodMs;
        Entity* c = entity(a.caster);
        const SpellX* s = data_ ? data_->sx(a.spell) : nullptr;
        if (!c || !c->alive() || !s) { a.used = false; continue; }
        const SpellEffectX& e = s->eff[a.eff];
        for (Entity& o : pool_) {
            if (!o.used || !o.alive() || &o == c || !canAttack(*c, o)) continue;
            if (std::hypot(o.x - a.x, o.y - a.y) > a.radius + o.boundingRadius) continue;
            if (e.aura == A_PERIODIC_DAMAGE || e.aura == 0) applyDamage(*c, o, std::max(1, a.value), a.spell, s->school, now);
            else if (e.aura == A_DEC_SPEED) { SpellControl ctl; ctl.durationMs = a.periodMs + 200; ctl.effects.emplace_back((uint8_t)Control::Slow, a.value); applyControl(o, ctl, now); }
            if (!c->used) return;
        }
    }
}

}  // namespace offline
}  // namespace w3n
