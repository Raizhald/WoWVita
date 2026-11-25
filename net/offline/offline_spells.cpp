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
#include <ctime>
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
    return spellModX(c, s, 8, v);   // SPELLMOD_ALL_EFFECTS (Unit::CalculateSpellEffectValue)
}

// Player::UpdateCritPercentage: 5 % + agility / the class's rate at the level + SPELL_AURA_MOD_CRIT_PERCENT (52)
// (+ the ability's SPELLMOD_CRITICAL_CHANCE); a creature 5 %
int32_t World::meleeCritX(const Entity& a, const SpellX* s) const {
    if (!a.self || &a != player_) return 500;
    static const float lv1[12] = {0, 3.9f, 4.6f, 3.5f, 2.2f, 11.0f, 0, 4.6f, 12.9f, 8.4f, 0, 4.6f};
    static const float lv60[12] = {0, 20.0f, 20.0f, 53.0f, 29.0f, 20.0f, 0, 20.0f, 20.0f, 20.0f, 0, 20.0f};
    const uint8_t cls = profiles_[0].cls < 12 ? profiles_[0].cls : 1;
    const float L = (float)std::min<int>(a.level, 60);
    const float rate = lv1[cls] * (60.0f - L) / 59.0f + lv60[cls] * (L - 1.0f) / 59.0f;
    float pct = 5.0f + (rate > 0 ? (float)a.statAgi / rate : 0.0f) + (float)auraSum(a, 52);
    int32_t v = (int32_t)(pct * 100.0f);
    if (s) v = spellModX(a, *s, 7, v / 100) * 100 + v % 100;   // SPELLMOD_CRITICAL_CHANCE (whole percents)
    return std::max(0, v);
}

// Player::UpdateSpellCritChance: GetSpellCritFromIntellect (base + intellect / (rate0 + rate1 x level)) +
// SPELL_AURA_MOD_SPELL_CRIT_CHANCE (57) + _SCHOOL (71) + SPELLMOD_CRITICAL_CHANCE; a creature 5 %
int32_t World::spellCritX(const Entity& c, const SpellX& s) const {
    if (!c.self || &c != player_) return 500;
    static const float base[12] = {0, 0, 3.70f, 0, 0, 2.97f, 0, 3.54f, 3.70f, 3.18f, 0, 3.33f};
    static const float r0[12] = {0, 0, 14.77f, 0, 0, 10.03f, 0, 11.51f, 14.77f, 11.30f, 0, 12.41f};
    static const float r1[12] = {0, 10, 0.65f, 10, 10, 0.82f, 0, 0.80f, 0.65f, 0.82f, 0, 0.79f};
    const uint8_t cls = profiles_[0].cls < 12 ? profiles_[0].cls : 1;
    const float ratio = r0[cls] + r1[cls] * (float)c.level;
    float pct = base[cls] + (ratio > 0 ? (float)c.statInt / ratio : 0.0f) + (float)auraSum(c, 57) + (float)auraSum(c, 71, 1 << s.school);
    int32_t v = (int32_t)(pct * 100.0f);
    v = spellModX(c, s, 7, v / 100) * 100 + v % 100;
    return std::max(0, v);
}

int32_t World::spellModX(const Entity& c, const SpellX& s, uint8_t op, int32_t base) const {
    if (!c.self || !data_ || !s.family || &c != player_) return base;
    static const uint8_t classFamily[12] = {0, 4, 10, 9, 8, 6, 0, 11, 3, 5, 0, 7};   // SpellFamilyName of each class
    const uint8_t cls = profiles_[0].cls;
    if (cls >= 12 || s.family != classFamily[cls]) return base;
    if (s.attrEx3 & 0x20000000) return base;   // SPELL_ATTR_EX3_IGNORE_CASTER_MODIFIERS
    int32_t flat = 0, pct = 100;
    for (uint8_t i = 0; i < c.auraCount; ++i) {
        const Aura& a = c.auras[i];
        if ((a.type != 107 && a.type != 108) || a.misc != op) continue;
        const SpellX* src = data_->sx(a.spell);
        if (!src || src->family != s.family) continue;
        auto am = data_->affectMask.find((a.spell << 8) | a.effIndex);
        const uint64_t mask = am != data_->affectMask.end() ? am->second : (uint64_t)src->eff[a.effIndex].itemType;
        if (!(mask & s.familyFlags)) continue;
        if (a.type == 107) flat += a.amount;
        else {
            if (op == 10 && base >= 10000 && a.amount <= -100) continue;   // (not a 10 s cast made instant)
            pct += a.amount;
        }
    }
    if (!flat && pct == 100) return base;
    return (int32_t)((int64_t)(base + flat) * std::max(0, pct) / 100);
}

