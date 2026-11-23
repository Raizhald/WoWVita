// offline_data.cpp — .w3do pack loader, referential-integrity validation and the built-in sample world.
#include "offline_data.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

#include "../inflate.hpp"

namespace w3n {
namespace offline {

namespace {

constexpr uint32_t kMaxCreatures = 16384;   // WoWVita: whole-continent packs
constexpr uint32_t kMaxSpawns = 131072;
constexpr uint32_t kMaxLootTables = 32768;
constexpr uint32_t kMaxItems = 32768;
constexpr uint32_t kMaxSpells = 2048;
constexpr uint32_t kMaxQuests = 2048;
constexpr uint32_t kMaxString = 512;
constexpr uint16_t kMaxSections = 64;

// Bounds-checked little-endian reader. Any overrun latches 'bad' and returns zeros.
struct Bin {
    const uint8_t* p = nullptr;
    size_t n = 0, pos = 0;
    bool bad = false;
    Bin() = default;
    Bin(const uint8_t* d, size_t s) : p(d), n(s) {}
    bool need(size_t k) {
        if (bad || pos + k > n) { bad = true; pos = n; return false; }
        return true;
    }
    uint8_t u8() { return need(1) ? p[pos++] : 0; }
    uint16_t u16() { uint16_t a = u8(); return (uint16_t)(a | (u8() << 8)); }
    uint32_t u32() { uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
    int32_t i32() { return (int32_t)u32(); }
    float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    std::string str() {
        uint16_t len = u16();
        if (bad || len > kMaxString || pos + len > n) { bad = true; return std::string(); }
        std::string s((const char*)p + pos, len);
        pos += len;
        return s;
    }
    const uint8_t* take(size_t k) {
        if (!need(k)) return nullptr;
        const uint8_t* q = p + pos;
        pos += k;
        return q;
    }
};

struct Section { uint32_t tag = 0, offset = 0, size = 0, codec = 0; };

constexpr uint32_t fourcc(const char* s) { return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) | ((uint32_t)(uint8_t)s[2] << 16) | ((uint32_t)(uint8_t)s[3] << 24); }

bool sectionBytes(const uint8_t* file, size_t size, const Section& s, Bytes& out) {
    if (s.offset > size || s.size > size - s.offset) return false;
    if (s.codec == 0) {
        out.assign(file + s.offset, file + s.offset + s.size);
        return true;
    }
    if (s.codec == 1) return zlibInflate(file + s.offset, s.size, out, 0, kWorldDataMaxBytes);
    return false;
}

bool readInfo(Bin& b, WorldData& w) {
    w.mapId = b.u32();
    w.zoneId = b.u32();
    w.spawnX = b.f32(); w.spawnY = b.f32(); w.spawnZ = b.f32(); w.spawnO = b.f32();
    w.name = b.str();
    return !b.bad;
}

bool readCreatures(Bin& b, WorldData& w) {
    uint32_t n = b.u32();
    if (b.bad || n > kMaxCreatures) return false;
    w.creatures.clear();
    w.creatures.reserve(n);
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        CreatureTemplate c;
        c.entry = b.u32();
        c.name = b.str();
        c.subname = b.str();
        c.displayId = b.u32();
        c.minLevel = b.u16();
        c.maxLevel = b.u16();
        c.faction = b.u32();
        c.hp = b.u32();
        c.armor = b.u32();
        c.xp = b.u32();
        c.dmgMin = b.u16();
        c.dmgMax = b.u16();
        c.attackMs = b.u32();
        c.powerType = b.u8();
        c.power = b.u32();
        c.rank = b.u8();
        c.creatureType = b.u8();
        c.npcFlags = b.u32();
        c.scale = b.f32();
        c.speed = b.f32();
        c.aggroRange = b.f32();
        c.combatReach = b.f32();
        c.boundingRadius = b.f32();
        c.lootId = b.u32();
        c.abilityCount = b.u8();
        if (c.abilityCount > c.abilities.size()) return false;
        for (uint8_t k = 0; k < c.abilityCount; ++k) c.abilities[k] = b.u32();
        if (!b.bad) w.creatures.push_back(std::move(c));
    }
    return !b.bad;
}

bool readSpawns(Bin& b, WorldData& w) {
    uint32_t n = b.u32();
    if (b.bad || n > kMaxSpawns) return false;
    w.spawns.clear();
    w.spawns.reserve(n);
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        Spawn s;
        s.entry = b.u32();
        s.x = b.f32(); s.y = b.f32(); s.z = b.f32(); s.o = b.f32();
        s.respawnMs = b.u32();
        s.elite = b.u8();
        if (!b.bad) w.spawns.push_back(s);
    }
    return !b.bad;
}

bool readLoot(Bin& b, WorldData& w) {
    uint32_t n = b.u32();
    if (b.bad || n > kMaxLootTables) return false;
    w.loot.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        LootTable t;
        t.id = b.u32();
        t.moneyMin = b.u32();
        t.moneyMax = b.u32();
        uint16_t m = b.u16();
        if (m > 64) return false;
        for (uint16_t k = 0; k < m && !b.bad; ++k) {
            LootItem it;
            it.item = b.u32();
            it.chance = b.u16();
            it.minCount = b.u16();
            it.maxCount = b.u16();
            t.items.push_back(it);
        }
        if (!b.bad) w.loot.push_back(std::move(t));
    }
    return !b.bad;
}

