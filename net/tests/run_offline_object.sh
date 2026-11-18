#!/bin/bash
# WoWVita: game objects offline (net/tests/offline_object_test.cpp): Milly's Harvest crates, a quest from an object.
#   run_offline_object.sh offline.w3do
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_object}
mkdir -p "$OUT"
SRC=$(ls "$N"/*.cpp "$N"/offline/*.cpp)
g++ -std=c++20 -O1 -Wall -I"$N" "$N/tests/offline_object_test.cpp" $SRC -lz -o "$OUT/offline_object_test"
(cd "$OUT" && ./offline_object_test "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
