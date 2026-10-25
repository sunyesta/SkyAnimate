"""Every frame of a video, as it is (no resampling), into a folder of PNGs: src_0001.png, src_0002.png, ...

Usage: python extract_frames.py VIDEO OUT_DIR
Prints how many frames and their rate as ffprobe reads them.
"""

import os
import subprocess
import sys

video, out_dir = sys.argv[1], sys.argv[2]
os.makedirs(out_dir, exist_ok=True)
subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", video, "-fps_mode", "passthrough",
                os.path.join(out_dir, "src_%04d.png")], check=True)
probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames", "-show_entries",
                        "stream=r_frame_rate,nb_read_frames,width,height", "-of", "csv=p=0", video],
                       capture_output=True, text=True, check=True).stdout.strip()
count = len([f for f in os.listdir(out_dir) if f.startswith("src_") and f.endswith(".png")])
print(f"{count} frames written to {out_dir} (ffprobe: width,height,rate,frames = {probe})")