bool readItems(Bin& b, WorldData& w) {
    uint32_t n = b.u32();
    if (b.bad || n > kMaxItems) return false;
    w.items.clear();
    w.items.reserve(n);
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        ItemTemplate t;
        t.entry = b.u32();
        t.name = b.str();
        t.description = b.str();
        t.displayId = b.u32();
        t.quality = b.u32();
        t.inventoryType = b.u32();
        t.itemLevel = b.u32();
        t.requiredLevel = b.u32();
        t.maxStack = b.u32();
        t.buyPrice = b.u32();
        t.sellPrice = b.u32();
        t.armor = b.u32();
        t.delayMs = b.u32();
        t.dmgMin = b.f32();
        t.dmgMax = b.f32();
        t.cls = b.u32();
        t.subclass = b.u32();
        if (!b.bad) w.items.push_back(std::move(t));
    }
    return !b.bad;
}

bool readSpells(Bin& b, WorldData& w) {
    uint32_t n = b.u32();
    if (b.bad || n > kMaxSpells) return false;
    w.spells.clear();
    w.spells.reserve(n);
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        SpellDef s;
        s.id = b.u32();
        s.name = b.str();
        s.kind = (SpellKind)b.u8();
        s.school = b.u8();
        s.castMs = b.u32();
        s.cooldownMs = b.u32();
        s.category = b.u32();
        s.categoryCooldownMs = b.u32();
        s.minRange = b.f32();
        s.maxRange = b.f32();
        s.powerType = b.u8();
        s.powerCost = b.u32();
        s.amountMin = b.i32();
        s.amountMax = b.i32();
        s.durationMs = b.u32();
        s.tickMs = b.u32();
        s.threatBonus = b.i32();
        s.radius = b.f32();
        s.friendly = b.u8() != 0;
        s.startsAutoAttack = b.u8() != 0;
        if (!b.bad) w.spells.push_back(std::move(s));
    }
    return !b.bad;
}

bool readQuests(Bin& b, WorldData& w) {
    uint32_t n = b.u32();
    if (b.bad || n > kMaxQuests) return false;
    w.quests.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        QuestDef q;
        q.id = b.u32();
        q.title = b.str();
        q.details = b.str();
        q.completion = b.str();
        q.level = b.u16();
        q.giverCreature = b.u32();
        q.objective.kind = b.u8();
        q.objective.target = b.u32();
        q.objective.count = b.u16();
        q.objective.text = b.str();
        q.rewardXp = b.u32();
        q.rewardMoney = b.u32();
        for (int k = 0; k < 3; ++k) q.rewardItems[k] = b.u32();
        for (int k = 0; k < 3; ++k) q.rewardCounts[k] = b.u16();
        if (!b.bad) w.quests.push_back(std::move(q));
    }
    return !b.bad;
}

bool readBots(Bin& b, WorldData& w) {
    uint8_t n = b.u8();
    if (b.bad || n == 0 || n > kMaxParty) return false;
    for (int i = 0; i < kMaxParty; ++i) w.bots[(size_t)i] = BotTemplate();
    for (uint8_t i = 0; i < n && !b.bad; ++i) {
        BotTemplate t;
        t.name = b.str();
        t.race = b.u8(); t.cls = b.u8(); t.gender = b.u8();
        t.skin = b.u8(); t.face = b.u8(); t.hairStyle = b.u8(); t.hairColor = b.u8(); t.facialHair = b.u8();
        t.role = (Role)b.u8();
        t.level = b.u16();
        t.hp = b.u32();
        t.power = b.u32();
        t.powerType = b.u8();
        t.displayId = b.u32();
        t.spellCount = b.u8();
        if (t.spellCount > t.spells.size()) return false;
        for (uint8_t k = 0; k < t.spellCount; ++k) t.spells[k] = b.u32();
        for (int k = 0; k < 19; ++k) t.equipment[(size_t)k] = b.u32();
        t.followDistance = b.f32();
        if (!b.bad) w.bots[(size_t)i] = std::move(t);
    }
    return !b.bad;
}

bool readVendors(Bin& b, WorldData& w) {   // WoWVita
    uint32_t n = b.u32();
    if (b.bad || n > 16384) return false;
    w.vendors.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t entry = b.u32();
        const uint16_t count = b.u16();
        if (b.bad || count > 255) return false;
        std::vector<VendorGoods> goods;
        for (uint16_t k = 0; k < count && !b.bad; ++k) {
            VendorGoods v;
            v.item = b.u32();
            v.buyCount = b.u16();
            if (!v.buyCount) v.buyCount = 1;
            goods.push_back(v);
        }
        if (!b.bad) w.vendors[entry] = std::move(goods);
    }
    return !b.bad;
}

std::string lstr(Bin& b) {   // WoWVita: u16-length text up to 4000 bytes (quest texts)
    const uint16_t len = b.u16();
    const uint8_t* p = (!b.bad && len <= 4000) ? b.take(len) : nullptr;
    if (!p) { b.bad = true; return std::string(); }
    return std::string((const char*)p, len);
}

