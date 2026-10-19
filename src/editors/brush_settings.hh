#pragma once

/* Moho's Brush Settings window: the asset library's brushes as a grid of stamps (None first),
 * the brush settings, and a preview line. OK hands the settings back; Cancel drops them. */

#include <functional>

#include "data/vector.hh"

namespace sky {

struct Context;

using BrushSettingsDone = std::function<void(const BrushSettings &settings)>;

/* Opens the window on `current`, previewed with `style`'s line colour. */
void brush_settings_open(const BrushSettings &current, const Style &style, BrushSettingsDone done);
/* Draws the window while it is open. Call once per frame. */
void brush_settings_draw(Context &C);

}  // namespace sky
