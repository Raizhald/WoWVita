// survivors.hpp — the Survivors game mode on top of the offline world core.
//
// A run: the player only moves; spells fire on their own at the nearest creatures; waves of creatures
// from the zone's own data close in from every side and grow in number and strength; every level-up
// pauses the run and offers three upgrade cards; a boss arrives at the end. The run ends when the boss
// dies (victory) or when the player dies.
//
// The director drives the authoritative World through its public API only (spawnAt / despawn /
// applyDamage / revive), so the stock offline packet layer turns everything into the normal 1.12.1
// packets: creatures, damage numbers, spell visuals, the XP bar and the level-up effect all come from
// the existing client code. Nothing here knows about packets or rendering; the few things that are not
// world events (spell visuals without a cast, run speed, a changed max health) go through Host.
//
// Kept in its own files so the offline core can keep following upstream with few conflicts.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "offline_core.hpp"

namespace w3n {
namespace offline {
namespace survivors {

// What the director needs from the server layer.
class Host {
public:
    virtual ~Host() = default;
    // A spell's visual (SMSG_SPELL_GO) from the caster to up to four targets; none = on the caster.
    virtual void spellVisual(const Entity& caster, uint32_t spell, const uint64_t* targets, int count) = 0;
    virtual void setRunSpeed(float yardsPerSecond) = 0;
    virtual void selfChanged() = 0;                       // the player's health / max health / level fields
    virtual void notice(const std::string& text) = 0;     // a system line in the chat
};

// Tunables. Defaults are the prototype's; survivors.txt (key=value) can override them on the device.
struct Config {
    uint32_t bossAtSec = 600;        // when the boss arrives
    int maxAlive = 40;               // creatures alive at once (Vita budget; the pool allows ~90)
    int startAlive = 8;              // wanted alive at 0:00 ...
    float alivePerMinute = 4.0f;     // ... plus this many per minute
    float hpPerMinute = 0.30f;       // creature health: x(1 + this * minutes)
    float damagePerMinute = 0.12f;   // creature damage: x(1 + this * minutes)
    float spawnRadius = 26.0f;       // yards from the player
    float swarmSpeed = 0.80f;        // creature run speed relative to the player's base 7 yd/s
    int playerHealth = 200;
    int playerArmor = 300;           // -43% physical damage
    uint32_t seed = 0;               // 0 = from the clock
    // Applies "key=value" lines; unknown keys are ignored. Returns how many keys were used.
    int parse(const std::string& text);
};

enum class Phase : uint8_t { Off, Running, LevelUp, Won, Lost };

enum class Upgrade : uint8_t {
    Fireball, FrostNova, ArcaneExplosion, Frostbolt, FireBlast,     // weapons
    Vitality, Swiftness, SpellPower, Haste, Reach, Regeneration,    // passives
    Count,
    Potion = 250,                                                   // filler card when everything is maxed
};
constexpr int kUpgradeCount = (int)Upgrade::Count;
constexpr int kMaxUpgradeLevel = 5;

struct Card {
    Upgrade id = Upgrade::Fireball;
    uint8_t nextLevel = 1;           // 1 = new
};

const char* upgradeName(Upgrade u);
// One short line: what the next level gives.
std::string upgradeText(Upgrade u, int nextLevel);
bool upgradeIsWeapon(Upgrade u);

struct RunStats {
    uint32_t timeMs = 0;
    uint32_t kills = 0;
    uint32_t damageDone = 0;
    uint32_t damageTaken = 0;
    uint8_t level = 1;
};

class Director {
public:
    // Prepares a loaded pack for Survivors: picks the creature pool (hostile creatures of the zone), adds
    // the boss template, clears the static spawns, loot and quests, and sets the per-kill XP values.
    // Returns false when the pack has no usable hostile creature.
    bool prepareData(WorldData& d, std::string* err = nullptr);
    void setConfig(const Config& c) { cfg_ = c; }
    const Config& config() const { return cfg_; }

