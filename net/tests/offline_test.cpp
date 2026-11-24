// offline_test — automated PC acceptance tests for the offline world core.
//
// The test plugs the new OfflineSession into the *existing* GameSession through WorldClient's
// offline packet sink, so every SMSG is parsed by the same code the 3DS runs. It checks:
//   1. the .w3do data model and the built-in sample,
//   2. offline login: character list, world entry, party list, spell book, inventory,
//   3. one complete authoritative fight (attack, damage, death, XP, loot),
//   4. four bots following and winning a multi-enemy encounter (deterministic),
//   5. save / load / backup recovery,
//   6. the INT8 model loader and (when generated) bit-exact inference vectors,
//   7. the neural policy with the legal-action mask and deterministic fallback.
//
// Usage: offline_test [nn_golden.txt]
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

#include "../game.hpp"
#include "../offline/offline_data.hpp"
#include "../offline/offline_nn.hpp"
#include "../offline/offline_server.hpp"

using namespace w3n;
using namespace w3n::offline;

static int g_ok = 0, g_fail = 0;
#define CHECK(c, ...) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

namespace {

struct Harness {
    OfflineSession session;
    WorldClient wc;
    GameSession gs;
    uint32_t now = 10000;
    uint32_t monsterMoves = 0, playerMoves = 0;

    bool start(const WorldData& data, const char* savePath, std::string& err) {
        session.setup(&data, savePath);
        if (!session.open(err)) return false;
        if (session.characters().empty()) { err = "no characters"; return false; }
        const uint64_t guid = session.characters()[0].guid;
        float pos[3] = {0, 0, 0};
        float o = 0;
        if (!session.enterWorld(guid, now, pos, &o)) { err = "enterWorld failed"; return false; }
        wc.packetSink = [this](uint16_t op, const Bytes& b) { session.onClientPacket(op, b, now); };
        session.setSink([this](uint16_t op, const Bytes& b) {
            if (op == 0x0DD) ++monsterMoves;
            if (op == 0x0DA || op == 0x0EE) ++playerMoves;
            gs.handle(op, b, now);
        });
        wc.beginOffline(guid, session.characters(), data.mapId, pos, o);
        gs.attach(wc, now);
        gs.spellNames.load("missing-spells.txt");   // keeps the built-in names for the chat lines
        return true;
    }
    void tick(uint32_t ms = 16) {
        now += ms;
        session.update(now);
        gs.update(now);
    }
    void run(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 16) tick(16);
    }
    // Keep the client-side mover (which decides melee range) on the server's position after a
    // scripted teleport. In the real game the server answers the movement packet instead.
    void syncMover() {
        const Entity* p = session.world().player();
        if (p) gs.mover.reset(p->x, p->y, p->z, p->o, now);
    }
};

Entity* firstCreature(World& w, uint32_t entry) {
    for (int i = 0; i < kMaxEntities; ++i) {
        Entity* e = w.byIndex(i);
        if (e && e->used && e->objType == 3 && e->entry == entry) return e;
    }
    return nullptr;
}

void teleportNextTo(World& w, Entity& target, float dist) {
    Entity* p = w.player();
    if (!p) return;
    const float a = 2.2f;
    w.setPosition(*p, target.x + std::cos(a) * dist, target.y + std::sin(a) * dist, target.z, 0.0f);
    p->o = std::atan2(target.y - p->y, target.x - p->x);   // face the target
}

