// offline_server.cpp — the server-equivalent packet layer of offline mode.
//
// Every SMSG here is the exact 1.12.1 layout the existing GameSession already parses; every CMSG is
// the packet GameSession already sends. The placeholders of the online path (realm, SRP, sockets,
// header cipher) are absent because there is nothing to authenticate against.
#include "offline_server.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../game.hpp"
#include "../movement.hpp"
#include "offline_bots.hpp"
#include "offline_data.hpp"

namespace w3n {
namespace offline {

namespace {

enum : uint16_t {
    SMSG_LOGIN_SETTIMESPEED = 0x042,
    CMSG_NAME_QUERY = 0x050, SMSG_NAME_QUERY_RESPONSE = 0x051,
    CMSG_ITEM_QUERY_SINGLE = 0x056, SMSG_ITEM_QUERY_SINGLE_RESPONSE = 0x058,
    CMSG_GAMEOBJECT_QUERY = 0x05E, SMSG_GAMEOBJECT_QUERY_RESPONSE = 0x05F,
    CMSG_CREATURE_QUERY = 0x060, SMSG_CREATURE_QUERY_RESPONSE = 0x061,
    SMSG_GROUP_LIST = 0x07D, SMSG_PARTY_MEMBER_STATS = 0x07E,
    CMSG_MESSAGECHAT = 0x095, SMSG_MESSAGECHAT = 0x096,
    SMSG_UPDATE_OBJECT = 0x0A9,
    SMSG_ACTION_BUTTONS = 0x129, SMSG_INITIAL_SPELLS = 0x12A,
    CMSG_CAST_SPELL = 0x12E, CMSG_CANCEL_CAST = 0x12F, SMSG_CAST_FAILED = 0x130,
    SMSG_SPELL_START = 0x131, SMSG_SPELL_GO = 0x132, SMSG_SPELL_FAILURE = 0x133,
    SMSG_UPDATE_AURA_DURATION = 0x137,
    CMSG_SET_SELECTION = 0x13D,
    CMSG_ATTACKSWING = 0x141, CMSG_ATTACKSTOP = 0x142, SMSG_ATTACKSTART = 0x143, SMSG_ATTACKSTOP = 0x144,
    SMSG_ATTACKSWING_NOTINRANGE = 0x145, SMSG_ATTACKSWING_BADFACING = 0x146, SMSG_ATTACKSWING_DEADTARGET = 0x148,
    SMSG_ATTACKSWING_CANT_ATTACK = 0x149, SMSG_ATTACKERSTATEUPDATE = 0x14A,
    SMSG_SPELLHEALLOG = 0x150,
    SMSG_LOOT_RESPONSE = 0x160, SMSG_LOOT_RELEASE_RESPONSE = 0x161, SMSG_LOOT_REMOVED = 0x162,
    SMSG_LOOT_MONEY_NOTIFY = 0x163, SMSG_LOOT_CLEAR_MONEY = 0x165,
    CMSG_LOOT = 0x15D, CMSG_LOOT_MONEY = 0x15E, CMSG_LOOT_RELEASE = 0x15F,
    CMSG_AUTOSTORE_LOOT_ITEM = 0x108,
    CMSG_REPOP_REQUEST = 0x15A, CMSG_RECLAIM_CORPSE = 0x1D2,
    SMSG_LOG_XPGAIN = 0x1D0,
    MSG_CORPSE_QUERY = 0x216,
    CMSG_SET_ACTIVE_MOVER = 0x26A,
    CMSG_REQUEST_PARTY_MEMBER_STATS = 0x27F,
    SMSG_SPELLNONMELEEDAMAGELOG = 0x250,
    SMSG_PARTY_MEMBER_STATS_FULL = 0x2F2,
    SMSG_CORPSE_RECLAIM_DELAY = 0x269,
    CMSG_LOGOUT_REQUEST = 0x04B,
    // WoWVita: vendors (layouts in net/game_vendor.cpp)
    CMSG_GOSSIP_HELLO = 0x17B, CMSG_LIST_INVENTORY = 0x19E, SMSG_LIST_INVENTORY = 0x19F, CMSG_SELL_ITEM = 0x1A0,
    SMSG_SELL_ITEM = 0x1A1, CMSG_BUY_ITEM = 0x1A2, SMSG_BUY_ITEM = 0x1A4, SMSG_BUY_FAILED = 0x1A5,
    CMSG_AUTOEQUIP_ITEM = 0x10A,
};

constexpr uint8_t kUpdateValues = 0, kUpdateMovement = 1, kUpdateCreate2 = 3;
constexpr uint8_t kUfSelf = 0x01, kUfTransport = 0x02, kUfMelee = 0x04, kUfHighGuid = 0x08, kUfAll = 0x10,
                  kUfLiving = 0x20, kUfPosition = 0x40;

// ---------------------------------------------------------------- update-field builder
struct FieldSet {
    uint16_t idx[128];
    uint32_t val[128];
    uint8_t n = 0;
    void set(uint16_t i, uint32_t v) {
        for (uint8_t k = 0; k < n; ++k)
            if (idx[k] == i) { val[k] = v; return; }
        if (n < 128) { idx[n] = i; val[n] = v; ++n; }
    }
};

void writeFields(Writer& w, const FieldSet& f) {
    if (!f.n) { w.u8(0); return; }
    uint16_t top = 0;
    for (uint8_t k = 0; k < f.n; ++k) top = std::max<uint16_t>(top, f.idx[k]);
    const uint8_t blocks = (uint8_t)(top / 32 + 1);
    w.u8(blocks);
    for (uint8_t b = 0; b < blocks; ++b) {
        uint32_t mask = 0;
        for (uint8_t k = 0; k < f.n; ++k)
            if (f.idx[k] / 32 == b) mask |= 1u << (f.idx[k] % 32);
        w.u32(mask);
    }
    for (uint8_t b = 0; b < blocks; ++b)
        for (int bit = 0; bit < 32; ++bit)
            for (uint8_t k = 0; k < f.n; ++k)
                if (f.idx[k] == b * 32 + bit) w.u32(f.val[k]);
}

struct UpdateBuilder {
    Writer body;
    uint32_t blocks = 0;
    void raw(const Bytes& b) { body.raw(b); ++blocks; }
    Bytes finish() const {
        Writer w;
        w.u32(blocks);
        w.u8(0);
        w.raw(body.b);
        return w.b;
    }
};

void addCreate(UpdateBuilder& u, uint64_t guid, uint8_t objType, uint8_t uflags, float x, float y, float z, float o,
               const FieldSet& fields) {
    Writer b;
    b.u8(kUpdateCreate2);
    b.packedGuid(guid);
    b.u8(objType);
    b.u8((uint8_t)(uflags | kUfLiving));
    b.u32(0);          // movement flags
    b.u32(0);          // time
    b.f32(x); b.f32(y); b.f32(z); b.f32(o);
    b.u32(0);          // fall time
    b.f32(2.5f); b.f32(7.0f); b.f32(4.5f); b.f32(4.722222f); b.f32(2.5f); b.f32(3.141594f);
    if (uflags & kUfHighGuid) b.u32(0);
    if (uflags & kUfAll) b.u32(1);
    writeFields(b, fields);
    u.raw(b.b);
}

void addValues(UpdateBuilder& u, uint64_t guid, const FieldSet& fields) {
    Writer b;
    b.u8(kUpdateValues);
    b.packedGuid(guid);
    writeFields(b, fields);
    u.raw(b.b);
}

// Items have no position and no LIVING block (the stock 1.12 create block for them is just
// UPDATEFLAG_ALL plus the fields).
void addCreateItem(UpdateBuilder& u, uint64_t guid, const FieldSet& fields) {
    Writer b;
    b.u8(kUpdateCreate2);
    b.packedGuid(guid);
    b.u8(1);          // object type: item
    b.u8(kUfAll);
    b.u32(1);
    writeFields(b, fields);
    u.raw(b.b);
}

bool caseInsensitiveEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

uint32_t wirePower(const Entity& e) {
    // Rage is sent as tenths on the 1.12 wire (UNIT_FIELD_POWER1 / POWER2).
    return (uint32_t)std::max<int32_t>(0, e.powerType == 1 ? e.power * 10 : e.power);
}

uint32_t nativeDisplay(uint8_t race, uint8_t gender) {
    switch (race) {
        case 1: return 49u + gender;   // human
        case 2: return 63u + gender;   // orc
        case 3: return 51u + gender;   // dwarf
        case 4: return 53u + gender;   // night elf
        case 5: return 55u + gender;   // undead
        case 6: return 57u + gender;   // tauren
        case 7: return 59u + gender;   // gnome
        case 8: return 61u + gender;   // troll
        default: return 49u;
    }
}

uint32_t itemGuidFor(bool equipment, int slot) {
    return 0x40000000u | (uint32_t)(equipment ? (0x1000 + slot) : (0x2000 + slot));
}

// The class's starting spells, filtered by what the pack actually defines.
uint8_t fillClassSpells(const WorldData* data, uint8_t cls, std::array<uint32_t, kMaxSpellsPerUnit>& out) {
    static const uint32_t warrior[] = {6603, 78, 6673, 355};
    static const uint32_t rogue[] = {6603, 1752, 2098, 1766};
    static const uint32_t mage[] = {133, 116, 2136};
    static const uint32_t priest[] = {585, 2050, 2061, 1243};
    const uint32_t* list = warrior;
    int n = 4;
    if (cls == 4) { list = rogue; n = 4; }
    else if (cls == 8) { list = mage; n = 3; }
    else if (cls == 5) { list = priest; n = 4; }
    uint8_t count = 0;
    for (int i = 0; i < n && count < out.size(); ++i)
        if (!data || data->spell(list[i])) out[count++] = list[i];
    return count;
}


// WoWVita: one creature's create block (the login packets and a spawn that comes into range share it)
void addCreatureCreate(UpdateBuilder& u, const Entity& ent) {
    const Entity* e = &ent;
    FieldSet f;
    f.set(UF::OBJECT_GUID, (uint32_t)e->guid);
    f.set((uint16_t)(UF::OBJECT_GUID + 1), (uint32_t)(e->guid >> 32));
    f.set(UF::OBJECT_TYPE, 0x9);
    f.set(UF::OBJECT_ENTRY, e->entry);
    {   // the template's scale (WoWVita: whole-world packs have giants and tiny critters)
        const float sc = e->scale > 0.05f && e->scale < 20.0f ? e->scale : 1.0f;
        uint32_t bits;
        std::memcpy(&bits, &sc, 4);
        f.set(UF::OBJECT_SCALE_X, bits);
    }
    f.set(UF::UNIT_HEALTH, (uint32_t)std::max<int32_t>(0, e->hp));
    f.set(UF::UNIT_MAXHEALTH, e->maxHp);
    f.set(UF::UNIT_LEVEL, e->level);
    f.set(UF::UNIT_FACTIONTEMPLATE, e->faction);
    f.set(UF::UNIT_BYTES_0, 0u | (1u << 8) | (0u << 16) | ((uint32_t)e->powerType << 24));
    f.set(UF::UNIT_POWER1 + e->powerType, wirePower(*e));
    f.set(UF::UNIT_MAXPOWER1 + e->powerType, e->maxPower);
    f.set(UF::UNIT_DISPLAYID, (uint32_t)e->displayId);
    f.set(UF::UNIT_NATIVEDISPLAYID, (uint32_t)e->displayId);
    f.set(UF::UNIT_NPC_FLAGS, e->npcFlags);
    f.set(UF::UNIT_BOUNDINGRADIUS, 0x3F000000u);
    f.set(UF::UNIT_COMBATREACH, 0x3FC00000u);
    if (e->dead) f.set(UF::UNIT_DYNAMIC_FLAGS, 1);
    addCreate(u, e->guid, 3, kUfHighGuid, e->x, e->y, e->z, e->o, f);
}

}  // namespace

// ---------------------------------------------------------------- setup
void OfflineSession::setup(const WorldData* data, const char* savePath) {
    data_ = data;
    savePath_ = savePath ? savePath : "";
    world_.setData(data);
    opened_ = false;
    inWorld_ = false;
}

void OfflineSession::defaultPlayer() {
    CharacterProfile& p = save_.player;
    p = CharacterProfile();
    p.used = true;
    p.guid = 0x0000000000000001ull;
    p.name = "Recruit";
    p.race = 1; p.cls = 1; p.gender = 0;
    p.skin = 1; p.face = 1; p.hairStyle = 0; p.hairColor = 0; p.facialHair = 0;
    p.level = 1;
    p.xp = 0;
    p.nextXp = 400;
    p.maxHp = 120;
    p.hp = 120;
    p.powerType = 1;          // warrior: rage
    p.maxPower = 100;
    p.power = 0;
    p.money = 0;
    p.equipment[15] = 25;     // Worn Shortsword
    p.equipment[3] = 38;      // Recruit's Shirt
    p.spellCount = fillClassSpells(data_, p.cls, p.spells);
    p.mapId = data_ ? data_->mapId : 0;
    p.zoneId = data_ ? data_->zoneId : 0;
    p.x = data_ ? data_->spawnX : 0;
    p.y = data_ ? data_->spawnY : 0;
    p.z = data_ ? data_->spawnZ : 0;
    p.o = data_ ? data_->spawnO : 0;
}

bool OfflineSession::open(std::string& err) {
    if (!data_) { err = "no offline world data"; return false; }
    save_ = OfflineSaveData();
    std::string loadErr;
    if (offlineSaveExists(savePath_.c_str())) {
        if (!loadOffline(savePath_.c_str(), save_, loadErr)) {
            lastError_ = loadErr;
            err = "the offline save could not be read: " + loadErr;
            return false;
        }
        if (!save_.rosterCount || !save_.roster[0].used) {
            defaultPlayer();
            save_.roster[0] = save_.player;
            save_.rosterCount = 1;
            save_.active = 0;
        }
    } else {
        defaultPlayer();
        save_.roster[0] = save_.player;
        save_.rosterCount = 1;
        save_.active = 0;
        save_.seed = 0x57334F46u;
    }
    // Refresh the four companion templates (names/roles/equipment may come from a newer pack) while
    // keeping the progression the player earned.
    for (int i = 0; i < kMaxParty; ++i) {
        CharacterProfile& b = save_.bots[(size_t)i];
        const BotTemplate& t = data_->bots[(size_t)i];
        const bool fresh = !b.used;
        if (fresh) {
            b = CharacterProfile();
            b.used = true;
            b.isBot = true;
            b.guid = 0x0000000000000B01ull + (uint64_t)i;
            b.level = t.level;
            b.maxHp = t.hp;
            b.hp = (int32_t)t.hp;
            b.maxPower = t.power;
            b.power = (int32_t)t.power;
        }
        b.name = t.name;
        b.race = t.race; b.cls = t.cls; b.gender = t.gender;
        b.skin = t.skin; b.face = t.face; b.hairStyle = t.hairStyle; b.hairColor = t.hairColor; b.facialHair = t.facialHair;
        b.role = (uint8_t)t.role;
        if (!b.level) b.level = t.level;
        if (!b.maxHp) { b.maxHp = t.hp; b.hp = (int32_t)t.hp; }
        if (!b.maxPower) { b.maxPower = t.power; b.power = (int32_t)t.power; }
        b.powerType = t.powerType;
        b.equipment = t.equipment;
        b.spellCount = t.spellCount;
        for (uint8_t k = 0; k < t.spellCount; ++k) b.spells[k] = t.spells[k];
        b.mapId = data_->mapId;
        b.zoneId = data_->zoneId;
        if (b.x == 0 && b.y == 0) {
            b.x = data_->spawnX + 2.0f + (float)i;
            b.y = data_->spawnY + 2.0f;
            b.z = data_->spawnZ;
        }
    }
    refreshCharacters();
    opened_ = true;
    err.clear();
    return true;
}

void OfflineSession::refreshCharacters() {
    chars_.clear();
    for (uint8_t i = 0; i < save_.rosterCount && i < 4; ++i) {
        const CharacterProfile& p = save_.roster[i];
        if (!p.used) continue;
        Character c;
        c.guid = p.guid;
        c.name = p.name;
        c.race = p.race; c.cls = p.cls; c.gender = p.gender;
        c.skin = p.skin; c.face = p.face; c.hairStyle = p.hairStyle; c.hairColor = p.hairColor; c.facialHair = p.facialHair;
        c.level = (uint8_t)std::min<uint16_t>(p.level, 60);
        c.zone = p.zoneId;
        c.map = p.mapId;
        c.x = p.x; c.y = p.y; c.z = p.z;
        c.firstLogin = false;
        for (int k = 0; k < 20; ++k) {
            const uint32_t entry = k < 19 ? p.equipment[(size_t)k] : 0;
            c.equipDisplay[k] = 0;
            c.equipInv[k] = 0;
            if (!entry) continue;
            if (const ItemTemplate* it = data_ ? data_->item(entry) : nullptr) {
                c.equipDisplay[k] = it->displayId;
                c.equipInv[k] = (uint8_t)it->inventoryType;
            }
        }
        chars_.push_back(std::move(c));
    }
}

int OfflineSession::createCharacter(const CharCreateInfo& ci) {
    if (save_.rosterCount >= 4) return 0x35;   // too many characters
    if (ci.name.size() < 2) return 0x46;       // name too short
    if (ci.name.size() > 12) return 0x47;      // name too long
    for (char ch : ci.name)
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))) return 0x48;
    for (uint8_t i = 0; i < save_.rosterCount; ++i)
        if (save_.roster[i].used && caseInsensitiveEqual(save_.roster[i].name, ci.name)) return 0x31;   // name in use
    CharacterProfile p;
    p.used = true;
    p.guid = 0x0000000000000100ull + (uint64_t)save_.rosterCount;
    p.name = ci.name;
    p.race = ci.race; p.cls = ci.cls; p.gender = ci.gender;
    p.skin = ci.skin; p.face = ci.face; p.hairStyle = ci.hairStyle; p.hairColor = ci.hairColor; p.facialHair = ci.facialHair;
    p.level = 1;
    p.nextXp = 400;
    p.maxHp = 120;
    p.hp = 120;
    p.powerType = ci.cls == 1 ? 1 : (ci.cls == 4 ? 3 : 0);
    p.maxPower = ci.cls == 1 ? 100 : (ci.cls == 4 ? 100 : 120);
    p.power = p.powerType == 1 ? 0 : (int32_t)p.maxPower;
    p.spellCount = fillClassSpells(data_, ci.cls, p.spells);
    p.mapId = data_ ? data_->mapId : 0;
    p.zoneId = data_ ? data_->zoneId : 0;
    if (data_) { p.x = data_->spawnX; p.y = data_->spawnY; p.z = data_->spawnZ; p.o = data_->spawnO; }
    save_.roster[(size_t)save_.rosterCount] = p;   // append: the roster never has gaps
    ++save_.rosterCount;
    refreshCharacters();
    return CHAR_CREATE_SUCCESS;
}

