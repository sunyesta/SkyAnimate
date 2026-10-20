#pragma once

/* A layer's drawing as it shows: its modifiers run in stack order on a copy, never on the
 * drawing itself (Blender's modifier stack).
 *
 * - Mirror adds a reflected copy. Points on the axis are shared, so a curve ending there joins
 *   its reflection into one line; mirrored points bound to a ..._L bone follow ..._R.
 * - Armature moves points by the bones' change from the setup pose, blended by each point's
 *   weights. A layer parented to a bone moves as a whole instead (its transform).
 *
 * The drawing's own points keep their indices in the result (Mirror only adds), so editing
 * can show and pick points where they appear. */

#include <functional>
#include <vector>

#include "data/vector.hh"

namespace sky {

struct Document;
struct Layer;
struct Modifier;

/* Whether the layer shows anything other than its plain drawing. */
bool layer_has_modifier_effect(const Document &doc, const Layer &layer, bool render);
VectorDrawing layer_evaluated_drawing(const Document &doc, const Layer &layer, bool render);
/* The layer's drawing as drawn on the setup frame, with its Mirror modifiers (bones don't bend
 * it there): what brushed lines lay their dabs out on, so they stick to the artwork. */
VectorDrawing layer_setup_drawing(const Document &doc, const Layer &layer, bool render);
/* For each of the layer's own points: from where it is stored to where it shows (layer space).
 * Identity everywhere when no bones move it. */
std::vector<Transform2D> layer_point_skins(const Document &doc, const Layer &layer);

/* How each of the layer's own points follows `bone`, 0-1 (its share of the point's motion), from
 * its weights. For the weight display. */
std::vector<float> layer_point_bone_weights(const Document &doc, const Layer &layer, uint64_t bone);

enum class WeightPaintMode { Draw, Add, Subtract };
struct WeightPaintBrush {
  /* Layer space. */
  float2 center;
  float radius = 0.1f;
  uint64_t bone = 0;
  /* Draw moves weights towards it; Add and Subtract add or take it away. */
  float weight = 1.0f;
  float strength = 0.5f;
  WeightPaintMode mode = WeightPaintMode::Draw;
  /* Keep each point's weights adding up to 1, taking from (or giving to) the other bones. */
  bool normalize = true;
  /* Where each point shows (layer space), when bones move it; null tests stored positions. */
  const std::vector<float2> *positions = nullptr;
  /* Blender's selection masking: only selected points take paint. */
  bool selected_only = false;
};
/* One dab of Blender's weight paint brush on a layer inside a bone layer: points within the
 * radius change, more at the centre (smooth falloff). A point painted for the first time starts
 * from how it followed the bones before. A layer parented to a bone stops being (its points now
 * follow their weights). Returns how many points changed. */
int layer_weight_paint(const Document &doc, Layer &layer, const WeightPaintBrush &brush);

/* Blender's Automatic Weights for a layer inside a bone layer: each point (the selected ones, when
 * `selected_only`) gets fresh weights from `bones` (empty: all of them), the nearer bones pulling
 * harder. A layer parented to a bone stops being. Returns how many points changed. */
int layer_automatic_weights(const Document &doc, Layer &layer, const std::vector<uint64_t> &bones,
                            bool selected_only);
/* Points not weighted yet (new ones) in layers inside a bone layer with bones get weights: those of
 * the nearest weighted points along their curves, or automatic weights when there are none.
 * `exact`: automatic weights only, unpruned (older files' flexi-binding, unchanged). Runs before
 * every undo step, so a new stroke follows the bones at once. */
bool document_weight_new_points(Document &doc, bool exact = false);

/* The Mirror modifier on a drawing; `flip_bone` maps a point's bone to the other side's. */
VectorDrawing drawing_mirror(const VectorDrawing &drawing, const Modifier &mirror,
                             const std::function<uint64_t(uint64_t)> &flip_bone);

}  // namespace sky