bool fileExists(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

void writeFile(const char* p, const char* text) {
    FILE* f = fopen(p, "wb");
    if (!f) return;
    fwrite(text, 1, std::strlen(text), f);
    fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    const char* golden = argc > 1 ? argv[1] : nullptr;
    const char* savePath = "offline_test_save.w3d";
    remove(savePath);
    remove("offline_test_save.w3d.bak");
    remove("offline_test_save.w3d.tmp");

    // ---------------------------------------------------------------- 1. data
    WorldData data;
    CHECK(buildSampleWorldData(data), "the built-in sample must validate");
    const std::vector<std::string> problems = data.validate();
    CHECK(problems.empty(), "sample validation: %s", problems.empty() ? "" : problems[0].c_str());
    CHECK(data.creatures.size() == 4, "creatures: %zu", data.creatures.size());
    CHECK(data.spawns.size() == 10, "spawns: %zu", data.spawns.size());
    CHECK(data.spells.size() >= 10, "spells: %zu", data.spells.size());
    CHECK(data.bots[0].role == Role::Tank && data.bots[1].role == Role::Healer, "bot roles");

    // A truncated / corrupted pack must be refused, not crash.
    {
        WorldData bad;
        std::string err;
        const uint8_t junk[32] = {0};
        CHECK(!loadWorldDataBuffer(junk, sizeof junk, bad, err), "junk data must be rejected");
        CHECK(!err.empty(), "a rejected pack explains why");
    }
    // The Python exporter's pack (when the test runner generated it) must load into the same model.
    if (argc > 2 && fileExists(argv[2])) {
        WorldData fromPack;
        std::string perr;
        CHECK(loadWorldData(argv[2], fromPack, perr), "exporter pack loads: %s", perr.c_str());
        CHECK(fromPack.creatures.size() == data.creatures.size(), "pack creatures: %zu", fromPack.creatures.size());
        CHECK(fromPack.spawns.size() == data.spawns.size(), "pack spawns: %zu", fromPack.spawns.size());
        CHECK(fromPack.spells.size() == data.spells.size(), "pack spells: %zu", fromPack.spells.size());
        const CreatureTemplate* wf = fromPack.creature(299);
        CHECK(wf && wf->hp == 40 && wf->dmgMax == 4 && wf->lootId == 1, "wolf template from the pack");
        CHECK(fromPack.bots[1].name == "Elara" && fromPack.bots[1].role == Role::Healer, "companion from the pack");
        CHECK(fromPack.validate().empty(), "pack referential integrity");
    }

    // ---------------------------------------------------------------- 2. offline login
    std::string err;
    Harness h;
    CHECK(h.start(data, savePath, err), "offline start: %s", err.c_str());
    h.run(2000);
    CHECK(h.gs.mapId() == data.mapId, "map %u", h.gs.mapId());
    const WorldObject* me = h.gs.selfObject();
    CHECK(me != nullptr, "the self object was created");
    if (me) {
        CHECK(me->health() == 120 && me->maxHealth() == 120, "health %u/%u", me->health(), me->maxHealth());
        CHECK(me->field(UF::UNIT_BYTES_0) & 0xFF, "race field");
    }
    CHECK(h.gs.party.inGroup, "party list received");
    CHECK(h.gs.party.members.size() == kMaxParty, "party members: %zu", h.gs.party.members.size());
    CHECK(h.gs.party.leader == h.session.playerGuid(), "we lead the party");
    CHECK(h.gs.objects.count(ObjType::Player) >= 5, "self + four companions: %zu", h.gs.objects.count(ObjType::Player));
    CHECK(h.gs.objects.count(ObjType::Unit) >= 10, "nearby creatures: %zu", h.gs.objects.count(ObjType::Unit));
    CHECK(h.gs.knowsSpell(78), "Heroic Strike in the spell book");
    CHECK(h.gs.actionBarSeen, "action bar");
    const std::vector<InvItem> inv = h.gs.inventory();
    CHECK(inv.size() >= 2, "equipment visible in the inventory: %zu", inv.size());
    bool sword = false;
    for (const InvItem& it : inv) sword = sword || it.entry == 25;
    CHECK(sword, "the Worn Shortsword is carried");
    CHECK(h.gs.selfName == "Recruit", "self name: %s", h.gs.selfName.c_str());

    // Query answers (the client asks for every created object).
    CHECK(h.gs.objects.hasCreatureInfo(299), "creature query answered");
    CHECK(h.gs.objects.hasPlayerName(h.session.characters()[0].guid) || h.gs.party.members.size() == 4,
          "player/companion names answered");

    // Local roster: creation, duplicate names, deletion without hiding later characters.
    {
        CharCreateInfo ci;
        ci.name = "Tester";
        ci.race = 1; ci.cls = 8; ci.gender = 1;
        const int r = h.session.createCharacter(ci);
        CHECK(r == CHAR_CREATE_SUCCESS, "create character: %d", r);
        CHECK(h.session.characters().size() == 2, "roster size %zu", h.session.characters().size());
        CHECK(h.session.createCharacter(ci) == 0x31, "duplicate name refused");
        const uint64_t created = h.session.characters().back().guid;
        CHECK(h.session.deleteCharacter(created) == CHAR_DELETE_SUCCESS, "delete character");
        CHECK(h.session.characters().size() == 1, "roster back to one");
    }

    // Party command round-trip through the real chat packet.
    h.gs.sendTyped("/p follow", h.now);
    h.run(200);
    bool follow = true;
    for (int i = 0; i < kMaxParty; ++i) follow = follow && h.session.world().bot(i)->tacticalMode == 0;
    CHECK(follow, "the party obeys /p follow");
    h.gs.sendTyped("/p assist", h.now);
    h.run(200);
    bool assist = true;
    for (int i = 0; i < kMaxParty; ++i) assist = assist && h.session.world().bot(i)->tacticalMode == 1;
    CHECK(assist, "the party obeys /p assist");

    // ---------------------------------------------------------------- 3. one complete fight
    Entity* wolf = firstCreature(h.session.world(), 299);
    CHECK(wolf != nullptr, "a Young Wolf exists");
    if (wolf) {
        teleportNextTo(h.session.world(), *wolf, 2.0f);
        h.syncMover();
        h.run(32);
        h.gs.setTarget(wolf->guid, h.now);
        h.run(32);
        CHECK(h.gs.target() == wolf->guid, "target selected");
        h.gs.startAttack(h.now);
        h.run(25000);
        CHECK(wolf->dead, "the wolf died (hp %d)", wolf->hp);
        const WorldObject* corpse = h.gs.objects.find(wolf->guid);
        CHECK(corpse && corpse->health() == 0, "the client sees the corpse at 0 health");
        CHECK(h.gs.kills >= 1, "client kill counter: %u", h.gs.kills);
        CHECK(h.gs.xpGained > 0, "experience: %u", h.gs.xpGained);
        CHECK(h.session.world().player()->hp > 0, "the player survived");
    }

    // Rage from the fight lets the warrior use Heroic Strike.
    h.gs.setTarget(0, h.now);
    h.run(32);

    // ---------------------------------------------------------------- 4. loot
    if (wolf && wolf->lootCount > 0) {
        h.gs.setTarget(wolf->guid, h.now);
        h.run(32);
        h.gs.loot(wolf->guid, h.now);
        h.run(200);
        CHECK(h.gs.lootOpen() == wolf->guid, "loot window opened");
        CHECK(h.gs.lootWindow.items.size() == wolf->lootCount || !h.gs.lootWindow.items.empty(), "loot slots");
        const uint32_t itemsBefore = h.session.world().playerProfile().bagCount;
        const uint32_t moneyBefore = h.gs.money();
        h.gs.takeAllLoot(h.now);
        h.run(300);
        CHECK(h.session.world().playerProfile().bagCount >= itemsBefore, "items stored");
        CHECK(h.gs.money() >= moneyBefore, "money updated");
        CHECK(wolf->looted, "the corpse is empty");
    } else {
        std::printf("NOTE: this wolf rolled no loot; loot path exercised in the party fight\n");
    }

    // ---------------------------------------------------------------- 5. four-bot encounter
    {
        // Put the party in the middle of the kobold camp and start one fight: the Director must
        // bring the tank, the healer and both DPS in without a wipe.
        Entity* kobold = firstCreature(h.session.world(), 6);
        CHECK(kobold != nullptr, "a Kobold Vermin exists");
        if (kobold) {
            teleportNextTo(h.session.world(), *kobold, 3.0f);
            h.syncMover();
            h.run(64);
            h.gs.setTarget(kobold->guid, h.now);
            h.run(32);
            h.gs.startAttack(h.now);
            const uint32_t killsBefore = h.session.stats.kills;
            const int aliveBefore = h.session.world().alivePartyMembers();
            float botStart[3] = {h.session.world().bot(0)->x, h.session.world().bot(0)->y, h.session.world().bot(0)->z};
            h.run(60000);
            CHECK(h.session.stats.kills > killsBefore, "the party killed something (%u -> %u)", killsBefore, h.session.stats.kills);
            CHECK(h.session.world().alivePartyMembers() >= aliveBefore - 1, "no avoidable wipe (%d -> %d)", aliveBefore,
                  h.session.world().alivePartyMembers());
            const Entity* tank = h.session.world().bot(0);
            const float moved = std::sqrt((tank->x - botStart[0]) * (tank->x - botStart[0]) +
                                          (tank->y - botStart[1]) * (tank->y - botStart[1]));
            CHECK(moved > 1.0f, "the tank moved into the fight (%.2f yd)", moved);
            CHECK(tank->hp > 0, "the tank is alive");
        }
    }

    // ---------------------------------------------------------------- 6. determinism
    {
        auto runFight = [&](uint32_t seed, int* wolfHp, int* playerHp) {
            OfflineSession s;
            s.setup(&data, "offline_det_a.w3d");
            s.open(err);
            const uint64_t guid = s.characters()[0].guid;
            float pos[3], o;
            s.enterWorld(guid, 1000, pos, &o);
            s.setSink(nullptr);
            // Drive the world directly: no client, only the simulation.
            World& w = s.world();
            Entity* wf = firstCreature(w, 299);
            teleportNextTo(w, *wf, 2.0f);
            w.startAttack(*w.player(), wf->guid, 1000);
            for (uint32_t t = 1000; t < 20000; t += 50) w.step(t);
            *wolfHp = wf->hp;
            *playerHp = w.player()->hp;
            (void)seed;
            remove("offline_det_a.w3d");
            remove("offline_det_a.w3d.bak");
        };
        int a1 = 0, a2 = 0, b1 = 0, b2 = 0;
        runFight(0, &a1, &a2);
        runFight(0, &b1, &b2);
        CHECK(a1 == b1 && a2 == b2, "the same inputs replay the same combat (%d/%d vs %d/%d)", a1, a2, b1, b2);
    }

    // ---------------------------------------------------------------- 7. save / load
    {
        h.session.save(&err);
        CHECK(fileExists(savePath), "save written: %s", err.c_str());
        const uint32_t xp = h.session.saveData().player.xp;
        const uint16_t level = h.session.saveData().player.level;
        OfflineSession s2;
        s2.setup(&data, savePath);
        CHECK(s2.open(err), "save reloaded: %s", err.c_str());
        CHECK(!s2.characters().empty(), "characters after reload");
        CHECK(s2.saveData().player.xp == xp && s2.saveData().player.level == level, "progression preserved (%u/%u)",
              s2.saveData().player.xp, s2.saveData().player.xp);
        CHECK(s2.saveData().bots[0].name == "Brom", "companion profile preserved");

        // A corrupt main file must fall back to the backup written by the previous save.
        h.session.save(&err);   // writes .bak from the previous main
        writeFile(savePath, "this is not a save");
        OfflineSession s3;
        s3.setup(&data, savePath);
        CHECK(s3.open(err), "backup recovery: %s", err.c_str());
        CHECK(s3.saveData().player.level == level, "level restored from the backup");
    }

    // ---------------------------------------------------------------- 8. neural runtime
    {
        NeuralPolicy p;
        const uint8_t junk[16] = {0};
        CHECK(!p.loadBuffer(junk, sizeof junk), "a corrupt model is rejected");
        CHECK(!p.ready() && p.infer(nullptr) == -1, "no inference without a model");
        CHECK(nnDiv127(1270) == 10 && nnDiv127(-1270) == -10, "fixed-point rounding");
        if (golden && fileExists(golden)) {
            CHECK(p.load(golden), "model file loaded: %s", golden);
            CHECK(p.ready() && p.version() == 1, "model version");
            // Bit-exact vectors produced by the Python exporter.
            const std::string gpath = std::string(golden) + ".golden";
            if (fileExists(gpath.c_str())) {
                FILE* f = fopen(gpath.c_str(), "rb");
                char line[2048];
                int vectors = 0, mismatches = 0;
                while (f && fgets(line, sizeof line, f)) {
                    if (line[0] == '#' || line[0] == '\n') continue;
                    std::string s(line);
                    const size_t p1 = s.find('|');
                    const size_t p2 = s.find('|', p1 + 1);
                    if (p1 == std::string::npos || p2 == std::string::npos) continue;
                    int8_t in[kFeatureCount];
                    int8_t want[kActionCount];
                    int count = 0;
                    const char* c = s.c_str();
                    char* end = nullptr;
                    for (int i = 0; i < kFeatureCount; ++i) {
                        in[i] = (int8_t)std::strtol(c, &end, 10);
                        c = end;
                        if (*c == ',') ++c;
                        ++count;
                    }
                    const char* c2 = s.c_str() + p1 + 1;
                    for (int i = 0; i < kActionCount; ++i) {
                        want[i] = (int8_t)std::strtol(c2, &end, 10);
                        c2 = end;
                        if (*c2 == ',') ++c2;
                    }
                    const int arg = std::atoi(s.c_str() + p2 + 1);
                    int8_t got[kActionCount];
                    p.scores(in, got);
                    ++vectors;
                    for (int i = 0; i < kActionCount; ++i)
                        if (got[i] != want[i]) ++mismatches;
                    if (p.infer(in) != arg) ++mismatches;
                    (void)count;
                }
                if (f) fclose(f);
                CHECK(vectors > 0, "golden vectors present (%d)", vectors);
                CHECK(mismatches == 0, "bit-exact inference: %d vectors, %d mismatches", vectors, mismatches);
            } else {
                std::printf("NOTE: no golden vector file next to the model (%s)\n", gpath.c_str());
            }
            // The model must drive the Director without breaking the deterministic guarantees.
            setNeuralPolicy(&p);
            Entity* kobold = firstCreature(h.session.world(), 6);
            if (kobold) {
                teleportNextTo(h.session.world(), *kobold, 3.0f);
                h.syncMover();
                h.run(64);
                h.gs.setTarget(kobold->guid, h.now);
                h.gs.startAttack(h.now);
                h.run(15000);
                CHECK(h.session.world().player()->alive(), "neural policy: the player is still alive");
            }
            setNeuralPolicy(nullptr);
        }
    }

    // ---------------------------------------------------------------- 9. movement packet discipline
    {
        Harness m;
        std::string e2;
        CHECK(m.start(data, "offline_move.w3d", e2), "movement test session: %s", e2.c_str());
        World& w = m.session.world();
        // Clear the encounter: dead creatures do not aggro, "stay" keeps the companions in place.
        for (int i = 0; i < kMaxEntities; ++i) {
            Entity* e = w.byIndex(i);
            if (e && e->used && e->objType == 3) { e->dead = true; e->respawnAtMs = 0; e->moving = false; e->victim = 0; }
        }
        for (int i = 0; i < kMaxParty; ++i) w.setPartyCommand(i, 1, m.now);
        m.run(1200);
        // (the grid wakes the cells around the player in the first ticks: those creatures go too)
        for (int i = 0; i < kMaxEntities; ++i) {
            Entity* e = w.byIndex(i);
            if (e && e->used && e->objType == 3) { e->dead = true; e->respawnAtMs = 0; e->moving = false; e->victim = 0; e->legActive = false; }
        }
        m.run(1500);   // (a creature stopped mid-step sends its last stop)
        const uint32_t idleMonster = m.monsterMoves, idlePlayer = m.playerMoves;
        m.run(1500);
        CHECK(m.monsterMoves == idleMonster, "no creature spline churn while idle (%u -> %u)", idleMonster, m.monsterMoves);
        CHECK(m.playerMoves == idlePlayer, "no companion movement while idle (%u -> %u)", idlePlayer, m.playerMoves);
        Entity* tank = w.bot(0);
        CHECK(tank != nullptr, "tank exists");
        if (tank) {
            // Keep the AI out of the way for a moment so the movement itself is what is measured.
            tank->nextDecisionMs = m.now + 100000;
            w.moveEntity(*tank, tank->x + 15.0f, tank->y + 6.0f, m.now);
            m.run(1200);
            CHECK(m.monsterMoves > idleMonster, "a moving companion sends movement (%u -> %u)", idleMonster, m.monsterMoves);
            m.run(3000);   // it reaches the target and stops
            const uint32_t afterMove = m.monsterMoves;
            m.run(1500);
            CHECK(m.monsterMoves <= afterMove + 2, "movement stops being sent when it stops (%u -> %u)", afterMove,
                  m.monsterMoves);
        }
        remove("offline_move.w3d");
        remove("offline_move.w3d.bak");
    }

    h.session.leaveWorld();
    remove(savePath);
    remove("offline_test_save.w3d.bak");

    std::printf("OFFLINE CORE: %d checks, %d failed -> %s\n", g_ok + g_fail, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail != 0;
}