int OfflineSession::deleteCharacter(uint64_t guid) {
    if (save_.rosterCount <= 1) return 0x3A;   // cannot delete the only character
    for (uint8_t i = 0; i < save_.rosterCount; ++i) {
        if (!save_.roster[i].used || save_.roster[i].guid != guid) continue;
        for (uint8_t k = i; k + 1 < save_.rosterCount; ++k) save_.roster[(size_t)k] = save_.roster[(size_t)k + 1];
        save_.roster[(size_t)(save_.rosterCount - 1)] = CharacterProfile();
        --save_.rosterCount;
        if (i < save_.active) --save_.active;
        if (save_.active >= save_.rosterCount) save_.active = 0;
        refreshCharacters();
        return CHAR_DELETE_SUCCESS;
    }
    return 0x3A;
}

bool OfflineSession::enterWorld(uint64_t guid, uint32_t nowMs, float pos[3], float* orient) {
    if (!opened_ || !data_) return false;
    int active = -1;
    for (uint8_t i = 0; i < save_.rosterCount; ++i)
        if (save_.roster[i].used && save_.roster[i].guid == guid) active = i;
    if (active < 0) return false;
    save_.active = (uint8_t)active;
    save_.player = save_.roster[(size_t)active];
    if (save_.player.x == 0 && save_.player.y == 0 && data_) {
        save_.player.x = data_->spawnX; save_.player.y = data_->spawnY; save_.player.z = data_->spawnZ;
        save_.player.o = data_->spawnO;
    }
    if (save_.player.maxHp == 0) { save_.player.maxHp = 120; save_.player.hp = 120; }
    if (save_.player.hp <= 0) save_.player.hp = (int32_t)save_.player.maxHp;
    playerGuid_ = guid;
    playerName_ = save_.player.name;
    mapId_ = save_.player.mapId ? save_.player.mapId : data_->mapId;
    target_ = 0;
    lootGuid_ = 0;
    ghost_ = false;
    inWorld_ = true;
    incoming_.clear();
    outgoing_.clear();
    world_.reset(nowMs, save_.seed, save_.player, save_.bots, partyEnabled_);
    lastPartyStats_ = 0;
    lastMoveSend_ = 0;
    lastSave_ = nowMs;
    partyHash_ = 0;
    lastPartyFull_ = 0;
    if (Entity* p = world_.player()) {
        pos[0] = p->x; pos[1] = p->y; pos[2] = p->z;
        if (orient) *orient = p->o;
    }
    loginPackets(nowMs);
    return true;
}

