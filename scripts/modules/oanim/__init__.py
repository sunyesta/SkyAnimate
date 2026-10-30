"""Open Animate's Python API, shaped like Blender's bpy.

oanim.context   the current context: the document and the editor under the mouse
oanim.data      the open document: oanim.data.document, oanim.data.layers, oanim.data.texts, oanim.data.filepath
oanim.assets    the asset library: oanim.assets.library, oanim.assets.brushes()
oanim.ops       run operators by name: oanim.ops.ed.undo()
oanim.types     base classes to subclass: oanim.types.Operator
oanim.utils     register_class() and unregister_class()
oanim.keymap    bind keys to operators
oanim.wm        window-manager helpers, such as simulating key presses in tests
oanim.media     media files as Media layers read them: probe(), pixel(), mix()
"""

import _oanim

from . import keymap, media, ops, types, utils, wm

__all__ = ("assets", "context", "data", "keymap", "media", "ops", "types", "utils", "wm")


class _Text:
    """A text block in the document, like bpy.types.Text."""

    def __init__(self, name):
        self._name = name

    @property
    def name(self):
        return self._name

    @property
    def body(self):
        return _oanim.text_body(self._name)

    @body.setter
    def body(self, value):
        _oanim.text_set_body(self._name, value)

    @property
    def filepath(self):
        return _oanim.text_filepath(self._name)

    def __eq__(self, other):
        return isinstance(other, _Text) and other._name == self._name

    def __repr__(self):
        return f"oanim.data.texts[{self._name!r}]"


class _Texts:
    """The document's text blocks, like bpy.data.texts."""

    def new(self, name="Text"):
        return _Text(_oanim.text_new(name))

    def remove(self, text):
        _oanim.text_remove(text.name)

    def keys(self):
        return _oanim.text_names()

    def __getitem__(self, name):
        if name not in _oanim.text_names():
            raise KeyError(name)
        return _Text(name)

    def __contains__(self, name):
        return name in _oanim.text_names()

    def __iter__(self):
        return iter([_Text(name) for name in _oanim.text_names()])

    def __len__(self):
        return len(_oanim.text_names())

    @property
    def active(self):
        """The text shown in the text editor, or None."""
        name = _oanim.text_active()
        return _Text(name) if name else None

    @active.setter
    def active(self, text):
        _oanim.text_set_active(text.name if text else "")


