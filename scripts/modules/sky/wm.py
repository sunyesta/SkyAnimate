"""Window-manager helpers."""

import _sky


def event_simulate(type, *, ctrl=False, shift=False, alt=False, area=""):
    """Send a key press through the keymaps as if typed. Returns True if an operator ran.

    area picks the editor keymap to try first, e.g. "Viewport"; the "Window" keymap is always tried.
    """
    return _sky.event_simulate(type, ctrl, shift, alt, area)


def undo_history():
    """(step names, index of the active step)."""
    return _sky.undo_history()


def report(message, type="INFO"):
    """Show a message in the Info editor. type is "INFO", "WARNING" or "ERROR"."""
    _sky.report(type, message)
