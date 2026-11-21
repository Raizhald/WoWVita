// WoWVita: flight masters in the offline world, through the real client session (GameSession).
//   offline_taxi_test pack.w3do   (a pack from tools/offline_data/export_world_sql.py)
// The flight data is the client's (taxi.txt, transports.txt from the converter); the test writes a small set of its own
// at the real flight masters: 2 Stormwind (Dungar Longdrink 352), 4 Sentinel Hill (Thor 523), 5 Lakeshire (Ariena
// Stormfeather 931); paths 2<->4 (50 c) and 2<->5 (60 c), each with points that bend through the air.
//   - a human knows Stormwind; talking to Thor discovers Sentinel Hill and opens the map
//   - 4 -> 2: paid, mounted (6852), TAXI_FLIGHT, creatures leave it alone, the server and the client follow the path, landing
//     at Stormwind; then the flags are gone
//   - 2 -> 5 before Lakeshire is known: refused; after the visit 5 -> 4 goes over Stormwind (ACTIVATETAXIEXPRESS, 110 c)
//   - the known points are saved
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../client.hpp"
#include "../game.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_server.hpp"

namespace off = w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: offline_taxi_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    auto spawnOf = [&](uint32_t entry) -> const off::Spawn* {
        for (const off::Spawn& s : data.spawns) if (s.entry == entry) return &s;
        return nullptr;
    };
    const off::Spawn* sw = spawnOf(352);
    const off::Spawn* sh = spawnOf(523);
    const off::Spawn* ls = spawnOf(931);
    CHECK(sw && sh && ls, "the three flight masters are in the pack");
    if (!sw || !sh || !ls) return 1;

    // the client's data: taxi.txt and transports.txt
    w3n::TaxiTable taxi;
    auto node = [&](uint32_t id, const off::Spawn* s, const char* name) {
        w3n::TaxiNode n; n.id = id; n.map = 0; n.x = s->x + 3; n.y = s->y; n.z = s->z; n.name = name; taxi.addNode(n);
    };
    node(2, sw, "Stormwind, Elwynn"); node(4, sh, "Sentinel Hill, Westfall"); node(5, ls, "Lakeshire, Redridge");
    taxi.addPath(2, 4, 50); taxi.addPath(4, 2, 50); taxi.addPath(2, 5, 60); taxi.addPath(5, 2, 60);
    const char* tpath = "offline_taxi_test_transports.txt";
    {
        FILE* f = std::fopen(tpath, "w");
        uint32_t pid = 10;
        auto path = [&](const off::Spawn* a, const off::Spawn* b) {   // 41 points (as dense as the real ones), a hump of 80 yd
            for (int i = 0; i <= 40; ++i) {
                const float t = i / 40.0f;
                const float x = a->x + 3 + (b->x - a->x) * t, y = a->y + (b->y - a->y) * t;
                const float z = a->z + (b->z - a->z) * t + std::sin(t * 3.14159f) * 80.0f;
                std::fprintf(f, "T\t%u\t%d\t0\t%.2f\t%.2f\t%.2f\t0\t0\n", pid, i, x, y, z);
            }
            ++pid;
        };
        std::fprintf(f, "P\t176495\t350818\n");   // (a ship's period line: skipped)
        path(sw, sh); path(sh, sw); path(sw, ls); path(ls, sw);
        std::fclose(f);
    }

    const char* savePath = "offline_taxi_test_save.w3d";
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    off::OfflineSession session;
    w3n::WorldClient wc;
    w3n::GameSession gs;
    uint32_t now = 10000;
    session.setup(&data, savePath);
    session.setParty(false);
    session.setTaxi(&taxi, tpath);
    if (getenv("QTRACE")) session.trace = [](const std::string& l) { std::printf("    [trace] %s\n", l.c_str()); };
    CHECK(session.open(err), "open: %s", err.c_str());
    const uint64_t guid = session.characters()[0].guid;
    float pos[3] = {0, 0, 0};
    float o = 0;
    CHECK(session.enterWorld(guid, now, pos, &o), "enter world");
    wc.packetSink = [&](uint16_t op, const w3n::Bytes& b) { session.onClientPacket(op, b, now); };
    session.setSink([&](uint16_t op, const w3n::Bytes& b) { gs.handle(op, b, now); });
    wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
    gs.attach(wc, now);
    gs.taxiTable = taxi;
    auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 16) { now += 16; session.update(now); gs.update(now); } };
    run(500);
    off::World& w = session.world();
    off::Entity* p = w.player();
    p->money = 1000;
    w.playerProfile().money = 1000;
    auto put = [&](float x, float y, float z) {
        gs.mover.reset(x, y, z, 0, now);
        w.setPosition(*p, x, y, z, 0);
        run(1500);
    };
    auto npcAt = [&](uint32_t entry) -> const off::Entity* {
        for (int i = 0; i < off::kMaxEntities; ++i) {
            const off::Entity* e = w.byIndex(i);
            if (e && e->used && e->objType == 3 && e->entry == entry) return e;
        }
        return nullptr;
    };
    auto talk = [&](uint32_t entry) {
        const off::Spawn* s = spawnOf(entry);
        put(s->x + 2.0f, s->y, s->z);
        const off::Entity* e = npcAt(entry);
        if (!e) return (const off::Entity*)nullptr;
        gs.taxi = w3n::TaxiWindow();
        float q[3] = {p->x, p->y, p->z};
        gs.interact(e->guid, q, now);
        run(500);
        return e;
    };
    auto selfField = [&](uint16_t f) { const w3n::WorldObject* me = gs.objects.find(gs.objects.self()); return me ? me->field(f) : 0u; };

    const off::Entity* thor = talk(523);
    CHECK(thor != nullptr, "Thor is there");
    CHECK(gs.taxi.open && gs.taxi.current == 4, "the flight map opens at Sentinel Hill (open %d, node %u)", gs.taxi.open, gs.taxi.current);
    CHECK(gs.taxi.known(2) && gs.taxi.known(4) && !gs.taxi.known(5), "known: Stormwind (a human's), Sentinel Hill (just found)");

    // ---- 4 -> 2
    CHECK(gs.flyTo(2, now), "fly to Stormwind");
    run(300);
    CHECK(session.flying() && p->money == 950, "flying, 50 copper paid (%u)", p->money);
    CHECK(selfField(w3n::UF::UNIT_MOUNTDISPLAYID) == 6852 && (selfField(w3n::UF::UNIT_FLAGS) & 0x00100000), "on a gryphon, TAXI_FLIGHT");
    float cpos[3];
    float cyaw = 0;
    bool taxiFlag = false;
    CHECK(gs.selfServerMove(now, cpos, &cyaw, &taxiFlag) && taxiFlag, "the client follows the flight spline");
    run(8000);
    CHECK(p->z > std::min(sw->z, sh->z) + 20.0f, "in the air on the path (z %.0f)", p->z);
    CHECK(gs.selfServerMove(now, cpos, &cyaw, &taxiFlag) && std::hypot(cpos[0] - p->x, cpos[1] - p->y) < 15.0f,
          "client and server fly together (%.0f yd apart)", std::hypot(cpos[0] - p->x, cpos[1] - p->y));
    // creatures leave a flying character alone
    bool attacked = false;
    for (int i = 0; i < off::kMaxEntities; ++i) {
        const off::Entity* e = w.byIndex(i);
        attacked = attacked || (e && e->used && e->victim == p->guid);
    }
    CHECK(!attacked, "nothing attacks a flight");
    run(180000);
    CHECK(!session.flying(), "landed");
    CHECK(std::hypot(p->x - (sw->x + 3), p->y - sw->y) < 2.0f, "at Stormwind (%.0f %.0f)", p->x, p->y);
    CHECK(selfField(w3n::UF::UNIT_MOUNTDISPLAYID) == 0 && !(selfField(w3n::UF::UNIT_FLAGS) & 0x00100000), "off the gryphon");

    // ---- 2 -> 5 unknown, then 5 -> 4 over Stormwind
    talk(352);
    CHECK(gs.taxi.open && gs.taxi.current == 2, "Dungar's map");
    {
        w3n::Writer wr;
        wr.u64(npcAt(352)->guid); wr.u32(2); wr.u32(5);
        session.onClientPacket(0x1AD, wr.b, now);   // CMSG_ACTIVATETAXI, as a client that does not check
        run(300);
        CHECK(!session.flying(), "no flight to a point not yet found");
    }
    p->x = ls->x; p->y = ls->y;   // (walked there)
    talk(931);
    CHECK(gs.taxi.open && gs.taxi.current == 5 && gs.taxi.known(5), "Lakeshire found");
    const uint32_t before = p->money;
    CHECK(gs.flyTo(4, now), "fly Lakeshire -> Sentinel Hill");
    run(300);
    CHECK(session.flying() && before - p->money == 110, "over Stormwind: 60 + 50 copper (%u)", before - p->money);
    run(400000);
    CHECK(!session.flying() && std::hypot(p->x - (sh->x + 3), p->y - sh->y) < 2.0f, "at Sentinel Hill");

    std::string serr;
    CHECK(session.save(&serr), "save: %s", serr.c_str());
    off::OfflineSaveData sd;
    CHECK(off::loadOffline(savePath, sd, serr), "load: %s", serr.c_str());
    const auto& m = sd.player.taxiMask;
    auto bit = [&](uint32_t n) { return (m[(n - 1) / 32] >> ((n - 1) % 32)) & 1u; };
    CHECK(bit(2) && bit(4) && bit(5) && !bit(3), "the points found are saved");
    remove(tpath);
    std::printf("OFFLINE TAXI: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
