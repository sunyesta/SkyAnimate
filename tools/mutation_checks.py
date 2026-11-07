"""Mutation checks, batched until a milestone: each proves a test fails without the fix it guards.

Each feature adds an entry to PENDING for every fix it makes: the file, a piece of its code (found exactly once), what
to put there instead to undo the fix, a name, and the test that should then fail. They are run together when the user
says "run milestone checks":

    python tools/mutation_checks.py              # every pending check
    python tools/mutation_checks.py 3 7          # only those
    python tools/mutation_checks.py --lanes 6    # six at once (this folder and five lane folders)
    python tools/mutation_checks.py --lanes 6 --prepare   # only make and build the lane folders
    python tools/mutation_checks.py --lanes 6 --not-here  # the five lane folders only: this one stays free to edit
    python tools/mutation_checks.py --remove-lanes

Lanes: a check edits the source and rebuilds, so checks run side by side only in folders of their own. --lanes N uses
this folder and N-1 lane worktrees beside it (<this folder's name>-mut1, ...), made on first use (a configure and full
build, about ten minutes, reusing this build's fetched sources) and kept for the next run; each run brings them to this
folder's commit plus its uncommitted changes. A lane takes the next check on the file it last changed when there is
one, so each rebuild compiles one file. --remove-lanes deletes them.

Run it from the worktree's root, with nothing else building or editing the source there: it changes one file at a
time, rebuilds (for C++), runs the test and puts the file back. A check the test still passes is "NOT CAUGHT": the
test is strengthened until it fails. Once every check is caught and committed, PENDING is emptied.

Two chats can both add entries here; on a merge conflict keep both lists.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import threading

CMAKE = (r"C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/"
         r"CMake/CMake/bin/cmake.exe")
EXE = os.path.join("build", "bin", "RelWithDebInfo", "OpenAnimate.exe")

# (file, old, new, name, test)
PENDING = [
    ('src/ops/layer_clipboard.cc', '      animation_evaluate(doc, doc.frame_current);', '      (void)0;', "Layer Paste evaluates the pasted layers so their points aren't new points", 'tests/test_layer_clipboard.py'),
    ('src/anim/reference.cc', '    if (ref.reference.match_original) {', '    if (false && ref.reference.match_original) {', 'Break Reference takes the modifiers a Match Original reference showed through', 'tests/test_references.py'),
    ('src/anim/animation.cc', '    if (!pastes(c)) {', '    if (false && !pastes(c)) {', "Paste Keys leaves a controller side's timing keys out off that action", 'tests/test_controller_side_timing.py'),
    ('src/anim/animation.cc', ' || (dial && ref.scalar == &dial->angle_keys)', '', "Copy Keys in a side's action marks the linked dial's turns as timing", 'tests/test_controller_side_timing.py'),
    ('src/anim/animation.cc', '  const int offset = frame - first_frame;', '  const int offset = frame - clipboard.first_frame;', 'Paste Keys lands the first key that pastes on the frame', 'tests/test_controller_side_timing.py'),
    ('src/python/py_runtime.cc', '    if (values.contains("knob") || values.contains("knob_snap")) {', '    if (false) {', "bone_set(knob=) plays the knob's actions at once", 'tests/test_controller_side_timing.py'),
    ('src/anim/motion_path.cc', '    k->hold = std::max(start - frame, 0);\n', '    k->hold = 0;\n', 'key added inside a hold keeps the rest of the hold', 'test_path_key_add_dots.py'),
    ('src/anim/motion_path.cc', '    if (span_in > 0 && span_out > 0 && frame <= start) {', '    if (false && span_in > 0 && span_out > 0 && frame <= start) {', "key added inside a hold sets off along the start key's handle", 'test_path_key_add_dots.py'),
    ('src/anim/motion_path.cc', '    else if (span_in > 0 && span_out > 0 && plain) {', '    else if (false && span_in > 0 && span_out > 0 && plain) {', 'key added on a plain stretch splits its Bezier exactly (holds, 2s)', 'test_path_key_add_dots.py'),
    ('src/anim/motion_path.cc', '          frame = start + (frame - start) / interval * interval;', '          frame = frame + 0 * interval;', 'key added on a held frame goes on the frame whose pose it holds', 'test_path_key_add_dots.py'),
    ('src/anim/motion_path.cc', '  if (layer && (target.point || target.drawing) && layer->key_whole_drawing && layer->type == LayerType::Vector) {', '  if (false && layer && (target.point || target.drawing) && layer->key_whole_drawing && layer->type == LayerType::Vector) {', "Key Whole Drawing: key added on a point's path keys every channel in place", 'test_path_key_add_dots.py'),
    ('src/ops/motion_path_ops.cc', '      return stroke_brush(C, brush) && path_brush_is_timing(brush);', '      return stroke_brush(C, brush) || true;', "Time Together: shape brush strokes don't retime linked channels", 'test_time_together.py'),
    ('src/ops/motion_path_ops.cc', '    ot.timing = [](Context &C) { return !op_has_arg(C, "what") || op_arg(C, "what") != "SHAPE"; };', '    ot.timing = [](Context &C) { return true || !op_has_arg(C, "what") || op_arg(C, "what") != "SHAPE"; };', "Time Together: Reset Shape doesn't retime linked channels", 'test_time_together.py'),
    ('src/anim/motion_path.cc', '    if (!timing && b.interp == Interp::Smooth) {', '    if (false && !timing && b.interp == Interp::Smooth) {', "Reset Shape keeps a Smooth stretch's timing", 'test_motion_path_stretch_reset.py'),
    ('src/anim/motion_path.cc', '      if (ch->keys[i].interp != Interp::Noisy) {', '      if (true) {', "shape brushes keep a Noisy stretch's noise", 'test_motion_path_sculpt.py'),
    ('src/anim/motion_path.cc', '      ok = ok && map_invertible(m);', '      ok = ok && (true || map_invertible(m));', "Grab refused on a target its keys can't move", 'test_motion_path_pupil.py'),
    ('src/anim/sculpt.cc', '  if (sl.skinned && !sl.settle) {', '  if (false && sl.skinned && !sl.settle) {', 'Dyntopo resample: new points take their skin from their own weights', 'tests/test_sculpt_posed.py'),
    ('src/anim/sculpt.cc', '    if (sl.settle) {\n      const Transform2D to_layer = sl.to_document.inverted();', '    if (false && sl.settle) {\n      const Transform2D to_layer = sl.to_document.inverted();', 'Sculpt settles points on a deformed layer', 'tests/test_sculpt_posed.py'),
    ('src/anim/sculpt.cc', '              if (w[k] > 0.0f) {\n                moved_to[k] = brush.center + rotate(', '              if (true) {\n                moved_to[k] = brush.center + rotate(', 'Twist leaves samples outside the brush exactly where they were', 'tests/test_sculpt_collapse.py'),
    ('src/anim/sculpt.cc', 'const float allowed = SCULPT_DYNTOPO_WIDTH_TOLERANCE * std::max(width[size_t(k)], fit) + 1e-5f;', 'const float allowed = 1e30f;', "Collapse checks the line's width", 'tests/test_sculpt_collapse.py'),
    ('src/anim/sculpt.cc', '        drawing_dissolve_points(drawing, gone);\n        removed += int(gone.size());', '        drawing_dissolve_points(drawing, gone);\n        removed += int(gone.size());\n        spans.clear();', 'Collapse spans take the handles fitted in document space', 'tests/test_sculpt_collapse.py'),
    ('src/anim/sculpt.cc', 'order.push_back({int(alone * 32.0f), uint32_t(i) * 2654435761u, i, alone});', 'order.push_back({int(alone * 32.0f), 0u, i, alone});', 'Collapse tries ties scrambled, so it is not quadratic', 'tests/test_sculpt_collapse.py'),
    ('src/data/vector.cc', '  dissolve_marked(drawing, marked);', '  for (int i = int(marked.size()) - 1; i >= 0; i--) {\n    if (marked[size_t(i)]) {\n      drawing_dissolve_point(drawing, i);\n    }\n  }', 'Dissolve of many points in one pass', 'tests/test_sculpt_collapse.py'),
    ('src/data/vector.cc', '  drawing_remove_curves(drawing, remove_curve);\n  drawing_remove_unused_points(drawing);\n  drawing_groups_prune(drawing);\n}', '  drawing_remove_curves(drawing, remove_curve);\n  drawing_remove_unused_points(drawing);\n}', 'Dissolve prunes grids and point groups as Delete does', 'tests/test_grid.py'),
    ('src/anim/animation.cc', '    if (!kept) {', '    if (true || !kept) {', "Keep Animation looking through actions puts the edited action's frame 0 keys back", 'keep_animation_edited_action'),
    ('src/anim/animation.cc', '  if (doc.action != saved.name || (!saved.name.empty() && doc.action_layer != saved.owner)) {', '  if (doc.action != saved.name) {', 'Keep Animation comes back to the edited action by its bone layer, not just its name', 'keep_animation_edited_action'),
    ('src/data/vector.cc', '    const float tiny = 1e-5f;', '    const float tiny = 0.0f;', 'Align gives setup handles with no length a direction in line, their keys measured again', 'handles_align_keys'),
    ('src/ops/vector_ops.cc', 'if (!positions && cp.aligned && !point && cp.select_in != cp.select_out &&', 'if (false && !positions && cp.aligned && !point && cp.select_in != cp.select_out &&', 'Clear Handles on one handle of an aligned point turns the other into line', 'clear_handles'),
    ('src/anim/deform.cc', "      if (length(off) >= best_off[i]) {\n        /* Farther off than before: halfway back to the nearest place yet (at the last pass, that place). */\n        stored = pass + 1 < passes ? best[i] + (stored - best[i]) * 0.5f : best[i];\n        done = false;\n        continue;\n      }\n      best[i] = stored;\n      best_off[i] = length(off);\n      if (length(off) <= std::max(1e-6f, 1e-5f * length(t.at)) || pass + 1 == passes) {\n        settled[i] = true;\n        continue;\n      }\n      done = false;\n      /* The step back through the stretch here, damped (Levenberg's): where the deform squashes the place nearly flat\n       * the plain inverse would go far for little, and the stretch says nothing true that far away. */\n      const float2 x = (shown[at + 1] - shown[at]) / STRETCH_STEP, y = (shown[at + 2] - shown[at]) / STRETCH_STEP;\n      const float xx = dot(x, x), xy = dot(x, y), yy = dot(y, y);\n      const float big = 0.5f * (xx + yy) + std::sqrt(0.25f * (xx - yy) * (xx - yy) + xy * xy);\n      const float damp = big * 0.0025f;\n      const float m00 = xx + damp, m11 = yy + damp, det = m00 * m11 - xy * xy;\n      float2 step;\n      if (det > 1e-20f && big > 1e-12f) {\n        const float gx = dot(x, off), gy = dot(y, off);\n        step = float2((m11 * gx - xy * gy) / det, (m00 * gy - xy * gx) / det);\n      }\n      else {\n        step = t.unskin.apply_vector(off);\n      }\n      float2 next = stored + step;", "      if (false) {\n        /* Farther off than before: halfway back to the nearest place yet (at the last pass, that place). */\n        stored = pass + 1 < passes ? best[i] + (stored - best[i]) * 0.5f : best[i];\n        done = false;\n        continue;\n      }\n      best[i] = stored;\n      best_off[i] = length(off);\n      if (length(off) <= std::max(1e-6f, 1e-5f * length(t.at)) || pass + 1 == passes) {\n        settled[i] = true;\n        continue;\n      }\n      done = false;\n      /* The step back through the stretch here, damped (Levenberg's): where the deform squashes the place nearly flat\n       * the plain inverse would go far for little, and the stretch says nothing true that far away. */\n      const float2 x = (shown[at + 1] - shown[at]) / STRETCH_STEP, y = (shown[at + 2] - shown[at]) / STRETCH_STEP;\n      const float xx = dot(x, x), xy = dot(x, y), yy = dot(y, y);\n      const float big = 0.5f * (xx + yy) + std::sqrt(0.25f * (xx - yy) * (xx - yy) + xy * xy);\n      const float damp = big * 0.0025f;\n      const float m00 = xx + damp, m11 = yy + damp, det = m00 * m11 - xy * xy;\n      float2 step;\n      if (det > 1e-20f && big > 1e-12f) {\n        const float gx = dot(x, off), gy = dot(y, off);\n        step = float2((m11 * gx - xy * gy) / det, (m00 * gy - xy * gx) / det);\n      }\n      else {\n        step = t.unskin.apply_vector(off);\n      }\n      float2 next = stored + t.unskin.apply_vector(off);", 'Settling a point steps damped where the deform squashes and takes back a step that shows it farther off', 'deform_settle_squashed'),
    ('src/anim/keep_poses.cc', '  const float most = 3.0f * length(off) / std::sqrt(big);', '  const float most = 1e30f;', 'Keep Animation keys a point squashed nearly flat back by at most three times what an unsquashed skin needs', 'keep_animation_squashed'),
    ('src/anim/animation.cc', '    for (int round = 0; round < 4; round++) {', '    for (int round = 0; round < 1; round++) {', 'Action bone actions are played again until what bones posed after them read stops changing (a chain of looks)', 'frame_visit_order'),
]

def build(root):
    return subprocess.run([CMAKE, "--build", "--preset", "release"], cwd=root, capture_output=True,
                          text=True).returncode == 0


def git(root, *args, data=None):
    r = subprocess.run(["git", *args], cwd=root, input=data, capture_output=True)
    if r.returncode:
        raise RuntimeError(f"git {' '.join(args)} in {root}: {r.stderr.decode(errors='replace')}")
    return r.stdout


def lane_roots(root, count):
    parent, name = os.path.split(os.path.abspath(root))
    return [os.path.join(parent, f"{name}-mut{k}") for k in range(1, count)]


def lane_ready(root, lane):
    """Bring a lane worktree to this folder's commit and uncommitted changes, made and built the first time."""
    sha = git(root, "rev-parse", "HEAD").decode().strip()
    if not os.path.isdir(lane):
        git(root, "worktree", "add", "--detach", lane, sha)
        deps = os.path.join(root, "build", "_deps")
        fetched = [f"-DFETCHCONTENT_SOURCE_DIR_{d[:-4].upper()}={os.path.join(deps, d)}"
                   for d in (os.listdir(deps) if os.path.isdir(deps) else []) if d.endswith("-src")]
        if subprocess.run([CMAKE, "--preset", "windows", *fetched], cwd=lane, capture_output=True).returncode:
            raise RuntimeError(f"configure failed in {lane}")
    else:
        git(lane, "checkout", "-f", "--detach", sha)
        git(lane, "clean", "-fdq", "--", "src", "tests", "scripts", "tools")
    diff = git(root, "diff", "--binary", "HEAD")
    if diff:
        git(lane, "apply", "--whitespace=nowarn", "-", data=diff)
    for path in git(root, "ls-files", "--others", "--exclude-standard").decode().splitlines():
        os.makedirs(os.path.dirname(os.path.join(lane, path)) or lane, exist_ok=True)
        shutil.copyfile(os.path.join(root, path), os.path.join(lane, path))
    if not build(lane):
        raise RuntimeError(f"build failed in {lane}")


