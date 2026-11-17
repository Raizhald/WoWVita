// offline_core.hpp — authoritative, server-independent world simulation for WoW3DS offline mode.
//
// This is the "server side" of the offline game:
//   * immutable world data (creature/item/spell/loot/quest/bot templates, spawns) loaded from a .w3do pack,
//   * mutable runtime state (entities, health, threat, auras, cooldowns, combat, loot, XP),
//   * a fixed 50 ms simulation step that never depends on the render rate,
//   * no network, no heap allocation during combat (fixed-size pools),
//   * deterministic integer combat maths and a seeded xorshift RNG.
//
// The net/offline_server layer turns the events produced here into the exact 1.12.1 SMSG packets that the
// existing GameSession already understands. Nothing in this file knows about packets, sockets or rendering.
//
// Supported Vanilla subset (documented in docs/OFFLINE_ARCHITECTURE.md): melee, direct damage/heal spells,
// one damage-over-time aura, taunt, interrupts, a global cooldown, per-spell cooldowns, threat, loot, XP,
// death/respawn. No line of sight, no vmaps, no pathfinding around geometry, no full spell fidelity.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "../wire.hpp"

namespace w3n {
namespace offline {

constexpr uint32_t kSimStepMs = 50;          // fixed simulation tick (20 Hz), independent of rendering
constexpr int kMaxEntities = 160;            // player + 4 bots + the creatures around the player (WoWVita: spawn grid)
constexpr int kMaxEvents = 512;              // up to 8 catch-up steps may be buffered before dispatch
constexpr int kMaxThreat = 12;
constexpr int kMaxAuras = 8;
constexpr int kMaxCooldowns = 8;
constexpr int kMaxLoot = 4;
constexpr int kMaxParty = 4;
constexpr int kMaxSpellsPerUnit = 12;

// ---------------------------------------------------------------- deterministic RNG
struct Rng {
    uint32_t s = 0x9E3779B9u;
    void seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }
    uint32_t next() {                     // xorshift32
        uint32_t x = s;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        return s = x;
    }
    // Uniform in [0, n) without modulo bias for the small ranges used here.
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
    int32_t range(int32_t lo, int32_t hi) { return hi <= lo ? lo : lo + (int32_t)below((uint32_t)(hi - lo + 1)); }
};

// ---------------------------------------------------------------- immutable world data
enum class Role : uint8_t { Tank = 0, Healer = 1, Dps = 2, Caster = 3 };

enum class SpellKind : uint8_t {
    Melee = 0,       // a weapon strike (Heroic Strike): melee range, physical school
    Damage = 1,      // direct spell damage
    Heal = 2,        // direct heal
    Dot = 3,         // damage over time (one aura per unit in this subset)
    Buff = 4,        // a stat/armour buff: tracked as an aura, no numeric effect beyond its spell entry
    Taunt = 5,       // pulls the target's attention to the caster
    Interrupt = 6,   // stops the target's cast
};

// Schools: 1 holy, 2 fire, 3 nature, 4 frost, 5 shadow, 6 arcane, 0 physical.
struct SpellDef {
    uint32_t id = 0;
    std::string name;
    SpellKind kind = SpellKind::Damage;
    uint8_t school = 0;
    uint32_t castMs = 0;              // 0 = instant
    uint32_t cooldownMs = 0;
    uint32_t category = 0, categoryCooldownMs = 0;
    float minRange = 0.0f, maxRange = 5.0f;   // yards; maxRange 0 = melee reach (5 yd)
    uint8_t powerType = 0;            // 0 mana, 1 rage, 3 energy
    uint32_t powerCost = 0;
    int32_t amountMin = 1, amountMax = 2;     // damage / heal per application
    uint32_t durationMs = 0;          // Dot / Buff
    uint32_t tickMs = 0;              // Dot
    int32_t threatBonus = 0;
    float radius = 0.0f;              // reserved: area spells are not implemented yet
    bool friendly = false;            // target must be friendly
    bool startsAutoAttack = false;    // melee abilities
};

struct CreatureTemplate {
    uint32_t entry = 0;
    std::string name, subname;
    uint32_t displayId = 0;
    uint16_t minLevel = 1, maxLevel = 1;
    uint32_t faction = 0;
    uint32_t hp = 100;
    uint32_t armor = 0;
    uint32_t xp = 50;
    uint16_t dmgMin = 4, dmgMax = 8;
    uint32_t attackMs = 2000;
    uint8_t powerType = 0;
    uint32_t power = 0;
    uint8_t rank = 0, creatureType = 7;
    uint32_t npcFlags = 0;
    float scale = 1.0f;
    float speed = 7.0f;
    float aggroRange = 12.0f;
    float combatReach = 1.5f;
    float boundingRadius = 0.5f;
    uint32_t lootId = 0;
    std::array<uint32_t, 4> abilities{};   // spell ids the creature may use
    uint8_t abilityCount = 0;
};

struct LootItem {
    uint32_t item = 0;
    uint16_t chance = 0;               // 0..10000 (10000 = always)
    uint16_t minCount = 1, maxCount = 1;
};

struct LootTable {
    uint32_t id = 0;
    uint32_t moneyMin = 0, moneyMax = 0;
    std::vector<LootItem> items;
};

struct ItemTemplate {
    uint32_t entry = 0;
    std::string name, description;
    uint32_t displayId = 0, quality = 1, inventoryType = 0, itemLevel = 1, requiredLevel = 0;
    uint32_t maxStack = 1, buyPrice = 0, sellPrice = 0, armor = 0, delayMs = 2000;
    float dmgMin = 0, dmgMax = 0;
    uint32_t cls = 2, subclass = 0;    // 2 = weapon
};

struct BotTemplate {
    std::string name;
    uint8_t race = 1, cls = 1, gender = 0;
    uint8_t skin = 1, face = 1, hairStyle = 0, hairColor = 0, facialHair = 0;
    Role role = Role::Dps;
    uint16_t level = 10;
    uint32_t hp = 200, power = 100;
    uint8_t powerType = 0;
    uint32_t displayId = 49;           // native player display id (human male 49, female 50, ...)
    std::array<uint32_t, kMaxSpellsPerUnit> spells{};
    uint8_t spellCount = 0;
    std::array<uint32_t, 19> equipment{};   // item entries per equipment slot
    float followDistance = 4.0f;
};

// A simple kill-or-collect objective. Turn-in is handled by the server layer.
struct QuestObjective {
    uint8_t kind = 0;                  // 0 = kill creature entry, 1 = collect item entry
    uint32_t target = 0;
    uint16_t count = 1;
    std::string text;
};

struct QuestDef {
    uint32_t id = 0;
    std::string title, details, completion;
    uint16_t level = 1;
    uint32_t giverCreature = 0;
    QuestObjective objective;
    uint32_t rewardXp = 0, rewardMoney = 0;
    std::array<uint32_t, 3> rewardItems{};
    std::array<uint16_t, 3> rewardCounts{};
};

struct Spawn {
    uint32_t entry = 0;
    float x = 0, y = 0, z = 0, o = 0;
    uint32_t respawnMs = 25000;
    uint8_t elite = 0;
};

struct HostilePair { uint32_t a = 0, b = 0; };

struct WorldData {
    uint16_t formatVersion = 1;
    std::string name;
    uint32_t mapId = 0, zoneId = 0;
    float spawnX = 0, spawnY = 0, spawnZ = 0, spawnO = 0;
    std::vector<CreatureTemplate> creatures;
    std::vector<Spawn> spawns;
    std::vector<LootTable> loot;
    std::vector<ItemTemplate> items;
    std::vector<SpellDef> spells;
    std::vector<QuestDef> quests;
    std::vector<HostilePair> hostility;
    std::array<BotTemplate, kMaxParty> bots{};

