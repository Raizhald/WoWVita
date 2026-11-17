# Server patch: a transport that arrives in a map is never announced there

> **Follow-up SD session (05:00:27):** entry 164871 is absent from the login create burst entirely. Its first
> server create is at 181.5 s, departing. Retention alone cannot fix that case. The old cache put a fake `fff...`
> transport 60 yd from the dock at 180.6 s because it saved each ship's LAST CREATE progress under the snapshot's
> later timestamp, losing different amounts of elapsed time per ship. Its fake GUID was deliberately not boardable.
> The cache now stores progress advanced to a common save time plus each server-observed GUID (`R` lines), ignores
> legacy `T` lines, and restores missing ships only after another real ship validates the snapshot clock within 10 s.
> A server restart / changed GUID / clock mismatch invalidates restored ships. The retained-object fix below remains
> necessary for already-known transports, but was not a complete fix for the missing-at-login case.

> **Diagnosis superseded by the latest SD log.** In `H:\3ds\w3dproto\perf_events.txt` the real zeppelin
> `1fc0000000028407` (entry 164871) is created at 59.3 s and immediately removed by OUT_OF_RANGE. It is created again
> at 249.7 s, already departing (at 250.4 s: map 1, moving, model loaded). Keep an already announced path transport
> across OUT_OF_RANGE, simulate its route using its real GUID, and gate drawing / collision by the pose's map.
> DESTROY and session / world clearing still remove it. No invented GUID or previous-session phase is needed for this case.
> The exact server-side reason for the missing earlier re-announcement is not proven by this log.
> Current upstream vMaNGOS already broadcasts creation from `Map::Add<GenericTransport>()`, called by
> `TeleportTransport()` via `Map::Add<ShipTransport>()`; the proposed extra broadcast below is not an established fix.
> Source: https://github.com/vmangos/core/blob/development/src/game/Maps/Map.cpp
>
> The `NO DECK` warning for display 3031 only means no `M` / `S` rectangle exists. The converter log identifies this
> display as `transport_zeppelin.wmo` with 2911 collision triangles, and boarding can use the WMO's floor owner instead.
> Replaying SD path 302 with speed 30 and acceleration 1 reproduces the logged period of 356175 ms. At progress
> `376592172 - 60000` through `376592172 - 10000` it is stationary at `1318.1 -4658.0 71.9` on map 1; at
> progress `376592172` (the re-create) it is already moving. The path does include the docking interval; losing
> the object, rather than a missing stop in the path, hides that interval in this session.

> **Not applied anywhere.** This is only for a server you admin yourself, or as an upstream PR for vMaNGOS. The public
> server we play on (`play.stonetavern.app`) is not ours; nothing here is sent to it.

## What the 7.27 log shows

One session, standing at the Orgrimmar zeppelin tower (map 1, tile 40_29, ~1324 -4651):

```
[ 32.9 s] online (in the world)
[ 32.9 s] transport create: guid ...b074 entry 176244 display 3015 ...      <- SendInitTransports at login
[ 32.9 s] transport create: guid ...b451 entry 177233 display 3015 ...
[ 32.9 s] transport create: guid ...5148 entry  20808 display 3015 ...
[ 32.9 s] transport create: guid ...b067 entry 176231 display 3015 ...
[ 32.9 s] transport create: guid ...          (9 elevators / other, display 360 and 1587)
[ 61.3 s] transport create: guid ...8407 entry 164871 display 3031 progress 374454380   <- the zeppelin
[ 61.4 s] transport ...8407 entry 164871 display 3031 at 0.0 0.0 0.0, pose map 0 (we are 1), phase 0 of 0
[ 61.5 s] ride: transport ...8407 display 3031 entry 164871 ... (pose 1317.9 -4646.3 71.8, 5 yd)
[ 62.5 s] transport ...8407 entry 164871 display 3031 at 1317.6 -4641.0 71.7, pose map 1 (we are 1), phase 114455 of 356175, 16 yd, moving 1
```

So:

* At login the server sent **every transport that was in map 1** (`Map::SendInitTransports`, 13 objects) - the client
  really does get stopped transports that way.
