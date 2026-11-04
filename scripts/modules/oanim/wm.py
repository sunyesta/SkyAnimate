"""Window-manager helpers."""

import _oanim


def tool():
    """The viewport tool: "SELECT", "SELECT_LAYER", "CURSOR", "ANNOTATE", "DRAW", "SHAPE", "SPLIT_CURVE",
    "WEIGHT_SELECT", "WEIGHT_PAINT", "TRANSFORM_BONES", "TRANSFORM_POINTS" or "SCULPT"."""
    return _oanim.tool()


def draw_order_pick_target(hit, moving):
    """The layer a viewport click on drawing `hit` picks with the Draw Order eyedropper (In Front
    Of... / Behind...) for the layers `moving`: the outermost group holding it that holds none of
    them, or `hit` itself (Ctrl-click always picks the drawing). Layers or ids; returns the name."""
    import oanim

    doc_layers = {l.id: l for l in oanim.data.layers.all()}
    as_id = lambda l: l if isinstance(l, int) else l.id
    return doc_layers[_oanim.draw_order_pick_target(as_id(hit), [as_id(m) for m in moving])].name


def event_simulate(type, *, ctrl=False, shift=False, alt=False, area="", typing=False):
    """Send a key press through the keymaps as if typed. Returns True if an operator ran.

    area picks the editor keymap to try first, e.g. "Viewport"; then the "Window" keymap.
    typing=True acts as if a text field has focus: keys it uses itself skip the "Window" keymap.
    """
    return _oanim.event_simulate(type, ctrl, shift, alt, area, typing)


def shading():
    """The project's Viewport Shading (saved with it): "layer_caching" (only the layers that changed, and those
    depending on them, drawn anew; the rest from images kept of them, and pans within a margin shown moved) and
    "zoom_preview" (while a zoom moves, the last image scaled over a blurry wider backdrop, drawn sharp once it
    rests), and the renderer: "gpu_geometry" (on by default: big fills drawn by the GPU, stencil-then-cover, and masks in the depth
    buffer, nesting without limit, and see-through layers faded as a whole; off: Classic, every
    fill cut into triangles on the CPU)."""
    return dict(_oanim.shading())


def set_shading(name, on=True):
    """Set a Viewport Shading option of the project by name ("layer_caching", "zoom_preview", "gpu_geometry")."""
    _oanim.set_shading(name, bool(on))


def set_render_stats(on=True):
    """Record each render's statistics from now on (or stop): for benchmarks (tools/render_bench). Off by default;
    on, every render also waits for the GPU to time it."""
    _oanim.set_render_stats(bool(on))


def fill_covers(outline):
    """The area an outline's fill covers, by the triangles made for it: [(x, y)]. A background run draws nothing, so
    this is how a test sees what a fill does."""
    return _oanim.fill_covers([tuple(p) for p in outline])


def render_stats():
    """The renders' statistics recorded since last asked (set_render_stats), oldest first: dicts with width, height,
    final (a render to a file), cpu_ms (the render's own CPU time), prepare_ms (finding and making drawings before
    drawing), evaluate_ms (bones and modifiers), make_ms (making drawings, summed over the threads), its parts
    outline_ms, earcut_ms, fan_ms and stroke_ms, mask_make_ms (of make_ms, masks' shapes, made one at a time as
    they are drawn), drawings_made, gpu_ms (the GPU's time from first draw to last) and
    gpu ({kind of draw: ms}: batches, fill fans, fill covers, fan clears, mask shapes, mask passes, mask depth,
    composites, stamps, media, colour draws, clears, resolves), cpu (the same kinds: the CPU's time in their draw calls)
    and calls (how many)."""
    return list(_oanim.render_stats())


