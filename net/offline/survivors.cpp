// survivors.cpp — waves, automatic spells, level-up cards and the boss of the Survivors mode.
// See survivors.hpp for the design. All balance numbers live in this file or in Config.
#include "survivors.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace w3n {
namespace offline {
namespace survivors {

namespace {

constexpr uint32_t kPlayerFaction = 1;      // the offline rules' Alliance player
constexpr uint32_t kBossEntry = 990001;     // a template added by prepareData (not a real creature entry)
constexpr float kPlayerRunSpeed = 7.0f;
constexpr uint32_t kCorpseMs = 2500;        // a dead swarm creature stays this long
constexpr float kMissileSpeed = 26.0f;      // yards per second (Fireball / Frostbolt travel)
constexpr int kMaxHits = 96;

// Schools as the offline core numbers them: 1 holy, 2 fire, 3 nature, 4 frost, 5 shadow, 6 arcane.
struct WeaponDef {
    Upgrade id;
    uint32_t spell;       // the real 1.12 spell id: the client shows its visual
    uint8_t school;
    int32_t lo, hi;       // damage at level 1
    uint32_t cooldownMs;  // at level 1
    float range;          // yards (area radius for the novas)
};

const WeaponDef kWeapons[] = {
    {Upgrade::Fireball, 133, 2, 30, 40, 1100, 30.0f},
    {Upgrade::FrostNova, 122, 4, 16, 22, 6000, 9.0f},
    {Upgrade::ArcaneExplosion, 1449, 6, 14, 20, 2400, 7.0f},
    {Upgrade::Frostbolt, 116, 4, 32, 42, 2000, 28.0f},
    {Upgrade::FireBlast, 2136, 2, 50, 64, 3000, 20.0f},
};

const WeaponDef* weapon(Upgrade u) {
    for (const WeaponDef& w : kWeapons)
        if (w.id == u) return &w;
    return nullptr;
}

float dist2d(float ax, float ay, float bx, float by) {
    const float dx = ax - bx, dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

// ---------------------------------------------------------------- texts
const char* upgradeName(Upgrade u) {
    switch (u) {
        case Upgrade::Fireball: return "Fireball";
        case Upgrade::FrostNova: return "Frost Nova";
        case Upgrade::ArcaneExplosion: return "Arcane Explosion";
        case Upgrade::Frostbolt: return "Frostbolt";
        case Upgrade::FireBlast: return "Fire Blast";
        case Upgrade::Vitality: return "Vitality";
        case Upgrade::Swiftness: return "Swiftness";
        case Upgrade::SpellPower: return "Spell Power";
        case Upgrade::Haste: return "Haste";
        case Upgrade::Reach: return "Reach";
        case Upgrade::Regeneration: return "Regeneration";
        case Upgrade::Potion: return "Healing Potion";
        default: return "?";
    }
}

bool upgradeIsWeapon(Upgrade u) { return weapon(u) != nullptr; }

std::string upgradeText(Upgrade u, int lvl) {
    char b[96];
    switch (u) {
        case Upgrade::Fireball:
            if (lvl <= 1) return "Hurls a fireball at the nearest enemy.";
            snprintf(b, sizeof b, "%d fireballs, +20%% damage.", lvl);
            return b;
        case Upgrade::FrostNova:
            if (lvl <= 1) return "Freezes and damages enemies around you.";
            return "+25% damage, cast more often.";
        case Upgrade::ArcaneExplosion:
            if (lvl <= 1) return "Pulses arcane damage around you.";
            return "+25% damage, pulses faster.";
        case Upgrade::Frostbolt:
            if (lvl <= 1) return "Slows a random enemy with a frostbolt.";
            snprintf(b, sizeof b, "%d frostbolts, +15%% damage.", lvl);
            return b;
        case Upgrade::FireBlast:
            if (lvl <= 1) return "Blasts the strongest enemy nearby.";
            return "+30% damage, shorter cooldown.";
        case Upgrade::Vitality: return "+20% maximum health.";
        case Upgrade::Swiftness: return "+10% movement speed.";
        case Upgrade::SpellPower: return "+15% damage of all spells.";
        case Upgrade::Haste: return "Spells recharge 8% faster.";
        case Upgrade::Reach: return "+15% area of effect.";
        case Upgrade::Regeneration: return "Restore health over time.";
        case Upgrade::Potion: return "Restore 30% of your health.";
        default: return "";
    }
}

// ---------------------------------------------------------------- config
int Config::parse(const std::string& text) {
    int used = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const char* v = line.c_str() + eq + 1;
        const double d = std::atof(v);
        bool ok = true;
        if (k == "boss_at_sec") bossAtSec = (uint32_t)std::max(10.0, d);
        else if (k == "max_alive") maxAlive = std::clamp((int)d, 1, kMaxEntities - 6);
        else if (k == "start_alive") startAlive = std::max(1, (int)d);
        else if (k == "alive_per_minute") alivePerMinute = (float)d;
        else if (k == "hp_per_minute") hpPerMinute = (float)d;
        else if (k == "damage_per_minute") damagePerMinute = (float)d;
        else if (k == "spawn_radius") spawnRadius = std::max(8.0f, (float)d);
        else if (k == "swarm_speed") swarmSpeed = std::clamp((float)d, 0.2f, 2.0f);
        else if (k == "player_health") playerHealth = std::max(10, (int)d);
        else if (k == "player_armor") playerArmor = std::max(0, (int)d);
        else if (k == "seed") seed = (uint32_t)std::strtoul(v, nullptr, 10);
        else ok = false;
        if (ok) ++used;
    }
    return used;
}

// ---------------------------------------------------------------- the pack
bool Director::prepareData(WorldData& d, std::string* err) {
    pool_.clear();
    auto usable = [&](const CreatureTemplate& t) {
        return t.hp > 0 && !(t.npcFlags & 3u) && d.hostile(t.faction, kPlayerFaction) && t.entry != kBossEntry;
    };
    // The zone's own creatures first (the spawn list says what lives here), then any other hostile one.
    for (const Spawn& s : d.spawns) {
        const CreatureTemplate* t = d.creature(s.entry);
        if (t && usable(*t) && std::find(pool_.begin(), pool_.end(), t->entry) == pool_.end()) pool_.push_back(t->entry);
    }
    if (pool_.empty())
        for (const CreatureTemplate& t : d.creatures)
            if (usable(t)) pool_.push_back(t.entry);
    if (pool_.empty()) {
        if (err) *err = "the world pack has no hostile creature";
        return false;
    }
    std::sort(pool_.begin(), pool_.end(), [&](uint32_t a, uint32_t b) {
        const CreatureTemplate* ta = d.creature(a);
        const CreatureTemplate* tb = d.creature(b);
        if (ta->minLevel != tb->minLevel) return ta->minLevel < tb->minLevel;
        if (ta->hp != tb->hp) return ta->hp < tb->hp;
        return a < b;
    });
    if (pool_.size() > 8) pool_.resize(8);

    // The boss: the strongest creature of the pool, grown.
    const CreatureTemplate base = *d.creature(pool_.back());
    d.creatures.erase(std::remove_if(d.creatures.begin(), d.creatures.end(),
                                     [](const CreatureTemplate& t) { return t.entry == kBossEntry; }),
                      d.creatures.end());
    CreatureTemplate boss = base;
    boss.entry = kBossEntry;
    boss.name = base.creatureType == 1 ? "Alpha " + base.name : base.name + " Champion";
    boss.subname = "Survivors Boss";
    boss.rank = 1;                   // elite
    boss.scale = std::max(1.0f, base.scale) * 2.0f;
    boss.hp = std::max<uint32_t>(base.hp * 60u, 2000u);
    boss.dmgMin = (uint16_t)std::max<int>(base.dmgMin * 4, 8);
    boss.dmgMax = (uint16_t)std::max<int>(base.dmgMax * 4, 14);
    boss.lootId = 0;
    boss.xp = 0;
    boss.combatReach = 3.0f;
    d.creatures.push_back(boss);
    bossEntry_ = kBossEntry;
    bossName_ = boss.name;

    // Nothing static in the world: the director spawns everything. No loot windows, no world XP (the run
    // has its own levels), no quests.
    d.spawns.clear();
    d.quests.clear();
    for (CreatureTemplate& t : d.creatures) { t.lootId = 0; t.xp = 0; }
    return true;
}

// ---------------------------------------------------------------- run control
void Director::start(World& w, uint32_t nowMs, Host& host) {
    resetRun(w, nowMs, host);
}

void Director::resetRun(World& w, uint32_t nowMs, Host& host) {
    // Remove whatever is left of the previous run.
    for (int i = 0; i < kMaxEntities; ++i) {
        Entity* e = w.byIndex(i);
        if (e && e->used && e->objType == 3) w.despawn(*e);
    }
    stats_ = RunStats();
    std::memset(levels_, 0, sizeof levels_);
    std::memset(nextFire_, 0, sizeof nextFire_);
    levels_[(int)Upgrade::Fireball] = 1;
    cards_.clear();
    hits_.clear();
    pendingLevelUps_ = 0;
    restartPending_ = false;
    lastNowMs_ = nowMs;
    nextSpawnMs_ = nowMs + 1500;
    nextRegenMs_ = nowMs + 1000;
    xp_ = 0;
    xpNext_ = xpForLevel(1);
    alive_ = 0;
    bossGuid_ = 0;
    bossSpawned_ = false;
    bossHealth_ = 0;
    rng_.seed(cfg_.seed ? cfg_.seed : (nowMs * 2654435761u) ^ 0x5A17u);
    phase_ = Phase::Running;

    if (Entity* p = w.player()) {
        if (!p->alive()) w.revive(*p, 100, nowMs);
        p->maxHp = (uint32_t)cfg_.playerHealth;
        p->armor = (uint32_t)cfg_.playerArmor;   // physical hits from the swarm are cut by armor/(armor+400)
        p->hp = cfg_.playerHealth;
        p->level = 1;
        p->levelXp = 0;
        p->nextLevelXp = xpNext_;
        p->victim = p->target = 0;
        p->castSpell = 0;
    }
    host.setRunSpeed(kPlayerRunSpeed);
    host.selfChanged();
    host.notice("Survive! The waves grow stronger every minute.");
}

void Director::choose(int index) {
    if (phase_ != Phase::LevelUp || index < 0 || index >= (int)cards_.size()) return;
    chosen_ = cards_[(size_t)index];
    haveChoice_ = true;
}

bool Director::beforeStep(World& w, uint32_t nowMs, Host& host) {
    if (phase_ == Phase::Off) return true;
    if (restartPending_) resetRun(w, nowMs, host);
    if (haveChoice_) {
        haveChoice_ = false;
        apply(chosen_.id, w, host);
        if (--pendingLevelUps_ > 0) rollCards();
        else { cards_.clear(); phase_ = Phase::Running; }
        lastNowMs_ = nowMs;   // the pause does not count
    }
    if (phase_ != Phase::Running) {
        lastNowMs_ = nowMs;
        return false;
    }
    const uint32_t dt = nowMs - lastNowMs_;
    lastNowMs_ = nowMs;
    stats_.timeMs += std::min<uint32_t>(dt, 250);   // a loading hitch is not survived time

    Entity* p = w.player();
    if (!p) return true;
    if (!p->alive()) {
        phase_ = Phase::Lost;
        return false;
    }
    if (!bossSpawned_ && stats_.timeMs >= bossAtMs()) spawnBoss(w, nowMs, host);
    if (nowMs >= nextSpawnMs_) {
        spawnWave(w, nowMs);
        nextSpawnMs_ = nowMs + 500;
    }
    tickSwarm(w, nowMs);
    fireWeapons(w, nowMs, host);
    landHits(w, nowMs);
    // Regeneration: a share of the maximum health every second, no combat text.
    if (nowMs >= nextRegenMs_) {
        nextRegenMs_ = nowMs + 1000;
        const int lvl = levels_[(int)Upgrade::Regeneration];
        if (lvl && p->hp < (int32_t)p->maxHp) {
            const int32_t gain = std::max<int32_t>(1, (int32_t)p->maxHp * lvl / 200);
            p->hp = std::min<int32_t>(p->hp + gain, (int32_t)p->maxHp);
            host.selfChanged();
        }
    }
    return true;
}

void Director::afterStep(World& w, uint32_t nowMs, Host& host) {
    if (phase_ == Phase::Off) return;
    Entity* p = w.player();
    const uint64_t self = p ? p->guid : 0;
    bool levelled = false;
    const SimEvent* ev = w.events();
    for (int i = 0; i < w.eventCount(); ++i) {
        const SimEvent& e = ev[i];
        if (e.type == SimEvent::Damage && e.value > 0) {
            if (e.a == self) stats_.damageDone += (uint32_t)e.value;
            else if (e.b == self) stats_.damageTaken += (uint32_t)e.value;
        } else if (e.type == SimEvent::Death) {
            const Entity* dead = w.entity(e.a);
            if (!dead || !dead->swarm) continue;
            ++stats_.kills;
            if (dead->guid == bossGuid_) {
                bossGuid_ = 0;
                bossHealth_ = 0;
                phase_ = Phase::Won;
                host.notice("Victory! The champion has fallen.");
                continue;
            }
            const CreatureTemplate* t = w.data() ? w.data()->creature(dead->entry) : nullptr;
            uint32_t gain = 1;
            if (t) gain += t->minLevel / 3 + (uint32_t)(dead->maxHp > 150 ? 1 : 0);
            xp_ += gain;
            while (xp_ >= xpNext_) {
                xp_ -= xpNext_;
                ++stats_.level;
                xpNext_ = xpForLevel(stats_.level);
                ++pendingLevelUps_;
                levelled = true;
            }
        }
    }
    if (p && p->alive()) {
        p->level = std::min<uint8_t>(stats_.level, 60);
        p->levelXp = xp_;
        p->nextLevelXp = xpNext_;
    }
    if (p && !p->alive() && phase_ == Phase::Running) {
        phase_ = Phase::Lost;
        host.notice("You have fallen.");
    }
    if (levelled) host.selfChanged();
    if (levelled && phase_ == Phase::Running && pendingLevelUps_ > 0) {
        rollCards();
        phase_ = Phase::LevelUp;
    }
    if (bossGuid_) {
        const Entity* b = w.entity(bossGuid_);
        bossHealth_ = b && b->maxHp ? (float)std::max<int32_t>(0, b->hp) / (float)b->maxHp : 0.0f;
    }
    (void)nowMs;
}

// ---------------------------------------------------------------- the swarm
void Director::spawnWave(World& w, uint32_t nowMs) {
    const Entity* p = w.player();
    if (!p || pool_.empty()) return;
    const float m = minutes();
    int want = (int)(cfg_.startAlive + cfg_.alivePerMinute * m);
    if (bossSpawned_) want = want * 2 / 3;
    want = std::min(want, cfg_.maxAlive);
    int burst = 3;
    while (alive_ < want && burst-- > 0) {
        // Newer (stronger) creatures unlock over time; the newest is the most common.
        const int unlocked = std::min<int>((int)pool_.size(), 1 + (int)(m / 1.5f));
        int pick = unlocked - 1;
        const uint32_t roll = rng_.below(100);
        if (unlocked > 1 && roll < 45) pick = (int)rng_.below((uint32_t)unlocked - 1);
        const uint32_t entry = pool_[(size_t)pick];
        const float a = (float)rng_.below(6283) / 1000.0f;
        const float r = cfg_.spawnRadius + (float)rng_.below(60) / 10.0f;
        const float x = p->x + std::cos(a) * r, y = p->y + std::sin(a) * r;
        Entity* e = w.spawnAt(entry, x, y, p->z + 1.0f, a + 3.14159f, nowMs);
        if (!e) return;   // the pool is full
        const float hpMul = 1.0f + cfg_.hpPerMinute * m;
        const float dmgMul = 1.0f + cfg_.damagePerMinute * m;
        e->maxHp = (uint32_t)std::max(1.0f, (float)e->maxHp * hpMul);
        e->hp = (int32_t)e->maxHp;
        e->dmgMin = (uint32_t)std::max(1.0f, (float)e->dmgMin * dmgMul);
        e->dmgMax = (uint32_t)std::max((float)e->dmgMin, (float)e->dmgMax * dmgMul);
        e->speed = kPlayerRunSpeed * cfg_.swarmSpeed;
        e->nextAbilityMs = 0;   // no creature spells in the swarm (keeps the screen readable)
        ++alive_;
    }
}

void Director::spawnBoss(World& w, uint32_t nowMs, Host& host) {
    bossSpawned_ = true;
    const Entity* p = w.player();
    if (!p || !bossEntry_) return;
    const float a = (float)rng_.below(6283) / 1000.0f;
    const float r = cfg_.spawnRadius;
    Entity* e = w.spawnAt(bossEntry_, p->x + std::cos(a) * r, p->y + std::sin(a) * r, p->z + 1.0f, a + 3.14159f, nowMs);
    if (!e) {
        // A full pool: make room by removing the farthest creature.
        Entity* far = nullptr;
        float fd = 0;
        for (int i = 0; i < kMaxEntities; ++i) {
            Entity* c = w.byIndex(i);
            if (!c || !c->used || !c->swarm) continue;
            const float d = dist2d(c->x, c->y, p->x, p->y);
            if (d > fd) { fd = d; far = c; }
        }
        if (far) { w.despawn(*far); --alive_; }
        e = w.spawnAt(bossEntry_, p->x + std::cos(a) * r, p->y + std::sin(a) * r, p->z + 1.0f, a + 3.14159f, nowMs);
    }
    if (!e) return;
    const float m = minutes();
    e->maxHp = (uint32_t)((float)e->maxHp * (1.0f + cfg_.hpPerMinute * m * 0.5f));
    e->hp = (int32_t)e->maxHp;
    e->speed = kPlayerRunSpeed * std::min(1.0f, cfg_.swarmSpeed + 0.05f);
    bossGuid_ = e->guid;
    bossHealth_ = 1.0f;
    ++alive_;
    host.notice(bossName_ + " has arrived!");
}

void Director::tickSwarm(World& w, uint32_t nowMs) {
    const Entity* p = w.player();
    alive_ = 0;
    Entity* list[kMaxEntities];
    int n = 0;
    for (int i = 0; i < kMaxEntities; ++i) {
        Entity* e = w.byIndex(i);
        if (!e || !e->used || !e->swarm) continue;
        if (e->dead) {
            if (nowMs - e->lastDamageMs > kCorpseMs) w.despawn(*e);
            continue;
        }
        ++alive_;
        // Thaw: Frost Nova freezes (stunnedUntilMs: no steps, no swings), Frostbolt slows. The slow's end is
        // kept in recoveringUntilMs, a companion-only field that creatures never use.
        const float base = kPlayerRunSpeed * (e->guid == bossGuid_ ? std::min(1.0f, cfg_.swarmSpeed + 0.05f) : cfg_.swarmSpeed);
        if (e->stunnedUntilMs <= nowMs) {
            const float want = e->recoveringUntilMs > nowMs ? base * 0.5f : base;
            if (e->speed < want || (e->recoveringUntilMs <= nowMs && e->speed < base)) e->speed = want;
        }
        // Too far behind (the player ran away): bring it back to the ring in front of the player.
        if (p && dist2d(e->x, e->y, p->x, p->y) > cfg_.spawnRadius * 2.2f && e->guid != bossGuid_) {
            const float a = (float)rng_.below(6283) / 1000.0f;
            w.setPosition(*e, p->x + std::cos(a) * cfg_.spawnRadius, p->y + std::sin(a) * cfg_.spawnRadius, p->z + 1.0f, a);
            w.snapToGround(*e);
            e->netMoveState = 1;
        }
        list[n++] = e;
    }
    // Keep the crowd from stacking into one model: push overlapping creatures apart.
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            Entity* a = list[i];
            Entity* b = list[j];
            const float minD = (a->guid == bossGuid_ || b->guid == bossGuid_) ? 2.4f : 1.1f;
            const float dx = b->x - a->x, dy = b->y - a->y;
            const float d2 = dx * dx + dy * dy;
            if (d2 >= minD * minD) continue;
            const float d = std::sqrt(d2);
            const float push = (minD - d) * 0.5f;
            const float ux = d > 0.001f ? dx / d : 1.0f, uy = d > 0.001f ? dy / d : 0.0f;
            a->x -= ux * push; a->y -= uy * push;
            b->x += ux * push; b->y += uy * push;
        }
}

// ---------------------------------------------------------------- the spells
float Director::area() const { return 1.0f + 0.15f * (float)levels_[(int)Upgrade::Reach]; }

uint32_t Director::cooldown(uint32_t baseMs, Upgrade weaponId) const {
    float k = 1.0f - 0.08f * (float)levels_[(int)Upgrade::Haste];
    const int lvl = levels_[(int)weaponId];
    if (weaponId == Upgrade::FrostNova) k *= 1.0f - 0.10f * (float)(lvl - 1);
    if (weaponId == Upgrade::ArcaneExplosion) k *= 1.0f - 0.08f * (float)(lvl - 1);
    if (weaponId == Upgrade::FireBlast) k *= 1.0f - 0.08f * (float)(lvl - 1);
    return (uint32_t)std::max(250.0f, (float)baseMs * k);
}

int32_t Director::scaled(int32_t lo, int32_t hi, Upgrade weaponId) {
    const int lvl = levels_[(int)weaponId];
    float k = 1.0f + 0.15f * (float)levels_[(int)Upgrade::SpellPower];
    switch (weaponId) {
        case Upgrade::Fireball: k *= 1.0f + 0.20f * (float)(lvl - 1); break;
        case Upgrade::FrostNova: case Upgrade::ArcaneExplosion: k *= 1.0f + 0.25f * (float)(lvl - 1); break;
        case Upgrade::Frostbolt: k *= 1.0f + 0.15f * (float)(lvl - 1); break;
        case Upgrade::FireBlast: k *= 1.0f + 0.30f * (float)(lvl - 1); break;
        default: break;
    }
    int32_t v = rng_.range(lo, hi);
    v = (int32_t)((float)v * k);
    if (rng_.below(100) < 8) v *= 2;   // a spell crit
    return std::max<int32_t>(1, v);
}

uint64_t Director::nearestEnemy(const World& w, float range, const uint64_t* skip, int nSkip) const {
    const Entity* p = w.player();
    if (!p) return 0;
    uint64_t best = 0;
    float bestD = range;
    for (int i = 0; i < kMaxEntities; ++i) {
        const Entity* e = w.byIndex(i);
        if (!e || !e->used || !e->swarm || !e->alive()) continue;
        bool skipped = false;
        for (int k = 0; k < nSkip; ++k) skipped = skipped || skip[k] == e->guid;
        if (skipped) continue;
        const float d = dist2d(e->x, e->y, p->x, p->y);
        if (d < bestD) { bestD = d; best = e->guid; }
    }
    return best;
}

void Director::fireWeapons(World& w, uint32_t nowMs, Host& host) {
    Entity* p = w.player();
    if (!p || !p->alive()) return;
    auto queueHit = [&](const Entity& t, int32_t dmg, uint32_t spell, uint8_t school, uint8_t effect, bool missile) {
        if (hits_.size() >= (size_t)kMaxHits) return;
        Hit h;
        h.target = t.guid;
        h.damage = dmg;
        h.spell = spell;
        h.school = school;
        h.effect = effect;
        const float d = dist2d(t.x, t.y, p->x, p->y);
        h.atMs = nowMs + (missile ? (uint32_t)(d / kMissileSpeed * 1000.0f) : 0u);
        hits_.push_back(h);
    };
    for (const WeaponDef& wd : kWeapons) {
        const int lvl = levels_[(int)wd.id];
        if (!lvl || nowMs < nextFire_[(int)wd.id]) continue;
        switch (wd.id) {
            case Upgrade::Fireball:
            case Upgrade::Frostbolt: {
                uint64_t targets[kMaxUpgradeLevel] = {};
                int n = 0;
                for (int k = 0; k < lvl; ++k) {
                    uint64_t g = 0;
                    if (wd.id == Upgrade::Fireball) g = nearestEnemy(w, wd.range, targets, n);
                    else {
                        // Frostbolt: a random enemy in range.
                        uint64_t cand[kMaxEntities];
                        int nc = 0;
                        for (int i = 0; i < kMaxEntities; ++i) {
                            const Entity* e = w.byIndex(i);
                            if (!e || !e->used || !e->swarm || !e->alive()) continue;
                            if (dist2d(e->x, e->y, p->x, p->y) > wd.range) continue;
                            bool used = false;
                            for (int q = 0; q < n; ++q) used = used || targets[q] == e->guid;
                            if (!used) cand[nc++] = e->guid;
                        }
                        if (nc) g = cand[rng_.below((uint32_t)nc)];
                    }
                    if (!g) break;
                    targets[n++] = g;
                }
                if (!n) break;   // nothing in range: try again next frame
                for (int k = 0; k < n; ++k)
                    if (const Entity* t = w.entity(targets[k]))
                        queueHit(*t, scaled(wd.lo, wd.hi, wd.id), wd.spell, wd.school,
                                 wd.id == Upgrade::Frostbolt ? 1 : 0, true);
                for (int k = 0; k < n; k += 4) host.spellVisual(*p, wd.spell, targets + k, std::min(4, n - k));
                if (const Entity* t = w.entity(targets[0])) p->o = std::atan2(t->y - p->y, t->x - p->x);
                nextFire_[(int)wd.id] = nowMs + cooldown(wd.cooldownMs, wd.id);
                break;
            }
            case Upgrade::FrostNova:
            case Upgrade::ArcaneExplosion: {
                const float radius = wd.range * area();
                bool any = false;
                for (int i = 0; i < kMaxEntities; ++i) {
                    Entity* e = w.byIndex(i);
                    if (!e || !e->used || !e->swarm || !e->alive()) continue;
                    if (dist2d(e->x, e->y, p->x, p->y) > radius) continue;
                    any = true;
                    queueHit(*e, scaled(wd.lo, wd.hi, wd.id), wd.spell, wd.school,
                             wd.id == Upgrade::FrostNova ? 2 : 0, false);
                }
                if (!any) break;   // keep it ready for the first enemy that comes close
                host.spellVisual(*p, wd.spell, nullptr, 0);
                nextFire_[(int)wd.id] = nowMs + cooldown(wd.cooldownMs, wd.id);
                break;
            }
            case Upgrade::FireBlast: {
                // The healthiest enemy in range (it softens the big ones and the boss).
                const Entity* best = nullptr;
                for (int i = 0; i < kMaxEntities; ++i) {
                    const Entity* e = w.byIndex(i);
                    if (!e || !e->used || !e->swarm || !e->alive()) continue;
                    if (dist2d(e->x, e->y, p->x, p->y) > wd.range) continue;
                    if (!best || e->hp > best->hp) best = e;
                }
                if (!best) break;
                queueHit(*best, scaled(wd.lo, wd.hi, wd.id), wd.spell, wd.school, 0, false);
                host.spellVisual(*p, wd.spell, &best->guid, 1);
                nextFire_[(int)wd.id] = nowMs + cooldown(wd.cooldownMs, wd.id);
                break;
            }
            default:
                break;
        }
    }
}

void Director::landHits(World& w, uint32_t nowMs) {
    Entity* p = w.player();
    if (!p) { hits_.clear(); return; }
    size_t out = 0;
    for (size_t i = 0; i < hits_.size(); ++i) {
        const Hit h = hits_[i];
        if (h.atMs > nowMs) { hits_[out++] = h; continue; }
        Entity* t = w.entity(h.target);
        if (!t || !t->alive()) continue;
        if (h.effect == 1) {        // Frostbolt: slowed for 3 s (see tickSwarm)
            t->speed = std::min(t->speed, kPlayerRunSpeed * cfg_.swarmSpeed * 0.5f);
            t->recoveringUntilMs = std::max(t->recoveringUntilMs, nowMs + 3000);
        } else if (h.effect == 2) { // Frost Nova: frozen for 2 s (no steps, no swings)
            t->speed = 0.0f;
            t->stunnedUntilMs = std::max(t->stunnedUntilMs, nowMs + 2000);
        }
        w.applyDamage(*p, *t, h.damage, h.spell, h.school, nowMs);
    }
    hits_.resize(out);
}

// ---------------------------------------------------------------- level-ups
void Director::rollCards() {
    cards_.clear();
    std::vector<Upgrade> cand;
    for (int i = 0; i < kUpgradeCount; ++i)
        if (levels_[i] < kMaxUpgradeLevel) cand.push_back((Upgrade)i);
    // Few weapons early on: a new weapon is twice as likely while fewer than three are known.
    int weapons = 0;
    for (const WeaponDef& wd : kWeapons) weapons += levels_[(int)wd.id] ? 1 : 0;
    while (cards_.size() < 3 && !cand.empty()) {
        uint32_t total = 0;
        for (Upgrade u : cand) total += (upgradeIsWeapon(u) && !levels_[(int)u] && weapons < 3) ? 2 : 1;
        uint32_t r = rng_.below(total);
        size_t pick = 0;
        for (size_t k = 0; k < cand.size(); ++k) {
            const uint32_t wgt = (upgradeIsWeapon(cand[k]) && !levels_[(int)cand[k]] && weapons < 3) ? 2 : 1;
            if (r < wgt) { pick = k; break; }
            r -= wgt;
        }
        Card c;
        c.id = cand[pick];
        c.nextLevel = (uint8_t)(levels_[(int)c.id] + 1);
        cards_.push_back(c);
        cand.erase(cand.begin() + (long)pick);
    }
    if (cards_.empty()) {
        Card c;
        c.id = Upgrade::Potion;
        c.nextLevel = 1;
        cards_.push_back(c);
    }
}

void Director::apply(Upgrade u, World& w, Host& host) {
    Entity* p = w.player();
    if (u == Upgrade::Potion) {
        if (p) p->hp = std::min<int32_t>(p->hp + (int32_t)p->maxHp * 3 / 10, (int32_t)p->maxHp);
        host.selfChanged();
        return;
    }
    if ((int)u >= kUpgradeCount) return;
    uint8_t& lvl = levels_[(int)u];
    if (lvl >= kMaxUpgradeLevel) return;
    ++lvl;
    switch (u) {
        case Upgrade::Vitality:
            if (p) {
                const uint32_t add = p->maxHp / 5;
                p->maxHp += add;
                p->hp = std::min<int32_t>(p->hp + (int32_t)add, (int32_t)p->maxHp);
            }
            host.selfChanged();
            break;
        case Upgrade::Swiftness:
            host.setRunSpeed(kPlayerRunSpeed * (1.0f + 0.10f * (float)lvl));
            break;
        default:
            break;
    }
    if (upgradeIsWeapon(u) && lvl == 1) nextFire_[(int)u] = 0;   // a new spell fires at once
}

}  // namespace survivors
}  // namespace offline
}  // namespace w3n
