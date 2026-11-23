// WoWVita: how the creatures behave offline (net/offline/offline_ai.cpp) with a whole-continent pack.
//   offline_creatures_test offline.w3do
// Checks, with the CMaNGOS classic-db data of the pack:
//  - the pack has the wander / waypoint movement, EventAI and its texts,
//  - a wandering creature walks (walking speed) and stays within its spawndist,
//  - a patrol walks its waypoints in order and waits where the path says,
//  - a creature pulled too far evades: it runs back to where the fight began, cannot be hurt and is whole again,
//  - its faction's creatures within 10 yards join the fight after 1.5 s,
//  - EventAI: a creature that flees at 15 % says so and runs; a caster casts its spell from the database.
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "../offline/offline_core.hpp"
#include "../offline/offline_data.hpp"

using namespace w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static WorldData data;
static World w;
static uint32_t now = 1000;
static std::vector<SimEvent> seen;

static void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += kSimStepMs) {
        now += kSimStepMs;
        w.step(now);
        for (int i = 0; i < w.eventCount(); ++i) seen.push_back(w.events()[i]);
        w.clearEvents();
    }
}
static uint64_t guidOf(uint32_t idx) {
    return 0xF130000000000000ull | ((uint64_t)(data.spawns[idx].entry & 0xFFFFFF) << 24) | (uint64_t)(idx + 1);
}
static float dist(float ax, float ay, float bx, float by) { return std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)); }
static void fresh(float x, float y) {
    CharacterProfile me;
    me.used = true; me.guid = 1; me.level = 60; me.maxHp = 1000000; me.hp = 1000000;
    me.x = x; me.y = y; me.z = 0;
    std::array<CharacterProfile, kMaxParty> bots{};
    w.reset(now, 77, me, bots, false);
    Entity* p = w.player();
    p->x = x; p->y = y;
    w.updateActivation(now, true);
    w.clearEvents();
    seen.clear();
}
static bool hostile(uint32_t entry) {
    const CreatureTemplate* t = data.creature(entry);
    return t && !t->npcFlags && t->aggroRange > 0 && data.hostile(t->faction, 1);
}

