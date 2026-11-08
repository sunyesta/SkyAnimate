#pragma once

/* The timeline's timing chart (row A of the spec's Motion paths: speed editing), shown only while the Motion Path
 * tool is in hand: a strip at the bottom of the keys, a row for each thing the paths trace. Each stretch between keys
 * is a line from key to key with a tick for each frame, placed by how far along the path that frame has got, as an
 * animator's timing chart; ticks bunched up are slow, spread out fast. Ticks are coloured as the path's dots are (time
 * through the stretch, the stretch being worked on lit). With Edit, a tick is a dot: a click selects it (Shift adds,
 * a double click goes to its frame), a drag slides it along the path (anim.motion_path_slide with progress=),
 * Esc or a right click cancels. Proportional editing is always on here, with the chart's own reach in frames
 * (path_chart_size, 0 the dot alone): shown as a band around the dot under the mouse, sized by the wheel while
 * dragging, the dots it pulls ringed. A Shift click or drag blends the timing there as a brush, a blend each frame the
 * mouse reaches (anim.motion_path_timing_blend with the stroke's frames, its settings in the timeline's Adjust Last
 * Operation panel; its reach a blue band, sized by the wheel while Shift is held); Ctrl click adds a dot to the
 * selection. Over the strip, the wheel zooms the timeline. */

#include <functional>

#include "imgui.h"
#include "ops/operator.hh"

namespace oanim {

struct Context;
struct KeySettings;

/* The timeline's Interpolation menu's modes (with their curves and Strength), `current` ticked unless `mixed`; a
 * choice calls `set` with anim.keys_interpolation_set's arguments for it (interp= or ease=). In timeline.cc. */
void timeline_interp_modes(const KeySettings &current, bool mixed, bool mixed_ease, bool separators,
                           const std::function<void(const OpArgs &)> &set);

/* How tall the strip is (0 when it isn't shown), for rows `row_h` tall. */
float timeline_path_strip_height(Context &C, float row_h);
/* Draws the strip from `min` to `max` (the names column ending at `keys_x`, frames `view_start` + `span` over the
 * keys' width) and takes its clicks. */
void timeline_path_strip(Context &C, ImVec2 min, ImVec2 max, float keys_x, float view_start, float span,
                         float row_h);
/* Whether the strip's ticks are picked and slid with the Motion Path tool's brush in `ts` (every brush). */
bool timeline_path_strip_ticks_editable(const ToolSettings &ts);
/* The selected stretch (teal) this frame, when `target` is one of those it is of; false otherwise. */
bool timeline_key_focus_span(const MotionPathTarget &target, int &r_start, int &r_end);
/* Its shade, behind the keys. */
constexpr ImU32 KEY_FOCUS_FILL = IM_COL32(0x2a, 0xb0, 0xa8, 60);
/* A tick or a blend is being dragged in the strip, or Shift is held: the wheel is the strip's, not the timeline's
 * zoom. */
bool timeline_path_strip_busy();

/* The Path Timing editor (path_timing_draw in editors.hh; panel B of the spec's speed editing) shows one stretch at a
 * time of the last thing picked on, as a graph of its timing curve (how far along its path, 0-1, against its frames,
 * the even speed dashed), with arrows to the stretches either side and the shapes in its right click menu. Under it,
 * Edit (the curve's points and handles, as a drawing's) and Sculpt (Grab and Smooth brushes, F to size them). Its view
 * zooms with the wheel, pans with a middle drag and stretches with Ctrl middle drag; Home or showing another stretch
 * fits it to the stretch. */
/* F sizing the graph's brush, as Blender's radial control: the centre one radius (`radius` frames, `per_frame`
 * pixels each) left of the mouse, so the brush keeps its size until the mouse moves; then the size the mouse's
 * distance from that centre gives. */
ImVec2 timing_resize_center(ImVec2 mouse, float radius, float per_frame);
float timing_resize_radius(ImVec2 center, ImVec2 mouse, float per_frame);

}  // namespace oanim