void OfflineSession::syncProfiles() {
    if (!inWorld_) return;
    for (int i = 0; i < kMaxParty + 1; ++i) world_.syncProfileFromEntity(i);
    save_.player = world_.playerProfile();
    for (int i = 0; i < kMaxParty; ++i) save_.bots[(size_t)i] = world_.botProfile(i);
    save_.quests = world_.questLog();
    save_.commands.tank = world_.bot(0) ? world_.bot(0)->tacticalMode : 1;
    save_.commands.healer = world_.bot(1) ? world_.bot(1)->tacticalMode : 1;
    save_.commands.dps1 = world_.bot(2) ? world_.bot(2)->tacticalMode : 1;
    save_.commands.dps2 = world_.bot(3) ? world_.bot(3)->tacticalMode : 1;
    if (save_.active < 4) save_.roster[save_.active] = save_.player;
}

bool OfflineSession::save(std::string* errOut) {
    std::string err;
    syncProfiles();
    if (save_.playTimeSec == 0) save_.playTimeSec = 1;
    if (!saveOffline(savePath_.c_str(), save_, err)) {
        lastError_ = err;
        if (errOut) *errOut = err;
        return false;
    }
    if (errOut) errOut->clear();
    return true;
}

void OfflineSession::leaveWorld() {
    if (!inWorld_) return;
    save();
    inWorld_ = false;
    world_.clearEvents();
}

// ---------------------------------------------------------------- packet plumbing
void OfflineSession::send(uint16_t op, Bytes data) {
    outgoing_.emplace_back(op, std::move(data));
    stats.maxOutQueue = std::max<uint32_t>(stats.maxOutQueue, (uint32_t)outgoing_.size());
}

void OfflineSession::flush() {
    if (outgoing_.empty()) return;
    std::vector<std::pair<uint16_t, Bytes>> out;
    out.swap(outgoing_);
    if (!sink_) return;
    for (auto& p : out) {
        ++stats.packetsOut;
        sink_(p.first, p.second);
    }
}

void OfflineSession::sendUpdateObject(const Bytes& body) { send(SMSG_UPDATE_OBJECT, body); }

void OfflineSession::sendChat(uint8_t type, uint64_t sender, const std::string& text, const std::string& name) {
    Writer w;
    w.u8(type);
    w.u32(7);   // LANG_COMMON
    if (type == 0x0B || type == 0x0C || type == 0x0D || type == 0x1A) {
        w.u64(sender);
        w.u32((uint32_t)name.size() + 1);
        w.raw(name.c_str(), name.size() + 1);
        w.u64(0);
    } else if (type == 0x00 || type == 0x01 || type == 0x05) {
        w.u64(sender);
        w.u64(sender);
    } else {
        w.u64(sender);
    }
    w.u32((uint32_t)text.size() + 1);
    w.raw(text.c_str(), text.size() + 1);
    w.u8(0);
    send(SMSG_MESSAGECHAT, w.b);
}

void OfflineSession::errorLine(const std::string& text, uint32_t nowMs) { (void)nowMs; sendChat(0x0A, 0, text); }

void OfflineSession::sendSelfFields() {
    Entity* p = world_.player();
    if (!p) return;
    FieldSet f;
    f.set(UF::UNIT_HEALTH, (uint32_t)std::max<int32_t>(0, p->hp));
    f.set(UF::UNIT_MAXHEALTH, p->maxHp);
    f.set(UF::UNIT_POWER1 + p->powerType, wirePower(*p));
    f.set(UF::UNIT_MAXPOWER1 + p->powerType, p->maxPower);
    f.set(UF::UNIT_LEVEL, p->level);
    f.set(UF::PLAYER_XP, p->levelXp);
    f.set(UF::PLAYER_NEXT_LEVEL_XP, p->nextLevelXp);
    f.set(UF::PLAYER_FIELD_COINAGE, p->money);
    f.set(UF::PLAYER_FLAGS, ghost_ ? 0x10u : 0u);
    UpdateBuilder u;
    addValues(u, p->guid, f);
    sendUpdateObject(u.finish());
}