    const CreatureTemplate* creature(uint32_t entry) const;
    const LootTable* lootTable(uint32_t id) const;
    const ItemTemplate* item(uint32_t entry) const;
    const SpellDef* spell(uint32_t id) const;
    bool hostile(uint32_t a, uint32_t b) const;
    // WoWVita: a HOST pair whose first faction has bit 31 set means "may be attacked, but does not attack first"
    // (the yellow, neutral creatures of a whole-world pack). hostile() ignores those pairs.
    static constexpr uint32_t kNeutralFlag = 0x80000000u;
    bool attackable(uint32_t a, uint32_t b) const;
    mutable std::unordered_set<uint64_t> neutralIdx_;
    // WoWVita: hash indexes for a whole-continent pack (built on first use, rebuilt if a vector grew)
    mutable std::unordered_map<uint32_t, uint32_t> creatureIdx_, itemIdx_, lootIdx_;
    mutable std::unordered_set<uint64_t> hostileIdx_;
    mutable size_t hostileIdxFor_ = (size_t)-1;
    // Referential integrity report for the converter-side validator and the runtime loader.
    std::vector<std::string> validate() const;
};

// ---------------------------------------------------------------- runtime entities
struct Entity;

struct Aura {
    uint32_t spell = 0;
    uint64_t caster = 0;
    uint32_t endMs = 0, nextTickMs = 0;
    int32_t amount = 0;
    uint8_t kind = 0;                  // SpellKind
    uint8_t school = 0;
    uint8_t stacks = 1;
};

struct Cooldown { uint32_t spell = 0, endMs = 0, lengthMs = 0; };

struct LootSlotState { uint32_t item = 0; uint16_t count = 0; bool taken = false; };

struct Entity {
    bool used = false;
    uint64_t guid = 0;
    uint32_t entry = 0;
    uint8_t objType = 3;               // ObjType: 3 unit, 4 player
    bool self = false, bot = false, playerLike = false;
    int8_t botIndex = -1;

