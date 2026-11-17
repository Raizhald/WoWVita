# Offline World Core + Neural PartyBots — architecture

Status: **M1–M7 implemented and tested on the PC; M8 (real New 3DS measurements) is pending
hardware access.** Online mode is untouched: the new code only runs after the main menu's *Offline*
button, and every shared file keeps its online behaviour.

This document is the Phase 0 deliverable: it traces the existing packet/state flow, states the exact
integration boundary, inventories the server-side rules that a 1.12.1 realm normally owns, reviews the
existing fixtures and data formats, and then describes the three layers that were added. The last
sections list implemented and unsupported Vanilla systems, so nothing here is presented as "complete
offline WoW".

---

## 1. Phase 0 — source audit

### 1.1 Packet and state flow (unchanged online path)

```
TcpSocket (net/socket.cpp)
   │  beginConnect / pollConnect / sendAll / recvSome
   ▼
WorldClient (net/client.cpp)
   │  frame header decrypt → Packet{opcode, bytes}
   │  own state machine (WaitChallenge → … → CharSelect → WaitLogin → InWorld)
   │  consumes: SMSG_AUTH_CHALLENGE/RESPONSE, SMSG_CHAR_ENUM, SMSG_CHAR_CREATE/DELETE,
   │            SMSG_LOGIN_VERIFY_WORLD, SMSG_PONG, logout, cinematics, Warden notice
   ▼  onPacket(p)                 (everything the session itself did not consume)
GameSession (net/game.cpp and game_*.cpp)
   │  char/objects → ObjectTable (net/objects.cpp)
   │  movement      → Mover (net/movement.cpp)
   │  spells/items/quests/party/life/social/services … each file handles its opcodes
   ▼
UI / renderer / audio (proto3ds/source/*)
```

`GameSession` already exposes the two halves of the boundary we need:

* **server → client**: `bool GameSession::handle(uint16_t opcode, const Bytes&, uint32_t nowMs)` is
  public and is exactly what `WorldClient::onPacket` calls. It also has a `tap` hook for recording.
* **client → server**: every outgoing packet goes through `GameSession::send()` →
  `WorldClient::send(opcode, payload)`; the payload is already serialized in the stock 1.12 layout.

### 1.2 Minimum injection interface (implemented)

The smallest possible change is in `WorldClient` (net/client.hpp/.cpp):

```cpp
// Internal logical transport: when set, CMSGs go here instead of the socket.
std::function<void(uint16_t opcode, const Bytes& payload)> packetSink;

// Enter the world without a handshake; the caller supplies what LOGIN_VERIFY_WORLD and the
// character list would have said.
void beginOffline(uint64_t playerGuid, std::vector<Character> chars, uint32_t mapId,
                  const float pos[3], float orient);
void offlineSetCharacters(std::vector<Character>);
void offlineSetCreateResult(int code);
void offlineSetDeleteResult(int code);
```

That is the complete network-surface change. There is **no internal TCP server, no SRP, no header
cipher, no Warden and no fake socket**. Offline mode still sends the real CMSG bytes and consumes the
real SMSG bytes, so the existing handlers, queries, cooldowns, chat, party, loot and UI code are
exercised rather than duplicated.

The other direction is an `OfflineSession` (net/offline/offline_server.*) that feeds
`GameSession::handle`. Packets are queued and drained once per frame; the two layers never call each
other recursively.

### 1.3 Server→client handlers relevant to a playable offline demo

All of these already exist and are reused unchanged:

