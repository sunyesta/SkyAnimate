#pragma once

/* Vector drawings, following Moho's model (M_Mesh, M_Curve, M_Shape):
 *
 * - A drawing owns a pool of points. Curves run through points by index, so two curves can
 *   share a point; that is how lines are welded together.
 * - Curves are cubic Beziers. Every handle is free, as Blender's Free handles: stored, moved only
 *   by hand. A new point gets handles once, smooth from Moho's spline (curvature, weight and
 *   offset) or straight toward its neighbours; Align keeps a point's two handles in line.
 * - A shape is a closed or open run of curve segments with a fill and an outline. Curves
 *   without a shape only show in the editor, as in Moho.
 *
 * Positions are in layer space. Document space has the camera frame 2 units tall, y up. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "anim/channel.hh"
#include "core/math.hh"
#include "data/grid.hh"

namespace oanim {

/* Moho's curvature presets (MOHO.SMOOTH, MOHO.PEAKED). */
inline constexpr float CURVATURE_SMOOTH = 0.3f;
inline constexpr float CURVATURE_PEAKED = 0.0f;


/* How strongly one bone moves a point (Blender's vertex group weight), 0-1. */
struct BoneWeight {
  uint64_t bone = 0;
  float weight = 0.0f;
  bool operator==(const BoneWeight &) const = default;
};

/* A detail point's place on its parents' segment (see data/detail.hh). Base points have level 0. */
struct PointDetail {
  int level = 0;
  /* Where along the parents' segment, 0 to 1. */
  float t = 0.5f;
  /* Offsets of the point and its handles from where the segment split there puts them, along and
   * across the segment at t, in lengths of it; and their setup pose (frame 0). Its keys hold the
   * change from the setup (see drawing_changes_from_values). */
  float2 pos, in, out;
  float2 setup_pos, setup_in, setup_out;
  /* The point and handles as last placed: one that differs since was moved, so its offsets are
   * taken from it. Not saved. */
  float2 built_pos, built_in, built_out;
  bool built = false;
  /* Above its curve's edit level: not shown, picked or selected. Not saved. */
  bool hidden = false;
  bool operator==(const PointDetail &) const = default;
};

struct MeshPoint {
  float2 pos;
  /* Stroke radius here as a fraction (0-1) of the line width of the shape drawing the stroke,
   * like a Grease Pencil point's radius. The draw tool sets it from pen pressure. */
  float radius = 1.0f;
  bool selected = false;
  /* How the bones of the bone layer it sits in move the point, blended by weight (Blender's vertex
   * groups). Empty: not weighted yet; it gets automatic weights once it is inside a bone layer
   * with bones. Weights that are all 0 (or only for bones that are gone) leave it where it is. */
  std::vector<BoneWeight> weights;
  /* The setup pose (frame 0): where the point rests. Its position keys hold its Transform, the
   * change from this, applied before bones bend the drawing (so a keyed shape follows the bones,
   * and reshaping frame 0 keeps every pose's motion). Set by editing on frame 0. */
  float2 setup_pos;
  /* Transform: pos less setup_pos, the value pos_keys animate (see drawing_changes_from_values). */
  float2 transform;
  /* Keys for the position (as its Transform) and radius (see anim/channel.hh). */
  Channel<float2> pos_keys;
  Channel<float> radius_keys;
  /* Its own number in the drawing, kept for good (0: not given one yet; see drawing_ensure_ids):
   * what a reference's overrides find it by, whatever happens to the points around it. */
  uint64_t id = 0;
  /* Evaluated drawings only, never saved: a point the Resample modifier made with Preserve Volume, which
   * the bones move by blending their turns and moves as rigid motions (dual complex numbers, Blender's
   * Preserve Volume) instead of their matrices, so it doesn't pinch or curl when they turn far apart. */
  bool preserve_volume = false;
  /* Evaluated drawings only, never saved: the modifier that made the point (its place in the layer's stack), -1
   * for the layer's own points. What Show Points marks. */
  int made_by = -1;
  /* Detail levels: a detail point's level and offsets. */
  PointDetail detail;
  bool operator==(const MeshPoint &) const = default;
};

struct CurvePoint {
  /* Index into VectorDrawing::points. */
  int point = 0;
  float curvature = CURVATURE_SMOOTH;
  float weight_in = 1.0f;
  float weight_out = 1.0f;
  /* Handle rotation in radians. */
  float offset_in = 0.0f;
  float offset_out = 0.0f;
  /* Moho's Hide Edge: the segment from this point to the next draws no outline. Fills still
   * use it as a boundary. */
  bool edge_hidden = false;
  /* The handles, as offsets from the point. They only move when moved (or with their point). */
  float2 handle_in;
  float2 handle_out;
  /* Align (the right-click menu): the two handles stay in a line, so moving one turns the other. */
  bool aligned = false;
  /* False until the point first gets its handles (drawing_init_handles). Until then they are
   * worked out from the neighbours: smooth, or straight toward them where curvature is 0. */
  bool handles_set = false;
  /* Evaluated drawings only, never saved: handles the Resample modifier stored for points whose
   * handles the curve works out (and for its new points). The bones turn and stretch them with the
   * curve around the point, as handles worked out from where the points went would, rather than
   * rigidly with the point. */
  bool handles_follow_curve = false;
  /* Handles picked on their own (clicking one). A selected point's handles move with it. */
  bool select_in = false;
  bool select_out = false;
  /* The handles in the setup pose (frame 0), as offsets from the point. Their keys hold Handle
   * Angle and Length, the change from these: (turn in radians, length added), so a handle swings
   * along an arc between keys and reshaping frame 0 keeps every pose's turn. */
  float2 setup_in;
  float2 setup_out;
  /* Handle Angle and Length now: what handle_in_keys and handle_out_keys animate. */
  float2 change_in;
  float2 change_out;
  /* Keys for the curvature and the handles (as Handle Angle and Length). */
  Channel<float> curvature_keys;
  Channel<float2> handle_in_keys;
  Channel<float2> handle_out_keys;
  bool operator==(const CurvePoint &) const = default;
};

