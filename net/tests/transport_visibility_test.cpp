// Regression: login CREATE -> OUT_OF_RANGE must not hide a known ship until its next departure.
#include "objects.hpp"
#include "wire.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace w3n;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void create(ObjectTable& ot, uint64_t guid, uint32_t type, uint32_t progress, uint32_t now) {
    Writer w;
    w.u32(1); w.u8(0); w.u8(3); w.packedGuid(guid); w.u8(5);
    w.u8(0x42);   // HAS_POSITION | TRANSPORT
    w.f32(0); w.f32(0); w.f32(5); w.f32(0); w.u32(progress);
    w.u8(1);
    w.u32((1u << UF::OBJECT_ENTRY) | (1u << UF::GAMEOBJECT_DISPLAYID) | (1u << UF::GAMEOBJECT_TYPE_ID));
    w.u32(500); w.u32(3031); w.u32(type);
    CHECK(ot.parseUpdate(w.b.data(), w.b.size(), now));
}
static void outOfRange(ObjectTable& ot, uint64_t guid, uint32_t now) {
    Writer w;
    w.u32(1); w.u8(0); w.u8(4); w.u32(1); w.packedGuid(guid);
    CHECK(ot.parseUpdate(w.b.data(), w.b.size(), now));
}

int main() {
    ObjectTable ot;
    const uint64_t ship = 0x1FC0000000028407ull;
    // Just like the SD log: template is not available yet when OUT_OF_RANGE arrives.
    create(ot, ship, 15, 0, 1000);
    outOfRange(ot, ship, 1000);
    CHECK(ot.find(ship) && ot.find(ship)->transportOutOfRange && !ot.find(ship)->predicted);
    auto queries = ot.takeCreated();
    CHECK(std::find(queries.begin(), queries.end(), ship) != queries.end());
    auto events = ot.takeTransportEvents();
    CHECK(events.size() == 2 && events.back().kind == 4);

    std::string txt;
    int idx = 0;
    auto node = [&](float x, uint32_t flags, uint32_t delay) {
        char b[128];
        std::snprintf(b, sizeof b, "T\t9\t%d\t1\t%.1f\t0\t5\t%u\t%u\n", idx++, x, flags, delay);
        txt += b;
    };
    node(-20, 0, 0); node(0, 2, 10);
    for (int x = 50; x < 600; x += 50) node((float)x, 0, 0);
    node(600, 2, 10);
    for (int x = 550; x > 0; x -= 50) node((float)x, 0, 0);
    node(-5, 0, 0);
    CHECK(ot.transports.parse(txt));
    ObjectTable::GoInfo gi;
    gi.type = 15; gi.data[0] = 9; gi.data[1] = 14; gi.data[2] = 1;
    ot.setGoInfo(500, gi);
    ot.tickTransports(6000);
    const TransportPose* p = ot.transportPose(ship);
    CHECK(p && p->valid && p->map == 1 && !p->moving && std::fabs(p->x) < 0.01f);
    ot.tickTransports(16000);
    p = ot.transportPose(ship);
    CHECK(p && p->moving && p->x > 1);

    // The server's departure CREATE re-synchronizes the SAME real GUID, with no duplicate or new query.
    create(ot, ship, 15, 30000, 20000);
    CHECK(ot.find(ship) && !ot.find(ship)->transportOutOfRange && ot.find(ship)->transportProgress == 30000);
    CHECK(ot.takeCreated().empty());
    ot.tickTransports(20000);
    p = ot.transportPose(ship);
    const TransportPath* path = ot.transports.path(9, 14, 1);
    CHECK(p && path && std::fabs(p->x - path->at(30000).x) < 0.01f);

    // OUT_OF_RANGE still removes elevators and ordinary objects, and DESTROY still removes the ship.
    const uint64_t lift = 0xF120000000000001ull;
    create(ot, lift, 11, 0, 20000);
    outOfRange(ot, lift, 20000);
    CHECK(!ot.find(lift));
    const uint64_t ordinary = 0xF110000000000002ull;
    create(ot, ordinary, 3, 0, 20000);
    outOfRange(ot, ordinary, 20000);
    CHECK(!ot.find(ordinary));
    Writer destroy; destroy.u64(ship);
    CHECK(ot.handle(ObjOp::SMSG_DESTROY_OBJECT, destroy.b, 21000));
    CHECK(!ot.find(ship));
    ot.tickTransports(21000);
    CHECK(!ot.transportPose(ship));

    // Some servers omit the type field: the real MO_TRANSPORT GUID still identifies a ship.
    create(ot, ship, 0, 0, 22000);
    outOfRange(ot, ship, 22000);
    CHECK(ot.find(ship) && ot.find(ship)->transportOutOfRange);
    ot.clear();
    CHECK(!ot.find(ship) && ot.transportPoses().empty());

    // Snapshot time must be common to ALL ships, even if their last create packets arrived minutes apart.
    create(ot, ship, 15, 30000, 1000);
    create(ot, ship + 1, 15, 90000, 6000);
    auto memory = ot.transportMemory(16000);
    CHECK(memory.size() == 2);
    for (const auto& m : memory) {
        CHECK(m.progress == (m.guid == ship ? 45000u : 100000u));
        CHECK((m.guid >> 48) == 0x1FC0u);
    }
    ObjectTable next;
    CHECK(next.transports.parse(txt));
    next.setGoInfo(500, gi);
    ObjectTable::TransportMemory saved{500, 3031, 5000, ship};
    CHECK(next.restoreTransport(saved, 10000) == ship);
    CHECK(next.restoreTransport(saved, 10000) == 0);   // never duplicate an entry / GUID
    CHECK(next.find(ship) && !next.find(ship)->predicted && next.find(ship)->restoredTransport);
    next.tickTransports(10000);
    p = next.transportPose(ship);
    CHECK(p && !p->moving && p->map == 1);            // visible / walkable at the stop BEFORE a new CREATE
    // Boarding sends the real GUID, not the previous fff... drawing-only placeholder.
    Mover mover;
    mover.reset(0, 0, 5, 0, 10000);
    mover.setTransport(ship, 1, 2, 3, 0, 10000);
    MovementInfo mi;
    Reader rd(mover.queue().back().data.data(), mover.queue().back().data.size());
    CHECK(readMovementInfo(rd, mi) && mi.transportGuid == ship);
    outOfRange(next, ship, 11000);
    CHECK(next.find(ship) && next.find(ship)->restoredTransport);
    create(next, ship, 15, 12000, 12000);
    CHECK(next.find(ship) && !next.find(ship)->restoredTransport);
    next.dropRestoredTransports();
    CHECK(next.find(ship));                          // do not drop a transport confirmed this session
    next.clear();
    CHECK(next.restoreTransport(saved, 10000) == ship);
    next.tickTransports(10000);
    next.dropRestoredTransports();
    CHECK(!next.find(ship) && !next.transportPose(ship));
    saved.guid = 0xFFF0000000028407ull;
    CHECK(next.restoreTransport(saved, 10000) == 0);  // legacy invented GUIDs must never become boardable
    // Unsigned millisecond wrap is part of the protocol clock, not a lost 49 days.
    create(ot, ship, 15, 0xFFFFFFF0u, 0xFFFFFF00u);
    memory = ot.transportMemory(0x100u);
    bool checkedWrap = false;
    for (const auto& m : memory) if (m.guid == ship) { CHECK(m.progress == 0x1F0u); checkedWrap = true; }
    CHECK(checkedWrap);

    // A 154 ms PERIOD error accumulates into another continent after hundreds of cycles. No phase guessing.
    ObjectTable crossing;
    std::string twoMaps;
    int ni = 0;
    auto mapNode = [&](uint32_t map, float x, uint32_t flags = 0, uint32_t delay = 0) {
        char b[128];
        std::snprintf(b, sizeof b, "T\t9\t%d\t%u\t%.1f\t0\t5\t%u\t%u\n", ni++, map, x, flags, delay);
        twoMaps += b;
    };
    mapNode(0, -20); mapNode(0, 0, 2, 10); mapNode(0, 50); mapNode(0, 100); mapNode(0, 150);
    mapNode(1, 950); mapNode(1, 1000, 2, 10); mapNode(1, 1050); mapNode(1, 1100); mapNode(1, 1150); mapNode(1, 1200);
    CHECK(crossing.transports.parse(twoMaps));
    crossing.setGoInfo(500, gi);
    const TransportPath* generatedPath = crossing.transports.path(9, 14, 1);
    CHECK(generatedPath != nullptr);
    if (!generatedPath) return 1;
    const uint32_t generated = generatedPath->period(), serverPeriod = generated + 154;
    uint32_t arrival = 0;
    for (uint32_t ms = 0; ms < generated; ++ms) if (generatedPath->at(ms).map == 1) { arrival = ms; break; }
    CHECK(arrival != 0);
    uint32_t raw = 0;
    for (uint32_t cycles = 1; cycles < 5000; ++cycles) {
        const uint32_t candidate = cycles * serverPeriod + arrival;
        if (generatedPath->at(candidate).map == 0) { raw = candidate; break; }
    }
    CHECK(raw != 0 && generatedPath->at(raw, serverPeriod).map == 1);
    CHECK(crossing.transports.period(500, generated) == generated); // no configuration: generated fallback
    twoMaps += "P\t500\t" + std::to_string(serverPeriod) + "\n";
    CHECK(crossing.transports.parse(twoMaps));
    CHECK(crossing.transports.period(500, generated) == serverPeriod);
    create(crossing, ship, 15, raw, 1000);
    crossing.tickTransports(1000);
    CHECK(crossing.transportPose(ship) && crossing.transportPose(ship)->map == 1);
    CHECK(std::fabs(crossing.transportPose(ship)->x - 1000) < 0.01f);
    CHECK(crossing.find(ship)->transportProgress == raw); // raw clock is unchanged
    create(crossing, ship, 15, raw + 100, 1100);
    crossing.tickTransports(1100);
    CHECK(crossing.transportPose(ship) && crossing.transportPose(ship)->map == 1);
    const auto alignedMemory = crossing.transportMemory(1200);
    CHECK(alignedMemory.size() == 1 && alignedMemory[0].progress == raw + 200);
    if (!alignedMemory.empty()) {
        ObjectTable afterWorldClear;
        CHECK(afterWorldClear.transports.parse(twoMaps));
        afterWorldClear.setGoInfo(500, gi);
        CHECK(afterWorldClear.restoreTransport(alignedMemory[0], 1200) == ship);
        afterWorldClear.tickTransports(1200);
        CHECK(afterWorldClear.transportPose(ship) && afterWorldClear.transportPose(ship)->map == 1);
    }
    // The extra final fraction of the server period must not wrap early through the generated period.
    const auto* configuredPath = crossing.transports.path(9, 14, 1);
    CHECK(configuredPath && configuredPath->at(serverPeriod - 1, serverPeriod).map == 0);
    CHECK(configuredPath && configuredPath->at(serverPeriod, serverPeriod).map == 0);
    printf("TRANSPORT-VISIBILITY: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