| Area | Messages the offline layer now produces |
|---|---|
| Session/bootstrap | `SMSG_LOGIN_SETTIMESPEED`, `SMSG_UPDATE_OBJECT` (create/values for self, companions, creatures, items) |
| Spells | `SMSG_INITIAL_SPELLS`, `SMSG_ACTION_BUTTONS`, `SMSG_SPELL_START`, `SMSG_SPELL_GO`, `SMSG_CAST_FAILED`, `SMSG_SPELL_FAILURE`, `SMSG_SPELLHEALLOG`, `SMSG_SPELLNONMELEEDAMAGELOG`, `SMSG_UPDATE_AURA_DURATION` |
| Melee | `SMSG_ATTACKSTART`, `SMSG_ATTACKSTOP`, `SMSG_ATTACKSWING_NOTINRANGE/BADFACING/DEADTARGET/CANT_ATTACK`, `SMSG_ATTACKERSTATEUPDATE` |
| Progression | `SMSG_LOG_XPGAIN` |
| Loot/items | `SMSG_LOOT_RESPONSE`, `SMSG_LOOT_REMOVED`, `SMSG_LOOT_CLEAR_MONEY`, `SMSG_LOOT_RELEASE_RESPONSE`, `SMSG_ITEM_QUERY_SINGLE_RESPONSE` |
| Queries | `SMSG_NAME_QUERY_RESPONSE`, `SMSG_CREATURE_QUERY_RESPONSE`, `SMSG_GAMEOBJECT_QUERY_RESPONSE` (unknown entries) |
| Party | `SMSG_GROUP_LIST`, `SMSG_PARTY_MEMBER_STATS_FULL` |
| Social | `SMSG_MESSAGECHAT` (say/party/system/NPC say) |
| Movement | relayed `MSG_MOVE_HEARTBEAT` for companions, `SMSG_MONSTER_MOVE` for creatures |
| Death | health-0 value update, `SMSG_CORPSE_RECLAIM_DELAY`, `MSG_CORPSE_QUERY` answer |

Client packets understood by the offline session: `CMSG_SET_ACTIVE_MOVER`, the `MSG_MOVE_*` family,
`CMSG_ATTACKSWING`, `CMSG_ATTACKSTOP`, `CMSG_CAST_SPELL`, `CMSG_CANCEL_CAST`, `CMSG_SET_SELECTION`,
`CMSG_LOOT`, `CMSG_LOOT_MONEY`, `CMSG_AUTOSTORE_LOOT_ITEM`, `CMSG_LOOT_RELEASE`,
`CMSG_NAME_QUERY`, `CMSG_CREATURE_QUERY`, `CMSG_GAMEOBJECT_QUERY`, `CMSG_ITEM_QUERY_SINGLE`,
`CMSG_REQUEST_PARTY_MEMBER_STATS`, `CMSG_MESSAGECHAT`, `CMSG_REPOP_REQUEST`,
`CMSG_RECLAIM_CORPSE`, `MSG_CORPSE_QUERY`, `CMSG_LOGOUT_REQUEST`.

Everything else (vendors, mail, auction, trade, guild, taxi, trainers, talents, channels) is
deliberately ignored offline; the existing client code stays compiled and online.

### 1.4 Rules that were authoritative on the remote server

The client is a renderer plus a prediction layer. Offline mode had to supply:

* HP/power pools, regeneration, rage/energy gain, death and resurrection.
* Melee swing timing, hit/miss/crit, damage, armour mitigation.
* Spell casting: cast times, GCD, per-spell and category cooldowns, resource cost, range and target
  validity.
* Threat and creature target selection; taunt.
* Auras (one DoT/buff per unit in this subset) and their ticks.
* Creature aggro, chase, leash/evade, respawn.
* XP awards, level-ups, loot rolls, money, item acquisition.
* Party membership and member stats (the client only *displays* them).
* Spawn/respawn timing and object visibility.
* Persistence.

The client already owns: terrain/water streaming, collision against map geometry, camera, input,
models/animation, UI, sound, action-bar layout, local cast prediction, cooldown *display*, gossip
window layout, loot window layout. None of that is reimplemented.

### 1.5 Existing party and healing fixtures reviewed

* `net/tests/party5.py` — independent Python server for `SMSG_GROUP_LIST`,
  `SMSG_PARTY_MEMBER_STATS(_FULL)`, invites/kick/promote. Its exact field order is what
  `OfflineSession::sendPartyList/sendPartyStats` reproduces.
* `net/tests/healer_test.cpp` — ghost + spirit-healer menu flow; the offline ghost/reclaim path keeps
  the same life-state semantics.
* `net/tests/stage5.py` — the packet layouts used for the offline builders (update blocks, movement,
  loot, melee, spells). The C++ side was written against the *client parsers* and cross-checked with
  these fixtures.
