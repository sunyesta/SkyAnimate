#pragma once

/* Turns fills and strokes into triangles with a soft one-pixel edge, so they look smooth
 * without multisampling (the same approach Dear ImGui uses for its own shapes). */

#include <vector>

#include "core/math.hh"

namespace oanim {

/* Colours are premultiplied by alpha. */
struct Vertex {
  float x, y;
  float r, g, b, a;
};

/* Fills a closed polygon (even if concave). `aa` is the soft edge width in the polygon's units. */
void tessellate_fill(const std::vector<float2> &polygon, Color color, float aa, std::vector<Vertex> &r_out);

/* Strokes a polyline. `half_widths` has one entry per point. `joins` (optional, one per point)
 * says how the stroke turns there: Smooth (the default) mitres gently, Round adds a round join
 * and Sharp a mitred one, bevelled past a 4x miter limit. Ends get round caps unless their join
 * is Sharp, which ends them flat. */
void tessellate_stroke(const std::vector<float2> &points, const std::vector<float> &half_widths, bool closed,
                       Color color, float aa, std::vector<Vertex> &r_out,
                       const std::vector<StrokeJoin> *joins = nullptr);

}  // namespace oanim
