// transportworld_test — 7.24: a zeppelin map change. SMSG_TRANSFER_PENDING names the transport entry, and SMSG_NEW_WORLD
// then carries the offset ON the transport, not a world position (vMaNGOS Player::SendNewWorld). The client keeps it in
// takeTransportSpawn() and does not feed the local coordinates to the mover as a world position.
#include <cstdio>
#include <cmath>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    GameSession s;
    uint32_t m = 0;
    float p[3] = {0, 0, 0}, o = 0;

    // A normal transfer: only the map id.
    {
        Writer w;
        w.u32(1);
        s.handle(0x03F, w.b, 1000);
    }
    CHECK(s.takeTransferPending(&m) && m == 1, "a plain transfer names the new map");
    CHECK(!s.takeTransferPending(&m), "...once");
    {
        Writer w;
        w.u32(1);
        w.f32(10); w.f32(20); w.f32(30); w.f32(1.5f);
        s.handle(0x03E, w.b, 2000);
    }
    CHECK(s.mapId() == 1, "the new world changes the map");
    CHECK(s.takeTeleport(p, &o) && p[0] == 10 && p[1] == 20 && p[2] == 30 && std::fabs(o - 1.5f) < 1e-6f,
          "a plain new world is a world position");
    GameSession::TransportSpawn ts;
    CHECK(!s.takeTransportSpawn(&ts), "a plain new world has no transport spawn");

    const uint64_t ship = 0x1FC0000000028407ull;
    ObjectTable::GoInfo gi;
    gi.type = 15; gi.data[0] = 302; gi.data[1] = 30; gi.data[2] = 1;
    s.objects.setGoInfo(176495, gi);
    CHECK(s.objects.restoreTransport({176495, 3031, 10000, ship}, 2500) == ship, "the passenger's real ship is known before transfer");
    s.mover.setTransport(ship, 1.5f, -2, 3.25f, 0.75f, 2500);

    // A zeppelin transfer: map, transport entry, old map id; then the offset on the transport.
    {
        Writer w;
        w.u32(0); w.u32(176495); w.u32(1);
        s.handle(0x03F, w.b, 3000);
    }
    CHECK(s.takeTransferPending(&m) && m == 0, "the transport transfer names the new map");
    {
        Writer w;
        w.u32(0);
        w.f32(1.5f); w.f32(-2.0f); w.f32(3.25f); w.f32(0.75f);
        s.handle(0x03E, w.b, 4000);
    }
    CHECK(s.mapId() == 0, "the transport new world changes the map");
    const auto* kept = s.objects.find(ship);
    CHECK(kept && kept->transportProgress == 11500 && !kept->predicted,
          "the server-confirmed passenger's ship survives NEW_WORLD with its current phase and real GUID");
    CHECK(s.mover.queue().empty(), "old-world movement is not sent after transfer");
    CHECK(s.takeTransportSpawn(&ts) && ts.entry == 176495 && ts.lx == 1.5f && ts.ly == -2.0f && ts.lz == 3.25f &&
          std::fabs(ts.lo - 0.75f) < 1e-6f, "the offset on the transport is kept");
    CHECK(!s.takeTransportSpawn(&ts), "...once");
    CHECK(!s.takeTeleport(p, &o), "the offset on the transport is not a world position");
    CHECK(s.mover.info().x == 10 && s.mover.info().y == 20 && s.mover.info().z == 30,
          "the mover kept the last world position, not the offset on the transport");

    // After the transport new world the entry is forgotten: the next one is an ordinary transfer again.
    {
        Writer w;
        w.u32(1);
        s.handle(0x03F, w.b, 5000);
    }
    s.takeTransferPending(&m);
    {
        Writer w;
        w.u32(1); w.f32(4); w.f32(5); w.f32(6); w.f32(2);
        s.handle(0x03E, w.b, 6000);
    }
    CHECK(s.takeTeleport(p, &o) && p[0] == 4 && p[1] == 5 && p[2] == 6, "the next new world is a world position again");
    CHECK(!s.takeTransportSpawn(&ts), "and no stale transport spawn");
    CHECK(!s.objects.find(ship), "an ordinary transfer still clears the old ship");

    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
