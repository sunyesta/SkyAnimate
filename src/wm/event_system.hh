#pragma once

/* Turns key events into operator calls through the keymaps. */

namespace sky {

struct Context;
struct KeyEvent;

/* Looks up the event in the keymap of the editor under the mouse, then in "Window", and runs
 * the first operator whose poll passes. With area_only (while typing), "Window" is skipped for
 * keys a text field uses itself. Returns true if an operator ran. */
bool wm_handle_key_event(Context &C, const KeyEvent &event, bool area_only = false);

}  // namespace sky
