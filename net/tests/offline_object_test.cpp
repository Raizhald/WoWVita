// WoWVita: game objects in the offline world, through the real client session (GameSession).
//   offline_object_test pack.w3do   (a pack from tools/offline_data/export_world_sql.py)
//   3903 Milly Osworth -> 3904 Milly's Harvest: 8 Milly's Harvest (11119) from the crates (object 161557, a chest
//        whose loot is a quest item only), turned in to Milly Osworth (9296)
//   138  Captain Sander's Hidden Treasure: given by an object (35)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../client.hpp"
#include "../game.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_server.hpp"

namespace off = w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: offline_object_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    std::printf("pack: %zu quests\n", data.questsFull.size());
    for (uint32_t id : {3903u, 3904u, 138u}) CHECK(data.questFull(id) != nullptr, "quest %u in the pack", id);
    std::printf("pack: %zu objects at %zu places, %zu chest loot tables\n", data.goTemplates.size(), data.goSpawns.size(), data.goLoot.size());

    const char* savePath = "offline_object_test_save.w3d";
    remove(savePath);
    off::OfflineSession session;
    w3n::WorldClient wc;
    w3n::GameSession gs;
    uint32_t now = 10000;
    session.setup(&data, savePath);
    session.setParty(false);
    if (getenv("QTRACE")) session.trace = [](const std::string& l) { std::printf("    [trace] %s\n", l.c_str()); };
    CHECK(session.open(err), "open: %s", err.c_str());
    const uint64_t guid = session.characters()[0].guid;
    float pos[3] = {0, 0, 0};
    float o = 0;
    CHECK(session.enterWorld(guid, now, pos, &o), "enter world");
    wc.packetSink = [&](uint16_t op, const w3n::Bytes& b) { session.onClientPacket(op, b, now); };
    session.setSink([&](uint16_t op, const w3n::Bytes& b) { gs.handle(op, b, now); });
    wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
    gs.attach(wc, now);
    auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 16) { now += 16; session.update(now); gs.update(now); } };
    run(500);
    off::World& w = session.world();
    off::Entity* p = w.player();

    auto goTo = [&](uint32_t entry) -> off::Entity* {
        const off::Spawn* sp = nullptr;
        for (const off::Spawn& x : data.spawns) if (x.entry == entry) { sp = &x; break; }
        if (!sp) return nullptr;
        w.setPosition(*p, sp->x + 2.0f, sp->y, sp->z, 0);
        gs.mover.reset(p->x, p->y, p->z, p->o, now);
        run(1500);
        for (int i = 0; i < off::kMaxEntities; ++i) {
            off::Entity* e = w.byIndex(i);
            if (e && e->used && e->objType == 3 && e->entry == entry) return e;
        }
        return nullptr;
    };
    auto use = [&](off::Entity* npc) {
        float q[3] = {p->x, p->y, p->z};
        gs.questDialog = w3n::QuestDialog();
        gs.gossip = w3n::GossipMenu();
        std::string why = gs.interact(npc->guid, q, now);
        run(400);
        return why;
    };
    auto inLog = [&](uint32_t id) {
        for (const w3n::QuestLogEntry& e : gs.questLog()) if (e.id == id) return true;
        return false;
    };
    auto openFromMenu = [&](uint32_t id) {   // the giver's list or gossip -> the quest
        if (gs.questDialog.kind == w3n::QuestDialog::Kind::List)
            for (const auto& q : gs.questDialog.list) if (q.id == id) { gs.chooseQuest(gs.questDialog.npc, q, now); run(300); return; }
        if (gs.gossip.open)
            for (size_t i = 0; i < gs.gossip.quests.size(); ++i) if (gs.gossip.quests[i].id == id) { gs.selectGossipQuest(i, now); run(300); return; }
    };

    auto objectNear = [&](uint32_t entry) -> uint64_t {   // the client's object of that entry nearest to us
        uint64_t best = 0;
        float bd = 1e9f;
        for (auto& [g, ob] : gs.objects.all()) {
            if (ob.type != w3n::ObjType::GameObject || ob.entry() != entry || !ob.hasPos) continue;
            float q[3];
            ob.positionAt(now, q);
            const float d = std::hypot(q[0] - p->x, q[1] - p->y);
            if (d < bd) { bd = d; best = g; }
        }
        return best;
    };
    run(1200);   // (the passive spells - The Human Spirit - are on: the health is set after them)
    p->maxHp = 100000; p->hp = 100000;
    for (uint32_t id : {783u, 7u, 5261u, 33u}) session.testMarkRewarded(id);   // (Northshire's chain up to 3903)

    // ---- 3903 Milly Osworth: its giver, then Milly
    const off::QuestFull* q3903 = data.questFull(3903);
    off::Entity* giver = q3903 && !q3903->starters.empty() ? goTo(q3903->starters[0]) : nullptr;
    CHECK(giver != nullptr, "the giver of 3903");
    if (!giver) { std::printf("OFFLINE OBJECT: FAIL\n"); return 1; }
    p->level = 2;   // (3904 wants level 2)
    run(1500);
    use(giver);
    openFromMenu(3903);
    CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::Details && gs.questDialog.quest == 3903, "Milly Osworth offered");
    gs.acceptQuest(now);
    run(500);
    off::Entity* milly = goTo(9296);
    CHECK(milly != nullptr, "Milly Osworth");
    if (!milly) { std::printf("OFFLINE OBJECT: FAIL\n"); return 1; }
    run(1500);
    use(milly);
    openFromMenu(3903);
    if (gs.questDialog.kind == w3n::QuestDialog::Kind::RequestItems) { gs.continueQuest(now); run(300); }
    gs.completeQuest(0, now);
    run(500);
    if (gs.questDialog.kind != w3n::QuestDialog::Kind::Details) { use(milly); openFromMenu(3904); }
    CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::Details && gs.questDialog.quest == 3904, "Milly's Harvest offered");
    gs.acceptQuest(now);
    run(500);
    CHECK(inLog(3904), "Milly's Harvest in the log");

    // ---- the crates
    int opened = 0, visits = 0;
    std::vector<size_t> crates;
    for (size_t i = 0; i < data.goSpawns.size(); ++i) if (data.goSpawns[i].entry == 161557) crates.push_back(i);
    CHECK(crates.size() >= 8, "crates of Milly's Harvest on the map (%zu)", crates.size());
    bool sawFlag = false, sawName = false;
    for (size_t ci = 0; ci < crates.size() && w.countPlayerItem(11119) < 8 && visits < 40; ++ci, ++visits) {
        const off::GoSpawn& sp = data.goSpawns[crates[ci]];
        w.setPosition(*p, sp.x + 1.5f, sp.y, sp.z, 0);
        gs.mover.reset(p->x, p->y, p->z, p->o, now);
        run(1200);
        const uint64_t crate = objectNear(161557);
        if (!crate) continue;
        const w3n::WorldObject* ob = gs.objects.find(crate);
        sawFlag = sawFlag || (ob && (ob->field(w3n::UF::GAMEOBJECT_DYN_FLAGS) & 1));
        sawName = sawName || (ob && ob->name == "Milly's Harvest");
        const char* verb = ob ? gs.useVerb(*ob) : nullptr;
        if (!verb) continue;
        float q[3] = {p->x, p->y, p->z};
        gs.interact(crate, q, now);
        run(400);
        if (gs.lootWindow.open) {
            ++opened;
            gs.takeAllLoot(now);
            run(400);
            gs.closeLoot(now);
            run(300);
        }
    }
    std::printf("  %d crates opened for %u Milly's Harvest\n", opened, w.countPlayerItem(11119));
    CHECK(sawName, "the client knows the crate's name (object query)");
    CHECK(sawFlag, "the crates sparkle for the quest (GO_DYNFLAG_LO_ACTIVATE)");
    CHECK(w.countPlayerItem(11119) >= 8, "8 Milly's Harvest (%u)", w.countPlayerItem(11119));
    // a looted crate is gone (until its respawn)
    {
        const off::GoSpawn& sp = data.goSpawns[crates[0]];
        w.setPosition(*p, sp.x + 1.5f, sp.y, sp.z, 0);
        gs.mover.reset(p->x, p->y, p->z, p->o, now);
        run(1200);
        bool there = false;
        for (auto& [g, ob] : gs.objects.all())
            if (ob.type == w3n::ObjType::GameObject && ob.entry() == 161557 && (g & 0xFFFFFF) == crates[0] + 1) there = true;
        CHECK(!there, "an opened crate is gone");
    }
    milly = goTo(9296);
    if (milly) {
        run(1500);
        use(milly);
        openFromMenu(3904);
        if (gs.questDialog.kind == w3n::QuestDialog::Kind::RequestItems) { gs.continueQuest(now); run(300); }
        CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::OfferReward, "Milly takes the harvest");
        gs.completeQuest(0, now);
        run(500);
        CHECK(!inLog(3904), "Milly's Harvest turned in");
        CHECK(w.countPlayerItem(11119) == 0, "the harvest was handed over");
    }

    // ---- 138: a quest from an object
    {
        const off::QuestFull* q138 = data.questFull(138);
        const off::GoSpawn* sp = nullptr;
        for (const off::GoSpawn& x : data.goSpawns) if (q138 && !q138->goStarters.empty() && x.entry == q138->goStarters[0]) { sp = &x; break; }
        CHECK(sp != nullptr, "the object that gives 138");
        if (sp) {
            p->level = 12;
            w.setPosition(*p, sp->x + 1.5f, sp->y, sp->z, 0);
            gs.mover.reset(p->x, p->y, p->z, p->o, now);
            run(2500);
            const uint64_t ob = objectNear(sp->entry);
            CHECK(ob != 0, "it is on the client");
            CHECK(gs.questGiverStatus(ob) == w3n::QuestMark::Available, "it has a yellow ! (%u)", gs.questGiverStatus(ob));
            float q[3] = {p->x, p->y, p->z};
            gs.questDialog = w3n::QuestDialog();
            gs.interact(ob, q, now);
            run(400);
            openFromMenu(138);
            CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::Details && gs.questDialog.quest == 138, "its quest opens (kind %d)", (int)gs.questDialog.kind);
            gs.acceptQuest(now);
            run(500);
            CHECK(inLog(138), "accepted from the object");
        }
    }
    remove(savePath);
    std::printf("OFFLINE OBJECT: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