struct Curve {
  std::vector<CurvePoint> points;
  bool closed = false;

  int segment_count() const;
  bool operator==(const Curve &) const = default;
};

/* How a line is painted with a brush: Moho's style brush settings (fBrush*). Picking a brush
 * copies its defaults here; each shape can then change its own. */
struct BrushSettings {
  /* The brush's name in the asset library. Empty: a plain, solid line. */
  std::string name;
  /* Turn each dab to follow the line (Moho's "Tangent"); off, dabs stay upright. */
  bool align = false;
  /* Random turn of each dab, radians (2 pi: any angle). */
  float jitter = 0.0f;
  /* Distance between dabs, as a fraction of the line width. */
  float spacing = 0.25f;
  /* A slow, smooth turn of the dabs along the line, radians. */
  float angle_drift = 0.0f;
  /* Line boil: a new random pattern every random_interval frames. */
  bool randomize = false;
  /* Multi-brushes: pick the next image at random rather than in turn. */
  bool random_order = true;
  /* Dabs don't build up where they overlap: the line's alpha is the strongest dab's. */
  bool merged_alpha = false;
  /* The line's width wavers by up to this fraction, in waves about size_variation_scale of
   * the frame height long. */
  float size_variation_amp = 0.0f;
  float size_variation_scale = 0.3f;
  /* Line boil: frames each random pattern holds. */
  int random_interval = 1;
  /* Colour brushes take the line colour instead of their own. */
  bool tint = false;
  /* Random change of each dab's hue, saturation and value (fractions). */
  float hue_drift = 0.0f;
  float sat_drift = 0.0f;
  float val_drift = 0.0f;

  /* The preset these settings were loaded from or saved as ("" for none): the one the UI names first
   * when several match, and the card it brings into view after saving. Not part of what a preset
   * holds. */
  std::string preset;

  /* Photoshop's Brush Tip Shape: each stamp turned by tip_angle (radians), squashed across to
   * `roundness` (1: as drawn), mirrored. `hardness` is the built-in Round tip's edge (1 hard, 0 soft). */
  float tip_angle = 0.0f;
  float roundness = 1.0f;
  bool flip_x = false;
  bool flip_y = false;
  float hardness = 1.0f;
  /* How stamps come and go as the line stretches and squashes (brush_stroke fills the gaps of a
   * stretched segment and thins a squashed one): fading in and out, or off: popping in and out
   * whole, halfway through the fade. */
  bool pop_in = false;

  /* Photoshop's sections, each on or off; one switched off keeps its values but doesn't apply.
   * Shape Dynamics holds align, jitter, angle drift and the size variation too; Color Dynamics the
   * hue, saturation and value drift. */
  bool shape_dynamics = true;
  bool scattering = true;
  bool color_dynamics = true;
  bool transfer = true;

  /* Shape Dynamics: each stamp's size random, down to 1 - size_jitter (never under size_min, a
   * fraction); the size growing over the first size_fade_in stamps and shrinking over the last
   * size_fade_out (0: no fade); each stamp's roundness random down to 1 - roundness_jitter. */
  float size_jitter = 0.0f;
  float size_min = 0.0f;
  int size_fade_in = 0;
  int size_fade_out = 0;
  float roundness_jitter = 0.0f;

  /* Scattering: stamps pushed off the line by up to `scatter` line widths (across it, or along it
   * too with scatter_both), `count` stamps at each place, randomly fewer by count_jitter. */
  float scatter = 0.0f;
  bool scatter_both = false;
  int count = 1;
  float count_jitter = 0.0f;

  /* Transfer: each stamp's opacity random, down to 1 - opacity_jitter; fading in over the first
   * opacity_fade_in stamps and out over the last opacity_fade_out. */
  float opacity_jitter = 0.0f;
  int opacity_fade_in = 0;
  int opacity_fade_out = 0;

  bool operator==(const BrushSettings &) const = default;
};

