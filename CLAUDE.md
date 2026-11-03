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
`src/data/fill_paint.*`, the Stroke Eraser and Vector Trimmer in `src/data/line_cut.*`. Still to do in milestone 2: Knife tool
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
Switch layers section. Next: key buttons in Properties, dope sheet, frame-by-frame layers
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
Controller (`src/anim/controller.*`, Convert To; the knob plays and links actions, `controllers_link` in `src/anim/animation.cc`; its fixes in the Bone tab's Fixes panel, `controller_fixes_panel` in `src/editors/properties.cc`). The Actions panel in the Armature
tab (`actions_panel` in `src/editors/properties.cc`; `ActionPlay` in `src/data/layer.hh`: each action's Frame, mute and loop,
played by `action_own_frames` and set by `action_frame_set`). Not yet: local
armature viewport, copy/paste poses.

Preferences live in `%LOCALAPPDATA%\OpenAnimate\preferences.json`; background runs (tests) never read
or write them.

Operators take string arguments (`OpArgs`, read with `op_arg()`), as in
`oanim.ops.wm.open_mainfile(filepath="walk.oanim")`. Changes without an undo step call
`UndoStack::mark_changed()` so the unsaved mark and autosave see them.
