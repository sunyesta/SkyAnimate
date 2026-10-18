#pragma once

/* Vector drawings, following Moho's model (M_Mesh, M_Curve, M_Shape):
 *
 * - A drawing owns a pool of points. Curves run through points by index, so two curves can
 *   share a point; that is how lines are welded together.
 * - Curves are smooth splines driven by per-point curvature, weight and offset, exactly as in
 *   Moho, rather than stored Bezier handles. Each segment still evaluates as a cubic Bezier.
 * - A shape is a closed or open run of curve segments with a fill and an outline. Curves
 *   without a shape only show in the editor, as in Moho.
 *
 * Positions are in layer space. Document space has the camera frame 2 units tall, y up. */

#include <cstdint>
#include <string>
#include <vector>

#include "core/math.hh"

namespace sky {

/* Moho's curvature presets (MOHO.SMOOTH, MOHO.PEAKED). */
inline constexpr float CURVATURE_SMOOTH = 0.3f;
inline constexpr float CURVATURE_PEAKED = 0.0f;

struct MeshPoint {
  float2 pos;
  /* Stroke radius here as a fraction (0-1) of the line width of the shape drawing the stroke,
   * like a Grease Pencil point's radius. The draw tool sets it from pen pressure. */
  float radius = 1.0f;
  bool selected = false;
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
};

struct Curve {
  std::vector<CurvePoint> points;
  bool closed = false;

  int segment_count() const;
};

struct Style {
  Color fill = {0.95f, 0.85f, 0.7f, 1.0f};
  Color line = {0.0f, 0.0f, 0.0f, 1.0f};
  /* Line width in document units (the frame is 2 units tall). */
  float line_width = 0.008f;
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

/* Moho's handle position for curve point i: `pre` gives the incoming handle. */
float2 curve_handle(const VectorDrawing &drawing, const Curve &curve, int i, bool pre);
/* The four Bezier control points of a segment. */
void curve_segment_bezier(const VectorDrawing &drawing, const Curve &curve, int segment, float2 r_ctrl[4]);
float2 bezier_eval(const float2 ctrl[4], float t);
/* Appends points along a segment (excluding its last point), `steps` subdivisions. */
void curve_segment_sample(const VectorDrawing &drawing, const Curve &curve, int segment, int steps,
                          std::vector<float2> &r_points);
/* The point indices at either end of a segment. */
void curve_segment_points(const Curve &curve, int segment, int *r_from, int *r_to);

/* The outline of a shape as a polyline in drawing space, edges joined end to end.
 * `r_radii` (optional) gets the point radius (0-1) at each sample, blended between points.
 * `r_visible` (optional) says for each sample whether the outline runs on from it to the next
 * sample: false along hidden edges. Returns false if the edges don't
 * form a connected run. `r_closed` reports whether the run returns to its start. */
bool shape_outline(const VectorDrawing &drawing, const Shape &shape, int steps_per_segment,
                   std::vector<float2> &r_points, std::vector<float> *r_radii, bool *r_closed,
                   std::vector<bool> *r_visible = nullptr);

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

/* Mean radius of the selected points, or -1 if none are selected. */
float drawing_selected_mean_radius(const VectorDrawing &drawing);
/* Sets the mean radius of the selected points as Blender's Mean Radius field does: each point
 * moves by the same amount (clamped to 0-1), so their differences are kept. */
void drawing_set_selected_mean_radius(VectorDrawing &drawing, float mean);

/* Shapes with any selected point: the ones arranging and the Properties shape list act on. */
std::vector<bool> drawing_selected_shapes(const VectorDrawing &drawing);

/* Where shapes go in the stacking order. Shapes later in the list draw on top. */
enum class ShapeArrange { Forward, Backward, ToFront, ToBack };
/* Moves the selected shapes, keeping their order among themselves. Returns false if nothing
 * moved (none selected, or already at the top or bottom). */
bool drawing_arrange_shapes(VectorDrawing &drawing, ShapeArrange arrange);

}  // namespace sky
