#include "gpu/scene_renderer.hh"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <functional>
#include <string>

#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "anim/dynamics.hh"
#include "anim/layer_pick.hh"
#include "assets/brushes.hh"
#include "core/log.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "data/media_layer.hh"
#include "core/parallel.hh"
#include "gpu/brush_stroke.hh"
#include "gpu/tessellate.hh"
#include "media/media.hh"

namespace oanim {

/* How wide a band of zooms a colour drawing made in the viewport serves (draw_drawing), in doublings. */
static constexpr float ZOOM_BAND_OCTAVES = 4.0f;


/* Render statistics: what the render in progress has spent so far (making drawings runs on many threads at once). */
static std::atomic<bool> g_stats_on{false};
enum Stat { STAT_MAKE, STAT_MAKE_MASK, STAT_OUTLINE, STAT_EARCUT, STAT_FAN, STAT_STROKE, STAT_EVALUATE, STAT_COUNT };
static std::atomic<long long> g_stat_ns[STAT_COUNT];
static std::atomic<int> g_stat_made{0};
static std::mutex g_stats_mutex;
static std::vector<RenderStats> g_stats;

/* Adds the time it is in scope to `stat`, when statistics are on (and `when`). */
struct StatTimer {
  Stat stat;
  bool on;
  std::chrono::steady_clock::time_point start;
  explicit StatTimer(Stat s, bool when = true)
      : stat(s),
        on(when && g_stats_on.load(std::memory_order_relaxed)),
        start(on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point())
  {
  }
  ~StatTimer()
  {
    if (on) {
      g_stat_ns[stat] += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                             .count();
    }
  }
};

void render_stats_set(bool on)
{
  g_stats_on = on;
}

bool render_stats_on()
{
  return g_stats_on;
}

std::vector<RenderStats> render_stats_take()
{
  const std::lock_guard<std::mutex> lock(g_stats_mutex);
  return std::exchange(g_stats, {});
}

SceneRenderer::~SceneRenderer()
{
  /* The device frees what it holds on the GPU. */
  for (LayerRun &run : runs_) {
    delete run.image;
  }
  for (Target *target : targets_) {
    delete target;
  }
}

bool SceneRenderer::init()
{
  if (initialized_ || failed_) {
    return initialized_;
  }
  device_ = gpu::make_gl_device();
  if (!device_->init()) {
    failed_ = true;
    return false;
  }
  initialized_ = true;
  return true;
}

/* The stencil bits as the GPU holds them: the masks' bits (below) the other way round, so that their levels (bits 0 to
 * 3) are its top four bits and the scratch they are worked out in (4 to 7) its bottom four, which a fill's winding is
 * counted in while colour is drawn (counting carries upwards, so it needs the bottom bits). Masks only test and set
 * bits, so which is which changes nothing for them. */
static unsigned stencil_bits(unsigned bits)
{
  unsigned out = 0;
  for (int i = 0; i < 8; i++) {
    out |= ((bits >> i) & 1u) << (7 - i);
  }
  return out;
}

/* Depth masks (GPU Geometry): the depth a mask level's samples hold, and the depth just short of it that a draw
 * tests against, so that it passes at that level or deeper whatever the rounding. Cleared, everything is level 0. */
static constexpr unsigned DEPTH_LEVELS_MAX = 1000;
static float level_depth(unsigned level)
{
  return float(level) / 1024.0f;
}
static float short_of_level(unsigned level)
{
  return (float(level) - 0.5f) / 1024.0f;
}

/* Colour drawn where every bit of `required` is set in the stencil, which is left as it is, and inside the first
 * `depth_levels` of the depth's mask levels. */
static gpu::DrawState color_state(unsigned required, unsigned depth_levels = 0)
{
  gpu::DrawState state;
  if (required != 0) {
    state.stencil.compare = gpu::Compare::Equal;
    state.stencil.ref = stencil_bits(required);
    state.stencil.read_mask = stencil_bits(required);
  }
  if (depth_levels != 0) {
    state.depth = short_of_level(depth_levels);
    state.depth_test = gpu::Compare::LessEqual;
  }
  return state;
}

/* The stencil bits stencil-then-cover fills count winding in: the masks' scratch (MASK_SPARE to MASK_NESTED_SCRATCH),
 * free while colour is drawn, the masks being worked out by then. The last is the count's lowest bit (odd or even). */
static constexpr unsigned WINDING_BITS = 0xF0u;
static constexpr unsigned WINDING_ODD = 1u << 7;

/* No colour; the stencil's `write` bits set to `ref`'s where the test passes (and by `fail` where not). */
static gpu::DrawState stencil_state(gpu::Compare compare, unsigned ref, unsigned read_mask, unsigned write,
                                    gpu::StencilOp fail = gpu::StencilOp::Keep)
{
  gpu::DrawState state;
  state.program = gpu::Program::Stencil;
  state.write_color = false;
  state.stencil = {true,
                   compare,
                   stencil_bits(ref),
                   stencil_bits(read_mask),
                   fail,
                   gpu::StencilOp::Replace,
                   stencil_bits(write)};
  return state;
}

SceneRenderer::Target *SceneRenderer::acquire_target()
{
  flush_batch();
  for (Target *target : targets_) {
    if (!target->in_use) {
      target->in_use = true;
      return target;
    }
  }
  Target *target = new Target;
  gpu::TargetDesc desc;
  desc.width = width_;
  desc.height = height_;
  desc.samples = target_samples_;
  target->target = device_->make_target(desc);
  target->in_use = true;
  targets_.push_back(target);
  return target;
}

void SceneRenderer::release_target(Target *target)
{
  target->in_use = false;
}

void SceneRenderer::free_targets()
{
  for (Target *target : targets_) {
    device_->free_target(target->target);
    delete target;
  }
  targets_.clear();
  output_ = nullptr;
}

void SceneRenderer::bind_target(const Target *target)
{
  flush_batch();
  device_->bind_target(target->target);
  bound_ = target;
}

void SceneRenderer::resolve(const Target *target)
{
  flush_batch();
  device_->resolve(target->target);
}

void SceneRenderer::clear(const Color &color)
{
  flush_batch();
  const float premultiplied[4] = {color.r * color.a, color.g * color.a, color.b * color.a, color.a};
  device_->clear(premultiplied);
  bound_->fan_dirty = false;
  bound_->depth_levels.clear();
}

void SceneRenderer::clear_within(const PixelRect &rect, const Color &color)
{
  flush_batch();
  scissor(rect);
  clear(color);
  scissor(PixelRect());
}

unsigned int SceneRenderer::render(const Document &doc, int width, int height, const Transform2D &document_to_pixels,
                                   int quality, int samples, const Color *background)
{
  const auto started = std::chrono::steady_clock::now();
  const bool stats = g_stats_on;
  if (stats) {
    for (auto &ns : g_stat_ns) {
      ns = 0;
    }
    g_stat_made = 0;
  }
  /* Every layer a rig moves asks for its pose: worked out once for the whole render. */
  const ArmaturePoseCacheScope pose_cache;
  /* Every layer drawn looks up its parents, masks and bones: by an index, not a search of the tree. */
  const LayerIndexScope layer_index(doc);
  quality_ = std::clamp(quality, 1, 64);
  if (!init() || width <= 0 || height <= 0) {
    return 0;
  }
  /* The viewport's margin: the image is drawn that much bigger all round, the part asked for in its middle. A render
   * asked for within the last one's margin (a pan) draws in the last one's place, so the layer runs' images, made in
   * it, hold. */
  asked_width_ = width;
  asked_height_ = height;
  Transform2D to_pixels = document_to_pixels;
  if (overscan_ && background == nullptr) {
    const int margin = std::max(width, height) / 8;
    const bool kept = margin == margin_ && width + 2 * margin == width_ && height + 2 * margin == height_ &&
                      pan_to(width, height, document_to_pixels);
    if (!kept) {
      margin_ = margin;
      to_pixels = Transform2D::translate({float(margin), float(margin)}) * document_to_pixels;
      uv_[0] = float(margin) / float(width + 2 * margin);
      uv_[1] = float(margin) / float(height + 2 * margin);
      uv_[2] = float(margin + width) / float(width + 2 * margin);
      uv_[3] = float(margin + height) / float(height + 2 * margin);
    }
    else {
      to_pixels = document_to_pixels_;
    }
    width += 2 * margin_;
    height += 2 * margin_;
  }
  else {
    margin_ = 0;
    uv_[0] = uv_[1] = 0.0f;
    uv_[2] = uv_[3] = 1.0f;
  }
  /* 1, 2, 4, 8 or 16 samples, no more than the GPU offers. */
  int use = 1;
  while (use * 2 <= std::min({samples, device_->caps().max_samples, 16})) {
    use *= 2;
  }
  samples_ = use;
  /* Offscreen images all match the image size and sample count; rebuild them when they change. */
  if (width != width_ || height != height_ || use != target_samples_) {
    free_runs();
    free_targets();
    width_ = width;
    height_ = height;
    target_samples_ = use;
  }
  for (Target *target : targets_) {
    target->in_use = false;
  }
  document_to_pixels_ = to_pixels;
  final_render_ = background != nullptr;
  gpu_geometry_ = doc.shading.gpu_geometry;
  required_stencil_ = 0;
  depth_required_ = 0;
  line_free_stencil_ = 0;
  fill_free_stencil_ = 0;
  mask_clauses_.clear();
  coverage_test_ = 0;
  mask_pass_ = {};

  device_->set_timing(stats);
  device_->begin();
  paint_ = color_state(0);

  /* Only the view changed since the last render (the caller's stamp of everything else the same, and the settings):
   * every drawing's evaluation and its key's view-independent part are kept from it. Otherwise worked out anew. */
  const auto mix = [](uint64_t h, uint64_t v) { return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2))); };
  const uint64_t memo_stamp =
      content_stamp_ == 0 ? 0
                          : mix(mix(content_stamp_, uint64_t(quality_) | uint64_t(samples_) << 8 |
                                                        uint64_t(gpu_geometry_) << 16 | uint64_t(final_render_) << 17),
                                mix(mask_preview_layer_, brushes_generation())) | 1;
  content_stamp_ = 0;
  if (memo_stamp == 0 || memo_stamp != memo_stamp_) {
    evaluated_.clear();
    key_memo_.clear();
  }
  memo_stamp_ = memo_stamp;
  drawn_.clear();
  regions_.clear();
  deferred_.clear();
  drawn_signature_.clear();
  signature_memo_.clear();
  complete_ = true;
  render_serial_++;
  render_start_ = std::chrono::steady_clock::now();
  output_ = acquire_target();
  bind_target(output_);
  clear(background ? Color{background->r, background->g, background->b, 1.0f} : Color{0, 0, 0, 0});
  const bool in_runs = interactive_ && !final_render_ && device_->caps().sample_shading && samples_ > 1 &&
                      mask_preview_layer_ == 0 && layer_runs_ && device_->has_sample_texture(output_->target);
  std::vector<DrawStep> steps;
  std::vector<char> clean;
  skip_prepare_.clear();
  if (in_runs) {
    steps = draw_order_steps(doc, doc.layers, [this](const Layer &l) { return shown(l); });
    clean = clean_steps(doc, steps, runs_view());
    for (size_t i = 0; i < steps.size(); i++) {
      if (clean[i]) {
        skip_prepare_.insert(steps[i].layer->id);
      }
    }
  }
  const auto prepare_start = std::chrono::steady_clock::now();
  prepare_drawings(doc);
  const double prepare_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepare_start).count();
  /* What's left to make one at a time (brushed lines, masks' shapes) has its own time: only making counts, so each
   * render makes some however long drawing the rest takes. */
  made_ms_ = 0.0;
  if (in_runs) {
    draw_layers_in_runs(doc, steps, clean);
  }
  else {
    free_runs();
    last_layers_.clear();
    draw_layers(doc, doc.layers, Transform2D(), output_, 1.0f);
  }
  /* Media layers In Front: over everything, unmasked. */
  required_stencil_ = 0;
  depth_required_ = 0;
  for (const Layer *l : document_layers_flat(doc)) {
    if (l->is_media() && l->media.in_front && layer_shown_through_parents(doc, *l)) {
      use_color_stencil();
      draw_media(doc, *l, layer_world_transform(doc, l->id),
                 full_opacity(*l) ? 1.0f : layer_opacity_through_parents(doc, *l));
    }
  }
  if (const Layer *previewed = background ? nullptr : layer_find(doc, mask_preview_layer_)) {
    draw_mask_preview(doc, *previewed);
  }
  resolve(output_);
  device_->end();
  if (stats) {
    RenderStats r;
    r.width = width_;
    r.height = height_;
    r.final_render = final_render_;
    r.cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    r.prepare_ms = prepare_ms;
    const auto ms = [](Stat s) { return double(g_stat_ns[s].load()) / 1e6; };
    r.evaluate_ms = ms(STAT_EVALUATE);
    r.make_ms = ms(STAT_MAKE);
    r.mask_make_ms = ms(STAT_MAKE_MASK);
    r.outline_ms = ms(STAT_OUTLINE);
    r.earcut_ms = ms(STAT_EARCUT);
    r.fan_ms = ms(STAT_FAN);
    r.stroke_ms = ms(STAT_STROKE);
    r.drawings_made = g_stat_made;
    r.kinds = device_->timings();
    for (const gpu::Timing &t : r.kinds) {
      r.gpu_ms += t.gpu_ms;
    }
    const std::lock_guard<std::mutex> lock(g_stats_mutex);
    if (g_stats.size() >= 10000) {
      g_stats.erase(g_stats.begin());
    }
    g_stats.push_back(std::move(r));
  }
  /* Drawings no render has used for a while (another zoom, a drawing since changed) go, with their
   * buffers. */
  std::erase_if(chunks_, [this](auto &item) {
    if (item.second.used + 120 >= render_serial_) {
      return false;
    }
    device_->free_mesh(item.second.mesh);
    return true;
  });
  std::erase_if(shapes_kept_, [this](auto &item) { return item.second.used + 120 < render_serial_; });
  std::erase_if(tessellated_, [this](auto &item) {
    if (item.second.used + 120 >= render_serial_) {
      return false;
    }
    for (PaintStep &step : item.second.steps) {
      free_step(step);
    }
    return true;
  });
  return device_->native_texture(output_->target);
}

bool SceneRenderer::read_pixels(std::vector<uint8_t> &r_rgba, int *r_width, int *r_height)
{
  flush_batch();
  if (!output_) {
    return false;
  }
  const size_t row = size_t(width_) * 4;
  std::vector<uint8_t> bottom_up;
  device_->read_pixels(output_->target, bottom_up);
  r_rgba.resize(bottom_up.size());
  for (int y = 0; y < height_; y++) {
    std::copy_n(bottom_up.data() + size_t(height_ - 1 - y) * row, row, r_rgba.data() + size_t(y) * row);
  }
  *r_width = width_;
  *r_height = height_;
  return true;
}