    float x = 0, y = 0, z = 0, o = 0;
    float homeX = 0, homeY = 0, homeZ = 0, homeO = 0;
    float speed = 7.0f, combatReach = 1.5f, boundingRadius = 0.5f, scale = 1.0f;

    int32_t hp = 1, power = 0;
    uint32_t maxHp = 1, maxPower = 0;
    uint8_t powerType = 0, level = 1, rank = 0, creatureType = 7;
    uint32_t faction = 0;
    uint64_t displayId = 0;
    uint32_t npcFlags = 0;
    bool dead = false;

    uint32_t dmgMin = 1, dmgMax = 2, attackMs = 2000, armor = 0;

    uint64_t victim = 0, target = 0;
    uint32_t nextSwingMs = 0, respawnAtMs = 0;
    uint32_t lastDamageMs = 0, lastThreatMs = 0;
    int32_t incomingDamage = 0;        // decayed "recent incoming damage" for features

    struct ThreatEntry { uint64_t who = 0; int32_t value = 0; } threat[kMaxThreat];
    uint8_t threatCount = 0;

    Aura auras[kMaxAuras];
    uint8_t auraCount = 0;

    Cooldown cooldowns[kMaxCooldowns];
    uint8_t cooldownCount = 0;

    uint32_t castSpell = 0, castEndMs = 0, castStartMs = 0;
    uint64_t castTarget = 0;
    uint32_t castInterruptible = 0;    // 0 = not interruptible (creature filler casts are)
    uint32_t gcdEndMs = 0;

    std::array<uint32_t, kMaxSpellsPerUnit> spells{};
    uint8_t spellCount = 0;

    LootSlotState loot[kMaxLoot];
    uint8_t lootCount = 0;
    uint32_t lootMoney = 0;
    bool looted = false;

    // Creature behaviour state.
    uint8_t aiState = 0;               // 0 idle, 1 chase, 2 combat, 3 evade, 4 dead
    uint32_t nextAbilityMs = 0;
    uint32_t stunnedUntilMs = 0;

    // Bot state (filled by offline_bots.cpp).
    uint8_t botRole = 0;
    uint8_t tacticalMode = 0;          // 0 follow, 1 assist, 2 stay, 3 retreat
    uint32_t nextDecisionMs = 0;
    uint8_t lastAction = 7;            // previous action category (feature 24)
    uint32_t moveTargetValidUntil = 0;
    float moveX = 0, moveY = 0;
    float moveStopAt = 0.4f;           // arrival radius of the current move order (chase vs formation)
    bool moving = false;
    uint32_t lastMoveSendMs = 0;
    uint8_t netMoveState = 0;          // 0 idle/unknown, 1 "moving" sent, 2 "stopped" sent
    float netO = 0;                    // the orientation the client was last told
    uint32_t recoveringUntilMs = 0;

