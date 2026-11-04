"""Mutation checks, batched until a milestone: each proves a test fails without the fix it guards.

Each feature adds an entry to PENDING for every fix it makes: the file, a piece of its code (found exactly once), what
to put there instead to undo the fix, a name, and the test that should then fail. They are run together when the user
says "run milestone checks":

    python tools/mutation_checks.py          # every pending check
    python tools/mutation_checks.py 3 7      # only those

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

CMAKE = (r"C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/"
         r"CMake/CMake/bin/cmake.exe")
EXE = os.path.join("build", "bin", "RelWithDebInfo", "OpenAnimate.exe")

# (file, old, new, name, test)
PENDING = [
    # Motion paths: the Shift drag blend stroke (2026-11-04).
    ("src/ops/motion_path_ops.cc", 'std::vector<int> frames = path_frames_parse(op_arg(C, "frames"));',
     "std::vector<int> frames;", "blend takes a stroke's frames", "tests/test_motion_path_timing.py"),
    ("src/anim/motion_path.cc", "  for (const int frame : frames) {\n    changed += motion_path_timing_blend(",
     "  for (const int frame : std::vector<int>{frames.front()}) {\n    changed += motion_path_timing_blend(",
     "a blend on each frame of the stroke", "tests/test_motion_path_timing.py"),
    # Key Whole Drawing (2026-11-04).
    ("src/anim/animation.cc", "      key_timing_set(ch.set_key(frame, held), *timing);", "      (void)held;",
     "every channel keyed where any is", "tests/test_key_whole_drawing.py"),
    ("src/anim/animation.cc", "        if (settle && key_timing(*k) != *timing) {", "        if (false) {",
     "turned on, a frame's keys take the timing most have", "tests/test_key_whole_drawing.py"),
    ("src/anim/animation.cc", "count - it->second.count > channels_gone) {", "false) {",
     "one key deleted takes the frame from all", "tests/test_key_whole_drawing.py"),
    ("src/anim/animation.cc", "count - it->second.count > channels_gone) {", "count - it->second.count > 0) {",
     "a point's channels gone delete no frames", "tests/test_key_whole_drawing.py"),
    ("src/anim/animation.cc", "    if (p.detail.level > 0) {\n      continue;\n    }\n    f(p.pos_keys",
     "    f(p.pos_keys", "detail points keep no keys", "tests/test_detail_levels.py"),
    ("src/anim/motion_path.cc", "    else if (layer->has_drawing() && !layer->key_whole_drawing) {",
     "    else if (layer->has_drawing()) {", "no point paths for a whole drawing", "tests/test_key_whole_drawing.py"),
    ("src/anim/motion_path.cc", "      f(ch);\n      found = true;\n      return true;",
     "      f(ch);\n      found = true;\n      return false;", "timing edits reach every channel",
     "tests/test_key_whole_drawing.py"),
    ("src/data/layer.hh", "  bool key_whole_drawing = true;", "  bool key_whole_drawing = false;",
     "on for new vector layers", "tests/test_key_whole_drawing.py"),
    ("src/io/oanim_file.cc", '      {"key_whole_drawing", layer.key_whole_drawing},\n', "",
     "saved with the file", "tests/test_key_whole_drawing.py"),
    ("src/editors/timeline.cc", 'add(whole ? "Drawing" : "Points", owned', 'add("Points", owned',
     "one Drawing row", "tests/test_key_whole_drawing.py"),
    # Motion paths: the timing views' key diamonds, clicked and right-clicked (2026-11-04).
    ("src/anim/motion_path.cc", "    key_settings_apply(key, s);\n  });", "    (void)s;\n  });",
     "the menu sets the key's interpolation", "tests/test_motion_path_key_menu.py"),
    ("src/anim/motion_path.cc", "  if (count > 0 || t.drawing) {\n    return count;", "  if (true) {\n    return count;",
     "a group's key moves it there", "tests/test_motion_path_key_menu.py"),
    ("src/anim/motion_path.cc", "    removals.push_back([&ch, frame] { ch.remove_key(frame); });", "    (void)ch;",
     "Delete Key deletes it", "tests/test_motion_path_key_menu.py"),
    ("src/anim/motion_path.cc", "    /* A drawing keyed as a whole: all its points' keys there. */\n    filter.kinds = KEY_POINTS;",
     "    filter.kinds = 0;", "a click selects a whole drawing's keys", "tests/test_motion_path_key_menu.py"),
    ("src/anim/motion_path.cc", "    return drawing_row && motion_path_stretches(doc, t).empty();", "    return false;",
     "no empty row for a whole drawing's layer", "tests/test_key_whole_drawing.py"),
    ("src/anim/motion_path.cc", "    if (!motion_path_stretches(doc, t).empty()) {\n      r_target = t;\n      break;",
     "    if (false) {\n      r_target = t;\n      break;", "the graph shows the first row with timing",
     "tests/test_key_whole_drawing.py"),
]


def build():
    return subprocess.run([CMAKE, "--build", "--preset", "release"], capture_output=True, text=True).returncode == 0


def main():
    only = set(sys.argv[1:])
    rebuilt = False
    caught = missed = 0
    for i, (path, old, new, name, test) in enumerate(PENDING):
        if only and str(i) not in only:
            continue
        with open(path, encoding="utf-8") as f:
            src = f.read()
        if src.count(old) != 1:
            print(f"{i} {name}: its code is found {src.count(old)} times in {path}; fix the entry", flush=True)
            missed += 1
            continue
        backup = os.path.join(tempfile.gettempdir(), f"oanim_mutation_{os.getpid()}_{os.path.basename(path)}")
        shutil.copyfile(path, backup)
        try:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(src.replace(old, new))
            native = not path.endswith(".py")
            if native:
                rebuilt = True
                if not build():
                    print(f"{i} {name}: BUILD FAILED with it undone; fix the entry", flush=True)
                    missed += 1
                    continue
            r = subprocess.run([EXE, "--background", "--python", test], capture_output=True, text=True)
            why = [l for l in (r.stdout + r.stderr).splitlines() if "Error" in l or "assert" in l][-1:]
            if r.returncode:
                caught += 1
                print(f"{i} {name}: caught {why}", flush=True)
            else:
                missed += 1
                print(f"{i} {name}: NOT CAUGHT by {test}", flush=True)
        finally:
            shutil.copyfile(backup, path)
            os.remove(backup)
    if rebuilt and not build():
        print("The build with every file put back failed: look before going on", flush=True)
        sys.exit(2)
    print(f"{caught} caught, {missed} not", flush=True)
    sys.exit(1 if missed else 0)


if __name__ == "__main__":
    main()