void SceneRenderer::use_color_stencil(Paint paint)
{
  const unsigned required = paint == Paint::Line ? required_stencil_ & ~line_free_stencil_
                                                 : required_stencil_ & ~fill_free_stencil_;
  paint_ = color_state(required, depth_required_);
}

void SceneRenderer::stencil_fill(unsigned bit, bool set)
{
  if (dry_masks_) {
    return;
  }
  flush_batch();
  gpu::Draw draw;
  draw.state = stencil_state(gpu::Compare::Always, set ? bit : 0u, bit, bit, gpu::StencilOp::Replace);
  draw.count = 3;
  device_->draw(draw);
  if (bit & WINDING_BITS) {
    bound_->fan_dirty = set || (bound_->fan_dirty && (bit & WINDING_BITS) != WINDING_BITS);
  }
}

std::set<uint64_t> local_view_layers(const Document &doc, const std::vector<uint64_t> &ids)
{
  std::set<uint64_t> local_view_;
  for (const uint64_t id : ids) {
    const Layer *layer = layer_find(doc, id);
    if (!layer) {
      continue;
    }
    const auto inside = [&](auto &&self, const Layer &l) -> void {
      local_view_.insert(l.id);
      for (const Layer &child : l.children) {
        self(self, child);
      }
    };
    inside(inside, *layer);
    for (const Layer *p = layer_parent(const_cast<Document &>(doc), id); p; p = layer_parent(const_cast<Document &>(doc), p->id)) {
      local_view_.insert(p->id);
    }
  }
  return local_view_;
}

void SceneRenderer::set_local_view(const Document &doc, const std::vector<uint64_t> &ids)
{
  local_view_ = local_view_layers(doc, ids);
}

bool SceneRenderer::shown(const Layer &layer) const
{
  if (!final_render_ && !local_view_.empty() && !local_view_.contains(layer.id)) {
    return false;
  }
  /* A layer not rendered is a guide: in the viewport only while it shows guides (its overlays), or a
   * media one in Media View. */
  const bool media_view = media_view_ && !final_render_ && layer.is_media();
  return layer.visible && (layer.render_visible || (!final_render_ && show_guides_) || media_view);
}

bool SceneRenderer::full_opacity(const Layer &layer) const
{
  return media_view_ && !final_render_ && layer.is_media();
}

bool SceneRenderer::layer_shown_through_parents(const Document &doc, const Layer &layer) const
{
  for (const Layer *l = &layer; l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    if (!shown(*l) || (l->opacity <= 0.0f && !full_opacity(*l))) {
      return false;
    }
  }
  return true;
}

float SceneRenderer::layer_opacity_through_parents(const Document &doc, const Layer &layer) const
{
  float opacity = 1.0f;
  for (const Layer *l = &layer; l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    opacity *= l->opacity;
  }
  return opacity;
}

void SceneRenderer::draw_layers(const Document &doc, const std::vector<Layer> &layers, const Transform2D & /*parent*/,
                                Target *target, float opacity)
{
  /* Bottom first, in Draw Order, each with the masks of it and the groups holding it. */
  const std::vector<DrawStep> steps = draw_order_steps(doc, layers, [this](const Layer &l) { return shown(l); });
  const unsigned outer_required = required_stencil_, outer_line_free = line_free_stencil_,
                 outer_fill_free = fill_free_stencil_, outer_depth = depth_required_;
  for (const DrawStep &step : steps) {
    required_stencil_ = outer_required | apply_masks(doc, step.masked);
    depth_required_ = std::max(outer_depth, applied_depth_);
    line_free_stencil_ = outer_line_free | applied_line_free_;
    fill_free_stencil_ = outer_fill_free | applied_fill_free_;
    draw_layer(doc, *step.layer, Transform2D(), target, opacity);
  }
  required_stencil_ = outer_required;
  depth_required_ = outer_depth;
  line_free_stencil_ = outer_line_free;
  fill_free_stencil_ = outer_fill_free;
}

void SceneRenderer::draw_step(const Document &doc, const DrawStep &step, Target *target)
{
  required_stencil_ = apply_masks(doc, step.masked);
  depth_required_ = applied_depth_;
  line_free_stencil_ = applied_line_free_;
  fill_free_stencil_ = applied_fill_free_;
  draw_layer(doc, *step.layer, Transform2D(), target, 1.0f);
  required_stencil_ = 0;
  depth_required_ = 0;
  line_free_stencil_ = 0;
  fill_free_stencil_ = 0;
}

void SceneRenderer::draw_layer(const Document &doc, const Layer &layer, const Transform2D & /*parent*/, Target *target,
                               float opacity)
{
  if (!shown(layer) || (layer.opacity <= 0.0f && !full_opacity(layer))) {
    return;
  }
  /* Where the layer is, bones included: a Parent Bone (its own, or a group's above it) moves it,
   * as everything else that places layers sees it. */
  const Transform2D transform = layer_world_transform(doc, layer.id);
  const float layer_opacity = opacity * layer.opacity;

  if (layer.is_media()) {
    if (!layer.media.in_front) {
      use_color_stencil();
      draw_media(doc, layer, transform, full_opacity(layer) ? 1.0f : layer_opacity);
    }
    return;
  }
  if (!layer.is_group()) {
    if (fade_whole(layer, layer_opacity)) {
      draw_faded_drawing(doc, layer, transform, target, layer_opacity);
      return;
    }
    use_color_stencil();
    draw_drawing(doc, layer, transform, layer_opacity);
    return;
  }
  if (layer.opacity < 1.0f) {
    /* Fade the group as a whole, so its overlapping children don't show through each other. Its
     * own image has its own stencil for the masks inside it; the masks it has (and those of the
     * groups holding it) clip it when it is composited back. */
    Target *group = acquire_target();
    bind_target(group);
    /* Laid back over sample for sample where it draws, where that is known (below): only that much of its image
     * cleared (what it holds elsewhere is never read); otherwise resolved and laid over whole. */
    const PixelRect r = layer_region(doc, layer);
    const bool per_sample = !r.whole && device_->caps().sample_shading && device_->has_sample_texture(group->target);
    clear_within(per_sample ? r : PixelRect(), Color{0, 0, 0, 0});
    const unsigned outer_required = required_stencil_, outer_line_free = line_free_stencil_,
                   outer_fill_free = fill_free_stencil_, outer_depth = depth_required_;
    const HeldMasks outer_clauses = std::move(mask_clauses_);
    mask_clauses_.clear();
    required_stencil_ = 0;
    depth_required_ = 0;
    line_free_stencil_ = 0;
    fill_free_stencil_ = 0;
    draw_layers(doc, layer.children, transform, group, 1.0f);
    required_stencil_ = outer_required;
    depth_required_ = outer_depth;
    line_free_stencil_ = outer_line_free;
    fill_free_stencil_ = outer_fill_free;
    mask_clauses_ = outer_clauses;
    bind_target(target);
    use_color_stencil();
    /* Laid over sample for sample where it draws (no image resolved, nothing laid over the rest), where that is
     * known; otherwise resolved and laid over the whole image. */
    if (per_sample) {
      lay_samples(group, layer_opacity,
                  float2(float(r.x0) / float(width_) * 2.0f - 1.0f, float(r.y0) / float(height_) * 2.0f - 1.0f),
                  float2(float(r.x1) / float(width_) * 2.0f - 1.0f, float(r.y1) / float(height_) * 2.0f - 1.0f));
    }
    else {
      resolve(group);
      bind_target(target);
      use_color_stencil();
      composite(group, layer_opacity);
    }
    release_target(group);
    return;
  }
  draw_layers(doc, layer.children, transform, target, layer_opacity);
}

/* A layer's mask cards that are on (in renders or the viewport): each its steps, with the layers
 * still there (a step left with none is left out, and so is a card left with none). Clip to Below
 * comes in as a card (layer_mask_cards), first. */
struct CardRows {
  std::vector<SceneRenderer::MaskRow> steps;
  MaskCuts cuts = MaskCuts::All;
  /* End Lines at Edge with Ends: Brush (the lines it ends aren't sliced by it). */
  bool brush_ends = false;
  /* Its place among the layer's cards. */
  int index = 0;
};
static std::vector<CardRows> cards_of(const Document &doc, const Layer &layer, bool render)
{
  std::vector<CardRows> cards;
  int index = -1;
  for (const LayerMask &m : layer_mask_cards(doc, layer)) {
    index++;
    if (!(render ? m.render : m.enabled)) {
      continue;
    }
    CardRows card;
    card.cuts = m.clip ? MaskCuts::All : m.cuts;
    bool flat = false;
    card.brush_ends = mask_card_ends_lines(m, &flat) && !flat;
    card.index = index;
    for (const MaskStep &s : m.steps) {
      SceneRenderer::MaskRow row;
      row.mode = s.mode;
      row.invert = s.invert;
      row.combine = s.combine;
      row.grow = s.grow;
      row.fills_only = s.fills_only;
      row.strokes_only = s.strokes_only;
      row.inside_stroke = s.fills_only && s.inside_stroke;
      row.filled = s.filled;
      row.extend = int(s.extend);
      row.clip = s.clip;
      row.start_shown = m.start_shown;
      for (const uint64_t id : s.layers) {
        if (id != 0 && id != layer.id && layer_find(doc, id)) {
          row.layers.push_back(id);
        }
      }
      if (!row.layers.empty()) {
        card.steps.push_back(std::move(row));
      }
    }
    if (!card.steps.empty()) {
      cards.push_back(std::move(card));
    }
  }
  std::stable_partition(cards.begin(), cards.end(), [](const CardRows &c) { return c.steps.front().clip; });
  return cards;
}

/* A layer's cards (cards_of), worked out again only once a revision has been given since (see Revision): they
 * depend on other layers too (Clip to Below, the layers rows name), so any change at all may change them. */
static const std::vector<CardRows> &kept_cards(const Document &doc, const Layer &layer, bool render)
{
  struct Kept {
    uint64_t latest = ~uint64_t(0);
    const Document *doc = nullptr;
    std::vector<CardRows> cards;
  };
  static std::unordered_map<uint64_t, Kept> kept;
  Kept &k = kept[layer.id << 1 | (render ? 1 : 0)];
  if (k.latest != revision_latest() || k.doc != &doc) {
    k.cards = cards_of(doc, layer, render);
    k.latest = revision_latest();
    k.doc = &doc;
  }
  return k.cards;
}

/* Stencil bits: one result per masked level (0 to 3), a spare for combining a clip base's levels (4),
 * the scratch a row's shape goes in (5), and the same two for a mask layer's own rows (6, 7). */
static constexpr int MASK_LEVELS_MAX = 4;
static constexpr unsigned MASK_SPARE = 1u << 4;
static constexpr unsigned MASK_SCRATCH = 1u << 5;
static constexpr unsigned MASK_NESTED = 1u << 6;
static constexpr unsigned MASK_NESTED_SCRATCH = 1u << 7;
static_assert(WINDING_BITS == (MASK_SPARE | MASK_SCRATCH | MASK_NESTED | MASK_NESTED_SCRATCH),
              "fills count winding only in the bits masks are worked out in");

unsigned SceneRenderer::apply_masks(const Document &doc, const std::vector<const Layer *> &masked)
{
  MaskLevels levels;
  applied_line_free_ = 0;
  applied_fill_free_ = 0;
  applied_depth_ = 0;
  /* GPU Geometry: the levels cutting fills and lines alike, for the depth (no limit but the depth's); the stencil
   * keeps the others, one bit of its four for working a depth level out in. */
  MaskLevels depth;
  std::vector<PixelRect> level_regions, depth_regions;
  const int stencil_max = gpu_geometry_ ? MASK_LEVELS_MAX - 1 : MASK_LEVELS_MAX;
  for (const Layer *l : masked) {
    const PixelRect region = layer_region(doc, *l);
    /* Its cards by what they cut: fills and lines (sliced), fills only (lines left alone or ended with
     * a brush), lines only (sliced); each kind a level its cards must all show in. */
    std::vector<std::vector<MaskRow>> both, fills, lines;
    for (const CardRows &card : kept_cards(doc, *l, final_render_)) {
      const bool f = card.cuts != MaskCuts::Lines;
      const bool lp = card.cuts != MaskCuts::Fills && !card.brush_ends;
      if (f || lp) {
        (f && lp ? both : f ? fills : lines).push_back(card.steps);
      }
    }
    const auto push = [&](std::vector<std::vector<MaskRow>> &cards, bool line_free, bool fill_free) {
      if (gpu_geometry_ && !line_free && !fill_free && !cards.empty()) {
        if (depth.size() < DEPTH_LEVELS_MAX) {
          depth.push_back(std::move(cards));
          depth_regions.push_back(region);
        }
        return;
      }
      if (cards.empty() || int(levels.size()) >= stencil_max) {
        return;
      }
      applied_line_free_ |= line_free ? 1u << levels.size() : 0u;
      applied_fill_free_ |= fill_free ? 1u << levels.size() : 0u;
      levels.push_back(std::move(cards));
      level_regions.push_back(region);
    };
    push(both, false, false);
    push(fills, true, false);
    push(lines, false, true);
  }
  unsigned required = 0;
  for (size_t k = 0; k < levels.size(); k++) {
    required |= 1u << k;
  }
  if (gpu_geometry_ && bound_) {
    /* The depth's levels: those it holds already, in order (the masks of the groups shared with the last layer
     * drawn, usually), stay; the rest are let go of and the new ones pushed. Fewer than it holds need nothing. */
    HeldMasks &held = bound_->depth_levels;
    size_t same = 0;
    while (same < depth.size() && held.holds(same, depth[same], depth_regions[same])) {
      same++;
    }
    if (same < depth.size()) {
      flush_batch();
      /* Each level only where its layer draws (a level within one holding it: a group's layers draw inside it). */
      if (held.levels.size() > same) {
        scissor(depth_regions[same]);
        pop_depth_levels(unsigned(same));
      }
      /* Worked out in the first bit the stencil's levels leave, with those above it free. */
      const unsigned bit = 1u << levels.size();
      const unsigned free = (MASK_SPARE | (MASK_SPARE - 1u)) & ~((bit << 1) - 1u);
      for (size_t k = same; k < depth.size(); k++) {
        scissor(depth_regions[k]);
        evaluate_mask_cards(doc, depth[k], bit, MASK_SCRATCH, free);
        push_depth_level(unsigned(k + 1), bit);
      }
      scissor(PixelRect());
      held.levels.resize(same);
      held.regions.resize(same);
      held.levels.insert(held.levels.end(), depth.begin() + same, depth.end());
      held.regions.insert(held.regions.end(), depth_regions.begin() + same, depth_regions.end());
    }
    applied_depth_ = unsigned(depth.size());
  }
  bool held = levels.size() == mask_clauses_.levels.size();
  for (size_t k = 0; held && k < levels.size(); k++) {
    held = mask_clauses_.holds(k, levels[k], level_regions[k]);
  }
  if (held) {
    /* The same as the last thing drawn (its neighbour, usually): the stencil already holds them, and what waits
     * to be drawn in a batch can wait on (a flush for every layer drawn costs more than the drawing). */
    return required;
  }
  /* What waits is drawn under the stencil as it is, before the masks change it. */
  flush_batch();
  for (size_t k = 0; k < levels.size(); k++) {
    /* The levels after this one, and the spare, are free while it is worked out. Only where its layer draws. */
    const unsigned free = (MASK_SPARE | (MASK_SPARE - 1u)) & ~((2u << k) - 1u);
    scissor(level_regions[k]);
    evaluate_mask_cards(doc, levels[k], 1u << k, MASK_SCRATCH, free);
  }
  scissor(PixelRect());
  mask_clauses_.levels = std::move(levels);
  mask_clauses_.regions = std::move(level_regions);
  return required;
}

