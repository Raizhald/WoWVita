# WoWVita: the whole continent offline

The upstream offline mode ships a small Northshire sample. WoWVita adds what a whole-continent pack needs:

- **Spawn grid** (`net/offline/offline_world.cpp`, `World::updateActivation`): the spawns are kept in 64-yard cells;
  every 0.5 s the creatures within 100 yd of the player get an entity in the pool (nearest first, the pool holds 160),
  those farther than 130 yd (and not chasing anyone) leave it. The client gets `SMSG_UPDATE_OBJECT` / `SMSG_DESTROY_OBJECT`
  (`SimEvent::Appear` / `Vanish`). A creature that leaves dead keeps its respawn time.
- **Neutral creatures**: a `HOST` pair whose first faction has bit 31 set means "may be attacked, only fights back"
  (`WorldData::attackable`, `World::canAttack`). Red creatures still attack on sight.
- **Hash lookups** for creatures, items, loot tables and hostility (thousands of entries).
- **Larger limits**: 16 384 creature templates, 131 072 spawns, 32 768 items and loot tables.

## Making the pack

You need a world database dump of a 1.12 server (CMaNGOS classic-db `Full_DB/ClassicDB_1_12_1_*.sql.gz`, or a
vMaNGOS world dump) and the `factions.txt` the converter writes (`w3dconv --texts-only`, or the interface step):

```
python3 tools/offline_data/export_world_sql.py --sql ClassicDB_1_12_1_z2815.sql.gz --factions factions.txt \
    --spells spells.txt --map 0 --out offline.w3do
```