bool readQuests2(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 16384) return false;
    w.questsFull.clear();
    w.questsFull.reserve(n);
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        QuestFull q;
        q.id = b.u32(); q.level = b.u16(); q.minLevel = b.u16(); q.races = b.u32(); q.classes = b.u32();
        q.zoneOrSort = b.u32(); q.type = b.u32(); q.flags = b.u32(); q.special = b.u32();
        q.prev = (int32_t)b.u32(); q.nextInChain = b.u32(); q.srcItem = b.u32(); q.srcCount = b.u8();
        q.money = (int32_t)b.u32(); q.moneyMaxLevel = b.u32();
        q.title = lstr(b); q.details = lstr(b); q.objectives = lstr(b); q.offerReward = lstr(b);
        q.requestItems = lstr(b); q.endText = lstr(b);
        for (std::string& t : q.objText) t = lstr(b);
        for (int k = 0; k < 4; ++k) { q.killEntry[k] = b.u32(); q.killCount[k] = b.u16(); }
        for (int k = 0; k < 4; ++k) { q.itemEntry[k] = b.u32(); q.itemCount[k] = b.u16(); }
        for (int k = 0; k < 6; ++k) { q.choiceItem[k] = b.u32(); q.choiceCount[k] = b.u16(); }
        for (int k = 0; k < 4; ++k) { q.rewItem[k] = b.u32(); q.rewCount[k] = b.u16(); }
        uint16_t ns = b.u16();
        if (ns > 64) return false;
        for (uint16_t k = 0; k < ns && !b.bad; ++k) q.starters.push_back(b.u32());
        uint16_t ne = b.u16();
        if (ne > 64) return false;
        for (uint16_t k = 0; k < ne && !b.bad; ++k) q.enders.push_back(b.u32());
        uint16_t gs = b.u16();
        if (gs > 64) return false;
        for (uint16_t k = 0; k < gs && !b.bad; ++k) q.goStarters.push_back(b.u32());
        uint16_t ge = b.u16();
        if (ge > 64) return false;
        for (uint16_t k = 0; k < ge && !b.bad; ++k) q.goEnders.push_back(b.u32());
        if (!b.bad) w.questsFull.push_back(std::move(q));
    }
    w.questIndex.clear(); w.questsStarted.clear(); w.questsEnded.clear();
    w.goQuestsStarted.clear(); w.goQuestsEnded.clear();
    for (uint32_t i = 0; i < (uint32_t)w.questsFull.size(); ++i) {
        const QuestFull& q = w.questsFull[i];
        w.questIndex[q.id] = i;
        for (uint32_t c : q.starters) w.questsStarted[c].push_back(q.id);
        for (uint32_t c : q.enders) w.questsEnded[c].push_back(q.id);
        for (uint32_t c : q.goStarters) w.goQuestsStarted[c].push_back(q.id);
        for (uint32_t c : q.goEnders) w.goQuestsEnded[c].push_back(q.id);
    }
    return !b.bad;
}

bool readQuestLoot(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.questLoot.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t lid = b.u32();
        const uint16_t k = b.u16();
        if (k > 64) return false;
        std::vector<QuestDrop> v;
        for (uint16_t j = 0; j < k && !b.bad; ++j) { QuestDrop d; d.item = b.u32(); d.chance = b.u16(); v.push_back(d); }
        w.questLoot[lid] = std::move(v);
    }
    return !b.bad;
}

bool readGoTemplates(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.goTemplates.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        GoTemplate t;
        t.entry = b.u32(); t.type = b.u32(); t.displayId = b.u32(); t.name = b.str();
        t.faction = b.u32(); t.flags = b.u32(); t.size = b.f32();
        for (uint32_t& d : t.data) d = b.u32();
        if (!b.bad) w.goTemplates[t.entry] = std::move(t);
    }
    return !b.bad;
}

bool readGoSpawns(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 262144) return false;
    w.goSpawns.clear();
    w.goSpawns.reserve(n);
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        GoSpawn s;
        s.entry = b.u32(); s.x = b.f32(); s.y = b.f32(); s.z = b.f32(); s.o = b.f32();
        for (float& r : s.rot) r = b.f32();
        s.respawnMs = b.u32();
        if (!b.bad) w.goSpawns.push_back(s);
    }
    return !b.bad;
}

bool readGraveyards(Bin& b, WorldData& w) {   // WoWVita
    uint32_t n = b.u32();
    if (b.bad || n > 4096) return false;
    w.graveyards.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        Graveyard g;
        g.id = b.u32(); g.x = b.f32(); g.y = b.f32(); g.z = b.f32(); g.o = b.f32();
        if (!b.bad) w.graveyards.push_back(g);
    }
    n = b.u32();
    if (b.bad || n > 65536) return false;
    w.graveyardLinks.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        GraveyardLink l;
        l.id = b.u32(); l.zone = b.u32(); l.faction = b.u16();
        if (!b.bad) w.graveyardLinks.push_back(l);
    }
    return !b.bad;
}

bool readItemExt(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 262144) return false;
    w.itemExt.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t entry = b.u32();
        ItemExt x;
        x.slots = b.u8(); x.maxDurability = b.u16(); x.kind = b.u8(); x.spell = b.u32();
        x.hpMin = b.u32(); x.hpMax = b.u32(); x.manaMin = b.u32(); x.manaMax = b.u32();
        x.durationMs = b.u32(); x.cooldownMs = b.u32(); x.category = b.u16(); x.flags = b.u8();
        if (!b.bad) w.itemExt[entry] = x;
    }
    return !b.bad;
}

bool readExploration(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 256) return false;
    w.explorationXp.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint8_t lv = b.u8();
        const uint32_t xp = b.u32();
        if (w.explorationXp.size() <= lv) w.explorationXp.resize((size_t)lv + 1, 0);
        w.explorationXp[lv] = xp;
    }
    return !b.bad;
}

bool readQuestTriggers(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.questTriggers.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t t = b.u32(), q = b.u32();
        if (!b.bad) w.questTriggers.emplace(t, q);
    }
    return !b.bad;
}

bool readTaverns(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 4096) return false;
    w.taverns.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) w.taverns.push_back(b.u32());
    return !b.bad;
}

bool readMovement(Bin& b, WorldData& w) {   // WoWVita "MOVE"
    const uint32_t np = b.u32();
    if (b.bad || np > 65536) return false;
    w.paths.clear();
    w.paths.reserve(np);
    for (uint32_t i = 0; i < np && !b.bad; ++i) {
        const uint16_t n = b.u16();
        if (n > 1000) return false;
        std::vector<MovePoint> pts(n);
        for (MovePoint& m : pts) { m.x = b.f32(); m.y = b.f32(); m.z = b.f32(); m.o = b.f32(); m.waitMs = b.u32(); }
        w.paths.push_back(std::move(pts));
    }
    const uint32_t nm = b.u32();
    if (b.bad || nm > 1000000) return false;
    w.spawnMoves.clear();
    for (uint32_t i = 0; i < nm && !b.bad; ++i) {
        const uint32_t idx = b.u32();
        SpawnMove m;
        m.type = b.u8(); m.wander = b.f32(); m.path = b.u32();
        if (m.type == 2 && m.path >= w.paths.size()) continue;
        w.spawnMoves[idx] = m;
    }
    return !b.bad;
}

