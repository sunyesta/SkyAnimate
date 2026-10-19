#pragma once

/* Paints a line with a brush: stamps ("dabs") of the brush's images placed along it at the
 * brush's spacing, turned, sized and coloured by its settings, as Moho draws brushed lines. */

#include <cstdint>
#include <vector>

#include "core/math.hh"
#include "data/vector.hh"

namespace sky {

struct BrushVertex {
  float x, y;
  float u, v;
  /* Premultiplied. */
  float r, g, b, a;
};

struct BrushDabs {
  /* Two triangles per dab, grouped by the brush image they use (one list per image). */
  std::vector<std::vector<BrushVertex>> per_image;
  int count = 0;
};

/* Dabs along a polyline. `half_widths` has one entry per point (the line's half width there);
 * `aspects` one entry per image (width / height). `min_step` keeps dabs at least that far apart
 * (half a pixel), so a very thin line doesn't turn into millions of them. `seed` picks the
 * random pattern: the same seed gives the same line. */
void brush_stroke(const std::vector<float2> &points, const std::vector<float> &half_widths, bool closed,
                  const BrushSettings &brush, Color color, const std::vector<float> &aspects, float min_step,
                  uint32_t seed, BrushDabs &r_dabs);

/* The seed for one shape's line: Line boil changes it every `random_interval` frames. */
uint32_t brush_seed(const BrushSettings &brush, uint64_t layer_id, int shape_index, int frame);

}  // namespace sky