    // Player-only profile pointer index (-1 = none).
    int8_t profileIndex = -1;

    uint32_t levelXp = 0, nextLevelXp = 0;
    uint32_t money = 0;

    bool alive() const { return !dead && hp > 0; }
    bool inCombat() const { return victim != 0; }
};

// ---------------------------------------------------------------- simulation events
struct SimEvent {
    enum Type : uint8_t {
        Damage = 0, Heal = 1, Death = 2, Respawn = 3, XpGain = 4, LevelUp = 5, LootReady = 6,
        CastStarted = 7, CastFinished = 8, CastFailed = 9, AttackStarted = 10, AttackStopped = 11,
        AuraApplied = 12, AuraRemoved = 13, AttackError = 14, Message = 15, Looted = 16,
        ThreatChanged = 17, PowerChanged = 18, ItemGained = 19, QuestProgress = 20, QuestComplete = 21,
        Appear = 22, Vanish = 23,      // WoWVita: a spawn came into / went out of the active area around the player
    };
    uint8_t type = 0;
    uint64_t a = 0, b = 0;             // actor / target (or victim / attacker)
    uint32_t spell = 0;
    int32_t value = 0;
    uint8_t school = 0, flags = 0;
    uint8_t text[48] = {};             // short message for Message events
};

// ---------------------------------------------------------------- character profile (persisted)
struct BagItem { uint32_t entry = 0; uint16_t count = 1; uint8_t slot = 0; };

struct CharacterProfile {
    bool used = false;
    bool isBot = false;
    uint8_t role = 2;                  // Role for bots
    uint64_t guid = 0;
    std::string name;
    uint8_t race = 1, cls = 1, gender = 0;
    uint8_t skin = 1, face = 1, hairStyle = 0, hairColor = 0, facialHair = 0;
    uint16_t level = 1;
    uint32_t xp = 0, nextXp = 400;
    int32_t hp = 100, power = 100;
    uint32_t maxHp = 100, maxPower = 100;
    uint8_t powerType = 0;
    uint32_t money = 0;
    float x = 0, y = 0, z = 0, o = 0;
    uint32_t mapId = 0, zoneId = 0;
    std::array<uint32_t, 19> equipment{};
    std::array<uint32_t, kMaxSpellsPerUnit> spells{};
    uint8_t spellCount = 0;
    BagItem bag[20];
    uint8_t bagCount = 0;
    uint8_t skinLook[3] = {0, 0, 0};    // spare look bytes (kept for forward compatibility)
};

struct QuestLogEntry {
    uint32_t id = 0;
    uint16_t kills[4] = {0, 0, 0, 0};
    uint16_t items[4] = {0, 0, 0, 0};
    bool complete = false;
};

struct PartyCommandState {
    uint8_t tank = 1;                  // 0 follow, 1 assist, 2 stay, 3 retreat
    uint8_t healer = 1;
    uint8_t dps1 = 1;
    uint8_t dps2 = 1;
    uint8_t formation = 0;
};

// ---------------------------------------------------------------- the world
class World {
public:
    void setData(const WorldData* d) { data_ = d; }
    const WorldData* data() const { return data_; }

    // Rebuild the whole runtime state from the immutable data. 'nowMs' is the session clock.
    void reset(uint32_t nowMs, uint32_t seed, const CharacterProfile& player,
               const std::array<CharacterProfile, kMaxParty>& bots, bool botsEnabled);
    void step(uint32_t nowMs);
    void setGroundQuery(bool (*fn)(float, float, float, float*)) { groundFn_ = fn; }

