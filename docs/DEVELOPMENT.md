# WoWVita - development

How the port works and how to build it. Day-to-day state and plans: [`../STATUS.md`](../STATUS.md).

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
`W3D_LAYOUT` (0 side by side, 1 big world, 2 one screen), `W3D_DTOUCH` (`frame:x,y` on the 960x544 display, through the layout like a finger), `W3D_ECHO=1` (console text to stdout). See `platform/src/platform_pc.cpp`.

### Converter

```
cmake -S converter -B build-conv -DCMAKE_BUILD_TYPE=Release && cmake --build build-conv -j
```

Usage is described in [`upstream/README.md`](upstream/README.md).


## How the touch screen is shown


The client was made for the 3DS's two screens: the world on the top screen, the touch menus on the bottom one.

- **One screen** (default): the world fills the display (906x544); the touch screen waits as a preview in the bottom
  right corner. Tap the preview to open it as a large panel (x1.5), tap inside the panel to use it, tap outside to
  close it. A tap on a creature, NPC or player in the world selects it; a tap on the selected one interacts
  (talk, loot, attack) like □.
- **Side by side:** the world 640x384 on the left, the touch screen 1:1 on the right, both always visible.
- **Big world + small touch screen:** the world 906x544 with the touch screen (x0.8) in the corner, tapped directly.


Every push to `main` builds the VPKs on GitHub Actions (`.github/workflows/vita.yml`, branch `builds`) and the Windows
converter (`.github/workflows/converter.yml`, branch `builds-converter`).