void SceneRenderer::scissor(const PixelRect &rect)
{
  if (rect.whole) {
    device_->set_scissor(false);
  }
  else {
    device_->set_scissor(true, rect.x0, rect.y0, rect.x1, rect.y1);
  }
}

void SceneRenderer::step_bounds(PaintStep &step)
{
  if (step.bounded) {
    return;
  }
  float2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
  const auto add = [&](float x, float y) {
    lo = float2(std::min(lo.x, x), std::min(lo.y, y));
    hi = float2(std::max(hi.x, x), std::max(hi.y, y));
  };
  for (const Vertex &v : step.vertices) {
    add(v.x, v.y);
  }
  for (const std::vector<BrushVertex> &list : step.dabs.per_image) {
    for (const BrushVertex &v : list) {
      add(v.x, v.y);
    }
  }
  step.lo = lo;
  step.hi = hi;
  step.bounded = true;
}

SceneRenderer::PixelRect SceneRenderer::layer_region(const Document &doc, const Layer &layer)
{
  if (auto it = regions_.find(layer.id); it != regions_.end()) {
    return it->second;
  }
  float2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
  bool whole = false;
  const auto add = [&](const PixelRect &r) {
    if (r.whole) {
      whole = true;
    }
    else if (r.x0 < r.x1 && r.y0 < r.y1) {
      lo = float2(std::min(lo.x, float(r.x0)), std::min(lo.y, float(r.y0)));
      hi = float2(std::max(hi.x, float(r.x1)), std::max(hi.y, float(r.y1)));
    }
  };
  if (layer.is_group()) {
    for (const Layer &child : layer.children) {
      if (shown(child) && (child.opacity > 0.0f || full_opacity(child))) {
        add(layer_region(doc, child));
      }
    }
  }
  else if (layer.is_media() || !layer.has_drawing()) {
    whole = layer.is_media();
  }
  else if (const auto drawn = drawn_.find(layer.id); drawn == drawn_.end()) {
    whole = true;
  }
  else if (auto kept = tessellated_.find(drawn->second.key);
           kept == tessellated_.end() &&
           [&] {
             /* Not made yet: what stands in for it (the same shapes in the layer's space, made for another zoom). */
             const auto other = drawn->second.any_zoom ? tessellated_any_zoom_.find(drawn->second.any_zoom)
                                                       : tessellated_any_zoom_.end();
             if (other != tessellated_any_zoom_.end()) {
               kept = tessellated_.find(other->second);
             }
             return kept == tessellated_.end();
           }())
  {
    whole = true;
  }
  else {
    const Transform2D m = document_to_pixels_ * drawn->second.transform;
    for (PaintStep &step : kept->second.steps) {
      step_bounds(step);
      if (step.lo.x > step.hi.x) {
        continue;
      }
      for (const float2 c : {step.lo, step.hi, float2(step.lo.x, step.hi.y), float2(step.hi.x, step.lo.y)}) {
        const float2 p = m.apply(c);
        lo = float2(std::min(lo.x, p.x), std::min(lo.y, p.y));
        hi = float2(std::max(hi.x, p.x), std::max(hi.y, p.y));
      }
    }
  }
  PixelRect r;
  if (!whole) {
    r.whole = false;
    if (lo.x <= hi.x) {
      /* A few pixels more all round: a drawing standing in as made for a nearby zoom lies a little differently. */
      constexpr int PAD = 3;
      r.x0 = std::clamp(int(std::floor(lo.x)) - PAD, 0, width_);
      r.y0 = std::clamp(int(std::floor(lo.y)) - PAD, 0, height_);
      r.x1 = std::clamp(int(std::ceil(hi.x)) + PAD, 0, width_);
      r.y1 = std::clamp(int(std::ceil(hi.y)) + PAD, 0, height_);
    }
  }
  regions_[layer.id] = r;
  return r;
}

void SceneRenderer::push_depth_level(unsigned level, unsigned bit)
{
  flush_batch();
  /* The bit kept only where the levels before hold (from the second level on; everything holds level 0). */
  if (level > 1) {
    gpu::Draw keep;
    keep.state.program = gpu::Program::Stencil;
    keep.state.write_color = false;
    keep.state.stencil = {true, gpu::Compare::Equal, stencil_bits(bit), stencil_bits(bit), gpu::StencilOp::Keep,
                          gpu::StencilOp::Keep, stencil_bits(bit), false, gpu::StencilOp::Zero};
    keep.state.depth = short_of_level(level - 1);
    keep.state.depth_test = gpu::Compare::LessEqual;
    keep.count = 3;
    device_->draw(keep);
  }
  /* Then the level written there, and the bit cleared. */
  gpu::Draw write;
  write.state.program = gpu::Program::Stencil;
  write.state.write_color = false;
  write.state.stencil = {true,
                         gpu::Compare::Equal,
                         stencil_bits(bit),
                         stencil_bits(bit),
                         gpu::StencilOp::Keep,
                         gpu::StencilOp::Zero,
                         stencil_bits(bit)};
  write.state.depth = level_depth(level);
  write.state.depth_write = true;
  write.count = 3;
  device_->draw(write);
}

void SceneRenderer::pop_depth_levels(unsigned level)
{
  flush_batch();
  gpu::Draw back;
  back.state.program = gpu::Program::Stencil;
  back.state.write_color = false;
  back.state.stencil.test = false;
  back.state.depth = level_depth(level);
  back.state.depth_test = gpu::Compare::Less;
  back.state.depth_write = true;
  back.count = 3;
  device_->draw(back);
}

void SceneRenderer::evaluate_mask_cards(const Document &doc, const std::vector<std::vector<MaskRow>> &cards,
                                        unsigned result, unsigned scratch, unsigned free)
{
  for (size_t c = 0; c < cards.size(); c++) {
    if (c == 0) {
      evaluate_mask_rows(doc, cards[c], result, scratch, false, free);
      continue;
    }
    if (!(free & MASK_SPARE)) {
      break;
    }
    evaluate_mask_rows(doc, cards[c], MASK_SPARE, scratch, false, free & ~MASK_SPARE);
    stencil_where(MASK_SPARE, 0, result, 0);
  }
}

void SceneRenderer::stencil_where(unsigned test, unsigned value, unsigned write, unsigned ref)
{
  if (dry_masks_) {
    return;
  }
  flush_batch();
  gpu::Draw draw;
  /* The reference holds both the value tested (under `test`) and the one written (under `write`). */
  draw.state = stencil_state(gpu::Compare::Equal, value | (ref & ~test), test, write);
  draw.count = 3;
  device_->draw(draw);
  if (write & WINDING_BITS) {
    bound_->fan_dirty = true;
  }
}

void SceneRenderer::evaluate_mask_rows(const Document &doc, const std::vector<MaskRow> &rows, unsigned result,
                                       unsigned scratch, bool nested, unsigned free)
{
  /* The card starts as its Start With says (white: everything shown), and its steps paint on it, top
   * to bottom. */
  stencil_fill(result, rows.front().start_shown);
  for (const MaskRow &row : rows) {
    /* The row's shape: its layers combined (as one, or the first minus the others, or where they all
     * are), grown or shrunk. Shrinking draws the areas, then clears a band around each area's edge,
     * then draws the lines (thinner). */
    stencil_fill(scratch, false);
    static const MaskPass::Stage SHRINK[] = {MaskPass::Areas, MaskPass::Bands, MaskPass::Lines};
    static const MaskPass::Stage GROW[] = {MaskPass::All};
    /* Inside the Stroke shrinks each area by its line's half width (worked out per shape). */
    const bool shrink = row.grow < 0.0f || row.inside_stroke;
    /* Each layer's lines trim only its own fills: worked out a layer at a time on a spare bit, then
     * added, so a group's inner lines (strands drawn over another layer's fill) don't cut the
     * fills of the layers around them. */
    const unsigned own = !nested ? MASK_NESTED_SCRATCH : 0u;
    const bool per_leaf = row.inside_stroke && !row.clip && own;
    /* One of the row's layers (a group: its layers together) into `bit`, a layer at a time with
     * Inside the Stroke. */
    const auto draw_one = [&](const Layer &source, unsigned bit) {
      if (!per_leaf) {
        for (int s = 0; s < (shrink ? 3 : 1); s++) {
          mask_pass_ = {row.grow, shrink ? SHRINK[s] : GROW[0], row.fills_only, row.filled, row.extend,
                        row.strokes_only, row.inside_stroke};
          draw_mask_source(doc, source, bit, nested);
        }
        return;
      }
      stencil_fill(own, false);
      std::function<void(const Layer &)> each_layer = [&](const Layer &layer) {
        if (layer.is_group()) {
          for (const Layer &child : layer.children) {
            each_layer(child);
          }
          return;
        }
        for (const MaskPass::Stage stage : SHRINK) {
          mask_pass_ = {row.grow, stage, row.fills_only, row.filled, row.extend, row.strokes_only, row.inside_stroke};
          draw_mask_source(doc, layer, own, nested);
        }
        /* Into the shape, and the bit cleared for the next. */
        stencil_where(own, own, bit, bit);
        stencil_fill(own, false);
      };
      each_layer(source);
    };
    /* Difference and Intersection: each layer's shape on a bit of its own, then into the row's (a
     * Clip to Below step has one layer). Short of a bit (deep in a clip base's masks), as one. */
    unsigned operand = 0;
    if (row.combine != MaskCombine::Union && !row.clip && row.layers.size() > 1) {
      operand = !nested ? MASK_NESTED : (free & (~free + 1u));
    }
    if (operand) {
      bool first = true;
      for (const uint64_t id : row.layers) {
        const Layer *source = layer_find(doc, id);
        if (!source) {
          continue;
        }
        stencil_fill(operand, false);
        draw_one(*source, operand);
        if (first) {
          stencil_where(operand, operand, scratch, scratch);
        }
        else if (row.combine == MaskCombine::Difference) {
          stencil_where(operand, operand, scratch, 0);
        }
        else {
          stencil_where(operand, 0, scratch, 0);
        }
        first = false;
      }
      stencil_fill(operand, false);
    }
    else if (per_leaf) {
      for (const uint64_t id : row.layers) {
        if (const Layer *source = layer_find(doc, id)) {
          draw_one(*source, scratch);
        }
      }
    }
    for (int s = 0; s < (shrink ? 3 : 1) && !per_leaf && !operand; s++) {
      for (const uint64_t id : row.layers) {
        if (const Layer *source = layer_find(doc, id)) {
          mask_pass_ = {row.grow, shrink ? SHRINK[s] : GROW[0], row.fills_only, row.filled, row.extend,
                        row.strokes_only, row.inside_stroke};
          if (row.clip) {
            /* Clip to Below: what the base shows, not every shape in it. */
            std::vector<const Layer *> chain;
            mask_pass_ = {};
            mask_pass_.exclude_strokes = row.fills_only;
            draw_clip_base(doc, *source, scratch, nested, free, chain);
          }
          else {
            draw_mask_source(doc, *source, scratch, nested);
          }
        }
      }
    }
    mask_pass_ = {};
    /* Where the row's shape is (or with Invert, where it isn't). */
    const unsigned inside = row.invert ? 0u : scratch;
    switch (row.mode) {
      case MaskMode::Add:
        stencil_where(scratch, inside, result, result);
        break;
      case MaskMode::Subtract:
        stencil_where(scratch, inside, result, 0);
        break;
    }
  }
}

void SceneRenderer::draw_mask_preview(const Document &doc, const Layer &layer)
{
  flush_batch();
  /* The card alone (all of the layer's cards that mask that part without one); lines ended at an
   * edge count by their card's shape. */
  const MaskCuts skip = mask_preview_lines_ ? MaskCuts::Fills : MaskCuts::Lines;
  std::vector<std::vector<MaskRow>> cards;
  for (CardRows &card : cards_of(doc, layer, false)) {
    const bool picked = mask_preview_upto_ >= 0 ? card.index == mask_preview_upto_ : card.cuts != skip;
    if (picked) {
      cards.push_back(std::move(card.steps));
    }
  }
  unsigned required = 0;
  if (!cards.empty()) {
    evaluate_mask_cards(doc, cards, 1u, MASK_SCRATCH, (MASK_SPARE | (MASK_SPARE - 1u)) & ~1u);
    required = 1u;
  }
  mask_clauses_.clear();
  required_stencil_ = 0;
  /* Over the drawing at 90%, so it shows faintly through: white where every level shows the layer,
   * black elsewhere. */
  const Transform2D to_document = document_to_pixels_.inverted();
  const float2 a = to_document.apply({0.0f, 0.0f}), b = to_document.apply({float(width_), float(height_)});
  const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
  constexpr float alpha = 0.9f;
  for (const bool shows : {true, false}) {
    if (required == 0 && !shows) {
      continue;
    }
    paint_ = color_state(required, 0);
    if (!shows) {
      paint_.stencil.compare = gpu::Compare::NotEqual;
    }
    /* Premultiplied. */
    const float v = shows ? alpha : 0.0f;
    const std::vector<Vertex> quad = {{x0, y0, v, v, v, alpha}, {x1, y0, v, v, v, alpha}, {x1, y1, v, v, v, alpha},
                                      {x0, y0, v, v, v, alpha}, {x1, y1, v, v, v, alpha}, {x0, y1, v, v, v, alpha}};
    draw_vertices(quad, Transform2D());
  }
}

