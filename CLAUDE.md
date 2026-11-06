# Open Animate

A private, Windows-only 2D animation app: Moho's animation model with Blender's interface and
editing habits. C++20 core, Dear ImGui UI, embedded Python API (`oanim`, shaped like Blender's `bpy`).

The full plan is `docs/SPEC.md` (exported from the live Claude Docs spec; keep both in sync).
Reference material lives outside the repo, one folder up: `../blender-main/blender` (Blender
source, GPL; fine to read and port for this private app) and `../Moho` (Moho scripting docs and
Lua scripts). Never copy them into this repo.

## Build and test

CMake is bundled with Visual Studio Build Tools and is not on PATH:

```
CMAKE="/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
"$CMAKE/cmake.exe" --preset windows          # configure (fetches SDL3, ImGui, pybind11)
"$CMAKE/cmake.exe" --build --preset release  # build -> build/bin/RelWithDebInfo/OpenAnimate.exe
"$CMAKE/ctest.exe" --preset release          # run tests
```

Tests are Python scripts in `tests/`, run headless:
`OpenAnimate.exe --background --python tests/<file>.py` exits 1 if the script raises.
Add each new test file to `CMakeLists.txt` with `add_test`. Prefer testing through operators and
`oanim.wm.event_simulate()` so tests cover the same path as the UI.

Mutation checks (a test must fail with its fix undone) are batched: each feature adds its checks to
`PENDING` in `tools/mutation_checks.py` and runs only its own tests. When the user says "run milestone
checks", run that script and the full ctest, strengthen any test that misses, commit and empty `PENDING`.
Run them before any merge; say so if a merge is asked for with checks pending.

The desktop and Start menu shortcuts run `tools/run_openanimate.ps1`, which starts a copy of the
build from `build/run/<build time>/`, so rebuilding works while Open Animate is open (reopen it to
get the new build). Run the build output directly only for tests.

Python is embedded from `C:/Python314` (`OANIM_PYTHON_ROOT`), isolated from PYTHONPATH and user
site-packages. Scripts load from the source tree's `scripts/` while developing.

## Two chats at once

Unrelated features can run side by side in separate chats, each in its own worktree. Two chats must never share
a folder: their edits and builds collide, and mutation checks temporarily change the source.

- Start the second chat in a new worktree (the Code tab offers it when starting a session), on a branch of its own
  made from `milestone-3-animation`. That folder has its own `build/`; its first configure and build take about
  5 minutes. Keep it off very long paths: a first configure that fails is worth one more try before looking further.
- Everything happens in that folder: edit, build, test, and run private copies from its `build/`.
- Commit on the branch. Don't push it, and don't merge it without being told.
- Merge one feature at a time: the user says when, it goes into `milestone-3-animation`, then rebuild and run every
  test there. The other chat then brings that branch in before it merges.
- `docs/SPEC.md` is where two features clash, because a section is one long line. Update it and the live Doc last, in
  small edits, and on a conflict keep both changes.
- The desktop shortcut runs the main folder's build, so a feature only shows there after merging. To try one sooner,
  run `tools/run_openanimate.ps1` from its own folder.
- The user's open app stays read-only from every chat. Only push from `milestone-3-animation`, when told.

## Layout

- `src/core` logging and reports (shown in the Info editor)
- `src/rna` property reflection: one description drives the UI, Python and later animation paths
- `src/data` saved data (`Document`); plain copyable values, undo snapshots copy them
- `src/ops` operators, built-in C++ operators, undo stack
- `src/wm` app and main loop, context, keymaps, key-event dispatch
- `src/editors` editor types drawn in dockable areas
- `src/ui` Blender theme for ImGui
- `src/python` interpreter and the low-level `_oanim` module
- `scripts/modules/oanim` the public Python API; `scripts/startup` runs at launch (keymap, Python operators)

## Conventions

- Follow Blender's patterns: every action is an operator (`category.name` idname, `poll`, `exec`,
  `OPTYPE_REGISTER`/`OPTYPE_UNDO` flags); keymaps are data; properties are described through RNA.
