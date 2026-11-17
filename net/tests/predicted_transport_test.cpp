// 7.28: a transport the client puts on its path by itself (ObjectTable::addPredictedTransport) - the phase comes from the
// previous session, the pose from transports.txt, and the server's own create replaces it the moment it arrives.
#include "objects.hpp"
#include "wire.hpp"
#include <cmath>
#include <cstdio>
using namespace w3n;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

static void fields(Writer& w, std::initializer_list<std::pair<int, uint32_t>> f) {
    uint32_t mask[4] = {0, 0, 0, 0};
    int top = 0;
    for (auto& p : f) { mask[p.first / 32] |= 1u << (p.first % 32); top = std::max(top, p.first / 32 + 1); }
    w.u8((uint8_t)top);
    for (int i = 0; i < top; ++i) w.u32(mask[i]);
    for (int b = 0; b < top; ++b)
        for (int bit = 0; bit < 32; ++bit)
            for (auto& p : f) if (p.first == b * 32 + bit) w.u32(p.second);
}

int main() {
    ObjectTable ot;
    ot.setSelf(1);
    std::string txt;
    int idx = 0;
    auto node = [&](float x, uint32_t fl, uint32_t delay) {
        char b[128];
        std::snprintf(b, sizeof b, "T\t9\t%d\t0\t%.1f\t0.0\t5.0\t%u\t%u\n", idx++, x, fl, delay);
        txt += b;
    };
    node(-20, 0, 0);
    node(0, 2, 10);
    for (int x = 50; x < 600; x += 50) node((float)x, 0, 0);
    node(600, 2, 10);
    for (int x = 550; x > 0; x -= 50) node((float)x, 0, 0);
    node(-5, 0, 0);
    CHECK(ot.transports.parse(txt));
    ObjectTable::GoInfo gi;
    gi.type = 15; gi.data[0] = 9; gi.data[1] = 14; gi.data[2] = 1;
    ot.setGoInfo(500, gi);

    // The server has sent nothing for entry 500 yet: put it on the path at the first stop (progress 0).
    const uint64_t guess = ot.addPredictedTransport(500, 3015, 0, 5000);
    CHECK(guess != 0);
    CHECK(ot.addPredictedTransport(500, 3015, 0, 5000) == 0);   // never two of them
    ot.tickTransports(5000);
    const TransportPose* p0 = ot.transportPose(guess);
    CHECK(p0 != nullptr);
    if (p0) {
        CHECK(std::fabs(p0->x - 0.0f) < 1.0f);   // waiting at the stop
        CHECK(!p0->moving);
        CHECK(p0->map == 0);
    }
    // 15 s later the same progress has it under way
    ot.tickTransports(20000);
    const TransportPose* p1 = ot.transportPose(guess);
    CHECK(p1 != nullptr);
    if (p1) { CHECK(p1->x > 1.0f); CHECK(p1->moving); }
    // our own guess is never written to the memory (it is not the server's word)
    CHECK(ot.transportMemory(20000).empty());

    // Now the server's own create block for the same transport: the guess goes, the real object stays.
    {
        Writer w;
        w.u32(1); w.u8(0);
        w.u8(3);   // CREATE2
        w.packedGuid(0x1F10000000000001ull); w.u8(5);
        w.u8(0x40 | 0x02);   // HAS_POSITION | TRANSPORT
        w.f32(0); w.f32(0); w.f32(5); w.f32(0);
        w.u32(30000);
        fields(w, {{3, 500}, {8, 3015}});
        CHECK(ot.parseUpdate(w.b.data(), w.b.size(), 21000));
    }
    CHECK(ot.find(guess) == nullptr);                       // dropped
    CHECK(ot.hasTransportEntry(500));
    CHECK(ot.find(0x1F10000000000001ull) != nullptr);
    const auto mem = ot.transportMemory(21000);
    CHECK(mem.size() == 1);
    if (mem.size() == 1) { CHECK(mem[0].entry == 500); CHECK(mem[0].display == 3015); CHECK(mem[0].progress == 30000); }
    CHECK(ot.addPredictedTransport(500, 3015, 0, 21000) == 0);   // it is known: no guess

    // dropPredictedTransports(0) removes everything guessed and nothing else
    ot.dropPredictedTransports(0);
    CHECK(ot.find(0x1F10000000000001ull) != nullptr);

    if (!fails) printf("predicted_transport: all checks passed\n");
    return fails ? 1 : 0;
}