/* Every brush setting: X(type, field, "pythonAndFileKey"). */
#define OANIM_BRUSH_SETTINGS(X) \
  X(std::string, name, "name") \
  X(bool, align, "align") \
  X(float, jitter, "jitter") \
  X(float, spacing, "spacing") \
  X(float, angle_drift, "angleDrift") \
  X(bool, randomize, "randomize") \
  X(bool, random_order, "randomOrder") \
  X(bool, merged_alpha, "mergedAlpha") \
  X(float, size_variation_amp, "sizeVariationAmp") \
  X(float, size_variation_scale, "sizeVariationScale") \
  X(int, random_interval, "randomInterval") \
  X(bool, tint, "tint") \
  X(float, hue_drift, "hueDrift") \
  X(float, sat_drift, "satDrift") \
  X(float, val_drift, "valDrift") \
  X(std::string, preset, "preset") \
  X(float, tip_angle, "tipAngle") \
  X(float, roundness, "roundness") \
  X(bool, flip_x, "flipX") \
  X(bool, flip_y, "flipY") \
  X(float, hardness, "hardness") \
  X(bool, pop_in, "popIn") \
  X(bool, shape_dynamics, "shapeDynamics") \
  X(bool, scattering, "scattering") \
  X(bool, color_dynamics, "colorDynamics") \
  X(bool, transfer, "transfer") \
  X(float, size_jitter, "sizeJitter") \
  X(float, size_min, "sizeMin") \
  X(int, size_fade_in, "sizeFadeIn") \
  X(int, size_fade_out, "sizeFadeOut") \
  X(float, roundness_jitter, "roundnessJitter") \
  X(float, scatter, "scatter") \
  X(bool, scatter_both, "scatterBoth") \
  X(int, count, "count") \
  X(float, count_jitter, "countJitter") \
  X(float, opacity_jitter, "opacityJitter") \
  X(int, opacity_fade_in, "opacityFadeIn") \
  X(int, opacity_fade_out, "opacityFadeOut")

/* What changed from `before` to `after`, copied into `target` (the rest of it stays): editing one
 * setting of several shapes changes only that setting, keeping their other differences. */
inline void brush_settings_apply_changes(const BrushSettings &before, const BrushSettings &after,
                                         BrushSettings &target)
{
#define OANIM_BRUSH_APPLY(type, field, key) \
  if (!(before.field == after.field)) { \
    target.field = after.field; \
  }
  OANIM_BRUSH_SETTINGS(OANIM_BRUSH_APPLY)
#undef OANIM_BRUSH_APPLY
}

/* The settings without the preset they came from, for comparing with presets. */
inline BrushSettings brush_settings_unnamed(BrushSettings b)
{
  b.preset.clear();
  return b;
}

/* A brush preset (assets/brush_presets.hh): a name and the settings it loads, `settings.name` being
 * its brush shape. A file preset also keeps a copy of the shape's images (file names and PNG
 * bytes), so the file carries them. */
struct BrushPreset {
  std::string name;
  /* A library preset's group: the subfolder of Brush Presets it's in ("" at the top), as
   * Photoshop's brush folders. */
  std::string group;
  /* Built into the app (Basic's Hard Round and Soft Round): in no folder, and can't be deleted. */
  bool builtin = false;
  BrushSettings settings;
  std::vector<std::string> shape_files;
  std::vector<std::string> shape_data;
  bool operator==(const BrushPreset &) const = default;
};

/* How wide a line can be at a point, as a multiple of its shape's line width (1: the width itself). */
inline constexpr float POINT_RADIUS_MAX = 10.0f;

struct Style {
  Color fill = {0.95f, 0.85f, 0.7f, 1.0f};
  Color line = {0.0f, 0.0f, 0.0f, 1.0f};
  /* Line width in document units (the frame is 2 units tall). */
  float line_width = 0.008f;
  BrushSettings brush;
  bool operator==(const Style &) const = default;
};

/* How a shape of several loops fills (Moho's holes, SVG's fill-rule). Loops: each loop filled on its own,
 * as shapes were before holes (and what files from then keep). Even-Odd: a loop inside another is a hole
 * (and one inside that filled again), whichever way it was drawn. Nonzero: each loop's direction counts,
 * so one drawn the other way round cuts a hole and one the same way adds to the fill (how fonts are made,
 * their overlapping strokes staying filled). */
enum class FillRule { Loops, EvenOdd, NonZero };

/* One curve segment: from curve point `segment` to the next one. */
struct ShapeEdge {
  int curve = 0;
  int segment = 0;
  bool operator==(const ShapeEdge &) const = default;
};

struct Shape {
  std::string name;
  /* Segments in order around the shape. */
  std::vector<ShapeEdge> edges;
  bool has_fill = true;
  bool has_outline = true;
  /* How its loops fill together (a shape of one loop fills the same whatever it is). */
  FillRule fill_rule = FillRule::Loops;
  Style style;
  /* Not drawn (a reference hiding one of its original's shapes). */
  bool hidden = false;
  /* Its fill, or its line, not drawn here because another layer's Boolean modifier draws it as part
   * of its outline (a Boolean takes what its own shape draws: fill, line or both); still picked as
   * this layer's. Set on evaluated drawings only, never saved. */
  bool merged_fill = false;
  bool merged_line = false;
  /* Made by a Boolean modifier (evaluated drawings only, never saved): a brushed line lays its stamps
   * along it as it is, having no setup shape of its own to lay them out on. */
  bool merge_made = false;
  /* The material it takes its whole look from (data/material.hh), or 0 for its own look; and the
   * last one it was linked to, for naming a variant made from it ("Skin.001"). */
  uint64_t material = 0;
  std::string material_last;
  /* Detail levels: the level it is edited at (its curves show their points up to it; see
   * data/detail.hh). */
  int detail_level = 0;
  /* Its own number in the drawing, kept for good (as MeshPoint::id). */
  uint64_t id = 0;
  /* No selection flag of its own: a shape counts as selected when all its points are (see
   * drawing_selected_shapes), so the viewport and the Properties shape list always agree. */
  bool operator==(const Shape &) const = default;
};

