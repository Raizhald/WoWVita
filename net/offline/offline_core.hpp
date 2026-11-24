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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "../wire.hpp"

namespace w3n {
namespace offline {

constexpr uint32_t kSimStepMs = 50;          // fixed simulation tick (20 Hz), independent of rendering
constexpr int kMaxEntities = 160;            // player + 4 bots + the creatures around the player (WoWVita: spawn grid)
constexpr int kMaxEvents = 512;              // up to 8 catch-up steps may be buffered before dispatch
constexpr int kMaxThreat = 12;
constexpr int kMaxAuras = 24;              // WoWVita: was 8 (a player keeps several buffs, a stance, debuffs)
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
    Script = 7,      // WoWVita: no effect in the world; the session acts when the cast finishes (the Hearthstone)
    Debuff = 8,      // WoWVita: an aura on an enemy (a creature spell): shown for its duration, no numeric effect yet
};
constexpr uint32_t kHearthstoneSpell = 8690, kHearthstoneItem = 6948;   // WoWVita
constexpr uint32_t kResurrectionSickness = 15007;                       // WoWVita: after the spirit healer (level 11+)

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

// WoWVita: a quest from the server database ("QST2" section): what the original quest_template holds that the
// client shows or the offline server needs. Objectives: up to 4 kills, 4 items.
struct QuestFull {
    uint32_t id = 0;
    uint16_t level = 1, minLevel = 0;
    uint32_t races = 0, classes = 0, zoneOrSort = 0, type = 0, flags = 0, special = 0;
    int32_t prev = 0;                  // PrevQuestId: > 0 must have been rewarded, < 0 must be in the log
    uint32_t nextInChain = 0, srcItem = 0;
    uint32_t rewSpell = 0, rewSpellCast = 0;   // WoWVita "QSPL": the spell shown / cast at the reward (CMaNGOS RewardQuest)
    uint8_t srcCount = 1;
    int32_t money = 0;                 // RewOrReqMoney (< 0: the quest costs that much)
    uint32_t moneyMaxLevel = 0;        // RewMoneyMaxLevel: the experience is derived from it (Quest::XPValue)
    std::string title, details, objectives, offerReward, requestItems, endText;
    std::string objText[4];
    uint32_t killEntry[4] = {0, 0, 0, 0};
    uint16_t killCount[4] = {0, 0, 0, 0};
    uint32_t itemEntry[4] = {0, 0, 0, 0};
    uint16_t itemCount[4] = {0, 0, 0, 0};
    uint32_t choiceItem[6] = {0, 0, 0, 0, 0, 0};
    uint16_t choiceCount[6] = {0, 0, 0, 0, 0, 0};
    uint32_t rewItem[4] = {0, 0, 0, 0};
    uint16_t rewCount[4] = {0, 0, 0, 0};
    std::vector<uint32_t> starters, enders;   // creature entries
    std::vector<uint32_t> goStarters, goEnders;   // game object entries (wanted posters, books ...)
};

// WoWVita: game objects ("GOBJ" templates, "GOSP" spawns, "GLOT" chest loot)
struct GoTemplate {
    uint32_t entry = 0, type = 0, displayId = 0, faction = 0, flags = 0;
    float size = 1.0f;
    std::string name;
    uint32_t data[24] = {};
};
struct GoSpawn { uint32_t entry = 0; float x = 0, y = 0, z = 0, o = 0; float rot[4] = {0, 0, 0, 1}; uint32_t respawnMs = 300000; };
struct GoLootItem { uint32_t item = 0; uint16_t chance = 0, minCount = 1, maxCount = 1; bool questOnly = false; };
struct GoLoot { uint32_t moneyMin = 0, moneyMax = 0; std::vector<GoLootItem> items; };
struct QuestDrop { uint32_t item = 0; uint16_t chance = 0; };   // chance in 1/100 %

// WoWVita: what a vendor sells ("VEND" section): the item and how many come in one purchase (arrows: 200)
struct VendorGoods { uint32_t item = 0; uint16_t buyCount = 1; };

// WoWVita: a graveyard ("GRAV": world_safe_locs) and a zone it serves (game_graveyard_zone; faction 0 any, 469 Alliance, 67 Horde)
// WoWVita: creature behaviour ("MOVE", "CRAI", "AITX")
struct MovePoint { float x = 0, y = 0, z = 0, o = 100.0f; uint32_t waitMs = 0; };   // o 100 = keep the heading
struct SpawnMove { uint8_t type = 0; float wander = 0; uint32_t path = 0xFFFFFFFFu; }; // 1 random, 2 waypoints
struct AiAction { uint8_t type = 0; int32_t p[3] = {0, 0, 0}; };
struct AiEvent {   // CMaNGOS EventAI (creature_ai_scripts): the subset the core runs
    uint8_t type = 0, chance = 100;
    uint16_t flags = 0;                // 0x01 repeatable, 0x20 one random action
    uint32_t phaseMask = 0;            // the phases it does NOT run in
    int32_t p[6] = {0, 0, 0, 0, 0, 0};
    AiAction act[3];
};
struct CreatureAi {
    float walkSpeed = 2.5f, callForHelp = 0;
    float leash = 0;                   // the most it follows from where the fight began (0: no limit)
    uint32_t pursuitMs = 15000;        // CMaNGOS Pursuit: no damage that long and the enemy 30 yd off -> evade
    bool civilian = false;
    std::vector<AiEvent> events;
};
// WoWVita "SPFX": what a spell's auras do to their target, for its duration
enum class Control : uint8_t { Stun = 1, Root = 2, Slow = 3, Fear = 4, Silence = 5, Confuse = 6, Pacify = 7, PacifySilence = 8 };
struct SpellControl { uint32_t durationMs = 0; std::vector<std::pair<uint8_t, int32_t>> effects; };   // (Control, value)
// WoWVita "SPX2": a spell as spell_template has it (the class spells; net/offline/offline_spells.cpp runs them)
struct SpellEffectX {
    uint8_t effect = 0, aura = 0, targetA = 0, targetB = 0;
    int32_t base = 0, die = 0, baseDice = 0;
    float perLevel = 0, dicePerLevel = 0;
    int32_t amplitude = 0;
    float radius = 0;
    int32_t misc = 0;
    uint32_t trigger = 0;
    uint8_t chain = 0;
    float multiple = 0;
    uint32_t itemType = 0;
    uint8_t mechanic = 0;
    float perCombo = 0, dmgMult = 1;
};
struct SpellX {
    uint32_t id = 0;
    std::string name, rank;
    uint8_t school = 0;
    uint32_t castMs = 0, cooldownMs = 0, catCooldownMs = 0, category = 0, gcdMs = 0, gcdCategory = 0;
    int32_t durBase = 0, durPerLevel = 0, durMax = 0;
    float rangeMin = 0, rangeMax = 0;
    uint8_t powerType = 0;
    uint32_t manaCost = 0, manaPerLevel = 0, manaPerSecond = 0;
    uint8_t manaPct = 0;
    uint16_t spellLevel = 0, baseLevel = 0, maxLevel = 0;
    uint32_t attr = 0, attrEx = 0, attrEx2 = 0, attrEx3 = 0, stances = 0, stancesNot = 0, targets = 0;
    uint32_t interruptFlags = 0, auraInterruptFlags = 0, channelInterruptFlags = 0, procFlags = 0, procCharges = 0;
    uint32_t mechanic = 0, casterAuraState = 0, targetAuraState = 0, targetCreatureType = 0, family = 0, focus = 0;
    uint64_t familyFlags = 0;
    uint8_t procChance = 0;
    uint16_t stackAmount = 0;
    uint8_t dmgClass = 0, prevention = 0;
    float speed = 0;
    int32_t equipClass = -1;
    uint32_t equipSubMask = 0;
    std::vector<std::pair<uint32_t, uint8_t>> reagents;
    uint32_t totem[2] = {0, 0};
    SpellEffectX eff[3];
    bool passive() const { return (attr & 0x40) != 0; }
    bool channeled() const { return (attrEx & 0x44) != 0; }
};
struct PlayerLevelStats { uint16_t hp = 0, mana = 0, str = 0, agi = 0, sta = 0, inte = 0, spi = 0; };   // WoWVita "PLST"
// WoWVita "EVSC": dbscripts_on_event SCRIPT_COMMAND_TEMP_SUMMON_CREATURE (10) rows of the events quest items send
struct EventSummon { uint32_t entry = 0, despawnMs = 0, delayMs = 0; float x = 0, y = 0, z = 0, o = 0; };
// WoWVita "IUSE": an item whose use casts a spell (spelltrigger 0): charges < 0 are used up with the item
struct ItemUse { uint32_t spell = 0; int32_t charges = 0; uint32_t cooldownMs = 0; };
// WoWVita "PETS": a summoned pet (CMaNGOS Pet::InitStatsForLevel for SUMMON_PET)
struct PetLevel { uint16_t hp = 0, mana = 0, armor = 0, str = 0, agi = 0, sta = 0, inte = 0, spi = 0; float dmgMin = 1, dmgMax = 2; };
struct PetTemplate {
    uint32_t entry = 0, displayId = 0;
    std::string name;
    uint16_t attackMs = 2000;
    uint8_t school = 0;
    float scale = 1, speed = 8;
    std::vector<uint32_t> spells;                   // petcreateinfo_spell
    std::unordered_map<uint8_t, PetLevel> levels;   // pet_levelstats
    std::vector<std::string> nameHalves[2];         // pet_name_generation
};
struct TrainerSpell { uint32_t teach = 0, learn = 0, cost = 0; uint8_t level = 0; uint16_t skill = 0, skillValue = 0; uint32_t reqSpell = 0; };
struct Trainer { uint8_t cls = 0; std::vector<TrainerSpell> spells; };

struct SpawnPool { uint16_t limit = 1; std::vector<uint32_t> members; };   // WoWVita "POOL": only 'limit' up at a time
struct SpawnLink { uint32_t master = 0; uint16_t flags = 0; };              // WoWVita "LINK": creature_linking
struct AiText { uint8_t chatType = 0; uint32_t emote = 0; std::string text; };   // 0 say, 1 yell, 2/3 emote

struct Graveyard { uint32_t id = 0; float x = 0, y = 0, z = 0, o = 0; };
struct GraveyardLink { uint32_t id = 0, zone = 0; uint16_t faction = 0; };

// WoWVita: "ITX1" - a bag's slots, the durability, and what using the item does (food / drink / potions)
struct ItemExt {
    uint8_t slots = 0;                // a bag
    uint16_t maxDurability = 0;
    uint8_t kind = 0;                 // bits: 1 food (health over time), 2 drink (mana over time), 4 heal, 8 mana
    uint32_t spell = 0;
    uint32_t hpMin = 0, hpMax = 0, manaMin = 0, manaMax = 0;   // food / drink: the total over durationMs
    uint32_t durationMs = 0, cooldownMs = 0;
    uint16_t category = 0;
    uint8_t flags = 0;                // 1 not in combat, 2 used up
};

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
    std::unordered_map<uint32_t, std::vector<VendorGoods>> vendors;   // WoWVita: creature entry -> its goods
    std::vector<QuestFull> questsFull;                                // WoWVita "QST2"
    std::unordered_map<uint32_t, uint32_t> questIndex;                // quest id -> questsFull index
    std::unordered_map<uint32_t, std::vector<uint32_t>> questsStarted, questsEnded;   // creature entry -> quest ids
    std::unordered_map<uint32_t, std::vector<QuestDrop>> questLoot;   // "QLOT": loot id -> quest drops
    std::unordered_map<uint32_t, std::string> greetings;              // "GOSS": creature entry -> greeting
    std::unordered_map<uint32_t, std::vector<uint32_t>> goQuestsStarted, goQuestsEnded;   // object entry -> quest ids
    std::unordered_map<uint32_t, GoTemplate> goTemplates;             // "GOBJ"
    std::vector<GoSpawn> goSpawns;                                    // "GOSP"
    std::unordered_map<uint32_t, GoLoot> goLoot;                      // "GLOT": loot id (data1 of a chest) -> loot
    std::vector<Graveyard> graveyards;                                // "GRAV"
    std::vector<GraveyardLink> graveyardLinks;
    std::vector<uint32_t> taverns;                                    // "TAVN": the inns' area trigger ids
    std::unordered_map<uint32_t, ItemExt> itemExt;                    // "ITX1"
    std::vector<uint32_t> explorationXp;                              // "EXPL": by area level
    std::unordered_multimap<uint32_t, uint32_t> questTriggers;        // "QTRG": area trigger -> exploration quest
    std::vector<std::vector<MovePoint>> paths;                        // "MOVE": waypoint paths
    std::unordered_map<uint32_t, SpawnMove> spawnMoves;               // spawn index -> how it moves
    std::unordered_map<uint32_t, CreatureAi> creatureAi;              // "CRAI": creature entry -> behaviour
    std::unordered_map<int32_t, AiText> aiTexts;                      // "AITX"
    std::unordered_map<uint32_t, SpellControl> spellControl;          // "SPFX"
    std::vector<SpawnPool> pools;                                     // "POOL"
    std::unordered_map<uint32_t, SpellX> spellx;                      // "SPX2"
    std::unordered_map<uint32_t, PlayerLevelStats> playerStats;       // "PLST": race | class << 8 | level << 16
    std::unordered_map<uint32_t, PetTemplate> pets;                   // "PETS": creature entry -> the pet
    std::unordered_map<uint32_t, std::vector<EventSummon>> eventSummons;   // "EVSC": event id -> what it summons
    std::unordered_map<uint32_t, ItemUse> itemUse;                    // "IUSE": item entry -> its use spell
    std::unordered_map<uint32_t, std::vector<std::pair<uint8_t, int32_t>>> itemStats;   // "ISTA": item -> (stat type, value)
    std::unordered_map<uint32_t, Trainer> trainers;                   // "TRNR": creature entry -> its class spells
    std::unordered_map<uint32_t, std::vector<uint32_t>> createSpells; // "CRSP": race | class << 8 -> spells
    const SpellX* sx(uint32_t id) const { auto it = spellx.find(id); return it == spellx.end() ? nullptr : &it->second; }
    std::unordered_map<uint32_t, std::vector<uint32_t>> outfits;      // "OUTF": race | class << 8 | sex << 16 -> items
    std::unordered_map<uint32_t, uint32_t> poolOf;                    // spawn index -> pools index
    std::unordered_map<uint32_t, SpawnLink> links;                    // "LINK": slave spawn -> its master
    std::unordered_multimap<uint32_t, uint32_t> slavesOf;             // master spawn -> slave spawns
    const SpawnLink* link(int spawn) const { auto it = links.find((uint32_t)spawn); return it == links.end() ? nullptr : &it->second; }
    const SpellControl* control(uint32_t spell) const { auto it = spellControl.find(spell); return it == spellControl.end() ? nullptr : &it->second; }
    const CreatureAi* ai(uint32_t entry) const { auto it = creatureAi.find(entry); return it == creatureAi.end() ? nullptr : &it->second; }
    const SpawnMove* spawnMove(int index) const { auto it = spawnMoves.find((uint32_t)index); return it == spawnMoves.end() ? nullptr : &it->second; }
    const ItemExt* ext(uint32_t item) const { auto it = itemExt.find(item); return it == itemExt.end() ? nullptr : &it->second; }
    const QuestFull* questFull(uint32_t id) const {
        auto it = questIndex.find(id);
        return it == questIndex.end() ? nullptr : &questsFull[it->second];
    }

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
    // WoWVita: a spell_template aura (offline_spells.cpp): its SPELL_AURA_* type, misc value, tick, the client slot
    uint8_t type = 0, effIndex = 0;
    int32_t misc = 0;
    uint32_t amplitudeMs = 0;
    uint8_t charges = 0;
    int8_t slot = -1;                  // UNIT_FIELD_AURA slot (0..31 buffs, 32..47 debuffs), kept while it lasts
    bool positive = false;
};