bool readCreatureAi(Bin& b, WorldData& w) {   // WoWVita "CRAI"
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.creatureAi.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t entry = b.u32();
        CreatureAi a;
        a.walkSpeed = b.f32(); a.callForHelp = b.f32();
        const uint8_t flags = b.u8();
        a.civilian = (flags & 1) != 0;
        a.leash = b.f32(); a.pursuitMs = b.u32();
        const uint8_t ne = b.u8();
        for (uint8_t k = 0; k < ne && !b.bad; ++k) {
            AiEvent e;
            e.type = b.u8(); e.phaseMask = b.u32(); e.chance = b.u8(); e.flags = b.u16();
            for (int32_t& v : e.p) v = b.i32();
            for (AiAction& ac : e.act) { ac.type = b.u8(); for (int32_t& v : ac.p) v = b.i32(); }
            a.events.push_back(e);
        }
        if (!(a.walkSpeed > 0.1f && a.walkSpeed < 50.0f)) a.walkSpeed = 2.5f;
        w.creatureAi[entry] = std::move(a);
    }
    return !b.bad;
}

bool readSpellControl(Bin& b, WorldData& w) {   // WoWVita "SPFX"
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.spellControl.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t id = b.u32();
        SpellControl c;
        c.durationMs = b.u32();
        const uint8_t k = b.u8();
        for (uint8_t j = 0; j < k && !b.bad; ++j) {
            const uint8_t t = b.u8();
            const int32_t v = b.i32();
            c.effects.emplace_back(t, v);
        }
        if (!b.bad && c.durationMs) w.spellControl[id] = std::move(c);
    }
    return !b.bad;
}

bool readOutfits(Bin& b, WorldData& w) {   // WoWVita "OUTF"
    const uint32_t n = b.u32();
    if (b.bad || n > 4096) return false;
    w.outfits.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t race = b.u8(), cls = b.u8(), sex = b.u8();
        const uint8_t k = b.u8();
        std::vector<uint32_t> items;
        for (uint8_t j = 0; j < k && !b.bad; ++j) items.push_back(b.u32());
        w.outfits[race | (cls << 8) | (sex << 16)] = std::move(items);
    }
    return !b.bad;
}

bool readPools(Bin& b, WorldData& w) {   // WoWVita "POOL"
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.pools.clear();
    w.poolOf.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        SpawnPool p;
        p.limit = b.u16();
        const uint16_t k = b.u16();
        for (uint16_t j = 0; j < k && !b.bad; ++j) p.members.push_back(b.u32());
        for (uint32_t m : p.members) w.poolOf[m] = (uint32_t)w.pools.size();
        w.pools.push_back(std::move(p));
    }
    return !b.bad;
}

bool readLinks(Bin& b, WorldData& w) {   // WoWVita "LINK"
    const uint32_t n = b.u32();
    if (b.bad || n > 1000000) return false;
    w.links.clear();
    w.slavesOf.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t slave = b.u32();
        SpawnLink l;
        l.master = b.u32(); l.flags = b.u16();
        w.links[slave] = l;
        w.slavesOf.emplace(l.master, slave);
    }
    return !b.bad;
}

bool readAiTexts(Bin& b, WorldData& w) {   // WoWVita "AITX"
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.aiTexts.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const int32_t id = b.i32();
        AiText t;
        t.chatType = b.u8(); t.emote = b.u32(); t.text = b.str();
        if (!b.bad) w.aiTexts[id] = std::move(t);
    }
    return !b.bad;
}

bool readGoLoot(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.goLoot.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t id = b.u32();
        GoLoot l;
        l.moneyMin = b.u32(); l.moneyMax = b.u32();
        const uint16_t k = b.u16();
        if (k > 64) return false;
        for (uint16_t j = 0; j < k && !b.bad; ++j) {
            GoLootItem it;
            it.item = b.u32(); it.chance = b.u16(); it.minCount = b.u16(); it.maxCount = b.u16(); it.questOnly = b.u8() != 0;
            l.items.push_back(it);
        }
        if (!b.bad) w.goLoot[id] = std::move(l);
    }
    return !b.bad;
}

bool readGreetings(Bin& b, WorldData& w) {   // WoWVita
    const uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.greetings.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        const uint32_t e = b.u32();
        std::string t = lstr(b);
        if (!b.bad) w.greetings[e] = std::move(t);
    }
    return !b.bad;
}

bool readHostility(Bin& b, WorldData& w) {
    uint32_t n = b.u32();
    if (b.bad || n > 65536) return false;
    w.hostility.clear();
    for (uint32_t i = 0; i < n && !b.bad; ++i) {
        HostilePair p;
        p.a = b.u32();
        p.b = b.u32();
        if (!b.bad) w.hostility.push_back(p);
    }
    return !b.bad;
}

}  // namespace