* The zeppelin `entry 164871` (`path 302`, Orgrimmar <-> Undercity) was **not** among them: at 32.9 s it was in the other
  map / on the way. It entered map 1 at ~61 s.
* Its create block arrives at **61.3 s**, five yards from the player, at the moment it departs - not when it docks.
* The client is not at fault: from 62.5 s on it has `pose map 1`, `phase 114455 of 356175` and puts the model at
  `1317.6 -4641.0 71.7`, i.e. right at the tower (5-16 yd). Path data and the pose simulation are correct.

## Cause

`ShipTransport::Update` and `Map::SendInitTransports` are fine; the gap is `ShipTransport::TeleportTransport()`
(`src/game/Transports/Transport.cpp`):

```cpp
bool ShipTransport::TeleportTransport(uint32 newMapid, float x, float y, float z, float o)
{
    Map const* oldMap = GetMap();
    ...
    GetMap()->Remove<ShipTransport>(this, false);
    SetMap(newMap);          // now in the new map - and nobody there is told
    ...                      // passengers get SMSG_TRANSFER_PENDING + SMSG_NEW_WORLD
}
```

A player standing in map 1 when the zeppelin flies in never receives its create block: `Map::Add(Player*)` already ran,
and the transport is not part of the grid visibility system. It is announced only on the next **update frame** - which
for a zeppelin is its departure. That is exactly the "appears out of thin air and immediately flies away" effect, and
it happens to the PC client as well, not just to the 3DS.

Relogging (or zoning in) while it stands at the stop works, because then `SendInitTransports` includes it - that is why
the same zeppelin often looks fine.

## Fix

`src/game/Transports/Transport.h`, in `GenericTransport` next to the existing senders:

```cpp
    void SendOutOfRangeUpdateToMap();
    void SendCreateUpdateToMap();
    void SendCreateUpdateToAllInMap();   // 7.27: the transport just entered this map
```

`src/game/Transports/Transport.cpp`, after `SendCreateUpdateToMap()`:

```cpp
// 7.27: the transport has just been teleported into this map. The players who were already there, and the passengers
// whose client starts the new world on it, know nothing about it - they get it only at the next update frame, which for
// a ship is its departure.
void GenericTransport::SendCreateUpdateToAllInMap()
{
    Map::PlayerList const& players = GetMap()->GetPlayers();
    for (const auto& player : players)
    {
        Player* p = player.getSource();
        if (!p || p->IsInVisibleList(this))
            continue;   // an earlier create / the initial update already sent it
        SendCreateUpdateToPlayer(p);
    }
}
```

And in `ShipTransport::Update`, the path-change branch:

```cpp
        if (m_currentFrame->Node->mapid != GetMapId() || m_currentFrame->IsTeleportFrame())
        {
            TeleportTransport(m_nextFrame->Node->mapid, m_nextFrame->Node->x, m_nextFrame->Node->y, m_nextFrame->Node->z,
                              m_nextFrame->InitialOrientation);
            SendCreateUpdateToAllInMap();   // 7.27: the map it just entered
            return;
        }
```

`IsInVisibleList()` is public in `Player`, `SendCreateUpdateToPlayer()` is public in `Object`, `GetMap()` returns the new
map after `TeleportTransport`. Nothing else has to change.

Optional hardening: repeat the create every couple of seconds while the transport stands (`!IsMoving()`), for players
who walk into range after the arrival - but with the fix above the object is already in their table and simulated.

## Build and test

```sh
cd <server>
cmake --build build -j
# restart worldserver
```

1. Stand at the zeppelin tower **before** the zeppelin enters the map (e.g. right after it left, or when it is on the
   Undercity side).
2. When it flies in it should appear at once and then stay visible while it stands.
3. `perf_events.txt` should contain a `transport create: ... entry 164871 ...` line long before the `pose ... 5 yd`
   moment, and pose lines with `23 yd`, `5 yd`, `0 yd` while it waits.

If your server is not vMaNGOS, the same one-line idea applies to any CMaNGOS-derived `Transport.cpp`: after the
transport changes map, send its create block to the players of the new map.
