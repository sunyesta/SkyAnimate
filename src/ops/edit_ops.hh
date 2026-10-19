#pragma once

#include <vector>

/* Operators that edit layers and drawings. */

namespace sky {

struct BrushSettings;
struct Context;
struct Layer;
struct VectorDrawing;

void layer_operators_register();
void vector_operators_register();
void asset_operators_register();

/* Gives the selected shapes on every edited layer these brush settings. Returns how many shapes
 * changed (no undo step: the caller pushes one). */
int shapes_set_brush(Context &C, const BrushSettings &brush);

/* The active layer's drawing if it can be edited: a vector layer that is not a reference and
 * not locked. Null otherwise. */
VectorDrawing *editable_drawing(Context &C, Layer **r_layer = nullptr);

/* The layers point editing works on: the active layer when it is editable, then every other
 * visible editable layer picked in the Layers editor (Ctrl or Shift click). Selecting, G R S E,
 * delete and the other point operators act on all of them at once; drawing tools still draw on
 * the active layer. */
std::vector<Layer *> editable_layers(Context &C);

/* In Stroke mode, selects every point of the curves that already have a selected point, so
 * the whole stroke moves together. Does nothing in Point and Edge mode. */
void drawing_expand_selection_by_mode(Context &C, VectorDrawing &drawing);

}  // namespace sky
