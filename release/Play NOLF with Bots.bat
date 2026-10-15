@echo off
rem Starts No One Lives Forever with Modernizer and the bots.
rem Keep this file in your NOLF folder, next to NOLF.exe.
cd /d "%~dp0"

if not exist "NOLF.exe" (
  echo This file belongs in your No One Lives Forever folder, next to NOLF.exe.
  pause
  exit /b 1
)

set MODERNIZER=
if exist "MODERNIZER.REZ" set MODERNIZER=MODERNIZER.REZ
if exist "Custom\MODERNIZER.REZ" set MODERNIZER=Custom\MODERNIZER.REZ
if "%MODERNIZER%"=="" (
  echo NOLF Modernizer isn't installed. Get it from https://haekb.itch.io/nolf-modernizer
  echo and unpack it into this folder, then start this file again.
  pause
  exit /b 1
)

if not exist "Custom\BOTS.REZ" (
  echo Custom\BOTS.REZ is missing. Unpack the NOLF Bots zip into this folder again.
  pause
  exit /b 1
)

rem The bots load last, so their game code is the one that runs
start "" "NOLF.exe" -rez %MODERNIZER% -rez Custom\BOTS.REZ
