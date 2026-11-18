// WoWVita: quests in the offline world, through the real client session (GameSession) - Northshire's first chain.
//   offline_quest_test pack.w3do   (a pack from tools/offline_data/export_world_sql.py)
//   783 A Threat Within (Deputy Willem 823 -> Marshal McBride 197): talk-to
//   7   Kobold Camp Cleanup (McBride): kill 10 Kobold Vermin (6)
//   33  Wolves Across the Border (Eagan Peltskinner 196): 8 Tough Wolf Meat (750), a quest drop of the wolves
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
    if (argc < 2) { std::printf("usage: offline_quest_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    std::printf("pack: %zu quests\n", data.questsFull.size());
    for (uint32_t id : {783u, 7u, 33u}) CHECK(data.questFull(id) != nullptr, "quest %u in the pack", id);

    const char* savePath = "offline_quest_test_save.w3d";
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

    // ---- 783 A Threat Within
    off::Entity* willem = goTo(823);
    CHECK(willem != nullptr, "Deputy Willem");
    if (!willem) { std::printf("OFFLINE QUEST: FAIL\n"); return 1; }
    run(1500);
    CHECK(gs.questGiverStatus(willem->guid) == w3n::QuestMark::Available, "Willem has a yellow ! (%u)", gs.questGiverStatus(willem->guid));
    use(willem);
    openFromMenu(783);
    CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::Details && gs.questDialog.quest == 783, "the details of A Threat Within");
    std::printf("  \"%s\": %.60s...\n", gs.questDialog.title.c_str(), gs.questDialog.text.c_str());
    gs.acceptQuest(now);
    run(500);
    CHECK(inLog(783), "accepted: in the log");

    off::Entity* mcbride = goTo(197);
    CHECK(mcbride != nullptr, "Marshal McBride");
    if (!mcbride) { std::printf("OFFLINE QUEST: FAIL\n"); return 1; }
    run(1500);
    CHECK(gs.questGiverStatus(mcbride->guid) == w3n::QuestMark::Reward2, "McBride has a yellow ? (%u)", gs.questGiverStatus(mcbride->guid));
    const uint32_t xp0 = p->levelXp + 0;
    use(mcbride);
    openFromMenu(783);
    if (gs.questDialog.kind == w3n::QuestDialog::Kind::RequestItems) { gs.continueQuest(now); run(300); }
    CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::OfferReward, "McBride offers the reward");
    gs.completeQuest(0, now);
    run(500);
    CHECK(!inLog(783), "turned in");
    CHECK(p->levelXp > xp0 || p->level > 1, "experience for it (%u -> %u)", xp0, p->levelXp);

    // ---- 7 Kobold Camp Cleanup: next in the chain, offered by McBride
    run(300);
    if (gs.questDialog.kind != w3n::QuestDialog::Kind::Details || gs.questDialog.quest != 7) { use(mcbride); openFromMenu(7); }
    CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::Details && gs.questDialog.quest == 7, "Kobold Camp Cleanup offered");
    gs.acceptQuest(now);
    run(500);
    CHECK(inLog(7), "Kobold Camp Cleanup in the log");
    for (int i = 0; i < 10; ++i) { w.onKillCredit(6, 0xF130000000000000ull | (6ull << 24) | (uint64_t)(100 + i)); run(50); }
    run(300);
    bool done7 = false;
    for (const w3n::QuestLogEntry& e : gs.questLog()) if (e.id == 7) { done7 = e.complete; CHECK(e.kills[0] == 10, "10 kobolds counted (%u)", e.kills[0]); }
    CHECK(done7, "Kobold Camp Cleanup complete");
    use(mcbride);
    openFromMenu(7);
    if (gs.questDialog.kind == w3n::QuestDialog::Kind::RequestItems) { gs.continueQuest(now); run(300); }
    CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::OfferReward, "Kobold Camp Cleanup: the reward");
    const size_t choices = gs.questDialog.choices.size();
    gs.completeQuest(0, now);
    run(500);
    CHECK(!inLog(7), "Kobold Camp Cleanup turned in");
    if (choices) {
        bool got = false;
        const uint32_t want = data.questFull(7)->choiceItem[0];
        for (const auto& it : gs.inventory()) got = got || it.entry == want;
        CHECK(got, "the chosen reward item (%u) is in the bags", want);
    }

    // ---- 5261 Eagan Peltskinner (Willem, after 783) -> Eagan, who offers 33 at once
    willem = goTo(823);
    run(1500);
    use(willem);
    openFromMenu(5261);
    CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::Details && gs.questDialog.quest == 5261, "Willem now offers Eagan Peltskinner");
    gs.acceptQuest(now);
    run(500);
    off::Entity* eagan = goTo(196);
    CHECK(eagan != nullptr, "Eagan Peltskinner");
    if (eagan) {
        run(1500);
        use(eagan);
        openFromMenu(5261);
        if (gs.questDialog.kind == w3n::QuestDialog::Kind::RequestItems) { gs.continueQuest(now); run(300); }
        CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::OfferReward && gs.questDialog.quest == 5261, "Eagan takes it");
        gs.completeQuest(0, now);
        run(500);
        // ---- 33 Wolves Across the Border: next in the chain, a quest drop
        if (gs.questDialog.kind != w3n::QuestDialog::Kind::Details) { use(eagan); openFromMenu(33); }
        CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::Details && gs.questDialog.quest == 33, "Wolves Across the Border offered");
        gs.acceptQuest(now);
        run(500);
        CHECK(inLog(33), "in the log");
        CHECK(w.questItemWanted && w.questItemWanted(750), "Tough Wolf Meat is wanted now");
        // the wolves drop it while the quest wants it: kill wolves until 8 are in the bags
        int kills = 0;
        while (w.countPlayerItem(750) < 8 && kills < 200) {
            off::Entity* wolf = nullptr;
            const off::Spawn* sp = nullptr;
            for (const off::Spawn& x : data.spawns) if (x.entry == 299) { sp = &x; break; }
            if (!sp) break;
            w.setPosition(*p, sp->x + 3.0f, sp->y, sp->z, 0);
            gs.mover.reset(p->x, p->y, p->z, p->o, now);
            run(800);
            for (int i = 0; i < off::kMaxEntities && !wolf; ++i) {
                off::Entity* e = w.byIndex(i);
                if (e && e->used && e->objType == 3 && e->entry == 299 && e->alive()) wolf = e;
            }
            if (!wolf) { run(30000); continue; }
            p->maxHp = 100000;   // (the test is about the drops, not the fights)
            p->hp = 100000;
            wolf->hp = 1;
            w.applyDamage(*p, *wolf, 100, 0, 0, now);
            ++kills;
            run(300);
            for (int s = 0; s < wolf->lootCount; ++s) if (wolf->loot[s].item == 750) w.addItemToPlayer(750, 1, now);
            wolf->looted = true;
            run(100);
        }
        std::printf("  %d wolves for 8 Tough Wolf Meat\n", kills);
        CHECK(w.countPlayerItem(750) >= 8, "8 Tough Wolf Meat (%u)", w.countPlayerItem(750));
        CHECK(!w.questItemWanted(750), "no more wolf meat drops once there are 8");
        p->hp = (int32_t)p->maxHp;
        off::Entity* eg = goTo(196);
        if (eg) {
            run(1500);
            CHECK(gs.questGiverStatus(eg->guid) == w3n::QuestMark::Reward2, "Eagan has a yellow ?");
            const std::string why = use(eg);
            if (!why.empty()) std::printf("  use Eagan: '%s' (hp %d, dead %d)\n", why.c_str(), p->hp, p->dead ? 1 : 0);
            openFromMenu(33);
            CHECK(gs.questDialog.kind == w3n::QuestDialog::Kind::RequestItems && gs.questDialog.completable,
                  "the meat is asked for, Continue enabled (kind %d, completable %d, quest %u, gossip %d)", (int)gs.questDialog.kind,
                  gs.questDialog.completable ? 1 : 0, gs.questDialog.quest, gs.gossip.open ? 1 : 0);
            gs.continueQuest(now);
            run(300);
            gs.completeQuest(0, now);
            run(500);
            CHECK(!inLog(33), "Wolves Across the Border turned in");
            CHECK(w.countPlayerItem(750) == 0, "the meat was handed over");
        }
    }

    // ---- the log and the history survive a restart
    const size_t rewarded = session.saveData().rewarded.size();
    run(3000);
    {
        off::OfflineSession again;
        again.setup(&data, savePath);
        std::string e2;
        CHECK(again.open(e2), "reopen");
        CHECK(again.saveData().rewarded.size() == rewarded && rewarded >= 4, "turned-in quests saved (%zu)", again.saveData().rewarded.size());
    }
    remove(savePath);
    std::printf("OFFLINE QUEST: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
