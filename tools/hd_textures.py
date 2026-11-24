# tools/hd_textures.py: makes HD textures with AI from the ones the game dumped (Menu > Video > Image > "Dump textures").
# Each PNG in <data>/textures-dump that is a color one (sRGB: "...srgb.png") and not done yet is enlarged x4 with
# Real-ESRGAN (the realesr-animevideov3 model, the one that best suits the game's style: clean edges without made-up
# detail) and reduced to the scale asked for (x2 by default: x4 takes 4 times more video memory) as
# <data>/textures/<hash>_ai_x<N>.png, which the game uses the next time it loads that texture ("HD textures").
# The alpha channel (cutouts, transparency) is enlarged apart, bicubic. Flat textures (few colors: fills, palettes) and
# those under 64 pixels are skipped; the ones that are not color (normals, masks: no "srgb") are left alone.
#
# Real-ESRGAN ncnn Vulkan (BSD-3, xinntao/Real-ESRGAN, portable version realesrgan-ncnn-vulkan-20220424-windows): the
# path of its .exe in the REALESRGAN variable or in --realesrgan.
# Usage: python tools/hd_textures.py <data folder> [--scale 2|3|4] [--realesrgan path.exe] [--redo]
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image


def is_flat(im):
    cols = im.convert('RGB').resize((64, 64)).getcolors(4096)  # (None: more than 4096 colors)
    return cols is not None and len(cols) < 48


def data_dir(data, name, old):
    """<data>/<name>; the Spanish name earlier versions gave it, if only that one is there (as runtime/nvn/vulkan/texpack.c)."""
    p, o = os.path.join(data, name), os.path.join(data, old)
    return o if not os.path.isdir(p) and os.path.isdir(o) else p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('data')
    ap.add_argument('--scale', type=int, default=2, choices=[2, 3, 4])
    ap.add_argument('--realesrgan', default=os.environ.get('REALESRGAN', ''))
    ap.add_argument('--redo', action='store_true')
    a = ap.parse_args()
    src = data_dir(a.data, 'textures-dump', 'texturas-volcadas')
    dst = data_dir(a.data, 'textures', 'texturas')
    if not os.path.isdir(src):
        sys.exit(f'{src} is not there: turn on "Dump textures" in the game\'s menu (Video > Image) and play for a while.')
    exe = a.realesrgan
    if not exe or not os.path.exists(exe):
        sys.exit('Real-ESRGAN is missing: put the path of realesrgan-ncnn-vulkan.exe in the REALESRGAN variable or in --realesrgan.')
    exe = os.path.abspath(exe)
    os.makedirs(dst, exist_ok=True)
    done = {f[:16] for f in os.listdir(dst) if f.lower().endswith('.png')}
    dumped = sorted(f for f in os.listdir(src) if f.lower().endswith('.png'))
    todo = []
    for f in dumped:
        if 'srgb' not in f or (f[:16] in done and not a.redo):
            continue
        im = Image.open(os.path.join(src, f))
        if im.width < 64 or im.height < 64 or is_flat(im):
            continue
        todo.append(f)
    print(f'{len(dumped)} dumped, {len(todo)} to enlarge (x{a.scale}) in {dst}', flush=True)
    if not todo:
        return
    tmp = tempfile.mkdtemp(prefix='hd_textures_')
    try:
        inp, out = os.path.join(tmp, 'in'), os.path.join(tmp, 'out')
        os.makedirs(inp)
        os.makedirs(out)
        for f in todo:
            Image.open(os.path.join(src, f)).convert('RGB').save(os.path.join(inp, f))
        models = os.path.join(os.path.dirname(exe), 'models')
        r = subprocess.run([exe, '-i', inp, '-o', out, '-n', 'realesr-animevideov3', '-s', '4', '-f', 'png', '-m', models],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-800:])
            sys.exit('Real-ESRGAN failed')
        n = 0
        for f in todo:
            p = os.path.join(out, f)
            if not os.path.exists(p):
                print('no output:', f)
                continue
            o = Image.open(os.path.join(src, f))
            w, h = o.width * a.scale, o.height * a.scale
            im = Image.open(p).convert('RGB')
            if im.size != (w, h):
                im = im.resize((w, h), Image.LANCZOS)
            im = im.convert('RGBA')
            if 'A' in o.getbands():
                im.putalpha(o.getchannel('A').resize((w, h), Image.BICUBIC))
            for old in [x for x in os.listdir(dst) if x.startswith(f[:16])]:
                os.remove(os.path.join(dst, old))
            im.save(os.path.join(dst, f'{f[:16]}_ai_x{a.scale}.png'))
            n += 1
            if n % 25 == 0:
                print(f'  {n} / {len(todo)}', flush=True)
        print(f'Done: {n} HD textures. The game uses each one the next time it loads it (better: restart it).')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


main()
