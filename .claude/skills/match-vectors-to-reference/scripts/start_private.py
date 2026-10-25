"""A private copy of the user's open document to work on: their running app saves a copy (unsaved changes
included, their file untouched), and a separate Open Animate (its own settings) opens it with remote control.

Usage: python start_private.py WORK_DIR [--user-pid PID] [--exe PATH]
Writes WORK_DIR/copy.oanim and WORK_DIR/private.json (the session file for the other scripts), and prints the
private copy's pid: stop it only by that pid (taskkill /PID <pid> /F), never by name.
"""

import glob
import json
import os
import subprocess
import sys
import time

import oa_remote

work = os.path.abspath(sys.argv[1])
os.makedirs(work, exist_ok=True)
user_pid = sys.argv[sys.argv.index("--user-pid") + 1] if "--user-pid" in sys.argv else None
here = os.path.dirname(os.path.abspath(__file__))
repo = os.path.abspath(os.path.join(here, "..", "..", "..", ".."))
exe = sys.argv[sys.argv.index("--exe") + 1] if "--exe" in sys.argv else os.path.join(
    repo, "build", "bin", "RelWithDebInfo", "OpenAnimate.exe")

folder = os.path.join(os.environ["LOCALAPPDATA"], "OpenAnimate", "remote")
if user_pid:
    user = os.path.join(folder, user_pid + ".json")
else:
    found = glob.glob(os.path.join(folder, "*.json"))
    if len(found) != 1:
        sys.exit("Say which Open Animate is the user's with --user-pid (running: %s)" % [os.path.basename(f) for f in found])
    user = found[0]
copy = os.path.join(work, "copy.oanim")
print(oa_remote.run(user, f"import oanim\nprint(oanim.ops.wm.save_copy(filepath={copy!r}))").strip())
if not os.path.exists(copy):
    sys.exit("The copy wasn't saved")

session = os.path.join(work, "private.json")
if os.path.exists(session):
    os.remove(session)
startup = os.path.join(work, "start.py")
with open(startup, "w") as f:
    f.write(f"""import json, os, _oanim, oanim
oanim.ops.wm.open_mainfile(filepath={copy!r}, confirmed="1")
port = _oanim.remote_start(0)
with open({session!r}, "w") as out:
    json.dump({{"pid": os.getpid(), "port": port, "token": _oanim.remote_token()}}, out)
""")
env = dict(os.environ, LOCALAPPDATA=os.path.join(work, "appdata"))
os.makedirs(env["LOCALAPPDATA"], exist_ok=True)
subprocess.Popen([exe, "--python", startup], env=env, stdout=open(os.path.join(work, "private.log"), "w"),
                 stderr=subprocess.STDOUT, creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
for _ in range(120):
    if os.path.exists(session) and os.path.getsize(session) > 0:
        break
    time.sleep(0.5)
else:
    sys.exit("The private copy didn't start (see private.log)")
info = oa_remote.load(session)
# Its own asset library, so its first-run prompt doesn't cover the window.
library = os.path.join(work, "library")
os.makedirs(library, exist_ok=True)
oa_remote.run(session, f"import oanim\noanim.ops.preferences.asset_library_set(directory={library!r})")
state = oa_remote.run(session, "import oanim\nd = oanim.data.document\nprint(d.frame_current, repr(d.action))").strip()
print(f"private copy pid {info['pid']} (stop it with: taskkill /PID {info['pid']} /F); frame and action: {state}")
print("session file:", session)
