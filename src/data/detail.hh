#pragma once

/* Detail levels: multiresolution shapes (Blender's Multiresolution modifier, ZBrush's subdivision
 * levels, Finkelstein and Salesin's multiresolution curves), so a shape's overall sweep can be
 * edited with few points while its fine detail rides along.
 *
 * - A point's level (PointDetail::level) is 0 for the drawing's own points (base points). A detail
 *   point (level k > 0) is added by the knife and sits on one curve, on the segment of the level
 *   k-1 curve between the nearest points of lower level around it (its parents): at `t` along that
 *   segment, offset across and along it by its PointDetail offsets, measured in the segment's frame
 *   at t and in lengths of the segment (so the detail stretches with it). Its handles are kept the
 *   same way, from the segment split there (de Casteljau).
 * - The drawing keeps every point where it is, and each point's handles as they are at its own
 *   level: a parent's handle facing finer detail is its coarse handle. The handles drawn and
 *   rendered come from drawing_detail_final_handles, which shortens those as the split does.
 * - drawing_detail_sync places the detail points from their offsets on the curve as it is now, so
 *   moving, bending or stretching the coarse points carries the detail along; a detail point (or
 *   handle) moved since it was last placed is an edit, and its offsets are taken from where it is.
 *   Animation keys a detail point's offsets (see drawing_changes_from_values).
 * - Each shape has its edit level (Shape::detail_level): a curve shows its points up to the highest
 *   level of the shapes using it; the others are hidden (and can't be picked) but still draw. */

#include <cstdint>
#include <map>
#include <vector>

#include "data/vector.hh"

