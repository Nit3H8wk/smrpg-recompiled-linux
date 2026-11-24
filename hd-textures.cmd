@echo off
rem HD textures with AI: enlarges the textures the game dumped (Menu > Video > Image > "Dump textures") and leaves them
rem in the textures folder, which the game uses with "HD textures". It needs Real-ESRGAN ncnn Vulkan in the realesrgan
rem folder (next to this file) or in the REALESRGAN variable, and a Python with Pillow.
rem Options: --scale 2 (default), 3 or 4; --redo (makes the ones already done again).
chcp 65001 >nul
setlocal
set "HERE=%~dp0"
if not defined REALESRGAN set "REALESRGAN=%HERE%realesrgan\realesrgan-ncnn-vulkan.exe"
set PY=
for %%v in (3.12 3.13 3.11 3.14 3) do if not defined PY py -%%v -c "import PIL" >nul 2>&1 && set "PY=py -%%v"
if not defined PY python -c "import PIL" >nul 2>&1 && set "PY=python"
if not defined PY (
  echo There is no Python with Pillow. Install it with:  py -m pip install pillow
  pause
  exit /b 1
)
%PY% "%HERE%tools\hd_textures.py" "%HERE%." %*
pause
