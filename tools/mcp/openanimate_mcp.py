"""MCP server for the running Open Animate app (stdio, standard library only).

Claude (or any MCP client) starts this script; it speaks the Model Context Protocol on stdin and
stdout and passes each tool call to Open Animate's remote control, which must be on (Edit >
Preferences > Remote Control) in a running Open Animate. See tools/mcp/README.md.

Tools: list_projects, run_python, screenshot, get_state, send_keys. Each running Open Animate writes
its port, a token (new each time remote control starts), its process id and its open file to
%LOCALAPPDATA%/OpenAnimate/remote/<process id>.json, so every open project can be reached: the tools
take `project` (a file name, a path or a process id). Without it they reach the one in remote.json
(the Open Animate holding the port Preferences asks for), or the only one running. OPENANIMATE_PORT
and OPENANIMATE_TOKEN override all of it.
"""

import ctypes
import glob
import json
import os
import socket
import sys

REMOTE_FILE = os.path.join(os.environ.get("LOCALAPPDATA", ""), "OpenAnimate", "remote.json")
# One file per running Open Animate (remote/<process id>.json).
INSTANCES_DIR = os.path.join(os.environ.get("LOCALAPPDATA", ""), "OpenAnimate", "remote")
# A build from before the rename (SkyAnimate) writes it here.
LEGACY_REMOTE_FILE = os.path.join(os.environ.get("LOCALAPPDATA", ""), "SkyAnimate", "remote.json")
SERVER_INFO = {"name": "openanimate", "version": "0.1.0"}
NOT_RUNNING = ("Can't reach Open Animate. Is it running, with Edit > Preferences > Remote Control > Allow "
               "Remote Control on?")

PROJECT = {"type": "string",
           "description": ("Which open project: its file name (\"walk\" or \"walk.oanim\"), its path, or the "
                           "Open Animate's process id (see list_projects). Empty: the main one")}

TOOLS = [
    {
        "name": "list_projects",
        "description": (
            "The Open Animate windows running with remote control on: each one's open file (empty for a new "
            "document), process id and port, and which is the main one (reached when a tool names no "
            "project). Pass a file name or process id as `project` to the other tools to work with that "
            "one; several can be used in turn (copy a rig from one file into another)."),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "run_python",
        "description": (
            "Run Python inside the running Open Animate, on its main loop, with the `oanim` module (shaped "
            "like Blender's bpy: oanim.data.document, oanim.data.layers, oanim.ops.<category>.<name>(...), "
            "oanim.data.simulation). Like a console: names stay defined between calls, and the value of a "
            "last line that is an expression comes back, with anything printed and any traceback. "
            "Operators push undo steps; plain data edits are recorded when the frame changes."),
        "inputSchema": {
            "type": "object",
            "properties": {"code": {"type": "string", "description": "Python code to run"}, "project": PROJECT},
            "required": ["code"],
        },
    },
    {
        "name": "screenshot",
        "description": (
            "A picture of the Open Animate window as it is drawn now, or of one editor in it (its tab "
            "name, e.g. Viewport, Timeline, Properties, Layers, Info, Console). Scaled down to fit "
            "max_size pixels."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "area": {"type": "string", "description": "An editor's name; empty for the whole window"},
                "max_size": {"type": "integer", "description": "Longest side in pixels (default 1600)"},
                "project": PROJECT,
            },
        },
    },
    {
        "name": "get_state",
        "description": (
            "Open Animate's state: open file and unsaved changes, current frame and range, playback, "
            "layers and the selection (selected bones or points of the active layer), whether a G/R/S "
            "transform is running, and the recent Info reports (operators run, warnings, errors)."),
        "inputSchema": {"type": "object", "properties": {"project": PROJECT}},
    },
    {
        "name": "send_keys",
        "description": (
            "Press a key (Blender names: A, G, S, X, RET, ESC, SPACE, DEL, F3, LEFT_ARROW, ...) as if "
            "the mouse were over `area` (default Viewport), with modifiers. During a G/R/S transform it "
            "goes to the transform. `text` types characters next frame (numbers into a transform, or a "
            "text field)."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "key": {"type": "string"},
                "ctrl": {"type": "boolean"},
                "shift": {"type": "boolean"},
                "alt": {"type": "boolean"},
                "area": {"type": "string", "description": "Editor the mouse is over (default Viewport)"},
                "text": {"type": "string", "description": "Characters to type"},
                "project": PROJECT,
            },
        },
    },
]


def process_alive(pid):
    """Whether process `pid` still runs (a file a crashed Open Animate left behind doesn't count)."""
    if not pid:
        return False
    if os.name != "nt":
        try:
            os.kill(pid, 0)
            return True
        except OSError:
            return False
    kernel = ctypes.windll.kernel32
    handle = kernel.OpenProcess(0x1000, False, int(pid))  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return False
    code = ctypes.c_ulong()
    alive = kernel.GetExitCodeProcess(handle, ctypes.byref(code)) and code.value == 259  # STILL_ACTIVE
    kernel.CloseHandle(handle)
    return bool(alive)


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def instances():
    """Every running Open Animate with remote control on: dicts of port, token, pid, filepath, main."""
    main = read_json(REMOTE_FILE) or read_json(LEGACY_REMOTE_FILE) or {}
    found = []
    for path in sorted(glob.glob(os.path.join(INSTANCES_DIR, "*.json"))):
        info = read_json(path)
        if not info or not process_alive(info.get("pid")):
            continue
        info["main"] = info.get("port") == main.get("port") and info.get("token") == main.get("token")
        found.append(info)
    if not found and main.get("port"):
        # An Open Animate from before per-instance files.
        found.append(dict(main, main=True, filepath=main.get("filepath", "")))
    return found


