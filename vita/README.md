# PS Vita port (work in progress)

The port goes in stages. Done so far:

1. **Network layer on Vita** — `net/` builds for the Vita with VitaSDK. `net/socket.cpp` and `net/crypto.cpp`
   have a `__vita__` branch next to the 3DS and PC ones (newlib sockets over SceNet, NetCtl connection check,
   `SO_NONBLOCK`, `sceKernelGetProcessTimeWide`, `sceKernelGetRandomNumber`). Nothing else in `net/` needed changes.
2. **`vita/w3dlogin/`** — the PC network test client (`net/tools/w3dlogin.cpp`) as a Vita app. It logs into a
   1.12.1 server exactly like the game client will; no graphics yet.

3. **`vita/compat/`** — a libctru + citro3d stand-in so `proto3ds/` builds unchanged: citro3d on OpenGL ES 2 (vitaGL
   on the Vita), PICA shaders translated to GLSL / Cg, NDSP as a software mixer, the touch-screen text console, path
   mapping (`sdmc:/` → `ux0:data/`, `romfs:/` → `app0:/`) and the two screens laid out on the 960x544 display.
   `platform_pc.cpp` runs the whole client headless on a Linux PC (Mesa) with scripted input and frame dumps;
   `platform_vita.cpp` is the Vita side (vitaGL, SceCtrl, SceTouch, SceAudioOut, the IME dialog) — written but not yet
   built with the VitaSDK. See `STATUS.md`.

### Vita controls

| Vita | 3DS |
|---|---|
| left stick / right stick | Circle Pad / C-Stick |
| Cross, Circle (swapped when the system's enter button is Circle) | A, B |
| Triangle, Square | X, Y |
| D-pad, L, R, START, SELECT | the same |
| rear touch panel, left / right half | ZL / ZR |
| front touch on the touch-screen rectangle | the touch screen |
| two fingers on the front panel | switch the layout (side by side / big top screen) |

Next: build and test on hardware, a `--target vita` texture layout in the converter.

## Building

Needs [VitaSDK](https://vitasdk.org) (`VITASDK` set, `$VITASDK/bin` on `PATH`).

```
cmake -S vita/w3dlogin -B build-vita-login
cmake --build build-vita-login -j      # -> build-vita-login/w3dlogin.vpk
```

## Testing the network client on a Vita

1. Install `w3dlogin.vpk` with VitaShell (HENkaku / h-encore / Ensō).
2. Create `ux0:data/wow3ds/w3dlogin.txt`, one argument per line (same arguments as the PC tool), for example:

   ```
   # server[:port]
   192.168.1.10:3724
   MYACCOUNT
   mypassword
   --stay
   30
   ```

   The password stays on your memory card in plain text; use a test account.
3. Make sure the Vita is connected to Wi-Fi, start **WoW3DS net test**. The screen stays black while it runs
   (no graphics yet); the app closes itself when the run ends.
4. Read `ux0:data/wow3ds/w3dlogin.log` in VitaShell. A good run shows the realm list, the character list and
   entering the world.
