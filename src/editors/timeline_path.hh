#pragma once

/* The timeline's timing chart (row A of the spec's Motion paths: speed editing), shown only while the Motion Path
 * tool is in hand: a strip at the bottom of the keys, a row for each thing the paths trace. Each stretch between keys
 * is a line from key to key with a tick for each frame, placed by how far along the path that frame has got, as an
 * animator's timing chart; ticks bunched up are slow, spread out fast. Ticks are coloured as the path's dots are (time
 * through the stretch, the stretch being worked on lit). With Edit, a tick is a dot: a click selects it (Shift adds,
 * a double click goes to its frame), a drag slides it along the path (anim.motion_path_slide with progress=,
 * proportional editing as in the viewport), Esc or a right click cancels. */

#include "imgui.h"

namespace oanim {

struct Context;

/* How tall the strip is (0 when it isn't shown), for rows `row_h` tall. */
float timeline_path_strip_height(Context &C, float row_h);
/* Draws the strip from `min` to `max` (the names column ending at `keys_x`, frames `view_start` + `span` over the
 * keys' width) and takes its clicks. */
void timeline_path_strip(Context &C, ImVec2 min, ImVec2 max, float keys_x, float view_start, float span,
                         float row_h);

/* The timeline's sidebar (N) and its Path Timing tab (panel B of the spec's speed editing): one stretch at a time of
 * the last thing picked on, as a progress graph (how far along its path, 0-1, against its frames, the even speed
 * dashed), its dots dragged up or down to slide them (Ctrl onto the even line), with the selected dot's % along and
 * speed, the shape buttons and arrows to the stretches either side. 0 wide while it is hidden. */
float timeline_sidebar_width(Context &C, float avail_w);
void timeline_sidebar(Context &C, ImVec2 min, ImVec2 max);

}  // namespace oanim
