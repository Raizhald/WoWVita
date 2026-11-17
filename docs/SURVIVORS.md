# Survivors mode (prototype)

Main menu > **Survivors**: an offline run in the zone of the offline pack. The player only moves; spells fire on
their own; creatures of the zone come in growing waves; every level-up pauses the run with three upgrade cards;
a boss (the zone's strongest creature, x2 size, elite) arrives at 10:00. Killing it wins the run, dying ends it.

Controls: move as usual. Cards: D-pad left / right + Cross (or tap a card twice). Results: Cross = Try again,
Circle = Main menu. START > Characters also returns to the main menu.

## Code

| File | What |
|---|---|
| `net/offline/survivors.hpp/.cpp` | the director: waves, spells, cards, boss, results; balance numbers |
| `game/source/survivors_ui.hpp/.cpp` | timer, XP strip, boss bar, cards, results |
| `net/tests/survivors_test.cpp` | headless test of whole runs through the real client session + benchmark |

Small hooks in the shared files: `World::spawnAt / despawn / holdClock`, `Entity::swarm` (always hunts the player,
no evade, no respawn), events `Spawned / Despawned` (create / destroy packets), `kMaxEntities` 56 -> 96 (static
spawns stay at 51), solo sessions without companions, the director called around `World::step` in
`OfflineSession::update`, the Survivors button and `LoginFlow::startSurvivors`.

Survivors uses its own profile: `ux0:data/wowvita/offline/survivors.w3d` (the normal offline save is untouched).

## Spells and upgrades

Weapons (real 1.12 spell ids, so the client plays their visuals): Fireball 133, Frost Nova 122, Arcane Explosion
1449, Frostbolt 116, Fire Blast 2136. Passives: Vitality, Swiftness, Spell Power, Haste, Reach, Regeneration.
Max level 5 each; a Healing Potion card when everything is maxed.

## Tuning without a rebuild

`ux0:data/wowvita/survivors.txt`, `key=value` lines:

```
boss_at_sec=600
max_alive=40
start_alive=8
alive_per_minute=4
hp_per_minute=0.30
damage_per_minute=0.12
spawn_radius=26
swarm_speed=0.80
player_health=200
player_armor=300
seed=0
```

Every second the perf log (`perf_events.txt`) gets a line `survivors: time, alive, level, kills, phase`.

## Test

```
# headless (sample world or an exported pack)
g++ -std=c++20 -O1 -I net <net sources from net/tests/run_offline.sh> net/offline/survivors.cpp \
    net/tests/survivors_test.cpp -o survivors_test && ./survivors_test [offline.w3do]
# PC client straight into a run
echo survivors > run/sdmc/wowvita/offline_start.txt
```

## Next steps

- measure on the Vita with 40 / 60 / 90 creatures, set `max_alive`
- balance (the PC test bot kites and wins at ~4 min with the boss at 2:00)
- a mage-looking character for the run, top-down camera option, more zones / bosses