/* A named set of points with a weight each, 0-1 (Blender's vertex groups): selection sets, and what
 * modifiers can be limited to. Points are kept by id, so it holds through edits. */
struct PointGroup {
  std::string name;
  std::map<uint64_t, float> weights;
  /* Locked: Assign and Remove leave it as it is (its lock in the list, as Blender's). */
  bool locked = false;
  bool operator==(const PointGroup &) const = default;
};

struct VectorDrawing {
  std::vector<MeshPoint> points;
  std::vector<Curve> curves;
  std::vector<Shape> shapes;
  /* Its point groups, and the active one (its index; what Assign and Select work on). */
  std::vector<PointGroup> groups;
  int active_group = 0;
  /* Its points laid out as grids (the Shape tool's Grid; see data/grid.hh). */
  std::vector<DrawingGrid> grids;
  /* The next point or shape id to give out. */
  uint64_t next_id = 1;

  bool empty() const { return points.empty(); }
  bool operator==(const VectorDrawing &) const = default;
};

/* ---------------------------------------------------------------------------------
 * Curve geometry
 */

/* Curve point i's handle (`pre`: the incoming one), in drawing space. */
float2 curve_handle(const VectorDrawing &drawing, const Curve &curve, int i, bool pre);
/* Gives every point that has none yet its handles, as worked out from its neighbours, so from
 * now on they stay where they are. Runs before edits are recorded (anim/animation.hh). */
void drawing_init_handles(VectorDrawing &drawing);
/* Moves one handle to `pos` (drawing space). An aligned point turns its other handle to stay in
 * line, keeping that one's length (unless `keep_other`). */
void curve_point_move_handle(VectorDrawing &drawing, Curve &curve, int i, bool pre, float2 pos,
                             bool keep_other = false);
/* Align: turns one handle (keeping its length) to face away from the other and keeps them in
 * line from now on. `turn_out`: the out handle turns; else the in handle. */
void curve_point_align(VectorDrawing &drawing, Curve &curve, int i, bool turn_out);
/* The four Bezier control points of a segment. */
void curve_segment_bezier(const VectorDrawing &drawing, const Curve &curve, int segment, float2 r_ctrl[4]);
float2 bezier_eval(const float2 ctrl[4], float t);
/* Schneider's fit of one cubic from `p0` to `p3`, its handles along `t1` (out of p0) and `t2` (out of p3,
 * back along the curve), to the samples `d` in order from p0 to p3: the two handle lengths, by least
 * squares, the samples' places along the curve refined by Newton steps (Graphics Gems 1990). A handle
 * that comes out turned back or vanishing gives thirds of the chord; `steady` instead holds each between
 * a thousandth of the chord and the chord (longer, the segment loops) and refines on as many steps, so the
 * lengths change smoothly as the samples move (for what is fitted again every frame, as a deform). */
void bezier_fit_handle_lengths(float2 p0, float2 p3, float2 t1, float2 t2, const std::vector<float2> &d,
                               float *r_a1, float *r_a2, bool steady = false);
/* The handles (offsets: `r_h1` from p0, `r_h2` from p3) of the cubic from p0 to p3 nearest the samples `pts` taken
 * at the segment's own parameters `params` (least squares). A handle given (`fix1`, `fix2`) stays as it is; `was`
 * is the longest the segment's handles were (a fit reaching far past it falls back to a third of the chord). */
void bezier_fit_handles(const std::vector<float2> &pts, const std::vector<float> &params, float2 p0, float2 p3,
                        const float2 *fix1, const float2 *fix2, float was, float2 *r_h1, float2 *r_h2);
/* How many pieces a Bezier segment needs: at least `min_steps`, and with `tolerance` > 0 enough
 * that the pieces stray no further than that from the curve, however long its handles are. */
int bezier_steps(const float2 ctrl[4], int min_steps, float tolerance);
/* Appends points along a segment (excluding its last point): at least `steps` pieces, more
 * where its handles bend it more than `tolerance` (0: exactly `steps`). */
void curve_segment_sample(const VectorDrawing &drawing, const Curve &curve, int segment, int steps,
                          std::vector<float2> &r_points, float tolerance = 0.0f);
/* The point indices at either end of a segment. */
void curve_segment_points(const Curve &curve, int segment, int *r_from, int *r_to);
/* Slide (G G): moves `points` along their curve, `factor` of the way to the next point (negative:
 * toward the one before), refitting the segments beside them to the line as it was so the curve
 * keeps its shape. A point on two curves slides along the first. Returns how many moved. */