void SceneRenderer::draw_clip_base(const Document &doc, const Layer &layer, unsigned bit, bool nested, unsigned free,
                                    std::vector<const Layer *> &chain)
{
  /* Hidden layers (and guides in a render) don't show, so they don't clip, unlike a mask row's. */
  if (!shown(layer) || layer.opacity <= 0.0f) {
    return;
  }
  chain.push_back(&layer);
  if (layer.is_group()) {
    for (const Layer &child : layer.children) {
      draw_clip_base(doc, child, bit, nested, free, chain);
    }
  }
  else {
    /* Where the layer's masks and its groups' (down from the base) all show it, as they draw: each
     * level's rows into a result bit (their layers' own masks counting), the next levels into another
     * and kept only where both hold. Short of free bits, one level deep in the nested bits. */
    unsigned bits[3] = {0, 0, 0};
    int count = 0;
    for (unsigned b = 1; b <= MASK_SPARE && count < 3; b <<= 1) {
      if (free & b) {
        bits[count++] = b;
      }
    }
    const bool deep = count >= 2;
    const unsigned result = deep ? bits[0] : MASK_NESTED;
    const unsigned scratch = deep ? bits[1] : MASK_NESTED_SCRATCH;
    const unsigned more = deep ? bits[2] : MASK_SPARE & free;
    unsigned test = 0;
    if (!nested) {
      const MaskPass pass = mask_pass_;
      for (const Layer *l : chain) {
        /* The base shows as its fills do: its cards that mask only lines left out. */
        for (const CardRows &card : cards_of(doc, *l, final_render_)) {
          if (card.cuts == MaskCuts::Lines) {
            continue;
          }
          if (test == 0) {
            evaluate_mask_rows(doc, card.steps, result, scratch, !deep);
            test = result;
          }
          else if (more) {
            evaluate_mask_rows(doc, card.steps, more, scratch, !deep);
            stencil_where(more, 0, result, 0);
          }
        }
      }
      mask_pass_ = pass;
    }
    coverage_test_ = test;
    draw_coverage(doc, layer, Transform2D(), bit, true);
    coverage_test_ = 0;
  }
  chain.pop_back();
}

void SceneRenderer::draw_mask_source(const Document &doc, const Layer &source, unsigned bit, bool /*nested*/)
{
  /* The mask layer's shapes as drawn, before any masks of its own (they mask how it shows, not the
   * shape it lends; only Clip to Below takes what its base shows). */
  coverage_test_ = 0;
  draw_coverage(doc, source, Transform2D(), bit, mask_pass_.stage != MaskPass::Bands);
}

void SceneRenderer::draw_coverage(const Document &doc, const Layer &layer, const Transform2D & /*parent*/, unsigned bit,
                                  bool set)
{
  if (!dry_masks_) {
    flush_batch();
  }
  /* Hidden layers still mask: a mask shape is often kept hidden. */
  if (layer.is_group()) {
    for (const Layer &child : layer.children) {
      draw_coverage(doc, child, Transform2D(), bit, set);
    }
    return;
  }
  const Transform2D transform = layer_world_transform(doc, layer.id);
  /* With coverage_test_, only where those bits are set (the mask layer's own masks). */
  if (!dry_masks_) {
    paint_ = stencil_state(coverage_test_ ? gpu::Compare::Equal : gpu::Compare::Always,
                           (set ? bit : 0u) | coverage_test_, coverage_test_ ? coverage_test_ : bit, bit);
    if (bit & WINDING_BITS) {
      bound_->fan_dirty = true;
    }
  }
  coverage_pass_ = true;
  draw_drawing(doc, layer, transform, 1.0f);
  coverage_pass_ = false;
}

static uint64_t hash_mix(uint64_t h, uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h * 0xff51afd7ed558ccdull;
}

static uint64_t hash_float(uint64_t h, float f)
{
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return hash_mix(h, bits);
}

static uint64_t hash_color(uint64_t h, const Color &c)
{
  return hash_float(hash_float(hash_float(hash_float(h, c.r), c.g), c.b), c.a);
}

/* Everything a drawing's outlines and triangles are made from (shape_outline, shape_runs and the
 * tessellators), and nothing they aren't (selection, keys, weights, names). With `unit`, the points are
 * taken relative to the first, to that size (the same for the drawing moved whole). */
static uint64_t drawing_geometry_hash(const VectorDrawing &d, float unit = 0.0f)
{
  uint64_t h = hash_mix(0, d.points.size());
  const float2 origin = unit > 0.0f && !d.points.empty() ? d.points[0].pos : float2();
  for (const MeshPoint &p : d.points) {
    if (unit > 0.0f) {
      h = hash_mix(hash_mix(h, uint64_t(std::llround((p.pos.x - origin.x) / unit))),
                   uint64_t(std::llround((p.pos.y - origin.y) / unit)));
      h = hash_float(h, p.radius);
    }
    else {
      h = hash_float(hash_float(hash_float(h, p.pos.x), p.pos.y), p.radius);
    }
    h = hash_mix(h, uint64_t(p.detail.level));
  }
  h = hash_mix(h, d.curves.size());
  for (const Curve &c : d.curves) {
    h = hash_mix(hash_mix(h, c.points.size()), c.closed ? 1 : 0);
    for (const CurvePoint &cp : c.points) {
      h = hash_mix(h, uint64_t(cp.point));
      h = hash_float(hash_float(hash_float(h, cp.curvature), cp.weight_in), cp.weight_out);
      h = hash_float(hash_float(h, cp.offset_in), cp.offset_out);
      if (unit > 0.0f) {
        /* Handles are worked out again as their points move: a hair apart is the same. */
        for (const float v : {cp.handle_in.x, cp.handle_in.y, cp.handle_out.x, cp.handle_out.y}) {
          h = hash_mix(h, uint64_t(std::llround(v / unit)));
        }
      }
      else {
        h = hash_float(hash_float(hash_float(hash_float(h, cp.handle_in.x), cp.handle_in.y), cp.handle_out.x),
                       cp.handle_out.y);
      }
      h = hash_mix(h, uint64_t(cp.edge_hidden) | uint64_t(cp.handles_set) << 1 | uint64_t(cp.aligned) << 2);
    }
  }
  h = hash_mix(h, d.shapes.size());
  for (const Shape &s : d.shapes) {
    h = hash_mix(h, s.edges.size());
    for (const ShapeEdge &e : s.edges) {
      h = hash_mix(hash_mix(h, uint64_t(e.curve)), uint64_t(e.segment));
    }
    h = hash_mix(h, uint64_t(s.has_fill) | uint64_t(s.has_outline) << 1 | uint64_t(s.hidden) << 2 |
                        uint64_t(s.merged_fill) << 3 | uint64_t(s.merged_line) << 4);
    h = hash_mix(h, uint64_t(s.detail_level));
    h = hash_float(hash_color(hash_color(h, s.style.fill), s.style.line), s.style.line_width);
  }
  return h;
}

static uint64_t hash_value(uint64_t h, float v) { return hash_float(h, v); }
static uint64_t hash_value(uint64_t h, int v) { return hash_mix(h, uint64_t(uint32_t(v))); }
static uint64_t hash_value(uint64_t h, bool v) { return hash_mix(h, v ? 1 : 2); }
static uint64_t hash_value(uint64_t h, const std::string &v) { return hash_mix(h, std::hash<std::string>()(v)); }

/* Every setting of a brush (they all change its stamps). */
static uint64_t brush_settings_hash(uint64_t h, const BrushSettings &b)
{
#define OANIM_BRUSH_HASH(type, field, key) h = hash_value(h, b.field);
  OANIM_BRUSH_SETTINGS(OANIM_BRUSH_HASH)
#undef OANIM_BRUSH_HASH
  return h;
}

/* The setup pose brushed lines lay their stamps out on: the points' and handles' rest places. */
static uint64_t setup_hash(const VectorDrawing &d)
{
  uint64_t h = hash_mix(0, d.points.size());
  for (const MeshPoint &p : d.points) {
    h = hash_float(hash_float(h, p.setup_pos.x), p.setup_pos.y);
  }
  for (const Curve &c : d.curves) {
    for (const CurvePoint &cp : c.points) {
      h = hash_float(hash_float(hash_float(hash_float(h, cp.setup_in.x), cp.setup_in.y), cp.setup_out.x), cp.setup_out.y);
    }
  }
  return h;
}

void SceneRenderer::draw_dabs_step(PaintStep &step, const Transform2D &transform, float opacity)
{
  /* The brush's images (a brush reloaded since gets its textures again). */
  if (const Brush *brush = brush_find(step.brush)) {
    coverage_alpha_ = step.coverage_alpha;
    draw_dabs(step.dabs, brush_tip_images(*brush, step.settings), step.own_colors, step.merged_alpha, transform,
              opacity, &step.dab_meshes);
    coverage_alpha_ = 0.5f;
  }
}

bool SceneRenderer::fade_whole(const Layer &layer, float opacity) const
{
  /* A layer whose lines and fills are masked apart can't be laid over as one image. */
  return gpu_geometry_ && !coverage_pass_ && opacity < 1.0f && opacity > 0.0f && !layer.is_media() &&
         (line_free_stencil_ | fill_free_stencil_) == 0;
}

void SceneRenderer::draw_faded_drawing(const Document &doc, const Layer &layer, const Transform2D &transform,
                                       Target *target, float opacity)
{
  flush_batch();
  Target *own = acquire_target();
  bind_target(own);
  /* Laid back over only what its steps cover (below), within its region: only that cleared, where known. */
  const bool per_sample = device_->caps().sample_shading && device_->has_sample_texture(own->target);
  clear_within(per_sample ? layer_region(doc, layer) : PixelRect(), Color{0, 0, 0, 0});
  /* Its own image has no masks of its own (they apply as it is laid over). */
  const unsigned outer_required = required_stencil_, outer_depth = depth_required_;
  required_stencil_ = 0;
  depth_required_ = 0;
  track_bounds_ = true;
  bounds_lo_ = float2(1e30f, 1e30f);
  bounds_hi_ = float2(-1e30f, -1e30f);
  use_color_stencil();
  draw_drawing(doc, layer, transform, 1.0f);
  flush_batch();
  track_bounds_ = false;
  required_stencil_ = outer_required;
  depth_required_ = outer_depth;
  bind_target(target);
  use_color_stencil();
  if (!per_sample) {
    resolve(own);
    composite(own, opacity);
  }
  else if (bounds_lo_.x <= bounds_hi_.x) {
    /* Over what it covers, a pixel more all round (clip space), not the whole image. */
    const float px = 2.0f / float(width_), py = 2.0f / float(height_);
    const float x0 = std::max(bounds_lo_.x - px, -1.0f), y0 = std::max(bounds_lo_.y - py, -1.0f);
    const float x1 = std::min(bounds_hi_.x + px, 1.0f), y1 = std::min(bounds_hi_.y + py, 1.0f);
    lay_samples(own, opacity, float2(x0, y0), float2(x1, y1));
  }
  release_target(own);
}

void SceneRenderer::lay_samples(const Target *source, float opacity, float2 lo, float2 hi)
{
  if (!(lo.x < hi.x && lo.y < hi.y)) {
    return;
  }
  const Vertex quad[6] = {{lo.x, lo.y, 0, 0, 0, 0}, {hi.x, lo.y, 0, 0, 0, 0}, {hi.x, hi.y, 0, 0, 0, 0},
                          {lo.x, lo.y, 0, 0, 0, 0}, {hi.x, hi.y, 0, 0, 0, 0}, {lo.x, hi.y, 0, 0, 0, 0}};
  gpu::Draw draw;
  draw.state = paint_;
  draw.state.program = gpu::Program::FadeSamples;
  draw.texture = device_->sample_texture(source->target);
  draw.stream = quad;
  draw.stream_bytes = sizeof(quad);
  draw.stream_layout = gpu::Layout::Color;
  draw.count = 6;
  draw.opacity = opacity;
  device_->draw(draw);
}

void SceneRenderer::note_bounds(PaintStep &step, const Transform2D &transform)
{
  step_bounds(step);
  if (step.lo.x > step.hi.x) {
    return;
  }
  const Transform2D m = to_clip(transform);
  for (const float2 corner : {step.lo, step.hi, float2(step.lo.x, step.hi.y), float2(step.hi.x, step.lo.y)}) {
    const float2 p = m.apply(corner);
    bounds_lo_ = float2(std::min(bounds_lo_.x, p.x), std::min(bounds_lo_.y, p.y));
    bounds_hi_ = float2(std::max(bounds_hi_.x, p.x), std::max(bounds_hi_.y, p.y));
  }
}

void SceneRenderer::draw_steps(std::vector<PaintStep> &steps, const Transform2D &transform, float opacity,
                               uint64_t key)
{
  if (key != 0 && !coverage_pass_ && device_->caps().batching) {
    const Transform2D step_to_clip = to_clip(transform);
    for (size_t i = 0; i < steps.size(); i++) {
      PaintStep &step = steps[i];
      if (track_bounds_) {
        note_bounds(step, transform);
      }
      if (step.is_dabs) {
        /* Stamps draw at once (draw_dabs flushes what waits before them), under the masks lines are. */
        if ((line_free_stencil_ | fill_free_stencil_) != 0) {
          use_color_stencil(step.is_line ? Paint::Line : Paint::Fill);
        }
        draw_dabs_step(step, transform, opacity);
        continue;
      }
      if (step.vertices.empty()) {
        continue;
      }
      const unsigned required = step.is_line ? required_stencil_ & ~line_free_stencil_
                                             : required_stencil_ & ~fill_free_stencil_;
      if (step.stencil_fan) {
        draw_stencil_fill(step, step_to_clip, opacity, required);
        continue;
      }
      batch_.push_back({&step, (key ^ (uint64_t(i) + 1) * 0x9e3779b97f4a7c15ull) * 1099511628211ull, step_to_clip,
                        opacity, required, depth_required_});
    }
    return;
  }
  flush_batch();
  /* In colour, a layer's lines leave out the masks that end them (End Lines at Edge, Brush), and its
   * fills the Lines Only ones. */
  const bool lines_apart = !coverage_pass_ && (line_free_stencil_ | fill_free_stencil_) != 0;
  for (PaintStep &step : steps) {
    if (track_bounds_) {
      note_bounds(step, transform);
    }
    if (lines_apart) {
      use_color_stencil(step.is_line ? Paint::Line : Paint::Fill);
    }
    if (!step.is_dabs) {
      if (step.stencil_fan) {
        draw_stencil_fill(step, to_clip(transform), opacity,
                          step.is_line ? required_stencil_ & ~line_free_stencil_
                                       : required_stencil_ & ~fill_free_stencil_);
        continue;
      }
      if (!step.vertices.empty()) {
        if (!step.mesh) {
          step.mesh = device_->make_mesh(gpu::Layout::Color, step.vertices.data(),
                                         step.vertices.size() * sizeof(Vertex));
        }
        gpu::Draw draw;
        draw.state = paint_;
        draw.state.program = gpu::Program::Color;
        draw.mesh = step.mesh;
        draw.count = int(step.vertices.size());
        draw.to_clip = to_clip(transform);
        draw.opacity = opacity;
        device_->draw(draw);
      }
      continue;
    }
    draw_dabs_step(step, transform, opacity);
  }
}