class _Layer:
    """A layer, like Moho's MohoLayer. Properties (name, visible, opacity, locked, ...) come
    from the layer's RNA description; dir() lists them."""

    def __init__(self, id):
        object.__setattr__(self, "_id", id)

    @property
    def id(self):
        return self._id

    @property
    def children(self):
        return [_Layer(i) for i in _oanim.layer_children(self._id)]

    @property
    def parent(self):
        parent = _oanim.layer_parent(self._id)
        return _Layer(parent) if parent else None

    @property
    def reference_source(self):
        """The layer this reference shows, or None."""
        source = _oanim.layer_reference_source(self._id)
        return _Layer(source) if source else None

    @property
    def selected(self):
        """Picked in the Layers editor (with the active layer, the layers edited together)."""
        return _oanim.layer_is_selected(self._id)

    @selected.setter
    def selected(self, value):
        _oanim.layer_set_selected(self._id, bool(value))

    @property
    def points(self):
        """[(x, y, selected), ...] for the drawing the layer shows."""
        return _oanim.layer_points(self._id)

    @property
    def radii(self):
        """Each point's stroke radius, 0-1, as a fraction of its shape's line width."""
        return _oanim.layer_radii(self._id)

    @property
    def mean_radius(self):
        """Mean radius of the selected points (None if none are selected). Setting it moves each
        selected point's radius by the same amount, like Blender's Mean Radius field."""
        return _oanim.layer_mean_radius(self._id)

    @mean_radius.setter
    def mean_radius(self, value):
        _oanim.layer_set_mean_radius(self._id, value)

    @property
    def mean_position(self):
        """Average position of the selected points (None if none); setting it moves them all."""
        p = _oanim.layer_mean_position(self._id, -1)
        return tuple(p) if p is not None else None

    @mean_position.setter
    def mean_position(self, value):
        _oanim.layer_set_mean_position(self._id, -1, tuple(value))

    def shape_position(self, index):
        """The middle of shape `index`'s points (the Shape panel's Position)."""
        p = _oanim.layer_mean_position(self._id, index)
        return tuple(p) if p is not None else None

    def set_shape_position(self, index, value):
        """Move shape `index` so the middle of its points is at `value`."""
        _oanim.layer_set_mean_position(self._id, index, tuple(value))

    @property
    def point_groups(self):
        """[(name, {point index: weight}, locked)], Blender's vertex groups; change them with
        oanim.ops.mesh.point_group_add / _remove / _set / _assign / _unassign / _select / _deselect /
        _move / _copy / _sort / _unassign_all / _remove_all / _lock_all."""
        groups, _ = _oanim.layer_point_groups(self._id)
        return [(name, dict(members), locked) for name, members, locked in groups]

    @property
    def active_point_group(self):
        """The active point group's index (what Assign and Select work on)."""
        return _oanim.layer_point_groups(self._id)[1]

    @property
    def shapes(self):
        """[(name, [curve indices]), ...] in stacking order, bottom first."""
        return [(name, list(curves)) for name, curves in _oanim.layer_shapes(self._id)]

    @property
    def point_count(self):
        return _oanim.layer_counts(self._id)[0]

    @property
    def curve_count(self):
        return _oanim.layer_counts(self._id)[1]

    @property
    def shape_count(self):
        return _oanim.layer_counts(self._id)[2]

    def add_curve(self, points, closed=False, fill=False):
        """Add a curve through points [(x, y), ...] with an outline shape (and a fill if closed and fill)."""
        return _oanim.layer_add_curve(self._id, points, closed, fill)

    def add_primitive(self, kind, points, fill=False, columns=3, rows=3):
        """Add a box, circle or grid as the Shape tool draws them: kind "BOX", "CIRCLE" or "GRID" (`columns` x
        `rows` cells), points = the drag's two corners. The new points become the selection. False if too
        small to draw."""
        return _oanim.layer_add_primitive(self._id, kind, points, fill, columns, rows)

    def extrude_selected(self):
        """Extrude's first step (E, before the move): a new point past each selected open end, its curve growing;
        any other selected point starts a new curve welded to it. The new points become the selection; returns
        their indices."""
        return list(_oanim.layer_extrude_selected(self._id))

    def shape_runs(self, shape):
        """Shape `shape`'s runs, its connected pieces each drawn as a line of its own: [[(curve, segment)]]."""
        return [list(run) for run in _oanim.layer_shape_runs(self._id, shape)]

    @property
    def grids(self):
        """The drawing's grids (the Shape tool's Grid), [(name, columns, rows, [node point index, row by row
        from the bottom, each row left to right])]."""
        return [(name, columns, rows, list(nodes)) for name, columns, rows, nodes in _oanim.layer_grids(self._id)]

    def grid_map(self, point, grid=0):
        """Where grid `grid` takes `point` (layer space): from where its setup pose (frame 0) has it to where the
        grid shows now, each cell bending as the Coons patch of the four curve segments round it."""
        return _oanim.layer_grid_map(self._id, grid, float(point[0]), float(point[1]))

    @property
    def detail(self):
        """Detail levels: {"levels": each point's level (0: its own points, 1 and up: detail the
        knife added), "hidden": each point hidden at its shape's level, "shape_levels": each shape's
        edit level, "shape_max_levels": each shape's highest detail level, "wires": the points each curve's wire runs through, at its
        edit level}."""
        return dict(_oanim.layer_detail(self._id))

    def shape_outline(self, index, steps=16):
        """(points, closed): shape `index`'s outline as drawn, `steps` samples per curve segment."""
        points, closed = _oanim.layer_shape_outline(self._id, index, steps)
        return list(points), closed

    @property
    def masks(self):
        """Its mask cards; the layer shows only where all of them show it: [{"name" ("" for "Mask 1"
        by its place), "start" (EVERYTHING or
        NOTHING), "cuts" (Affects: ALL, FILLS, LINES), "end_lines" (End Lines at Edge), "ends"
        (BRUSH, FLAT), "reach", "enabled" (in the viewport), "render", "steps"}]. The steps paint on
        the start in order: [{"mode" (ADD paints white, shows; SUBTRACT black, hides), "invert" (the
        outside), "layers" (names; together one shape), "combine" (how they make it: UNION, DIFFERENCE
        the first minus the rest, INTERSECTION where all are), "grow" (pixels, negative shrinks), "shape"
        (AS_DRAWN, FILLS_ONLY, STROKES_ONLY; also as "fills_only" and "strokes_only"), "filled" (Fill
        Every Shape), "inside_stroke", "extend", and "outside" (mode is SUBTRACT) and "expand"
        (grow > 0) as before rows}]; the first step's keys are on its card too. Clip to Below is
        `clip_to_below`. Change them with oanim.ops.mask.add (card= adds a step) / set (step=) /
        remove / move / duplicate / clip_to_below."""
        return [dict(m) for m in _oanim.layer_masks(self._id)]

    def line_shows(self, x, y, render=False):
        """Whether this layer's lines show at (x, y) (document space) by its mask rows with End Lines
        at Edge on (and its groups'): where they don't, the lines end. True with no such row."""
        return _oanim.layer_line_shows(self._id, float(x), float(y), bool(render))

    @property
    def mask_summary(self):
        """Its mask rows in words, as the Masking tab's header says them ("Shows: inside head, minus mouth")."""
        return _oanim.layer_mask_summary(self._id)

    @property
    def masking(self):
        """The names of the layers whose masks name this one (or that clip to it)."""
        return list(_oanim.layer_masking(self._id))

    @property
    def draw_order(self):
        """Its Draw Order now: ("NORMAL", ""), ("FRONT", name of the layer it's drawn in front of) or
        ("BEHIND", name). Set it with oanim.ops.layer.draw_order_set (keyed on frames other than 0)."""
        return tuple(_oanim.layer_draw_order(self._id))

    @property
    def switch_active(self):
        """A switch layer: the name of the child it shows now, or None. Who wins: a key made by hand
        (after frame 0) until a Rig key, then smart actions, then the default. Set it with
        oanim.ops.layer.switch_set(child=name) (keyed on frames other than 0)."""
        return _oanim.layer_switch(self._id)[0]

    @property
    def switch_default(self):
        """A switch layer: the name of the child it shows on frame 0 and wherever nothing else picks, or None."""
        return _oanim.layer_switch(self._id)[1]

    @property
    def hidden_edges(self):
        """[(curve, segment), ...] whose outline is hidden, as with Moho's Hide Edge."""
        return [tuple(e) for e in _oanim.layer_hidden_edges(self._id)]

    def set_selected_edges_visible(self, visible):
        """Show or hide the outline along the selected edges (both points selected), like the
        Shape tab's Edge > Visible. Returns how many edges changed."""
        return _oanim.layer_set_selected_edges_visible(self._id, visible)

    @property
    def text_runs(self):
        """A text layer's runs, in order: dicts with text, font (family), weight (100-900: 400 regular, 700 bold),
        italic, size (document units: the frame is 2 tall), has_fill, has_outline, fill and line (r, g, b, a),
        line_width, brush and material (a shared material's name, "" for its own look). Set it to a list of
        such dicts (keys left out keep their defaults) to restyle the text; layer.text_body is the whole text."""
        return [dict(r) for r in _oanim.layer_text_runs(self._id)]

    @text_runs.setter
    def text_runs(self, runs):
        _oanim.layer_set_text_runs(self._id, [dict(r) for r in runs])

    @property
    def text_bounds(self):
        """A text layer's lines' bounds, layer space: ((min x, min y), (max x, max y))."""
        return _oanim.layer_text_bounds(self._id)

    @property
    def media_clips(self):
        """A Media layer's clips, in time order: dicts with id, filepath, sequence (a numbered image
        sequence), packed (kept inside the .oanim), start (its first frame), length (frames), in_point
        (seconds into the file it starts at), end ("HOLD", "LOOP" or "HIDE": after the file runs out),
        timing ("REAL_TIME" or "FRAME_BY_FRAME"), missing and selected. Change them with
        oanim.ops.media.clip_set(clip=..., start=..., length=..., in_point=...)."""
        return [dict(c) for c in _oanim.layer_media_clips(self._id)]

    @property
    def actions(self):
        """A bone layer's actions: [{"name", "kind": "ACTION", "SMART" or "FIX"}]. Make them with
        oanim.ops.anim.action_new / fix_action_new, edit one with action_set, rename with action_rename."""
        return [dict(a) for a in _oanim.layer_actions(self._id)]

    @property
    def smart_actions_playing(self):
        """The smart actions playing now, their bones turned: [(name, frame, share of its length 0-1)]."""
        return [tuple(p) for p in _oanim.layer_smart_actions_playing(self._id)]

    @property
    def fix_actions(self):
        """A bone layer's fix actions (correctives): [{"name", "drivers" (smart action names), "poses" (the
        poses it fixes: [{"frame" (its frame in the fix, keyed with the correction), "at" (each driver's frame
        in its smart action), "shares" (the same as shares of their lengths, 0-1), "label"}]), "strength" (how
        much of it plays now, 0-1), "nearest_pose" (index of the pose nearest now, -1), "broken" (why it can't
        play, or "")}]. Fix a pose with oanim.ops.anim.fix_action_new (it adds a pose, or opens the one here),
        fix_pose_add(name=, shares="0.5,0.3") and fix_pose_remove(name=, pose=N)."""
        return [dict(f) for f in _oanim.layer_fix_actions(self._id)]

    def field_state(self, path):
        """How the field of channel `path` ("location", 'bones["arm"].angle') shows now: (key state:
        "NONE", "ANIMATED" or "KEYED" on this frame, smart: True while a smart action plays into it,
        drawn pink, its value shown with the action in)."""
        return tuple(_oanim.layer_field_state(self._id, path))

    def media_shown(self, frame=None):
        """What a Media layer shows on `frame` (the current one by default): (clip id, seconds into
        its file; a sequence's image number / 1000), or None between clips."""
        if frame is None:
            frame = data.document.frame_current
        return _oanim.layer_media_shown(self._id, int(frame))

    @property
    def bones(self):
        """A bone layer's bones: dicts with name, parent, position, angle (radians), setup_position
        and setup_angle (the setup pose, frame 0: their keys hold the change from it), length,
        scale, strength, selected, select_head, select_tail, connected, shape, pin_size, shape_layer,
        color_set, colors (its regular, selected and active shades), active, and head and tail (where it runs now, in the bone
        layer's space)."""
        return _oanim.layer_bones(self._id, False)

    @property
    def bones_keyed(self):
        """As bones, with head and tail where the keys put them, without bone dynamics (what
        editing works from, and the viewport draws with Physics Ghosts on)."""
        return _oanim.layer_bones(self._id, True)

    def bone_set(self, name, **values):
        """Pose or edit a bone: position=(x, y), angle, scale=(x, y) (x across the bone, y along
        it; one number sets both), length, strength, name; and how it is drawn: shape ("BONE",
        "PIN" or "CUSTOM", while the viewport shows custom bone shapes), pin_size, shape_layer (a
        vector layer's name, "" for none), color_set ("DEFAULT", "THEME01" to "THEME15" or
        "CUSTOM") and custom_colors (three (r, g, b): regular, selected, active; makes it Custom)."""
        _oanim.layer_bone_set(self._id, name, values)

    def select_bone(self, name, extend=False, part="BONE"):
        """Select bone `name` and make it active ("" deselects all). part: "BONE" (the body and
        both ends), "HEAD" or "TAIL" (one end, as clicking its circle does)."""
        _oanim.layer_select_bone(self._id, name, extend, part)

    def transform_bones(self, offset=(0.0, 0.0), scale=(1.0, 1.0), angle=0.0, pivot=None):
        """The Transform Bones gizmo: scale and turn the selected bones about pivot (default: the
        middle of their bounds), then move them by offset. Unselected children stay put."""
        if pivot is None:
            bounds = self.selected_bone_bounds
            if bounds is None:
                return False
            pivot = ((bounds[0][0] + bounds[1][0]) / 2, (bounds[0][1] + bounds[1][1]) / 2)
        return _oanim.layer_map_bones(self._id, offset[0], offset[1], scale[0], scale[1], angle, pivot[0], pivot[1])

    @property
    def selected_bone_bounds(self):
        """((min x, min y), (max x, max y)) around the selected bones' ends, or None."""
        return _oanim.layer_picked_bounds(self._id)

    def box_select_bones(self, corner_a, corner_b, whole=False, extend=False, deselect=False):
        """Box-select bones (bone layer space). whole=False picks the ends inside, as the Select
        tool does; whole=True picks any bone touching the box, as the Transform Bones tool does."""
        _oanim.layer_box_select_bones(self._id, corner_a[0], corner_a[1], corner_b[0], corner_b[1], whole, extend,
                                    deselect)

    def bone_constraints(self, bone):
        """The constraints of bone `bone`, top first: dicts with type (LIMIT_ANGLE, INDEPENDENT_ANGLE,
        MAINTAIN_VOLUME, COPY_ANGLE, COPY_POSITION, COPY_SCALE, TRACK_TO, STRETCH_TO, IK, DYNAMICS)
        and settings (angles in radians, target as a bone name)."""
        return list(_oanim.layer_bone_constraints(self._id, bone))

    def bone_constraint_set(self, bone, index, **values):
        """Change constraint `index` of bone `bone`: enabled, influence, min_angle, max_angle,
        amount, target, factor, delay, chain, max_stretch, weight, wind_catch, own_gravity (with
        gravity), own_wind (with wind, wind_direction (radians), wind_gusts, wind_gust_speed,
        wind_turbulence), own_forces (both own_gravity and own_wind), angle_spring / position_spring /
        scale_spring (dicts of enabled, settle_speed, bounce, inertia, stretch, use_limit
        and limit: radians for angle_spring, bone layer units for position_spring, a fraction of
        the size for scale_spring)."""
        _oanim.layer_bone_constraint_set(self._id, bone, index, values)

    def rotate_bones(self, angle):
        """R on the selected bones by angle (radians), each about its base; children come along.
        A picked child of a picked parent doesn't turn twice."""
        return _oanim.layer_rotate_bones(self._id, angle)

    def scale_bones(self, factor, axis="", squash=False):
        """S on the selected bones, children along with them (once). On frame 0 it changes their
        size (lengths and places, about the middle of their bases). Posing, it scales each along
        its own axes: axis "" both, "X" across (thickness), "Y" along (length); squash=True is
        S S, longer and thinner (or shorter and fatter), keeping their area."""
        return _oanim.layer_scale_bones(self._id, factor, {"": 0, "X": 1, "Y": 2}[axis.upper()], squash)

    def move_bone_ends(self, offset):
        """G on the selected bones by offset=(dx, dy): a bone picked by its body or base moves,
        one picked by its tip turns to follow it (and on frame 0 stretches). Children come along.
        Returns False when nothing is selected."""
        return _oanim.layer_move_bone_ends(self._id, offset[0], offset[1])

    def drag_bone_ends(self, offsets):
        """One G drag through each (dx, dy) in offsets in turn, every one measured from the
        drag's start, as dragging in the viewport does. Returns False when nothing is selected."""
        return _oanim.layer_drag_bone_ends(self._id, [(float(x), float(y)) for x, y in offsets])

    @property
    def point_bones(self):
        """The bone that moves each point most, "" when none does."""
        return list(_oanim.layer_point_bones(self._id))

    @property
    def parent_bone(self):
        """The bone this whole layer follows (Parent to Bone), or ""."""
        return _oanim.layer_parent_bone(self._id)

    @property
    def bone_collections(self):
        """A bone layer's bone collections, in list order, All first (fixed: every bone and every
        drawing Editable in Bone Group; nothing is assigned to it): dicts with name, visible (the eye),
        solo (the star), shown, active, fixed, bones and layers (the drawings in it). Change them with the
        armature.collection_* operators (add, remove, move, assign, unassign, select, deselect,
        set) and armature.move_to_collection / assign_to_collection (M and Shift M)."""
        return [dict(c) for c in _oanim.layer_bone_collections(self._id)]

    @property
    def collections(self):
        """The bone collections this drawing is in (names, of its bone layer); layer.bone_collection_toggle
        puts it in one or takes it out."""
        return list(_oanim.layer_collections(self._id)[0])

    @property
    def edited_with_bones(self):
        """Whether this drawing's points are picked and moved along with its bone layer's bones now: it
        is in a bone collection that shows."""
        return _oanim.layer_collections(self._id)[1]

    def keyed_at(self, frame):
        """The paths of this layer's channels keyed at `frame`, as the timeline's Keyed Here list
        has them (Ctrl-click a key)."""
        return list(_oanim.layer_keyed_at(self._id, frame))

    @property
    def parent_bone_deform(self):
        """What Parent Bone moves: "LAYER" (the layer as a whole, lines and stamps squashing with the
        bone) or "POINTS" (its points; lines keep their width). Set with layer.parent_bone_set(deform=)."""
        return _oanim.layer_parent_bone_deform(self._id)

    @property
    def parent_bone_group(self):
        """The group above whose Parent Bone overrides this layer's own (and its weights), or ""."""
        return _oanim.layer_parent_bone_group(self._id)

    def to_document(self, point):
        """Where `point` (layer space) shows in the document, bones and parents included."""
        return _oanim.layer_to_document(self._id, tuple(point))

    def bone_weights(self, bone):
        """How strongly bone `bone` moves each point, 0-1, from its weights."""
        return list(_oanim.layer_bone_weights(self._id, bone))

    @property
    def weights(self):
        """Each point's weights, {bone name: weight} ({} when not weighted yet)."""
        return list(_oanim.layer_painted_weights(self._id))

    painted_weights = weights

    def weight_paint(self, center, radius, bone, weight=1.0, strength=1.0, mode="DRAW", normalize=True,
                     selected_only=False, keep_animation=False):
        """One dab of the Weight Paint brush at `center` (layer space): mode "DRAW", "ADD" or
        "SUBTRACT"; selected_only paints only selected points (Selection Mask); keep_animation keeps
        the drawing where it shows on every keyed frame of every action. Returns how many points
        changed."""
        return _oanim.layer_weight_paint(self._id, tuple(center), radius, bone, weight, strength, mode, normalize,
                                       selected_only, keep_animation)

    @property
    def modifiers(self):
        """The modifier stack, top first: dicts with type ("MIRROR", "ARMATURE", "BEVEL", "BOOLEAN", "RESAMPLE",
        "CURVE_DEFORM", "CAGE_DEFORM" or "MESH_DEFORM") and settings. A deform's: layer and shape (names), kind ("GRID", "CAGE",
        "LINE" as "Grid"..., "" when it has none), weights ("AUTOMATIC": every point fully, or "CUSTOM"), group, strength,
        fits (the deform type that takes its shape: its own, or the one to switch to), precision (how far a bent segment may stray from the true bend before points are added; 0 off), stretch
        (a line: what lies along it spreads as it grows), resample_self (the drawing sampled evenly, bent, and its own points and handles fitted
        to that; no points added), cage_method (a cage: "MEAN_VALUE", "RIGID": the inside as rigid as possible, or
        "ENVELOPE": Toon Boom Harmony's, each side keeping what lies near it at its distance from it),
        blend (Envelope: 0 the nearest side leads sharply, 1 gently)."""
        return _oanim.layer_modifiers(self._id)

    def modifier_set(self, index, **values):
        """Change modifier `index`'s settings by name, as modifiers lists them. A deform modifier takes layer then
        shape by name (one it takes: an open line for Curve Deform, a closed shape for Cage Deform, a grid or mesh of
        cells for Mesh Deform); type= changes a deform's type, keeping its settings and weights; weights="CUSTOM" starts its point group from the automatic weights."""
        _oanim.layer_modifier_set(self._id, index, values)

    def deform_weights(self, index, reset=False):
        """Deform modifier `index`'s weight for each point (Custom, else automatic), before Strength.
        reset=True first fills its Custom group with the automatic weights (Reset to Automatic)."""
        return _oanim.layer_deform_weights(self._id, index, reset)

    def evaluated(self, render=False):
        """The drawing as it shows after its modifiers: (points [(x, y)], curve count,
        shapes [(name, closed, connected, merged, area, fill)]): merged, what another layer's Boolean
        modifier draws of it instead ("ALL", "FILL", "LINE" or ""); area, what a closed one fills;
        fill, its fill colour (r, g, b, a)."""
        points, curves, shapes = _oanim.layer_evaluated(self._id, render)
        return [tuple(p) for p in points], curves, [tuple(s) for s in shapes]

    def evaluated_outline(self, shape, steps=16, render=False):
        """(points, closed): shape `shape`'s outline as the drawing shows after its modifiers, `steps` samples per
        curve segment."""
        points, closed = _oanim.layer_evaluated_outline(self._id, shape, steps, render)
        return [tuple(p) for p in points], closed

    def channel_pinned(self, path):
        """Whether channel `path` is pinned to the timeline (its pin button)."""
        return _oanim.layer_channel_pinned(self._id, path)

    def pin_channel(self, path, pinned=True):
        """Pin channel `path` to the timeline (or unpin it), as its pin button does."""
        from . import ops
        return ops.anim.channel_pin(layer=str(self._id), paths=path, pinned="1" if pinned else "0")

    def keyframes(self, path):
        """The keys of channel `path` ("position", "rotation", "scale", "opacity",
        "points[3].pos", "points[3].radius", "curves[0].points[2].curvature", ...): dicts with
        "frame", "value", "interp" and the key's settings."""
        return _oanim.layer_keyframes(self._id, path)

    def keyframe_insert(self, path="", frame=None):
        """Key channel `path` (or, with no path, every channel of the layer) at `frame` (default:
        the current frame) with its current value, like Blender's I."""
        _oanim.layer_keyframe_insert(self._id, path, -1 if frame is None else frame)

    def keyframe_delete(self, path, frame):
        return _oanim.layer_keyframe_delete(self._id, path, frame)

    def keyframe_set(self, path, frame, **settings):
        """Change one key: interp="LINEAR"|"SMOOTH"|"EASE_IN_OUT"|"EASE_IN"|"EASE_OUT"|"STEP"|
        "BEZIER"|"BACK"|"BOUNCE"|"ELASTIC"|"NOISY"|"CYCLE"|"POSE", ease="SINE"|"QUAD"|"CUBIC"|
        "QUART"|"QUINT"|"EXPO"|"CIRC" (how strongly the Ease modes slow), hold, interval,
        overshoot, elastic_amplitude, elastic_period, noise_amplitude, noise_frequency,
        cycle_back."""
        _oanim.layer_keyframe_set(self._id, path, frame, settings)

    @property
    def key_frames(self):
        """Frames where the layer or its drawing has keys."""
        return list(_oanim.layer_key_frames(self._id))

    def curve_handles(self, curve):
        """[(alignment, (in_x, in_y), (out_x, out_y)), ...] for each point of curve `curve`:
        "ALIGNED" (its handles stay in line) or "FREE", and its handle positions in layer space."""
        return [(t, tuple(i), tuple(o)) for t, i, o in _oanim.layer_curve_handles(self._id, curve)]

    def evaluated_handles(self, curve, shown=False):
        """As curve_handles, after the modifiers: shown=True as the viewport shows them while editing (a deform's
        segments each as one Bezier, without the points Precision adds)."""
        return [(t, tuple(i), tuple(o)) for t, i, o in _oanim.layer_evaluated_handles(self._id, curve, shown)]

    def move_handle(self, curve, index, side, pos):
        """Drag one handle ("IN" or "OUT") of a curve point to `pos`, as in the viewport: an
        aligned point's other handle turns to stay in line."""
        _oanim.layer_move_handle(self._id, curve, index, side, tuple(pos))

    def select_handle(self, curve, index, side):
        _oanim.layer_select_handle(self._id, curve, index, side)

    def shape_brush(self, index):
        """Shape `index`'s brush settings as a dict with Moho's .mohobrush keys; "name" is ""
        for a plain line."""
        return _oanim.layer_shape_brush(self._id, index)

    def set_active_shape(self, index):
        """Make shape `index` the active shape, as clicking it does (the one Link Materials passes on)."""
        _oanim.layer_set_active_shape(self._id, int(index))

    def shape_material(self, index):
        """The name of the material shape `index` is linked to, or "" when its look is its own."""
        return self.shape_style(index)["material"]

    def shape_style(self, index):
        """Shape `index`'s look: {fill, line (r, g, b, a), line_width, has_fill, has_outline, hidden,
        fill_rule ("LOOPS": each loop filled on its own; "EVEN_ODD": a loop inside another a hole; "NONZERO":
        a loop drawn the other way round a hole), brush, material}."""
        return dict(_oanim.layer_shape_style(self._id, index))

    def set_shape_style(self, index, **values):
        """Change shape `index`'s look (fill, line, line_width, has_fill, has_outline, fill_rule), as the Shape
        panel does. On a reference this becomes its own look for that setting."""
        _oanim.layer_set_shape_style(self._id, index, values)

    @property
    def point_ids(self):
        """Each point's id, kept for good whatever happens to the points around it."""
        return list(_oanim.layer_point_ids(self._id))

    @property
    def shape_ids(self):
        return list(_oanim.layer_shape_ids(self._id))

    @property
    def reference(self):
        """A reference's settings: {edit_points, follow_animation, match_original} (change them with
        oanim.ops.layer.reference_set)."""
        return dict(_oanim.layer_reference(self._id))

    def reference_overrides(self):
        """A reference's own changes: {"points": {point id: (dx, dy, keyed)}, "handles": {(point id,
        toward point id, "IN" or "OUT"): (dx, dy, keyed)}, "looks": {shape id: {setting: value}},
        "swaps": [(from, to, fill, line)], "weights": [point ids], "channels": [layer channels it took
        over]}."""
        d = _oanim.layer_reference_overrides(self._id)
        return {"points": dict(d["points"]), "looks": {k: dict(v) for k, v in d["looks"].items()},
                "swaps": list(d["swaps"]), "weights": list(d["weights"]), "handles": dict(d["handles"]),
                "channels": list(d["channels"])}

    def shown(self, level=None):
        """Where the points and handles show, bones and parents included, in document space (where a
        guide or reference over the drawing is): (points [(x, y)], curves [([(point, (in_x, in_y),
        (out_x, out_y), aligned, hidden, detail_level, index), ...], closed)]). Handles are where their
        ends show. As drawn (level None), or the curves as they are at detail `level`: only the points up
        to it, with that level's handles. `index` is the point's place in the whole curve, what
        place_handle takes."""
        points, curves = _oanim.layer_shown(self._id, -1 if level is None else int(level))
        return [tuple(p) for p in points], [([(c[0], tuple(c[1]), tuple(c[2]), c[3], c[4], c[5], c[6]) for c in cps], closed)
                                            for cps, closed in curves]

    def set_detail_level(self, level, shapes=None):
        """Set shapes' detail edit level (all, or those indices), as Detail Level does for the selected
        ones: edits then move that level's points, and finer detail rides along."""
        _oanim.layer_detail_level_set(self._id, int(level), list(shapes or []))

    def detail_sync(self):
        """Place the detail points now, as the viewport does each frame: an edited one keeps where it is,
        the rest ride their coarser curve (by each shape's edit level)."""
        _oanim.layer_detail_sync(self._id)

    def place_point(self, index, pos):
        """Move point `index` so it shows at `pos` (document space), the bones' bend undone, as a G drag
        places it; its handles come along. Keyed (with auto-key) when the next undo step records it."""
        _oanim.layer_place_point(self._id, index, (float(pos[0]), float(pos[1])))

    def place_handle(self, curve, index, side, pos, keep_other=False):
        """Move one handle ("IN" or "OUT") of point `index` of curve `curve` so its end shows at `pos`
        (document space). An aligned point's other handle turns to stay in line unless `keep_other`."""
        _oanim.layer_place_handle(self._id, curve, index, side, (float(pos[0]), float(pos[1])), bool(keep_other))

    def set_point(self, index, pos):
        """Move point `index` to `pos` (layer space), as dragging it in the viewport does."""
        _oanim.layer_set_point(self._id, index, tuple(pos))

    def split_segment(self, curve, segment, t=0.5):
        """Add a point on segment `segment` of curve `curve`, `t` along it, as the Split tool does
        (on an animated curve it takes its neighbours' motion). Returns the new point's index."""
        return _oanim.layer_split_segment(self._id, curve, segment, t)

    def move_points(self, offset):
        """G on the selected points by offset=(dx, dy) (layer space), with mirror editing as in the
        viewport: each point's mirror image in the setup pose snaps to its reflection, on any frame.
        False with nothing selected."""
        return _oanim.layer_move_points(self._id, offset[0], offset[1])

    def select_points(self, indices, select=True):
        _oanim.layer_select_points(self._id, list(indices), select)

    def __getattr__(self, name):
        return getattr(_oanim.layer_pointer(self._id), name)

    def __setattr__(self, name, value):
        # Properties defined here (mean_radius) set themselves; the rest are RNA properties.
        prop = getattr(type(self), name, None)
        if isinstance(prop, property) and prop.fset:
            prop.fset(self, value)
            return
        setattr(_oanim.layer_pointer(self._id), name, value)

    def __dir__(self):
        return sorted(set(dir(_oanim.layer_pointer(self._id))) | {"id", "children", "parent", "reference_source",
                                                                 "points", "radii", "mean_radius", "shapes", "point_count", "curve_count", "shape_count",
                                                                 "add_curve", "add_primitive", "shape_outline", "select_points", "grids", "extrude_selected", "shape_runs",
                                                                 "grid_map"})

    def __eq__(self, other):
        return isinstance(other, _Layer) and other._id == self._id

    def __hash__(self):
        return hash(self._id)

    def __repr__(self):
        return f"<oanim Layer {self.name!r}>"


