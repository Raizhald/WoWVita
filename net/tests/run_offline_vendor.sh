#!/bin/bash
# WoWVita: vendors and wearing items offline (net/tests/offline_vendor_test.cpp). Needs a pack from export_world_sql.py.
#   run_offline_vendor.sh offline.w3do
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_vendor}
mkdir -p "$OUT"
SRC=$(ls "$N"/*.cpp "$N"/offline/*.cpp)
g++ -std=c++20 -O1 -Wall -I"$N" "$N/tests/offline_vendor_test.cpp" $SRC -lz -o "$OUT/offline_vendor_test"
(cd "$OUT" && ./offline_vendor_test "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
