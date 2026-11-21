#!/bin/bash
# WoWVita: flight masters offline (net/tests/offline_taxi_test.cpp).
#   run_offline_taxi.sh offline.w3do
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_offline_taxi}
mkdir -p "$OUT"
SRC=$(ls "$N"/*.cpp "$N"/offline/*.cpp)
g++ -std=c++20 -O1 -Wall -I"$N" "$N/tests/offline_taxi_test.cpp" $SRC -lz -o "$OUT/offline_taxi_test"
(cd "$OUT" && ./offline_taxi_test "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")")
