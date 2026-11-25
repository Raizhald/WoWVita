// offline_server.hpp — the OfflineSession: the "server-equivalent" layer of offline mode.
//
// It has two jobs:
//   1. own the local account: one realm, a character list, creation and deletion (no SRP, no TCP),
//   2. translate between the existing GameSession packet interface and the authoritative World.
//
// Data flow (all on the main thread, never recursive):
//
//   GameSession::send(CMSG) -> WorldClient::packetSink -> OfflineSession::onClientPacket (queue)
//   OfflineSession::update -> drain queue -> World::step(50 ms) -> packets -> GameSession::handle
//
// Nothing here duplicates the login protocol: the session only produces the packets a 1.12.1 world
// server would have produced after LOGIN_VERIFY_WORLD.
#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <vector>
#include <unordered_map>

#include "../client.hpp"
#include "../tables.hpp"
#include "offline_core.hpp"
#include "offline_save.hpp"

namespace w3n {
namespace offline {

class OfflineSession {
public:
    void sendSelfFieldsPublic() { sendSelfFields(); }   // (tests)
    static uint32_t petNextXp(uint32_t level);     // ObjectMgr::GetXPForPetLevel
    using PacketSink = std::function<void(uint16_t opcode, const Bytes& data)>;

    void setup(const WorldData* data, const char* savePath);

    // WoWVita: the four companions (the game turns them off unless offline_party.txt exists - see login.cpp)

    void setParty(bool on) { partyEnabled_ = on; }
    // WoWVita: the client's AreaTrigger.dbc shapes (areatriggers.txt): the inns are triggers ("TAVN")
    void setAreaTriggers(const AreaTriggerTable* t) { triggers_ = t; }
    // WoWVita: the client's taxi.txt (TaxiNodes, TaxiPath) and transports.txt (TaxiPathNode points): the flight masters
    void setTaxi(const TaxiTable* t, const std::string& transportsPath);
    // WoWVita: the area under the player (the client's area grid) with its AreaTable exploration level and explore bit:
    // a new one gives exploration XP (SMSG_EXPLORATION_EXPERIENCE)
    void reportArea(uint32_t area, uint32_t level, uint32_t bit);
    void setDurabilityFile(const std::string& path);   // WoWVita: the client's repair cost tables (durability.txt)
    bool flying() const { return flight_.active; }
    std::function<uint32_t()> wallClock;   // WoWVita: unix time for the rested bonus between sessions (default time())
    bool resting() const { return resting_; }

    bool party() const { return partyEnabled_; }
    // WoWVita: diagnostics (login.cpp writes them to offline_trace.txt): NPC interaction and each opcode the
    // offline server does not handle (once per opcode)
    std::function<void(const std::string&)> trace;
    // tests: a quest counts as turned in (to reach the middle of a chain)
    void testMarkRewarded(uint32_t id) { if (std::find(save_.rewarded.begin(), save_.rewarded.end(), id) == save_.rewarded.end()) save_.rewarded.push_back(id); questFieldsDirty_ = true; }
    // Load the profile, or create a fresh one. The four companions come from the .w3do templates.
    bool open(std::string& err);
    bool save(std::string* err = nullptr);

    // ---- local character selection (the glue screens' world_.characters() mirrors this list)
    const std::vector<Character>& characters() const { return chars_; }
    int createCharacter(const CharCreateInfo& c);     // returns a CHAR_CREATE_* code
    int deleteCharacter(uint64_t guid);              // returns a CHAR_DELETE_* code
    bool enterWorld(uint64_t guid, uint32_t nowMs, float pos[3], float* orient);
    void leaveWorld();                               // sync profiles, save, detach

    bool inWorld() const { return inWorld_; }
    uint64_t playerGuid() const { return playerGuid_; }
    uint32_t mapId() const { return mapId_; }
    World& world() { return world_; }
    const World& world() const { return world_; }
    const OfflineSaveData& saveData() const { return save_; }
    const std::string& lastError() const { return lastError_; }    void setGroundQuery(bool (*fn)(float, float, float, float*)) { world_.setGroundQuery(fn); }
    void setSink(PacketSink s) { sink_ = std::move(s); }
    void setCharacterName(const std::string& name) { save_.player.name = name; }

    // ---- the internal logical message interface
    void onClientPacket(uint16_t opcode, const Bytes& data, uint32_t nowMs);
    void update(uint32_t nowMs);

    struct Stats {
        uint32_t packetsIn = 0, packetsOut = 0, updates = 0, loots = 0, kills = 0, deaths = 0, steps = 0;
        uint32_t maxOutQueue = 0;
    } stats;

private:
    std::vector<uint16_t> unhandledSeen_;
    bool saveSoon_ = false, saveTraced_ = false;
    bool partyEnabled_ = true;   // (the core and its tests: on; the game sets it from offline_party.txt)
    // ---- setup / profile
    void defaultPlayer();
    void refreshCharacters();
    void syncProfiles();
    void loginPackets(uint32_t nowMs);

