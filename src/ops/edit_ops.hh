#pragma once

/* Operators that edit layers and drawings. */

namespace sky {

struct Context;
struct Layer;
struct VectorDrawing;

void layer_operators_register();
void vector_operators_register();

/* The active layer's drawing if it can be edited: a vector layer that is not a reference and
 * not locked. Null otherwise. */
VectorDrawing *editable_drawing(Context &C, Layer **r_layer = nullptr);

/* In Stroke mode, selects every point of the curves that already have a selected point, so
 * the whole stroke moves together. Does nothing in Point and Edge mode. */
void drawing_expand_selection_by_mode(Context &C, VectorDrawing &drawing);

}  // namespace sky
