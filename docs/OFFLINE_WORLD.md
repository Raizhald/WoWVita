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
    --map 0 --out offline.w3do
```

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

No companions: the upstream core gives the player four AI party members; WoWVita leaves them out. An empty
`ux0:data/wowvita/offline_party.txt` brings them back.

Still missing (the offline core's own limits): trainers, talents, creature spells beyond the
documented subset, pathfinding, other maps. The start is Northshire Abbey.

Test: `net/tests/run_offline_grid.sh offline.w3do`.
