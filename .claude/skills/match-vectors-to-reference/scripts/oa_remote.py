"""Talking to a running Open Animate over its remote control.

A session file (JSON: {"port", "token", "pid"}) names which one: start_private.py writes one for the private
copy; for the user's own app use their %LOCALAPPDATA%/OpenAnimate/remote/<pid>.json (read only, until they
approve applying edits).
"""

import json
import os
import socket
import time


def load(session):
    with open(session) as f:
        return json.load(f)


def call(session, method, **params):
    info = load(session) if isinstance(session, str) else session
    message = {"id": 1, "token": info["token"], "method": method, "params": params}
    with socket.create_connection(("127.0.0.1", info["port"]), timeout=300) as s:
        s.sendall((json.dumps(message) + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            data += chunk
    return json.loads(data)


def run(session, code):
    """Runs Python in the app; returns what it printed. Raises with the app's error."""
    r = call(session, "run_python", code=code)
    if r.get("error"):
        raise RuntimeError(r["error"])
    result = r.get("result", {})
    if result.get("error"):
        raise RuntimeError(result["error"])
    return result.get("output", "")


def run_json(session, code):
    """Runs Python that prints one JSON value last (print(json.dumps(...))); returns it."""
    out = run(session, code).strip().splitlines()
    return json.loads(out[-1]) if out else None


def wait_file(path, timeout=30.0):
    """A render to a file is done by the app on its next frame: wait for the file."""
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(path) and os.path.getsize(path) > 0:
            time.sleep(0.15)
            return True
        time.sleep(0.1)
    return False