class _Layers:
    """The document's layers, like Moho's layer list. Iterating gives the top level, top first."""

    def __iter__(self):
        return iter([_Layer(i) for i in _oanim.layer_ids()])

    def __len__(self):
        return len(_oanim.layer_ids())

    def __getitem__(self, key):
        if isinstance(key, int):
            return _Layer(_oanim.layer_ids()[key])
        for layer in self.all():
            if layer.name == key:
                return layer
        raise KeyError(key)

    def all(self):
        """Every layer, depth first, top first."""
        result = []

        def walk(ids):
            for i in ids:
                result.append(_Layer(i))
                walk(_oanim.layer_children(i))

        walk(_oanim.layer_ids())
        return result

    @property
    def active(self):
        active = _oanim.layer_active()
        return _Layer(active) if active else None

    @active.setter
    def active(self, layer):
        _oanim.layer_set_active(layer.id)


class _Camera(_Layer):
    """The camera: what renders show, a frame on the infinite canvas (zoom 1 at the origin is 2
    units tall). A layer outside the tree, for keyframes(path) and anim.keyframe_toggle with
    "position" (its centre), "rotation" (its roll, degrees) and "zoom" (whose keys hold log2 of the
    zoom, so it changes at a steady rate between them). `selected`: picked, the viewport looks
    through it."""

    def __init__(self):
        super().__init__(2 ** 64 - 3)

    @property
    def position(self):
        return (self.location_x, self.location_y)

    @position.setter
    def position(self, value):
        self.location_x, self.location_y = value

    @property
    def zoom(self):
        return _oanim.context().document.camera_zoom

    @zoom.setter
    def zoom(self, value):
        _oanim.context().document.camera_zoom = value

    @property
    def roll(self):
        return _oanim.context().document.camera_roll

    @roll.setter
    def roll(self, value):
        _oanim.context().document.camera_roll = value

    def to_frame(self, point):
        """Where a document point is in the camera's frame, as renders see it: the frame is
        [-aspect, aspect] x [-1, 1]."""
        return _oanim.camera_to_frame(point[0], point[1])

    @property
    def passepartout(self):
        """How dark the canvas outside the frame is in the viewport, 0 to 1."""
        return _oanim.context().document.camera_passepartout

    @passepartout.setter
    def passepartout(self, value):
        _oanim.context().document.camera_passepartout = value


