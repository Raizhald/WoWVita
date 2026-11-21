// offline_taxi.cpp — WoWVita: flight masters in the offline world, as the original 1.12 server runs them.
//
// The data is the client's own, from the converter: taxi.txt (TaxiNodes.dbc: the flight points; TaxiPath.dbc: which
// point flies to which and for how much) and transports.txt ("T" lines: every TaxiPathNode.dbc point). A path's points
// are found by its ends: the TaxiPathNode path that starts at the first flight point and ends at the second.
//
// Talking to a flight master discovers its point (SMSG_NEW_TAXI_PATH, SMSG_TAXINODE_STATUS) and shows the map
// (SMSG_SHOWTAXINODES: the point and the mask of the known ones). A flight (CMSG_ACTIVATETAXI, or CMSG_ACTIVATETAXIEXPRESS
// over several points: the client's cheapest route) costs the sum of its legs; the character mounts (Riding Gryphon 6852,
// Wind Rider 6851, the Undercity bats 1566), gets UNIT_FLAG_TAXI_FLIGHT and flies the whole path as one flying spline
// (SMSG_MONSTER_MOVE, SPLINE_FLYING) at 32 yd/s (vMaNGOS PLAYER_FLIGHT_SPEED); the server moves it along the same path
// (the world around it streams in), creatures leave it alone, and at the end it lands where the path ends.
// A new character knows its race's capital (vMaNGOS PlayerTaxi::InitTaxiNodes): Stormwind, Ironforge, Undercity.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "offline_server.hpp"

