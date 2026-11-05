#pragma once

/* The viewport's edit overlays for drawings (wires, handles, point squares) on the GPU, as Blender's overlay
 * engine draws them: each layer's are kept in buffers on the graphics card, in the layer's own space, and
 * made again only when what they show changes (a fingerprint the viewport works out); panning, zooming
 * and moving a layer whole only change the matrix they are drawn with. Lines are widened to their pixel
 * width and marks shaped (squares, round dots) in the shaders, with soft edges, so nothing is built per
 * frame on the CPU. They draw through one ImGui callback, where ImGui drawing them would have gone. */

#include <cstdint>
#include <vector>

#include "core/math.hh"

struct ImDrawList;

namespace oanim {

/* A piece of line from `a` to `b` (layer space), `width` pixels wide; colours are ImGui's (IM_COL32). `color_b`, when
 * set, is its colour at `b`, blending from `color` at `a` (a wire fading from a picked point). */
struct OverlayLine {
  float2 a, b;
  uint32_t color = 0;
  float width = 1.0f;
  uint32_t color_b = 0;
};

/* A point square or a round handle dot at `at` (layer space), `radius` pixels from its middle to its edge.
 * Squares draw their outline just inside their edge, dots across it (as ImGui's rect and circle). */
struct OverlayMark {
  float2 at;
  uint32_t fill = 0;
  uint32_t outline = 0;
  float radius = 3.0f;
  float round = 0.0f;
};

struct OverlayGeometry {
  std::vector<OverlayLine> lines;
  std::vector<OverlayMark> marks;
};

/* Whether `layer`'s overlays are kept as made under `key`. */
bool overlay_kept(uint64_t layer, uint64_t key);
/* Keeps (uploads) `layer`'s overlays, made under `key` with its first point at `origin` (layer space). */
void overlay_keep(uint64_t layer, uint64_t key, const OverlayGeometry &geometry, float2 origin);
/* Where the kept overlays' first point was (a layer moved whole draws them moved by the difference). */
float2 overlay_kept_origin(uint64_t layer);
/* Draws `layer`'s kept overlays this frame with `to_screen` (layer space to ImGui's display coordinates),
 * in the order queued, at the next overlay_flush. */
void overlay_queue(uint64_t layer, const Transform2D &to_screen);
/* Puts what was queued into `dl` (one callback), where it should draw. */
void overlay_flush(ImDrawList *dl);
/* Lets go of overlays no frame has drawn for a while (layers no longer edited). */
void overlay_collect();

}  // namespace oanim
