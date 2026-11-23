#!/bin/bash
# WoWVita: the offline spawn grid (net/tests/offline_grid_test.cpp).
#   run_offline_grid.sh [pack.w3do]   - with a whole-continent pack (tools/offline_data/export_world_sql.py) it also
#                                       walks Northshire -> Goldshire -> Stormwind -> Westfall; without one, the sample.
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_grid}
mkdir -p "$OUT"
g++ -std=c++20 -O2 -Wall -I"$N" "$N/tests/offline_grid_test.cpp" "$N/offline/offline_world.cpp" "$N/offline/offline_data.cpp" \
    "$N/offline/offline_bots.cpp" "$N/offline/offline_nn.cpp" "$N/offline/offline_ai.cpp" "$N/offline/offline_spells.cpp" "$N/inflate.cpp" -lz -o "$OUT/offline_grid_test"
"$OUT/offline_grid_test" "$@"
