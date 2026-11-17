// survivors_test — PC test of the Survivors mode through the real client session.
//
// The OfflineSession runs with a survivors::Director attached and feeds the existing GameSession, so
// the creature create / destroy packets, spell visuals and damage logs are parsed by the code the Vita
// runs. The test plays scripted runs (the "player" walks in a circle and always takes the first card):
//   1. creatures appear around the player and the client knows them,
//   2. spells fire on their own, creatures die, corpses are removed (DESTROY_OBJECT),
//   3. levels come, the run pauses for cards, a card applies,
//   4. the boss arrives and the run ends (victory or defeat), "Try again" starts a clean run,
//   5. a timing benchmark of the simulation with a full swarm.
//
// Usage: survivors_test [pack.w3do]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../game.hpp"
#include "../movement.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_server.hpp"
#include "../offline/survivors.hpp"

using namespace w3n;
using namespace w3n::offline;

static int g_ok = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace {

struct Harness {
    OfflineSession session;
    survivors::Director director;
    WorldClient wc;
    GameSession gs;
    uint32_t now = 10000;
    uint32_t destroys = 0, spellGos = 0, damageLogs = 0, speedChanges = 0;

    bool start(const WorldData& data, const char* savePath, std::string& err) {
        session.setup(&data, savePath);
        session.setSurvivors(&director);
        if (!session.open(err)) return false;
        const uint64_t guid = session.characters()[0].guid;
        float pos[3] = {0, 0, 0};
        float o = 0;
        if (!session.enterWorld(guid, now, pos, &o)) { err = "enterWorld failed"; return false; }
        wc.packetSink = [this](uint16_t op, const Bytes& b) { session.onClientPacket(op, b, now); };
        session.setSink([this](uint16_t op, const Bytes& b) {
            if (op == 0x0AA) ++destroys;
            if (op == 0x132) ++spellGos;
            if (op == 0x250) ++damageLogs;
            if (op == 0x0E2) ++speedChanges;
            gs.handle(op, b, now);
        });
        wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
        gs.attach(wc, now);
        gs.spellNames.load("missing-spells.txt");
        return true;
    }
    void tick(uint32_t ms = 16) {
        now += ms;
        session.update(now);
        gs.update(now);
    }
    // The player kites: runs away from the creatures close to it (7 yd/s, like the client), and drifts
    // around the start when nothing is near. Positions go straight into the server's world.
    void walk(float t) {
        Entity* p = session.world().player();
        if (!p || !p->alive()) return;
        float ax = 0, ay = 0;
        for (int i = 0; i < kMaxEntities; ++i) {
            const Entity* e = session.world().byIndex(i);
            if (!e || !e->used || !e->swarm || !e->alive()) continue;
            const float dx = p->x - e->x, dy = p->y - e->y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > 12.0f * 12.0f || d2 < 0.01f) continue;
            ax += dx / d2;
            ay += dy / d2;
        }
        // Stay near the start (the map pack is only loaded there): a pull back towards it.
        const WorldData* d = session.world().data();
        ax += (d->spawnX - p->x) * 0.004f + std::cos(t * 0.3f) * 0.02f;
        ay += (d->spawnY - p->y) * 0.004f + std::sin(t * 0.3f) * 0.02f;
        const float len = std::sqrt(ax * ax + ay * ay);
        if (len < 1e-4f) return;
        const float step = 7.0f * 0.016f;
        session.world().setPosition(*p, p->x + ax / len * step, p->y + ay / len * step, p->z, std::atan2(ay, ax));
    }
    int clientCreatures() {
        int n = 0;
        for (auto& [g, o] : gs.objects.all())
            if (o.type == ObjType::Unit) ++n;
        return n;
    }
};

}  // namespace

