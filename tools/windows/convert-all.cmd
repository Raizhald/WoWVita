@echo off
setlocal
rem WoWVita: converts the WHOLE game (interface, sound, every map and dungeon) for the PS Vita.
rem Takes HOURS and several GB.
rem   convert-all.cmd ["<WoW Data folder>"] ["<output folder>"]
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
echo Converting the whole game (this takes hours) - log: "%OUT%\log_all.txt"
w3dconv.exe --data "%DATA%" --all -o "%OUT%" > "%OUT%\log_all.txt" 2>&1 || goto fail
echo.
echo Done. Copy everything in "%OUT%" (except log_*.txt and preview) to ux0:data/wowvita/ on the Vita.
pause
exit /b 0
:fail
echo.
echo The conversion failed. See "%OUT%\log_all.txt".
pause
exit /b 1
