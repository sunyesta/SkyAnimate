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
    ('src/ops/rig_ops.cc', '              if (copied.any()) {', '              if (false) {',
     'Apply Pose as Rest: a bone a Copy constraint moves rests as it shows', 'apply_rest_copy'),
    ('src/anim/keep_poses.cc', 'u += was.angle - b.setup_angle + copied.angle; }', 'u += was.angle - b.setup_angle; }',
     "a Copy Angle bone's keys make up for its target's rest moving", 'apply_rest_copy'),
    ('src/anim/keep_poses.cc', 'rotate(t, was.angle) + copied.position;', 'rotate(t, was.angle);',
     "a Copy Position bone's keys make up for its target's rest moving", 'apply_rest_copy'),
    ('src/ops/rig_ops.cc', 'const float turn = was_angle - b.setup_angle + copied.angle;', 'const float turn = was_angle - b.setup_angle;',
     "a Copy Angle bone with no keys is held where its target's rest moved", 'apply_rest_copy'),
    ('src/anim/keep_poses.cc', 'setup.angle - it->second.angle - own.angle', 'setup.angle - it->second.angle',
     "a bone copying a bone that copies makes up only for the turn its target's keys change by", 'apply_rest_copy'),
    ('src/ops/rig_ops.cc', 'rotate(was_position + copied.position - b.setup_position', 'rotate(was_position - b.setup_position',
     "a Copy Position bone with no keys is held where its target's rest moved", 'apply_rest_copy'),
    ('src/anim/motion_path.cc', '    if (p.frame <= 0) {\n      ch.keys.resize(1);', '    if (true) {\n      ch.keys.resize(1);',
     "a value placed on a path keeps the setup actions add their change from", 'motion_path_control_actions'),
    ('src/editors/timeline.cc', '    if (allowed && l.key_together) {\n      add(l.name, under, layer_keys, depth, "", false,',
     '    if (false) {\n      add(l.name, under, layer_keys, depth, "", false,',
     "a layer keying together is a chained row that never opens", 'timeline_chain'),
    ('src/editors/timeline.cc', '  return row.picked_frames && !row.picked_frames->contains(frame);', '  return false;',
     "a folded or chained row's frame without the picked bones' keys is hollow", 'timeline_chain'),
    ('src/editors/timeline.cc', '      if (s.keys > s.fillers) {\n        out->insert(frame);',
     '      if (s.keys > 0) {\n        out->insert(frame);',
     "a filler of a picked bone doesn't make its frame full", 'timeline_chain'),
    ('src/editors/timeline.cc', '    return chain != Chain::None ||', '    return false ||',
     "Alt click picks a stretch on a chained row", 'timeline_chain'),
    ('src/editors/timeline.cc', 'fuzzy_match(row.name, search) || chain_holds_match(row, search));',
     'fuzzy_match(row.name, search));', "a search finds a chained row by a bone inside it", 'timeline_chain'),
    ('src/editors/timeline.cc', '    else if (row.chain != ChannelRow::Chain::None) {\n      /* A chained row never opens',
     '    else if (false) {\n      /* A chained row never opens',
     "an unkeyed chained layer is listed with All", 'timeline_filters'),
    ('src/anim/motion_path.cc', '  if (!group && point_keyed_whole(doc, target)) {', '  if (false) {',
     "a point keyed with its whole drawing is timed as the drawing", 'timeline_chain'),
    ('src/anim/motion_path.cc', '    else if (layer->has_drawing()) {',
     '    else if (layer->has_drawing() && !layer->key_whole_drawing) {',
     "a drawing keyed whole shows its picked points' paths", 'timeline_chain'),
    ('src/editors/timeline.cc', '    else {\n      row.filter.kinds = 0;\n    }', '    else {\n    }',
     "an open folder (Bones, a bone, Points) is a heading with no keys of its own", 'timeline_tree'),
    ('src/anim/animation.cc', '      owner_for_each_channel(*owner, clear);\n    }\n    return count;',
     '      owner_for_each_channel(*owner, clear);\n    }',
     "Clear Timeline in an action leaves the mainline alone", 'timeline_clear'),
    ('src/anim/motion_path.cc', 'return angle != 0.0f ? rotate(v, angle) : v;', 'return v;',
     "a bone's moves pasted onto a point go the same way on screen", 'motion_path_copy_paste'),
    ('src/anim/motion_path.cc', 'return flip && name == "angle" ? -v : v;', 'return v;',
     "Paste Flipped turns the other way", 'motion_path_copy_paste'),
    ('src/anim/motion_path.cc', 'return flip ? float2{-v.x, v.y} : v;', 'return v;',
     "Paste Flipped mirrors left to right", 'motion_path_copy_paste'),
    ('src/anim/motion_path.cc', '  if (how == PathPaste::Timing && !motion_path_timing_owner(doc, target).same(target)) {',
     '  if (false) {', "Timing Only refuses a point timed as its drawing", 'motion_path_copy_paste'),
    ('src/ops/motion_path_ops.cc', '(op_has_arg(C, "layer") || (C.wm && C.wm->path_under));', '(op_has_arg(C, "layer"));',
     "Ctrl C and Ctrl V work over a path", 'motion_path_copy_paste'),
    ('src/anim/motion_path.cc', 'const bool shared = !motion_path_timing_owner(doc, target).same(target);',
     'const bool shared = false;', "dragging a whole drawing's point's dot retimes the drawing", 'key_whole_drawing'),
    ('src/anim/animation.cc', '  layer_for_each_channel(document_camera(doc), clear);\n  return count;', '  return count;',
     "Clear Timeline on the mainline clears the camera's keys too", 'timeline_clear'),
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
    # On disk before the source changes: a power cut mid-check once left a source file zeroed with no copy to go back to.
    with open(backup, "rb+") as f:
        os.fsync(f.fileno())
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
