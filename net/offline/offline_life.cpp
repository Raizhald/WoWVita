// offline_life.cpp — WoWVita: death and home in the offline world, as the original 1.12 does them.
//
// Death: "Release Spirit" (CMSG_REPOP_REQUEST) leaves the body where it fell and takes the ghost to the graveyard of the
// zone it died in (game_graveyard_zone + world_safe_locs from the database, the "GRAV" section; the nearest one its team
// may use). The ghost can then run back and reclaim the body within 39 yards of it once the 30 s delay has passed
// (CMSG_RECLAIM_CORPSE), or talk to the spirit healer at the graveyard (only ghosts see it): SMSG_SPIRIT_HEALER_CONFIRM ->
// CMSG_SPIRIT_HEALER_ACTIVATE. Both give back half of the health; the spirit healer adds resurrection sickness from
// level 11 on (15007: 1 minute per level above 10, 10 minutes from level 20).
//
// Home: an innkeeper's "Make this inn your home." (gossip option 2) -> SMSG_BINDER_CONFIRM -> CMSG_BINDER_ACTIVATE binds the
// character where it stands (SMSG_BINDPOINTUPDATE). The Hearthstone (item 6948, spell 8690: a 10 s cast, a 1 hour
// cooldown, broken by moving) takes it there. A character that was never bound has its start as home.
//
// The client tells the zone it is in with CMSG_ZONEUPDATE (the original client does the same); within a map a teleport is
// MSG_MOVE_TELEPORT_ACK (vMaNGOS Player::TeleportTo -> SendTeleportAckPacket): until the client answers, its older
// movement packets are dropped.
#include <algorithm>
#include <cmath>
#include <ctime>

#include "offline_server.hpp"
#include "../movement.hpp"

