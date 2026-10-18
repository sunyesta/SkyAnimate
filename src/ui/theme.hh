#pragma once

/* Blender's default theme applied to Dear ImGui. Colours come from Blender's
 * release/datafiles/userdef/userdef_default_theme.c. */

namespace sky {

/* Base font size in pixels at 100% interface scale. */
inline constexpr float UI_FONT_SIZE = 15.0f;

/* Loads the interface font. Call once after creating the ImGui context. */
void theme_load_fonts();
/* Resets the ImGui style to the Blender theme at the given interface scale. */
void theme_apply_blender(float ui_scale);

}  // namespace sky
