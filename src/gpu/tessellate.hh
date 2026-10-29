#pragma once

/* Turns fills and strokes into triangles with a soft one-pixel edge, so they look smooth
 * without multisampling (the same approach Dear ImGui uses for its own shapes). */

#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/math.hh"
#include "data/fill_area.hh"

namespace oanim {

/* A 16-bit float's bits (round to nearest; tiny values go to 0, huge to infinity). */
uint16_t half_from_float(float f);

/* Packed small, as the GPU reads it (20 bytes; drawings keep many). */
struct Vertex {
  float x = 0.0f, y = 0.0f;
  /* The colour, premultiplied by alpha, a byte a channel: red, green, blue, alpha from the lowest byte. */
  uint32_t rgba = 0;
  /* A colour stroke's (16-bit floats): where the vertex lies from the line's centre, and the stroke's widest half
   * width (0: not a stroke). The GPU draws a stroke thinner than half a pixel half a pixel wide, from its centre out,
   * and as much fainter, so its triangles serve every zoom. */
  uint16_t ox = 0, oy = 0, w = 0, spare = 0;

  Vertex() = default;
  /* At (x, y), in a premultiplied colour. */
  Vertex(float x_, float y_, float r, float g, float b, float a) : x(x_), y(y_), rgba(pack_rgba(r, g, b, a)) {}
  static uint32_t pack_rgba(float r, float g, float b, float a)
  {
    const auto byte = [](float v) { return uint32_t(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
    return byte(r) | byte(g) << 8 | byte(b) << 16 | byte(a) << 24;
  }
  void set_offset(float dx, float dy)
  {
    ox = half_from_float(dx);
    oy = half_from_float(dy);
  }
  /* The stroke's widest half width; one too thin for a 16-bit float kept the thinnest it has, still a stroke. */
  void set_stroke_half(float half)
  {
    w = half > 0.0f ? std::max<uint16_t>(half_from_float(half), 1) : 0;
  }
};

/* Fills a closed polygon (even if concave). `aa` is the soft edge width in the polygon's units. */
void tessellate_fill(const std::vector<float2> &polygon, Color color, float aa, std::vector<Vertex> &r_out);

/* Fills pieces with holes (data/fill_area.hh): each piece's outer ring and holes cut into triangles
 * together, the soft edge on every ring's outside (outward of the fill, into its holes). */
void tessellate_fill_pieces(const std::vector<FillPiece> &pieces, Color color, float aa, std::vector<Vertex> &r_out);

/* A fill for stencil-then-cover (no triangulating): for every edge of every ring a triangle from the middle of the
 * rings' bounds (which keeps them mostly within a spiky outline, where a point on it would send them across it), as
 * `r_indices` into the vertices (the middle first, then each ring's points: one vertex a point); counted into the
 * stencil (one way round adding one, the other taking one) they leave each place's winding number, the fill being
 * where it isn't 0 (or, Even-Odd, where it is odd). Then the cover: the rings' bounds grown by `pad`, the last 6
 * vertices, in `color` (triangles), drawn where the count says. */
void tessellate_stencil_fan(const std::vector<const std::vector<float2> *> &rings, Color color, float pad,
                            std::vector<Vertex> &r_out, std::vector<uint32_t> &r_indices);

/* Strokes a polyline (each vertex with its offset from the line and the stroke's widest half width, Vertex). `half_widths` has one entry per point. `joins` (optional, one per point)
 * says how the stroke turns there: Smooth (the default) mitres gently, Round adds a round join
 * and Sharp a mitred one, bevelled past a 4x miter limit. Ends get round caps unless their join
 * is Sharp, which ends them flat. */
void tessellate_stroke(const std::vector<float2> &points, const std::vector<float> &half_widths, bool closed,
                       Color color, float aa, std::vector<Vertex> &r_out,
                       const std::vector<StrokeJoin> *joins = nullptr);

}  // namespace oanim