def mouse_simulate(x, y, button=None, down=True, ctrl=False, alt=False, shift=False, wheel=0.0, pen=None,
                   eraser=False):
    """Move the mouse to (x, y) in the window's UI units (as a remote screenshot of the whole
    window shows them), and press (down=True) or release a button: 0 left, 1 right, 2 middle; None
    only moves; `ctrl`, `alt`, `shift`: held; `wheel`: notches the wheel turns there (up positive,
    zooming the viewport in). Events queue and one is used per frame, so a click is a press then a
    release.

    `pen`: a pen tablet's samples arriving with this event, as Windows Ink hands them over (many to a
    frame): a list of (x, y[, pressure[, tilt_x, tilt_y[, seconds]]]) in the same units, tilts in degrees,
    seconds from now. A left press with them starts the pen's touch (`eraser`: with its eraser end), a
    release ends it; moves in between add to it."""
    _oanim.mouse_simulate(float(x), float(y), -1 if button is None else int(button), bool(down), bool(ctrl),
                          bool(alt), bool(shift), float(wheel), None if pen is None else [tuple(p) for p in pen],
                          bool(eraser))


def pen(samples=False):
    """The pen tablet as last heard from: a dict with windows_ink (samples have come through Windows Ink),
    near, touching, eraser (its eraser end), pressure (0-1), tilt (degrees across and toward you), touch
    (which touch of the pen, counting from 1) and sample_count (its samples); with samples=True also
    samples, each (x, y, pressure, tilt_x, tilt_y, seconds since the touch began)."""
    return dict(_oanim.pen(bool(samples)))


def wintab_convert(x, y, pressure, azimuth, altitude, in_x=0, in_y=0, in_width=1, in_height=1, screen_x=0, screen_y=0,
                   screen_width=1, screen_height=1, pressure_max=1, azimuth_max=0, altitude_max=0):
    """A WinTab packet as the app reads it, given what the tablet's values are out of: (screen x, screen y, pressure,
    tilt_x, tilt_y)."""
    return tuple(_oanim.wintab_convert(int(x), int(y), int(pressure), int(azimuth), int(altitude), {
        "in_x": in_x, "in_y": in_y, "in_width": in_width, "in_height": in_height, "screen_x": screen_x,
        "screen_y": screen_y, "screen_width": screen_width, "screen_height": screen_height,
        "pressure_max": pressure_max, "azimuth_max": azimuth_max, "altitude_max": altitude_max}))


def draw_settings():
    """The Draw tool's settings: a dict with mode (FREEHAND, PEN, FILL_PEN, FILL_ERASER, STROKE_ERASER or TRIMMER),
    preset (the pen's name), fill_size (the Fill Pen and erasers' width, document units), erase_all_layers,
    erase_selected, erase_whole (the Stroke Eraser takes whole lines), trim (ends past a crossing go), use_pressure, fill,
    fill_color and line_color to set (r, g, b, a),
    weld (a stroke's ends join points they finish on), cursor_circle, line_width (document units), prediction (how far ahead of a pen's last sample the line is drawn while drawing, ms; 0 off),
    stabilizer (OFF, SMART or PULLED_STRING), smoothing (Smart's amount, 0-1), string_length (screen pixels),
    catch_up, fidelity (how closely the Bezier points follow the line, 0-1), pressure_smoothing (0-1),
    pressure_curve (the two handles
    of pressure's Bezier from (0, 0) to (1, 1), as (x1, y1, x2, y2)), min_size (fraction of the width),
    speed_width and tilt_width (0-1), taper_start and taper_end (document pixels)."""
    return dict(_oanim.draw_settings())


def draw_presets():
    """The pens on the Draw tool's shelf: dicts with name, mode, line_width, category (Pens or Mine), builtin and the
    line's settings as in draw_settings. Pick one with oanim.ops.draw.preset_load(name=...)."""
    return list(_oanim.draw_presets())


def set_draw(**values):
    """Change the Draw tool's settings by name (see draw_settings)."""
    _oanim.set_draw(values)


