#!/usr/bin/env python3
# Builds an HD texture pack from texpack_extract's output (port/tools/texpack_extract.cpp) and/or a
# runtime dump (graphics.ini texdump = 1) with Real-ESRGAN (realesrgan-ncnn-vulkan).
#
#   texpack_upscale.py <extracted folder> <pack folder> --esrgan <realesrgan-ncnn-vulkan> [--models <dir>]
#
# Per texture: the colour under fully transparent texels (often magenta) is replaced by the colour
# of the nearest visible texels, so it cannot bleed into edges; interface graphics (2d, menu,
# common, font, dump subfolders) use the line-art model, scenery the photo model mixed with a
# plain Lanczos upscale to keep some of the grain the photo model smooths away. Output is 4x, which
# the game also uses for mip levels down to the original texture.
# Needs Pillow and NumPy. Already finished textures are skipped, so an interrupted run resumes.

import argparse, os, shutil, subprocess, sys, tempfile
from multiprocessing import Pool

import numpy as np
from PIL import Image

UI = {'2d', 'menu', 'common', 'font', 'dump', 'misc'}
PHOTO_MIX = 0.3             # share of the Lanczos upscale in scenery textures


def fill_transparent(rgba):
    """Push-pull fill: transparent texels take the average colour of the nearest visible ones."""
    a = rgba[..., 3] > 0
    if a.all() or not a.any():
        return rgba
    levels = []
    c = rgba[..., :3].astype(np.float64) * a[..., None]
    w = a.astype(np.float64)
    while True:
        levels.append((c, w))
        h, wd = w.shape
        if h == 1 and wd == 1:
            break
        ph, pw = h + (h & 1), wd + (wd & 1)       # pad to even, then 2x2 sums
        c2 = np.zeros((ph, pw, 3)); c2[:h, :wd] = c
        w2 = np.zeros((ph, pw)); w2[:h, :wd] = w
        c = c2[0::2, 0::2] + c2[1::2, 0::2] + c2[0::2, 1::2] + c2[1::2, 1::2]
        w = w2[0::2, 0::2] + w2[1::2, 0::2] + w2[0::2, 1::2] + w2[1::2, 1::2]
    col = levels[-1][0] / np.maximum(levels[-1][1], 1e-9)[..., None]
    for c, w in reversed(levels[:-1]):
        up = np.repeat(np.repeat(col, 2, 0), 2, 1)[:w.shape[0], :w.shape[1]]
        mine = c / np.maximum(w, 1e-9)[..., None]
        col = np.where((w > 0)[..., None], mine, up)
    out = rgba.copy()
    out[..., :3] = np.where(a[..., None], rgba[..., :3], np.clip(col + 0.5, 0, 255).astype(np.uint8))
    return out


def prep(job):
    src, dst = job
    rgba = np.asarray(Image.open(src).convert('RGBA'))
    Image.fromarray(fill_transparent(rgba)).save(dst)


def post(job):
    src, up, dst, mix = job
    out = Image.open(up).convert('RGBA')
    if mix > 0:
        orig = Image.open(src).convert('RGBA').resize(out.size, Image.LANCZOS)
        o = np.asarray(out).astype(np.float32)
        l = np.asarray(orig).astype(np.float32)
        o[..., :3] = o[..., :3] * (1 - mix) + l[..., :3] * mix
        out = Image.fromarray(np.clip(o + 0.5, 0, 255).astype(np.uint8))
    tmp = dst + '.tmp.png'
    out.save(tmp, optimize=False)
    os.replace(tmp, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--esrgan', required=True)
    ap.add_argument('--models', default=None)
    ap.add_argument('--batch', type=int, default=2000, help='textures per Real-ESRGAN run')
    args = ap.parse_args()
    models = args.models or os.path.join(os.path.dirname(os.path.abspath(args.esrgan)), 'models')

    for sub in sorted(os.listdir(args.src)):
        sdir = os.path.join(args.src, sub)
        if not os.path.isdir(sdir):
            continue
        ui = sub in UI
        model, mix = ('realesrgan-x4plus-anime', 0.0) if ui else ('realesrgan-x4plus', PHOTO_MIX)
        ddir = os.path.join(args.dst, sub)
        os.makedirs(ddir, exist_ok=True)
        todo = [f for f in sorted(os.listdir(sdir))
                if f.endswith('.png') and not os.path.exists(os.path.join(ddir, f))]
        print(f'{sub}: {len(todo)} to upscale ({model})', flush=True)
        for b in range(0, len(todo), args.batch):
            names = todo[b:b + args.batch]
            with tempfile.TemporaryDirectory() as tmp:
                pin, pout = os.path.join(tmp, 'in'), os.path.join(tmp, 'out')
                os.makedirs(pin); os.makedirs(pout)
                with Pool() as pool:
                    pool.map(prep, [(os.path.join(sdir, f), os.path.join(pin, f)) for f in names], chunksize=16)
                r = subprocess.run([args.esrgan, '-i', pin, '-o', pout, '-n', model, '-m', models, '-s', '4', '-f', 'png'],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
                done = [f for f in names if os.path.exists(os.path.join(pout, f))]
                if r.returncode != 0 or len(done) < len(names):
                    print(f'  realesrgan: {len(names) - len(done)} failed (exit {r.returncode}): {r.stderr[-500:]}', flush=True)
                with Pool() as pool:
                    pool.map(post, [(os.path.join(sdir, f), os.path.join(pout, f), os.path.join(ddir, f), mix)
                                    for f in done], chunksize=16)
            print(f'  {min(b + args.batch, len(todo))}/{len(todo)}', flush=True)


if __name__ == '__main__':
    sys.exit(main())
