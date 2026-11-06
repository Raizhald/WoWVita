# WoWVita

**World of Warcraft 1.12.1 (Vanilla) on the PS Vita** - a native Vita client that plays on private servers.

## What you need

- A PS Vita with HENkaku / h-encore / Enso and VitaShell
- Your own **WoW 1.12.1** game files (the `Data` folder with `terrain.MPQ`, `model.MPQ`, ...)
- A Windows PC for converting the game files
- A 1.12.1 private server **without Warden** anticheat (for example Microbot WoW, `logon.microbot-wow.net`)

No game files are included. Do not share your converted files.

## Install

1. Install **`WoWVita.vpk`** with VitaShell.
2. Unpack the converter on your PC and drag your WoW **`Data`** folder onto **`convert-test.cmd`** (Elwynn Forest and
   Northshire, tens of minutes) or **`convert-all.cmd`** (the whole world, hours).
3. Copy everything from the `wowvita` folder it makes (except `log_*.txt`) to **`ux0:data/wowvita/`** on the Vita.
4. Start **WoWVita**, tap the small panel in the corner, enter the server, your account and password, tap **Login**.

## Controls

| | |
|---|---|
| Left stick | move |
| Right stick | camera |
| D-pad | action bar |
| ✕ ○ □ △ | as shown on screen (□ talk / loot / attack, ○ jump); with L2 / R2 on: more action bar slots |
| L / R (held) | target friends / enemies |
| **Tap a creature or NPC** | select it, tap again to interact |
| **L2 / R2** buttons above the bars | switch the action bar set |
| Panel in the bottom right corner | menus: bags, spells, quests, map, chat, settings |
| Two fingers on the screen | change the screen layout |
| START | menu |

## Credits

Based on **[WoW3DS](https://github.com/ponuno/WoW3DS)** by ponuno, used with permission.
The converter uses [WoWee](https://github.com/Kelsidavis/WoWee) (MIT); quest data from Questie (GPL-3.0).

Unofficial fan project, not affiliated with Blizzard Entertainment or Sony. World of Warcraft is a trademark of
Blizzard Entertainment.

**Found a bug?** [Report it here](../../issues/new/choose) - please attach `perf_events.txt` from `ux0:data/wowvita/`.

Building from source: [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).
