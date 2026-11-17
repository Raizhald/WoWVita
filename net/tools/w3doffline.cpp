// w3doffline.cpp — PC tool for the offline world core: data check, a scripted fight (M3 demo on the
// PC) and the imitation-learning dataset generator used by tools/offline_train.
//
//   w3doffline --check [pack.w3do]
//   w3doffline --fight
//   w3doffline --gen-data out.csv [episodes] [pack.w3do]
//
// It links the same OfflineSession, World and Party Director the 3DS builds, so the recorded teacher
// decisions are exactly the ones the console's deterministic fallback makes.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

#include "../offline/offline_bots.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_nn.hpp"
#include "../offline/offline_server.hpp"

using namespace w3n;
using namespace w3n::offline;

namespace {

const char* kTempSave = "w3doffline_profile.w3d";

void sessionCleanup() {
    remove(kTempSave);
    remove("w3doffline_profile.w3d.bak");
    remove("w3doffline_profile.w3d.tmp");
}

struct Sim {
    OfflineSession session;
    std::string err;
    World* w = nullptr;
    uint32_t now = 1000;

    bool begin(const WorldData& data) {
        sessionCleanup();
        session.setup(&data, kTempSave);
        if (!session.open(err)) { std::printf("cannot open profile: %s\n", err.c_str()); return false; }
        if (session.characters().empty()) { std::printf("no characters\n"); return false; }
        float pos[3], o;
        if (!session.enterWorld(session.characters()[0].guid, now, pos, &o)) { std::printf("enterWorld failed\n"); return false; }
        session.setSink(nullptr);
        w = &session.world();
        return true;
    }
};

Entity* nearestHostile(World& w, const Entity& from, float range) {
    Entity* best = nullptr;
    float bestD = range;
    for (int i = 0; i < kMaxEntities; ++i) {
        Entity* e = w.byIndex(i);
        if (!e || !e->used || e->objType != 3 || !e->alive()) continue;
        if (!w.hostileTowards(from, *e)) continue;
        const float dx = e->x - from.x, dy = e->y - from.y;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < bestD) { bestD = d; best = e; }
    }
    return best;
}

// The scripted human for the data generator: walk to the nearest hostile and auto-attack it.
void playerThink(World& w, uint32_t now) {
    Entity* p = w.player();
    if (!p || !p->alive()) return;
    Entity* t = p->victim ? w.entity(p->victim) : nullptr;
    if (!t || !t->alive()) t = nearestHostile(w, *p, 30.0f);
    if (!t) { if (p->victim) w.stopAttack(*p, now); return; }
    const float dx = t->x - p->x, dy = t->y - p->y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d > 4.2f) {
        const float step = 1.4f;   // one 200 ms think at run speed
        w.setPosition(*p, p->x + dx / d * step, p->y + dy / d * step, t->z, std::atan2(dy, dx));
        return;
    }
    p->o = std::atan2(dy, dx);
    if (!p->victim) w.startAttack(*p, t->guid, now);
    if (p->powerType == 1 && p->power >= 15 && w.cooldownLeft(*p, 78, now) == 0) w.tryCast(*p, 78, t->guid, now);
}

void printParty(World& w) {
    auto line = [](const char* role, const Entity* e) {
        if (!e) return;
        std::printf("  %-7s %-8s hp %4d/%-4d power %3d/%-3d %s\n", role, "", e->hp, (int)e->maxHp, e->power,
                    (int)e->maxPower, e->alive() ? "" : "(dead)");
    };
    line("player", w.player());
    line("tank", w.bot(0));
    line("healer", w.bot(1));
    line("dps", w.bot(2));
    line("dps", w.bot(3));
}

