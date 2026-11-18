// offline_save.hpp — compact, versioned, checksummed persistence for the offline profile.
//
// The whole offline profile is small (the player, four companions, quest log, party commands).
// The world itself is *not* stored: creatures, loot and templates come from the immutable .w3do pack
// and are reconstructed on load; only deliberate world changes (a killed quest target, a completed
// quest) live in the save.
//
// Writes are journaled and atomic from the caller's point of view:
//   1. the new file is written to <path>.tmp and closed (the SD card flushes on close),
//   2. the previous <path> is renamed to <path>.bak,
//   3. <path>.tmp is renamed over <path>.
// A power loss at any step leaves <path> or <path>.bak readable; the loader falls back to .bak
// when the main file is missing or fails its checksum. The format is versioned and migrated.
#pragma once

#include <array>
#include <string>

#include "offline_core.hpp"

namespace w3n {
namespace offline {

constexpr uint16_t kSaveVersion = 1;

struct OfflineSaveData {
    uint32_t version = kSaveVersion;
    uint32_t seed = 0x57334F46u;      // world seed: the same save replays the same spawn rolls
    uint32_t playTimeSec = 0;
    // Up to four local characters (the glue character screen lists them). 'player' is a working copy
    // of roster[active] for the world; it is written back before saving.
    std::array<CharacterProfile, 4> roster{};
    uint8_t rosterCount = 0;
    uint8_t active = 0;
    CharacterProfile player;
    std::array<CharacterProfile, kMaxParty> bots{};
    PartyCommandState commands{};
    std::vector<QuestLogEntry> quests;
    std::vector<uint32_t> rewarded;   // WoWVita: quests turned in (an optional block at the end of the payload)
};

bool saveOffline(const char* path, const OfflineSaveData& data, std::string& err);
// Tries 'path', then '<path>.bak'. Returns false with a reason when both are unusable.
bool loadOffline(const char* path, OfflineSaveData& data, std::string& err);
bool offlineSaveExists(const char* path);
// Delete the save (the "delete character" path never removes the file, this is for a fresh start).
bool deleteOfflineSave(const char* path, std::string& err);

}  // namespace offline
}  // namespace w3n