    // ---- packet helpers
    void send(uint16_t op) { send(op, Bytes{}); }
    void send(uint16_t op, const Writer& w) { send(op, w.b); }
    void send(uint16_t op, Bytes data);
    void flush();
    void sendUpdateObject(const Bytes& body);
    void sendSelfFields();
    void sendInventory();
    void sendPartyList();
    void sendPartyStats();
    void sendChat(uint8_t type, uint64_t sender, const std::string& text, const std::string& name = std::string());
    void sendEmote(uint64_t guid, uint32_t emote);   // WoWVita
    void giveStartOutfit(CharacterProfile& p);        // WoWVita
    // WoWVita: the spell book and the class trainers (offline_class.cpp)
    bool knows(uint32_t spell) const;
    void setupBook();
    void startStance(uint32_t nowMs);
    void sendSpellBook();
    bool trainerOf(uint64_t npc, const Trainer** t, bool* near) const;
    void sendTrainerList(uint64_t npc, uint32_t nowMs);
    bool trainerHello(uint64_t npc, uint32_t nowMs);
    void learn(uint32_t spell, uint32_t nowMs);
    bool handleClassSpells(uint16_t op, const Bytes& data, uint32_t nowMs);
    bool handlePet(uint16_t op, const Bytes& data, uint32_t nowMs);   // WoWVita: offline_class.cpp - the pet bar
    void sendPetRenamed(uint64_t guid);
    void sendPetSpells();                                           // SMSG_PET_SPELLS (guid 0 when there is none)
    void sendPetSummonField();                                      // the player's UNIT_FIELD_SUMMON
    void sendCharge(const Entity& p, uint32_t nowMs);
    static constexpr uint32_t kGossipTrain = 4;   // "I would like to train."
    static constexpr uint32_t kGossipUnlearn = 6; // WoWVita: "I wish to unlearn my talents." (GOSSIP_OPTION_UNLEARNTALENTS)
    bool canResetTalentsAt(uint64_t npc) const;   // Creature::CanTrainAndResetTalentsOf
    void sendCreatureQuery(uint32_t entry);
    void sendPlayerName(uint64_t guid);
    void sendItemQuery(uint32_t entry);
    void sendSpellStart(const Entity& caster, uint32_t spell, uint64_t target, uint32_t castMs, uint8_t school);
    void sendSpellGo(const Entity& caster, uint32_t spell, uint64_t target, uint8_t school);
    void sendCastFailed(const Entity& caster, uint32_t spell, uint8_t reason);

    // ---- client packet handlers
    void handleMovement(const Bytes& data, uint32_t nowMs);
    void handleAttack(const Bytes& data, uint32_t nowMs);
    void handleCast(const Bytes& data, uint32_t nowMs);
    void handleLoot(uint16_t op, const Bytes& data, uint32_t nowMs);
    void handleChat(const Bytes& data, uint32_t nowMs);
    void handleDeath(uint16_t op, const Bytes& data, uint32_t nowMs);
    void handleQuery(uint16_t op, const Bytes& data, uint32_t nowMs);
    void handleVendor(uint16_t op, const Bytes& data, uint32_t nowMs);   // WoWVita
    void sendListInventory(uint64_t npc);

    // ---- WoWVita: quests (offline_quests.cpp)
    bool handleQuest(uint16_t op, const Bytes& data, uint32_t nowMs);
    bool questHello(uint64_t npc, uint16_t op, bool vendor);
    void questKill(uint32_t entry, uint64_t guid, bool go = false);
    bool isExploreQuest(uint32_t id) const;
    void questAreaTrigger(uint32_t trigger);
    bool questWantsObject(uint32_t goEntry) const;
    struct Giver { bool ok = false, go = false; uint32_t entry = 0; float x = 0, y = 0; };
    Giver giverOf(uint64_t guid) const;
    void questItem(uint32_t item, uint32_t count);
    bool questWantsItem(uint32_t item) const;
    const QuestFull* questDef(uint32_t id) const;
    int questSlot(uint32_t id) const;
    bool questRewarded(uint32_t id) const;
    bool questDone(const QuestLogEntry& e) const;
    bool questCanTake(const QuestFull& q) const;
    uint32_t questXp(const QuestFull& q) const;
    uint32_t questMarkFor(uint32_t entry, bool go) const;
    std::vector<std::pair<uint32_t, uint32_t>> questMenu(uint32_t entry, bool go) const;
    void questLogFields(std::vector<std::pair<uint16_t, uint32_t>>& f) const;
    void sendQuestStatus(uint64_t npc);
    void sendQuestDetails(uint64_t npc, const QuestFull& q);
    void sendQuestRequestItems(uint64_t npc, const QuestFull& q, bool done);
    void sendQuestOfferReward(uint64_t npc, const QuestFull& q);
    void sendQuestQuery(uint32_t id);
    bool questFieldsDirty_ = false;

