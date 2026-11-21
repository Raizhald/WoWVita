#!/bin/bash
# WoWVita: food, drink, potions and exploration offline (net/tests/offline_items_test.cpp).
#   run_offline_items.sh offline.w3do
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_items}
mkdir -p "$OUT"
SRC=$(ls "$N"/*.cpp "$N"/offline/*.cpp)
g++ -std=c++20 -O1 -Wall -I"$N" "$N/tests/offline_items_test.cpp" $SRC -lz -o "$OUT/offline_items_test"
(cd "$OUT" && ./offline_items_test "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