class Lane:
    def __init__(self, root, number):
        self.root, self.number = root, number
        self.rebuilt = False  # its build holds a native entry's change
        self.last = None      # the file it last changed


def check(lane, i, entry, say):
    """Run one check in a lane: True when its test fails with the fix undone."""
    path, old, new, name, test = entry
    full = os.path.join(lane.root, path)
    if not os.path.exists(os.path.join(lane.root, test)):
        # Older entries name the test without its folder and prefix.
        test = os.path.join("tests", f"test_{test}.py")
    if not os.path.exists(os.path.join(lane.root, test)):
        say(f"{i} {name}: no test {test}; fix the entry")
        return False
    with open(full, encoding="utf-8") as f:
        src = f.read()
    if src.count(old) != 1:
        say(f"{i} {name}: its code is found {src.count(old)} times in {path}; fix the entry")
        return False
    backup = os.path.join(tempfile.gettempdir(), f"oanim_mutation_{os.getpid()}_{lane.number}_{os.path.basename(path)}")
    shutil.copyfile(full, backup)
    try:
        with open(full, "w", encoding="utf-8", newline="") as f:
            f.write(src.replace(old, new))
        lane.last = path
        native = not path.endswith(".py")
        if not native and lane.rebuilt:
            # The build still holds the last native entry's change: a script-only entry runs on a clean one.
            shutil.copyfile(backup, full)
            if not build(lane.root):
                raise RuntimeError(f"the build in {lane.root} with every file put back failed: look before going on")
            lane.rebuilt = False
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write(src.replace(old, new))
        if native:
            lane.rebuilt = True
            if not build(lane.root):
                say(f"{i} {name}: BUILD FAILED with it undone; fix the entry")
                return False
        r = subprocess.run([os.path.join(lane.root, EXE), "--background", "--python", test], cwd=lane.root,
                           capture_output=True, text=True)
        why = [l for l in (r.stdout + r.stderr).splitlines() if "Error" in l or "assert" in l][-1:]
        if r.returncode:
            say(f"{i} {name}: caught {why}")
            return True
        say(f"{i} {name}: NOT CAUGHT by {test}")
        return False
    finally:
        shutil.copyfile(backup, full)
        os.remove(backup)


