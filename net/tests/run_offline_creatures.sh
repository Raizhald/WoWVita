#!/bin/bash
# WoWVita: creature behaviour offline - movement, evade, assistance, EventAI (net/tests/offline_creatures_test.cpp).
#   run_offline_creatures.sh offline.w3do
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_creatures}
mkdir -p "$OUT"
g++ -std=c++20 -O2 -Wall -I"$N" "$N/tests/offline_creatures_test.cpp" "$N/offline/offline_world.cpp" "$N/offline/offline_data.cpp" \
    "$N/offline/offline_bots.cpp" "$N/offline/offline_nn.cpp" "$N/offline/offline_ai.cpp" "$N/offline/offline_spells.cpp" "$N/offline/offline_pet.cpp" "$N/inflate.cpp" -lz -o "$OUT/offline_creatures_test"
"$OUT/offline_creatures_test" "$@"
# and through the session, as the client sees it
SRC=$(ls "$N"/*.cpp "$N"/offline/*.cpp)
g++ -std=c++20 -O1 -Wall -I"$N" "$N/tests/offline_creatures_session_test.cpp" $SRC -lz -o "$OUT/offline_creatures_session_test"
(cd "$OUT" && ./offline_creatures_session_test "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
