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
        else   // (a pack without playercreateinfo_spell: the sample's few spells; with it, nothing else - a shaman knew
               // the warrior's Heroic Strike before)
            for (uint8_t i = 0; i < p.spellCount; ++i)
                if (std::find(p.book.begin(), p.book.end(), p.spells[i]) == p.book.end()) p.book.push_back(p.spells[i]);
    }
    // (older saves: a paladin, hunter, shaman, warlock or druid got the warrior's Heroic Strike, Battle Shout and Taunt)
    if (p.cls != 1 && data_->createSpells.count((uint32_t)p.race | ((uint32_t)p.cls << 8)))
        p.book.erase(std::remove_if(p.book.begin(), p.book.end(), [](uint32_t s) { return s == 78 || s == 6673 || s == 355; }), p.book.end());
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

// Creature::CanTrainAndResetTalentsOf: a class trainer of our class (TRAINER_TYPE_CLASS), and we are level 10 or more
bool OfflineSession::canResetTalentsAt(uint64_t npc) const {
    const Entity* e = world_.entity(npc);
    const Entity* p = world_.player();
    if (!e || !p || !data_ || !(e->npcFlags & 0x10) || p->level < 10) return false;
    auto t = data_->trainers.find(e->entry);
    return t != data_->trainers.end() && t->second.cls == world_.playerProfile().cls;
}

