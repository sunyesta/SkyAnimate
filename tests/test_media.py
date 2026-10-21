"""Media layers: reading images, image sequences, video and sound; clips and their timing; packing,
moving and finding files; the sound playback mixes.

Run with:  OpenAnimate --background --python tests/test_media.py
"""

import os
import shutil
import tempfile

import oanim

HERE = os.path.dirname(os.path.abspath(__file__))
MEDIA = os.path.join(HERE, "data", "media")
VIDEO = os.path.join(MEDIA, "counting.mp4")    # 64x48, 30 fps, 2 s: frame n is red n*4, blue 255-n*4; a 440 Hz tone
STILL = os.path.join(MEDIA, "still.png")       # 40x20, green
SEQ = os.path.join(MEDIA, "seq_0001.png")      # 3 images, 32x16: image n (from 0) is red n*100
TONE = os.path.join(MEDIA, "tone_then_silence.wav")  # 0.5 s of 1 kHz at half volume, then 0.5 s of silence

doc = oanim.data.document
layers = oanim.data.layers
doc.fps = 24.0


def near(a, b, tol):
    return abs(a - b) <= tol


# --- Reading files ------------------------------------------------------------------------------
v = oanim.media.probe(VIDEO)
assert (v["kind"], v["source_width"], v["source_height"], v["has_audio"]) == ("VIDEO", 64, 48, True), v
assert near(v["fps"], 30.0, 0.01) and near(v["duration"], 2.0, 0.05), v
s = oanim.media.probe(STILL)
assert (s["kind"], s["width"], s["height"]) == ("IMAGE", 40, 20), s
q = oanim.media.probe(SEQ, sequence=True)
assert (q["kind"], q["frame_count"]) == ("SEQUENCE", 3), q
a = oanim.media.probe(TONE)
assert a["kind"] == "AUDIO" and near(a["duration"], 1.0, 0.02), a
missing = oanim.media.probe(os.path.join(MEDIA, "no_such_file.mp4"))
assert missing["kind"] == "NONE" and missing["error"] == "The file is missing", missing

# The exact frame, forwards, then back again (a seek to the keyframe before it).
for seconds, frame in ((0.5, 15), (1.0, 30), (0.2, 6), (1.9, 57)):
    w, h, (r, g, b, alpha), t = oanim.media.pixel(VIDEO, seconds, 32, 24)
    assert (w, h, alpha) == (64, 48, 255), (w, h, alpha)
    # Colours shift a few levels in the video's colour conversion; the frame time is exact.
    assert near(r, frame * 4, 14) and near(b, 255 - frame * 4, 14), (seconds, frame, r, b)
    assert near(t, frame / 30.0, 0.02), (seconds, t)
w, h, (r, g, b, alpha), t = oanim.media.pixel(STILL, 0, 5, 5)
assert (w, h) == (40, 20) and near(g, 0xC0, 3) and near(r, 0x30, 3), (r, g, b)
for n in range(3):
    w, h, (r, g, b, alpha), t = oanim.media.pixel(SEQ, n / 1000.0, 3, 3, sequence=True)
    assert (w, h) == (32, 16) and near(r, n * 100, 3), (n, r)

# --- Add Media ------------------------------------------------------------------------------------
before = len(layers.all())
assert oanim.ops.media.add(filepath=VIDEO) == {'FINISHED'}
video = layers.active
assert video.type == "MEDIA" and video.name == "counting" and len(layers.all()) == before + 1
assert video.media_guide and near(video.opacity, 0.5, 1e-6) and video.media_fit == "FIT", "a guide at half opacity"
(clip,) = video.media_clips
assert (clip["start"], clip["length"], clip["timing"], clip["end"]) == (1, 48, "REAL_TIME", "HOLD"), clip
assert oanim.ops.ed.undo() == {'FINISHED'} and len(layers.all()) == before
assert oanim.ops.ed.redo() == {'FINISHED'}
video = layers.active
cid = video.media_clips[0]["id"]