namespace w3n {
namespace offline {

namespace {
enum : uint16_t {
    CMSG_USE_ITEM = 0x0AB, SMSG_DESTROY_OBJECT = 0x0AA, MSG_MOVE_TELEPORT_ACK = 0x0C7,
    SMSG_BINDPOINTUPDATE = 0x155, SMSG_PLAYERBOUND = 0x158, CMSG_GOSSIP_SELECT_OPTION = 0x17C, SMSG_GOSSIP_COMPLETE = 0x17E,
    CMSG_BINDER_ACTIVATE = 0x1B5, CMSG_ZONEUPDATE = 0x1F4, MSG_CORPSE_QUERY = 0x216, CMSG_SPIRIT_HEALER_ACTIVATE = 0x21C,
    SMSG_SPIRIT_HEALER_CONFIRM = 0x222, SMSG_CORPSE_RECLAIM_DELAY = 0x269, SMSG_BINDER_CONFIRM = 0x2EB,
    CMSG_REPOP_REQUEST = 0x15A, CMSG_RECLAIM_CORPSE = 0x1D2,
};
constexpr uint32_t kNpcSpiritHealer = 0x20, kNpcInnkeeper = 0x80;
constexpr float kReclaimRadius = 39.0f;        // CORPSE_RECLAIM_RADIUS
constexpr uint32_t kReclaimDelayMs = 30000;    // the first death's delay
constexpr float kTalkRange = 15.0f;            // as the vendors
constexpr uint16_t kAlliance = 469, kHorde = 67;

bool allianceRace(uint8_t r) { return r == 1 || r == 3 || r == 4 || r == 7; }
}  // namespace

bool OfflineSession::isSpiritHealer(const Entity& e) { return (e.npcFlags & kNpcSpiritHealer) != 0; }

// ---------------------------------------------------------------- graveyards
const Graveyard* OfflineSession::nearestGraveyard(float x, float y, uint32_t zone) const {
    if (!data_ || data_->graveyards.empty()) return nullptr;
    const uint16_t team = allianceRace(world_.playerProfile().race) ? kAlliance : kHorde;
    auto find = [&](uint32_t id) -> const Graveyard* {
        for (const Graveyard& g : data_->graveyards)
            if (g.id == id) return &g;
        return nullptr;
    };
    const Graveyard* best = nullptr;
    float bestD = 1e30f;
    // 1. the graveyards of the zone the body lies in; 2. without one (or without a known zone) any this team may use
    for (int pass = 0; pass < 2 && !best; ++pass)
        for (const GraveyardLink& l : data_->graveyardLinks) {
            if (l.faction && l.faction != team) continue;
            if (pass == 0 && (!zone || l.zone != zone)) continue;
            const Graveyard* g = find(l.id);
            if (!g) continue;
            const float d = std::hypot(g->x - x, g->y - y);
            if (d < bestD) { bestD = d; best = g; }
        }
    return best;
}

// ---------------------------------------------------------------- teleport within the map
void OfflineSession::teleportPlayer(float x, float y, float z, float o, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return;
    world_.setPosition(*p, x, y, z, o);
    MovementInfo m;
    m.time = nowMs;
    m.x = x; m.y = y; m.z = z; m.o = o;
    Writer w;
    w.packedGuid(playerGuid_);
    w.u32(teleportCounter_++);
    writeMovementInfo(w, m);
    send(MSG_MOVE_TELEPORT_ACK, w.b);
    awaitTeleportAck_ = true;
    teleportSentMs_ = nowMs;
    updateGameObjects(nowMs, true);
}

void OfflineSession::backToLife(int32_t hpPercent, bool sickness, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return;
    ghost_ = false;
    corpseSet_ = false;
    world_.revive(*p, hpPercent, nowMs);
    if (p->powerType == 1) p->power = 0;   // rage starts empty
    else p->power = (int32_t)p->maxPower / 2;
    if (sickness) {   // WoWVita: the spirit healer takes another 25 % of the durability
        world_.durabilityLossAll(25);
        send(0x2BD);   // SMSG_DURABILITY_DAMAGE_DEATH
    }
    if (sickness && p->level > 10) {
        const uint32_t min = p->level >= 20 ? 10u : (uint32_t)(p->level - 10);
        world_.addTimedAura(*p, kResurrectionSickness, min * 60000u);
    }
    sendSelfFields();
    showSpiritHealers(false);
    saveSoon_ = true;
}

void OfflineSession::handleDeath(uint16_t op, const Bytes& data, uint32_t nowMs) {
    (void)data;
    Entity* p = world_.player();
    if (!p) return;
    if (op == CMSG_REPOP_REQUEST) {
        if (!p->dead) { errorLine("You are not dead.", nowMs); return; }
        if (ghost_) return;
        ghost_ = true;
        corpseSet_ = true;
        corpseX_ = p->x; corpseY_ = p->y; corpseZ_ = p->z;
        reclaimAtMs_ = nowMs + kReclaimDelayMs;
        p->hp = 1;
        p->victim = 0;
        sendSelfFields();
        Writer w;
        w.u32(kReclaimDelayMs);
        send(SMSG_CORPSE_RECLAIM_DELAY, w.b);
        if (const Graveyard* g = nearestGraveyard(p->x, p->y, zone_)) teleportPlayer(g->x, g->y, g->z, g->o, nowMs);
        showSpiritHealers(true);
        return;
    }
    if (op == CMSG_RECLAIM_CORPSE) {
        if (!ghost_) return;
        if (corpseSet_ && std::hypot(p->x - corpseX_, p->y - corpseY_) > kReclaimRadius) {
            errorLine("You must be closer to your corpse.", nowMs);
            return;
        }
        if (nowMs < reclaimAtMs_) {
            errorLine("You can't resurrect yet: " + std::to_string((reclaimAtMs_ - nowMs + 999) / 1000) + " s.", nowMs);
            return;
        }
        backToLife(50, false, nowMs);
    }
}

// ---------------------------------------------------------------- NPCs: spirit healers and innkeepers
bool OfflineSession::lifeHello(uint64_t npc, bool near, uint32_t nowMs) {
    const Entity* v = world_.entity(npc);
    if (!v || !isSpiritHealer(*v)) return false;
    if (!ghost_) return true;   // (the living do not see it)
    if (!near) { errorLine("You are too far away.", nowMs); return true; }
    Writer w;
    w.u64(npc);
    send(SMSG_SPIRIT_HEALER_CONFIRM, w.b);
    return true;
}

bool OfflineSession::handleLife(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return false;
    Reader r(data);
    switch (op) {
        case CMSG_ZONEUPDATE: {
            const uint32_t z = r.u32();
            if (!r.bad && z) zone_ = z;
            return true;
        }
        case MSG_MOVE_TELEPORT_ACK:
            awaitTeleportAck_ = false;
            return true;
        case 0x0B4: {   // CMSG_AREATRIGGER: the exploration quests
            const uint32_t t = r.u32();
            if (!r.bad) questAreaTrigger(t);
            return true;
        }
        case MSG_CORPSE_QUERY: {
            Writer w;
            if (!ghost_ || !corpseSet_) {
                w.u8(0);
            } else {
                w.u8(1);
                w.u32(mapId_);
                w.f32(corpseX_); w.f32(corpseY_); w.f32(corpseZ_);
                w.u32(mapId_);
            }
            send(MSG_CORPSE_QUERY, w.b);
            return true;
        }
        case CMSG_SPIRIT_HEALER_ACTIVATE: {
            const uint64_t g = r.u64();
            const Entity* v = world_.entity(g);
            if (r.bad || !ghost_ || !v || !isSpiritHealer(*v)) return true;
            if (std::hypot(v->x - p->x, v->y - p->y) > kTalkRange) { errorLine("You are too far away.", nowMs); return true; }
            backToLife(50, true, nowMs);
            return true;
        }
        case CMSG_GOSSIP_SELECT_OPTION: {
            const uint64_t g = r.u64();
            const uint32_t opt = r.u32();
            const Entity* v = world_.entity(g);
            if (r.bad || opt != kGossipBind || !v || !(v->npcFlags & kNpcInnkeeper)) return false;
            send(SMSG_GOSSIP_COMPLETE);
            if (std::hypot(v->x - p->x, v->y - p->y) > kTalkRange) { errorLine("You are too far away.", nowMs); return true; }
            Writer w;
            w.u64(g);
            send(SMSG_BINDER_CONFIRM, w.b);
            return true;
        }
        case CMSG_BINDER_ACTIVATE: {
            const uint64_t g = r.u64();
            const Entity* v = world_.entity(g);
            if (r.bad || !v || !(v->npcFlags & kNpcInnkeeper) || !p->alive()) return true;
            if (std::hypot(v->x - p->x, v->y - p->y) > kTalkRange) { errorLine("You are too far away.", nowMs); return true; }
            CharacterProfile& pr = world_.playerProfile();
            pr.homeSet = true;
            pr.homeX = p->x; pr.homeY = p->y; pr.homeZ = p->z;
            pr.homeZone = zone_;
            sendBindPoint();
            Writer b;
            b.u64(g);
            b.u32(zone_);
            send(SMSG_PLAYERBOUND, b.b);
            // a character that lost its Hearthstone gets a new one from the innkeeper
            if (data_ && data_->item(kHearthstoneItem) && world_.countPlayerItem(kHearthstoneItem) == 0) {
                int why = 0;
                if (world_.buyItem(kHearthstoneItem, 1, 0, nowMs, &why)) sendInventory();
            }
            saveSoon_ = true;
            return true;
        }
        case CMSG_USE_ITEM: {
            const uint8_t bag = r.u8(), slot = r.u8();
            r.u8();   // the item's spell slot
            const int index = r.bad ? -1 : bagIndex(bag, slot);   // the backpack or a bag
            if (index < 0) return false;
            const CharacterProfile& pr = world_.playerProfile();
            if (index < pr.bagCount && pr.bag[index].entry != kHearthstoneItem) return useConsumable(index, nowMs);   // food, potions
            if (index >= pr.bagCount || pr.bag[index].entry != kHearthstoneItem) return false;
            if (ghost_ || !p->alive()) { errorLine("You can't do that while dead.", nowMs); return true; }
            world_.tryCast(*p, kHearthstoneSpell, p->guid, nowMs);
            return true;
        }
        default:
            return false;
    }
}

void OfflineSession::sendBindPoint() {
    const CharacterProfile& pr = world_.playerProfile();
    Writer w;
    if (pr.homeSet) { w.f32(pr.homeX); w.f32(pr.homeY); w.f32(pr.homeZ); }
    else if (data_) { w.f32(data_->spawnX); w.f32(data_->spawnY); w.f32(data_->spawnZ); }
    else { w.f32(0); w.f32(0); w.f32(0); }
    w.u32(mapId_);
    w.u32(pr.homeSet ? pr.homeZone : (data_ ? data_->zoneId : 0));
    send(SMSG_BINDPOINTUPDATE, w.b);
}

// The Hearthstone's cast is over: home.
void OfflineSession::hearthHome(uint32_t nowMs) {
    const CharacterProfile& pr = world_.playerProfile();
    if (pr.homeSet) teleportPlayer(pr.homeX, pr.homeY, pr.homeZ, world_.player() ? world_.player()->o : 0.0f, nowMs);
    else if (data_) teleportPlayer(data_->spawnX, data_->spawnY, data_->spawnZ, data_->spawnO, nowMs);
    saveSoon_ = true;
}

// ---------------------------------------------------------------- exploration
// vMaNGOS Player::CheckAreaExploreAndOutdoor: an area seen for the first time gives exploration_basexp of its level
// (5 levels below the character or more: less, 5 % per level; above it: as for level + 5); nothing at level 60.
void OfflineSession::reportArea(uint32_t area, uint32_t level, uint32_t bit) {
    Entity* p = world_.player();
    if (!inWorld_ || !p || !data_ || ghost_ || !area || bit >= 64 * 32 || area == lastArea_) return;
    lastArea_ = area;
    CharacterProfile& pr = world_.playerProfile();
    if (pr.explored[bit / 32] & (1u << (bit % 32))) return;
    pr.explored[bit / 32] |= 1u << (bit % 32);
    saveSoon_ = true;
    if (!level) return;   // (an area without a level gives nothing, and says nothing)
    auto base = [&](uint32_t lv) { return lv < data_->explorationXp.size() ? data_->explorationXp[lv] : 0u; };
    uint32_t xp = 0;
    if (p->level < 60) {
        const int diff = (int)p->level - (int)level;
        if (diff < -5) xp = base(p->level + 5);
        else if (diff > 5) xp = base(level) * (uint32_t)std::clamp(100 - (diff - 5) * 5, 0, 100) / 100;
        else xp = base(level);
    }
    if (xp) world_.addXp(*p, xp, world_.nowMs(), 0, true);
    Writer w;
    w.u32(area);
    w.u32(xp);
    send(0x1F8, w.b);   // SMSG_EXPLORATION_EXPERIENCE
}

// ---------------------------------------------------------------- rested experience
// vMaNGOS Player::Update / SetRestBonus / LoadFromDB: in an inn (its area trigger, areatrigger_tavern) or a capital the
// pool grows by next-level-XP / 72000 * 0.125 a second (5 % of a level per 8 hours); between sessions the same when the
// character left resting, a quarter of it (0.031) elsewhere. The pool holds at most 1.5 levels of kills (75 % of a level
// as bonus); a kill gives as much again from it (World::addXp). PLAYER_FLAGS_RESTING 0x20 (the portrait's zZ),
// PLAYER_BYTES_2 byte 3: 1 rested / 2 normal, PLAYER_REST_STATE_EXPERIENCE: the pool.
uint32_t OfflineSession::wallNow() const { return wallClock ? wallClock() : (uint32_t)std::time(nullptr); }

namespace {
bool capitalZone(uint32_t z) { return z == 1519 || z == 1537 || z == 1497 || z == 1657 || z == 1637 || z == 1638; }
void capRest(CharacterProfile& pr, uint32_t nextXp) {
    const float maxPool = (float)nextXp * 1.5f / 2.0f;
    if (pr.restBonus > maxPool) pr.restBonus = maxPool;
    if (!(pr.restBonus >= 0.0f)) pr.restBonus = 0.0f;
}
}  // namespace

void OfflineSession::restOnLogin() {
    CharacterProfile& pr = save_.player;
    const uint32_t now = wallNow();
    if (pr.logoutTime && now > pr.logoutTime && pr.level < 60) {
        const uint32_t away = std::min<uint32_t>(now - pr.logoutTime, 60u * 86400u);
        const float bubble = pr.logoutResting ? 0.125f : 0.031f;
        pr.restBonus += (float)away * ((float)pr.nextXp / 72000.0f) * bubble;
        capRest(pr, pr.nextXp);
    }
    pr.logoutTime = now;
    resting_ = pr.logoutResting;   // (the first check corrects it at once)
    restTickMs_ = 0;
}

void OfflineSession::updateRest(uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return;
    if (restTickMs_ && nowMs - restTickMs_ < 1000) return;
    const uint32_t dt = restTickMs_ ? nowMs - restTickMs_ : 0;
    restTickMs_ = nowMs;
    // the inns' shapes, once the client's table is there
    if (triggers_ && data_ && (tavernShapesOf_ != triggers_ || tavernShapesCount_ != triggers_->all().size())) {
        tavernShapesOf_ = triggers_;
        tavernShapesCount_ = triggers_->all().size();
        tavernShapes_.clear();
        for (const AreaTrigger& t : triggers_->all())
            if (std::find(data_->taverns.begin(), data_->taverns.end(), t.id) != data_->taverns.end()) tavernShapes_.push_back(&t);
    }
    bool r = capitalZone(zone_);
    const float pos[3] = {p->x, p->y, p->z};
    for (const AreaTrigger* t : tavernShapes_)
        if (!r && t->map == mapId_ && AreaTriggerTable::inside(*t, pos)) r = true;
    CharacterProfile& pr = world_.playerProfile();
    if (resting_ && dt && p->level < 60) {
        pr.restBonus += (float)dt / 1000.0f * ((float)p->nextLevelXp / 72000.0f) * 0.125f;
        capRest(pr, p->nextLevelXp);
    }
    const uint32_t pool = (uint32_t)pr.restBonus;
    if (r != resting_ || pool != restSent_) {
        resting_ = r;
        sendSelfFields();
    }
}

}  // namespace offline
}  // namespace w3n
