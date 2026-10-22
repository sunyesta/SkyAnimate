# Open Animate MCP server

Lets Claude (or any MCP client) work with the running Open Animate: run Python in it, take
screenshots, read its state and press keys.

```
Claude  --MCP over stdio-->  tools/mcp/openanimate_mcp.py  --JSON lines over 127.0.0.1-->  Open Animate
```

The bridge is one Python file using only the standard library. Open Animate's side is
`src/wm/remote.cc`: a listener on 127.0.0.1 (this computer only), off until turned on, whose
requests run on the app's main loop between frames.

**Safety.** Every request carries a token, made afresh each time remote control starts and
written with the port, the process id and the open file to
`%LOCALAPPDATA%\OpenAnimate\remote\<process id>.json` (removed when it stops; a crashed one's is
cleared by the next start), where the bridge reads it. The Open Animate holding the port set in
Preferences also writes `remote.json`.

**Several projects.** Each Open Animate window holds one document, so each open project is its own
instance. The first takes the port in Preferences; others take a free one. `list_projects` names
them all, and every tool takes `project` (a file name like `walk` or `walk.oanim`, a full path, or a
process id) to pick one; without it, the tools reach the main one (holding the usual port), or the
only one running. Other programs and web pages can't know it, and a connection that doesn't open
with a request (a browser's HTTP request) is closed before anything is read from it. Everything
the bridge does is something you could do in Open Animate's own Python console.

## Setup

1. In Open Animate: **Edit > Preferences > Remote Control > Allow Remote Control** (port 7853 by
   default). It stays on across launches until turned off.
2. Register the bridge with Claude Code. This project's `.mcp.json` already does, for sessions
   started in this folder (Claude Code asks to approve it once). Elsewhere:

   ```
   claude mcp add openanimate -- C:/Python314/python.exe C:/Users/Mary/Documents/Code/MyAnimationApp/OpenAnimate/tools/mcp/openanimate_mcp.py
   ```

   The bridge finds each window's port and token in `remote\`; `OPENANIMATE_PORT` and
   `OPENANIMATE_TOKEN` override them.

## Tools

| Tool | What it does |
| --- | --- |
| `list_projects()` | The open Open Animate windows: file, process id, port, and which is the main one |
| `run_python(code)` | Runs code with the `oanim` module inside the app, as a console: names stay between calls; the last line's value, anything printed and any traceback come back |
| `screenshot(area, max_size)` | PNG of the window, or of one editor by its tab name (Viewport, Timeline, Properties, ...) |
| `get_state()` | File, unsaved changes, frame, playback, layers, selection, running transform, recent Info reports |
| `send_keys(key, ctrl, shift, alt, area, text)` | A key as if the mouse were over `area`; during G/R/S it goes to the transform. `text` types characters |

## The protocol underneath

One JSON request per line on `127.0.0.1:<port>`, one reply per line:

```
{"id": 1, "token": "<from remote.json>", "method": "run_python", "params": {"code": "oanim.data.document.frame_current"}}
{"id": 1, "result": {"output": "", "result": "12", "error": null}}
```

Methods: `run_python`, `screenshot`, `get_state`, `send_keys`, `ping`. Every tool above also takes
`project`.
`tests/test_remote_control.py` runs the whole chain headless.
