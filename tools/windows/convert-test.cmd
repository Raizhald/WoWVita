@echo off
setlocal
rem WoWVita: converts your own WoW 1.12.1 game files for the PS Vita.
rem   convert-test.cmd ["<WoW Data folder>"] ["<output folder>"]
rem You can also drag your WoW "Data" folder onto this file. Without it you are asked for the path.
set "DATA=%~1"
if "%DATA%"=="" if exist "%~dp0Data\terrain.MPQ" set "DATA=%~dp0Data"
if "%DATA%"=="" set /p "DATA=Path to your WoW 1.12.1 Data folder (e.g. C:\Games\WoW\Data): "
if defined DATA set "DATA=%DATA:"=%"
if not defined DATA set "DATA=none"
set "OUT=%~2"
if "%OUT%"=="" set "OUT=%~dp0wowvita"
cd /d "%~dp0"
if not exist "%DATA%\terrain.MPQ" (
  echo Not found: "%DATA%\terrain.MPQ" - choose the Data folder of a full WoW 1.12.1 client.
  pause
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
echo Game data: %DATA%
echo Output:    %OUT%
echo.
echo [1/3] Interface, icons, zone maps and texts (a few minutes)...
w3dconv.exe --data "%DATA%" --ui -o "%OUT%\ui.w3dp" > "%OUT%\log_ui.txt" 2>&1 || goto fail
echo [2/3] Sound and music (a few minutes)...
w3dconv.exe --data "%DATA%" --sound -o "%OUT%\sound" > "%OUT%\log_sound.txt" 2>&1 || goto fail
echo [3/3] Elwynn Forest and Northshire with creatures and models (tens of minutes)...
w3dconv.exe --data "%DATA%" --map Azeroth --tiles 30 46 34 50 --creatures --players --transports -o "%OUT%\test.w3dp" > "%OUT%\log_map.txt" 2>&1 || goto fail
echo.
echo Done. Copy everything in "%OUT%" (except log_*.txt and preview) to ux0:data/wowvita/ on the Vita.
pause
exit /b 0
:fail
echo.
echo The conversion failed. See the log_*.txt files in "%OUT%".
pause
exit /b 1
