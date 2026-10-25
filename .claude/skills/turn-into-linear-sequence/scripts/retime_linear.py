"""Retime frames so the move goes at an even speed: each new frame covers an equal share of the move (from
track_feature.py's progress), a frame falling between two source frames being their linear blend.

Usage: python retime_linear.py FRAMES_DIR TRACK_JSON OUT_DIR PREFIX [COUNT]
  COUNT: how many frames to make (default: as many as the source has).
Writes OUT_DIR/PREFIX_0001.png ... (OUT_DIR must not exist yet: an app showing an older sequence may keep
it cached, so a new set goes in a new folder). Prints which source frame each new one came from. Check
the result by tracking the same feature in the new frames (track_feature.py --pattern "PREFIX_*.png").
"""

import glob
import json
import os
import sys

import cv2
import numpy as np

frames_dir, track_json, out_dir, prefix = sys.argv[1:5]
files = sorted(glob.glob(os.path.join(frames_dir, "src_*.png")))
count = int(sys.argv[5]) if len(sys.argv) > 5 else len(files)
if os.path.exists(out_dir):
    sys.exit(out_dir + " exists: give a new folder (an app may have the old frames cached)")
os.makedirs(out_dir)
p = np.array(json.load(open(track_json))["progress"])
n = len(files)
frames = [cv2.imread(f).astype(np.float32) for f in files]
used = []
for j in range(count):
    target = j / (count - 1)
    i = int(np.clip(np.searchsorted(p, target, side="right") - 1, 0, n - 2))
    span = p[i + 1] - p[i]
    frac = 0.0 if span <= 1e-9 else float(np.clip((target - p[i]) / span, 0.0, 1.0))
    if j == count - 1:
        i, frac = int(np.argmax(p >= 1.0)), 0.0
    img = frames[i] * (1.0 - frac) + frames[min(i + 1, n - 1)] * frac
    cv2.imwrite(os.path.join(out_dir, "%s_%04d.png" % (prefix, j + 1)), np.clip(img + 0.5, 0, 255).astype(np.uint8))
    used.append(round(i + 1 + frac, 2))
print("source frame for each new frame:", used)

print(f"{count} frames written to {out_dir}; check them with track_feature.py --pattern \"{prefix}_*.png\"")
