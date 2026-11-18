#!/bin/bash
# WoWVita: quests offline (net/tests/offline_quest_test.cpp) - Northshire's first chain through the real client.
#   run_offline_quest.sh offline.w3do
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_quest}
mkdir -p "$OUT"
SRC=$(ls "$N"/*.cpp "$N"/offline/*.cpp)
g++ -std=c++20 -O1 -Wall -I"$N" "$N/tests/offline_quest_test.cpp" $SRC -lz -o "$OUT/offline_quest_test"
(cd "$OUT" && ./offline_quest_test "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
