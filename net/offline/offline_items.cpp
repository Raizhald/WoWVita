// offline_items.cpp — WoWVita: food, drink and potions in the offline world, as the original 1.12 server uses them.
//
// The pack's "ITX1" carries what an item does when used (spell_template = Spell.dbc, item_template): food (aura 84) and
// drink (aura 85) give their total over the spell's duration (the tooltip's "Restores N health over 18 sec") while the
// character sits; moving or being hit stands it up and ends the meal (AuraInterruptFlags NOT_SEATED). They cannot be
// started in combat (SPELL_ATTR_CANT_USED_IN_COMBAT). Potions heal (effect 10) or give mana (effect 30) at once.
// One item of the stack is used up; items of one category share its cooldown (potions 2 minutes).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "offline_server.hpp"
#include "../objects.hpp"

namespace w3n {
namespace offline {

namespace {
constexpr uint32_t kEatTickMs = 3000;
}

bool OfflineSession::inCombat() const {
    const Entity* p = world_.player();
    if (!p) return false;
    for (int i = 0; i < kMaxEntities; ++i) {
        const Entity* e = world_.byIndex(i);
        if (e && e->used && e->alive() && e->objType == 3 && e->victim == p->guid) return true;
    }
    return false;
}

bool OfflineSession::useConsumable(int index, uint32_t nowMs) {
    Entity* p = world_.player();
    const CharacterProfile& pr = world_.playerProfile();
    if (!p || !data_ || index < 0 || index >= pr.bagCount) return false;
    const uint32_t entry = pr.bag[index].entry;
    const ItemExt* x = data_->ext(entry);
    if (!x || !x->kind) return false;
    if (ghost_ || !p->alive()) { errorLine("You can't do that while dead.", nowMs); return true; }
    if (flight_.active) { errorLine("You can't do that while flying.", nowMs); return true; }
    if ((x->flags & 1) && inCombat()) { errorLine("You can't do that while in combat.", nowMs); return true; }
    const uint32_t key = x->category ? x->category : 0x10000u + entry;
    auto cd = itemReady_.find(key);
    if (cd != itemReady_.end() && (int32_t)(cd->second - nowMs) > 0) { errorLine("Item is not ready yet.", nowMs); return true; }
    if (x->cooldownMs) itemReady_[key] = nowMs + x->cooldownMs;
    // the spell's visual and its name in the combat log
    sendSpellGo(*p, x->spell, p->guid, 0);
    if (x->kind & (4 | 8)) {   // a potion: at once
        if (x->kind & 4) {
            const int32_t amount = (int32_t)(x->hpMin + (x->hpMax > x->hpMin ? (uint32_t)rand() % (x->hpMax - x->hpMin + 1) : 0));
            world_.applyHeal(*p, *p, amount, x->spell, nowMs);
        }
        if ((x->kind & 8) && p->powerType == 0) {
            const int32_t amount = (int32_t)(x->manaMin + (x->manaMax > x->manaMin ? (uint32_t)rand() % (x->manaMax - x->manaMin + 1) : 0));
            p->power = std::min<int32_t>(p->power + amount, (int32_t)p->maxPower);
        }
    }
    if ((x->kind & (1 | 2)) && x->durationMs) {   // food / drink: sit down, a little every 3 s
        if (eat_.active) stopEating(nowMs);
        eat_ = Eating();
        eat_.active = true;
        eat_.spell = x->spell;
        eat_.startMs = nowMs;
        eat_.endMs = nowMs + x->durationMs;
        eat_.nextTickMs = nowMs + kEatTickMs;
        const uint32_t ticks = std::max<uint32_t>(1, x->durationMs / kEatTickMs);
        eat_.hpLeft = (x->kind & 1) ? x->hpMax : 0;
        eat_.manaLeft = (x->kind & 2) && p->powerType == 0 ? x->manaMax : 0;
        eat_.hpTick = (eat_.hpLeft + ticks - 1) / ticks;
        eat_.manaTick = (eat_.manaLeft + ticks - 1) / ticks;
        eat_.x = p->x; eat_.y = p->y;
        world_.addTimedAura(*p, x->spell, x->durationMs);
        world_.stopAttack(*p, nowMs);
    }
    if (x->flags & 2) {   // one of the stack is used up
        const uint8_t before = pr.bagCount;
        world_.takeFromBag(index, 1);
        if (world_.playerProfile().bagCount != before) bankChanged({}, index, before);   // the slot emptied: the rest moved up
        else sendInventory();
    }
    sendSelfFields();
    saveSoon_ = true;
    return true;
}

void OfflineSession::stopEating(uint32_t nowMs) {
    (void)nowMs;
    if (!eat_.active) return;
    if (Entity* p = world_.player()) world_.removeAuraSpell(*p, eat_.spell);
    eat_ = Eating();
    sendSelfFields();   // standing again
}

void OfflineSession::updateEating(uint32_t nowMs) {
    if (!eat_.active) return;
    Entity* p = world_.player();
    if (!p || !p->alive() || ghost_) { eat_ = Eating(); return; }
    if (p->lastDamageMs >= eat_.startMs && p->lastDamageMs) { stopEating(nowMs); return; }   // hit: up
    bool changed = false;
    while (eat_.active && (int32_t)(nowMs - eat_.nextTickMs) >= 0 && (eat_.hpLeft || eat_.manaLeft)) {
        const uint32_t h = std::min(eat_.hpTick, eat_.hpLeft), m = std::min(eat_.manaTick, eat_.manaLeft);
        eat_.hpLeft -= h;
        eat_.manaLeft -= m;
        if (h) p->hp = std::min<int32_t>(p->hp + (int32_t)h, (int32_t)p->maxHp);
        if (m) p->power = std::min<int32_t>(p->power + (int32_t)m, (int32_t)p->maxPower);
        eat_.nextTickMs += kEatTickMs;
        changed = true;
    }
    if ((int32_t)(nowMs - eat_.endMs) >= 0 || (!eat_.hpLeft && !eat_.manaLeft)) {
        stopEating(nowMs);
        return;
    }
    if (changed) sendSelfFields();
}

// ---------------------------------------------------------------- repairs
// vMaNGOS Item::GetRepairCost / Player::DurabilityRepair: lost points * DurabilityCosts[itemLevel][subclass] *
// DurabilityQuality[(quality + 1) * 2]; the tables are the client's (durability.txt from the converter). Without the
// file: a stand-in multiplier (item level / 2, quality 1). An NPC with UNIT_NPC_FLAG_REPAIR (0x4000), near.
void OfflineSession::setDurabilityFile(const std::string& path) { durabilityPath_ = path; durabilityLoaded_ = false; }

uint32_t OfflineSession::repairCost(uint32_t entry, uint16_t lost) {
    if (!lost || !data_) return 0;
    if (!durabilityLoaded_) {
        durabilityLoaded_ = true;
        if (FILE* f = std::fopen(durabilityPath_.c_str(), "r")) {
            char line[512];
            while (std::fgets(line, sizeof line, f)) {
                if (line[0] == 'C' && line[1] == '\t') {
                    char* q = line + 2;
                    const uint32_t lv = (uint32_t)std::strtoul(q, &q, 10);
                    std::array<uint32_t, 29> m{};
                    for (uint32_t& v : m) v = (uint32_t)std::strtoul(q, &q, 10);
                    repairCosts_[lv] = m;
                } else if (line[0] == 'Q' && line[1] == '\t') {
                    char* q = line + 2;
                    const uint32_t id = (uint32_t)std::strtoul(q, &q, 10);
                    repairQuality_[id] = (float)std::strtod(q, &q);
                }
            }
            std::fclose(f);
        }
    }
    const ItemTemplate* t = data_->item(entry);
    if (!t) return 0;
    const int col = t->cls == 2 ? (int)t->subclass : t->cls == 4 ? 21 + (int)t->subclass : -1;
    double mult = std::max(1.0, t->itemLevel / 2.0), qmod = 1.0;
    auto c = repairCosts_.find(t->itemLevel);
    if (c != repairCosts_.end() && col >= 0 && col < 29) mult = c->second[(size_t)col];
    auto q = repairQuality_.find((t->quality + 1) * 2);
    if (q != repairQuality_.end()) qmod = q->second;
    return std::max<uint32_t>(1, (uint32_t)(lost * mult * qmod));
}

bool OfflineSession::handleRepair(uint16_t op, const Bytes& data, uint32_t nowMs) {
    if (op != 0x2A8) return false;   // CMSG_REPAIR_ITEM: u64 npc, u64 item (0 = everything)
    Entity* p = world_.player();
    if (!p) return true;
    Reader r(data);
    const uint64_t npc = r.u64(), item = r.u64();
    if (r.bad) return true;
    const Entity* v = world_.entity(npc);
    if (!v || !(v->npcFlags & 0x4000u) || std::hypot(v->x - p->x, v->y - p->y) > 15.0f) return true;
    CharacterProfile& pr = world_.playerProfile();
    struct Fix { uint16_t* lost; uint32_t entry; };
    std::vector<Fix> fix;
    for (int s = 0; s < 19; ++s)
        if (pr.equipment[(size_t)s] && pr.equipLost[(size_t)s] && (!item || item == itemGuidOf(true, s)))
            fix.push_back({&pr.equipLost[(size_t)s], pr.equipment[(size_t)s]});
    for (int i = 0; i < pr.bagCount; ++i)
        if (pr.bag[i].entry && pr.bag[i].lost && (!item || item == itemGuidOf(false, i))) fix.push_back({&pr.bag[i].lost, pr.bag[i].entry});
    uint32_t cost = 0;
    for (const Fix& f : fix) cost += repairCost(f.entry, *f.lost);
    if (fix.empty()) return true;
    if (p->money < cost) { errorLine("You don't have enough money.", nowMs); return true; }
    p->money -= cost;
    pr.money = p->money;
    for (const Fix& f : fix) *f.lost = 0;
    world_.recomputeGearPublic();
    // the items come again with their durability
    for (int s = 0; s < 19; ++s) { Writer w; w.u64(itemGuidOf(true, s)); send(0x0AA, w.b); }
    for (int i = 0; i < pr.bagCount; ++i) { Writer w; w.u64(itemGuidOf(false, i)); send(0x0AA, w.b); }
    sendInventory();
    sendSelfFields();
    saveSoon_ = true;
    return true;
}

}  // namespace offline
}  // namespace w3n