- Blender's file style: `.hh`/`.cc`, `snake_case` functions, `CamelCase` types, two-space indent,
  `/* */` comments that explain why. Compile clean at `/W4`.
- Moho decides data-model behaviour (layers, channels, actions, action bones, interpolation, frame 0
  as the setup pose); Blender decides interaction (tools, keymaps, editors, UI).
- Use ImGui wherever it can do the job; write custom widgets on top of it only where Blender's look
  or behaviour differs.
- ImGui gotcha: in a docked window `SetCursorPos()` is measured from above the tab bar and gets
  clipped; position overlays with `GetCursorScreenPos()` / `SetCursorScreenPos()`.

## Status

Milestone 1 (Foundation) done. Milestone 2 (Drawing) in progress. Done: vector layers (Moho curve
model), GL renderer with masking groups, group and reference layers, Layers editor, Properties tabs,
select/draw/pen/split/fill tools, G R S E, .oanim save and open (`src/io/oanim_file.cc`, zip + JSON
with a format version), File menu, unsaved-changes prompt, autosave, File > Recover, Save
Incremental, pen pressure, stamp brushes (`src/assets`: the asset library set in Preferences, brush
shapes in `brushes.cc` with the one-time upgrade of Moho-format libraries, brushes as presets in
`brush_presets.cc`; dabs in `src/gpu/brush_stroke.cc`; the Brush Settings window in
`src/editors/brush_editor.cc`). The Annotate tool: notes and drawing guides over the drawing, never rendered
(`src/data/annotations.*`, operators in `src/ops/annotation_ops.cc`, the tool in `src/editors/viewport.cc`). The draw tools (the spec's "Draw tools" note): every pen sample through Windows Ink or WinTab in
`src/wm/pen_input.*` and `src/wm/pen_wintab.*`, the Stroke Pen's engine in `src/data/ink_stroke.*`, the shelf and presets in
`src/editors/draw_shelf.cc`, `src/assets/draw_presets.*` and `src/ops/draw_ops.cc`, the Fill Pen and Fill Eraser in
`src/data/fill_paint.*`, the Stroke Eraser and Vector Trimmer in `src/data/line_cut.*`. Overlay colours (`src/editors/overlay_contrast.*`: wires, points and
motion path lines take a lighter or darker colour only where hard to see, from the canvas read back after each redraw into
`g_canvas_now` in `viewport.cc`; half-picked wires fade from orange via `OverlayLine::color_b`; light red handles, stronger red where picked, no outlines, shaded only where hard to see; colours in
Preferences; test hook `oanim.wm.overlay_colors`). Still to do in milestone 2: Knife tool
(Ctrl K), the style library.

Milestone 3 (Animation) in progress on `milestone-3-animation`. Done: animated channels
(`src/anim/channel.*`, Moho interpolation modes, hold, interval) on layer transform, opacity,
points, radius, curvature and stored handles; edits become keys through `src/anim/animation.*`
(recorded before every undo push, frame change and save; frame 0 is the setup frame; auto-key on
`Document`); I / Alt I, keyframe jumps, key diamonds in the timeline. Media layers
(`LayerType::Media`: images, sequences, video and sound as clips): decoding in `src/media/media.*`
(WIC and Media Foundation, a worker thread per video), playback sound in `src/media/audio.*` (SDL
audio stream; Sync to Audio, scrubbing), clips and packing in `src/data/media_layer.*`, operators in
`src/ops/media_ops.cc`, the timeline's clip bars in `src/editors/timeline_media.*`; test media in
`tests/data/media`. Switch layers stage 1 (`LayerType::Switch`, `src/anim/switch.*`: keys hold the
child's id, hand keys win over action bone actions until a Rig key, switched-out children via
`Layer::shown()`) and stage 2 (picking: Layers editor dots, [ ], Switch menus, timeline bars,
editing a hidden child via `Layer::edit_out`, Replace Keys With, Wrap in Switch) and stage 3 (sub bone
groups and bone swaps, `src/anim/bone_family.*`: families of bone layers drawn and clicked together,
Match Pose from Previous Drawing) and stage 4 (the Asset Shelf editor, `src/editors/asset_shelf.cc`, with
thumbnails in `src/editors/switch_thumbnails.*`, hover previews via `switch_preview_set`, Duplicate and
Duplicate with Keys) and stage 5 (2-Way Linked drivers, `Driver::two_way` in `src/anim/drivers.cc`: one
channel of keys, edits to the driven side written back; Link All Channels); later items are in the spec's
Switch layers section. Motion paths stage 1 (`src/anim/motion_path.*`: targets, range by keys, paths worked out on copies like onion skins; drawn by `draw_motion_paths` in `src/editors/viewport.cc`; settings in the onion popover; operators in `src/ops/motion_path_ops.cc`; Alt Space is `screen.animation_play_path_range`) and stage 2 (`Tool::MotionPath`: `motion_path_tool_input` and `motion_path_header` in `src/editors/viewport.cc`; placing, breakdowns and Smooth Arc in `motion_path.cc`); stage 3 (path handles on position keys: `Key::path_handles`, `key_path_handles` and the Bezier stretch in `src/anim/channel.cc`; `motion_path_handles` in `motion_path.cc`; drawn and dragged in `draw_motion_paths`; control points' handles, worked as a vector point's: `Key::path_control_handles`, `stretch_points` in `channel.cc`, `motion_path_control_handle_set` and `motion_path_controls_align` in `motion_path.cc`); stages 4-6 (path brushes: `src/anim/path_brush.hh`, the shelf in `src/editors/motion_path_shelf.cc`, strokes by `motion_path_brush_input` in `viewport.cc` and `anim.motion_path_stroke`; Grab and Smooth in `motion_path.cc`; speed brushes paint `Interp::Custom`, per-frame speeds in `Key::speeds` evaluated by `custom_progress` in `channel.cc`; the Edit brush selects keys and dots (`wm.path_dots`), slides dots with proportional editing (`motion_path_slide` in `motion_path.cc`), G R S on keys (`motion_path_keys_transform`), V handle types, Alt S scales the timing (`motion_path_scale_spacing`); its input, modals and menus are `motion_path_tool_input`, `motion_path_edit_modals` and `motion_path_menus` in `viewport.cc`); timing (dots coloured by time
through their stretch, `PathDotColors` and `motion_path_dot_colors` in `motion_path.cc`; the timeline's timing chart and its N
sidebar's Path Timing tab in `src/editors/timeline_path.*`, stretches from `motion_path_stretches`; overshoot is signed
`Key::speeds`, `custom_progress` in `channel.cc`; Shift swaps in a brush, `path_brush_shifted` in `src/anim/path_brush.hh`;
the chart and graph have always-on proportional editing and Shift click's `motion_path_timing_blend`; one selected stretch, teal, shared by the key header, the rows, the chart and the graph: `motion_path_picked_stretches` in `motion_path.cc`, `motion_path_picks` in `motion_path_ops.cc`, `key_focus` and `focus_rows` in `timeline.cc`; Alt click in a row is `anim.timeline_stretch_pick`). Key Whole Drawing (`Layer::key_whole_drawing`, on for new vector layers:
`drawing_key_whole` in `animation.cc` keys every shape channel on every keyed frame and takes a deleted key's frame from them
all; drawing targets, `MotionPathTarget::drawing`, in the timing chart via `motion_path_timing_targets` and
`with_path_channels` in `motion_path.cc`). Key Together on groups (`Layer::key_together`: `key_together_sync` and `key_together_release` in `animation.cc`, sharing `keys_together_sync` with Key Whole Drawing; filler keys are `Key::filler`, let go when it is turned off unless edited). Breakdown keys (`Key::type`, `KeyType` in `src/anim/channel.hh`: a span across breakdowns timed as one in `Channel::evaluate`; timing tools see the span as one stretch through `SpanFlat` in `motion_path.cc`, which moves the breakdowns to their poses' frames; dragged ones clamped in `layers_move_selected_keys`; the Key Type menu is `key_type_header` in `timeline.cc`; Alt click and Alt Shift make them via `motion_path_key_type_set`). Time Together (`Document::time_together`, the timeline's chain toggle: `keys_move_selected`, `time_together_linked`, `time_together_timing_follow` for operators flagged `OPTYPE_TIMING`, `keys_span_frames_add` for + and -, `keys_offset_clamp` for sliding a stretch and `keys_match_timing` in `src/anim/animation.cc`). The timeline's rows as a layer tree (`channel_rows_built` and the filtering `channel_rows` in `src/editors/timeline.cc`: Summary, Pinned, layers in Layers-editor order, Camera and Simulation; Only Selected, Lock and the filters on `Document::timeline_*`, `anim.timeline_lock` and `anim.timeline_filter`; key clicks and drags by `timeline_key_click` and `g_key_press`, typed numbers through `src/editors/num_input.hh`; filler keys hollow by `key_summary_draw`; the scroll bar, `timeline_scrollbar_drag`; test hooks `oanim.wm.timeline_row_details`, `timeline_key_press`). Next: key buttons in Properties, dope sheet, frame-by-frame layers
and onion skins, PNG sequence export, playback range.

Rigging and modifiers (milestones 4 and 5, started early): bone layers (`LayerType::Bone`,
`src/data/bones.*` for the skeleton and _L/_R names), weights (a new point averages its neighbours, one connected to nothing has none,
Weight Select / Weight Paint tools) and Parent to Bone; the modifier stack (`src/anim/deform.*`: Mirror, and the Armature modifier every
layer inside a bone layer gets, kept by `document_sync_armature_modifiers`, and on groups the deforms, `group_deforms` in `deform.cc`, which bend every drawing inside); rig operators in
`src/ops/rig_ops.cc` (setup frame only for rig changes); Blender-style bone editing (head/tail joints, E, Shift A,
Shift D, Ctrl P / Alt P; `armature_move_selected_joints` for G R S on frame 0); Weight Paint tool. Actions (bone layer
timelines swapped into the channels) and action bone actions (`apply_action_bone_actions` in
`src/anim/animation.cc`; offsets live in `Channel::from_action_bones`, mirrored by `src/anim/action_mirror.*`). Bone constraints (`BoneConstraint` stack solved in `armature_pose`, IK targets and
`armature_reach` for Auto IK) and dynamics (`src/anim/dynamics.*`, simulated and cached). Bone types: Regular and 2 Axis
Controller (`src/anim/controller.*`, Convert To; the knob plays and links actions, `controllers_link` in `src/anim/animation.cc`; its fixes and spots in the Bone tab, `controller_fixes_panel` and `controller_spots_panel` in `src/editors/properties.cc`, previews and thumbnails from posed copies in `src/anim/controller_preview.*` and `src/editors/controller_thumbnails.*`). The Actions panel in the Armature
tab (`actions_panel` in `src/editors/properties.cc`; `ActionPlay` in `src/data/layer.hh`: each action's Frame, mute and loop,
played by `action_own_frames` and set by `action_frame_set`). Not yet: local
armature viewport, copy/paste poses.

Preferences live in `%LOCALAPPDATA%\OpenAnimate\preferences.json`; background runs (tests) never read
or write them.

Operators take string arguments (`OpArgs`, read with `op_arg()`), as in
`oanim.ops.wm.open_mainfile(filepath="walk.oanim")`. Changes without an undo step call
`UndoStack::mark_changed()` so the unsaved mark and autosave see them.
