// offline_items.cpp — WoWVita: food, drink and potions in the offline world, as the original 1.12 server uses them.
//
// The pack's "ITX1" carries what an item does when used (spell_template = Spell.dbc, item_template): food (aura 84) and
// drink (aura 85) give their total over the spell's duration (the tooltip's "Restores N health over 18 sec") while the
// character sits; moving or being hit stands it up and ends the meal (AuraInterruptFlags NOT_SEATED). They cannot be
// started in combat (SPELL_ATTR_CANT_USED_IN_COMBAT). Potions heal (effect 10) or give mana (effect 30) at once.
// One item of the stack is used up; items of one category share its cooldown (potions 2 minutes).
#include <algorithm>
#include <cmath>

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

}  // namespace offline
}  // namespace w3n
