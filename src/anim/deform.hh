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

namespace oanim {

struct BonePose;
struct Document;
struct Layer;
struct Modifier;

/* Whether the layer shows anything other than its plain drawing. */
bool layer_has_modifier_effect(const Document &doc, const Layer &layer, bool render);
/* Whether a deform modifier on a group around the drawing bends it. They run after its own stack, nearest group first,
 * on every drawing inside however deep (a group's images and video stay as they are). */
bool layer_has_group_deforms(const Document &doc, const Layer &layer, bool render);
/* While a drawing reads another as a deform's shape: that one shows without the group deforms of the groups the reader
 * is in, which bend the reader once themselves. */
struct GroupDeformReading {
  explicit GroupDeformReading(uint64_t layer);
  ~GroupDeformReading();
};
/* Points of a bone layer's space (bones) where the deforms on it and the groups around it bend them, as they bend the
 * drawings the bones carry: bones show and are picked there, their data stays as it is. */
std::vector<float2> layer_group_deform_points(const Document &doc, const Layer &layer, std::vector<float2> points);
/* How those deforms move things right around `at`: from there, the way a drag shown on screen is taken back. */
Transform2D layer_group_deform_at(const Document &doc, const Layer &layer, float2 at);
/* The layer's setup drawing moved as its bones now move it: a deform's shape at rest when only its change of shape
 * should count (Modifier::deform_rest_posed). */
void layer_pose_drawing(const Document &doc, const Layer &layer, VectorDrawing &drawing);
/* With `r_rigid`: when the bones carry the whole drawing as one piece (turned and moved, as by a head
 * bone), their move is left out of the result and put there, to draw it with; otherwise identity. A
 * drawing kept that way while bones turn is drawn again from what was made of it before. */
VectorDrawing layer_evaluated_drawing(const Document &doc, const Layer &layer, bool render,
                                      Transform2D *r_rigid = nullptr);
/* A drawing as layer_evaluated_drawing makes it, and its `r_rigid`. */
struct EvaluatedDrawing {
  VectorDrawing drawing;
  Transform2D rigid;
};
/* Each of `layers` as layer_evaluated_drawing makes it, in their order: worked out side by side on the pool's threads
 * (a render's drawings, bent before any is drawn), each the same as on its own. Nothing in the document may change
 * meanwhile. `r_threads`: how many threads took part. */
std::vector<EvaluatedDrawing> layers_evaluated_drawings(const Document &doc, const std::vector<const Layer *> &layers,
                                                        bool render, int *r_threads = nullptr);
/* The layer's drawing as drawn on the setup frame, with its Mirror modifiers (bones don't bend
 * it there): what brushed lines lay their dabs out on, so they stick to the artwork. */
VectorDrawing layer_setup_drawing(const Document &doc, const Layer &layer, bool render);
/* For each of the layer's own points: from where it is stored to where it shows (layer space).
 * Identity everywhere when no bones move it. */
std::vector<Transform2D> layer_point_skins(const Document &doc, const Layer &layer);
/* Whether a deform's Resample Self fits the layer's segments afresh: their handles as they show then come of fitting the
 * bent line, not of bending the handles, so a small change to one can turn up anywhere. Such a handle's end is shown and
 * edited as a place of its own, where the deforms put it (layer_places_shown), and the wire still takes the fit. */
bool layer_handles_fitted(const Document &doc, const Layer &layer);
/* A place in the layer's stored space, carried as point `point` is (its weights and bones). */
struct StackPlace {
  int point = 0;
  float2 at;
};
/* Where the stack shows places (layer space). */
std::vector<float2> layer_places_shown(const Document &doc, const Layer &layer, const std::vector<StackPlace> &places);
/* What the stack does right around a place, undone: from a move as it shows to the move stored. */
Transform2D layer_place_unskin(const Document &doc, const Layer &layer, const StackPlace &place);
/* A place to show one of the layer's points at (layer space), and its skin undone, from when it was picked up. */
struct PointTarget {
  int point = 0;
  float2 at;
  Transform2D unskin;
  /* How far the move itself takes the stored point. */
  float moved = 0.0f;
};
/* A skin is right only around where a point was: a deform isn't straight, so a point sent far by its skin alone lands
 * off. This moves the points on until they show at their targets, as near as the deforms let them. */
void layer_points_settle(const Document &doc, Layer &layer, const std::vector<PointTarget> &targets);
/* A handle dragged by itself, likewise: where its end is to show (layer space) as the edit wire shows it. `keep_other`
 * as curve_point_move_handle's. */
struct HandleTarget {
  int curve = 0;
  int index = 0;
  bool in = false;
  bool keep_other = false;
  float2 at;
  Transform2D unskin;
  /* How far the move itself takes the stored handle. */
  float moved = 0.0f;
};
void layer_handles_settle(const Document &doc, Layer &layer, const std::vector<HandleTarget> &targets);
/* The same with the bones posed as `poses` has them (each one's skin), for a change worked out
 * without posing the bones: Apply Pose as Rest Pose bending the setup drawing to the new rest. */
std::vector<Transform2D> layer_point_skins_posed(const Document &doc, const Layer &layer,
                                                 const std::vector<BonePose> &poses);

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
/* Points not weighted yet (new ones) in layers inside a bone layer with bones get weights: the average
 * of the points next to them along their curves, or none at all when they aren't connected to any
 * (Automatic Weights gives a new drawing some). `exact`: automatic weights only, unpruned (older files'
 * flexi-binding, unchanged). Runs before every undo step. `skip`: layers (and what is in them) known to
 * have no new points. */
bool document_weight_new_points(Document &doc, bool exact = false, bool (*skip)(const Layer &) = nullptr);

/* The Mirror modifier on a drawing; `flip_bone` maps a point's bone to the other side's. */
VectorDrawing drawing_mirror(const VectorDrawing &drawing, const Modifier &mirror,
                             const std::function<uint64_t(uint64_t)> &flip_bone);

}  // namespace oanim
