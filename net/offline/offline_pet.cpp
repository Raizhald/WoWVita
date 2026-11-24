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

void World::dismissPet(bool forget) {
    Entity* e = pet();
    const uint64_t guid = petGuid_;
    petGuid_ = 0;
    if (forget) { profiles_[0].petEntry = 0; profiles_[0].petSpell = 0; profiles_[0].petName.clear(); }
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
    if (e.level != owner->level) {   // Pet::SynchronizeLevelWithOwner
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

}  // namespace offline
}  // namespace w3n
