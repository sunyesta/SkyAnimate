# SkyAnimate MCP server

Lets Claude (or any MCP client) work with the running SkyAnimate: run Python in it, take
screenshots, read its state and press keys.

```
Claude  --MCP over stdio-->  tools/mcp/skyanimate_mcp.py  --JSON lines over 127.0.0.1-->  SkyAnimate
```

The bridge is one Python file using only the standard library. SkyAnimate's side is
`src/wm/remote.cc`: a listener on 127.0.0.1 (this computer only), off until turned on, whose
requests run on the app's main loop between frames.

**Safety.** Every request carries a token, made afresh each time remote control starts and
written with the port to `%LOCALAPPDATA%\SkyAnimate\remote.json` (removed when it stops), where
the bridge reads it. Other programs and web pages can't know it, and a connection that doesn't open
with a request (a browser's HTTP request) is closed before anything is read from it. Everything
the bridge does is something you could do in SkyAnimate's own Python console.

## Setup

1. In SkyAnimate: **Edit > Preferences > Remote Control > Allow Remote Control** (port 7853 by
   default). It stays on across launches until turned off.
2. Register the bridge with Claude Code. This project's `.mcp.json` already does, for sessions
   started in this folder (Claude Code asks to approve it once). Elsewhere:

   ```
   claude mcp add skyanimate -- C:/Python314/python.exe C:/Users/Mary/Documents/Code/MyAnimationApp/SkyAnimate/tools/mcp/skyanimate_mcp.py
   ```

   The bridge finds the port and token in `remote.json`; `SKYANIMATE_PORT` and `SKYANIMATE_TOKEN`
   override them.

## Tools

| Tool | What it does |
| --- | --- |
| `run_python(code)` | Runs code with the `sky` module inside the app, as a console: names stay between calls; the last line's value, anything printed and any traceback come back |
| `screenshot(area, max_size)` | PNG of the window, or of one editor by its tab name (Viewport, Timeline, Properties, ...) |
| `get_state()` | File, unsaved changes, frame, playback, layers, selection, running transform, recent Info reports |
| `send_keys(key, ctrl, shift, alt, area, text)` | A key as if the mouse were over `area`; during G/R/S it goes to the transform. `text` types characters |

## The protocol underneath

One JSON request per line on `127.0.0.1:<port>`, one reply per line:

```
{"id": 1, "token": "<from remote.json>", "method": "run_python", "params": {"code": "sky.data.document.frame_current"}}
{"id": 1, "result": {"output": "", "result": "12", "error": null}}
```

Methods: `run_python`, `screenshot`, `get_state`, `send_keys`, `ping`.
`tests/test_remote_control.py` runs the whole chain headless.