int drawing_slide_points(VectorDrawing &drawing, const std::vector<int> &points, float factor);
/* Smooth Points (right-click on points; Blender's Smooth Vertices, for curves): the selected points go
 * half of `factor` of the way to their neighbours' middle along their curves, `repeat` times, their handles
 * as far toward smooth ones (in line, a third of each side long). Unselected points hold the ends;
 * an open curve's ends stay unless `ends`. `keep_shape` adds Taubin's inflating step so shapes don't
 * shrink; `position`, `handles` and `radius` say what is smoothed. Works in the drawing's own space
 * (a posed copy, for what shows). Returns how many points it smoothed. */
struct SmoothPointsSettings {
  float factor = 0.5f;
  int repeat = 1;
  bool position = true;
  bool handles = true;
  bool radius = false;
  bool keep_shape = false;
  bool ends = false;
  bool operator==(const SmoothPointsSettings &) const = default;
};
int drawing_smooth_points(VectorDrawing &drawing, const SmoothPointsSettings &settings);

/* A shape's edges in the order its outline runs, each with the way it is run (`reversed`: from
 * its segment's end to its start). Returns false if they don't all join into one run;
 * `r_closed` says whether the run returns to its start. */
struct OrientedEdge {
  ShapeEdge edge;
  bool reversed = false;
  bool operator==(const OrientedEdge &) const = default;
};
bool shape_chain(const VectorDrawing &drawing, const Shape &shape, std::vector<OrientedEdge> &r_chain,
                 bool *r_closed);

/* Gives every point and shape without an id (or with one already taken, as a copy has) a new
 * one. Returns true if any changed. */
bool drawing_ensure_ids(VectorDrawing &drawing);
/* Point groups: a name not taken yet ("Group", "Group.001"); and dropping members whose points are
 * gone. */
std::string drawing_group_unique_name(const VectorDrawing &drawing, const std::string &base);
void drawing_groups_prune(VectorDrawing &drawing);

/* The index of the point or shape with `id`, or -1. */
int drawing_point_index(const VectorDrawing &drawing, uint64_t id);
int drawing_shape_index(const VectorDrawing &drawing, uint64_t id);

/* The drawing as it is on the setup frame (0): every animated point, radius, curvature and
 * handle at its frame-0 value (what was drawn, before any animation). */
VectorDrawing drawing_setup_pose(const VectorDrawing &drawing);

/* The outline of a shape as a polyline in drawing space, edges joined end to end.
 * `r_radii` (optional) gets the point radius (0-1) at each sample, blended between points.
 * `r_visible` (optional) says for each sample whether the outline runs on from it to the next
 * sample: false along hidden edges. `r_joins` (optional) gives each sample's join: Round at the curve's points
 * (round corners and ends), Smooth between them. Returns false if the edges don't
 * form a connected run. `r_closed` reports whether the run returns to its start. `tolerance`
 * (drawing units, 0 = off) adds samples where long handles bend a segment hard. */
bool shape_outline(const VectorDrawing &drawing, const Shape &shape, int steps_per_segment,
                   std::vector<float2> &r_points, std::vector<float> *r_radii, bool *r_closed,
                   std::vector<bool> *r_visible = nullptr, std::vector<StrokeJoin> *r_joins = nullptr,
                   float tolerance = 0.0f);

/* A shape split into its connected runs of edges, each as a shape of its own (the same look): just
 * the shape when its edges join up. A Moho stroke shape can be loose edges across many curves (a
 * row of stitches), each run drawn as a line of its own. */
std::vector<Shape> shape_runs(const VectorDrawing &drawing, const Shape &shape);

/* ---------------------------------------------------------------------------------
 * Editing helpers
 */

int drawing_add_point(VectorDrawing &drawing, float2 pos);
/* Removes points and everything that depended on them (curve points, emptied curves, shapes
 * that lost an edge). */
void drawing_remove_points(VectorDrawing &drawing, const std::vector<bool> &remove);
/* Merge (M): each group of points becomes one, at their middle (the first of them is kept, the others go). Curves
 * through them run through it; a segment between two of them goes (shapes along it carry on across). Shapes that
 * met only at some of them become one shape, a line with a line and a fill with a fill, in the first one's look.
 * Returns how many points went. */
int drawing_merge_points(VectorDrawing &drawing, const std::vector<std::vector<int>> &groups);
/* The selected points in groups of those within `distance` of one another (by way of each other): what Merge by
 * Distance merges. Groups of one are left out. */
std::vector<std::vector<int>> drawing_points_within(const VectorDrawing &drawing, float distance);
/* Removes curves and the shapes that use them. Points stay. */
void drawing_remove_curves(VectorDrawing &drawing, const std::vector<bool> &remove);
/* Curves that end (not just pass through) at a point: their index and which end (true = last). */
struct CurveEnd {
  int curve = -1;
  bool at_end = false;
  bool operator==(const CurveEnd &) const = default;
};
CurveEnd drawing_curve_end_at(const VectorDrawing &drawing, int point);
/* Extrude from a point that isn't an open end: a new open curve from it (welded: it is that curve's first
 * point) to a new point at the same place, returned for the move to take. The new segment shows as part of an
 * outline-only shape through the point (a grid's), else as a new outline shape in the look of a shape through
 * the point (`style` when none goes through it). */
