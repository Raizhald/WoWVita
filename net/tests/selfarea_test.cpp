// selfarea_test — 7.21: a harmful spell refused "invalid target" at a selected enemy is retried once with no target,
// for an old spells.txt that does not mark the areas centred on the caster (Thunder Clap, Frost Nova, Arcane Explosion)
// with "@self". A ranged spell or a helpful one is not retried that way.
#include <cstdio>
#include <map>
#include <vector>

#include "../game.hpp"

using namespace w3n;

static int g_fail = 0, g_ok = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main() {
    GameSession s;
    std::vector<std::pair<uint16_t, Bytes>> out;
    s.tap = [&](uint32_t, int dir, uint16_t op, const Bytes& d) { if (dir == 1) out.push_back({op, d}); };
    const uint64_t me = 0x77, wolf = 0xF130000045000101ull;
    auto values = [&](Writer& w, const std::map<uint16_t, uint32_t>& f) {
        uint16_t top = 0;
        for (auto& kv : f) top = std::max<uint16_t>(top, kv.first);
        const uint8_t blocks = (uint8_t)(top / 32 + 1);
        w.u8(blocks);
        std::vector<uint32_t> mask(blocks, 0);
        for (auto& kv : f) mask[kv.first / 32] |= 1u << (kv.first % 32);
        for (uint32_t m : mask) w.u32(m);
        for (auto& kv : f) w.u32(kv.second);
    };
    auto create = [&](uint64_t guid, uint8_t type, bool self, float x, std::map<uint16_t, uint32_t> f) {
        Writer w;
        w.u32(1); w.u8(0); w.u8(2); w.packedGuid(guid); w.u8(type);
        w.u8(self ? 0x71 : 0x70); w.u32(0); w.u32(0); w.f32(x); w.f32(0); w.f32(0); w.f32(0); w.f32(0);
        for (int k = 0; k < 6; ++k) w.f32(7);
        w.u32(1);
        f[0] = (uint32_t)guid; f[1] = (uint32_t)(guid >> 32); f[2] = type == 4 ? 0x19u : 0x9u;
        values(w, f);
        s.handle(0x0A9, w.b, 1);
    };
    create(me, 4, true, 0, {{UF::UNIT_HEALTH, 100}, {UF::UNIT_MAXHEALTH, 100}});
    s.objects.setSelf(me);
    create(wolf, 3, false, 4, {{UF::UNIT_HEALTH, 50}, {UF::UNIT_MAXHEALTH, 50}});
    s.spellNames.addFriendly(635);   // spells.txt with @friend data: the old helpful fallback stays out of the way

    const uint16_t CAST = 0x12E, FAILED = 0x130;
    auto casts = [&]() { size_t n = 0; for (auto& p : out) n += p.first == CAST; return n; };
    auto lastCast = [&]() -> const Bytes* {
        for (auto it = out.rbegin(); it != out.rend(); ++it) if (it->first == CAST) return &it->second;
        return nullptr;
    };
    auto lastTarget = [&]() -> uint64_t {   // 0 = no target flags at all
        const Bytes* b = lastCast();
        if (!b || b->size() < 7) return 0;
        if (!((*b)[4] | (*b)[5])) return 0;
        uint8_t mask = (*b)[6]; uint64_t g = 0; size_t p = 7;
        for (int k = 0; k < 8; ++k) if (mask & (1 << k)) g |= (uint64_t)(*b)[p++] << (8 * k);
        return g;
    };
    auto fail = [&](uint32_t spell, uint32_t ms) {
        Writer w;
        w.u32(spell);
        w.u8(2);    // failed
        w.u8(10);   // SPELL_FAILED_BAD_TARGETS ("invalid target")
        s.handle(FAILED, w.b, ms);
    };

    // Thunder Clap at an enemy: the first cast carries the enemy, the refusal is retried with no target, once.
    s.setTarget(wolf, 1000);
    out.clear();
    CHECK(s.castSpell(6343, 1000), "Thunder Clap is sent");
    CHECK(casts() == 1 && lastTarget() == wolf, "the first cast carries the selected enemy");
    fail(6343, 1100);
    CHECK(casts() == 2 && lastTarget() == 0, "after 'invalid target' it is retried with no target");
    fail(6343, 1200);
    CHECK(casts() == 2, "a second refusal does not retry again");

    // A ranged spell (with @range data) is a genuinely wrong target: no second cast.
    s.spellNames.addRange(133, 0, 30);
    s.setTarget(wolf, 2000);
    out.clear();
    CHECK(s.castSpell(133, 2000), "Fireball is sent");
    fail(133, 2100);
    CHECK(casts() == 1, "a ranged spell refused 'invalid target' is not retried without a target");

    // A helpful spell (with @friend data) is not retried without a target either.
    s.setTarget(wolf, 3000);
    out.clear();
    CHECK(s.castSpell(635, 3000), "Holy Light is sent");
    fail(635, 3100);
    CHECK(casts() == 1, "a helpful spell is not retried without a target");

    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
