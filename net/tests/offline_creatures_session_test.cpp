// WoWVita: creature behaviour as the client sees it (OfflineSession -> GameSession), with a whole-continent pack.
//   offline_creatures_session_test offline.w3do
//  - a wandering creature's move reaches the client as one walking spline (not a stream of short ones),
//    and the client's copy of it moves,
//  - a creature that flees at 15 % health: the client gets its text emote, with the creature's name.
#include <cmath>
#include <cstdio>
#include <string>

#include "../client.hpp"
#include "../game.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_server.hpp"

namespace off = w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: offline_creatures_session_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    const char* savePath = "offline_creatures_session_save.w3d";
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    off::OfflineSession session;
    w3n::WorldClient wc;
    w3n::GameSession gs;
    uint32_t now = 10000;
    session.setup(&data, savePath);
    session.setParty(false);
    CHECK(session.open(err), "open: %s", err.c_str());
    const uint64_t guid = session.characters()[0].guid;
    float pos[3] = {0, 0, 0};
    float o = 0;
    CHECK(session.enterWorld(guid, now, pos, &o), "enter world");
    int splines = 0, walking = 0;
    uint64_t watch = 0;
    wc.packetSink = [&](uint16_t op, const w3n::Bytes& b) { session.onClientPacket(op, b, now); };
    session.setSink([&](uint16_t op, const w3n::Bytes& b) {
        if (op == 0x0DD && watch && b.size() > 30) {
            // packed guid, x y z, time, kind, flags...
            uint8_t mask = b[0];
            size_t at = 1;
            uint64_t g = 0;
            for (int i = 0; i < 8; ++i) if (mask & (1 << i)) g |= (uint64_t)b[at++] << (8 * i);
            if (g == watch && b.size() >= at + 12 + 4 + 1 + 4 && b[at + 16] == 0) {
                ++splines;
                const uint32_t flags = b[at + 17] | (b[at + 18] << 8) | (b[at + 19] << 16) | ((uint32_t)b[at + 20] << 24);
                if (!(flags & 0x100)) ++walking;
            }
        }
        gs.handle(op, b, now);
    });
    wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
    gs.attach(wc, now);
    auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 16) { now += 16; session.update(now); gs.update(now); } };
    run(500);
    off::World& w = session.world();
    off::Entity* p = w.player();
    p->maxHp = 100000; p->hp = 100000;
    auto put = [&](float x, float y, float z) {
        gs.mover.reset(x, y, z, 0, now);
        w.setPosition(*p, x, y, z, 0);
        run(1500);
    };
    auto guidOf = [&](uint32_t idx) {
        return 0xF130000000000000ull | ((uint64_t)(data.spawns[idx].entry & 0xFFFFFF) << 24) | (uint64_t)(idx + 1);
    };

    // a wanderer near Northshire that does not attack
    int idx = -1;
    float best = 1e9f;
    for (auto& [i, m] : data.spawnMoves) {
        const off::CreatureTemplate* t = data.creature(data.spawns[i].entry);
        if (m.type != 1 || m.wander < 3.0f || !t || data.hostile(t->faction, 1)) continue;
        const float d = std::hypot(data.spawns[i].x + 8913.0f, data.spawns[i].y + 136.0f);
        if (d < best) { best = d; idx = (int)i; }
    }
    CHECK(idx >= 0, "a wanderer");
    if (idx >= 0) {
        const off::Spawn& s = data.spawns[idx];
        put(s.x + 40, s.y, s.z);
        watch = guidOf(idx);
        off::Entity* e = w.entity(watch);
        CHECK(e != nullptr, "it is there");
        float minX = 1e9f, maxX = -1e9f;
        int legs = 0;
        uint16_t last = e ? e->moveLeg : 0;
        for (int k = 0; k < 60 * 4; ++k) {   // a minute
            run(250);
            if (const w3n::WorldObject* ob = gs.objects.find(watch)) {
                float at[3];
                ob->positionAt(now, at);
                minX = std::min(minX, at[0]); maxX = std::max(maxX, at[0]);
            }
            if (e && e->moveLeg != last) { ++legs; last = e->moveLeg; }
        }
        std::printf("wander: %d moves, %d splines sent (%d walking), the client's copy moved %.1f yd east-west\n",
                    legs, splines, walking, maxX - minX);
        CHECK(legs >= 3, "it moved");
        CHECK(splines >= legs && splines <= legs + 2, "one spline per move (%d for %d)", splines, legs);
        CHECK(walking == splines, "walking");
        CHECK(maxX - minX > 1.0f, "the client sees it move");
    }

    // a creature that flees at 15 %: its emote
    int fleeIdx = -1;
    for (uint32_t i = 0; i < data.spawns.size() && fleeIdx < 0; ++i) {
        const off::CreatureAi* ai = data.ai(data.spawns[i].entry);
        if (!ai) continue;
        for (const off::AiEvent& ev : ai->events)
            if (ev.type == 2) {
                bool text = false, flee = false;
                for (const off::AiAction& ac : ev.act) { text |= ac.type == 1; flee |= ac.type == 25; }
                if (text && flee && data.hostile(data.creature(data.spawns[i].entry)->faction, 1)) fleeIdx = (int)i;
            }
    }
    CHECK(fleeIdx >= 0, "a creature that flees");
    if (fleeIdx >= 0) {
        const off::Spawn& s = data.spawns[fleeIdx];
        put(s.x + 3, s.y, s.z);
        off::Entity* e = w.entity(guidOf(fleeIdx));
        CHECK(e != nullptr, "the runner is there");
        if (e) {
            const size_t before = gs.chat.size();
            w.applyDamage(*p, *e, (int32_t)e->maxHp * 87 / 100, 0, 1, now);
            run(1500);
            bool seen = false;
            for (size_t i = before; i < gs.chat.size(); ++i) {
                const std::string line = gs.lineText(gs.chat[i]);
                if (line.find("run away") != std::string::npos || line.find("flee") != std::string::npos) {
                    std::printf("chat: %s\n", line.c_str());
                    seen = line.find(data.creature(s.entry)->name) == 0 && line.find("%s") == std::string::npos;
                }
            }
            CHECK(seen, "the client shows '<name> attempts to run away in fear!'");
        }
    }
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    std::printf("OFFLINE CREATURES (SESSION): %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