void OfflineSession::sendInventory() {
    Entity* p = world_.player();
    if (!p) return;
    const CharacterProfile& pr = world_.playerProfile();
    UpdateBuilder u;
    FieldSet self;
    // Worn items are visible on the model: PLAYER_VISIBLE_ITEM_1_0 + 12 * slot.
    for (int slot = 0; slot < 19; ++slot) {
        const uint32_t entry = pr.equipment[(size_t)slot];
        self.set((uint16_t)(UF::PLAYER_VISIBLE_ITEM_1_0 + UF::PLAYER_VISIBLE_ITEM_STRIDE * slot), entry);
        if (!entry) {   // WoWVita: an emptied slot (an off hand moved to the backpack)
            const uint16_t f = (uint16_t)(UF::PLAYER_FIELD_INV_SLOT_HEAD + slot * 2);
            self.set(f, 0);
            self.set((uint16_t)(f + 1), 0);
            continue;
        }
        const uint64_t g = itemGuidFor(true, slot);
        const uint16_t f = (uint16_t)(UF::PLAYER_FIELD_INV_SLOT_HEAD + slot * 2);
        self.set(f, (uint32_t)g);
        self.set((uint16_t)(f + 1), (uint32_t)(g >> 32));
        FieldSet it;
        it.set(UF::OBJECT_GUID, (uint32_t)g);
        it.set((uint16_t)(UF::OBJECT_GUID + 1), (uint32_t)(g >> 32));
        it.set(UF::OBJECT_TYPE, 0x3);
        it.set(UF::OBJECT_ENTRY, entry);
        it.set(UF::OBJECT_SCALE_X, 0x3F800000u);
        it.set(UF::ITEM_FIELD_STACK_COUNT, 1);
        addCreateItem(u, g, it);
    }
    // Backpack slots 23..38.
    for (uint8_t i = 0; i < pr.bagCount && i < 20; ++i) {
        if (!pr.bag[i].entry) continue;
        const uint64_t g = itemGuidFor(false, i);
        const int slot = 23 + i;
        if (slot >= 39) break;
        const uint16_t f = (uint16_t)(UF::PLAYER_FIELD_INV_SLOT_HEAD + slot * 2);
        self.set(f, (uint32_t)g);
        self.set((uint16_t)(f + 1), (uint32_t)(g >> 32));
        FieldSet it;
        it.set(UF::OBJECT_GUID, (uint32_t)g);
        it.set((uint16_t)(UF::OBJECT_GUID + 1), (uint32_t)(g >> 32));
        it.set(UF::OBJECT_TYPE, 0x3);
        it.set(UF::OBJECT_ENTRY, pr.bag[i].entry);
        it.set(UF::OBJECT_SCALE_X, 0x3F800000u);
        it.set(UF::ITEM_FIELD_STACK_COUNT, pr.bag[i].count ? pr.bag[i].count : 1);
        addCreateItem(u, g, it);
    }
    for (int i = pr.bagCount; i < 16; ++i) {   // WoWVita: a sold item's slot is empty again
        const uint16_t f = (uint16_t)(UF::PLAYER_FIELD_INV_SLOT_HEAD + (23 + i) * 2);
        self.set(f, 0);
        self.set((uint16_t)(f + 1), 0);
    }
    addValues(u, p->guid, self);
    sendUpdateObject(u.finish());
}

// ---------------------------------------------------------------- WoWVita: vendors
void OfflineSession::sendListInventory(uint64_t npc) {
    const Entity* e = world_.entity(npc);
    if (!e || !data_) return;
    Writer w;
    w.u64(npc);
    auto it = data_->vendors.find(e->entry);
    if (it == data_->vendors.end()) { w.u8(0); w.u8(0); send(SMSG_LIST_INVENTORY, w.b); return; }
    const auto& goods = it->second;
    const uint8_t n = (uint8_t)std::min<size_t>(goods.size(), 255);
    w.u8(n);
    for (uint8_t i = 0; i < n; ++i) {
        const ItemTemplate* t = data_->item(goods[i].item);
        w.u32((uint32_t)i + 1);                  // slot (visible row, from 1)
        w.u32(goods[i].item);
        w.u32(t ? t->displayId : 0);
        w.u32(0xFFFFFFFFu);                      // unlimited stock
        w.u32(t ? t->buyPrice : 0);
        w.u32(0);                                // max durability
        w.u32(goods[i].buyCount);
    }
    send(SMSG_LIST_INVENTORY, w.b);
}

void OfflineSession::handleVendor(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p || !data_) return;
    Reader r(data);
    const uint64_t npc = r.u64();
    if (r.bad) return;
    const Entity* v = world_.entity(npc);
    const bool isVendor = v && v->alive() && (v->npcFlags & 0x4u) && data_->vendors.count(v->entry);
    const bool near = v && std::hypot(v->x - p->x, v->y - p->y) < 12.0f;
    switch (op) {
        case CMSG_GOSSIP_HELLO:
        case CMSG_LIST_INVENTORY:
            if (isVendor && near) sendListInventory(npc);
            return;
        case CMSG_BUY_ITEM: {
            const uint32_t item = r.u32();
            const uint8_t bundles = r.u8();
            if (r.bad || !isVendor || !near) return;
            const auto& goods = data_->vendors.at(v->entry);
            uint32_t slot = 0;
            const VendorGoods* g = nullptr;
            for (size_t i = 0; i < goods.size() && i < 255; ++i)
                if (goods[i].item == item) { g = &goods[i]; slot = (uint32_t)i + 1; break; }
            const ItemTemplate* t = data_->item(item);
            auto fail = [&](uint8_t why) {
                Writer w;
                w.u64(npc); w.u32(item); w.u8(why);
                send(SMSG_BUY_FAILED, w.b);
            };
            if (!g || !t) { fail(0); return; }   // BUY_ERR_CANT_FIND_ITEM
            const uint32_t n = std::max<uint32_t>(1, bundles);
            int why = 0;
            if (!world_.buyItem(item, n * g->buyCount, t->buyPrice * n, nowMs, &why)) {
                if (why == 1) fail(2);           // BUY_ERR_NOT_ENOUGHT_MONEY
                else errorLine("Inventory is full.", nowMs);
                return;
            }
            Writer w;
            w.u64(npc); w.u32(slot); w.u32(0xFFFFFFFFu); w.u32(n * g->buyCount);
            send(SMSG_BUY_ITEM, w.b);
            sendSelfFields();   // the money
            return;
        }
        case CMSG_SELL_ITEM: {
            const uint64_t itemGuid = r.u64();
            const uint8_t count = r.u8();
            if (r.bad) return;
            const uint32_t low = (uint32_t)itemGuid;
            const int index = (low & 0xFFFFF000u) == (0x40000000u | 0x2000u) ? (int)(low & 0xFFFu) : -1;
            const uint8_t before = world_.playerProfile().bagCount;
            const uint32_t paid = (isVendor && near && index >= 0) ? world_.sellBagItem(index, count, nowMs) : 0;
            if (!paid) {
                Writer w;
                w.u64(npc); w.u64(itemGuid); w.u8(2);   // SELL_ERR_CANT_SELL_ITEM
                send(SMSG_SELL_ITEM, w.b);
                return;
            }
            // The slots after the sold one moved up: their objects go and come back with the new contents.
            for (int i = index; i < before; ++i) {
                Writer w;
                w.u64((uint64_t)itemGuidFor(false, i));
                send(0x0AA, w.b);   // SMSG_DESTROY_OBJECT
            }
            sendInventory();
            sendSelfFields();
            return;
        }
        default:
            return;
    }
}

void OfflineSession::sendPartyList() {
    Writer w;
    w.u8(0);       // not a raid
    w.u8(0);       // our subgroup / assistant
    w.u32(kMaxParty);
    for (int i = 0; i < kMaxParty; ++i) {
        const CharacterProfile& b = world_.botProfile(i);
        w.cstr(b.name);
        w.u64(b.guid);
        w.u8(1);   // online
        w.u8(0);   // subgroup
    }
    w.u64(playerGuid_);   // we are the leader
    w.u8(0);               // loot method: free for all
    w.u64(0);
    w.u8(0);
    w.u8(0);               // difficulty (1.11+)
    send(SMSG_GROUP_LIST, w.b);
}

void OfflineSession::sendPartyStats() {
    for (int i = 0; i < kMaxParty; ++i) {
        const Entity* b = world_.bot(i);
        if (!b) continue;
        Writer w;
        w.packedGuid(b->guid);
        w.u32(0xFF);   // status, hp, max hp, power type, power, max power, level, zone
        w.u8(b->alive() ? 1 : (b->dead ? 4 : 1));
        w.u16((uint16_t)std::max<int32_t>(0, b->hp));
        w.u16((uint16_t)b->maxHp);
        w.u8(b->powerType);
        w.u16((uint16_t)std::max<int32_t>(0, b->power));
        w.u16((uint16_t)b->maxPower);
        w.u16(b->level);
        w.u16((uint16_t)(data_ ? data_->zoneId : 0));
        send(SMSG_PARTY_MEMBER_STATS_FULL, w.b);
    }
}

void OfflineSession::sendCreatureQuery(uint32_t entry) {
    const CreatureTemplate* t = data_ ? data_->creature(entry) : nullptr;
    Writer w;
    if (!t) { w.u32(entry | 0x80000000u); send(SMSG_CREATURE_QUERY_RESPONSE, w.b); return; }
    w.u32(entry);
    w.cstr(t->name);
    w.cstr(""); w.cstr(""); w.cstr("");
    w.cstr(t->subname);
    w.u32(0);                    // type flags
    w.u32(t->creatureType);
    w.u32(0);                    // family
    w.u32(t->rank);
    w.u32(0);                    // pet display
    w.u32(0);                    // pet level
    w.u32(t->displayId);
    w.u8(0); w.u8(0);
    send(SMSG_CREATURE_QUERY_RESPONSE, w.b);
}

void OfflineSession::sendPlayerName(uint64_t guid) {
    std::string name;
    uint32_t race = 1, gender = 0, cls = 1;
    if (guid == playerGuid_) {
        name = world_.playerProfile().name;
        race = world_.playerProfile().race;
        gender = world_.playerProfile().gender;
        cls = world_.playerProfile().cls;
    } else {
        for (int i = 0; i < kMaxParty; ++i)
            if (world_.botProfile(i).guid == guid) {
                name = world_.botProfile(i).name;
                race = world_.botProfile(i).race;
                gender = world_.botProfile(i).gender;
                cls = world_.botProfile(i).cls;
            }
    }
    if (name.empty()) return;
    Writer w;
    w.u64(guid);
    w.cstr(name);
    w.cstr("");       // realm
    w.u32(race);
    w.u32(gender);
    w.u32(cls);
    send(SMSG_NAME_QUERY_RESPONSE, w.b);
}

