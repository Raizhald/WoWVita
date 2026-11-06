@echo off
setlocal
rem WoWVita: prevod dat WoW 1.12.1 pro PS Vitu - testovaci oblast (Elwynn Forest + Northshire), rozhrani a zvuk.
rem   prevod-test.cmd ["<slozka Data hry>"] ["<vystupni slozka>"]
rem Bez argumentu: E:\World Of Warcraft Classic\Data -> slozka "wowvita" vedle tohoto souboru.
set "DATA=%~1"
if "%DATA%"=="" set "DATA=E:\World Of Warcraft Classic\Data"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%~dp0wowvita"
cd /d "%~dp0"
if not exist "%DATA%\terrain.MPQ" (
  echo Nenalezeno: "%DATA%\terrain.MPQ" - zadej spravnou slozku Data hry WoW 1.12.1.
  pause
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
echo Data:  %DATA%
echo Vystup: %OUT%
echo.
echo [1/3] Rozhrani, ikony, mapy zon a texty (par minut)...
w3dconv.exe --data "%DATA%" --ui -o "%OUT%\ui.w3dp" > "%OUT%\log_ui.txt" 2>&1 || goto fail
echo [2/3] Zvuk a hudba (par minut)...
w3dconv.exe --data "%DATA%" --sound -o "%OUT%\sound" > "%OUT%\log_sound.txt" 2>&1 || goto fail
echo [3/3] Krajina Elwynn Forest a Northshire s postavami a modely (desitky minut)...
w3dconv.exe --data "%DATA%" --map Azeroth --tiles 30 46 34 50 --creatures --players --transports -o "%OUT%\test.w3dp" > "%OUT%\log_map.txt" 2>&1 || goto fail
echo.
echo Hotovo. Obsah slozky "%OUT%" (krome log_*.txt a preview) zkopiruj na Vitu do ux0:data/wowvita/
pause
exit /b 0
:fail
echo.
echo Prevod selhal. Posli log_*.txt ze slozky "%OUT%".
pause
exit /b 1