* `net/tests/life5.py`, `trade5.py` — item/life packet layouts used for the item-query response.

### 1.6 Existing data formats reviewed

* `.w3dp` (`common/w3d_format.h`, `converter/`) — map/asset packs, streamed by `proto3ds/source/streamer.cpp`.
  Offline mode **reuses them unchanged**; it does not embed world models.
* DBC-derived text (`spells.txt`, `factions.txt`, `talents.txt`, `spelldesc.bin`, `skills.txt`,
  `recipes.txt`, `locks.txt`, `areatriggers.txt`, `taxi.txt`, `questdb.bin`) — still loaded and used by
  the same `GameSession` fields (names, tooltips, icons, reactions).
* DBC files themselves contain client-side definitions (spell schools, icons, display ids) but **not**
  spawn positions, loot, creature stats, threat coefficients or quest objectives. That is why the new
  `.w3do` pack exists.

### 1.7 Memory / CPU budget in the existing builds

* The client ships a 178 MB-memory-mode CIA and a 124 MB build (`proto3ds/Makefile`, `README`).
  Rendering and streaming already dominate; the offline additions must stay in the low hundreds of KB.
* Measured additions on the PC reference build:
  * `WorldData` for the Northshire sample: **3.1 KB** on disk, ~8 KB in RAM (4 creatures, 10 spawns,
    4 items, 15 spells, 1 quest, 4 companions).
  * The simulation state is a fixed pool of **56 entities** (`sizeof(Entity)` ≈ 500 bytes →
    ~28 KB), no per-tick heap allocation.
  * `NeuralPolicy`: 16×24 + 8×16 INT8 weights + biases = **628 bytes** in the model file, the same in
    RAM. 512 MACs per inference.
  * Save file: a few KB.
  * 20 Hz simulation work measured on the PC: **~0.27 µs per bot decision**, ~0.4–0.5 ms per 20 s
    encounter for the whole world (PC, -O2). A 3DS measurement is still required (M8); see §9.
* Rendering/audio are untouched, and the simulation advances at most 8 catch-up steps per frame, so a
  long loading screen cannot cause a death spiral.

---

## 2. Architecture

Three layers, strictly separated:

```
┌──────────────────────────────────────────────────────────────────────┐
│ 1. Existing WoW3DS client                                            │
│    WorldClient (packet framing)  GameSession (1.12 semantics)        │
│    streamer / renderer / camera / UI / audio / character data         │
│    unchanged, except for WorldClient::packetSink + beginOffline       │
└───────────────▲──────────────────────────────────────┬───────────────┘
                │ SMSG (queued)                        │ CMSG (serialized, real bytes)
┌───────────────┴──────────────────────────────────────▼───────────────┐
│ 2. OfflineWorldCore                                                  │
│    OfflineSession  — local realm/profile, packet translation          │
│    World           — authoritative simulation (50 ms fixed step)      │
│    WorldData       — immutable .w3do pack (creatures, spawns, loot…)  │
│    PartyDirector   — four bots, deterministic Utility AI, legal mask  │
│    NeuralPolicy    — optional INT8 MLP over tactical categories       │
│    OfflineSave     — journaled, checksummed, versioned profile        │
└──────────────────────────────────────────────────────────────────────┘
```

The **same** `GameSession` renders and controls both modes. The offline layer has no access to
rendering, input or assets.

### 2.1 Modules

| File | Responsibility |
|---|---|
| `net/offline/offline_core.hpp` | immutable data types, entity/aura/cooldown structs, `World` API, events |
| `net/offline/offline_data.hpp/.cpp` | `.w3do` loader + referential-integrity validation + built-in sample |
| `net/offline/offline_world.cpp` | combat, threat, auras, creature AI, spawn/respawn, loot, XP, quests |
| `net/offline/offline_bots.hpp/.cpp` | Party Director, 24-feature contract, 8 legal actions, Utility AI |
| `net/offline/offline_nn.hpp/.cpp` | `W3NN` model loading, INT8 fixed-point inference, fallback |
| `net/offline/offline_server.hpp/.cpp` | local account, character list, all SMSG/CMSG translation |
| `net/offline/offline_save.hpp/.cpp` | atomic journaled save, checksums, backup recovery |
| `net/tools/w3doffline.cpp` | PC data check, scripted M3 fight, teacher dataset generator, benchmark |
| `net/tests/offline_test.cpp` | end-to-end test through the real `GameSession` |

