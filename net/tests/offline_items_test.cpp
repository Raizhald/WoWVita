// WoWVita: food, drink, potions and exploration in the offline world, through the real client session (GameSession).
//   offline_items_test pack.w3do   (a pack from tools/offline_data/export_world_sql.py)
//   - Tough Jerky (117, spell 433: 61 health over 18 s): the character sits, health comes every 3 s, not in combat;
//     moving stands it up and ends the meal; one Jerky of the stack is used up
//   - Refreshing Spring Water (159) for a mage: mana over 18 s
//   - Minor Healing Potion (118, spell 439: 70-90 at once), then 2 minutes before the next
//   - an exploration quest (areatrigger_involvedrelation) completes at its area trigger (CMSG_AREATRIGGER)
//   - exploration XP: a new area of level 5 gives exploration_basexp[5] once (SMSG_EXPLORATION_EXPERIENCE)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../client.hpp"
#include "../game.hpp"
#include "../offline/offline_data.hpp"
#include "../movement.hpp"
#include "../offline/offline_server.hpp"

namespace off = w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: offline_items_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    const off::ItemExt* jerky = data.ext(117);
    const off::ItemExt* water = data.ext(159);
    const off::ItemExt* potion = data.ext(118);
    CHECK(jerky && jerky->kind == 1 && jerky->hpMax == 61 && jerky->durationMs == 18000 && (jerky->flags & 1), "Tough Jerky: 61 health over 18 s");
    CHECK(water && water->kind == 2 && water->manaMax == 151, "Refreshing Spring Water: 151 mana");
    CHECK(potion && potion->kind == 4 && potion->hpMin == 70 && potion->hpMax == 90 && potion->cooldownMs == 120000, "Minor Healing Potion: 70-90, 2 min");
    size_t bags = 0;
    for (const auto& [e, x] : data.itemExt) bags += x.slots > 0;
    std::printf("pack: %zu items with use / durability / slots, %zu bags, %zu exploration levels, %zu quest triggers\n",
                data.itemExt.size(), bags, data.explorationXp.size(), data.questTriggers.size());
    if (!jerky || !water || !potion) return 1;

    const char* savePath = "offline_items_test_save.w3d";
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    off::OfflineSession session;
    w3n::WorldClient wc;
    w3n::GameSession gs;
    uint32_t now = 10000;
    session.setup(&data, savePath);
    session.setParty(false);
    CHECK(session.open(err), "open: %s", err.c_str());
    w3n::CharCreateInfo mage; mage.name = "Jaina"; mage.race = 1; mage.cls = 8;
    CHECK(session.createCharacter(mage) == w3n::CHAR_CREATE_SUCCESS, "a mage");
    const uint64_t guid = session.characters()[1].guid;
    float pos[3];
    float o = 0;
    CHECK(session.enterWorld(guid, now, pos, &o), "enter world");
    wc.packetSink = [&](uint16_t op, const w3n::Bytes& b) { session.onClientPacket(op, b, now); };
    session.setSink([&](uint16_t op, const w3n::Bytes& b) { gs.handle(op, b, now); });
    wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
    gs.attach(wc, now);
    auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 16) { now += 16; session.update(now); gs.update(now); } };
    run(800);
    off::World& w = session.world();
    off::Entity* p = w.player();
    int why = 0;
    w.buyItem(117, 3, 0, now, &why);
    w.buyItem(159, 2, 0, now, &why);
    w.buyItem(118, 2, 0, now, &why);
    run(800);
    auto item = [&](uint32_t entry) { for (const w3n::InvItem& i : gs.inventory()) if (i.entry == entry && !i.equipped()) return i; return w3n::InvItem(); };
    auto count = [&](uint32_t entry) { uint32_t n = 0; for (const w3n::InvItem& i : gs.inventory()) if (i.entry == entry && !i.equipped()) n += i.count; return n; };
    auto selfField = [&](uint16_t f) { const w3n::WorldObject* me = gs.objects.find(gs.objects.self()); return me ? me->field(f) : 0u; };
    CHECK(gs.itemInfo(117) && gs.itemInfo(117)->spellId[0] == 433, "the client knows Jerky's spell (it can use it)");

    // ---- Tough Jerky (the natural regeneration goes on: measured first, the meal must add its 61 to it)
    p->hp = 20;
    run(18300);
    const int32_t regen = p->hp - 20;
    p->hp = 20;
    gs.useItem(item(117), now);
    run(300);
    CHECK((selfField(w3n::UF::UNIT_BYTES_1) & 0xFF) == 1, "sitting");
    CHECK(w.hasAura(*p, 433), "the Food aura");
    CHECK(count(117) == 2, "one Jerky used up (%u left)", count(117));
    run(9000);
    CHECK(p->hp - 20 > regen / 2 + 20, "health comes in ticks (%d)", p->hp);
    run(9000);
    const int32_t meal = p->hp - 20 - regen;
    CHECK(meal >= 55 && meal <= 67, "61 health in 18 s besides the regeneration (+%d, regen %d)", meal, regen);
    CHECK((selfField(w3n::UF::UNIT_BYTES_1) & 0xFF) == 0 && !w.hasAura(*p, 433), "standing again at the end");
    // moving ends it
    p->hp = 20;
    gs.useItem(item(117), now);
    run(3200);
    const int32_t at = p->hp;
    gs.mover.reset(p->x + 3, p->y, p->z, 0, now);
    {
        w3n::MovementInfo m = gs.mover.info();
        w3n::Writer wr;
        w3n::writeMovementInfo(wr, m);
        session.onClientPacket(w3n::MoveOp::Heartbeat, wr.b, now);
    }
    run(6000);
    CHECK(p->hp - at <= regen / 3 + 4 && !w.hasAura(*p, 433), "moving stood it up (%d -> %d)", at, p->hp);

    // ---- water for the mage
    p->power = 10;
    gs.useItem(item(159), now);
    run(18500);
    CHECK(p->power == (int32_t)p->maxPower, "the drink filled the mana (%d / %u)", p->power, p->maxPower);

    // ---- in combat: no food
    off::Entity* foe = nullptr;
    for (int i = 0; i < off::kMaxEntities && !foe; ++i) {
        off::Entity* e = w.byIndex(i);
        if (e && e->used && e->objType == 3 && e->alive() && !e->npcFlags) foe = e;
    }
    if (foe) {
        foe->victim = p->guid;
        const uint32_t before = count(117);
        gs.useItem(item(117), now);
        run(300);
        CHECK(count(117) == before && !w.hasAura(*p, 433), "no food in combat");
        foe->victim = 0;
    }

    // ---- the potion: at once, then 2 minutes
    p->hp = 10;
    gs.useItem(item(118), now);
    run(300);
    CHECK(p->hp >= std::min<int32_t>(80, (int32_t)p->maxHp) && p->hp <= 100, "70-90 health at once (%d)", p->hp);
    p->hp = 10;
    gs.useItem(item(118), now);
    run(300);
    CHECK(p->hp <= 14 && count(118) == 1, "not again within 2 minutes");
    run(120000);
    gs.useItem(item(118), now);
    run(300);
    CHECK(p->hp > 10 && count(118) == 0, "after 2 minutes again");

    // ---- an exploration quest
    uint32_t trig = 0, qid = 0;
    for (const auto& [t, q] : data.questTriggers) if (data.questFull(q)) { trig = t; qid = q; break; }
    CHECK(qid != 0, "an exploration quest in the pack");
    if (qid) {
        off::OfflineSaveData& sd = const_cast<off::OfflineSaveData&>(session.saveData());
        off::QuestLogEntry le; le.id = qid;
        sd.quests.push_back(le);
        run(300);
        w3n::Writer wr;
        wr.u32(trig);
        session.onClientPacket(0x0B4, wr.b, now);   // CMSG_AREATRIGGER
        run(300);
        bool done = false;
        for (const off::QuestLogEntry& e : sd.quests) if (e.id == qid) done = e.complete;
        CHECK(done, "quest %u explored at trigger %u", qid, trig);
    }

    // ---- exploration XP
    {
        const uint32_t xp0 = p->levelXp, lv0 = p->level;
        session.reportArea(87, 5, 100);   // Goldshire, level 5 (a made-up bit)
        run(300);
        const uint32_t want = data.explorationXp.size() > 5 ? data.explorationXp[5] : 0;
        CHECK(want > 0 && (p->level > lv0 || p->levelXp - xp0 == want), "exploration XP %u (want %u)", p->levelXp - xp0, want);
        CHECK(gs.discovery.area == 87 && gs.discovery.xp == want, "the client says \"Discovered ...\"");
        const uint32_t xp1 = p->levelXp;
        session.reportArea(12, 1, 7);
        session.reportArea(87, 5, 100);   // again
        run(300);
        CHECK(p->levelXp - xp1 == data.explorationXp[1], "an area only once");
    }
    session.leaveWorld();
    off::OfflineSaveData sd;
    std::string serr;
    CHECK(off::loadOffline(savePath, sd, serr), "load: %s", serr.c_str());
    CHECK(sd.roster[1].explored[100 / 32] & (1u << (100 % 32)), "the explored areas are saved");
    std::printf("OFFLINE ITEMS: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
