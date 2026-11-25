// offline_pet.cpp — WoWVita: the warlock's summoned demons (SPELL_EFFECT_SUMMON_PET: Imp, Voidwalker, Succubus,
// Felhunter) in the offline world, after CMaNGOS:
//   - Pet::InitStatsForLevel (SUMMON_PET): health, mana, armour and stats from pet_levelstats at the owner's level,
//     melee damage from creature_template_classlevelstats (the exporter's "PETS" section), the creature's attack speed;
//   - Pet::addSpell: petcreateinfo_spell, autocast on unless SPELL_ATTR_EX_NO_AUTOCAST_AI / passive;
//   - PetAI::UpdateAI: follow the owner (1.5 yd at 90 degrees), stay, assist the owner in combat unless passive,
//     defend itself, the Imp keeps 25 yd and does not melee (m_attackDistance / m_meleeEnabled), autocast;
//   - CharmInfo commands (stay / follow / attack) and reactions (passive / defensive / aggressive);
//   - Pet::GenerateNameForPet: pet_name_generation, a first half and a second half.
// A summoned pet stays with its owner: it is no spawn of the grid, kills by it count as the owner's, and it comes back
// at login ("PET1" in the save). One pet at a time: a new summon sends the old one away.
#include <algorithm>
#include <cmath>

#include "offline_core.hpp"