    // ---- WoWVita: game objects around the player (offline_server.cpp, "game objects")
    bool handleGameObject(uint16_t op, const Bytes& data, uint32_t nowMs);
    void updateGameObjects(uint32_t nowMs, bool force);
    void goCreate(uint32_t idx);
    uint32_t goDynFlags(uint32_t idx) const;
    void goRefreshDynFlags();
    void goDespawn(uint32_t idx, uint32_t nowMs);
    bool handleGoLoot(uint16_t op, const Bytes& data, uint32_t nowMs);
    static uint64_t goGuid(uint32_t entry, uint32_t idx) { return 0xF110000000000000ull | ((uint64_t)(entry & 0xFFFFFF) << 24) | (uint64_t)(idx + 1); }
    std::unordered_map<int64_t, std::vector<uint32_t>> goCells_;
    std::vector<uint32_t> goRespawnAt_;
    std::vector<uint8_t> goActiveFlag_;
    std::vector<uint32_t> goActive_;                 // spawn indexes with an object on the client
    std::unordered_map<uint32_t, uint32_t> goDynSent_;
    uint32_t goNextScan_ = 0;
    struct GoLootState {
        uint64_t guid = 0;
        uint32_t idx = 0, money = 0;
        uint8_t n = 0;
        struct Slot { uint32_t item = 0; uint16_t count = 0; bool taken = false; } slots[16];
    } goLootState_;

    // ---- WoWVita: death and home (offline_life.cpp)
    bool handleLife(uint16_t op, const Bytes& data, uint32_t nowMs);
    bool lifeHello(uint64_t npc, bool near, uint32_t nowMs);
    const Graveyard* nearestGraveyard(float x, float y, uint32_t zone) const;
    void teleportPlayer(float x, float y, float z, float o, uint32_t nowMs);
    void showSpiritHealers(bool show);
    void backToLife(int32_t hpPercent, bool sickness, uint32_t nowMs);
    void sendBindPoint();
    void hearthHome(uint32_t nowMs);
    static bool isSpiritHealer(const Entity& e);
    static constexpr uint32_t kGossipBind = 2;   // the innkeeper's "Make this inn your home." (1 = the goods)
    uint32_t zone_ = 0;                          // CMSG_ZONEUPDATE
    bool corpseSet_ = false;
    float corpseX_ = 0, corpseY_ = 0, corpseZ_ = 0;
    uint32_t reclaimAtMs_ = 0;
    bool awaitTeleportAck_ = false;
    uint32_t teleportSentMs_ = 0, teleportCounter_ = 1;
    // bags (offline_server.cpp): the flat backpack list <-> bag 255 / 19..22 and a slot
    bool bagPosition(int index, uint8_t* bag, uint8_t* slot) const;
    int bagIndex(uint8_t bag, uint8_t slot) const;
    // food, drink, potions (offline_items.cpp)
    bool useConsumable(int index, uint32_t nowMs);
    void stopEating(uint32_t nowMs);
    void updateEating(uint32_t nowMs);
    bool inCombat() const;
    struct Eating {
        bool active = false;
        uint32_t spell = 0, startMs = 0, endMs = 0, nextTickMs = 0;
        uint32_t hpLeft = 0, manaLeft = 0, hpTick = 0, manaTick = 0;
        float x = 0, y = 0;
    } eat_;
    std::unordered_map<uint32_t, uint32_t> itemReady_;   // item category -> when it may be used again
    bool handleRepair(uint16_t op, const Bytes& data, uint32_t nowMs);
    uint32_t repairCost(uint32_t entry, uint16_t lost);
    static uint64_t itemGuidOf(bool equipment, int slot) { return 0x40000000ull | (uint64_t)(equipment ? (0x1000 + slot) : (0x2000 + slot)); }
    std::string durabilityPath_;
    bool durabilityLoaded_ = false;
    std::unordered_map<uint32_t, std::array<uint32_t, 29>> repairCosts_;
    std::unordered_map<uint32_t, float> repairQuality_;
    // bank and mail (offline_bank_mail.cpp)
    bool handleBank(uint16_t op, const Bytes& data, uint32_t nowMs);
    bool handleMail(uint16_t op, const Bytes& data, uint32_t nowMs);
    bool bankHello(uint64_t npc, bool near, uint32_t nowMs);
    bool bankerNear() const;
    void bankFields(std::vector<std::pair<uint16_t, uint32_t>>& self, std::vector<std::pair<uint64_t, BagItem>>& items) const;
    void bankChanged(const std::vector<int>& bankSlots, int bagFrom, uint8_t bagBefore);
    static uint64_t bankItemGuid(int slot);
    static uint64_t bankBagGuid(int k);                 // WoWVita: the bank's bags
    static uint64_t bankBagItemGuid(int k, int slot);
    uint8_t bankBagSize(int k) const;
    void bagChanged(int from, uint8_t before);          // WoWVita: backpack / bag objects from 'from' come again
    bool handleInventory(uint16_t op, const Bytes& data, uint32_t nowMs);
    bool mailboxNear(uint64_t box) const;
    OfflineMail* mailById(uint32_t id);
    void mailExpire();
    void sendMailList();
    void mailResult(uint32_t id, uint32_t action, uint32_t err);
    void mailAtLogin();
    uint64_t bankerGuid_ = 0;
    // flight masters (offline_taxi.cpp)
    bool handleTaxi(uint16_t op, const Bytes& data, uint32_t nowMs);
    bool taxiHello(uint64_t npc, bool near, bool viaGossipOption, uint32_t nowMs);
    void loadTaxiPaths();
    uint32_t taxiNodeFor(const Entity& npc) const;
    bool taxiKnown(uint32_t node) const;
    void taxiLearn(uint32_t node);
    void taxiInitial();
    void sendShowTaxi(uint64_t npc, uint32_t node);
    void startFlight(const std::vector<std::array<float, 3>>& pts, uint32_t fromNode, uint32_t nowMs);
    void updateFlight(uint32_t nowMs);
    static constexpr uint32_t kGossipTaxi = 3;   // "Show me where I can fly."
    const TaxiTable* taxi_ = nullptr;
    std::string transportsPath_;
    bool taxiPathsLoaded_ = false;
    std::unordered_map<uint64_t, std::vector<std::array<float, 3>>> taxiLegs_;   // from << 32 | to -> the points
    struct Flight {
        bool active = false;
        std::vector<std::array<float, 3>> pts;
        std::vector<float> cum;
        uint32_t startMs = 0, durMs = 0, mount = 0;
    } flight_;
    // rested experience (offline_life.cpp)
    void updateRest(uint32_t nowMs);
    void restOnLogin();
    uint32_t wallNow() const;
    const AreaTriggerTable* triggers_ = nullptr;
    std::vector<const AreaTrigger*> tavernShapes_;
    const AreaTriggerTable* tavernShapesOf_ = nullptr;
    size_t tavernShapesCount_ = 0;
    bool resting_ = false;
    uint32_t lastArea_ = 0;
    uint32_t restTickMs_ = 0, restSent_ = 0;

