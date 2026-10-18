"""Window-manager helpers."""

import _sky


def event_simulate(type, *, ctrl=False, shift=False, alt=False, area="", typing=False):
    """Send a key press through the keymaps as if typed. Returns True if an operator ran.

    area picks the editor keymap to try first, e.g. "Viewport"; then the "Window" keymap.
    typing=True acts as if a text field has focus: keys it uses itself skip the "Window" keymap.
    """
    return _sky.event_simulate(type, ctrl, shift, alt, area, typing)


def undo_history():
    """(step names, index of the active step)."""
    return _sky.undo_history()


def report(message, type="INFO"):
    """Show a message in the Info editor. type is "INFO", "WARNING" or "ERROR"."""
    _sky.report(type, message)
