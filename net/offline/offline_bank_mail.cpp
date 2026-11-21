// offline_bank_mail.cpp — WoWVita: the bank and the mailboxes in the offline world, as the original 1.12 server runs them.
//
// Bank (vMaNGOS ItemHandler: HandleBankerActivateOpcode, HandleAutoBankItemOpcode, HandleAutoStoreBankItemOpcode): a banker
// (npcflag 0x100) opens it (SMSG_SHOW_BANK); its 24 slots are inventory slots 39..62 (PLAYER_FIELD_INV_SLOT_HEAD + 2 * slot),
// each with an item object of its own. An item goes in (CMSG_AUTOBANK_ITEM: onto a stack of the same item first, then the first
// free slot) and comes out into the backpack (CMSG_AUTOSTORE_BANK_ITEM) while a banker is near. The offline world has no bags,
// so no bank bag slots to buy.
//
// Mail (vMaNGOS MailHandler): a letter goes to another character of this save (all of them are one account, so it arrives at
// once, as vMaNGOS does for the same account), with a subject, a text, one item and / or money; postage 30 copper. Not to
// yourself, not to the other faction, no soulbound item (the Hearthstone). The mailbox lists the letters
// (SMSG_MAIL_LIST_RESULT), the text is an item text (CMSG_ITEM_TEXT_QUERY: the mail id), the money and the item are taken out,
// letters are marked read and deleted. A letter lasts 30 days; then an item or money in it goes back to the sender, the rest
// is gone. A character with unread mail hears "You have new mail." at login (SMSG_RECEIVED_MAIL).
#include <algorithm>
#include <cctype>
#include <cmath>

#include "offline_server.hpp"
#include "../objects.hpp"

