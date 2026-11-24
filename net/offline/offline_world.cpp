// offline_world.cpp — the authoritative combat / spawn / loot / progression simulation.
//
// Rules are deliberately small and explicit; every constant that matters for balance is in one place
// below. The simulation advances in fixed 50 ms steps, uses integer health/damage and a seeded
// xorshift RNG, so the same inputs (seed, positions, actions, time) produce the same result.
#include "offline_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "offline_bots.hpp"

namespace w3n {
namespace offline {

namespace {

constexpr float kMeleeReach = 5.0f;         // vMaNGOS ATTACK_DISTANCE
constexpr float kAggroLeash = 30.0f;        // a creature gives up beyond this from its home
constexpr float kEvadeDist = 45.0f;
constexpr uint32_t kGcdMs = 1500;
constexpr uint32_t kSwingWindupMs = 200;
constexpr int32_t kCritChancePct = 5;
constexpr int32_t kMissChancePct = 5;

float dist2d(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

float angleTo(float ax, float ay, float bx, float by) {
    return std::atan2(by - ay, bx - ax);
}

uint32_t xpForNextLevel(uint32_t level) {
    // A compact, readable curve. Vanilla's exact table is not needed for a level-10 demo; the
    // converter can replace it later without touching the combat code.
    if (level < 1) level = 1;
    return 400u + (level - 1) * 300u;
}

}  // namespace

// ---------------------------------------------------------------- pools and setup
Entity& World::allocate(uint64_t guid) {
    for (Entity& e : pool_)
        if (!e.used) {
            e = Entity();
            e.used = true;
            e.guid = guid;
            return e;
        }
    static Entity dummy;
    dummy = Entity();
    return dummy;
}

Entity* World::entity(uint64_t guid) {
    if (!guid) return nullptr;
    for (Entity& e : pool_)
        if (e.used && e.guid == guid) return &e;
    return nullptr;
}
const Entity* World::entity(uint64_t guid) const {
    if (!guid) return nullptr;
    for (const Entity& e : pool_)
        if (e.used && e.guid == guid) return &e;
    return nullptr;
}

void World::reset(uint32_t nowMs, uint32_t seed, const CharacterProfile& player,
                  const std::array<CharacterProfile, kMaxParty>& bots, bool botsEnabled) {
    for (Entity& e : pool_) e = Entity();
    eventCount_ = 0;
    questLog_.clear();
    player_ = nullptr;
    for (Entity*& b : bots_) b = nullptr;
    botsEnabled_ = botsEnabled;
    seed_ = seed ? seed : 1;
    rng_.seed(seed_);
    now_ = lastStepMs_ = nowMs;
    tick_ = 0;

    profiles_[0] = player;
    profiles_[0].isBot = false;
    profiles_[0].used = true;
    if (profiles_[0].guid == 0) profiles_[0].guid = 0x0000000000000001ull;
    if (profiles_[0].maxHp == 0) profiles_[0].maxHp = 100;
    if (profiles_[0].hp <= 0) profiles_[0].hp = (int32_t)profiles_[0].maxHp;
    if (profiles_[0].nextXp == 0) profiles_[0].nextXp = xpForNextLevel(profiles_[0].level);

    // ---- the player entity
    {
        Entity& e = allocate(profiles_[0].guid);
        player_ = &e;
        e.objType = 4;
        e.self = true;
        e.playerLike = true;
        e.profileIndex = 0;
        e.level = (uint8_t)std::min<uint32_t>(profiles_[0].level, 60);
        e.faction = 1;                              // human / Alliance player faction for the offline rules
        e.displayId = 49;                           // human male; the server layer writes the real look fields
        e.hp = profiles_[0].hp;
        e.maxHp = profiles_[0].maxHp;
        e.power = profiles_[0].power;
        e.maxPower = profiles_[0].maxPower;
        e.powerType = profiles_[0].powerType;
        e.money = profiles_[0].money;
        e.levelXp = profiles_[0].xp;
        e.nextLevelXp = profiles_[0].nextXp;
        e.speed = 7.0f;
        e.combatReach = 1.5f;
        e.boundingRadius = 0.5f;
        e.x = profiles_[0].x;
        e.y = profiles_[0].y;
        e.z = profiles_[0].z;
        e.o = profiles_[0].o;
        if (e.x == 0 && e.y == 0 && data_) {
            e.x = data_->spawnX; e.y = data_->spawnY; e.z = data_->spawnZ; e.o = data_->spawnO;
        }
        e.homeX = e.x; e.homeY = e.y; e.homeZ = e.z; e.homeO = e.o;
        e.spellCount = std::min<uint8_t>(profiles_[0].spellCount, (uint8_t)e.spells.size());
        for (uint8_t i = 0; i < e.spellCount; ++i) e.spells[i] = profiles_[0].spells[i];
        recomputeGear();   // WoWVita: the weapon and the armour of everything worn
    }

    // ---- the four companions
    for (int i = 0; i < kMaxParty; ++i) {
        profiles_[1 + i] = bots[(size_t)i];
        profiles_[1 + i].isBot = true;
        profiles_[1 + i].used = true;
        if (profiles_[1 + i].guid == 0) profiles_[1 + i].guid = 0x0000000000000B01ull + (uint64_t)i;
        if (!botsEnabled) continue;   // WoWVita: alone (the profiles stay in the save for when the party comes back)
        CharacterProfile& p = profiles_[1 + i];
        Entity& e = allocate(p.guid);
        e.objType = 4;
        e.playerLike = true;
        e.bot = true;
        e.botIndex = (int8_t)i;
        e.profileIndex = (int8_t)(1 + i);
        e.botRole = (uint8_t)p.role;
        e.level = (uint8_t)std::min<uint32_t>(p.level, 60);
        e.faction = 1;
        e.displayId = 49;
        e.hp = p.hp > 0 ? p.hp : (int32_t)p.maxHp;
        e.maxHp = p.maxHp ? p.maxHp : 100;
        e.power = p.power;
        e.maxPower = p.maxPower;
        e.powerType = p.powerType;
        e.speed = 7.0f;
        e.combatReach = 1.5f;
        e.boundingRadius = 0.5f;
        e.spellCount = std::min<uint8_t>(p.spellCount, (uint8_t)e.spells.size());
        for (uint8_t k = 0; k < e.spellCount; ++k) e.spells[k] = p.spells[k];
        e.dmgMin = 4; e.dmgMax = 7; e.attackMs = 2000;
        if (const ItemTemplate* it = data_ ? data_->item(p.equipment[15]) : nullptr) {
            if (it->delayMs >= 500) e.attackMs = it->delayMs;
            if (it->dmgMax > 0) { e.dmgMin = (uint32_t)it->dmgMin; e.dmgMax = (uint32_t)it->dmgMax; }
        }
        // Form up behind/around the player at the start.
        const float a = (float)i * 1.5708f + 1.2f;
        e.x = player_->x + std::cos(a) * 3.5f;
        e.y = player_->y + std::sin(a) * 3.5f;
        e.z = player_->z;
        e.o = angleTo(e.x, e.y, player_->x, player_->y);
        e.homeX = player_->x; e.homeY = player_->y; e.homeZ = player_->z; e.homeO = 0;
        e.tacticalMode = 1;   // assist
        e.nextDecisionMs = nowMs + 200 + (uint32_t)i * 50;
        bots_[(size_t)i] = &e;
        p.x = e.x; p.y = e.y; p.z = e.z; p.o = e.o;
    }

    // ---- creatures from the immutable spawn list: only those around the player (WoWVita: spawn grid)
    cellsFor_ = nullptr;
    nextActivationMs_ = 0;
    updateActivation(nowMs, true);
    commands_ = PartyCommandState();
    emitText(SimEvent::Message, 0, "Offline world ready");
    applyGround(*player_);
}

void World::spawnCreature(const Spawn& s, uint32_t nowMs, int index) {
    const CreatureTemplate* t = data_ ? data_->creature(s.entry) : nullptr;
    if (!t) return;
    const uint64_t guid = 0xF130000000000000ull | ((uint64_t)(t->entry & 0xFFFFFF) << 24) | (uint64_t)(uint32_t)(index + 1);
    if (entity(guid)) return;
    Entity& e = allocate(guid);
    if (!e.used) return;
    e.objType = 3;
    e.entry = t->entry;
    e.level = (uint8_t)std::max<uint16_t>(1, t->minLevel);
    e.hp = (int32_t)t->hp;
    e.maxHp = t->hp;
    e.powerType = t->powerType;
    e.power = (int32_t)t->power;
    e.maxPower = t->power;
    e.faction = t->faction;
    e.displayId = t->displayId;
    e.rank = t->rank;
    e.creatureType = t->creatureType;
    e.npcFlags = t->npcFlags;
    e.scale = t->scale;
    e.speed = t->speed;
    e.combatReach = t->combatReach;
    e.boundingRadius = t->boundingRadius;
    e.dmgMin = t->dmgMin;
    e.dmgMax = t->dmgMax;
    e.attackMs = t->attackMs;
    e.armor = t->armor;
    e.x = s.x; e.y = s.y; e.z = s.z; e.o = s.o;
    e.homeX = s.x; e.homeY = s.y; e.homeZ = s.z; e.homeO = s.o;
    e.respawnAtMs = 0;
    e.aiState = 0;
    e.spellCount = std::min<uint8_t>(t->abilityCount, (uint8_t)e.spells.size());
    for (uint8_t i = 0; i < e.spellCount; ++i) e.spells[i] = t->abilities[i];
    e.nextAbilityMs = nowMs + 1500 + rng_.below(2000);
    e.lootCount = 0;
    e.dead = false;
    applyGround(e);
    initBehaviour(e, index, nowMs);   // WoWVita
}

// ---------------------------------------------------------------- events
void World::emit(uint8_t type, uint64_t a, uint64_t b, uint32_t spell, int32_t value, uint8_t school, uint8_t flags) {
    if (eventCount_ >= kMaxEvents) return;
    SimEvent& ev = events_[eventCount_++];
    ev = SimEvent();
    ev.type = type;
    ev.a = a;
    ev.b = b;
    ev.spell = spell;
    ev.value = value;
    ev.school = school;
    ev.flags = flags;
}

void World::emitText(uint8_t type, uint64_t a, const char* text) {
    if (eventCount_ >= kMaxEvents) return;
    SimEvent& ev = events_[eventCount_++];
    ev = SimEvent();
    ev.type = type;
    ev.a = a;
    if (text) {
        std::strncpy((char*)ev.text, text, sizeof(ev.text) - 1);
        ev.text[sizeof(ev.text) - 1] = 0;
    }
}

// ---------------------------------------------------------------- geometry / hostility
float World::distance(const Entity& a, const Entity& b) const {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool World::inMeleeRange(const Entity& a, const Entity& b) const {
    return dist2d(a.x, a.y, b.x, b.y) <= kMeleeReach + a.boundingRadius + b.boundingRadius;
}

bool World::hostileTowards(const Entity& a, const Entity& b) const {
    if (a.guid == b.guid || a.dead || b.dead) return false;
    if (a.untargetable || b.untargetable) return false;   // WoWVita: a flight
    if (a.npcFlags & 3u) return false;          // gossip / quest NPCs are never hostile in this subset
    if (b.npcFlags & 3u) return false;
    if (a.faction == 0 || b.faction == 0) return false;
    return data_ && data_->hostile(a.faction, b.faction);
}

bool World::canAttack(const Entity& a, const Entity& b) const {
    if (hostileTowards(a, b)) return true;
    if (a.guid == b.guid || a.dead || b.dead) return false;
    if (a.untargetable || b.untargetable) return false;
    if ((a.npcFlags & 3u) || (b.npcFlags & 3u)) return false;
    if (a.faction == 0 || b.faction == 0) return false;
    return data_ && data_->attackable(a.faction, b.faction);
}

int World::alivePartyMembers() const {
    int n = player_ && player_->alive() ? 1 : 0;
    for (Entity* b : bots_)
        if (b && b->alive()) ++n;
    return n;
}

int World::injuredPartyMembers(int belowPercent) const {
    int n = 0;
    auto hurt = [&](const Entity* e) {
        return e && e->alive() && e->maxHp && (int)(e->hp * 100 / (int32_t)e->maxHp) < belowPercent;
    };
    if (hurt(player_)) ++n;
    for (Entity* b : bots_) if (hurt(b)) ++n;
    return n;
}

float World::partyHealthFraction() const {
    int64_t hp = 0, max = 0;
    auto add = [&](const Entity* e) {
        if (!e || !e->alive()) return;
        hp += e->hp; max += e->maxHp;
    };
    add(player_);
    for (Entity* b : bots_) add(b);
    return max ? (float)hp / (float)max : 0.0f;
}

float World::lowestPartyHealthFraction(uint64_t* who) const {
    float best = 1.0f;
    uint64_t at = 0;
    auto add = [&](const Entity* e) {
        if (!e || !e->alive() || !e->maxHp) return;
        const float f = (float)e->hp / (float)e->maxHp;
        if (f < best) { best = f; at = e->guid; }
    };
    add(player_);
    for (Entity* b : bots_) add(b);
    if (who) *who = at;
    return best;
}

int World::nearbyHostileCount(const Entity& around, float range) const {
    int n = 0;
    for (const Entity& e : pool_)
        if (e.used && e.objType == 3 && e.alive() && hostileTowards(around, e) &&
            dist2d(around.x, around.y, e.x, e.y) <= range)
            ++n;
    return n;
}

uint64_t World::lowestHealthEnemy(const Entity& around, float range) const {
    uint64_t bestGuid = 0;
    int32_t bestHp = 0x7FFFFFFF;
    float bestD = 1e9f;
    for (const Entity& e : pool_) {
        if (!e.used || e.objType != 3 || !e.alive() || !hostileTowards(around, e)) continue;
        const float d = dist2d(around.x, around.y, e.x, e.y);
        if (d > range) continue;
        if (e.hp < bestHp || (e.hp == bestHp && d < bestD)) { bestHp = e.hp; bestD = d; bestGuid = e.guid; }
    }
    return bestGuid;
}

// ---------------------------------------------------------------- threat
bool World::addThreat(Entity& target, uint64_t who, int32_t amount) {
    if (!who || amount <= 0 || target.dead) return false;
    for (uint8_t i = 0; i < target.threatCount; ++i)
        if (target.threat[i].who == who) {
            target.threat[i].value += amount;
            target.lastThreatMs = now_;
            return true;
        }
    if (target.threatCount < kMaxThreat) {
        target.threat[target.threatCount++] = Entity::ThreatEntry{who, amount};
        target.lastThreatMs = now_;
        return true;
    }
    // Replace the weakest entry if this one is stronger.
    uint8_t weakest = 0;
    for (uint8_t i = 1; i < target.threatCount; ++i)
        if (target.threat[i].value < target.threat[weakest].value) weakest = i;
    if (target.threat[weakest].value < amount) {
        target.threat[weakest] = Entity::ThreatEntry{who, amount};
        target.lastThreatMs = now_;
        return true;
    }
    return false;
}

int32_t World::threatOf(const Entity& target, uint64_t who) const {
    for (uint8_t i = 0; i < target.threatCount; ++i)
        if (target.threat[i].who == who) return target.threat[i].value;
    return 0;
}

void World::taunt(Entity& target, uint64_t who) {
    int32_t top = 0;
    for (uint8_t i = 0; i < target.threatCount; ++i) top = std::max(top, target.threat[i].value);
    addThreat(target, who, top + 1 - threatOf(target, who));
    target.victim = who;
    target.target = who;
    emit(SimEvent::ThreatChanged, who, target.guid, 0, top + 1);
}

uint64_t World::highestThreatTarget(const Entity& e) const {
    uint64_t best = 0;
    int32_t bestV = -1;
    for (uint8_t i = 0; i < e.threatCount; ++i) {
        const Entity* who = entity(e.threat[i].who);
        if (!who || !who->alive()) continue;
        if (e.threat[i].value > bestV) { bestV = e.threat[i].value; best = e.threat[i].who; }
    }
    return best;
}

void World::updateThreatDecay(uint32_t nowMs) {
    for (Entity& e : pool_) {
        if (!e.used || e.objType != 3) continue;
        uint8_t out = 0;
        for (uint8_t i = 0; i < e.threatCount; ++i) {
            const Entity* who = entity(e.threat[i].who);
            if (!who || !who->alive() || dist2d(e.x, e.y, who->x, who->y) > 60.0f) continue;
            e.threat[out++] = e.threat[i];
        }
        e.threatCount = out;
        // A creature that has been out of combat for a while forgets its table.
        if (!e.victim && nowMs - e.lastDamageMs > 8000) e.threatCount = 0;
    }
}

// ---------------------------------------------------------------- position
void World::applyGround(Entity& e) {
    if (!groundFn_) return;
    float z = e.z;
    if (groundFn_(e.x, e.y, e.z, &z)) e.z = z;
}

void World::setPosition(Entity& e, float x, float y, float z, float o) {
    e.x = x; e.y = y; e.z = z; e.o = o;
    if (!e.self && !e.bot) { e.homeX = x; e.homeY = y; e.homeZ = z; e.homeO = o; }
}

void World::moveEntity(Entity& e, float tx, float ty, uint32_t nowMs, float stopAt) {
    (void)nowMs;
    e.moveX = tx;
    e.moveY = ty;
    e.moveStopAt = stopAt;
    e.moving = true;
    e.moveTargetValidUntil = now_ + 2000;
}

void World::moveToward(Entity& e, float tx, float ty, uint32_t nowMs, float stopAt) {
    const float dx = tx - e.x, dy = ty - e.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= stopAt || d < 0.01f) { e.moving = false; return; }
    if (e.stunned(now_) || e.rooted(now_)) { e.moving = false; return; }   // WoWVita: control auras
    const float step = e.speed * e.speedFactor(now_) * ((float)kSimStepMs / 1000.0f);
    const float k = std::min(1.0f, step / d);
    e.x += dx * k;
    e.y += dy * k;
    e.o = angleTo(e.x, e.y, tx, ty);
    e.moving = true;
    (void)nowMs;
    applyGround(e);
}

// ---------------------------------------------------------------- combat actions
bool World::startAttack(Entity& actor, uint64_t targetGuid, uint32_t nowMs) {
    (void)nowMs;
    if (!actor.alive()) return false;
    Entity* t = entity(targetGuid);
    if (!t || !t->alive() || t->guid == actor.guid) return false;
    if (actor.bot || actor.self) {
        if (!canAttack(actor, *t)) return false;
    }
    actor.victim = t->guid;
    actor.target = t->guid;
    if (!actor.nextSwingMs) actor.nextSwingMs = now_ + kSwingWindupMs;
    addThreat(*t, actor.guid, 1);
    if (t->victim == 0) {
        t->victim = actor.guid;
        t->target = actor.guid;
        if (t->objType == 3) t->aiState = 1;
    }
    emit(SimEvent::AttackStarted, actor.guid, t->guid, 0, 0);
    return true;
}

void World::stopAttack(Entity& actor, uint32_t nowMs) {
    (void)nowMs;
    if (!actor.victim) return;
    emit(SimEvent::AttackStopped, actor.guid, actor.victim, 0, 0);
    actor.victim = 0;
    actor.target = 0;
}

uint32_t World::cooldownLeft(const Entity& e, uint32_t spell, uint32_t nowMs) const {
    for (uint8_t i = 0; i < e.cooldownCount; ++i)
        if (e.cooldowns[i].spell == spell)
            return e.cooldowns[i].endMs > nowMs ? e.cooldowns[i].endMs - nowMs : 0;
    return 0;
}

bool World::knowsSpell(const Entity& e, uint32_t spell) const {
    for (uint8_t i = 0; i < e.spellCount; ++i)
        if (e.spells[i] == spell) return true;
    if (e.self)   // WoWVita: the spell book
        for (uint32_t s : profiles_[0].book) if (s == spell) return true;
    return false;
}

bool World::hasAura(const Entity& e, uint32_t spell) const {
    for (uint8_t i = 0; i < e.auraCount; ++i)
        if (e.auras[i].spell == spell) return true;
    return false;
}

void World::addAura(Entity& e, const Aura& a0) {
    Aura a = a0;
    if (a.kind == (uint8_t)SpellKind::Buff && !a.type) a.positive = true;
    for (uint8_t i = 0; i < e.auraCount; ++i)
        if (e.auras[i].spell == a.spell && (e.auras[i].caster == a.caster || a.positive) && e.auras[i].effIndex == a.effIndex) {
            a.slot = e.auras[i].slot;   // WoWVita: renewed in its slot
            e.auras[i] = a;
            emit(SimEvent::AuraApplied, e.guid, a.caster, a.spell, (int32_t)(a.endMs - now_), a.school, (uint8_t)std::max<int8_t>(0, a.slot));
            return;
        }
    if (e.auraCount >= kMaxAuras) removeAura(e, 0);   // the oldest goes
    // WoWVita: a client slot of its own while it lasts (buffs 0..31, debuffs 32..47); the other effects of the same
    // spell share it (one icon)
    a.slot = -1;
    for (uint8_t i = 0; i < e.auraCount; ++i)
        if (e.auras[i].spell == a.spell && e.auras[i].caster == a.caster) a.slot = e.auras[i].slot;
    const bool shared = a.slot >= 0;
    if (!shared) a.slot = freeAuraSlot(e, a.positive);
    e.auras[e.auraCount++] = a;
    if (!shared && a.slot >= 0)
        emit(SimEvent::AuraApplied, e.guid, a.caster, a.spell, a.endMs ? (int32_t)(a.endMs - now_) : -1, a.school, (uint8_t)a.slot);
}

int8_t World::freeAuraSlot(const Entity& e, bool positive) const {
    for (int s = positive ? 0 : 32; s < (positive ? 32 : 48); ++s) {
        bool taken = false;
        for (uint8_t i = 0; i < e.auraCount && !taken; ++i) taken = e.auras[i].slot == s;
        if (!taken) return (int8_t)s;
    }
    return -1;
}

void World::addTimedAura(Entity& e, uint32_t spell, uint32_t durationMs) {
    Aura a;
    a.spell = spell;
    a.caster = e.guid;
    a.kind = (uint8_t)SpellKind::Buff;
    a.endMs = now_ + durationMs;
    addAura(e, a);
}

void World::removeAura(Entity& e, int index) {
    if (index < 0 || index >= e.auraCount) return;
    const Aura gone = e.auras[index];
    for (int i = index; i + 1 < e.auraCount; ++i) e.auras[i] = e.auras[i + 1];
    --e.auraCount;
    if (gone.type == 36 && e.form == (uint8_t)gone.misc) leaveForm(e, e.form);   // WoWVita: out of the stance / form
    // WoWVita: a control aura that ends early (broken by damage, dispelled) takes its effect along
    if (gone.type) {
        auto stillType = [&](std::initializer_list<uint8_t> ts) {
            for (uint8_t i = 0; i < e.auraCount; ++i) for (uint8_t t : ts) if (e.auras[i].type == t) return true;
            return false;
        };
        if (gone.type == 12 && !stillType({12})) e.stunnedUntilMs = 0;
        if (gone.type == 26 && !stillType({26})) e.rootUntilMs = 0;
        if ((gone.type == 5 || gone.type == 7) && !stillType({5, 7})) { e.fearUntilMs = 0; if (e.self) e.stunnedUntilMs = 0; }
        if ((gone.type == 27 || gone.type == 60) && !stillType({27, 60})) e.silenceUntilMs = 0;
        if (e.self && (gone.type == 29 || gone.type == 34 || gone.type == 142)) recomputeGear();
    }
    bool still = false;   // WoWVita: the slot empties when the spell's last effect goes
    for (uint8_t i = 0; i < e.auraCount; ++i) still = still || (e.auras[i].slot == gone.slot && gone.slot >= 0);
    if (!still && gone.slot >= 0) emit(SimEvent::AuraRemoved, e.guid, 0, gone.spell, gone.slot);
}

bool World::tryCast(Entity& actor, uint32_t spellId, uint64_t targetGuid, uint32_t nowMs) {
    if (!data_ || !actor.alive()) return false;
    if (spellEngine(actor, spellId)) return castX(actor, spellId, targetGuid, nowMs);   // WoWVita: class spells
    const SpellDef* s = data_->spell(spellId);
    if (!s) return false;
    if (actor.castSpell) { emit(SimEvent::CastFailed, actor.guid, 0, spellId, 0, 0, 1); return false; }
    // WoWVita: stunned / feared / confused nothing, silenced no spells, pacified no weapon strikes
    if (actor.stunned(nowMs) || actor.feared(nowMs) ||
        (actor.silenced(nowMs) && s->kind != SpellKind::Melee && s->school != 0) ||
        (actor.pacified(nowMs) && s->kind == SpellKind::Melee)) {
        emit(SimEvent::CastFailed, actor.guid, 0, spellId, 0, 0, 11);
        return false;
    }
    if (actor.gcdEndMs > nowMs && s->kind != SpellKind::Melee) {
        emit(SimEvent::CastFailed, actor.guid, 0, spellId, 0, 0, 2);
        return false;
    }
    if (cooldownLeft(actor, spellId, nowMs) > 0) {
        emit(SimEvent::CastFailed, actor.guid, 0, spellId, 0, 0, 3);
        return false;
    }
    if (s->powerCost > 0) {
        if (actor.power < (int32_t)s->powerCost) {
            emit(SimEvent::CastFailed, actor.guid, 0, spellId, 0, 0, 4);
            return false;
        }
    }
    Entity* target = entity(targetGuid);
    if (s->friendly && actor.objType == 3) {   // WoWVita: a creature heals itself or one of its faction
        if (!target || !target->alive() || target->objType != 3 || target->faction != actor.faction) target = &actor;
    } else if (s->kind == SpellKind::Buff && actor.objType == 3) {
        target = &actor;
    } else if (s->friendly) {
        if (!target || !target->alive() || !(target->self || target->bot)) target = &actor;
        if (!target->alive()) {
            emit(SimEvent::CastFailed, actor.guid, targetGuid, spellId, 0, 0, 5);
            return false;
        }
    } else if (s->kind == SpellKind::Buff) {
        target = &actor;   // self buffs in this subset (Battle Shout): never on the selected enemy
    } else if (s->kind == SpellKind::Taunt) {
        if (!target) target = &actor;
    }
    if (!target) {
        emit(SimEvent::CastFailed, actor.guid, targetGuid, spellId, 0, 0, 6);
        return false;
    }
    const float maxRange = s->maxRange > 0 ? s->maxRange : kMeleeReach;
    const float d = dist2d(actor.x, actor.y, target->x, target->y);
    if (d > maxRange + target->boundingRadius || d < s->minRange) {
        emit(SimEvent::CastFailed, actor.guid, target->guid, spellId, 0, 0, 7);
        return false;
    }
    if (!s->friendly && s->kind != SpellKind::Buff && s->kind != SpellKind::Heal &&
        !canAttack(actor, *target) && !target->self && !target->bot) {
        emit(SimEvent::CastFailed, actor.guid, target->guid, spellId, 0, 0, 8);
        return false;
    }

    if (s->powerCost > 0) {
        actor.power -= (int32_t)s->powerCost;
        emit(SimEvent::PowerChanged, actor.guid, 0, 0, actor.power);
    }
    const uint32_t cd = s->cooldownMs ? s->cooldownMs : s->categoryCooldownMs;
    // WoWVita: a scripted cast (the Hearthstone) starts its cooldown when it finishes, as the original: moving breaks it for free
    if (cd && !(s->kind == SpellKind::Script && s->castMs)) {
        bool stored = false;
        for (uint8_t i = 0; i < actor.cooldownCount; ++i)
            if (actor.cooldowns[i].spell == spellId) {
                actor.cooldowns[i].endMs = nowMs + cd;
                actor.cooldowns[i].lengthMs = cd;
                stored = true;
            }
        if (!stored && actor.cooldownCount < kMaxCooldowns)
            actor.cooldowns[actor.cooldownCount++] = Cooldown{spellId, nowMs + cd, cd};
    }
    if (s->kind != SpellKind::Melee && spellId != 6603) actor.gcdEndMs = nowMs + kGcdMs;

    actor.castTarget = target->guid;
    actor.target = target->guid;
    if (s->castMs > 0) {
        actor.castSpell = spellId;
        actor.castStartMs = nowMs;
        actor.castEndMs = nowMs + s->castMs;
        actor.castInterruptible = actor.objType == 3 ? 1 : 0;
        emit(SimEvent::CastStarted, actor.guid, target->guid, spellId, (int32_t)s->castMs, s->school);
        return true;
    }
    actor.castSpell = spellId;
    actor.castStartMs = nowMs;
    actor.castEndMs = nowMs;
    finishCast(actor, nowMs);
    return true;
}

void World::cancelCast(Entity& e, uint8_t reason, uint32_t nowMs) {
    if (!e.castSpell) return;
    if (e.channeling) endChannel(e, nowMs);   // WoWVita: a channel broken
    emit(SimEvent::CastFailed, e.guid, e.castTarget, e.castSpell, reason, 0, 0);
    e.castSpell = 0;
    e.castEndMs = e.castStartMs = 0;
    e.castTarget = 0;
    (void)nowMs;
}

void World::finishCast(Entity& e, uint32_t nowMs) {
    (void)nowMs;
    const uint32_t spellId = e.castSpell;
    const uint64_t targetGuid = e.castTarget;
    e.castSpell = 0;
    e.castEndMs = e.castStartMs = 0;
    e.castTarget = 0;
    const SpellDef* s = data_ ? data_->spell(spellId) : nullptr;
    if (!s) { emit(SimEvent::CastFailed, e.guid, 0, spellId, 0, 0, 9); return; }
    Entity* t = entity(targetGuid);
    if ((!t || !t->alive()) && s->friendly) t = &e;
    if (!t || (!t->alive() && s->kind != SpellKind::Buff)) {
        emit(SimEvent::CastFailed, e.guid, targetGuid, spellId, 0, 0, 10);
        return;
    }
    emit(SimEvent::CastFinished, e.guid, t->guid, spellId, 0, s->school);
    if (const SpellControl* ctl = data_ ? data_->control(spellId) : nullptr) {   // WoWVita: stun, root, slow, ...
        if (t != &e && t->alive()) {
            applyControl(*t, *ctl, now_);
            if (s->kind != SpellKind::Debuff && s->kind != SpellKind::Dot) {   // (a strike with a root: show its aura)
                Aura a;
                a.spell = spellId; a.caster = e.guid; a.kind = (uint8_t)SpellKind::Debuff; a.school = s->school;
                a.endMs = now_ + ctl->durationMs;
                addAura(*t, a);
            }
        }
    }

    switch (s->kind) {
        case SpellKind::Melee:
        case SpellKind::Damage: {
            int32_t dmg = rng_.range(s->amountMin, s->amountMax);
            if (s->kind == SpellKind::Melee && e.objType == 3) dmg += rng_.range((int32_t)e.dmgMin, (int32_t)e.dmgMax);   // WoWVita: + its swing
            applyDamage(e, *t, dmg, spellId, s->school, now_);
            if (s->threatBonus) addThreat(*t, e.guid, s->threatBonus);
            if (s->startsAutoAttack) startAttack(e, t->guid, now_);
            break;
        }
        case SpellKind::Heal:
            applyHeal(e, *t, rng_.range(s->amountMin, s->amountMax), spellId, now_);
            break;
        case SpellKind::Dot: {
            Aura a;
            a.spell = spellId;
            a.caster = e.guid;
            a.kind = (uint8_t)SpellKind::Dot;
            a.school = s->school;
            a.amount = rng_.range(s->amountMin, s->amountMax);
            a.endMs = now_ + s->durationMs;
            a.nextTickMs = now_ + (s->tickMs ? s->tickMs : 2000);
            addAura(*t, a);
            break;
        }
        case SpellKind::Buff: {
            Aura a;
            a.spell = spellId;
            a.caster = e.guid;
            a.kind = (uint8_t)SpellKind::Buff;
            a.school = s->school;
            a.endMs = s->durationMs ? now_ + s->durationMs : 0;
            a.nextTickMs = 0;
            addAura(*t, a);
            break;
        }
        case SpellKind::Debuff: {   // WoWVita: a creature's aura on its enemy
            Aura a;
            a.spell = spellId;
            a.caster = e.guid;
            a.kind = (uint8_t)SpellKind::Debuff;
            a.school = s->school;
            a.endMs = now_ + (s->durationMs ? s->durationMs : 10000);
            addAura(*t, a);
            if (t->objType == 3 && !t->victim && canAttack(*t, e)) { t->victim = t->target = e.guid; addThreat(*t, e.guid, 1); }
            break;
        }
        case SpellKind::Taunt:
            taunt(*t, e.guid);
            break;
        case SpellKind::Script: {   // WoWVita: the session reacts on CastFinished; the cooldown starts now
            const uint32_t cd = s->cooldownMs ? s->cooldownMs : s->categoryCooldownMs;
            bool stored = false;
            for (uint8_t i = 0; i < e.cooldownCount; ++i)
                if (e.cooldowns[i].spell == spellId) { e.cooldowns[i] = Cooldown{spellId, now_ + cd, cd}; stored = true; }
            if (cd && !stored && e.cooldownCount < kMaxCooldowns) e.cooldowns[e.cooldownCount++] = Cooldown{spellId, now_ + cd, cd};
            break;
        }
        case SpellKind::Interrupt:
            if (t->castSpell) {
                cancelCast(*t, 35, now_);   // SPELL_FAILED_INTERRUPTED
                emit(SimEvent::Message, e.guid, t->guid, spellId, 0);
            }
            break;
    }
}

void World::applyDamage(Entity& src, Entity& dst, int32_t amount, uint32_t spell, uint8_t school, uint32_t nowMs) {
    if (!dst.alive() || amount <= 0) return;
    if (dst.objType == 3 && dst.aiState == 3) return;   // WoWVita: an evading creature cannot be hurt
    if (hasAuraType(dst, 39, 1 << school)) { emit(SimEvent::Damage, src.guid, dst.guid, spell, 0, school, 4); return; }   // WoWVita: immune
    amount += auraSum(src, 13, 1 << school) + auraSum(dst, 14, 1 << school);   // WoWVita: flat damage done / taken
    if (amount <= 0) return;
    if (dst.objType == 3) { dst.refreshMs = nowMs; dst.refreshX = dst.x; dst.refreshY = dst.y; }   // its combat timer
    int32_t dmg = amount;
    // WoWVita: resurrection sickness - the original takes 75% of the stats; offline it takes 75% of the damage done
    if (src.self && hasAura(src, kResurrectionSickness)) dmg = std::max<int32_t>(1, dmg / 4);
    {   // WoWVita: aura armour, damage done / taken %, absorb shields
        const int32_t armor = std::max<int32_t>(0, (int32_t)dst.armor + auraSum(dst, 22, 1));
        if (armor > 0 && school == 0) dmg = dmg - dmg * armor / (armor + 400);
        const int32_t pct = 100 + auraSum(src, 79, 1 << school) + auraSum(dst, 87, 1 << school);
        if (pct != 100) dmg = dmg * std::max(0, pct) / 100;
        if (dmg < 1) dmg = 1;
        const int32_t left = absorb(dst, dmg, school);
        if (left <= 0) { emit(SimEvent::Damage, src.guid, dst.guid, spell, 0, school, 4); return; }   // (absorbed)
        dmg = left;
    }
    if (dmg < 1) dmg = 1;
    dst.hp -= dmg;
    dst.lastDamageMs = nowMs;
    dst.incomingDamage = std::min<int32_t>(dst.incomingDamage + dmg, 100000);
    if (dst.self) gainRage(dst, dmg, false);   // WoWVita
    interruptAuras(dst, 0x2 | (spell ? 0u : 0x01000000u));   // WoWVita: AURA_INTERRUPT_FLAG_DAMAGE (stealth, polymorph)
    if (&src != &dst) interruptAuras(src, 0x1000);         // ATTACKING
    else if (dst.powerType == 1 && dst.alive())   // rage from being hit
        dst.power = std::min<int32_t>(dst.power + 5, (int32_t)dst.maxPower);
    emit(SimEvent::Damage, src.guid, dst.guid, spell, dmg, school);
    if (dst.hp <= 0) { dst.hp = 0; onDeath(dst, src, nowMs); return; }
    if (dst.objType == 3 && src.guid) {
        addThreat(dst, src.guid, dmg);
        if (!dst.victim && canAttack(dst, src)) {
            dst.victim = src.guid;
            dst.target = src.guid;
            dst.aiState = 1;
        }
    }
    // A unit that is hit by a spell interrupts its own creature filler cast.
    // WoWVita: one with EventAI casts as in the original: a hit only pushes the cast back (500 ms, twice at most)
    if (dst.objType == 3 && dst.castSpell && behaviourData() && data_->ai(dst.entry)) {
        if (dst.castPushes < 2) { dst.castEndMs += 500; ++dst.castPushes; }
    } else if (dst.objType == 3 && dst.castSpell && dst.castInterruptible) cancelCast(dst, 35, nowMs);
}

void World::applyHeal(Entity& src, Entity& dst, int32_t amount, uint32_t spell, uint32_t nowMs) {
    if (!dst.alive() || amount <= 0) return;
    const int32_t before = dst.hp;
    dst.hp = std::min<int32_t>(dst.hp + amount, (int32_t)dst.maxHp);
    const int32_t healed = dst.hp - before;
    emit(SimEvent::Heal, src.guid, dst.guid, spell, healed, 0);
    // Healing threat: half of the effective heal, on every creature that already has the healed
    // unit on its threat table (the vanilla "healers pull aggro" rule, simplified).
    for (Entity& e : pool_) {
        if (!e.used || e.objType != 3 || e.dead) continue;
        if (threatOf(e, dst.guid) > 0 || e.victim == dst.guid) addThreat(e, src.guid, healed / 2 + 1);
    }
    (void)nowMs;
}

void World::resolveSwing(Entity& a, Entity& b, uint32_t nowMs) {
    if (!a.alive() || !b.alive()) { a.victim = 0; return; }
    if (!inMeleeRange(a, b)) return;
    if (a.stunnedUntilMs > nowMs || a.feared(nowMs) || a.pacified(nowMs)) return;
    a.o = angleTo(a.x, a.y, b.x, b.y);
    {   // WoWVita: SPELL_AURA_MOD_HASTE (Slice and Dice, Thunder Clap)
        const int32_t haste = std::max(-80, auraSum(a, 138));
        a.nextSwingMs = nowMs + (uint32_t)((int64_t)a.attackMs * 100 / (100 + haste));
    }
    if ((int32_t)rng_.below(100) < kMissChancePct + auraSum(b, 49)) {   // (WoWVita: + dodge auras: Evasion)
        emit(SimEvent::Damage, a.guid, b.guid, 0, 0, 0, 1);   // miss
        return;
    }
    // WoWVita: a queued "on next swing" spell (Heroic Strike) is this swing, if there is the rage for it
    if (a.nextSwingSpell && spellEngine(a, a.nextSwingSpell)) {
        const uint32_t id = a.nextSwingSpell;
        a.nextSwingSpell = 0;
        const SpellX& sx = *data_->sx(id);
        const uint32_t cost = powerCostX(a, sx);
        if (a.power >= (int32_t)cost) {
            a.castTriggered = false;
            finishX(a, sx, b.guid, nowMs);
            return;
        }
    }
    int32_t dmg = rng_.range((int32_t)a.dmgMin, (int32_t)a.dmgMax);
    if (const int32_t ap = auraSum(a, 99)) dmg += ap * (int32_t)a.attackMs / 14000;   // WoWVita: attack power auras
    if (hasAuraType(a, 67)) dmg = std::max(1, dmg / 2);                              // WoWVita: disarmed
    if (const int32_t block = auraSum(b, 51); block > 0 && (int32_t)rng_.below(100) < block) {   // Shield Block
        // (the pack has no shield block values: the strength part of CMaNGOS GetShieldBlockValue and a level-sized shield)
        dmg = std::max(0, dmg - ((int32_t)b.statStr / 20 + 5 + (int32_t)b.level));
        for (int i = 0; i < b.auraCount; ++i)
            if (b.auras[i].type == 51 && b.auras[i].charges) { if (--b.auras[i].charges == 0) removeAura(b, i); break; }
    }
    // WoWVita: wear (vMaNGOS DurabilityLossChance.Damage 0.5 %): the weapon that hits, a piece of armour that is hit
    if (a.self && rng_.below(1000) < 5) wearSlot(15, 1);
    if (b.self && rng_.below(1000) < 5) {
        static const int armour[] = {0, 2, 4, 5, 6, 7, 8, 9, 14, 16};
        wearSlot(armour[rng_.below(10)], 1);
    }
    const bool crit = (int32_t)rng_.below(100) < kCritChancePct;
    if (crit) dmg *= 2;
    applyDamage(a, b, dmg, 0, 0, nowMs);
    if (a.self) gainRage(a, dmg, true);   // WoWVita: as the original (Player::RewardRage)
    else if (a.powerType == 1 && a.alive())   // rage from landing a swing
        a.power = std::min<int32_t>(a.power + 12, (int32_t)a.maxPower);
    // WoWVita: damage shields (Thorns) strike back
    if (b.alive() && a.alive())
        for (uint8_t i = 0; i < b.auraCount; ++i)
            if (b.auras[i].type == 15 && b.auras[i].amount > 0) {
                applyDamage(b, a, b.auras[i].amount, b.auras[i].spell, b.auras[i].school, nowMs);
                if (!a.alive()) break;
            }
    if (a.alive() && b.alive()) { procAuras(a, b, 0x4, nowMs); procAuras(b, a, 0x8, nowMs); }   // WoWVita: procs
    if (crit) {
        // Mark the last damage event as a critical for the HUD.
        for (int i = eventCount_ - 1; i >= 0; --i)
            if (events_[i].type == SimEvent::Damage && events_[i].a == a.guid && events_[i].b == b.guid) {
                events_[i].flags |= 2;
                break;
            }
    }
}

void World::onDeath(Entity& e, Entity& killer, uint32_t nowMs) {
    if (e.self && killer.objType == 3) durabilityLossAll(10);   // WoWVita: killed by a creature: 10 % durability
    e.dead = true;
    e.hp = 0;
    e.victim = 0;
    e.target = 0;
    e.castSpell = 0;
    e.auraCount = 0;
    e.cooldownCount = 0;
    e.gcdEndMs = 0;
    e.aiState = 4;
    e.threatCount = 0;
    e.moving = false;
    if (e.netMoveState == 1) e.netMoveState = 2;   // the client gets one stop at the death spot
    emit(SimEvent::Death, e.guid, killer.guid, 0, 0);
    if (e.objType == 3) aiEvent(e, 6, nowMs, killer.guid);                          // WoWVita: EventAI death
    if (killer.objType == 3 && (e.self || e.bot)) aiEvent(killer, 5, nowMs, e.guid);  // and kill
    if (e.objType == 3) {
        rollLoot(e);
        if (const CreatureTemplate* t = data_ ? data_->creature(e.entry) : nullptr) {
            if (killer.self || killer.bot) {
                addXp(*player_, t->xp, nowMs, e.guid);
                if (onKillCredit) onKillCredit(e.entry, e.guid);   // WoWVita: the quests of the server layer
                // Quest progress for every accepted kill quest.
                for (QuestLogEntry& q : questLog_) {
                    const QuestDef* qd = nullptr;
                    for (const QuestDef& d : data_->quests) if (d.id == q.id) qd = &d;
                    if (!qd || qd->objective.kind != 0 || qd->objective.target != e.entry) continue;
                    if (q.complete) continue;
                    if (q.kills[0] < qd->objective.count) ++q.kills[0];
                    emit(SimEvent::QuestProgress, player_->guid, q.id, 0, q.kills[0]);
                    if (q.kills[0] >= qd->objective.count) {
                        q.complete = true;
                        emit(SimEvent::QuestComplete, player_->guid, q.id, 0, q.kills[0]);
                    }
                }
            }
        }
        const Spawn* spawn = nullptr;
        if (data_) {
            const uint32_t low = (uint32_t)(e.guid & 0xFFFFFF);
            if (low >= 1 && low <= data_->spawns.size()) spawn = &data_->spawns[low - 1];
        }
        e.respawnAtMs = nowMs + (spawn ? spawn->respawnMs : 30000u);
        if (e.respawnAtMs < nowMs + 5000) e.respawnAtMs = nowMs + 5000;
        emit(SimEvent::LootReady, e.guid, killer.guid, 0, (int32_t)e.lootCount);
    } else if (e.bot) {
        e.recoveringUntilMs = nowMs + 10000;
        e.respawnAtMs = nowMs + 10000;
    } else if (e.self) {
        e.respawnAtMs = 0;   // the player releases the spirit / reclaims the corpse
    }
}

void World::respawn(Entity& e, uint32_t nowMs) {
    (void)nowMs;
    e.dead = false;
    e.hp = (int32_t)e.maxHp;
    e.power = (int32_t)e.maxPower;
    e.auraCount = 0;
    e.threatCount = 0;
    e.victim = e.target = 0;
    e.castSpell = 0;
    e.nextSwingMs = 0;
    e.respawnAtMs = 0;
    e.lootCount = 0;
    e.lootMoney = 0;
    e.looted = false;
    e.aiState = 0;
    e.x = e.homeX; e.y = e.homeY; e.z = e.homeZ; e.o = e.homeO;
    if (e.bot) {
        // Companions come back beside the player, not at the creature's spawn.
        if (player_) { e.x = player_->x - 2.0f; e.y = player_->y - 2.0f; e.z = player_->z; }
        e.recoveringUntilMs = 0;
    }
    e.netMoveState = 1;   // make the next movement update send a "stop" at the new place
    e.moving = false;
    applyGround(e);
    emit(SimEvent::Respawn, e.guid, 0, 0, 0);
    if (e.objType == 3 && !e.bot) initBehaviour(e, e.spawnIndex, nowMs);   // WoWVita
}

void World::rollLoot(Entity& e) {
    e.lootCount = 0;
    e.lootMoney = 0;
    e.looted = false;
    const CreatureTemplate* t = data_ ? data_->creature(e.entry) : nullptr;
    if (!t || !t->lootId) return;
    const LootTable* lt = data_->lootTable(t->lootId);
    if (!lt) return;
    if (lt->moneyMax > lt->moneyMin) e.lootMoney = (uint32_t)rng_.range((int32_t)lt->moneyMin, (int32_t)lt->moneyMax);
    else e.lootMoney = lt->moneyMin;
    // WoWVita: quest drops first (only while a quest in the log still wants the item)
    auto qd = data_->questLoot.find(t->lootId);
    if (qd != data_->questLoot.end() && questItemWanted)
        for (const QuestDrop& d : qd->second) {
            if (e.lootCount >= kMaxLoot) break;
            if (!questItemWanted(d.item)) continue;
            if ((uint32_t)rng_.below(10000) >= d.chance) continue;
            LootSlotState& s = e.loot[e.lootCount++];
            s.item = d.item;
            s.count = 1;
            s.taken = false;
        }
    for (const LootItem& li : lt->items) {
        if (e.lootCount >= kMaxLoot) break;
        if ((uint32_t)rng_.below(10000) >= li.chance) continue;
        LootSlotState& s = e.loot[e.lootCount++];
        s.item = li.item;
        s.count = (uint16_t)rng_.range(li.minCount, li.maxCount);
        s.taken = false;
    }
    if (e.lootMoney && e.lootCount < kMaxLoot) {
        // Money is shown as the first slot in the vanilla loot window; the server layer encodes it
        // separately (SMSG_LOOT_RESPONSE money field), so nothing is stored here.
    }
}

// ---------------------------------------------------------------- loot / items / xp
bool World::openLoot(Entity& corpse, uint32_t nowMs, std::string* why) {
    (void)nowMs;
    if (!corpse.dead) { if (why) *why = "That creature is still alive."; return false; }
    if (corpse.looted) { if (why) *why = "There is nothing left to loot."; return false; }
    if (!player_) return false;
    if (dist2d(player_->x, player_->y, corpse.x, corpse.y) > 6.0f) {
        if (why) *why = "You are too far away to loot that corpse.";
        return false;
    }
    return true;
}

bool World::takeLoot(Entity& corpse, int slot, uint32_t nowMs) {
    if (slot < 0 || slot >= corpse.lootCount) return false;
    LootSlotState& s = corpse.loot[(size_t)slot];
    if (s.taken) return false;
    s.taken = true;
    addItemToPlayer(s.item, s.count, nowMs);
    bool left = false;
    for (int i = 0; i < corpse.lootCount; ++i) left = left || !corpse.loot[(size_t)i].taken;
    if (!left && !corpse.lootMoney) corpse.looted = true;
    return true;
}

uint32_t World::takeLootMoney(Entity& corpse, uint32_t nowMs) {
    const uint32_t m = corpse.lootMoney;
    corpse.lootMoney = 0;
    if (m) addMoneyToPlayer(m, nowMs);
    bool left = false;
    for (int i = 0; i < corpse.lootCount; ++i) left = left || !corpse.loot[(size_t)i].taken;
    if (!left) corpse.looted = true;
    return m;
}

void World::addItemToPlayer(uint32_t entry, uint16_t count, uint32_t nowMs) {
    if (!entry || !count || !player_) return;
    CharacterProfile& p = profiles_[0];
    const ItemTemplate* it = data_ ? data_->item(entry) : nullptr;
    const uint32_t stackMax = it ? std::max<uint32_t>(1u, it->maxStack) : 1u;
    uint16_t left = count;
    // stack into existing bag items first
    for (uint8_t i = 0; i < p.bagCount && left; ++i) {
        if (p.bag[i].entry != entry || p.bag[i].count >= stackMax) continue;
        const uint16_t add = (uint16_t)std::min<uint32_t>(left, stackMax - p.bag[i].count);
        p.bag[i].count = (uint16_t)(p.bag[i].count + add);
        left = (uint16_t)(left - add);
        emit(SimEvent::ItemGained, player_->guid, entry, 0, add);
    }
    while (left && p.bagCount < bagCapacity()) {
        BagItem& b = p.bag[p.bagCount];
        b.entry = entry;
        b.slot = p.bagCount;
        b.count = (uint16_t)std::min<uint32_t>(left, stackMax);
        left = (uint16_t)(left - b.count);
        ++p.bagCount;
        emit(SimEvent::ItemGained, player_->guid, entry, 0, b.count);
    }
    (void)nowMs;
}

// ---------------------------------------------------------------- WoWVita: the bag for quests
uint32_t World::countPlayerItem(uint32_t entry) const {
    const CharacterProfile& p = profiles_[0];
    uint32_t n = 0;
    for (uint8_t i = 0; i < p.bagCount; ++i)
        if (p.bag[i].entry == entry) n += p.bag[i].count ? p.bag[i].count : 1;
    return n;
}

uint32_t World::removePlayerItem(uint32_t entry, uint32_t count) {
    CharacterProfile& p = profiles_[0];
    uint32_t removed = 0;
    for (int i = (int)p.bagCount - 1; i >= 0 && removed < count; --i) {
        if (p.bag[i].entry != entry) continue;
        const uint16_t have = p.bag[i].count ? p.bag[i].count : 1;
        const uint32_t take = std::min<uint32_t>(have, count - removed);
        removed += take;
        if (take >= have) {
            for (int k = i; k + 1 < p.bagCount; ++k) { p.bag[k] = p.bag[k + 1]; p.bag[k].slot = (uint8_t)k; }
            --p.bagCount;
            p.bag[p.bagCount] = BagItem();
        } else {
            p.bag[i].count = (uint16_t)(have - take);
        }
    }
    return removed;
}

// ---------------------------------------------------------------- WoWVita: vendors
bool World::buyItem(uint32_t entry, uint32_t count, uint32_t price, uint32_t nowMs, int* why) {
    if (why) *why = 0;
    if (!player_ || !entry || !count) return false;
    if (player_->money < price) { if (why) *why = 1; return false; }
    CharacterProfile& p = profiles_[0];
    const ItemTemplate* it = data_ ? data_->item(entry) : nullptr;
    const uint32_t stackMax = it ? std::max<uint32_t>(1u, it->maxStack) : 1u;
    uint32_t room = 0;   // the 16 visible backpack slots
    for (uint8_t i = 0; i < p.bagCount; ++i)
        if (p.bag[i].entry == entry && p.bag[i].count < stackMax) room += stackMax - p.bag[i].count;
    if (p.bagCount < bagCapacity()) room += (bagCapacity() - p.bagCount) * stackMax;
    if (room < count) { if (why) *why = 2; return false; }
    player_->money -= price;
    p.money = player_->money;
    addItemToPlayer(entry, (uint16_t)std::min<uint32_t>(count, 65535u), nowMs);
    return true;
}

uint32_t World::bagCapacity() const {
    const CharacterProfile& p = profiles_[0];
    uint32_t n = 16;
    for (uint32_t b : p.bags)
        if (const ItemExt* x = b && data_ ? data_->ext(b) : nullptr) n += x->slots;
    return std::min<uint32_t>(n, 160);
}

bool World::putBagItem(const BagItem& b) {
    CharacterProfile& p = profiles_[0];
    if (!b.entry || p.bagCount >= bagCapacity()) return false;
    p.bag[p.bagCount] = b;
    p.bag[p.bagCount].slot = p.bagCount;
    ++p.bagCount;
    return true;
}

bool World::takeFromBag(int index, uint16_t n) {
    CharacterProfile& p = profiles_[0];
    if (index < 0 || index >= p.bagCount || !p.bag[index].entry) return false;
    const uint16_t have = p.bag[index].count ? p.bag[index].count : 1;
    if (n >= have) removeBagItem(index);
    else p.bag[index].count = (uint16_t)(have - n);
    return true;
}

void World::removeAuraSpell(Entity& e, uint32_t spell) {
    for (int i = 0; i < e.auraCount;) {   // (WoWVita: every effect of it)
        if (e.auras[i].spell == spell) { removeAura(e, i); i = 0; continue; }   // (from the start: a form takes its boosts along)
        ++i;
    }
}

void World::wearSlot(int slot, uint16_t points) {
    if (!player_ || slot < 0 || slot >= 19) return;
    CharacterProfile& p = profiles_[0];
    const uint16_t m = maxDurability(p.equipment[(size_t)slot]);
    if (!m || !points || p.equipLost[(size_t)slot] >= m) return;
    const bool was = broken(p.equipment[(size_t)slot], p.equipLost[(size_t)slot]);
    p.equipLost[(size_t)slot] = (uint16_t)std::min<uint32_t>(m, (uint32_t)p.equipLost[(size_t)slot] + points);
    if (!was && broken(p.equipment[(size_t)slot], p.equipLost[(size_t)slot])) recomputeGear();
    emit(SimEvent::Durability, player_->guid, 0, 0, slot);
}

void World::durabilityLossAll(uint32_t percent) {
    if (!player_) return;
    for (int s = 0; s < 19; ++s) {
        const uint16_t m = maxDurability(profiles_[0].equipment[(size_t)s]);
        if (m) wearSlot(s, (uint16_t)std::max<uint32_t>(1, m * percent / 100));
    }
}

void World::removeBagItem(int index) {
    CharacterProfile& p = profiles_[0];
    if (index < 0 || index >= p.bagCount) return;
    for (int i = index; i + 1 < p.bagCount; ++i) { p.bag[i] = p.bag[i + 1]; p.bag[i].slot = (uint8_t)i; }
    --p.bagCount;
    p.bag[p.bagCount] = BagItem();
}

uint32_t World::sellBagItem(int index, uint32_t count, uint32_t nowMs) {
    (void)nowMs;
    if (!player_) return 0;
    CharacterProfile& p = profiles_[0];
    if (index < 0 || index >= p.bagCount || !p.bag[index].entry) return 0;
    BagItem& b = p.bag[index];
    const ItemTemplate* it = data_ ? data_->item(b.entry) : nullptr;
    if (!it || !it->sellPrice) return 0;
    const uint16_t have = b.count ? b.count : 1;
    const uint16_t n = (count == 0 || count >= have) ? have : (uint16_t)count;
    const uint32_t copper = it->sellPrice * n;
    if (n >= have) {   // the slot empties: the rest moves up
        for (int i = index; i + 1 < p.bagCount; ++i) { p.bag[i] = p.bag[i + 1]; p.bag[i].slot = (uint8_t)i; }
        --p.bagCount;
        p.bag[p.bagCount] = BagItem();
    } else {
        b.count = (uint16_t)(have - n);
    }
    player_->money += copper;
    p.money = player_->money;
    return copper;
}

namespace {
// InventoryType (1.12) -> the first equipment slot (-1: not worn)
int equipSlotFor(uint32_t invType) {
    switch (invType) {
        case 1: return 0;  case 2: return 1;  case 3: return 2;  case 4: return 3;  case 5: case 20: return 4;
        case 6: return 5;  case 7: return 6;  case 8: return 7;  case 9: return 8;  case 10: return 9;
        case 11: return 10; case 12: return 12; case 16: return 14; case 19: return 18;
        case 13: case 17: case 21: return 15;
        case 14: case 22: case 23: return 16;
        case 15: case 25: case 26: return 17;
        default: return -1;
    }
}
}  // namespace

void World::recomputeGear() {
    if (!player_) return;
    const CharacterProfile& p = profiles_[0];
    Entity& e = *player_;
    e.dmgMin = 3; e.dmgMax = 6; e.attackMs = 2000;
    if (const ItemTemplate* it = data_ && !broken(p.equipment[15], p.equipLost[15]) ? data_->item(p.equipment[15]) : nullptr) {
        if (it->delayMs >= 500) e.attackMs = it->delayMs;
        if (it->dmgMax > 0) {
            e.dmgMin = (uint32_t)std::max(1.0f, it->dmgMin);
            e.dmgMax = (uint32_t)std::max(1.0f, it->dmgMax);
        }
    }
    uint32_t armor = 0;
    for (int s = 0; s < 19; ++s)
        if (const ItemTemplate* it = data_ && !broken(p.equipment[(size_t)s], p.equipLost[(size_t)s]) ? data_->item(p.equipment[(size_t)s]) : nullptr)
            armor += it->armor;
    e.armor = armor;
    if (const int32_t pct = auraSum(e, 142, 1)) e.armor = (uint32_t)std::max<int64_t>(0, (int64_t)armor * (100 + pct) / 100);   // bear: +180 %
    // WoWVita: the race and class's stats at its level (player_levelstats / player_classlevelstats) and the gear's;
    // health = base + stamina (the first 20 one each, then 10), mana = base + intellect (20 one each, then 15),
    // melee attack power by class (1.12), its bonus to a swing AP / 14 per second of the weapon's speed
    const uint32_t statKey = (uint32_t)p.race | ((uint32_t)p.cls << 8) | ((uint32_t)std::min<uint16_t>(e.level, 60) << 16);
    if (data_ && data_->playerStats.count(statKey)) {
        const PlayerLevelStats& b = data_->playerStats.at(statKey);
        int32_t st[8] = {0, 0, 0, 0, 0, 0, 0, 0};   // 0 mana 1 health 3 agi 4 str 5 int 6 spi 7 sta
        for (int s = 0; s < 19; ++s) {
            if (!p.equipment[(size_t)s] || broken(p.equipment[(size_t)s], p.equipLost[(size_t)s])) continue;
            auto is = data_->itemStats.find(p.equipment[(size_t)s]);
            if (is == data_->itemStats.end()) continue;
            for (const auto& [type, v] : is->second) if (type < 8) st[type] += v;
        }
        e.baseMana = (uint32_t)b.mana;
        e.statStr = b.str + st[4]; e.statAgi = b.agi + st[3]; e.statSta = b.sta + st[7];
        e.statInt = b.inte + st[5]; e.statSpi = b.spi + st[6];
        const int32_t hpSta = std::min(e.statSta, 20) + std::max(0, e.statSta - 20) * 10;
        const uint32_t oldMax = e.maxHp;
        e.maxHp = (uint32_t)std::max(1, b.hp + hpSta + st[1] + auraSum(e, 34));   // (+ SPELL_AURA_MOD_INCREASE_HEALTH)
        if (oldMax && e.hp > 0) e.hp = std::min<int32_t>((int32_t)e.maxHp, e.hp + (int32_t)e.maxHp - (int32_t)oldMax);
        if (e.powerType == 0 && b.mana) {
            const int32_t mInt = std::min(e.statInt, 20) + std::max(0, e.statInt - 20) * 15;
            const uint32_t oldP = e.maxPower;
            e.maxPower = (uint32_t)std::max(1, b.mana + mInt + st[0]);
            if (oldP) e.power = std::min<int32_t>((int32_t)e.maxPower, e.power + (int32_t)e.maxPower - (int32_t)oldP);
        }
        const int L = e.level;
        switch (p.cls) {   // (CMaNGOS StatSystem UpdateAttackPowerAndDamage)
            case 1: case 2: e.attackPower = L * 3 + e.statStr * 2 - 20; break;           // warrior, paladin
            case 7: e.attackPower = L * 2 + e.statStr * 2 - 20; break;                   // shaman
            case 3: case 4: e.attackPower = L * 2 + e.statStr + e.statAgi - 20; break;   // hunter, rogue
            case 11: e.attackPower = L * 3 + e.statStr * 2 - 20 + (e.form == 1 ? e.statAgi : 0); break;   // druid (cat: + agility)
            default: e.attackPower = e.statStr - 10; break;                              // mage, priest, warlock
        }
        // Player::InitDataForForm / UpdateDamagePhysical: cat 1.0 s, bear 2.5 s, the paw's damage by level
        if (e.form == 1 || e.form == 5 || e.form == 8) {
            e.attackMs = e.form == 1 ? 1000 : 2500;
            const float sp = (float)e.attackMs / 1000.0f, lv = (float)std::min<int>(L, 60);
            e.dmgMin = (uint32_t)std::max(1.0f, lv * 0.85f * sp);
            e.dmgMax = (uint32_t)std::max(1.0f, lv * 1.25f * sp);
        }
        e.attackPower = std::max(0, e.attackPower);
        const uint32_t bonus = (uint32_t)(e.attackPower * (int32_t)e.attackMs / 14000);
        e.dmgMin += bonus; e.dmgMax += bonus;
        profiles_[0].maxHp = e.maxHp; profiles_[0].maxPower = e.maxPower;
    }
}

int World::equipFromBag(int index, int slotsOut[2]) {
    slotsOut[0] = slotsOut[1] = -1;
    if (!player_) return 1;
    CharacterProfile& p = profiles_[0];
    if (index < 0 || index >= p.bagCount || !p.bag[index].entry) return 1;
    const ItemTemplate* it = data_ ? data_->item(p.bag[index].entry) : nullptr;
    if (!it) return 1;
    if (it->inventoryType == 18) {   // WoWVita: a bag goes into the first free bag slot (19..22)
        const ItemExt* x = data_->ext(it->entry);
        if (!x || !x->slots) return 1;
        int k = 0;
        while (k < 4 && p.bags[(size_t)k]) ++k;
        if (k == 4) return 3;
        p.bags[(size_t)k] = it->entry;
        for (int i = index; i + 1 < p.bagCount; ++i) { p.bag[i] = p.bag[i + 1]; p.bag[i].slot = (uint8_t)i; }
        --p.bagCount;
        p.bag[p.bagCount] = BagItem();
        slotsOut[0] = 19 + k;
        return 0;
    }
    int slot = equipSlotFor(it->inventoryType);
    if (slot < 0) return 1;
    if (it->requiredLevel > player_->level) return 2;
    if (slot == 10 && p.equipment[10] && !p.equipment[11]) slot = 11;   // the second ring / trinket slot if free
    if (slot == 12 && p.equipment[12] && !p.equipment[13]) slot = 13;
    const bool twoHand = it->inventoryType == 17;
    const ItemTemplate* main = data_->item(p.equipment[15]);
    const bool mainIsTwoHand = main && main->inventoryType == 17;
    // a two-hander frees the off hand (into the backpack); an off-hand item sends a two-hander back
    int freeSlot = -1;
    if ((twoHand && p.equipment[16]) || (slot == 16 && mainIsTwoHand)) {
        if (p.bagCount >= bagCapacity()) return 3;
        freeSlot = twoHand ? 16 : 15;
    }
    const uint32_t newEntry = p.bag[index].entry;
    const uint16_t newLost = p.bag[index].lost;   // WoWVita: durability goes with the item
    const uint32_t old = p.equipment[(size_t)slot];
    const uint16_t oldLost = p.equipLost[(size_t)slot];
    p.equipment[(size_t)slot] = newEntry;
    p.equipLost[(size_t)slot] = newLost;
    if (old) {
        p.bag[index].entry = old;
        p.bag[index].count = 1;
        p.bag[index].lost = oldLost;
    } else {
        for (int i = index; i + 1 < p.bagCount; ++i) { p.bag[i] = p.bag[i + 1]; p.bag[i].slot = (uint8_t)i; }
        --p.bagCount;
        p.bag[p.bagCount] = BagItem();
    }
    slotsOut[0] = slot;
    if (freeSlot >= 0 && p.equipment[(size_t)freeSlot]) {
        BagItem& b = p.bag[p.bagCount];
        b.entry = p.equipment[(size_t)freeSlot];
        b.count = 1;
        b.slot = p.bagCount;
        b.lost = p.equipLost[(size_t)freeSlot];
        ++p.bagCount;
        p.equipment[(size_t)freeSlot] = 0;
        p.equipLost[(size_t)freeSlot] = 0;
        slotsOut[1] = freeSlot;
    }
    recomputeGear();
    return 0;
}

void World::addMoneyToPlayer(uint32_t copper, uint32_t nowMs) {
    if (!player_ || !copper) return;
    player_->money += copper;
    profiles_[0].money = player_->money;
    emit(SimEvent::Looted, player_->guid, 0, 0, (int32_t)copper);
    (void)nowMs;
}

void World::addXp(Entity& e, uint32_t xp, uint32_t nowMs, uint64_t victim, bool silent) {
    if (!e.self && !e.bot) return;
    if (!xp) return;
    // WoWVita: rested - a kill gives as much again from the pool, which shrinks by that (vMaNGOS Player::GetXPRestBonus)
    uint32_t rested = 0;
    if (victim && e.self && e.level < 60) {
        float& pool = profiles_[0].restBonus;
        rested = std::min<uint32_t>(xp, (uint32_t)std::max(0.0f, pool));
        pool -= (float)rested;
        if (pool < 0) pool = 0;
    }
    xp += rested;
    e.levelXp += xp;
    emit(SimEvent::XpGain, e.guid, victim, rested, (int32_t)xp, 0, silent ? 1 : 0);
    while (e.nextLevelXp && e.levelXp >= e.nextLevelXp && e.level < 60) {
        e.levelXp -= e.nextLevelXp;
        ++e.level;
        e.nextLevelXp = xpForNextLevel(e.level);
        const bool tables = e.self && data_ && !data_->playerStats.empty();
        if (tables) { profiles_[0].level = e.level; recomputeGear(); }   // WoWVita: the level's stats
        else { e.maxHp += 20; e.maxPower += 15; }
        e.hp = (int32_t)e.maxHp;
        if (e.powerType != 1) e.power = (int32_t)e.maxPower;
        emit(SimEvent::LevelUp, e.guid, 0, 0, e.level);
    }
    CharacterProfile& p = profiles_[0];
    p.level = e.level;
    p.xp = e.levelXp;
    p.nextXp = e.nextLevelXp;
    p.maxHp = e.maxHp;
    p.maxPower = e.maxPower;
    p.hp = e.hp;
    p.power = e.power;
    (void)nowMs;
}

// ---------------------------------------------------------------- auras and regen
void World::tickAuras(Entity& e, uint32_t nowMs) {
    for (int i = 0; i < e.auraCount;) {
        Aura& a = e.auras[i];
        if (a.endMs && nowMs >= a.endMs) { removeAura(e, i); continue; }
        if (a.type) {   // WoWVita: a spell_template aura
            tickAuraX(e, a, nowMs);
            if (!e.used || !e.alive()) return;
            ++i;
            continue;
        }
        if (a.kind == (uint8_t)SpellKind::Dot && a.nextTickMs && nowMs >= a.nextTickMs) {
            a.nextTickMs = nowMs + 2000;
            const SpellDef* s = data_ ? data_->spell(a.spell) : nullptr;
            if (s && s->tickMs) a.nextTickMs = nowMs + s->tickMs;
            Entity* caster = entity(a.caster);
            if (caster && caster->alive()) applyDamage(*caster, e, a.amount, a.spell, a.school, nowMs);
            else if (e.alive()) {
                e.hp -= a.amount;
                emit(SimEvent::Damage, a.caster, e.guid, a.spell, a.amount, a.school);
                if (e.hp <= 0) { e.hp = 0; if (caster) onDeath(e, *caster, nowMs); else { e.dead = true; emit(SimEvent::Death, e.guid, 0, 0, 0); } }
            }
            // applyDamage may have changed auraCount through onDeath; re-check the slot.
            if (!e.alive()) return;
        }
        ++i;
    }
}

void World::tickRegen(Entity& e, uint32_t nowMs) {
    if (!e.alive()) return;
    const bool combat = e.victim != 0 || (nowMs - e.lastDamageMs < 5000);
    if (e.self) {
        // WoWVita: the player regenerates as in the original - a tick every 2 s (Player::RegenerateAll), health only
        // out of combat (about 3 % of it; food and drink are what refills faster), mana a little in combat too
        if ((nowMs / 2000) == ((nowMs - kSimStepMs) / 2000)) return;
        if (e.powerType == 0 && e.power < (int32_t)e.maxPower && e.statSpi > 0) {
            // WoWVita: 1.12's spirit regeneration per 2 s tick (Player::OCTRegenMPPerSpirit), none for 5 s after spending mana
            if (nowMs - e.lastManaSpendMs >= 5000) {
                const uint8_t c = profiles_[0].cls;
                const float r = (c == 8 || c == 5) ? 12.5f + e.statSpi / 4.0f : c == 7 ? 17.0f + e.statSpi / 5.0f : 15.0f + e.statSpi / 5.0f;
                e.power = std::min<int32_t>(e.power + std::max<int32_t>(1, (int32_t)r), (int32_t)e.maxPower);
            }
        } else if (e.powerType == 0 && e.power < (int32_t)e.maxPower)
            e.power = std::min<int32_t>(e.power + std::max<int32_t>(1, (int32_t)e.maxPower * (combat ? 1 : 3) / 100), (int32_t)e.maxPower);
        else if (e.powerType == 3 && e.power < (int32_t)e.maxPower)
            e.power = std::min<int32_t>(e.power + 20, (int32_t)e.maxPower);   // energy: 20 per 2 s
        else if (e.powerType == 1 && !combat && e.power > 0)
            e.power = std::max<int32_t>(0, e.power - 2);                     // rage decays
        if (!combat && e.hp < (int32_t)e.maxHp)
            e.hp = std::min<int32_t>(e.hp + std::max<int32_t>(1, (int32_t)e.maxHp * 3 / 100), (int32_t)e.maxHp);
        return;
    }
    if (e.powerType == 0) {   // mana
        const int32_t gain = std::max<int32_t>(1, (int32_t)(e.maxPower / (combat ? 100 : 35)));
        if (e.power < (int32_t)e.maxPower) {
            e.power = std::min<int32_t>(e.power + gain, (int32_t)e.maxPower);
        }
    } else if (e.powerType == 3) {   // energy
        const int32_t gain = std::max<int32_t>(1, (int32_t)(e.maxPower / 10));
        if (e.power < (int32_t)e.maxPower) e.power = std::min<int32_t>(e.power + gain, (int32_t)e.maxPower);
    } else if (e.powerType == 1) {   // rage decays out of combat
        if (!combat && e.power > 0 && (nowMs / 2000) != ((nowMs - kSimStepMs) / 2000)) --e.power;
    }
    if (!combat && e.hp < (int32_t)e.maxHp) {
        const int32_t gain = std::max<int32_t>(1, (int32_t)(e.maxHp / 200));
        e.hp = std::min<int32_t>(e.hp + gain, (int32_t)e.maxHp);
    }
    if (e.incomingDamage > 0 && (nowMs % 1000) < kSimStepMs) e.incomingDamage = e.incomingDamage * 3 / 4;
}

// ---------------------------------------------------------------- creature AI
void World::selectCreatureTarget(Entity& e, uint32_t nowMs) {
    const uint64_t top = highestThreatTarget(e);
    if (top) {
        Entity* t = entity(top);
        if (t && t->alive()) { e.victim = top; e.target = top; return; }
    }
    // No threat: look for the nearest hostile party member inside the aggro radius.
    const CreatureTemplate* c = data_ ? data_->creature(e.entry) : nullptr;
    const float aggro = c ? std::max(4.0f, c->aggroRange) : 10.0f;
    Entity* best = nullptr;
    float bestD = aggro;
    auto consider = [&](Entity* p) {
        if (!p || !p->alive() || !hostileTowards(e, *p)) return;
        if (!detectsStealth(e, *p)) return;   // WoWVita: a rogue in stealth passes unseen
        const float d = dist2d(e.x, e.y, p->x, p->y);
        if (d < bestD) { bestD = d; best = p; }
    };
    consider(player_);
    for (Entity* b : bots_) consider(b);
    if (best) {
        e.victim = best->guid;
        e.target = best->guid;
        addThreat(e, best->guid, 10);
    }
    (void)nowMs;
}

void World::tickCreature(Entity& e, uint32_t nowMs) {
    if (e.dead) {
        if (e.respawnAtMs && nowMs >= e.respawnAtMs) {
            // WoWVita: a pooled spawn (a rare, Hogger) comes back at one of its pool's places, maybe another one
            const bool pooled = data_ && e.spawnIndex >= 0 && data_->poolOf.count((uint32_t)e.spawnIndex);
            if (pooled) {
                const auto pit = data_->poolOf.find((uint32_t)e.spawnIndex);
                const SpawnPool& pool = data_->pools[pit->second];
                std::vector<uint32_t> free;
                for (uint32_t m : pool.members) if (m == (uint32_t)e.spawnIndex || (m < poolChosen_.size() && !poolChosen_[m])) free.push_back(m);
                const uint32_t next = free.empty() ? (uint32_t)e.spawnIndex : free[rng_.below((uint32_t)free.size())];
                if (next != (uint32_t)e.spawnIndex && next < poolChosen_.size()) {
                    poolChosen_[(uint32_t)e.spawnIndex] = 0;
                    poolChosen_[next] = 1;
                    spawnRespawnAt_[next] = 0;
                    releaseCreature(e, nowMs);
                    return;
                }
            }
            respawn(e, nowMs);
        }
        return;
    }
    if (behaviourData()) { tickBehaviour(e, nowMs); return; }   // WoWVita: movement, evade, help, EventAI
    // Evade: too far from home, or the target is unreachable for too long.
    if (e.victim) {
        Entity* t = entity(e.victim);
        if (!t || !t->alive()) { e.victim = e.target = 0; e.threatCount = 0; }
        else if (dist2d(e.x, e.y, e.homeX, e.homeY) > kEvadeDist ||
                 (dist2d(e.x, e.y, t->x, t->y) > kAggroLeash && nowMs - e.lastDamageMs > 5000)) {
            e.victim = e.target = 0;
            e.threatCount = 0;
            e.hp = (int32_t)e.maxHp;
            e.auraCount = 0;
            e.x = e.homeX; e.y = e.homeY; e.z = e.homeZ; e.o = e.homeO;
            e.aiState = 0;
            emit(SimEvent::ThreatChanged, e.guid, 0, 0, 0);
        }
    }
    if (!e.victim) selectCreatureTarget(e, nowMs);
    if (!e.victim) {
        // wander home slowly if displaced
        if (dist2d(e.x, e.y, e.homeX, e.homeY) > 1.0f) moveToward(e, e.homeX, e.homeY, nowMs, 0.5f);
        return;
    }
    Entity* t = entity(e.victim);
    if (!t || !t->alive()) { e.victim = e.target = 0; return; }
    e.o = angleTo(e.x, e.y, t->x, t->y);
    if (!inMeleeRange(e, *t)) {
        moveToward(e, t->x, t->y, nowMs, kMeleeReach * 0.9f);
        if (e.castSpell) cancelCast(e, 35, nowMs);
        return;
    }
    // In reach: use a creature ability occasionally (damage spells only in the sample data).
    if (e.nextAbilityMs && nowMs >= e.nextAbilityMs && e.spellCount && !e.castSpell) {
        const uint32_t sp = e.spells[(uint32_t)rng_.below(e.spellCount)];
        if (data_ && data_->spell(sp)) {
            const SpellDef* sd = data_->spell(sp);
            if (sd->kind == SpellKind::Damage || sd->kind == SpellKind::Dot || sd->kind == SpellKind::Melee)
                tryCast(e, sp, t->guid, nowMs);
        }
        e.nextAbilityMs = nowMs + 6000 + rng_.below(4000);
    }
}

// ---------------------------------------------------------------- step
void World::tickEntity(Entity& e, uint32_t nowMs, bool isPlayer) {
    if (!e.used) return;
    if (e.incomingDamage && (nowMs % 1000) < kSimStepMs) e.incomingDamage = e.incomingDamage * 3 / 4;
    tickAuras(e, nowMs);
    if (!e.used) return;
    if (!e.alive()) {
        if (e.objType == 3) tickCreature(e, nowMs);
        else if (e.bot && e.respawnAtMs && nowMs >= e.respawnAtMs) respawn(e, nowMs);
        return;
    }
    tickRegen(e, nowMs);
    if (e.castSpell && nowMs >= e.castEndMs) {
        if (e.channeling) { endChannel(e, nowMs); e.castSpell = 0; e.castEndMs = e.castStartMs = 0; }   // WoWVita
        else if (spellEngine(e, e.castSpell)) {
            const uint32_t id = e.castSpell;
            const uint64_t tg = e.castTarget;
            e.castSpell = 0;
            e.castEndMs = e.castStartMs = 0;
            finishX(e, *data_->sx(id), tg, nowMs);
        } else finishCast(e, nowMs);
    }
    if (e.objType == 3) {
        tickCreature(e, nowMs);
    } else if (e.bot) {
        tickBot(e, nowMs);
        if (e.moving) moveToward(e, e.moveX, e.moveY, nowMs, e.moveStopAt);
    }
    // Auto-attack
    if (e.victim && e.alive()) {
        Entity* t = entity(e.victim);
        if (!t || !t->alive()) { e.victim = e.target = 0; }
        else if (e.objType == 3 && (e.fleeUntilMs > nowMs || e.aiState == 3 || e.castSpell || e.feared(nowMs))) {}   // WoWVita: no swings
        else if (nowMs >= e.nextSwingMs) resolveSwing(e, *t, nowMs);
    }
    (void)isPlayer;
}

void World::step(uint32_t nowMs) {
    if (!lastStepMs_) lastStepMs_ = nowMs;
    if (nowMs < lastStepMs_) lastStepMs_ = nowMs;   // a clock reset must not stall the world
    uint32_t steps = 0;
    while (nowMs - lastStepMs_ >= kSimStepMs && steps < 8) {
        lastStepMs_ += kSimStepMs;
        now_ = lastStepMs_;
        ++tick_;
        // Deterministic pool order: index order.
        for (Entity& e : pool_) tickEntity(e, now_, e.self);
        tickAreas(now_);   // WoWVita: Blizzard, Consecration
        updateThreatDecay(now_);
        ++steps;
    }
    // Too far behind (a long loading screen): drop the backlog rather than run a burst of catch-up.
    if (nowMs - lastStepMs_ > 8 * kSimStepMs) lastStepMs_ = nowMs;
    now_ = nowMs;
    updateActivation(nowMs, false);
}

// ---------------------------------------------------------------- WoWVita: the spawn grid
namespace {
constexpr float kCellYards = 64.0f;
constexpr float kAppearDist = 100.0f;   // a spawn this close to the player gets a creature
constexpr float kVanishDist = 130.0f;   // a creature (and its home) this far away goes back to the list
inline int64_t cellKey(int cx, int cy) { return ((int64_t)cx << 32) ^ (int64_t)(uint32_t)cy; }
inline int cellOf(float v) { return (int)std::floor(v / kCellYards); }
}  // namespace

void World::buildSpawnCells() {
    spawnCells_.clear();
    cellsFor_ = data_;
    cellsSpawnCount_ = data_ ? data_->spawns.size() : 0;
    spawnRespawnAt_.assign(cellsSpawnCount_, 0);
    spawnActive_.assign(cellsSpawnCount_, 0);
    for (uint32_t i = 0; i < (uint32_t)cellsSpawnCount_; ++i) {
        const Spawn& sp = data_->spawns[i];
        spawnCells_[cellKey(cellOf(sp.x), cellOf(sp.y))].push_back(i);
    }
    choosePools();   // WoWVita
}

int World::activeCreatures() const {
    int n = 0;
    for (const Entity& e : pool_) if (e.used && e.objType == 3) ++n;
    return n;
}

void World::updateActivation(uint32_t nowMs, bool initial) {
    if (!data_ || !player_) return;
    if (cellsFor_ != data_ || cellsSpawnCount_ != data_->spawns.size()) buildSpawnCells();
    if (!initial && nowMs < nextActivationMs_) return;
    nextActivationMs_ = nowMs + 500;
    const float px = player_->x, py = player_->y;

    // 1. creatures far from the player (and not chasing anyone) leave the pool; a dead one keeps its respawn time
    int used = 0;
    for (Entity& e : pool_) {
        if (!e.used) continue;
        ++used;
        if (e.objType != 3) continue;
        const float d = std::min(dist2d(e.x, e.y, px, py), dist2d(e.homeX, e.homeY, px, py));
        if (d <= kVanishDist) continue;
        if (!e.dead && e.victim) continue;
        const uint32_t idx = (uint32_t)(e.guid & 0xFFFFFF);
        if (idx >= 1 && idx <= spawnRespawnAt_.size()) {
            spawnRespawnAt_[idx - 1] = e.dead ? std::max(e.respawnAtMs, nowMs + 5000) : 0;
            spawnActive_[idx - 1] = 0;
        }
        if (!initial) emit(SimEvent::Vanish, e.guid, 0, 0, 0);
        for (Entity& o : pool_)   // nobody keeps a pointer-by-guid to it
            if (o.used) {
                if (o.victim == e.guid) o.victim = 0;
                if (o.target == e.guid) o.target = 0;
            }
        e = Entity();
        --used;
    }

    // 2. spawns near the player get a creature, the nearest first, while the pool has room
    struct Cand { float d; uint32_t idx; };
    std::vector<Cand> cands;
    const int r = (int)std::ceil(kAppearDist / kCellYards);
    const int cx0 = cellOf(px), cy0 = cellOf(py);
    for (int cy = cy0 - r; cy <= cy0 + r; ++cy)
        for (int cx = cx0 - r; cx <= cx0 + r; ++cx) {
            auto it = spawnCells_.find(cellKey(cx, cy));
            if (it == spawnCells_.end()) continue;
            for (uint32_t idx : it->second) {
                if (spawnActive_[idx]) continue;
                if (spawnRespawnAt_[idx] && nowMs < spawnRespawnAt_[idx]) continue;
                if (idx < poolChosen_.size() && !poolChosen_[idx]) continue;   // WoWVita: another of its pool is up
                const Spawn& sp = data_->spawns[idx];
                const float d = dist2d(sp.x, sp.y, px, py);
                if (d <= kAppearDist) cands.push_back(Cand{d, idx});
            }
        }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
    for (const Cand& c : cands) {
        if (used >= kMaxEntities - 1) break;
        const Spawn& sp = data_->spawns[c.idx];
        spawnCreature(sp, nowMs, (int)c.idx);
        const CreatureTemplate* t = data_->creature(sp.entry);
        if (!t) { spawnActive_[c.idx] = 1; continue; }   // (an unknown template never comes back)
        const uint64_t guid = 0xF130000000000000ull | ((uint64_t)(t->entry & 0xFFFFFF) << 24) | (uint64_t)(c.idx + 1);
        if (!entity(guid)) break;   // the pool is full
        spawnActive_[c.idx] = 1;
        spawnRespawnAt_[c.idx] = 0;
        ++used;
        if (!initial) emit(SimEvent::Appear, guid, 0, 0, 0);
    }
}

// ---------------------------------------------------------------- bots helpers used by the director
void World::tickBot(Entity& e, uint32_t nowMs) {
    if (!botsEnabled_ || !e.bot) return;
    if (e.recoveringUntilMs && nowMs < e.recoveringUntilMs) {
        // Retreat: walk away from the fight while the recovery timer runs.
        if (player_) {
            const float dx = e.x - player_->x, dy = e.y - player_->y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < 14.0f) moveToward(e, e.x + (d > 0.1f ? dx / d : 1.0f) * 2.0f, e.y + (d > 0.1f ? dy / d : 0.0f) * 2.0f, nowMs, 0.0f);
        }
        return;
    }
    if (e.nextDecisionMs > nowMs) return;
    e.nextDecisionMs = nowMs + 200;
    botThink(*this, e, nowMs);
    // Rejoin if the party has been left far behind: run faster first, teleport only when it is really lost.
    if (player_) {
        const float d = dist2d(e.x, e.y, player_->x, player_->y);
        e.speed = (d > 28.0f && !e.victim) ? 9.5f : 7.0f;
        if (d > 90.0f) {
            e.x = player_->x + 2.0f;
            e.y = player_->y + 2.0f;
            e.z = player_->z;
            e.moving = false;
            e.netMoveState = 1;   // the next movement update sends one stop at the new place
            applyGround(e);
            emit(SimEvent::Message, e.guid, 0, 0, 0);
        }
    }
}

void World::setPartyCommand(int botIndex, uint8_t mode, uint32_t nowMs) {
    Entity* b = bot(botIndex);
    if (!b) return;
    b->tacticalMode = mode > 3 ? 1 : mode;
    b->nextDecisionMs = nowMs;
    if (mode == 3) b->recoveringUntilMs = nowMs + 6000;   // retreat: break off for six seconds
    else b->recoveringUntilMs = 0;
    if (mode == 2 || mode == 3) { b->victim = 0; b->target = 0; }
    emit(SimEvent::Message, b->guid, 0, 0, mode);
}

void World::setAllPartyCommands(uint8_t mode, uint32_t nowMs) {
    for (int i = 0; i < kMaxParty; ++i) setPartyCommand(i, mode, nowMs);
}

void World::handlePartyChat(const std::string& text, uint32_t nowMs) {
    std::string t;
    for (char c : text) t += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    // Commands may be prefixed with '!' or '@' so they never collide with normal party chat.
    size_t at = 0;
    while (at < t.size() && (t[at] == '!' || t[at] == '@' || t[at] == ' ')) ++at;
    t = t.substr(at);
    if (t == "follow" || t == "follow me") setAllPartyCommands(0, nowMs);
    else if (t == "assist" || t == "attack") setAllPartyCommands(1, nowMs);
    else if (t == "stay" || t == "hold") setAllPartyCommands(2, nowMs);
    else if (t == "retreat" || t == "fall back") setAllPartyCommands(3, nowMs);
    else if (t == "tank pull") { if (bot(0)) setPartyCommand(0, 1, nowMs); }
    else if (t == "heal me") { if (bot(1)) setPartyCommand(1, 1, nowMs); }
    else if (t.rfind("moon", 0) == 0) emitText(SimEvent::Message, player_ ? player_->guid : 0, "The party acknowledges with a moon.");
    else emitText(SimEvent::Message, player_ ? player_->guid : 0, "Party commands: follow, assist, stay, retreat.");
}

// ---------------------------------------------------------------- resurrection
void World::revive(Entity& e, int32_t hpPercent, uint32_t nowMs) {
    if (e.alive()) return;
    e.dead = false;
    e.hp = (int32_t)std::max<int32_t>(1, (int32_t)e.maxHp * std::max<int32_t>(1, std::min<int32_t>(100, hpPercent)) / 100);
    e.power = (int32_t)e.maxPower;
    e.victim = 0;
    e.target = 0;
    e.castSpell = 0;
    e.auraCount = 0;
    e.threatCount = 0;
    e.nextSwingMs = 0;
    e.respawnAtMs = 0;
    e.aiState = 0;
    if (!e.self && !e.bot) { e.x = e.homeX; e.y = e.homeY; e.z = e.homeZ; }
    emit(SimEvent::Respawn, e.guid, 0, 0, hpPercent);
    (void)nowMs;
}

// ---------------------------------------------------------------- quests
bool World::acceptQuest(uint32_t questId, uint32_t nowMs) {    if (!data_) return false;
    const QuestDef* q = nullptr;
    for (const QuestDef& d : data_->quests) if (d.id == questId) q = &d;
    if (!q) return false;
    for (const QuestLogEntry& e : questLog_) if (e.id == questId) return true;
    if (questLog_.size() >= 20) return false;
    QuestLogEntry e;
    e.id = questId;
    questLog_.push_back(e);
    emit(SimEvent::QuestProgress, player_ ? player_->guid : 0, questId, 0, 0);
    (void)nowMs;
    return true;
}

int World::questKillProgress(uint32_t questId) const {
    for (const QuestLogEntry& e : questLog_)
        if (e.id == questId) return e.kills[0];
    return 0;
}

bool World::questComplete(uint32_t questId) const {
    for (const QuestLogEntry& e : questLog_)
        if (e.id == questId) return e.complete;
    return false;
}

bool World::turnInQuest(uint32_t questId, uint32_t nowMs) {
    if (!data_ || !player_) return false;
    const QuestDef* q = nullptr;
    for (const QuestDef& d : data_->quests) if (d.id == questId) q = &d;
    if (!q) return false;
    bool found = false;
    for (size_t i = 0; i < questLog_.size(); ++i)
        if (questLog_[i].id == questId) {
            if (!questLog_[i].complete) return false;
            questLog_.erase(questLog_.begin() + (long)i);
            found = true;
            break;
        }
    if (!found) return false;
    addXp(*player_, q->rewardXp, nowMs);
    if (q->rewardMoney) addMoneyToPlayer(q->rewardMoney, nowMs);
    for (int i = 0; i < 3; ++i)
        if (q->rewardItems[(size_t)i]) addItemToPlayer(q->rewardItems[(size_t)i], q->rewardCounts[(size_t)i] ? q->rewardCounts[(size_t)i] : 1, nowMs);
    emit(SimEvent::QuestComplete, player_->guid, questId, 0, 1);
    return true;
}

// ---------------------------------------------------------------- profiles
void World::syncProfileFromEntity(int index) {
    Entity* e = index == 0 ? player_ : bot(index - 1);
    if (!e) return;
    CharacterProfile& p = (index == 0) ? profiles_[0] : profiles_[1 + (index - 1)];
    p.level = e->level;
    p.xp = e->levelXp;
    p.nextXp = e->nextLevelXp;
    p.hp = e->hp;
    p.maxHp = e->maxHp;
    p.power = e->power;
    p.maxPower = e->maxPower;
    p.powerType = e->powerType;
    if (e->formPower) { p.powerType = e->savedPowerType; p.power = e->savedPower; p.maxPower = e->savedMaxPower; }   // (not a form's)
    p.x = e->x; p.y = e->y; p.z = e->z; p.o = e->o;
    p.money = e->money;
    p.used = true;
}

}  // namespace offline
}  // namespace w3n
