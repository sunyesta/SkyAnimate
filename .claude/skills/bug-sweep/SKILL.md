---
name: bug-sweep
description: Robust bug testing of an Open Animate file or feature - when the user asks to test robustly, sweep, hunt or find bugs, do random operations on the bones and points, stress-test a file (such as the eye path test file), or fix the bugs a sweep finds. Covers probing with background agents on private copies, the seeded random-operations fuzzer and its shrinker, confirming each report, and fixing confirmed bugs while the probes keep running.
---

# Bug sweep

A sweep finds bugs in a file the user cares about, confirms each one, and fixes the confirmed ones, all without
touching the user's open app. Probes run in the background on private copies while you fix what has already come in.

## 1. Set up (a few minutes)

- **Private copy of the file.** Copy the user's file into your scratchpad (`<sweep>/file.oanim`). Probes open the copy;
  nothing is ever saved over it or over the user's file. The user's open app is read-only: never script it.
- **Frozen app.** Copy the build's output folder (`build/bin/RelWithDebInfo/`) to `<sweep>/bin/`. Probes run that copy,
  so you can rebuild the real one while they work. (Without it, a rebuild either fails with LNK1168 while a probe runs
  the app, or changes the app under the probes.)
- **Known list.** Collect what's already been found or fixed (earlier sweeps, the conversation) and known artifacts, so
  probes don't report them again.

## 2. Probe in parallel (background agents)

Launch probes as background `general-purpose` agents, all in one message, with the briefs in
`references/probe-briefs.md`: one per area (bones and rigging; motion paths and timing; timeline, layers, undo and
files) plus one running the random-operations fuzzer. Give each its own output folder. Add an area when the file
invites one (switches, media, masks). Tell the user what's running in a few lines, and that nothing is fixed yet.

The fuzzer (`scripts/fuzz.py`) throws seeded random sequences at the file's bones and points, about 45 kinds of op:
frames, auto-key, undo and redo; bone moves, turns, scales and lengths (Python and G R S, Without Children, Only
Bones), knobs inside and past their box, add/delete/duplicate/extrude/subdivide/parent/convert/clear bones; actions
entered, left and made; points moved, deleted, dissolved, subdivided, split, added, extruded; weights assigned,
removed, automatic, cleaned; Parent Bone with its deform modes; new drawings in the rig; insert/delete keys; Sculpt strokes with every brush (Dyntopo 40% of the time, at Resolution
8-400 and each Refine Method) and Simplify; curvature, width, smooth, slide, merge, mirror, symmetrize, clear handles,
unalign and handle moves (`OA_FUZZ_ONLY` limits a run to a list of op names). It checks
every op (no exception, finite numbers, a change only with an undo step, CANCELLED changes nothing, entering an action
and leaving changes nothing) and every few ops (undo then redo exact, frames in different orders give the same pose,
save/open exact, a point weighted wholly to one plain bone turns rigidly with it). A watchdog (`OA_FUZZ_HANG`, default
150 s per op) dumps the stacks and exits; the log is written before each op, so a crash or hang still leaves one. A
failure writes the op log; `scripts/shrink.py` cuts it down to the fewest ops that fail the same way (past about 100 ops that takes too long: save the state just before the failing op and write the
repro from there). Run it yourself
too while waiting: a handful of seeds costs little. When a probe adds an op to its own copy, fold it in here.

```
set OA_SWEEP_FILE=<sweep>\file.oanim & set OA_SWEEP_TMP=<sweep>\t1 & set OA_FUZZ_SEED=1 & set OA_FUZZ_OPS=300
<sweep>\bin\OpenAnimate.exe --background --python <skill>\scripts\fuzz.py
python <skill>\scripts\shrink.py <sweep>\bin\OpenAnimate.exe <sweep>\fuzz_fail_1.json
```

## 3. Confirm every report

