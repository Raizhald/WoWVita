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
    p->maxHp = 200;   // (room for the meal on top of the regeneration: a level 1 character has 60)
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
    // ---- durability: the death of the character (10 %), a broken weapon, the repair
    {
        off::CharacterProfile& pr = w.playerProfile();
        uint32_t weapon = pr.equipment[15];
        if (!weapon || !w.maxDurability(weapon)) {   // (a mage has no weapon here: a Worn Shortsword, put on)
            w.buyItem(25, 1, 0, now, &why);
            run(300);
            int idx = -1;
            for (int i = 0; i < pr.bagCount; ++i) if (pr.bag[i].entry == 25) idx = i;
            w3n::Writer wr;
            wr.u8(255); wr.u8((uint8_t)(23 + idx));
            session.onClientPacket(0x10A, wr.b, now);   // CMSG_AUTOEQUIP_ITEM
            run(500);
        }
        weapon = pr.equipment[15];
        const uint16_t m = w.maxDurability(weapon);
        CHECK(m > 0, "the weapon has durability (%u)", m);
        const uint64_t wg = 0x40000000ull | 0x1000 + 15;
        auto dur = [&]() { const w3n::WorldObject* o = gs.objects.find(wg); return o ? o->field(w3n::UF::ITEM_FIELD_DURABILITY) : 9999u; };
        CHECK(dur() == m, "full at first (%u / %u)", dur(), m);
        // killed by a creature
        off::Entity* killer = nullptr;
        for (int i = 0; i < off::kMaxEntities && !killer; ++i) {
            off::Entity* e = w.byIndex(i);
            if (e && e->used && e->objType == 3 && e->alive() && !e->npcFlags) killer = e;
        }
        if (killer) {
            w.applyDamage(*killer, *p, 100000, 0, 0, now);
            run(500);
            CHECK(pr.equipLost[15] == std::max<uint16_t>(1, m / 10) && dur() == m - pr.equipLost[15], "death: -10 %% (%u / %u)", dur(), m);
            CHECK(gs.life() == w3n::Life::Dead, "dead");
            w.revive(*p, 100, now);   // (back at once: the test needs a living character)
            run(500);
        }
        const uint32_t dmgBefore = p->dmgMax;
        w.wearSlot(15, m);
        run(300);
        const uint32_t bare = 6 + (uint32_t)(p->attackPower * 2000 / 14000);   // (+ attack power, WoWVita)
        CHECK(dur() == 0 && p->dmgMax == bare && p->attackMs == 2000, "broken: the weapon counts no more, bare hands (%u -> %u)", dmgBefore, p->dmgMax);
        // a repairer near Northshire
        const off::Spawn* rep = nullptr;
        for (const off::Spawn& sp : data.spawns)
            if (const off::CreatureTemplate* t = data.creature(sp.entry); t && (t->npcFlags & 0x4000) &&
                (!rep || std::hypot(sp.x + 8913, sp.y + 136) < std::hypot(rep->x + 8913, rep->y + 136))) rep = &sp;
        CHECK(rep != nullptr, "a repairer");
        if (rep) {
            gs.mover.reset(rep->x + 2, rep->y, rep->z, 0, now);
            w.setPosition(*p, rep->x + 2, rep->y, rep->z, 0);
            run(1500);
            const off::Entity* re = nullptr;
            for (int i = 0; i < off::kMaxEntities; ++i) { const off::Entity* e = w.byIndex(i); if (e && e->used && e->entry == rep->entry) re = e; }
            w.addMoneyToPlayer(10000, now);
            run(300);
            const uint32_t money = p->money;
            w3n::Writer wr;
            wr.u64(re ? re->guid : 0); wr.u64(0);
            session.onClientPacket(0x2A8, wr.b, now);   // CMSG_REPAIR_ITEM: everything
            run(500);
            CHECK(pr.equipLost[15] == 0 && dur() == m && p->dmgMax == dmgBefore, "repaired (%u / %u)", dur(), m);
            CHECK(p->money < money, "for money (%u c)", money - p->money);
        }
    }
    // ---- bags: Small Brown Pouch (4496, 6 slots) into slot 19, the backpack grows to 22, food from the bag
    {
        off::CharacterProfile& pr = w.playerProfile();
        const off::ItemExt* pouch = data.ext(4496);
        CHECK(pouch && pouch->slots == 6, "Small Brown Pouch: 6 slots");
        w.buyItem(4496, 1, 0, now, &why);
        run(300);
        int idx = -1;
        for (int i = 0; i < pr.bagCount; ++i) if (pr.bag[i].entry == 4496) idx = i;
        w3n::Writer wr;
        wr.u8(255); wr.u8((uint8_t)(23 + idx));
        session.onClientPacket(0x10A, wr.b, now);   // CMSG_AUTOEQUIP_ITEM: the pouch goes into a bag slot
        run(500);
        CHECK(pr.bags[0] == 4496 && w.bagCapacity() == 22, "the pouch is worn: 22 slots (%u)", w.bagCapacity());
        const w3n::WorldObject* bagObj = gs.objects.find(0x40000000ull | (0x1000 + 19));
        CHECK(bagObj && bagObj->type == w3n::ObjType::Container && bagObj->field(w3n::UF::CONTAINER_FIELD_NUM_SLOTS) == 6,
              "the client sees the bag (a container with 6 slots)");
        while (pr.bagCount < 21) w.buyItem(25, 1, 0, now, &why);   // fill: past the backpack's 16
        w.buyItem(4540, 1, 0, now, &why);   // the 22nd: Tough Hunk of Bread (433) in the bag
        run(500);
        CHECK(pr.bagCount == 22, "22 items carried (%u)", pr.bagCount);
        CHECK(!w.buyItem(25, 1, 0, now, &why), "then full");
        w3n::InvItem inBag;
        int inBags = 0;
        for (const w3n::InvItem& i : gs.inventory()) {
            if (i.bag == 19) ++inBags;
            if (i.bag == 19 && i.entry == 4540) inBag = i;
        }
        CHECK(inBags == 6 && inBag.entry == 4540, "6 items in the bag, the bread among them (%d)", inBags);
        p->hp = 20;
        gs.useItem(inBag, now);
        run(500);
        CHECK(w.hasAura(*p, 433) && pr.bagCount == 21, "eaten from the bag");
    }
    session.leaveWorld();
    off::OfflineSaveData sd;
    std::string serr;
    CHECK(off::loadOffline(savePath, sd, serr), "load: %s", serr.c_str());
    CHECK(sd.roster[1].explored[100 / 32] & (1u << (100 % 32)), "the explored areas are saved");
    CHECK(sd.roster[1].bags[0] == 4496 && sd.roster[1].bagCount == 21, "the bag and its items are saved (%u)", sd.roster[1].bagCount);
    std::printf("OFFLINE ITEMS: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
