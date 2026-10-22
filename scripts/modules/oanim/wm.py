"""Window-manager helpers."""

import _oanim


def event_simulate(type, *, ctrl=False, shift=False, alt=False, area="", typing=False):
    """Send a key press through the keymaps as if typed. Returns True if an operator ran.

    area picks the editor keymap to try first, e.g. "Viewport"; then the "Window" keymap.
    typing=True acts as if a text field has focus: keys it uses itself skip the "Window" keymap.
    """
    return _oanim.event_simulate(type, ctrl, shift, alt, area, typing)


def mouse_simulate(x, y, button=None, down=True, ctrl=False, alt=False, shift=False):
    """Move the mouse to (x, y) in the window's UI units (as a remote screenshot of the whole
    window shows them), and press (down=True) or release a button: 0 left, 1 right, 2 middle; None
    only moves; `ctrl`, `alt`, `shift`: held. Events queue and one is used per frame, so a click is a
    press then a release."""
    _oanim.mouse_simulate(float(x), float(y), -1 if button is None else int(button), bool(down), bool(ctrl),
                          bool(alt), bool(shift))


def drop_simulate(filepath, x, y):
    """Drop a file on the window at (x, y), as from Explorer (handled on the next frame): a picture,
    video or sound becomes a Media layer (starting on the frame under it in a timeline), an .oanim
    opens. Needs the window."""
    return _oanim.drop_simulate(filepath, float(x), float(y))


def file_drag_simulate(filepaths, x=0, y=0):
    """As if `filepaths` were being dragged from Explorer over the window at (x, y): the viewport and
    timeline show where they would land. An empty list ends the drag (nothing is dropped)."""
    _oanim.file_drag_simulate(list(filepaths), float(x), float(y))


def mouse_click(x, y, button=0, ctrl=False, alt=False, shift=False):
    """A click at (x, y): moves there, presses, releases (over three frames), with the keys held."""
    mouse_simulate(x, y, None, True, ctrl, alt, shift)
    mouse_simulate(x, y, button, True, ctrl, alt, shift)
    mouse_simulate(x, y, button, False, ctrl, alt, shift)
    mouse_simulate(x, y, None, True)


def debug_crash():
    """Crash Open Animate on purpose, to try its crash report (written to
    %LOCALAPPDATA%\\OpenAnimate\\crashes). Unsaved work is lost: save first."""
    _oanim.debug_crash()


def brush_settings_open(target="SHAPES"):
    """Open the Brush Settings window on the selected shapes' brush, or with target "TOOL" the Draw
    tool's."""
    _oanim.brush_settings_open(target.upper() == "TOOL")


def properties_tab(tab):
    """Show a Properties tab (in the next Properties editor drawn): "DOCUMENT", "LAYER", "SHAPE"
    (the shapes and their looks; Armature for a bone layer; "DRAWING" still works), "MODIFIERS",
    "CONSTRAINTS" (Bone Constraints), "REFERENCE" (a reference's), "MASKING", "CAMERA" (the picked
    camera's) or "MEDIA" (a Media layer's)."""
    name = tab.upper()
    if name == "SHAPE":
        name = "DRAWING"
    _oanim.properties_tab(["DOCUMENT", "LAYER", "DRAWING", "MODIFIERS", "CONSTRAINTS", "REFERENCE",
                         "MASKING", "CAMERA", "MEDIA"].index(name))


def local_view():
    """The names of the layers Local View shows on their own (/ or Numpad /), or [] when it's off."""
    return list(_oanim.local_view())


def overlays():
    """The project's Viewport Overlays (saved with it): a dict of its switches ("show", "grid",
    "cursor", "camera_frame", "info", "wireframe", "points", "mask_outlines", "relationship_lines",
    "physics_ghosts", "live_physics", "bone_shapes") and "handles" ("NONE",
    "SELECTED", "SHAPE", "ALL"), "armature" ("OFF", "SELECTED", "ON"), "point_size" and "handle_size"
    (pixels across, 2 to 24)."""
    return dict(_oanim.overlays())


def ui_open():
    """Panels and folding rows the project keeps open or folded (saved with it), by
    "<editor>/<panel>" ("Properties/Transform", "Timeline/<row>", "Viewport Overlays/Guides"): only
    those not at their default."""
    return dict(_oanim.ui_open())


def set_ui_open(key, open=True):
    """Open or fold a panel as clicking its header does (kept by the project)."""
    _oanim.set_ui_open(key, bool(open))


def set_overlay(name, on=True):
    """Set a Viewport Overlays setting of the project: a switch by name to True or False, "handles"
    / "armature" to one of their values, or "point_size" / "handle_size" to pixels (2 to 24)."""
    if name in ("point_size", "handle_size"):
        _oanim.set_overlay(name, float(on))
    else:
        _oanim.set_overlay(name, on if isinstance(on, str) else bool(on))


def last_operation():
    """The operator the ViewportActionPanel adjusts, as (idname, args, editor it ran in), or None
    when there's nothing to adjust (the last action wasn't one with options, or something changed
    since)."""
    return _oanim.last_operation()


def redo_last(**args):
    """Run the last operation again with some options changed, as changing them in the
    ViewportActionPanel does (its step is undone first). Returns "FINISHED" or "CANCELLED"."""
    last = _oanim.last_operation()
    if last is None:
        return "CANCELLED"
    merged = dict(last[1])
    merged.update({k: ("1" if v is True else "0" if v is False else str(v)) for k, v in args.items()})
    return _oanim.redo_last(merged)


def properties_flash():
    """What the Properties editor was last asked to show (a key's property picked in the timeline's
    Keyed Here list), until it has: (tab asked for, -1 once taken; layer id; channel path; panel)."""
    return _oanim.properties_flash()


def proportional_editing():
    """The viewport's proportional editing (O): (on, falloff, connected, size). falloff: SMOOTH,
    SPHERE, ROOT, INVERSE_SQUARE, SHARP, LINEAR, CONSTANT or RANDOM."""
    return tuple(_oanim.proportional_editing())


def set_proportional_editing(on=True, falloff="SMOOTH", connected=False, size=0.2):
    """Set proportional editing: G R S on points drag the points within `size` (document units)
    of the selection too, by the falloff; connected=True only along the curves."""
    _oanim.set_proportional_editing(on, falloff.upper(), connected, size)


def timeline_rows(group_by="KIND", changed=False):
    """The timeline's channel rows, every one open: (depth, name, red, pinned) for each, red for the
    Pinned and Selected sections, pinned for a pinned property's row; with changed=True a fifth,
    changed: a reference's channel it took over from its original (drawn teal). group_by: "OWNER"
    or "KIND" (the default; the timeline's View menu)."""
    rows = [tuple(r) for r in _oanim.timeline_rows(group_by.upper() == "KIND")]
    return rows if changed else [r[:4] for r in rows]


def undo_history():
    """(step names, index of the active step)."""
    return _oanim.undo_history()


def banner_report():
    """The report the status bar shows, as (how many have been shown, type ("INFO", "WARNING" or
    "ERROR"), title, message, hint): a test compares the count before and after to see whether an
    action reported something."""
    return _oanim.banner_report()


def report(message, type="INFO"):
    """Show a message in the Info editor. type is "INFO", "WARNING" or "ERROR"."""
    _oanim.report(type, message)


def mirror_editing():
    """(x, y): whether mirror editing (the viewport header's butterfly) is on for each axis."""
    return tuple(_oanim.mirror_editing())


def set_mirror_editing(x=False, y=False):
    """Turn mirror editing on or off: edits to a point also apply to its mirror image."""
    _oanim.set_mirror_editing(x, y)
