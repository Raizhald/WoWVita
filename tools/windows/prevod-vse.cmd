@echo off
setlocal
rem WoWVita: CELA hra (rozhrani, zvuk, vsechny mapy a dungeony) - trva HODINY a zabere nekolik GB.
rem   prevod-vse.cmd ["<slozka Data hry>"] ["<vystupni slozka>"]
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
echo Prevadim celou hru do "%OUT%" - log: "%OUT%\log_all.txt"
w3dconv.exe --data "%DATA%" --all -o "%OUT%" > "%OUT%\log_all.txt" 2>&1 || goto fail
echo Hotovo. Obsah slozky "%OUT%" (krome log_*.txt a preview) zkopiruj na Vitu do ux0:data/wowvita/
pause
exit /b 0
:fail
echo Prevod selhal. Posli "%OUT%\log_all.txt".
pause
exit /b 1
