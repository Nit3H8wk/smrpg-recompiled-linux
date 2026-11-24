# tools/zone_tour.py: tests areas of the game without playing up to them. SMRPG's save is unsigned UTF-16 JSON: its
# respawn floor (_respawn_floor_name, the scene's name: d01f1b Bowser's Keep, d02out Mushroom Way...) decides where Mario
# appears when it is loaded. For each scene asked for, it copies the base save of a separate data folder, changes the
# floor, runs tools/run_test.sh (scripted input: skip the movie, the title screen, the menu and load the autosave) and keeps
# the screenshots in build/zones/<scene>_<frame>.png and a contact sheet, build/zones/sheet.png. The logs stay in
# build/zones/<scene>.log.
# Usage (MSYS2, in the repository's folder):
#   python tools/zone_tour.py <separate data folder> <base save> scene [scene ...]
# Example: python tools/zone_tour.py <separate folder> <separate folder>/<base save> d02out d04b1
import json
import os
import re
import shutil
import subprocess
import sys

FRAMES = [2700, 3400]
INPUT = '1300:A:8,1600:A:8,1900:A:8,2200:A:8'


def set_floor(src, dst, floor):
    raw = open(src, 'rb').read()
    text = raw.decode('utf-16-le')
    body = text.rstrip('\x00')
    pad = len(text) - len(body)
    j = json.loads(body)
    j['_respawn_floor_name'] = floor
    out = json.dumps(j, ensure_ascii=False, separators=(',', ':'))
    open(dst, 'wb').write(out.encode('utf-16-le') + b'\x00\x00' * pad)


def wait_closed(secs=30):  # (the previous run takes a while to close: run_test.sh does not start with the game open)
    import time
    for _ in range(secs * 2):
        r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq smrpg-recomp.exe'], capture_output=True, text=True)
        if 'smrpg-recomp' not in r.stdout.lower():
            return
        time.sleep(0.5)


def main():
    root, base = sys.argv[1], sys.argv[2]
    scenes = sys.argv[3:]
    if not scenes:  # (SMRPG: the list from a file: on the command line the paths get split by sh)
        scenes = open(os.environ.get('ZONES_FILE', 'build/zones_list.txt')).read().split()
    root_win = subprocess.run(['cygpath', '-w', os.path.abspath(root)], capture_output=True, text=True).stdout.strip()
    os.makedirs('build/zones', exist_ok=True)
    done_path = os.environ.get('DONE_FILE', 'build/zones_done.txt')  # (SMRPG: resumable; one per instance in the 3-way sweep)
    try:
        done = set(open(done_path).read().split())
    except OSError:
        done = set()
    done_f = open(done_path, 'a')
    shots = []
    for sc in scenes:
        if sc in done:
            print(f'{sc}: already done, skipping', flush=True)
            continue
        save_dir = os.path.join(root, 'save', 'save_0')
        shutil.rmtree(os.path.join(root, 'save'), ignore_errors=True)
        shutil.copytree(base, os.path.join(root, 'save'))
        set_floor(os.path.join(base, 'save_0', 'SMR_Save_Ver0011000'), os.path.join(save_dir, 'SMR_Save_Ver0011000'), sc)
        frames = os.path.join(root, 'build', 'frames')  # (the screenshots land in the data folder, not in the repository)
        for f in os.listdir(frames) if os.path.isdir(frames) else []:
            os.remove(os.path.join(frames, f))
        log = f'build/zones/{sc}.log'
        env_args = [f'LARECOMP_DATA_ROOT={root_win}', 'LARECOMP_HIDDEN=1', 'LARECOMP_NO_AUDIO=1', f'LARECOMP_INPUT={INPUT}',
                    'LARECOMP_SHOT_AT=' + ','.join(str(x) for x in FRAMES)]
        wait_closed()
        r = subprocess.run(['sh', 'tools/run_test.sh', '75', log] + env_args, capture_output=True, text=True)
        crash = ''
        try:
            txt = open(log, 'rb').read().decode('utf-8', 'replace')
            m = re.search(r'\[crash\][^\n]*', txt)
            crash = m.group(0)[:120] if m else ''
            skipped = re.findall(r'(\d+) skipped', txt)
            skipped = skipped[-1] if skipped else '?'
        except OSError:
            skipped = '?'
        got = []
        for fr in FRAMES:
            p = os.path.join(frames, f'frame_{fr:05d}.png')
            if os.path.exists(p):
                q = f'build/zones/{sc}_{fr}.png'
                shutil.copy(p, q)
                got.append(q)
                shots.append((f'{sc} {fr}', q))
        print(f'{sc}: {len(got)} screenshots, skipped {skipped} {crash} {r.stdout.strip()[-60:]}', flush=True)
        done_f.write(sc + '\n')
        done_f.flush()
    if len(shots) > 80:
        print(f'(sheet left out: {len(shots)} screenshots)')
        return
    try:
        from PIL import Image, ImageDraw
        cols = 4
        rows = (len(shots) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * 400, rows * 225), (20, 20, 20))
        d = ImageDraw.Draw(sheet)
        for i, (name, p) in enumerate(shots):
            im = Image.open(p).convert('RGB').resize((400, 225))
            x, y = (i % cols) * 400, (i // cols) * 225
            sheet.paste(im, (x, y))
            d.text((x + 4, y + 4), name, fill=(255, 255, 0))
        sheet.save('build/zones/sheet.png')
    except ImportError:
        pass


main()