def ink_stroke(samples, pixel=1.0, line_width=10.0, closed=False, **settings):
    """Runs the Stroke Pen's engine on samples [(x, y, pressure, tilt_x, tilt_y, seconds)], with settings by name
    as in draw_settings (a document pixel counting as 1 for tapers). A dict: live and tail (the line while the
    pen is still down, and the rest to its last sample), points (the finished line), each (x, y, radius, speed),
    and knots (its Bezier points, each (x, y, radius, handle_in, handle_out, corner))."""
    return dict(_oanim.ink_stroke([tuple(s) for s in samples], settings, float(pixel), float(line_width) * 0.5,
                                  bool(closed)))


def pen_predict(samples, ahead, max_pixels=40.0):
    """Where the line is drawn ahead of a pen's samples [(x, y, seconds)], `ahead` seconds on, at most
    `max_pixels` away (the Draw tool's prediction): (x, y)."""
    return tuple(_oanim.pen_predict([tuple(s) for s in samples], float(ahead), float(max_pixels)))


def layer_grid_lines(on=None):
    """Transform Layer's Show Grid Lines (the active layer's own grid across the viewport): whether it is
    on, after setting it to `on` when given."""
    if on is not None:
        _oanim.layer_grid_lines_set(bool(on))
    return _oanim.layer_grid_lines()


GIZMO_MODES = ('TRANSFORM', 'DISTORT', 'WARP')


def gizmo_mode(mode=None, cells=None):
    """The Transform Points and Transform Bones box's mode, TRANSFORM, DISTORT or WARP (and Warp's grid, `cells`
    each way, 1 to 8), after setting them when given. Returns (mode, cells)."""
    if mode is not None or cells is not None:
        current = GIZMO_MODES[_oanim.gizmo_mode()] if mode is None else str(mode).upper()
        _oanim.gizmo_mode_set(GIZMO_MODES.index(current), 0 if cells is None else int(cells))
    return GIZMO_MODES[_oanim.gizmo_mode()], _oanim.warp_cells()


def text_simulate(text):
    """Type `text` (letters, not keys: for a text field or the Text tool) as the keyboard's text input
    would, handled on the next frame. Needs the window."""
    _oanim.text_simulate(str(text))


def key_press_simulate(key, ctrl=False, shift=False, alt=False):
    """Press and let go of `key` (SDL's name: "Tab", "A", "F9") as the keyboard sends it, with the
    modifiers held: through ImGui and the keymaps alike, handled on the next frames. Needs the
    window."""
    _oanim.key_press_simulate(key, bool(ctrl), bool(shift), bool(alt))


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


def brush_settings_open(target="SHAPES", tab="SETTINGS"):
    """Open the Brush Settings window on the selected shapes' brush, or with target "TOOL" the Draw
    tool's; tab "BRUSHES" opens it on its Brushes tab (the presets)."""
    _oanim.brush_settings_open(target.upper() == "TOOL", tab.upper() == "BRUSHES")


def properties_tab(tab):
    """Show a Properties tab (in the next Properties editor drawn): "DOCUMENT", "LAYER", "MASKING",
    "MODIFIERS", "VECTOR" (a drawing's data: point groups, the selected points and edges) or
    "ARMATURE" (a bone layer's collections, bones and fixes; "DRAWING" is either), "SHAPE" (a drawing's
    shapes and their looks), "BONE" (the active bone), "CONSTRAINTS" (Bone Constraints), "REFERENCE"
    (a reference's), "CAMERA" (the picked camera's), "MEDIA" (a Media layer's) or "TEXT" (a text
    layer's). A tab the active layer doesn't have is kept, and shows when a layer that has it is."""
    name = tab.upper()
    if name in ("VECTOR", "ARMATURE"):
        name = "DRAWING"
    _oanim.properties_tab(["DOCUMENT", "LAYER", "DRAWING", "MODIFIERS", "CONSTRAINTS", "REFERENCE",
                         "MASKING", "CAMERA", "MEDIA", "TEXT", "SHAPE", "BONE"].index(name))


def media_view():
    """Whether Media View is on (`, oanim.ops.view.media_view()): every media layer at full
    opacity, guides too, with the viewport's overlays hidden. A view only: nothing changes or is keyed."""
    return _oanim.media_view()