    // A new run (also used by "Try again").
    void start(World& w, uint32_t nowMs, Host& host);
    // Called by the server layer each frame. beforeStep returns false while the run is paused (the
    // world must not advance), afterStep reads the step's events (kills, damage, level-ups).
    bool beforeStep(World& w, uint32_t nowMs, Host& host);
    void afterStep(World& w, uint32_t nowMs, Host& host);
    void stop() { phase_ = Phase::Off; }

    // ---- the UI side
    Phase phase() const { return phase_; }
    bool active() const { return phase_ != Phase::Off; }
    bool paused() const { return phase_ == Phase::LevelUp || phase_ == Phase::Won || phase_ == Phase::Lost; }
    const std::vector<Card>& cards() const { return cards_; }
    void choose(int index);                 // a level-up card
    void requestRestart() { restartPending_ = true; }
    uint32_t runMs() const { return stats_.timeMs; }
    uint32_t bossAtMs() const { return cfg_.bossAtSec * 1000u; }
    bool bossAlive() const { return bossGuid_ != 0; }
    float bossHealth() const { return bossHealth_; }
    const std::string& bossName() const { return bossName_; }
    const RunStats& stats() const { return stats_; }
    uint8_t upgradeLevel(Upgrade u) const { return (int)u < kUpgradeCount ? levels_[(int)u] : 0; }
    int alive() const { return alive_; }
    int pendingLevelUps() const { return pendingLevelUps_; }
    uint32_t xp() const { return xp_; }
    uint32_t xpNext() const { return xpNext_; }

private:
    struct Hit {
        uint64_t target = 0;
        uint32_t atMs = 0;
        int32_t damage = 0;
        uint32_t spell = 0;
        uint8_t school = 0;
        uint8_t effect = 0;   // 0 none, 1 slow
    };
    void resetRun(World& w, uint32_t nowMs, Host& host);
    void spawnWave(World& w, uint32_t nowMs);
    void spawnBoss(World& w, uint32_t nowMs, Host& host);
    void fireWeapons(World& w, uint32_t nowMs, Host& host);
    void landHits(World& w, uint32_t nowMs);
    void tickSwarm(World& w, uint32_t nowMs);
    void rollCards();
    void apply(Upgrade u, World& w, Host& host);
    int32_t scaled(int32_t lo, int32_t hi, Upgrade weapon);
    uint32_t cooldown(uint32_t baseMs, Upgrade weapon) const;
    float area() const;
    uint64_t nearestEnemy(const World& w, float range, const uint64_t* skip, int nSkip) const;
    uint32_t xpForLevel(uint32_t level) const { return 3u + level * 2u + level * level / 4u; }
    float minutes() const { return (float)stats_.timeMs / 60000.0f; }

    Config cfg_;
    Phase phase_ = Phase::Off;
    RunStats stats_;
    Rng rng_;
    uint8_t levels_[kUpgradeCount] = {};
    uint32_t nextFire_[kUpgradeCount] = {};
    std::vector<Card> cards_;
    Card chosen_;
    bool haveChoice_ = false;        // applied at the next beforeStep (it needs the world and the host)
    int pendingLevelUps_ = 0;
    bool restartPending_ = false;
    uint32_t lastNowMs_ = 0;
    uint32_t nextSpawnMs_ = 0, nextRegenMs_ = 0;
    uint32_t xp_ = 0, xpNext_ = 0;
    int alive_ = 0;
    uint64_t bossGuid_ = 0;
    bool bossSpawned_ = false;
    float bossHealth_ = 0;
    std::string bossName_;
    std::vector<Hit> hits_;
    // Prepared from the pack.
    std::vector<uint32_t> pool_;     // creature entries, weakest first
    uint32_t bossEntry_ = 0;
};

}  // namespace survivors
}  // namespace offline
}  // namespace w3n
