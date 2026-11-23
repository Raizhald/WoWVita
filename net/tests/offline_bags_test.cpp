// WoWVita: worn items, bags and the bank's bags in the offline world, through the real client session (GameSession).
//   offline_bags_test pack.w3do
//   - a worn item comes off into the backpack (CMSG_AUTOSTORE_BAG_ITEM), an item is destroyed (CMSG_DESTROYITEM)
//   - a bag: worn, filled, taken off again (what was in it moves up), destroyed only if the rest still fits
//   - the bank: a bag slot bought (10 s, then 1 g), a bag put in it, the bank's 24 slots full -> into the bank's bag,
//     out again, the empty bag back; all of it saved
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../client.hpp"
#include "../game.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_save.hpp"
#include "../offline/offline_server.hpp"

namespace off = w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: offline_bags_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    const float swX = -8913.0f, swY = 554.0f;   // Stormwind
    const off::Spawn* banker = nullptr;
    for (const off::Spawn& s : data.spawns)
        if (const off::CreatureTemplate* t = data.creature(s.entry); t && (t->npcFlags & 0x100))
            if (!banker || std::hypot(s.x - swX, s.y - swY) < std::hypot(banker->x - swX, banker->y - swY)) banker = &s;
    uint32_t bagItem = 0, bagSlots = 0;
    for (const off::ItemTemplate& it : data.items)
        if (const off::ItemExt* x = data.ext(it.entry); x && it.inventoryType == 18 && x->slots >= 6 && x->slots <= 10 && it.cls == 1 && it.subclass == 0)
            if (!bagItem || it.entry < bagItem) { bagItem = it.entry; bagSlots = x->slots; }
    CHECK(banker && bagItem, "a banker and a bag in the pack");
    if (!banker || !bagItem) return 1;
    std::printf("bag: %s (%u slots)\n", data.item(bagItem)->name.c_str(), bagSlots);

    const char* savePath = "offline_bags_test_save.w3d";
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    off::OfflineSession session;
    uint32_t now = 10000;
    session.setup(&data, savePath);
    session.setParty(false);
    CHECK(session.open(err), "open: %s", err.c_str());
    const uint64_t guid = session.characters()[0].guid;
    w3n::WorldClient wc;
    w3n::GameSession gs;
    auto play = [&]() {
        float pos[3];
        float o = 0;
        CHECK(session.enterWorld(guid, now, pos, &o), "enter world");
        wc.packetSink = [&](uint16_t op, const w3n::Bytes& b) { session.onClientPacket(op, b, now); };
        session.setSink([&](uint16_t op, const w3n::Bytes& b) { gs.handle(op, b, now); });
        wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
        gs.attach(wc, now);
    };
    play();
    auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 16) { now += 16; session.update(now); gs.update(now); } };
    run(800);
    off::World& w = session.world();
    off::CharacterProfile& pr = w.playerProfile();
    auto worn = [&](int slot) -> const w3n::InvItem* {
        static w3n::InvItem it;
        for (const w3n::InvItem& i : gs.inventory()) if (i.equipped() && i.slot == slot) { it = i; return &it; }
        return nullptr;
    };
    auto carried = [&](uint32_t entry) {
        uint32_t n = 0;
        for (const w3n::InvItem& i : gs.inventory()) if (i.entry == entry && !i.equipped() && !i.isBagSlot()) n += i.count;
        return n;
    };
    auto itemOf = [&](uint32_t entry) {
        for (const w3n::InvItem& i : gs.inventory()) if (i.entry == entry && !i.equipped() && !i.isBagSlot()) return i;
        return w3n::InvItem();
    };
    int why = 0;

    // ---- a worn item off and back on; an item destroyed
    {
        int slot = -1;
        for (int s = 0; s < 19 && slot < 0; ++s) if (pr.equipment[(size_t)s]) slot = s;
        if (slot < 0) {   // (a pack without start outfits: something to wear first)
            w.buyItem(25, 1, 0, now, &why);
            run(300);
            gs.equipItem(itemOf(25), now);
            run(500);
            slot = 15;
        }
        const uint32_t entry = pr.equipment[(size_t)slot];
        CHECK(worn(slot) != nullptr, "the client shows the worn %s", data.item(entry)->name.c_str());
        gs.unequipItem(*worn(slot), now);
        run(500);
        CHECK(!pr.equipment[(size_t)slot] && !worn(slot) && carried(entry) == 1, "taken off into the backpack");
        gs.equipItem(itemOf(entry), now);
        run(500);
        CHECK(pr.equipment[(size_t)slot] == entry && worn(slot), "and on again");
        w.buyItem(750, 3, 0, now, &why);
        run(300);
        gs.destroyItem(itemOf(750), now);
        run(500);
        CHECK(carried(750) == 0, "destroyed");
    }

    // ---- a bag: on, filled past the backpack, off again
    {
        w.buyItem(bagItem, 1, 0, now, &why);
        run(300);
        gs.equipItem(itemOf(bagItem), now);
        run(500);
        CHECK(pr.bags[0] == bagItem && w.bagCapacity() == 16 + bagSlots, "the bag is worn (%u slots)", w.bagCapacity());
        const uint32_t fill = 16 - pr.bagCount + 2;   // two in the bag
        for (uint32_t k = 0; k < fill; ++k) w.buyItem(25, 1, 0, now, &why);   // (Worn Shortswords: one a slot)
        run(500);
        const uint8_t had = pr.bagCount;
        std::printf("items: %u (backpack 16 + %u in the bag)\n", had, had > 16 ? had - 16 : 0);
        const w3n::InvItem* bagSlot = nullptr;
        for (const w3n::InvItem& i : gs.inventory()) if (i.isBagSlot() && i.entry == bagItem) { static w3n::InvItem b; b = i; bagSlot = &b; }
        CHECK(bagSlot != nullptr, "the client shows the bag in its slot");
        if (bagSlot) {
            gs.unequipItem(*bagSlot, now);
            run(500);
            CHECK(pr.bags[0] == bagItem, "a full bag does not come off when the rest does not fit (%u items, room %u)", pr.bagCount, 16u);
            for (int k = 0; k < 6; ++k) { gs.destroyItem(itemOf(25), now); run(200); }
            gs.unequipItem(*bagSlot, now);
            run(500);
            CHECK(!pr.bags[0] && carried(bagItem) == 1, "with room for all of it, it comes off into the backpack");
            CHECK(pr.bagCount <= 16, "everything in the backpack (%u)", pr.bagCount);
        }
        while (carried(25)) { gs.destroyItem(itemOf(25), now); run(200); }
        if (!carried(bagItem)) w.buyItem(bagItem, 1, 0, now, &why);
        run(500);
    }

    // ---- the bank's bags
    {
        gs.mover.reset(banker->x + 2.0f, banker->y, banker->z, 0, now);
        w.setPosition(*w.player(), banker->x + 2.0f, banker->y, banker->z, 0);
        run(1500);
        const off::Entity* bk = nullptr;
        for (int i = 0; i < off::kMaxEntities; ++i) {
            const off::Entity* e = w.byIndex(i);
            if (e && e->used && e->entry == banker->entry && std::hypot(e->x - banker->x, e->y - banker->y) < 1) bk = e;
        }
        CHECK(bk != nullptr, "the banker");
        if (bk) {
            float q[3] = {w.player()->x, w.player()->y, w.player()->z};
            gs.interact(bk->guid, q, now);
            run(500);
            CHECK(gs.bank.open, "the bank opens");
            w.addMoneyToPlayer(1500, now);
            run(300);
            const uint32_t money = w.player()->money;
            gs.buyBankSlot(now);
            run(500);
            CHECK(gs.bankBagSlots() == 1 && w.player()->money == money - 1000, "a bank bag slot for 10 s (%d, %u copper paid)",
                  gs.bankBagSlots(), money - w.player()->money);
            gs.buyBankSlot(now);
            run(500);
            CHECK(gs.bankBagSlots() == 1, "the next costs 1 g: not with %u copper", w.player()->money);
            gs.depositItem(itemOf(bagItem), now);
            run(500);
            CHECK(pr.bankBags[0] == bagItem && carried(bagItem) == 0, "the bag goes into the bank's bag slot");
            for (int i = 0; i < 24; ++i) pr.bank[(size_t)i] = off::BagItem{2589, 20, (uint8_t)i, 0};   // full stacks: no room
            w.buyItem(750, 2, 0, now, &why);
            run(300);
            gs.depositItem(itemOf(750), now);
            run(500);
            uint32_t inBag = 0;
            for (const w3n::InvItem& i : gs.bankItems()) if (i.entry == 750 && i.bag == 63) inBag += i.count;
            CHECK(inBag == 2 && carried(750) == 0, "the 24 slots full: into the bank's bag (%u)", inBag);
            w3n::InvItem out;
            for (const w3n::InvItem& i : gs.bankItems()) if (i.entry == 750) out = i;
            gs.withdrawItem(out, now);
            run(500);
            CHECK(carried(750) == 2, "and out of it");
            w3n::InvItem bagOut;
            for (const w3n::InvItem& i : gs.bankItems()) if (i.entry == bagItem) bagOut = i;
            gs.withdrawItem(bagOut, now);
            run(500);
            CHECK(carried(bagItem) == 1 && !pr.bankBags[0], "the empty bag comes back");
            gs.depositItem(itemOf(bagItem), now);
            run(500);
            gs.depositItem(itemOf(750), now);
            run(500);
        }
    }
    session.leaveWorld();
    off::OfflineSaveData sd;
    std::string serr;
    CHECK(off::loadOffline(savePath, sd, serr), "load: %s", serr.c_str());
    uint32_t kept = 0;
    for (const off::BagItem& b : sd.roster[0].bankBagItems) if (b.entry == 750) kept += b.count;
    CHECK(sd.roster[0].bankBagSlots == 1 && sd.roster[0].bankBags[0] == bagItem && kept == 2, "saved: the slot, the bag and what is in it");
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    std::printf("OFFLINE BAGS: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
