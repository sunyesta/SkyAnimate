"""How far through its move each frame is, from one feature followed across the frames.

Pick a feature with a colour of its own that travels the whole move and isn't hidden or flapping (a nose,
a pupil, a hand against a sleeve; not hair or cloth). Give its box in the first frame: its colour is read
from the middle of the box, and each frame the patch of that colour and about its size nearest its last
place is followed (by its centre). That holds on to a feature that changes shape as it turns, where matching a picture of it or
following the image's motion drifts.

Usage: python track_feature.py FRAMES_DIR X Y W H [OUT_JSON] [--search PIXELS] [--search-y PIXELS]
                                [--tolerance N] [--pattern GLOB]
  X Y W H: the feature's box in the first frame (pixels, top-left corner and size).
  --search: how far it may move between frames (default 60); --search-y: up and down, when that is less (a
  head turn: keep it small so hair or a hat above isn't taken). --tolerance: how far from its colour a pixel
  may be, per channel 0-255 (default 30). --pattern: which files (default src_*.png;
  "PREFIX_*.png" to check a retimed set).
Writes OUT_JSON (default FRAMES_DIR/track.json): {"points": [[x, y], ...], "progress": [0..1 per frame]}.
Progress is how far it is along the way from where it starts to where it settles, smoothed and made to
only go forward, from 0 on the first frame to 1 once it has all but arrived (the last 1.5% of creep, often
just settling, counts as there). Prints the path and how the move is spread over the frames.
"""

import glob
import json
import os
import sys

import cv2
import numpy as np

def option(name, fallback):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else fallback


search = int(option("--search", 60))
search_y = int(option("--search-y", search))
tolerance = int(option("--tolerance", 30))
pattern = option("--pattern", "src_*.png")
args, skip = [], False
for a in sys.argv[1:]:
    if skip:
        skip = False
    elif a in ("--search", "--search-y", "--pattern", "--tolerance"):
        skip = True
    else:
        args.append(a)
frames_dir = args[0]
x, y, w, h = (int(v) for v in args[1:5])
out = args[5] if len(args) > 5 else os.path.join(frames_dir, "track.json")
files = sorted(glob.glob(os.path.join(frames_dir, pattern)))
if not files:
    sys.exit("No %s frames in %s" % (pattern, frames_dir))

images = [cv2.imread(f).astype(np.int16) for f in files]
H, W = images[0].shape[:2]
# The feature's colour: the middle of its box in the first frame. Each frame, the pixels near that colour
# close to where it was make patches; the one nearest its last place (and not much smaller) is it.
inner = images[0][y + h // 4:y + 3 * h // 4, x + w // 4:x + 3 * w // 4].reshape(-1, 3)
colour = np.median(inner, axis=0)


def patches(img, x0, y0, x1, y1):
    near = np.all(np.abs(img[y0:y1, x0:x1] - colour) <= tolerance, axis=2).astype(np.uint8)
    n, _, stats, centres = cv2.connectedComponentsWithStats(near)
    return [(float(centres[i][0] + x0), float(centres[i][1] + y0), int(stats[i, cv2.CC_STAT_AREA])) for i in range(1, n)]


first = [p for p in patches(images[0], x, y, x + w, y + h)]
if not first:
    sys.exit("No patch of the feature's colour in its box: pick a box on a feature with a colour of its own")
cx, cy, area = max(first, key=lambda p: p[2])
points = [(cx, cy)]
lost = 0
for img in images[1:]:
    x0, y0 = int(max(cx - w / 2 - search, 0)), int(max(cy - h / 2 - search_y, 0))
    x1, y1 = int(min(cx + w / 2 + search, W)), int(min(cy + h / 2 + search_y, H))
    # About its size: a much bigger patch is something else of its colour (hair, a shadow), or it merged
    # with one.
    found = [p for p in patches(img, x0, y0, x1, y1) if area * 0.3 <= p[2] <= area * 2.5]
    if found:
        cx, cy, size = min(found, key=lambda p: (p[0] - cx) ** 2 + (p[1] - cy) ** 2)
        area = area * 0.8 + size * 0.2
    else:
        lost += 1
    points.append((cx, cy))
if lost:
    print(f"lost it on {lost} frames (kept its last place there): try another feature, --tolerance or --search")

pts = np.array(points)
k = np.exp(-0.5 * (np.arange(-4, 5) / 1.5) ** 2)
k /= k.sum()
smooth = np.stack([np.convolve(np.pad(pts[:, i], 4, mode="edge"), k, mode="valid") for i in range(2)], axis=1)
# How far along the way from where it starts to where it settles (the last few frames), so its jiggle
# doesn't add up as distance.
start, end = smooth[:3].mean(axis=0), smooth[-6:].mean(axis=0)
way = end - start
length = float(np.linalg.norm(way))
if length < 1.0:
    sys.exit("It hardly moves (%.1f px): pick a feature that travels with the move" % length)
progress = np.clip((smooth - start) @ way / (length * length), 0.0, 1.0)
progress = np.maximum.accumulate(progress)
progress[0] = 0.0
progress = np.minimum(progress / 0.985, 1.0)
json.dump({"points": pts.tolist(), "progress": progress.tolist()}, open(out, "w"))
quarter = [float(np.interp(q, np.linspace(0, 1, len(progress)), progress)) for q in (0.25, 0.5, 0.75)]
print(f"{len(files)} frames tracked; it goes {length:.0f} px from {start.round().tolist()} to {end.round().tolist()}")
print("move done at 1/4, 1/2, 3/4 of the frames: %.0f%% %.0f%% %.0f%% (even would be 25%% 50%% 75%%)" % tuple(100 * q for q in quarter))
print("written", out)
