#pragma once

/* The Stroke Eraser and Vector Trimmer: cutting lines where they are brushed, and out to where they cross.
 *
 * A cut splits the curve where the brush's edge meets it (an exact split, the line doesn't move) and takes the
 * pieces between: gone from a line, hidden (Moho's Hide Edge) along a fill's edge, so the fill keeps its boundary
 * and only its line goes. Nothing is rebuilt, so keys and weights stay on the points that stay; the new ends get
 * the line's round caps. On a curve that moves the cuts are riding points (data/detail.hh), and where a piece goes
 * they would be left as a line's ends, which can't ride: `free` is given them to make ordinary, keyed points of.
 * Brushing reaches a line when its edge is touched, its half width counted, so a line wider than the eraser
 * still parts and its cap ends at the eraser's edge. */

#include <cstdint>
#include <functional>
#include <vector>

#include "data/ink_stroke.hh"
#include "data/vector.hh"

namespace oanim {

/* Cuts what `sweep` covers out of the drawing's lines (`selected_only`: of the selected shapes'). `whole`: every
 * line it touches goes, all of it. False: it reached none. */
using RidingFree = std::function<void(const std::vector<uint64_t> &ids)>;
bool drawing_lines_erase(VectorDrawing &drawing, const InkSweep &sweep, float pixel, bool whole, bool selected_only,
                         const RidingFree &free);

/* The drawing's lines as polylines, to count as crossings in another drawing. */
std::vector<std::vector<float2>> drawing_line_paths(const VectorDrawing &drawing, float pixel);

/* Takes the pieces of line `sweep` touches, each out to its nearest crossings with the drawing's lines or `others`
 * (Clip Studio's Erase up to intersection), or to its end. A closed line crossing nothing goes whole. False: it
 * touched none. */
bool drawing_lines_trim(VectorDrawing &drawing, const InkSweep &sweep, const std::vector<std::vector<float2>> &others,
                        float pixel, bool selected_only, const RidingFree &free);

/* Takes the ends of curve `curve` that overshoot the last line they crossed, those no longer than `reach`. */
void drawing_trim_overshoots(VectorDrawing &drawing, int curve, float reach, float pixel);

}  // namespace oanim