// ---------------------------------------------------------------- lookups
template <typename V, typename F>
static void reindex(std::unordered_map<uint32_t, uint32_t>& idx, const V& v, F key) {
    if (idx.size() == v.size()) return;
    idx.clear();
    idx.reserve(v.size());
    for (uint32_t i = 0; i < (uint32_t)v.size(); ++i) idx.emplace(key(v[i]), i);   // (the first one wins, as before)
}
const CreatureTemplate* WorldData::creature(uint32_t entry) const {
    reindex(creatureIdx_, creatures, [](const CreatureTemplate& c) { return c.entry; });
    auto it = creatureIdx_.find(entry);
    return it == creatureIdx_.end() ? nullptr : &creatures[it->second];
}
const LootTable* WorldData::lootTable(uint32_t id) const {
    reindex(lootIdx_, loot, [](const LootTable& t) { return t.id; });
    auto it = lootIdx_.find(id);
    return it == lootIdx_.end() ? nullptr : &loot[it->second];
}
const ItemTemplate* WorldData::item(uint32_t entry) const {
    reindex(itemIdx_, items, [](const ItemTemplate& i) { return i.entry; });
    auto it = itemIdx_.find(entry);
    return it == itemIdx_.end() ? nullptr : &items[it->second];
}
const SpellDef* WorldData::spell(uint32_t id) const {
    for (const SpellDef& s : spells)
        if (s.id == id) return &s;
    if (id == kHearthstoneSpell) {   // WoWVita: every pack has the Hearthstone (the session teleports when it finishes)
        static const SpellDef hearth = [] {
            SpellDef d;
            d.id = kHearthstoneSpell; d.name = "Hearthstone"; d.kind = SpellKind::Script; d.school = 6;
            d.castMs = 10000; d.cooldownMs = 3600000; d.maxRange = 0.0f; d.friendly = true;
            return d;
        }();
        return &hearth;
    }
    return nullptr;
}
bool WorldData::hostile(uint32_t a, uint32_t b) const {
    if (!a || !b || a == b) return false;
    if (hostileIdxFor_ != hostility.size()) {
        hostileIdx_.clear();
        neutralIdx_.clear();
        for (const HostilePair& p : hostility) {
            const uint32_t a = p.a & ~kNeutralFlag;
            auto& set = (p.a & kNeutralFlag) ? neutralIdx_ : hostileIdx_;
            set.insert(((uint64_t)a << 32) | p.b);
            set.insert(((uint64_t)p.b << 32) | a);
        }
        hostileIdxFor_ = hostility.size();
    }
    return hostileIdx_.count(((uint64_t)a << 32) | b) != 0;
}
bool WorldData::attackable(uint32_t a, uint32_t b) const {
    if (!a || !b || a == b) return false;
    if (hostile(a, b)) return true;   // (also builds the indexes)
    return neutralIdx_.count(((uint64_t)a << 32) | b) != 0;
}

// ---------------------------------------------------------------- validation
std::vector<std::string> WorldData::validate() const {
    std::vector<std::string> r;
    auto err = [&](const std::string& s) { if (r.size() < 200) r.push_back(s); };
    if (name.empty()) err("INFO: the pack has no name");
    if (spawnX == 0 && spawnY == 0 && spawnZ == 0) err("INFO: the spawn point is (0,0,0)");

    std::unordered_map<uint32_t, int> creatureIds;
    for (const CreatureTemplate& c : creatures) {
        if (!c.entry) err("CREA: entry 0");
        if (!creatureIds.emplace(c.entry, 1).second) err("CREA: duplicate entry " + std::to_string(c.entry));
        if (c.name.empty()) err("CREA " + std::to_string(c.entry) + ": empty name");
        if (c.maxLevel < c.minLevel) err("CREA " + std::to_string(c.entry) + ": maxLevel < minLevel");
        if (c.dmgMax < c.dmgMin) err("CREA " + std::to_string(c.entry) + ": dmgMax < dmgMin");
        if (c.attackMs < 500) err("CREA " + std::to_string(c.entry) + ": attackMs < 500");
        if (c.lootId && !lootTable(c.lootId)) err("CREA " + std::to_string(c.entry) + ": unknown loot table " + std::to_string(c.lootId));
        for (uint8_t i = 0; i < c.abilityCount; ++i)
            if (!spell(c.abilities[i])) err("CREA " + std::to_string(c.entry) + ": unknown ability " + std::to_string(c.abilities[i]));
    }
    for (const Spawn& s : spawns)
        if (!creature(s.entry)) err("SPWN: unknown creature entry " + std::to_string(s.entry));

    for (const LootTable& t : loot)
        for (const LootItem& i : t.items) {
            if (!item(i.item)) err("LOOT " + std::to_string(t.id) + ": unknown item " + std::to_string(i.item));
            if (i.chance > 10000) err("LOOT " + std::to_string(t.id) + ": chance > 10000");
            if (i.maxCount < i.minCount || i.minCount == 0) err("LOOT " + std::to_string(t.id) + ": bad count range");
        }

    std::unordered_map<uint32_t, int> spellIds;
    for (const SpellDef& s : spells) {
        if (!spellIds.emplace(s.id, 1).second) err("SPEL: duplicate id " + std::to_string(s.id));
        if (s.name.empty()) err("SPEL " + std::to_string(s.id) + ": empty name");
        if (s.maxRange < s.minRange) err("SPEL " + std::to_string(s.id) + ": maxRange < minRange");
        if (s.amountMax < s.amountMin) err("SPEL " + std::to_string(s.id) + ": amountMax < amountMin");
        if (s.kind == SpellKind::Dot && (!s.durationMs || !s.tickMs)) err("SPEL " + std::to_string(s.id) + ": DoT without duration/tick");
        if ((s.kind == SpellKind::Dot || s.kind == SpellKind::Buff) && s.durationMs > 3600000u)
            err("SPEL " + std::to_string(s.id) + ": duration over one hour");
    }

    for (const QuestDef& q : quests) {
        if (q.giverCreature && !creature(q.giverCreature)) err("QUES " + std::to_string(q.id) + ": unknown giver " + std::to_string(q.giverCreature));
        if (q.objective.kind == 0 && !creature(q.objective.target))
            err("QUES " + std::to_string(q.id) + ": unknown kill target " + std::to_string(q.objective.target));
        if (q.objective.kind == 1 && !item(q.objective.target))
            err("QUES " + std::to_string(q.id) + ": unknown item target " + std::to_string(q.objective.target));
    }

    int botCount = 0;
    for (const BotTemplate& b : bots) {
        if (b.name.empty()) continue;
        ++botCount;
        if (b.race < 1 || b.race > 10) err("BOTS: bad race for " + b.name);
        if (b.cls < 1 || b.cls > 11) err("BOTS: bad class for " + b.name);
        if (b.hp == 0) err("BOTS: zero health for " + b.name);
        for (uint8_t i = 0; i < b.spellCount; ++i)
            if (!spell(b.spells[i])) err("BOTS " + b.name + ": unknown spell " + std::to_string(b.spells[i]));
        for (uint32_t e : b.equipment)
            if (e && !item(e)) err("BOTS " + b.name + ": unknown equipment item " + std::to_string(e));
    }
    if (botCount != kMaxParty) err("BOTS: expected exactly four templates, found " + std::to_string(botCount));

    for (const HostilePair& p : hostility) {
        if (!p.a || !p.b) err("HOST: faction 0");
        if ((p.a & ~kNeutralFlag) == p.b) err("HOST: faction " + std::to_string(p.b) + " hostile to itself");
    }
    return r;
}