uint32_t World::durationX(const Entity& c, const SpellX& s, int combo) const {
    if (s.durBase < 0) return 0;   // until cancelled
    int32_t d = s.durBase;
    // CMaNGOS CalculateSpellDuration: a player's combo points stretch it toward the maximum (Slice and Dice, Rupture)
    if (combo > 0 && c.self && s.durMax > 0 && s.durMax != s.durBase) return (uint32_t)(d + (s.durMax - d) * std::min(combo, 5) / 5);
    if (s.durPerLevel) {
        const int over = std::max(0, (int)c.level - (int)s.spellLevel);
        d += s.durPerLevel * over;
        if (s.durMax > 0) d = std::min(d, s.durMax);
    }
    return (uint32_t)std::max(0, spellModX(c, s, 1, d));   // SPELLMOD_DURATION
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
    // ManaCostPercentage: of the base (create) mana for a player (CMaNGOS Spell::CalculatePowerCost: GetCreateMana)
    if (s.manaPct) cost += (e.self && e.baseMana ? e.baseMana : (e.formPower ? e.savedMaxPower : e.maxPower)) * s.manaPct / 100;
    cost = (uint32_t)std::max(0, spellModX(e, s, 14, (int32_t)cost));   // SPELLMOD_COST (in tenths of rage too)
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

bool World::hasAuraType(const Entity& e, uint8_t type, int32_t miscMask) const {
    for (uint8_t i = 0; i < e.auraCount; ++i)
        if (e.auras[i].type == type && (miscMask < 0 || (e.auras[i].misc & miscMask))) return true;
    return false;
}

void World::rewardSpell(Entity& p, uint32_t id, uint32_t now) {
    const SpellX* s = data_ ? data_->sx(id) : nullptr;
    if (!s) return;
    bool learnt = false;
    for (const SpellEffectX& x : s->eff)
        if (x.effect == E_LEARN_SPELL && x.trigger) { emit(SimEvent::Message, p.guid, p.guid, x.trigger, 2); learnt = true; }
    if (!learnt) castX(p, id, p.guid, now, true);
}

void World::interruptAuras(Entity& e, uint32_t flags) {
    if (!data_) return;
    for (int i = 0; i < e.auraCount;) {
        const SpellX* s = e.auras[i].type ? data_->sx(e.auras[i].spell) : nullptr;
        if (s && (s->auraInterruptFlags & flags)) { removeAura(e, i); continue; }
        ++i;
    }
}

// PROC_FLAG_* (CMaNGOS SpellMgr.h): 0x4 dealt / 0x8 taken melee swing, 0x10 / 0x20 a melee ability
void World::procAuras(Entity& o, Entity& other, uint32_t flag, uint32_t now, uint32_t procEx, const SpellX* procSpell) {
    if (!data_) return;
    for (int i = 0; i < o.auraCount && o.alive();) {
        Aura& a = o.auras[i];
        const SpellX* s = (a.type == 42 || a.type == 4 || a.charges) ? data_->sx(a.spell) : nullptr;
        if (!s) { ++i; continue; }
        // WoWVita: spell_proc_event (SpellMgr::IsSpellProcEventCanTriggeredBy): its flags, hit or crit, the spells
        // it is for, procs per minute or its own chance, a cooldown
        auto pe = data_->procEvents.find(a.spell);
        const WorldData::ProcEvent* ev = pe != data_->procEvents.end() ? &pe->second : nullptr;
        const uint32_t flags = ev && ev->flags ? ev->flags : s->procFlags;
        if (!(flags & flag)) { ++i; continue; }
        if (ev) {
            bool ok;
            if (!ev->ex) ok = (procEx & 0x3) != 0;                     // (none set: any hit)
            else ok = (ev->ex & 0x10000) || (ev->ex & procEx);         // PROC_EX_EX_TRIGGER_ALWAYS
            if (ok && ev->family) ok = procSpell && procSpell->family == ev->family;
            const uint64_t mask = ev->mask[a.effIndex < 3 ? a.effIndex : 0];
            if (ok && mask) ok = procSpell && (procSpell->familyFlags & mask);
            if (ok && ev->cooldownMs && cooldownLeft(o, 0x40000000u | a.spell, now)) ok = false;
            if (!ok) { ++i; continue; }
        }
        float chance = (float)(s->procChance ? s->procChance : 100);
        if (ev && ev->ppm > 0) chance = (float)o.attackMs * ev->ppm / 600.0f;
        else if (ev && ev->chance > 0) chance = ev->chance;
        if (chance < 100.0f && (float)rng_.below(10000) >= chance * 100.0f) { ++i; continue; }
        if (ev && ev->cooldownMs) {
            if (o.cooldownCount < kMaxCooldowns) o.cooldowns[o.cooldownCount++] = Cooldown{0x40000000u | a.spell, now + ev->cooldownMs, ev->cooldownMs};
        }
        bool fired = false;
        if (a.type == 42 && s->eff[a.effIndex].trigger) {   // PROC_TRIGGER_SPELL (Lightning Shield, Frost Armor)
            castX(o, s->eff[a.effIndex].trigger, other.guid, now, true);
            fired = true;
        } else if (a.type == 4 && s->family == 10 && (s->familyFlags & 0x8000000ull) && a.effIndex == 0 && (flag & 0x4) && o.self) {
            // Seal of Righteousness (CMaNGOS Unit::HandleDummyAuraProc): holy damage by the weapon's speed
            static const uint32_t ranks[][2] = {{20154, 25742}, {21084, 25741}, {20287, 25740}, {20288, 25739}, {20289, 25738},
                                                {20290, 25737}, {20291, 25736}, {20292, 25735}, {20293, 25713}};
            uint32_t sid = 25742;
            for (const auto& r : ranks) if (r[0] == a.spell) sid = r[1];
            const ItemTemplate* w = profiles_[0].equipment[15] ? data_->item(profiles_[0].equipment[15]) : nullptr;
            const float speed = (float)o.attackMs / 1000.0f;
            float base;
            if (w && w->inventoryType == 17) base = 1.20f * a.amount * 1.2f * 1.03f * speed / 100.0f + 1;
            else base = 0.85f * std::ceil(a.amount * 1.2f * 1.03f * speed / 100.0f) - 1;
            const int32_t dmg = (int32_t)(base + 0.03f * (float)(o.dmgMin + o.dmgMax) / 2.0f) + 1;
            if (dmg > 0 && other.alive()) applyDamage(o, other, dmg, sid, 1, now);
            fired = true;
        }
        else if (a.charges && a.type != 51 && a.type != 42 && a.type != 4) fired = true;   // a charge used (Inner Fire)
        if (fired && a.charges) {
            if (--a.charges == 0) { removeAura(o, i); continue; }
        }
        ++i;
    }
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
        if (a.type == 97 && a.amount > 0 && dst.powerType == 0 && dst.power > 1) {   // Mana Shield: 2 mana a point
            const int32_t take = std::min({a.amount, dmg, dst.power / 2});
            a.amount -= take;
            dst.power -= take * 2;
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
    if (!triggered && e.form && e.self) {
        // what the 1.12 client does with a form button while in a form (CastShapeshiftForm): the active one, or any while
        // shifted, cancels the form (CMSG_CANCEL_AURA) - our client has no stance bar, so the server does it for it
        int newForm = -1;
        for (const SpellEffectX& x : s.eff) if (x.effect == E_APPLY_AURA && x.aura == A_SHAPESHIFT) newForm = x.misc;
        const bool shifted = e.form != 17 && e.form != 18 && e.form != 19 && e.form != 28 && e.form != 30;
        if (newForm >= 0 && (newForm == e.form || shifted)) {
            for (int i = e.auraCount - 1; i >= 0; --i)
                if (e.auras[i].type == A_SHAPESHIFT) { removeAuraSpell(e, e.auras[i].spell); break; }
            return true;
        }
    }
    if (!triggered) {
        if (s.passive()) return fail(56);                                   // SPELL_FAILED_NOT_KNOWN (no casting a passive)
        if (e.castSpell) return fail(97);                                   // SPELL_FAILED_SPELL_IN_PROGRESS
        if (e.stunned(now) || e.feared(now)) return fail(100);               // SPELL_FAILED_STUNNED
        if (e.silenced(now) && s.dmgClass != 2 && s.school != 0) return fail(96);   // SPELL_FAILED_SILENCED
        if (e.pacified(now) && s.dmgClass == 2) return fail(90);           // SPELL_FAILED_PACIFIED
        if (s.gcdMs && e.gcdEndMs > now) return fail(60);                    // SPELL_FAILED_NOT_READY
        if (cooldownLeft(e, id, now) || (s.category && cooldownLeft(e, kSlotCategoryBit | s.category, now))) return fail(60);
        if ((s.attr & 0x20000) && !hasAuraType(e, 16)) return fail(0x57);   // SPELL_ATTR_ONLY_STEALTHED
        if (s.focus && !nearSpellFocus(e, s.focus)) return fail(0x5E);        // SPELL_FAILED_REQUIRES_SPELL_FOCUS (a circle)
        if ((s.attr & 0x10000000) && inCombatNow(e)) return fail(0);          // NOT_IN_COMBAT: SPELL_FAILED_AFFECTING_COMBAT
        // CMaNGOS GetErrorAtShapeshiftedCast. Stances, Shadowform and Stealth act as no form (SpellShapeshiftForm flags1 & 1)
        {
            const uint32_t mask = e.form ? 1u << (e.form - 1) : 0;
            const bool shifted = e.form && e.form != 17 && e.form != 18 && e.form != 19 && e.form != 28 && e.form != 30;
            if (mask & s.stancesNot) return fail(61);                                    // SPELL_FAILED_NOT_SHAPESHIFT
            if (!(mask & s.stances)) {
                if (shifted && (s.attr & 0x10000)) return fail(61);                        // SPELL_ATTR_NOT_SHAPESHIFT
                if (shifted && s.stances) return fail(86);                                 // SPELL_FAILED_ONLY_SHAPESHIFT
                if (!shifted && s.stances && !(s.attrEx2 & 0x80000)) return fail(86);      // (ALLOW_WHILE_NOT_SHAPESHIFTED)
            }
        }
        const uint32_t cost = powerCostX(e, s);
        if (cost && (s.powerType == e.powerType || s.powerType > 3) && e.power < (int32_t)cost) return fail(77);   // SPELL_FAILED_NO_POWER
        if (s.powerType == 0xFE && e.hp <= (int32_t)cost) return fail(77);   // (health)
        for (const auto& [item, n] : s.reagents)
            if (countPlayerItem(item) < n) return fail(92);                 // SPELL_FAILED_REAGENTS
        if (e.self && data_)   // WoWVita: a grimoire (SPELL_EFFECT_LEARN_SPELL at TARGET_UNIT_CASTER_PET: Spell::CheckCast)
            for (int k = 0; k < 3; ++k)
                if (s.eff[k].effect == E_LEARN_SPELL && s.eff[k].targetA == 5) {
                    Entity* pt = pet();
                    if (!pt || !pt->alive()) return fail(76);                       // SPELL_FAILED_NO_PET
                    const SpellX* ls = data_->sx(s.eff[k].trigger);
                    if (!ls) return fail(56);                                        // SPELL_FAILED_NOT_KNOWN
                    if (ls->spellLevel > pt->level) return fail(43);                 // SPELL_FAILED_LOWLEVEL
                    int active = 0;                                                  // CanTakeMoreActiveSpells: 4
                    bool replaces = false;
                    for (uint8_t i = 0; i < pt->spellCount; ++i) {
                        const SpellX* ps = data_->sx(pt->spells[i]);
                        if (ps && !ps->passive()) ++active;
                        if (pt->spells[i] == ls->id || sameSpellChain(pt->spells[i], ls->id)) replaces = true;
                    }
                    if (!replaces && !ls->passive() && active >= 4) return fail(137);   // SPELL_FAILED_TOO_MANY_SKILLS
                }
        if (e.self && data_)   // WoWVita: a teleport to a place of another map (Kalimdor: Orgrimmar, Darnassus...)
            for (int k = 0; k < 3; ++k)
                if (s.eff[k].effect == E_TELEPORT && (s.eff[k].targetB == 17 || s.eff[k].targetA == 17)) {
                    auto tp = data_->teleports.find(s.id);
                    if (tp == data_->teleports.end() || tp->second.map != data_->mapId) return fail(53);   // SPELL_FAILED_NOT_HERE
                }
        if (e.self && data_)   // WoWVita: an enchantment the pack does not know (the converter's "@enchfx" missing):
            for (int k = 0; k < 3; ++k)   // nothing would happen - no stone or poison spent for nothing
                if (s.eff[k].effect == 54 && !data_->enchants.count((uint32_t)s.eff[k].misc)) return fail(28);   // SPELL_FAILED_ERROR
        if (e.self)   // Spell::CheckItems: the totem items it needs, in the bags (Earth Totem, Fire Totem...)
            for (uint32_t item : s.totem)
                if (item && countPlayerItem(item) < 1) return fail(0x78);   // SPELL_FAILED_TOTEMS
        if (s.equipClass >= 0) {   // a weapon (2) or a shield (4 / 6) worn
            const CharacterProfile& p = profiles_[0];
            bool ok = false;
            for (int slot : {15, 16, 17}) {
                const ItemTemplate* it = p.equipment[(size_t)slot] ? data_->item(p.equipment[(size_t)slot]) : nullptr;
                if (it && (int32_t)it->cls == s.equipClass && (!s.equipSubMask || (s.equipSubMask & (1u << it->subclass)))) ok = true;
            }
            if (!ok) return fail(25);                                       // SPELL_FAILED_EQUIPPED_ITEM_CLASS
        }
        if (s.dmgClass == 3 && e.self) {   // a ranged attack: the weapon and its ammo (CMaNGOS Spell::CheckItems)
            bool weaponEffect58 = false;
            for (const SpellEffectX& x : s.eff) if (x.effect == 58 || x.effect == 121 || x.effect == 31 || x.effect == 17) weaponEffect58 = true;
            if (weaponEffect58) if (const uint8_t why = ammoCheck(e)) return fail(why);
        }
        bool usesCombo = false;
        usesCombo = (s.attrEx & 0x00500000) != 0;   // CMaNGOS NeedsComboPoints: SPELL_ATTR_EX_REQ_(TARGET_)COMBO_POINTS
        // (a finisher on oneself - Slice and Dice - needs only the points)
        if (usesCombo && (!e.comboPoints || (unitTarget(s) == 1 && e.comboTarget != targetGuid))) return fail(70);   // SPELL_FAILED_CANT_DO_THAT_YET...
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
        const float maxR = (s.rangeMax > 0 ? (float)spellModX(e, s, 5, (int32_t)s.rangeMax) : kReach) + t->boundingRadius + e.boundingRadius;   // SPELLMOD_RANGE
        const float d = dist(e, *t);
        if (d > maxR) return fail(89, t->guid);                            // SPELL_FAILED_OUT_OF_RANGE
        if (s.rangeMin > 0 && d < s.rangeMin) return fail(118, t->guid);    // SPELL_FAILED_TOO_CLOSE
        if (s.targetAuraState == 1 && t->hp * 5 > (int32_t)t->maxHp) return fail(103, t->guid);   // (Execute: under 20 %)
        if (s.mechanic && hasAuraType(*t, 77, -1)) {
            for (uint8_t i = 0; i < t->auraCount; ++i)
                if (t->auras[i].type == 77 && (uint32_t)t->auras[i].misc == s.mechanic) return fail(10, t->guid);
        }
        // (CMaNGOS Spell::CheckCast) behind the target: Backstab, Ambush, Garrote; Pounce is exempt
        if ((s.attrEx2 & 0x100000) && (s.attrEx & 0x200) && !(s.family == 7 && (s.familyFlags & 0x20000ull)) &&
            (hasInArc(*t, e, 3.14159265f) || !hasInArc(e, *t, 3.14159265f)))
            return fail(0x33, t->guid);                                       // SPELL_FAILED_NOT_BEHIND
        if (s.attr == 0x150010 && !hasInArc(e, *t, 3.14159265f)) return fail(0x36, t->guid);   // NOT_INFRONT (Gouge)
        if ((s.attrEx & 0x100) && inCombatNow(*t)) return fail(0x66, t->guid);   // ONLY_PEACEFUL_TARGETS: Sap
    }
    // "on the next swing": queued, paid when it lands (Heroic Strike, Cleave, Raptor Strike, Maul)
    if ((s.attr & 0x4) && !triggered) {
        e.nextSwingSpell = id;
        if (!e.victim && t && t != &e) startAttack(e, t->guid, now);
        return true;
    }
    if ((s.attrEx2 & 0x20) && !triggered && e.self) {
        // SPELL_ATTR_EX2_AUTOREPEAT_FLAG (Auto Shot, Shoot): it becomes the current auto repeat spell; the shots come from
        // Unit::_UpdateAutoRepeatSpell, the first after a 0.5 s wind-up
        if (e.autoRepeatSpell == s.id) return true;   // (already shooting)
        if (ammoCheck(e) == 0x43) pickAmmo();
        e.autoRepeatSpell = s.id;
        e.autoRepeatWindup = true;
        e.autoRepeatReadyMs = now;
        e.autoRepeatX = e.x; e.autoRepeatY = e.y;
        if (t && t != &e) selection_ = t->guid;
        return true;
    }
    if (s.gcdMs && !triggered) e.gcdEndMs = now + (uint32_t)std::max(0, spellModX(e, s, 21, (int32_t)s.gcdMs));   // SPELLMOD_GLOBAL_COOLDOWN
    const uint32_t castMs = s.castMs > 0 ? (uint32_t)std::max(0, spellModX(e, s, 10, (int32_t)s.castMs)) : 0;   // SPELLMOD_CASTING_TIME
    if (!triggered && !(s.attrEx & 0x20))   // (SPELL_ATTR_EX_ALLOW_WHILE_STEALTHED: Sprint, Vanish keep stealth)
        interruptAuras(e, 0x4 | (need == 1 ? 0x1000u : 0u));   // AURA_INTERRUPT_FLAG_ACTION (and ATTACKING)
    e.castTarget = t ? t->guid : e.guid;
    e.castTriggered = triggered;
    if (castMs > 0 && !triggered) {
        e.castSpell = id;
        e.castStartMs = now;
        e.castEndMs = now + castMs;
        e.castInterruptible = 1;
        e.channeling = false;
        emit(SimEvent::CastStarted, e.guid, e.castTarget, id, (int32_t)castMs, s.school);
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
    if (!e.channeling) return;
    const uint32_t id = e.castSpell;
    // WoWVita: a channel that ran its full time fires its once-at-the-end periodic trigger (Tame Beast: the 20 s tick of
    // its SPELL_AURA_PERIODIC_TRIGGER_SPELL - 13535 - lands as the channel ends)
    if (now >= e.castEndMs && data_)
        if (const SpellX* cs = data_->sx(id))
            for (const SpellEffectX& x : cs->eff)
                if (x.effect == E_APPLY_AURA && x.aura == A_PERIODIC_TRIGGER && x.trigger && x.amplitude >= (int32_t)durationX(e, *cs) &&
                    durationX(e, *cs) > 0) {
                    const uint64_t tg = e.castTarget;
                    e.channeling = false;
                    castX(e, x.trigger, tg, now, true);
                    e.channeling = true;
                    e.castTarget = tg;
                }
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
        cd(s.id, (uint32_t)std::max(0, spellModX(e, s, 11, (int32_t)s.cooldownMs)));   // SPELLMOD_COOLDOWN
        if (s.category) cd(kSlotCategoryBit | s.category, (uint32_t)std::max(0, spellModX(e, s, 11, (int32_t)s.catCooldownMs)));
    }
    emit(SimEvent::CastFinished, e.guid, t->guid, s.id, 0, s.school);
    int combo = 0;
    bool usesCombo = false;
    usesCombo = (s.attrEx & 0x00500000) != 0;   // CMaNGOS NeedsComboPoints: SPELL_ATTR_EX_REQ_(TARGET_)COMBO_POINTS
    if (usesCombo) combo = e.comboPoints;
    if (scriptX(e, s, t, now)) { if (usesCombo) { e.comboPoints = 0; e.comboTarget = 0; } return true; }
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
    if (s.dmgClass == 3 && e.self) takeAmmo(e);   // Spell::TakeAmmo: a ranged attack spends an arrow / a bullet
    if (weapon && s.dmgClass == 3 && t != &e && t->alive() && e.rangedMs) {
        // a ranged shot (CMaNGOS CalculateMinMaxDamage(RANGED_ATTACK)): weapon + ammo, + ranged AP / 14 x speed
        // (+ SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS on the target: Hunter's Mark)
        const int32_t rap = e.rangedAP + auraSum(*t, 127);
        int32_t dmg = rng_.range((int32_t)e.rangedMin, std::max((int32_t)e.rangedMin, (int32_t)e.rangedMax)) + weaponBonus;
        dmg += rap * (int32_t)e.rangedMs / 14000;
        dmg = dmg * weaponPct / 100;
        if ((int32_t)rng_.below(100) < 5) {   // (the melee swing's 5 % miss)
            emit(SimEvent::Damage, e.guid, t->guid, s.id, 0, 0, 1);   // a miss
        } else {
            if ((int32_t)rng_.below(10000) < meleeCritX(e, &s)) dmg *= 2;   // a crit
            // a wand's school (Spell::Spell: CLASSMASK_WAND_USERS - priest, mage, warlock - shoot with
            // GetRangedDamageSchoolMask, the ranged weapon's damage type)
            uint8_t school = s.school;
            if (e.self && data_) {
                const uint8_t cls = profiles_[0].cls;
                auto ws = data_->itemStats.find(profiles_[0].equipment[17]);
                if ((cls == 5 || cls == 8 || cls == 9) && ws != data_->itemStats.end())
                    for (const auto& [type, v] : ws->second) if (type == 200 && v > 0 && v < 7) school = (uint8_t)v;
            }
            applyDamage(e, *t, std::max(1, dmg), s.id, school, now);
        }
        weapon = false;
    }
    if (weapon && t != &e && t->alive()) {
        int32_t dmg = rng_.range((int32_t)e.dmgMin, (int32_t)e.dmgMax) + weaponBonus;
        dmg = dmg * weaponPct / 100;
        dmg += auraSum(e, A_ATTACK_POWER) * (int32_t)e.attackMs / 14000;
        const bool crit = (int32_t)rng_.below(10000) < meleeCritX(e, &s);
        if (crit) dmg *= 2;   // a crit
        applyDamage(e, *t, std::max(1, dmg), s.id, s.school, now);
        gainRage(e, dmg, true);
        if (t->alive()) { procAuras(e, *t, 0x10, now, crit ? 0x2 : 0x1, &s); procAuras(*t, e, 0x20, now, crit ? 0x2 : 0x1, &s); }   // a melee ability dealt / taken
        if (!e.victim) startAttack(e, t->guid, now);
    }
    if (usesCombo) { e.comboPoints = 0; e.comboTarget = 0; }
    // CMaNGOS Spell::prepare precast spells (here after the effects, so that they do not stop their own spell):
    // Weakened Soul after Power Word: Shield, Forbearance after Divine Shield / Protection, Blessing of Protection
    uint32_t pre = 0;
    if (s.family == 6 && (s.familyFlags & 0x1ull)) pre = 6788;
    else if (s.mechanic == 25 && s.id != 25771 && (s.family == 0 || s.family == 10)) pre = 25771;
    if (const SpellX* ps = pre ? data_->sx(pre) : nullptr)
        for (int k = 0; k < 3; ++k)
            if (ps->eff[k].effect == E_APPLY_AURA && t->alive()) applyAuraX(e, *t, *ps, k, now, 0);
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
                // Eviscerate (CMaNGOS SpellEffects SchoolDMG): + attack power x combo points x 3 %
                if (s.family == 8 && (s.familyFlags & 0x20000ull) && c.self && combo > 0)
                    dmg += (int32_t)((float)c.attackPower * (float)combo * 0.03f);
                dmg = spellModX(c, s, 0, dmg);   // SPELLMOD_DAMAGE
                if ((int32_t)rng_.below(10000) < spellCritX(c, s)) dmg = dmg * (100 + spellModX(c, s, 15, 50)) / 100;   // a crit (+ SPELLMOD_CRIT_DAMAGE_BONUS)
                applyDamage(c, *x, std::max(1, dmg), s.id, s.school, now);
            }
            break;
        case E_HEAL: case E_HEAL_MAX:
            for (Entity* x : ts) {
                if (!x->alive()) continue;
                int32_t v = e.effect == E_HEAL_MAX ? (int32_t)x->maxHp : spellModX(c, s, 0, effValue(c, s, k, combo));
                if (e.effect != E_HEAL_MAX && (int32_t)rng_.below(10000) < spellCritX(c, s)) v = v * (100 + spellModX(c, s, 15, 50)) / 100;
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
        case 61:   // SPELL_EFFECT_SEND_EVENT: the event's script summons (dbscripts_on_event command 10)
            if (data_) {
                auto it = data_->eventSummons.find((uint32_t)e.misc);
                if (it != data_->eventSummons.end())
                    for (const EventSummon& es : it->second)
                        summonCreature(es.entry, es.x, es.y, es.z, es.o, es.despawnMs, c.guid, now);
            }
            break;
        case 56:   // SPELL_EFFECT_SUMMON_PET (Summon Imp, Voidwalker...; without an entry: Call Pet): offline_pet.cpp
            if (c.self && e.misc > 0) summonPet(c, (uint32_t)e.misc, s.id, now);
            else if (c.self) callHunterPet(c, now);
            break;
        case 55:   // SPELL_EFFECT_TAME_CREATURE (13481 at the end of Tame Beast's channel)
            if (c.self && t && t != &c) tameCreature(c, *t, s.id, now);
            break;
        case E_TELEPORT:   // SPELL_EFFECT_TELEPORT_UNITS to TARGET_LOCATION_DATABASE (spell_target_position)
            if (c.self && &c == player_ && data_) {
                auto tp = data_->teleports.find(s.id);
                if (tp != data_->teleports.end() && tp->second.map == data_->mapId) {
                    teleportPending_ = true;
                    teleportX_ = tp->second.x; teleportY_ = tp->second.y; teleportZ_ = tp->second.z; teleportO_ = tp->second.o;
                }
            }
            break;
        case 54: {   // SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY: on the item the cast is at (a poison, a stone), else the main hand
            if (!c.self || &c != player_) break;
            CharacterProfile& p = profiles_[0];
            const int slot = c.castItemSlot >= 15 && c.castItemSlot <= 17 ? c.castItemSlot : 15;
            if (!p.equipment[(size_t)slot] || !data_ || !data_->enchants.count((uint32_t)e.misc)) break;
            const int32_t secs = effValue(c, s, k, 0);   // (its damage: the seconds it lasts, else an hour)
            p.tempEnchant[(size_t)(slot - 15)] = TempEnchant{(uint32_t)e.misc, (uint32_t)(secs > 0 ? secs : 3600) * 1000u,
                                                            p.equipment[(size_t)slot]};
            recomputeGear();
            break;
        }
        case 74: case 87: case 88: case 89: case 90:   // SPELL_EFFECT_SUMMON_TOTEM (_SLOT1..4): offline_pet.cpp
            summonTotem(c, s, k, now);
            break;
        case 102:   // SPELL_EFFECT_DISMISS_PET: the hunter's pet goes, kept for Call Pet
            if (c.self && pet()) { dismissPet(false); profiles_[0].petOut = false; }
            break;
        case 109:   // SPELL_EFFECT_RESURRECT_PET (Revive Pet): its health share
            if (c.self) callHunterPet(c, now, std::max(1, effValue(c, s, k, 0)));
            break;
        case E_LEARN_SPELL:   // SPELL_EFFECT_LEARN_SPELL: the session puts it in the book (SMSG_LEARNED_SPELL)
            if (e.targetA == 5) {   // at the pet (a grimoire): Spell::EffectLearnPetSpell, kept for this demon (pet_spell)
                Entity* pt = c.self ? pet() : nullptr;
                if (pt && pt->alive() && e.trigger && petLearn(*pt, e.trigger)) {
                    CharacterProfile& p = profiles_[0];
                    auto& pl = p.petLearned;
                    pl.erase(std::remove_if(pl.begin(), pl.end(), [&](const std::pair<uint32_t, uint32_t>& x) {
                                 return x.first == pt->entry && sameSpellChain(x.second, e.trigger); }), pl.end());
                    pl.emplace_back(pt->entry, e.trigger);
                    emit(SimEvent::PetChanged, pt->guid, c.guid, 0, 2);   // (the session sends SMSG_PET_SPELLS again)
                }
                break;
            }
            for (Entity* x : ts) if (x->self && e.trigger) emit(SimEvent::Message, x->guid, x->guid, e.trigger, 2);
            break;
        case E_DISPEL:   // one magic aura: a harmful one from a friend, a helpful one from an enemy
            for (Entity* x : ts) {
                const bool friendT = !canAttack(c, *x);
                for (int i = x->auraCount - 1; i >= 0; --i)
                    if (x->auras[i].positive != friendT && x->auras[i].school != 0 && !x->auras[i].passive) { removeAura(*x, i); break; }
            }
            break;
        case 79:   // SPELL_EFFECT_SANCTUARY (Vanish): out of every creature's threat
            for (Entity& o : pool_) {
                if (!o.used || o.objType != 3) continue;
                uint8_t n = 0;
                for (uint8_t i = 0; i < o.threatCount; ++i) if (o.threat[i].who != c.guid) o.threat[n++] = o.threat[i];
                o.threatCount = n;
                if (o.victim == c.guid) { o.victim = o.target = 0; }
            }
            if (c.victim) stopAttack(c, now);
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
    // (a spell of one's own on oneself - Divine Protection with its pacify - is a blessing)
    const bool positive = (positiveX(s) || (&c == &t && unitTarget(s) != 1)) && !canAttack(c, t);
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
    if (e.aura == A_PERIODIC_DAMAGE || e.aura == A_PERIODIC_LEECH || e.aura == A_PERIODIC_HEAL) a.amount = spellModX(c, s, 22, a.amount);   // SPELLMOD_DOT
    const uint32_t d = durationX(c, s, combo);
    // WoWVita: SPELL_AURA_MECHANIC_IMMUNITY (Vanish, Blink, Blessing of Freedom) keeps such auras off
    {
        const uint32_t mech = e.mechanic ? e.mechanic : s.mechanic;
        if (mech && !(e.aura == 77 && s.mechanic == (uint32_t)e.misc))   // (Weakened Soul itself has the mechanic it blocks)
            for (uint8_t i = 0; i < t.auraCount; ++i)
                if (t.auras[i].type == 77 && (uint32_t)t.auras[i].misc == mech) return;
    }
    a.endMs = d ? now + d : 0;
    a.amplitudeMs = e.amplitude > 0 ? (uint32_t)e.amplitude : 0;
    a.nextTickMs = a.amplitudeMs ? now + a.amplitudeMs : 0;
    a.charges = (uint8_t)std::min<uint32_t>(255, s.procCharges);
    switch (e.aura) {
        case A_SHAPESHIFT:   // a stance / form: the others go
            for (int i = t.auraCount - 1; i >= 0; --i)
                if (t.auras[i].type == A_SHAPESHIFT && t.auras[i].spell != s.id) removeAura(t, i);
            enterForm(t, (uint8_t)e.misc, now);
            break;
        case 77:   // SPELL_AURA_MECHANIC_IMMUNITY: what has that mechanic goes
            for (int i = t.auraCount - 1; i >= 0; --i) {
                const SpellX* o = t.auras[i].spell != s.id ? data_->sx(t.auras[i].spell) : nullptr;
                if (!o || t.auras[i].positive) continue;
                const uint32_t m = o->eff[t.auras[i].effIndex].mechanic ? o->eff[t.auras[i].effIndex].mechanic : o->mechanic;
                if (m == (uint32_t)e.misc) removeAura(t, i);
            }
            if (e.misc == 7) t.rootUntilMs = 0;
            if (e.misc == 12) t.stunnedUntilMs = 0;
            break;
        case 16:   // SPELL_AURA_MOD_STEALTH: whoever fights it and cannot see it lets go
            for (Entity& o : pool_)
                if (o.used && o.objType == 3 && o.alive() && o.victim == t.guid && !detectsStealth(o, t)) {
                    o.victim = o.target = 0;
                }
            break;
        case 11:   // SPELL_AURA_MOD_TAUNT (Mocking Blow)
            if (t.objType == 3) taunt(t, c.guid);
            break;
        case 103:   // SPELL_AURA_MOD_TOTAL_THREAT (Fade): less threat with every creature
            for (Entity& o : pool_) if (o.used && o.objType == 3 && o.alive()) addThreat(o, t.guid, a.amount);
            break;
        case A_STUN: case A_ROOT: case A_FEAR: case A_CONFUSE: case A_SILENCE: case A_PACIFY: case A_PACIFY_SILENCE: {
            SpellControl ctl;
            ctl.durationMs = d ? d : 10000;
            const Control type = e.aura == A_STUN ? Control::Stun : e.aura == A_ROOT ? Control::Root : e.aura == A_FEAR ? Control::Fear : e.aura == A_CONFUSE ? Control::Confuse : e.aura == A_SILENCE ? Control::Silence
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
    if (t.self && (e.aura == 29 || e.aura == 34 || e.aura == 142)) recomputeGear();   // stats, health, armour %: again
}

// ---------------------------------------------------------------- the class scripts CMaNGOS has for these
// (Spells/Scripts/Scripting/ClassScripts): true when the spell was done here
// ---------------------------------------------------------------- passives and talents
namespace {
// the aura types recomputeGear reads (stats, health, armour, attack power): a change of those wants it run again
bool statAura(uint8_t t) { return t == 29 || t == 34 || t == 133 || t == 137 || t == 142 || t == 22 || t == 99 || t == 124; }
}
void World::applyPassives(Entity& p, uint32_t now) {
    if (!data_ || !p.self || &p != player_) return;
    const CharacterProfile& pr = profiles_[0];
    bool gear = false;
    for (uint32_t id : pr.book) {
        const SpellX* s = data_->sx(id);
        if (!s || !s->passive()) continue;
        const bool formOk = !s->stances || (p.form && (s->stances & (1u << (p.form - 1))));
        for (int k = 0; k < 3; ++k) {
            const SpellEffectX& e = s->eff[k];
            if ((e.effect != E_APPLY_AURA && e.effect != E_AREA_AURA) || !e.aura || e.aura == A_SHAPESHIFT) continue;
            int have = -1;
            for (uint8_t i = 0; i < p.auraCount; ++i)
                if (p.auras[i].passive && p.auras[i].spell == id && p.auras[i].effIndex == k) { have = i; break; }
            if (formOk && have < 0) {
                Aura a;
                a.spell = id; a.caster = p.guid; a.type = e.aura; a.effIndex = (uint8_t)k; a.misc = e.misc;
                a.school = s->school; a.positive = true; a.passive = true; a.kind = (uint8_t)SpellKind::Buff;
                a.amount = effValue(p, *s, k, 0);
                a.amplitudeMs = e.amplitude > 0 ? (uint32_t)e.amplitude : 0;
                a.nextTickMs = a.amplitudeMs ? now + a.amplitudeMs : 0;
                a.charges = (uint8_t)std::min<uint32_t>(255, s->procCharges);
                addAura(p, a);
                gear = gear || statAura(e.aura);
            } else if (!formOk && have >= 0) {
                removeAura(p, have);
                gear = gear || statAura(e.aura);
            }
        }
    }
    // (a passive no longer in the book - a talent's lower rank - goes)
    for (int i = p.auraCount - 1; i >= 0; --i)
        if (p.auras[i].passive && std::find(pr.book.begin(), pr.book.end(), p.auras[i].spell) == pr.book.end()) {
            gear = gear || statAura(p.auras[i].type);
            removeAura(p, i);
        }
    if (gear) recomputeGear();
}

int World::talentRank(const WorldData::TalentInfo& t) const {
    const auto& book = profiles_[0].book;
    for (int r = (int)t.ranks.size(); r >= 1; --r)
        if (std::find(book.begin(), book.end(), t.ranks[(size_t)r - 1]) != book.end()) return r;
    return 0;
}

uint32_t World::talentPointsFree() const {
    if (!data_ || !player_) return 0;
    const uint32_t total = player_->level >= 10 ? (uint32_t)player_->level - 9 : 0;
    uint32_t spent = 0;
    for (const WorldData::TalentInfo& t : data_->talents) spent += (uint32_t)talentRank(t);
    return total > spent ? total - spent : 0;
}

// Player::LearnTalent: the class's tab, the next rank, a free point, the prerequisite's rank, 5 points a row in the tab
uint8_t World::learnTalent(uint32_t talentId, uint32_t rankIndex, uint32_t now) {
    if (!data_ || !player_) return 1;
    const WorldData::TalentInfo* t = data_->talent(talentId);
    if (!t) return 1;
    auto tab = data_->talentTabs.find(t->tab);
    if (tab == data_->talentTabs.end() || !(tab->second & (1u << (profiles_[0].cls - 1)))) return 2;
    const int cur = talentRank(*t);
    if ((int)rankIndex != cur || rankIndex >= t->ranks.size()) return 3;   // (only the next rank)
    if (!talentPointsFree()) return 4;
    if (t->reqTalent) {
        const WorldData::TalentInfo* req = data_->talent(t->reqTalent);
        if (req && talentRank(*req) < (int)t->reqRank + 1) return 5;
    }
    uint32_t spent = 0;
    for (const WorldData::TalentInfo& o : data_->talents) if (o.tab == t->tab) spent += (uint32_t)talentRank(o);
    if (spent < (uint32_t)t->row * 5) return 6;
    const uint32_t spell = t->ranks[rankIndex];
    if (!data_->sx(spell)) return 7;
    auto& book = profiles_[0].book;
    for (size_t r = 0; r < rankIndex; ++r)   // (the lower ranks go: Player::learnSpell of a talent's rank)
        book.erase(std::remove(book.begin(), book.end(), t->ranks[r]), book.end());
    book.push_back(spell);
    // a talent that teaches a spell (Mortal Strike's rank learns 12294): SPELL_EFFECT_LEARN_SPELL
    if (const SpellX* s = data_->sx(spell))
        for (const SpellEffectX& e : s->eff)
            if (e.effect == E_LEARN_SPELL && e.trigger && std::find(book.begin(), book.end(), e.trigger) == book.end())
                emit(SimEvent::Message, player_->guid, player_->guid, e.trigger, 2);   // (the session teaches it)
    applyPassives(*player_, now);
    return 0;
}

uint32_t World::talentResetCost() const {
    const CharacterProfile& p = profiles_[0];
    const uint32_t gold = 10000, last = p.talentResetCostLast;
    if (last < 1 * gold) return 1 * gold;
    if (last < 5 * gold) return 5 * gold;
    if (last < 10 * gold) return 10 * gold;
    const uint32_t nowSec = (uint32_t)std::time(nullptr);
    const uint32_t months = nowSec > p.talentResetTime ? (nowSec - p.talentResetTime) / (30u * 24u * 3600u) : 0;
    if (months > 0) {
        const int64_t c = (int64_t)last - (int64_t)5 * gold * months;
        return (uint32_t)std::max<int64_t>(10 * gold, c);
    }
    return std::min<uint32_t>(50 * gold, last + 5 * gold);
}

bool World::resetTalents(std::vector<uint32_t>& removed, uint32_t now) {
    if (!data_ || !player_) return false;
    CharacterProfile& p = profiles_[0];
    bool any = false;
    for (const WorldData::TalentInfo& t : data_->talents) if (talentRank(t)) any = true;
    if (!any) return false;
    const uint32_t cost = talentResetCost();
    if (player_->money < cost) return false;
    for (const WorldData::TalentInfo& t : data_->talents) {
        auto tab = data_->talentTabs.find(t.tab);
        if (tab == data_->talentTabs.end() || !(tab->second & (1u << (p.cls - 1)))) continue;
        for (uint32_t r : t.ranks) {
            if (std::find(p.book.begin(), p.book.end(), r) == p.book.end()) continue;
            p.book.erase(std::remove(p.book.begin(), p.book.end(), r), p.book.end());
            removed.push_back(r);
            for (int i = player_->auraCount - 1; i >= 0; --i)   // (an active talent's own auras too: Sweeping Strikes)
                if (player_->auras[i].spell == r) removeAura(*player_, i);
        }
    }
    player_->money -= cost;
    p.money = player_->money;
    p.talentResetCostLast = cost;
    p.talentResetTime = (uint32_t)std::time(nullptr);
    applyPassives(*player_, now);
    recomputeGear();
    return true;
}

// WoWVita: the temporary enchantment of an equipped weapon - only while that item is the one in the slot
const ItemEnchant* World::tempEnchantOn(int slot) const {
    if (slot < 15 || slot > 17 || !data_) return nullptr;
    const CharacterProfile& p = profiles_[0];
    const TempEnchant& t = p.tempEnchant[(size_t)(slot - 15)];
    if (!t.id || !t.msLeft || t.item != p.equipment[(size_t)slot]) return nullptr;
    auto it = data_->enchants.find(t.id);
    return it == data_->enchants.end() ? nullptr : &it->second;
}

// Player::UpdateEnchantTime: the time runs while the character is in the world; at its end the enchantment goes
void World::tickEnchants(uint32_t elapsedMs) {
    if (!player_) return;
    CharacterProfile& p = profiles_[0];
    bool changed = false;
    for (size_t i = 0; i < 3; ++i) {
        TempEnchant& t = p.tempEnchant[i];
        if (!t.id) continue;
        if (t.item != p.equipment[15 + i]) continue;   // (the item is not worn: its time stands)
        if (t.msLeft <= elapsedMs) { t = TempEnchant(); changed = true; }
        else t.msLeft -= elapsedMs;
    }
    if (changed) recomputeGear();
}

bool World::scriptX(Entity& c, const SpellX& s, Entity* t, uint32_t now) {
    // Flametongue Weapon Proc (CMaNGOS Spell::EffectDummy, shaman 0x200000): damage x 0.01 x the weapon's speed as
    // Flametongue Attack (10444)
    if (s.family == 11 && (s.familyFlags & 0x200000ull) && s.eff[0].effect == 3 && t && t != &c) {
        const float speed = (float)c.attackMs / 1000.0f;
        const int32_t dmg = std::max(1, (int32_t)((float)effValue(c, s, 0) * 0.01f * speed));
        applyDamage(c, *t, dmg, 10444, 2, now);
        return true;
    }
    // Execute (5308 and ranks): effect 0 + the rage left x DmgMultiplier (CMaNGOS rage in tenths), all rage gone
    if (s.family == 4 && (s.familyFlags & 0x20000000ull) && t && t != &c) {
        const int32_t dmg = effValue(c, s, 0) + (int32_t)((float)(c.power * 10) * s.eff[0].dmgMult);
        c.power = 0;
        applyDamage(c, *t, std::max(1, dmg), s.id, 0, now);
        emit(SimEvent::PowerChanged, c.guid, 0, 0, c.power);
        return true;
    }
    // Life Tap (1454 and ranks): health into mana
    if (s.family == 5 && (s.familyFlags & 0x40000ull) && s.eff[0].effect == 3) {
        const int32_t v = effValue(c, s, 0);
        if (c.hp <= v) { emit(SimEvent::CastFailed, c.guid, 0, s.id, 21, 0, 0); return true; }   // SPELL_FAILED_FIZZLE
        c.hp -= v;
        c.power = std::min<int32_t>((int32_t)c.maxPower, c.power + v);
        emit(SimEvent::Damage, c.guid, c.guid, s.id, v, s.school, 0);
        emit(SimEvent::PowerChanged, c.guid, 0, 0, c.power);
        return true;
    }
    // Judgement (CMaNGOS spell_judgement): the seal's third effect holds the judgement to cast; the seal goes
    if (s.family == 10 && (s.familyFlags & 0x800000ull) && t && t != &c) {
        for (int i = 0; i < c.auraCount; ++i) {
            const SpellX* seal = data_->sx(c.auras[i].spell);
            if (!seal || c.auras[i].type != 4 || c.auras[i].effIndex != 2 || seal->family != 10 || !(seal->familyFlags & 0x0A000200ull))
                continue;
            const uint32_t j = (uint32_t)(seal->eff[2].base + 1);
            removeAuraSpell(c, seal->id);
            if (j > 1) castX(c, j, t->guid, now, true);
            break;
        }
        return true;
    }
    // Tame Beast (CMaNGOS Hunter.cpp TameBeastDummy): 13535 casts 13481 at the channel's target
    if (s.id == 13535) {
        const uint64_t tg = c.channeling ? c.castTarget : (t && t != &c ? t->guid : 0);
        const Entity* beast = tg ? entity(tg) : nullptr;
        if (beast && beast->alive() && !beast->ownerGuid) castX(c, 13481, tg, now, true);   // (once: then it is ours)
        return true;
    }
    // Vanish (CMaNGOS VanishRogue::OnCast): the best Stealth known, its cooldown no matter; the effects still run
    if (s.family == 8 && (s.familyFlags & 0x800ull) && c.self) {
        uint32_t best = 0;
        for (uint32_t id : profiles_[0].book) {
            const SpellX* o = data_->sx(id);
            if (o && o->family == 8 && (o->familyFlags & 0x400000ull) && (!best || o->spellLevel > data_->sx(best)->spellLevel)) best = id;
        }
        if (best) castX(c, best, c.guid, now, true);
        return false;
    }
    // Blink (SPELL_EFFECT_LEAP): 20 yards ahead
    for (const SpellEffectX& e : s.eff)
        if (e.effect == 29) {
            const float d = e.radius > 0 ? e.radius : 20.0f;
            c.castDestX = c.x; c.castDestY = c.y; c.castDestZ = c.z;
            c.x += std::cos(c.o) * d;
            c.y += std::sin(c.o) * d;
            applyGround(c);
            for (int k = 0; k < c.auraCount;) {   // free of roots and stuns (its aura effects)
                if (c.auras[k].type == A_STUN || c.auras[k].type == A_ROOT) { removeAura(c, k); continue; }
                ++k;
            }
            c.stunnedUntilMs = c.rootUntilMs = 0;
            emit(SimEvent::Message, c.guid, c.guid, s.id, 1);
            return true;
        }
    return false;
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
        case 24:   // SPELL_AURA_PERIODIC_ENERGIZE: misc the power (4: a hunter pet's happiness - Feed Pet)
            if (e.alive() && a.misc == 4 && e.petHunter) e.petHappiness = std::min(1000000, e.petHappiness + a.amount);
            else if (e.alive() && a.misc == e.powerType) {
                e.power = std::min<int32_t>((int32_t)e.maxPower, e.power + a.amount);
                emit(SimEvent::PowerChanged, e.guid, 0, 0, e.power);
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

// ---------------------------------------------------------------- forms (CMaNGOS Aura::HandleAuraModShapeshift,
// HandleShapeshiftBoosts, Player::InitDataForForm)
namespace {
bool allianceRace(uint8_t r) { return r == 1 || r == 3 || r == 4 || r == 7; }
void formBoosts(uint8_t form, uint32_t out[2]) {
    out[0] = out[1] = 0;
    switch (form) {
        case 1: out[0] = 3025; break;                    // cat
        case 2: out[0] = 5420; break;                    // tree
        case 3: out[0] = 5419; break;                    // travel
        case 4: out[0] = 5421; break;                    // aquatic
        case 5: out[0] = 1178; out[1] = 21178; break;    // bear
        case 8: out[0] = 9635; out[1] = 21178; break;    // dire bear
        case 17: out[0] = 21156; break;                  // battle stance
        case 18: out[0] = 7376; break;                   // defensive stance
        case 19: out[0] = 7381; break;                   // berserker stance
        case 31: out[0] = 24905; break;                  // moonkin
        default: break;
    }
}
}  // namespace

void World::enterForm(Entity& t, uint8_t form, uint32_t now) {
    const uint8_t race = t.self ? profiles_[0].race : 1;
    const bool ally = allianceRace(race);
    uint32_t display = 0;
    uint8_t power = 0xFF;   // the form's power (0xFF: the class's own)
    switch (form) {
        case 1: display = ally ? 892 : 8571; power = 3; break;
        case 2: display = 864; break;
        case 3: display = 632; break;
        case 4: display = 2428; break;
        case 5: case 8: display = ally ? 2281 : 2289; power = 1; break;
        case 16: display = 4613; break;
        case 31: display = ally ? 15374 : 15375; break;
        case 17: case 18: case 19: power = 1; break;
        default: break;
    }
    // a shifted form shakes off roots and snares (those with a mechanic)
    if (form == 1 || form == 2 || form == 3 || form == 4 || form == 5 || form == 8 || form == 31 || form == 16) {
        for (int i = t.auraCount - 1; i >= 0; --i) {
            if (t.auras[i].type != A_ROOT && t.auras[i].type != A_DEC_SPEED) continue;
            const SpellX* o = data_ ? data_->sx(t.auras[i].spell) : nullptr;
            const uint32_t m = o ? (o->eff[t.auras[i].effIndex].mechanic ? o->eff[t.auras[i].effIndex].mechanic : o->mechanic) : 0;
            if (form != 16 && m) removeAura(t, i);
        }
        if (form != 16) t.rootUntilMs = 0;
    }
    t.form = form;
    t.formDisplay = display;
    if (power != 0xFF && power != t.powerType) {   // energy / rage from nothing
        if (!t.formPower) { t.savedPowerType = t.powerType; t.savedPower = t.power; t.savedMaxPower = t.maxPower; t.formPower = true; }
        t.powerType = power;
        t.maxPower = 100;
        t.power = 0;
    } else if (form == 17 || form == 18 || form == 19) {
        t.power = 0;   // a stance change keeps no rage (no Tactical Mastery)
    } else if (form == 1 || form == 5 || form == 8) {
        t.power = 0;
    }
    emit(SimEvent::PowerChanged, t.guid, 0, 0, t.power);
    uint32_t boost[2];
    formBoosts(form, boost);
    for (uint32_t b : boost)
        if (const SpellX* bs = b && data_ ? data_->sx(b) : nullptr)
            for (int k = 0; k < 3; ++k)
                if (bs->eff[k].effect == E_APPLY_AURA) applyAuraX(t, t, *bs, k, now, 0);
    if (t.self) recomputeGear();
}

void World::leaveForm(Entity& t, uint8_t form) {
    uint32_t boost[2];
    formBoosts(form, boost);
    t.form = 0;
    t.formDisplay = 0;
    for (uint32_t b : boost) if (b) removeAuraSpell(t, b);
    if (t.formPower) {
        t.powerType = t.savedPowerType;
        t.power = t.savedPower;
        t.maxPower = t.savedMaxPower;
        t.formPower = false;
        emit(SimEvent::PowerChanged, t.guid, 0, 0, t.power);
    }
    if (t.self) recomputeGear();
}

// ---------------------------------------------------------------- ranged: ammo and Auto Shot
// CMaNGOS Spell::CheckItems for SPELL_EFFECT_WEAPON_DAMAGE with RANGED_ATTACK: a ranged weapon; a thrown one is its own
// ammo; a bow / crossbow wants arrows and a gun bullets in PLAYER_AMMO_ID, and one of them in the bags; a wand none
uint8_t World::ammoCheck(const Entity& e) const {
    if (!e.self || !data_) return 0;
    const CharacterProfile& p = profiles_[0];
    const ItemTemplate* rw = p.equipment[17] ? data_->item(p.equipment[17]) : nullptr;
    if (!rw) return 29;   // SPELL_FAILED_EQUIPPED_ITEM
    auto inBags = [&](uint32_t entry) {
        for (uint8_t i = 0; i < p.bagCount; ++i) if (p.bag[i].entry == entry && p.bag[i].count) return true;
        return false;
    };
    switch (rw->subclass) {
        case 16: return inBags(rw->entry) || true ? 0 : 0x43;   // thrown (the worn one)
        case 2: case 3: case 18: {
            const ItemTemplate* am = p.ammoId ? data_->item(p.ammoId) : nullptr;
            if (!am || am->cls != 6) return 0x43;   // SPELL_FAILED_NO_AMMO
            if ((rw->subclass == 3) != (am->subclass == 3) || (rw->subclass != 3 && am->subclass != 2)) return 0x43;
            if (!inBags(p.ammoId)) return 0x43;
            return 0;
        }
        default: return 0;   // a wand
    }
}

void World::takeAmmo(Entity& e) {
    if (!e.self || !data_) return;
    CharacterProfile& p = profiles_[0];
    const ItemTemplate* rw = p.equipment[17] ? data_->item(p.equipment[17]) : nullptr;
    if (!rw || rw->subclass == 19 || rw->subclass == 16 || !p.ammoId) return;   // a wand: none; thrown: (not stackable here)
    for (int i = 0; i < p.bagCount; ++i)
        if (p.bag[i].entry == p.ammoId && p.bag[i].count) {
            const uint8_t before = p.bagCount;
            takeFromBag(i, 1);
            emit(SimEvent::Message, e.guid, e.guid, p.ammoId, 4, (uint8_t)i, before);   // (the session sends the bag again)
            return;
        }
}

// WoWVita: our client has no paper-doll ammo slot to drop arrows into, so a character without fitting ammo in it gets
// the first fitting stack of its bags (what the original's player would do by hand)
bool World::pickAmmo() {
    if (!data_ || !player_) return false;
    CharacterProfile& p = profiles_[0];
    const ItemTemplate* rw = p.equipment[17] ? data_->item(p.equipment[17]) : nullptr;
    if (!rw || (rw->subclass != 2 && rw->subclass != 3 && rw->subclass != 18)) return false;
    if (!ammoCheck(*player_)) return false;
    for (uint8_t i = 0; i < p.bagCount; ++i) {
        const ItemTemplate* am = data_->item(p.bag[i].entry);
        if (!am || am->cls != 6 || !p.bag[i].count) continue;
        if ((rw->subclass == 3 && am->subclass == 3) || (rw->subclass != 3 && am->subclass == 2)) {
            p.ammoId = am->entry;
            recomputeGear();
            return true;
        }
    }
    return false;
}

void World::stopAutoRepeat(Entity& e) {
    if (!e.autoRepeatSpell) return;
    const uint32_t s = e.autoRepeatSpell;
    e.autoRepeatSpell = 0;
    e.autoRepeatWindup = false;
    emit(SimEvent::Message, e.guid, e.guid, s, 5);   // SMSG_CANCEL_AUTO_REPEAT
}

// CMaNGOS Unit::_UpdateAutoRepeatSpell: moving pauses it (0.5 s wind-up again), a shot when the spell is ready at the
// current target, the next after the ranged weapon's speed (Player::AddCooldown: RecoveryTime + UNIT_FIELD_RANGEDATTACKTIME)
void World::tickAutoRepeat(Entity& e, uint32_t now) {
    const SpellX* s = data_ ? data_->sx(e.autoRepeatSpell) : nullptr;
    if (!s) { e.autoRepeatSpell = 0; return; }
    const bool moved = std::hypot(e.x - e.autoRepeatX, e.y - e.autoRepeatY) > 0.05f;
    e.autoRepeatX = e.x; e.autoRepeatY = e.y;
    if (moved || e.castSpell) { e.autoRepeatWindup = true; return; }
    if (e.autoRepeatWindup) {
        e.autoRepeatReadyMs = std::max(e.autoRepeatReadyMs, now + 500);
        e.autoRepeatWindup = false;
        return;
    }
    if (now < e.autoRepeatReadyMs) return;
    Entity* t = selection_ ? entity(selection_) : nullptr;
    if (!t || !t->alive() || !canAttack(e, *t)) { stopAutoRepeat(e); return; }
    const float d = std::hypot(t->x - e.x, t->y - e.y);
    uint8_t why = 0;
    if (s->rangeMax > 0 && d > s->rangeMax + t->combatReach) why = 0x56;   // SPELL_FAILED_OUT_OF_RANGE
    else if (s->rangeMin > 0 && d < s->rangeMin) why = 0x76;              // SPELL_FAILED_TOO_CLOSE
    else why = ammoCheck(e);
    if (why) {
        emit(SimEvent::CastFailed, e.guid, t->guid, s->id, why, 0, 0);
        stopAutoRepeat(e);
        return;
    }
    e.o = std::atan2(t->y - e.y, t->x - e.x);
    const bool wasTriggered = e.castTriggered;
    e.castTriggered = true;
    finishX(e, *s, t->guid, now);   // the shot (TRIGGERED_AUTOREPEAT: no cast bar, no price, no GCD)
    e.castTriggered = wasTriggered;
    e.autoRepeatReadyMs = now + std::max<uint32_t>(500, e.rangedMs ? e.rangedMs : 2000) + (uint32_t)std::max<int32_t>(0, (int32_t)s->cooldownMs);
}

// ---------------------------------------------------------------- facing, combat, stealth
// CMaNGOS GameObjectFocusCheck: a spell focus object (type 8) of that focus id (data0) within its distance (data1)
bool World::nearSpellFocus(const Entity& e, uint32_t focus) const {
    if (!data_) return false;
    for (const GoSpawn& g : data_->goSpawns) {
        auto it = data_->goTemplates.find(g.entry);
        if (it == data_->goTemplates.end() || it->second.type != 8 || it->second.data[0] != focus) continue;
        const float dx = g.x - e.x, dy = g.y - e.y, dz = g.z - e.z;
        const float dist = (float)it->second.data[1];
        if (dx * dx + dy * dy + dz * dz <= dist * dist) return true;
    }
    return false;
}

bool World::hasInArc(const Entity& from, const Entity& to, float arc) {
    float a = std::atan2(to.y - from.y, to.x - from.x) - from.o;
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return std::fabs(a) <= arc / 2;
}

bool World::inCombatNow(const Entity& e) const {
    if (e.victim) return true;
    for (const Entity& o : pool_)
        if (o.used && o.objType == 3 && o.alive() && o.victim == e.guid) return true;
    return false;
}

// CMaNGOS Unit::IsVisibleForOrDetect + VisibilityData::GetStealthVisibilityDistance: 30 points + 5 a viewer level over 1,
// less the stealth's strength, x 0.3 yards; only in front of the viewer; at least the combat reach
bool World::detectsStealth(const Entity& v, const Entity& h) const {
    const int32_t strength = auraSum(h, 16);
    if (strength <= 0 && !hasAuraType(h, 16)) return true;
    if (!hasInArc(v, h, 3.14159265f)) return false;
    const float range = std::max(1.5f, (float)(30 + ((int32_t)v.level - 1) * 5 - strength) * 0.3f);
    return std::hypot(h.x - v.x, h.y - v.y) <= range;
}

}  // namespace offline
}  // namespace w3n
