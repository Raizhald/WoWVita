# Offline world data exporter

`export_offline_data.py` builds the immutable `.w3do` pack that the console's OfflineWorldCore
loads. It runs on the PC next to the normal `w3dconv` conversion; it does not read MPQ archives,
because creature spawns, loot and server-side spell rules are not in the client's files.

## Quick start (sample, no database)

```bash
python tools/offline_data/export_offline_data.py --sample offline.w3do
python tools/offline_data/export_offline_data.py --verify offline.w3do
```

Copy `offline.w3do` to `sdmc:/3ds/w3dproto/offline.w3do`. Without it the 3DS build uses the compiled-in
Northshire sample.

A ready-made sample is checked in as `tools/offline_data/sample/northshire.w3do` (the same dataset the
compiled-in fallback and the automated tests use, 3 088 bytes).

## From a Vanilla server database

Put the `.sql` dumps (or a subset with the world data) in one directory:

```bash
python tools/offline_data/export_offline_data.py \
  --sql /path/to/sql --map 0 --tiles 30 46 34 50 \
  --out offline.w3do --compress --report offline_report.txt
```

`--tiles` is `x0 y0 x1 y1` on the 64×64 tile grid (the default region covers Northshire). Omit it to
write every spawn of the map. `--compress` stores every section as a zlib stream (the 3DS reads it
with its existing inflate code).

Supported tables and column aliases are listed in `docs/OFFLINE_DATA_FORMAT.md`. The importer matches
columns by name and prints a warning for every value it had to substitute.

## Validation

Every run writes `<out>.report.txt` and exits non-zero if there are referential-integrity errors:

* spawn → creature template,
* creature → loot table,
* loot entry → item,
* creature abilities / companion spells → spell definitions,
* required sections (`INFO`, `CREA`, `SPWN`, `BOTS`), version.

`--verify PACK` runs the same checks on a pack produced by an older exporter or edited by hand.
