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
#include <string>
#include <vector>

#include "anim/channel.hh"
#include "core/math.hh"

namespace sky {

/* Moho's curvature presets (MOHO.SMOOTH, MOHO.PEAKED). */
inline constexpr float CURVATURE_SMOOTH = 0.3f;
inline constexpr float CURVATURE_PEAKED = 0.0f;

/* How strokes turn at a point and end there: Round (a round join or cap) or Sharp (a mitred
 * corner, bevelled when very tight, and a flat end). */
enum class CornerType : uint8_t { Round, Sharp };

/* How strongly one bone moves a point (Blender's vertex group weight), 0-1. */
struct BoneWeight {
  uint64_t bone = 0;
  float weight = 0.0f;
  bool operator==(const BoneWeight &) const = default;
};

struct MeshPoint {
  float2 pos;
  /* Stroke radius here as a fraction (0-1) of the line width of the shape drawing the stroke,
   * like a Grease Pencil point's radius. The draw tool sets it from pen pressure. */
  float radius = 1.0f;
  bool selected = false;
  CornerType corner = CornerType::Round;
  /* Moho's point binding: the id of the one bone this point follows, or 0 to follow the bones
   * by distance (flexi-binding) when the layer allows it. */
  uint64_t bone = 0;
  /* Painted weights (the Weight Paint tool). When there are any they decide how the bones move
   * the point, blended by weight, instead of `bone` or flexi-binding. */
  std::vector<BoneWeight> weights;
  /* Keys for the position and radius (see anim/channel.hh). */
  Channel<float2> pos_keys;
  Channel<float> radius_keys;
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
  /* Handles picked on their own (clicking one). A selected point's handles move with it. */
  bool select_in = false;
  bool select_out = false;
  /* Keys for the curvature and the handles. */
  Channel<float> curvature_keys;
  Channel<float2> handle_in_keys;
  Channel<float2> handle_out_keys;
};

struct Curve {
  std::vector<CurvePoint> points;
  bool closed = false;

  int segment_count() const;
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

  bool operator==(const BrushSettings &) const = default;
};

struct Style {
  Color fill = {0.95f, 0.85f, 0.7f, 1.0f};
  Color line = {0.0f, 0.0f, 0.0f, 1.0f};
  /* Line width in document units (the frame is 2 units tall). */
  float line_width = 0.008f;
  BrushSettings brush;
};

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
  Style style;
  /* No selection flag of its own: a shape counts as selected when all its points are (see
   * drawing_selected_shapes), so the viewport and the Properties shape list always agree. */
};

struct VectorDrawing {
  std::vector<MeshPoint> points;
  std::vector<Curve> curves;
  std::vector<Shape> shapes;

