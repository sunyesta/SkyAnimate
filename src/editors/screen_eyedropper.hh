#pragma once

/* The colour eyedropper over the whole screen (Windows): a pipette cursor that picks from any window,
 * Open Animate's or another app's, with the colour under its tip shown in a round swatch at the bottom
 * right of the cursor, as Substance Painter's.
 *
 * A clear window covering the desktop takes the click (so the window under it doesn't) and gives the
 * pipette cursor everywhere; a small click-through window draws the swatch. Both are left out of screen
 * capture, so what is sampled is what is under them. */

namespace oanim {

/* Starts it. False when the windows can't be made (then the in-window eyedropper is used). */
bool screen_eyedropper_begin();
/* Each frame while it runs: the swatch follows the cursor with the colour under it. Returns true once
 * it has ended: `r_picked` says whether a colour was picked (left click) or not (right click, Esc, or
 * switching away), and `r_rgb` holds it. */
bool screen_eyedropper_update(bool *r_picked, float r_rgb[3]);
bool screen_eyedropper_active();
/* Takes the windows down (also when it ends by itself). */
void screen_eyedropper_end();

}  // namespace oanim