    Entity* entity(uint64_t guid);
    const Entity* entity(uint64_t guid) const;
    Entity* player() { return player_; }
    const Entity* player() const { return player_; }
    Entity* bot(int i) { return (i >= 0 && i < kMaxParty) ? bots_[i] : nullptr; }
    const Entity* bot(int i) const { return (i >= 0 && i < kMaxParty) ? bots_[i] : nullptr; }
    Entity* byIndex(int i) { return (i >= 0 && i < kMaxEntities) ? &pool_[i] : nullptr; }
    const Entity* byIndex(int i) const { return (i >= 0 && i < kMaxEntities) ? &pool_[i] : nullptr; }
    int entityCount() const { return kMaxEntities; }

    // ---- actions (also used by bots; the server layer uses the same entry points)
    bool startAttack(Entity& actor, uint64_t target, uint32_t nowMs);
    void stopAttack(Entity& actor, uint32_t nowMs);
    bool tryCast(Entity& actor, uint32_t spellId, uint64_t targetGuid, uint32_t nowMs);
    void moveEntity(Entity& e, float tx, float ty, uint32_t nowMs, float stopAt = 0.4f);
    void setPosition(Entity& e, float x, float y, float z, float o);
    void applyDamage(Entity& src, Entity& dst, int32_t amount, uint32_t spell, uint8_t school, uint32_t nowMs);
    void applyHeal(Entity& src, Entity& dst, int32_t amount, uint32_t spell, uint32_t nowMs);
    bool addThreat(Entity& target, uint64_t who, int32_t amount);
    int32_t threatOf(const Entity& target, uint64_t who) const;
    void taunt(Entity& target, uint64_t who);

    // ---- queries used by the server layer / bots
    bool inMeleeRange(const Entity& a, const Entity& b) const;
    float distance(const Entity& a, const Entity& b) const;
    bool hostileTowards(const Entity& a, const Entity& b) const;
    // WoWVita: hostile, or neutral to each other (may be attacked; a neutral creature only fights back)
    bool canAttack(const Entity& a, const Entity& b) const;
    uint64_t highestThreatTarget(const Entity& e) const;
    uint32_t cooldownLeft(const Entity& e, uint32_t spell, uint32_t nowMs) const;
    bool knowsSpell(const Entity& e, uint32_t spell) const;
    bool hasAura(const Entity& e, uint32_t spell) const;
    int alivePartyMembers() const;
    int injuredPartyMembers(int belowPercent) const;
    float partyHealthFraction() const;
    float lowestPartyHealthFraction(uint64_t* who = nullptr) const;
    int nearbyHostileCount(const Entity& around, float range) const;
    uint64_t lowestHealthEnemy(const Entity& around, float range) const;
    uint32_t nowMs() const { return now_; }

    // ---- event drain (the server layer calls this after step()/actions)
    const SimEvent* events() const { return events_; }
    int eventCount() const { return eventCount_; }
    void clearEvents() { eventCount_ = 0; }

    // ---- loot (called by the server on CMSG_LOOT / loot take)
    bool openLoot(Entity& corpse, uint32_t nowMs, std::string* why = nullptr);
    bool takeLoot(Entity& corpse, int slot, uint32_t nowMs);
    uint32_t takeLootMoney(Entity& corpse, uint32_t nowMs);
    void addItemToPlayer(uint32_t entry, uint16_t count, uint32_t nowMs);
    void addMoneyToPlayer(uint32_t copper, uint32_t nowMs);
    void addXp(Entity& e, uint32_t xp, uint32_t nowMs);
    // Player resurrection (corpse reclaim / spirit healer): clears combat and restores a share of HP.
    void revive(Entity& e, int32_t hpPercent, uint32_t nowMs);
    // Cancel an in-progress cast (player pressed cancel / an interrupt landed).
    void stopCast(Entity& e, uint8_t reason, uint32_t nowMs) { cancelCast(e, reason, nowMs); }