    // ---- events / state
    void dispatchEvents(uint32_t nowMs);
    void sendMovementToClient(uint32_t nowMs);
    void syncControl(uint32_t nowMs);   // WoWVita: the player's stun / root / slow -> forced movement packets
    bool ccRoot_ = false, ccStun_ = false;
    uint8_t netCombo_ = 0;       // WoWVita: the combo points the client was last told
    uint32_t itemCastSpell_ = 0, itemCastEntry_ = 0;   // WoWVita: an item's use spell under way (used up when it goes off)
    uint8_t netForm_ = 0, netPowerType_ = 0xFF;   // ... the form and power type
    uint32_t netEnchant_[3] = {};                  // WoWVita: the weapons' temporary enchantments the client knows
    uint32_t netPet_[6] = {};                      // ... the hunter pet's focus, happiness, loyalty, experience, level
    uint64_t netComboTarget_ = 0;
    float ccSpeed_ = 7.0f;
    uint32_t ccCounter_ = 0;
    void applyEventFieldUpdates(uint32_t nowMs);
    void errorLine(const std::string& text, uint32_t nowMs);
    Entity* botEntity(int i) { return world_.bot(i); }

    const WorldData* data_ = nullptr;
    std::string savePath_;
    OfflineSaveData save_;
    std::vector<Character> chars_;
    World world_;
    std::vector<std::pair<uint16_t, Bytes>> incoming_;
    std::vector<std::pair<uint16_t, Bytes>> outgoing_;
    PacketSink sink_;
    bool inWorld_ = false;
    bool ghost_ = false;
    bool opened_ = false;
    uint64_t playerGuid_ = 0;
    uint32_t mapId_ = 0;
    uint64_t target_ = 0;             // CMSG_SET_SELECTION
    uint64_t lootGuid_ = 0;
    uint32_t lastPartyStats_ = 0, lastMoveSend_ = 0, lastSave_ = 0;
    uint32_t partyHash_ = 0, lastPartyFull_ = 0;
    bool saveWarned_ = false;
    uint32_t lastInventoryItems_ = 0;  // a cheap change detector for the bag sync
    std::string lastError_;
    std::string playerName_;
};

}  // namespace offline
}  // namespace w3n
