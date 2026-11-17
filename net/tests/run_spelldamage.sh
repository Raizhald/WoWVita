#!/bin/bash
# 7.21: spell damage, damage-over-time ticks and damage shields give the floating combat numbers the HUD draws.
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_net_spelldamage}
mkdir -p "$OUT"
SRC="$N/client.cpp $N/crypto.cpp $N/socket.cpp $N/inflate.cpp $N/movement.cpp $N/objects.cpp $N/transport.cpp $N/game.cpp $N/game_social.cpp $N/game_spells.cpp $N/game_items.cpp $N/game_life.cpp $N/result_text.cpp $N/factions.cpp $N/game_factions.cpp $N/game_vendor.cpp $N/game_quests.cpp $N/game_group.cpp $N/game_pet.cpp $N/game_friends.cpp $N/game_services.cpp $N/game_mail.cpp $N/game_auction.cpp $N/game_trade.cpp $N/game_water.cpp $N/tables.cpp"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$N" "$N/tests/spelldamage_test.cpp" $SRC -o "$OUT/spelldamage_test"
"$OUT/spelldamage_test"
