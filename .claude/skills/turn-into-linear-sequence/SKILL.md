---
name: turn-into-linear-sequence
description: "Turn this into a linear sequence": retime a reference video (or image sequence) in a Media layer of the user's Open Animate project so its move goes at an even speed, taking out its easing (speeding up into a head turn and slowing out of it), and load the result into that layer as a PNG sequence. Use when the user asks to make a reference linear, even, constant-speed or un-eased, to turn a video into a PNG sequence with linear interpolation, or says the move speeds up and slows down and they don't want that.
---

# Turn this into a linear sequence

Retimes a reference so its motion is even: each new frame covers the same share of the move, a frame that
falls between two source frames being their linear blend. Then the user's Media layer shows the new frames
as a PNG sequence. (Made for HeadTurn in Clay011.oanim: a troll's head turning from front to profile, eased
at both ends; the nose was the feature followed.)

"Linear" here means the motion's spacing, not the file's frame rate: a video already at the document's rate
still eases. Resampling it with frame blending (ffmpeg's `framerate`) changes nothing then; the move itself
has to be measured.

Scripts are in `scripts/` beside this file. Work in the scratchpad; write only the final frames next to the
user's project. Read the open document through remote control (see the open-animate skill: "my file" is the
running app, `%LOCALAPPDATA%\OpenAnimate\remote\<pid>.json`); change it only through `swap_clip.py`, one
undoable step.

## Steps

1. **Find the layer and its file.** In the running app (read only):
   `[(l.name, l.media_clips) for l in oanim.data.layers.all() if l.type == "MEDIA"]`. Note the clip's
   `filepath`, `start`, `length`, and the document's rate (`oanim.data.document.fps`).
2. **Extract every frame as it is:** `python scripts/extract_frames.py VIDEO SCRATCH/frames`
   (no resampling; it prints ffprobe's count and rate). For an image sequence already, copy or link its
   frames as `src_0001.png...`.
3. **Look, and pick what to follow.** `python scripts/sheet.py contact "SCRATCH/frames/src_*.png" sheet.png`
   shows where the move eases; `python scripts/sheet.py grid SCRATCH/frames/src_0001.png grid.png` gives
   pixel positions. Pick a feature with a colour of its own that travels the whole move and isn't hidden
   or flapping: a nose, a pupil, a hand against a sleeve. Not hair, cloth or anything secondary: their
   follow-through makes the ends look like motion that isn't the move. Read its box (x, y, w, h) in frame 1.
4. **Track it:** `python scripts/track_feature.py SCRATCH/frames X Y W H [--search-y 15]`.
   - For a turn that goes sideways, keep `--search-y` small so hair or a hat above isn't taken.
   - It prints where the feature goes and how much of the move is done at 1/4, 1/2 and 3/4 of the frames.
     Eased shows as something like 19% 59% 90%.
   - "lost it on N frames" means pick another feature, or widen `--tolerance`.
   - To check the track, compare it with the contact sheet.
5. **Retime:** `python scripts/retime_linear.py SCRATCH/frames SCRATCH/frames/track.json OUT_DIR PREFIX [COUNT]`.
   - `OUT_DIR` is a new folder next to the .oanim, named for the layer (`HeadTurn_Even`).
   - Never reuse a folder the app has shown: it keeps decoded frames cached.
   - `COUNT` defaults to the source's frame count. Give another only if the user wants it longer or shorter.
6. **Check it:** `python scripts/track_feature.py OUT_DIR X Y W H OUT_DIR/../check.json --pattern "PREFIX_*.png"`
   (same box, same options).
   - Aim for about 25% 50% 75%.
   - Look too: `python scripts/sheet.py compare "SCRATCH/frames/src_*.png" "OUT_DIR/PREFIX_*.png" before_after.png`.
     The face (or hand) should move by even steps, not bunch up in the middle.
7. **Swap it in:** `python scripts/swap_clip.py LAYER_NAME OUT_DIR/PREFIX_0001.png [--pid PID]`.
   - It shows the sequence in place of what the clip showed: same start, length, place and settings, timing
     frame by frame.
   - It's one undo step (Replace Media), and the user's active layer is put back.
   - Frames past the sequence's end hold its last picture, as the video's did.
8. **Tell the user** and send them the before/after sheet:
   - what changed, including the numbers (eased before → even after);
   - where the frames are;
   - that the file isn't saved;
   - that any folders from earlier tries are left for undo, theirs to delete.

## Notes

- **The ends.** The first new frame is the first source frame, and the last is where the feature arrives
  (within 1.5%). The slow settle after that is left out, so a still hold of the final pose, if wanted, is
  the clip's own Hold.
- **Even spacing, not even angle.** Spacing is measured on screen, as a linearly keyed move plays. Turning
  it into an angle (asin of the sideways travel) magnifies tracking noise near a profile, so don't.
- **Blended frames** are only ever between neighbouring source frames. During the fastest part of the
  original that's a few pixels, too little to ghost.