int runFight(const WorldData& data) {
    Sim sim;
    if (!sim.begin(data)) return 1;
    World& w = *sim.w;
    Entity* wolf = nullptr;
    for (int i = 0; i < kMaxEntities; ++i) {
        Entity* e = w.byIndex(i);
        if (e && e->used && e->objType == 3 && e->entry == 299) { wolf = e; break; }
    }
    if (!wolf) { std::printf("no wolf in the pack\n"); return 1; }
    Entity* p = w.player();
    w.setPosition(*p, wolf->x - 2.0f, wolf->y, wolf->z, 0.0f);
    p->o = std::atan2(wolf->y - p->y, wolf->x - p->x);
    w.startAttack(*p, wolf->guid, sim.now);

    std::printf("M3 scripted fight: %s at %.1f yd\n", data.name.c_str(), 2.0f);
    uint32_t hits = 0, deaths = 0;
    for (int sec = 0; sec < 40; ++sec) {
        for (int k = 0; k < 5; ++k) {
            sim.now += 50;
            w.step(sim.now);
            for (int e = 0; e < w.eventCount(); ++e) {
                const SimEvent& ev = w.events()[e];
                if (ev.type == SimEvent::Damage && ev.value > 0) ++hits;
                if (ev.type == SimEvent::Death) ++deaths;
            }
            w.clearEvents();
        }
        if (wolf->dead && p->alive()) break;
    }
    std::printf("  result: wolf hp %d, player hp %d/%d, hits %u, deaths %u, xp %u, level %u\n", wolf->hp, p->hp,
                (int)p->maxHp, hits, deaths, p->levelXp, p->level);
    std::printf("  loot: %u items, %u copper; corpse lootable %s\n", wolf->lootCount, wolf->lootMoney,
                wolf->dead ? "yes" : "no");
    printParty(w);
    const bool ok = wolf->dead && p->alive();
    std::printf("M3 LOCAL COMBAT: %s\n", ok ? "PASS" : "FAIL");
    sessionCleanup();
    return ok ? 0 : 1;
}

int genData(const WorldData& data, const char* outPath, int episodes) {    FILE* f = fopen(outPath, "w");
    if (!f) { std::printf("cannot write %s\n", outPath); return 1; }
    std::fprintf(f, "# offline teacher dataset v1: features(24),action,mask,reward,role\n");
    setNeuralPolicy(nullptr);   // the teacher must never be the model it trains
    int rows = 0, wins = 0, wipes = 0;
    for (int ep = 0; ep < episodes; ++ep) {
        Sim sim;
        if (!sim.begin(data)) { fclose(f); return 1; }
        World& w = *sim.w;
        std::vector<std::string> episodeRows;
        // Randomize the start: a spawn cluster, a random level of hurt/mana, and assist mode.
        Entity* p = w.player();
        const Spawn& s = data.spawns[(size_t)w.rng().below((uint32_t)data.spawns.size())];
        w.setPosition(*p, s.x - 2.5f, s.y, s.z, 0.0f);
        // A fifth of the episodes start hurt, so the teacher demonstrates retreat and emergency heals.
        const bool hurtStart = (ep % 5) == 0;
        p->hp = std::max(15, (int)p->maxHp * (int)w.rng().range(hurtStart ? 18 : 45, hurtStart ? 40 : 100) / 100);
        for (int i = 0; i < kMaxParty; ++i) {
            Entity* b = w.bot(i);
            if (!b) continue;
            b->hp = std::max(12, (int)b->maxHp * (int)w.rng().range(hurtStart ? 15 : 40, hurtStart ? 45 : 100) / 100);
            b->power = std::max(0, (int)b->maxPower * (int)w.rng().range(25, 100) / 100);
            b->x = p->x + (float)(i + 1) * 1.5f;
            b->y = p->y - 2.0f;
            b->z = p->z;
            b->tacticalMode = 1;
            b->nextDecisionMs = sim.now + 100;
        }
        const int aliveStart = w.alivePartyMembers();
        uint32_t lastThink = 0;
        for (uint32_t t = 0; t < 30000; t += 50) {
            sim.now += 50;
            if (t - lastThink >= 200) {
                lastThink = t;
                playerThink(w, sim.now);
            }
            w.step(sim.now);
            for (int i = 0; i < kMaxParty; ++i) {
                if ((int)((sim.now / 50) % 4) != i) continue;   // stagger + subsample
                Entity* b = w.bot(i);
                if (!b || !b->alive()) continue;
                TacticalFeatures feat;
                buildFeatures(w, *b, feat, sim.now);
                const uint32_t mask = legalActionMask(w, *b, sim.now);
                const TacticalAction act = utilityChoose(w, *b, mask, sim.now);
                std::string line;
                line.reserve(200);
                for (int k = 0; k < kFeatureCount; ++k) {
                    line += std::to_string((int)feat.v[k]);
                    line += ',';
                }
                line += std::to_string((int)act);
                line += ',';
                line += std::to_string(mask);
                line += ",@R@,";                     // reward, filled in when the episode ends
                line += std::to_string((int)b->botRole);
                episodeRows.push_back(std::move(line));
            }
            w.clearEvents();
            if (t > 3000 && !nearestHostile(w, *p, 30.0f) && !p->victim) break;
            if (!p->alive()) break;
        }
        const bool alive = p->alive();
        const int aliveEnd = w.alivePartyMembers();
        const int reward = alive && aliveEnd >= aliveStart - 1 ? 1 : (alive ? 0 : -1);
        if (reward > 0) ++wins;
        if (reward < 0) ++wipes;
        for (std::string& line : episodeRows) {
            const size_t at = line.find("@R@");
            if (at != std::string::npos) line.replace(at, 3, std::to_string(reward));
        }
        for (const std::string& line : episodeRows) std::fprintf(f, "%s\n", line.c_str());
        rows += (int)episodeRows.size();
        sessionCleanup();
    }
    fclose(f);
    std::printf("teacher data: %d samples, %d episodes (%d party wins, %d wipes) -> %s\n", rows, episodes, wins, wipes, outPath);
    return 0;
}

