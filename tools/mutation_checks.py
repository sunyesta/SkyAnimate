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