def main():
    root = os.getcwd()
    args = sys.argv[1:]
    if "--remove-lanes" in args:
        parent, name = os.path.split(root)
        for lane in sorted(d for d in os.listdir(parent) if d.startswith(f"{name}-mut")):
            git(root, "worktree", "remove", "--force", os.path.join(parent, lane))
            print(f"removed {lane}", flush=True)
        return
    count = 1
    if "--lanes" in args:
        at = args.index("--lanes")
        count = max(1, int(args[at + 1]))
        del args[at:at + 2]
    only = set(args) - {"--prepare", "--not-here"}
    todo = [i for i in range(len(PENDING)) if not only or str(i) in only]
    # --not-here: only the lane folders, so this one stays free for editing while they run.
    here = "--not-here" not in args
    lanes = [Lane(root, 0)] if here else []
    extra = lane_roots(root, min(count, len(todo) + (not here)))
    if extra:
        print(f"getting {len(extra)} lane folder(s) ready", flush=True)
        errors = []

        def ready(lane):
            try:
                lane_ready(root, lane)
            except RuntimeError as e:
                errors.append(str(e))

        threads = [threading.Thread(target=ready, args=(lane,)) for lane in extra]
        [t.start() for t in threads]
        [t.join() for t in threads]
        if errors:
            print("\n".join(errors), flush=True)
            sys.exit(2)
        lanes += [Lane(lane, k + 1) for k, lane in enumerate(extra)]
    if "--prepare" in args:
        # Lanes made and built ahead of a run (it brings them up to date again).
        return
    lock = threading.Lock()
    tally = {"caught": 0, "missed": 0}
    broken = []

    def say(line):
        with lock:
            print(line, flush=True)

    def next_for(lane):
        with lock:
            if not todo or broken:
                return None
            # The next check on the file this lane last changed: its rebuild then compiles that one file.
            k = next((k for k, i in enumerate(todo) if PENDING[i][0] == lane.last), 0)
            return todo.pop(k)

    def work(lane):
        try:
            while (i := next_for(lane)) is not None:
                ok = check(lane, i, PENDING[i], say)
                with lock:
                    tally["caught" if ok else "missed"] += 1
            if lane.rebuilt and not build(lane.root):
                raise RuntimeError(f"the build in {lane.root} with every file put back failed: look before going on")
        except RuntimeError as e:
            with lock:
                broken.append(str(e))

    threads = [threading.Thread(target=work, args=(lane,)) for lane in lanes]
    [t.start() for t in threads]
    [t.join() for t in threads]
    if broken:
        print("\n".join(broken), flush=True)
        sys.exit(2)
    print(f"{tally['caught']} caught, {tally['missed']} not", flush=True)
    sys.exit(1 if tally["missed"] else 0)


if __name__ == "__main__":
    main()
