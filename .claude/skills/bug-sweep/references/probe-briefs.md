# Probe briefs

Each probe is a background general-purpose agent. Fill in `<WORKTREE>`, `<EXE>` (the frozen copy), `<FILE>` (the
private copy), `<SKILL>` (this skill's folder), `<OUT>` (its own folder) and `<KNOWN>` (bugs already found or fixed,
and known artifacts), then paste the shared part and one area.

## Shared part (every probe)

You are hunting bugs in Open Animate (a C++/ImGui 2D animation app with an embedded Python API `oanim`, shaped like
Blender's bpy; data model like Moho). Read-only on source: do NOT edit anything under the repo, do NOT build, do NOT
run tools/mutation_checks.py.

- Repo: `<WORKTREE>`. Read `CLAUDE.md` (layout, conventions, the Status section naming files and test hooks) and the
  parts of `docs/SPEC.md` for your area: that is the intended behaviour. The Python API is `scripts/modules/oanim/`;
  `tests/` shows how to drive every operator from Python.
- App: `<EXE>` (a frozen copy; another session rebuilds the real one). Run scripts headless:
  `<EXE> --background --python <script.py>`; it exits 1 if the script raises; a crash dies without a traceback (note
  the exit code). Run several in parallel.
- File: `<FILE>` (a private copy). Never save over it: save copies only to `<OUT>`.
- Harness: `<SKILL>/scripts/harness.py` (load, run, rebase, order_check, roundtrip, snap/diff/edits, summary) and
  `<SKILL>/scripts/fuzz.py`. Set `OA_SWEEP_FILE=<FILE>` and `OA_SWEEP_TMP=<OUT>`. Read the harness's notes: Python data
  edits (bone_set, set_point) are not undo steps, undo keeps the playhead, selection is never an undo step.
- Never use a tool that drives the user's running app (openanimate MCP, computer use).
- Known, don't re-report: `<KNOWN>`.

For every candidate bug, confirm it with a minimal standalone repro script that exits 1 while the bug is there (an
assert with the numbers in its message), and check the code and spec to tell a bug from intended behaviour. Report
back (under ~600 words): a numbered list of CONFIRMED bugs, each with one line of what goes wrong, expected vs actual
(with numbers), the repro's full absolute path, and your best guess at the cause (file:function); then a short list of
doubtful / maybe-by-design findings; then what you checked that came out clean. Don't fix anything.

## Areas

**Bones and rigging.** Bones, 2 Axis Controllers, actions and action bone actions, constraints, dynamics: on frame 0
and posed frames, inside and outside actions (`doc.action`), auto-key on and off. Bone add/delete/duplicate/parent/
connect, convert types and back, controller sides, knobs past the box, fixes and spots, action new/rename/delete/
mirror/frames, constraints add/remove/reorder/disable, dynamics on/off/bake, Apply Pose as Rest, Freeze Keys, IK.

**Motion paths and timing.** The Motion Path tool, path keys, handles, control points and their handles, breakdowns,
path brushes, Slide, overshoot and anticipation, the timing chart and graph, Time Together, Key Whole Drawing, paths
drawn Relative To. Every interpolation, holds, intervals (on 2s and 3s), keys on adjacent frames and frame 0, many
control points. Check that dots don't move when an op promises they won't, and that the drawn path matches the pose
on every frame.

**Timeline, layers, undo, files.** Key select/move/scale/delete across rows, ripple, Time Together, interpolation and
key types, insert/delete keys, copy/paste keys, keys inside actions; layer duplicate/delete/reorder/group/move in and
out of bone layers/copy/paste/references; point edits on posed frames; long undo/redo chains; save/open after each
kind of edit, Save Incremental.

**Random operations.** Run `<SKILL>/scripts/fuzz.py` with many seeds (`OA_FUZZ_SEED`, `OA_FUZZ_OPS=300`) in parallel,
then shrink each failure with `python <SKILL>/scripts/shrink.py <EXE> <fuzz_fail_N.json>` and turn the shrunk log into
a standalone repro. If the fuzzer misses a kind of edit the file invites (masks, switches, references, actions), add
an op for it to your own copy of fuzz.py and say so in the report, so the skill can take it.