namespace w3n {
namespace offline {

namespace {
enum : uint16_t {
    SMSG_DESTROY_OBJECT = 0x0AA, CMSG_BANKER_ACTIVATE = 0x1B7, SMSG_SHOW_BANK = 0x1B8, CMSG_BUY_BANK_SLOT = 0x1B9,
    SMSG_BUY_BANK_SLOT_RESULT = 0x1BA, CMSG_AUTOSTORE_BANK_ITEM = 0x282, CMSG_AUTOBANK_ITEM = 0x283,
    CMSG_SEND_MAIL = 0x238, SMSG_SEND_MAIL_RESULT = 0x239, CMSG_GET_MAIL_LIST = 0x23A, SMSG_MAIL_LIST_RESULT = 0x23B,
    CMSG_ITEM_TEXT_QUERY = 0x243, SMSG_ITEM_TEXT_QUERY_RESPONSE = 0x244, CMSG_MAIL_TAKE_MONEY = 0x245,
    CMSG_MAIL_TAKE_ITEM = 0x246, CMSG_MAIL_MARK_AS_READ = 0x247, CMSG_MAIL_DELETE = 0x249, SMSG_RECEIVED_MAIL = 0x285,
};
constexpr uint32_t kNpcBanker = 0x100;
constexpr float kBankRange = 15.0f, kMailboxRange = 10.0f;
constexpr int kBankSlots = 24, kBankFirst = 39;
constexpr uint32_t kPostage = 30;
constexpr uint32_t kMailDays = 30;
// mail actions / errors (SMSG_SEND_MAIL_RESULT)
constexpr uint32_t MAIL_SEND = 0, MAIL_MONEY_TAKEN = 1, MAIL_ITEM_TAKEN = 2, MAIL_DELETED = 4;
constexpr uint32_t MAIL_OK = 0, MAIL_ERR_EQUIP = 1, MAIL_ERR_TO_SELF = 2, MAIL_ERR_NO_MONEY = 3, MAIL_ERR_NOT_FOUND = 4,
                   MAIL_ERR_TEAM = 5, MAIL_ERR_INTERNAL = 6;

bool alliance(uint8_t race) { return race == 1 || race == 3 || race == 4 || race == 7; }
std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
}  // namespace

uint64_t OfflineSession::bankItemGuid(int slot) { return 0x40000000ull | (uint64_t)(0x3000 + slot); }

// ---------------------------------------------------------------- bank
bool OfflineSession::bankerNear() const {
    const Entity* p = world_.player();
    const Entity* v = bankerGuid_ ? world_.entity(bankerGuid_) : nullptr;
    return p && v && (v->npcFlags & kNpcBanker) && std::hypot(v->x - p->x, v->y - p->y) < kBankRange;
}

bool OfflineSession::bankHello(uint64_t npc, bool near, uint32_t nowMs) {
    const Entity* v = world_.entity(npc);
    if (!v || !(v->npcFlags & kNpcBanker) || ghost_) return false;
    if (!questMenu(v->entry, false).empty()) return false;   // (a banker with quests: its gossip)
    if (!near) { errorLine("You are too far away.", nowMs); return true; }
    bankerGuid_ = npc;
    Writer w;
    w.u64(npc);
    send(SMSG_SHOW_BANK, w.b);
    return true;
}

// The bank slots' fields and objects (sendInventory adds them).
void OfflineSession::bankFields(std::vector<std::pair<uint16_t, uint32_t>>& self, std::vector<std::pair<uint64_t, BagItem>>& items) const {
    const CharacterProfile& pr = world_.playerProfile();
    for (int i = 0; i < kBankSlots; ++i) {
        const BagItem& b = pr.bank[(size_t)i];
        const uint16_t f = (uint16_t)(UF::PLAYER_FIELD_INV_SLOT_HEAD + (kBankFirst + i) * 2);
        const uint64_t g = b.entry ? bankItemGuid(i) : 0;
        self.emplace_back(f, (uint32_t)g);
        self.emplace_back((uint16_t)(f + 1), (uint32_t)(g >> 32));
        if (b.entry) items.emplace_back(g, b);
    }
}

void OfflineSession::bankChanged(const std::vector<int>& bankSlots, int bagFrom, uint8_t bagBefore) {
    for (int s : bankSlots) {
        Writer w;
        w.u64(bankItemGuid(s));
        send(SMSG_DESTROY_OBJECT, w.b);
    }
    if (bagFrom >= 0)   // the backpack moved up from there: those objects go and come back
        for (int i = bagFrom; i < std::max<int>(bagBefore, world_.playerProfile().bagCount); ++i) {
            Writer w;
            w.u64(0x40000000ull | (uint64_t)(0x2000 + i));
            send(SMSG_DESTROY_OBJECT, w.b);
        }
    sendInventory();
    saveSoon_ = true;
}

bool OfflineSession::handleBank(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return false;
    Reader r(data);
    switch (op) {
        case CMSG_BANKER_ACTIVATE: {
            const uint64_t g = r.u64();
            const Entity* v = world_.entity(g);
            if (r.bad || !v) return true;
            bankHello(g, std::hypot(v->x - p->x, v->y - p->y) < kBankRange, nowMs);
            return true;
        }
        case CMSG_BUY_BANK_SLOT: {
            Writer w;
            w.u32(0);   // ERR_BANKSLOT_FAILED_TOO_MANY
            send(SMSG_BUY_BANK_SLOT_RESULT, w.b);
            errorLine("There are no bags offline: bank bag slots cannot be bought.", nowMs);
            return true;
        }
        case CMSG_AUTOBANK_ITEM: {   // backpack -> bank
            const uint8_t bag = r.u8(), slot = r.u8();
            if (r.bad) return true;
            if (!bankerNear()) { errorLine("You are too far away from a banker.", nowMs); return true; }
            CharacterProfile& pr = world_.playerProfile();
            const int index = slot - 23;
            if (bag != 255 || index < 0 || index >= pr.bagCount || !pr.bag[index].entry) return true;
            BagItem moving = pr.bag[index];
            const ItemTemplate* t = data_ ? data_->item(moving.entry) : nullptr;
            const uint32_t stackMax = t ? std::max<uint32_t>(1, t->maxStack) : 1;
            // where it goes: the stacks of the same item, then the first free slot
            std::vector<int> touched;
            uint32_t left = moving.count ? moving.count : 1;
            uint32_t room = 0;
            int freeSlot = -1;
            for (int i = 0; i < kBankSlots; ++i) {
                const BagItem& b = pr.bank[(size_t)i];
                if (b.entry == moving.entry && b.count < stackMax) room += stackMax - b.count;
                if (!b.entry && freeSlot < 0) freeSlot = i;
            }
            if (room < left && freeSlot < 0) { errorLine("Your bank is full.", nowMs); return true; }
            for (int i = 0; i < kBankSlots && left; ++i) {
                BagItem& b = pr.bank[(size_t)i];
                if (b.entry != moving.entry || b.count >= stackMax) continue;
                const uint32_t k = std::min<uint32_t>(left, stackMax - b.count);
                b.count = (uint16_t)(b.count + k);
                left -= k;
                touched.push_back(i);
            }
            if (left) {
                BagItem& b = pr.bank[(size_t)freeSlot];
                b.entry = moving.entry; b.count = (uint16_t)left; b.slot = (uint8_t)freeSlot; b.lost = moving.lost;
                touched.push_back(freeSlot);
            }
            const uint8_t before = pr.bagCount;
            world_.removeBagItem(index);
            bankChanged(touched, index, before);
            return true;
        }
        case CMSG_AUTOSTORE_BANK_ITEM: {   // bank -> backpack
            const uint8_t bag = r.u8(), slot = r.u8();
            if (r.bad) return true;
            if (!bankerNear()) { errorLine("You are too far away from a banker.", nowMs); return true; }
            CharacterProfile& pr = world_.playerProfile();
            const int i = slot - kBankFirst;
            if (bag != 255 || i < 0 || i >= kBankSlots || !pr.bank[(size_t)i].entry) return true;
            BagItem& b = pr.bank[(size_t)i];
            const uint8_t before = pr.bagCount;
            int why = 0;
            const bool asItIs = world_.maxDurability(b.entry) > 0;   // (an item with durability keeps it)
            if (asItIs ? !world_.putBagItem(b) : !world_.buyItem(b.entry, b.count ? b.count : 1, 0, nowMs, &why)) {
                errorLine("Inventory is full.", nowMs);
                return true;
            }
            b = BagItem();
            bankChanged({i}, 0, before);   // (it may have joined a stack: the whole backpack comes again)
            return true;
        }
        default:
            return false;
    }
}

// ---------------------------------------------------------------- mail
bool OfflineSession::mailboxNear(uint64_t box) const {
    const Entity* p = world_.player();
    if (!p || !data_ || (box >> 48) != 0xF110) return false;
    const uint32_t idx = (uint32_t)(box & 0xFFFFFF) - 1;
    if (idx >= data_->goSpawns.size()) return false;
    const GoSpawn& s = data_->goSpawns[idx];
    auto t = data_->goTemplates.find(s.entry);
    return t != data_->goTemplates.end() && t->second.type == 19 && std::hypot(s.x - p->x, s.y - p->y) < kMailboxRange;
}

OfflineMail* OfflineSession::mailById(uint32_t id) {
    for (OfflineMail& m : save_.mails)
        if (m.id == id && m.receiver == playerGuid_) return &m;
    return nullptr;
}

// 30 days: an item or money goes back to its sender, the rest is gone (vMaNGOS Mail expiry)
void OfflineSession::mailExpire() {
    const uint32_t now = wallNow();
    for (OfflineMail& m : save_.mails) {
        if (!m.sentTime || now < m.sentTime + kMailDays * 86400u) continue;
        if ((m.item || m.money) && !(m.flags & 2) && m.sender) {
            std::swap(m.receiver, m.sender);
            m.flags = 2;   // returned, unread
            m.sentTime = now;
            m.subject = m.subject.empty() ? std::string("(returned)") : m.subject;
        } else {
            m.receiver = 0;   // gone
        }
    }
    save_.mails.erase(std::remove_if(save_.mails.begin(), save_.mails.end(), [](const OfflineMail& m) { return !m.receiver; }),
                      save_.mails.end());
}

void OfflineSession::sendMailList() {
    mailExpire();
    const uint32_t now = wallNow();
    std::vector<const OfflineMail*> mine;
    for (const OfflineMail& m : save_.mails)
        if (m.receiver == playerGuid_ && mine.size() < 50) mine.push_back(&m);
    std::sort(mine.begin(), mine.end(), [](const OfflineMail* a, const OfflineMail* b) { return a->id > b->id; });
    Writer w;
    w.u8((uint8_t)mine.size());
    for (const OfflineMail* m : mine) {
        w.u32(m->id);
        w.u8(0);                         // a player's letter
        w.u64(m->sender);
        w.cstr(m->subject);
        w.u32(m->body.empty() ? 0 : m->id);   // the text (an item text with the mail's id)
        w.u32(0);                        // package
        w.u32(41);                       // stationery: the default
        w.u32(m->item);
        w.u32(0); w.u32(0); w.u32(0);    // enchant, random property, suffix factor
        w.u8((uint8_t)std::min<uint32_t>(m->itemCount, 255));
        w.u32(0); w.u32(0); w.u32(0);    // charges, max durability, durability
        w.u32(m->money);
        w.u32(0);                        // cash on delivery
        w.u32((m->flags & 1) | ((m->flags & 2) ? 0x2u : 0u));   // read, returned
        const float left = m->sentTime ? ((float)m->sentTime + kMailDays * 86400.0f - (float)now) / 86400.0f : (float)kMailDays;
        w.f32(std::max(0.0f, left));
        w.u32(0);                        // mail template
    }
    send(SMSG_MAIL_LIST_RESULT, w.b);
}

void OfflineSession::mailResult(uint32_t id, uint32_t action, uint32_t err) {
    Writer w;
    w.u32(id); w.u32(action); w.u32(err);
    send(SMSG_SEND_MAIL_RESULT, w.b);
}

void OfflineSession::mailAtLogin() {
    mailExpire();
    for (const OfflineMail& m : save_.mails)
        if (m.receiver == playerGuid_ && !(m.flags & 1)) {
            Writer w;
            w.u32(0);
            send(SMSG_RECEIVED_MAIL, w.b);
            return;
        }
}

bool OfflineSession::handleMail(uint16_t op, const Bytes& data, uint32_t nowMs) {
    Entity* p = world_.player();
    if (!p) return false;
    Reader r(data);
    switch (op) {
        case CMSG_GET_MAIL_LIST: {
            const uint64_t box = r.u64();
            if (r.bad) return true;
            if (!mailboxNear(box)) { errorLine("You are too far away from a mailbox.", nowMs); return true; }
            sendMailList();
            return true;
        }
        case CMSG_ITEM_TEXT_QUERY: {
            const uint32_t id = r.u32();
            if (r.bad) return true;
            const OfflineMail* m = mailById(id);
            Writer w;
            w.u32(id);
            w.cstr(m ? m->body : std::string());
            send(SMSG_ITEM_TEXT_QUERY_RESPONSE, w.b);
            return true;
        }
        case CMSG_SEND_MAIL: {
            const uint64_t box = r.u64();
            const std::string to = r.cstr(), subject = r.cstr(), body = r.cstr();
            r.u32(); r.u32();                 // stationery, package
            const uint64_t itemGuid = r.u64();
            const uint32_t money = r.u32();
            r.u32();                          // cash on delivery (not between one's own characters)
            if (r.bad) return true;
            if (!mailboxNear(box)) { errorLine("You are too far away from a mailbox.", nowMs); return true; }
            const CharacterProfile* dest = nullptr;
            for (uint8_t i = 0; i < save_.rosterCount; ++i)
                if (save_.roster[i].used && lower(save_.roster[i].name) == lower(to)) dest = &save_.roster[i];
            if (!dest) { mailResult(0, MAIL_SEND, MAIL_ERR_NOT_FOUND); return true; }
            if (dest->guid == playerGuid_) { mailResult(0, MAIL_SEND, MAIL_ERR_TO_SELF); return true; }
            CharacterProfile& pr = world_.playerProfile();
            if (alliance(dest->race) != alliance(pr.race)) { mailResult(0, MAIL_SEND, MAIL_ERR_TEAM); return true; }
            int index = -1;
            if (itemGuid) {
                const uint32_t low = (uint32_t)itemGuid;
                index = (low & 0xFFFFF000u) == (0x40000000u | 0x2000u) ? (int)(low & 0xFFFu) : -1;
                if (index < 0 || index >= pr.bagCount || !pr.bag[index].entry) { mailResult(0, MAIL_SEND, MAIL_ERR_INTERNAL); return true; }
                if (pr.bag[index].entry == kHearthstoneItem) { errorLine("You can't mail a soulbound item.", nowMs); mailResult(0, MAIL_SEND, MAIL_ERR_INTERNAL); return true; }
            }
            if ((uint64_t)p->money < (uint64_t)money + kPostage) { mailResult(0, MAIL_SEND, MAIL_ERR_NO_MONEY); return true; }
            OfflineMail m;
            m.id = save_.nextMailId++;
            m.receiver = dest->guid;
            m.sender = playerGuid_;
            m.subject = subject.substr(0, 64);
            m.body = body.substr(0, 500);
            m.money = money;
            m.sentTime = wallNow();
            const uint8_t before = pr.bagCount;
            if (index >= 0) {
                m.item = pr.bag[index].entry;
                m.itemCount = pr.bag[index].count ? pr.bag[index].count : 1;
                world_.removeBagItem(index);
            }
            p->money -= money + kPostage;
            pr.money = p->money;
            save_.mails.push_back(std::move(m));
            mailResult(0, MAIL_SEND, MAIL_OK);
            if (index >= 0) bankChanged({}, index, before);
            else sendSelfFields();
            saveSoon_ = true;
            return true;
        }
        case CMSG_MAIL_TAKE_MONEY: case CMSG_MAIL_TAKE_ITEM: case CMSG_MAIL_DELETE: case CMSG_MAIL_MARK_AS_READ: {
            const uint64_t box = r.u64();
            const uint32_t id = r.u32();
            if (r.bad) return true;
            OfflineMail* m = mailById(id);
            if (!m) return true;
            if (!mailboxNear(box)) { errorLine("You are too far away from a mailbox.", nowMs); return true; }
            if (op == CMSG_MAIL_MARK_AS_READ) { m->flags |= 1; saveSoon_ = true; return true; }
            if (op == CMSG_MAIL_TAKE_MONEY) {
                if (!m->money) return true;
                p->money += m->money;
                world_.playerProfile().money = p->money;
                m->money = 0;
                mailResult(id, MAIL_MONEY_TAKEN, MAIL_OK);
                sendSelfFields();
                saveSoon_ = true;
                return true;
            }
            if (op == CMSG_MAIL_TAKE_ITEM) {
                if (!m->item) return true;
                int why = 0;
                const uint8_t before = world_.playerProfile().bagCount;
                (void)before;
                if (!world_.buyItem(m->item, m->itemCount ? m->itemCount : 1, 0, nowMs, &why)) {
                    mailResult(id, MAIL_ITEM_TAKEN, MAIL_ERR_EQUIP);
                    return true;
                }
                m->item = 0;
                m->itemCount = 0;
                mailResult(id, MAIL_ITEM_TAKEN, MAIL_OK);
                bankChanged({}, 0, world_.playerProfile().bagCount);
                saveSoon_ = true;
                return true;
            }
            // delete
            save_.mails.erase(std::remove_if(save_.mails.begin(), save_.mails.end(),
                                             [&](const OfflineMail& x) { return x.id == id && x.receiver == playerGuid_; }),
                              save_.mails.end());
            mailResult(id, MAIL_DELETED, MAIL_OK);
            saveSoon_ = true;
            return true;
        }
        default:
            return false;
    }
}

}  // namespace offline
}  // namespace w3n
