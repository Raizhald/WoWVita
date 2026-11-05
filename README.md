# WoWVita

A World of Warcraft **1.12.1 (Vanilla)** client for the **PS Vita**, plus a PC converter that turns *your own* 1.12 game
files into packs the console can stream.

WoWVita is a fork of **[WoW3DS](https://github.com/ponuno/WoW3DS)**, the Nintendo 3DS client. The game code (world
streaming, collision, characters, the network client, the whole game UI) comes from WoW3DS; this fork drops the 3DS
build and carries the client to the Vita. All credit for the client itself goes to the WoW3DS author.

> **Licence.** The WoW3DS code has no licence file, so its author keeps all rights to it. Do not publish this fork
> (or builds of it) without the WoW3DS author's permission. `tools/questdb/questdb.bin` and its script are GPL-3.0
> (derived from Questie), see [`docs/upstream/README.md`](docs/upstream/README.md).

No game data, textures, models or sounds are in this repository. You need your own legally obtained WoW 1.12.1 client
(`Data/*.MPQ`) and a private server that speaks the 1.12 protocol (developed against vMaNGOS-compatible servers).

**Status:** work in progress. The client runs on a Linux PC through the Vita platform layer (login screens, the world,
movement); the Vita build itself has not been built with the VitaSDK or run on a console yet. See [`STATUS.md`](STATUS.md).

## Layout

| Folder | What |
|---|---|
| `game/` | the client (from WoW3DS `proto3ds/`): `source/` and its PC tests `tests/` |
| `net/` | the 1.12.1 network client (login, realm, world session, all game packets) |
| `common/` | the pack format shared by the client and the converter |
| `platform/` | the Vita platform: libctru + citro3d replacement on OpenGL ES 2 / vitaGL (`src/c3d_gl.cpp`), PICA shaders to Cg / GLSL (`tools/pica2glsl.py`), software audio mixer, text console, paths, screen layout; `platform_vita.cpp` (Vita) and `platform_pc.cpp` (headless Linux test) |
| `converter/` | `w3dconv`: your WoW 1.12 data -> `.w3dp` packs |
| `netcheck/` | a network-only test app for the Vita (logs in, writes a log) |
| `sce_sys/` | icon and LiveArea of the VPK (`tools/gen_sce_sys.py`) |
| `data/synth.w3dp` | a small synthetic test region inside the VPK (`w3dconv --synthetic`) |
| `docs/upstream/` | the WoW3DS README (converter usage, features), devlog and perf log |

## Building

### Vita

Needs [VitaSDK](https://vitasdk.org) (`VITASDK` set, `$VITASDK/bin` on `PATH`) and the packages
`vitaGL vitaShaRK mathneon SceShaccCgExt taihen` (`vdpm vitaGL` ...).

```
cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build-vita -j         # -> build-vita/WoWVita.vpk
```

### PC test (Linux, no window)

Needs Mesa (`libEGL`, `libGLESv2`) and the EGL / GLES2 headers (`libegl-dev libgles-dev`).

```
cmake -S . -B build-pc -DW3D_PC=1 -DKHR_INCLUDE=/usr/include && cmake --build build-pc -j
mkdir -p run/sdmc
# tap "Offline", fly forward; frames are written to run/frame_<n>.ppm
EGL_PLATFORM=surfaceless W3D_SDMC=run/sdmc W3D_FRAMES=500 W3D_SHOTS=250,500 W3D_SHOT_DIR=run \
  W3D_TOUCH="30:57,183;34:-" W3D_INPUT="260:CUP;400:-" ./build-pc/wowvita_pc
```

Script variables: `W3D_FRAMES`, `W3D_SHOTS`, `W3D_INPUT` (`frame:A+B;frame:-`), `W3D_TOUCH` (`frame:x,y`),
`W3D_LAYOUT` (0/1), `W3D_ECHO=1` (console text to stdout). See `platform/src/platform_pc.cpp`.

### Converter

```
cmake -S converter -B build-conv -DCMAKE_BUILD_TYPE=Release && cmake --build build-conv -j
```

Usage is described in [`docs/upstream/README.md`](docs/upstream/README.md).

## Installing on the Vita

1. Install `WoWVita.vpk` with VitaShell (HENkaku / h-encore / Ensō).
2. Convert your game data with `w3dconv` and copy the output (the `.w3dp` packs, `ui.w3dp`, `sound/` and the text files
   the converter writes) to **`ux0:data/wowvita/`**. This is the folder the 3DS client calls `sdmc:/3ds/w3dproto/`:
   settings, logs and `questdb.bin` go there too. Without packs the client starts in the built-in test region.
3. Start **WoWVita**, enter the server, account and password (the system keyboard opens on tap).

## Controls

| Vita | 3DS button the game sees |
|---|---|
| left stick / right stick | Circle Pad / C-Stick |
| Cross, Circle (swapped when the system's enter button is Circle) | A, B |
| Triangle, Square | X, Y |
| D-pad, L, R, START, SELECT | the same |
| rear touch panel, left / right half | ZL / ZR |
| front touch on the touch-screen rectangle | the touch screen |
| two fingers on the front panel | switch the layout (side by side / big top screen) |

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Blizzard Entertainment or Sony Interactive
Entertainment. "World of Warcraft" is a trademark of Blizzard Entertainment. Do not share converted packs: they contain
data derived from the game. The converter uses [WoWee](https://github.com/Kelsidavis/WoWee) (MIT). `platform/` contains
declarations from libctru and citro3d (see `platform/LICENSE.libctru.md`, `platform/LICENSE.citro3d`).
