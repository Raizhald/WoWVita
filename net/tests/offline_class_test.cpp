// WoWVita: the class spells offline (net/offline/offline_spells.cpp, offline_class.cpp) through the real client session.
//   offline_class_test pack.w3do
//   - a new Human Warrior knows its race and class's spells (playercreateinfo_spell), stands in Battle Stance
//   - Heroic Strike: queued, the next swing, 15 rage paid; rage from hitting and being hit
//   - the Northshire warrior trainer: the list (green / red / grey), Rend learned for its cost at level 4, cast: it ticks
//   - a Gnome Mage: Fireball (1.5 s cast, mana, damage), Frost Nova (roots the enemies around), Arcane Intellect
//     (a buff in a buff slot), Conjure Water (an item); a stance / range / power refusal; the book saved
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
    if (argc < 2) { std::printf("usage: offline_class_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    std::printf("pack: %zu class spells, %zu trainers\n", data.spellx.size(), data.trainers.size());
    CHECK(data.spellx.size() > 1000 && data.trainers.size() > 50, "class spells and trainers in the pack");
    const char* savePath = "offline_class_test_save.w3d";
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    off::OfflineSession session;
    uint32_t now = 10000;
    session.setup(&data, savePath);
    session.setParty(false);
    CHECK(session.open(err), "open: %s", err.c_str());
    w3n::CharCreateInfo mage; mage.name = "Fizz"; mage.race = 7; mage.cls = 8;
    CHECK(session.createCharacter(mage) == w3n::CHAR_CREATE_SUCCESS, "a gnome mage");
    const uint64_t warriorGuid = session.characters()[0].guid, mageGuid = session.characters()[1].guid;
    w3n::WorldClient wc;
    w3n::GameSession gs;
    auto play = [&](uint64_t guid) {
        float pos[3];
        float o = 0;
        CHECK(session.enterWorld(guid, now, pos, &o), "enter world");
        wc.packetSink = [&](uint16_t op, const w3n::Bytes& b) { session.onClientPacket(op, b, now); };
        session.setSink([&](uint16_t op, const w3n::Bytes& b) { gs.handle(op, b, now); });
        wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
        gs.attach(wc, now);
    };
    std::vector<off::SimEvent> seen;
    auto run = [&](uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 16) {
            now += 16;
            session.update(now);
            gs.update(now);
        }
    };
    play(warriorGuid);
    run(800);
    off::World& w = session.world();
    off::Entity* p = w.player();
    std::printf("warrior level %d: %u health, str %d agi %d sta %d, attack power %d, swing %u-%u\n", p->level, p->maxHp,
                p->statStr, p->statAgi, p->statSta, p->attackPower, p->dmgMin, p->dmgMax);
    CHECK(data.playerStats.empty() || (p->maxHp > 40 && p->attackPower > 0), "health and attack power from the level's stats");
    p->maxHp = p->hp = 5000;
    const off::CharacterProfile& pr = w.playerProfile();
    auto inBook = [&](uint32_t s) { return std::find(pr.book.begin(), pr.book.end(), s) != pr.book.end(); };
    CHECK(inBook(78) && inBook(2457) && inBook(6603), "the warrior knows Heroic Strike, Battle Stance, Attack (%zu spells)", pr.book.size());
    CHECK(p->form == 17, "it stands in Battle Stance (form %d)", p->form);
    CHECK(gs.knowsSpell(78), "the client's book has Heroic Strike");

    // a hostile creature near Northshire Abbey, alone
    auto nearestHostile = [&](float x, float y, float maxD) -> off::Entity* {
        off::Entity* best = nullptr;
        float bd = maxD;
        for (int i = 0; i < off::kMaxEntities; ++i) {
            off::Entity* e = w.byIndex(i);
            if (!e || !e->used || e->objType != 3 || !e->alive() || !w.canAttack(*p, *e) || e->npcFlags) continue;
            const float d = std::hypot(e->x - x, e->y - y);
            if (d < bd) { bd = d; best = e; }
        }
        return best;
    };
    auto stand = [&](off::Entity* e, float d) {
        const float x = e->x + d, y = e->y;
        gs.mover.reset(x, y, e->z, 3.14159f, now);
        w.setPosition(*p, x, y, e->z, 3.14159f);
        p->o = 3.14159f;
    };
    off::Entity* mob = nearestHostile(p->x, p->y, 200);
    CHECK(mob != nullptr, "a creature to fight");
    if (mob) {
        mob->maxHp = mob->hp = 3000;
        stand(mob, 2.0f);
        run(300);
        gs.setTarget(mob->guid, now);
        p->power = 0;
        w.startAttack(*p, mob->guid, now);
        uint32_t t0 = now;
        while (now - t0 < 6000 && p->power < 20) run(100);
        CHECK(p->power >= 10, "rage from the fight (%d)", p->power);
        p->power = 40;
        const int32_t hpBefore = mob->hp;
        gs.castSpell(78, now);
        run(100);
        CHECK(p->nextSwingSpell == 78, "Heroic Strike waits for the next swing");
        t0 = now;
        while (now - t0 < 4000 && p->nextSwingSpell) run(50);
        CHECK(!p->nextSwingSpell && p->power <= 40 - 15 + 25, "the swing was Heroic Strike (rage %d)", p->power);
        CHECK(mob->hp < hpBefore, "and it hurt (%d -> %d)", hpBefore, mob->hp);
        w.stopAttack(*p, now);
        mob->hp = mob->maxHp; mob->victim = 0; mob->threatCount = 0;
    }

    // ---- the warrior trainer of Northshire
    {
        const off::Spawn* tsp = nullptr;
        for (const off::Spawn& s : data.spawns) {
            auto it = data.trainers.find(s.entry);
            if (it == data.trainers.end() || it->second.cls != 1) continue;
            if (!tsp || std::hypot(s.x + 8913, s.y + 136) < std::hypot(tsp->x + 8913, tsp->y + 136)) tsp = &s;
        }
        CHECK(tsp != nullptr, "a warrior trainer");
        if (tsp) {
            std::printf("trainer: %s\n", data.creature(tsp->entry)->name.c_str());
            gs.mover.reset(tsp->x + 2, tsp->y, tsp->z, 0, now);
            w.setPosition(*p, tsp->x + 2, tsp->y, tsp->z, 0);
            run(1500);
            off::Entity* tr = nullptr;
            for (int i = 0; i < off::kMaxEntities; ++i) {
                off::Entity* e = w.byIndex(i);
                if (e && e->used && e->entry == tsp->entry) tr = e;
            }
            CHECK(tr != nullptr, "the trainer is there");
            if (tr) {
                gs.openTrainer(tr->guid, now);
                run(400);
                CHECK(gs.trainer.open && !gs.trainer.spells.empty(), "the trainer's list (%zu)", gs.trainer.spells.size());
                int rend = -1, known = 0, red = 0;
                for (size_t i = 0; i < gs.trainer.spells.size(); ++i) {
                    const w3n::TrainerSpell& t = gs.trainer.spells[i];
                    known += t.state == 2;
                    red += t.state == 1;
                    for (const off::TrainerSpell& ts : data.trainers[tsp->entry].spells)
                        if (ts.teach == t.spell && ts.learn == 772) rend = (int)i;
                }
                std::printf("list: %zu spells, %d known, %d not yet\n", gs.trainer.spells.size(), known, red);
                CHECK(rend >= 0 && gs.trainer.spells[(size_t)rend].state == 1, "Rend: not yet at level 1");
                p->level = 4;
                w.addMoneyToPlayer(1000, now);
                run(300);
                gs.openTrainer(tr->guid, now);
                run(400);
                if (rend >= 0) {
                    CHECK(gs.trainer.spells[(size_t)rend].state == 0, "Rend: can learn at level 4");
                    const uint32_t money = p->money;
                    gs.trainSpell((size_t)rend, now);
                    run(400);
                    CHECK(inBook(772) && p->money == money - gs.trainer.spells[(size_t)rend].cost, "Rend learned (%u copper)", money - p->money);
                    CHECK(gs.knowsSpell(772), "the client knows it");
                }
            }
        }
    }
    // ---- Rend ticks
    if (mob && inBook(772)) {
        stand(mob, 2.0f);
        run(300);
        gs.setTarget(mob->guid, now);
        p->power = 50;
        const int32_t hp0 = mob->hp;
        gs.castSpell(772, now);
        run(300);
        bool aura = w.hasAura(*mob, 772);
        CHECK(aura, "Rend is on it");
        const int32_t hp1 = mob->hp;
        run(6500);
        CHECK(mob->hp < hp1 && hp1 <= hp0, "it bleeds (%d -> %d)", hp1, mob->hp);
        w.stopAttack(*p, now);
    }
    session.leaveWorld();

    // ---- the mage
    play(mageGuid);
    run(800);
    p = w.player();
    std::printf("mage level %d: %u health, %u mana, int %d spi %d\n", p->level, p->maxHp, p->maxPower, p->statInt, p->statSpi);
    CHECK(data.playerStats.empty() || p->maxPower > 100, "mana from intellect");
    p->maxHp = p->hp = 5000;
    p->level = 10;
    CHECK(p->powerType == 0 && p->maxPower > 0, "a mage with mana (%u)", p->maxPower);
    off::CharacterProfile& mp = w.playerProfile();
    for (uint32_t s : {116u, 122u, 1459u, 5504u, 100u}) if (std::find(mp.book.begin(), mp.book.end(), s) == mp.book.end()) mp.book.push_back(s);
    off::Entity* t = nearestHostile(p->x, p->y, 300);
    CHECK(t != nullptr, "something to burn");
    if (t) {
        t->maxHp = t->hp = 3000;
        stand(t, 20.0f);
        run(300);
        gs.setTarget(t->guid, now);
        p->power = (int32_t)p->maxPower;
        const int32_t mana = p->power, hp0 = t->hp;
        gs.castSpell(133, now);
        run(200);
        CHECK(p->castSpell == 133, "Fireball is being cast");
        run(1600);
        CHECK(!p->castSpell && t->hp < hp0 && p->power < mana, "Fireball: %d damage, %d mana", hp0 - t->hp, mana - p->power);
        // Frost Nova roots what is near
        stand(t, 3.0f);
        run(2000);
        w.tryCast(*p, 122, t->guid, now);   // (learned on the server only: the client's book does not have it)
        run(300);
        CHECK(t->rooted(now), "Frost Nova roots it");
        // Arcane Intellect on self: a buff slot
        gs.setTarget(0, now);
        run(1600);   // (the global cooldown of Frost Nova)
        p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 1459, p->guid, now);
        run(1700);
        bool buff = false;
        for (uint8_t i = 0; i < p->auraCount; ++i) if (p->auras[i].spell == 1459 && p->auras[i].slot >= 0 && p->auras[i].slot < 32) buff = true;
        CHECK(buff, "Arcane Intellect: a buff");
        // Conjure Water
        const uint32_t before = w.countPlayerItem(5350);
        p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 5504, p->guid, now);
        run(3500);
        CHECK(w.countPlayerItem(5350) > before, "Conjure Water makes water (%u)", w.countPlayerItem(5350));
        // refusals: no mana, a warrior's spell
        p->power = 0;
        gs.setTarget(t->guid, now);
        gs.castSpell(133, now);
        run(300);
        CHECK(!p->castSpell, "no Fireball without mana");
    }
    session.leaveWorld();
    off::OfflineSaveData sd;
    std::string serr;
    CHECK(off::loadOffline(savePath, sd, serr), "load: %s", serr.c_str());
    CHECK(std::find(sd.roster[0].book.begin(), sd.roster[0].book.end(), 772u) != sd.roster[0].book.end(), "the warrior's book saved (Rend)");
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
    std::printf("OFFLINE CLASS: %d checks, %d failed -> %s\n", checks, failed, failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
