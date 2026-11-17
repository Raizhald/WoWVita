# Offline world data format (`.w3do`)

Version 1 — little-endian. Written by `tools/offline_data/export_offline_data.py`, read by
`net/offline/offline_data.cpp`. The runtime refuses any other version, any out-of-range count and any
section that points outside the file.

This pack holds **immutable, server-authoritative** data only. Progressive state (levels, health,
inventory, quest log) lives in the save file. Dynamic world state is reconstructed from the pack plus
the save's world seed.

---

## 1. File layout

```
offset  size  field
0       4     magic  "W3DO"
4       2     u16    version (1)
6       2     u16    flags (0; reserved)
8       4     u32    sectionCount (1..64)
12      16*n  table  per section:
                     u32 tag            four characters, e.g. "CREA"
                     u32 offset         absolute file offset
                     u32 size           stored size in bytes
                     u32 codec          0 = raw, 1 = zlib (RFC 1950)
...           ...   section bodies, in any order
```

* Sections may appear in any order; unknown tags are skipped (forward compatibility).
* A pack must contain `INFO`, `CREA`, `SPWN` and `BOTS`; `LOOT`, `ITEM`, `SPEL`, `QUES`, `HOST`
  are optional.
* `codec = 1` is a normal zlib stream (2-byte header, deflate data, Adler-32), decoded with the
  client's existing `zlibInflate`. Uncompressed size is capped at 64 MiB.
* Strings are `u16 length` + that many UTF-8 bytes, no NUL terminator. Maximum 500 bytes.
* A missing optional section means "none of these objects", not an error.

### Safety limits enforced by the loader

| Item | Limit |
|---|---|
| file size | 64 MiB |
| sections | 64 |
| string | 512 bytes |
| `CREA` | 4 096 templates |
| `SPWN` | 16 384 spawns |
| `LOOT` | 4 096 tables, 64 entries each |
| `ITEM` | 8 192 templates |
| `SPEL` | 2 048 definitions |
| `QUES` | 2 048 quests |
| `BOTS` | exactly 4 |

---

## 2. Sections

### `INFO` — where the test area starts

```
u32 mapId
u32 zoneId
f32 spawnX, spawnY, spawnZ, spawnO
str name                      e.g. "Northshire Abbey (offline sample)"
```

Coordinates are the client's render/world coordinates (the same convention as
`SMSG_LOGIN_VERIFY_WORLD`), so no conversion is needed on the console.

### `CREA` — creature templates

```
u32 count
per template:
  u32 entry
  str name
  str subname                 "" or e.g. "Northshire Guard"
  u32 displayId               CreatureDisplayInfo id (from the server DB)
  u16 minLevel, maxLevel
  u32 faction                 faction template id
  u32 hp
  u32 armor
  u32 xp                      XP awarded to the player on death
  u16 dmgMin, dmgMax          base melee damage per swing
  u32 attackMs                swing interval (>= 500)
  u8  powerType               0 mana, 1 rage, 2 focus, 3 energy
  u32 power
  u8  rank                    0 normal, 1 elite, 2 rare elite, 3 boss, 4 rare
  u8  creatureType            1 beast, 2 dragonkin, … 7 humanoid
  u32 npcFlags                1 gossip, 2 quest giver, 4 vendor, … (0 = ordinary creature)
  f32 scale, speed, aggroRange, combatReach, boundingRadius
  u32 lootId                  0 = no loot, else a LOOT table id
  u8  abilityCount            0..4
  u32 abilities[abilityCount] spell ids the creature may cast
```

### `SPWN` — spawn points

```
u32 count
per spawn:
  u32 entry                   must exist in CREA
  f32 x, y, z, orientation
  u32 respawnMs               time from death to respawn (>= 5000 is enforced)
  u8  elite                   0/1 (reserved; the template's rank is used today)
```

The runtime GUID of a spawn is `0xF130000000000000 | (entry << 24) | (index + 1)`: stable across
sessions, so a save never needs to name a creature.

### `LOOT` — loot tables

```
u32 count
per table:
  u32 id
  u32 moneyMin, moneyMax      copper, rolled uniformly
  u16 entryCount
  per entry:
    u32 item                  must exist in ITEM
    u16 chance                0..10000 (10000 = always)
    u16 minCount, maxCount
```

### `ITEM` — item templates (loot, equipment, quest rewards)

```
u32 count
per item:
  u32 entry
  str name
  str description
  u32 displayId               ItemDisplayInfo id
  u32 quality                 0 poor … 5 legendary
  u32 inventoryType           0 = not equippable, 21 = main hand, …
  u32 itemLevel, requiredLevel
  u32 maxStack
  u32 buyPrice, sellPrice     copper
  u32 armor
  u32 delayMs                 weapon speed (0 for non-weapons)
  f32 dmgMin, dmgMax
  u32 class, subclass         2 = weapon; subclass 7 = one-handed sword, etc.
```

### `SPEL` — the supported spell subset

```
u32 count
per spell:
  u32 id
  str name
  u8  kind                    0 melee, 1 damage, 2 heal, 3 dot, 4 buff,
                              5 taunt, 6 interrupt
  u8  school                  0 physical, 1 holy, 2 fire, 3 nature, 4 frost,
                              5 shadow, 6 arcane
  u32 castMs                  0 = instant
  u32 cooldownMs
  u32 category, categoryCooldownMs
  f32 minRange, maxRange      yards; maxRange 0 = melee reach (5 yd)
  u8  powerType
  u32 powerCost
  i32 amountMin, amountMax    damage/heal per application
  u32 durationMs              dot/buff duration (0 = none)
  u32 tickMs                  dot tick interval
  i32 threatBonus             flat extra threat for melee abilities
  f32 radius                  reserved for AoE (not implemented)
  u8  friendly                1 = must target a friendly unit
  u8  startsAutoAttack        1 = melee ability that starts auto-attack
```

