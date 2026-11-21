#!/bin/bash
# Offline-mode PC tests. No console, no game data, no server, no network.
#
#   net/tests/run_offline.sh
#
# Builds the offline core with the real client sources, exports the sample .w3do pack, runs the
# end-to-end test (OfflineSession -> the existing GameSession), generates teacher data, trains the
# INT8 policy, re-runs the test with bit-exact inference vectors, plays the M3 fight and benchmarks
# the utility AI against the MLP.
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$N/.." && pwd)
OUT=${OUT:-$ROOT/build/offline}
CXX=${CXX:-g++}
PY=${PYTHON:-python3}
command -v "$PY" >/dev/null 2>&1 || PY=python
mkdir -p "$OUT"

EXE=""
LIBS=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXE=".exe"; LIBS="-lws2_32 -lbcrypt" ;;
esac

# Native Windows GCC needs a Windows-style temp directory; MSYS's /tmp is not accepted and GCC
# falls back to C:\WINDOWS (permission denied). Point it at the user's temp folder instead.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        win_tmp="C:/Temp"
        mkdir -p /c/Temp 2>/dev/null || win_tmp=$(cygpath -m /tmp 2>/dev/null || echo "C:/Temp")
        export TMPDIR="$win_tmp" TMP="$win_tmp" TEMP="$win_tmp"
        ;;
esac

SRC="$N/client.cpp $N/crypto.cpp $N/socket.cpp $N/inflate.cpp $N/movement.cpp $N/objects.cpp $N/transport.cpp \
$N/game.cpp $N/game_social.cpp $N/game_spells.cpp $N/game_items.cpp $N/game_life.cpp $N/result_text.cpp \
$N/factions.cpp $N/game_factions.cpp $N/game_vendor.cpp $N/game_quests.cpp $N/game_group.cpp $N/game_pet.cpp \
$N/game_friends.cpp $N/game_services.cpp $N/game_mail.cpp $N/game_auction.cpp $N/game_trade.cpp $N/game_water.cpp \
$N/tables.cpp $N/offline/offline_data.cpp $N/offline/offline_world.cpp $N/offline/offline_bots.cpp \
$N/offline/offline_nn.cpp $N/offline/offline_save.cpp $N/offline/offline_server.cpp $N/offline/offline_quests.cpp \
$N/offline/offline_life.cpp"

echo "== building the offline test and tools"
$CXX -std=c++20 -O1 -g -Wall -Wextra -I "$N" "$N/tests/offline_test.cpp" $SRC -o "$OUT/offline_test$EXE" $LIBS
$CXX -std=c++20 -O2 -Wall -Wextra -I "$N" "$N/tools/w3doffline.cpp" $SRC -o "$OUT/w3doffline$EXE" $LIBS

echo "== exporting the sample world pack"
"$PY" "$ROOT/tools/offline_data/export_offline_data.py" --sample "$OUT/offline_sample.w3do"
"$PY" "$ROOT/tools/offline_data/export_offline_data.py" --verify "$OUT/offline_sample.w3do"
"$PY" "$ROOT/tools/offline_data/export_offline_data.py" --sample "$OUT/offline_sample_c.w3do" --compress
"$PY" "$ROOT/tools/offline_data/export_offline_data.py" --verify "$OUT/offline_sample_c.w3do"

echo "== data check and M3 fight"
"$OUT/w3doffline$EXE" --check "$OUT/offline_sample.w3do"
"$OUT/w3doffline$EXE" --check "$OUT/offline_sample_c.w3do"
"$OUT/w3doffline$EXE" --fight "$OUT/offline_sample.w3do"

echo "== imitation-learning dataset from the deterministic teacher"
"$OUT/w3doffline$EXE" --gen-data "$OUT/teacher.csv" 60 "$OUT/offline_sample.w3do"

echo "== training the INT8 policy"
( cd "$ROOT/tools/offline_train" && "$PY" train_policy.py --data "$OUT/teacher.csv" --out "$OUT/party.w3nn" --epochs 40 )
test -s "$OUT/party.w3nn"
test -s "$OUT/party.w3nn.golden"
"$PY" - "$OUT/party.w3nn" "$OUT/party.w3nn.golden" "$ROOT/tools/offline_train" <<'PY'
import sys
sys.path.insert(0, sys.argv[3])
import w3nn
model = w3nn.from_bytes(open(sys.argv[1], "rb").read())
golden = open(sys.argv[2], encoding="utf-8").read()
bad = w3nn.verify_golden(model, golden)
print(f"golden self-check: {bad} mismatches")
sys.exit(1 if bad else 0)
PY

echo "== end-to-end offline session with the trained model"
"$OUT/offline_test$EXE" "$OUT/party.w3nn" "$OUT/offline_sample.w3do"

echo "== benchmark (held-out encounters)"
"$OUT/w3doffline$EXE" --bench "$OUT/party.w3nn" 20 "$OUT/offline_sample.w3do"

echo
echo "OFFLINE TESTS PASSED"
