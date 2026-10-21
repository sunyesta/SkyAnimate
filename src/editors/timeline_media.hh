#pragma once

/* The timeline's Media row (shown under Selected while a Media layer is active): its clips as bars,
 * as a video editor's track. Each bar shows its file as a filmstrip (or its sound as a waveform), its
 * name, and hatching where the file has run out (Hold, Loop, Hide). Dragging a bar moves it, its
 * edges trim it, Alt-drag slips it (the bar stays, the part of the file it shows moves), as DaVinci
 * Resolve's smart trim picks the edit from where the mouse is, the cursor saying which. Edges snap to
 * the playhead, the frame range and other clips' edges (Ctrl while dragging: no snapping), and a
 * readout follows the mouse. Right-click has the rest (Replace, Split, Delete, After End, Timing). */

#include "imgui.h"

namespace oanim {

struct Context;
struct Layer;

/* Where the timeline's keys region is and which frames it shows. */
struct TimelineMediaView {
  float x0 = 0, x1 = 0;
  float view_start = 0, span = 1, keys_w = 1;
  float x_of(float frame) const { return x0 + (frame - view_start) / span * keys_w; }
  float frame_of(float x) const { return view_start + (x - x0) / keys_w * span; }
  float px_per_frame() const { return keys_w / span; }
};

void timeline_media_row_draw(Context &C, ImDrawList *dl, const TimelineMediaView &v, const Layer &layer, float top,
                             float h);
/* A left press on the row at the mouse: picks the clip under it and starts dragging it. Returns true
 * when it took the press. */
bool timeline_media_row_press(Context &C, const TimelineMediaView &v, Layer &layer, float top, float h);
/* Each frame: carries a drag on (moving, trimming or slipping), ending it on release. True while one
 * is going on. */
bool timeline_media_drag_update(Context &C, const TimelineMediaView &v);
/* The mouse over the row, no button down: the cursor for what a drag would do, and a tooltip. */
void timeline_media_row_hover(Context &C, const TimelineMediaView &v, const Layer &layer, float top, float h);
/* A file being dragged in from Explorer over the timeline: the clip it would make, at the frame under
 * it, in the band from `top` (`h` tall): into `layer`'s clips row, or (null) as a new Media layer. */
void timeline_media_drop_preview(Context &C, ImDrawList *dl, const TimelineMediaView &v, const Layer *layer, float top,
                                 float h);
/* Right-click on the row: the clip menu (opened here, drawn by timeline_media_menu). */
void timeline_media_menu_open(Context &C, const TimelineMediaView &v, const Layer &layer);
void timeline_media_menu(Context &C);

}  // namespace oanim