# Real time: a 30 fps video in a 24 fps scene plays at its own speed; frame f shows (f - 1) / 24 s.
assert video.media_shown(1) == (cid, 0.0)
assert near(video.media_shown(13)[1], 0.5, 1e-6)
assert video.media_shown(0)[1] == 0.0, "the setup frame shows what frame 1 does"
assert video.media_shown(49) is None, "past the clip: nothing"
# After End: Hold shows the last frame, Loop starts again from the In point, Hide shows nothing.
assert oanim.ops.media.clip_set(clip=str(cid), length="96") == {'FINISHED'}
assert near(video.media_shown(60)[1], 2.0, 0.01), video.media_shown(60)
oanim.ops.media.clip_set(clip=str(cid), end="LOOP")
assert near(video.media_shown(49)[1], 0.0, 1e-6) and near(video.media_shown(61)[1], 0.5, 1e-6), video.media_shown(61)
oanim.ops.media.clip_set(clip=str(cid), end="HIDE")
assert video.media_shown(61) is None
oanim.ops.media.clip_set(clip=str(cid), end="HOLD", length="48")
# Frame by frame: one video frame per scene frame.
oanim.ops.media.clip_set(clip=str(cid), timing="FRAME_BY_FRAME")
assert near(video.media_shown(13)[1], 12 / 30.0, 1e-6)
oanim.ops.media.clip_set(clip=str(cid), timing="REAL_TIME")

# Slide, trim and slip.
oanim.ops.media.clip_set(clip=str(cid), start="11")
assert video.media_shown(10) is None and video.media_shown(11) == (cid, 0.0)
oanim.ops.media.clip_set(clip=str(cid), in_point="0.5")
assert near(video.media_shown(11)[1], 0.5, 1e-6), "slipped: the same frames show half a second later in the file"
oanim.ops.media.clip_set(clip=str(cid), start="1", in_point="0")

# Replace from Current Frame: the clip ends there and the new file starts.
doc.frame_current = 20
assert oanim.ops.media.replace_from_frame(filepath=STILL) == {'FINISHED'}
first, second = video.media_clips
assert (first["start"], first["length"]) == (1, 19) and second["start"] == 20 and second["filepath"] == STILL, video.media_clips
assert second["length"] == doc.frame_end - 20 + 1, "a still image runs to the end of the frame range"
assert video.media_shown(25)[0] == second["id"]
# Moving a clip over another overwrites what it lands on.
oanim.ops.media.clip_set(clip=str(second["id"]), start="10")
first, second = video.media_clips
assert first["length"] == 9 and second["start"] == 10, video.media_clips
oanim.ops.media.clip_split(frame="30")
assert len(video.media_clips) == 3 and video.media_clips[2]["start"] == 30
assert oanim.ops.media.clip_delete(clip=str(video.media_clips[2]["id"])) == {'FINISHED'}
assert len(video.media_clips) == 2

# Replace keeps the timing.
doc.frame_current = 5
assert oanim.ops.media.replace(filepath=VIDEO) == {'FINISHED'}
assert video.media_clips[0]["filepath"] == VIDEO and video.media_clips[0]["length"] == 9

# The look, through RNA.
video.media_greyscale = True
video.media_tint_amount = 0.5
video.media_fit = "FILL"
assert (video.media_greyscale, video.media_tint_amount, video.media_fit) == (True, 0.5, "FILL")

# A numbered image is added as a sequence, one image per frame.
assert oanim.ops.media.add(filepath=SEQ) == {'FINISHED'}
seq = layers.active
(sclip,) = seq.media_clips
assert sclip["sequence"] and sclip["length"] == 3 and sclip["timing"] == "FRAME_BY_FRAME", sclip
assert seq.media_shown(2) == (sclip["id"], 1 / 1000.0)
assert oanim.ops.media.add(filepath=SEQ, sequence="0") == {'FINISHED'}
assert not layers.active.media_clips[0]["sequence"]

# --- Sound --------------------------------------------------------------------------------------
for l in list(layers.all()):
    if l.type == "MEDIA":
        l.media_muted = True
assert oanim.ops.media.add(filepath=TONE) == {'FINISHED'}
tone = layers.active
assert near(tone.opacity, 1.0, 1e-6), "sound has no picture to fade"


def peak(seconds, frames=480):
    return max(abs(x) for x in oanim.media.mix(seconds, frames))