struct Cooldown { uint32_t spell = 0, endMs = 0, lengthMs = 0; };

struct LootSlotState { uint32_t item = 0; uint16_t count = 0; bool taken = false; };

struct Entity {
    bool used = false;
    uint64_t guid = 0;
    uint32_t entry = 0;
    uint8_t objType = 3;               // ObjType: 3 unit, 4 player
    bool self = false, bot = false, playerLike = false;
    bool untargetable = false;         // WoWVita: on a flight (creatures neither see nor attack it)
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

    // WoWVita: behaviour (offline_ai.cpp)
    int32_t spawnIndex = -1;
    float walkSpeed = 2.5f;
    bool legActive = false, legWalk = false;   // an idle move (wander / waypoint) the client got as one spline
    uint16_t moveLeg = 0, netLeg = 0;
    float legX = 0, legY = 0, legZ = 0, legFromZ = 0, legLen = 0;
    uint32_t nextIdleMoveMs = 0;
    uint16_t wpIndex = 0;
    bool aiCombat = false;                     // in combat as EventAI sees it (enter / leave)
    float combatX = 0, combatY = 0, combatZ = 0;   // where the fight began: evade goes back there
    uint32_t fleeUntilMs = 0;                  // fleeing for help
    uint64_t fleeTo = 0;
    uint32_t assistAtMs = 0;                   // called for help: joins assistTarget then (CreatureFamilyAssistanceDelay)
    uint64_t assistTarget = 0;
    uint8_t aiPhase = 0;
    bool combatMove = true;
    float rangedDist = 0;                      // EventAI ranged mode: stays this far and casts
    uint64_t lastHitBy = 0;
    uint32_t refreshMs = 0;                    // CMaNGOS combat timer: the last time it was hurt, and where
    float refreshX = 0, refreshY = 0;
    // WoWVita: control auras on it (until these times): stunnedUntilMs (above) also takes confuse for a player
    uint32_t rootUntilMs = 0, fearUntilMs = 0, silenceUntilMs = 0, pacifyUntilMs = 0, slowUntilMs = 0;
    int32_t slowPct = 0;                       // -50 = half speed
    uint32_t nextFearMoveMs = 0;
    bool netStun = false;                      // the client was told UNIT_FLAG_STUNNED
    bool stunned(uint32_t now) const { return stunnedUntilMs > now; }
    bool rooted(uint32_t now) const { return rootUntilMs > now; }
    bool feared(uint32_t now) const { return fearUntilMs > now; }
    bool silenced(uint32_t now) const { return silenceUntilMs > now; }
    bool pacified(uint32_t now) const { return pacifyUntilMs > now; }
    float speedFactor(uint32_t now) const {
        float f = slowUntilMs > now ? std::max(0.05f, 1.0f + (float)slowPct / 100.0f) : 1.0f;
        int32_t up = 0, down = 0;   // WoWVita: SPELL_AURA_MOD_INCREASE / DECREASE_SPEED, the strongest of each
        for (uint8_t i = 0; i < auraCount; ++i) {
            if (auras[i].type == 31 || auras[i].type == 32) up = std::max(up, auras[i].amount);
            if (auras[i].type == 33) down = std::min(down, auras[i].amount);
        }
        return std::max(0.05f, f * (1.0f + (float)up / 100.0f) * (1.0f + (float)std::max(-95, down) / 100.0f));
    }
    // WoWVita: the class spell engine (offline_spells.cpp)
    uint8_t form = 0;                          // shapeshift form / stance (SPELL_AURA_MOD_SHAPESHIFT misc)
    uint64_t ownerGuid = 0;                    // WoWVita: a pet's owner (offline_pet.cpp)
    uint64_t summonerGuid = 0;                 // WoWVita: who summoned it (a script summon: TARGET_T_SPAWNER)
    uint32_t autoRepeatSpell = 0, autoRepeatReadyMs = 0;   // WoWVita: Auto Shot / Shoot (CURRENT_AUTOREPEAT_SPELL)
    bool autoRepeatWindup = false;             // (m_AutoRepeatFirstCast: 0.5 s before the first shot / after moving)
    float autoRepeatX = 0, autoRepeatY = 0;    // (where it stood: a step pauses the shooting)
    uint32_t rangedMs = 0;                     // the ranged weapon: its speed, damage with the ammo's, ranged attack power
    float rangedMin = 0, rangedMax = 0;
    int32_t rangedAP = 0;
    uint32_t despawnAtMs = 0;                  // ... and when it goes (TEMPSUMMON_TIMED_OR_DEAD_DESPAWN)
    uint32_t petNumber = 0, createdBySpell = 0;
    uint8_t petCommand = 1, petReact = 1;      // CommandStates (0 stay, 1 follow) / ReactStates (0 passive, 1 defensive, 2 aggressive)
    uint8_t petAutocastOff = 0;                // its spells (bit = index) that it does not cast by itself
    bool petMelee = true;                      // (the Imp does not melee: PetAI m_meleeEnabled)
    std::string petName;
    uint32_t formDisplay = 0;                  // WoWVita: the form's model (0: one's own)
    bool formPower = false;                    // energy / rage of a form; the class's own power kept aside
    uint32_t baseMana = 0;                     // a player's create mana at its level (player_classlevelstats)
    uint8_t savedPowerType = 0;
    int32_t savedPower = 0;
    uint32_t savedMaxPower = 0;
    int32_t statStr = 0, statAgi = 0, statSta = 0, statInt = 0, statSpi = 0, attackPower = 0;   // the player's (PLST + gear)
    uint32_t lastManaSpendMs = 0;              // the five-second rule
    uint8_t comboPoints = 0;
    uint64_t comboTarget = 0;
    uint32_t nextSwingSpell = 0;               // Heroic Strike and the like: replaces the next swing
    bool channeling = false;                   // castSpell is a channel: its effects are on already
    bool castTriggered = false;
    float castDestX = 0, castDestY = 0, castDestZ = 0;
    bool castHasDest = false;
    uint8_t castPushes = 0;                    // spell pushback taken in this cast (two at most)

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
        Durability = 24,               // WoWVita: an equipped item lost durability (value = the equipment slot)
        CreatureText = 25,             // WoWVita: EventAI says / yells / emotes AITX text 'value' (b = its target)
        CreatureEmote = 26,            // WoWVita: an animation (value = the emote id)
        PetChanged = 27,               // WoWVita: our pet a (owner b) summoned by 'spell' (value 1) or gone (value 0)
    };
    uint8_t type = 0;
    uint64_t a = 0, b = 0;             // actor / target (or victim / attacker)
    uint32_t spell = 0;
    int32_t value = 0;
    uint8_t school = 0, flags = 0;
    uint8_t text[48] = {};             // short message for Message events
};

