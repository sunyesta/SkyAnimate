"""Pictures to look at: a contact sheet of frames, or the first frame with a pixel grid (to read a feature's
box from), or before and after side by side.

Usage:
  python sheet.py contact FRAMES_GLOB OUT_PNG [EVERY]      every EVERY-th frame (default 4), numbered
  python sheet.py grid FIRST_FRAME OUT_PNG [STEP]          the frame with grid lines every STEP px (default 50)
  python sheet.py compare BEFORE_GLOB AFTER_GLOB OUT_PNG [EVERY]   two rows, the same frame numbers
"""

import glob
import sys

from PIL import Image, ImageDraw


def thumbs(files, every, w=213):
    pick = list(range(0, len(files), every))
    if pick[-1] != len(files) - 1:
        pick.append(len(files) - 1)
    first = Image.open(files[0])
    h = round(w * first.height / first.width)
    return pick, w, h


mode = sys.argv[1]
if mode == "contact":
    files = sorted(glob.glob(sys.argv[2]))
    pick, w, h = thumbs(files, int(sys.argv[4]) if len(sys.argv) > 4 else 4)
    cols = 8
    rows = (len(pick) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w, rows * (h + 14)), "white")
    d = ImageDraw.Draw(sheet)
    for k, i in enumerate(pick):
        x, y = (k % cols) * w, (k // cols) * (h + 14)
        sheet.paste(Image.open(files[i]).convert("RGB").resize((w, h)), (x, y + 14))
        d.text((x + 2, y), str(i + 1), fill="black")
    sheet.save(sys.argv[3])
elif mode == "grid":
    im = Image.open(sys.argv[2]).convert("RGB")
    step = int(sys.argv[4]) if len(sys.argv) > 4 else 50
    d = ImageDraw.Draw(im)
    for x in range(0, im.width, step):
        d.line([(x, 0), (x, im.height)], fill=(255, 0, 0) if x % (step * 4) == 0 else (255, 160, 160), width=1)
        if x % (step * 2) == 0:
            d.text((x + 2, 2), str(x), fill="red")
    for y in range(0, im.height, step):
        d.line([(0, y), (im.width, y)], fill=(255, 0, 0) if y % (step * 4) == 0 else (255, 160, 160), width=1)
        if y % (step * 2) == 0:
            d.text((2, y + 2), str(y), fill="red")
    im.save(sys.argv[3])
elif mode == "compare":
    before, after = sorted(glob.glob(sys.argv[2])), sorted(glob.glob(sys.argv[3]))
    every = int(sys.argv[5]) if len(sys.argv) > 5 else 8
    pick, w, h = thumbs(before, every, 160)
    sheet = Image.new("RGB", (len(pick) * w, 2 * (h + 16)), "white")
    d = ImageDraw.Draw(sheet)
    for row, (name, files) in enumerate((("before", before), ("after", after))):
        for k, i in enumerate(pick):
            if i < len(files):
                sheet.paste(Image.open(files[i]).convert("RGB").resize((w, h)), (k * w, row * (h + 16) + 16))
                d.text((k * w + 2, row * (h + 16) + 2), f"{name} {i + 1}", fill="black")
    sheet.save(sys.argv[4])
else:
    sys.exit(__doc__)
print("written", sys.argv[-1] if mode != "contact" and mode != "grid" else sys.argv[3])