namespace w3n {
namespace offline {

namespace {
float pdist(float ax, float ay, float bx, float by) { return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }
constexpr float kPetFollowDist = 1.5f, kPetFollowAngle = 1.5707963f;
constexpr float kPetMeleeReach = 5.0f;
}  // namespace

Entity* World::pet() { return petGuid_ ? entity(petGuid_) : nullptr; }
const Entity* World::pet() const { return petGuid_ ? entity(petGuid_) : nullptr; }

bool World::summonPet(Entity& owner, uint32_t entry, uint32_t spell, uint32_t now, const std::string& name) {
    if (!owner.self || !data_) return false;
    auto it = data_->pets.find(entry);
    if (it == data_->pets.end()) return false;
    const PetTemplate& t = it->second;
    dismissPet(false);
    const uint32_t number = ++petCounter_;
    const uint64_t guid = 0xF140000000000000ull | ((uint64_t)(entry & 0xFFFFFF) << 24) | (uint64_t)number;
    Entity& e = allocate(guid);
    if (!e.used) return false;
    e.objType = 3;
    e.entry = entry;
    e.ownerGuid = owner.guid;
    e.petNumber = number;
    e.createdBySpell = spell;
    e.petCommand = 1;   // follow
    e.petReact = 1;     // defensive
    e.level = owner.level;
    e.faction = owner.faction;
    e.displayId = t.displayId;
    e.scale = t.scale;
    e.speed = t.speed;
    e.attackMs = t.attackMs;
    e.petMelee = spell != 688;   // the Imp casts (PetAI m_meleeEnabled), the others fight
    applyPetLevel(e);
    e.hp = (int32_t)e.maxHp;
    e.power = (int32_t)e.maxPower;
    e.spellCount = 0;
    for (uint32_t s : t.spells)
        if (e.spellCount < e.spells.size() && data_->sx(s)) e.spells[e.spellCount++] = s;
    const CharacterProfile& pr = profiles_[0];
    e.petAutocastOff = pr.petEntry == entry ? pr.petAutocastOff : 0;
    for (uint8_t i = 0; i < e.spellCount; ++i) {   // CMaNGOS IsAutocastable: not passive, not NO_AUTOCAST_AI
        const SpellX* s = data_->sx(e.spells[i]);
        if (s && (s->passive() || (s->attrEx & 0x20000))) e.petAutocastOff |= (uint8_t)(1u << i);
    }
    const float a = owner.o + kPetFollowAngle;
    e.x = owner.x + std::cos(a) * kPetFollowDist;
    e.y = owner.y + std::sin(a) * kPetFollowDist;
    e.z = owner.z;
    e.o = owner.o;
    e.homeX = e.x; e.homeY = e.y; e.homeZ = e.z;
    applyGround(e);
    // Pet::GenerateNameForPet: one first half, one second half
    if (!name.empty()) e.petName = name;
    else if (!t.nameHalves[0].empty() && !t.nameHalves[1].empty())
        e.petName = t.nameHalves[0][rng_.below((uint32_t)t.nameHalves[0].size())] +
                    t.nameHalves[1][rng_.below((uint32_t)t.nameHalves[1].size())];
    else e.petName = t.name;
    petGuid_ = guid;
    CharacterProfile& p = profiles_[0];
    p.petEntry = entry;
    p.petSpell = spell;
    p.petName = e.petName;
    p.petAutocastOff = e.petAutocastOff;
    emit(SimEvent::Appear, guid, 0, 0, 0);
    emit(SimEvent::PetChanged, guid, owner.guid, spell, 1);
    (void)now;
    return true;
}

void World::applyPetLevel(Entity& e) {
    if (!data_) return;
    auto it = data_->pets.find(e.entry);
    if (it == data_->pets.end() || it->second.levels.empty()) return;
    const PetTemplate& t = it->second;
    const PetLevel* l = nullptr;
    for (int L = e.level; L >= 1 && !l; --L) {   // the nearest level the table has at or under the pet's
        auto li = t.levels.find((uint8_t)L);
        if (li != t.levels.end()) l = &li->second;
    }
    if (!l) l = &t.levels.begin()->second;
    e.maxHp = std::max<uint32_t>(1, l->hp);
    e.maxPower = l->mana;
    e.powerType = 0;
    e.armor = l->armor;
    e.statStr = l->str; e.statAgi = l->agi; e.statSta = l->sta; e.statInt = l->inte; e.statSpi = l->spi;
    e.dmgMin = (uint32_t)std::max(1.0f, l->dmgMin);
    e.dmgMax = (uint32_t)std::max((float)e.dmgMin, l->dmgMax);
    // Pet::UpdateAttackPowerAndDamage: the Imp str - 10, the others 2 x str - 20; its bonus to a swing
    e.attackPower = std::max(0, e.entry == 416 ? (int32_t)l->str - 10 : 2 * (int32_t)l->str - 20);
    const uint32_t bonus = (uint32_t)(e.attackPower * (int32_t)e.attackMs / 14000);
    e.dmgMin += bonus; e.dmgMax += bonus;
}

void World::storePet() {
    const Entity* e = pet();
    if (!e || !e->petHunter) return;
    CharacterProfile& p = profiles_[0];
    p.petHunter = true;
    p.petLevel = (uint8_t)e->level;
    p.petXp = e->petXp;
    p.petHp = e->alive() ? (uint32_t)std::max(1, e->hp) : 0;
    p.petHappiness = e->petHappiness;
    p.petLoyaltyLevel = e->petLoyaltyLevel;
    p.petLoyaltyPoints = e->petLoyaltyPoints;
    p.petLoyaltyXpNeed = e->petLoyaltyXpNeed;
    p.petAutocastOff = e->petAutocastOff;
    p.petName = e->petName;
}

void World::dismissPet(bool forget) {
    storePet();
    Entity* e = pet();
    const uint64_t guid = petGuid_;
    petGuid_ = 0;
    if (forget) {
        CharacterProfile& p = profiles_[0];
        p.petEntry = 0; p.petSpell = 0; p.petName.clear(); p.petHunter = false; p.petHp = 0;
    }
    if (!e) return;
    for (Entity& o : pool_)   // nobody keeps fighting it
        if (o.used && o.objType == 3) {
            if (o.victim == guid) o.victim = o.target = 0;
            uint8_t n = 0;
            for (uint8_t i = 0; i < o.threatCount; ++i) if (o.threat[i].who != guid) o.threat[n++] = o.threat[i];
            o.threatCount = n;
        }
    emit(SimEvent::PetChanged, guid, e->ownerGuid, 0, 0);
    emit(SimEvent::Vanish, guid, 0, 0, 0);
    *e = Entity();
}

void World::petCommand(uint8_t command, uint64_t target, uint32_t now) {
    Entity* e = pet();
    if (!e || !e->alive()) return;
    switch (command) {
        case 0:   // COMMAND_STAY
            e->petCommand = 0;
            e->victim = e->target = 0;
            e->homeX = e->x; e->homeY = e->y; e->homeZ = e->z;
            e->moving = false;
            break;
        case 1:   // COMMAND_FOLLOW: also breaks off the fight
            e->petCommand = 1;
            e->victim = e->target = 0;
            if (e->castSpell) cancelCast(*e, 0, now);
            break;
        case 2: {   // COMMAND_ATTACK
            Entity* t = entity(target);
            if (!t || !t->alive() || !canAttack(*e, *t)) { emit(SimEvent::AttackError, e->guid, target, 0, 3); return; }
            e->target = t->guid;
            e->victim = e->petMelee ? t->guid : 0;
            e->petCommand = e->petCommand == 0 ? 0 : 1;
            break;
        }
        case 3:   // COMMAND_DISMISS (CMSG_PET_ABANDON for a demon too)
            dismissPet(true);
            break;
        default: break;
    }
}

void World::setPetAutocast(uint32_t spell, bool on) {
    Entity* e = pet();
    if (!e) return;
    for (uint8_t i = 0; i < e->spellCount; ++i)
        if (e->spells[i] == spell) {
            if (on) e->petAutocastOff &= (uint8_t)~(1u << i);
            else e->petAutocastOff |= (uint8_t)(1u << i);
        }
    profiles_[0].petAutocastOff = e->petAutocastOff;
}

bool World::petCast(uint32_t spell, uint64_t target, uint32_t now) {
    Entity* e = pet();
    if (!e || !e->alive()) return false;
    bool known = false;
    for (uint8_t i = 0; i < e->spellCount; ++i) known = known || e->spells[i] == spell;
    if (!known) return false;
    const SpellX* s = data_ ? data_->sx(spell) : nullptr;
    if (!s) return false;
    return castX(*e, spell, target ? target : e->target, now, false);
}

// PetAI::UpdateAI (one decision a tick)
void World::tickPet(Entity& e, uint32_t now) {
    Entity* owner = player_;
    if (e.dead) {
        if (e.respawnAtMs && now >= e.respawnAtMs) dismissPet(false);   // the corpse goes; the demon can be summoned again
        return;
    }
    if (!owner || owner->guid != e.ownerGuid) { dismissPet(false); return; }
    if (!owner->alive()) { e.victim = e.target = 0; }
    if (e.petHunter) tickHunterPet(e, *owner, now);
    if (!e.used || !e.alive()) return;
    if (!e.petHunter && e.level != owner->level) {   // Pet::SynchronizeLevelWithOwner (a demon; a beast earns its levels)
        e.level = owner->level;
        const uint32_t oldMax = e.maxHp;
        applyPetLevel(e);
        e.hp = std::min<int32_t>((int32_t)e.maxHp, e.hp + (int32_t)e.maxHp - (int32_t)oldMax);
    }
    // lost far behind (a flight, a teleport): at the owner's side again
    if (pdist(e.x, e.y, owner->x, owner->y) > 80.0f) {
        const float a = owner->o + kPetFollowAngle;
        e.x = owner->x + std::cos(a) * kPetFollowDist;
        e.y = owner->y + std::sin(a) * kPetFollowDist;
        e.z = owner->z;
        e.victim = e.target = 0;
        e.moving = false;
        e.netMoveState = 1;
        applyGround(e);
    }
    Entity* victim = e.target ? entity(e.target) : nullptr;
    if (victim && (!victim->alive() || !canAttack(e, *victim) || victim->aiState == 3)) { e.victim = e.target = 0; victim = nullptr; }
    if (!victim && e.petReact != 0 && e.petCommand != 0 && owner->alive()) {
        // assist the owner: its target if it fights it, else whoever fights the owner; defend itself
        Entity* pick = nullptr;
        if (owner->victim) if (Entity* t = entity(owner->victim)) if (t->alive() && canAttack(e, *t)) pick = t;
        if (!pick)
            for (Entity& o : pool_)
                if (o.used && o.objType == 3 && o.alive() && (o.victim == owner->guid || o.victim == e.guid) && canAttack(e, o)) { pick = &o; break; }
        if (!pick && e.petReact == 2)   // aggressive: whatever hostile comes near
            for (Entity& o : pool_)
                if (o.used && o.objType == 3 && o.alive() && !o.ownerGuid && hostileTowards(o, e) &&
                    pdist(o.x, o.y, e.x, e.y) < 15.0f) { pick = &o; break; }
        if (pick) { e.target = pick->guid; e.victim = e.petMelee ? pick->guid : 0; victim = pick; }
    }
    if (e.castSpell) return;   // a Firebolt on its way: stand still
    if (victim) {
        // autocast (Firebolt, Torment...) at the victim, while it has mana and the spell is ready
        for (uint8_t i = 0; i < e.spellCount; ++i) {
            if (e.petAutocastOff & (1u << i)) continue;
            const SpellX* s = data_ ? data_->sx(e.spells[i]) : nullptr;
            if (!s || s->passive()) continue;
            bool harmful = false;
            for (const SpellEffectX& x : s->eff) if (x.targetA == 6) harmful = true;
            if (!harmful) continue;
            const float range = s->rangeMax > 0 ? s->rangeMax : 30.0f;
            if (pdist(e.x, e.y, victim->x, victim->y) > range) continue;
            if (castX(e, s->id, victim->guid, now, false)) return;
        }
        e.o = std::atan2(victim->y - e.y, victim->x - e.x);
        const float d = pdist(e.x, e.y, victim->x, victim->y);
        if (e.petMelee) {
            if (d > kPetMeleeReach * 0.9f) moveToward(e, victim->x, victim->y, now, kPetMeleeReach * 0.8f);
        } else if (d > 25.0f) moveToward(e, victim->x, victim->y, now, 24.0f);   // the Imp keeps its distance
        return;
    }
    // no fight: follow (1.5 yd at the owner's left) or stay
    if (e.petCommand == 0) {
        if (pdist(e.x, e.y, e.homeX, e.homeY) > 1.0f) moveToward(e, e.homeX, e.homeY, now, 0.5f);
        return;
    }
    const float a = owner->o + kPetFollowAngle;
    const float fx = owner->x + std::cos(a) * kPetFollowDist, fy = owner->y + std::sin(a) * kPetFollowDist;
    const float d = pdist(e.x, e.y, fx, fy);
    e.speed = d > 12.0f ? std::max(owner->speed, 7.0f) * 1.15f : std::max(owner->speed, 7.0f);
    if (d > 2.0f) moveToward(e, fx, fy, now, 0.3f);
}

// ================================================================ the hunter's pet (HUNTER_PET)
namespace {
constexpr int32_t kHappinessLevel = 333000, kHappinessMax = 1000000;   // HAPPINESS_LEVEL_SIZE, POWER_HAPPINESS_DEFAULT
constexpr int32_t kLevelUpLoyalty[6] = {5500, 11500, 17000, 23500, 31000, 39500};
constexpr int32_t kLevelStartLoyalty[6] = {2000, 4500, 7000, 10000, 13500, 17500};
const char* const kFamilyNames[] = {"", "Wolf", "Cat", "Spider", "Bear", "Boar", "Crocolisk", "Carrion Bird", "Crab", "Gorilla", "",
                                    "Raptor", "Tallstrider"};
uint32_t xpForLevel(uint32_t l) {   // (World::xpForNextLevel's table)
    static const uint32_t t[] = {0, 400, 900, 1400, 2100, 2800, 3600, 4500, 5400, 6500, 7600, 8800, 10100, 11400, 12900, 14400, 16000,
                                 17700, 19400, 21300, 23200, 25200, 27300, 29400, 31700, 34000, 36400, 38900, 41400, 44300, 47400,
                                 50800, 54500, 58600, 62800, 67100, 71600, 76100, 80800, 85700, 90700, 95800, 101000, 106300,
                                 111800, 117500, 123200, 129100, 135100, 141200, 147500, 153900, 160400, 167100, 173900, 180800,
                                 187900, 195000, 202300, 209800};
    return l < 60 ? t[l] : 209800;
}
}  // namespace

// MaNGOS::XP::Gain (Formulas.h): BaseGain by the level difference (+5 % a level above, to 4; below, down to the zero
// difference; nothing past the trivial difference), x2.5 for an elite of the open world, x ExperienceMultiplier
uint32_t World::xpGain(uint8_t unitLevel, const Entity& v) const {
    const CreatureTemplate* t = data_ ? data_->creature(v.entry) : nullptr;
    if (!t || !t->xp || v.ownerGuid || v.creatureType == 8) return 0;
    const uint32_t ul = unitLevel, ml = v.level;
    const float base = (float)(ul * 5 + 45);
    float gain = 0;
    if (ml >= ul) gain = base * (1.0f + 0.05f * (float)std::min<uint32_t>(4, ml - ul));
    else {
        const uint32_t diff = ul - ml;
        static const uint32_t triv[] = {4, 4, 5, 5, 6, 6, 7, 7, 8, 9, 10, 11};
        const uint32_t tr = ul / 5 < 12 ? triv[ul / 5] : 12;
        if (diff > tr) return 0;
        const uint32_t zd = ul < 8 ? 5 : ul < 10 ? 6 : ul < 12 ? 7 : ul < 16 ? 8 : ul < 20 ? 9 : ul < 30 ? 11 : ul < 40 ? 12 :
                            ul < 45 ? 13 : ul < 50 ? 14 : ul < 55 ? 15 : ul < 60 ? 16 : 17;
        gain = base * (1.0f - (float)diff / (float)zd);
    }
    if (gain <= 0) return 0;
    const bool elite = t->rank != 0 && t->rank != 4;
    // (the pack keeps the template's experience for its own level; its ExperienceMultiplier is what is left of that)
    const float own = (float)(t->minLevel * 5 + 45) * (elite ? 2.0f : 1.0f);
    const float mult = own > 0 ? (float)t->xp / own : 1.0f;
    if (elite) gain *= 2.5f;
    return (uint32_t)std::lround(gain * mult);
}

// Pet::InitStatsForLevel (HUNTER_PET): pet_levelstats entry 1, melee damage by speed and level, the family's scale and
// passives; happiness / loyalty / experience as Pet::CreateBaseAtCreature sets them for a new one
static void hunterPetStats(Entity& e, const WorldData& d) {
    auto pit = d.pets.find(1);
    const PetLevel* l = nullptr;
    if (pit != d.pets.end())
        for (int L = e.level; L >= 1 && !l; --L) { auto li = pit->second.levels.find((uint8_t)L); if (li != pit->second.levels.end()) l = &li->second; }
    const uint32_t oldMax = e.maxHp;
    e.maxHp = l ? std::max<uint32_t>(1, l->hp) : 1;
    e.armor = l ? l->armor : 0;
    if (l) { e.statStr = l->str; e.statAgi = l->agi; e.statSta = l->sta; e.statInt = l->inte; e.statSpi = l->spi; }
    const uint32_t mDmg = e.attackMs * e.level / 2000;
    e.dmgMin = (uint32_t)std::max(1.0f, (float)mDmg - (float)mDmg / 4.0f);
    e.dmgMax = (uint32_t)std::max((float)e.dmgMin, ((float)mDmg - (float)mDmg / 4.0f) * 1.5f);
    // Pet::UpdateAttackPowerAndDamage: AP = str x 2 - 20, its bonus to a swing
    e.attackPower = std::max(0, 2 * e.statStr - 20);
    const uint32_t bonus = (uint32_t)(e.attackPower * (int32_t)e.attackMs / 14000);
    e.dmgMin += bonus; e.dmgMax += bonus;
    if (auto f = d.families.find(e.family); f != d.families.end() && f->second.minScale > 0) {
        const PetFamily& fa = f->second;
        float sc;
        if (e.level >= fa.maxScaleLevel) sc = fa.maxScale;
        else if (e.level <= fa.minScaleLevel) sc = fa.minScale;
        else sc = fa.minScale + (float)(e.level - fa.minScaleLevel) / (float)std::max<uint8_t>(1, fa.maxScaleLevel) * (fa.maxScale - fa.minScale);
        e.scale = sc;
    }
    if (oldMax && e.hp > 0) e.hp = std::min<int32_t>((int32_t)e.maxHp, e.hp + (int32_t)e.maxHp - (int32_t)oldMax);
}

void World::applyHunterPassives(Entity& e, uint32_t now) {
    if (!data_) return;
    auto f = data_->families.find(e.family);
    if (f == data_->families.end()) return;
    for (uint32_t sp : f->second.passives)
        if (const SpellX* s = data_->sx(sp); s && !hasAura(e, sp))
            for (int k = 0; k < 3; ++k)
                if (s->eff[k].effect == 6) applyAuraX(e, e, *s, k, now, 0);   // (SPELL_EFFECT_APPLY_AURA)
    // (their modifiers: SPELL_AURA_MOD_INCREASE_HEALTH_PERCENT 133, armour % 142; damage % 79 counts at each hit)
    const int32_t hpPct = auraSum(e, 133), arPct = auraSum(e, 142, 1);
    if (hpPct) { const uint32_t m = (uint32_t)std::max<int64_t>(1, (int64_t)e.maxHp * (100 + hpPct) / 100); e.hp = std::min<int32_t>((int32_t)m, e.hp + (int32_t)m - (int32_t)e.maxHp); e.maxHp = m; }
    if (arPct) e.armor = (uint32_t)std::max<int64_t>(0, (int64_t)e.armor * (100 + arPct) / 100);
}

Entity* World::spawnHunterPet(Entity& owner, uint32_t entry, uint32_t display, uint32_t family, uint8_t level, uint32_t now) {
    const CreatureTemplate* t = data_ ? data_->creature(entry) : nullptr;
    dismissPet(false);
    const uint32_t number = ++petCounter_;
    const uint64_t guid = 0xF140000000000000ull | ((uint64_t)(entry & 0xFFFFFF) << 24) | (uint64_t)number;
    Entity& e = allocate(guid);
    if (!e.used) return nullptr;
    e.objType = 3;
    e.entry = entry;
    e.ownerGuid = owner.guid;
    e.petNumber = number;
    e.petHunter = true;
    e.family = family;
    e.petCommand = 1;
    e.petReact = 1;
    e.petMelee = true;
    e.level = level;
    e.faction = owner.faction;
    e.displayId = display ? display : (t ? t->displayId : 0);
    e.scale = t ? t->scale : 1.0f;
    e.speed = t ? t->speed : 8.0f;
    e.attackMs = t && t->attackMs >= 500 ? t->attackMs : 2000;
    e.combatReach = t ? t->combatReach : 1.5f;
    e.boundingRadius = t ? t->boundingRadius : 0.5f;
    e.creatureType = 1;   // a beast
    e.powerType = 2;      // POWER_FOCUS
    e.maxPower = 100;
    e.power = 100;
    e.spellCount = 0;
    hunterPetStats(e, *data_);
    e.hp = (int32_t)e.maxHp;
    const float a = owner.o + kPetFollowAngle;
    e.x = owner.x + std::cos(a) * kPetFollowDist;
    e.y = owner.y + std::sin(a) * kPetFollowDist;
    e.z = owner.z;
    e.o = owner.o;
    e.homeX = e.x; e.homeY = e.y; e.homeZ = e.z;
    applyGround(e);
    e.petHappyTickMs = now + 7500;
    e.petLoyaltyTickMs = now + 12000;
    e.petFocusTickMs = now + 4000;
    petGuid_ = guid;
    return &e;
}

// Spell::EffectTameCreature / Pet::CreateBaseAtCreature: the beast becomes the hunter's pet at its level, with its
// health share, half content (166500), rebellious with 1000 loyalty points, named after its family
bool World::tameCreature(Entity& owner, Entity& beast, uint32_t spell, uint32_t now) {
    auto fail = [&](uint8_t why) { emit(SimEvent::Message, owner.guid, owner.guid, why, 6); return false; };   // SMSG_PET_TAME_FAILURE
    if (!owner.self || !data_ || beast.objType != 3 || !beast.alive()) return fail(1);
    if (profiles_[0].cls != 3) return fail(6);                       // PETTAME_UNITSCANTTAME
    if (beast.ownerGuid || beast.summonerGuid) return fail(3);        // PETTAME_CREATUREALREADYOWNED
    if (beast.level > owner.level) return fail(9);                    // PETTAME_TOOHIGHLEVEL
    auto tm = data_->tameable.find(beast.entry);
    if (tm == data_->tameable.end()) return fail(4);                  // PETTAME_NOTTAMEABLE
    if (pet() || (profiles_[0].petEntry && profiles_[0].petHunter)) return fail(5);   // PETTAME_ANOTHERSUMMONACTIVE
    const uint32_t hpPct = beast.maxHp ? (uint32_t)std::max(1, beast.hp * 100 / (int32_t)beast.maxHp) : 100;
    const uint32_t entry = beast.entry, display = beast.displayId, family = tm->second;
    const uint8_t level = (uint8_t)beast.level;
    const float bx = beast.x, by = beast.y, bz = beast.z, bo = beast.o;
    // the creature goes (ForcedDespawn): its spawn comes back as usual
    for (Entity& o : pool_)
        if (o.used && o.objType == 3 && o.victim == beast.guid) o.victim = o.target = 0;
    beast.hp = 0;
    beast.dead = true;
    beast.respawnAtMs = now + 1000;   // (the grid brings it back after its respawn time; first it is gone)
    emit(SimEvent::Vanish, beast.guid, 0, 0, 0);
    releaseCreature(beast, now);
    Entity* e = spawnHunterPet(owner, entry, display, family, level, now);
    if (!e) return fail(8);
    e->x = bx; e->y = by; e->z = bz; e->o = bo;
    e->createdBySpell = spell;
    e->hp = std::max<int32_t>(1, (int32_t)e->maxHp * (int32_t)hpPct / 100);
    e->petHappiness = 166500;
    e->petLoyaltyLevel = 1;    // REBELLIOUS
    e->petLoyaltyPoints = 1000;
    e->petXp = 0;
    e->petLoyaltyXpNeed = xpForLevel(owner.level) * 5 / 100;   // SetRequiredXpForNextLoyaltyLevel
    auto f = data_->families.find(family);
    const CreatureTemplate* t = data_->creature(entry);
    e->petName = f != data_->families.end() && !f->second.name.empty() ? f->second.name
                 : family < sizeof(kFamilyNames) / sizeof(kFamilyNames[0]) && kFamilyNames[family][0] ? kFamilyNames[family]
                 : t ? t->name : std::string("Pet");
    e->petAutocastOff = 0;
    applyHunterPassives(*e, now);
    CharacterProfile& p = profiles_[0];
    p.petEntry = entry; p.petSpell = spell; p.petHunter = true; p.petDisplay = display; p.petFamily = family; p.petRenamed = false;
    p.petOut = true;
    storePet();
    emit(SimEvent::Appear, e->guid, 0, 0, 0);
    emit(SimEvent::PetChanged, e->guid, owner.guid, spell, 1);
    return true;
}

// Call Pet (SPELL_EFFECT_SUMMON_PET with no entry: Pet::LoadPetFromDB) / Revive Pet (SPELL_EFFECT_RESURRECT_PET)
bool World::callHunterPet(Entity& owner, uint32_t now, int32_t revivePct) {
    CharacterProfile& p = profiles_[0];
    if (!p.petHunter || !p.petEntry) { emit(SimEvent::CastFailed, owner.guid, 0, 883, 0x4C, 0, 0); return false; }   // NO_PET
    if (Entity* cur = pet()) {
        if (revivePct && !cur->alive()) {   // resurrected where it lies
            cur->dead = false;
            cur->hp = std::max<int32_t>(1, (int32_t)cur->maxHp * revivePct / 100);
            cur->respawnAtMs = 0;
            emit(SimEvent::Appear, cur->guid, 0, 0, 0);
            return true;
        }
        return false;
    }
    if (!p.petHp && !revivePct) { emit(SimEvent::Message, owner.guid, owner.guid, 10, 6); return false; }   // PETTAME_DEAD
    if (p.petHp && revivePct) return false;   // (it lives: nothing to revive)
    Entity* e = spawnHunterPet(owner, p.petEntry, p.petDisplay, p.petFamily, std::max<uint8_t>(1, p.petLevel), now);
    if (!e) return false;
    e->createdBySpell = p.petSpell;
    e->petXp = p.petXp;
    e->petHappiness = p.petHappiness;
    e->petLoyaltyLevel = std::max<uint8_t>(1, p.petLoyaltyLevel);
    e->petLoyaltyPoints = p.petLoyaltyPoints;
    e->petLoyaltyXpNeed = p.petLoyaltyXpNeed;
    e->petName = p.petName;
    e->petRenamed = p.petRenamed;
    e->petAutocastOff = p.petAutocastOff;
    applyHunterPassives(*e, now);
    e->hp = revivePct ? std::max<int32_t>(1, (int32_t)e->maxHp * revivePct / 100) : std::min<int32_t>((int32_t)e->maxHp, (int32_t)p.petHp);
    p.petOut = true;
    storePet();
    emit(SimEvent::Appear, e->guid, 0, 0, 0);
    emit(SimEvent::PetChanged, e->guid, owner.guid, p.petSpell, 1);
    return true;
}

// Pet::GivePetXP: not past the owner's level; the loyalty's experience need shrinks, a kill's loyalty bonus
void World::givePetXp(Entity& e, uint32_t xp, uint8_t victimLevel) {
    if (!e.petHunter || !e.alive() || !xp || !player_) return;
    const uint32_t maxLevel = std::min<uint32_t>(60, player_->level);
    uint32_t level = e.level;
    if (level < maxLevel) {
        uint32_t next = xpForLevel(level) / 4;   // ObjectMgr::GetXPForPetLevel
        uint32_t cur = e.petXp + xp;
        while (cur >= next && level < maxLevel) {
            cur -= next;
            ++level;
            e.level = (uint8_t)level;
            hunterPetStats(e, *data_);
            applyHunterPassives(e, now_);
            e.hp = (int32_t)e.maxHp;
            emit(SimEvent::LevelUp, e.guid, 0, 0, (int32_t)level);
            next = xpForLevel(level) / 4;
        }
        e.petXp = level < maxLevel ? cur : 0;
    }
    e.petLoyaltyXpNeed = xp > e.petLoyaltyXpNeed ? 0 : e.petLoyaltyXpNeed - xp;   // UpdateRequireXpForNextLoyaltyLevel
    if (victimLevel <= 100) modifyLoyalty(e, (int32_t)((100 - victimLevel) / 10 + (6 - e.petLoyaltyLevel)));   // KillLoyaltyBonus
    storePet();
}

// Pet::ModifyLoyalty: under 0 a level down (rebellious: it leaves, turns aggressive or stays passive), past the level's
// points and the experience needed a level up (training points left aside: no beast training here yet)
void World::modifyLoyalty(Entity& e, int32_t add) {
    const uint8_t lv = e.petLoyaltyLevel;
    auto maxPts = [](uint8_t l) { return kLevelUpLoyalty[std::min<int>(5, std::max(0, l - 1))]; };
    if (lv >= 6 && add + e.petLoyaltyPoints > maxPts(lv)) return;
    e.petLoyaltyPoints += add;
    if (e.petLoyaltyPoints < 0) {
        if (lv > 1) {
            e.petLoyaltyLevel = lv - 1;
            e.petLoyaltyPoints = kLevelStartLoyalty[e.petLoyaltyLevel - 1];
            e.petLoyaltyXpNeed = player_ ? xpForLevel(player_->level) * 5 / 100 : 0;
        } else {
            switch (rng_.below(3)) {
                case 0:   // it abandons its owner (SMSG_PET_BROKEN)
                    emit(SimEvent::Message, e.ownerGuid, e.ownerGuid, 0, 7);
                    e.petLoyaltyPoints = 0;
                    dismissPet(true);
                    return;
                case 1: e.petReact = 2; e.petLoyaltyPoints = 500; break;   // aggressive
                default: e.petCommand = 0; e.petReact = 0; e.victim = e.target = 0; e.petLoyaltyPoints = 500; break;   // stay, passive
            }
            emit(SimEvent::PetChanged, e.guid, e.ownerGuid, 0, 2);
        }
    } else if (e.petLoyaltyPoints > maxPts(lv) && e.petLoyaltyXpNeed == 0 && lv < 6) {
        e.petLoyaltyLevel = lv + 1;
        e.petLoyaltyPoints = kLevelStartLoyalty[e.petLoyaltyLevel - 1];
        e.petLoyaltyXpNeed = e.petLoyaltyLevel >= 6 ? 0 : player_ ? xpForLevel(player_->level) * 5 / 100 : 0;
        emit(SimEvent::PetChanged, e.guid, e.ownerGuid, 0, 2);
    }
}

// Pet::RegenerateAll: focus +24 every 4 s; happiness -(140 >> loyalty level) x 125 every 7.5 s (x 1.5 in combat);
// loyalty every 12 s by happiness (+20 happy, +10 content, -20 unhappy)
void World::tickHunterPet(Entity& e, Entity& owner, uint32_t now) {
    (void)owner;
    if (!e.alive()) return;
    if (now >= e.petFocusTickMs) {
        e.petFocusTickMs = now + 4000;
        if (e.power < (int32_t)e.maxPower) { e.power = std::min<int32_t>((int32_t)e.maxPower, e.power + 24); emit(SimEvent::PowerChanged, e.guid, 0, 0, e.power); }
    }
    if (now >= e.petHappyTickMs) {
        e.petHappyTickMs = now + 7500;
        if (e.petHappiness > 0) {
            int32_t loss = (140 >> e.petLoyaltyLevel) * 125;
            if (e.victim || e.target) loss = loss * 3 / 2;
            e.petHappiness = std::max(0, e.petHappiness - loss);
        }
    }
    if (now >= e.petLoyaltyTickMs) {
        e.petLoyaltyTickMs = now + 12000;
        modifyLoyalty(e, e.petHappiness >= 2 * kHappinessLevel ? 20 : e.petHappiness >= kHappinessLevel ? 10 : -20);
    }
}

// SPELL_EFFECT_FEED_PET (Spell::CheckCast + EffectFeedPet): the family's diet (petFoodMask & 1 << FoodType - 1), the
// food's level (Pet::GetCurrentFoodBenefitLevel: 35000 / 17000 / 8000 a tick by how far under the pet's), not in combat;
// the item is used up and Feed Pet Effect (1539) gives that happiness over its time
bool World::feedPet(Entity& owner, uint32_t item, uint32_t now) {
    Entity* e = pet();
    auto fail = [&](uint8_t why) { emit(SimEvent::CastFailed, owner.guid, 0, 6991, why, 0, 0); return false; };
    if (!e || !e->petHunter || !e->alive()) return fail(0x4C);   // SPELL_FAILED_NO_PET
    auto fd = data_ ? data_->foods.find(item) : data_->foods.end();
    auto fam = data_->families.find(e->family);
    if (fd == data_->foods.end() || fam == data_->families.end() || !(fam->second.foodMask & (1u << (fd->second.first - 1)))) return fail(0x7D);
    const uint32_t il = fd->second.second;
    const int32_t benefit = e->level <= il + 5 ? 35000 : e->level <= il + 10 ? 17000 : e->level <= il + 14 ? 8000 : 0;
    if (!benefit) return fail(0x1F);                                // SPELL_FAILED_FOOD_LOWLEVEL
    if (e->victim || e->target) return fail(0);                     // SPELL_FAILED_AFFECTING_COMBAT
    if (!removePlayerItem(item, 1)) return fail(0x0C);
    const SpellX* fx = data_->sx(1539);
    Aura a;
    a.spell = 1539; a.caster = owner.guid; a.type = 24; a.effIndex = 0; a.misc = 4; a.amount = benefit; a.positive = true;
    a.amplitudeMs = fx && fx->eff[0].amplitude > 0 ? (uint32_t)fx->eff[0].amplitude : 1000;
    a.nextTickMs = now + a.amplitudeMs;
    a.endMs = now + (fx ? durationX(owner, *fx) : 10000);
    addAura(*e, a);
    emit(SimEvent::CastFinished, owner.guid, e->guid, 6991, 0, 0);
    return true;
}

// ================================================================ WoWVita: totems, after CMaNGOS
//   - Spell::EffectSummonTotem (74, _SLOT1..4 87..90): one totem a slot (fire, earth, water, air), the old one goes;
//     at the spot TARGET_LOCATION_CASTER_FRONT_RIGHT / BACK_RIGHT / BACK_LEFT / FRONT_LEFT (41..44) puts it, 2 yd off
//     (Spell::SetTargetMap: "all shaman totems have 0 radius"); the owner's faction and level, the spell's duration,
//     its damage as the totem's health, UNIT_CREATED_BY_SPELL;
//   - Totem::SetTypeBySummonSpell: a totem whose spell has a cast time is TOTEM_ACTIVE (Searing Totem), else
//     TOTEM_PASSIVE: Totem::Summon casts its spells on itself (Stoneskin, Healing Stream, Earthbind...);
//   - Totem::Update: it goes when its time is up, when it or its owner dies; Totem::UnSummon takes its auras away;
//   - TotemAI::UpdateAI: an active totem casts at its victim, or the nearest enemy within the spell's range;
//   - AreaAura (SPELL_EFFECT_APPLY_AREA_AURA_PARTY 35): the totem's aura reaches its owner (and the owner's pet) within
//     the effect's radius, and leaves them when they step out of it.
Entity* World::totemIn(uint8_t slot) { return slot < 4 && totems_[slot] ? entity(totems_[slot]) : nullptr; }

Entity* World::summonTotem(Entity& owner, const SpellX& s, int k, uint32_t now) {
    if (!data_ || !owner.self) return nullptr;
    const SpellEffectX& fx = s.eff[k];
    auto it = data_->totems.find((uint32_t)fx.misc);
    if (it == data_->totems.end()) return nullptr;
    const TotemTemplate& tt = it->second;
    const uint8_t slot = fx.effect >= 87 && fx.effect <= 90 ? (uint8_t)(fx.effect - 87) : 4;
    if (Entity* old = totemIn(slot)) unsummonTotem(*old);
    float angle = owner.o;
    switch (fx.targetA) {
        case 41: angle += 3.14159265f * 1.75f; break;   // TARGET_LOCATION_CASTER_FRONT_RIGHT
        case 42: angle += 3.14159265f * 1.25f; break;   // BACK_RIGHT
        case 43: angle += 3.14159265f * 0.75f; break;   // BACK_LEFT
        case 44: angle += 3.14159265f * 0.25f; break;   // FRONT_LEFT
        default: break;
    }
    const float r = fx.radius > 0 ? fx.radius : 2.0f;
    const uint64_t guid = 0xF130000000000000ull | ((uint64_t)(tt.entry & 0xFFFFFF) << 24) | (uint64_t)(0xC00000u + (++summonCounter_ & 0x3FFFFF));
    Entity& e = allocate(guid);
    if (!e.used) return nullptr;
    e.objType = 3;
    e.entry = tt.entry;
    e.ownerGuid = owner.guid;
    e.totem = true;
    e.totemSlot = slot;
    e.createdBySpell = s.id;
    e.faction = owner.faction;
    e.level = owner.level;
    e.displayId = tt.displayId;
    e.scale = tt.scale;
    e.speed = 0;
    e.boundingRadius = 0.5f;
    e.combatReach = 1.5f;
    const int32_t dmg = effValue(owner, s, k, 0);   // (if not, the template's health)
    e.maxHp = dmg > 0 ? (uint32_t)dmg : std::max<uint32_t>(1, tt.hp);
    e.hp = (int32_t)e.maxHp;
    e.powerType = 0; e.power = 0; e.maxPower = 0;
    e.x = owner.x + std::cos(angle) * r;
    e.y = owner.y + std::sin(angle) * r;
    e.z = owner.z;
    e.o = owner.o;
    applyGround(e);
    if (std::fabs(e.z - owner.z) > 5.0f) e.z = owner.z;   // (Totem::Create: at the caster's height when far off it)
    e.homeX = e.x; e.homeY = e.y; e.homeZ = e.z; e.homeO = e.o;
    const uint32_t d = durationX(owner, s);
    e.despawnAtMs = d ? now + d : 0;
    e.spellCount = 0;
    for (uint32_t sp : tt.spells)
        if (e.spellCount < e.spells.size() && data_->sx(sp)) e.spells[e.spellCount++] = sp;
    const SpellX* first = e.spellCount ? data_->sx(e.spells[0]) : nullptr;
    e.totemActive = first && first->castMs > 0;
    if (slot < 4) totems_[slot] = guid;
    emit(SimEvent::Appear, guid, 0, 0, 0);
    if (!e.totemActive)   // TOTEM_PASSIVE: CastSpell(nullptr, spell, TRIGGERED_OLD_TRIGGERED)
        for (uint8_t i = 0; i < e.spellCount; ++i) castX(e, e.spells[i], e.guid, now, true);
    return &e;
}

void World::unsummonTotem(Entity& t) {
    if (!t.used || !t.totem) return;
    const uint64_t guid = t.guid;
    for (Entity& o : pool_) {   // (its auras: on itself, its owner, the owner's pet)
        if (!o.used) continue;
        for (int i = o.auraCount - 1; i >= 0; --i)
            if (o.auras[i].caster == guid) removeAura(o, i);
        if (o.victim == guid) o.victim = o.target = 0;
    }
    for (uint64_t& g : totems_) if (g == guid) g = 0;
    for (AreaFx& a : areas_) if (a.used && a.caster == guid) a.used = false;
    emit(SimEvent::Vanish, guid, 0, 0, 0);
    Entity* self = entity(guid);
    if (self) *self = Entity();
}

void World::tickTotem(Entity& e, uint32_t now) {
    Entity* owner = entity(e.ownerGuid);
    if (!owner || !owner->alive() || e.dead || !e.alive() || (e.despawnAtMs && now >= e.despawnAtMs)) { unsummonTotem(e); return; }
    e.victim = 0;   // (it never swings)
    // AreaAura::Update (party): the owner and its pet, when within the radius
    for (uint8_t i = 0; i < e.auraCount; ++i) {
        const Aura a = e.auras[i];
        if (a.caster != e.guid) continue;
        const SpellX* s = data_ ? data_->sx(a.spell) : nullptr;
        if (!s || s->eff[a.effIndex].effect != 35) continue;
        const float radius = s->eff[a.effIndex].radius > 0 ? s->eff[a.effIndex].radius : 30.0f;
        Entity* members[2] = {owner, pet()};
        for (Entity* m : members) {
            if (!m) continue;
            int have = -1;
            for (int j = 0; j < m->auraCount; ++j)
                if (m->auras[j].spell == a.spell && m->auras[j].caster == e.guid && m->auras[j].effIndex == a.effIndex) { have = j; break; }
            const bool in = m->alive() && distance(e, *m) <= radius;
            if (in && have < 0) applyAuraX(e, *m, *s, a.effIndex, now, 0);
            else if (!in && have >= 0) removeAura(*m, have);
        }
    }
    if (!e.totemActive || e.castSpell || !e.spellCount) return;
    // TotemAI::UpdateAI
    const SpellX* s = data_->sx(e.spells[0]);
    if (!s || cooldownLeft(e, s->id, now)) return;
    const float range = s->rangeMax;
    Entity* v = e.totemVictim ? entity(e.totemVictim) : nullptr;
    if (!v || !v->alive() || !hostileTowards(e, *v) || distance(e, *v) > range || v->untargetable) {
        v = nullptr;
        float best = range;
        for (Entity& o : pool_) {   // MaNGOS::NearestAttackableUnitInObjectRangeCheck
            if (!o.used || !o.alive() || &o == &e || o.untargetable || !hostileTowards(e, o)) continue;
            if (o.objType == 3 && o.aiState == 3) continue;   // (evading)
            const float dd = distance(e, o);
            if (dd < best) { best = dd; v = &o; }
        }
    }
    e.totemVictim = v ? v->guid : 0;
    if (!v) return;
    e.o = std::atan2(v->y - e.y, v->x - e.x);
    castX(e, s->id, v->guid, now, false);
}

}  // namespace offline
}  // namespace w3n
