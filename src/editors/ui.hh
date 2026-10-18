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

}  // namespace sky
