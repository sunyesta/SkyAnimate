/* The deform modifiers, Curve, Cage and Mesh Deform: a shape on any layer bends a layer as it moves from where it
 * is on the setup frame (Moho's Smart Warp, Live2D's warp deformers, Blender's Curve, Mesh Deform and Lattice).
 * Each takes its own shapes (Curve Deform an open line or a Part of any line or closed shape, Cage Deform one closed
 * shape, Mesh Deform a grid or a mesh of cells, or a closed four-sided shape as one cell); their kind:
 *
 * - Grid (the Shape tool's Grid): each cell bends what is in it as its Coons patch (data/grid.hh), so a
 *   grid line curved with its handles curves what lies along it.
 * - Cage (a closed shape): mean value coordinates against the shape's outline as drawn, curves and all,
 *   so pulling a handle bends what is inside, and a point on the outline stays on it.
 * - Line (an open shape): what lies along it keeps its place along the line and its offset from it.
 * - A network of segments drawn by hand (lines joined into cells, Make Segment's welds): each cell bends what is
 *   in it, a four-sided one as a grid's cell does (so a hand-made grid is a Grid), any other as a small cage
 *   (Mesh). Cells sharing a side follow it alike, so they join without a seam.
 * - Lines in a Mesh Deform (an open line, or lines left dangling from a mesh: Wire when it is only lines): each point
 *   is pinned on frame 0 to its nearest place on them (that segment, that far along it) and its offset from there,
 *   and goes where that place goes, turned as the line turns there, as a bendy bone carries what it holds: nothing
 *   slides along the line. Past an end it rides that end. Points outside the cells follow the lines and the
 *   outline together, each the more the nearer, so their pulls meet without a seam.
 *
 * Each point follows it by its weight: Automatic (worked out from where the point is against the shape,
 * never stored) or Custom (a point group, painted in Weight Paint). The weight also shares the point
 * between the deform and the bones: at 1 it follows only the shape (which brings the bones' move along
 * when the shape follows them too), at 0 only its bones, so nothing moves twice. Handles are bent too.
 * Frame 0 is the rest: editing the shape there changes what the deform measures from, never bends. */

#pragma once

#include <memory>
#include <vector>

#include "data/vector.hh"