// ---------------------------------------------------------------- character profile (persisted)
struct BagItem { uint32_t entry = 0; uint16_t count = 1; uint8_t slot = 0; uint16_t lost = 0; };   // WoWVita: lost = durability points gone

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
    BagItem bag[160];                  // WoWVita: the backpack (16) and the bags' slots after it, in order (was 20)
    uint8_t bagCount = 0;
    uint8_t skinLook[3] = {0, 0, 0};    // spare look bytes (kept for forward compatibility)
    // WoWVita: the inn that is home (the save's optional "HOM1" block) and what is left of the Hearthstone's cooldown
    bool homeSet = false;
    float homeX = 0, homeY = 0, homeZ = 0;
    uint32_t homeZone = 0;
    uint32_t hearthCdSec = 0;
    // WoWVita: rested experience (the save's "RST1" block): the bonus pool and where / when the character left the world
    float restBonus = 0;
    uint32_t logoutTime = 0;          // unix time of the last save
    bool logoutResting = false;       // in an inn or a capital then
    std::array<uint32_t, 8> taxiMask{};   // WoWVita: the flight paths discovered (TaxiNodes ids as bits; the "TAX1" block)
    std::array<BagItem, 24> bank{};       // WoWVita: the bank's 24 slots (inventory slots 39..62; the "BNK1" block)
    std::array<uint32_t, 64> explored{};  // WoWVita: the AreaTable explore bits found (the "EXP1" block)
    std::array<uint16_t, 19> equipLost{}; // WoWVita: durability points gone of the worn items (the "DUR1" block)
    std::array<uint32_t, 4> bags{};       // WoWVita: the bags in the slots 19..22 (the "BAG2" block)
    std::vector<uint32_t> book;           // WoWVita: the spell book - every spell learned (the "SPB1" block)
    std::array<uint32_t, 120> actionBar{};// WoWVita: the action buttons as the client set them ("SPB1" too)
    bool actionBarSet = false;
    uint32_t ammoId = 0;                  // WoWVita: PLAYER_AMMO_ID, the arrows / bullets in the ammo slot ("AMO1")
    uint32_t petEntry = 0, petSpell = 0;  // WoWVita: the summoned pet ("PET1"), its name and the spells it does not autocast
    std::string petName;
    uint8_t petAutocastOff = 0;
    uint8_t bankBagSlots = 0;             // WoWVita: bank bag slots bought (0..6; the "BKB1" block)
    std::array<uint32_t, 6> bankBags{};   // the bags in the bank's bag slots 63..68
    std::array<BagItem, 6 * 36> bankBagItems{};   // what is in them: [bag * 36 + slot]
};

