#!/bin/bash
# WoWVita: death, graveyards, inns and the Hearthstone offline (net/tests/offline_life_test.cpp).
#   run_offline_life.sh offline.w3do
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_life}
mkdir -p "$OUT"
SRC=$(ls "$N"/*.cpp "$N"/offline/*.cpp)
g++ -std=c++20 -O1 -Wall -I"$N" "$N/tests/offline_life_test.cpp" $SRC -lz -o "$OUT/offline_life_test"
(cd "$OUT" && ./offline_life_test "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
