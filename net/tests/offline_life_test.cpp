// WoWVita: death and home in the offline world, through the real client session (GameSession).
//   offline_life_test pack.w3do   (a pack from tools/offline_data/export_world_sql.py)
//   - die in Northshire (Elwynn Forest, zone 12) -> release: the ghost is at the Northshire graveyard (world_safe_locs 105),
//     the body stays; reclaiming needs 39 yd and the 30 s delay
//   - the spirit healer: only ghosts see it; it brings back to life at the graveyard, with resurrection sickness at level 25
//   - Innkeeper Farley (295, Goldshire): "Make this inn your home." -> binder confirm -> home; the Hearthstone takes the
//     character there after 10 s, moving breaks the cast, then a 1 hour cooldown; home and cooldown are saved
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../client.hpp"
#include "../game.hpp"
#include "../movement.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_server.hpp"

namespace off = w3n::offline;

static int failed = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++failed; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: offline_life_test pack.w3do\n"); return 2; }
    off::WorldData data;
    std::string err;
    if (!off::loadWorldData(argv[1], data, err)) { std::printf("cannot load: %s\n", err.c_str()); return 2; }
    std::printf("pack: %zu graveyards, %zu zone links\n", data.graveyards.size(), data.graveyardLinks.size());
    CHECK(data.graveyards.size() >= 40, "the Eastern Kingdoms graveyards are in the pack");
    CHECK(data.item(off::kHearthstoneItem) != nullptr, "the Hearthstone item is in the pack");
    CHECK(data.spell(off::kHearthstoneSpell) && data.spell(off::kHearthstoneSpell)->castMs == 10000, "the Hearthstone spell");
    size_t healers = 0, inns = 0;
    for (const off::Spawn& s : data.spawns)
        if (const off::CreatureTemplate* t = data.creature(s.entry)) { healers += (t->npcFlags & 0x20) != 0; inns += (t->npcFlags & 0x80) != 0; }
    std::printf("pack: %zu spirit healers, %zu innkeepers\n", healers, inns);
    CHECK(healers >= 20 && inns >= 10, "spirit healers and innkeepers are spawned");

    const char* savePath = "offline_life_test_save.w3d";
    remove(savePath);
    remove((std::string(savePath) + ".bak").c_str());
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
    // the client's position follows the server's: a teleport (MSG_MOVE_TELEPORT_ACK) moves gs.mover
    auto run = [&](uint32_t ms) { for (uint32_t t = 0; t < ms; t += 16) { now += 16; session.update(now); gs.update(now); } };
    run(500);
    off::World& w = session.world();
    off::Entity* p = w.player();
    auto put = [&](float x, float y, float z) {   // the player walks somewhere: the client says so
        gs.mover.reset(x, y, z, 0, now);
        w.setPosition(*p, x, y, z, 0);
        run(1200);
    };
    auto hearthItem = [&]() -> const w3n::InvItem* {
        static w3n::InvItem it;
        for (const w3n::InvItem& i : gs.inventory()) if (i.entry == off::kHearthstoneItem) { it = i; return &it; }
        return nullptr;
    };
    auto seenHealer = [&]() {
        for (auto& [g, ob] : gs.objects.all())
            if (ob.type == w3n::ObjType::Unit && (ob.field(w3n::UF::UNIT_NPC_FLAGS) & 0x20)) return true;
        return false;
    };

    run(800);
    CHECK(hearthItem() != nullptr, "a new character carries the Hearthstone");
    gs.reportZone(12, now);   // Elwynn Forest (main.cpp: the zone banner)
    run(200);

    // ---- death in Northshire -> the Northshire graveyard
    const float dieX = -8913.0f, dieY = -136.0f, dieZ = 81.0f;
    put(dieX, dieY, dieZ);
    CHECK(!seenHealer(), "the living do not see the spirit healer");
    w.applyDamage(*p, *p, 100000, 0, 0, now);
    run(500);
    CHECK(gs.life() == w3n::Life::Dead, "dead");
    gs.releaseSpirit(now);
    run(1500);
    CHECK(gs.life() == w3n::Life::Ghost, "a ghost after release");
    const float gx = -8935.33f, gy = -188.646f;
    CHECK(std::hypot(p->x - gx, p->y - gy) < 2.0f, "the ghost is at the Northshire graveyard (server %.1f %.1f)", p->x, p->y);
    CHECK(std::hypot(gs.mover.info().x - gx, gs.mover.info().y - gy) < 2.0f, "the client was teleported (%.1f %.1f)",
          gs.mover.info().x, gs.mover.info().y);
    CHECK(gs.corpse.known && std::hypot(gs.corpse.x - dieX, gs.corpse.y - dieY) < 2.0f, "the body stays where it fell");
    CHECK(seenHealer(), "the ghost sees the spirit healer");
    // a stale movement packet from before the teleport must not take the ghost back
    run(300);
    gs.reclaimCorpse(now);
    run(300);
    CHECK(gs.life() == w3n::Life::Ghost, "no reclaim from the graveyard (too far, too early)");
    put(dieX + 10.0f, dieY, dieZ);
    run(30000);
    gs.reclaimCorpse(now);
    run(500);
    CHECK(gs.life() == w3n::Life::Alive, "reclaimed at the body after 30 s");
    CHECK(p->hp >= (int32_t)p->maxHp / 2 && p->hp < (int32_t)p->maxHp, "half of the health, then regeneration (%d / %u)", p->hp, p->maxHp);
    run(1500);
    CHECK(!seenHealer(), "the spirit healer is gone again");

    // ---- the spirit healer, level 25: resurrection sickness
    p->level = 25;
    w.applyDamage(*p, *p, 100000, 0, 0, now);
    run(500);
    gs.releaseSpirit(now);
    run(1500);
    const off::Entity* healer = nullptr;
    for (int i = 0; i < off::kMaxEntities; ++i) {
        const off::Entity* e = w.byIndex(i);
        if (e && e->used && e->objType == 3 && (e->npcFlags & 0x20) && (!healer || std::hypot(e->x - p->x, e->y - p->y) < std::hypot(healer->x - p->x, healer->y - p->y))) healer = e;
    }
    CHECK(healer && std::hypot(healer->x - p->x, healer->y - p->y) < 40.0f, "a spirit healer at the graveyard");
    if (healer) {
        put(healer->x + 2.0f, healer->y, healer->z);
        run(1500);
        float q[3] = {p->x, p->y, p->z};
        const std::string why = gs.talkToHealer(q, now);
        run(500);
        CHECK(gs.spiritHealerConfirm == healer->guid, "the spirit healer asks (%s)", why.c_str());
        gs.acceptSpiritHealer(now);
        run(500);
        CHECK(gs.life() == w3n::Life::Alive, "alive at the spirit healer");
        CHECK(w.hasAura(*p, off::kResurrectionSickness), "resurrection sickness at level 25");
    }

    // ---- Innkeeper Farley: home
    const off::Spawn* farley = nullptr;
    for (const off::Spawn& s : data.spawns) if (s.entry == 295) farley = &s;
    CHECK(farley != nullptr, "Innkeeper Farley in the pack");
    if (farley) {
        put(farley->x + 2.0f, farley->y, farley->z);
        run(1500);
        const off::Entity* inn = nullptr;
        for (int i = 0; i < off::kMaxEntities; ++i) {
            const off::Entity* e = w.byIndex(i);
            if (e && e->used && e->entry == 295) inn = e;
        }
        CHECK(inn != nullptr, "Farley is there");
        if (inn) {
            float q[3] = {p->x, p->y, p->z};
            gs.gossip = w3n::GossipMenu();
            gs.interact(inn->guid, q, now);
            run(500);
            int bindIdx = -1;
            for (size_t i = 0; i < gs.gossip.options.size(); ++i)
                if (gs.gossip.options[i].text.find("inn your home") != std::string::npos) bindIdx = (int)i;
            CHECK(gs.gossip.open && bindIdx >= 0, "the innkeeper offers \"Make this inn your home.\" (%zu options)", gs.gossip.options.size());
            if (bindIdx >= 0) {
                gs.selectGossip((size_t)bindIdx, now);
                run(500);
                CHECK(gs.binderConfirm == inn->guid, "binder confirmation");
                gs.acceptBinder(now);
                run(500);
                const off::CharacterProfile& pr = w.playerProfile();
                CHECK(pr.homeSet && std::hypot(pr.homeX - p->x, pr.homeY - p->y) < 1.0f, "Goldshire is home");
            }
        }
    }
    const float homeX = w.playerProfile().homeX, homeY = w.playerProfile().homeY;

    // ---- the Hearthstone: moving breaks it, then 10 s take the character home, then the cooldown
    put(-8913.0f, -136.0f, 81.0f);
    const w3n::InvItem* hs = hearthItem();
    CHECK(hs != nullptr, "the Hearthstone in the bag");
    if (hs) {
        gs.useItem(*hs, now);
        run(3000);
        CHECK(p->castSpell == off::kHearthstoneSpell, "casting the Hearthstone");
        gs.mover.reset(p->x + 3.0f, p->y, p->z, 0, now);
        {   // a step: the client's movement packet
            w3n::MovementInfo m = gs.mover.info();
            w3n::Writer wr;
            w3n::writeMovementInfo(wr, m);
            session.onClientPacket(w3n::MoveOp::Heartbeat, wr.b, now);
        }
        run(500);
        CHECK(p->castSpell == 0, "moving broke the cast");
        CHECK(w.cooldownLeft(*p, off::kHearthstoneSpell, now) == 0, "a broken cast leaves no cooldown");
        gs.useItem(*hs, now);
        run(10500);
        CHECK(std::hypot(p->x - homeX, p->y - homeY) < 1.0f, "the Hearthstone took the character home (%.1f %.1f)", p->x, p->y);
        CHECK(std::hypot(gs.mover.info().x - homeX, gs.mover.info().y - homeY) < 1.0f, "the client is home too");
        CHECK(w.cooldownLeft(*p, off::kHearthstoneSpell, now) > 3500000u, "a 1 hour cooldown");
        put(-8913.0f, -136.0f, 81.0f);
        gs.useItem(*hs, now);
        run(11000);
        CHECK(std::hypot(p->x + 8913.0f, p->y + 136.0f) < 2.0f, "not again during the cooldown");
    }

    // ---- saved: home and the cooldown
    std::string serr;
    CHECK(session.save(&serr), "save: %s", serr.c_str());
    off::OfflineSaveData sd;
    CHECK(off::loadOffline(savePath, sd, serr), "load: %s", serr.c_str());
    CHECK(sd.player.homeSet && std::hypot(sd.player.homeX - homeX, sd.player.homeY - homeY) < 1.0f, "home in the save");
    CHECK(sd.player.hearthCdSec > 3400, "the cooldown in the save (%u s)", sd.player.hearthCdSec);

    std::printf("%d / %d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
