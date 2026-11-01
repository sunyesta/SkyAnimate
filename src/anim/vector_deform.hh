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

/* Where `m` puts place `at` of `layer`'s space now, by its Strength alone (no drawing point, so no weights). `at`
 * itself on frame 0, or when it bends nothing. */
float2 vector_deform_point(const Document &doc, const Layer &layer, const Modifier &m, float2 at);

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
