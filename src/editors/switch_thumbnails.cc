#include "editors/switch_thumbnails.hh"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <map>
#include <memory>
#include <vector>

#include "imgui.h"
#include <SDL3/SDL_opengl.h>

#include "anim/deform.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "gpu/scene_renderer.hh"
#include "media/media.hh"
#include "core/profile.hh"

namespace oanim {

namespace {

struct Thumb {
  GLuint texture = 0;
  uint64_t stamp = 0;
  bool drawn = false;
};

std::map<uint64_t, Thumb> g_thumbs;
std::unique_ptr<SceneRenderer> g_renderer;
bool g_hold = false;
int g_budget_frame = -1;
int g_budget = 0;
/* Drawn again per frame at most: the rest wait their turn, showing their last picture. */
constexpr int PER_FRAME = 2;
/* Seconds the document stays unchanged before pictures out of date are drawn again. */
constexpr double QUIET = 0.3;

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

uint64_t mix(uint64_t h, const Transform2D &t)
{
  for (const float v : {t.a, t.b, t.c, t.d, t.tx, t.ty}) {
    h = mix(h, uint64_t(std::bit_cast<uint32_t>(v)));
  }
  return h;
}

/* What the child's picture is drawn from: its layers as they are (their revisions), where the switch is, and
 * the poses of the bones that move them. Not the whole document's latest revision: picking a bone or any other
 * edit elsewhere would draw every thumbnail again. */
uint64_t content_stamp(const Document &doc, const Layer &sw, const Layer &child)
{
  uint64_t h = mix(0, layer_world_transform(doc, sw.id));
  const auto pose = [&](const Layer &armature) {
    for (const BonePose &p : armature_pose(armature, doc.frame_current == 0)) {
      h = mix(mix(h, p.id), p.skin);
    }
  };
  if (const Layer *armature = layer_armature(doc, sw.id)) {
    pose(*armature);
  }
  const auto walk = [&](auto &&self, const Layer &l) -> void {
    h = mix(h, l.revision.value);
    if (l.is_bone_layer()) {
      pose(l);
    }
    for (const Layer &c : l.children) {
      self(self, c);
    }
  };
  walk(walk, child);
  return h;
}

}  // namespace

void switch_thumbnails_hold(bool hold)
{
  g_hold = hold;
}

unsigned int switch_thumbnail(Document &doc, const Layer &sw, const Layer &child, int size)
{
  OANIM_PROFILE("switch_thumbnail");
  Thumb &thumb = g_thumbs[child.id];
  if (thumb.drawn && g_hold) {
    return thumb.texture;
  }
  const uint64_t stamp = mix(mix(mix(mix(content_stamp(doc, sw, child), uint64_t(doc.frame_current)), uint64_t(size)),
                                 media_generation()),
                             sw.id);
  if (thumb.drawn && thumb.stamp == stamp) {
    return thumb.texture;
  }
  /* Drawn again once the document has been still a moment: a click or an undo can touch every layer, and drawing a
   * big switch's pictures again (each read back from the GPU) right then makes the click itself slow. */
  static uint64_t last_latest = 0;
  static double changed_at = 0.0;
  const double now = ImGui::GetTime();
  if (revision_latest() != last_latest) {
    last_latest = revision_latest();
    changed_at = now;
  }
  if (thumb.drawn && now - changed_at < QUIET) {
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
  /* The box is the same for every child of the switch: worked out once for this state of the document (a big
   * switch's children are thousands of layers), with each layer and bone pose found once. */
  static struct {
    uint64_t sw = 0, stamp = 0;
    bool any = false;
    float2 lo, hi;
  } box;
  const uint64_t box_stamp = mix(mix(revision_latest(), uint64_t(doc.frame_current)), media_generation());
  if (box.sw != sw.id || box.stamp != box_stamp) {
    const LayerIndexScope layer_index(doc);
    const ArmaturePoseCacheScope poses;
    box.sw = sw.id;
    box.stamp = box_stamp;
    box.any = children_bounds(doc, sw, &box.lo, &box.hi);
  }
  const float2 lo = box.lo, hi = box.hi;
  if (!box.any) {
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
