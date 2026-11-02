#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "core/math.hh"

/* Operators that edit layers and drawings. */

namespace oanim {

struct BrushSettings;
struct Context;
struct Document;
struct Layer;
struct LayerMask;
struct Shape;
struct VectorDrawing;
class InkStroke;
enum class DrawMode;
struct ToolSettings;

void layer_operators_register();
/* layer.copy and layer.paste (ops/layer_clipboard.cc). */
void layer_clipboard_operators_register();
void vector_operators_register();
void asset_operators_register();
void anim_operators_register();
void draw_order_operators_register();
void mask_operators_register();
void material_operators_register();
void driver_operators_register();
void rig_operators_register();
/* sculpt.* (ops/sculpt_ops.cc). */
void sculpt_operators_register();
/* draw.* (ops/draw_ops.cc). */
void draw_operators_register();
/* Ends a stroke of `ink` on `layer` as `mode` makes it (a line, painted fill, or a cut), with no undo step. False:
 * nothing came of it. */
bool draw_stroke_commit(Context &C, Layer &layer, InkStroke &ink, DrawMode mode);
/* Makes `mode` the Draw tool's kind of pen, with the preset it last used. */
void draw_mode_set(ToolSettings &ts, DrawMode mode);
/* annotation.* and guide.* (ops/annotation_ops.cc). */
void annotation_operators_register();
/* Where Ctrl puts handle `point` of guide `guide` (an index into the document's guides, or -1 for a
 * guide being made) dragged to `p` (document space): the nearest point of the drawing or another guide's
 * handle within `radius`, else the nearest point on another guide's straight line within it, else (for
 * a two-handle guide) the line's turn in 15 degree steps. `r_snapped`: whether it moved. */
float2 guide_snap(const Document &doc, int guide, int point, float2 p, float radius, bool *r_snapped);

/* A layer named by its id ("12") or its name. */
Layer *layer_by_name_or_id(Document &doc, const std::string &text);
/* "Subtract head", "Add head, Subtract outside neck": a card's steps, for menus and undo steps. */
std::string mask_label(const Document &doc, const LayerMask &mask);
/* Global masking areas, as shared materials: every step with the number. */
struct MaskAreaName {
  uint64_t id = 0;
  std::string name;
};
std::vector<MaskAreaName> mask_areas(const Document &doc);
int mask_area_users(const Document &doc, uint64_t global);
/* A layer's mask cards in words, as the Masking tab's header shows them ("Shows: inside head, minus
 * mouth; lines: everything but ear, lines ending at the edge"). */
std::string mask_summary(const Document &doc, const Layer &layer);
/* Each card's part of the summary, one per layer_mask_cards (empty for a card that's off). */
std::vector<std::string> mask_row_phrases(const Document &doc, const Layer &layer);

/* The bone layer rigging works on: the active layer if it is one, else the bone layer it is in. */
Layer *active_armature(Context &C);
/* Adds a bone running from `head` to `tail` (bone layer space, setup pose) under `parent` (0 for
 * a root), selects it and makes it active. */
struct Bone;
Bone *armature_add_bone(Layer &armature, const std::string &name, float2 head, float2 tail, uint64_t parent);

/* The layers keying works on: the active layer and those picked in the Layers editor. */
std::vector<Layer *> keyed_layers(Context &C);
/* What the timeline shows: keyed_layers, plus the animated layers inside a selected bone layer
 * and the document's Simulation settings when keyed. */
std::vector<Layer *> timeline_shown_layers(Context &C);
/* What the timeline edits keys of: those, plus the layers of pinned channels (listed under
 * Pinned whatever is picked). */
std::vector<Layer *> timeline_layers(Context &C);
/* The layers with pinned channels, flat order (kept between frames until the next change). */
const std::vector<uint64_t> &timeline_pinned_layer_ids(Context &C);

/* Gives the selected shapes on every edited layer these brush settings. Returns how many shapes
 * changed (no undo step: the caller pushes one). */
int shapes_set_brush(Context &C, const BrushSettings &brush);
/* The brush of the shape the Material panel shows (the active shape while selected, else the topmost
 * selected). False when no shape is selected. */
bool shown_shape_brush(Context &C, BrushSettings *r_brush);
Shape *shown_shape(Context &C);
/* What changed from `before` to `after` (one field, typically), given to every selected shape;
 * their other settings stay. Returns how many changed. */
int shapes_change_brush(Context &C, const BrushSettings &before, const BrushSettings &after);

/* The active layer's drawing if it can be edited: a vector layer that is not locked (a reference's
 * looks, and its points with Edit Points on). Null otherwise. */
VectorDrawing *editable_drawing(Context &C, Layer **r_layer = nullptr);

/* The layers point editing works on: the active layer when it is editable, then every other
 * visible editable layer picked in the Layers editor (Ctrl or Shift click). Selecting, G R S E,
 * delete and the other point operators act on all of them at once; drawing tools still draw on
 * the active layer. */
std::vector<Layer *> editable_layers(Context &C);
std::vector<Layer *> document_editable_layers(Document &doc);
/* What the weight tools work on, so only one bone layer's bones are ever in play: the active drawing alone, whatever
 * else is picked; or on an active bone layer, or group with Edit Descendants, the drawings inside it that go by those
 * bones. A bone layer inside has its own, and a Parent Bone moves what has one as a whole, so those stay out, and
 * nothing under a Parent Bone is weighed at all. The weight tools' editable_layers. */
std::vector<Layer *> document_weight_layers(Document &doc);
/* Their ids, for KeyFilter::point_layers: the points whose selection counts now. */
std::shared_ptr<const std::set<uint64_t>> editable_layer_ids(Document &doc);

/* For an operator flagged OPTYPE_MIRROR while X mirror editing is on: the selected points' mirror
 * images are selected too, in the same layer (across its axis) or in a sided layer's partner (Ear_R
 * for Ear_L, picked for the run so it is edited too), so the operator does the same on both sides.
 * _end deselects what was added and puts the partners' picking back. */
struct MirrorSelection {
  /* By layer id: the point ids selected only for the run. */
  std::map<uint64_t, std::vector<uint64_t>> added;
  std::vector<uint64_t> picked;
};
MirrorSelection mirror_selection_begin(Context &C);
void mirror_selection_end(Context &C, const MirrorSelection &mirror);

/* In Stroke mode, selects every point of the curves that already have a selected point, so
 * the whole stroke moves together. Does nothing in Point and Edge mode. */
void drawing_expand_selection_by_mode(Context &C, VectorDrawing &drawing);

}  // namespace oanim
