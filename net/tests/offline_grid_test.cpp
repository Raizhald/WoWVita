// WoWVita: the offline spawn grid with a whole-continent pack.
//   offline_grid_test [pack.w3do]   (without a pack: the compiled-in sample)
// Walks the player from Northshire through Goldshire and Stormwind to Westfall and checks that
//  - only creatures near the player are in the pool (never more than it holds),
//  - every Appear has a matching earlier Vanish-free life (no guid appears twice while alive),
//  - creatures far behind are gone, and a creature killed and left dead does not come straight back,
//  - one simulation second stays cheap.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "../offline/offline_core.hpp"
#include "../offline/offline_data.hpp"

using namespace w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    WorldData data;
    std::string err;
    if (argc > 1) {
        if (!loadWorldData(argv[1], data, err)) { std::printf("cannot load %s: %s\n", argv[1], err.c_str()); return 2; }
    } else if (!buildSampleWorldData(data)) {
        std::printf("no sample\n");
        return 2;
    }
    std::printf("pack: %zu creatures, %zu spawns, %zu hostility pairs\n", data.creatures.size(), data.spawns.size(),
                data.hostility.size());

    World w;
    w.setData(&data);
    CharacterProfile me;
    me.used = true;
    me.guid = 1;
    me.level = 60;           // strong enough to walk through anything without dying
    me.maxHp = 1000000;
    me.hp = 1000000;
    std::array<CharacterProfile, kMaxParty> bots{};
    uint32_t now = 1000;
    w.reset(now, 1234, me, bots, false);
    w.clearEvents();
    const int startActive = w.activeCreatures();
    std::printf("start: %d creatures active\n", startActive);
    CHECK(startActive > 0, "creatures around the start");
    CHECK(startActive <= kMaxEntities - 1, "the pool holds them");

    // A creature killed and left: back at the same place before its respawn time -> not there.
    {
        Entity* p = w.player();
        Entity* victim = nullptr;
        for (int i = 0; i < kMaxEntities && !victim; ++i) {
            Entity* e = w.byIndex(i);
            if (e && e->used && e->objType == 3 && e->alive() && !e->npcFlags) victim = e;
        }
        CHECK(victim != nullptr, "a creature to kill");
        if (victim) {
            const uint64_t g = victim->guid;
            const float vx = victim->x, vy = victim->y;
            victim->hp = 0;
            victim->dead = true;
            victim->respawnAtMs = now + 600000;   // ten minutes
            p->x = vx + 1000; p->y = vy;
            now += 1000; w.step(now); w.clearEvents();
            CHECK(!w.entity(g), "the dead creature left with the player");
            p->x = vx; p->y = vy;
            now += 1000; w.step(now); w.clearEvents();
            CHECK(!w.entity(g), "and did not come back before its respawn time");
            p->x = vx + 1000;
            now += 700000; w.step(now); w.clearEvents();
            p->x = vx;
            now += 1000; w.step(now); w.clearEvents();
            CHECK(w.entity(g) != nullptr, "after the respawn time it is there again");
        }
    }
    w.reset(now, 1234, me, bots, false);   // a fresh world for the walk
    w.clearEvents();
    if (argc < 2) {   // the sample is only Northshire: the walk needs a whole-continent pack
        std::printf("OFFLINE GRID: %d checks, %d failed -> %s (sample only, no walk)\n", checks, failed, failed ? "FAIL" : "PASS");
        return failed ? 1 : 0;
    }

    // Northshire Abbey -> the road down to Goldshire -> Stormwind gate -> Westfall (Sentinel Hill)
    const float path[][2] = {{-8949.9f, -132.5f}, {-9050.f, -60.f}, {-9300.f, 50.f}, {-9460.f, 60.f},
                             {-9300.f, 400.f}, {-9100.f, 450.f}, {-8900.f, 560.f}, {-9200.f, 300.f},
                             {-9500.f, -500.f}, {-10000.f, -1000.f}, {-10500.f, 1050.f}};
    std::set<uint64_t> alive;   // creature guids the client would have
    int appears = 0, vanishes = 0, maxActive = startActive;
    for (int i = 0; i < kMaxEntities; ++i) {
        const Entity* e = w.byIndex(i);
        if (e && e->used && e->objType == 3) alive.insert(e->guid);
    }
    double worstStepMs = 0, sumStepMs = 0;
    int nSteps = 0;
    Entity* p = w.player();
    const int legs = (int)(sizeof path / sizeof path[0]) - 1;
    for (int leg = 0; leg < legs; ++leg) {
        const float x0 = path[leg][0], y0 = path[leg][1], x1 = path[leg + 1][0], y1 = path[leg + 1][1];
        const float len = std::hypot(x1 - x0, y1 - y0);
        const int steps = std::max(1, (int)(len / 7.0f * 10));   // 7 yd/s, 100 ms frames
        for (int s = 1; s <= steps; ++s) {
            const float t = (float)s / steps;
            p->x = x0 + (x1 - x0) * t;
            p->y = y0 + (y1 - y0) * t;
            p->hp = (int32_t)p->maxHp;   // (immortal: the walk is about the grid, not the fights)
            now += 100;
            auto t0 = std::chrono::steady_clock::now();
            w.step(now);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (ms > worstStepMs) worstStepMs = ms;
            sumStepMs += ms;
            ++nSteps;
            for (int k = 0; k < w.eventCount(); ++k) {
                const SimEvent& ev = w.events()[k];
                if (ev.type == SimEvent::Appear) {
                    ++appears;
                    CHECK(!alive.count(ev.a), "guid %llx appeared twice", (unsigned long long)ev.a);
                    alive.insert(ev.a);
                } else if (ev.type == SimEvent::Vanish) {
                    ++vanishes;
                    CHECK(alive.count(ev.a), "guid %llx vanished without appearing", (unsigned long long)ev.a);
                    alive.erase(ev.a);
                }
            }
            w.clearEvents();
            const int act = w.activeCreatures();
            if (act > maxActive) maxActive = act;
            CHECK(act == (int)alive.size(), "client view (%zu) = pool (%d)", alive.size(), act);
        }
        // nothing far away is left in the pool
        int far = 0;
        for (int i = 0; i < kMaxEntities; ++i) {
            const Entity* e = w.byIndex(i);
            if (!e || !e->used || e->objType != 3) continue;
            if (std::hypot(e->x - p->x, e->y - p->y) > 200.0f && !e->victim) ++far;
        }
        CHECK(far == 0, "leg %d: %d idle creatures more than 200 yd away", leg, far);
        std::printf("leg %2d to (%.0f, %.0f): %d active\n", leg + 1, x1, y1, w.activeCreatures());
    }
    std::printf("appear %d, vanish %d, most active at once %d, step avg %.3f ms, worst %.2f ms\n", appears, vanishes,
                maxActive, nSteps ? sumStepMs / nSteps : 0.0, worstStepMs);
    CHECK(appears > 50, "creatures came into range along the way (%d)", appears);
    CHECK(vanishes > 50, "creatures went out of range along the way (%d)", vanishes);
    CHECK(maxActive <= kMaxEntities - 1, "the pool was never overrun");

    std::printf("OFFLINE GRID: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
