---
name: match-vectors-to-reference
description: Match one frame of a rigged character's vector drawings (points and Bézier handles only, no bones) to a reference picture or reference video frame in the user's Open Animate project, such as Clay's head turn against the HeadTurn media. Use when the user asks to match, trace, fit or line up a frame's shapes, outlines or features to a reference, to reshape a pose's vectors to a drawing, or to fix a frame's silhouette, nose, eyes, mouth or ears against a guide. Covers reading the masks, extracting the reference's shapes, fitting points and handles without adding points, detail levels (level 0 first, then level 1), applying as one undo step and checking the render.
---

# Match a frame's vectors to a reference

Reshape one frame of a character so its render matches a reference that is **already aligned** in the
document (a Media layer the user lined up: don't move, scale or re-fit it). Only the vectors change:
points and handles, keyed at that frame. **No bones, no new points, no layer, mask or style changes**,
and nothing the user put off-limits (for Clay: **never the hair**, anything under `Hair_X`).

This overrides the open-animate skill's "bones first" for this task. Everything else in that skill
still holds: read the masks first, work on a private copy, apply only after the user says yes.

Scripts are in `scripts/` (Python 3 with numpy, Pillow, opencv). `vm.py` has subcommands; run it without
arguments for its help. Work files go in the scratchpad (`WORK` below).

## 1. Set up

1. Find the user's app and frame (read only): `%LOCALAPPDATA%\OpenAnimate\remote\<pid>.json`; read
   `oanim.data.document.frame_current`, `.action`, the reference layer's name. Ask which frame if they
   didn't say and the app isn't on it.
2. `python scripts/start_private.py WORK --user-pid PID` saves a copy of their open document (unsaved
   changes too) and opens it in a private Open Animate. It prints that copy's pid: stop it with
   `taskkill /PID <pid> /F` at the end, never by name. The session file is `WORK/private.json`.
   Set the private copy to the same action and frame if it isn't.
3. `python scripts/vm.py snapshot WORK/private.json WORK/s0 --char Clay --ref HeadTurn` renders the
   character alone and the reference alone at the document's size, pixel-aligned, and lists every layer
   with its masks. Look at `WORK/s0/overlay.png`, `clay.png` and `ref.png` before anything else.

## 2. Read the rig and its masks

What you see is the drawings after masks, so decide for each visible edge **which layer draws it**:

- `Shows: inside X`: the layer is clipped to X's shape. Its parts outside X never show; don't fit them to
  anything. Only X's outline decides that edge (Clay: `Eye_R`, `Eyebrow_R`, `Mouth`, `Freckles_R` inside
  `Head_Base_X`, so the far eye and brow vanish past the head's edge by themselves as the head turns).
- `everything but X`: the layer shows outside X only (Clay's ears: `Ear_R`/`Ear_L` fills and lines show
  outside `Head_Base_X`; `Ear_Line_Show_*` adds the ear's line back inside it). An ear's root can stay
  buried under the head: only its part outside the head is seen.
- A **reference** (`Head_Base_Outline_X` is a reference of `Head_Base_X`) follows its original: edit the
  original. The outline layer's own masks hide the head's line under `Neck_X` and the ears and add it back
  along `Jaw_Line_Show_*`; those are what make the jaw line stop at the ear.
- `lines ending at the edge`: strokes stop where the mask cuts them, so a line can be longer than it looks.
- Masks still cut in solo renders (a hidden layer still masks), so `vm.py solo` shows a part as it is.

So: **the silhouette is the mask's shape's job.** To move where a masked layer is cut, move the mask
layer (its outline), not the masked layer. Keep the overlap under masks (a feature reaching past the head
edge into the hidden side is right: it gets cut cleanly).

`snapshot` lists each layer's **detail levels** (`detail levels 0-1 (edit level 1)`): Clay's
`Head_Base_X` (p8, p9), `Nose_X` (p6) and `Neck_X` (p4, p5) have level-1 points. Those layers are fitted
in passes, lowest level first (step 4).

Write a short plan before editing: for each part, which layer, which curve, which run of points shows,
and what in the reference it should match.

## 3. Get each target shape from the reference

`python scripts/vm.py region WORK/s0 X,Y [X,Y ...] NAME [--tol 28] [--grow 1]` flood-fills the
reference from seed pixels and saves the region's outline (document space) as `NAME.json`, with a
preview `NAME.png`. Look at every preview.

- Seed inside a flat colour; the painted outlines stop the fill. Several seeds make one region (their
  union): for a head whose outline passes behind the nose, seed the skin and the nose together.
- Leaks into a neighbour of a similar colour: lower `--tol` (Clay's nose: 14). Too little filled: raise it.
- `--grow 1` (or 2) moves the outline onto the middle of the reference's line; the drawn stroke is
  centred on the curve.
- A region often includes more than one layer's edge (skin = head + ear + neck). Fit only the stretch a
  layer owns: `fit --part X0,Y0,X1,Y1` keeps the outline inside that pixel box.
- For a feature with no fill of its own (a mouth line, a brow stroke), find its pixels by colour in
  `ref.png` (numpy), order them along the stroke, and save `[[x, y], ...]` in document space through
  `Mapping.to_doc` in `vm.py`; `fit` takes that JSON file as its target.

## 4. Fit points and handles

`python scripts/vm.py wires WORK/s0 LAYER... --target NAME` draws the curves as they show (round =
smooth point, square = corner, labels `curve:index pPoint`) over the reference with the target outline in
cyan. Then:

`python scripts/vm.py fit WORK/s0 LAYER CURVE NAME [--range A:B] [--part box] [--corner I] [--pin I=X,Y] [--fixed I]`

- A whole closed curve to a whole closed outline: each point keeps its share of the curve's length, the
  set turned to where it fits best, so every point keeps its role (the nose's notch point stays the
  notch); corner points then take their role: a crease (a corner turning inward, as where the nose's
  lobe meets its bridge) takes the outline's most inward turn nearby however gentle, any other corner
  the outline's nearest sharp turn; smooth points take the outline's
  farthest place out their way from the shape's middle, turned as the whole shape turned (the middle of
  a rounded end, the top of a hump: where Bézier points belong). The share alone leaves a point short of
  the extreme it had, like the nose's tip point sitting on the slope above the tip.
- Part of a curve (`--range 9:4`, wrapping on a closed curve) to part of an outline (`--part`): each
  point goes to its nearest place, in order. The neighbouring handles outside the range are kept in line.
- `--pin I=X,Y` (pixels) puts a point where you decide (a landmark: the nose tip, the chin, where the ear
  meets the head); `--fixed I` keeps one; `--corner I` lets a smooth point break; `--handles-only`
  reshapes without moving points.
- Each segment's two handles are fitted by least squares with the smooth points' handles kept in line
  (Schneider's method), shorter than 3/4 of the segment, never crossing.

It prints the error before and after in pixels. Then look: `vm.py wires WORK/s0 LAYER --target NAME
--edits WORK/s0/edits_LAYER_cN.json` (orange = the fit). Good: mean under 1 px, worst under 3 px at the
document's size, and the points spread as the original was. If not, pin landmarks, narrow the part or
the range, and fit again.

### Detail levels: level 0 first, then level 1

A detail point (level 1 and up, added by the knife) sits on its parents' coarse segment by offsets, so
it rides that segment when the coarse points move. The stored handle of a parent that faces detail is its
coarse handle; what's drawn is shortened from it. So a layer with detail is fitted a level at a time:

1. **Level 0.** `fit ... --level 0` (the default for a layer with detail) fits the curve as level 0 has
   it: only the level-0 points, with their coarse handles. `wires --level 0` shows that curve. The level-1
   points aren't fitted; they come along. `--range` indices are the level-0 curve's (as `wires --level 0`
   labels them).
2. **Apply it, snapshot again.** `apply` sets the layer's edit level to the edits' level, places them, lets
   the detail ride (`layer.detail_sync()`), then puts each shape's edit level back. Editing level 0 at
   edit level 1 would leave the detail where it was instead, as editing by hand does.
3. **Level 1.** `fit ... --level 1` on the new snapshot moves only level-1 points and their handles. The
   level-0 points and their handles stay as level 0 fitted them. Where the level-0 curve already lies
   within 3 px of the outline around a detail point (`--smooth-under`), the point is **smoothed onto it**
   (Smooth to Lower Level: no offset of its own) instead of fitted. Fitting it there only chases the
   traced outline's pixel noise and leaves a lump: short corner handles and a kink, as Nose_X's p6 had.
   Only where the reference really has a bump or notch is a detail point fitted, starting at its share of
   the way between the coarse points around it. Its edit files end `_L1`, and the level-0 ones end `_L0`.
   Look at the result zoomed in the viewport (`oa_remote.call(session, "screenshot", area="Viewport")`):
   a kink at a detail point is a lump even when the error in pixels is small.

Never fit a layer with detail as drawn with every point free: that pulls detail points onto the outline
as if they were their own points, and sets the coarse handles from the shortened ones.

### Parts that sit on or behind others

- **Ears behind the head.** Only the part outside the head shows. Fit that part only: `--range` over the
  visible points, `--drop` the stretch of outline that is the head's edge, `--fixed` the root points inside
  the head, and `--join-fixed` so the edges run on behind the head to them. Keep the root where the rig
  puts it, on the skull. The far ear in 3/4 is a thin sliver: its outer edge is the back rim, and its inner
  line runs toward that rim. Never trace the cheek line with the ear; the head's outline makes that cut.
- **Eyes.** The iris, pupil and shine are drawn on top of the white and aren't masked to it, so the white is
  the whole eyeball. Seed the white, the pupil and the shine together, then `--grow 2` to reach the lid
  line. Make the white a true oval (`ellipse`), not a point-by-point fit (that's lumpy). Then `carry` the
  lid, iris, pupil and shine from a frame-0 snapshot by the white's own change. The rig's design stays:
  the lid hugs the white without covering it, and the iris keeps its size. `--shift-to` the reference's
  dark pupil region then sets the gaze. Use frame 0 as the guide wherever the reference is too blurry to
  trace small parts.
- **Strokes** (brows, mouth): `stroke` traces a drawn line's middle (`--not nose` leaves out a traced
  region), and a whole open line fitted to it puts its ends on the stroke's ends.
- **Dots** (freckles): move each dot onto a reference dot, one to one, least total movement. A dot the
  reference hides (behind the nose) stays.
- **Someone may be using the copy.** `snapshot --frame 49` and `apply --set-frame` work at that frame and
  put the app's frame back after.

### What the user's own corrections showed (Clay, head_turn frame 49)

- **An eye is one unit.** The user moved, turned and scaled each eye's white, iris, pupil, shine and lid
  together. Keep their frame-0 relationships, and never fit them one by one.
- **Both eyes foreshorten in a turn, the irises most.** Far white: 31.6 px wide where the reference fit gave
  35 (height kept, 54). Near white: a little narrower too (52.6 against 56.8 at frame 0). Near iris: 27 px
  wide against 31 (a disc facing across the turn). Eyes never grow from frame 0.
- **Ear joint.** The head's top edge and the ear's top edge meet in one shared point, a clean V with no
  notch of hair between them.
- **The ear's hidden root** is a rounded shape well inside the head. It covers any gap when the head's edge
  moves, and it is never pulled out to the visible edge.
- **Show helpers move with what they show.** `Ear_Line_Show_L` (where the ear's inner line shows inside the
  head) was moved and narrowed to suit the ear's new place. Check every mask helper of a part you move.
- **The ear's inner line** follows the reference's line.
- **Smooth jaw.** No kink where the jaw meets the neck: the user smoothed the handles there.
- **Keys match the action's interpolation.** The head_turn action is keyed Linear (its reference was
  retimed to even speed). Auto-key made the frame-49 keys Smooth, and the user set them back to Linear.
  `apply` now does this itself.
- **Don't refit what the user called fine.** When they say a part was fine, put it back (`restore`) and fix
  what actually causes the look: the near ear seemed misplaced because the head's edge behind it was wrong.

## 5. Apply, render, judge

1. `python scripts/vm.py apply WORK/private.json WORK/s0/edits_*.json` places all of them as **one undo
   step** ("Match Vectors"), keyed at the current frame (auto-key). It refuses if the app isn't on the
   frame the edits were made for.
2. `vm.py snapshot` again into a new folder (`WORK/s1`) and compare `overlay.png` with the last one;
   `vm.py solo` + `vm.py score WORK/s1 SOLO NAME` gives a part's IoU and outline error (red = drawn but
   not in the reference, blue = missing).
3. Order of work: the silhouette (head outline) first, then the big shapes (ears, neck), then features
   (nose, eyes, brows, mouth), then details (freckles, lines). Re-snapshot after each stage: moving the
   head changes what its masks cut.
4. Have a fresh subagent rate `ref.png` against the final `clay.png`/`overlay.png` (open-animate skill's
   "Rate it with a subagent"), fix the three things it names, and stop when the error is about 1-2% of
   the head's height or stops improving.

## 6. Hand it over

Show the user the before and after overlays (SendUserFile) with the numbers. Only after they say yes,
apply the same edit files to their app: `vm.py apply <their remote json> WORK/sN/edits_*.json` (one undo
step; the frame check protects the wrong frame). Never run `snapshot` or `solo` on their app: those
change visibility for the render. Then stop the private copy by its pid.

## Head turns and Béziers

See [references/vector-matching.md](references/vector-matching.md) for the rules this follows: how a head
turns (and what it means for each part), how to place points and handles, and checking a match.
