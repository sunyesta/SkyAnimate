"""Show a PNG sequence in a Media layer of the user's running Open Animate, in place of what its clip shows
(timing, place and settings kept), as one undo step (Replace Media). Their active layer is put back after.

Usage: python swap_clip.py LAYER_NAME FIRST_PNG [--pid PID]
  LAYER_NAME: the Media layer (its name in the Layers editor). FIRST_PNG: the sequence's first image.
  --pid: which Open Animate, when more than one runs (default: the one remote.json names, else the only one).
The clip it replaces is the one at the current frame, else the layer's first.
"""

import glob
import json
import os
import socket
import sys


def swap_code(layer_name, first):
    """The Python the running app is asked to run."""
    return f"""
import oanim
layers = oanim.data.layers
target = [l for l in layers.all() if l.name == {layer_name!r}]
if len(target) != 1 or target[0].type != "MEDIA":
    raise RuntimeError("Need exactly one Media layer named " + {layer_name!r} + ": found %d" % len(target))
media = target[0]
at = media.media_shown()
clip = at[0] if at else media.media_clips[0]["id"]
was = layers.active
try:
    # Older builds checked the active layer before reading layer=, so make it active for the call.
    layers.active = media
    status = oanim.ops.media.replace(layer=str(media.id), clip=str(clip), filepath={first!r}, sequence="1")
finally:
    if was is not None:
        layers.active = was
c = next(c for c in media.media_clips if c["id"] == clip)
print(status, c["filepath"], "sequence" if c["sequence"] else "", c["timing"], "start", c["start"], "length", c["length"])
"""


def remote_info(pid):
    folder = os.path.join(os.environ["LOCALAPPDATA"], "OpenAnimate", "remote")
    if pid:
        return json.load(open(os.path.join(folder, pid + ".json")))
    if os.path.exists(os.path.join(folder, "remote.json")):
        return json.load(open(os.path.join(folder, "remote.json")))
    found = glob.glob(os.path.join(folder, "*.json"))
    if len(found) != 1:
        sys.exit("Say which Open Animate with --pid (running: %s)" % [os.path.basename(f) for f in found])
    return json.load(open(found[0]))


if __name__ == "__main__":
    pid = sys.argv[sys.argv.index("--pid") + 1] if "--pid" in sys.argv else None
    args = [a for a in sys.argv[1:] if not a.startswith("--") and a != pid]
    layer_name, first = args[0], os.path.abspath(args[1])
    remote = remote_info(pid)
    message = {"id": 1, "token": remote["token"], "method": "run_python", "params": {"code": swap_code(layer_name, first)}}
    with socket.create_connection(("127.0.0.1", remote["port"]), timeout=120) as s:
        s.sendall((json.dumps(message) + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            data += s.recv(1 << 20)
    result = json.loads(data).get("result", {})
    print(result.get("output", "").strip() or result)
    if result.get("error"):
        sys.exit(result["error"])
