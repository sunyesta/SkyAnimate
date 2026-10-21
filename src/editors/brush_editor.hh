#pragma once

/* Brushes in the UI, after Photoshop's Brush Settings and Brushes panels:
 *
 * - The Material panel's Brush row shows a preview stroke, named with the brush's preset ("Ink") or
 *   "Custom", with Blender's preset button (three sliders) at its left. The
 *   button opens the Brush Settings window on its settings, the preview on its presets.
 * - The Brush Settings window floats over the app (the viewport stays in view and shows every change
 *   as it's made). Its Brush Settings tab has Photoshop's sections down the left, each with its
 *   switch (Brush Tip Shape, Shape Dynamics, Scattering, Color Dynamics, Transfer, Line Boil), the
 *   chosen one's settings on the right and a preview stroke along the bottom. Its Brushes tab lists
 *   the presets (the project's, then the library's by category) as preview strokes: hovering one shows
 *   it on the shapes until the mouse leaves, a click loads it. Its footer saves the brush as a preset,
 *   in the project or in a library category (Substance Painter's dropdown).
 *
 * The window edits the selected shapes' brush (the one the Material panel shows, every selected
 * shape getting just the setting changed), or the Draw tool's, for new strokes. */

#include <string>

#include "imgui.h"

#include "data/vector.hh"

namespace oanim {

struct Context;
struct Document;

enum class BrushTarget { Shapes, Tool };

/* The Material panel's Brush row (a row of the current two-column table), previewed in `style`'s
 * line colour. `revert_tip` adds a revert button (a reference's changed brush); returns true when
 * it's clicked. */
bool brush_row(Context &C, const BrushSettings &brush, const Style &style, const char *revert_tip = nullptr);
/* Opens the Brush Settings window on the selected shapes' brush or the Draw tool's. */
void brush_window_open(BrushTarget target);
/* Draws the window while it's open. Call once per frame. */
void brush_window_draw(Context &C);
/* Shows or takes back the hovered preset (call once per frame, after the UI is drawn). */
void brush_editor_frame_end(Context &C);

/* How the UI names a brush: the preset it matches ("Ink"), "Custom" when it matches none (edited
 * since it was loaded, or never one), or "Solid line". */
std::string brush_shown_name(const Document &doc, const BrushSettings &brush);
/* Blender's preset icon: three sliders, centred in the box a..b. */
void brush_preset_icon(ImDrawList *dl, ImVec2 a, ImVec2 b, unsigned int color);

}  // namespace oanim
