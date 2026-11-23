// offline_class.cpp — WoWVita: the spell book, the class trainers and the cast details of the class spells for the
// client (OfflineSession side of net/offline/offline_spells.cpp).
//
//   * the book: the spells of the race and class (playercreateinfo_spell, the pack's CRSP) at the start, then what
//     the trainers teach; SMSG_INITIAL_SPELLS at login, SMSG_LEARNED_SPELL for a new one; saved ("SPB1")
//   * trainers (npc_trainer / npc_trainer_template, the pack's TRNR): CMSG_TRAINER_LIST -> SMSG_TRAINER_LIST
//     (green: can learn now, red: not yet - level or the rank before it, grey: known), CMSG_TRAINER_BUY_SPELL ->
//     the money, the spell, SMSG_TRAINER_BUY_SUCCEEDED; a trainer of another class says so
//   * a warrior stands in Battle Stance when it knows it (the client has no stance bar offline)
#include "offline_server.hpp"

#include <algorithm>
#include <cmath>

#include "offline_data.hpp"

namespace w3n {
namespace offline {

namespace {
enum : uint16_t {
    CMSG_TRAINER_LIST = 0x1B0, SMSG_TRAINER_LIST = 0x1B1, CMSG_TRAINER_BUY_SPELL = 0x1B2, SMSG_TRAINER_BUY_SUCCEEDED = 0x1B3,
    SMSG_TRAINER_BUY_FAILED = 0x1B4, SMSG_LEARNED_SPELL = 0x12B, SMSG_INITIAL_SPELLS_ = 0x12A, SMSG_ACTION_BUTTONS_ = 0x129,
    CMSG_SET_ACTION_BUTTON = 0x128, CMSG_CANCEL_AURA = 0x136, SMSG_MONSTER_MOVE_ = 0x0DD, SMSG_GOSSIP_COMPLETE_ = 0x17E,
};
constexpr uint32_t kNpcTrainer = 0x10;
constexpr float kTrainerRange = 8.0f;
const char* className(uint8_t c) {
    static const char* n[] = {"", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "", "Shaman", "Mage", "Warlock", "", "Druid"};
    return c < 12 ? n[c] : "";
}
}  // namespace

bool OfflineSession::knows(uint32_t spell) const {
    for (uint32_t s : world_.playerProfile().book) if (s == spell) return true;
    return false;
}

void OfflineSession::setupBook() {
    if (!data_) return;
    CharacterProfile& p = save_.player;
    if (p.book.empty()) {   // a new character, or one of an older save: the race and class's own spells
        auto it = data_->createSpells.find((uint32_t)p.race | ((uint32_t)p.cls << 8));
        if (it != data_->createSpells.end()) p.book = it->second;
        for (uint8_t i = 0; i < p.spellCount; ++i)
            if (std::find(p.book.begin(), p.book.end(), p.spells[i]) == p.book.end()) p.book.push_back(p.spells[i]);
    }
}

void OfflineSession::startStance(uint32_t nowMs) {   // a warrior in Battle Stance (2457) unless in another one
    Entity* e = world_.player();
    if (!e || !data_ || e->form || !knows(2457) || !data_->sx(2457)) return;
    world_.castX(*e, 2457, e->guid, nowMs, true);
}

void OfflineSession::sendSpellBook() {
    const CharacterProfile& pr = world_.playerProfile();
    Writer w;
    w.u8(0);
    const uint16_t n = (uint16_t)std::min<size_t>(pr.book.size(), 1024);
    w.u16(n);
    for (uint16_t i = 0; i < n; ++i) { w.u16((uint16_t)pr.book[i]); w.u16(i); }
    w.u16(0);   // cooldowns
    send(SMSG_INITIAL_SPELLS_, w.b);
    // the action bar: as the player left it, else Attack and the first spells that can be cast
    uint32_t bar[120] = {0};
    if (pr.actionBarSet) {
        for (int i = 0; i < 120; ++i) bar[i] = pr.actionBar[(size_t)i];
    } else {
        bar[0] = 6603;
        int k = 1;
        for (uint32_t s : pr.book) {
            if (k >= 11) break;
            const SpellX* x = data_ ? data_->sx(s) : nullptr;
            if (s == 6603 || !x || x->passive() || (x->attr & 0x80)) continue;   // (passive, hidden)
            bool skill = false;
            for (const SpellEffectX& e : x->eff) skill = skill || e.effect == 44 || e.effect == 60 || e.effect == 118 || e.effect == 47;
            if (skill) continue;
            bar[k++] = s;
        }
    }
    Writer b;
    for (int i = 0; i < 120; ++i) b.u32(bar[i]);
    send(SMSG_ACTION_BUTTONS_, b.b);
}

bool OfflineSession::trainerOf(uint64_t npc, const Trainer** t, bool* near) const {
    const Entity* v = world_.entity(npc);
    const Entity* p = world_.player();
    if (!v || !p || !data_ || !(v->npcFlags & kNpcTrainer)) return false;
    auto it = data_->trainers.find(v->entry);
    if (it == data_->trainers.end()) return false;
    *t = &it->second;
    *near = std::hypot(v->x - p->x, v->y - p->y) < kTrainerRange;
    return true;
}

void OfflineSession::sendTrainerList(uint64_t npc, uint32_t nowMs) {
    const Trainer* t = nullptr;
    bool near = false;
    if (!trainerOf(npc, &t, &near)) return;
    if (!near) { errorLine("You are too far away.", nowMs); return; }
    const CharacterProfile& pr = world_.playerProfile();
    if (t->cls != pr.cls) {
        errorLine(std::string("I can only train ") + className(t->cls) + "s.", nowMs);
        return;
    }
    const Entity* p = world_.player();
    Writer w;
    w.u64(npc);
    w.u32(0);   // TRAINER_TYPE_CLASS
    w.u32((uint32_t)t->spells.size());
    for (const TrainerSpell& s : t->spells) {
        uint8_t state = 0;   // TRAINER_SPELL_GREEN
        if (knows(s.learn)) state = 2;   // GRAY: known
        else if ((p && s.level > p->level) || (s.reqSpell && !knows(s.reqSpell))) state = 1;   // RED
        w.u32(s.teach); w.u8(state); w.u32(s.cost);
        w.u32(0); w.u32(0);   // profession dialog / button
        w.u8(s.level); w.u32(s.skill); w.u32(s.skillValue); w.u32(s.reqSpell); w.u32(0); w.u32(0);
    }
    w.cstr(std::string("Hello, ") + className(pr.cls) + "! Ready for some training?");
    send(SMSG_TRAINER_LIST, w.b);
}

bool OfflineSession::trainerHello(uint64_t npc, uint32_t nowMs) {
    const Trainer* t = nullptr;
    bool near = false;
    if (!trainerOf(npc, &t, &near)) return false;
    sendTrainerList(npc, nowMs);
    return true;
}

void OfflineSession::learn(uint32_t spell, uint32_t nowMs) {
    (void)nowMs;
    CharacterProfile& pr = world_.playerProfile();
    if (knows(spell)) return;
    pr.book.push_back(spell);
    Writer w;
    w.u16((uint16_t)spell);
    w.u16(0);
    send(SMSG_LEARNED_SPELL, w.b);
    saveSoon_ = true;
}

bool OfflineSession::handleClassSpells(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    Entity* p = world_.player();
    if (!p) return false;
    switch (op) {
        case CMSG_TRAINER_LIST: {
            const uint64_t npc = r.u64();
            if (!r.bad) sendTrainerList(npc, nowMs);
            return true;
        }
        case CMSG_TRAINER_BUY_SPELL: {
            const uint64_t npc = r.u64();
            const uint32_t teach = r.u32();
            if (r.bad) return true;
            const Trainer* t = nullptr;
            bool near = false;
            auto failed = [&](uint32_t why) { Writer w; w.u64(npc); w.u32(teach); w.u32(why); send(SMSG_TRAINER_BUY_FAILED, w.b); };
            if (!trainerOf(npc, &t, &near) || !near || t->cls != world_.playerProfile().cls) { failed(0); return true; }
            const TrainerSpell* s = nullptr;
            for (const TrainerSpell& x : t->spells) if (x.teach == teach) s = &x;
            if (!s || knows(s->learn) || s->level > p->level || (s->reqSpell && !knows(s->reqSpell))) { failed(0); return true; }
            if (p->money < s->cost) { failed(1); return true; }   // TRAINER_BUY_FAILED_NOT_ENOUGH_MONEY
            p->money -= s->cost;
            world_.syncPlayerMoney();
            learn(s->learn, nowMs);
            sendSelfFields();
            Writer w;
            w.u64(npc); w.u32(teach);
            send(SMSG_TRAINER_BUY_SUCCEEDED, w.b);
            if (s->learn == 2457) startStance(nowMs);
            return true;
        }
        case CMSG_SET_ACTION_BUTTON: {   // u8 button, u32 action | type << 24
            const uint8_t slot = r.u8();
            const uint32_t packed = r.u32();
            if (r.bad || slot >= 120) return true;
            CharacterProfile& pr = world_.playerProfile();
            if (!pr.actionBarSet) {   // (the bar it had: the one sent at login)
                pr.actionBarSet = true;
            }
            pr.actionBar[slot] = packed;
            saveSoon_ = true;
            return true;
        }
        case CMSG_CANCEL_AURA: {
            const uint32_t spell = r.u32();
            if (!r.bad) world_.cancelAuraX(*p, spell);
            return true;
        }
        default:
            return false;
    }
}

void OfflineSession::sendCharge(const Entity& p, uint32_t nowMs) {   // the client's character runs to its new place
    const Entity* me = world_.player();
    if (!me) return;
    const float d = std::hypot(me->x - me->castDestX, me->y - me->castDestY);
    Writer w;
    w.packedGuid(me->guid);
    w.f32(me->castDestX); w.f32(me->castDestY); w.f32(me->castDestZ);
    w.u32(nowMs);
    w.u8(0);
    w.u32(0x100);
    w.u32((uint32_t)std::max(100.0f, d / 25.0f * 1000.0f));   // (Charge: about 25 yd/s)
    w.u32(1);
    w.f32(me->x); w.f32(me->y); w.f32(me->z);
    send(SMSG_MONSTER_MOVE_, w.b);
    (void)p;
}

}  // namespace offline
}  // namespace w3n
