SMRPG Recomp @VERSION@
=====================================

Super Mario RPG (Switch, 2023) recompiled for PC from your own copy of the game.

Playing
  The "SMRPG Recomp" shortcut (desktop or Start menu), or smrpg-recomp.exe --play in this folder.
  The port's menu: Esc or F1 (video, sound, controls). With a controller: - and + together.

What is in this folder
  extracted\       the files of your copy of the game and the game code built on your PC (lasgame.dll)
  save\            your saves              larecomp.ini   your settings        cache\   the shaders already prepared
  builder\, gamelib-src\: what the installer uses to build the game code on your PC, inside its own process (the
                   generator in a bundled Python, and clang and lld): there is nothing else to install
  licenses\        the licenses of everything that comes with the program

Updating
  Close the game, run the new installer and choose this same folder both as the installation and as the copy of the
  game (the one it already has, extracted\, works). Your saves and settings stay, and only what changed is compiled
  again.

Uninstalling
  Copy save\ first if you want to keep your saves. Then delete this folder and the shortcuts.

If something goes wrong
  install.log (the installer), build.log (building the game code) and lasrecomp.log (the game).

It needs 64-bit Windows 10 or 11 and a graphics card with Vulkan 1.3. The game code is built for this PC's processor:
with AVX2 (Intel since 2013, AMD since 2015) it runs as intended; without AVX2 it works too, more slowly. If you take
this folder to a PC with another processor, run the installer there again.

Legal notice
  A fan project, not affiliated with or endorsed by Nintendo or Square Enix. Super Mario RPG and Mario are trademarks of
  Nintendo; the game is © Nintendo / SQUARE ENIX. The installer and the program contain nothing of the game: its code
  and data come from your copy. Don't share this folder (extracted\ and any dumped textures belong to the game).