void OfflineSession::sendItemQuery(uint32_t entry) {
    Writer w;
    const ItemTemplate* t = data_ ? data_->item(entry) : nullptr;
    if (!t) { w.u32(entry | 0x80000000u); send(SMSG_ITEM_QUERY_SINGLE_RESPONSE, w.b); return; }
    w.u32(entry);
    w.u32(t->cls);
    w.u32(t->subclass);
    w.cstr(t->name);
    w.cstr(""); w.cstr(""); w.cstr("");
    w.u32(t->displayId);
    w.u32(t->quality);
    w.u32(0);                 // flags
    w.u32(t->buyPrice);
    w.u32(t->sellPrice);
    w.u32(t->inventoryType);
    w.u32(0); w.u32(0);       // allowable class / race
    w.u32(t->itemLevel);
    w.u32(t->requiredLevel);
    w.u32(0); w.u32(0);       // required skill / rank
    for (int i = 0; i < 5; ++i) w.u32(0);   // spell, honor, city, rep faction, rep rank
    w.u32(0);                 // max count
    w.u32(t->maxStack);
    w.u32(0);                 // container slots
    for (int i = 0; i < 10; ++i) { w.u32(0); w.u32(0); }   // stats
    for (int i = 0; i < 5; ++i) {
        w.f32(i == 0 ? t->dmgMin : 0.0f);
        w.f32(i == 0 ? t->dmgMax : 0.0f);
        w.u32(0);             // damage school
    }
    w.u32(t->armor);
    for (int i = 0; i < 6; ++i) w.u32(0);   // resistances
    w.u32(t->delayMs);
    w.u32(0);                 // ammo type
    w.f32(0.0f);              // ranged range modifier
    for (int i = 0; i < 5; ++i) { for (int k = 0; k < 6; ++k) w.u32(0); }   // spell, trigger, charges, cd, category, cat cd
    w.u32(0);                 // bonding
    w.cstr(t->description);
    w.u32(0); w.u32(0); w.u32(0);   // page text, language, page material
    w.u32(0);                 // start quest
    w.u32(0);                 // lock id
    for (int i = 0; i < 5; ++i) w.u32(0);   // material, sheath, random property, block, item set
    w.u32(t->inventoryType ? 60u : 0u);     // max durability
    send(SMSG_ITEM_QUERY_SINGLE_RESPONSE, w.b);
}

void OfflineSession::sendSpellStart(const Entity& caster, uint32_t spell, uint64_t target, uint32_t castMs, uint8_t school) {
    (void)school;
    Writer w;
    w.packedGuid(0);
    w.packedGuid(caster.guid);
    w.u32(spell);
    w.u16(0);            // cast flags
    w.u32(castMs);
    if (target && target != caster.guid) {
        w.u16(0x0002);   // TARGET_FLAG_UNIT
        w.packedGuid(target);
    } else {
        w.u16(0);
    }
    send(SMSG_SPELL_START, w.b);
}

void OfflineSession::sendSpellGo(const Entity& caster, uint32_t spell, uint64_t target, uint8_t school) {
    (void)school;
    Writer w;
    w.packedGuid(0);
    w.packedGuid(caster.guid);
    w.u32(spell);
    w.u16(0);            // cast flags
    if (target && target != caster.guid) {
        w.u8(1);
        w.u64(target);
        w.u8(0);         // misses
        w.u16(0x0002);
        w.packedGuid(target);
    } else {
        w.u8(0);
        w.u8(0);
        w.u16(0);
    }
    send(SMSG_SPELL_GO, w.b);
}

void OfflineSession::sendCastFailed(const Entity& caster, uint32_t spell, uint8_t reason) {
    // Translate the simulation's internal reason into 1.12 SpellCastResult codes the client knows.
    uint8_t out = 0;
    switch (reason) {
        case 1: out = 31; break;   // busy / another spell in progress
        case 2: out = 26; break;   // global cooldown: not ready
        case 3: out = 26; break;   // cooldown: not ready
        case 4: out = 77; break;   // not enough power
        case 5: case 6: case 8: case 10: out = 10; break;   // bad targets
        case 7: out = 4; break;    // out of range
        default: out = 0; break;   // generic failure
    }
    if (reason == 35) {   // interrupted
        Writer f;
        f.u64(caster.guid);
        f.u32(spell);
        f.u8(35);
        send(SMSG_SPELL_FAILURE, f.b);
        return;
    }
    Writer w;
    w.u32(spell);
    w.u8(2);        // failed
    w.u8(out);
    send(SMSG_CAST_FAILED, w.b);
}

// ---------------------------------------------------------------- login
void OfflineSession::loginPackets(uint32_t nowMs) {
    (void)nowMs;
    Entity* p = world_.player();
    if (!p || !data_) return;

    // Server clock (the sky follows it): 12:00, normal speed.
    {
        Writer w;
        w.u32((12u << 6) | 0u);
        w.f32(1.0f / 60.0f);
        send(SMSG_LOGIN_SETTIMESPEED, w.b);
    }

    UpdateBuilder u;
    // ---- self: full player fields
    {
        const CharacterProfile& pr = world_.playerProfile();
        FieldSet f;
        f.set(UF::OBJECT_GUID, (uint32_t)p->guid);
        f.set((uint16_t)(UF::OBJECT_GUID + 1), (uint32_t)(p->guid >> 32));
        f.set(UF::OBJECT_TYPE, 0x19);
        f.set(UF::OBJECT_ENTRY, 0);
        f.set(UF::OBJECT_SCALE_X, 0x3F800000u);
        f.set(UF::UNIT_HEALTH, (uint32_t)std::max<int32_t>(0, p->hp));
        f.set(UF::UNIT_MAXHEALTH, p->maxHp);
        f.set(UF::UNIT_LEVEL, p->level);
        f.set(UF::UNIT_FACTIONTEMPLATE, 1);
        f.set(UF::UNIT_BYTES_0, (uint32_t)pr.race | ((uint32_t)pr.cls << 8) | ((uint32_t)pr.gender << 16) |
                                      ((uint32_t)pr.powerType << 24));
        f.set(UF::UNIT_POWER1 + pr.powerType, wirePower(*p));
        f.set(UF::UNIT_MAXPOWER1 + pr.powerType, p->maxPower);
        f.set(UF::UNIT_DISPLAYID, nativeDisplay(pr.race, pr.gender));
        f.set(UF::UNIT_NATIVEDISPLAYID, nativeDisplay(pr.race, pr.gender));
        f.set(UF::UNIT_BOUNDINGRADIUS, 0x3F000000u);
        f.set(UF::UNIT_COMBATREACH, 0x3FC00000u);
        f.set(UF::PLAYER_FLAGS, 0);
        f.set(UF::PLAYER_BYTES, (uint32_t)pr.skin | ((uint32_t)pr.face << 8) | ((uint32_t)pr.hairStyle << 16) |
                                    ((uint32_t)pr.hairColor << 24));
        f.set(UF::PLAYER_BYTES_2, pr.facialHair);
        f.set(UF::PLAYER_XP, p->levelXp);
        f.set(UF::PLAYER_NEXT_LEVEL_XP, p->nextLevelXp);
        f.set(UF::PLAYER_FIELD_COINAGE, p->money);
        addCreate(u, p->guid, 4, kUfSelf | kUfAll, p->x, p->y, p->z, p->o, f);
    }
    // ---- the four companions
    for (int i = 0; i < kMaxParty; ++i) {
        const Entity* b = world_.bot(i);
        if (!b) continue;
        const CharacterProfile& pr = world_.botProfile(i);
        FieldSet f;
        f.set(UF::OBJECT_GUID, (uint32_t)b->guid);
        f.set((uint16_t)(UF::OBJECT_GUID + 1), (uint32_t)(b->guid >> 32));
        f.set(UF::OBJECT_TYPE, 0x19);
        f.set(UF::OBJECT_SCALE_X, 0x3F800000u);
        f.set(UF::UNIT_HEALTH, (uint32_t)std::max<int32_t>(0, b->hp));
        f.set(UF::UNIT_MAXHEALTH, b->maxHp);
        f.set(UF::UNIT_LEVEL, b->level);
        f.set(UF::UNIT_FACTIONTEMPLATE, 1);
        f.set(UF::UNIT_BYTES_0, (uint32_t)pr.race | ((uint32_t)pr.cls << 8) | ((uint32_t)pr.gender << 16) |
                                      ((uint32_t)pr.powerType << 24));
        f.set(UF::UNIT_POWER1 + pr.powerType, wirePower(*b));
        f.set(UF::UNIT_MAXPOWER1 + pr.powerType, b->maxPower);
        f.set(UF::UNIT_DISPLAYID, nativeDisplay(pr.race, pr.gender));
        f.set(UF::UNIT_NATIVEDISPLAYID, nativeDisplay(pr.race, pr.gender));
        f.set(UF::UNIT_BOUNDINGRADIUS, 0x3F000000u);
        f.set(UF::UNIT_COMBATREACH, 0x3FC00000u);
        f.set(UF::PLAYER_BYTES, (uint32_t)pr.skin | ((uint32_t)pr.face << 8) | ((uint32_t)pr.hairStyle << 16) |
                                    ((uint32_t)pr.hairColor << 24));
        f.set(UF::PLAYER_BYTES_2, pr.facialHair);
        for (int slot = 0; slot < 19; ++slot)
            f.set((uint16_t)(UF::PLAYER_VISIBLE_ITEM_1_0 + UF::PLAYER_VISIBLE_ITEM_STRIDE * slot), pr.equipment[(size_t)slot]);
        addCreate(u, b->guid, 4, kUfHighGuid, b->x, b->y, b->z, b->o, f);
    }
    // ---- nearby creatures
    float px = p->x, py = p->y;
    for (int i = 0; i < kMaxEntities; ++i) {
        const Entity* e = world_.byIndex(i);
        if (!e || !e->used || e->objType != 3) continue;
        const float dx = e->x - px, dy = e->y - py;
        if (dx * dx + dy * dy > 150.0f * 150.0f) continue;
        addCreatureCreate(u, *e);
    }
    sendUpdateObject(u.finish());

    // ---- spell book and action bar
    {
        const CharacterProfile& pr = world_.playerProfile();
        Writer w;
        w.u8(0);
        w.u16(pr.spellCount);
        for (uint8_t i = 0; i < pr.spellCount; ++i) { w.u16((uint16_t)pr.spells[i]); w.u16(i); }
        w.u16(0);   // no cooldowns
        send(SMSG_INITIAL_SPELLS, w.b);
    }
    {
        Writer w;
        uint32_t bar[120] = {0};
        bar[0] = 6603;
        const CharacterProfile& pr = world_.playerProfile();
        for (uint8_t i = 0; i < pr.spellCount && i + 1 < 120; ++i)
            if (pr.spells[i] != 6603) bar[i + 1] = pr.spells[i];
        for (int i = 0; i < 120; ++i) w.u32(bar[i]);
        send(SMSG_ACTION_BUTTONS, w.b);
    }

    sendInventory();
    if (partyEnabled_) {
        sendPartyList();
        sendPartyStats();
    }
    sendChat(0x0A, 0, "Offline world: " + (data_ ? data_->name : std::string("unnamed")) +
                            (partyEnabled_ ? ". Party commands: /p follow, assist, stay, retreat." : "."));
    if (ghost_) errorLine("You are a ghost: reclaim your corpse to return to life.", 0);
}