uint64_t SceneRenderer::geometry_hash(const Layer &layer, const VectorDrawing &drawing, bool modified)
{
  /* A drawing that is its layer's own (no modifiers, not a reference built from another) changes only with the
   * layer: its hash holds while the layer's revision does. */
  if (modified || layer.is_reference() || &drawing != &layer.drawing) {
    return drawing_geometry_hash(drawing);
  }
  const auto [it, added] = geometry_hashes_.try_emplace(layer.id);
  if (added || it->second.revision != layer.revision.value) {
    it->second = {layer.revision.value, drawing_geometry_hash(drawing)};
  }
  return it->second.hash;
}

std::vector<SceneRenderer::PaintStep> SceneRenderer::make_steps(const MakeArgs &args)
{
  const StatTimer stat_make(STAT_MAKE);
  /* Masks' shapes are made as they are drawn, one at a time, not before with the rest. */
  const StatTimer stat_make_mask(STAT_MAKE_MASK, args.coverage);
  if (stat_make.on) {
    g_stat_made++;
  }
  const Document &doc = *args.doc;
  const Layer &layer = *args.layer;
  const VectorDrawing &drawing = *args.drawing;
  const Transform2D &transform = args.transform;
  const float pixel = args.pixel;
  /* A banded drawing's pixel at the band's nearest zoom and farthest (MakeArgs::banded). */
  const float band_reach = args.banded ? std::exp2(0.5f * ZOOM_BAND_OCTAVES) : 1.0f;
  const float pixel_nearest = pixel / band_reach, pixel_farthest = pixel * band_reach;
  const LineCutter &cutter = args.cutter;
  constexpr float feather = 0.0f;
  /* A point of a line (layer space) shows by those rows. */
  const auto line_shows = [&](float2 p) { return line_cutter_shows(cutter, transform.apply(p)); };
  BrushCut brush_cut;
  brush_cut.shows = line_shows;
  brush_cut.taper = !cutter.flat;
  brush_cut.reach = cutter.reach;
  std::vector<PaintStep> steps;

  std::vector<Vertex> vertices;
  std::vector<float2> outline;
  std::vector<float> radii;
  std::vector<bool> visible;
  std::vector<StrokeJoin> joins;

  /* The colour batch so far, as a step: before a brushed line so shapes keep their stacking order. */
  /* Whether the batch so far holds lines (they may be drawn under fewer masks than fills). */
  bool batch_lines = false;
  const auto flush = [&] {
    if (!vertices.empty()) {
      PaintStep step;
      step.vertices = std::move(vertices);
      step.is_line = batch_lines;
      steps.push_back(std::move(step));
      vertices.clear();
    }
  };
  /* Starts a batch of lines or of fills, closing one of the other kind. */
  const auto batch = [&](bool lines) {
    if (batch_lines != lines) {
      flush();
      batch_lines = lines;
    }
  };
  /* GPU Geometry: a colour fill this big (outline points) is drawn by stencil-then-cover, not cut into triangles,
   * which costs more than drawing it twice once the outline is long (cutting grows about as its square on a spiky
   * one); smaller ones stay in the batches, drawing two passes each costing more than cutting them (Clay013, a new
   * zoom: 85 ms Classic, 64 with this at 256 points, 60 at 1,024; drawn again as kept: 31, 37, 32). Masks' shapes
   * are always cut: the stencil is theirs while they are worked out. OANIM_FAN_MIN_POINTS sets another size (0:
   * every fill), for render_check. */
  static const size_t fan_min_points = [] {
#pragma warning(suppress : 4996)
    const char *v = std::getenv("OANIM_FAN_MIN_POINTS");
    return v ? size_t(std::strtoul(v, nullptr, 10)) : size_t(1024);
  }();
  const auto fan_fill = [&](size_t points) { return gpu_geometry_ && !args.coverage && points >= fan_min_points; };
  /* A fill as a step of its own, between the batches before and after it. */
  const auto stencil_step = [&](const std::vector<const std::vector<float2> *> &rings, Color color, bool even_odd) {
    flush();
    PaintStep step;
    const StatTimer stat_fan(STAT_FAN);
    tessellate_stencil_fan(rings, color, pixel_farthest, step.vertices, step.fan_indices);
    if (step.vertices.size() > 6) {
      step.stencil_fan = true;
      step.even_odd = even_odd;
      steps.push_back(std::move(step));
    }
  };
  const Brush *brush = nullptr;
  const std::vector<BrushImage> *images = nullptr;
  std::vector<float> aspects;
  BrushDabs dabs;
  uint32_t seed = 0;
  /* The drawing as on the setup frame, which brushed lines lay their dabs out on (made when the
   * first one needs it). */
  VectorDrawing setup;
  bool setup_made = false;
  /* One stroke (the whole outline, or a run between hidden edges). */
  const auto stroke = [&](const Shape &shape, const std::vector<float2> &pts, const std::vector<float> &half,
                          bool is_closed, const std::vector<StrokeJoin> &stroke_joins) {
    const StatTimer stat_stroke(STAT_STROKE);
    batch(!args.coverage);
    /* A line thinner than a pixel is drawn a pixel wide and as much fainter, so its weight stays true however far
     * the view is zoomed out rather than every hairline looking a pixel thick: by the GPU, from each vertex's
     * offset from the line (Vertex), so the triangles serve every zoom. A mask takes it as it is (its widths kept
     * a pixel wide where it's made). */
    const size_t first = vertices.size();
    tessellate_stroke(pts, half, is_closed, shape.style.line, feather, vertices, &stroke_joins);
    if (args.coverage) {
      for (size_t i = first; i < vertices.size(); i++) {
        vertices[i].w = 0;
      }
    }
  };

  /* Each shape's steps as made last time, kept while the shape, its curves and their points are as they were. */
  ShapesKept &kept_shapes = *args.kept;
  kept_shapes.shapes.resize(drawing.shapes.size());
  const auto shape_unchanged = [&](const Shape &shape, const ShapeKept &k) {
    if (!(k.shape == shape) || (k.steps.empty() && k.curves.empty())) {
      return false;
    }
    for (const auto &[index, curve] : k.curves) {
      if (index >= int(drawing.curves.size()) || !(drawing.curves[size_t(index)] == curve)) {
        return false;
      }
    }
    for (const auto &[index, point] : k.points) {
      if (index >= int(drawing.points.size()) || !(drawing.points[size_t(index)] == point)) {
        return false;
      }
    }
    return true;
  };
  const auto remember_shape = [&](const Shape &shape, ShapeKept &k, size_t first_step, size_t first_vertex) {
    k.shape = shape;
    k.curves.clear();
    k.points.clear();
    std::vector<int> curves;
    for (const ShapeEdge &e : shape.edges) {
      if (e.curve >= 0 && e.curve < int(drawing.curves.size())) {
        curves.push_back(e.curve);
      }
    }
    std::sort(curves.begin(), curves.end());
    curves.erase(std::unique(curves.begin(), curves.end()), curves.end());
    std::vector<int> points;
    for (const int c : curves) {
      k.curves.emplace_back(c, drawing.curves[size_t(c)]);
      for (const CurvePoint &cp : drawing.curves[size_t(c)].points) {
        points.push_back(cp.point);
      }
    }
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    for (const int i : points) {
      if (i >= 0 && i < int(drawing.points.size())) {
        k.points.emplace_back(i, drawing.points[size_t(i)]);
      }
    }
    /* What the shape made: from where the batch stood when it began (the rest of that batch the shapes' before it)
     * to the batch as it stands now. */
    k.steps.clear();
    const auto add_plain = [&](const std::vector<Vertex> &from, size_t begin, bool is_line) {
      if (begin < from.size()) {
        PaintStep piece;
        piece.is_line = is_line;
        piece.vertices.assign(from.begin() + std::ptrdiff_t(begin), from.end());
        k.steps.push_back(std::move(piece));
      }
    };
    if (steps.size() > first_step) {
      /* (Stamps and stencil fills flush the batch before them, so one of them comes first only whole.) */
      const PaintStep &head = steps[first_step];
      if (head.is_dabs || head.stencil_fan) {
        k.steps.push_back(head);
      }
      else {
        add_plain(head.vertices, first_vertex, head.is_line);
      }
      k.steps.insert(k.steps.end(), steps.begin() + std::ptrdiff_t(first_step) + 1, steps.end());
      add_plain(vertices, 0, batch_lines);
    }
    else {
      add_plain(vertices, first_vertex, batch_lines);
    }
    /* A shape that made nothing still counts as made (`curves` says so). */
    if (k.curves.empty()) {
      k.curves.emplace_back(-1, Curve());
    }
  };

  for (int shape_index = 0; shape_index < int(drawing.shapes.size()); shape_index++) {
    ShapeKept &kept_shape = kept_shapes.shapes[size_t(shape_index)];
    if (shape_unchanged(drawing.shapes[size_t(shape_index)], kept_shape)) {
      /* Into the batch as it goes, as made (stamps and stencil-then-cover fills as steps of their own). */
      for (const PaintStep &step : kept_shape.steps) {
        if (step.is_dabs || step.stencil_fan) {
          flush();
          steps.push_back(step);
          continue;
        }
        batch(step.is_line);
        vertices.insert(vertices.end(), step.vertices.begin(), step.vertices.end());
      }
      continue;
    }
    /* The batch goes on across shapes (each keeps its part, remember_shape), so a drawing's steps are as few as
     * its kinds of drawing allow without joining them again after. */
    const size_t first_step = steps.size(), first_vertex = vertices.size();
    struct Remember {
      const std::function<void()> fn;
      ~Remember() { fn(); }
    } remember{[&] { remember_shape(drawing.shapes[size_t(shape_index)], kept_shape, first_step, first_vertex); }};
    /* Each connected run of the shape's edges draws on its own (see shape_runs). */
    const std::vector<Shape> runs = shape_runs(drawing, drawing.shapes[shape_index]);
    /* With a fill rule its closed runs fill together, holes and all (once, at the first run that fills);
     * the lines still go round each run. */
    const Shape &whole = drawing.shapes[shape_index];
    const bool fills_together = whole.fill_rule != FillRule::Loops && runs.size() > 1;
    bool filled_together = false;
    /* A brushed line's runs as the setup frame has them (worked out once per shape, at its first brushed run). */
    std::vector<Shape> setup_runs;
    bool setup_runs_made = false;
    for (size_t run_index = 0; run_index < runs.size(); run_index++) {
      Shape shape = runs[run_index];
      /* Merged: another layer's Boolean draws its fill or line (or both), as part of its outline, and
       * masks with them. */
      if (shape.hidden || (shape.merged_fill && shape.merged_line)) {
        continue;
      }
      shape.has_fill = shape.has_fill && !shape.merged_fill;
      shape.has_outline = shape.has_outline && !shape.merged_line;
      bool closed = false;
      /* Quality sets the samples per segment, and the stray allowed where handles stretch a
       * segment: a quarter pixel at the viewport's default 12, finer for renders. */
      {
        const StatTimer stat_outline(STAT_OUTLINE);
        shape_outline(drawing, shape, quality_, outline, &radii, &closed, &visible, &joins,
                      pixel_nearest * 3.0f / float(quality_));
      }
      if (outline.size() < 2) {
        continue;
      }
      /* A mask's Extend: the edges that mask swept to far off in the layer's own direction, so the
       * shape and everything beyond it that way masks. A filled shape (or Filled Shape) sweeps its
       * whole outline; an outline-only one just the edges it draws, so a line with hidden edges (an
       * upper lip drawn from a whole mouth shape) cuts along what shows. */
      const MaskPass::Stage stage = args.coverage ? args.mask_pass.stage : MaskPass::All;
      const float grow = args.coverage ? args.mask_pass.grow * pixel : 0.0f;
      const bool is_area = !(args.coverage && args.mask_pass.exclude_fills) &&
                           ((shape.has_fill && closed) || (args.coverage && args.mask_pass.filled));
      /* Inside the Stroke: the area ends at its line's inner edge: the line's half width in from
       * the outline at each point (as wide as the line is there), less any grow, and wherever the
       * line itself is drawn (a brushed line's dabs, turned or jittered, reach past its width). */
      const bool inside_stroke = args.coverage && args.mask_pass.inside_stroke && shape.has_outline &&
                                 shape.style.line_width > 0.0f;
      /* The line as drawn, to clear (Bands with Inside the Stroke): no grow, whatever the row's. */
      bool line_as_drawn = false;
      if (stage == MaskPass::Bands) {
        /* Shrinking: clear a band as wide as the shrink around the area's edge. */
        if (is_area) {
          std::vector<float> band(outline.size(), std::max(-grow, 0.0f));
          if (inside_stroke) {
            for (size_t k = 0; k < band.size(); k++) {
              const float r = (k < radii.size() ? radii[k] : 1.0f) * shape.style.line_width * 0.5f;
              band[k] = std::max(r - grow, 0.0f);
            }
          }
          tessellate_stroke(outline, band, true, shape.style.fill, feather, vertices);
        }
        if (!(is_area && inside_stroke)) {
          continue;
        }
        line_as_drawn = true;
      }
      if (args.coverage && args.mask_pass.extend != 0 && stage != MaskPass::Lines && !line_as_drawn) {
        static const float2 DIRS[] = {{0, 0}, {0, 1}, {0, -1}, {-1, 0}, {1, 0}};
        const float2 reach = DIRS[std::clamp(args.mask_pass.extend, 0, 4)] * (pixel * 100000.0f);
        const bool area = !args.mask_pass.exclude_fills && ((shape.has_fill && closed) || args.mask_pass.filled);
        const bool strokes = shape.has_outline && !args.mask_pass.exclude_strokes;
        const size_t n = outline.size();
        for (size_t i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
          if (!area && !(strokes && i < visible.size() && visible[i])) {
            continue;
          }
          const float2 a = outline[i], b = outline[(i + 1) % n];
          batch(false);
          tessellate_fill({a, b, b + reach, a + reach}, shape.style.fill, feather, vertices);
        }
      }
      /* A mask's Filled Shape: any shape masks with its inside, an open one closed straight across. */
      if (is_area && stage != MaskPass::Lines && !line_as_drawn) {
        batch(false);
        if (!fills_together) {
          if (fan_fill(outline.size())) {
            /* One outline fills where it winds round (as fill_contains finds), or Even-Odd where it does so an odd
             * number of times. */
            stencil_step({&outline}, shape.style.fill, whole.fill_rule == FillRule::EvenOdd);
          }
          else {
            const StatTimer stat_earcut(STAT_EARCUT);
            tessellate_fill(outline, shape.style.fill, feather, vertices);
          }
        }
        else if (!filled_together) {
          filled_together = true;
          const std::vector<std::vector<float2>> loops =
              shape_fill_loops(drawing, whole, quality_, pixel_nearest * 3.0f / float(quality_));
          if (gpu_geometry_ && !args.coverage) {
            /* GPU Geometry: the loops as they are, their fill rule worked out by the stencil's count (Even-Odd: where
             * it's odd; Nonzero: where it isn't 0), not cut into pieces on the CPU first (fill_area: Rainbow Dash's
             * holed fills, imported from Flash, 452 ms of making to 360, summed over the cores). */
            std::vector<const std::vector<float2> *> rings;
            for (const std::vector<float2> &loop : loops) {
              rings.push_back(&loop);
            }
            stencil_step(rings, shape.style.fill, whole.fill_rule == FillRule::EvenOdd);
          }
          else {
            const StatTimer stat_earcut(STAT_EARCUT);
            tessellate_fill_pieces(fill_area(loops, whole.fill_rule), shape.style.fill, feather, vertices);
          }
        }
        if (grow > 0.0f && !inside_stroke) {
          /* A mask's Grow: a rim that wide around the area. */
          tessellate_stroke(outline, std::vector<float>(outline.size(), grow), true, shape.style.fill, feather,
                            vertices);
        }
      }
      if (shape.has_outline && shape.style.line_width > 0.0f &&
          (line_as_drawn || !(args.coverage && args.mask_pass.exclude_strokes)) && stage != MaskPass::Areas)
      {
        const float line_grow = line_as_drawn ? 0.0f : grow;
        /* A brushed line, if its brush is in the library and has images; otherwise solid. */
        brush = brush_find(shape.style.brush.name);
        images = brush ? &brush_tip_images(*brush, shape.style.brush) : nullptr;
        if (images && images->empty()) {
          brush = nullptr;
        }
        if (brush) {
          aspects.clear();
          for (const BrushImage &image : *images) {
            aspects.push_back(float(image.width) / float(std::max(image.height, 1)));
          }
          for (auto &list : dabs.per_image) {
            list.clear();
          }
          dabs.centers.clear();
          dabs.alphas.clear();
          dabs.count = 0;
          seed = brush_seed(shape.style.brush, layer.id, shape_index, doc.frame_current);
          /* Dabs stick to the artwork: laid out on the line as on the setup frame, put on it as it
           * is now (hidden edges get none). */
          if (!setup_made) {
            setup = layer_setup_drawing(doc, layer, final_render_);
            setup_made = true;
          }
          const float extra = line_grow;
          const BrushPath shown = brush_path(drawing, shape, extra);
          if (!setup_runs_made && !shape.merge_made && shape_index < int(setup.shapes.size()) &&
              setup.shapes.size() == drawing.shapes.size())
          {
            setup_runs = shape_runs(setup, setup.shapes[shape_index]);
          }
          setup_runs_made = true;
          const BrushPath rest = run_index < setup_runs.size() ? brush_path(setup, setup_runs[run_index], extra) : shown;
          brush_stroke(shown, rest, shape.style.brush, shape.style.line, aspects, pixel * 0.5f, seed, dabs,
                       cutter.empty() ? nullptr : &brush_cut, !args.coverage);
          if (dabs.count > 0) {
            flush();
            PaintStep step;
            step.is_dabs = true;
            /* Handed over, not copied: a long line has thousands of dabs. */
            step.dabs = std::move(dabs);
            dabs = BrushDabs();
            step.brush = shape.style.brush.name;
            step.settings = shape.style.brush;
            step.own_colors = brush->is_color && !shape.style.brush.tint;
            step.merged_alpha = shape.style.brush.merged_alpha;
            step.coverage_alpha = line_as_drawn ? 0.02f : 0.5f;
            step.is_line = !args.coverage;
            steps.push_back(std::move(step));
          }
          continue;
        }
        /* Half widths for the tessellator: the shape's width scaled by each point's radius, a pixel
         * wider when the mask expands. A mask keeps hairlines a pixel wide; in colour they keep their
         * true width here, and the stroke draws them a pixel wide and as much fainter. */
        bool any_width = false;
        for (float &r : radii) {
          r = r * shape.style.line_width * 0.5f + line_grow;
          /* A line shrunk away is gone. */
          r = line_grow < 0.0f || !args.coverage ? std::max(r, 0.0f) : std::max(r, pixel * 0.5f);
          any_width |= r > 0.0f;
        }
        if (!any_width) {
          continue;
        }
        /* Hidden edges (and End Lines at Edge) break the outline into open runs, each stroked. */
        line_runs(outline, radii, visible, joins, closed, cutter.empty() || args.coverage ? nullptr : &cutter,
                  transform, [&](const std::vector<float2> &pts, const std::vector<float> &half, bool is_closed,
                                 const std::vector<StrokeJoin> &stroke_joins) {
                    stroke(shape, pts, half, is_closed, stroke_joins);
                  });
      }
    }
  }
  flush();
  return steps;
}

