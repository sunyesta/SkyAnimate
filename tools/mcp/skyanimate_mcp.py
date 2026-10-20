"""MCP server for the running SkyAnimate app (stdio, standard library only).

Claude (or any MCP client) starts this script; it speaks the Model Context Protocol on stdin and
stdout and passes each tool call to SkyAnimate's remote control, which must be on (Edit >
Preferences > Remote Control) in a running SkyAnimate. See tools/mcp/README.md.

Tools: run_python, screenshot, get_state, send_keys. SkyAnimate writes its port and a token (new
each time remote control starts) to %LOCALAPPDATA%/SkyAnimate/remote.json; SKYANIMATE_PORT and
SKYANIMATE_TOKEN override them.
"""

import json
import os
import socket
import sys

REMOTE_FILE = os.path.join(os.environ.get("LOCALAPPDATA", ""), "SkyAnimate", "remote.json")
SERVER_INFO = {"name": "skyanimate", "version": "0.1.0"}
NOT_RUNNING = ("Can't reach SkyAnimate. Is it running, with Edit > Preferences > Remote Control > Allow "
               "Remote Control on?")

TOOLS = [
    {
        "name": "run_python",
        "description": (
            "Run Python inside the running SkyAnimate, on its main loop, with the `sky` module (shaped "
            "like Blender's bpy: sky.data.document, sky.data.layers, sky.ops.<category>.<name>(...), "
            "sky.data.simulation). Like a console: names stay defined between calls, and the value of a "
            "last line that is an expression comes back, with anything printed and any traceback. "
            "Operators push undo steps; plain data edits are recorded when the frame changes."),
        "inputSchema": {
            "type": "object",
            "properties": {"code": {"type": "string", "description": "Python code to run"}},
            "required": ["code"],
        },
    },
    {
        "name": "screenshot",
        "description": (
            "A picture of the SkyAnimate window as it is drawn now, or of one editor in it (its tab "
            "name, e.g. Viewport, Timeline, Properties, Layers, Info, Console). Scaled down to fit "
            "max_size pixels."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "area": {"type": "string", "description": "An editor's name; empty for the whole window"},
                "max_size": {"type": "integer", "description": "Longest side in pixels (default 1600)"},
            },
        },
    },
    {
        "name": "get_state",
        "description": (
            "SkyAnimate's state: open file and unsaved changes, current frame and range, playback, "
            "layers and the selection (selected bones or points of the active layer), whether a G/R/S "
            "transform is running, and the recent Info reports (operators run, warnings, errors)."),
        "inputSchema": {"type": "object", "properties": {}},
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
            },
        },
    },
]


def connection():
    """SkyAnimate's port and token: from remote.json, which it writes while remote control is on."""
    port, token = os.environ.get("SKYANIMATE_PORT"), os.environ.get("SKYANIMATE_TOKEN")
    if port is None or token is None:
        try:
            with open(REMOTE_FILE, encoding="utf-8") as f:
                published = json.load(f)
        except (OSError, ValueError):
            raise RuntimeError(NOT_RUNNING)
        port = port or published.get("port")
        token = token or published.get("token")
    return int(port), token


def call_app(method, params):
    """One request to SkyAnimate's remote control; its result, or raises RuntimeError."""
    port, token = connection()
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
        raise RuntimeError("SkyAnimate closed the connection (an old token? Turn remote control off and on)")
    reply = json.loads(data.decode("utf-8"))
    if "error" in reply:
        raise RuntimeError(reply["error"])
    return reply["result"]


def text(t):
    return {"type": "text", "text": t}


def call_tool(name, args):
    """The MCP content for a tool call, and whether it is an error."""
    if name == "run_python":
        r = call_app("run_python", {"code": args.get("code", "")})
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
        r = call_app("screenshot", params)
        return [{"type": "image", "data": r["png"], "mimeType": "image/png"}], False
    if name == "get_state":
        return [text(json.dumps(call_app("get_state", {}), indent=2))], False
    if name == "send_keys":
        params = dict(args)
        params.setdefault("area", "Viewport")
        r = call_app("send_keys", params)
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
                  "instructions": "Drives the running SkyAnimate app. Use get_state and screenshot to look, "
                                  "run_python to act through the sky API, send_keys for shortcuts."}
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