struct BenchResult {
    int episodes = 0, wins = 0, losses = 0, playerDeaths = 0, allyDeaths = 0;
    uint64_t decisions = 0, switches = 0, damageTaken = 0, healingDone = 0, manaSpent = 0;
    int maxEngaged = 0;
    double seconds = 0.0;
};

BenchResult benchRun(const WorldData& data, NeuralPolicy* policy, int episodes) {
    BenchResult r;
    setNeuralPolicy(policy);
    const auto t0 = std::chrono::steady_clock::now();
    for (int ep = 0; ep < episodes; ++ep) {
        Sim sim;
        if (!sim.begin(data)) break;
        World& w = *sim.w;
        w.rng().seed(0x9E3779B9u * (uint32_t)(ep + 1));
        Entity* p = w.player();
        // A harder held-out encounter: the densest spawn cluster, the party arriving at 60% health.
        size_t bestSpawn = 0;
        int bestNeighbours = -1;
        for (size_t i = 0; i < data.spawns.size(); ++i) {
            int n = 0;
            for (const Spawn& o : data.spawns) {
                const float dx = o.x - data.spawns[i].x, dy = o.y - data.spawns[i].y;
                if (dx * dx + dy * dy < 22.0f * 22.0f) ++n;
            }
            if (n > bestNeighbours) { bestNeighbours = n; bestSpawn = i; }
        }
        const Spawn& s = data.spawns[bestSpawn];
        w.setPosition(*p, s.x - 2.5f, s.y, s.z, 0.0f);
        p->hp = std::max(20, (int)p->maxHp * 60 / 100);
        for (int i = 0; i < kMaxParty; ++i) {
            Entity* b = w.bot(i);
            if (!b) continue;
            b->hp = std::max(20, (int)b->maxHp * (55 + i * 5) / 100);
            b->power = (int)b->maxPower * 70 / 100;
            b->x = p->x + (float)(i + 1) * 1.5f;
            b->y = p->y - 2.0f;
            b->z = p->z;
            b->tacticalMode = 1;
        }
        uint8_t prevAction[kMaxParty] = {0xFF, 0xFF, 0xFF, 0xFF};
        const int partyHpStart = [&] { int h = 0; h += p->hp; for (int i = 0; i < kMaxParty; ++i) h += w.bot(i)->hp; return h; }();
        uint32_t manaStart = 0;
        for (int i = 0; i < kMaxParty; ++i) manaStart += (uint32_t)std::max(0, w.bot(i)->power);
        for (uint32_t t = 0; t < 20000; t += 50) {
            sim.now += 50;
            if (t % 200 == 0) playerThink(w, sim.now);
            w.step(sim.now);
            for (int i = 0; i < kMaxParty; ++i) {
                const Entity* b = w.bot(i);
                if (!b || !b->alive()) continue;
                if (prevAction[i] != 0xFF && prevAction[i] != b->lastAction) ++r.switches;
                prevAction[i] = b->lastAction;
                ++r.decisions;
            }
            int engaged = 0;
            for (int i = 0; i < kMaxEntities; ++i) {
                const Entity* e = w.byIndex(i);
                if (e && e->used && e->objType == 3 && e->alive() && e->victim) ++engaged;
            }
            r.maxEngaged = std::max(r.maxEngaged, engaged);
            for (int i = 0; i < w.eventCount(); ++i) {
                const SimEvent& ev = w.events()[i];
                if (ev.type == SimEvent::Heal) r.healingDone += (uint64_t)std::max(0, ev.value);
                if (ev.type == SimEvent::Damage) {
                    const Entity* v = w.entity(ev.b);
                    if (v && (v->self || v->bot)) r.damageTaken += (uint64_t)std::max(0, ev.value);
                }
            }
            w.clearEvents();
        }
        const bool alive = p->alive();
        int aliveAllies = 0;
        for (int i = 0; i < kMaxParty; ++i) aliveAllies += w.bot(i)->alive() ? 1 : 0;
        int partyHpEnd = p->hp;
        uint32_t manaEnd = 0;
        for (int i = 0; i < kMaxParty; ++i) { partyHpEnd += w.bot(i)->hp; manaEnd += (uint32_t)std::max(0, w.bot(i)->power); }
        int kills = 0;
        for (int i = 0; i < kMaxEntities; ++i) {
            const Entity* e = w.byIndex(i);
            if (e && e->used && e->objType == 3 && e->dead) ++kills;
        }
        if (alive && aliveAllies >= 3 && kills > 0) ++r.wins;
        else ++r.losses;
        if (!alive) ++r.playerDeaths;
        r.allyDeaths += kMaxParty - aliveAllies;
        r.manaSpent += manaStart > manaEnd ? manaStart - manaEnd : 0;
        (void)partyHpStart;
        (void)partyHpEnd;
        ++r.episodes;
        sessionCleanup();
    }
    const auto t1 = std::chrono::steady_clock::now();
    r.seconds = std::chrono::duration<double>(t1 - t0).count();
    setNeuralPolicy(nullptr);
    return r;
}

