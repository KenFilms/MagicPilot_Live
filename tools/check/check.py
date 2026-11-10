#!/usr/bin/env python3
"""
Checks the firmware's UI and camera code on this computer, against the references in tools/check/ref.

    python3 tools/check/check.py            compare with the references
    python3 tools/check/check.py --update   take the current results as the references

  screens  every scene of tools/preview/preview.c, drawn and composited by the firmware's own code
  touch    what ui_hit_test finds every 3 px in each of those scenes
  camera   every Bluetooth packet, the change flags and the camera state of each command (camera/driver.c)

The references are a SHA-256 per scene, and the camera test's printout. For a closer look, the results
that last matched (or were taken as the references) stay in tools/check/out/accepted: a changed screen is
written to tools/check/out/changed/<scene>.png as before | after | changed pixels, and the first changed
lines of a touch map are printed. Needs a C compiler (cc) and Python 3, nothing else.
"""

import difflib
import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zlib

W, H = 1280, 720
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
REF = os.path.join(HERE, 'ref')
OUT = os.path.join(HERE, 'out')
ACCEPTED = os.path.join(OUT, 'accepted')
CHANGED = os.path.join(OUT, 'changed')
DARK = bytes(v // 3 for v in range(256))


def run(cmd):
    r = subprocess.run(cmd, capture_output=True)
    err = r.stderr.decode(errors='replace').strip()
    if r.returncode != 0:
        sys.exit(f'{" ".join(cmd)} failed:\n{err}')
    if err:
        print(err)                      # compiler warnings
    return r.stdout


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_ref(name):
    path = os.path.join(REF, name)
    if not os.path.exists(path):
        return {}
    return dict(line.split() for line in open(path) if line.strip())


def write_ref(name, hashes):
    os.makedirs(REF, exist_ok=True)
    with open(os.path.join(REF, name), 'w') as f:
        f.writelines(f'{k} {hashes[k]}\n' for k in sorted(hashes))


def write_png(path, rgb, w, h):
    rows = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def write_change(scene, before, after):
    """before | after | the changed pixels in magenta over the after screen, darkened"""
    os.makedirs(CHANGED, exist_ok=True)
    path = os.path.join(CHANGED, scene + '.png')
    if before is None or len(before) != len(after):
        write_png(path, after, W, H)
        return
    gap = bytes((60, 60, 60)) * 8
    rows = []
    for y in range(H):
        a, b = before[y * W * 3:(y + 1) * W * 3], after[y * W * 3:(y + 1) * W * 3]
        mark = bytearray(b.translate(DARK))
        if a != b:
            for i in range(0, W * 3, 3):
                if a[i:i + 3] != b[i:i + 3]:
                    mark[i:i + 3] = b'\xff\x00\xff'
        rows.append(a + gap + b + gap + bytes(mark))
    write_png(path, b''.join(rows), 3 * W + 2 * len(gap) // 3, H)


class Scenes:
    """Per-scene results against the references, keeping the accepted ones for a closer look"""

    def __init__(self, kind, update):
        self.kind, self.update = kind, update
        self.ref = read_ref(kind + '.txt')
        self.now, self.changed, self.details = {}, [], []
        self.cache = os.path.join(ACCEPTED, kind)
        os.makedirs(self.cache, exist_ok=True)

    def cached(self, scene, digest):
        return os.path.join(self.cache, f'{scene}.{digest[:16]}.gz')

    def add(self, scene, data, show):
        digest = self.now[scene] = sha(data)
        same = digest == self.ref.get(scene)
        if same or self.update:
            path = self.cached(scene, digest)
            if not os.path.exists(path):
                for old in os.listdir(self.cache):
                    if old.startswith(scene + '.'):
                        os.remove(os.path.join(self.cache, old))
                with gzip.open(path, 'wb', compresslevel=1) as f:
                    f.write(data)
        if not same:
            self.changed.append(scene)
            if not self.update:
                old = self.cached(scene, self.ref.get(scene, ''))
                before = gzip.open(old).read() if scene in self.ref and os.path.exists(old) else None
                self.details += show(scene, before, data) or []

    def finish(self, label):
        gone = sorted(set(self.ref) - set(self.now))
        if self.update:
            write_ref(self.kind + '.txt', self.now)
        if not self.changed and not gone:
            print(f'{label} {len(self.now)} scenes, all as before')
            return True
        new = [s for s in self.changed if s not in self.ref]
        diff = [s for s in self.changed if s in self.ref]
        print(f'{label} {len(self.now)} scenes: ' + '; '.join(
            part for part in (f'{len(diff)} changed ({", ".join(diff)})' if diff else '',
                              f'{len(new)} new ({", ".join(new)})' if new else '',
                              f'{len(gone)} gone ({", ".join(gone)})' if gone else '') if part))
        for line in self.details[:12]:
            print(f'         {line}')
        return self.update


def check_screens(update):
    black = os.path.join(OUT, 'black.rgb565')
    if not os.path.exists(black) or os.path.getsize(black) != W * H * 2:
        with open(black, 'wb') as f:
            f.write(bytes(W * H * 2))
    frames = os.path.join(OUT, 'screens')
    shutil.rmtree(frames, ignore_errors=True)
    shutil.rmtree(CHANGED, ignore_errors=True)
    os.makedirs(frames)
    run(['sh', os.path.join(ROOT, 'tools', 'preview', 'build.sh'), black, frames])
    scenes = Scenes('screens', update)
    for name in sorted(os.listdir(frames)):
        if name.endswith('.rgb'):
            with open(os.path.join(frames, name), 'rb') as f:
                scenes.add(name[:-4], f.read(), write_change)
    shutil.rmtree(frames)               # 2.7 MB a screen
    ok = scenes.finish('screens:')
    if scenes.changed and not update:
        print(f'         pictures in {os.path.relpath(CHANGED, ROOT)}')
    return ok


def check_touch(update):
    text = run([os.path.join(ROOT, 'tools', 'preview', 'preview'), '--touch']).decode()
    maps = {}
    for line in text.splitlines():
        maps.setdefault(line.split(' ', 1)[0], []).append(line)

    def show(scene, before, after):
        """The first changed lines: 'scene y x: kind index fraction' where what a touch finds changes"""
        if before is None:
            return []
        old, new = before.decode().splitlines(), after.decode().splitlines()
        diff = difflib.unified_diff(old, new, lineterm='', n=0)
        return [line for line in diff if line[:1] in '+-' and line[:3] not in ('---', '+++')][:6]

    scenes = Scenes('touch', update)
    for scene, lines in maps.items():
        scenes.add(scene, '\n'.join(lines).encode(), show)
    return scenes.finish('touch:  ')


def check_camera(update):
    binary = os.path.join(OUT, 'camera')
    run(['cc', '-std=gnu11', '-Wall', '-I', os.path.join(HERE, 'camera', 'stubs'), '-I', os.path.join(ROOT, 'main'),
         '-o', binary, os.path.join(HERE, 'camera', 'driver.c'), os.path.join(ROOT, 'main', 'cam.c')])
    now = run([binary]).decode()
    path = os.path.join(REF, 'camera.txt')
    ref = open(path).read() if os.path.exists(path) else None
    packets = now.count('  send ')
    if update:
        os.makedirs(REF, exist_ok=True)
        with open(path, 'w') as f:
            f.write(now)
    if now == ref:
        print(f'camera:  {packets} packets, state and change flags all as before')
        return True
    print('camera:  changed' if ref is not None else 'camera:  no reference yet')
    if ref is not None and not update:
        diff = difflib.unified_diff(ref.splitlines(), now.splitlines(), 'reference', 'now', lineterm='', n=1)
        for line in list(diff)[:30]:
            print(f'         {line}')
    return update


def main():
    update = '--update' in sys.argv[1:]
    os.makedirs(OUT, exist_ok=True)
    results = [check_screens(update), check_touch(update), check_camera(update)]
    if update:
        print('References updated')
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