def local_view():
    """The names of the layers Local View shows on their own (/ or Numpad /), or [] when it's off."""
    return list(_oanim.local_view())


def move_options():
    """The ViewportActionPanel's checkboxes for moves, as the next move will take them: "keep_poses"
    (Keep Poses), "only_bones" (Move Only Bones) and "without_children" (Move Without Children). Each
    stays as last set for the moves after it until the frame or the action being edited changes, then
    it's off again."""
    return dict(_oanim.move_options())


def overlays():
    """The project's Viewport Overlays (saved with it): a dict of its switches ("show", "grid",
    "cursor", "camera_frame", "info", "wireframe", "points", "mask_outlines", "relationship_lines",
    "physics_ghosts", "live_physics", "bone_shapes", "guides", "annotations") and "handles" ("NONE",
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


def onion_frames():
    """The frames onion skins show ghosts on for the layers being edited (none with onion skins off):
    dicts with frame, after (else before) and distance (1 for the nearest on its side). The settings are
    the document's: onion_skin, onion_mode (KEYFRAMES or FRAMES), onion_before, onion_after, onion_step,
    onion_fade, onion_all_layers."""
    return _oanim.onion_frames()


def motion_paths():
    """The motion paths drawn now (none with doc.motion_paths off): dicts with label, layer, pinned, head, color,
    first (the frame of points[0]), points (where it is on each frame, as drawn: relative to doc.path_relative),
    speed (0 still to 1 its fastest, per frame), keys, handles (each key's path handles as the Motion Path tool shows
    them once selected: dicts with frame, in and out, as drawn, None for a side not shown) and speed_colors ((still,
    fast) rgb). They trace the selected
    bones (tip, or head) or points of the layers being edited, else those layers' origins, and pinned ones."""
    return _oanim.motion_paths()


def motion_path_range():
    """(first, last): the frames the paths cover, path_before keys back to path_after keys ahead (or all with
    path_all); what Alt Space plays. None with nothing to trace."""
    return _oanim.motion_path_range()


def motion_path_ghost(frame):
    """The wires (lists of (x, y)) of the ghost hovering a path's dot on `frame` shows."""
    return _oanim.motion_path_ghost(frame)


def motion_path_snapshot():
    """The kept paths (lists of (x, y)) drawn faintly to compare against (anim.motion_path_snapshot)."""
    return _oanim.motion_path_snapshot()


def play_range():
    """(playing, first, last): the frames Play Path Range loops (0, 0 when playing everything)."""
    return tuple(_oanim.play_range())


def onion_wires():
    """The ghosts as they would be drawn: dicts with frame, after and lines (each a list of (x, y) in
    document space, one per curve)."""
    return _oanim.onion_wires()


def onion_colors():
    """The onion skins' colours: a dict with before and after, each (r, g, b, a) 0-1."""
    return _oanim.onion_colors()


def set_onion_colors(before=None, after=None):
    """Set the onion skins' before and/or after colour: (r, g, b) or (r, g, b, a), 0-1."""
    _oanim.set_onion_colors(before, after)


def motion_path_dots():
    """The dots selected between keys with the Motion Path tool's Edit: dicts with layer, bone, point (ids, 0 for
    none), head and frame. Keys selected are the timeline's (keyframes()' "selected")."""
    return _oanim.motion_path_dots()


def path_brush_settings():
    """The Motion Path tool's brush: {"brush": "EDIT", "GRAB", "SMOOTH", "PAINT_SPEED", "FASTER", "SLOWER" or
    "BLEND_SPEED", "radius" (screen pixels), "strength", "speed" (Paint Speed's), and for sliding dots with Edit
    "proportional", "proportional_size" (frames) and "falloff"}. Set with
    oanim.ops.anim.motion_path_brush_set(brush=..., radius=..., strength=..., speed=..., proportional=...,
    proportional_size=..., falloff=...)."""
    return _oanim.path_brush_settings()