namespace oanim {

/* The highest level any detail point of the curve has (0: none). */
int curve_detail_max_level(const VectorDrawing &drawing, const Curve &curve);
bool drawing_has_detail(const VectorDrawing &drawing);

/* Places every curve's detail points from their offsets (see above). With `capture`, a detail
 * point or handle that moved since it was last placed keeps where it is: its offsets are taken from
 * there. Also marks the points hidden at their curves' edit levels. */
void drawing_detail_sync(VectorDrawing &drawing, bool capture);
/* The handles as drawn and rendered: a handle facing finer detail shortened as its segment splits.
 * For a copy (drawing, rendering), never the drawing itself. */
void drawing_detail_final_handles(VectorDrawing &drawing);
/* How much drawing_detail_final_handles shortens the handles of curve `curve`, point by point. */
void drawing_detail_handle_factors(const VectorDrawing &drawing, int curve, std::vector<float> &r_in,
                                   std::vector<float> &r_out);

/* The curves as edited: each without the points above its edit level, its handles as they are at
 * that level (the wires drawn over a drawing, and the segments Edges and Strokes picking finds). Points
 * keep their indices; shapes don't match its curves, so a copy for drawing and picking only. */
VectorDrawing drawing_detail_shown_curves(const VectorDrawing &drawing);
/* How much drawing_detail_shown_curves shortens each of its points' handles, per curve (indexed as the
 * shown curve's points; 1 where it doesn't): a handle stored from one as shown is divided by it. */
void drawing_detail_shown_handle_factors(const VectorDrawing &drawing, std::vector<std::vector<float>> &r_in,
                                         std::vector<std::vector<float>> &r_out);
/* The same by each curve's own points (1 on those not shown): where the handles drawn over a drawing end, each where
 * its line really bends, is the stored handle times this. A handle dragged there is stored divided by it. */
struct ShownHandleFactors {
  std::vector<std::vector<float>> in, out;
  float of(int curve, int i, bool pre) const
  {
    return in.empty() ? 1.0f : (pre ? in : out)[size_t(curve)][size_t(i)];
  }
};
ShownHandleFactors drawing_detail_shown_factors(const VectorDrawing &drawing);
/* Each curve's edit level: the highest detail level of the shapes using it (a curve no shape uses
 * shows every level). */
std::vector<int> drawing_curve_detail_levels(const VectorDrawing &drawing);
/* Marks each point hidden (PointDetail::hidden) at its curve's edit level, and deselects it. */
void drawing_detail_update_shown(VectorDrawing &drawing);

/* Structural edits (inserting, deleting, reversing points) on a drawing with detail: _begin turns
 * the detail curves into their final shape, _end works their detail back out (new points, and
 * points whose parents changed, get their place along their parents' segment anew), so the drawing
 * keeps its look. Nested pairs do nothing. */
struct DetailEdit {
  bool active = false;
  /* By point id: the ids of its parents. */
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> parents;
  /* Each detail curve as it was (its point ids in order, and its handles), to put back untouched. */
  struct CurveState {
    std::vector<uint64_t> ids;
    std::vector<float2> in, out;
  };
  std::vector<CurveState> curves;
};
DetailEdit drawing_detail_edit_begin(VectorDrawing &drawing);
void drawing_detail_edit_end(VectorDrawing &drawing, DetailEdit &edit);

/* The level the knife adds points at on a curve: its edit level (0 on a curve no shape uses). */
int drawing_knife_level(const VectorDrawing &drawing, int curve);
/* The knife: inserts a point into segment `edge` of the curve as it shows (final handles) at `t`,
 * keeping its shape, as a detail point of `level` (0: a base point). Returns the new point. */
int drawing_detail_split(VectorDrawing &drawing, ShapeEdge edge, float t, int level);

/* The curves a shape runs along. */
std::vector<int> shape_curves(const VectorDrawing &drawing, const Shape &shape);
/* The highest detail level on a shape's curves, and how many detail points they have. */
int shape_detail_max_level(const VectorDrawing &drawing, const Shape &shape);
int shape_detail_point_count(const VectorDrawing &drawing, const Shape &shape);
/* How high a shape's edit level can go: one past its highest detail level (an empty level to add
 * detail at), at most DETAIL_LEVEL_MAX. */
inline constexpr int DETAIL_LEVEL_MAX = 4;
int shape_detail_level_limit(const VectorDrawing &drawing, const Shape &shape);

/* Smooth to Lower Level: the selected detail points (and handles of detail points picked on their
 * own) move `influence` of the way (1: all of it) onto the curve of the level below, so they keep only
 * what that curve gives them: no offset of their own. Returns how many points and handles changed. */
int drawing_detail_smooth_selected(VectorDrawing &drawing, float influence);
/* Move to Higher Level: the selected points go one detail level up, as the curve shows (it keeps
 * its look; their places along their new parents' segment worked out anew). Points that can't are
 * left (`r_skipped`): an open curve's ends, points on more than one curve, ones already at
 * DETAIL_LEVEL_MAX, and any that would leave a closed curve too few coarser points to sit between.
 * Their keys held places in their old frames, so they go (`r_keys_dropped`). Returns how many moved. */
int drawing_detail_raise_selected(VectorDrawing &drawing, int *r_skipped, bool *r_keys_dropped);
/* Move to Lower Level: the selected detail points go one detail level down (from level 1, base
 * points), as the curve shows (it keeps its look): the finer detail around them takes them for a
 * parent, its places worked out anew. Base points can't (`r_skipped`); the lowered points' keys go
 * (`r_keys_dropped`). Returns how many moved. */
int drawing_detail_lower_selected(VectorDrawing &drawing, int *r_skipped, bool *r_keys_dropped);
/* Whether any selected point (or handle picked on its own) is detail. */
bool drawing_detail_selected(const VectorDrawing &drawing);

/* Delete Higher: removes the detail above each picked shape's edit level. Returns how many points
 * went. */
int drawing_detail_delete_higher(VectorDrawing &drawing, const std::vector<bool> &shapes);
/* Removes detail points (by id) as Delete Higher does: the coarser points keep their own-level handles,
 * so the curve is the level below's, exactly as it was before the knife added them. */
void drawing_detail_remove_points(VectorDrawing &drawing, const std::vector<uint64_t> &ids);
/* Apply Detail: the picked shapes' detail points become base points, as they show. Returns how
 * many points changed; `r_keys_dropped` says whether any had keys (their offsets' keys go). */
int drawing_detail_apply(VectorDrawing &drawing, const std::vector<bool> &shapes, bool *r_keys_dropped);
/* Rebuild Levels: the picked shapes' curves (their detail flattened first) keep the points that make
 * their shape as base points, and each point within `tolerance` of the curve its neighbours make
 * without it becomes level 1 detail, as they show. Returns how many points became detail; when none
 * can, the drawing is left as it was. `r_keys_dropped` as above. */
int drawing_detail_rebuild(VectorDrawing &drawing, const std::vector<bool> &shapes, float tolerance,
                           bool *r_keys_dropped);

}  // namespace oanim
