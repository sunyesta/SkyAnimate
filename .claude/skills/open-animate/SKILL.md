---
name: open-animate
description: Use when working in the user's Open Animate projects - animating (a head turn, mouth shapes, a walk, an action or smart action, from a reference video or image), posing, rigging or editing drawings through its Python API and remote control. Covers how to animate (bones first, points to fine-tune), working on a private copy, matching a reference and getting the result rated.
---

# Open Animate

Open Animate is the user's 2D animation app (this repo; Python API `import oanim`, files `.oanim`).

## Animating: bones first, then points

**Prioritize moving bones over points.** Move the bones into the rough positions first, then move
points to fine-tune.

**Don't add points to a rig's drawings** (no knife cuts, no new detail points) unless the user asks.
Animate what the rig already has: its bones, then its existing points. Its masks usually make the
shapes a turn needs: in the mouth rig the head is masked outside Mouth_Background_X, so the mouth's
background cuts the head's silhouette by itself as the mouth slides past the edge.

**Prioritize lower shape detail levels, and increase the detail as needed.** Pose a shape's points at
its lowest detail level (level 0, its own points) first; go up a level (Ctrl 1.. or
`oanim.ops.mesh.detail_level_set`) to move the detail points a shape already has only where the lower
level can't make the shape. Fewer points keyed means smoother motion and less to fix later.

## How to pose

- **Bones first, points to fine-tune.** Rough every pose in with bones: position, angle and scale
  (`layer.bone_set(name, position=(x, y), angle=..., scale=(x, y))` on the bone layer, at the frame,
  auto-key on). Only once the bones have the big shapes, move points (`layer.set_point(i, (x, y))`,
  handles with `layer.move_handle`) for what bones can't do: a lip curling, a silhouette notch, a
  squash a bone would distort.
- **Frame 0 may be edited.** It is the setup pose: changing it moves every pose and action with it
  (bone, point and handle keys hold the change from the setup). Fix the setup there when a rig needs
  it, rather than fighting it with keys on every frame.
- Bone positions are in the parent bone's space (a child of a bone pointing up has its x axis up). Read
  `rig.bones` (setup_position, setup_angle, parent) before moving anything.
- Smart actions: an action named after a bone (`HeadTurn_L`) is driven by that bone's angle. Inside the
  action, key the driving bone's angle from its setup angle (frame 0) to the full turn (the last frame),
  and key the poses on those frames. On the mainline, animating the bone's angle plays the action.
- Detail levels: a shape can hide fine points (`layer.detail`); set `oanim.ops.mesh.detail_level_set`
  before moving them, and remember detail rides its coarser curve.

## Before posing: read the rig

Dump the layer tree with types, masks, references and bindings (`layer.parent_bone`, point weights),
the bones and the action's existing keys.

**Understand the masking in the document before animating.** For every layer, read `layer.masks` (each
mask: Inside or Outside which layers, Mask With: As Drawn, Fills Only, Filled Shape; Extend) and
`layer.masking` (the layers it masks), and work out what each does as things move: which layer cuts
which, what shows through where, and which hidden layers (often references of a visible one, like a
hidden head_shape) only exist to be masks. Then check it: on the private copy, move the bone that
carries a mask layer and look at the result before deciding anything needs reshaping. Masks usually
do the hard part: in the mouth rig the mouth is masked inside a hidden copy of the head and the head
is masked outside Mouth_Line_X and Mouth_Background_X, so the mouth's background cuts the head's
silhouette by itself as the mouth slides past the edge.

## Work on a copy, then show the user

- The user's own Open Animate is often open: read from it only, and read the open document (unsaved
  changes too) rather than the file on disk. Each running Open Animate publishes
  `%LOCALAPPDATA%\OpenAnimate\remote\<process id>.json` (port, token, pid, open file); `remote.json` is
  the one holding the usual port. With the MCP bridge: `list_projects`, then `project=` on each tool. Copy their saved `.oanim`/`.sky` file (check
  `oanim.data.is_dirty` is False first) into the scratchpad and animate that.
- Run a private copy with its own settings: `LOCALAPPDATA=<scratch>/appdata`, a startup script that
  opens the copy, calls `_oanim.remote_start(0)` and writes its port, token and `os.getpid()` to a file.
  Stop it only by that PID, never by name or by excluding the user's PIDs.
- Apply to the user's file only after they have seen the result and said yes, through undoable
  operators (one undo step: a `oanim.types.Operator` with `bl_options = {"REGISTER", "UNDO"}`).

## Matching a reference video

- In the private copy, `oanim.ops.media.add(filepath=ref)` puts the reference in as a Media layer (a
  Guide: never rendered), to compare against in place; `oanim.media.pixel(path, seconds, x, y)` reads its
  frames. Never add one to the user's file unless they ask.
- Frames: `ffmpeg -i ref.mp4 -vf "fps=4,scale=320:-1,tile=6x3" -frames:v 1 sheet.png` for the whole
  move, and `select='eq(n\,36)+...'` with `-fps_mode passthrough` for key moments. Note the timing (holds,
  where the motion starts and ends) as well as the poses.
- Frame the result the same way: in the copy, a camera on the subject (`oanim.data.camera`), overlays off
  (`oanim.wm.set_overlay("show", False)`), the camera picked so the viewport looks through it; screenshot
  the viewport per frame and tile them into a sheet matching the reference's moments.

## Rate it with a subagent

Give a fresh subagent the reference sheet and the result sheet (paths) and ask for a score out of 10 on:
silhouette per moment, the parts' positions and squash, overlap and timing (holds, easing), and anything
that pops or breaks between frames; plus the three changes that would raise the score most. Fix those,
re-render and rate again until it scores well or stops improving; report the scores to the user.