class _Data:
    """All saved data: the document, its layers and its text blocks."""

    @property
    def document(self):
        return _oanim.context().document

    @property
    def simulation(self):
        """The document's gravity and wind as animatable channels ("gravity", "wind",
        "wind_direction", "wind_gusts", "wind_gust_speed", "wind_turbulence"): a layer outside the
        tree, for keyframes(path) and anim.keyframe_toggle(layer=str(simulation.id), paths=...)."""
        return _Layer(2 ** 64 - 2)

    @property
    def camera(self):
        """The camera (see _Camera)."""
        return _Camera()

    @property
    def draw_order(self):
        """The names of the drawing layers as drawn now, bottom first: the Layers editor's order
        with every layer's Draw Order rule applied (hidden layers left out)."""
        return list(_oanim.draw_order())

    @property
    def materials(self):
        """The shared looks shapes link to: [{"name", "users" (shapes using it), "fill", "line",
        "line_width", "brush"}]. Change them with oanim.ops.material.new / assign / unlink / rename /
        remove / remove_unused; a linked shape's set_shape_style changes its material."""
        return [dict(m) for m in _oanim.materials()]

    @property
    def drivers(self):
        """Values worked out every frame from other values (Blender's drivers): [{"target": {"layer",
        "path", "component"}, "label", "type" (EXPRESSION, AVERAGE, SUM, MIN, MAX), "expression",
        "vars": [{"name", "source": {...}}], "remap", "from", "to", "clamp", "error" ("" when it works),
        "value"}]. Change them with oanim.ops.anim.driver_add / driver_set / driver_remove /
        driver_copy_new / driver_copy / driver_paste."""
        return [dict(d) for d in _oanim.drivers()]

    @property
    def annotations(self):
        """Notes over the drawing (Blender's annotation layers; never rendered, on every frame):
        [{"name", "color" (r, g, b, a), "thickness" (screen pixels), "hidden", "locked", "strokes":
        [{"points": [(x, y)] in document space, "start", "end" (NONE, ARROW, ARROW_OPEN, SEGMENT,
        DOT)}]}]. Change them with oanim.ops.annotation.draw / erase / layer_add / layer_remove /
        layer_set."""
        return [dict(a) for a in _oanim.annotations()[0]]

    @property
    def active_annotation(self):
        """The index of the annotation layer new strokes go on (-1 with none)."""
        return _oanim.annotations()[1]

    @property
    def guides(self):
        """Drawing guides, visual aids over the drawing (never rendered; drawing ignores them):
        [{"type" (LINE, PERSPECTIVE, GRID), "points" (its handles, document space: a line's two, a
        perspective's vanishing points, a grid's origin and cell corner), "lines" (perspective: how
        many round each point), "hidden", "locked"}]. Change them with oanim.ops.guide.add / set /
        move_point / delete."""
        return [dict(g) for g in _oanim.guides()[0]]

    @property
    def active_guide(self):
        """The index of the picked guide (-1 with none)."""
        return _oanim.guides()[1]

    @property
    def filepath(self):
        """The .oanim file the document was opened from or saved to; "" for a new document."""
        return _oanim.file_state()[0]

    @property
    def is_dirty(self):
        """True when there are changes since the document was saved or opened."""
        return _oanim.file_state()[1]

    layers = _Layers()
    texts = _Texts()


