#!/usr/bin/env python3
# Converts a PNG texture pack into BC7-compressed DDS files with their mip chain, which the game
# loads without decoding and keeps compressed on the GPU (a quarter of the memory of BGRA).
#
#   texpack_bc7.py <PNG pack folder> <DDS pack folder> [--originals <texpack_extract output>]
#
# Mip levels go down to the game's own texture size. With --originals (or a runtime dump) the last
# level is the original texture, as the game uses for PNG replacements; distant scenery then looks
# as before. Folder structure and names are kept (.png -> .dds). Needs Pillow, NumPy and etcpak.
# Existing DDS files are skipped, so an interrupted run resumes.

import argparse, os, re, struct, sys
from multiprocessing import Pool

import etcpak
import numpy as np
from PIL import Image

NAME = re.compile(r'ddg_(\d+)x(\d+)_([0-9a-fA-F]{16})\.png$')
DXGI_FORMAT_BC7_UNORM = 98


def dds_header(w, h, levels, level0_size):
    # DDS_HEADER + DDS_HEADER_DXT10 (a 2D texture, one layer)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000     # caps, height, width, pixelformat, mipmapcount, linearsize
    pf = struct.pack('<II4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)
    caps = 0x1000 | (0x8 | 0x400000 if levels > 1 else 0)
    hdr = struct.pack('<7I', 124, flags, h, w, level0_size, 0, levels) + b'\0' * 44 + pf + struct.pack('<4I', caps, 0, 0, 0) + b'\0' * 4
    dx10 = struct.pack('<5I', DXGI_FORMAT_BC7_UNORM, 3, 0, 1, 0)
    return b'DDS ' + hdr + dx10


def bc7(img):
    """RGBA uint8 array -> BC7 blocks; sizes that are not a multiple of 4 are padded by repeating the edge."""
    h, w = img.shape[:2]
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (ph, pw) != (h, w):
        img = np.pad(img, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    return etcpak.compress_bc7(np.ascontiguousarray(img).tobytes(), pw, ph, etcpak.BC7CompressBlockParams())


def convert(job):
    src, dst, orig_path = job
    m = NAME.search(os.path.basename(src))
    ow, oh = int(m.group(1)), int(m.group(2))          # the game's texture size
    hd = Image.open(src).convert('RGBA')
    levels = [hd]
    while levels[-1].width > ow and levels[-1].height > oh and levels[-1].width % 2 == 0 and levels[-1].height % 2 == 0:
        levels.append(levels[-1].reduce(2))
    if orig_path and levels[-1].size == (ow, oh) and len(levels) > 1:
        levels[-1] = Image.open(orig_path).convert('RGBA')
    data = [bc7(np.asarray(l)) for l in levels]
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(dds_header(hd.width, hd.height, len(data), len(data[0])))
        for d in data:
            f.write(d)
    os.replace(tmp, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--originals', help='texpack_extract output or a texdump folder (any depth)')
    args = ap.parse_args()
    originals = {}
    if args.originals:
        for root, _, files in os.walk(args.originals):
            for f in files:
                if NAME.search(f):
                    originals[f] = os.path.join(root, f)
    jobs = []
    for root, _, files in os.walk(args.src):
        for f in files:
            if not NAME.search(f):
                continue
            out = os.path.join(args.dst, os.path.relpath(root, args.src), f[:-4] + '.dds')
            if os.path.exists(out):
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            jobs.append((os.path.join(root, f), out, originals.get(f)))
    print(f'{len(jobs)} textures to convert', flush=True)
    with Pool() as pool:
        for i, _ in enumerate(pool.imap_unordered(convert, jobs, chunksize=8), 1):
            if i % 1000 == 0 or i == len(jobs):
                print(f'  {i}/{len(jobs)}', flush=True)


if __name__ == '__main__':
    sys.exit(main())
