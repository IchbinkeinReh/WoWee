#!/usr/bin/env python3
"""Frame-to-frame change of the picture, at geometric edges and elsewhere.

Reads the bursts WOWEE_BURST_DIR writes (src/core/application.cpp):
<tag>_v<view>_<burst>_<frame>.png, consecutive frames of one RT debug view
(0 the picture, 5 traced sun, 6 point lights, 7 occlusion) and of view 11,
the depth-edge mask (magenta at a geometric edge, green elsewhere).

For a burst with the camera still (the first burst of each view, taken
before the turn starts), every change between two frames is flicker:
  mean   the mean absolute change, 0-255, of the brightest channel
  flick  the share of pixels changing by more than 12 (about 5%)
over the edge pixels of the mask (dilated by one pixel) and over the other
surface pixels.

For a burst while the view turns, the picture moves on its own; the second
difference |f(t+1) - 2 f(t) + f(t-1)| leaves what moves smoothly and keeps
what jumps back and forth. Over all pixels with a surface in the mask.

The top 8% and bottom 22% of the screen (the HUD) are left out.

Usage: rt_flicker.py <dir> [<dir> ...]   (one line per dir, burst, view)
"""

import glob
import os
import re
import sys

import numpy as np
from PIL import Image

THRESH = 12


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def crop(img):
    h = img.shape[0]
    return img[int(h * 0.08):int(h * 0.78)]


def dilate(mask):
    out = mask.copy()
    out[1:, :] |= mask[:-1, :]
    out[:-1, :] |= mask[1:, :]
    out[:, 1:] |= mask[:, :-1]
    out[:, :-1] |= mask[:, 1:]
    return out


def bursts(folder):
    found = {}
    for path in glob.glob(os.path.join(folder, "*.png")):
        m = re.match(r"(.+)_v(\d+)_(\d+)_(\d+)\.png$", os.path.basename(path))
        if not m:
            continue
        key = (int(m.group(3)), int(m.group(2)))
        found.setdefault(key, []).append((int(m.group(4)), path))
    return {k: [p for _, p in sorted(v)] for k, v in sorted(found.items())}


def main(folders):
    print(f"{'run':<22}{'burst':>6}{'view':>5}  {'edge mean':>9} {'edge flick':>10}  "
          f"{'rest mean':>9} {'rest flick':>10}  {'2nd diff':>8} {'2nd>12':>7}")
    for folder in folders:
        b = bursts(folder)
        mask_paths = [p for (i, v), p in b.items() if v == 11]
        if not mask_paths:
            print(f"{folder}: no mask burst (view 11)")
            continue
        m = crop(load(mask_paths[0][0]))
        edge_raw = (m[..., 0] > 200) & (m[..., 1] < 60) & (m[..., 2] > 200)
        surface = edge_raw | ((m[..., 1] > 200) & (m[..., 0] < 60) & (m[..., 2] < 60))
        edge = dilate(edge_raw) & surface
        rest = surface & ~edge
        name = os.path.basename(os.path.normpath(folder))
        for (index, view), paths in b.items():
            if view == 11 or len(paths) < 3:
                continue
            frames = [crop(load(p)) for p in paths]
            # The brightest channel's change per pixel and frame pair.
            d = np.stack([np.abs(frames[i + 1] - frames[i]).max(axis=2) for i in range(len(frames) - 1)])
            d2 = np.stack([np.abs(frames[i + 1] - 2 * frames[i] + frames[i - 1]).max(axis=2)
                           for i in range(1, len(frames) - 1)])
            first = index == min(i for (i, v) in b if v == view)
            if first:
                em, ef = d[:, edge].mean(), (d[:, edge] > THRESH).mean() * 100
                rm, rf = d[:, rest].mean(), (d[:, rest] > THRESH).mean() * 100
                print(f"{name:<22}{index:>6}{view:>5}  {em:9.2f} {ef:9.2f}%  {rm:9.2f} {rf:9.2f}%  "
                      f"{d2[:, surface].mean():8.2f} {(d2[:, surface] > THRESH).mean() * 100:6.2f}%")
            else:
                print(f"{name:<22}{index:>6}{view:>5}  {'(moving)':>9} {'':>10}  {'':>9} {'':>10}  "
                      f"{d2[:, surface].mean():8.2f} {(d2[:, surface] > THRESH).mean() * 100:6.2f}%")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1:])
