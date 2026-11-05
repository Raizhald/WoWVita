# WoWVita – stav a plán (5. 11. 2026)

Fork projektu [WoW3DS](https://github.com/ponuno/WoW3DS) jen pro PS Vitu. Původní větev 3DS je v gitu jako
`upstream-main` (vzdálený repozitář `upstream`); vlastní vývoj jde ve větvi `main`.

**Licence:** WoW3DS nemá licenční soubor – před zveřejněním forku je potřeba svolení autora WoW3DS.

## Co se při forku změnilo
- Odstraněno vše jen pro 3DS: `Makefile` (devkitARM), balení CIA, `tools/armcheck`, starý `romfs/synth.w3dp` (formát 3),
  `heaps.cpp`, `linear_lock.cpp`, `crash.cpp`, zip s vydáním `wow3ds-stage7k.zip`.
- Nová struktura: `proto3ds/` → `game/`, `vita/compat/` → `platform/`, `vita/w3dlogin/` → `netcheck/`, build v kořeni
  (`CMakeLists.txt`), `sce_sys/` a `data/synth.w3dp` v kořeni, původní dokumentace v `docs/upstream/`.
- Název **WoWVita**, Title ID `WOWV00001` (síťový test `WOWV00002`), výstup `WoWVita.vpk`.
- Data na Vitě v `ux0:data/wowvita/` (hra si myslí, že je to `sdmc:/3ds/w3dproto/`).

## Ověřeno na PS Vitě (5. 11. 2026, první build z GitHub Actions)
- VPK se nainstaluje a spustí; login obrazovka v rozvržení „jedna obrazovka“ s náhledem panelu.
- Offline svět: terén, stromy, obloha; ladicí panel. **59,9 FPS**, CPU 16,7 ms, GPU 3,3 ms (velká rezerva GPU).
- Ověřeno: obě páčky, D-pad, dotykový panel (otevření, zavření, tlačítka), mlha, zvuk (testovací tón po startu).
- Zbývá ověřit: IME (klávesnice), přihlášení na server, skutečná herní data (konvertor), paměť.

## Ověřeno na PC (po forku)
- Build PC verze, login obrazovka, offline svět + pohyb, ladicí konzole na dotykové obrazovce.
- Všechna 3 rozvržení obrazovek včetně klepnutí přes rozvržení (`W3D_DTOUCH`).
- Původní testy hry: `run_swim.sh`, `run_slope.sh`, `run_anim.sh` – PASS.
- Konvertor `w3dconv` se přeloží a `--synthetic` vyrobí stejný balík jako `data/synth.w3dp`.

## Hotovo z portu
- `net/` na Vitě (`__vita__` v `socket.cpp`, `crypto.cpp`), `netcheck/` jako VPK.
- `platform/`: citro3d na GLES2/vitaGL, shadery PICA → Cg/GLSL, mixer zvuku, textová konzole, cesty, rozvržení obrazovek,
  vlákna/zámky/paměť; `platform_vita.cpp` (vitaGL, SceCtrl, SceTouch, SceAudioOut, IME) – **zatím nepřeložené VitaSDK**.

## Plán – z 3DS klienta na čistě Vita klienta
1. ✅ **Přeložit s VitaSDK a spustit na konzoli** (GitHub Actions, `.github/workflows/vita.yml`; hotové VPK na větvi
   `builds`). Dál: ověřit ovládání, zvuk, IME, online přihlášení, paměť.
2. **Jedna obrazovka.**
   - ✅ Krok 1 (vrstva `platform/layout.cpp`): rozvržení „jedna obrazovka“, výchozí na Vitě – svět 906×544, dotyková
     obrazovka jako náhled v rohu, klepnutí ji otevře jako panel ×1,5 (480×360), klepnutí mimo zavře. Dva prsty
     přepínají 3 rozvržení. Cíl horní obrazovky se kreslí vždy v 906×544. Ověřeno na PC (`W3D_LAYOUT=2`, `W3D_DTOUCH`).
   - Krok 2 (kód hry): HUD a okna z dotykové obrazovky přímo do světa (akční lišta, mapa, chat), aby panel byl potřeba
     jen pro menu. Postupně z `game_ui*.cpp`, `hud*.cpp`, `main.cpp`.
3. **Nativní vykreslování.** Teď běží přes emulaci citro3d (TEV kombinéry → shadery, převod textur PICA při nahrání).
   Postupně přepsat `render.cpp` a spol. přímo na vitaGL a vrstvu `platform/` zmenšovat.
4. **Konvertor `--target vita`.** Textury rovnou ve formátu a uspořádání pro GPU Vity (bez dlaždicového PICA layoutu),
   vyšší rozlišení textur a LOD – Vita má 512 MB RAM + 128 MB VRAM oproti 3DS.
5. **Ovládání pro Vitu.** Vlastní schéma místo převodu na tlačítka 3DS (dvě páčky = pohyb + kamera, zadní panel).
6. **Větší dohled a detaily** podle výkonu na konzoli.

## Nastavení na novém PC
- VitaSDK: https://vitasdk.org (vdpm), balíčky `vitaGL vitaShaRK mathneon SceShaccCgExt taihen`.
- PC test: Mesa + `libegl-dev libgles-dev` (postup v README.md).