  bool empty() const { return points.empty(); }
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
/* How many pieces a Bezier segment needs: at least `min_steps`, and with `tolerance` > 0 enough
 * that the pieces stray no further than that from the curve, however long its handles are. */
int bezier_steps(const float2 ctrl[4], int min_steps, float tolerance);
/* Appends points along a segment (excluding its last point): at least `steps` pieces, more
 * where its handles bend it more than `tolerance` (0: exactly `steps`). */
void curve_segment_sample(const VectorDrawing &drawing, const Curve &curve, int segment, int steps,
                          std::vector<float2> &r_points, float tolerance = 0.0f);
/* The point indices at either end of a segment. */
void curve_segment_points(const Curve &curve, int segment, int *r_from, int *r_to);

/* The outline of a shape as a polyline in drawing space, edges joined end to end.
 * `r_radii` (optional) gets the point radius (0-1) at each sample, blended between points.
 * `r_visible` (optional) says for each sample whether the outline runs on from it to the next
 * sample: false along hidden edges. `r_joins` (optional) gives each sample's join: the point's
 * corner type at the curve's points, Smooth between them. Returns false if the edges don't
 * form a connected run. `r_closed` reports whether the run returns to its start. `tolerance`
 * (drawing units, 0 = off) adds samples where long handles bend a segment hard. */
bool shape_outline(const VectorDrawing &drawing, const Shape &shape, int steps_per_segment,
                   std::vector<float2> &r_points, std::vector<float> *r_radii, bool *r_closed,
                   std::vector<bool> *r_visible = nullptr, std::vector<StrokeJoin> *r_joins = nullptr,
                   float tolerance = 0.0f);

/* ---------------------------------------------------------------------------------
 * Editing helpers
 */

int drawing_add_point(VectorDrawing &drawing, float2 pos);
/* Removes points and everything that depended on them (curve points, emptied curves, shapes
 * that lost an edge). */
void drawing_remove_points(VectorDrawing &drawing, const std::vector<bool> &remove);
/* Removes curves and the shapes that use them. Points stay. */
void drawing_remove_curves(VectorDrawing &drawing, const std::vector<bool> &remove);
/* Curves that end (not just pass through) at a point: their index and which end (true = last). */
struct CurveEnd {
  int curve = -1;
  bool at_end = false;
};
CurveEnd drawing_curve_end_at(const VectorDrawing &drawing, int point);
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
/* Reverses a curve's direction, keeping shapes that use it intact. */
void drawing_reverse_curve(VectorDrawing &drawing, int curve);
/* Removes points no curve uses. */
void drawing_remove_unused_points(VectorDrawing &drawing);
/* Deletes curve segments (Edges mode delete): curves split where a segment goes, every point
 * stays, and shapes keep the outline of what is left, losing their fill when it no longer
 * closes. Returns how many went. */
int drawing_delete_edges(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges);
/* Finds a closed loop of segments whose points are all selected and makes a shape from it.
 * Returns the new shape index or -1. */
int drawing_shape_from_selection(VectorDrawing &drawing, bool fill, const Style &style);
/* Bounding box of selected points; false if none. */
bool drawing_selection_bounds(const VectorDrawing &drawing, float2 *r_min, float2 *r_max);
/* Copies the selected points with the curve pieces between them (runs of two or more selected
 * points; a fully selected closed curve stays closed) and the shapes those pieces fully cover,
 * then selects the copy instead. Points welded in the original stay welded in the copy.
 * Returns false if nothing could be copied. */
bool drawing_duplicate_selected(VectorDrawing &drawing);

/* What the Shape tool draws, dragged out corner to corner. */
enum class PrimitiveKind { Box, Circle };
inline constexpr PrimitiveKind PRIMITIVE_KINDS[] = {PrimitiveKind::Box, PrimitiveKind::Circle};
/* "Box", and the Python name "BOX". */
const char *primitive_name(PrimitiveKind kind);
const char *primitive_identifier(PrimitiveKind kind);
bool primitive_from_identifier(const std::string &identifier, PrimitiveKind *r_kind);

/* A drawing holding a box or circle and its shape (filled when `fill`), or an empty drawing
 * if the drag is too small. `points`: the drag's two corners, opposite corners of the bounds.
 * Circles are four points with Moho's circle curvature. */
VectorDrawing primitive_drawing(PrimitiveKind kind, const std::vector<float2> &points, const Style &style,
                                bool fill);
/* Adds `src` to `dst` (its points, curves and shapes) and selects only the added points. */
void drawing_append_selected(VectorDrawing &dst, const VectorDrawing &src);

/* Edges (curve segments) whose two points are both selected. */
std::vector<ShapeEdge> drawing_selected_edges(const VectorDrawing &drawing);
/* Moho's Hide Edge: hides or shows the outline along these segments. */
void drawing_set_edges_hidden(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges, bool hidden);

/* Sets the corner type of the selected points. */
void drawing_set_selected_corner(VectorDrawing &drawing, CornerType corner);

/* Mean radius of the selected points, or -1 if none are selected. */
float drawing_selected_mean_radius(const VectorDrawing &drawing);
/* Sets the mean radius of the selected points as Blender's Mean Radius field does: each point
 * moves by the same amount (clamped to 0-1), so their differences are kept. */
void drawing_set_selected_mean_radius(VectorDrawing &drawing, float mean);

/* Shapes with any selected point: the ones arranging and the Properties shape list act on. */
std::vector<bool> drawing_selected_shapes(const VectorDrawing &drawing);

/* The selected points, and the points a shape runs through (each once). */
std::vector<int> drawing_selected_points(const VectorDrawing &drawing);
std::vector<int> shape_points(const VectorDrawing &drawing, const Shape &shape);
/* The average position of some points (false when there are none), and moving them all by the
 * same amount; their stored handles come along. */
bool drawing_points_mean(const VectorDrawing &drawing, const std::vector<int> &points, float2 *r_mean);
void drawing_points_move(VectorDrawing &drawing, const std::vector<int> &points, float2 delta);

/* Mirror editing (Blender's mesh symmetry), across the layer's vertical (`x`) and/or horizontal
 * (`y`) axis. The point at `point`'s mirror image, or -1 (a point on the axis has none). */
int drawing_find_mirror_point(const VectorDrawing &drawing, int point, bool x, bool y);
/* Gives `dst`'s curve points the alignment, curvature and (reflected) handles of the matching
 * curve points of `src`, its mirror image. */
void drawing_mirror_handles(VectorDrawing &drawing, int src, int dst, bool x, bool y);
/* After changing the selected points' (or handles') handles: the same on their mirror images,
 * for each mirror axis that is on. */
void drawing_mirror_selected_handles(VectorDrawing &drawing, bool mirror_x, bool mirror_y);

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

}  // namespace sky