namespace oanim {

struct Document;
struct Layer;
struct Modifier;
struct WeightPaintBrush;
enum class ModifierType;

enum class DeformKind { None, Grid, Cage, Line, Mesh, Wire };
/* "Grid", "Cage", "Line", "Mesh", "Wire" (None: ""). */
const char *deform_kind_name(DeformKind kind);

/* What `m`'s shape is (None: no such layer or shape, or it is on `layer` itself). */
DeformKind vector_deform_kind(const Document &doc, const Layer &layer, const Modifier &m);
/* Whether it deforms by a grid drawn as one, what Smooth works on (not a mesh of cells drawn by hand). */
bool vector_deform_drawn_grid(const Document &doc, const Layer &layer, const Modifier &m);
/* Whether `m`'s shape is one line or loop (no grid, no branches): what a Curve Deform's Part is a stretch of. */
bool vector_deform_takes_part(const Document &doc, const Modifier &m);
/* Sets `m`'s Part from the points selected on its shape, which must run along it end to end, two at least and not
 * a whole loop. False, `m` as it was, if they don't. */
bool vector_deform_part_from_selection(Document &doc, Modifier &m);
/* The deform type (Curve, Cage or Mesh Deform) that takes `m`'s shape: its own when it does (a closed
 * four-sided shape fits Cage and Mesh Deform both), else the one that would; its own when none would. */
ModifierType vector_deform_fitting_type(const Document &doc, const Layer &layer, const Modifier &m);
/* After opening a file of the single Vector Deform modifier of before: each becomes the deform its shape fits
 * (its default name with it). */
void vector_deform_resolve_legacy(Document &doc);

/* Whether `m`'s shape is bent, through other deforms, by `layer` itself: such a loop does nothing. */
bool vector_deform_loops(const Document &doc, const Layer &layer, const Modifier &m);

/* `m`'s shape as it is now, to read places by: null on frame 0, or when it bends nothing. */
struct DeformShape;
std::shared_ptr<const DeformShape> vector_deform_shape(const Document &doc, const Layer &layer, const Modifier &m);
/* Where that shape puts place `at`, by `m`'s Strength alone (no drawing point, so no weights). `at` itself with none. */
float2 vector_deform_point(const DeformShape *shape, const Modifier &m, float2 at);

/* What an Envelope's Range reaches, in `layer`'s space where it shows now: lines running in from the cage's outline,
 * each place with how much say the sides have there (1 at the outline, 0 at the limit); `quads` joins them. A place in
 * `limit` is where a line got as far as the limit, so the limit is drawn from one to the next. */
struct DeformRangeView {
  std::vector<float2> at;
  std::vector<float> say;
  std::vector<std::array<int, 4>> quads;
  std::vector<std::array<int, 2>> limit;
};
bool vector_deform_range_view(const Document &doc, const Layer &layer, const Modifier &m, DeformRangeView &r);
/* Where an Envelope's Blend matters, the same way (no `limit`): `say` is how evenly the sides share each place, 1 where
 * two have an even say, 0 where one leads alone, faded as the sides' say fades inside the cage. Lines run in until
 * another side is nearer, so they meet where sides hand over. */
bool vector_deform_blend_view(const Document &doc, const Layer &layer, const Modifier &m, DeformRangeView &r);

/* `m`'s shape at rest and now, in `layer`'s space, as its build reads it: to read a point of the shape by. False with
 * nothing to read. */
bool vector_deform_shape_read(const Document &doc, const Layer &layer, const Modifier &m, VectorDrawing &r_rest,
                              VectorDrawing &r_bent);

/* Follow Deform `m` on `layer` now: what moves the whole drawing. `carry` is what its bones moved it by: with
 * `m.deform_rest_posed` the shape's rest is posed by them too, so only its change of shape is added to it. */
Transform2D vector_deform_follow(const Document &doc, const Layer &layer, const Modifier &m, float2 carry, bool render);
/* How much of that move each of `drawing`'s points takes, by its Limit Method: `drawing` is the layer's as the stack has
 * it so far, so a point something above has cut into a segment takes its share from the two ends. Empty with no limit,
 * every point taking all of it. A shape or point group that isn't there moves nothing. */
std::vector<float> vector_deform_follow_share(const Document &doc, const Layer &layer, const Modifier &m,
                                              const VectorDrawing &drawing);
/* The middle of what it moves, at rest in the layer's space: where its point starts. */
float2 vector_deform_follow_middle(const Document &doc, const Layer &layer, const Modifier &m);
/* After its Limit Method changed from `was`: a point still at the middle of what it moved goes to the middle of what it
 * moves now. One put somewhere stays there. */
void vector_deform_follow_limit_changed(const Document &doc, const Layer &layer, const Modifier &was, Modifier &m);
/* Where its point (`second`: its second) rests on the drawing, and where it is now, in the layer's space. */
float2 vector_deform_follow_rest(const Document &doc, const Layer &layer, const Modifier &m, bool second);
float2 vector_deform_follow_now(const Document &doc, const Layer &layer, const Modifier &m, bool second);
/* What its point (`second`: its second) has to be shifted by to show at `to` (layer space) on this frame, through the
 * bend. */
float2 vector_deform_follow_shift(const Document &doc, const Layer &layer, const Modifier &m, bool second, float2 to);
/* Its second point on or off: on for the first time, it starts beside the first. */
void vector_deform_follow_two(Modifier &m, bool on);
/* Its point (`second`: its second) goes as point `id` of the shape does (0: its own place, where the shape's point
 * rested). Nothing jumps unless the shape's point is somewhere else. */
void vector_deform_follow_pick(const Document &doc, const Layer &layer, Modifier &m, bool second, uint64_t id);

/* The modifier on `drawing`, the stack's drawing so far. `before_bones` is that drawing as it was before the
 * Armature modifier moved it (the drawing itself when none did): where the deform measures each point from,
 * and what its weight shares between the deform and the bones. */
void vector_deform_apply(const Document &doc, const Layer &layer, const Modifier &m, VectorDrawing &drawing,
                         const VectorDrawing &before_bones, bool render);

/* While one is alive, deform modifiers leave out Precision and bend every handle, those still worked out from
 * the neighbours too (made explicit first): each segment stays one Bezier, bent as near as one can be. What the
 * viewport's edit wire draws between the points, so it shows the bend where they are. */
struct VectorDeformWireScope {
  VectorDeformWireScope();
  ~VectorDeformWireScope();
  VectorDeformWireScope(const VectorDeformWireScope &) = delete;
  VectorDeformWireScope &operator=(const VectorDeformWireScope &) = delete;
};

/* While alive: what the deforms make on this thread is kept apart from what the drawing itself makes, under `kind` (1, 2...),
 * for the drawing moved a little to measure the stretch around its points. */
struct VectorDeformProbeScope {
  explicit VectorDeformProbeScope(int kind);
  ~VectorDeformProbeScope();
  VectorDeformProbeScope(const VectorDeformProbeScope &) = delete;
  VectorDeformProbeScope &operator=(const VectorDeformProbeScope &) = delete;
};

/* How much each of the layer's own points follows the deform, 0-1, before Strength: its Custom weight, else
 * the automatic one. For Weight Paint's display. */
std::vector<float> vector_deform_weights(const Document &doc, const Layer &layer, const Modifier &m);
/* The automatic weights alone. */
std::vector<float> vector_deform_auto_weights(const Document &doc, const Layer &layer, const Modifier &m);

/* Fills the modifier's point group (made, named after the modifier, if it has none) with the automatic
 * weights: switching to Custom, and Reset to Automatic. */
void vector_deform_fill_group(const Document &doc, Layer &layer, Modifier &m);

/* One Weight Paint dab on a Custom modifier's point group (`brush.bone` unused; Normalize doesn't apply).
 * A point painted for the first time starts from its automatic weight. Returns how many points changed. */
int vector_deform_paint(const Document &doc, Layer &layer, const Modifier &m, const WeightPaintBrush &brush);

}  // namespace oanim
