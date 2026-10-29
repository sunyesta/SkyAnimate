#pragma once

/* Turns fills and strokes into triangles with a soft one-pixel edge, so they look smooth
 * without multisampling (the same approach Dear ImGui uses for its own shapes). */

#include <vector>

#include "core/math.hh"
#include "data/fill_area.hh"

namespace oanim {

/* Colours are premultiplied by alpha. */
struct Vertex {
  float x, y;
  float r, g, b, a;
};

/* Fills a closed polygon (even if concave). `aa` is the soft edge width in the polygon's units. */
void tessellate_fill(const std::vector<float2> &polygon, Color color, float aa, std::vector<Vertex> &r_out);

/* Fills pieces with holes (data/fill_area.hh): each piece's outer ring and holes cut into triangles
 * together, the soft edge on every ring's outside (outward of the fill, into its holes). */
void tessellate_fill_pieces(const std::vector<FillPiece> &pieces, Color color, float aa, std::vector<Vertex> &r_out);

/* A fill for stencil-then-cover (no triangulating): each ring a triangle fan from the middle of the rings' bounds
 * (which keeps them mostly within a spiky outline, where a point on it would send them across it), its first vertex
 * that middle and the rest the ring, closed (`r_fan_firsts` and `r_fan_counts` say where each is); counted into the
 * stencil (one way round adding one, the other taking one) they leave each place's winding number, the fill being
 * where it isn't 0 (or, Even-Odd, where it is odd). Then the cover: the rings' bounds grown by `pad`, 6 vertices in
 * `color` (triangles), drawn where the count says. */
void tessellate_stencil_fan(const std::vector<const std::vector<float2> *> &rings, Color color, float pad,
                            std::vector<Vertex> &r_out, std::vector<int> &r_fan_firsts,
                            std::vector<int> &r_fan_counts);

/* Strokes a polyline. `half_widths` has one entry per point. `joins` (optional, one per point)
 * says how the stroke turns there: Smooth (the default) mitres gently, Round adds a round join
 * and Sharp a mitred one, bevelled past a 4x miter limit. Ends get round caps unless their join
 * is Sharp, which ends them flat. */
void tessellate_stroke(const std::vector<float2> &points, const std::vector<float> &half_widths, bool closed,
                       Color color, float aa, std::vector<Vertex> &r_out,
                       const std::vector<StrokeJoin> *joins = nullptr);

}  // namespace oanim
