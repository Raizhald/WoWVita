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

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "../client.hpp"
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

    bool party() const { return partyEnabled_; }
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