void SceneRenderer::prepare_drawings(const Document &doc)
{
  /* Found as the render would draw them (and their modifiers worked out, as it would), nothing drawn. */
  preparing_ = true;
  jobs_.clear();
  queued_.clear();
  for (const Layer *l : document_layers_flat(doc)) {
    if (l->is_group() || l->is_media() || !l->has_drawing() || !layer_shown_through_parents(doc, *l)) {
      continue;
    }
    if ((l->opacity <= 0.0f && !full_opacity(*l)) || skip_prepare_.count(l->id)) {
      continue;
    }
    draw_drawing(doc, *l, layer_world_transform(doc, l->id), 1.0f);
  }
  /* The masks' shapes too (a render worked out with masks' layers moving, or bent, makes many). */
  if (mask_preview_layer_ == 0) {
    dry_masks_ = true;
    prepared_masks_.clear();
    prepare_masks(doc, doc.layers);
    dry_masks_ = false;
  }
  preparing_ = false;
  /* Those with nothing to stand in for them first, all of them; then those that could be drawn as made for another
   * zoom (a zoom just rested), a few at a time over the cores while this render has time for them: the rest stand in
   * and are made over the next renders, so a big drawing sharpens over a few frames rather than in one long one. */
  std::stable_partition(jobs_.begin(), jobs_.end(), [](const Job &job) { return !job.optional; });
  const size_t needed = size_t(std::count_if(jobs_.begin(), jobs_.end(), [](const Job &job) { return !job.optional; }));
  /* With what each step covers (layer_region asks), worked out on the thread making it. */
  const auto make = [&](Job &job) {
    job.steps = make_steps(job.args);
    if (!job.args.coverage) {
      for (PaintStep &step : job.steps) {
        step_bounds(step);
      }
    }
  };
  parallel_for(needed, 1, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      make(jobs_[i]);
    }
  });
  constexpr double OPTIONAL_MS = 8.0;
  const auto optional_start = std::chrono::steady_clock::now();
  const size_t piece = size_t(parallel_threads()) * 2;
  size_t made = needed;
  while (made < jobs_.size() &&
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - optional_start).count() <
             OPTIONAL_MS)
  {
    const size_t end = std::min(made + piece, jobs_.size());
    parallel_for(end - made, 1, [&](size_t begin, size_t stop) {
      for (size_t i = made + begin; i < made + stop; i++) {
        make(jobs_[i]);
      }
    });
    made = end;
  }
  for (size_t i = made; i < jobs_.size(); i++) {
    deferred_.insert(jobs_[i].key);
  }
  jobs_.resize(made);
  for (Job &job : jobs_) {
    tessellated_[job.key] = {std::move(job.steps), render_serial_};
    tessellated_any_zoom_[job.any_zoom] = job.key;
    if (job.movable) {
      tessellated_anywhere_[job.anywhere] = {job.key, job.origin};
    }
  }
  jobs_.clear();
}

void SceneRenderer::prepare_masks(const Document &doc, const std::vector<Layer> &layers)
{
  for (const DrawStep &step : draw_order_steps(doc, layers, [this](const Layer &l) { return shown(l); })) {
    for (const Layer *masked : step.masked) {
      if (!prepared_masks_.insert(masked->id).second) {
        continue;
      }
      /* As apply_masks works a level out first: in the lowest bit, the bits above it free. */
      for (const CardRows &card : kept_cards(doc, *masked, final_render_)) {
        evaluate_mask_rows(doc, card.steps, 1u, MASK_SCRATCH, false, (MASK_SPARE | (MASK_SPARE - 1u)) & ~1u);
      }
    }
    /* A faded group's layers, drawn in its own image under their own masks. */
    if (step.layer->is_group()) {
      prepare_masks(doc, step.layer->children);
    }
  }
}

void SceneRenderer::draw_drawing(const Document &doc, const Layer &layer, const Transform2D &layer_transform,
                                 float opacity)
{
  /* Modifiers (Mirror, the bones) draw from an evaluated copy, made once per render; otherwise the
   * drawing itself. A drawing the bones carry as one piece is drawn moved instead, so turning a bone
   * draws again what was made of it before rather than making it anew. */
  /* While only the view has changed since the last render (set_content), what this layer's key holds but the view is
   * as it was: kept (by the pass too: a mask's coverage keys apart). */
  const uint64_t pass = hash_float(
      hash_mix(layer.id, uint64_t(coverage_pass_) | uint64_t(mask_pass_.stage) << 1 |
                             uint64_t(mask_pass_.exclude_strokes) << 4 | uint64_t(mask_pass_.filled) << 5 |
                             uint64_t(mask_pass_.extend) << 6 | uint64_t(mask_pass_.exclude_fills) << 9 |
                             uint64_t(mask_pass_.inside_stroke) << 10),
      coverage_pass_ ? mask_pass_.grow : 0.0f);
  KeyMemo *memo = memo_stamp_ != 0 ? &key_memo_[pass] : nullptr;
  const bool memo_hit = memo && memo->made;
  const bool modified = memo_hit ? memo->modified : layer_has_modifier_effect(doc, layer, final_render_);
  const VectorDrawing *source = &layer_drawing(doc, layer);
  Transform2D rigid;
  if (modified) {
    auto it = evaluated_.find(layer.id);
    if (it == evaluated_.end()) {
      Evaluated e;
      {
        const StatTimer stat_evaluate(STAT_EVALUATE);
        e.drawing = layer_evaluated_drawing(doc, layer, final_render_, &e.rigid);
      }
      it = evaluated_.emplace(layer.id, std::move(e)).first;
    }
    source = &it->second.drawing;
    rigid = it->second.rigid;
  }
  const Transform2D transform = layer_transform * rigid;
  const VectorDrawing &drawing = *source;
  if (drawing.shapes.empty()) {
    return;
  }
  const Transform2D to_pixels = document_to_pixels_ * transform;
  /* One pixel in layer space. Edges are hard: multisampling smooths them, and a soft edge
   * strip would bring back seams where edges meet. */
  /* Zooming reuses what was drawn: the pixel size goes in steps (a band of zooms for most drawings, 2^(1/8) for the
   * rest; below, once what the drawing has is known), so a zoom redoes a drawing's triangles and stamps only now and
   * then, not on each wheel notch. */
  const float pixel_log2 = std::log2(1.0f / std::max(to_pixels.scale(), 1e-6f));

  /* What it's drawn as is kept, and drawn again while what makes it stays the same. Brushed lines
   * add their brushes (a brush that isn't there draws a solid line, so it counts once it's found),
   * the setup shape their stamps are laid out on, and with line boil the frame. */
  uint64_t key = 0, settings = 0;
  bool brushed = false, boils = false;
  LineCutter own_cutter;
  if (memo_hit) {
    key = memo->any_zoom;
    settings = memo->settings;
    brushed = memo->brushed;
    boils = memo->boils;
  }
  else {
    key = hash_mix(0x9e3779b97f4a7c15ull, layer.id);
    for (const Shape &s : drawing.shapes) {
      const bool has_brush = s.has_outline && !s.style.brush.name.empty() && brush_find(s.style.brush.name);
      key = hash_mix(key, has_brush ? 1 : 0);
      if (has_brush) {
        brushed = true;
        boils = boils || s.style.brush.randomize;
        key = brush_settings_hash(key, s.style.brush);
      }
    }
    if (brushed) {
      key = hash_mix(key, setup_hash(layer.drawing));
      key = hash_mix(key, uint64_t(final_render_));
      if (boils) {
        key = hash_mix(key, uint64_t(uint32_t(doc.frame_current)));
      }
    }
    key = hash_mix(key, uint64_t(quality_) | uint64_t(gpu_geometry_ && !coverage_pass_) << 8);
    key = hash_mix(key, uint64_t(coverage_pass_) | uint64_t(mask_pass_.stage) << 1 |
                            uint64_t(mask_pass_.exclude_strokes) << 4 | uint64_t(mask_pass_.filled) << 5 |
                            uint64_t(mask_pass_.extend) << 6 | uint64_t(mask_pass_.exclude_fills) << 9 |
                            uint64_t(mask_pass_.inside_stroke) << 10);
    key = hash_float(key, coverage_pass_ ? mask_pass_.grow : 0.0f);
    /* End Lines at Edge: where the masks' rows with it on let the lines show (in the colour pass; a
     * layer lends its shapes to masks as drawn). Its shapes and where this layer is go in the key. */
    if (!coverage_pass_) {
      own_cutter = line_cutter(doc, layer, final_render_);
    }
    if (!own_cutter.empty()) {
      key = hash_float(hash_mix(key, own_cutter.flat ? 0x5eedu : 0xf1a7u), own_cutter.reach);
      for (const auto &rows : own_cutter.levels) {
        for (const LineCutter::Row &row : rows) {
          key = hash_mix(key, uint64_t(row.mode) | uint64_t(row.invert) << 4 | uint64_t(row.fills_only) << 5 |
                                  uint64_t(row.strokes_only) << 6 | uint64_t(row.inside_stroke) << 7 |
                                  uint64_t(row.combine) << 8);
          for (const LineCutter::Piece &piece : row.pieces) {
            key = hash_mix(key, uint64_t(piece.area) | uint64_t(piece.line) << 1 | uint64_t(piece.closed) << 2 |
                                    uint64_t(piece.source) << 3);
            key = hash_float(key, piece.half);
            for (const float2 &p : piece.outline) {
              key = hash_float(hash_float(key, p.x), p.y);
            }
          }
        }
      }
      for (const float v : {transform.apply({0.0f, 0.0f}).x, transform.apply({0.0f, 0.0f}).y,
                            transform.apply({1.0f, 0.0f}).x, transform.apply({1.0f, 0.0f}).y,
                            transform.apply({0.0f, 1.0f}).x, transform.apply({0.0f, 1.0f}).y})
      {
        key = hash_float(key, v);
      }
    }
    /* Everything but the drawing's shape and place, then those. */
    settings = key;
    key = hash_mix(key, geometry_hash(layer, drawing, modified));
  }
  if (memo && !memo_hit) {
    *memo = {true, modified, brushed, boils, settings, key, std::move(own_cutter)};
  }
  const LineCutter &cutter = memo ? memo->cutter : own_cutter;
  (void)boils;
  /* A colour drawing comes out the same at every zoom but for how finely its curves are cut where handles stretch
   * them, and how far a stencil fill's cover reaches (a hairline is the GPU's, Vertex): in the viewport it is made
   * for a band of zooms ZOOM_BAND_OCTAVES wide, cut for the band's nearest and covered for its farthest, and drawn as
   * it is across the band, so a zoom remakes nothing until it leaves the band. Brushed lines (their dabs' spacing),
   * lines ended at masks' edges, masks' shapes (grown by pixels) and final renders go in steps of 2^(1/8). */
  const bool banded = interactive_ && !final_render_ && !coverage_pass_ && !brushed && cutter.empty();
  const float steps_per_octave = banded ? 1.0f / ZOOM_BAND_OCTAVES : 8.0f;
  const float pixel = std::exp2(std::round(pixel_log2 * steps_per_octave) / steps_per_octave);
  const uint64_t any_zoom = key;
  key = hash_float(key, pixel);
  if (!coverage_pass_) {
    uint64_t drawn = key;
    for (const float v : {transform.a, transform.b, transform.c, transform.d, transform.tx, transform.ty}) {
      drawn = hash_float(drawn, v);
    }
    drawn_signature_[layer.id] = drawn;
  }
  if (auto it = tessellated_.find(key); it != tessellated_.end()) {
    if (preparing_) {
      if (!coverage_pass_) {
        drawn_[layer.id] = {key, transform};
      }
      return;
    }
    it->second.used = render_serial_;
    tessellated_any_zoom_[any_zoom] = key;
    draw_steps(it->second.steps, transform, opacity, key);
    return;
  }
  /* Moved whole (every point the same way, as when all of it is dragged): what was made of it before,
   * moved. Not with lines ended at masks' edges or brushed ones, which depend on where it is. A 16th of a
   * pixel apart is the same. */
  const bool movable = cutter.empty() && !brushed && !drawing.points.empty();
  uint64_t anywhere = 0;
  if (movable) {
    if (memo && memo->anywhere_pixel == pixel) {
      anywhere = memo->anywhere;
    }
    else {
      anywhere = hash_mix(hash_float(settings, pixel), drawing_geometry_hash(drawing, pixel / 16.0f));
      if (memo) {
        memo->anywhere = anywhere;
        memo->anywhere_pixel = pixel;
      }
    }
  }
  if (movable) {
    if (auto placed = tessellated_anywhere_.find(anywhere); placed != tessellated_anywhere_.end()) {
      if (auto it = tessellated_.find(placed->second.key); it != tessellated_.end()) {
        if (preparing_) {
          if (!coverage_pass_) {
            drawn_[layer.id] = {placed->second.key,
                                transform * Transform2D::translate(drawing.points[0].pos - placed->second.origin)};
          }
          return;
        }
        it->second.used = render_serial_;
        draw_steps(it->second.steps, transform * Transform2D::translate(drawing.points[0].pos - placed->second.origin),
                   opacity, placed->second.key);
        return;
      }
    }
  }
  /* Zooming: once this frame's time is used, what was made for another zoom stands in (the same
   * shapes, edges a little finer or coarser). */
  constexpr double budget_ms = 6.0;
  if (preparing_) {
    /* Brushed lines and lines ended at masks' edges are made as they are drawn. Everything else is made here, a new
     * zoom's too, once the zoom rests; while it moves (each step drawn, Zoom Preview off) a drawing made for another
     * zoom stands in as it is (draw_drawing). */
    if (brushed || !cutter.empty()) {
      return;
    }
    if (zooming_ && interactive_ && !final_render_ && tessellated_any_zoom_.count(any_zoom)) {
      if (!coverage_pass_) {
        drawn_[layer.id] = {key, transform, any_zoom};
      }
      return;
    }
    /* A mask's shape asked for by more than one row (or card) is made once. */
    if (!queued_.insert(key).second) {
      return;
    }
    Job job;
    job.args = {&doc,   &layer, &drawing, transform, pixel, cutter, &shapes_kept_[hash_float(settings, pixel)],
                coverage_pass_, mask_pass_, banded};
    job.args.kept->used = render_serial_;
    job.optional = interactive_ && !final_render_ && tessellated_any_zoom_.count(any_zoom) != 0;
    job.key = key;
    job.any_zoom = any_zoom;
    job.anywhere = anywhere;
    job.movable = movable;
    job.origin = movable ? drawing.points[0].pos : float2();
    jobs_.push_back(std::move(job));
    if (!coverage_pass_) {
      drawn_[layer.id] = {key, transform, any_zoom};
    }
    return;
  }
  if (interactive_ && !final_render_ && (made_ms_ > budget_ms || zooming_ || deferred_.count(key))) {
    if (auto other = tessellated_any_zoom_.find(any_zoom); other != tessellated_any_zoom_.end()) {
      if (auto it = tessellated_.find(other->second); it != tessellated_.end()) {
        complete_ = false;
        it->second.used = render_serial_;
        draw_steps(it->second.steps, transform, opacity, other->second);
        return;
      }
    }
  }
  const auto making = std::chrono::steady_clock::now();
  std::vector<PaintStep> steps = make_steps({&doc, &layer, &drawing, transform, pixel, cutter,
                                             &shapes_kept_[hash_float(settings, pixel)], coverage_pass_, mask_pass_,
                                             banded});
  made_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - making).count();
  shapes_kept_[hash_float(settings, pixel)].used = render_serial_;
  /* Made here, kept in tessellated_ (the steps keep their place in memory as the vector moves there). */
  draw_steps(steps, transform, opacity, key);
  tessellated_[key] = {std::move(steps), render_serial_};
  tessellated_any_zoom_[any_zoom] = key;
  if (tessellated_any_zoom_.size() > 4 * tessellated_.size() + 256) {
    tessellated_any_zoom_.clear();
  }
  if (movable) {
    tessellated_anywhere_[anywhere] = {key, drawing.points[0].pos};
    if (tessellated_anywhere_.size() > 4 * tessellated_.size() + 256) {
      tessellated_anywhere_.clear();
    }
  }
}

