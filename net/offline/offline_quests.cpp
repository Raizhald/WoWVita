// offline_quests.cpp - WoWVita: quests in the offline world, the way a 1.12.1 server answers them.
//
// The data is the server database's quest_template ("QST2" pack section, tools/offline_data/export_world_sql.py):
// texts, objectives, rewards, the creatures that give and take each quest, and quest drops ("QLOT").
// The client (net/game_quests.cpp) does everything a quest needs on its side; this file only produces the packets
// vMaNGOS would: quest giver marks, the quest list / gossip, details, accept, kill and item progress,
// request items, offer reward, the reward itself.
//
// Kinds the offline core cannot finish (exploration, game objects, spells, scripted events, timed quests) are
// left out by the exporter; the quest log lives in the save (OfflineSaveData::quests, ::rewarded).
#include "offline_server.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../game.hpp"
#include "offline_data.hpp"

namespace w3n {
namespace offline {

namespace {
enum : uint16_t {
    CMSG_QUEST_QUERY = 0x05C, SMSG_QUEST_QUERY_RESPONSE = 0x05D,
    CMSG_GOSSIP_HELLO = 0x17B, CMSG_GOSSIP_SELECT_OPTION = 0x17C, SMSG_GOSSIP_MESSAGE = 0x17D, SMSG_GOSSIP_COMPLETE = 0x17E,
    CMSG_NPC_TEXT_QUERY = 0x17F, SMSG_NPC_TEXT_UPDATE = 0x180,
    CMSG_QUESTGIVER_STATUS_QUERY = 0x182, SMSG_QUESTGIVER_STATUS = 0x183, CMSG_QUESTGIVER_HELLO = 0x184,
    SMSG_QUESTGIVER_QUEST_LIST = 0x185, CMSG_QUESTGIVER_QUERY_QUEST = 0x186, SMSG_QUESTGIVER_QUEST_DETAILS = 0x188,
    CMSG_QUESTGIVER_ACCEPT_QUEST = 0x189, CMSG_QUESTGIVER_COMPLETE_QUEST = 0x18A, SMSG_QUESTGIVER_REQUEST_ITEMS = 0x18B,
    CMSG_QUESTGIVER_REQUEST_REWARD = 0x18C, SMSG_QUESTGIVER_OFFER_REWARD = 0x18D, CMSG_QUESTGIVER_CHOOSE_REWARD = 0x18E,
    SMSG_QUESTGIVER_QUEST_INVALID = 0x18F, CMSG_QUESTGIVER_CANCEL = 0x190, SMSG_QUESTGIVER_QUEST_COMPLETE = 0x191,
    CMSG_QUESTLOG_REMOVE_QUEST = 0x194, SMSG_QUESTLOG_FULL = 0x195, SMSG_QUESTUPDATE_COMPLETE = 0x198,
    SMSG_QUESTUPDATE_ADD_KILL = 0x199, SMSG_QUESTUPDATE_ADD_ITEM = 0x19A, SMSG_LIST_INVENTORY = 0x19F,
};
// client QuestMark (net/game.hpp)
constexpr uint32_t kMarkNone = 0, kMarkIncomplete = 3, kMarkAvailable = 5, kMarkReward = 7;
constexpr uint16_t kQuestLog1 = 198;   // PLAYER_QUEST_LOG_1_1: id, kills (6 bits each) | state << 24, timer
constexpr int kLogSize = 20;
constexpr uint32_t kGreetingTextId = 0x57560000u;   // our NPC_TEXT ids: this | creature entry
constexpr float kQuestReach = 15.0f;

uint32_t itemDisplay(const WorldData* d, uint32_t item) {
    const ItemTemplate* t = d ? d->item(item) : nullptr;
    return t ? t->displayId : 0;
}
}  // namespace

// ---------------------------------------------------------------- state
const QuestFull* OfflineSession::questDef(uint32_t id) const { return data_ ? data_->questFull(id) : nullptr; }

int OfflineSession::questSlot(uint32_t id) const {
    for (size_t i = 0; i < save_.quests.size(); ++i)
        if (save_.quests[i].id == id) return (int)i;
    return -1;
}

bool OfflineSession::questRewarded(uint32_t id) const {
    return std::find(save_.rewarded.begin(), save_.rewarded.end(), id) != save_.rewarded.end();
}

bool OfflineSession::questDone(const QuestLogEntry& e) const {
    const QuestFull* q = questDef(e.id);
    if (!q) return false;
    for (int k = 0; k < 4; ++k)
        if (q->killEntry[k] && q->killCount[k] && e.kills[k] < q->killCount[k]) return false;
    for (int k = 0; k < 4; ++k)
        if (q->itemEntry[k] && q->itemCount[k] && world_.countPlayerItem(q->itemEntry[k]) < q->itemCount[k]) return false;
    if (q->money < 0) {
        const Entity* p = world_.player();
        if (!p || p->money < (uint32_t)(-q->money)) return false;
    }
    if (isExploreQuest(q->id) && !e.complete) return false;   // WoWVita: the place not reached yet
    return true;
}

// WoWVita: exploration quests - their area trigger (CMSG_AREATRIGGER, areatrigger_involvedrelation)
bool OfflineSession::isExploreQuest(uint32_t id) const {
    if (!data_) return false;
    for (const auto& [t, q] : data_->questTriggers)
        if (q == id) return true;
    return false;
}

void OfflineSession::questAreaTrigger(uint32_t trigger) {
    if (!data_) return;
    auto range = data_->questTriggers.equal_range(trigger);
    for (auto it = range.first; it != range.second; ++it)
        for (QuestLogEntry& e : save_.quests) {
            if (e.id != it->second || e.complete) continue;
            const bool before = questDone(e);
            e.complete = true;
            questFieldsDirty_ = true;
            saveSoon_ = true;
            if (!before && questDone(e)) { Writer c; c.u32(e.id); send(SMSG_QUESTUPDATE_COMPLETE, c.b); }
        }
}

bool OfflineSession::questCanTake(const QuestFull& q) const {
    const Entity* p = world_.player();
    if (!p) return false;
    const CharacterProfile& me = world_.playerProfile();
    if (questSlot(q.id) >= 0) return false;
    if (questRewarded(q.id) && !(q.special & 1)) return false;   // (SpecialFlags 1: repeatable)
    if (p->level < q.minLevel) return false;
    if (q.races && !(q.races & (1u << (me.race - 1)))) return false;
    if (q.classes && !(q.classes & (1u << (me.cls - 1)))) return false;
    if (q.prev > 0 && !questRewarded((uint32_t)q.prev)) return false;
    if (q.prev < 0 && questSlot((uint32_t)(-q.prev)) < 0) return false;
    return true;
}

uint32_t OfflineSession::questXp(const QuestFull& q) const {
    // Quest::XPValue (CMaNGOS classic): RewMoneyMaxLevel / 0.6 at full value, less once the quest is grey
    const Entity* p = world_.player();
    if (!p || !q.moneyMaxLevel || p->level >= 60) return 0;
    float full = q.moneyMaxLevel / 0.6f;
    const int diff = (int)p->level - (int)q.level;
    if (diff > 9) full *= 0.1f;
    else if (diff == 9) full *= 0.2f;
    else if (diff == 8) full *= 0.4f;
    else if (diff == 7) full *= 0.6f;
    else if (diff == 6) full *= 0.8f;
    uint32_t xp = (uint32_t)full;
    if (xp > 1000) xp = (xp + 25) / 50 * 50;   // (the original rounds the larger values)
    else if (xp > 500) xp = (xp + 12) / 25 * 25;
    else if (xp > 100) xp = (xp + 5) / 10 * 10;
    else if (xp > 10) xp = (xp + 2) / 5 * 5;
    return xp;
}

uint32_t OfflineSession::questMarkFor(uint32_t entry, bool go) const {
    if (!data_) return kMarkNone;
    uint32_t mark = kMarkNone;
    const auto& ended = go ? data_->goQuestsEnded : data_->questsEnded;
    const auto& started = go ? data_->goQuestsStarted : data_->questsStarted;
    auto en = ended.find(entry);
    if (en != ended.end())
        for (uint32_t id : en->second) {
            const int s = questSlot(id);
            if (s < 0) continue;
            if (questDone(save_.quests[(size_t)s])) return kMarkReward;
            mark = kMarkIncomplete;
        }
    auto st = started.find(entry);
    if (st != started.end())
        for (uint32_t id : st->second)
            if (const QuestFull* q = questDef(id))
                if (questCanTake(*q)) return kMarkAvailable;
    return mark;
}

// The quests a giver shows: those ending here that are in the log, then those it can give now.
std::vector<std::pair<uint32_t, uint32_t>> OfflineSession::questMenu(uint32_t entry, bool go) const {
    std::vector<std::pair<uint32_t, uint32_t>> out;   // quest id, icon (the client's QuestMark)
    if (!data_) return out;
    const auto& ended = go ? data_->goQuestsEnded : data_->questsEnded;
    const auto& started = go ? data_->goQuestsStarted : data_->questsStarted;
    auto en = ended.find(entry);
    if (en != ended.end())
        for (uint32_t id : en->second) {
            const int s = questSlot(id);
            if (s >= 0) out.push_back({id, questDone(save_.quests[(size_t)s]) ? kMarkReward : kMarkIncomplete});
        }
    auto st = started.find(entry);
    if (st != started.end())
        for (uint32_t id : st->second) {
            const QuestFull* q = questDef(id);
            if (q && questCanTake(*q) && std::none_of(out.begin(), out.end(), [&](auto& x) { return x.first == id; }))
                out.push_back({id, kMarkAvailable});
        }
    if (out.size() > 20) out.resize(20);
    return out;
}

// ---------------------------------------------------------------- the client's fields
void OfflineSession::questLogFields(std::vector<std::pair<uint16_t, uint32_t>>& f) const {
    for (int s = 0; s < kLogSize; ++s) {
        const uint16_t base = (uint16_t)(kQuestLog1 + 3 * s);
        if (s >= (int)save_.quests.size()) {
            f.push_back({base, 0}); f.push_back({(uint16_t)(base + 1), 0}); f.push_back({(uint16_t)(base + 2), 0});
            continue;
        }
        const QuestLogEntry& e = save_.quests[(size_t)s];
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= (uint32_t)std::min<uint16_t>(e.kills[k], 63) << (6 * k);
        if (questDone(e)) v |= 1u << 24;
        f.push_back({base, e.id});
        f.push_back({(uint16_t)(base + 1), v});
        f.push_back({(uint16_t)(base + 2), 0});
    }
}

// ---------------------------------------------------------------- packets
OfflineSession::Giver OfflineSession::giverOf(uint64_t guid) const {
    Giver g;
    if ((guid >> 48) == 0xF110) {   // a game object of ours
        const uint32_t idx = (uint32_t)(guid & 0xFFFFFF);
        if (!data_ || idx == 0 || idx > data_->goSpawns.size() || idx > goActiveFlag_.size() || !goActiveFlag_[idx - 1]) return g;
        const GoSpawn& s = data_->goSpawns[idx - 1];
        g.ok = true; g.go = true; g.entry = s.entry; g.x = s.x; g.y = s.y;
        return g;
    }
    const Entity* e = world_.entity(guid);
    if (!e || !e->alive()) return g;
    g.ok = true; g.entry = e->entry; g.x = e->x; g.y = e->y;
    return g;
}

void OfflineSession::sendQuestStatus(uint64_t npc) {
    const Giver gv = giverOf(npc);
    Writer w;
    w.u64(npc);
    const uint32_t mark = gv.ok ? questMarkFor(gv.entry, gv.go) : kMarkNone;
    w.u32(mark);
    send(SMSG_QUESTGIVER_STATUS, w.b);
    if (trace && gv.ok && mark) trace("quest mark " + std::to_string(mark) + " for " + std::to_string(gv.entry) + (gv.go ? " (object)" : ""));
}

void OfflineSession::sendQuestDetails(uint64_t npc, const QuestFull& q) {
    Writer w;
    w.u64(npc);
    w.u32(q.id);
    w.cstr(q.title);
    w.cstr(q.details);
    w.cstr(q.objectives);
    w.u32(0);   // auto finish
    uint32_t nc = 0, nr = 0;
    for (int k = 0; k < 6; ++k) if (q.choiceItem[k]) ++nc;
    for (int k = 0; k < 4; ++k) if (q.rewItem[k]) ++nr;
    w.u32(nc);
    for (int k = 0; k < 6; ++k)
        if (q.choiceItem[k]) { w.u32(q.choiceItem[k]); w.u32(q.choiceCount[k]); w.u32(itemDisplay(data_, q.choiceItem[k])); }
    w.u32(nr);
    for (int k = 0; k < 4; ++k)
        if (q.rewItem[k]) { w.u32(q.rewItem[k]); w.u32(q.rewCount[k]); w.u32(itemDisplay(data_, q.rewItem[k])); }
    w.u32(q.money > 0 ? (uint32_t)q.money : 0);
    w.u32(q.rewSpell);   // reward spell (its icon)
    w.u32(0);   // emotes
    send(SMSG_QUESTGIVER_QUEST_DETAILS, w.b);
}

void OfflineSession::sendQuestRequestItems(uint64_t npc, const QuestFull& q, bool done) {
    Writer w;
    w.u64(npc);
    w.u32(q.id);
    w.cstr(q.title);
    w.cstr(q.requestItems.empty() ? q.objectives : q.requestItems);
    w.u32(0); w.u32(0);   // emote delay, emote
    w.u32(0);             // close on cancel
    w.u32(q.money < 0 ? (uint32_t)(-q.money) : 0);
    uint32_t n = 0;
    for (int k = 0; k < 4; ++k) if (q.itemEntry[k] && q.itemCount[k]) ++n;
    w.u32(n);
    for (int k = 0; k < 4; ++k)
        if (q.itemEntry[k] && q.itemCount[k]) { w.u32(q.itemEntry[k]); w.u32(q.itemCount[k]); w.u32(itemDisplay(data_, q.itemEntry[k])); }
    const uint32_t ok = done ? 3u : 0u;
    w.u32(2); w.u32(ok); w.u32(4); w.u32(8);   // the completable flags (vMaNGOS: 0x02, 0x03 | 0x00, 0x04, 0x08)
    send(SMSG_QUESTGIVER_REQUEST_ITEMS, w.b);
}

void OfflineSession::sendQuestOfferReward(uint64_t npc, const QuestFull& q) {
    Writer w;
    w.u64(npc);
    w.u32(q.id);
    w.cstr(q.title);
    w.cstr(q.offerReward.empty() ? q.details : q.offerReward);
    w.u32(0);   // auto finish
    w.u32(0);   // emotes
    uint32_t nc = 0, nr = 0;
    for (int k = 0; k < 6; ++k) if (q.choiceItem[k]) ++nc;
    for (int k = 0; k < 4; ++k) if (q.rewItem[k]) ++nr;
    w.u32(nc);
    for (int k = 0; k < 6; ++k)
        if (q.choiceItem[k]) { w.u32(q.choiceItem[k]); w.u32(q.choiceCount[k]); w.u32(itemDisplay(data_, q.choiceItem[k])); }
    w.u32(nr);
    for (int k = 0; k < 4; ++k)
        if (q.rewItem[k]) { w.u32(q.rewItem[k]); w.u32(q.rewCount[k]); w.u32(itemDisplay(data_, q.rewItem[k])); }
    w.u32(q.money > 0 ? (uint32_t)q.money : 0);
    w.u32(q.flags);
    w.u32(q.rewSpell);   // reward spell
    send(SMSG_QUESTGIVER_OFFER_REWARD, w.b);
}

void OfflineSession::sendQuestQuery(uint32_t id) {
    const QuestFull* q = questDef(id);
    if (!q) return;
    Writer w;
    w.u32(q->id);
    w.u32(2);                 // method
    w.u32(q->level);
    w.u32(q->zoneOrSort);
    w.u32(q->type);
    w.u32(0); w.u32(0); w.u32(0); w.u32(0);   // reputation objective
    w.u32(q->nextInChain);
    w.u32(q->money > 0 ? (uint32_t)q->money : 0);
    w.u32(q->moneyMaxLevel);
    w.u32(q->rewSpell);       // reward spell
    w.u32(q->srcItem);
    w.u32(q->flags);
    for (int k = 0; k < 4; ++k) { w.u32(q->rewItem[k]); w.u32(q->rewCount[k]); }
    for (int k = 0; k < 6; ++k) { w.u32(q->choiceItem[k]); w.u32(q->choiceCount[k]); }
    w.u32(0); w.f32(0); w.f32(0); w.u32(0);   // point of interest
    w.cstr(q->title);
    w.cstr(q->objectives);
    w.cstr(q->details);
    w.cstr(q->endText);
    for (int k = 0; k < 4; ++k) {
        const int32_t c = (int32_t)q->killEntry[k];   // an object (< 0) goes on the wire as entry | 0x80000000
        w.u32(c < 0 ? ((uint32_t)(-c) | 0x80000000u) : (uint32_t)c);
        w.u32(q->killEntry[k] ? q->killCount[k] : 0);
        w.u32(q->itemEntry[k]);
        w.u32(q->itemEntry[k] ? q->itemCount[k] : 0);
    }
    for (int k = 0; k < 4; ++k) w.cstr(q->objText[k]);
    send(SMSG_QUEST_QUERY_RESPONSE, w.b);
}

// The giver's menu: a quest list (quests only), or gossip (a greeting, a vendor option, the quests).
bool OfflineSession::questHello(uint64_t npc, uint16_t op, bool vendor) {
    const Giver gv = giverOf(npc);
    if (!gv.ok || !data_) return false;
    const auto menu = questMenu(gv.entry, gv.go);
    const Entity* npcE = gv.go ? nullptr : world_.entity(npc);
    const bool inn = npcE && (npcE->npcFlags & 0x80u);   // WoWVita: an innkeeper ("Make this inn your home.")
    const bool fm = npcE && (npcE->npcFlags & 0x8u) && taxiNodeFor(*npcE);   // WoWVita: a flight master with quests
    const bool trainer = npcE && (npcE->npcFlags & 0x10u) && data_->trainers.count(npcE->entry);   // WoWVita: a class trainer
    if (menu.empty() && !inn) return false;   // nothing to do with quests: the caller opens the vendor (or says so)
    const auto gr = gv.go ? data_->greetings.end() : data_->greetings.find(gv.entry);
    if (!vendor && !inn && !fm && !trainer && op == CMSG_QUESTGIVER_HELLO) {
        // one quest and nothing else: straight to it, as the original does
        if (menu.size() == 1) {
            const QuestFull* q = questDef(menu[0].first);
            if (q && menu[0].second == kMarkAvailable) { sendQuestDetails(npc, *q); return true; }
            if (q) { const int s = questSlot(q->id); sendQuestRequestItems(npc, *q, s >= 0 && questDone(save_.quests[(size_t)s])); return true; }
        }
        Writer w;
        w.u64(npc);
        w.cstr(gr != data_->greetings.end() ? gr->second : std::string("Greetings, $N."));
        w.u32(0); w.u32(0);   // emote delay, emote
        w.u8((uint8_t)menu.size());
        for (const auto& [id, icon] : menu) {
            const QuestFull* q = questDef(id);
            w.u32(id); w.u32(icon); w.u32(q ? q->level : 0); w.cstr(q ? q->title : std::string());
        }
        send(SMSG_QUESTGIVER_QUEST_LIST, w.b);
        return true;
    }
    Writer w;
    w.u64(npc);
    w.u32(kGreetingTextId | (gv.entry & 0xFFFF));
    w.u32((vendor ? 1u : 0u) + (inn ? 1u : 0u) + (fm ? 1u : 0u) + (trainer ? 1u : 0u));
    if (trainer) { w.u32(kGossipTrain); w.u8(3); w.u8(0); w.cstr("I would like to train."); }   // WoWVita
    if (fm) { w.u32(kGossipTaxi); w.u8(2); w.u8(0); w.cstr("Show me where I can fly."); }   // WoWVita: the flight master
    if (inn) { w.u32(kGossipBind); w.u8(5); w.u8(0); w.cstr("Make this inn your home."); }   // WoWVita: the binder option
    if (vendor) { w.u32(1); w.u8(1); w.u8(0); w.cstr("I want to browse your goods."); }   // option 1: the vendor
    w.u32((uint32_t)menu.size());
    for (const auto& [id, icon] : menu) {
        const QuestFull* q = questDef(id);
        w.u32(id); w.u32(icon); w.u32(q ? q->level : 0); w.cstr(q ? q->title : std::string());
    }
    send(SMSG_GOSSIP_MESSAGE, w.b);
    return true;
}

// ---------------------------------------------------------------- progress
bool OfflineSession::questWantsObject(uint32_t goEntry) const {
    const uint32_t key = (uint32_t)(-(int32_t)goEntry);
    for (const QuestLogEntry& e : save_.quests)
        if (const QuestFull* q = questDef(e.id))
            for (int k = 0; k < 4; ++k)
                if (q->killEntry[k] == key && q->killCount[k] && e.kills[k] < q->killCount[k]) return true;
    return false;
}

void OfflineSession::questKill(uint32_t entry, uint64_t guid, bool go) {
    const uint32_t key = go ? (uint32_t)(-(int32_t)entry) : entry;   // (objects: -entry, as quest_template has it)
    for (QuestLogEntry& e : save_.quests) {
        const QuestFull* q = questDef(e.id);
        if (!q) continue;
        for (int k = 0; k < 4; ++k) {
            if (q->killEntry[k] != key || !q->killCount[k] || e.kills[k] >= q->killCount[k]) continue;
            const bool before = questDone(e);
            ++e.kills[k];
            Writer w;
            w.u32(q->id); w.u32(go ? (entry | 0x80000000u) : entry); w.u32(e.kills[k]); w.u32(q->killCount[k]); w.u64(guid);
            send(SMSG_QUESTUPDATE_ADD_KILL, w.b);
            if (!before && questDone(e)) { Writer c; c.u32(q->id); send(SMSG_QUESTUPDATE_COMPLETE, c.b); }
            questFieldsDirty_ = true;
            saveSoon_ = true;
        }
    }
}

void OfflineSession::questItem(uint32_t item, uint32_t count) {
    for (QuestLogEntry& e : save_.quests) {
        const QuestFull* q = questDef(e.id);
        if (!q) continue;
        for (int k = 0; k < 4; ++k) {
            if (q->itemEntry[k] != item || !q->itemCount[k]) continue;
            Writer w;
            w.u32(item); w.u32(count);
            send(SMSG_QUESTUPDATE_ADD_ITEM, w.b);
            const uint32_t have = world_.countPlayerItem(item);
            if (have >= q->itemCount[k] && have - count < q->itemCount[k] && questDone(e)) {
                Writer c; c.u32(q->id); send(SMSG_QUESTUPDATE_COMPLETE, c.b);
            }
            questFieldsDirty_ = true;
        }
    }
}

bool OfflineSession::questWantsItem(uint32_t item) const {
    for (const QuestLogEntry& e : save_.quests) {
        const QuestFull* q = questDef(e.id);
        if (!q) continue;
        for (int k = 0; k < 4; ++k)
            if (q->itemEntry[k] == item && q->itemCount[k] && world_.countPlayerItem(item) < q->itemCount[k]) return true;
    }
    return false;
}

// ---------------------------------------------------------------- client packets
bool OfflineSession::handleQuest(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    Entity* p = world_.player();
    if (!p || !data_) return false;
    auto npcNear = [&](uint64_t g) -> Giver {
        Giver gv = giverOf(g);
        if (gv.ok && std::hypot(gv.x - p->x, gv.y - p->y) > kQuestReach) gv.ok = false;
        return gv;
    };
    auto gives = [](const QuestFull& q, const Giver& gv) {
        const auto& v = gv.go ? q.goStarters : q.starters;
        return std::find(v.begin(), v.end(), gv.entry) != v.end();
    };
    auto takes = [](const QuestFull& q, const Giver& gv) {
        const auto& v = gv.go ? q.goEnders : q.enders;
        return std::find(v.begin(), v.end(), gv.entry) != v.end();
    };
    switch (op) {
        case CMSG_QUESTGIVER_STATUS_QUERY: {
            const uint64_t g = r.u64();
            if (!r.bad) sendQuestStatus(g);
            return true;
        }
        case CMSG_QUEST_QUERY: {
            const uint32_t id = r.u32();
            if (!r.bad) sendQuestQuery(id);
            return true;
        }
        case CMSG_NPC_TEXT_QUERY: {
            const uint32_t id = r.u32();
            if (r.bad) return true;
            std::string text = "Greetings, $N.";
            if ((id & 0xFFFF0000u) == kGreetingTextId)
                for (const auto& [entry, t] : data_->greetings)
                    if ((entry & 0xFFFF) == (id & 0xFFFF)) { text = t; break; }
            Writer w;
            w.u32(id);
            for (int i = 0; i < 8; ++i) {
                w.f32(i == 0 ? 1.0f : 0.0f);
                w.cstr(i == 0 ? text : std::string());
                w.cstr(i == 0 ? text : std::string());
                w.u32(0);
                for (int k = 0; k < 6; ++k) w.u32(0);
            }
            send(SMSG_NPC_TEXT_UPDATE, w.b);
            return true;
        }
        case CMSG_GOSSIP_SELECT_OPTION: {
            const uint64_t g = r.u64();
            const uint32_t opt = r.u32();
            if (r.bad) return true;
            const Giver gv = npcNear(g);
            const Entity* e = gv.ok && !gv.go ? world_.entity(g) : nullptr;
            if (e && opt == 1 && (e->npcFlags & 4)) { send(SMSG_GOSSIP_COMPLETE); sendListInventory(g); }
            if (e && opt == kGossipTrain && (e->npcFlags & 0x10)) { send(SMSG_GOSSIP_COMPLETE); sendTrainerList(g, nowMs); }   // WoWVita
            return true;
        }
        case CMSG_QUESTGIVER_QUERY_QUEST: {
            const uint64_t g = r.u64();
            const uint32_t id = r.u32();
            if (r.bad) return true;
            const Giver gv = npcNear(g);
            const QuestFull* q = questDef(id);
            if (!gv.ok || !q) return true;
            if (gives(*q, gv) && questCanTake(*q)) sendQuestDetails(g, *q);
            return true;
        }
        case CMSG_QUESTGIVER_ACCEPT_QUEST: {
            const uint64_t g = r.u64();
            const uint32_t id = r.u32();
            if (r.bad) return true;
            const Giver gv = npcNear(g);
            const QuestFull* q = questDef(id);
            if (!gv.ok || !q || !gives(*q, gv)) return true;
            if (!questCanTake(*q)) { Writer w; w.u32(questSlot(id) >= 0 ? 13u : 2u); send(SMSG_QUESTGIVER_QUEST_INVALID, w.b); return true; }
            if ((int)save_.quests.size() >= kLogSize) { send(SMSG_QUESTLOG_FULL); return true; }
            if (q->srcItem && world_.countPlayerItem(q->srcItem) == 0) {
                int why = 0;
                if (!world_.buyItem(q->srcItem, q->srcCount ? q->srcCount : 1, 0, nowMs, &why)) {
                    Writer w; w.u32(4u); send(SMSG_QUESTGIVER_QUEST_INVALID, w.b);   // inventory full
                    return true;
                }
            }
            QuestLogEntry le;
            le.id = id;
            save_.quests.push_back(le);
            questFieldsDirty_ = true;
            saveSoon_ = true;
            send(SMSG_GOSSIP_COMPLETE);
            if (trace) trace("quest accepted: " + std::to_string(id) + " " + q->title);
            return true;
        }
        case CMSG_QUESTGIVER_COMPLETE_QUEST: {
            const uint64_t g = r.u64();
            const uint32_t id = r.u32();
            if (r.bad) return true;
            const Giver gv = npcNear(g);
            const QuestFull* q = questDef(id);
            const int s = questSlot(id);
            if (!gv.ok || !q || s < 0 || !takes(*q, gv)) return true;
            const bool done = questDone(save_.quests[(size_t)s]);
            bool needsItems = q->money < 0;
            for (int k = 0; k < 4; ++k) needsItems = needsItems || (q->itemEntry[k] && q->itemCount[k]);
            if (done && !needsItems) sendQuestOfferReward(g, *q);
            else sendQuestRequestItems(g, *q, done);
            return true;
        }
        case CMSG_QUESTGIVER_REQUEST_REWARD: {
            const uint64_t g = r.u64();
            const uint32_t id = r.u32();
            if (r.bad) return true;
            const QuestFull* q = questDef(id);
            const int s = questSlot(id);
            if (npcNear(g).ok && q && s >= 0 && questDone(save_.quests[(size_t)s])) sendQuestOfferReward(g, *q);
            return true;
        }
        case CMSG_QUESTGIVER_CHOOSE_REWARD: {
            const uint64_t g = r.u64();
            const uint32_t id = r.u32();
            const uint32_t choice = r.u32();
            if (r.bad) return true;
            const Giver gv = npcNear(g);
            const QuestFull* q = questDef(id);
            const int s = questSlot(id);
            if (!gv.ok || !q || s < 0 || !questDone(save_.quests[(size_t)s]) || !takes(*q, gv))
                return true;
            // room for the rewards (the 16 backpack slots)
            uint32_t newSlots = 0;
            for (int k = 0; k < 4; ++k) if (q->rewItem[k] && !world_.countPlayerItem(q->rewItem[k])) ++newSlots;
            if (choice < 6 && q->choiceItem[choice]) ++newSlots;
            uint32_t freed = 0;
            for (int k = 0; k < 4; ++k) if (q->itemEntry[k] && q->itemCount[k] && world_.countPlayerItem(q->itemEntry[k]) <= q->itemCount[k]) ++freed;
            if (world_.playerProfile().bagCount + newSlots > 16 + freed) { Writer w; w.u32(4u); send(SMSG_QUESTGIVER_QUEST_INVALID, w.b); return true; }
            // take what the quest wanted, give what it pays
            for (int k = 0; k < 4; ++k)
                if (q->itemEntry[k] && q->itemCount[k]) world_.removePlayerItem(q->itemEntry[k], q->itemCount[k]);
            if (q->srcItem) world_.removePlayerItem(q->srcItem, 255);
            if (q->money < 0) p->money -= std::min<uint32_t>(p->money, (uint32_t)(-q->money));
            uint32_t given = 0;
            Writer items;
            for (int k = 0; k < 4; ++k)
                if (q->rewItem[k]) { world_.addItemToPlayer(q->rewItem[k], q->rewCount[k] ? q->rewCount[k] : 1, nowMs); items.u32(q->rewItem[k]); items.u32(q->rewCount[k]); ++given; }
            if (choice < 6 && q->choiceItem[choice]) {
                world_.addItemToPlayer(q->choiceItem[choice], q->choiceCount[choice] ? q->choiceCount[choice] : 1, nowMs);
                items.u32(q->choiceItem[choice]); items.u32(q->choiceCount[choice]); ++given;
            }
            const uint32_t xp = questXp(*q);
            const uint32_t money = q->money > 0 ? (uint32_t)q->money : 0;
            if (money) p->money += money;
            world_.syncPlayerMoney();
            save_.quests.erase(save_.quests.begin() + s);
            if (!questRewarded(id)) save_.rewarded.push_back(id);
            Writer w;
            w.u32(id); w.u32(3); w.u32(xp); w.u32(money); w.u32(given);
            w.raw(items.b.data(), items.b.size());
            send(SMSG_QUESTGIVER_QUEST_COMPLETE, w.b);
            if (xp) world_.addXp(*p, xp, nowMs);
            // CMaNGOS Player::RewardQuest: RewSpellCast, else RewSpell, cast on the player (a learning spell teaches)
            if (const uint32_t rs = q->rewSpellCast ? q->rewSpellCast : q->rewSpell) world_.rewardSpell(*p, rs, nowMs);
            questFieldsDirty_ = true;
            saveSoon_ = true;
            sendInventory();
            sendSelfFields();
            if (trace) trace("quest done: " + std::to_string(id) + " " + q->title + " xp " + std::to_string(xp));
            // the next one in the chain, from the same NPC: offered at once, as the original does
            if (const QuestFull* next = questDef(q->nextInChain))
                if (gives(*next, gv) && questCanTake(*next)) sendQuestDetails(g, *next);
            return true;
        }
        case CMSG_QUESTGIVER_CANCEL:
            return true;
        case CMSG_QUESTLOG_REMOVE_QUEST: {
            const uint8_t slot = r.u8();
            if (r.bad || slot >= save_.quests.size()) return true;
            const QuestFull* q = questDef(save_.quests[slot].id);
            if (q && q->srcItem) world_.removePlayerItem(q->srcItem, 255);
            save_.quests.erase(save_.quests.begin() + slot);
            questFieldsDirty_ = true;
            saveSoon_ = true;
            sendInventory();
            return true;
        }
        default:
            return false;
    }
}

}  // namespace offline
}  // namespace w3n
