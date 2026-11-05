# Stav portu na PS Vita (5. 11. 2026)

## Hotovo
- `net/` funguje na Vitě (větev `__vita__` v `socket.cpp`, `crypto.cpp`); PC testy sítě prošly (15× PASS).
- `vita/w3dlogin/` – síťový test jako VPK (bez grafiky).
- `vita/compat/` – náhrada libctru + citro3d:
  - `src/c3d_gl.cpp` – citro3d na OpenGL ES 2 (uniformy, TEV → fragment shader, textury PICA → GL, render targety, skládání obrazovek)
  - `tools/pica2glsl.py` – shadery .v.pica → GLSL + Cg
  - `src/ctru_common.cpp` – čas, vlákna, zámky, paměť, stuby gfx/apt (oprava `threadJoin`/`threadFree` zkompilovaná a otestovaná)
  - `src/console.cpp` – textová konzole libctru (40×30 znaků, font 8×8 z `tools/gen_console_font.py`) na dotykové obrazovce;
    `printf`/`puts`/`putchar`/`vprintf` jsou přesměrované přes `-Wl,--wrap`. Konzoli ukazuje skládání od `consoleInit`/`consoleClear`,
    dokud hra znovu nenakreslí GPU cíl dotykové obrazovky (stejné předávání jako na 3DS) → ladicí/offline stránka je vidět.
  - `src/ndsp.cpp` – softwarový mixer místo DSP 3DS
  - `src/paths.cpp` – sdmc:/3ds/… → ux0:data/…, romfs:/ → app0:/
  - `src/layout.cpp` – horní obrazovka 640×384 vlevo, dotyková 320×240 vpravo
  - `src/platform_pc.cpp` – PC test (headless EGL, skriptovaný vstup, snímky do .ppm)
  - `src/platform_vita.cpp` – **napsaný, zatím nepřeložený VitaSDK** (jen kontrola syntaxe proti náhradním hlavičkám):
    vitaGL init/swap, takty CPU/GPU, SceCtrl (levá páčka → Circle Pad, pravá → C-Stick, Kříž/Kolečko → A/B podle systémového
    tlačítka potvrzení, Trojúhelník → X, Čtverec → Y), zadní panel → ZL/ZR, přední dotyk → dotyková obrazovka, dva prsty →
    přepnutí rozvržení, SceAudioOut (BGM port 48 kHz, vlákno), IME dialog místo swkbd (přes poslední snímek hry, heslo skryté),
    vypnutí automatického uspání, heap newlib 280 MB (`-DW3D_VITA_HEAP_MB=…`).
- `vita/sce_sys/` – ikona, LiveArea (8bit PNG s paletou, `vita/tools/gen_sce_sys.py`), `template.xml`.
- `vita/CMakeLists.txt` – build celého klienta (VPK nebo PC test); VPK bere `vita/synth.w3dp` (formát 5).
- Jediná změna kódu hry: `proto3ds/source/portrait.cpp` (readback pod `W3D_COMPAT`).

## Ověřeno na PC (Mesa llvmpipe)
- Login obrazovka: horní i dotyková obrazovka se kreslí správně.
- Offline (klepnutí na „Offline“): 3D svět – terén, stromy, voda, mlha, obloha; pohyb Circle Padem funguje;
  na dotykové obrazovce ladicí konzole.

## Chybí
1. Přeložit s VitaSDK a opravit případné odchylky API v `platform_vita.cpp` (psáno podle hlaviček VitaSDK zpaměti).
2. Otestovat na Vitě: start, login, IME, zvuk, výkon, paměť (velikost heapu vs. vitaGL).
3. `proto3ds/romfs/synth.w3dp` je stále stará verze formátu (3) – pro 3DS build ji nahradit novou.

## Nastavení na novém PC
- VitaSDK: https://vitasdk.org (vdpm), plus balíčky `vitaGL vitaShaRK mathneon SceShaccCgExt taihen`
  (`vdpm vitaGL` atd., nebo vitaGL zkompilovat s `make HAVE_GLSL_SUPPORT=1`).
- PC test: Mesa (libEGL, libGLESv2) + hlavičky EGL/GLES2 (balíček `libegl-dev libgles-dev`, nebo z Khronos registry).
  Bez nich stačí minimální hlavičky v `KHR_INCLUDE` (deklarace funkcí a konstant, které vrstva používá).

```
# PC test
cmake -S vita -B build-pc -DW3D_PC=1 -DKHR_INCLUDE=/usr/include && cmake --build build-pc -j
mkdir -p run/sdmc run/romfs && cp vita/synth.w3dp run/romfs/
# login obrazovka
EGL_PLATFORM=surfaceless W3D_SDMC=run/sdmc W3D_ROMFS=run/romfs W3D_FRAMES=300 \
  W3D_SHOTS=2,60,150,300 W3D_SHOT_DIR=run ./build-pc/w3dvita_pc
# offline svět + pohyb
EGL_PLATFORM=surfaceless W3D_SDMC=run/sdmc W3D_ROMFS=run/romfs W3D_FRAMES=500 W3D_SHOTS=250,500 W3D_SHOT_DIR=run \
  W3D_TOUCH="30:57,183;34:-" W3D_INPUT="260:CUP;400:CLEFT;460:-" ./build-pc/w3dvita_pc

# Vita
cmake -S vita -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake && cmake --build build-vita
# -> build-vita/WoW3DS.vpk
```
