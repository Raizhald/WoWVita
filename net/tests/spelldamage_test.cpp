// spelldamage_test — 7.21: the floating combat numbers the HUD draws (GameSession::hits) come from spell damage
// (SMSG_SPELLNONMELEEDAMAGELOG), damage-over-time ticks (SMSG_PERIODICAURALOG) and damage shields
// (SMSG_SPELLDAMAGESHIELD) too, not only from melee swings (SMSG_ATTACKERSTATEUPDATE).
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
    const uint64_t me = 0x77, wolf = 0xF130000045000101ull, other = 0xF130000045000102ull, other2 = 0xF130000045000103ull;
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
    create(other, 3, false, 8, {{UF::UNIT_HEALTH, 50}, {UF::UNIT_MAXHEALTH, 50}});
    create(other2, 3, false, 9, {{UF::UNIT_HEALTH, 50}, {UF::UNIT_MAXHEALTH, 50}});
    s.spellNames.add(133, "Fireball", "Rank 1");
    s.spellNames.add(703, "Garrote", "Rank 1");

    // SMSG_SPELLNONMELEEDAMAGELOG: packed victim, packed caster, spell, damage, school, absorbed, resisted, periodic,
    // unused, blocked, hit type flags, extended-present byte.
    auto nonMelee = [&](uint64_t victim, uint64_t caster, uint32_t spell, uint32_t dmg, bool crit, bool full = true,
                        uint32_t ms = 2000) {
        Writer w;
        w.packedGuid(victim);
        w.packedGuid(caster);
        w.u32(spell);
        w.u32(dmg);
        w.u8(3);        // fire
        if (!full) { s.handle(0x250, w.b, ms); return; }
        w.u32(0);       // absorbed
        w.u32(0);       // resisted
        w.u8(0);        // periodic
        w.u8(0);        // unused
        w.u32(0);       // blocked
        w.u32(crit ? 0x2u : 0u);
        w.u8(0);        // extended data absent
        s.handle(0x250, w.b, ms);
    };
    auto lastHit = [&]() -> const MeleeHit* { return s.hits.empty() ? nullptr : &s.hits.back(); };

    nonMelee(wolf, me, 133, 25, false);
    CHECK(lastHit() && lastHit()->attacker == me && lastHit()->victim == wolf && lastHit()->damage == 25 && lastHit()->spell == 133,
          "direct spell damage gives a floating number");

    nonMelee(wolf, me, 133, 60, true);
    CHECK(lastHit() && lastHit()->damage == 60 && lastHit()->crit(), "a critical spell hit is marked as one");

    nonMelee(me, wolf, 133, 17, false);
    CHECK(lastHit() && lastHit()->attacker == wolf && lastHit()->victim == me && lastHit()->damage == 17,
          "spell damage on us gives a number over our head");

    const size_t before = s.hits.size();
    nonMelee(other, other2, 133, 40, false);
    CHECK(s.hits.size() == before, "spell damage between two strangers adds no number");

    nonMelee(wolf, me, 133, 9, false, false);
    CHECK(lastHit() && lastHit()->attacker == me && lastHit()->damage == 9, "a short packet still gives its number");

    // A next-melee ability (Heroic Strike) sends the same blow twice: SMSG_ATTACKERSTATEUPDATE with its spell id and
    // SMSG_SPELLNONMELEEDAMAGELOG. The floating number must appear once, not twice.
    {
        Writer w;
        w.u32(0);                    // hitInfo
        w.packedGuid(me);
        w.packedGuid(wolf);
        w.u32(30);                   // total damage
        w.u8(1);                     // one school
        w.u32(0); w.f32(30); w.u32(30); w.u32(0); w.u32(0);   // school, float damage, damage, absorb, resist
        w.u32(1);                    // victimState: normal
        w.u32(1000);                 // attackerState
        w.u32(78);                   // meleeSpellId: Heroic Strike
        w.u32(0);                    // blocked
        s.handle(0x14A, w.b, 2400);
    }
    const size_t afterSwing = s.hits.size();
    CHECK(lastHit() && lastHit()->spell == 78 && lastHit()->damage == 30, "a next-melee swing keeps its spell id");
    nonMelee(wolf, me, 78, 30, false, true, 2500);
    CHECK(s.hits.size() == afterSwing, "the spell log of the same swing adds no second number");
    nonMelee(wolf, me, 133, 30, false, true, 2600);
    CHECK(s.hits.size() == afterSwing + 1, "another spell with the same damage still gets its number");

    // SMSG_PERIODICAURALOG: packed victim, packed caster, spell, count, aura type 3 (periodic damage), damage, school,
    // absorbed, resisted.
    auto periodic = [&](uint64_t victim, uint64_t caster, uint32_t spell, uint32_t dmg) {
        Writer w;
        w.packedGuid(victim);
        w.packedGuid(caster);
        w.u32(spell);
        w.u32(1);
        w.u32(3);
        w.u32(dmg);
        w.u32(3);
        w.u32(0);
        w.u32(0);
        s.handle(0x24E, w.b, 3000);
    };
    periodic(wolf, me, 703, 7);
    CHECK(lastHit() && lastHit()->attacker == me && lastHit()->victim == wolf && lastHit()->damage == 7 && lastHit()->spell == 703,
          "a damage-over-time tick gives a floating number");

    periodic(me, wolf, 703, 3);
    CHECK(lastHit() && lastHit()->attacker == wolf && lastHit()->victim == me && lastHit()->damage == 3,
          "a damage-over-time tick on us gives a number over our head");

    // SMSG_SPELLDAMAGESHIELD: the shield's owner is the attacker, the unit that struck is the victim (raw u64s).
    {
        Writer w;
        w.u64(me);
        w.u64(wolf);
        w.u32(5);
        w.u32(2);
        s.handle(0x24F, w.b, 4000);
    }
    CHECK(lastHit() && lastHit()->attacker == me && lastHit()->victim == wolf && lastHit()->damage == 5,
          "a damage shield gives a number over the unit that struck");

    bool chat = false;
    for (const ChatLine& c : s.chat) chat |= c.text.find("Your Fireball hits") != std::string::npos && c.text.find("for 9.") != std::string::npos;
    CHECK(chat, "the combat log line of a spell hit is still there");

    std::printf("%d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