data = _Data()


class _Assets:
    """The asset library: the folder (set in Preferences) holding the app's brushes."""

    @property
    def library(self):
        """The asset library folder; "" until one is chosen."""
        return _oanim.asset_library()

    def brushes(self):
        """The library's brush shapes (Brush Shapes): dicts of "name" and "images", the number of
        images (more than one for a multi-image shape). Their settings live in presets."""
        return _oanim.brushes()

    def brush_presets(self, library=False):
        """Brush presets: this file's (each carrying a copy of its shape's images), or with
        `library` the asset library's. Dicts: "name", "brush" (its settings, as brushes() gives),
        "shape", "shape_missing" (no such shape here: loading keeps the current one) and
        "shape_images" (how many images a file preset carries)."""
        return _oanim.brush_presets(library)

    def brush_categories(self):
        """The library's brush categories: its folders of Brushes (Basic's built-ins included)."""
        return _oanim.brush_categories()

    def brush_dab_count(self, name, length, width):
        """How many dabs brush `name` lays along a straight line `length` long and `width` wide
        (document units), with its default settings."""
        return _oanim.brush_dab_count(name, length, width)


assets = _Assets()


def layers_at(point):
    """The layers drawn at `point` (document space), top first, as Select Layer finds them (a fill's
    hole shows what is under it)."""
    return [_Layer(i) for i in _oanim.layers_at(float(point[0]), float(point[1]))]


def text_fonts():
    """The installed font families (what a text run's font can be), sorted."""
    return list(_oanim.text_fonts())


def text_font_installed(family):
    return _oanim.text_font_installed(family)


def text_font_from_postscript(name):
    """A font by its PostScript name ("Arial-Black", as Flash files name them): (family, weight, italic),
    or None when no installed font has it."""
    return _oanim.text_font_from_postscript(name)


def __getattr__(name):
    # The context changes as the mouse moves between editors, so it is looked up on access.
    if name == "context":
        return _oanim.context()
    raise AttributeError(f"module 'oanim' has no attribute '{name}'")
