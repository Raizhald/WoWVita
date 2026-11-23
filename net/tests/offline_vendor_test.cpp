// WoWVita: vendors in the offline world (the "VEND" section of export_world_sql.py packs).
//   offline_vendor_test pack.w3do
// Northshire: walks to the nearest vendor, opens its list through the real client session (GameSession), buys,
// checks money and backpack, sells the item back.
#include <cmath>
#include <cstdio>
#include <string>

#include "../client.hpp"
#include "../game.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_server.hpp"

using namespace w3n;
using namespace w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: offline_vendor_test pack.w3do\n"); return 2; }
    WorldData data;
    std::string err;
    if (!loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    std::printf("pack: %zu vendors\n", data.vendors.size());
    CHECK(!data.vendors.empty(), "the pack has vendors");

    const char* savePath = "offline_vendor_test_save.w3d";
    remove(savePath);
    OfflineSession session;
    WorldClient wc;
    GameSession gs;
    uint32_t now = 10000;
    session.setup(&data, savePath);
    session.setParty(false);
    CHECK(session.open(err), "open: %s", err.c_str());
    const uint64_t guid = session.characters()[0].guid;
    float pos[3] = {0, 0, 0};
    float o = 0;
    CHECK(session.enterWorld(guid, now, pos, &o), "enter world");
    wc.packetSink = [&](uint16_t op, const Bytes& b) { session.onClientPacket(op, b, now); };
    session.setSink([&](uint16_t op, const Bytes& b) { gs.handle(op, b, now); });
    wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
    gs.attach(wc, now);
    auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 16) { now += 16; session.update(now); gs.update(now); } };
    run(500);

    World& w = session.world();
    Entity* p = w.player();
    CHECK(p != nullptr, "a player");
    CHECK(w.bot(0) == nullptr, "no companions (party off)");
    // the nearest vendor around the start
    Entity* vendor = nullptr;
    float best = 1e9f;
    for (int i = 0; i < kMaxEntities; ++i) {
        Entity* e = w.byIndex(i);
        if (!e || !e->used || e->objType != 3 || !(e->npcFlags & 4) || !data.vendors.count(e->entry)) continue;
        const float d = std::hypot(e->x - p->x, e->y - p->y);
        if (d < best) { best = d; vendor = e; }
    }
    CHECK(vendor != nullptr, "a vendor near the start");
    if (!vendor) { std::printf("OFFLINE VENDOR: %d checks, %d failed -> FAIL\n", checks, failed); return 1; }
    const CreatureTemplate* vt = data.creature(vendor->entry);
    std::printf("vendor: %s (%u), %.0f yd away, %zu goods\n", vt ? vt->name.c_str() : "?", vendor->entry, best,
                data.vendors[vendor->entry].size());

    // too far: no list
    gs.openVendor(vendor->guid, now);
    run(200);
    CHECK(!gs.vendor.open || best < 12.0f, "no list from afar");
    // next to it
    w.setPosition(*p, vendor->x + 2.0f, vendor->y, vendor->z, 0);
    gs.mover.reset(p->x, p->y, p->z, p->o, now);
    gs.openVendor(vendor->guid, now);
    run(300);
    CHECK(gs.vendor.open, "the vendor window opened");
    CHECK(gs.vendor.items.size() == data.vendors[vendor->entry].size(), "all its goods (%zu)", gs.vendor.items.size());
    if (gs.vendor.items.empty()) { std::printf("OFFLINE VENDOR: %d checks, %d failed -> FAIL\n", checks, failed); return 1; }

    // the cheapest thing that sells back
    size_t pick = 0;
    for (size_t i = 0; i < gs.vendor.items.size(); ++i) {
        const ItemTemplate* a = data.item(gs.vendor.items[i].entry);
        const ItemTemplate* b = data.item(gs.vendor.items[pick].entry);
        if (a && a->sellPrice && (!b || !b->sellPrice || a->buyPrice < b->buyPrice)) pick = i;
    }
    const VendorGoods& g0 = data.vendors[vendor->entry][pick];
    const ItemTemplate* it = data.item(g0.item);
    std::printf("buying: %s x%u for %u copper\n", it ? it->name.c_str() : "?", g0.buyCount, it ? it->buyPrice : 0);

    // no money: refused
    p->money = 0;
    {
        Writer w2; w2.u64(vendor->guid); w2.u32(g0.item); w2.u8(1); w2.u8(0);
        session.onClientPacket(0x1A2, w2.b, now);
    }
    run(200);
    int have = 0;
    for (const InvItem& x : gs.inventory()) if (x.entry == g0.item) have += (int)x.count;
    CHECK(have == 0, "nothing bought without money");

    // money: bought
    p->money = 100000;
    {
        Writer w2; w2.u64(vendor->guid); w2.u32(g0.item); w2.u8(1); w2.u8(0);
        session.onClientPacket(0x1A2, w2.b, now);
    }
    run(300);
    have = 0;
    const InvItem* bought = nullptr;
    std::vector<InvItem> inv = gs.inventory();
    for (const InvItem& x : inv) if (x.entry == g0.item) { have += (int)x.count; bought = &x; }
    CHECK(have == (int)g0.buyCount, "bought %d (want %u)", have, g0.buyCount);
    CHECK(gs.money() == 100000 - (it ? it->buyPrice : 0), "money after buying: %u", gs.money());

    // sell it back through the client
    if (bought && it && it->sellPrice) {
        const uint32_t before = gs.money();
        CHECK(gs.sellItem(*bought, now), "sell sent");
        run(300);
        have = 0;
        for (const InvItem& x : gs.inventory()) if (x.entry == g0.item) have += (int)x.count;
        CHECK(have == 0, "sold (left %d)", have);
        CHECK(gs.money() == before + it->sellPrice * g0.buyCount, "money after selling: %u (was %u)", gs.money(), before);
    }
    // buy a weapon the level-1 player can use and put it on
    {
        size_t wpick = (size_t)-1;
        for (size_t i = 0; i < gs.vendor.items.size(); ++i) {
            const ItemTemplate* a = data.item(gs.vendor.items[i].entry);
            if (a && (a->inventoryType == 13 || a->inventoryType == 21 || a->inventoryType == 17) && a->requiredLevel <= 1) { wpick = i; break; }
        }
        CHECK(wpick != (size_t)-1, "the vendor sells a weapon");
        if (wpick != (size_t)-1) {
            const uint32_t wentry = gs.vendor.items[wpick].entry;
            const ItemTemplate* wt = data.item(wentry);
            p->money = 100000;
            Writer w2; w2.u64(vendor->guid); w2.u32(wentry); w2.u8(1); w2.u8(0);
            session.onClientPacket(0x1A2, w2.b, now);
            run(300);
            const InvItem* inBag = nullptr;
            std::vector<InvItem> inv2 = gs.inventory();
            for (const InvItem& x : inv2) if (x.entry == wentry && !x.equipped()) inBag = &x;
            CHECK(inBag != nullptr, "the weapon is in the backpack");
            if (inBag) {
                const uint32_t oldMain = w.playerProfile().equipment[15];
                gs.equipItem(*inBag, now);
                run(300);
                CHECK(w.playerProfile().equipment[15] == wentry, "worn in the main hand (%u)", w.playerProfile().equipment[15]);
                const uint32_t ap = (uint32_t)(p->attackPower * (int32_t)p->attackMs / 14000);   // (+ attack power, WoWVita)
                CHECK(p->dmgMax == (uint32_t)std::max(1.0f, wt->dmgMax) + ap, "its damage (%u)", p->dmgMax);
                bool clientSees = false, oldBack = !oldMain;
                for (const InvItem& x : gs.inventory()) {
                    if (x.entry == wentry && x.equipped()) clientSees = true;
                    if (oldMain && x.entry == oldMain && !x.equipped()) oldBack = true;
                }
                CHECK(clientSees, "the client shows it worn");
                CHECK(oldBack, "the old weapon went to the backpack");
            }
        }
    }
    // the purchase survives a restart: the autosave comes 2 s after it (the Vita is often closed with PS)
    {
        const uint32_t wornMain = w.playerProfile().equipment[15];
        const uint32_t moneyNow = p->money;
        const float px = p->x, py = p->y;
        run(3000);
        OfflineSession again;
        again.setup(&data, savePath);
        again.setParty(false);
        std::string e2;
        CHECK(again.open(e2), "reopen: %s", e2.c_str());
        float pos2[3];
        float o2 = 0;
        CHECK(again.enterWorld(again.characters()[0].guid, now + 100, pos2, &o2), "enter again");
        const Entity* p2 = again.world().player();
        CHECK(again.world().playerProfile().equipment[15] == wornMain, "the worn weapon was saved (%u, want %u)",
              again.world().playerProfile().equipment[15], wornMain);
        CHECK(p2 && p2->money == moneyNow, "the money was saved (%u, want %u)", p2 ? p2->money : 0, moneyNow);
        CHECK(p2 && std::hypot(p2->x - px, p2->y - py) < 5.0f, "the place was saved");
    }

    // "Use" on vendors of every kind of NPC flags (vendor only, + quests, + gossip): the client's interact()
    for (uint32_t flags : {0x4004u, 0x6u, 0x5u, 0x87u}) {
        const Spawn* sp = nullptr;
        for (const Spawn& x : data.spawns) {
            const CreatureTemplate* t = data.creature(x.entry);
            if (t && t->npcFlags == flags && data.vendors.count(x.entry)) { sp = &x; break; }
        }
        CHECK(sp != nullptr, "a vendor with flags %x", flags);
        if (!sp) continue;
        gs.vendor = VendorWindow();
        w.setPosition(*p, sp->x + 2.0f, sp->y, sp->z, 0);
        gs.mover.reset(p->x, p->y, p->z, p->o, now);
        run(1500);   // the spawn grid brings it in, the client gets it
        Entity* v = nullptr;
        for (int i = 0; i < kMaxEntities; ++i) {
            Entity* e = w.byIndex(i);
            if (e && e->used && e->objType == 3 && e->entry == sp->entry && std::hypot(e->x - sp->x, e->y - sp->y) < 1) v = e;
        }
        CHECK(v != nullptr, "flags %x: the vendor is in the world", flags);
        if (!v) continue;
        float pos[3] = {p->x, p->y, p->z};
        const std::string msg = gs.interact(v->guid, pos, now);
        run(300);
        if ((flags & 0x80) && gs.gossip.open && !gs.vendor.open) {   // WoWVita: an innkeeper asks first (home / goods), as the original
            for (size_t i = 0; i < gs.gossip.options.size(); ++i)
                if (gs.gossip.options[i].text.find("goods") != std::string::npos) { gs.selectGossip(i, now); break; }
            run(300);
        }
        const CreatureTemplate* t = data.creature(v->entry);
        CHECK(gs.vendor.open && !gs.vendor.items.empty(), "flags %x (%s): Use opens the goods (msg '%s')", flags,
              t ? t->name.c_str() : "?", msg.c_str());
    }
    remove(savePath);
    std::printf("OFFLINE VENDOR: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
