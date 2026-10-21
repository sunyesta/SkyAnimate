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
- Moho decides data-model behaviour (layers, channels, actions, smart bones, interpolation, frame 0
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
Incremental, pen pressure, Moho stamp brushes (`src/assets`: the asset library set in Preferences,
.mohobrush reading and Moho import; dabs in `src/gpu/brush_stroke.cc`; Brush Settings popup in
`src/editors/brush_settings.cc`). Still to do in milestone 2: Knife tool (Ctrl K), pen eraser end,
the style library.

Milestone 3 (Animation) in progress on `milestone-3-animation`. Done: animated channels
(`src/anim/channel.*`, Moho interpolation modes, hold, interval) on layer transform, opacity,
points, radius, curvature and stored handles; edits become keys through `src/anim/animation.*`
(recorded before every undo push, frame change and save; frame 0 is the setup frame; auto-key on
`Document`); I / Alt I, keyframe jumps, key diamonds in the timeline. Next: key buttons in
Properties, dope sheet, frame-by-frame layers and onion skins, switch and image layers, PNG
sequence export, playback range.

Rigging and modifiers (milestones 4 and 5, started early): bone layers (`LayerType::Bone`,
`src/data/bones.*` for the skeleton and _L/_R names), weights (automatic for new points,
Weight Select / Weight Paint tools) and Parent to Bone; the modifier stack (`src/anim/deform.*`: Mirror, and the Armature modifier every
layer inside a bone layer gets, kept by `document_sync_armature_modifiers`); rig operators in
`src/ops/rig_ops.cc` (setup frame only for rig changes); Blender-style bone editing (head/tail joints, E, Shift A,
Shift D, Ctrl P / Alt P; `armature_move_selected_joints` for G R S on frame 0); Weight Paint tool. Actions (bone layer
timelines swapped into the channels) and smart actions (`apply_smart_actions` in
`src/anim/animation.cc`; offsets live in `Channel::smart`). Bone constraints (`BoneConstraint` stack solved in `armature_pose`, IK targets and
`armature_reach` for Auto IK) and dynamics (`src/anim/dynamics.*`, simulated and cached). Not yet: local
armature viewport, copy/paste poses.

Preferences live in `%LOCALAPPDATA%\OpenAnimate\preferences.json`; background runs (tests) never read
or write them.

Operators take string arguments (`OpArgs`, read with `op_arg()`), as in
`oanim.ops.wm.open_mainfile(filepath="walk.oanim")`. Changes without an undo step call
`UndoStack::mark_changed()` so the unsaved mark and autosave see them.