int main(int argc, char** argv) {
    WorldData data;
    std::string err;
    if (argc > 1) {
        if (!loadWorldData(argv[1], data, err)) { std::printf("cannot load %s: %s\n", argv[1], err.c_str()); return 2; }
    } else {
        buildSampleWorldData(data);
    }

    // ---- 1. data preparation
    {
        survivors::Director d;
        WorldData copy = data;
        CHECK(d.prepareData(copy, &err), "prepareData: %s", err.c_str());
        CHECK(copy.spawns.empty(), "static spawns cleared");
        bool bossFound = false;
        for (const CreatureTemplate& t : copy.creatures) bossFound = bossFound || t.entry == 990001;
        CHECK(bossFound, "boss template added");
        std::printf("boss: %s\n", d.bossName().c_str());
        survivors::Config c;
        CHECK(c.parse("boss_at_sec=90\nmax_alive=30\n# comment\nbogus=1\n") == 2, "config parse");
        CHECK(c.bossAtSec == 90 && c.maxAlive == 30, "config values");
    }

    // ---- 2. a full run: spawns, kills, levels, cards, boss, end
    Harness h;
    survivors::Config cfg;
    cfg.bossAtSec = 120;
    cfg.seed = 1234;
    WorldData prepared = data;
    CHECK(h.director.prepareData(prepared, &err), "prepare: %s", err.c_str());
    h.director.setConfig(cfg);
    std::remove("survivors_test_save.w3d");
    std::remove("survivors_test_save.w3d.bak");
    CHECK(h.start(prepared, "survivors_test_save.w3d", err), "start: %s", err.c_str());
    CHECK(h.session.world().bot(0) == nullptr, "solo: no companions");
    CHECK(h.director.phase() == survivors::Phase::Running, "the run starts with the world");
    h.tick(16);
    CHECK(h.speedChanges >= 1, "run speed sent");

    uint32_t maxAlive = 0, maxClient = 0, levelUps = 0, cardsTaken = 0;
    bool sawBoss = false, ended = false;
    float t = 0;
    const uint32_t limitMs = 8 * 60 * 1000;
    uint32_t simMs = 0;
    while (simMs < limitMs) {
        if (h.director.phase() == survivors::Phase::LevelUp) {
            ++levelUps;
            CHECK(!h.director.cards().empty() && h.director.cards().size() <= 3, "1-3 cards");
            // Paused: the world must not move on while the cards are up.
            const uint32_t before = h.director.runMs();
            h.tick(16); h.tick(16);
            CHECK(h.director.runMs() == before, "paused while choosing");
            h.director.choose(0);
            ++cardsTaken;
            h.tick(16);
            continue;
        }
        if (h.director.phase() == survivors::Phase::Won || h.director.phase() == survivors::Phase::Lost) { ended = true; break; }
        h.walk(t);
        t += 0.016f;
        h.tick(16);
        simMs += 16;
        maxAlive = std::max<uint32_t>(maxAlive, (uint32_t)h.director.alive());
        maxClient = std::max<uint32_t>(maxClient, (uint32_t)h.clientCreatures());
        sawBoss = sawBoss || h.director.bossAlive();
    }
    const survivors::RunStats& st = h.director.stats();
    std::printf("run: %s at %u:%02u | level %u, %u kills, %u damage done, %u taken | max alive %u (client %u) | "
                "destroys %u, spell visuals %u, damage logs %u | cards %u\n",
                h.director.phase() == survivors::Phase::Won ? "WON" : h.director.phase() == survivors::Phase::Lost ? "LOST" : "running",
                st.timeMs / 60000, (st.timeMs / 1000) % 60, st.level, st.kills, st.damageDone, st.damageTaken,
                maxAlive, maxClient, h.destroys, h.spellGos, h.damageLogs, cardsTaken);
    std::printf("upgrades:");
    for (int i = 0; i < survivors::kUpgradeCount; ++i)
        if (h.director.upgradeLevel((survivors::Upgrade)i))
            std::printf(" %s %u", survivors::upgradeName((survivors::Upgrade)i), h.director.upgradeLevel((survivors::Upgrade)i));
    std::printf("\n");
    CHECK(maxAlive >= 8, "creatures spawned (%u)", maxAlive);
    CHECK(maxClient >= 8, "the client knows the creatures (%u)", maxClient);
    CHECK(st.kills >= 10, "kills (%u)", st.kills);
    CHECK(h.destroys >= 5, "corpses removed (%u)", h.destroys);
    CHECK(h.spellGos >= 20 && h.damageLogs >= 20, "spell visuals %u / damage logs %u", h.spellGos, h.damageLogs);
    CHECK(levelUps >= 2 && cardsTaken >= 2, "level-ups (%u)", levelUps);
    CHECK(ended, "the run ended");
    if (h.director.phase() == survivors::Phase::Won) CHECK(sawBoss, "won only after the boss");
    if (st.timeMs >= cfg.bossAtSec * 1000u) CHECK(sawBoss || h.director.phase() == survivors::Phase::Won, "the boss arrived");
    CHECK(h.session.world().player() && h.session.world().player()->level == st.level, "player level = run level");

    // ---- 3. try again: a clean run
    h.director.requestRestart();
    h.tick(16);
    CHECK(h.director.phase() == survivors::Phase::Running, "restart runs");
    CHECK(h.director.stats().kills == 0 && h.director.stats().level == 1, "restart resets the stats");
    CHECK(h.session.world().player()->alive(), "player alive after restart");
    int left = 0;
    for (int i = 0; i < kMaxEntities; ++i) {
        const Entity* e = h.session.world().byIndex(i);
        if (e && e->used && e->objType == 3) ++left;
    }
    CHECK(left == 0, "no creatures left after restart (%d)", left);
    for (int i = 0; i < 200; ++i) { h.walk(t); t += 0.016f; h.tick(16); }
    CHECK(h.director.alive() > 0, "new run spawns again");

    // ---- 4. benchmark: simulation cost with a full swarm (no rendering)
    {
        Harness b;
        survivors::Config bc;
        bc.maxAlive = kMaxEntities - 6;
        bc.startAlive = kMaxEntities - 6;
        bc.seed = 99;
        bc.playerHealth = 1000000;
        WorldData bd = data;
        b.director.prepareData(bd, &err);
        b.director.setConfig(bc);
        std::remove("survivors_bench_save.w3d");
        b.start(bd, "survivors_bench_save.w3d", err);
        float bt = 0;
        double total = 0, worst = 0;
        int frames = 0;
        for (int i = 0; i < 60 * 60; ++i) {   // one minute at 60 FPS
            if (b.director.phase() == survivors::Phase::LevelUp) b.director.choose(0);
            b.walk(bt);
            bt += 0.016f;
            const auto t0 = std::chrono::steady_clock::now();
            b.tick(16);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            total += ms;
            worst = std::max(worst, ms);
            ++frames;
        }
        std::printf("bench: %d alive, session+client update avg %.3f ms, worst %.3f ms per frame (PC)\n",
                    b.director.alive(), total / frames, worst);
        CHECK(b.director.alive() >= 60, "bench swarm size (%d)", b.director.alive());
    }

    std::printf("SURVIVORS: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
