"""Remote control (src/wm/remote.cc) and the MCP bridge (tools/mcp/openanimate_mcp.py): requests
answered directly, over a socket, and through the bridge speaking MCP, all headless.

Run with:  OpenAnimate --background --python tests/test_remote_control.py
"""

import json
import os
import pathlib
import socket
import subprocess
import sys
import threading
import time

import _oanim
import oanim

doc = oanim.data.document


def ask(method, **params):
    return json.loads(_oanim.remote_dispatch(json.dumps({"id": 7, "method": method, "params": params})))


# --- Requests answered directly ----------------------------------------------------------------
r = ask("run_python", code="x = 40\nprint('hello')\nx + 2")
assert r["id"] == 7 and r["result"] == {"output": "hello\n", "result": "42", "error": None}, r
assert ask("run_python", code="x")["result"]["result"] == "40", "names stay between calls, as in a console"
r = ask("run_python", code="1 / 0")["result"]
assert r["result"] is None and "ZeroDivisionError" in r["error"], r
assert "_remote.py" not in r["error"], "the traceback starts at the code sent"
assert ask("run_python", code="oanim.data.document.frame_current = 12")["result"]["error"] is None
state = ask("get_state")["result"]
assert state["frame"] == 12 and state["active_layer"]["name"] == "Layer 1", state
assert "unsaved_changes" in state and isinstance(state["recent_reports"], list)
assert ask("send_keys", key="i", area="Viewport")["result"]["handled"], "I keys the active layer"
assert ask("screenshot")["error"].startswith("No window"), "no screenshots without a window"
assert "Unknown method" in ask("fly")["error"]
assert "JSON" in json.loads(_oanim.remote_dispatch("not json"))["error"]
assert ask("ping")["result"] == {"pong": True}

# --- Over a socket: the listener queues requests; the main loop (here, this loop) answers -------
port = _oanim.remote_start(0)
token = _oanim.remote_token()
assert port > 0 and _oanim.remote_port() == port and len(token) == 32


def pump(until, seconds=20):
    end = time.time() + seconds
    while not until() and time.time() < end:
        _oanim.remote_process()
        time.sleep(0.005)
    assert until(), "timed out"


replies = []


def client():
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        for i, (method, params) in enumerate([("ping", {}), ("run_python", {"code": "6 * 7"})]):
            s.sendall((json.dumps({"id": i, "token": token, "method": method, "params": params}) + "\n").encode())
            data = b""
            while not data.endswith(b"\n"):
                data += s.recv(65536)
            replies.append(json.loads(data))


t = threading.Thread(target=client)
t.start()
pump(lambda: not t.is_alive())
assert [r["id"] for r in replies] == [0, 1] and replies[1]["result"]["result"] == "42", replies


# Without the token, or opening the way a browser does (an HTTP request with a JSON body), the
# connection is closed and nothing runs.
def refused(first_lines):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        s.sendall(first_lines.encode())
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                return data
            data += chunk


sneaky = {"id": 1, "method": "run_python", "params": {"code": "oanim.data.document.frame_current = 99"}}
attempts = {
    "no token": json.dumps(sneaky) + "\n",
    "web page": "POST / HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\n\r\n"
                + json.dumps(dict(sneaky, token="guess")) + "\n",
}
ran = {}
for name, lines in attempts.items():
    t = threading.Thread(target=lambda n=name, l=lines: ran.__setitem__(n, refused(l)))
    t.start()
    pump(lambda: not t.is_alive())
assert b"Wrong or missing token" in ran["no token"] and ran["web page"] == b"", ran
assert doc.frame_current == 12, "neither ran anything"

# --- Through the MCP bridge, as Claude would use it ----------------------------------------------
bridge = pathlib.Path(__file__).resolve().parent.parent / "tools" / "mcp" / "openanimate_mcp.py"
python = os.path.join(sys.base_prefix, "python.exe")
messages = [
    {"jsonrpc": "2.0", "id": 1, "method": "initialize",
     "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}},
    {"jsonrpc": "2.0", "method": "notifications/initialized"},
    {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
    {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
     "params": {"name": "run_python", "arguments": {"code": "oanim.data.document.frame_current"}}},
    {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "get_state", "arguments": {}}},
    {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "screenshot", "arguments": {}}},
]
env = dict(os.environ, OPENANIMATE_PORT=str(port), OPENANIMATE_TOKEN=token)
out = {}


def run_bridge():
    p = subprocess.run([python, str(bridge)], input="".join(json.dumps(m) + "\n" for m in messages),
                       capture_output=True, text=True, env=env, timeout=60)
    out["stdout"], out["stderr"], out["code"] = p.stdout, p.stderr, p.returncode


t = threading.Thread(target=run_bridge)
t.start()
pump(lambda: not t.is_alive(), seconds=60)
assert out["code"] == 0, out
answers = {m["id"]: m for m in map(json.loads, out["stdout"].splitlines())}
assert answers[1]["result"]["serverInfo"]["name"] == "openanimate"
assert {tool["name"] for tool in answers[2]["result"]["tools"]} == {"list_projects", "run_python", "screenshot", "get_state", "send_keys"}
assert answers[3]["result"]["content"][0]["text"] == "12" and not answers[3]["result"]["isError"], answers[3]
assert json.loads(answers[4]["result"]["content"][0]["text"])["frame"] == 12
assert answers[5]["result"]["isError"], "the screenshot's error comes back as a tool error"

# --- Stopped: nothing listens --------------------------------------------------------------------
_oanim.remote_stop()
assert _oanim.remote_port() == 0
try:
    socket.create_connection(("127.0.0.1", port), timeout=1).close()
    raise AssertionError("still listening")
except OSError:
    pass
# Without Open Animate, the bridge says how to turn it on.
p = subprocess.run([python, str(bridge)], input=json.dumps(messages[4]) + "\n", capture_output=True, text=True,
                   env=env, timeout=30)
reply = json.loads(p.stdout)
assert reply["result"]["isError"] and "Allow Remote Control" in reply["result"]["content"][0]["text"], reply

print("Remote control tests passed")