`--spells` (the converter's spells.txt, same folder as factions.txt) gives the creature spells their cast times,
ranges and durations (`@cast`, `@range`, and the `@idx cast|dur|range` lines of the SpellCastTimes / SpellDuration /
SpellRange tables a newer converter writes). Without the `@idx` lines most damage-over-time and aura spells of the
creatures are left out.

Map 0 (Eastern Kingdoms) gives about 24 000 spawns, 3 300 creature kinds and 3 500 items in 0.7 MB. Copy
`offline.w3do` to `ux0:data/wowvita/offline.w3do`; without it the game uses the Northshire sample.

Vendors: the pack's `VEND` section (npc_vendor / npc_vendor_template; limited-stock items left out) - talking to a
vendor opens its list, buying and selling change the backpack and the money, and backpack items can be worn
(`CMSG_AUTOEQUIP_ITEM`: weapon damage and armour follow). No buyback, repairs or bags yet.

Quests (`QST2`, `QLOT`, `GOSS` sections, `net/offline/offline_quests.cpp`): the server database's quest_template with
the creatures that give and take each quest. Quest giver marks, the quest list / gossip with the NPC's greeting,
details, accept, kill and item progress (quest drops only while a quest wants the item), request items, offer
reward, reward choice, money and experience (Quest::XPValue), chains (the next quest offered at once), abandon.
The log and the turned-in quests are saved. Left out by the exporter, because the offline core cannot finish them:
exploration and scripted events, game objects, spell casts, professions / reputation, timed quests, and items
that cannot be got on the map. Eastern Kingdoms: 1055 quests without game objects.

Game objects (`GOBJ`, `GOSP`, `GLOT`): buttons, quest givers (wanted posters, books), chests (herbs and ore too),
decorations, campfires and anvils, texts and quest objects of the map, within 100 yd of the player. Chests are
looted (quest items only while a quest wants them) and come back after their respawn time; objects a quest wants
used give the credit and sparkle (GO_DYNFLAG_LO_ACTIVATE); quests given and taken by objects work like NPCs'.
Herbs and ore ask for the profession (the client's lock check), as in the original. Eastern Kingdoms: 22 487 objects,
1285 quests with them.

Death and home (`GRAV`, net/offline/offline_life.cpp): "Release Spirit" takes the ghost to the graveyard of the zone
it died in (`game_graveyard_zone` + `world_safe_locs`, the nearest its team may use; the client reports its zone with
CMSG_ZONEUPDATE as the original does); the body stays where it fell. Reclaim it within 39 yd after 30 s, or talk to the
spirit healer (only ghosts see it): resurrection sickness from level 11. Innkeepers: "Make this inn your home."; every
character carries the Hearthstone (10 s cast, broken by moving, 1 hour cooldown); home and cooldown are saved.
No durability loss (the offline core has no durability).
Rested experience (`TAVN`: areatrigger_tavern; the shapes are the client's areatriggers.txt): resting in an inn or a
capital (the portrait's zZ) fills the pool by 5 % of a level per 8 hours, time away by the same in an inn or a capital,
a quarter elsewhere; a kill gives double XP from the pool (at most 1.5 levels of kills), "(+N exp Rested bonus)".

Flight masters (net/offline/offline_taxi.cpp): the client's taxi.txt (TaxiNodes, TaxiPath) and transports.txt (every
TaxiPathNode point, matched to a path by its ends). Talking to one discovers its point and opens the map; a flight costs
its legs (several legs: ACTIVATETAXIEXPRESS), mounts the character (gryphon / wind rider / bat), flies the real path at
32 yd/s as a flying spline, creatures leave it alone. A new character knows its race's capital. Without the two files
flight masters only talk.

Bank and mail (net/offline/offline_bank_mail.cpp): bankers open the bank's 24 slots (stacks join; no bank bags: there
are no bags offline). Mailboxes (game object type 19, now in the pack): letters between the characters of the save
(one account: they arrive at once), an item and / or money, postage 30 c; not to yourself, not to the other faction, no
Hearthstone. After 30 days a letter's item or money goes back to its sender. "You have new mail." at login.

Items (net/offline/offline_items.cpp, the pack's `ITX1` from spell_template = Spell.dbc): food and drink over their
duration while sitting (moving or a hit ends it, not in combat), potions at once, shared category cooldowns. The player
regenerates every 2 s (about 3 % out of combat). Durability: -10 % at death by a creature, -25 % more at the spirit
healer, a point at 0.5 % per hit; broken items give nothing; repairs at NPCs with the repair flag (costs from the
client's DurabilityCosts / DurabilityQuality: durability.txt from the converter). Bags: four bag slots (19..22), the
backpack grows by their slots (no moving items between bags yet, no bank bags). Exploration: quests completed at their
area trigger (`QTRG`), exploration XP for a new area (`EXPL`; the area's level and bit are the converter's glue.txt). Eastern Kingdoms: 51 graveyards, 40 spirit healers, 26 innkeepers.

Creature behaviour (`MOVE`, `CRAI`, `AITX`, net/offline/offline_ai.cpp), as CMaNGOS runs the classic-db data:
- movement: `MovementType` 1 wanders at walking speed (SpeedWalk) within `spawndist` of its spawn, one to three moves
  and then a 3-10 s pause (WanderMovementGenerator); `MovementType` 2 walks its path (`creature_movement` by spawn,
  `creature_movement_template` by entry) and waits each point's WaitTime. A move goes to the client once, as one walking
  spline to its end. Eastern Kingdoms: 16 000 wanderers, 1 009 patrols (25 000 points).
- evade: the creature remembers where the fight began; with no one left to fight, or when it leashes (not hurt for its
  Pursuit, 15 s, and its enemy more than 30 yd from where it was last hurt; or past its Leash from the start) it runs
  back there, cannot be hurt on the way and is whole again when it arrives (CMaNGOS CombatManager).
- assistance: 1.5 s after a creature is pulled, the creatures of its faction within 5 yd (or its CallForHelp) that are
  not fighting and are hostile to the enemy join in (CallAssistance / CanAssistTo; neutral ones do not).
- EventAI (`creature_ai_scripts`, 2 100 events of 1 000 creatures here): timers in and out of combat, health, mana,
  range, target health, target casting, a hurt friend, a missing aura, aggro, kill, death, evade, spawn and reached home;
  the actions say / yell / text emote (broadcast_text, e.g. "attempts to run away in fear!"), animations, cast (the
  creature spells of the subset: direct damage, weapon strikes, heals, damage over time, auras shown on their target),
  phases, combat movement, ranged mode, evade, flee for help (30 yd, 10 s) and call for help. A hit pushes a creature's
  cast back (500 ms, twice) instead of breaking it. Left out: summons, scripts, despawns and unit flags.
- control auras (`SPFX`: the spell's auras 12 stun, 26 root, 33 slow, 7 fear, 27 silence, 5 confuse, 25 / 60 pacify,
  for their SpellDuration): a stunned unit does nothing (the client: UNIT_FLAG_STUNNED, held by SMSG_FORCE_MOVE_ROOT),
  a rooted one cannot move, a slowed one runs slower (the player: SMSG_FORCE_RUN_SPEED_CHANGE), a silenced one casts
  no spells, a pacified one does not strike; a feared / confused creature runs about. A feared player is held like a
  stunned one (the server does not run the player yet).

No companions: the upstream core gives the player four AI party members; WoWVita leaves them out. An empty
`ux0:data/wowvita/offline_party.txt` brings them back.

Still missing (the offline core's own limits): trainers, talents, stat auras (armour, damage), pathfinding, other maps. The start is Northshire Abbey.

Tests: `net/tests/run_offline_grid.sh offline.w3do`, `net/tests/run_offline_life.sh offline.w3do`.