/* A batched vertex: a tessellated one and its step's slot in the chunk. */

void SceneRenderer::draw_stencil_fill(PaintStep &step, const Transform2D &step_to_clip, float opacity,
                                      unsigned required)
{
  flush_batch();
  if (!step.mesh) {
    step.mesh = device_->make_mesh(gpu::Layout::Color, step.vertices.data(), step.vertices.size() * sizeof(Vertex),
                                   step.fan_indices.data(), step.fan_indices.size());
  }
  const int cover = int(step.vertices.size()) - 6;
  /* Masks worked out since the target was cleared leave their scratch where the count goes: cleared under the cover
   * (all a fill can reach), not the whole image, which at 16 samples costs more than most fills. */
  if (bound_->fan_dirty) {
    gpu::Draw clear;
    clear.state.write_color = false;
    clear.state.stencil = {true,
                           gpu::Compare::Always,
                           0,
                           0,
                           gpu::StencilOp::Zero,
                           gpu::StencilOp::Zero,
                           stencil_bits(WINDING_BITS)};
    clear.mesh = step.mesh;
    clear.first = cover;
    clear.count = 6;
    clear.to_clip = step_to_clip;
    device_->draw(clear);
  }
  /* The winding counted where the masks show the fill (elsewhere it stays 0, so the cover needn't test them too). */
  gpu::Draw fan;
  fan.state = color_state(required, depth_required_);
  fan.state.write_color = false;
  fan.state.stencil.write_mask = stencil_bits(WINDING_BITS);
  fan.state.stencil.winding = true;
  fan.mesh = step.mesh;
  fan.indexed = true;
  fan.count = int(step.fan_indices.size());
  fan.to_clip = step_to_clip;
  device_->draw(fan);
  /* The colour where the count isn't 0 (or is odd), each sample once; the count cleared again all over the cover. */
  gpu::Draw draw;
  draw.state.stencil = {true,
                        gpu::Compare::NotEqual,
                        0,
                        stencil_bits(step.even_odd ? WINDING_ODD : WINDING_BITS),
                        gpu::StencilOp::Zero,
                        gpu::StencilOp::Zero,
                        stencil_bits(WINDING_BITS)};
  draw.mesh = step.mesh;
  draw.first = cover;
  draw.count = 6;
  draw.to_clip = step_to_clip;
  draw.opacity = opacity;
  device_->draw(draw);
}

void SceneRenderer::flush_batch()
{
  if (batch_.empty()) {
    return;
  }
  constexpr size_t SLOTS = 32;
  std::vector<gpu::BatchVertex> vertices;
  float slots[SLOTS * 8];
  for (size_t i = 0; i < batch_.size();) {
    const unsigned required = batch_[i].required, depth = batch_[i].depth;
    size_t j = i;
    uint64_t chunk_key = 1469598103934665603ull;
    while (j < batch_.size() && j - i < SLOTS && batch_[j].required == required && batch_[j].depth == depth) {
      chunk_key = (chunk_key ^ batch_[j].id) * 1099511628211ull;
      j++;
    }
    Chunk &chunk = chunks_[chunk_key];
    chunk.used = render_serial_;
    if (!chunk.mesh) {
      vertices.clear();
      for (size_t k = i; k < j; k++) {
        for (const Vertex &v : batch_[k].step->vertices) {
          vertices.push_back({v.x, v.y, v.rgba, v.ox, v.oy, v.w, uint16_t(k - i)});
        }
      }
      chunk.mesh = device_->make_mesh(gpu::Layout::Batch, vertices.data(), vertices.size() * sizeof(gpu::BatchVertex));
      chunk.vertices = int(vertices.size());
    }
    for (size_t k = i; k < j; k++) {
      const Batched &b = batch_[k];
      float *slot = slots + (k - i) * 8;
      slot[0] = b.to_clip.a;
      slot[1] = b.to_clip.b;
      slot[2] = b.to_clip.c;
      slot[3] = b.to_clip.d;
      slot[4] = b.to_clip.tx;
      slot[5] = b.to_clip.ty;
      slot[6] = b.opacity;
      slot[7] = 0.0f;
    }
    gpu::Draw draw;
    draw.state = color_state(required, depth);
    draw.state.program = gpu::Program::Batch;
    draw.mesh = chunk.mesh;
    draw.count = chunk.vertices;
    draw.slots = slots;
    draw.slot_count = int(j - i);
    device_->draw(draw);
    i = j;
  }
  batch_.clear();
}

Transform2D SceneRenderer::to_clip(const Transform2D &transform) const
{
  return Transform2D::translate({-1.0f, -1.0f}) * Transform2D::scaling({2.0f / float(width_), 2.0f / float(height_)}) *
         document_to_pixels_ * transform;
}

gpu::Texture SceneRenderer::brush_texture(const BrushImage &image)
{
  if (auto it = brush_textures_.find(image.id); it != brush_textures_.end()) {
    return it->second;
  }
  const gpu::Texture texture = device_->make_texture();
  device_->upload_texture(texture, image.width, image.height, image.rgba.data());
  brush_textures_[image.id] = texture;
  return texture;
}

void SceneRenderer::free_step(PaintStep &step)
{
  device_->free_mesh(step.mesh);
  for (gpu::Mesh &mesh : step.dab_meshes) {
    device_->free_mesh(mesh);
  }
  step.dab_meshes.clear();
}

void SceneRenderer::draw_dabs(const BrushDabs &dabs, const std::vector<BrushImage> &images, bool own_colors,
                              bool merged, const Transform2D &transform, float opacity, std::vector<gpu::Mesh> *meshes)
{
  flush_batch();
  const Target *outer = bound_;
  Target *own = nullptr;
  gpu::Draw draw;
  draw.state = paint_;
  /* Merged alpha: the dabs go into an image of their own, keeping the strongest one where
   * they overlap, which is then laid over the layer. (Mask coverage needs no merging.) */
  if (merged && !coverage_pass_ && outer) {
    own = acquire_target();
    bind_target(own);
    clear(Color{0, 0, 0, 0});
    draw.state = gpu::DrawState();
    draw.state.stencil.test = false;
    draw.state.blend = gpu::Blend::Max;
  }
  draw.state.program = gpu::Program::Brush;
  draw.to_clip = to_clip(transform);
  draw.opacity = own ? 1.0f : opacity;
  draw.own_colors = own_colors;
  draw.alpha_test = coverage_pass_ ? coverage_alpha_ : 0.0f;
  if (meshes) {
    meshes->resize(dabs.per_image.size());
  }
  for (size_t i = 0; i < dabs.per_image.size() && i < images.size(); i++) {
    const std::vector<BrushVertex> &list = dabs.per_image[i];
    if (list.empty()) {
      continue;
    }
    draw.texture = brush_texture(images[i]);
    if (meshes) {
      if (!(*meshes)[i]) {
        (*meshes)[i] = device_->make_mesh(gpu::Layout::Brush, list.data(), list.size() * sizeof(BrushVertex));
      }
      draw.mesh = (*meshes)[i];
    }
    else {
      draw.stream = list.data();
      draw.stream_bytes = list.size() * sizeof(BrushVertex);
      draw.stream_layout = gpu::Layout::Brush;
    }
    draw.count = int(list.size());
    device_->draw(draw);
  }
  if (own) {
    resolve(own);
    bind_target(outer);
    use_color_stencil();
    composite(own, opacity);
    release_target(own);
  }
}

void SceneRenderer::draw_media(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity)
{
  flush_batch();
  const MediaShown shown_now = media_shown_at(doc, layer, doc.frame_current);
  if (!shown_now.clip || opacity <= 0.0f) {
    return;
  }
  const MediaClip &clip = *shown_now.clip;
  const std::string file = media_clip_file(clip);
  /* A render waits for the exact frame; the viewport shows the nearest one until it comes. */
  const MediaFramePtr frame = media_frame(file, clip.sequence, shown_now.seconds, final_render_ || wait_for_media_);
  if (!frame || frame->rgba.empty()) {
    return;
  }
  MediaTexture &tex = media_textures_[layer.id];
  if (!tex.texture) {
    tex.texture = device_->make_texture();
  }
  if (tex.serial != frame->serial) {
    device_->upload_texture(tex.texture, frame->width, frame->height, frame->rgba.data());
    tex.serial = frame->serial;
  }
  const MediaInfo info = media_clip_info(clip);
  float2 q[4];
  media_quad(doc, layer, info.source_width > 0 ? info.source_width : frame->width,
             info.source_height > 0 ? info.source_height : frame->height, q);
  /* Texture rows run top first: v 0 is the top edge. */
  const BrushVertex v[6] = {{q[0].x, q[0].y, 0, 1, 1, 1, 1, 1}, {q[1].x, q[1].y, 1, 1, 1, 1, 1, 1},
                            {q[2].x, q[2].y, 1, 0, 1, 1, 1, 1}, {q[0].x, q[0].y, 0, 1, 1, 1, 1, 1},
                            {q[2].x, q[2].y, 1, 0, 1, 1, 1, 1}, {q[3].x, q[3].y, 0, 0, 1, 1, 1, 1}};
  gpu::Draw draw;
  draw.state = paint_;
  draw.state.program = gpu::Program::Media;
  draw.to_clip = to_clip(transform);
  draw.opacity = opacity;
  draw.grey = layer.media.greyscale ? 1.0f : 0.0f;
  const Color &tint = layer.media.tint;
  draw.tint[0] = tint.r;
  draw.tint[1] = tint.g;
  draw.tint[2] = tint.b;
  draw.tint[3] = layer.media.tint_amount;
  draw.texture = tex.texture;
  draw.stream = v;
  draw.stream_bytes = sizeof(v);
  draw.stream_layout = gpu::Layout::Brush;
  draw.count = 6;
  device_->draw(draw);
}