int drawing_extrude_branch(VectorDrawing &drawing, int point, const Style &style);
/* Extrude (E) on the selected points: an open end's curve grows a new point past it (shapes covering the whole
 * curve growing with it); any other point branches (drawing_extrude_branch). The new points, at their points'
 * places, become the selection; returned. */
std::vector<int> drawing_extrude_selected(VectorDrawing &drawing, const Style &style);
/* Every curve and segment that touches a point. */
std::vector<ShapeEdge> drawing_segments_at_point(const VectorDrawing &drawing, int point);
/* Closest point within `radius`, or -1. */
int drawing_pick_point(const VectorDrawing &drawing, float2 pos, float radius);
/* Closest segment within `radius`; `r_t` gets the parameter along it. */
bool drawing_pick_segment(const VectorDrawing &drawing, float2 pos, float radius, ShapeEdge *r_edge, float *r_t);
/* Top-most shape whose fill contains pos, or whose outline is within radius. -1 if none. */
int drawing_pick_shape(const VectorDrawing &drawing, float2 pos, float radius);
/* Inserts a point into a segment at parameter t, keeping the curve's shape as closely as the
 * curvature model allows. Shapes using that segment are updated. Returns the new point. */
int drawing_split_segment(VectorDrawing &drawing, ShapeEdge edge, float t);
/* Removes a point from every curve through it, joining its neighbours, so shapes keep their
 * other edges. Then removes the point itself. */
void drawing_dissolve_point(VectorDrawing &drawing, int point);
/* Dissolves `points` (indices) keeping the curves' shape: each span of removed points becomes one
 * segment whose two handles keep their directions and take the lengths that fit it closest to the
 * curve as it was (Schneider's least-squares fit, Graphics Gems 1990, as Blender's Dissolve Vertices
 * fits curves). A run of neighbouring points is fitted at once. */
void drawing_dissolve_points(VectorDrawing &drawing, const std::vector<int> &points);
/* Reverses a curve's direction, keeping shapes that use it intact. */
void drawing_reverse_curve(VectorDrawing &drawing, int curve);
/* Make Segment (F, as Blender's for curves): a segment from point `a` to point `b`, each the end of
 * an open curve. Ends of two curves join them into one; the two ends of one curve close it. A shape
 * whose outline stops at a joined end runs on along the new segment (two such shapes, one each
 * side, become one, the first's look kept), and one that now goes all the way round is filled.
 * Returns false (nothing changed) if they aren't two such ends. */
bool drawing_make_segment(VectorDrawing &drawing, int a, int b);
/* Removes points no curve uses. */
void drawing_remove_unused_points(VectorDrawing &drawing);
/* Deletes curve segments (Edges mode delete): curves split where a segment goes, the points they leave on
 * no curve go too (as Blender's Delete Edges), and shapes keep the outline of what is left, losing their
 * fill when it no longer closes. Returns how many went. */
int drawing_delete_edges(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges);
/* Bounding box of selected points; false if none. */
bool drawing_selection_bounds(const VectorDrawing &drawing, float2 *r_min, float2 *r_max);
/* Copies the selected points with the curve pieces between them (runs of two or more selected
 * points; a fully selected closed curve stays closed) and the shapes those pieces fully cover (a shape
 * they cover part of gives its line to the copy, as a line-only shape), then selects the copy instead. Points welded in the original stay welded in the copy.
 * Returns false if nothing could be copied. */
bool drawing_duplicate_selected(VectorDrawing &drawing);

/* What the Shape tool draws, dragged out corner to corner. */
enum class PrimitiveKind { Box, Circle, Grid };
inline constexpr PrimitiveKind PRIMITIVE_KINDS[] = {PrimitiveKind::Box, PrimitiveKind::Circle, PrimitiveKind::Grid};
/* "Box", and the Python name "BOX". */
const char *primitive_name(PrimitiveKind kind);
const char *primitive_identifier(PrimitiveKind kind);
bool primitive_from_identifier(const std::string &identifier, PrimitiveKind *r_kind);

/* A drawing holding a box or circle and its shape (filled when `fill`), or an empty drawing
 * if the drag is too small. `points`: the drag's two corners, opposite corners of the bounds.
 * Circles are four points with Moho's circle curvature. */
VectorDrawing primitive_drawing(PrimitiveKind kind, const std::vector<float2> &points, const Style &style,
                                bool fill, int columns = 3, int rows = 3);
/* Adds `src` to `dst` (its points, curves, shapes and grids) and selects only the added points. */
void drawing_append_selected(VectorDrawing &dst, const VectorDrawing &src);

/* Edges (curve segments) whose two points are both selected. */
std::vector<ShapeEdge> drawing_selected_edges(const VectorDrawing &drawing);
/* Moho's Hide Edge: hides or shows the outline along these segments. */
void drawing_set_edges_hidden(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges, bool hidden);

