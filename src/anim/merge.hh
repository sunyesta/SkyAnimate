/* The Boolean modifier: a layer's shapes and other layers' made one outline (union, subtract or
 * intersect), the corners where they meet rounded by Smoothing. The result is ordinary curves, so
 * brushes, masks and line ends work on it; the shapes it takes in from other layers stop drawing
 * there (Shape::merged), so nothing is drawn twice. */

#pragma once

#include <cstdint>
#include <vector>

#include "core/math.hh"
#include "data/layer.hh"

namespace oanim {

struct Document;

/* Shapes of another drawing to merge in: `to_layer` takes its points into the merging layer's space;
 * `shapes` picks them by id (empty: every one that goes all the way round). */
struct MergeSource {
  const VectorDrawing *drawing = nullptr;
  Transform2D to_layer;
  std::vector<uint64_t> shapes;
};

/* Merges `own` shapes of `drawing` (ids; empty: every closed one) with the sources' into new
 * curves and shapes in `drawing`, put where the first own shape was, with its look; the own
 * shapes merged leave. `corners_only`: they only gain the rounded corners where they meet the
 * sources. Returns false (nothing changed) when there are no own shapes to merge. */
bool drawing_merge(VectorDrawing &drawing, const std::vector<uint64_t> &own, const std::vector<MergeSource> &sources,
                   MergeOperation operation, float smoothing, bool corners_only = false);

/* Whether a shape of a drawing is one a Boolean picking `ids` takes: picked (or `ids` empty) and
 * going all the way round. */
bool merge_takes_shape(const VectorDrawing &drawing, const Shape &shape, const std::vector<uint64_t> &ids);

/* The Boolean modifier `m` of `layer` run on its drawing as evaluated so far (layer space, posed). */
void merge_modifier_apply(const Document &doc, const Layer &layer, const Modifier &m, VectorDrawing &drawing,
                          bool render);

/* Whether a shown layer's Boolean modifier takes shapes from `layer` (shown too); and marks those
 * shapes of its evaluated drawing as merged (not drawn there). */
bool merge_takes_from(const Document &doc, const Layer &layer, bool render);
void merge_mark_taken(const Document &doc, const Layer &layer, VectorDrawing &drawing, bool render);

/* Whether a layer and the groups it is in are all shown. */
bool layer_shown_in_document(const Document &doc, uint64_t id);

}  // namespace oanim