int main(int argc, char** argv) {
    std::string err;
    if (argc < 2 || !loadWorldData(argv[1], data, err)) { std::printf("cannot load the pack: %s\n", err.c_str()); return 2; }
    int random = 0, patrol = 0;
    for (auto& [i, m] : data.spawnMoves) (m.type == 1 ? random : patrol)++;
    std::printf("pack: %zu paths, %d wandering, %d patrolling, %zu creatures with behaviour, %zu texts\n",
                data.paths.size(), random, patrol, data.creatureAi.size(), data.aiTexts.size());
    CHECK(random > 10000 && patrol > 500, "movement of the spawns");
    size_t withEvents = 0;
    for (auto& [e, a] : data.creatureAi) withEvents += !a.events.empty();
    CHECK(withEvents > 500, "EventAI creatures: %zu", withEvents);
    CHECK(data.aiTexts.size() > 100, "texts");
    w.setData(&data);

    // 1. a wandering creature (the nearest to Northshire Abbey that is not hostile, so the player can watch)
    {
        int idx = -1;
        float best = 1e9f;
        for (auto& [i, m] : data.spawnMoves) {
            if (m.type != 1 || m.wander < 3.0f || hostile(data.spawns[i].entry)) continue;
            const float d = dist(data.spawns[i].x, data.spawns[i].y, -8913, -136);
            if (d < best) { best = d; idx = (int)i; }
        }
        CHECK(idx >= 0, "a wandering creature");
        if (idx >= 0) {
            const Spawn& s = data.spawns[idx];
            const float wander = data.spawnMove(idx)->wander;
            fresh(s.x + 60, s.y);
            Entity* e = w.entity(guidOf(idx));
            CHECK(e != nullptr, "it is in the world");
            if (e) {
                float maxFromHome = 0, maxStep = 0, px = e->x, py = e->y, moved = 0;
                int legs = 0;
                uint16_t lastLeg = e->moveLeg;
                for (int k = 0; k < 1200; ++k) {   // a minute
                    run(50);
                    const float st = dist(px, py, e->x, e->y);
                    maxStep = std::max(maxStep, st);
                    moved += st;
                    px = e->x; py = e->y;
                    maxFromHome = std::max(maxFromHome, dist(e->x, e->y, s.x, s.y));
                    if (e->moveLeg != lastLeg) { ++legs; lastLeg = e->moveLeg; CHECK(e->legWalk, "it walks"); }
                }
                std::printf("wander: %s, spawndist %.1f, %d moves in a minute, %.1f yd walked, at most %.1f yd from its spot\n",
                            data.creature(s.entry)->name.c_str(), wander, legs, moved, maxFromHome);
                CHECK(legs >= 3, "it moved several times (%d)", legs);
                CHECK(maxFromHome <= wander + 0.6f, "within its spawndist (%.1f > %.1f)", maxFromHome, wander);
                CHECK(maxStep <= e->walkSpeed * 0.05f + 0.01f, "at walking speed (%.3f per step, walk %.2f)", maxStep, e->walkSpeed);
            }
        }
    }

    // 2. a patrol: walks its points in order
    {
        int idx = -1;
        for (auto& [i, m] : data.spawnMoves) {
            if (m.type != 2 || hostile(data.spawns[i].entry)) continue;
            const auto& pts = data.paths[m.path];
            if (pts.size() < 4) continue;
            bool shortHops = true;
            for (size_t k = 1; k < pts.size(); ++k) if (dist(pts[k].x, pts[k].y, pts[k - 1].x, pts[k - 1].y) > 40) shortHops = false;
            if (shortHops && dist(data.spawns[i].x, data.spawns[i].y, -8913, -136) < 3000) { idx = (int)i; break; }
        }
        CHECK(idx >= 0, "a patrol");
        if (idx >= 0) {
            const SpawnMove* m = data.spawnMove(idx);
            const auto& pts = data.paths[m->path];
            const Spawn& s = data.spawns[idx];
            fresh(s.x + 40, s.y);
            Entity* e = w.entity(guidOf(idx));
            CHECK(e != nullptr, "the patrol is in the world");
            int reached = 0;
            uint16_t want = 0;
            for (int k = 0; e && k < 20 * 120 && reached < 4; ++k) {   // up to two minutes
                Entity* p = w.player();
                p->x = e->x + 40; p->y = e->y;   // the player follows at a distance (the pool keeps it)
                run(50);
                e = w.entity(guidOf(idx));
                if (e && !e->legActive && e->wpIndex != want) { ++reached; want = e->wpIndex; }
            }
            std::printf("patrol: %s, %zu points, %d reached in order\n", data.creature(s.entry)->name.c_str(), pts.size(), reached);
            CHECK(reached >= 3, "it walked from point to point (%d)", reached);
        }
    }

    // 3. evade, 4. assistance: two hostile creatures of one faction close together
    {
        int a = -1, b = -1;
        for (uint32_t i = 0; i < data.spawns.size() && a < 0; ++i) {
            if (!hostile(data.spawns[i].entry) || data.spawnMove(i)) continue;
            const CreatureAi* ai = data.ai(data.spawns[i].entry);
            if (ai && !ai->events.empty()) continue;
            for (uint32_t j = i + 1; j < data.spawns.size() && j < i + 40; ++j) {
                if (data.spawns[j].entry != data.spawns[i].entry || data.spawnMove(j)) continue;
                const float d = dist(data.spawns[i].x, data.spawns[i].y, data.spawns[j].x, data.spawns[j].y);
                if (d > 2 && d < 4.5f) { a = (int)i; b = (int)j; break; }
            }
        }
        CHECK(a >= 0, "a pair to pull");
        if (a >= 0) {
            const Spawn& sa = data.spawns[a];
            // the player 25 yd away on the far side, out of both aggro ranges
            const float ux = sa.x - data.spawns[b].x, uy = sa.y - data.spawns[b].y, ul = std::sqrt(ux * ux + uy * uy);
            fresh(sa.x + ux / ul * 25, sa.y + uy / ul * 25);
            Entity* p = w.player();
            Entity* ea = w.entity(guidOf(a));
            Entity* eb = w.entity(guidOf(b));
            CHECK(ea && eb && !ea->victim && !eb->victim, "both idle");
            if (ea && eb) {
                w.applyDamage(*p, *ea, 1, 0, 0, now);   // a shot
                run(500);
                CHECK(ea->victim == p->guid && !eb->victim, "the one hit comes, the other not yet");
                run(1500);
                CHECK(eb->victim == p->guid, "after 1.5 s its neighbour joins in (%s)", data.creature(sa.entry)->name.c_str());
                // now run far away: both give up and go back
                const float cx = ea->combatX, cy = ea->combatY;
                ea->hp = (int32_t)ea->maxHp / 2;
                int steps = 0;
                for (; steps < 600 && ea->aiState != 3; ++steps) {   // away at 10 yd/s: not hurt for 15 s, 30 yd off
                    p->x += 0.35f; p->y += 0.35f;
                    run(50);
                }
                std::printf("evade: after %.1f s of running away\n", steps * 0.05f);
                CHECK(ea->aiState == 3, "it evades");
                CHECK(steps * 50 + 2000 >= 15000, "not before its 15 s pursuit (since it was hit)");
                const int32_t hpWas = ea->hp;
                w.applyDamage(*p, *ea, 50, 0, 0, now);
                CHECK(ea->hp == hpWas, "an evading creature cannot be hurt");
                p->x = cx + 50; p->y = cy;   // (in sight: the pool keeps it)
                run(25000);
                CHECK(ea->aiState == 0 && dist(ea->x, ea->y, cx, cy) < 1.0f, "back where the fight began");
                CHECK(ea->hp == (int32_t)ea->maxHp, "and whole again");
            }
        }
    }

    // 5. EventAI: flee at 15 % with its text, 6. a cast from the database
    {
        int fleeIdx = -1, castIdx = -1;
        uint32_t castSpell = 0;
        for (uint32_t i = 0; i < data.spawns.size(); ++i) {
            const CreatureAi* ai = data.ai(data.spawns[i].entry);
            if (!ai || !hostile(data.spawns[i].entry)) continue;
            for (const AiEvent& ev : ai->events)
                for (const AiAction& ac : ev.act) {
                    if (fleeIdx < 0 && ac.type == 25 && ev.type == 2) fleeIdx = (int)i;
                    if (castIdx < 0 && ac.type == 11 && (ev.type == 0 || ev.type == 9) && ev.phaseMask == 0 && ev.chance >= 50) {
                        const SpellDef* s = data.spell((uint32_t)ac.p[0]);
                        if (s && s->kind == SpellKind::Damage && ac.p[1] == 1) { castIdx = (int)i; castSpell = s->id; }
                    }
                }
        }
        CHECK(fleeIdx >= 0 && castIdx >= 0, "EventAI creatures to fight");
        if (fleeIdx >= 0) {
            const Spawn& s = data.spawns[fleeIdx];
            fresh(s.x + 3, s.y);
            Entity* p = w.player();
            Entity* e = w.entity(guidOf(fleeIdx));
            CHECK(e != nullptr, "the runner is there");
            if (e) {
                w.applyDamage(*p, *e, (int32_t)e->maxHp * 87 / 100, 0, 1, now);
                run(1500);
                bool said = false;
                for (const SimEvent& ev : seen) if (ev.type == SimEvent::CreatureText && ev.a == e->guid) said = true;
                std::printf("flee: %s at %d %% health: %s\n", data.creature(s.entry)->name.c_str(), (int)(e->hp * 100 / (int)e->maxHp),
                            e->fleeUntilMs ? "runs" : "stays");
                CHECK(e->fleeUntilMs > 0, "it flees");
                CHECK(said, "and says so");
                const float d0 = dist(e->x, e->y, p->x, p->y);
                run(2000);
                CHECK(dist(e->x, e->y, p->x, p->y) > d0 + 3 || e->fleeTo, "it runs off (%.1f -> %.1f)", d0, dist(e->x, e->y, p->x, p->y));
            }
        }
        if (castIdx >= 0) {
            const Spawn& s = data.spawns[castIdx];
            fresh(s.x + 4, s.y);
            Entity* p = w.player();
            Entity* e = w.entity(guidOf(castIdx));
            if (e) {
                w.applyDamage(*p, *e, 1, 0, 0, now);
                bool cast = false;
                for (int k = 0; k < 20 * 30 && !cast; ++k) {
                    run(50);
                    for (const SimEvent& ev : seen)
                        if ((ev.type == SimEvent::CastStarted || ev.type == SimEvent::CastFinished) && ev.a == e->guid && ev.spell == castSpell) cast = true;
                    if (e->hp < (int32_t)e->maxHp / 2) e->hp = (int32_t)e->maxHp;
                }
                std::printf("cast: %s casts %s (%u)\n", data.creature(s.entry)->name.c_str(), data.spell(castSpell)->name.c_str(), castSpell);
                CHECK(cast, "the caster casts its spell");
            }
        }
    }

    std::printf("OFFLINE CREATURES: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