/* Sets the corner type of the selected points. */

/* Mean radius of the selected points, or -1 if none are selected. */
float drawing_selected_mean_radius(const VectorDrawing &drawing);
/* Sets the mean radius of the selected points as Blender's Mean Radius field does: each point
 * moves by the same amount (clamped to 0-1), so their differences are kept. */
void drawing_set_selected_mean_radius(VectorDrawing &drawing, float mean);

/* Shapes with any selected point: the ones arranging and the Properties shape list act on. */
std::vector<bool> drawing_selected_shapes(const VectorDrawing &drawing);

/* Join Fills (F with whole loops selected, as Moho's Create Shape and Grease Pencil's Join Fills): the shapes
 * all of whose points are selected and that go round become one shape, its loops filling together (a loop
 * inside another a hole, Even-Odd unless it had a rule already), with the look and place of the first of
 * them. Returns how many shapes went into it (0 with fewer than two). */
int drawing_join_fills(VectorDrawing &drawing);
/* Separate Fills: each shape all of whose points are selected and that has several runs of edges becomes
 * a shape per run, each with its look, the first keeping its place and number. Returns how many shapes
 * were made. */
int drawing_separate_fills(VectorDrawing &drawing);

/* The selected points, and the points a shape runs through (each once). */
std::vector<int> drawing_selected_points(const VectorDrawing &drawing);
std::vector<int> shape_points(const VectorDrawing &drawing, const Shape &shape);
/* The average position of some points (false when there are none), and moving them all by the
 * same amount; their stored handles come along. */
bool drawing_points_mean(const VectorDrawing &drawing, const std::vector<int> &points, float2 *r_mean);
void drawing_points_move(VectorDrawing &drawing, const std::vector<int> &points, float2 delta);

/* Proportional editing (Blender's, O): moving selected points drags the points around them too,
 * the less the further they are, out to the proportional size. Falloffs as Blender's menu. */
enum class ProportionalFalloff { Smooth, Sphere, Root, InverseSquare, Sharp, Linear, Constant, Random };
constexpr int PROPORTIONAL_FALLOFF_COUNT = 8;
/* Its name in the menu ("Inverse Square"), and its identifier for scripts ("INVERSE_SQUARE"). */
const char *proportional_falloff_name(ProportionalFalloff falloff);
const char *proportional_falloff_identifier(ProportionalFalloff falloff);
bool proportional_falloff_from_identifier(const std::string &id, ProportionalFalloff *r_falloff);
/* How far each point is from the selected points (0 for those; infinity where it can't reach):
 * straight across, or along the curves (`connected`, Blender's Connected Only: only points on a
 * curve that has selected points move). `positions`: where the points are, one per point. */
std::vector<float> drawing_proportional_distances(const VectorDrawing &drawing, const std::vector<float2> &positions,
                                                  bool connected);
/* How much of the move a point `distance` from the selection gets, from 1 (selected) to 0 at
 * `size` and beyond. `point`: its index (Random gives each point its own amount). */
float proportional_weight(ProportionalFalloff falloff, float distance, float size, int point);

/* Where a point is in the setup pose (frame 0): its frame-0 key, or, not keyed, its value as last
 * recorded. Mirror editing pairs points by it, so the pairs hold on every frame. */
float2 point_setup_pos(const MeshPoint &point);
/* A handle's offset from its point in the setup pose (frame 0). */
float2 handle_setup(const CurvePoint &cp, bool in);
/* Handle Angle and Length: how a handle (an offset from its point) has turned from its setup one
 * (radians, the turn nearest `near_turn`, so whole turns carry on) and how much longer it is; and
 * the handle they make. */
float2 handle_change(float2 setup, float2 handle, float near_turn);
float2 handle_from_change(float2 setup, float2 change);
/* The points' Transforms and the handles' Angle and Length from the points and handles as they
 * are (only what moved since: a round trip isn't exact to the last bit, and a value differing from
 * its last recorded one is an edit), and the points and handles back from them. A point or handle
 * new since the keys last saw it (nothing recorded, no keys) is in its setup pose. */
void drawing_changes_from_values(VectorDrawing &drawing);
void drawing_values_from_changes(VectorDrawing &drawing);
/* Frame 0's edits become the setup pose. */
void drawing_take_setup(VectorDrawing &drawing);
/* Away from frame 0, as Blender's Clear Location and Clear Rotation: `positions` (Alt G) puts the
 * selected points back where they are in the setup pose (their Transform 0); `handles` (Alt R)
 * gives their handles, or a handle picked on its own, back their setup shape (Angle and Length 0).
 * Returns how many points and handles changed. */
int drawing_clear_selected_pose(VectorDrawing &drawing, bool positions, bool handles);
/* Points new since the keys were last recorded, on a curve whose points are animated: each takes
 * the motion of its nearest recorded neighbours along the curve (in between, blended by distance;
 * past an end, the end's), keyed where they are, so adding a point doesn't break the animation.
 * `frame` is the frame it was added on: it stays where it was put there. */