    // ---- party commands / quests
    void setPartyCommand(int botIndex, uint8_t mode, uint32_t nowMs);
    void setAllPartyCommands(uint8_t mode, uint32_t nowMs);
    void handlePartyChat(const std::string& text, uint32_t nowMs);
    // profiles_[0] = the player, profiles_[1..4] = the four companions.
    CharacterProfile& playerProfile() { return profiles_[0]; }
    const CharacterProfile& playerProfile() const { return profiles_[0]; }
    CharacterProfile& botProfile(int i) { return profiles_[1 + ((i < 0 || i >= kMaxParty) ? 0 : i)]; }
    const CharacterProfile& botProfile(int i) const { return profiles_[1 + ((i < 0 || i >= kMaxParty) ? 0 : i)]; }
    void syncProfileFromEntity(int index);   // 0 = player, 1..4 = bots
    // Quest state (accept / progress is tracked here; the packet flow lives in the server layer).
    bool acceptQuest(uint32_t questId, uint32_t nowMs);
    int questKillProgress(uint32_t questId) const;
    bool questComplete(uint32_t questId) const;
    bool turnInQuest(uint32_t questId, uint32_t nowMs);
    const std::vector<QuestLogEntry>& questLog() const { return questLog_; }

    Rng& rng() { return rng_; }
    uint32_t seed() const { return seed_; }

    // WoWVita: creatures exist only around the player (a whole continent does not fit the pool). Called by step();
    // 'initial' (from reset) spawns without Appear events - the login packets create those units.
    void updateActivation(uint32_t nowMs, bool initial);
    int activeCreatures() const;

private:
    void spawnCreature(const Spawn& s, uint32_t nowMs, int index);
    void buildSpawnCells();
    std::unordered_map<int64_t, std::vector<uint32_t>> spawnCells_;   // 64-yard cells -> spawn indexes
    std::vector<uint32_t> spawnRespawnAt_;   // per spawn: the time a creature that vanished dead may come back
    std::vector<uint8_t> spawnActive_;       // per spawn: has an entity in the pool now
    const WorldData* cellsFor_ = nullptr;
    size_t cellsSpawnCount_ = 0;
    uint32_t nextActivationMs_ = 0;
    Entity& allocate(uint64_t guid);
    void emit(uint8_t type, uint64_t a, uint64_t b, uint32_t spell, int32_t value, uint8_t school = 0, uint8_t flags = 0);
    void emitText(uint8_t type, uint64_t a, const char* text);
    void tickEntity(Entity& e, uint32_t nowMs, bool isPlayer);
    void tickCreature(Entity& e, uint32_t nowMs);
    void tickBot(Entity& e, uint32_t nowMs);
    void tickAuras(Entity& e, uint32_t nowMs);
    void tickRegen(Entity& e, uint32_t nowMs);
    void finishCast(Entity& e, uint32_t nowMs);
    void cancelCast(Entity& e, uint8_t reason, uint32_t nowMs);
    void resolveSwing(Entity& a, Entity& b, uint32_t nowMs);
    void onDeath(Entity& e, Entity& killer, uint32_t nowMs);
    void respawn(Entity& e, uint32_t nowMs);
    void rollLoot(Entity& e);
    void updateThreatDecay(uint32_t nowMs);
    uint64_t tickCount() const { return tick_; }
    void moveToward(Entity& e, float tx, float ty, uint32_t nowMs, float stopAt);
    void applyGround(Entity& e);
    void selectCreatureTarget(Entity& e, uint32_t nowMs);
    void addAura(Entity& e, const Aura& a);
    void removeAura(Entity& e, int index);

    const WorldData* data_ = nullptr;
    Entity pool_[kMaxEntities];
    Entity* player_ = nullptr;
    Entity* bots_[kMaxParty] = {nullptr, nullptr, nullptr, nullptr};
    CharacterProfile profiles_[kMaxParty + 1];
    std::vector<QuestLogEntry> questLog_;
    PartyCommandState commands_;
    SimEvent events_[kMaxEvents];
    int eventCount_ = 0;
    bool botsEnabled_ = true;
    Rng rng_;
    uint32_t seed_ = 1;
    uint32_t now_ = 0, lastStepMs_ = 0;
    uint64_t tick_ = 0;
    bool (*groundFn_)(float, float, float, float*) = nullptr;
};

// A tiny helper shared by the data loader, the server and the tests.
inline uint32_t percentOf(uint32_t max, int pct) { return max * (uint32_t)pct / 100u; }

}  // namespace offline
}  // namespace w3n
