#include "editors/switch_thumbnails.hh"

#include <algorithm>
#include <cfloat>
#include <map>
#include <memory>
#include <vector>

#include "imgui.h"
#include <SDL3/SDL_opengl.h>

#include "anim/deform.hh"
#include "data/document.hh"
#include "gpu/scene_renderer.hh"
#include "media/media.hh"

namespace oanim {

namespace {

struct Thumb {
  GLuint texture = 0;
  uint64_t stamp = 0;
  bool drawn = false;
};

std::map<uint64_t, Thumb> g_thumbs;
std::unique_ptr<SceneRenderer> g_renderer;
int g_budget_frame = -1;
int g_budget = 0;
/* Drawn again per frame at most: the rest wait their turn, showing their last picture. */
constexpr int PER_FRAME = 3;

uint64_t mix(uint64_t h, uint64_t v)
{
  return (h ^ v) * 0x100000001b3ull + 0x9e3779b97f4a7c15ull;
}

/* The box of everything the switch's children draw, as they are now (document space). */
bool children_bounds(const Document &doc, const Layer &sw, float2 *r_min, float2 *r_max)
{
  bool any = false;
  const auto add = [&](auto &&self, const Layer &l) -> void {
    if (l.has_shapes()) {
      const VectorDrawing drawing = layer_has_modifier_effect(doc, l, false) ? layer_evaluated_drawing(doc, l, false)
                                                                              : layer_drawing(doc, l);
      const Transform2D to_document = layer_world_transform(doc, l.id);
      for (const MeshPoint &p : drawing.points) {
        const float2 q = to_document.apply(p.pos);
        *r_min = any ? float2(std::min(r_min->x, q.x), std::min(r_min->y, q.y)) : q;
        *r_max = any ? float2(std::max(r_max->x, q.x), std::max(r_max->y, q.y)) : q;
        any = true;
      }
    }
    for (const Layer &c : l.children) {
      self(self, c);
    }
  };
  for (const Layer &c : sw.children) {
    add(add, c);
  }
  return any;
}

}  // namespace

unsigned int switch_thumbnail(Document &doc, const Layer &sw, const Layer &child, int size)
{
  Thumb &thumb = g_thumbs[child.id];
  const uint64_t stamp = mix(mix(mix(mix(revision_latest(), uint64_t(doc.frame_current)), uint64_t(size)),
                                 media_generation()),
                             sw.id);
  if (thumb.drawn && thumb.stamp == stamp) {
    return thumb.texture;
  }
  if (g_budget_frame != ImGui::GetFrameCount()) {
    g_budget_frame = ImGui::GetFrameCount();
    g_budget = PER_FRAME;
  }
  if (g_budget <= 0) {
    return thumb.texture;
  }
  g_budget--;
  float2 lo, hi;
  if (!children_bounds(doc, sw, &lo, &hi)) {
    thumb.stamp = stamp;
    thumb.drawn = true;
    return thumb.texture;
  }
  /* Square, with a margin, the switch's children all framed alike. */
  const float2 center = (lo + hi) * 0.5f;
  const float extent = std::max(std::max(hi.x - lo.x, hi.y - lo.y), 1e-3f) * 1.12f;
  const float scale = float(size) / extent;
  const Transform2D to_pixels{scale, 0, 0, scale, float(size) * 0.5f - center.x * scale, float(size) * 0.5f - center.y * scale};

  if (!g_renderer) {
    g_renderer = std::make_unique<SceneRenderer>();
  }
  g_renderer->set_show_guides(false);
  g_renderer->set_local_view(doc, {child.id});
  g_renderer->set_force_shown(doc, child.id);
  const unsigned int rendered = g_renderer->render(doc, size, size, to_pixels, 8, 4);
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  const bool read = rendered != 0 && g_renderer->read_pixels(rgba, &w, &h);
  thumb.stamp = stamp;
  thumb.drawn = true;
  if (!read) {
    return thumb.texture;
  }
  /* Straight alpha, as ImGui blends. */
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    const int a = rgba[i + 3];
    if (a > 0 && a < 255) {
      for (int k = 0; k < 3; k++) {
        rgba[i + size_t(k)] = uint8_t(std::min(255, rgba[i + size_t(k)] * 255 / a));
      }
    }
  }
  if (!thumb.texture) {
    glGenTextures(1, &thumb.texture);
  }
  glBindTexture(GL_TEXTURE_2D, thumb.texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  return thumb.texture;
}

}  // namespace oanim