// ---------------------------------------------------------------- loader
bool loadWorldDataBuffer(const uint8_t* data, size_t size, WorldData& out, std::string& err) {
    out = WorldData();
    if (!data || size < 12) { err = "offline pack is empty or truncated"; return false; }
    Bin h(data, size);
    const uint32_t magic = h.u32();
    if (magic != fourcc("W3DO")) { err = "not an offline world pack (bad magic)"; return false; }
    const uint16_t version = h.u16();
    if (version != kWorldDataVersion) {
        err = "unsupported offline pack version " + std::to_string(version) + " (this build reads version 1)";
        return false;
    }
    h.u16();   // flags, reserved
    const uint32_t count = h.u32();
    if (h.bad || count == 0 || count > kMaxSections) { err = "bad section count"; return false; }
    Section sections[kMaxSections];
    for (uint32_t i = 0; i < count; ++i) {
        sections[i].tag = h.u32();
        sections[i].offset = h.u32();
        sections[i].size = h.u32();
        sections[i].codec = h.u32();
        if (h.bad) { err = "truncated section table"; return false; }
        if (sections[i].offset < h.pos || sections[i].offset > size || sections[i].size > size) {
            err = "section points outside the file";
            return false;
        }
    }
    bool haveInfo = false, haveCreatures = false, haveSpawns = false, haveBots = false;
    for (uint32_t i = 0; i < count; ++i) {
        Bytes body;
        if (!sectionBytes(data, size, sections[i], body)) { err = "cannot unpack a section (corrupt or unsupported codec)"; return false; }
        Bin b(body.data(), body.size());
        bool ok = true;
        switch (sections[i].tag) {
            case fourcc("INFO"): ok = readInfo(b, out); haveInfo = true; break;
            case fourcc("CREA"): ok = readCreatures(b, out); haveCreatures = true; break;
            case fourcc("SPWN"): ok = readSpawns(b, out); haveSpawns = true; break;
            case fourcc("LOOT"): ok = readLoot(b, out); break;
            case fourcc("ITEM"): ok = readItems(b, out); break;
            case fourcc("SPEL"): ok = readSpells(b, out); break;
            case fourcc("QUES"): ok = readQuests(b, out); break;
            case fourcc("BOTS"): ok = readBots(b, out); haveBots = true; break;
            case fourcc("HOST"): ok = readHostility(b, out); break;
            case fourcc("VEND"): ok = readVendors(b, out); break;
            case fourcc("QST2"): ok = readQuests2(b, out); break;
            case fourcc("QLOT"): ok = readQuestLoot(b, out); break;
            case fourcc("GOSS"): ok = readGreetings(b, out); break;
            case fourcc("GOBJ"): ok = readGoTemplates(b, out); break;
            case fourcc("GOSP"): ok = readGoSpawns(b, out); break;
            case fourcc("GRAV"): ok = readGraveyards(b, out); break;
            case fourcc("TAVN"): ok = readTaverns(b, out); break;
            case fourcc("ITX1"): ok = readItemExt(b, out); break;
            case fourcc("EXPL"): ok = readExploration(b, out); break;
            case fourcc("QTRG"): ok = readQuestTriggers(b, out); break;
            case fourcc("GLOT"): ok = readGoLoot(b, out); break;
            case fourcc("MOVE"): ok = readMovement(b, out); break;
            case fourcc("CRAI"): ok = readCreatureAi(b, out); break;
            case fourcc("AITX"): ok = readAiTexts(b, out); break;
            case fourcc("SPFX"): ok = readSpellControl(b, out); break;
            case fourcc("POOL"): ok = readPools(b, out); break;
            case fourcc("OUTF"): ok = readOutfits(b, out); break;
            case fourcc("LINK"): ok = readLinks(b, out); break;
            default: break;   // forward compatibility: unknown sections are skipped
        }
        if (!ok) { err = "malformed section"; return false; }
    }
    if (!haveInfo || !haveCreatures || !haveSpawns || !haveBots) {
        err = "the pack is missing one of INFO / CREA / SPWN / BOTS";
        return false;
    }
    out.formatVersion = version;
    const std::vector<std::string> problems = out.validate();
    if (!problems.empty()) {
        err = "pack validation failed: " + problems.front();
        if (problems.size() > 1) err += " (+" + std::to_string(problems.size() - 1) + " more)";
        return false;
    }
    err.clear();
    return true;
}

bool loadWorldData(const char* path, WorldData& out, std::string& err) {
    FILE* f = fopen(path, "rb");
    if (!f) { err = std::string("cannot open ") + path; return false; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); err = "seek failed"; return false; }
    const long len = ftell(f);
    if (len <= 0 || (unsigned long)len > kWorldDataMaxBytes) { fclose(f); err = "offline pack has an impossible size"; return false; }
    Bytes buf((size_t)len);
    rewind(f);
    const size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) { err = "short read"; return false; }
    return loadWorldDataBuffer(buf.data(), buf.size(), out, err);
}

