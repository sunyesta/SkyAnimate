#pragma once

/* Fills with holes: what a shape's loops cover together under its fill rule (data/vector.hh). */

#include <vector>

#include "data/vector.hh"

namespace oanim {

/* One piece of a fill: its outer ring and the holes in it. Outer rings go counter-clockwise and
 * holes clockwise, so a ring's right-hand side is always outside the fill. */
struct FillPiece {
  std::vector<std::vector<float2>> rings;
};

/* Whether a shape fills its loops together: it has a fill rule other than Loops and more than one
 * connected run of edges. */
bool shape_fills_together(const VectorDrawing &drawing, const Shape &shape);

/* The closed runs of a shape's edges as outlines (drawing space), sampled as shape_outline samples
 * them. Open runs are left out: they have no inside. */
std::vector<std::vector<float2>> shape_fill_loops(const VectorDrawing &drawing, const Shape &shape, int min_steps,
                                                  float tolerance = 0.0f);

/* What `loops` cover together under `rule`, as pieces with holes (Loops: their union). Kept for a
 * while by the loops' exact points, as the same fill is worked out again every redraw. */
const std::vector<FillPiece> &fill_area(const std::vector<std::vector<float2>> &loops, FillRule rule);

/* What two sets of pieces make together. */
enum class FillBoolean { Union, Subtract };
std::vector<FillPiece> fill_boolean(const std::vector<FillPiece> &a, const std::vector<FillPiece> &b, FillBoolean op);
/* Whether two sets of pieces share any area. */
bool fill_overlap(const std::vector<FillPiece> &a, const std::vector<FillPiece> &b);
/* What a disc covers going along `centers`, its radius changing with `radii`: a brush's stroke as an area (the
 * envelope of its discs, so a tight turn or a loop is what it should be, where offsetting a line folds over).
 * `tolerance`: how far its round edges may be off. */
std::vector<FillPiece> fill_sweep(const std::vector<float2> &centers, const std::vector<float> &radii, float tolerance);

/* Whether `p` is inside `loops` under `rule` (Loops: inside any of them). */
bool fill_contains(const std::vector<std::vector<float2>> &loops, FillRule rule, float2 p);

/* Whether a shape's fill covers `p` (drawing space): its one outline, or its loops under its fill
 * rule. Only closed outlines have an inside. */
bool shape_fill_contains(const VectorDrawing &drawing, const Shape &shape, float2 p, int min_steps = 12);

}  // namespace oanim