void SceneRenderer::draw_vertices(const std::vector<Vertex> &vertices, const Transform2D &transform)
{
  flush_batch();
  gpu::Draw draw;
  draw.state = paint_;
  draw.state.program = gpu::Program::Color;
  draw.stream = vertices.data();
  draw.stream_bytes = vertices.size() * sizeof(Vertex);
  draw.stream_layout = gpu::Layout::Color;
  draw.count = int(vertices.size());
  draw.to_clip = to_clip(transform);
  device_->draw(draw);
}

bool SceneRenderer::pan_to(int width, int height, const Transform2D &document_to_pixels)
{
  if (!overscan_ || width != asked_width_ || height != asked_height_ || width_ != width + 2 * margin_ ||
      height_ != height + 2 * margin_ || !output_)
  {
    return false;
  }
  const Transform2D &at = document_to_pixels_;
  const float scale = std::max({std::fabs(at.a), std::fabs(at.b), std::fabs(at.c), std::fabs(at.d), 1e-12f});
  for (const auto &[x, y] : {std::pair{at.a, document_to_pixels.a}, std::pair{at.b, document_to_pixels.b},
                             std::pair{at.c, document_to_pixels.c}, std::pair{at.d, document_to_pixels.d}})
  {
    if (std::fabs(x - y) > scale * 1e-6f) {
      return false;
    }
  }
  /* Where the part asked for starts in the image: whole pixels (a hair off is the same), within the margin. */
  const float dx = at.tx - document_to_pixels.tx, dy = at.ty - document_to_pixels.ty;
  const float rx = std::round(dx), ry = std::round(dy);
  if (std::fabs(dx - rx) > 1e-3f || std::fabs(dy - ry) > 1e-3f || rx < 0.0f || ry < 0.0f ||
      rx > float(2 * margin_) || ry > float(2 * margin_))
  {
    return false;
  }
  uv_[0] = rx / float(width_);
  uv_[1] = ry / float(height_);
  uv_[2] = (rx + float(width)) / float(width_);
  uv_[3] = (ry + float(height)) / float(height_);
  return true;
}

/* --- Layer runs ------------------------------------------------------------------------------------------------ */

static uint64_t hash_transform(uint64_t h, const Transform2D &t)
{
  for (const float v : {t.a, t.b, t.c, t.d, t.tx, t.ty}) {
    h = hash_float(h, v);
  }
  return h;
}

uint64_t SceneRenderer::masks_signature(const Document &doc, const Layer &layer)
{
  uint64_t h = 0x6d61736b73ull;
  for (const CardRows &card : kept_cards(doc, layer, final_render_)) {
    h = hash_mix(h, uint64_t(card.cuts) | uint64_t(card.brush_ends) << 4 | uint64_t(uint32_t(card.index)) << 8);
    for (const MaskRow &row : card.steps) {
      h = hash_mix(h, uint64_t(row.mode) | uint64_t(row.invert) << 4 | uint64_t(row.combine) << 5 |
                          uint64_t(row.fills_only) << 8 | uint64_t(row.strokes_only) << 9 | uint64_t(row.filled) << 10 |
                          uint64_t(row.inside_stroke) << 11 | uint64_t(row.clip) << 12 |
                          uint64_t(row.start_shown) << 13 | uint64_t(uint32_t(row.extend)) << 16);
      h = hash_float(h, row.grow);
      for (const uint64_t id : row.layers) {
        const Layer *source = layer_find(doc, id);
        h = hash_mix(h, source ? layer_signature(doc, *source) : id);
      }
    }
  }
  return h;
}

uint64_t SceneRenderer::layer_signature(const Document &doc, const Layer &layer)
{
  if (const auto it = signature_memo_.find(layer.id); it != signature_memo_.end()) {
    return it->second;
  }
  /* A layer reached again through its own masks' layers: counts as itself only. */
  if (std::find(signature_busy_.begin(), signature_busy_.end(), layer.id) != signature_busy_.end()) {
    return hash_mix(0xb05ull, layer.id);
  }
  signature_busy_.push_back(layer.id);
  uint64_t h = hash_mix(hash_mix(0x51647e5ull, layer.id), uint64_t(layer.type));
  h = hash_mix(h, uint64_t(shown(layer)) | uint64_t(layer.visible) << 1 | uint64_t(full_opacity(layer)) << 2);
  h = hash_float(h, layer.opacity);
  h = hash_transform(h, layer_world_transform(doc, layer.id));
  if (layer.is_group()) {
    for (const Layer &child : layer.children) {
      h = hash_mix(h, layer_signature(doc, child));
    }
  }
  else if (layer.is_media()) {
    /* Its settings, the frame and what has been decoded. */
    h = hash_mix(hash_mix(hash_mix(h, layer.revision.value), uint64_t(uint32_t(doc.frame_current))),
                 media_generation());
  }
  else if (const auto drawn = drawn_signature_.find(layer.id); drawn != drawn_signature_.end()) {
    h = hash_mix(h, drawn->second);
  }
  else if (layer.has_drawing()) {
    /* Not drawn in this render (hidden, a mask's layer): its drawing as it is now. */
    const bool modified = layer_has_modifier_effect(doc, layer, final_render_);
    if (modified) {
      auto it = evaluated_.find(layer.id);
      if (it == evaluated_.end()) {
        Evaluated e;
        {
          const StatTimer stat_evaluate(STAT_EVALUATE);
          e.drawing = layer_evaluated_drawing(doc, layer, final_render_, &e.rigid);
        }
        it = evaluated_.emplace(layer.id, std::move(e)).first;
      }
      h = hash_transform(hash_mix(h, geometry_hash(layer, it->second.drawing, true)), it->second.rigid);
    }
    else {
      h = hash_mix(h, geometry_hash(layer, layer_drawing(doc, layer), false));
    }
  }
  h = hash_mix(h, masks_signature(doc, layer));
  signature_busy_.pop_back();
  signature_memo_[layer.id] = h;
  return h;
}

uint64_t SceneRenderer::step_signature(const Document &doc, const DrawStep &step)
{
  uint64_t h = layer_signature(doc, *step.layer);
  for (const Layer *m : step.masked) {
    if (m != step.layer) {
      h = hash_mix(h, masks_signature(doc, *m));
    }
  }
  h = hash_float(h, layer_opacity_through_parents(doc, *step.layer));
  return h;
}

SceneRenderer::Target *SceneRenderer::make_run_image()
{
  flush_batch();
  if (!run_depth_stencil_) {
    run_depth_stencil_ = device_->make_depth_stencil(width_, height_, target_samples_);
  }
  Target *target = new Target;
  gpu::TargetDesc desc;
  desc.width = width_;
  desc.height = height_;
  desc.samples = target_samples_;
  desc.resolved = false;
  desc.depth_stencil = run_depth_stencil_;
  target->target = device_->make_target(desc);
  return target;
}

void SceneRenderer::free_run_image(Target *target)
{
  if (!target) {
    return;
  }
  device_->free_target(target->target);
  delete target;
}

void SceneRenderer::free_runs()
{
  for (LayerRun &run : runs_) {
    free_run_image(run.image);
  }
  runs_.clear();
  run_layers_.clear();
  if (run_depth_stencil_) {
    device_->free_depth_stencil(run_depth_stencil_);
  }
}

void SceneRenderer::composite_run(const Target *run)
{
  flush_batch();
  gpu::Draw draw;
  draw.state.program = gpu::Program::Run;
  draw.texture = device_->sample_texture(run->target);
  draw.count = 3;
  device_->draw(draw);
}

uint64_t SceneRenderer::runs_view() const
{
  uint64_t view = hash_transform(0x76696577ull, document_to_pixels_);
  return hash_mix(hash_mix(hash_mix(view, uint64_t(width_) << 32 | uint64_t(height_)), uint64_t(samples_)),
                  uint64_t(quality_) | uint64_t(show_guides_) << 8 | uint64_t(media_view_) << 9 |
                      uint64_t(gpu_geometry_) << 10);
}

std::vector<char> SceneRenderer::clean_steps(const Document &doc, const std::vector<DrawStep> &steps, uint64_t view)
{
  std::vector<char> clean(steps.size(), 0);
  /* The layers with a new revision since the last render. */
  std::set<uint64_t> changed;
  for (const Layer *l : document_layers_flat(doc)) {
    uint64_t &seen = seen_revisions_[l->id];
    if (seen != l->revision.value) {
      changed.insert(l->id);
      seen = l->revision.value;
    }
  }
  constexpr size_t CHANGED_MAX = 16;
  constexpr int CHECK_EVERY = 30;
  if (runs_.empty() || view != run_view_ || changed.size() > CHANGED_MAX || ++clean_renders_ >= CHECK_EVERY ||
      dynamics_live_running() || steps.size() != run_layers_.size() || last_layers_ != run_layers_)
  {
    clean_renders_ = 0;
    return clean;
  }
  /* Which steps lie in a run (the rest are drawn live anyway). */
  std::vector<char> in_run(steps.size(), 0);
  for (const LayerRun &run : runs_) {
    for (size_t i = run.begin; i < run.end && i < steps.size(); i++) {
      in_run[i] = 1;
    }
  }
  for (size_t i = 0; i < steps.size(); i++) {
    const Layer &layer = *steps[i].layer;
    if (!in_run[i] || layer.id != run_layers_[i] || !steps[i].masked.empty() || layer.is_group() || layer.is_media() ||
        layer.is_reference() || !layer.has_drawing())
    {
      continue;
    }
    bool plain = true;
    for (const Modifier &m : layer.modifiers) {
      plain = plain && m.type == ModifierType::Armature;
    }
    /* (layer_parent only reads the document; it takes it as it is for those who change what it finds.) */
    Document &read = const_cast<Document &>(doc);
    for (const Layer *p = &layer; p && plain; p = layer_parent(read, p->id)) {
      plain = !changed.count(p->id) && p->masks.empty();
    }
    clean[i] = plain;
  }
  return clean;
}

void SceneRenderer::draw_layers_in_runs(const Document &doc, const std::vector<DrawStep> &steps,
                                        const std::vector<char> &clean)
{
  /* At most this many drawn live: more, and drawing everything is as quick and needs no images. */
  constexpr size_t LIVE_MAX = 4;
  const uint64_t view = runs_view();
  /* The view has just moved (a zoom, a pan past the margin): every step drawn as it is, with no image made of it (the
   * next frame most likely moves the view again) and no signatures worked out. The first frame at a view that holds
   * makes the runs (last_layers_ forgotten: one run of everything). */
  if (view != last_view_) {
    free_runs();
    for (const DrawStep &step : steps) {
      draw_step(doc, step, output_);
    }
    last_layers_.clear();
    last_view_ = view;
    return;
  }
  std::vector<uint64_t> layers(steps.size()), signatures(steps.size());
  for (size_t i = 0; i < steps.size(); i++) {
    layers[i] = steps[i].layer->id;
    signatures[i] = clean[i] ? last_signatures_[i] : step_signature(doc, steps[i]);
  }

  const auto draw_all = [&] {
    for (const DrawStep &step : steps) {
      draw_step(doc, step, output_);
    }
  };
  /* The runs kept still hold: draw them and what's live between them. */
  bool held = !runs_.empty() && view == run_view_ && layers == run_layers_;
  for (size_t r = 0; held && r < runs_.size(); r++) {
    const LayerRun &run = runs_[r];
    held = std::equal(run.signatures.begin(), run.signatures.end(), signatures.begin() + std::ptrdiff_t(run.begin));
  }
  if (!held) {
    free_runs();
    /* Live: those changed since the last render (the same layers in the same view). */
    std::vector<size_t> live;
    if (view == last_view_ && layers == last_layers_) {
      for (size_t i = 0; i < steps.size() && live.size() <= LIVE_MAX; i++) {
        if (signatures[i] != last_signatures_[i]) {
          live.push_back(i);
        }
      }
    }
    const bool again = live.empty() || live == last_live_;
    last_live_ = live;
    if (live.size() > LIVE_MAX || steps.empty() || !again) {
      draw_all();
      last_layers_ = layers;
      last_signatures_ = signatures;
      last_view_ = view;
      return;
    }
    /* Each stretch between them a run, its image made now (with none live, one run of everything: a frame drawn
     * again unchanged, a pan within the margin, lays down that image alone). */
    size_t begin = 0;
    for (size_t k = 0; k <= live.size(); k++) {
      const size_t end = k < live.size() ? live[k] : steps.size();
      if (end > begin) {
        LayerRun run;
        run.begin = begin;
        run.end = end;
        run.signatures.assign(signatures.begin() + std::ptrdiff_t(begin), signatures.begin() + std::ptrdiff_t(end));
        run.image = make_run_image();
        bind_target(run.image);
        mask_clauses_.clear();
        clear(Color{0, 0, 0, 0});
        for (size_t i = begin; i < end; i++) {
          draw_step(doc, steps[i], run.image);
        }
        flush_batch();
        mask_clauses_.clear();
        runs_.push_back(std::move(run));
      }
      begin = end + 1;
    }
    run_layers_ = layers;
    run_view_ = view;
    bind_target(output_);
  }
  /* Bottom first: each run's image, and each live step drawn as it is. */
  size_t r = 0;
  for (size_t i = 0; i < steps.size();) {
    if (r < runs_.size() && runs_[r].begin == i) {
      composite_run(runs_[r].image);
      /* The stencil holds no masks the live steps can reuse. */
      mask_clauses_.clear();
      i = runs_[r].end;
      r++;
      continue;
    }
    draw_step(doc, steps[i], output_);
    i++;
  }
  last_layers_ = layers;
  last_signatures_ = signatures;
  last_view_ = view;
  /* A drawing drawn as made for another zoom (still being made) mustn't stay in a run's image. */
  if (!complete_) {
    free_runs();
    last_layers_.clear();
  }
}

void SceneRenderer::composite(const Target *source, float opacity)
{
  flush_batch();
  gpu::Draw draw;
  draw.state = paint_;
  draw.state.program = gpu::Program::Composite;
  draw.texture = device_->resolved_texture(source->target);
  draw.opacity = opacity;
  draw.count = 3;
  device_->draw(draw);
}

}  // namespace oanim