void drawing_new_points_follow(VectorDrawing &drawing, int frame);
/* Mirror editing (Blender's mesh symmetry), across the layer's vertical (`x`) and/or horizontal
 * (`y`) axis. The point at `point`'s mirror image in the setup pose, or -1 (a point on the axis has
 * none). */
int drawing_find_mirror_point(const VectorDrawing &drawing, int point, bool x, bool y);
/* Gives `dst`'s curve points the alignment, curvature and (reflected) handles of the matching
 * curve points of `src`, its mirror image. */
void drawing_mirror_handles(VectorDrawing &drawing, int src, int dst, bool x, bool y);

/* A moving point (`src`) and its mirror image (`dst`, not moving itself), reflected across x, y
 * or both. */
struct MirrorPair {
  int src = -1, dst = -1;
  bool x = false, y = false;
  bool operator==(const MirrorPair &) const = default;
};
/* The mirror images of the `moving` points, paired in the setup pose; selected points move by
 * themselves, so they are no one's image. */
std::vector<MirrorPair> drawing_mirror_pairs(const VectorDrawing &drawing, const std::vector<int> &moving, bool mirror_x,
                                             bool mirror_y);
/* After the moving points moved, snaps their images to the reflected spot, handles too, on any
 * frame. */
void drawing_apply_mirror(VectorDrawing &drawing, const std::vector<MirrorPair> &pairs);
/* After changing the selected points' (or handles') handles: the same on their mirror images,
 * for each mirror axis that is on. */
void drawing_mirror_selected_handles(VectorDrawing &drawing, bool mirror_x, bool mirror_y);

/* Symmetrize (the right-click menu): makes the selected points mirror images across the layer's
 * vertical axis, copying one side onto the other (`right_to_left`: the right side's onto the left).
 * Points within `threshold` of the axis are its own: they snap onto it and their two handles mirror
 * each other (made flat when nearly flat). The others pair up by where each one's reflection lands
 * nearest, in the setup pose: each side needs as many as the other, or nothing changes. A copy takes
 * its partner's place, handles (reflected), width and corner, and with `weight_flip` its weights
 * (bone ids through the map: _L to _R). On the setup frame it reshapes the drawing; posed
 * (`setup_frame` false) it mirrors the pose: each copy's change from its setup becomes its
 * partner's, reflected. */
struct SymmetrizeResult {
  bool done = false;
  int right = 0, left = 0, centre = 0;
  /* Uneven: the points left without a partner. */
  std::vector<int> unmatched;
  /* Done: each point copied (source) onto its partner (destination), and the points on the axis. */
  std::vector<std::pair<int, int>> copied;
  std::vector<int> centre_points;
  bool operator==(const SymmetrizeResult &) const = default;
};
/* Symmetrize's looks: each shape of `from` whose points all have an image in `image` (point -> point
 * of `to`) gives its look (Fill and Outline, colours, width, brush, material) to the shape of `to` made
 * of those images (not itself). Returns how many shapes changed. */
int drawing_copy_shape_looks(const VectorDrawing &from, VectorDrawing &to, const std::map<int, int> &image);
SymmetrizeResult drawing_symmetrize(VectorDrawing &drawing, bool right_to_left, float threshold, bool setup_frame,
                                    const std::map<uint64_t, uint64_t> *weight_flip);

/* Layer Symmetrize's copy: the drawing flipped left to right across its own vertical axis, as its
 * setup pose, with its point, handle and curvature keys gone (so it rests there on every frame).
 * Detail points flip across their segments. */
void drawing_mirror_setup_x(VectorDrawing &drawing);

/* Deselects every handle. */
void drawing_deselect_handles(VectorDrawing &drawing);
/* Whether curve point `cp` counts as selected for handle editing: its point or a handle is. */
bool curve_point_selected(const VectorDrawing &drawing, const CurvePoint &cp);
/* Align (or, `align` false, Unalign) the selected points' handles. With only one handle of a
 * point picked, that handle is the one that turns into line. Returns how many points changed. */
int drawing_align_selected_handles(VectorDrawing &drawing, bool align);
/* The selected points' alignment: -1 none selected, 0 free, 1 aligned, -2 some of each. */
int drawing_selected_alignment(const VectorDrawing &drawing);
/* Peak (`peak`) or Smooth the selected points' handles, once: Peak points them a third of the
 * way to the neighbours (straight lines, a corner) and unaligns them; Smooth gives them Moho's
 * smooth handles and aligns them. Returns how many points changed. */
int drawing_set_selected_handles_shape(VectorDrawing &drawing, bool peak);
/* Older files could key zero-length handles on frame 0 for points whose handles were first
 * stored on a later frame; gives those keys the handles the points had in the setup pose. */
void drawing_repair_stale_handle_keys(VectorDrawing &drawing);

/* Where shapes go in the stacking order. Shapes later in the list draw on top. */
enum class ShapeArrange { Forward, Backward, ToFront, ToBack };
/* Moves the selected shapes, keeping their order among themselves. Returns false if nothing
 * moved (none selected, or already at the top or bottom). */
bool drawing_arrange_shapes(VectorDrawing &drawing, ShapeArrange arrange);

}  // namespace oanim