// ---------------------------------------------------------------- client packets
void OfflineSession::onClientPacket(uint16_t op, const Bytes& data, uint32_t nowMs) {
    if (!inWorld_) return;
    ++stats.packetsIn;
    if (incoming_.size() > 256) return;   // a runaway client must not grow the queue without bound
    incoming_.emplace_back(op, data);
    (void)nowMs;
}

void OfflineSession::handleMovement(const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return;
    Reader r(data);
    MovementInfo m;
    if (!readMovementInfo(r, m)) return;
    if (!(m.x > -1e6f && m.x < 1e6f && m.y > -1e6f && m.y < 1e6f && m.z > -1e6f && m.z < 1e6f)) return;
    // The client is the authority for its own position in offline mode; the simulation consumes it.
    float o = m.o;
    if (!(o >= 0.0f)) o = 0.0f;
    world_.setPosition(*p, m.x, m.y, m.z, normalizeAngle(o));
    (void)nowMs;
}

void OfflineSession::handleAttack(const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return;
    Reader r(data);
    const uint64_t g = r.u64();
    if (r.bad) return;
    Entity* t = world_.entity(g);
    if (!t || !t->alive()) { send(SMSG_ATTACKSWING_DEADTARGET); return; }
    if (!world_.canAttack(*p, *t)) { send(SMSG_ATTACKSWING_CANT_ATTACK); return; }
    if (!world_.inMeleeRange(*p, *t)) { send(SMSG_ATTACKSWING_NOTINRANGE); return; }
    const float want = std::atan2(t->y - p->y, t->x - p->x);
    float diff = p->o - want;
    while (diff > 3.14159265f) diff -= 6.2831853f;
    while (diff < -3.14159265f) diff += 6.2831853f;
    if (std::fabs(diff) > 1.5707963f) { send(SMSG_ATTACKSWING_BADFACING); return; }
    if (!world_.startAttack(*p, g, nowMs)) send(SMSG_ATTACKSWING_CANT_ATTACK);
}

void OfflineSession::handleCast(const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return;
    Reader r(data);
    const uint32_t spell = r.u32();
    const uint16_t mask = r.u16();
    uint64_t target = 0;
    if (!r.bad && (mask & 0x0002)) target = r.packedGuid();
    if (r.bad) return;
    if (spell == 6603) return;   // Attack is a toggle, never a cast (the client enforces this too)
    // No unit target in the packet means: self / no explicit target. Ground-target spells are not
    // supported yet, so resolving to self is the correct, safe default.
    if (!target) target = p->guid;
    world_.tryCast(*p, spell, target, nowMs);
}

void OfflineSession::handleLoot(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return;
    Reader r(data);
    if (op == CMSG_LOOT) {
        const uint64_t g = r.u64();
        Entity* corpse = world_.entity(g);
        std::string why;
        if (!corpse || !world_.openLoot(*corpse, nowMs, &why)) {
            Writer w;
            w.u64(g);
            w.u8(0);
            w.u8(why.find("far") != std::string::npos ? 4 : 0);
            send(SMSG_LOOT_RESPONSE, w.b);
            if (!why.empty()) errorLine(why, nowMs);
            return;
        }
        lootGuid_ = g;
        ++stats.loots;
        Writer w;
        w.u64(g);
        w.u8(1);
        w.u32(corpse->lootMoney);
        uint8_t n = 0;
        for (uint8_t i = 0; i < corpse->lootCount; ++i)
            if (!corpse->loot[i].taken) ++n;
        w.u8(n);
        for (uint8_t i = 0; i < corpse->lootCount; ++i) {
            if (corpse->loot[i].taken) continue;
            const ItemTemplate* it = data_ ? data_->item(corpse->loot[i].item) : nullptr;
            w.u8(i);
            w.u32(corpse->loot[i].item);
            w.u32(corpse->loot[i].count);
            w.u32(it ? it->displayId : 0);
            w.u32(0);
            w.u32(0);
            w.u8(0);
        }
        send(SMSG_LOOT_RESPONSE, w.b);
        return;
    }
    if (op == CMSG_LOOT_MONEY) {
        Entity* corpse = world_.entity(lootGuid_);
        if (!corpse) return;
        const uint32_t m = world_.takeLootMoney(*corpse, nowMs);
        if (m) {
            send(SMSG_LOOT_CLEAR_MONEY);
            sendSelfFields();
            char line[64];
            std::snprintf(line, sizeof line, "You loot %u copper.", (unsigned)m);
            sendChat(0x0A, 0, line);
        }
        return;
    }
    if (op == CMSG_AUTOSTORE_LOOT_ITEM) {
        const uint8_t slot = r.u8();
        Entity* corpse = world_.entity(lootGuid_);
        if (!corpse) return;
        if (world_.takeLoot(*corpse, slot, nowMs)) {
            Writer w;
            w.u8(slot);
            send(SMSG_LOOT_REMOVED, w.b);
            sendInventory();
        }
        return;
    }
    if (op == CMSG_LOOT_RELEASE) {
        Writer w;
        w.u64(lootGuid_);
        w.u8(1);
        send(SMSG_LOOT_RELEASE_RESPONSE, w.b);
        lootGuid_ = 0;
        return;
    }
}

void OfflineSession::handleChat(const Bytes& data, uint32_t nowMs) {
    Reader r(data);
    const uint32_t type = r.u32();
    r.u32();   // language
    std::string to, text;
    if (type == 6 || type == 14) { to = r.cstr(); text = r.cstr(); }
    else text = r.cstr();
    if (r.bad) return;
    if (type == 1) {   // party
        world_.handlePartyChat(text, nowMs);
        sendChat(0x01, playerGuid_, text);
    } else if (type == 0) {   // say (local echo: there are no other players)
        sendChat(0x00, playerGuid_, text);
    }
}

void OfflineSession::handleDeath(uint16_t op, const Bytes& data, uint32_t nowMs) {
    (void)data;
    Entity* p = world_.player();
    if (!p) return;
    if (op == CMSG_REPOP_REQUEST) {
        if (!p->dead) { errorLine("You are not dead.", nowMs); return; }
        ghost_ = true;
        p->hp = 1;
        p->victim = 0;
        sendSelfFields();
        Writer w;
        w.u32(5000);
        send(SMSG_CORPSE_RECLAIM_DELAY, w.b);
        sendChat(0x0A, 0, "You are a ghost. Reclaim your corpse to return to life.");
        return;
    }
    if (op == CMSG_RECLAIM_CORPSE) {
        if (!ghost_) return;
        ghost_ = false;
        world_.revive(*p, 50, nowMs);
        sendSelfFields();
        sendChat(0x0A, 0, "You are alive again.");
    }
}