A probe's report is a claim. Rerun each repro yourself (in parallel) and read the numbers before telling the user it's
a bug. Sort into: confirmed bugs; maybe-by-design (check the spec and the code, and ask the user when it's a design
call); harness mistakes (drop them). Tell the user each batch as it's confirmed: one short line per bug, worst first,
in their words (what they'd see), with the numbers that show it.

## 4. Fix while the probes keep running

Many bugs: fix them in groups (by area, so no two groups touch the same files), each in a worktree of its own made from
the branch (`git worktree add -b claude/fix-<area> ../fix-<area> HEAD`, configured with
`-DFETCHCONTENT_SOURCE_DIR_<DEP>=<this build>/_deps/<dep>-src` so nothing is fetched again), and cherry-pick each group's
commits into the branch when it is done. Groups write their mutation entries and spec wording to notes files; the
branch takes them in one go (two groups appending to PENDING or editing one spec line conflict).

When the user wants fixes (they may say so up front), fix confirmed bugs one at a time, worst or simplest first,
while probes keep reporting:

- Read the code the repro points at; find the cause, not the symptom. A design call (two behaviours that can't both
  hold) goes to the user with a recommendation before you build it.
- Write the test first through operators (`tests/`, `add_test` in CMakeLists.txt), see it fail, fix, see it pass.
- Add mutation entries to `PENDING` in `tools/mutation_checks.py`, and don't run them: they wait until the user says
  "run milestone checks" (or asks for mutation checks in this request). Instead, undo each fix by hand once and see
  its test fail (below). When they do run, nothing edits source meanwhile (the script swaps it), and each must be
  caught; strengthen the test if not.
- Rebuild with the LNK1168 retry loop (the user's own copy may hold the exe), then run the broad related tests
  (`ctest -R` over every area the change touches), not only the new one: a fix in a shared path (how a key goes in,
  how a frame is evaluated) can change everyday behaviour. Keep such a fix to the cases the bug is about (a key put in
  a stretch shaped on the path, not every key) and the plain cases as they were.
- A test that passes with its fix undone tests nothing: undo the fix by hand once (`if (false && ...)`), rebuild and
  see the test fail before relying on it. Build its scene like the repro's (the eye file's Look At had an Eye Shape;
  without one the bug never showed).
- A mutation entry's test is `'tests/test_x.py'` or a bare stem (`'timing_curve'`); a bare `'test_x.py'` finds no
  test and shows as missed. `--lanes N` counts this folder as one, so with `--not-here` `--lanes 1` runs nothing ("0
  caught, 0 not"): one lane is `--lanes 2`. An entry whose undone code doesn't compile at `/W4` (a variable left
  unused) reports BUILD FAILED: undo the whole change in it. Guards the operators can't reach (op_arg_float already turns nan, inf and text into the
  fallback) need no entry.
- A hang's test: `faulthandler.dump_traceback_later(20, exit=True)` before the op, cancelled after. Its watchdog runs
  outside Python's lock, so it fires while a C++ operator loops (a `threading.Timer` never gets to run).
- Taking out a floor or clamp (a least speed, a least gap) can break code that counted on it: what was always slightly
  increasing becomes flat, and a strict `<` then reads a held frame as a turn. Run the tests on 2s and 3s too.
- Precision leftovers: measure what is left after a fix (frame by frame) and say what it comes from (sampling, a
  polyline's chords) rather than chasing it; tell the user how big it is next to the scene.
- New reports keep arriving: confirm them between fixes, add them to the list, never interrupt a fix half done.
- At the end: the spec (`docs/SPEC.md`) and the live Doc in small find-and-replace edits, then one commit on the
  branch. Never push or merge unless told.

## 5. Improve this skill

After each sweep, fold back what you learned: a new fuzzer op for a kind of edit it missed (an area probe's report
may name one), a new invariant, a harness pitfall below, a better brief. The skill lives in the repo
(`.claude/skills/bug-sweep/`), so commit its changes with the sweep's fixes.

## Load

Keep the machine's load moderate: about three heavy jobs at once (a build, a probe, a mutation lane each count). A
sweep with five mutation lanes, four fix builds and probes running for hours ended with the computer switching off,
and the power cut zeroed a source file a mutation check had open (the runner now syncs its backup first; after a
crash, check every folder for files of zero bytes and broken .pdb/.obj files before building).
`tests/test_motion_path_places.py` fails on a saturated CPU (its work never spreads over threads): rerun it quiet.

## Pitfalls (harness mistakes that look like bugs)

- **Python data edits aren't undo steps.** `bone_set`, `set_point`, `layer.keyframe_insert` join the next operator's
  step, so undoing that operator also takes them back. Call `harness.rebase()` (save and reopen) before checking an
  operator after such edits.
- **Undo keeps the playhead**, as Blender does: compare after undo at the frame the state belongs to.
- **Selection and the active layer or bone are never undo steps**: `harness.edits()` leaves them out.
- **A reference's points are locked** unless its Edit Points is on; the fuzzer skips such layers.
- **The undo history holds about 62 steps**: a long chain can't undo to its start.
- **Bones in Python have no id key**; `parent_bone` is a name; a controller with no side actions has a zero box,
  and Contain Knob clamps the knob to it (use `contain_knob=False` in probes).
- **"path dots off the pose by ~0.0003" with Own Motion** is a known artifact of the eye file.
- **Frame-order bugs hide behind reads.** Setting `doc.frame_current` evaluates at once; a probe that reads between
  frames changes what the next frame starts from. Compare a frame reached from several others (`f-1`, `f+1`, 0, keys)
  without reads between, and diff every layer's points and bones to find which one depends on the order.
- **A parent with uneven scale** turns and stretches its children by a skew, not a similarity: a check that a point
  weighted to one bone turns rigidly with it must skip such bones (`skewed_above` in fuzz.py).
- **Some operators push two undo steps** (a select-none and the edit), and `doc.time_together` set from Python isn't
  one: undo back to each saved step index, not one undo per op.
- **A rig loop** (a bone's IK target riding on the chain, a constraint fed by what it moves) can make a pose depend on
  history by design: report it as a question, don't fix it as a bug.
