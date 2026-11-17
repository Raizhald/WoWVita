// Independently decode as the Vanilla server does, not with our matching reader/writer pair.
#include "objects.hpp"
#include <cmath>
#include <cstdio>
using namespace w3n;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
static constexpr uint64_t ship = 0x1FC0000000028407ull;

static void serverDecode(const Bytes& bytes, float tx, float ty, float tz) {
    CHECK(bytes.size() == 52);   // flags/time/world=24, full GUID=8, local=16, fallTime=4
    Reader r(bytes);
    CHECK(r.u32() & MoveFlag::OnTransport);
    r.u32();
    r.f32(); r.f32(); r.f32(); r.f32();
    CHECK(r.u64() == ship);     // server MovementInfo::Read: ObjectGuid, NOT ReadAsPacked()
    CHECK(std::fabs(r.f32() - tx) < 0.001f);
    CHECK(std::fabs(r.f32() - ty) < 0.001f);
    CHECK(std::fabs(r.f32() - tz) < 0.001f);
    r.f32(); r.u32();
    CHECK(!r.bad && r.left() == 0);
}
int main() {
    Mover m;
    m.reset(1318, -4658, 54, 1, 1000);
    m.setTransport(ship, -5, 0.5f, -17.9f, 0.25f, 1100);
    CHECK(m.queue().size() == 1 && m.queue().back().opcode == MoveOp::Heartbeat);
    serverDecode(m.queue().back().data, -5, 0.5f, -17.9f);
    m.queue().clear();
    // Standing on deck must keep sending passenger state, even with no movement keys held.
    m.update(0, 1318, -4658, 54, 1, 1600);
    CHECK(m.queue().size() == 1);
    if (!m.queue().empty()) serverDecode(m.queue().back().data, -5, 0.5f, -17.9f);
    m.queue().clear();
    m.setTransportOffset(2, -7, -17.9f, 0.25f);
    m.update(MoveFlag::Forward, 1320, -4640, 54, 1, 1700);
    CHECK(!m.queue().empty());
    if (!m.queue().empty()) serverDecode(m.queue().back().data, 2, -7, -17.9f);

    // A Vanilla server's relayed movement carries a packed MOVER prefix, but a FULL transport GUID inside.
    Writer relay;
    relay.packedGuid(77);
    relay.u32(MoveFlag::OnTransport); relay.u32(2000);
    relay.f32(1318); relay.f32(-4658); relay.f32(54); relay.f32(1);
    relay.u64(ship);
    relay.f32(-5); relay.f32(0.5f); relay.f32(-17.9f); relay.f32(0.25f);
    relay.u32(0);
    ObjectTable objects;
    Writer create;
    create.u32(1); create.u8(0); create.u8(3); create.packedGuid(77); create.u8(4);
    create.u8(0x40); create.f32(0); create.f32(0); create.f32(0); create.f32(0); create.u8(0);
    CHECK(objects.parseUpdate(create.b.data(), create.b.size(), 1000));
    CHECK(objects.handle(MoveOp::Heartbeat, relay.b, 2000));
    const auto* passenger = objects.find(77);
    CHECK(passenger && passenger->mv.transportGuid == ship);
    if (passenger) CHECK(std::fabs(passenger->mv.tz + 17.9f) < 0.001f);
    printf("MOVEMENT-TRANSPORT-WIRE: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