// ---------------------------------------------------------------- built-in sample
//
// Northshire Abbey. Numbers are hand-authored to be readable and to give one complete fight,
// not to be a faithful copy of any server database. The exporter replaces this with the real
// creature_template / creature / loot values from a Vanilla database.
void sampleSpell(WorldData& w, uint32_t id, const char* name, SpellKind kind, uint8_t school, uint32_t castMs,
                 uint32_t cd, float minR, float maxR, uint8_t pt, uint32_t cost, int32_t lo, int32_t hi,
                 uint32_t dur, uint32_t tick, bool friendly, bool autoAttack, int32_t threat = 0) {
    SpellDef s;
    s.id = id; s.name = name; s.kind = kind; s.school = school; s.castMs = castMs; s.cooldownMs = cd;
    s.minRange = minR; s.maxRange = maxR; s.powerType = pt; s.powerCost = cost;
    s.amountMin = lo; s.amountMax = hi; s.durationMs = dur; s.tickMs = tick;
    s.friendly = friendly; s.startsAutoAttack = autoAttack; s.threatBonus = threat;
    w.spells.push_back(std::move(s));
}

void sampleItem(WorldData& w, uint32_t entry, const char* name, uint32_t display, uint32_t quality, uint32_t invType,
                uint32_t maxStack, uint32_t sell, uint32_t delay, float lo, float hi) {
    ItemTemplate t;
    t.entry = entry; t.name = name; t.displayId = display; t.quality = quality; t.inventoryType = invType;
    t.maxStack = maxStack; t.sellPrice = sell; t.buyPrice = sell * 4; t.delayMs = delay;
    t.dmgMin = lo; t.dmgMax = hi; t.cls = 2; t.subclass = 7;   // 7 = one-handed sword
    w.items.push_back(std::move(t));
}