### 2.2 Frame order (offline)

```
LoginFlow::update()
  └─ networkOffline()
       ├─ OfflineSession::update(ms)
       │    1. drain CMSGs queued by GameSession::send (previous frame)
       │    2. World::step(ms)  — fixed 50 ms steps, max 8 catch-up
       │    3. SimEvents → SMSG (queued)
       │    4. periodic companion movement, party stats, autosave
       │    5. flush SMSGs → GameSession::handle
       └─ (returns to the glue screens only when not InWorld)
main loop
  └─ GameSession::update(ms)  — sends movement packets, drains its own queries
```

Both directions are queued, so a packet handler can never re-enter the simulation.

### 2.3 Determinism

* Health, damage, healing, threat and XP are integers.
* The simulation advances in fixed 50 ms steps independent of FPS.
* A seeded xorshift32 RNG is the only source of randomness; it is part of the save.
* Entity iteration is pool-index order.
* Positions are floats but the same binary produces bit-identical results for the same inputs; the
  PC test replays a fight twice and compares HP. Cross-platform float variance is limited to
  positions (combat maths is integer).

---

## 3. Combat subset (Phase 3)

Implemented:

* HP and power (mana/rage/energy), regeneration, rage from hits and swings.
* Faction hostility from the pack (`HOST` section), NPCs with gossip/quest flags are never hostile.
* Melee: 2 s baseline (weapon delay from the item template), 5 % miss, 5 % crit, armour mitigation
  `damage * (1 - armour/(armour+400))`, swing timers independent of the frame rate.
* Spells: instant and cast-time, GCD 1.5 s, per-spell/category cooldowns, resource cost, range,
  friendly/hostile target validation, interrupt.
* Effects: direct damage, direct heal, one DoT per unit, buff aura, taunt, interrupt.
* Casting interrupts on damage for creature filler casts.
* Threat: damage adds threat, healing adds half of the effective heal, taunt sets `top + 1`,
  creature target = highest threat, threat tables decay when units die/leave.
* Creature behaviour: aggro radius, chase, melee, one ability on a 6–10 s timer, leash/evade at 45 yd
  from home or 30 yd from the target with no recent damage, respawn at the spawn point.
* Death: creature corpse becomes lootable, player becomes dead → ghost (release) → reclaim (50 % HP),
  companions recover after 10 s beside the player.
* Loot: deterministic rolls from the loot table, money, items stacked into a 20-slot backpack,
  loot window via the stock packets, `looted` flag.
* XP and level-ups (an intentionally simple, documented curve `400 + 300*(level-1)`).

Not implemented in this subset: dodge/parry/block/glancing, armour/resistance schools beyond physical
mitigation, AoE spells, crowd control chains, line of sight (no vmaps), spell pushback, weapon skills,
procs, pets, durability, resting/inn XP.

The supported spell list is data-driven (`SPEL` section). The sample defines 15 spells: Attack,
Heroic Strike, Battle Shout, Taunt, Lesser Heal, Flash Heal, Smite, Power Word: Fortitude, Fireball,
Frostbolt, Fire Blast, Shadow Word: Pain, Sinister Strike, Eviscerate, Kick.

---

## 4. PartyBots (Phase 4)

Four companions are defined by the pack's `BOTS` section and created from the same
`CharacterProfile` structure as the player:

| Role | Sample | Behaviour |
|---|---|---|
| Tank | Brom, warrior | acquires the player's target, keeps threat, taunts when the healer is attacked, avoids fresh packs |
| Healer | Elara, priest | emergency heal below 30 %, normal heal below 85 %, damage only with comfortable mana |
| DPS | Mira, mage | assists the tank, keeps range, respects the 80 % threat cap |
| DPS | Rik, rogue | assists in melee, uses energy abilities, kicks casts |

