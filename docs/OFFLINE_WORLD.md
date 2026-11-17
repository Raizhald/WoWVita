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

Still missing (the offline core's own limits): quests, vendors, trainers, talents, creature spells beyond the
documented subset, pathfinding, other maps. The start is Northshire Abbey.

Test: `net/tests/run_offline_grid.sh offline.w3do`.