void OfflineSession::handleQuery(uint16_t op, const Bytes& data, uint32_t nowMs) {
    (void)nowMs;
    Reader r(data);
    switch (op) {
        case CMSG_NAME_QUERY: sendPlayerName(r.u64()); break;
        case CMSG_CREATURE_QUERY: { uint32_t entry = r.u32(); r.u64(); sendCreatureQuery(entry); break; }
        case CMSG_GAMEOBJECT_QUERY: {
            uint32_t entry = r.u32();
            Writer w;
            w.u32(entry | 0x80000000u);
            send(SMSG_GAMEOBJECT_QUERY_RESPONSE, w.b);
            break;
        }
        case CMSG_ITEM_QUERY_SINGLE: { uint32_t entry = r.u32(); sendItemQuery(entry); break; }
        case CMSG_REQUEST_PARTY_MEMBER_STATS: {
            const uint64_t g = r.u64();
            for (int i = 0; i < kMaxParty; ++i) {
                const Entity* b = world_.bot(i);
                if (!b || b->guid != g) continue;
                Writer w;
                w.packedGuid(b->guid);
                w.u32(0xFF);
                w.u8(b->alive() ? 1 : 4);
                w.u16((uint16_t)std::max<int32_t>(0, b->hp));
                w.u16((uint16_t)b->maxHp);
                w.u8(b->powerType);
                w.u16((uint16_t)std::max<int32_t>(0, b->power));
                w.u16((uint16_t)b->maxPower);
                w.u16(b->level);
                w.u16((uint16_t)(data_ ? data_->zoneId : 0));
                send(SMSG_PARTY_MEMBER_STATS_FULL, w.b);
                break;
            }
            break;
        }
        case CMSG_SET_SELECTION: target_ = r.u64(); break;
        default: break;
    }
}

// ---------------------------------------------------------------- events -> packets
void OfflineSession::dispatchEvents(uint32_t nowMs) {
    const SimEvent* ev = world_.events();
    const int n = world_.eventCount();
    bool inventoryDirty = false;
    for (int i = 0; i < n; ++i) {
        const SimEvent& e = ev[i];
        Entity* a = world_.entity(e.a);
        Entity* b = world_.entity(e.b);
        switch (e.type) {
            case SimEvent::Damage: {
                if (!a || !b) break;
                if (e.spell == 0) {
                    Writer w;
                    if (e.value <= 0) {
                        w.u32(0x10);   // miss
                        w.packedGuid(a->guid);
                        w.packedGuid(b->guid);
                        w.u32(0);
                        w.u8(0);
                        w.u32(0);
                        w.u32(0); w.u32(0);
                        send(SMSG_ATTACKERSTATEUPDATE, w.b);
                        break;
                    }
                    w.u32((e.flags & 2) ? 0x80u : 0u);
                    w.packedGuid(a->guid);
                    w.packedGuid(b->guid);
                    w.u32((uint32_t)e.value);
                    w.u8(1);
                    w.u32(0);                       // school: physical
                    w.f32((float)e.value);
                    w.u32((uint32_t)e.value);
                    w.u32(0); w.u32(0);
                    w.u32(1);                       // victim state: hit
                    w.u32(0);                       // attacker state
                    w.u32(0);                       // melee spell id
                    send(SMSG_ATTACKERSTATEUPDATE, w.b);
                } else {
                    Writer w;
                    w.packedGuid(b->guid);
                    w.packedGuid(a->guid);
                    w.u32(e.spell);
                    w.u32((uint32_t)e.value);
                    w.u8(e.school);
                    w.u32(0); w.u32(0);
                    w.u8(0); w.u8(0);
                    w.u32(0);
                    w.u32((e.flags & 2) ? 2u : 0u);
                    send(SMSG_SPELLNONMELEEDAMAGELOG, w.b);
                }
                FieldSet f;
                f.set(UF::UNIT_HEALTH, (uint32_t)std::max<int32_t>(0, b->hp));
                UpdateBuilder u;
                addValues(u, b->guid, f);
                sendUpdateObject(u.finish());
                break;
            }
            case SimEvent::Heal: {
                if (!a || !b) break;
                Writer w;
                w.packedGuid(b->guid);
                w.packedGuid(a->guid);
                w.u32(e.spell);
                w.u32((uint32_t)std::max<int32_t>(0, e.value));
                send(SMSG_SPELLHEALLOG, w.b);
                FieldSet f;
                f.set(UF::UNIT_HEALTH, (uint32_t)std::max<int32_t>(0, b->hp));
                UpdateBuilder u;
                addValues(u, b->guid, f);
                sendUpdateObject(u.finish());
                break;
            }
            case SimEvent::Death: {
                if (!a) break;
                FieldSet f;
                f.set(UF::UNIT_HEALTH, 0);
                if (a->objType == 3) f.set(UF::UNIT_DYNAMIC_FLAGS, 1);
                UpdateBuilder u;
                addValues(u, a->guid, f);
                sendUpdateObject(u.finish());
                if (a->self) {
                    ++stats.deaths;
                    sendChat(0x0A, 0, "You have died. Release your spirit, then reclaim your corpse.");
                }
                if (a->objType == 3) ++stats.kills;
                // The attacker's auto-attack stops when the victim dies.
                if (b && b->self) {
                    Writer w;
                    w.packedGuid(b->guid);
                    w.packedGuid(a->guid);
                    w.u32(0);
                    send(SMSG_ATTACKSTOP, w.b);
                }
                break;
            }
            case SimEvent::Appear: {   // WoWVita: the spawn grid brought a creature into range
                if (!a) break;
                UpdateBuilder u;
                addCreatureCreate(u, *a);
                sendUpdateObject(u.finish());
                break;
            }
            case SimEvent::Vanish: {   // ... and took one out of it (the entity is gone: only the guid is left)
                Writer w;
                w.u64(e.a);
                send(0x0AA, w.b);   // SMSG_DESTROY_OBJECT
                break;
            }
            case SimEvent::Respawn: {
                if (!a) break;
                FieldSet f;
                f.set(UF::UNIT_HEALTH, (uint32_t)std::max<int32_t>(0, a->hp));
                f.set(UF::UNIT_DYNAMIC_FLAGS, 0);
                UpdateBuilder u;
                addValues(u, a->guid, f);
                sendUpdateObject(u.finish());
                break;
            }
            case SimEvent::XpGain: {
                if (!a) break;
                Writer w;
                w.u64(0);            // victim (unknown for quest XP)
                w.u32((uint32_t)std::max<int32_t>(0, e.value));
                w.u8(0);             // kill
                w.u32(0);
                w.f32(1.0f);
                send(SMSG_LOG_XPGAIN, w.b);
                sendSelfFields();
                break;
            }
            case SimEvent::LevelUp: {
                if (a) sendChat(0x0A, 0, "You have reached level " + std::to_string((int)e.value) + "!");
                sendSelfFields();
                break;
            }
            case SimEvent::AttackStarted: {
                if (!a || !b) break;
                Writer w;
                w.u64(a->guid);
                w.u64(b->guid);
                send(SMSG_ATTACKSTART, w.b);
                break;
            }
            case SimEvent::AttackStopped: {
                if (!a) break;
                Writer w;
                w.packedGuid(a->guid);
                w.packedGuid(e.b);
                w.u32(0);
                send(SMSG_ATTACKSTOP, w.b);
                break;
            }
            case SimEvent::CastStarted: {
                if (!a) break;
                sendSpellStart(*a, e.spell, e.b, (uint32_t)std::max<int32_t>(0, e.value), e.school);
                break;
            }
            case SimEvent::CastFinished: {
                if (!a) break;
                sendSpellGo(*a, e.spell, e.b, e.school);
                break;
            }
            case SimEvent::CastFailed: {
                if (!a) break;
                sendCastFailed(*a, e.spell, (uint8_t)e.value);
                break;
            }
            case SimEvent::AuraApplied: {
                if (!a) break;
                const int slot = e.flags;   // the slot assigned by World::addAura
                if (slot < 0 || slot >= kAuraSlots) break;
                FieldSet f;
                f.set((uint16_t)(UF::UNIT_FIELD_AURA + slot), e.spell);
                UpdateBuilder u;
                addValues(u, a->guid, f);
                sendUpdateObject(u.finish());
                if (a->self) {
                    Writer w;
                    w.u8((uint8_t)slot);
                    w.u32((uint32_t)std::max<int32_t>(0, e.value));
                    send(SMSG_UPDATE_AURA_DURATION, w.b);
                }
                break;
            }
            case SimEvent::AuraRemoved: {
                if (!a) break;
                const int slot = e.value;   // the slot the aura had before it was removed
                if (slot < 0 || slot >= kAuraSlots) break;
                FieldSet f;
                f.set((uint16_t)(UF::UNIT_FIELD_AURA + slot), 0);
                UpdateBuilder u;
                addValues(u, a->guid, f);
                sendUpdateObject(u.finish());
                break;
            }
            case SimEvent::LootReady:
                if (a) {
                    FieldSet f;
                    f.set(UF::UNIT_DYNAMIC_FLAGS, 1);
                    UpdateBuilder u;
                    addValues(u, a->guid, f);
                    sendUpdateObject(u.finish());
                }
                break;
            case SimEvent::ItemGained:
                inventoryDirty = true;
                if (data_) {
                    const ItemTemplate* it = data_->item((uint32_t)e.b);
                    if (it) sendChat(0x0A, 0, "You receive loot: " + it->name + " x" + std::to_string((int)e.value) + ".");
                }
                break;
            case SimEvent::Looted:
                sendSelfFields();
                break;
            case SimEvent::PowerChanged:
                if (a && a->self) sendSelfFields();
                break;
            case SimEvent::QuestProgress:
                if (a && a->self)
                    sendChat(0x0A, 0, "Quest progress: " + std::to_string((int)e.value) + ".");
                break;
            case SimEvent::QuestComplete:
                if (a && a->self) sendChat(0x0A, 0, "Quest complete.");
                break;
            case SimEvent::Message:
                if (e.text[0]) sendChat(0x0A, 0, std::string((const char*)e.text));
                break;
            default:
                break;
        }
    }
    world_.clearEvents();
    if (inventoryDirty) sendInventory();
    (void)nowMs;
}

