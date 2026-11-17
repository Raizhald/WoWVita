// offline_data.hpp — loader for the versioned, indexable .w3do offline world pack.
//
// The pack is produced on the PC by tools/offline_data/export_offline_data.py (see
// docs/OFFLINE_DATA_FORMAT.md). It holds only immutable, server-authoritative data: creature
// templates, spawns, loot, items, a small spell subset, quests, faction hostility and the four
// PartyBot templates. Mutable progression lives in the save file (offline_save.hpp).
//
// The loader is defensive: every count is bounded, the section table is validated, strings are
// length-capped and the total unpacked size is limited, so a corrupt SD-card file cannot make the
// 3DS allocate or read out of bounds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "offline_core.hpp"

namespace w3n {
namespace offline {

constexpr uint32_t kWorldDataMaxBytes = 64u << 20;   // a test-region pack is tens of KB
constexpr uint16_t kWorldDataVersion = 1;

// Loads 'path'. On failure returns false and fills 'err'; 'out' is left empty.
bool loadWorldData(const char* path, WorldData& out, std::string& err);
bool loadWorldDataBuffer(const uint8_t* data, size_t size, WorldData& out, std::string& err);

// A built-in fallback used by tests and by `Offline` when no pack is on the SD card yet: the same
// data the sample exporter writes, in C++. It keeps M1..M5 playable without a PC conversion.
bool buildSampleWorldData(WorldData& out);

}  // namespace offline
}  // namespace w3n
