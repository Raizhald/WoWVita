// WoWVita: the class spells offline (net/offline/offline_spells.cpp, offline_class.cpp) through the real client session.
//   offline_class_test pack.w3do
//   - a new Human Warrior knows its race and class's spells (playercreateinfo_spell), stands in Battle Stance
//   - Heroic Strike: queued, the next swing, 15 rage paid; rage from hitting and being hit
//   - the Northshire warrior trainer: the list (green / red / grey), Rend learned for its cost at level 4, cast: it ticks
//   - a Gnome Mage: Fireball (1.5 s cast, mana, damage), Frost Nova (roots the enemies around), Arcane Intellect
//     (a buff in a buff slot), Conjure Water (an item); a stance / range / power refusal; the book saved
//   - Execute, Polymorph broken by damage, Mana Shield, Blink, Frost Armor's proc
//   - a Human Rogue: Stealth (CMaNGOS detection range), Backstab only from behind, combo points (on the client too),
//     Slice and Dice longer with more points, Eviscerate, no Stealth in combat, Vanish
//   - a Dwarf Priest: spells outside Shadowform, Power Word: Shield + Weakened Soul, Inner Fire's charges
//   - a Human Paladin: Seal of Righteousness + Judgement, Divine Protection + Forbearance (no Blessing of Protection then)
//   - a Night Elf Druid: Bear Form / Cat Form (model, power, swing, armour; on the client too), Maul, no Moonfire as a bear,
//     the mana back after; a learning reward spell
//   - a Human Warlock: Summon Imp (pet_levelstats, name, the pet bar), follow, Firebolt at our target and our kill,
//     stay, the Imp back at login, dismissed; The Binding: the Summoning Circle's focus, the Summoned Voidwalker
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
    w3n::CharCreateInfo rogue; rogue.name = "Shade"; rogue.race = 1; rogue.cls = 4;
    CHECK(session.createCharacter(rogue) == w3n::CHAR_CREATE_SUCCESS, "a human rogue");
    const uint64_t warriorGuid = session.characters()[0].guid, mageGuid = session.characters()[1].guid;
    w3n::CharCreateInfo priest; priest.name = "Brann"; priest.race = 3; priest.cls = 5;
    CHECK(session.createCharacter(priest) == w3n::CHAR_CREATE_SUCCESS, "a dwarf priest");
    const uint64_t rogueGuid = session.characters()[2].guid, priestGuid = session.characters()[3].guid;
    uint64_t paladinGuid = 0;   // (made when the mage has gone: four characters a save)
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
        // Execute: 125 + 3 a rage point left (less the armour), all the rage gone
        w.playerProfile().book.push_back(5308);
        mob->hp = mob->maxHp / 10;
        p->power = 45;
        p->gcdEndMs = 0;
        const int32_t eh = mob->hp;
        w.tryCast(*p, 5308, mob->guid, now);
        run(200);
        std::printf("execute: %d damage, rage %d\n", eh - mob->hp, p->power);
        CHECK(eh - mob->hp >= 150 && p->power == 0, "Execute: %d damage, rage left %d", eh - mob->hp, p->power);
        // Shield Block: a charge used by a blocked hit; Evasion-like dodge from an aura
        mob->hp = mob->maxHp;
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
        // Polymorph: a sheep that wanders; damage breaks it
        p->power = (int32_t)p->maxPower;
        t->hp = t->maxHp; t->rootUntilMs = 0;
        while (t->auraCount) w.removeAuraSpell(*t, t->auras[0].spell);
        p->gcdEndMs = 0;
        w.tryCast(*p, 118, t->guid, now);
        run(1800);
        CHECK(w.hasAura(*t, 118), "Polymorph is on it");
        p->gcdEndMs = 0;
        p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 133, t->guid, now);   // Fireball breaks it
        run(4000);
        CHECK(!w.hasAura(*t, 118), "damage broke the Polymorph");
        // Mana Shield: hits cost mana instead of health
        p->gcdEndMs = 0;
        p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 1463, p->guid, now);
        run(300);
        CHECK(w.hasAura(*p, 1463), "Mana Shield is up");
        {
            const int32_t hp = p->hp, mana = p->power;
            w.applyDamage(*t, *p, 40, 0, 0, now);
            CHECK(p->hp == hp && p->power == mana - 80, "40 damage took 80 mana (%d health, %d mana)", hp - p->hp, mana - p->power);
        }
        // Blink: 20 yards ahead, out of the root
        p->gcdEndMs = 0;
        p->rootUntilMs = now + 5000;
        const float bx = p->x, by = p->y;
        w.tryCast(*p, 1953, p->guid, now);
        run(300);
        const float moved = std::hypot(p->x - bx, p->y - by);
        CHECK(moved > 15.0f && !p->rooted(now), "Blink: %.1f yards, free", moved);
        // Frost Armor: a swing at the mage chills the attacker (proc of 6136)
        p->gcdEndMs = 0;
        p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 168, p->guid, now);
        run(300);
        CHECK(w.hasAura(*p, 168), "Frost Armor is up");
        stand(t, 2.0f);
        t->hp = t->maxHp;
        while (t->auraCount) w.removeAuraSpell(*t, t->auras[0].spell);
        t->victim = t->target = p->guid;
        w.startAttack(*t, p->guid, now);
        uint32_t t1 = now;
        while (now - t1 < 8000 && !w.hasAura(*t, 6136)) run(100);
        CHECK(w.hasAura(*t, 6136), "the attacker was chilled by Frost Armor");
    }
    session.leaveWorld();

    // ---- the rogue: combo points (also on the client), Eviscerate, Slice and Dice, Stealth, Backstab, Vanish
    play(rogueGuid);
    run(800);
    p = w.player();
    p->maxHp = p->hp = 5000;
    p->level = 10;
    off::CharacterProfile& rp = w.playerProfile();
    for (uint32_t s : {1752u, 2098u, 5171u, 1784u, 53u, 1856u})
        if (std::find(rp.book.begin(), rp.book.end(), s) == rp.book.end()) rp.book.push_back(s);
    std::printf("rogue: energy %d / %u, main hand %u\n", p->power, p->maxPower, rp.equipment[15]);
    off::Entity* r = nearestHostile(p->x, p->y, 300);
    CHECK(r != nullptr, "something to stab");
    if (r) {
        r->maxHp = r->hp = 3000;
        r->level = 5;
        stand(r, 2.0f);
        r->o = 0;   // it faces the rogue
        run(200);
        // Stealth, out of combat; a creature behind it does not see the rogue, one face to face does
        p->power = 100;
        w.tryCast(*p, 1784, p->guid, now);
        run(200);
        CHECK(w.hasAura(*p, 1784) && p->form == 30, "Stealth (form %d)", p->form);
        r->o = 3.14159f;   // it turns away
        CHECK(!w.detectsStealth(*r, *p), "unseen from behind");
        r->o = 0;
        CHECK(!w.detectsStealth(*r, *p), "a level 5 creature does not see a level 10 rogue at 2 yards");
        stand(r, 1.0f);
        CHECK(w.detectsStealth(*r, *p), "but bumps into it at 1 yard (its combat reach)");
        stand(r, 2.0f);
        r->o = 3.14159f;
        r->victim = 0; r->threatCount = 0;
        run(600);
        CHECK(r->victim != p->guid, "it does not attack the rogue it cannot see");
        // Backstab: from the front no, from behind yes (and it ends Stealth)
        r->o = 0;
        p->gcdEndMs = 0;
        p->power = 100;
        gs.setTarget(r->guid, now);
        int32_t h0 = r->hp;
        w.tryCast(*p, 53, r->guid, now);
        run(100);
        CHECK(r->hp == h0, "no Backstab face to face");
        r->o = 3.14159f;
        p->gcdEndMs = 0;
        w.tryCast(*p, 53, r->guid, now);
        run(100);
        CHECK(r->hp < h0, "Backstab from behind (%d)", h0 - r->hp);
        CHECK(!w.hasAura(*p, 1784), "out of Stealth");
        CHECK(p->comboTarget == r->guid && p->comboPoints == 1, "a combo point (%d)", p->comboPoints);
        run(300);
        CHECK(gs.comboPointsOn(r->guid) == 1, "the client shows it (%d)", gs.comboPointsOn(r->guid));
        // Sinister Strike x2: three points; Slice and Dice for three; then Eviscerate with the next
        for (int k = 0; k < 2; ++k) { p->gcdEndMs = 0; p->power = 100; w.tryCast(*p, 1752, r->guid, now); run(100); }
        CHECK(p->comboPoints == 3, "three combo points (%d)", p->comboPoints);
        p->gcdEndMs = 0; p->power = 100;
        w.tryCast(*p, 5171, r->guid, now);   // (its dummy effect wants the combo target)
        run(100);
        uint32_t sndLeft = 0;
        for (uint8_t i = 0; i < p->auraCount; ++i) if (p->auras[i].spell == 5171) sndLeft = p->auras[i].endMs - now;
        const off::SpellX* snd = data.sx(5171);
        std::printf("slice and dice: %u ms (base %d, max %d)\n", sndLeft, snd ? snd->durBase : 0, snd ? snd->durMax : 0);
        CHECK(snd && sndLeft > (uint32_t)snd->durBase && p->comboPoints == 0, "Slice and Dice longer for 3 points, the points used");
        for (int k = 0; k < 2; ++k) { p->gcdEndMs = 0; p->power = 100; w.tryCast(*p, 1752, r->guid, now); run(100); }
        const int32_t h1 = r->hp;
        p->gcdEndMs = 0; p->power = 100;
        w.tryCast(*p, 2098, r->guid, now);
        run(100);
        CHECK(r->hp < h1 && p->comboPoints == 0, "Eviscerate for two (%d)", h1 - r->hp);
        // Vanish: the creature lets go, the rogue is in Stealth again
        r->victim = r->target = p->guid;
        w.addThreat(*r, p->guid, 500);
        p->gcdEndMs = 0;
        w.tryCast(*p, 1784, p->guid, now);
        run(100);
        CHECK(!w.hasAura(*p, 1784), "no Stealth in combat");
        p->gcdEndMs = 0;
        w.addItemToPlayer(5140, 1, now);   // Flash Powder
        const bool vanished = w.tryCast(*p, 1856, p->guid, now);
        run(100);
        std::printf("vanish %d, stealth %d, victim %llx\n", vanished, w.hasAura(*p, 1784), (unsigned long long)r->victim);
        CHECK(w.hasAura(*p, 1784) && r->victim != p->guid, "Vanish: in Stealth, the creature lets go");
    }
    session.leaveWorld();

    // ---- the priest: spells without a form (Stances + ALLOW_WHILE_NOT_SHAPESHIFTED), Power Word: Shield and Weakened Soul,
    // Inner Fire's charges
    play(priestGuid);
    run(800);
    p = w.player();
    p->maxHp = p->hp = 5000;
    p->level = 12;
    off::CharacterProfile& pp = w.playerProfile();
    for (uint32_t s : {589u, 17u, 588u, 2050u})
        if (std::find(pp.book.begin(), pp.book.end(), s) == pp.book.end()) pp.book.push_back(s);
    off::Entity* pm = nearestHostile(p->x, p->y, 300);
    CHECK(pm != nullptr, "something for the priest");
    if (pm) {
        pm->maxHp = pm->hp = 3000;
        stand(pm, 10.0f);
        run(200);
        p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 589, pm->guid, now);
        run(200);
        CHECK(w.hasAura(*pm, 589), "Shadow Word: Pain without Shadowform");
        p->gcdEndMs = 0; p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 17, p->guid, now);
        run(200);
        CHECK(w.hasAura(*p, 17) && w.hasAura(*p, 6788), "Power Word: Shield (%d) and Weakened Soul (%d)", w.hasAura(*p, 17), w.hasAura(*p, 6788));
        w.removeAuraSpell(*p, 17);
        p->gcdEndMs = 0; p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 17, p->guid, now);
        run(200);
        CHECK(!w.hasAura(*p, 17), "no second shield under Weakened Soul");
        p->gcdEndMs = 0; p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 588, p->guid, now);
        run(200);
        int ch0 = -1;
        for (uint8_t i = 0; i < p->auraCount; ++i) if (p->auras[i].spell == 588) ch0 = p->auras[i].charges;
        CHECK(ch0 >= 18, "Inner Fire with 20 charges (%d)", ch0);
        stand(pm, 2.0f);
        pm->victim = pm->target = p->guid;
        w.startAttack(*pm, p->guid, now);
        run(6000);
        int ch1 = -1;
        for (uint8_t i = 0; i < p->auraCount; ++i) if (p->auras[i].spell == 588) ch1 = p->auras[i].charges;
        CHECK(ch1 >= 0 && ch1 < 20, "hits used its charges (%d left)", ch1);
        w.stopAttack(*pm, now);
    }
    session.leaveWorld();

    // ---- the paladin: Seal of Righteousness + Judgement, Divine Protection and Forbearance
    CHECK(session.deleteCharacter(mageGuid) == w3n::CHAR_DELETE_SUCCESS, "the mage leaves room");
    w3n::CharCreateInfo pal; pal.name = "Uther"; pal.race = 1; pal.cls = 2;
    CHECK(session.createCharacter(pal) == w3n::CHAR_CREATE_SUCCESS, "a human paladin");
    for (const auto& c : session.characters()) if (c.name == "Uther") paladinGuid = c.guid;
    play(paladinGuid);
    run(800);
    p = w.player();
    p->maxHp = p->hp = 5000;
    p->level = 12;
    off::CharacterProfile& lp = w.playerProfile();
    for (uint32_t s : {21084u, 20271u, 498u, 1022u})
        if (std::find(lp.book.begin(), lp.book.end(), s) == lp.book.end()) lp.book.push_back(s);
    off::Entity* lm = nearestHostile(p->x, p->y, 300);
    CHECK(lm != nullptr, "something for the paladin");
    if (lm) {
        lm->maxHp = lm->hp = 3000;
        stand(lm, 5.0f);
        run(200);
        p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 21084, p->guid, now);
        run(200);
        CHECK(w.hasAura(*p, 21084), "Seal of Righteousness");
        p->gcdEndMs = 0; p->power = (int32_t)p->maxPower;
        const int32_t jh = lm->hp;
        w.tryCast(*p, 20271, lm->guid, now);
        run(300);
        CHECK(!w.hasAura(*p, 21084) && lm->hp < jh, "Judgement of Righteousness: %d, the seal used (%d)", jh - lm->hp, w.hasAura(*p, 21084));
        p->gcdEndMs = 0; p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 498, p->guid, now);
        run(200);
        CHECK(w.hasAura(*p, 498) && w.hasAura(*p, 25771), "Divine Protection (%d) and Forbearance (%d)", w.hasAura(*p, 498), w.hasAura(*p, 25771));
        p->gcdEndMs = 0; p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 1022, p->guid, now);
        run(200);
        CHECK(!w.hasAura(*p, 1022), "no Blessing of Protection under Forbearance");
    }
    session.leaveWorld();

    // ---- the druid: Bear Form (model, rage, 2.5 s paws, armour), Maul only in it, Moonfire not; Cat Form (energy,
    // 1.0 s), out of it the mana back; a quest's learning spell teaches
    CHECK(session.deleteCharacter(rogueGuid) == w3n::CHAR_DELETE_SUCCESS, "the rogue leaves room");
    w3n::CharCreateInfo dru; dru.name = "Malfurion"; dru.race = 4; dru.cls = 11;
    CHECK(session.createCharacter(dru) == w3n::CHAR_CREATE_SUCCESS, "a night elf druid");
    uint64_t druidGuid = 0;
    for (const auto& c : session.characters()) if (c.name == "Malfurion") druidGuid = c.guid;
    play(druidGuid);
    run(800);
    p = w.player();
    p->level = 20;
    w.recomputeGearPublic();
    off::CharacterProfile& dp = w.playerProfile();
    for (uint32_t s : {5487u, 6807u, 8921u, 768u})
        if (std::find(dp.book.begin(), dp.book.end(), s) == dp.book.end()) dp.book.push_back(s);
    const int32_t mana0 = p->power;
    const uint32_t armor0 = p->armor, swing0 = p->attackMs;
    p->power = (int32_t)p->maxPower;
    const int32_t manaFull = p->power;
    (void)mana0;
    w.tryCast(*p, 5487, p->guid, now);
    run(300);
    const int32_t manaKept = p->savedPower;
    std::printf("bear: form %u display %u power %u/%d swing %u armour %u -> %u, hp %u\n", p->form, p->formDisplay, p->powerType,
                p->power, p->attackMs, armor0, p->armor, p->maxHp);
    CHECK(p->form == 5 && p->formDisplay == 2281 && p->powerType == 1 && p->attackMs == 2500, "Bear Form: model 2281, rage, 2.5 s");
    CHECK(p->armor > armor0 && p->dmgMax > 20, "a bear's armour and paws (%u, %u-%u)", p->armor, p->dmgMin, p->dmgMax);
    {
        const w3n::WorldObject* me = gs.selfObject();
        CHECK(me && me->displayId() == 2281 && ((me->field(w3n::UF::UNIT_BYTES_0) >> 24) & 0xFF) == 1, "the client sees the bear and its rage");
    }
    off::Entity* dm = nearestHostile(p->x, p->y, 300);
    CHECK(dm != nullptr, "something for the druid");
    if (dm) {
        dm->maxHp = dm->hp = 3000;
        stand(dm, 2.0f);
        p->power = 50; p->gcdEndMs = 0;
        int32_t h = dm->hp;
        w.tryCast(*p, 6807, dm->guid, now);   // Maul: on the next swing
        w.startAttack(*p, dm->guid, now);
        run(3000);
        CHECK(dm->hp < h, "the bear mauls (%d)", h - dm->hp);
        w.stopAttack(*p, now);
        p->gcdEndMs = 0;
        h = dm->hp;
        w.tryCast(*p, 8921, dm->guid, now);
        run(300);
        CHECK(!w.hasAura(*dm, 8921), "no Moonfire as a bear");
    }
    p->gcdEndMs = 0;
    w.tryCast(*p, 768, p->guid, now);   // (as the client's form button: the first press leaves the bear)
    run(300);
    CHECK(p->form == 0 && p->powerType == 0, "the cat button out of the bear: herself (form %u)", p->form);
    p->gcdEndMs = 0;
    w.tryCast(*p, 768, p->guid, now);
    run(300);
    CHECK(p->form == 1 && p->formDisplay == 892 && p->powerType == 3 && p->attackMs == 1000 && !w.hasAura(*p, 5487),
          "Cat Form: model 892, energy, 1.0 s, the bear gone (form %u display %u power %u swing %u bear %d)", p->form, p->formDisplay, p->powerType, p->attackMs, w.hasAura(*p, 5487));
    w.removeAuraSpell(*p, 768);
    run(300);
    CHECK(p->form == 0 && p->formDisplay == 0 && p->powerType == 0 && p->power >= manaKept - (manaFull - manaKept) - 1 &&
          p->power < manaFull && p->attackMs == swing0, "herself again: mana %d (%d kept in the bear, %d full)", p->power, manaKept, manaFull);
    {
        const w3n::WorldObject* me = gs.selfObject();
        CHECK(me && me->displayId() == me->field(w3n::UF::UNIT_NATIVEDISPLAYID), "the client sees the night elf again");
    }
    {   // a quest's reward spell that teaches (CMaNGOS RewardQuest -> SPELL_EFFECT_LEARN_SPELL)
        uint32_t teach = 0, learn = 0;
        for (const auto& [entry, t] : data.trainers)
            for (const off::TrainerSpell& ts : t.spells)
                if (!teach && ts.teach != ts.learn && t.cls == 11 &&
                    std::find(dp.book.begin(), dp.book.end(), ts.learn) == dp.book.end()) { teach = ts.teach; learn = ts.learn; }
        w.rewardSpell(*p, teach, now);
        run(200);
        CHECK(teach && std::find(dp.book.begin(), dp.book.end(), learn) != dp.book.end(), "a learning spell taught %u", learn);
    }
    session.leaveWorld();

    // ---- the warlock: Summon Imp (pet_levelstats, a generated name, the pet bar on the client), it follows, it
    // fights the owner's target with Firebolt from afar, its kill is ours, stay, back at login, dismissed
    CHECK(session.deleteCharacter(priestGuid) == w3n::CHAR_DELETE_SUCCESS, "the priest leaves room");
    w3n::CharCreateInfo wl; wl.name = "Gulbash"; wl.race = 1; wl.cls = 9;
    CHECK(session.createCharacter(wl) == w3n::CHAR_CREATE_SUCCESS, "a human warlock");
    uint64_t warlockGuid = 0;
    for (const auto& c : session.characters()) if (c.name == "Gulbash") warlockGuid = c.guid;
    play(warlockGuid);
    run(800);
    p = w.player();
    {
        off::CharacterProfile& lk = w.playerProfile();
        lk.book.push_back(688);
        p->power = (int32_t)p->maxPower;
        w.tryCast(*p, 688, p->guid, now);
        run(10600);   // a 10 s cast
        off::Entity* imp = w.pet();
        CHECK(imp && imp->entry == 416 && imp->ownerGuid == p->guid, "the Imp is here");
        if (imp) {
            std::printf("imp: '%s' hp %u mana %u dmg %u-%u spells %u (%u) level %u\n", imp->petName.c_str(), imp->maxHp, imp->maxPower,
                        imp->dmgMin, imp->dmgMax, imp->spellCount, imp->spellCount ? imp->spells[0] : 0, imp->level);
            CHECK(!imp->petName.empty() && imp->maxHp > 20 && imp->maxPower > 0 && imp->spellCount && imp->spells[0] == 3110,
                  "pet_levelstats, a name, Firebolt");
            CHECK(gs.petGuid() == imp->guid && gs.pet.active && (gs.pet.bar[3] & 0xFFFFFF) == 3110 && (gs.pet.bar[3] >> 24) == 0xC1,
                  "the client has the pet and its bar (Firebolt on autocast)");
            const w3n::WorldObject* io = gs.objects.find(imp->guid);
            CHECK(io != nullptr, "the client sees the Imp");
            // it follows
            const float ox = p->x, oy = p->y;
            gs.mover.reset(ox + 20, oy, p->z, 0, now);
            w.setPosition(*p, ox + 20, oy, p->z, 0);
            run(5000);
            imp = w.pet();
            CHECK(imp && std::hypot(imp->x - p->x, imp->y - p->y) < 4.0f, "it follows (%.1f yd)", imp ? std::hypot(imp->x - p->x, imp->y - p->y) : -1.0f);
            // it fights what we fight: Firebolt from afar, the kill is ours
            off::Entity* foe = nullptr;   // (one worth experience: no critter)
            for (int i = 0; i < off::kMaxEntities && !foe; ++i) {
                off::Entity* e = w.byIndex(i);
                if (e && e->used && e->objType == 3 && e->alive() && !e->ownerGuid && w.canAttack(*p, *e) && !e->npcFlags &&
                    data.creature(e->entry) && data.creature(e->entry)->xp > 0) foe = e;
            }
            CHECK(foe != nullptr, "something worth experience");
            if (foe && imp) {
                foe->level = 1;
                foe->maxHp = foe->hp = 60;
                foe->victim = 0; foe->threatCount = 0;
                stand(foe, 15.0f);
                imp->x = p->x; imp->y = p->y + 1;
                gs.setTarget(foe->guid, now);
                gs.petCommand(w3n::kPetAttack, now);
                const uint32_t xp0 = p->levelXp;
                uint32_t t0 = now;
                bool firebolt = false;
                while (now - t0 < 30000 && foe->alive()) {
                    run(200);
                    firebolt = firebolt || (w.pet() && w.pet()->castSpell == 3110);
                }
                std::printf("imp fight: firebolt %d, foe hp %d, imp at %.1f yd, xp %u -> %u\n", firebolt, foe->hp,
                            w.pet() ? std::hypot(w.pet()->x - foe->x, w.pet()->y - foe->y) : -1.0f, xp0, p->levelXp);
                CHECK(firebolt && !foe->alive() && p->levelXp > xp0, "the Imp burns it down with Firebolt, the experience is ours");
            }
            // stay
            gs.petCommand(w3n::kPetStay, now);
            run(300);
            imp = w.pet();
            const float sx = imp ? imp->x : 0, sy = imp ? imp->y : 0;
            gs.mover.reset(p->x + 15, p->y, p->z, 0, now);
            w.setPosition(*p, p->x + 15, p->y, p->z, 0);
            run(3000);
            CHECK(imp && std::hypot(imp->x - sx, imp->y - sy) < 1.5f && gs.pet.command == w3n::kPetStay, "it stays");
        }
        // back at login
        const std::string name = w.pet() ? w.pet()->petName : "";
        session.leaveWorld();
        play(warlockGuid);
        run(1000);
        p = w.player();
        CHECK(w.pet() && w.pet()->petName == name && gs.pet.active, "the Imp '%s' comes back at login", name.c_str());
        gs.petDismiss(now);
        run(300);
        CHECK(!w.pet() && !gs.pet.active && gs.petGuid() == 0, "dismissed");
        // "The Binding": the Bloodstone Choker's Summon Voidwalker needs the Summoning Circle (spell focus 83); there it
        // calls the Summoned Voidwalker (dbscripts_on_event 1131), which turns hostile and goes for its summoner (EventAI)
        p->level = 10; p->maxHp = p->hp = 3000;
        p->power = (int32_t)p->maxPower; p->gcdEndMs = 0;
        lk.book.push_back(7728);
        CHECK(!w.tryCast(*p, 7728, p->guid, now), "no Summon Voidwalker away from the circle");
        const off::GoSpawn* circle = nullptr;
        for (const off::GoSpawn& g : data.goSpawns) if (g.entry == 92015) circle = &g;
        CHECK(circle != nullptr, "the Summoning Circle under the Slaughtered Lamb");
        if (circle) {
            gs.mover.reset(circle->x + 2, circle->y, circle->z, 0, now);
            w.setPosition(*p, circle->x + 2, circle->y, circle->z, 0);
            run(300);
            p->gcdEndMs = 0;
            CHECK(w.tryCast(*p, 7728, p->guid, now), "Summon Voidwalker at the circle");
            run(10300);   // a 10 s cast
            off::Entity* vw = nullptr;
            for (int i = 0; i < off::kMaxEntities; ++i) {
                off::Entity* e = w.byIndex(i);
                if (e && e->used && e->entry == 5676 && e->alive()) vw = e;
            }
            CHECK(vw && vw->summonerGuid == p->guid, "the Summoned Voidwalker is here");
            run(2500);
            CHECK(vw && vw->faction == 14 && vw->victim == p->guid, "it turns on its summoner (faction %u)", vw ? vw->faction : 0);
            if (vw) {
                vw->hp = 1;
                w.startAttack(*p, vw->guid, now);
                uint32_t t1 = now;
                while (now - t1 < 6000 && vw->alive()) run(200);
                CHECK(!vw->alive(), "it is beaten");
                w.stopAttack(*p, now);
            }
        }
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