* Identity: stable GUID `0x…0B01 + i`, persisted profile, class/role/level/equipment/spell list.
* Party UI: they appear in the existing `SMSG_GROUP_LIST` / `PARTY_MEMBER_STATS` flow; the player is
  the leader.
* Movement: formation slots around the player (tank ahead, healer behind, DPS on the sides),
  separation/rejoin at 55 yd, relayed `MSG_MOVE_HEARTBEAT` so the stock client interpolates them.
* Commands: party chat `/p follow`, `/p assist`, `/p stay`, `/p retreat` (also `!follow` etc.
  without the slash command path). They are ordinary `CMSG_MESSAGECHAT` packets interpreted by the
  offline session, so the chat log shows them.
* Safety: no voluntary pull while the player is out of combat unless the pack is small and close;
  healer never heals dead units; threat cap for DPS; deterministic fallback below.
* No overlap: each bot has a distinct formation slot; a "stay" bot holds position.

The Director is a Utility AI: each bot computes 24 normalized features every 200 ms, gets a legal
action mask, and scores eight categories. Emergency rules (heal below 30 %, taunt when the healer is
attacked, retreat below 20 % HP) run *before* any learned model and cannot be overridden.

---

## 5. Tiny neural policy (Phases 5–7)

* Model A: **MLP 24 → 16 → 8**, 512 MACs per inference excluding bias/activation.
* Inputs: the 24-feature contract from the brief, refined to the data the core actually has
  (the list is in `offline_bots.hpp`; feature 14 is constant 1 because line of sight is not simulated,
  feature 18 is 0 because crowd-control restrictions are not simulated). Roles/classes are encoded in
  the input.
* Outputs: eight tactical categories, mapped to role-specific legal actions afterwards:
  basic attack, high-priority ability, defensive, heal/support, interrupt/CC, reposition,
  retreat/recover, hold/conserve.
* Runtime: `W3NN` file, INT8 weights, per-layer power-of-two shifts, INT32 accumulators,
  `round(n/127)` with truncation toward zero, no allocation, no floating point. 628-byte file.
* **Legal-action mask + emergency overrides always apply.** The model cannot cast without mana, heal a
  dead unit, ignore a cooldown or break threat rules.
* Fallback: if the file is missing, corrupt or returns an illegal category, the deterministic Utility
  AI decides. Missing is the default state (production AI is deterministic).
* Training: imitation learning from the deterministic teacher. `w3doffline --gen-data` records the
  teacher's features/labels/mask/reward in the real engine; `tools/offline_train/train_policy.py`
  trains the MLP (stdlib) and exports INT8 + bit-exact golden vectors;
  `tools/offline_train/train_policy_torch.py` is the same pipeline with PyTorch when it is installed.
* Measured teacher dataset: 21 588 samples / 60 episodes, validation accuracy **99.2 %** float and
  **99.2 %** INT8 (quantisation loss 0.0 %), golden vectors 0 mismatches.
* Benchmark (`w3doffline --bench`, 20 held-out encounters, 60 % starting health, densest camp):
  both controllers complete **20/20** encounters; the model heals slightly more (5 225 vs 5 080) and
  makes 1 042 vs 966 action changes. **No meaningful advantage is demonstrated.** The honest
  conclusion for M6 is therefore: the model is integrated, safe and bit-exact, but it only imitates
  the teacher; the deterministic Director remains the default and no DSP work is justified.
* Decisions run at 5 Hz per bot, staggered by 50 ms, so four bots do at most one inference each
  200 ms. The model is shared; each bot has independent features and (if ever added) recurrent state.

Model B (32→32→12) and Model C (GRU) were deliberately **not** added: the mission says not to add
complexity without a demonstrated limitation, and Model A already reproduces the teacher.

---

## 6. Persistence (Phase 9)

* `sdmc:/3ds/w3dproto/offline/profile.w3d` (packed test path: `offline_test_save.w3d`).
* Contents: world seed, play time, up to four local characters, the four companion profiles
  (level/XP/HP/power/equipment/spells/bags), party command modes, quest log.
* Not stored: creature state, loot rolls, spawn timers — all reconstructed from immutable data and the
  seed. This keeps saves small and makes "world changes" explicit.