void OfflineSession::sendMovementToClient(uint32_t nowMs) {
    if (nowMs - lastMoveSend_ < 400) return;
    lastMoveSend_ = nowMs;
    for (int i = 0; i < kMaxEntities; ++i) {
        Entity* e = world_.byIndex(i);
        if (!e || !e->used || e->self) continue;
        if (!e->dead && e->moving) {
            // Companions and creatures use the same server spline: the client interpolates it and blends
            // from where the model was drawn. Player dead-reckoning from a 500 ms heartbeat made the
            // companions snap around corners, so it is not used any more.
            const float durationMs = 450.0f;
            const float ahead = std::max(0.5f, e->speed * durationMs / 1000.0f);
            Writer w;
            w.packedGuid(e->guid);
            w.f32(e->x); w.f32(e->y); w.f32(e->z);
            w.u32(nowMs);
            w.u8(0);                 // normal move
            w.u32(0x100);            // run
            w.u32((uint32_t)durationMs);
            w.u32(1);
            w.f32(e->x + std::cos(e->o) * ahead);
            w.f32(e->y + std::sin(e->o) * ahead);
            w.f32(e->z);
            send(0x0DD, w.b);
            e->netMoveState = 1;
            e->netO = e->o;
        } else if (e->netMoveState == 1) {
            // One packet when it stops: no more spline resets while a unit stands still.
            Writer w;
            w.packedGuid(e->guid);
            w.f32(e->x); w.f32(e->y); w.f32(e->z);
            w.u32(nowMs);
            w.u8(1);                 // stop: the client's parser stops here
            send(0x0DD, w.b);
            e->netMoveState = 2;
        } else if (e->objType == 4) {
            // A standing companion still turns to its target (the tank keeps facing the fight).
            float diff = e->o - e->netO;
            while (diff > 3.14159265f) diff -= 6.2831853f;
            while (diff < -3.14159265f) diff += 6.2831853f;
            if (std::fabs(diff) > 0.40f) {
                Writer w;
                w.packedGuid(e->guid);
                w.u32(0);            // movement flags: standing
                w.u32(nowMs);
                w.f32(e->x); w.f32(e->y); w.f32(e->z); w.f32(e->o);
                w.u32(0);
                send(MoveOp::SetFacing, w.b);
                e->netO = e->o;
            }
        }
    }
}

// ---------------------------------------------------------------- update
void OfflineSession::update(uint32_t nowMs) {
    if (!inWorld_) return;
    ++stats.updates;
    std::vector<std::pair<uint16_t, Bytes>> in;
    in.swap(incoming_);
    for (auto& p : in) {
        const uint16_t op = p.first;
        switch (op) {
            case MoveOp::StartForward: case MoveOp::StartBackward: case MoveOp::Stop:
            case MoveOp::StartStrafeLeft: case MoveOp::StartStrafeRight: case MoveOp::StopStrafe:
            case MoveOp::Jump: case MoveOp::FallLand: case MoveOp::SetFacing:
            case MoveOp::Heartbeat: case MoveOp::SetRunMode: case MoveOp::SetWalkMode:
            case MoveOp::StartSwim: case MoveOp::StopSwim: case MoveOp::SetPitch:
                handleMovement(p.second, nowMs);
                break;
            case CMSG_SET_ACTIVE_MOVER: break;
            case CMSG_ATTACKSWING: handleAttack(p.second, nowMs); break;
            case CMSG_ATTACKSTOP:
                if (Entity* pl = world_.player()) world_.stopAttack(*pl, nowMs);
                break;
            case CMSG_CAST_SPELL: handleCast(p.second, nowMs); break;
            case CMSG_CANCEL_CAST:
                if (Entity* pl = world_.player()) world_.stopCast(*pl, 35, nowMs);
                break;
            case CMSG_LOOT: case CMSG_LOOT_MONEY: case CMSG_AUTOSTORE_LOOT_ITEM: case CMSG_LOOT_RELEASE:
                handleLoot(op, p.second, nowMs);
                break;
            case CMSG_MESSAGECHAT: handleChat(p.second, nowMs); break;
            case CMSG_GOSSIP_HELLO: case CMSG_LIST_INVENTORY: case CMSG_BUY_ITEM: case CMSG_SELL_ITEM:
                handleVendor(op, p.second, nowMs);
                break;
            case CMSG_AUTOEQUIP_ITEM: {   // WoWVita: u8 bag (255 = the character), u8 slot (23..38 = the backpack)
                Reader r(p.second);
                const uint8_t bag = r.u8(), slot = r.u8();
                if (r.bad || bag != 255 || slot < 23 || slot > 38) break;
                const int index = slot - 23;
                const uint8_t before = world_.playerProfile().bagCount;
                int changed[2];
                const int why = world_.equipFromBag(index, changed);
                if (why) {
                    errorLine(why == 2 ? "You must reach a higher level to use that item."
                              : why == 3 ? "Inventory is full." : "You can't equip that.", nowMs);
                    break;
                }
                // the objects of the touched slots go and come back with their new contents
                auto destroy = [&](uint64_t g) { Writer w; w.u64(g); send(0x0AA, w.b); };
                for (int i = index; i < std::max<int>(before, world_.playerProfile().bagCount); ++i) destroy(itemGuidFor(false, i));
                for (int c : changed) if (c >= 0) destroy(itemGuidFor(true, c));
                sendInventory();
                break;
            }
            case CMSG_REPOP_REQUEST: case CMSG_RECLAIM_CORPSE: handleDeath(op, p.second, nowMs); break;
            case CMSG_NAME_QUERY: case CMSG_CREATURE_QUERY: case CMSG_GAMEOBJECT_QUERY:
            case CMSG_ITEM_QUERY_SINGLE: case CMSG_REQUEST_PARTY_MEMBER_STATS: case CMSG_SET_SELECTION:
                handleQuery(op, p.second, nowMs);
                break;
            case MSG_CORPSE_QUERY: {
                Entity* pl = world_.player();
                Writer w;
                w.u8(1);
                w.u32(mapId_);
                w.f32(pl ? pl->homeX : 0); w.f32(pl ? pl->homeY : 0); w.f32(pl ? pl->homeZ : 0);
                w.u32(mapId_);
                send(MSG_CORPSE_QUERY, w.b);
                break;
            }
            case CMSG_LOGOUT_REQUEST:
                send(0x04D);   // SMSG_LOGOUT_COMPLETE
                break;
            default:
                break;
        }
    }

    world_.setData(data_);
    world_.step(nowMs);    ++stats.steps;
    dispatchEvents(nowMs);
    sendMovementToClient(nowMs);
    if (nowMs - lastPartyStats_ >= 1000) {
        lastPartyStats_ = nowMs;
        uint32_t h = 2166136261u;
        for (int i = 0; i < kMaxParty; ++i) {
            const Entity* b = world_.bot(i);
            if (!b) continue;
            h = (h ^ (uint32_t)b->hp) * 16777619u;
            h = (h ^ (uint32_t)b->power) * 16777619u;
            h = (h ^ (uint32_t)b->alive()) * 16777619u;
        }
        // Send on any change, and at least every 3 s so the party frame cannot go stale.
        if (h != partyHash_ || nowMs - lastPartyFull_ >= 3000) {
            partyHash_ = h;
            lastPartyFull_ = nowMs;
            sendPartyStats();
        }
    }
    if (nowMs - lastSave_ >= 60000) {
        lastSave_ = nowMs;
        std::string serr;
        if (!save(&serr)) {
            if (!saveWarned_) {
                saveWarned_ = true;
                sendChat(0x0A, 0, "Offline save failed (" + serr + "). The SD card may be full or removed.");
            }
        } else {
            saveWarned_ = false;
        }
    }
    flush();
}

}  // namespace offline
}  // namespace w3n
