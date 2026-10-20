"""Window-manager helpers."""

import _sky


def event_simulate(type, *, ctrl=False, shift=False, alt=False, area="", typing=False):
    """Send a key press through the keymaps as if typed. Returns True if an operator ran.

    area picks the editor keymap to try first, e.g. "Viewport"; then the "Window" keymap.
    typing=True acts as if a text field has focus: keys it uses itself skip the "Window" keymap.
    """
    return _sky.event_simulate(type, ctrl, shift, alt, area, typing)


def debug_crash():
    """Crash SkyAnimate on purpose, to try its crash report (written to
    %LOCALAPPDATA%\SkyAnimate\crashes). Unsaved work is lost: save first."""
    _sky.debug_crash()


def properties_tab(tab):
    """Show a Properties tab (in the next Properties editor drawn): "DOCUMENT", "LAYER", "DRAWING"
    (Armature for a bone layer), "MODIFIERS", "CONSTRAINTS" (Bone Constraints), "REFERENCE" (a reference's)
    or "MASKING"."""
    _sky.properties_tab(["DOCUMENT", "LAYER", "DRAWING", "MODIFIERS", "CONSTRAINTS", "REFERENCE",
                         "MASKING"].index(tab.upper()))


def properties_flash():
    """What the Properties editor was last asked to show (a key's property picked in the timeline's
    Keyed Here list), until it has: (tab asked for, -1 once taken; layer id; channel path; panel)."""
    return _sky.properties_flash()


def proportional_editing():
    """The viewport's proportional editing (O): (on, falloff, connected, size). falloff: SMOOTH,
    SPHERE, ROOT, INVERSE_SQUARE, SHARP, LINEAR, CONSTANT or RANDOM."""
    return tuple(_sky.proportional_editing())


def set_proportional_editing(on=True, falloff="SMOOTH", connected=False, size=0.2):
    """Set proportional editing: G R S on points drag the points within `size` (document units)
    of the selection too, by the falloff; connected=True only along the curves."""
    _sky.set_proportional_editing(on, falloff.upper(), connected, size)


def timeline_rows(group_by="KIND", changed=False):
    """The timeline's channel rows, every one open: (depth, name, red, pinned) for each, red for the
    Pinned and Selected sections, pinned for a pinned property's row; with changed=True a fifth,
    changed: a reference's channel it took over from its original (drawn teal). group_by: "OWNER"
    or "KIND" (the default; the timeline's View menu)."""
    rows = [tuple(r) for r in _sky.timeline_rows(group_by.upper() == "KIND")]
    return rows if changed else [r[:4] for r in rows]


def undo_history():
    """(step names, index of the active step)."""
    return _sky.undo_history()


def mouse_report():
    """The last warning shown at the mouse, as (how many have been shown, title, message): a
    test compares the count before and after to see whether an action showed one."""
    return _sky.mouse_report()


def report(message, type="INFO"):
    """Show a message in the Info editor. type is "INFO", "WARNING" or "ERROR"."""
    _sky.report(type, message)


def mirror_editing():
    """(x, y): whether mirror editing (the viewport header's butterfly) is on for each axis."""
    return tuple(_sky.mirror_editing())


def set_mirror_editing(x=False, y=False):
    """Turn mirror editing on or off: edits to a point also apply to its mirror image."""
    _sky.set_mirror_editing(x, y)
