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
    using PacketSink = std::function<void(uint16_t opcode, const Bytes& data)>;

    void setup(const WorldData* data, const char* savePath);

    // WoWVita: the four companions (the game turns them off unless offline_party.txt exists - see login.cpp)

    void setParty(bool on) { partyEnabled_ = on; }
    // WoWVita: the client's AreaTrigger.dbc shapes (areatriggers.txt): the inns are triggers ("TAVN")
    void setAreaTriggers(const AreaTriggerTable* t) { triggers_ = t; }
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
    // rested experience (offline_life.cpp)
    void updateRest(uint32_t nowMs);
    void restOnLogin();
    uint32_t wallNow() const;
    const AreaTriggerTable* triggers_ = nullptr;
    std::vector<const AreaTrigger*> tavernShapes_;
    const AreaTriggerTable* tavernShapesOf_ = nullptr;
    size_t tavernShapesCount_ = 0;
    bool resting_ = false;
    uint32_t restTickMs_ = 0, restSent_ = 0;

    // ---- events / state
    void dispatchEvents(uint32_t nowMs);
    void sendMovementToClient(uint32_t nowMs);
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
