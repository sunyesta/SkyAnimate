#pragma once

/* Shared widgets for editors, like Blender's UI layout: panels of properties drawn from their
 * RNA descriptions. */

#include "rna/rna.hh"

namespace sky {

struct Context;

/* A collapsible panel with a two-column (label, value) table. Call ui_panel_end() only when
 * this returns true. */
bool ui_panel_begin(const char *title, bool default_open = true);
void ui_panel_end();

/* One property row inside a panel. Edits push an undo step when the widget is released.
 * `python_path` is shown in the tooltip, e.g. "sky.data.document". */
void ui_prop(Context &C, const rna::PointerRNA &ptr, const char *identifier, const char *python_path);

/* A colour row inside a panel; returns true when the colour changed. */
bool ui_color(Context &C, const char *label, float rgba[4], const char *undo_name);

/* A colour swatch that opens SkyAnimate's colour picker, which shows #RRGGBBAA and has an
 * eyedropper; with `hex`, a #RRGGBBAA field beside it too. Returns true while the colour
 * changes; `r_finished` (optional) is set when an edit ends, for an undo step. */
bool ui_color_edit(const char *id, float rgba[4], bool hex, bool *r_finished = nullptr);

/* The eyedropper picks a colour from anywhere in the window. Draw the overlay with the rest of
 * the UI; take the sample after the frame is drawn, before it is shown (it reads the pixel). */
void ui_eyedropper_overlay();
void ui_eyedropper_sample(int framebuffer_height);

}  // namespace sky