bool buildSampleWorldData(WorldData& w) {
    w = WorldData();
    w.name = "Northshire Abbey (offline sample)";
    w.mapId = 0;
    w.zoneId = 9;                                  // Northshire Valley
    w.spawnX = -8949.95f; w.spawnY = -132.49f; w.spawnZ = 83.53f; w.spawnO = 3.07f;

    const uint32_t factionMonster = 32, factionDefias = 14, factionStormwind = 12, factionHuman = 1;

    // ---- creatures
    struct C { uint32_t entry; const char* name; uint32_t display; uint16_t lvl; uint32_t hp, xp, dmgMin, dmgMax, atkMs; uint32_t loot; };
    const C cs[] = {
        {299, "Young Wolf", 903, 1, 40, 45, 2, 4, 2000, 1},
        {6, "Kobold Vermin", 41, 1, 55, 50, 2, 5, 1900, 2},
        {73, "Defias Thug", 377, 2, 60, 60, 3, 6, 2100, 3},
        {197, "Marshal McBride", 1297, 30, 500, 0, 8, 12, 2000, 0},
    };
    for (const C& c : cs) {
        CreatureTemplate t;
        t.entry = c.entry;
        t.name = c.name;
        t.displayId = c.display;
        t.minLevel = t.maxLevel = c.lvl;
        t.faction = c.entry == 197 ? factionStormwind : (c.entry == 73 ? factionDefias : factionMonster);
        t.hp = c.hp;
        t.xp = c.xp;
        t.dmgMin = (uint16_t)c.dmgMin;
        t.dmgMax = (uint16_t)c.dmgMax;
        t.attackMs = c.atkMs;
        t.lootId = c.loot;
        t.npcFlags = c.entry == 197 ? 3 : 0;   // gossip | quest giver
        t.power = c.entry == 6 ? 200u : 0u;    // the kobold can pay for its Fireball
        t.combatReach = 1.5f;
        t.creatureType = c.entry == 299 ? 1 : 7;   // beast / humanoid
        if (c.entry == 6) t.abilities[t.abilityCount++] = 133;   // Kobold Fireball: gives the rogue something to Kick
        w.creatures.push_back(std::move(t));
    }
    w.creatures[3].subname = "Northshire Guard";
    w.creatures[3].aggroRange = 0.0f;

    // ---- spawns around the abbey
    struct S { uint32_t entry; float dx, dy, dz, o; };
    const S ss[] = {
        {299, 9, 4, 0, 1.0f}, {299, 15, -3, 0, 2.0f}, {299, 21, 6, 0, 4.0f}, {299, 6, -9, 0, 0.5f},
        {6, -10, 8, 0, 3.0f}, {6, -18, 2, 0, 5.0f}, {6, 2, -14, 0, 2.5f},
        {73, 26, -8, 0, 3.5f}, {73, 31, -2, 0, 4.5f},
        {197, 3, -2, 0, 1.5f},
    };
    for (const S& s : ss) {
        Spawn sp;
        sp.entry = s.entry;
        sp.x = w.spawnX + s.dx; sp.y = w.spawnY + s.dy; sp.z = w.spawnZ + s.dz; sp.o = s.o;
        sp.respawnMs = 30000;
        w.spawns.push_back(sp);
    }

    // ---- items
    sampleItem(w, 25, "Worn Shortsword", 1542, 1, 21, 1, 1, 1900, 1.0f, 3.0f);
    sampleItem(w, 38, "Recruit's Shirt", 9900, 1, 4, 1, 1, 0, 0.0f, 0.0f);
    sampleItem(w, 2589, "Linen Cloth", 6803, 1, 0, 20, 13, 0, 0.0f, 0.0f);
    sampleItem(w, 750, "Tough Wolf Meat", 6362, 1, 0, 10, 4, 0, 0.0f, 0.0f);

    // ---- loot
    auto loot = [&](uint32_t id, uint32_t lo, uint32_t hi, std::initializer_list<LootItem> items) {
        LootTable t;
        t.id = id; t.moneyMin = lo; t.moneyMax = hi;
        for (const LootItem& i : items) t.items.push_back(i);
        w.loot.push_back(std::move(t));
    };
    loot(1, 5, 23, {{750, 6000, 1, 2}, {2589, 3000, 1, 2}});
    loot(2, 4, 18, {{2589, 4000, 1, 1}});
    loot(3, 10, 35, {{2589, 5000, 1, 2}, {25, 1500, 1, 1}});

    // ---- spells (a deliberately small, documented subset)
    sampleSpell(w, 6603, "Attack", SpellKind::Melee, 0, 0, 0, 0, 5, 0, 0, 1, 2, 0, 0, false, true);
    sampleSpell(w, 78, "Heroic Strike", SpellKind::Melee, 0, 0, 0, 0, 5, 1, 15, 8, 12, 0, 0, false, true, 20);
    sampleSpell(w, 6673, "Battle Shout", SpellKind::Buff, 0, 0, 0, 0, 0, 1, 10, 0, 0, 120000, 0, false, false);
    sampleSpell(w, 355, "Taunt", SpellKind::Taunt, 0, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0, false, false, 200);
    sampleSpell(w, 2050, "Lesser Heal", SpellKind::Heal, 1, 1500, 0, 0, 40, 0, 30, 40, 50, 0, 0, true, false);
    sampleSpell(w, 2061, "Flash Heal", SpellKind::Heal, 1, 1500, 0, 0, 40, 0, 45, 70, 85, 0, 0, true, false);
    sampleSpell(w, 585, "Smite", SpellKind::Damage, 1, 2000, 0, 0, 30, 0, 20, 12, 16, 0, 0, false, false);
    sampleSpell(w, 1243, "Power Word: Fortitude", SpellKind::Buff, 1, 0, 0, 0, 30, 0, 20, 0, 0, 1800000, 0, true, false);
    sampleSpell(w, 133, "Fireball", SpellKind::Damage, 2, 2500, 0, 0, 35, 0, 30, 14, 22, 0, 0, false, false);
    sampleSpell(w, 116, "Frostbolt", SpellKind::Damage, 4, 1800, 0, 0, 30, 0, 25, 12, 18, 0, 0, false, false);
    sampleSpell(w, 2136, "Fire Blast", SpellKind::Damage, 2, 0, 8000, 0, 20, 0, 25, 12, 16, 0, 0, false, false);
    sampleSpell(w, 589, "Shadow Word: Pain", SpellKind::Dot, 5, 0, 0, 0, 30, 0, 25, 6, 6, 18000, 2000, false, false);
    sampleSpell(w, 1752, "Sinister Strike", SpellKind::Melee, 0, 0, 0, 0, 5, 3, 45, 10, 14, 0, 0, false, true);
    sampleSpell(w, 2098, "Eviscerate", SpellKind::Melee, 0, 0, 0, 0, 5, 3, 35, 15, 20, 0, 0, false, true);
    sampleSpell(w, 1766, "Kick", SpellKind::Interrupt, 0, 0, 10000, 0, 5, 3, 25, 0, 0, 0, 0, false, false);

    // ---- the four companions
    auto bot = [&](int i, const char* name, uint8_t race, uint8_t cls, uint8_t gender, Role role, uint16_t lvl,
                   uint32_t hp, uint32_t power, uint8_t pt, uint32_t display, std::initializer_list<uint32_t> spells) {
        BotTemplate& b = w.bots[(size_t)i];
        b.name = name; b.race = race; b.cls = cls; b.gender = gender; b.role = role;
        b.level = lvl; b.hp = hp; b.power = power; b.powerType = pt; b.displayId = display;
        b.skin = 1; b.face = 1;
        b.spellCount = 0;
        for (uint32_t s : spells) b.spells[b.spellCount++] = s;
        b.equipment[15] = 25;   // main hand
        b.equipment[3] = 38;    // shirt
        b.followDistance = role == Role::Tank ? 3.5f : 6.0f;
    };
    //        i  name          race cls gender role            lvl hp  power pt display
    bot(0, "Brom", 1, 1, 0, Role::Tank, 10, 320, 100, 1, 49, {6603, 78, 355, 6673});
    bot(1, "Elara", 1, 5, 1, Role::Healer, 10, 240, 260, 0, 50, {585, 2050, 2061, 1243});
    bot(2, "Mira", 7, 8, 1, Role::Caster, 10, 200, 300, 0, 60, {133, 116, 2136});
    bot(3, "Rik", 3, 4, 0, Role::Dps, 10, 230, 100, 3, 51, {6603, 1752, 2098, 1766});

    // ---- quests
    QuestDef q;
    q.id = 7;
    q.title = "Kobold Camp Cleanup";
    q.details = "Kobold vermin have been seen near the abbey. Thin their numbers.";
    q.completion = "The abbey is safer already.";
    q.level = 2;
    q.giverCreature = 197;
    q.objective.kind = 0;
    q.objective.target = 6;
    q.objective.count = 5;
    q.objective.text = "Kobold Vermin slain";
    q.rewardXp = 250;
    q.rewardMoney = 100;
    q.rewardItems[0] = 2589;
    q.rewardCounts[0] = 3;
    w.quests.push_back(q);

    // ---- faction hostility (both directions are required explicitly)
    auto hostile = [&](uint32_t a, uint32_t b) { w.hostility.push_back(HostilePair{a, b}); };
    hostile(factionHuman, factionMonster); hostile(factionMonster, factionHuman);
    hostile(factionHuman, factionDefias); hostile(factionDefias, factionHuman);
    (void)factionStormwind;

    return w.validate().empty();
}

}  // namespace offline
}  // namespace w3n