* Write protocol: write `profile.w3d.tmp` → flush/close → rename the old file to `.bak` → rename the
  new file into place. The loader falls back to `.bak` when the main file is missing or its checksum
  fails. The file has a magic, a format version, a payload checksum and bounded counts.
* Autosave every 60 s plus on logout/quit/character switch. A failed autosave (full or removed SD
  card) is reported in chat once instead of silently losing progress.
* If both the main file and the backup are unreadable, the launcher moves them to
  `profile.w3d.corrupt` / `.corrupt.bak` and starts a fresh profile rather than locking the player out.
* Migration: `kSaveVersion` gates the reader; unknown newer files are refused with a clear message
  instead of being half-read. A v0 reader would be added alongside `kSaveVersion+1` when the layout
  changes.

---

## 7. 3DS integration

* Main menu → **Offline** → local character list (reusing the original character screens).
  * `Explore` (only shown offline) keeps the old developer free-camera map flight.
  * Character creation/deletion work on the local roster (up to four characters).
  * Entering the world ends in the normal `Phase::InWorld`, so rendering, streaming, UI, audio,
    camera, controls and `GameSession` consumers are unchanged.
* Files on the SD card:

```
sdmc:/3ds/w3dproto/
  map0.w3dp, common*.w3dp, ui.w3dp, sound/, *.txt    (existing converter output)
  offline.w3do            optional exported world pack (preferred)
  offline_sample.w3do     optional sample pack (fallback)
  offline/profile.w3d     the local save (created on first run)
  offline/party.w3nn      optional trained policy (deterministic AI if absent)
  offline_trace.txt       debug trace of the offline steps (created when Offline is used)
```

* Without any pack the build uses the compiled-in Northshire sample so M1 still works; the real
  creature/item/quest data comes from the exporter.
* `perf_events.txt` gains one `offline:` line a second (simulation time, packets, entities, policy).
* `Makefile`: `SOURCES := source ../net ../net/offline`; the offline files are ordinary C++20 with no
  new dependency and no exceptions/RTTI.
* Wi-Fi is never initialised on the offline path (`netInit` is only called by the online bootstrap).

---

## 8. Implemented vs missing Vanilla systems

Implemented: local login/character list/creation, one test zone from existing map packs, world
objects (creatures, corpses, items), movement input, melee and the listed spells, threat, auras,
death/ghost/reclaim, loot/money/items, XP/levels, four companions with the listed commands,
persistence, optional neural policy with deterministic fallback, autosave and backup recovery.

Missing (explicitly out of scope for this milestone): quest UI turn-in/dialog packets (data model and
kill progress exist), vendors/trainers/bank/mail/auction/trade offline, professions, talents, pets,
flight paths, transports, dungeons/raids (the packs can be loaded, but instance rules, doors and
scripts are not simulated), battlegrounds/PvP, line of sight/vertical collision for spells, creature
pathfinding around geometry, AoE/CC spell fidelity, full stat/armour/resistance model, resting/inn,
durability, item random properties/enchants, server-side anti-cheat, multiple concurrent zones and
dynamic spawn grids beyond the loaded pack.

---

## 9. Milestones and validation

| Milestone | Status | Evidence |
|---|---|---|
| M1 Offline login | done | `offline_test`: character list → world with no socket |
| M2 Local world | done | 5 players/companions + 10 creatures created from the pack |
| M3 Combat | done | scripted wolf fight: death, XP, loot; `w3doffline --fight` PASS |
| M4 Party | done | group list, 4 companions follow/assist, party commands |
| M5 Cooperative combat | done | kobold camp encounter, tank/healer/DPS, no avoidable wipe |
| M6 Neural party | done (no advantage shown) | INT8 model, 99.2 % imitation, legal mask, fallback |
| M7 Persistence | done | save/load/migration guard/backup recovery |
| M8 Hardware validation | **pending** | needs a New 3DS: CPU, RAM, FPS, stability |

PC reference numbers (MSYS2 GCC 16, -O2, 3.9 GHz desktop — *not* a 3DS measurement):