// WoWVita: a letter between the characters of this save (the "MAL1" block)
struct OfflineMail {
    uint32_t id = 0;
    uint64_t receiver = 0, sender = 0;
    std::string subject, body;
    uint32_t item = 0, itemCount = 0, money = 0;
    uint32_t flags = 0;               // 1 read, 2 returned
    uint32_t sentTime = 0;            // unix time
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
    // WoWVita: vendors. buyItem: false = not enough money (*why = 1) or no room in the backpack (*why = 2).
    bool buyItem(uint32_t entry, uint32_t count, uint32_t price, uint32_t nowMs, int* why);
    // Sells (count 0 = all) of backpack slot 'index'; the copper paid, 0 = nothing sold.
    uint32_t sellBagItem(int index, uint32_t count, uint32_t nowMs);
    void removeBagItem(int index);   // WoWVita: the whole stack leaves the backpack (the bank, a letter)
    bool takeFromBag(int index, uint16_t n);   // WoWVita: n of a stack are used up (food)
    bool putBagItem(const BagItem& b);         // WoWVita: one item as it is (with its durability) into the backpack
    uint32_t bagCapacity() const;              // WoWVita: 16 + the slots of the equipped bags
    void removeAuraSpell(Entity& e, uint32_t spell);   // WoWVita
    // WoWVita: durability (vMaNGOS Player::DurabilityLoss*): the worn items lose a share of their maximum (death 10 %, the
    // spirit healer 25 %) or a point (a hit); a broken item gives nothing
    void durabilityLossAll(uint32_t percent);
    void wearSlot(int slot, uint16_t points);
    uint16_t maxDurability(uint32_t entry) const { const ItemExt* x = data_ ? data_->ext(entry) : nullptr; return x ? x->maxDurability : 0; }
    bool broken(uint32_t entry, uint16_t lost) const { const uint16_t m = maxDurability(entry); return m && lost >= m; }
    void recomputeGearPublic() { recomputeGear(); }
    // Puts backpack slot 'index' on (what was worn there goes to the backpack). 0 = done, else a reason for the
    // player: 1 cannot be worn, 2 too low level, 3 no room for the off-hand item. *slotsOut: the equipment slots
    // that changed (up to 2).
    int equipFromBag(int index, int slotsOut[2]);
    // WoWVita quests: kill credit (the creature's entry and guid), quest drops while a quest wants them,
    // the bag for item objectives and turn-ins
    std::function<void(uint32_t entry, uint64_t guid)> onKillCredit;
    std::function<bool(uint32_t item)> questItemWanted;
    uint32_t countPlayerItem(uint32_t entry) const;
    uint32_t removePlayerItem(uint32_t entry, uint32_t count);   // how many were removed
    void syncPlayerMoney() { if (player_) profiles_[0].money = player_->money; }
    void recomputeGear();   // weapon damage / speed and armour from what is worn
    void enterForm(Entity& e, uint8_t form, uint32_t nowMs);   // CMaNGOS HandleAuraModShapeshift / ShapeshiftBoosts
    void leaveForm(Entity& e, uint8_t form);
    void addMoneyToPlayer(uint32_t copper, uint32_t nowMs);
    void addXp(Entity& e, uint32_t xp, uint32_t nowMs, uint64_t victim = 0, bool silent = false);   // WoWVita: a kill (victim) uses the rested bonus
    // Player resurrection (corpse reclaim / spirit healer): clears combat and restores a share of HP.
    void revive(Entity& e, int32_t hpPercent, uint32_t nowMs);
    // Cancel an in-progress cast (player pressed cancel / an interrupt landed).
    void stopCast(Entity& e, uint8_t reason, uint32_t nowMs) { cancelCast(e, reason, nowMs); }
    void addTimedAura(Entity& e, uint32_t spell, uint32_t durationMs);   // WoWVita: an aura without a SpellDef (resurrection sickness)

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
    // WoWVita: creature behaviour (offline_ai.cpp)
    void initBehaviour(Entity& e, int spawnIndex, uint32_t nowMs);
    void tickBehaviour(Entity& e, uint32_t nowMs);
    // WoWVita: the class spell engine (offline_spells.cpp)
    struct AreaFx { uint64_t caster = 0; uint32_t spell = 0; uint8_t eff = 0; float x = 0, y = 0, radius = 0;
                    uint32_t endMs = 0, nextMs = 0, periodMs = 1000; int32_t value = 0; bool used = false; };
    AreaFx areas_[16];
    void tickAreas(uint32_t nowMs);
    bool finishX(Entity& e, const SpellX& s, uint64_t target, uint32_t nowMs);
    void applyEffectX(Entity& caster, const SpellX& s, int k, Entity* t, uint32_t nowMs, int combo, int32_t* weaponBonus, int32_t* weaponPct, bool* weapon);
    void applyAuraX(Entity& caster, Entity& t, const SpellX& s, int k, uint32_t nowMs, int combo);
    void tickAuraX(Entity& e, Aura& a, uint32_t nowMs);
    void targetsX(Entity& caster, const SpellX& s, int k, Entity* t, std::vector<Entity*>& out);
    int32_t auraSum(const Entity& e, uint8_t type, int32_t miscMask = -1) const;
    bool hasAuraType(const Entity& e, uint8_t type, int32_t miscMask = -1) const;
    void procAuras(Entity& owner, Entity& other, uint32_t procFlag, uint32_t nowMs);
    bool scriptX(Entity& c, const SpellX& s, Entity* t, uint32_t nowMs);
public:
    void interruptAuras(Entity& e, uint32_t flags);
    // WoWVita: the summoned pet (offline_pet.cpp)
    Entity* pet();
    const Entity* pet() const;
    bool summonPet(Entity& owner, uint32_t entry, uint32_t spell, uint32_t nowMs, const std::string& name = std::string());
    void dismissPet(bool forget);              // forget: not back at the next login either (abandon, a new pet)
    void petCommand(uint8_t command, uint64_t target, uint32_t nowMs);   // CMSG_PET_ACTION ACT_COMMAND
    void setPetReact(uint8_t react) { if (Entity* e = pet()) e->petReact = react > 2 ? 1 : react; }
    void setPetAutocast(uint32_t spell, bool on);
    bool petCast(uint32_t spell, uint64_t target, uint32_t nowMs);
    void rewardSpell(Entity& p, uint32_t spell, uint32_t nowMs);
    Entity* summonCreature(uint32_t entry, float x, float y, float z, float o, uint32_t despawnMs, uint64_t summoner, uint32_t nowMs);
    bool nearSpellFocus(const Entity& e, uint32_t focus) const;   // a GAMEOBJECT_TYPE_SPELL_FOCUS of that focus id in reach
    void setSelection(uint64_t g) { selection_ = g; }            // the player's target (CMSG_SET_SELECTION)
    void stopAutoRepeat(Entity& e);                                // CMSG_CANCEL_AUTO_REPEAT_SPELL / SMSG_CANCEL_AUTO_REPEAT
    uint8_t ammoCheck(const Entity& e) const;                      // 0, or SPELL_FAILED_NO_AMMO / EQUIPPED_ITEM
    bool pickAmmo();                                               // the ammo slot: the first fitting stack in the bags   // a quest's reward spell (a learning one teaches)   // AURA_INTERRUPT_FLAG_*: what an action ends (stealth, polymorph)
private:
    int32_t absorb(Entity& dst, int32_t dmg, uint8_t school);
    void endChannel(Entity& e, uint32_t nowMs);
    int8_t freeAuraSlot(const Entity& e, bool positive) const;
public:
    bool castX(Entity& e, uint32_t spell, uint64_t target, uint32_t nowMs, bool triggered = false);
    int32_t effValue(const Entity& caster, const SpellX& s, int k, int combo = 0);
    uint32_t durationX(const Entity& caster, const SpellX& s, int combo = 0) const;
    bool detectsStealth(const Entity& viewer, const Entity& hidden) const;   // WoWVita: CMaNGOS stealth detection
    bool inCombatNow(const Entity& e) const;
    static bool hasInArc(const Entity& from, const Entity& to, float arc);
    bool positiveX(const SpellX& s) const;
    uint32_t powerCostX(const Entity& e, const SpellX& s) const;
    void gainRage(Entity& e, int32_t damage, bool dealt);
    bool spellEngine(const Entity& e, uint32_t spell) const { return (e.self || e.ownerGuid) && data_ && data_->sx(spell); }   // (our pet too)
    void cancelAuraX(Entity& e, uint32_t spell);
private:
    Entity* spawnEntity(uint32_t spawnIndex);
    void releaseCreature(Entity& e, uint32_t nowMs);
    void linkedAggro(Entity& e, uint32_t nowMs);
    void linkedEvade(Entity& e, uint32_t nowMs);
    bool followMaster(Entity& e, uint32_t nowMs);
    void choosePools();
    std::vector<uint8_t> poolChosen_;   // per spawn: a pooled spawn that is up (or may come up)
public:
    void applyControl(Entity& target, const SpellControl& c, uint32_t nowMs);   // WoWVita: stun, root, slow, ...
private:
    bool behaviourData() const { return behaviourOn_ && data_ && (!data_->creatureAi.empty() || !data_->spawnMoves.empty()); }
    void tickIdleMovement(Entity& e, uint32_t nowMs);
    bool stepLeg(Entity& e, float speed, uint32_t nowMs);
    void startLeg(Entity& e, float x, float y, float z, bool walk);
    void startEvade(Entity& e, uint32_t nowMs);
    bool tickEvade(Entity& e, uint32_t nowMs);
    bool tickFlee(Entity& e, uint32_t nowMs);
    void enterCombat(Entity& e, uint32_t nowMs);
    void callAssistance(Entity& e, float radius, uint32_t nowMs, uint32_t delayMs = 0);
    void aiEvent(Entity& e, uint8_t type, uint32_t nowMs, uint64_t other = 0);
    void aiTick(Entity& e, uint32_t nowMs);
    bool aiRun(Entity& e, int index, const AiEvent& ev, uint32_t nowMs, uint64_t other);
    bool aiAction(Entity& e, const AiAction& a, uint32_t nowMs, uint64_t other);
    void aiArm(Entity& e, int index, const AiEvent& ev, bool initial, uint32_t nowMs);
    bool aiCanRun(const Entity& e, const AiEvent& ev) const;
    struct AiTimers { uint32_t next[48] = {}; uint64_t done = 0; };
    AiTimers aiTimers_[kMaxEntities];
    AiTimers& timersOf(const Entity& e) { return aiTimers_[&e - pool_]; }
    bool behaviourOn_ = true;
public:
    void setBehaviour(bool on) { behaviourOn_ = on; }   // tests: the upstream checks run without it
private:

    const WorldData* data_ = nullptr;
    Entity pool_[kMaxEntities];
    Entity* player_ = nullptr;
    Entity* bots_[kMaxParty] = {nullptr, nullptr, nullptr, nullptr};
    uint64_t petGuid_ = 0;     // WoWVita: our pet
    uint32_t petCounter_ = 0;
    uint32_t summonCounter_ = 0;   // WoWVita: script summons' guids
    uint64_t selection_ = 0;       // WoWVita: the player's target
    void tickAutoRepeat(Entity& e, uint32_t nowMs);
    void takeAmmo(Entity& e);
    void tickPet(Entity& e, uint32_t nowMs);
    void applyPetLevel(Entity& e);
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
