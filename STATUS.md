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
- **Skutečná herní data (6. 11. 2026):** převodník pro Windows (`.github/workflows/converter.yml`, větev `builds-converter`,
  `tools/windows/prevod-test.cmd`) převedl plného klienta 1.12.1 (rozhraní, zvuk, Elwynn Forest + Northshire); na Vitě
  login s pozadím a logem hry, Northshire Abbey, terén, budovy, stromy.
- **Online (6. 11. 2026):** Microbot WoW (dřív NycterMoon, vMaNGOS 1.12.1, `logon.microbot-wow.net`, bez Wardenu):
  přihlášení, výběr realmu, vytvoření postavy (rasa, povolání, vzhled, jméno přes IME), vstup do světa
  v Northshire, pohyb, HUD. Elysium Project (`logon.elysium-project.org`): přihlášení a realmy ano, herní server ne
  (Warden – klient ho neumí a umět nebude).
- Výkon (benchmark Northshire, po VBO): průměr 48,8 FPS, běžný snímek 17 ms (60 FPS); zbývají zaseknutí u nových dlaždic
  (oprava „views“ v `de9d3a4`, na Vitě zatím neměřená) a jednorázový překlad shaderů (obloha, voda, efekty).
- Zbývá: celá hra (`prevod-vse.cmd`), delší hraní online (souboje, questy, chat, paměť).
- **Ovládání (6. 11. 2026):** nápisy tlačítek Vity (✕ ○ △ □, L2/R2) v celém rozhraní; klepnutí na jednotku ve světě
  ji vybere, další klepnutí na vybranou interaguje – na Vitě ověřeno, funguje. Panel se otevírá jen z náhledu v rohu.
- **Rychlejší načítání (slučování čtení + 2. čtecí vlákno, `W3D_IO_FAST`) je VYPNUTÉ:** build s ním se na Vitě zasekl
  při startu. Příčina zatím neznámá – potřeba `perf_prev_events.txt` ze zaseknutého spuštění (podezřelé:
  `thread_local` buffer – už nahrazen, souběh dvou vláken).
- Autorovi WoW3DS odeslána žádost o svolení ke zveřejnění (6. 11. 2026); do odpovědi repozitář soukromý.

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

## Jak pokračovat na jiném místě
- **Repozitář:** soukromý `https://github.com/Raizhald/WoWVita`, větev `main` (vývoj), `builds` (poslední hotová VPK),
  `upstream-main` jen v lokálním gitu (původní WoW3DS, vzdálený repozitář `upstream` = github.com/ponuno/WoW3DS).
- **Překlad VPK:** každý push do `main` spustí GitHub Actions (`.github/workflows/vita.yml`, obraz `vitasdk/vitasdk`).
  Hotové `WoWVita.vpk` a `netcheck.vpk` jsou ve větvi `builds` (`git fetch origin builds` → `git show FETCH_HEAD:WoWVita.vpk`)
  i jako artefakt běhu (Actions → běh → Artifacts). Lokálně: VitaSDK + `cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=...`.
- **PC test bez Vity:** `cmake -S . -B build-pc -DW3D_PC=1 && cmake --build build-pc -j` (bez systémových hlaviček EGL/GLES
  se použijí `platform/tools/khr/include`), spuštění viz README.md. Skriptovaný vstup a snímky obrazovky do `run/`.
- **Další kroky (pořadí):**
  1. Skutečná herní data: plný klient WoW 1.12.1 (`Data/` s `terrain.MPQ`, `model.MPQ`, `texture.MPQ`, …) → `w3dconv`
     (postup v `docs/upstream/README.md`) → obsah do `ux0:data/wowvita/`. Začít jednou oblastí (Elwynn Forest).
  2. Přihlášení na server 1.12.1 (vMaNGOS); vyzkoušet IME klávesnici, `netcheck.vpk` pro samotnou síť.
  3. Paměť na Vitě s velkými daty (`-DW3D_VITA_HEAP_MB`, výchozí 280).
  4. Plán výše: HUD do jedné obrazovky, nativní vykreslování, `--target vita` v konvertoru.
- **Licence:** kód WoW3DS nemá licenci – repozitář nechat soukromý, dokud autor WoW3DS nesvolí.
- **Související projekt:** WoWee PSP port (github.com/SandstoneDev/WoWee-psp-port) – PSP, WoW 3.3.5a, MIT; na Vitě jen
  přes Adrenaline. Jiná verze hry i protokolu než WoWVita.