start = 1 / 24.0  # the clip starts on frame 1
assert near(peak(start + 0.1), 0.5, 0.05), peak(start + 0.1)
assert peak(start + 0.75) < 0.01, "the file's second half is silent"
assert peak(0.0, 100) < 0.01, "before frame 1 nothing plays"
tone.media_volume = 2.0
assert near(peak(start + 0.1), 1.0, 0.1)
tone.media_volume = 1.0
oanim.ops.media.clip_set(layer=str(tone.id), start="25")
assert peak(start + 0.1) < 0.01 and near(peak(25 / 24.0 + 0.1), 0.5, 0.05), "the sound moves with its clip"
oanim.ops.media.clip_set(layer=str(tone.id), in_point="0.5")
assert peak(25 / 24.0 + 0.1) < 0.01, "slipped half a second in: the silent part"
oanim.ops.media.clip_set(layer=str(tone.id), in_point="0")
tone.media_muted = True
assert peak(25 / 24.0 + 0.1) < 1e-6
tone.media_muted = False
# The video's own sound plays too.
video.media_muted = False
assert peak(0.2) > 0.05, "its first clip (frames 1 to 9)"

# --- Saving: relative paths, packing, missing files -----------------------------------------------
folder = tempfile.mkdtemp(prefix="oanim_media_")
project = os.path.join(folder, "project")
os.makedirs(os.path.join(project, "footage"))
clip_file = os.path.join(project, "footage", "counting.mp4")
shutil.copy(VIDEO, clip_file)
assert oanim.ops.wm.read_homefile() == {'FINISHED'}
doc = oanim.data.document
assert oanim.ops.media.add(filepath=clip_file, frame_start="5") == {'FINISHED'}
saved = os.path.join(project, "shot.oanim")
assert oanim.ops.wm.save_mainfile(filepath=saved) == {'FINISHED'}

# The whole project folder moves: the file is found beside the .oanim, where it was relative to it.
moved = os.path.join(folder, "moved")
shutil.move(project, moved)
assert oanim.ops.wm.open_mainfile(filepath=os.path.join(moved, "shot.oanim"), confirmed="1") == {'FINISHED'}
m = oanim.data.layers.active if oanim.data.layers.active.type == "MEDIA" else [l for l in oanim.data.layers.all() if l.type == "MEDIA"][0]
(c,) = m.media_clips
assert os.path.normcase(c["filepath"]) == os.path.normcase(os.path.join(moved, "footage", "counting.mp4")), c
assert not c["missing"] and c["start"] == 5 and oanim.media.missing_count() == 0

# Missing: renamed away, then Locate finds it (and Pack keeps a copy inside).
os.rename(os.path.join(moved, "footage"), os.path.join(moved, "elsewhere"))
assert oanim.ops.wm.open_mainfile(filepath=os.path.join(moved, "shot.oanim"), confirmed="1") == {'FINISHED'}
m = [l for l in oanim.data.layers.all() if l.type == "MEDIA"][0]
assert m.media_clips[0]["missing"] and oanim.media.missing_count() == 1
oanim.data.layers.active = m
assert oanim.ops.media.locate(filepath=os.path.join(moved, "elsewhere", "counting.mp4")) == {'FINISHED'}
assert oanim.media.missing_count() == 0
assert oanim.ops.media.pack() == {'FINISHED'} and m.media_clips[0]["packed"]
assert oanim.ops.wm.save_mainfile(filepath=os.path.join(moved, "shot.oanim")) == {'FINISHED'}
shutil.rmtree(os.path.join(moved, "elsewhere"))
assert oanim.ops.wm.open_mainfile(filepath=os.path.join(moved, "shot.oanim"), confirmed="1") == {'FINISHED'}
m = [l for l in oanim.data.layers.all() if l.type == "MEDIA"][0]
assert m.media_clips[0]["packed"] and not m.media_clips[0]["missing"], "packed: it opens without the file"
assert m.media_shown(5) is not None
assert oanim.ops.media.unpack() == {'FINISHED'}
written = m.media_clips[0]["filepath"]
assert os.path.isfile(written) and not m.media_clips[0]["packed"], written
assert os.path.normcase(os.path.dirname(written)) == os.path.normcase(os.path.join(moved, "media"))

shutil.rmtree(folder, ignore_errors=True)
print("Media tests passed")