def sculpt_settings():
    """The Sculpt tool's settings: a dict with brush (GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST,
    SURFACE_SMOOTH), preset (the shelf's preset it was picked from), radius (screen pixels), strength (0-1),
    falloff (SMOOTH, SPHERE, CONSTANT), pressure_radius, pressure_strength, selected_only and
    surface_sensitivity (Surface Smooth's Lump Sensitivity: 0-1 with no top, how big a lump it takes out; 1 is a
    tenth of the canvas's height, whatever its resolution)."""
    return _oanim.sculpt_settings()


def set_sculpt(**values):
    """Change the Sculpt tool's settings by name (see sculpt_settings), as its header does."""
    _oanim.set_sculpt(values)


def sculpt_presets():
    """The sculpt brush presets on the shelf: dicts with name, brush, radius, strength, falloff,
    category (Move, Shape or Mine) and builtin. Pick one with oanim.ops.sculpt.preset_load(name=...)."""
    return list(_oanim.sculpt_presets())


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


def clipboard_set_image(rgba, width, height):
    """Put an image on the system clipboard (as the Render Result's Copy Image does): `rgba` is bytes,
    width * height * 4 of them, rows from the top. It goes as a PNG (keeping transparency) and a bitmap."""
    _oanim.clipboard_set_image(bytes(rgba), int(width), int(height))


def switch_link_report(switch):
    """Open the Link Report for a linked switch (its group's names by switch). False when it isn't linked."""
    return _oanim.switch_link_report(switch.id if hasattr(switch, "id") else int(switch))


def bone_turn_snap(degrees, rotate_key=True):
    """For tests: where Ctrl puts a turn of `degrees` of the active bone layer's picked bones, as R has it
    (`rotate_key`) or the Transform Bones tool's turn: (degrees, what it snapped to: "45°", "start" or
    "setup")."""
    return tuple(_oanim.bone_turn_snap(float(degrees), bool(rotate_key)))


def frame_sync_playing(frame, edits):
    """Go to `frame` as playing does in the main loop (for tests): `edits` stands for the stamp of edits the app
    can see coming (Python runs, undo steps, operators, UI edits). While it stays the same, drawings switches hide
    that come out the same on every frame are neither recorded nor evaluated."""
    _oanim.frame_sync_playing(int(frame), int(edits))


def switch_preview(switch=None, child=None):
    """Preview a switch's child in the viewport, as hovering a tile on the asset shelf does: shown in
    place of the pick, nothing keyed, no undo step; renders keep the pick. Without arguments it ends."""
    if switch is None:
        _oanim.switch_preview(0, 0)
    else:
        _oanim.switch_preview(switch.id if hasattr(switch, "id") else int(switch),
                              0 if child is None else (child.id if hasattr(child, "id") else int(child)))


def render_png(filepath, width=0, height=0, guides=False, transparent=False):
    """Render the current frame through the camera to a PNG, as Render Frame does: `width` x `height`
    (0: the document's size), with guide layers (a reference) too if `guides`, and a clear background if
    `transparent` (always with guides: a render with a background is a final one, which leaves guides out). The window renders it on its next frame, so the file appears just after this returns
    (wait for it). The camera's frame fills the picture: a document point shows at pixel
    (w/2 + s*fx, h/2 - s*fy), s = h/2, where (fx, fy) = oanim.data.camera.to_frame(point)."""
    _oanim.render_png(str(filepath), int(width), int(height), bool(guides), bool(transparent))


def cursor():
    """The 2D cursor's place (document space), as Shift S and the Cursor tool set it."""
    return tuple(_oanim.cursor_get())


def set_cursor(position):
    """Put the 2D cursor at `position` (x, y), document space."""
    _oanim.cursor_set(float(position[0]), float(position[1]))


def mirror_editing():
    """(x, y): whether mirror editing (the viewport header's butterfly) is on for each axis."""
    return tuple(_oanim.mirror_editing())


def set_mirror_editing(x=False, y=False):
    """Turn mirror editing on or off: edits to a point also apply to its mirror image."""
    _oanim.set_mirror_editing(x, y)