def describe(info):
    name = info.get("filepath") or "(new document)"
    return "%s (process %s)%s" % (name, info.get("pid", "?"), ", main" if info.get("main") else "")


def pick(project):
    """The instance `project` names (file name, path or process id), or the main or only one."""
    running = instances()
    if not running:
        raise RuntimeError(NOT_RUNNING)
    if not project:
        for info in running:
            if info.get("main"):
                return info
        if len(running) == 1:
            return running[0]
        raise RuntimeError("Several Open Animate windows are open; name one as `project`: " +
                           "; ".join(describe(i) for i in running))
    key = str(project).strip().lower()

    def matches(info):
        path = (info.get("filepath") or "").replace("/", "\\").lower()
        base = os.path.basename(path)
        stem = os.path.splitext(base)[0]
        return key in (str(info.get("pid")), path, base, stem) or (key and key.replace("/", "\\") == path)

    hits = [info for info in running if matches(info)]
    if len(hits) == 1:
        return hits[0]
    if not hits:
        raise RuntimeError("No open project matches %r. Open: %s" % (project, "; ".join(describe(i) for i in running)))
    raise RuntimeError("%r matches several; use a process id: %s" % (project, "; ".join(describe(i) for i in hits)))


def connection(project=""):
    """The port and token of the Open Animate `project` names (see pick)."""
    port, token = os.environ.get("OPENANIMATE_PORT"), os.environ.get("OPENANIMATE_TOKEN")
    if port is None or token is None:
        info = pick(project)
        port = port or info.get("port")
        token = token or info.get("token")
    return int(port), token


def call_app(method, params, project=""):
    """One request to Open Animate's remote control; its result, or raises RuntimeError."""
    port, token = connection(project)
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.settimeout(40)
            request = {"id": 1, "token": token, "method": method, "params": params}
            s.sendall((json.dumps(request) + "\n").encode("utf-8"))
            data = b""
            while not data.endswith(b"\n"):
                chunk = s.recv(1 << 20)
                if not chunk:
                    break
                data += chunk
    except OSError:
        raise RuntimeError(NOT_RUNNING)
    if not data:
        raise RuntimeError("Open Animate closed the connection (an old token? Turn remote control off and on)")
    reply = json.loads(data.decode("utf-8"))
    if "error" in reply:
        raise RuntimeError(reply["error"])
    return reply["result"]


def text(t):
    return {"type": "text", "text": t}


def call_tool(name, args):
    """The MCP content for a tool call, and whether it is an error."""
    project = args.pop("project", "") if isinstance(args, dict) else ""
    if name == "list_projects":
        running = instances()
        if not running:
            return [text(NOT_RUNNING)], True
        listed = [{"file": i.get("filepath", ""), "pid": i.get("pid"), "port": i.get("port"), "main": i.get("main")}
                  for i in running]
        return [text(json.dumps(listed, indent=2))], False
    if name == "run_python":
        r = call_app("run_python", {"code": args.get("code", "")}, project)
        parts = []
        if r.get("output"):
            parts.append(r["output"].rstrip("\n"))
        if r.get("result") is not None:
            parts.append(r["result"])
        if r.get("error"):
            parts.append(r["error"].rstrip("\n"))
        return [text("\n".join(parts) if parts else "(no output)")], bool(r.get("error"))
    if name == "screenshot":
        params = {"area": args.get("area", "")}
        if "max_size" in args:
            params["max_size"] = int(args["max_size"])
        r = call_app("screenshot", params, project)
        return [{"type": "image", "data": r["png"], "mimeType": "image/png"}], False
    if name == "get_state":
        return [text(json.dumps(call_app("get_state", {}, project), indent=2))], False
    if name == "send_keys":
        params = dict(args)
        params.setdefault("area", "Viewport")
        r = call_app("send_keys", params, project)
        return [text(json.dumps(r))], False
    raise RuntimeError("Unknown tool " + name)


def handle(message):
    """The reply to one JSON-RPC message, or None for a notification."""
    method = message.get("method")
    mid = message.get("id")
    if mid is None:
        return None  # notifications (initialized, cancelled) need no reply
    if method == "initialize":
        version = message.get("params", {}).get("protocolVersion", "2025-06-18")
        result = {"protocolVersion": version, "capabilities": {"tools": {}}, "serverInfo": SERVER_INFO,
                  "instructions": "Drives the running Open Animate app. Use get_state and screenshot to look, "
                                  "run_python to act through the oanim API, send_keys for shortcuts. With "
                                  "several open, list_projects names them; pass `project` to pick one."}
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": TOOLS}
    elif method == "tools/call":
        params = message.get("params", {})
        try:
            content, is_error = call_tool(params.get("name"), params.get("arguments") or {})
        except RuntimeError as e:
            content, is_error = [text(str(e))], True
        result = {"content": content, "isError": is_error}
    else:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "Unknown method " + str(method)}}
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def main():
    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except ValueError:
            continue
        reply = handle(message)
        if reply is not None:
            stdout.write((json.dumps(reply) + "\n").encode("utf-8"))
            stdout.flush()


if __name__ == "__main__":
    main()
