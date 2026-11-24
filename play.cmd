@echo off
rem SMRPG Recomp: starts the game with sound, controller and keyboard.
rem It needs the extracted game in extracted\ and the program built in build\ (MSYS2 UCRT64).
rem Controls (Switch style, by the button's position):
rem   move: left stick / d-pad; keyboard W A S D or the arrow keys
rem   A (talk, confirm, attack): right button (Xbox B) / K
rem   B (jump, cancel): bottom button (Xbox A) / J
rem   X (items): top button (Xbox Y) / L      Y (special): left button (Xbox X) / I
rem   + (menu): Start / Enter       - : Back / Backspace      L / R: Q / E      ZL / ZR: 1 / 3
rem   Esc or F1: the runtime's menu (its Controls tab changes them); F11 or Alt+Enter: full screen
cd /d "%~dp0"
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "PATH=%MSYS2_ROOT%\ucrt64\bin;%PATH%"
set LARECOMP_INPUT=
set LARECOMP_NO_AUDIO=
build\smrpg-recomp.exe --play extracted
