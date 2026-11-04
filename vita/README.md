# PS Vita port (work in progress)

The port goes in stages. Done so far:

1. **Network layer on Vita** — `net/` builds for the Vita with VitaSDK. `net/socket.cpp` and `net/crypto.cpp`
   have a `__vita__` branch next to the 3DS and PC ones (newlib sockets over SceNet, NetCtl connection check,
   `SO_NONBLOCK`, `sceKernelGetProcessTimeWide`, `sceKernelGetRandomNumber`). Nothing else in `net/` needed changes.
2. **`vita/w3dlogin/`** — the PC network test client (`net/tools/w3dlogin.cpp`) as a Vita app. It logs into a
   1.12.1 server exactly like the game client will; no graphics yet.

Next: a platform layer for `proto3ds/` (graphics, input, audio, time, locks, memory, threads), the renderer on
vitaGL, a `--target vita` texture layout in the converter, then input / audio / UI layout.

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
