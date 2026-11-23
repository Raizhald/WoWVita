#!/bin/bash
# WoWVita: worn items, bags and the bank's bags offline (net/tests/offline_bags_test.cpp).
#   run_offline_bags.sh offline.w3do
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_bags}
mkdir -p "$OUT"
SRC=$(ls "$N"/*.cpp "$N"/offline/*.cpp)
g++ -std=c++20 -O1 -Wall -I"$N" "$N/tests/offline_bags_test.cpp" $SRC -lz -o "$OUT/offline_bags_test"
(cd "$OUT" && ./offline_bags_test "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
