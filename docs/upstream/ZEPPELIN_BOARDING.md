# Zeppelin passenger registration (SD session 05:36)

The display/cache fixes are not enough: the server must receive a valid passenger MovementInfo.

## Observed SD log

`perf_events.txt` from the session ending at console time 2011-01-06 05:45:

- 30.8 s: entry 164871 restored with its real GUID `1fc0000000028407`, snapshot clock validated.
- 109.0 s: local boarding, offset `-5.0 0.5 -17.9`.
- 214.5 s: the path enters map 0; client applies `3056.4 1764.9 107.1` in map 1 and drops the ride.
- No server transport transfer / NEW_WORLD was logged before that. Re-login leaves the player at the original dock.

## Wire bug

Vanilla MovementInfo has a **full little-endian u64 transport GUID**, followed by four floats.
The packed GUID preceding a relayed movement packet identifies the **mover**, not the transport inside MovementInfo.
The client incorrectly used packed GUIDs for both. Its matching writer/reader unit tests hid the error.

For the observed ship, the packed GUID takes 6 bytes instead of 8. A Vanilla server reads the wrong transport GUID
and shifts all following passenger coordinates by 2 bytes. It cannot register the intended passenger.

Independent server references:

- [MovementInfo::Read / Write](https://github.com/vmangos/core/blob/development/src/game/Objects/Object.cpp)
- [ObjectGuid byte-buffer operators: full u64](https://github.com/vmangos/core/blob/development/src/game/ObjectGuid.cpp)
- [HandleMoverRelocation: transport lookup and AddPassenger](https://github.com/vmangos/core/blob/development/src/game/Handlers/MovementHandler.cpp)

Turtle's incoming packed-GUID/time variant remains separate from Vanilla's layout; it is not a reason to emit a
packed transport GUID in the Vanilla client-to-server movement packets.

## Changes and verification

- Correct outgoing and incoming Vanilla transport GUIDs, including living object create blocks.
- Keep periodic passenger heartbeats even when the player holds no movement keys.
- At a simulated continent boundary, preserve local passenger offsets and wait for the server's map transfer.
  Do not send the other continent's coordinates as movement in the old map or manufacture a disembark.
- On a server-confirmed transport NEW_WORLD, keep the known ship and its phase while clearing other old-map objects;
  resolve the passenger spawn only from a pose belonging to the server's new map.
- `movement_transport_wire_test.cpp` independently reads packets like a Vanilla server (24-byte header, 8-byte GUID,
  16-byte offset, 4-byte fall time). It failed before the fix and passes afterward.
- `transportworld_test.cpp` also checks ship preservation and queued-movement clearing across NEW_WORLD.

Console verification remains necessary: board, wait through a continent change, disembark, and re-login. The server
must issue a transport transfer, and re-login must retain the destination rather than return to the original tower.

## Follow-up: server transfer received, destination never loads (SD session 07:02)

**The phase-correction workaround described in this section has been removed.** The next session proved that
the root cause was a slightly wrong modulo period, not a reason to force the ship onto a guessed route entry.
See the confirmed period diagnosis below. The prohibition on using passenger offsets as world coordinates remains.

Passenger registration now works. In the session ending at 07:13:22, entry 175080 receives transport NEW_WORLD at
341.9 s (map 1) and 497.7 s (map 0). The estimated path is on the opposite map at both times, so requiring the
unadjusted path phase to enter the server's map makes loading wait until an unrelated point of the next cycle.
The 25 s timeout then incorrectly treats the passenger's local offset as world XYZ and drops the ride.

The generated spline's timing is an estimate, not authority over a server-confirmed map transition. CMaNGOS can
also override its generated route period from the server `transports` database; its exact configured period is not
available in these perf logs. See [MapManager::LoadTransports](https://github.com/cmangos/mangos-classic/blob/master/src/game/Entities/Transports.cpp).

On a confirmed passenger NEW_WORLD, reconcile a known ship to the destination map's route entry if its estimated
pose is still on the wrong map. Keep a separate local phase correction, without changing the raw server progress.
Preserve the correction across repeated creates, world clears and the transport cache (`R` rows gain an optional
phase-correction column). Do not apply this correction merely because the local simulation changed continents.
An unresolved passenger transfer now stays pending; its local offset is never used as a world-space timeout fallback.

Replaying SD path 285 with the logged raw progress reproduces both mismatches:

- `386759417`: estimated map 0; reconcile to server map 1 at `1132.9 -5400.9 99.0`.
- `386915296`: estimated map 1; reconcile to server map 0 at `-12465.8 931.3 59.2`.
- Offset `-2.3 -3.1 -17.9` then becomes passenger world position `-12462.5 929.3 41.3`, not a position near the
  world origin. The destination pack can be loaded immediately instead of waiting for the old phase to loop.

Synthetic transport tests cover cross-map reconciliation, impossible destinations, unchanged raw server time,
and phase correction surviving a destination create and snapshot/restore.

## Confirmed root cause: server period != approximated spline duration (SD session ending 07:48:46)

For entry 175080 the generated spline takes **303309 ms**, but the standard Vanilla server `transports` table
specifies **303463 ms**. Taking a multi-day raw server clock modulo the slightly different generated duration
accumulates into several minutes of phase error, enough to render a ship on the wrong continent.
The local boarding at 513.4 s was onto a phantom at Orgrimmar: the corrected server period puts the real ship on map 0.
The later NEW_WORLD to map 1 was therefore the server bringing its passenger back, not a destination-loading failure.

Reference data: [ClassicDB_1_12_1_z2815.sql.gz](https://github.com/cmangos/classic-db/blob/master/Full_DB/ClassicDB_1_12_1_z2815.sql.gz),
the `transports` table. Its periods are separate server data, not inferred by rounding a generated duration.

`transports.txt` now accepts **`P entry serverPeriodMs`**. The converter exports standard Vanilla periods and keeps
existing explicit period overrides for custom servers. The client uses the configured period for modulo, without
stretching the path's stop delays. Unknown/custom entries without a P row retain the generated-period fallback.
The SD file must be updated along with the CIA; reconverting the world packs is not needed.

At a teleport frame the server immediately places the model at the next frame. The small tail between a generated
duration and a longer configured period therefore belongs to the destination, not the previous continent.

Replaying actual SD path 285 with configured period 303463, with NO guessed phase correction:

| Raw server time | Server packet map | Computed map |
| --- | --- | --- |
| 386759417 | 1 | 1 |
| 386915296 | 0 | 0 |
| 389187163 | 1 | 1 |

The obsolete correction column in older R cache rows is ignored. Raw progress and real GUID snapshots remain valid.
Synthetic regression tests now reproduce a 154 ms period mismatch over hundreds of cycles, verify correct map
selection before boarding and after restores/creates, and check the final teleport tail without an early modulo wrap.
