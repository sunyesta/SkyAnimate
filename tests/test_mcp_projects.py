"""The MCP bridge (tools/mcp/openanimate_mcp.py) with several Open Animate windows open: each publishes
remote/<process id>.json (port, token, process id, open file); the bridge lists the running ones,
picks one by file name, path or process id, uses the main one (remote.json) when none is named,
and skips files a crashed one left behind.

Run with:  OpenAnimate --background --python tests/test_mcp_projects.py
"""

import importlib.util
import json
import os
import pathlib
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
root = pathlib.Path(tempfile.mkdtemp())
os.environ["LOCALAPPDATA"] = str(root)
os.environ.pop("OPENANIMATE_PORT", None)
os.environ.pop("OPENANIMATE_TOKEN", None)
spec = importlib.util.spec_from_file_location("openanimate_mcp", HERE.parent / "tools" / "mcp" / "openanimate_mcp.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

folder = root / "OpenAnimate" / "remote"
folder.mkdir(parents=True)
alive = os.getpid()


def publish(name, pid, port, filepath):
    (folder / name).write_text(json.dumps({"port": port, "token": "t%d" % port, "pid": pid, "filepath": filepath}))


# Nothing running: the usual message.
try:
    bridge.pick("")
    raise AssertionError("nothing is running")
except RuntimeError as e:
    assert "Can't reach Open Animate" in str(e), e

# Two windows, and a file a crashed one left (its process is gone).
publish("a.json", alive, 7001, r"C:\Art\walk.oanim")
publish("b.json", alive, 7002, r"C:\Art\Rig Test.oanim")
publish("c.json", 4000000000, 7003, r"C:\Art\gone.oanim")
running = bridge.instances()
assert sorted(i["port"] for i in running) == [7001, 7002], running

# Named: by file name (with or without .oanim, any case), full path (either slash), or process id.
assert bridge.pick("walk")["port"] == 7001
assert bridge.pick("Walk.OANIM")["port"] == 7001
assert bridge.pick("rig test")["port"] == 7002
assert bridge.pick(r"C:\Art\Rig Test.oanim")["port"] == 7002
assert bridge.pick("C:/Art/rig test.oanim")["port"] == 7002
for bad in ("gone", "nope"):
    try:
        bridge.pick(bad)
        raise AssertionError(bad)
    except RuntimeError as e:
        assert "No open project matches" in str(e) and "walk.oanim" in str(e), e
# The same process id twice is ambiguous: it says so and lists them.
try:
    bridge.pick(str(alive))
    raise AssertionError("ambiguous")
except RuntimeError as e:
    assert "matches several" in str(e), e

# Unnamed with several open and no main one: asks to name one.
try:
    bridge.pick("")
    raise AssertionError("several")
except RuntimeError as e:
    assert "name one as `project`" in str(e), e

# The main one (holding the usual port: remote.json) is used when none is named.
(root / "OpenAnimate" / "remote.json").write_text(json.dumps({"port": 7002, "token": "t7002", "pid": alive}))
assert bridge.pick("")["port"] == 7002
assert [i["main"] for i in sorted(bridge.instances(), key=lambda i: i["port"])] == [False, True]
assert bridge.connection("walk") == (7001, "t7001")

# One window left: it's picked unnamed, main or not.
(root / "OpenAnimate" / "remote.json").unlink()
(folder / "b.json").unlink()
assert bridge.pick("")["port"] == 7001

# An Open Animate from before per-window files: remote.json alone still works.
(folder / "a.json").unlink()
(root / "OpenAnimate" / "remote.json").write_text(json.dumps({"port": 7853, "token": "old"}))
assert bridge.connection("") == (7853, "old")

# The tool list: list_projects, and `project` on the others.
tools = {t["name"]: t for t in bridge.TOOLS}
assert "list_projects" in tools
for name in ("run_python", "screenshot", "get_state", "send_keys"):
    assert "project" in tools[name]["inputSchema"]["properties"], name

print("mcp_projects: ok")