* world + 4 bots + packets: 0.4–0.5 ms per 20 s encounter in `--bench`;
* 0.27 µs per bot decision (features + utility choice + int8 inference);
* model 628 B, world data 3.1 KB, entity pool ≈ 28 KB, save ≈ 3 KB.

On hardware, M8 should record: `perf.csv` frame splits with offline mode on/off, `perf_events.txt`,
free-memory before/after entering the world, and the same fight repeated on a New 3DS. The
simulation cost is designed to be far below a frame; the risk is memory, not CPU.

Preliminary emulator baseline (Azahar, emulated New 3DS, sample pack, four bots + 15 entities):
`offline: sim avg 0.07–0.12 ms max 1–2 ms` per frame, 19 CMSGs/s, 48–131 SMSGs/s, 15 live entities.
The 200 ms frame spikes in that log are Azahar's GPU shader preparation (`prepare`), not the
simulation. Hardware numbers are still required for M8.

First real New 3DS run (sample pack, `.3dsx`, 124 MB mode, 43 s in the world): **44 FPS average**
(29–60), 12 units drawn, ~520 draws / 68k triangles, 15 hitches over 50 ms. The hitches are
`prepare`/texture spikes of 150–170 ms that also appear in the *online* baseline log from the same
area: the client's VRAM is at its limit (`vram_free` 16 KB, `vram_tex` 2.7 MB, `vram_targets` 1.4 MB)
and every new model/texture forces eviction. The world core is not the cause: the same log shows the
offline step at well under a millisecond (the `offline:` lines below confirm it on the next build).
Practical mitigations for smoother play: the 178 MB CIA mode instead of the 124 MB `.3dsx`, lower
draw distance / doodads / effects, and (for the demo data) fewer simultaneous creatures. Movement
packets are now sent only while a unit actually moves (one stop packet when it stops), and party
stats only when a value changes, which removes per-tick spline/path churn from the client.

A 64 KB local array in the model loader was the one hard fault found during bring-up: the CIA build
gives the main thread a stack too small for it, so pressing *Offline* jumped to address 0. It is now a
heap buffer (`NeuralPolicy::load`, 40-byte frame). `offline_trace.txt` records each offline step and is
the first file to look at if a console build ever stops at the same place. `offline_start.txt`
(`chars` or `world`) is a debug hook that enters offline mode at boot, used for the emulator runs.

---

## 10. Testing

Everything runs without a console, a server or game data:

```bash
net/tests/run_offline.sh
```

It builds the core with the real client sources, exports and verifies the sample pack (raw and
compressed), runs the data check and the M3 fight, generates the teacher dataset, trains and verifies
the INT8 model against golden vectors, runs the 72-check end-to-end session test through
`GameSession`, and prints the Phase 7 benchmark. Online tests are unchanged and still build the same
`net/*.cpp` files.

Known test gap: the sample data has only 39 interrupt samples out of 21 588; the model rarely predicts
that category. The legal mask still permits it and the deterministic teacher handles it when no model
is loaded. More scenarios (or real server data) are needed before claiming interrupt parity.

---

## 11. Deviations and deliberate decisions

* **No internal TCP server / SRP.** The existing architecture already exposes `GameSession::handle`
  and a serialized send path; an in-process queue is strictly smaller and removes an entire failure
  mode. The packet bytes are still the real ones.
* **One logical realm, up to four local characters.** The brief asks for one local profile; the
  roster exists so the original character screens are not a dead end.
* **All spawns of the pack are loaded.** The sample is a single zone. The format is indexable and the
  loader is built to stream regions later (a spawn grid would be the next step for a continent-sized
  pack); the mission explicitly starts with one test area.
* **No line of sight.** The client has geometry, but the server-side vmap data is not part of the
  packs. Feature 14 is documented as constant.
* **The model is optional and off by default**, because the benchmark did not show an advantage.
  Adding it costs 628 bytes and 512 MACs; the DSP is not touched (the ARM11 backend is fast enough,
  and stock DSP audio remains in use).
* **The free-camera dev flight was moved** from the main-menu *Offline* button to the offline
  character screen's *Explore* button. Online behaviour is unchanged.
