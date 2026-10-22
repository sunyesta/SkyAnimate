#pragma once

/* Remote control: lets a program on this computer (the MCP bridge in tools/mcp, so Claude can work
 * with the running app) run Python in Open Animate, take screenshots, read its state and press keys.
 *
 * A thread listens on 127.0.0.1 only (never other computers), off unless turned on in
 * Preferences. Each request carries a token made afresh each time listening starts and written,
 * with the port, the process id and the open file, to %LOCALAPPDATA%/OpenAnimate/remote/<process
 * id>.json (one per running Open Animate, so the bridge can reach each open project), and to
 * remote.json by the one holding the port Preferences asks for (others take a free port). Other
 * programs and web pages can't know it, and a connection that doesn't open with a request (a
 * browser's HTTP request line) is closed unread. Each connection sends one JSON request per line,
 *   {"id": 1, "token": "...", "method": "run_python", "params": {"code": "..."}}
 * and gets one JSON reply per line, {"id": 1, "result": {...}} or {"id": 1, "error": "..."}.
 * Requests run on the main loop between frames, as if typed in the console; screenshots are read
 * from the frame just drawn. Methods: run_python (code), get_state, send_keys (key, ctrl, shift,
 * alt, area), screenshot (area: an editor's name, or "" for the whole window), ping. */

#include <string>

namespace oanim {

struct Context;

/* Starts listening on `port`, or (taken by another Open Animate) any free one; 0 asks for any free
 * one. Returns the port, or 0 if it couldn't. With `publish`, the port, token, process id and open
 * file go in remote/<process id>.json, and remote.json while holding `port`, for the bridge (not in
 * tests). */
int remote_start(int port, bool publish = true);
/* The token requests must carry ("" when off). */
std::string remote_token();
void remote_stop();
/* The port listened on, or 0 when off. */
int remote_port();

/* Runs the requests that have arrived (screenshots wait for remote_capture). Each frame, before
 * drawing. */
void remote_process(Context &C);
/* After a frame is drawn, before it is shown: answers screenshot requests from the back buffer
 * (`width` x `height` pixels). */
void remote_capture(int width, int height);

/* One request answered at once, JSON in and out (a screenshot can't be: there is no frame). */
std::string remote_dispatch(Context &C, const std::string &request);

}  // namespace oanim
