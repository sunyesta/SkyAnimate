#pragma once

/* The Brush section, shared by the Material panel (the selected shapes' brush) and the Draw tool's
 * header (the brush new strokes get): a preset button (Blender's preset menu), a preview stroke, the
 * Shape (the stamp images, None = a solid line), Spacing, and the other settings folded under
 * Advanced.
 *
 * The preset menu lists this file's presets and the asset library's, each with a preview stroke in
 * the line's colour. Hovering one shows it on the shapes (or the tool) until the mouse leaves; a
 * click loads it (brush.preset_load). */

#include "data/vector.hh"

namespace oanim {

struct Context;

enum class BrushTarget { Shapes, Tool };

struct BrushEditResult {
  /* `brush` changed: show it (apply it to what it belongs to). */
  bool changed = false;
  /* An edit finished (a click, or the end of a drag): the moment for an undo step. */
  bool done = false;
  /* The revert button was clicked. */
  bool revert = false;
};

/* Draws the section as rows of the current two-column table (label, value), editing `brush` in
 * place. `style` gives the preview its line colour. `revert_tip` adds a revert button to the first
 * row (a reference's changed brush). */
BrushEditResult brush_editor(Context &C, BrushSettings &brush, const Style &style, BrushTarget target,
                             const char *revert_tip = nullptr);

/* Shows or takes back the hovered preset (call once per frame, after the UI is drawn). */
void brush_editor_frame_end(Context &C);

}  // namespace oanim