bool OfflineSession::handleClassSpells(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    Entity* p = world_.player();
    if (!p) return false;
    switch (op) {
        case 0x2AA: {   // MSG_TALENT_WIPE_CONFIRM: the trainer's guid (WorldSession::HandleTalentWipeConfirmOpcode)
            const uint64_t g = r.u64();
            if (r.bad || !canResetTalentsAt(g)) return true;
            std::vector<uint32_t> gone;
            if (!world_.resetTalents(gone, nowMs)) {   // (no talents, or not the money: "you have not any talent")
                Writer w; w.u64(0); w.u32(0);
                send(0x2AA, w.b);
                return true;
            }
            for (uint32_t s : gone) { Writer w; w.u16((uint16_t)s); send(0x203, w.b); }   // SMSG_REMOVED_SPELL
            sendSelfFields();
            saveSoon_ = true;
            return true;
        }
        case 0x251: {   // CMSG_LEARN_TALENT: talent id, the rank index to learn (WorldSession::HandleLearnTalentOpcode)
            const uint32_t id = r.u32(), rank = r.u32();
            if (r.bad) return true;
            const WorldData::TalentInfo* t = data_ ? data_->talent(id) : nullptr;
            const uint8_t why = world_.learnTalent(id, rank, nowMs);
            if (why || !t) return true;   // (the client checks the same rules and says why)
            for (uint32_t k = 0; k < rank && k < t->ranks.size(); ++k) {   // SMSG_REMOVED_SPELL of the lower ranks
                Writer w;
                w.u16((uint16_t)t->ranks[k]);
                send(0x203, w.b);
            }
            Writer w;
            w.u16((uint16_t)t->ranks[rank]);
            w.u16(0);
            send(SMSG_LEARNED_SPELL, w.b);
            sendSelfFields();   // (PLAYER_CHARACTER_POINTS1, the stats a talent changed)
            saveSoon_ = true;
            return true;
        }
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

// ---------------------------------------------------------------- WoWVita: the pet bar (vMaNGOS 1.12 formats, see
// net/game_pet.cpp): SMSG_PET_SPELLS (CharmInfo::BuildActionBar: attack / follow / stay, four spells, aggressive /
// defensive / passive), CMSG_PET_ACTION (u64 pet, u32 action | type << 24, u64 target), CMSG_PET_SPELL_AUTOCAST,
// CMSG_PET_STOP_ATTACK, CMSG_PET_ABANDON, CMSG_PET_NAME_QUERY -> SMSG_PET_NAME_QUERY_RESPONSE
namespace {
enum : uint16_t {
    CMSG_PET_NAME_QUERY_ = 0x052, SMSG_PET_NAME_QUERY_RESPONSE_ = 0x053, CMSG_PET_ACTION_ = 0x175, CMSG_PET_ABANDON_ = 0x176,
    CMSG_PET_RENAME_ = 0x177, SMSG_PET_SPELLS_ = 0x179, SMSG_PET_MODE_ = 0x17A, CMSG_PET_STOP_ATTACK_ = 0x2EA,
    CMSG_PET_SPELL_AUTOCAST_ = 0x2F3,
};
constexpr uint8_t kActPassive = 0x01, kActReaction = 0x06, kActCommand = 0x07, kActDisabled = 0x81, kActEnabled = 0xC1;
uint32_t button(uint32_t action, uint8_t type) { return action | ((uint32_t)type << 24); }
}  // namespace

void OfflineSession::sendPetSpells() {
    const Entity* pet = world_.pet();
    Writer w;
    if (!pet) { w.u64(0); send(SMSG_PET_SPELLS_, w.b); return; }
    w.u64(pet->guid);
    w.u32(0);                  // duration
    w.u8(pet->petReact);
    w.u8(pet->petCommand);
    w.u8(0);
    w.u8(0);                   // flags (0x8: actions disabled)
    w.u32(button(2, kActCommand));   // attack
    w.u32(button(1, kActCommand));   // follow
    w.u32(button(0, kActCommand));   // stay
    for (int i = 0; i < 4; ++i) {
        if (i < pet->spellCount) {
            const SpellX* s = data_ ? data_->sx(pet->spells[i]) : nullptr;
            const bool passive = s && s->passive();
            w.u32(button(pet->spells[i], passive ? kActPassive : (pet->petAutocastOff & (1u << i)) ? kActDisabled : kActEnabled));
        } else {
            w.u32(0);
        }
    }
    w.u32(button(2, kActReaction));  // aggressive
    w.u32(button(1, kActReaction));  // defensive
    w.u32(button(0, kActReaction));  // passive
    w.u8(pet->spellCount);
    for (uint8_t i = 0; i < pet->spellCount; ++i) {
        const SpellX* s = data_ ? data_->sx(pet->spells[i]) : nullptr;
        const bool passive = s && s->passive();
        w.u32(button(pet->spells[i], passive ? kActPassive : (pet->petAutocastOff & (1u << i)) ? kActDisabled : kActEnabled));
    }
    w.u16(0);                  // cooldowns
    send(SMSG_PET_SPELLS_, w.b);
}

bool OfflineSession::handlePet(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    switch (op) {
        case CMSG_PET_ACTION_: {
            const uint64_t g = r.u64();
            const uint32_t act = r.u32();
            const uint64_t target = r.u64();
            const Entity* pet = world_.pet();
            if (r.bad || !pet || pet->guid != g) return true;
            const uint8_t type = (uint8_t)(act >> 24);
            const uint32_t action = act & 0xFFFFFF;
            if (type == kActCommand) {
                world_.petCommand((uint8_t)action, target, nowMs);
                if (world_.pet()) {
                    Writer m;
                    m.u64(g); m.u8(world_.pet()->petReact); m.u8(world_.pet()->petCommand); m.u8(0); m.u8(0);
                    send(SMSG_PET_MODE_, m.b);
                }
            } else if (type == kActReaction) {
                world_.setPetReact((uint8_t)action);
                Writer m;
                m.u64(g); m.u8(world_.pet()->petReact); m.u8(world_.pet()->petCommand); m.u8(0); m.u8(0);
                send(SMSG_PET_MODE_, m.b);
            } else {   // a spell of its bar, at the target the owner has
                world_.petCast(action, target, nowMs);
            }
            saveSoon_ = true;
            return true;
        }
        case CMSG_PET_SPELL_AUTOCAST_: {
            const uint64_t g = r.u64();
            const uint32_t spell = r.u32();
            const uint8_t on = r.u8();
            if (r.bad || !world_.pet() || world_.pet()->guid != g) return true;
            world_.setPetAutocast(spell, on != 0);
            saveSoon_ = true;
            return true;
        }
        case CMSG_PET_STOP_ATTACK_: {
            const uint64_t g = r.u64();
            if (!r.bad && world_.pet() && world_.pet()->guid == g) world_.petCommand(1, 0, nowMs);
            return true;
        }
        case CMSG_PET_ABANDON_: {   // a demon is dismissed; a hunter's beast is let go for good (PET_SAVE_AS_DELETED)
            const uint64_t g = r.u64();
            if (!r.bad && world_.pet() && world_.pet()->guid == g) world_.dismissPet(true);
            saveSoon_ = true;
            return true;
        }
        case CMSG_PET_RENAME_: {   // a tamed beast, once (UNIT_FLAG_PET_RENAME); a demon cannot be renamed
            const uint64_t g = r.u64();
            std::string name = r.cstr();
            Entity* pet = world_.pet();
            CharacterProfile& pr = world_.playerProfile();
            if (r.bad || !pet || pet->guid != g || !pet->petHunter || pr.petRenamed || name.size() < 2 || name.size() > 12) return true;
            for (char& ch : name) if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))) return true;
            pet->petName = name;
            pr.petName = name;
            pr.petRenamed = true;
            pet->petRenamed = true;
            Writer w;   // the new name, and the fields that make the client ask again
            w.u32(pet->petNumber); w.cstr(name); w.u32(2);
            send(SMSG_PET_NAME_QUERY_RESPONSE_, w.b);
            sendPetRenamed(pet->guid);   // (WorldSession::HandlePetRename: UNIT_FLAG_RENAME off, a new name timestamp)
            sendPetSpells();
            saveSoon_ = true;
            return true;
        }
        case CMSG_PET_NAME_QUERY_: {
            const uint32_t number = r.u32();
            r.u64();
            const Entity* pet = world_.pet();
            if (r.bad || !pet || pet->petNumber != number) return true;
            Writer w;
            w.u32(number);
            w.cstr(pet->petName);
            w.u32(world_.playerProfile().petRenamed ? 2u : 1u);   // the name's timestamp
            send(SMSG_PET_NAME_QUERY_RESPONSE_, w.b);
            return true;
        }
        default:
            return false;
    }
}

}  // namespace offline
}  // namespace w3n

namespace w3n {
namespace offline {
uint32_t OfflineSession::petNextXp(uint32_t level) {   // ObjectMgr::GetXPForPetLevel: a quarter of a player's
    static const uint32_t t[] = {0, 400, 900, 1400, 2100, 2800, 3600, 4500, 5400, 6500, 7600, 8800, 10100, 11400, 12900, 14400, 16000,
                                 17700, 19400, 21300, 23200, 25200, 27300, 29400, 31700, 34000, 36400, 38900, 41400, 44300, 47400,
                                 50800, 54500, 58600, 62800, 67100, 71600, 76100, 80800, 85700, 90700, 95800, 101000, 106300,
                                 111800, 117500, 123200, 129100, 135100, 141200, 147500, 153900, 160400, 167100, 173900, 180800,
                                 187900, 195000, 202300, 209800};
    return (level < 60 ? t[level] : 209800) / 4;
}
}  // namespace offline
}  // namespace w3n