A spell id not present here cannot be cast in offline mode. This is intentional: the offline layer
implements a documented subset, not all of Vanilla's spell effects.

### `QUES` — simple quests

```
u32 count
per quest:
  u32 id
  str title, details, completionText
  u16 level
  u32 giverCreature           CREA entry (0 = none)
  u8  objectiveKind           0 kill creature entry, 1 collect item entry
  u32 objectiveTarget
  u16 objectiveCount
  str objectiveText
  u32 rewardXp, rewardMoney
  u32 rewardItems[3]
  u16 rewardCounts[3]
```

The core tracks acceptance, kill progress and turn-in rewards. The 1.12 gossip/quest packet flow is
**not** wired to this yet (see `docs/OFFLINE_ARCHITECTURE.md` §8); the data is validated and the
progression API is exercised by the tests.

### `BOTS` — the four PartyBot templates

```
u8 count                        must be 4
per bot:
  str name
  u8 race, class, gender
  u8 skin, face, hairStyle, hairColor, facialHair
  u8 role                       0 tank, 1 healer, 2 dps (melee), 3 caster
  u16 level
  u32 hp, power
  u8  powerType
  u32 displayId                 native player display id (49 human male, 50 female …)
  u8  spellCount (<= 12)
  u32 spells[spellCount]
  u32 equipment[19]             item entries by equipment slot; 0 = empty
  f32 followDistance            preferred distance when following
```

The four bots become normal player-look world objects with stable GUIDs `0x…0B01 + index`, so the
existing party frame, name query and model renderer work without changes.

### `HOST` — faction hostility

```
u32 count
per pair:
  u32 factionA
  u32 factionB
```

Hostility is explicit and symmetric-by-convention: the exporter writes both directions. The player
and companions use faction `1`. Faction `0`, NPCs with gossip/quest flags and dead units are never
hostile.

---

## 3. Producing a pack

### From the sample (no game data needed)

```bash
python tools/offline_data/export_offline_data.py --sample offline.w3do
python tools/offline_data/export_offline_data.py --verify offline.w3do
```

`--sample` mirrors the compiled-in Northshire dataset: 4 creature templates, 10 spawns, 4 items,
15 spells, 1 quest, 4 companions. It is the dataset used by the automated tests and as the fallback
when no exported pack is on the SD card.

### From a Vanilla database dump

The importer accepts a directory of `.sql` files with `INSERT INTO` statements (vMaNGOS / CMaNGOS
style; columns are matched by name, so minor schema differences are tolerated):

```bash
python tools/offline_data/export_offline_data.py \
  --sql /path/to/vmangos-sql --map 0 --tiles 30 46 34 50 \
  --out offline.w3do --compress
```

Tables used:

| Table | Used for |
|---|---|
| `creature_template` | name, subname, level, health, mana, damage, attack time, armour, faction, NPC flags, display id, loot id |
| `creature` | spawn position/orientation, map, respawn time |
| `creature_loot_template` | item, chance, min/max count |
| `item_template` | name, display, quality, inventory type, damage, delay, armour, stack, prices |
| `creature_questrelation` | the quest giver of a quest |
| `quest_template` | title, details, kill/collect objective, rewards |

The supported spell subset is not read from `spell_template` (its coefficient columns vary between
cores and are incomplete for the effects this runtime implements). The exporter always emits the
documented 15-spell subset and validates that every creature ability and companion spell exists.

`--tiles X0 Y0 X1 Y1` clips spawns to a tile rectangle (64×64 per continent; the default test region is
`30 46 34 50`, Elwynn Forest/Northshire). Without `--tiles`, every spawn of the map is written.

### Validation report

Every export re-reads its own output and performs an independent referential-integrity pass:

* every spawn points at an existing creature template,
* every loot item exists,
* every temple's loot id exists,
* every creature ability and companion spell exists,
* the pack has the required sections and the expected version.

The report is written to `<out>.report.txt` (or `--report FILE`). The command exits with status 1 if
there are errors, so it can gate a release. Example:

```
offline world pack report
==========================
counts: creatures=4, spawns=10, items=4, spells=15, bots=4
errors: 0
warnings: 0
```

Unresolved ids are reported as warnings/errors with the table and id; nothing is silently dropped.

---

## 4. Neural model file (`.w3nn`)

Separate from the world pack, small and versioned:

```
0   u32  magic "W3NN" (0x4E4E3357)
4   u16  version (1)
6   u16  inputs (24)
8   u16  hidden (16)
10  u16  outputs (8)
12  u8   shift1           per-layer power-of-two weight scale
13  u8   shift2
14  u16  reserved
16  u32  checksum         FNV-1a over the payload
20  payload:
      i8  W1[hidden][inputs]
      i32 B1[hidden]
      i8  W2[outputs][hidden]
      i32 B2[outputs]
```

Total 628 bytes. `tools/offline_train/w3nn.py` and `net/offline/offline_nn.cpp` implement the same
fixed-point expressions and are checked against shared golden vectors on every test run.

---

## 5. Where the files go on the console

```
sdmc:/3ds/w3dproto/offline.w3do          the exported pack (preferred)
sdmc:/3ds/w3dproto/offline_sample.w3do   the sample (used if offline.w3do is absent)
sdmc:/3ds/w3dproto/offline/profile.w3d   the local save (created automatically)
sdmc:/3ds/w3dproto/offline/party.w3nn    the optional trained policy
```

The 3DS build also contains the Northshire sample compiled in, so *Offline* works before any file is
copied. Map geometry/models still come from the normal `map*.w3dp`/`common*.w3dp` packs.
