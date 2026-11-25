// offline_save.cpp — journaled, checksummed, versioned offline save.
#include "offline_save.hpp"

#include <cstdio>
#include <cstring>

namespace w3n {
namespace offline {

namespace {

constexpr uint32_t kMagic = 0x53443357u;   // 'W','3','D','S'
constexpr size_t kMaxSaveBytes = 1u << 20;

uint32_t fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void putProfile(Writer& w, const CharacterProfile& p) {
    w.u8(p.used ? 1 : 0);
    w.u8(p.isBot ? 1 : 0);
    w.u8(p.role);
    w.u64(p.guid);
    w.cstr(p.name);
    w.u8(p.race); w.u8(p.cls); w.u8(p.gender);
    w.u8(p.skin); w.u8(p.face); w.u8(p.hairStyle); w.u8(p.hairColor); w.u8(p.facialHair);
    w.u16(p.level);
    w.u32(p.xp); w.u32(p.nextXp);
    w.u32((uint32_t)p.hp); w.u32((uint32_t)p.power);
    w.u32(p.maxHp); w.u32(p.maxPower);
    w.u8(p.powerType);
    w.u32(p.money);
    w.f32(p.x); w.f32(p.y); w.f32(p.z); w.f32(p.o);
    w.u32(p.mapId); w.u32(p.zoneId);
    for (uint32_t e : p.equipment) w.u32(e);
    w.u8(p.spellCount);
    for (uint8_t i = 0; i < p.spellCount && i < p.spells.size(); ++i) w.u32(p.spells[i]);
    w.u8(p.bagCount > 20 ? 20 : p.bagCount);
    for (uint8_t i = 0; i < p.bagCount && i < 20; ++i) {
        w.u32(p.bag[i].entry);
        w.u16(p.bag[i].count);
        w.u8(p.bag[i].slot);
    }
    for (uint8_t b : p.skinLook) w.u8(b);
}

bool getProfile(Reader& r, CharacterProfile& p) {
    p = CharacterProfile();
    p.used = r.u8() != 0;
    p.isBot = r.u8() != 0;
    p.role = r.u8();
    p.guid = r.u64();
    p.name = r.cstr();
    p.race = r.u8(); p.cls = r.u8(); p.gender = r.u8();
    p.skin = r.u8(); p.face = r.u8(); p.hairStyle = r.u8(); p.hairColor = r.u8(); p.facialHair = r.u8();
    p.level = r.u16();
    p.xp = r.u32(); p.nextXp = r.u32();
    p.hp = (int32_t)r.u32(); p.power = (int32_t)r.u32();
    p.maxHp = r.u32(); p.maxPower = r.u32();
    p.powerType = r.u8();
    p.money = r.u32();
    p.x = r.f32(); p.y = r.f32(); p.z = r.f32(); p.o = r.f32();
    p.mapId = r.u32(); p.zoneId = r.u32();
    for (uint32_t& e : p.equipment) e = r.u32();
    p.spellCount = r.u8();
    if (p.spellCount > p.spells.size()) return false;
    for (uint8_t i = 0; i < p.spellCount; ++i) p.spells[i] = r.u32();
    p.bagCount = r.u8();
    if (p.bagCount > 20) return false;
    for (uint8_t i = 0; i < p.bagCount; ++i) {
        p.bag[i].entry = r.u32();
        p.bag[i].count = r.u16();
        p.bag[i].slot = r.u8();
    }
    for (uint8_t& b : p.skinLook) b = r.u8();
    return !r.bad;
}

}  // namespace

bool saveOffline(const char* path, const OfflineSaveData& d, std::string& err) {
    Writer payload;
    payload.u32(d.version);
    payload.u32(d.seed);
    payload.u32(d.playTimeSec);
    payload.u8(d.rosterCount > 4 ? 4 : d.rosterCount);
    for (uint8_t i = 0; i < 4; ++i) putProfile(payload, d.roster[i]);
    payload.u8(d.active < 4 ? d.active : 0);
    for (const CharacterProfile& b : d.bots) putProfile(payload, b);
    payload.u8(d.commands.tank); payload.u8(d.commands.healer);
    payload.u8(d.commands.dps1); payload.u8(d.commands.dps2);
    payload.u8(d.commands.formation);
    payload.u8(0);
    const uint32_t qn = (uint32_t)d.quests.size();
    payload.u16((uint16_t)qn);
    for (const QuestLogEntry& q : d.quests) {
        payload.u32(q.id);
        for (uint16_t k : q.kills) payload.u16(k);
        for (uint16_t k : q.items) payload.u16(k);
        payload.u8(q.complete ? 1 : 0);
    }
    // WoWVita: the turned-in quests. Older readers stop before it; an older save simply has none.
    payload.u32(0x31445151u);   // "QQD1"
    payload.u32((uint32_t)d.rewarded.size());
    for (uint32_t id : d.rewarded) payload.u32(id);
    // WoWVita: each character's home (the inn) and the Hearthstone's cooldown
    payload.u32(0x314D4F48u);   // "HOM1"
    for (const CharacterProfile& c : d.roster) {
        payload.u8(c.homeSet ? 1 : 0);
        payload.f32(c.homeX); payload.f32(c.homeY); payload.f32(c.homeZ);
        payload.u32(c.homeZone);
        payload.u32(c.hearthCdSec);
    }
    // WoWVita: rested experience
    payload.u32(0x31545352u);   // "RST1"
    for (const CharacterProfile& c : d.roster) {
        payload.f32(c.restBonus);
        payload.u32(c.logoutTime);
        payload.u8(c.logoutResting ? 1 : 0);
    }
    // WoWVita: the flight paths discovered
    payload.u32(0x31584154u);   // "TAX1"
    for (const CharacterProfile& c : d.roster)
        for (uint32_t m : c.taxiMask) payload.u32(m);
    // WoWVita: the bank
    payload.u32(0x314B4E42u);   // "BNK1"
    for (const CharacterProfile& c : d.roster)
        for (const BagItem& b : c.bank) { payload.u32(b.entry); payload.u16(b.count); }
    // WoWVita: the areas explored
    payload.u32(0x31505845u);   // "EXP1"
    for (const CharacterProfile& c : d.roster)
        for (uint32_t m : c.explored) payload.u32(m);
    // WoWVita: durability (worn, backpack, bank)
    payload.u32(0x31525544u);   // "DUR1"
    for (const CharacterProfile& c : d.roster) {
        for (uint16_t l : c.equipLost) payload.u16(l);
        for (int i = 0; i < 20; ++i) payload.u16(c.bag[i].lost);
        for (const BagItem& b : c.bank) payload.u16(b.lost);
    }
    // WoWVita: the bags and what is in them beyond the profile's 20 items
    payload.u32(0x32474142u);   // "BAG2"
    for (const CharacterProfile& c : d.roster) {
        for (uint32_t b : c.bags) payload.u32(b);
        const uint16_t extra = c.bagCount > 20 ? (uint16_t)(c.bagCount - 20) : 0;
        payload.u16(extra);
        for (int i = 20; i < 20 + extra; ++i) { payload.u32(c.bag[i].entry); payload.u16(c.bag[i].count); payload.u16(c.bag[i].lost); }
    }
    // WoWVita: the mail
    payload.u32(0x314C414Du);   // "MAL1"
    payload.u32(d.nextMailId);
    payload.u32((uint32_t)d.mails.size());
    for (const OfflineMail& m : d.mails) {
        payload.u32(m.id); payload.u64(m.receiver); payload.u64(m.sender);
        payload.cstr(m.subject); payload.cstr(m.body);
        payload.u32(m.item); payload.u32(m.itemCount); payload.u32(m.money); payload.u32(m.flags); payload.u32(m.sentTime);
    }
    // WoWVita: the bank's bags
    payload.u32(0x31424B42u);   // "BKB1"
    for (const CharacterProfile& c : d.roster) {
        payload.u8(c.bankBagSlots);
        for (uint32_t b : c.bankBags) payload.u32(b);
        uint16_t n = 0;
        for (const BagItem& b : c.bankBagItems) n += b.entry != 0;
        payload.u16(n);
        for (size_t i = 0; i < c.bankBagItems.size(); ++i) {
            const BagItem& b = c.bankBagItems[i];
            if (!b.entry) continue;
            payload.u16((uint16_t)i); payload.u32(b.entry); payload.u16(b.count); payload.u16(b.lost);
        }
    }
    // WoWVita: the spell books and the action bars
    payload.u32(0x31425053u);   // "SPB1"
    for (const CharacterProfile& c : d.roster) {
        payload.u16((uint16_t)std::min<size_t>(c.book.size(), 4096));
        for (size_t i = 0; i < c.book.size() && i < 4096; ++i) payload.u32(c.book[i]);
        payload.u8(c.actionBarSet ? 1 : 0);
        if (c.actionBarSet) for (uint32_t a : c.actionBar) payload.u32(a);
    }
    // WoWVita: the summoned pet (Imp, Voidwalker): it comes back at login, as the original's current pet does
    payload.u32(0x31544550u);   // "PET1"
    for (const CharacterProfile& c : d.roster) {
        payload.u32(c.petEntry);
        payload.u32(c.petSpell);
        payload.cstr(c.petName);
        payload.u8(c.petAutocastOff);
    }
    payload.u32(0x31545048u);   // "HPT1": the hunter's pet
    for (const CharacterProfile& c : d.roster) {
        payload.u8(c.petHunter ? 1 : 0);
        payload.u32(c.petDisplay); payload.u32(c.petFamily); payload.u32(c.petXp); payload.u32(c.petHp);
        payload.u32(c.petLoyaltyXpNeed); payload.u8(c.petLevel); payload.u8(c.petLoyaltyLevel);
        payload.u32((uint32_t)c.petHappiness); payload.u32((uint32_t)c.petLoyaltyPoints);
        payload.u8((c.petRenamed ? 1 : 0) | (c.petOut ? 2 : 0));
    }
    payload.u32(0x314F4D41u);   // "AMO1": the ammo slot
    for (const CharacterProfile& c : d.roster) payload.u32(c.ammoId);
    payload.u32(0x314E4554u);   // "TEN1": the weapons' temporary enchantments (main hand, off hand, ranged)
    for (const CharacterProfile& c : d.roster)
        for (const TempEnchant& t : c.tempEnchant) { payload.u32(t.id); payload.u32(t.msLeft); payload.u32(t.item); }
    payload.u32(0x31534C50u);   // "PLS1": what the grimoires taught the pets (pet entry, spell)
    for (const CharacterProfile& c : d.roster) {
        payload.u8((uint8_t)std::min<size_t>(c.petLearned.size(), 64));
        for (size_t i = 0; i < c.petLearned.size() && i < 64; ++i) { payload.u32(c.petLearned[i].first); payload.u32(c.petLearned[i].second); }
    }
    Writer file;
    file.u32(kMagic);
    file.u16(kSaveVersion);
    file.u16(0);
    file.u32((uint32_t)payload.b.size());
    file.raw(payload.b);
    file.u32(fnv(payload.b.data(), payload.b.size()));

    const std::string finalPath(path);
    const std::string tmp = finalPath + ".tmp";
    const std::string bak = finalPath + ".bak";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) { err = "cannot write " + tmp; return false; }
    setvbuf(f, nullptr, _IOFBF, 16 * 1024);
    const size_t wrote = fwrite(file.b.data(), 1, file.b.size(), f);
    const int flushed = fflush(f);
    const int closed = fclose(f);
    if (wrote != file.b.size() || flushed != 0 || closed != 0) {
        remove(tmp.c_str());
        err = "the save could not be written (SD card full or removed?)";
        return false;
    }
    // Journal: keep the previous good file as .bak, then move the new one into place.
    remove(bak.c_str());
    rename(finalPath.c_str(), bak.c_str());   // fails harmlessly when there is no previous save
    if (rename(tmp.c_str(), finalPath.c_str()) != 0) {
        // The final rename failed: restore the backup so the next launch still has a save.
        rename(bak.c_str(), finalPath.c_str());
        remove(tmp.c_str());
        err = "the save could not be moved into place";
        return false;
    }
    err.clear();
    return true;
}

namespace {
bool loadFile(const char* path, OfflineSaveData& out, std::string& err) {
    FILE* f = fopen(path, "rb");
    if (!f) { err = "no save file"; return false; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); err = "seek failed"; return false; }
    const long len = ftell(f);
    if (len < 16 || (unsigned long)len > kMaxSaveBytes) { fclose(f); err = "bad save size"; return false; }
    Bytes buf((size_t)len);
    rewind(f);
    const size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) { err = "short read"; return false; }
    Reader r(buf);
    if (r.u32() != kMagic) { err = "not a WoW3DS save"; return false; }
    const uint16_t ver = r.u16();
    r.u16();
    const uint32_t payloadLen = r.u32();
    if (r.bad || payloadLen > r.left() || payloadLen + 4 != r.left()) { err = "truncated save"; return false; }
    const uint8_t* payload = buf.data() + r.pos;
    const uint32_t want = [&] { Reader c(buf, r.pos + payloadLen); return c.u32(); }();
    if (fnv(payload, payloadLen) != want) { err = "save checksum mismatch"; return false; }
    if (ver < 1 || ver > kSaveVersion) { err = "unsupported save version " + std::to_string(ver); return false; }
    Reader p(payload, payloadLen);
    OfflineSaveData d;
    d.version = ver;
    (void)p.u32();   // payload format version (the file header already carries it)
    d.seed = p.u32();
    d.playTimeSec = p.u32();
    d.rosterCount = p.u8();
    if (d.rosterCount > 4) { err = "malformed character roster"; return false; }
    for (CharacterProfile& c : d.roster)
        if (!getProfile(p, c)) { err = "malformed character profile"; return false; }
    d.active = p.u8();
    if (d.active >= 4) d.active = 0;
    if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
    for (CharacterProfile& b : d.bots)
        if (!getProfile(p, b)) { err = "malformed companion profile"; return false; }
    d.commands.tank = p.u8();
    d.commands.healer = p.u8();
    d.commands.dps1 = p.u8();
    d.commands.dps2 = p.u8();
    d.commands.formation = p.u8();
    p.u8();
    const uint16_t qn = p.u16();
    if (qn > 64) { err = "malformed quest log"; return false; }
    for (uint16_t i = 0; i < qn && !p.bad; ++i) {
        QuestLogEntry q;
        q.id = p.u32();
        for (uint16_t& k : q.kills) k = p.u16();
        for (uint16_t& k : q.items) k = p.u16();
        q.complete = p.u8() != 0;
        d.quests.push_back(q);
    }
    if (!p.bad && p.left() >= 8) {   // WoWVita: the turned-in quests
        const uint32_t magic = p.u32();
        if (magic == 0x31445151u) {
            const uint32_t n = p.u32();
            if (n > 16384 || (size_t)n * 4 > p.left()) { err = "malformed quest history"; return false; }
            for (uint32_t i = 0; i < n; ++i) d.rewarded.push_back(p.u32());
        }
    }
    if (!p.bad && p.left() >= 4 + 4 * 21) {   // WoWVita: the homes
        if (p.u32() == 0x314D4F48u) {
            for (CharacterProfile& c : d.roster) {
                c.homeSet = p.u8() != 0;
                c.homeX = p.f32(); c.homeY = p.f32(); c.homeZ = p.f32();
                c.homeZone = p.u32();
                c.hearthCdSec = p.u32();
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        }
    }
    if (!p.bad && p.left() >= 4 + 4 * 9) {   // WoWVita: rested experience
        if (p.u32() == 0x31545352u) {
            for (CharacterProfile& c : d.roster) {
                c.restBonus = p.f32();
                if (!(c.restBonus >= 0.0f && c.restBonus < 1e7f)) c.restBonus = 0;
                c.logoutTime = p.u32();
                c.logoutResting = p.u8() != 0;
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        }
    }
    if (!p.bad && p.left() >= 4 + 4 * 32) {   // WoWVita: the flight paths
        if (p.u32() == 0x31584154u) {
            for (CharacterProfile& c : d.roster)
                for (uint32_t& m : c.taxiMask) m = p.u32();
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        }
    }
    if (!p.bad && p.left() >= 4 + 4 * 24 * 6) {   // WoWVita: the bank
        if (p.u32() == 0x314B4E42u) {
            for (CharacterProfile& c : d.roster)
                for (BagItem& b : c.bank) { b.entry = p.u32(); b.count = p.u16(); if (!b.entry) b.count = 0; }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        }
    }
    if (!p.bad && p.left() >= 4 + 4 * 64 * 4) {   // WoWVita: the areas explored
        const size_t mark = p.pos;
        if (p.u32() == 0x31505845u) {
            for (CharacterProfile& c : d.roster)
                for (uint32_t& m : c.explored) m = p.u32();
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;   // (an older save: the mail follows)
        }
    }
    if (!p.bad && p.left() >= 4 + 4 * 63 * 2) {   // WoWVita: durability
        const size_t mark = p.pos;
        if (p.u32() == 0x31525544u) {
            for (CharacterProfile& c : d.roster) {
                for (uint16_t& l : c.equipLost) l = p.u16();
                for (int i = 0; i < 20; ++i) c.bag[i].lost = p.u16();
                for (BagItem& b : c.bank) b.lost = p.u16();
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (!p.bad && p.left() >= 4 + 4 * 18) {   // WoWVita: the bags
        const size_t mark = p.pos;
        if (p.u32() == 0x32474142u) {
            for (CharacterProfile& c : d.roster) {
                for (uint32_t& b : c.bags) b = p.u32();
                const uint16_t extra = p.u16();
                if (extra > 140) { err = "malformed bags"; return false; }
                for (int i = 20; i < 20 + extra && !p.bad; ++i) {
                    c.bag[i].entry = p.u32(); c.bag[i].count = p.u16(); c.bag[i].lost = p.u16(); c.bag[i].slot = (uint8_t)i;
                }
                if (extra) c.bagCount = (uint8_t)(20 + extra);
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (!p.bad && p.left() >= 12) {   // WoWVita: the mail
        if (p.u32() == 0x314C414Du) {
            d.nextMailId = p.u32();
            const uint32_t n = p.u32();
            if (n > 4096) { err = "malformed mail"; return false; }
            for (uint32_t i = 0; i < n && !p.bad; ++i) {
                OfflineMail m;
                m.id = p.u32(); m.receiver = p.u64(); m.sender = p.u64();
                m.subject = p.cstr(); m.body = p.cstr();
                m.item = p.u32(); m.itemCount = p.u32(); m.money = p.u32(); m.flags = p.u32(); m.sentTime = p.u32();
                if (!p.bad) d.mails.push_back(std::move(m));
            }
        }
    }
    if (!p.bad && p.left() >= 4) {   // WoWVita: the bank's bags
        const size_t mark = p.pos;
        if (p.u32() == 0x31424B42u) {
            for (CharacterProfile& c : d.roster) {
                c.bankBagSlots = std::min<uint8_t>(6, p.u8());
                for (uint32_t& b : c.bankBags) b = p.u32();
                const uint16_t n = p.u16();
                if (n > c.bankBagItems.size()) { err = "malformed bank bags"; return false; }
                for (uint16_t k = 0; k < n && !p.bad; ++k) {
                    const uint16_t i = p.u16();
                    BagItem b;
                    b.entry = p.u32(); b.count = p.u16(); b.lost = p.u16();
                    if (i < c.bankBagItems.size()) { b.slot = (uint8_t)(i % 36); c.bankBagItems[i] = b; }
                }
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (!p.bad && p.left() >= 4) {   // WoWVita: the spell books
        const size_t mark = p.pos;
        if (p.u32() == 0x31425053u) {
            for (CharacterProfile& c : d.roster) {
                const uint16_t n = p.u16();
                if (n > 4096) { err = "malformed spell book"; return false; }
                c.book.resize(n);
                for (uint32_t& s : c.book) s = p.u32();
                c.actionBarSet = p.u8() != 0;
                if (c.actionBarSet) for (uint32_t& a : c.actionBar) a = p.u32();
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (!p.bad && p.left() >= 4) {   // WoWVita: the pets
        const size_t mark = p.pos;
        if (p.u32() == 0x31544550u) {
            for (CharacterProfile& c : d.roster) {
                c.petEntry = p.u32();
                c.petSpell = p.u32();
                c.petName = p.cstr();
                c.petAutocastOff = p.u8();
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (!p.bad && p.left() >= 4) {   // WoWVita: the hunters' pets
        const size_t mark = p.pos;
        if (p.u32() == 0x31545048u) {
            for (CharacterProfile& c : d.roster) {
                c.petHunter = p.u8() != 0;
                c.petDisplay = p.u32(); c.petFamily = p.u32(); c.petXp = p.u32(); c.petHp = p.u32();
                c.petLoyaltyXpNeed = p.u32(); c.petLevel = p.u8(); c.petLoyaltyLevel = p.u8();
                c.petHappiness = (int32_t)p.u32(); c.petLoyaltyPoints = (int32_t)p.u32();
                const uint8_t fl = p.u8();
                c.petRenamed = (fl & 1) != 0;
                c.petOut = (fl & 2) != 0;
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (!p.bad && p.left() >= 4) {   // WoWVita: the ammo slots
        const size_t mark = p.pos;
        if (p.u32() == 0x314F4D41u) {
            for (CharacterProfile& c : d.roster) c.ammoId = p.u32();
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (!p.bad && p.left() >= 4) {   // WoWVita: the temporary enchantments
        const size_t mark = p.pos;
        if (p.u32() == 0x314E4554u) {
            for (CharacterProfile& c : d.roster)
                for (TempEnchant& t : c.tempEnchant) { t.id = p.u32(); t.msLeft = p.u32(); t.item = p.u32(); }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (!p.bad && p.left() >= 4) {   // WoWVita: the pets' grimoire spells
        const size_t mark = p.pos;
        if (p.u32() == 0x31534C50u) {
            for (CharacterProfile& c : d.roster) {
                c.petLearned.clear();
                const uint8_t n = p.u8();
                for (uint8_t i = 0; i < n && !p.bad; ++i) { const uint32_t pe = p.u32(); c.petLearned.emplace_back(pe, p.u32()); }
            }
            if (d.rosterCount && d.roster[d.active].used) d.player = d.roster[d.active];
        } else {
            p.pos = mark;
        }
    }
    if (p.bad) { err = "malformed save payload"; return false; }
    out = std::move(d);
    err.clear();
    return true;
}
}  // namespace

bool loadOffline(const char* path, OfflineSaveData& data, std::string& err) {
    std::string first;
    if (loadFile(path, data, first)) return true;
    const std::string bak = std::string(path) + ".bak";
    std::string second;
    if (loadFile(bak.c_str(), data, second)) {
        err = "the main save was unreadable (" + first + "); restored the backup";
        return true;
    }
    err = first + " / backup: " + second;
    return false;
}

bool offlineSaveExists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

bool deleteOfflineSave(const char* path, std::string& err) {
    const std::string p(path);
    remove((p + ".tmp").c_str());
    remove((p + ".bak").c_str());
    if (remove(p.c_str()) != 0) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) { fclose(f); err = "could not delete the save"; return false; }
    }
    err.clear();
    return true;
}

}  // namespace offline
}  // namespace w3n
