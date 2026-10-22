#pragma once

/* Paints a line with a brush: stamps ("dabs") of the brush's images placed along it at the
 * brush's spacing, turned, sized and coloured by its settings, as Moho draws brushed lines.
 *
 * The dabs stick to the artwork. They are laid out once along the line as drawn on the setup
 * frame (its "rest" shape), each at a fraction of one of its segments, and put at the same
 * fraction of the same segment on every frame: when points move or bones bend the line, the dabs
 * move with it, like a texture printed on it, instead of sliding along it (as dabs laid out from
 * the start of the line do whenever its length changes). A segment stretched to twice its rest
 * length or more fades in dabs between its own (more at four times, ...), and one squashed to
 * half or less fades out every other one, so a stretched line doesn't break into dots nor a
 * squashed one clot, without anything sliding (after Bénard et al.'s self-similar line textures).
 * With the brush's Pop In they appear and go whole instead of fading.
 * The layout doesn't depend on the zoom, so the viewport and a render agree. */

#include <cstdint>
#include <functional>
#include <vector>

#include "core/math.hh"
#include "data/vector.hh"

namespace oanim {

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
  /* Each dab's middle and how much of it shows (faded in or out), in the order laid down. */
  std::vector<float2> centers;
  std::vector<float> alphas;
};

/* A brushed line's path: its segments end to end, each a Bezier (layer space) with the line's
 * half width at either end and whether it shows (a hidden edge has no dabs). */
struct BrushPath {
  struct Edge {
    float2 ctrl[4];
    float half0 = 0.0f, half1 = 0.0f;
    bool visible = true;
  };
  std::vector<Edge> edges;
  bool closed = false;
};

/* A shape's outline as a brush path, its half widths from the shape's line width and its points'
 * radii, plus `extra_half` (a mask's one-pixel rim). Empty if its edges don't join up. */
BrushPath brush_path(const VectorDrawing &drawing, const Shape &shape, float extra_half);

/* A mask's End Lines at Edge for a brushed line: whether the line shows at a point (layer space),
 * and whether the ends it makes get the brush's taper and fade (Ends: Brush) or none (Flat). */
struct BrushCut {
  std::function<bool(float2)> shows;
  bool taper = true;
  /* With taper (Ends: Brush): how far the line reaches past the edge, in line widths (0: its last
   * dab just touches it; 0.5: centred on it). */
  float reach = 0.0f;
};

/* Dabs along `shown` (the line on the frame shown), laid out on `rest` (the same line on the
 * setup frame: the same segments, in the same order; `shown` itself when there is none).
 * `aspects`: one entry per image (width / height). `min_half` keeps a dab at least that big (half
 * a pixel), so a hairline still shows; with `fade_thin` a dab that thin shows only as much as it is
 * thick (a line half a pixel wide is drawn a pixel wide at half strength), so a line keeps its
 * weight however far the view is zoomed out. `seed` picks the random pattern: the same seed gives
 * the same line. `cut` (optional): no dabs where it doesn't show, and the ends that makes tapered
 * and faded as the brush's own ends are. */
void brush_stroke(const BrushPath &shown, const BrushPath &rest, const BrushSettings &brush, Color color,
                  const std::vector<float> &aspects, float min_half, uint32_t seed, BrushDabs &r_dabs,
                  const BrushCut *cut = nullptr, bool fade_thin = false);

/* The seed for one shape's line: Line boil changes it every `random_interval` frames. */
uint32_t brush_seed(const BrushSettings &brush, uint64_t layer_id, int shape_index, int frame);

}  // namespace oanim
