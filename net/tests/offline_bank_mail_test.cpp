// WoWVita: the bank and the mailboxes in the offline world, through the real client session (GameSession).
//   offline_bank_mail_test pack.w3do   (a pack from tools/offline_data/export_world_sql.py)
//   - a Stormwind banker opens the bank; Linen Cloth goes in (stacks join), comes out, only near the banker; saved
//   - a mailbox: a letter with an item and money to another character of the save (postage 30 c); not to yourself, not to
//     the other faction, not to a name nobody has, no Hearthstone
//   - the other character: "You have new mail." at login, the letter, its text, the money and the item taken, deleted
#include <algorithm>
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
    if (argc < 2) { std::printf("usage: offline_bank_mail_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    const float swX = -8913.0f, swY = 554.0f;   // Stormwind
    const off::Spawn* banker = nullptr;
    size_t bankers = 0;
    for (const off::Spawn& s : data.spawns)
        if (const off::CreatureTemplate* t = data.creature(s.entry); t && (t->npcFlags & 0x100)) {
            ++bankers;
            if (!banker || std::hypot(s.x - swX, s.y - swY) < std::hypot(banker->x - swX, banker->y - swY)) banker = &s;
        }
    int box = -1;
    size_t boxes = 0;
    for (size_t i = 0; i < data.goSpawns.size(); ++i) {
        auto t = data.goTemplates.find(data.goSpawns[i].entry);
        if (t == data.goTemplates.end() || t->second.type != 19) continue;
        ++boxes;
        if (box < 0 || std::hypot(data.goSpawns[i].x - swX, data.goSpawns[i].y - swY) <
                           std::hypot(data.goSpawns[(size_t)box].x - swX, data.goSpawns[(size_t)box].y - swY)) box = (int)i;
    }
    std::printf("pack: %zu bankers, %zu mailboxes\n", bankers, boxes);
    CHECK(banker && bankers >= 5, "bankers in the pack");
    CHECK(box >= 0 && boxes >= 20, "mailboxes in the pack");
    if (!banker || box < 0) return 1;
    const off::GoSpawn& mb = data.goSpawns[(size_t)box];
    const uint64_t boxGuid = 0xF110000000000000ull | ((uint64_t)(mb.entry & 0xFFFFFF) << 24) | (uint64_t)(box + 1);

    const char* savePath = "offline_bank_mail_test_save.w3d";
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    off::OfflineSession session;
    uint32_t now = 10000;
    uint32_t wall = 1800000000u;
    session.wallClock = [&] { return wall; };
    session.setup(&data, savePath);
    session.setParty(false);
    CHECK(session.open(err), "open: %s", err.c_str());
    w3n::CharCreateInfo dwarf; dwarf.name = "Brann"; dwarf.race = 3; dwarf.cls = 1;
    w3n::CharCreateInfo orc; orc.name = "Grom"; orc.race = 2; orc.cls = 1;
    CHECK(session.createCharacter(dwarf) == w3n::CHAR_CREATE_SUCCESS, "a dwarf");
    CHECK(session.createCharacter(orc) == w3n::CHAR_CREATE_SUCCESS, "an orc");
    const uint64_t recruit = session.characters()[0].guid, brann = session.characters()[1].guid;

    // one client session per character
    auto play = [&](uint64_t guid, w3n::WorldClient& wc, w3n::GameSession& gs) {
        float pos[3];
        float o = 0;
        CHECK(session.enterWorld(guid, now, pos, &o), "enter world");
        wc.packetSink = [&](uint16_t op, const w3n::Bytes& b) { session.onClientPacket(op, b, now); };
        session.setSink([&](uint16_t op, const w3n::Bytes& b) { gs.handle(op, b, now); });
        wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
        gs.attach(wc, now);
    };
    w3n::WorldClient wc;
    w3n::GameSession gs;
    play(recruit, wc, gs);
    auto run = [&](w3n::GameSession& g, uint32_t ms) { for (uint32_t t = 0; t < ms; t += 16) { now += 16; session.update(now); g.update(now); } };
    run(gs, 800);
    off::World& w = session.world();
    off::Entity* p = w.player();
    w.addMoneyToPlayer(1000, now);   // (the client hears it: its own checks see the money)
    int why = 0;
    CHECK(w.buyItem(2589, 5, 0, now, &why) && w.buyItem(750, 2, 0, now, &why), "Linen Cloth and Tough Wolf Meat");
    run(gs, 300);
    auto put = [&](w3n::GameSession& g, float x, float y, float z) { g.mover.reset(x, y, z, 0, now); w.setPosition(*w.player(), x, y, z, 0); run(g, 1500); };
    auto invCount = [&](w3n::GameSession& g, uint32_t entry) {
        uint32_t n = 0;
        for (const w3n::InvItem& i : g.inventory()) if (i.entry == entry && !i.equipped()) n += i.count;
        return n;
    };
    auto bankCount = [&](w3n::GameSession& g, uint32_t entry, int* slots = nullptr) {
        uint32_t n = 0;
        int s = 0;
        for (const w3n::InvItem& i : g.bankItems()) if (i.entry == entry) { n += i.count; ++s; }
        if (slots) *slots = s;
        return n;
    };
    auto invItem = [&](w3n::GameSession& g, uint32_t entry) {
        for (const w3n::InvItem& i : g.inventory()) if (i.entry == entry && !i.equipped()) return i;
        return w3n::InvItem();
    };

    // ---- the bank
    put(gs, banker->x + 2.0f, banker->y, banker->z);
    const off::Entity* bk = nullptr;
    for (int i = 0; i < off::kMaxEntities; ++i) {
        const off::Entity* e = w.byIndex(i);
        if (e && e->used && e->entry == banker->entry && std::hypot(e->x - banker->x, e->y - banker->y) < 1) bk = e;
    }
    CHECK(bk != nullptr, "the banker is there");
    if (bk) {
        float q[3] = {p->x, p->y, p->z};
        gs.interact(bk->guid, q, now);
        run(gs, 500);
        CHECK(gs.bank.open, "the bank opens");
        gs.depositItem(invItem(gs, 2589), now);
        run(gs, 500);
        CHECK(bankCount(gs, 2589) == 5 && invCount(gs, 2589) == 0, "5 Linen Cloth in the bank (bank %u, bags %u)", bankCount(gs, 2589), invCount(gs, 2589));
        w.buyItem(2589, 3, 0, now, &why);
        run(gs, 300);
        gs.depositItem(invItem(gs, 2589), now);
        run(gs, 500);
        int slots = 0;
        CHECK(bankCount(gs, 2589, &slots) == 8 && slots == 1, "the stacks join (%u in %d slots)", bankCount(gs, 2589), slots);
        w3n::InvItem bi;
        for (const w3n::InvItem& i : gs.bankItems()) if (i.entry == 2589) bi = i;
        gs.withdrawItem(bi, now);
        run(gs, 500);
        CHECK(invCount(gs, 2589) == 8 && bankCount(gs, 2589) == 0, "out again into the bags");
        gs.depositItem(invItem(gs, 2589), now);
        run(gs, 500);
        CHECK(bankCount(gs, 2589) == 8, "back in the bank");
        put(gs, banker->x + 60.0f, banker->y, banker->z);
        gs.depositItem(invItem(gs, 750), now);
        run(gs, 500);
        CHECK(bankCount(gs, 750) == 0 && invCount(gs, 750) == 2, "no bank far from the banker");
    }

    // ---- mail
    put(gs, mb.x + 2.0f, mb.y, mb.z);
    gs.openMailbox(boxGuid, now);
    run(gs, 500);
    CHECK(gs.mail.listed && gs.mail.mails.empty(), "an empty mailbox");
    const uint32_t m0 = p->money;
    auto lastErr = [&]() { return gs.chat.empty() ? std::string() : gs.chat.back().text; };
    gs.sendMail("Recruit", "hi", "", 0, 0, now); run(gs, 300);
    CHECK(p->money == m0 && lastErr().find("yourself") != std::string::npos, "not to yourself (%s)", lastErr().c_str());
    gs.sendMail("Grom", "hi", "", 0, 0, now); run(gs, 300);
    CHECK(p->money == m0 && lastErr().find("other faction") != std::string::npos, "not to the Horde (%s)", lastErr().c_str());
    gs.sendMail("Nobody", "hi", "", 0, 0, now); run(gs, 300);
    CHECK(p->money == m0 && lastErr().find("not found") != std::string::npos, "not to nobody (%s)", lastErr().c_str());
    gs.sendMail("Brann", "home", "", invItem(gs, off::kHearthstoneItem).guid, 0, now); run(gs, 300);
    CHECK(p->money == m0 && invCount(gs, off::kHearthstoneItem) == 1, "no Hearthstone by mail");
    gs.sendMail("brann", "Wolf meat", "For the road, cousin.", invItem(gs, 750).guid, 100, now);
    run(gs, 500);
    CHECK(p->money == m0 - 130, "100 c and 30 c postage (%u; %s)", m0 - p->money, lastErr().c_str());
    CHECK(invCount(gs, 750) == 0, "the meat left the bags");
    std::string serr;
    session.leaveWorld();

    // ---- Brann's mailbox
    w3n::WorldClient wc2;
    w3n::GameSession gs2;
    play(brann, wc2, gs2);
    run(gs2, 800);
    CHECK(gs2.newMail, "Brann hears \"You have new mail.\"");
    off::Entity* b = session.world().player();
    const uint32_t bm = b->money;
    gs2.mover.reset(mb.x + 2.0f, mb.y, mb.z, 0, now);
    session.world().setPosition(*b, mb.x + 2.0f, mb.y, mb.z, 0);
    run(gs2, 1500);
    gs2.openMailbox(boxGuid, now);
    run(gs2, 500);
    CHECK(gs2.mail.mails.size() == 1, "one letter (%zu)", gs2.mail.mails.size());
    if (!gs2.mail.mails.empty()) {
        const w3n::MailEntry m = gs2.mail.mails[0];
        CHECK(m.sender == recruit && m.subject == "Wolf meat" && m.item == 750 && m.itemCount == 2 && m.money == 100,
              "from Recruit: Wolf meat, 2 x 750, 100 c");
        CHECK(m.expireDays > 29.0f && m.expireDays <= 30.0f, "30 days (%.1f)", m.expireDays);
        gs2.mailText(m.textId, m.id, now);
        run(gs2, 300);
        const std::string* text = gs2.mailText(m.textId, m.id, now);
        CHECK(text && *text == "For the road, cousin.", "the letter's text");
        gs2.markMailRead(m.id, now);
        gs2.takeMailMoney(m.id, now);
        run(gs2, 300);
        CHECK(b->money == bm + 100, "the money taken");
        gs2.takeMailItem(m.id, now);
        run(gs2, 300);
        CHECK(invCount(gs2, 750) == 2, "the meat taken");
        gs2.deleteMail(m.id, now);
        run(gs2, 300);
        CHECK(gs2.mail.mails.empty(), "deleted");
    }
    session.leaveWorld();
    off::OfflineSaveData sd;
    CHECK(off::loadOffline(savePath, sd, serr), "load: %s", serr.c_str());
    uint32_t linen = 0;
    for (const off::BagItem& i : sd.roster[0].bank) if (i.entry == 2589) linen += i.count;
    CHECK(linen == 8, "Recruit's bank saved (%u Linen Cloth)", linen);
    CHECK(sd.mails.empty(), "no letters left");

    // ---- a letter left 30 days goes back with its item
    {
        play(recruit, wc, gs);
        run(gs, 500);
        off::World& w2 = session.world();
        w2.setPosition(*w2.player(), mb.x + 2.0f, mb.y, mb.z, 0);
        gs.mover.reset(mb.x + 2.0f, mb.y, mb.z, 0, now);
        run(gs, 1500);
        gs.openMailbox(boxGuid, now);
        run(gs, 300);
        w2.buyItem(750, 1, 0, now, &why);
        run(gs, 300);
        gs.sendMail("Brann", "old", "", invItem(gs, 750).guid, 0, now);
        run(gs, 300);
        wall += 31 * 86400;
        gs.openMailbox(boxGuid, now);
        run(gs, 300);
        CHECK(gs.mail.mails.size() == 1 && gs.mail.mails[0].item == 750, "after 30 days the letter is back with its item");
        session.leaveWorld();
    }

    std::printf("OFFLINE BANK MAIL: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