void printBench(const char* name, const BenchResult& r) {
    std::printf("%-14s %3d ep | %3d win %3d loss | player deaths %2d ally deaths %2d | dmg %6llu heal %6llu | "
                "max engaged %2d | decisions %6llu switches %5llu | %.1f ms/ep %.2f us/decision\n",
                name, r.episodes, r.wins, r.losses, r.playerDeaths, r.allyDeaths,
                (unsigned long long)r.damageTaken, (unsigned long long)r.healingDone, r.maxEngaged,
                (unsigned long long)r.decisions, (unsigned long long)r.switches,
                r.episodes ? r.seconds * 1000.0 / r.episodes : 0.0,
                r.decisions ? r.seconds * 1e6 / (double)r.decisions : 0.0);
}

int benchmark(const WorldData& data, const char* modelPath, int episodes) {
    std::printf("Phase 7 benchmark: %d held-out scenarios per controller\n", episodes);
    const BenchResult det = benchRun(data, nullptr, episodes);
    printBench("utility AI", det);
    if (modelPath) {
        NeuralPolicy policy;
        if (!policy.load(modelPath)) {
            std::printf("cannot load model %s\n", modelPath);
            return 1;
        }
        const BenchResult nn = benchRun(data, &policy, episodes);
        printBench("MLP 24-16-8", nn);
        std::printf("note: the legal-action mask and emergency rules apply to both controllers; "
                    "a model never bypasses them.\n");
        if (nn.wins < det.wins) std::printf("RESULT: the model does not beat the teacher on this set\n");
        else if (nn.wins > det.wins) std::printf("RESULT: the model wins more encounters than the teacher\n");
        else std::printf("RESULT: equal encounter completion\n");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    WorldData data;
    const char* pack = nullptr;
    if (argc >= 3 && (std::strstr(argv[2], ".w3do") || std::strstr(argv[2], ".W3DO"))) pack = argv[2];
    if (argc >= 4 && (std::strstr(argv[3], ".w3do") || std::strstr(argv[3], ".W3DO"))) pack = argv[3];
    std::string err;
    if (pack) {
        if (!loadWorldData(pack, data, err)) { std::printf("cannot load %s: %s\n", pack, err.c_str()); return 1; }
    } else if (!buildSampleWorldData(data)) {
        std::printf("the built-in sample failed validation\n");
        return 1;
    }
    const char* mode = argc >= 2 ? argv[1] : "--check";
    if (!std::strcmp(mode, "--check")) {
        std::printf("pack: %s (%u creatures, %u spawns, %u items, %u spells, %u quests)\n", data.name.c_str(),
                    (unsigned)data.creatures.size(), (unsigned)data.spawns.size(), (unsigned)data.items.size(),
                    (unsigned)data.spells.size(), (unsigned)data.quests.size());
        const std::vector<std::string> problems = data.validate();
        for (const std::string& p : problems) std::printf("  problem: %s\n", p.c_str());
        std::printf("OFFLINE DATA: %s\n", problems.empty() ? "PASS" : "FAIL");
        return problems.empty() ? 0 : 1;
    }
    if (!std::strcmp(mode, "--fight")) return runFight(data);
    if (!std::strcmp(mode, "--bench")) {
        const char* model = argc >= 3 ? argv[2] : nullptr;
        const int episodes = argc >= 4 ? std::atoi(argv[3]) : 20;
        return benchmark(data, model, episodes);
    }
    if (!std::strcmp(mode, "--gen-data") && argc >= 3) {
        const int episodes = argc >= 4 ? std::atoi(argv[3]) : 40;
        return genData(data, argv[2], episodes);
    }
    std::printf("usage: w3doffline --check [pack.w3do] | --fight [pack.w3do] | --gen-data out.csv [episodes] [pack.w3do]\n");
    return 2;
}