namespace w3n {
namespace offline {

namespace {
enum : uint16_t {
    SMSG_MONSTER_MOVE = 0x0DD, CMSG_GOSSIP_SELECT_OPTION = 0x17C, SMSG_GOSSIP_COMPLETE = 0x17E,
    SMSG_SHOWTAXINODES = 0x1A9, CMSG_TAXINODE_STATUS_QUERY = 0x1AA, SMSG_TAXINODE_STATUS = 0x1AB,
    CMSG_TAXIQUERYAVAILABLENODES = 0x1AC, CMSG_ACTIVATETAXI = 0x1AD, SMSG_ACTIVATETAXIREPLY = 0x1AE,
    SMSG_NEW_TAXI_PATH = 0x1AF, CMSG_ACTIVATETAXIEXPRESS = 0x312,
};
enum : uint32_t {   // ActivateTaxiReply
    TAXI_OK = 0, TAXI_UNSPECIFIED = 1, TAXI_NO_PATH = 2, TAXI_NO_MONEY = 3, TAXI_TOO_FAR = 4, TAXI_NO_VENDOR = 5,
    TAXI_NOT_VISITED = 6, TAXI_BUSY = 7, TAXI_SAME_NODE = 11,
};
constexpr uint32_t kNpcFlightMaster = 0x8;
constexpr float kFlightSpeed = 32.0f;        // yd/s
constexpr float kNodeReach = 60.0f;          // a flight master stands this close to its TaxiNodes point
constexpr float kTalkRange = 15.0f;
constexpr uint32_t kSplineFlying = 0x200;
constexpr uint32_t kUnitFlagTaxi = 0x00100000u | 0x00000004u;   // TAXI_FLIGHT | DISABLE_MOVE

uint64_t legKey(uint32_t a, uint32_t b) { return ((uint64_t)a << 32) | b; }
float dist3(const std::array<float, 3>& a, float x, float y, float z) {
    const float dx = a[0] - x, dy = a[1] - y, dz = a[2] - z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}  // namespace

void OfflineSession::setTaxi(const TaxiTable* t, const std::string& transportsPath) {
    taxi_ = t;
    transportsPath_ = transportsPath;
    taxiPathsLoaded_ = false;
    taxiLegs_.clear();
}

// transports.txt "T path node map x y z flags delay" -> the points of every flight path on this map, matched by its ends
void OfflineSession::loadTaxiPaths() {
    if (taxiPathsLoaded_ || !taxi_ || !taxi_->loaded()) return;
    taxiPathsLoaded_ = true;
    std::map<uint32_t, std::map<uint32_t, std::array<float, 3>>> raw;   // path -> node index -> point
    if (FILE* f = std::fopen(transportsPath_.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            if (line[0] != 'T' || line[1] != '\t') continue;
            unsigned path = 0, idx = 0, map = 0;
            float x = 0, y = 0, z = 0;
            if (std::sscanf(line + 2, "%u %u %u %f %f %f", &path, &idx, &map, &x, &y, &z) != 6 || map != mapId_) continue;
            raw[path][idx] = {x, y, z};
        }
        std::fclose(f);
    }
    std::vector<std::pair<uint32_t, std::vector<std::array<float, 3>>>> paths;
    for (const auto& [path, pts] : raw) {
        std::vector<std::array<float, 3>> v;
        for (const auto& [i, p] : pts) v.push_back(p);
        if (v.size() >= 2) paths.emplace_back(path, std::move(v));
    }
    for (const TaxiTable::Path& tp : taxi_->paths()) {
        const TaxiNode* a = taxi_->node(tp.from);
        const TaxiNode* b = taxi_->node(tp.to);
        if (!a || !b || a->map != mapId_ || b->map != mapId_) continue;
        const std::vector<std::array<float, 3>>* best = nullptr;
        float bestScore = 400.0f;
        for (const auto& [id, v] : paths) {
            const float s = dist3(v.front(), a->x, a->y, a->z) + dist3(v.back(), b->x, b->y, b->z);
            if (s < bestScore) { bestScore = s; best = &v; }
        }
        if (best) taxiLegs_[legKey(tp.from, tp.to)] = *best;
    }
    if (trace) {
        char b[128];
        std::snprintf(b, sizeof b, "taxi: %zu flight paths with points on map %u", taxiLegs_.size(), (unsigned)mapId_);
        trace(b);
    }
}

uint32_t OfflineSession::taxiNodeFor(const Entity& npc) const {
    if (!taxi_ || !taxi_->loaded()) return 0;
    uint32_t best = 0;
    float bestD = kNodeReach;
    for (const auto& [id, n] : taxi_->nodes()) {
        if (n.map != mapId_) continue;
        const float d = std::hypot(n.x - npc.x, n.y - npc.y);
        if (d < bestD) { bestD = d; best = id; }
    }
    return best;
}

bool OfflineSession::taxiKnown(uint32_t node) const {
    const auto& m = world_.playerProfile().taxiMask;
    return node >= 1 && node <= 256 && (m[(node - 1) / 32] >> ((node - 1) % 32) & 1u);   // bit node - 1 (PlayerTaxi)
}

void OfflineSession::taxiLearn(uint32_t node) {
    if (node < 1 || node > 256) return;
    world_.playerProfile().taxiMask[(node - 1) / 32] |= 1u << ((node - 1) % 32);
}

void OfflineSession::taxiInitial() {
    CharacterProfile& pr = save_.player;
    for (uint32_t m : pr.taxiMask) if (m) return;
    uint32_t node = 0;
    switch (pr.race) {
        case 1: node = 2; break;            // Human: Stormwind
        case 3: case 7: node = 6; break;    // Dwarf, Gnome: Ironforge
        case 5: node = 11; break;           // Undead: Undercity
        case 2: case 8: node = 23; break;   // Orc, Troll: Orgrimmar
        case 6: node = 22; break;           // Tauren: Thunder Bluff
        case 4: node = 26; break;           // Night Elf: Auberdine
        default: break;
    }
    if (node) pr.taxiMask[(node - 1) / 32] |= 1u << ((node - 1) % 32);
}

void OfflineSession::sendShowTaxi(uint64_t npc, uint32_t node) {
    Writer w;
    w.u32(1);
    w.u64(npc);
    w.u32(node);
    for (uint32_t m : world_.playerProfile().taxiMask) w.u32(m);
    send(SMSG_SHOWTAXINODES, w.b);
}

// A flight master: its point is discovered, then the map (or, with quests, the gossip with "Show me where I can fly.").
bool OfflineSession::taxiHello(uint64_t npc, bool near, bool viaGossipOption, uint32_t nowMs) {
    const Entity* v = world_.entity(npc);
    if (!v || !(v->npcFlags & kNpcFlightMaster) || ghost_) return false;
    const uint32_t node = taxiNodeFor(*v);
    if (!node) return false;
    if (!near) { errorLine("You are too far away.", nowMs); return true; }
    if (!taxiKnown(node)) {
        taxiLearn(node);
        send(SMSG_NEW_TAXI_PATH);
        Writer s;
        s.u64(npc);
        s.u8(1);
        send(SMSG_TAXINODE_STATUS, s.b);
        saveSoon_ = true;
    }
    if (!viaGossipOption && !questMenu(v->entry, false).empty()) return false;   // the gossip shows the quests and the map
    sendShowTaxi(npc, node);
    return true;
}

bool OfflineSession::handleTaxi(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return false;
    Reader r(data);
    switch (op) {
        case CMSG_TAXINODE_STATUS_QUERY: {
            const uint64_t g = r.u64();
            const Entity* v = world_.entity(g);
            if (r.bad || !v) return true;
            const uint32_t node = taxiNodeFor(*v);
            Writer w;
            w.u64(g);
            w.u8(node && taxiKnown(node) ? 1 : 0);
            send(SMSG_TAXINODE_STATUS, w.b);
            return true;
        }
        case CMSG_TAXIQUERYAVAILABLENODES: {
            const uint64_t g = r.u64();
            const Entity* v = world_.entity(g);
            if (r.bad || !v) return true;
            taxiHello(g, std::hypot(v->x - p->x, v->y - p->y) < kTalkRange, true, nowMs);
            return true;
        }
        case CMSG_GOSSIP_SELECT_OPTION: {
            const uint64_t g = r.u64();
            const uint32_t opt = r.u32();
            const Entity* v = world_.entity(g);
            if (r.bad || opt != kGossipTaxi || !v || !(v->npcFlags & kNpcFlightMaster)) return false;
            send(SMSG_GOSSIP_COMPLETE);
            taxiHello(g, std::hypot(v->x - p->x, v->y - p->y) < kTalkRange, true, nowMs);
            return true;
        }
        case CMSG_ACTIVATETAXI: case CMSG_ACTIVATETAXIEXPRESS: {
            const uint64_t g = r.u64();
            std::vector<uint32_t> route;
            if (op == CMSG_ACTIVATETAXI) {
                const uint32_t a = r.u32(), b = r.u32();
                route = {a, b};
            } else {
                r.u32();   // the client's total
                const uint32_t n = r.u32();
                if (r.bad || n < 2 || n > 64) return true;
                for (uint32_t i = 0; i < n && !r.bad; ++i) route.push_back(r.u32());
            }
            if (r.bad) return true;
            auto reply = [&](uint32_t code) { Writer w; w.u32(code); send(SMSG_ACTIVATETAXIREPLY, w.b); };
            const Entity* v = world_.entity(g);
            if (!v || !(v->npcFlags & kNpcFlightMaster)) { reply(TAXI_NO_VENDOR); return true; }
            if (std::hypot(v->x - p->x, v->y - p->y) > kTalkRange) { reply(TAXI_TOO_FAR); return true; }
            if (ghost_ || !p->alive() || flight_.active) { reply(TAXI_BUSY); return true; }
            for (int i = 0; i < kMaxEntities; ++i) {   // in combat: busy
                const Entity* e = world_.byIndex(i);
                if (e && e->used && e->alive() && e->victim == p->guid) { reply(TAXI_BUSY); return true; }
            }
            if (route.front() != taxiNodeFor(*v)) { reply(TAXI_UNSPECIFIED); return true; }
            if (route.back() == route.front()) { reply(TAXI_SAME_NODE); return true; }
            loadTaxiPaths();
            uint32_t cost = 0;
            std::vector<std::array<float, 3>> pts;
            for (size_t i = 0; i + 1 < route.size(); ++i) {
                if (!taxiKnown(route[i + 1])) { reply(TAXI_NOT_VISITED); return true; }
                const uint32_t c = taxi_ ? taxi_->directCost(route[i], route[i + 1]) : 0;
                auto leg = taxiLegs_.find(legKey(route[i], route[i + 1]));
                if (leg == taxiLegs_.end()) { reply(TAXI_NO_PATH); return true; }
                cost += c;
                for (size_t k = pts.empty() ? 0 : 1; k < leg->second.size(); ++k) pts.push_back(leg->second[k]);
            }
            if (p->money < cost) { reply(TAXI_NO_MONEY); return true; }
            p->money -= cost;
            world_.playerProfile().money = p->money;
            reply(TAXI_OK);
            startFlight(pts, route.front(), nowMs);
            saveSoon_ = true;
            return true;
        }
        default:
            return false;
    }
}

void OfflineSession::startFlight(const std::vector<std::array<float, 3>>& pts, uint32_t fromNode, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p || pts.empty()) return;
    flight_ = Flight();
    flight_.pts.push_back({p->x, p->y, p->z});
    for (const auto& q : pts) flight_.pts.push_back(q);
    flight_.cum.push_back(0.0f);
    for (size_t i = 1; i < flight_.pts.size(); ++i) {
        const auto& a = flight_.pts[i - 1];
        flight_.cum.push_back(flight_.cum.back() + dist3(a, flight_.pts[i][0], flight_.pts[i][1], flight_.pts[i][2]));
    }
    flight_.durMs = (uint32_t)(flight_.cum.back() / kFlightSpeed * 1000.0f) + 1;
    flight_.startMs = nowMs;
    flight_.active = true;
    const bool alliance = save_.player.race == 1 || save_.player.race == 3 || save_.player.race == 4 || save_.player.race == 7;
    const bool bats = fromNode == 11 || fromNode == 13 || fromNode == 10;   // Undercity, Tarren Mill, the Sepulcher
    flight_.mount = alliance ? 6852u : bats ? 1566u : 6851u;
    p->untargetable = true;
    world_.stopAttack(*p, nowMs);
    if (p->castSpell) world_.stopCast(*p, 35, nowMs);
    sendSelfFields();   // the mount and the taxi flag
    Writer w;
    w.packedGuid(p->guid);
    w.f32(p->x); w.f32(p->y); w.f32(p->z);
    w.u32(nowMs);
    w.u8(0);
    w.u32(kSplineFlying);
    w.u32(flight_.durMs);
    w.u32((uint32_t)(flight_.pts.size() - 1));
    for (size_t i = 1; i < flight_.pts.size(); ++i) { w.f32(flight_.pts[i][0]); w.f32(flight_.pts[i][1]); w.f32(flight_.pts[i][2]); }
    send(SMSG_MONSTER_MOVE, w.b);
}

void OfflineSession::updateFlight(uint32_t nowMs) {
    if (!flight_.active) return;
    Entity* p = world_.player();
    if (!p) { flight_ = Flight(); return; }
    const uint32_t t = nowMs - flight_.startMs;
    if (t >= flight_.durMs) {   // landed
        const auto& e = flight_.pts.back();
        const auto& b = flight_.pts[flight_.pts.size() - 2];
        world_.setPosition(*p, e[0], e[1], e[2], std::atan2(e[1] - b[1], e[0] - b[0]));
        flight_ = Flight();
        p->untargetable = false;
        sendSelfFields();
        updateGameObjects(nowMs, true);
        saveSoon_ = true;
        return;
    }
    const float d = flight_.cum.back() * (float)t / (float)flight_.durMs;
    size_t i = 1;
    while (i + 1 < flight_.cum.size() && flight_.cum[i] < d) ++i;
    const float seg = std::max(0.001f, flight_.cum[i] - flight_.cum[i - 1]);
    const float f = std::clamp((d - flight_.cum[i - 1]) / seg, 0.0f, 1.0f);
    const auto& a = flight_.pts[i - 1];
    const auto& b = flight_.pts[i];
    world_.setPosition(*p, a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f,
                       std::atan2(b[1] - a[1], b[0] - a[0]));
}

}  // namespace offline
}  // namespace w3n
