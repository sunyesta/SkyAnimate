#pragma once

#include <cstdint>
#include <string>
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

/* A layer named by its id ("12") or its name. */
Layer *layer_by_name_or_id(Document &doc, const std::string &text);
/* "Inside head", "Outside mouth hole, teeth gap": what a mask does, for menus and undo steps. */
std::string mask_label(const Document &doc, const LayerMask &mask);
/* A layer's mask rows in words, as the Masking tab's header shows them ("Shows: inside head, minus
 * mouth, plus tongue"). */
std::string mask_summary(const Document &doc, const Layer &layer);
/* Each row's part of the summary ("minus mouth"), one per layer_mask_rows (empty for a row that's off). */
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

/* In Stroke mode, selects every point of the curves that already have a selected point, so
 * the whole stroke moves together. Does nothing in Point and Edge mode. */
void drawing_expand_selection_by_mode(Context &C, VectorDrawing &drawing);

}  // namespace oanim
