#include "gpu/scene_renderer.hh"

#include <algorithm>
#include <cstring>
#include <functional>
#include <string>

#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "anim/layer_pick.hh"
#include "assets/brushes.hh"
#include "core/log.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "data/media_layer.hh"
#include "gpu/brush_stroke.hh"
#include "gpu/gl.hh"
#include "gpu/tessellate.hh"

namespace oanim {

static const char *COLOR_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
uniform mat3 u_matrix;
out vec4 v_color;
void main()
{
  vec3 p = u_matrix * vec3(a_pos, 1.0);
  gl_Position = vec4(p.xy, 0.0, 1.0);
  v_color = a_color;
}
)";

/* Batched steps (see flush_batch): each vertex's slot picks its step's place (clip space) and opacity. */
static const char *BATCH_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in float a_slot;
uniform vec4 u_slots[64];
out vec4 v_color;
void main()
{
  int s = int(a_slot + 0.5) * 2;
  vec4 m = u_slots[s];
  vec4 t = u_slots[s + 1];
  gl_Position = vec4(m.x * a_pos.x + m.z * a_pos.y + t.x, m.y * a_pos.x + m.w * a_pos.y + t.y, 0.0, 1.0);
  v_color = a_color * t.z;
}
)";

static const char *BATCH_FRAGMENT_SHADER = R"(#version 330 core
in vec4 v_color;
out vec4 frag_color;
void main()
{
  frag_color = v_color;
}
)";

static const char *COLOR_FRAGMENT_SHADER = R"(#version 330 core
in vec4 v_color;
uniform float u_opacity;
out vec4 frag_color;
void main()
{
  frag_color = v_color * u_opacity;
}
)";

/* Brush dabs: a textured quad each. Ink brushes are white with coverage as alpha, so the dab's
 * colour tints them; a colour brush in its own colours keeps the texture's colour. */
static const char *BRUSH_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
uniform mat3 u_matrix;
out vec2 v_uv;
out vec4 v_color;
void main()
{
  vec3 p = u_matrix * vec3(a_pos, 1.0);
  gl_Position = vec4(p.xy, 0.0, 1.0);
  v_uv = a_uv;
  v_color = a_color;
}
)";

static const char *BRUSH_FRAGMENT_SHADER = R"(#version 330 core
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_image;
uniform float u_opacity;
uniform float u_own_colors;
uniform float u_alpha_test;
out vec4 frag_color;
void main()
{
  vec4 t = texture(u_image, v_uv);
  vec4 c = mix(v_color * t.a, t * v_color.a, u_own_colors);
  if (c.a < u_alpha_test) {
    discard;
  }
  frag_color = c * u_opacity;
}
)";

/* A Media layer's picture (premultiplied): greyscale and the light table's tint work on its
 * colours, the dark parts taking the tint and the light ones staying white, as blue-line pencil. */
static const char *MEDIA_FRAGMENT_SHADER = R"(#version 330 core
in vec2 v_uv;
in vec4 v_color;
uniform sampler2D u_image;
uniform float u_opacity;
uniform float u_grey;
uniform vec4 u_tint;
out vec4 frag_color;
void main()
{
  vec4 t = texture(u_image, v_uv);
  vec3 c = t.a > 0.0 ? t.rgb / t.a : vec3(0.0);
  float l = dot(c, vec3(0.299, 0.587, 0.114));
  c = mix(c, vec3(l), u_grey);
  c = mix(c, mix(u_tint.rgb, vec3(1.0), l), u_tint.a);
  frag_color = vec4(c * t.a, t.a) * u_opacity;
}
)";

/* A full-screen triangle from gl_VertexID. */
static const char *COMPOSITE_VERTEX_SHADER = R"(#version 330 core
out vec2 v_uv;
void main()
{
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

static const char *COMPOSITE_FRAGMENT_SHADER = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_source;
uniform float u_opacity;
out vec4 frag_color;
void main()
{
  frag_color = texture(u_source, v_uv) * u_opacity;
}
)";

/* Stencil-only passes write no colour. */
static const char *STENCIL_FRAGMENT_SHADER = R"(#version 330 core
out vec4 frag_color;
void main()
{
  frag_color = vec4(0.0);
}
)";

static unsigned int compile(GLenum type, const char *source)
{
  const GLuint shader = gl::CreateShader(type);
  gl::ShaderSource(shader, 1, &source, nullptr);
  gl::CompileShader(shader);
  GLint ok = 0;
  gl::GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    gl::GetShaderInfoLog(shader, sizeof(log), nullptr, log);
    report(ReportType::Error, std::string("Shader error: ") + log);
  }
  return shader;
}

static unsigned int link(const char *vertex, const char *fragment)
{
  const GLuint program = gl::CreateProgram();
  const GLuint vs = compile(GL_VERTEX_SHADER, vertex);
  const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
  gl::AttachShader(program, vs);
  gl::AttachShader(program, fs);
  gl::LinkProgram(program);
  gl::DeleteShader(vs);
  gl::DeleteShader(fs);
  GLint ok = 0;
  gl::GetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024];
    gl::GetProgramInfoLog(program, sizeof(log), nullptr, log);
    report(ReportType::Error, std::string("Shader link error: ") + log);
    return 0;
  }
  return program;
}

/* Premultiplied "over". */
static void set_blend_over()
{
  glEnable(GL_BLEND);
  gl::BlendEquation(GL_FUNC_ADD);
  gl::BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

SceneRenderer::~SceneRenderer()
{
  if (gl::DeleteFramebuffers) {
    free_targets();
  }
  if (initialized_ && gl::DeleteBuffers) {
    for (auto &[key, kept] : tessellated_) {
      for (PaintStep &step : kept.steps) {
        free_step(step);
      }
    }
  }
  if (initialized_ && gl::DeleteProgram) {
    gl::DeleteProgram(color_program_);
    gl::DeleteProgram(composite_program_);
    gl::DeleteProgram(stencil_program_);
    gl::DeleteProgram(brush_program_);
    gl::DeleteProgram(media_program_);
    for (const auto &[id, texture] : brush_textures_) {
      glDeleteTextures(1, &texture);
    }
    for (const auto &[id, texture] : media_textures_) {
      glDeleteTextures(1, &texture.texture);
    }
    gl::DeleteBuffers(1, &brush_vbo_);
    gl::DeleteVertexArrays(1, &brush_vao_);
    gl::DeleteBuffers(1, &vbo_);
    gl::DeleteVertexArrays(1, &vao_);
    gl::DeleteVertexArrays(1, &quad_vao_);
  }
}

bool SceneRenderer::init()
{
  if (initialized_ || failed_) {
    return initialized_;
  }
  if (!gl::load()) {
    failed_ = true;
    return false;
  }
  color_program_ = link(COLOR_VERTEX_SHADER, COLOR_FRAGMENT_SHADER);
  batch_program_ = link(BATCH_VERTEX_SHADER, BATCH_FRAGMENT_SHADER);
  composite_program_ = link(COMPOSITE_VERTEX_SHADER, COMPOSITE_FRAGMENT_SHADER);
  stencil_program_ = link(COMPOSITE_VERTEX_SHADER, STENCIL_FRAGMENT_SHADER);
  brush_program_ = link(BRUSH_VERTEX_SHADER, BRUSH_FRAGMENT_SHADER);
  media_program_ = link(BRUSH_VERTEX_SHADER, MEDIA_FRAGMENT_SHADER);
  if (!color_program_ || !composite_program_ || !stencil_program_ || !brush_program_ || !media_program_) {
    failed_ = true;
    return false;
  }
  gl::GenVertexArrays(1, &vao_);
  gl::GenBuffers(1, &vbo_);
  gl::BindVertexArray(vao_);
  gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
  gl::EnableVertexAttribArray(0);
  gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(0));
  gl::EnableVertexAttribArray(1);
  gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(2 * sizeof(float)));
  gl::GenVertexArrays(1, &brush_vao_);
  gl::GenBuffers(1, &brush_vbo_);
  gl::BindVertexArray(brush_vao_);
  gl::BindBuffer(GL_ARRAY_BUFFER, brush_vbo_);
  gl::EnableVertexAttribArray(0);
  gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(BrushVertex), reinterpret_cast<void *>(0));
  gl::EnableVertexAttribArray(1);
  gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BrushVertex), reinterpret_cast<void *>(2 * sizeof(float)));
  gl::EnableVertexAttribArray(2);
  gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(BrushVertex), reinterpret_cast<void *>(4 * sizeof(float)));
  gl::GenVertexArrays(1, &quad_vao_);
  gl::BindVertexArray(0);
  initialized_ = true;
  return true;
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
  /* 0 samples asks for a plain (not multisampled) buffer. */
  const int samples = target_samples_ > 1 ? target_samples_ : 0;
  gl::GenRenderbuffers(1, &target->color);
  gl::BindRenderbuffer(GL_RENDERBUFFER, target->color);
  gl::RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width_, height_);
  gl::GenRenderbuffers(1, &target->depth_stencil);
  gl::BindRenderbuffer(GL_RENDERBUFFER, target->depth_stencil);
  gl::RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width_, height_);
  gl::GenFramebuffers(1, &target->draw_framebuffer);
  gl::BindFramebuffer(GL_FRAMEBUFFER, target->draw_framebuffer);
  gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, target->color);
  gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target->depth_stencil);
  if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    report(ReportType::Error, "Could not create a multisampled image for drawing");
  }

  glGenTextures(1, &target->texture);
  glBindTexture(GL_TEXTURE_2D, target->texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl::GenFramebuffers(1, &target->resolve_framebuffer);
  gl::BindFramebuffer(GL_FRAMEBUFFER, target->resolve_framebuffer);
  gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target->texture, 0);
  if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    report(ReportType::Error, "Could not create an offscreen image for drawing");
  }
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
    gl::DeleteFramebuffers(1, &target->draw_framebuffer);
    gl::DeleteFramebuffers(1, &target->resolve_framebuffer);
    gl::DeleteRenderbuffers(1, &target->color);
    gl::DeleteRenderbuffers(1, &target->depth_stencil);
    glDeleteTextures(1, &target->texture);
    delete target;
  }
  targets_.clear();
  output_ = nullptr;
}

void SceneRenderer::bind_target(const Target *target)
{
  flush_batch();
  gl::BindFramebuffer(GL_FRAMEBUFFER, target->draw_framebuffer);
  glViewport(0, 0, width_, height_);
  bound_ = target;
}

void SceneRenderer::resolve(const Target *target)
{
  flush_batch();
  /* Average each pixel's samples into the target's image. */
  gl::BindFramebuffer(GL_READ_FRAMEBUFFER, target->draw_framebuffer);
  gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, target->resolve_framebuffer);
  gl::BlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void SceneRenderer::clear(const Color &color)
{
  flush_batch();
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glStencilMask(0xFF);
  glClearColor(color.r * color.a, color.g * color.a, color.b * color.a, color.a);
  glClearStencil(0);
  glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

unsigned int SceneRenderer::render(const Document &doc, int width, int height, const Transform2D &document_to_pixels,
                                   int quality, int samples, const Color *background)
{
  /* Every layer a rig moves asks for its pose: worked out once for the whole render. */
  const ArmaturePoseCacheScope pose_cache;
  /* Every layer drawn looks up its parents, masks and bones: by an index, not a search of the tree. */
  const LayerIndexScope layer_index(doc);
  quality_ = std::clamp(quality, 1, 64);
  if (!init() || width <= 0 || height <= 0) {
    return 0;
  }
  /* 1, 2, 4, 8 or 16 samples, no more than the GPU offers. */
  GLint max_samples = 1;
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  int use = 1;
  while (use * 2 <= std::min({samples, int(max_samples), 16})) {
    use *= 2;
  }
  samples_ = use;
  /* Offscreen images all match the image size and sample count; rebuild them when they change. */
  if (width != width_ || height != height_ || use != target_samples_) {
    free_targets();
    width_ = width;
    height_ = height;
    target_samples_ = use;
  }
  for (Target *target : targets_) {
    target->in_use = false;
  }
  document_to_pixels_ = document_to_pixels;
  final_render_ = background != nullptr;
  required_stencil_ = 0;
  line_free_stencil_ = 0;
  mask_clauses_.clear();
  coverage_test_ = 0;
  mask_pass_ = {};

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_STENCIL_TEST);
  set_blend_over();

  evaluated_.clear();
  render_serial_++;
  render_start_ = std::chrono::steady_clock::now();
  output_ = acquire_target();
  bind_target(output_);
  clear(background ? Color{background->r, background->g, background->b, 1.0f} : Color{0, 0, 0, 0});
  draw_layers(doc, doc.layers, Transform2D(), output_, 1.0f);
  /* Media layers In Front: over everything, unmasked. */
  required_stencil_ = 0;
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

  glDisable(GL_STENCIL_TEST);
  glStencilMask(0xFF);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl::BindVertexArray(0);
  gl::UseProgram(0);
  /* Drawings no render has used for a while (another zoom, a drawing since changed) go, with their
   * buffers. */
  std::erase_if(chunks_, [this](auto &item) {
    if (item.second.used + 120 >= render_serial_) {
      return false;
    }
    gl::DeleteVertexArrays(1, &item.second.vao);
    gl::DeleteBuffers(1, &item.second.vbo);
    return true;
  });
  std::erase_if(tessellated_, [this](auto &item) {
    if (item.second.used + 120 >= render_serial_) {
      return false;
    }
    for (PaintStep &step : item.second.steps) {
      free_step(step);
    }
    return true;
  });
  return output_->texture;
}

bool SceneRenderer::read_pixels(std::vector<uint8_t> &r_rgba, int *r_width, int *r_height)
{
  flush_batch();
  if (!output_) {
    return false;
  }
  const size_t row = size_t(width_) * 4;
  std::vector<uint8_t> bottom_up(row * size_t(height_));
  gl::BindFramebuffer(GL_READ_FRAMEBUFFER, output_->resolve_framebuffer);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, bottom_up.data());
  gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
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
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glStencilMask(0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  const unsigned required = paint == Paint::Line ? required_stencil_ & ~line_free_stencil_
                                                 : required_stencil_ & ~fill_free_stencil_;
  color_required_ = required;
  if (required == 0) {
    glStencilFunc(GL_ALWAYS, 0, 0);
  }
  else {
    glStencilFunc(GL_EQUAL, GLint(required), required);
  }
}

void SceneRenderer::stencil_fill(unsigned bit, bool set)
{
  flush_batch();
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glStencilMask(bit);
  glStencilFunc(GL_ALWAYS, set ? GLint(bit) : 0, bit);
  glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
  gl::UseProgram(stencil_program_);
  gl::BindVertexArray(quad_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
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
                 outer_fill_free = fill_free_stencil_;
  for (const DrawStep &step : steps) {
    required_stencil_ = outer_required | apply_masks(doc, step.masked);
    line_free_stencil_ = outer_line_free | applied_line_free_;
    fill_free_stencil_ = outer_fill_free | applied_fill_free_;
    draw_layer(doc, *step.layer, Transform2D(), target, opacity);
  }
  required_stencil_ = outer_required;
  line_free_stencil_ = outer_line_free;
  fill_free_stencil_ = outer_fill_free;
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
    clear(Color{0, 0, 0, 0});
    const unsigned outer_required = required_stencil_, outer_line_free = line_free_stencil_,
                   outer_fill_free = fill_free_stencil_;
    const MaskLevels outer_clauses = std::move(mask_clauses_);
    mask_clauses_.clear();
    required_stencil_ = 0;
    line_free_stencil_ = 0;
    fill_free_stencil_ = 0;
    draw_layers(doc, layer.children, transform, group, 1.0f);
    required_stencil_ = outer_required;
    line_free_stencil_ = outer_line_free;
    fill_free_stencil_ = outer_fill_free;
    mask_clauses_ = outer_clauses;
    resolve(group);
    bind_target(target);
    use_color_stencil();
    composite(group, layer_opacity);
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

unsigned SceneRenderer::apply_masks(const Document &doc, const std::vector<const Layer *> &masked)
{
  flush_batch();
  MaskLevels levels;
  applied_line_free_ = 0;
  applied_fill_free_ = 0;
  for (const Layer *l : masked) {
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
      if (cards.empty() || int(levels.size()) >= MASK_LEVELS_MAX) {
        return;
      }
      applied_line_free_ |= line_free ? 1u << levels.size() : 0u;
      applied_fill_free_ |= fill_free ? 1u << levels.size() : 0u;
      levels.push_back(std::move(cards));
    };
    push(both, false, false);
    push(fills, true, false);
    push(lines, false, true);
  }
  unsigned required = 0;
  for (size_t k = 0; k < levels.size(); k++) {
    required |= 1u << k;
  }
  if (levels == mask_clauses_) {
    /* The same as the last thing drawn (its neighbour, usually): the stencil already holds them. */
    return required;
  }
  for (size_t k = 0; k < levels.size(); k++) {
    /* The levels after this one, and the spare, are free while it is worked out. */
    const unsigned free = (MASK_SPARE | (MASK_SPARE - 1u)) & ~((2u << k) - 1u);
    evaluate_mask_cards(doc, levels[k], 1u << k, MASK_SCRATCH, free);
  }
  mask_clauses_ = std::move(levels);
  return required;
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
    stencil_where(MASK_SPARE, 0, result, 0, GL_REPLACE);
  }
}

void SceneRenderer::stencil_where(unsigned test, unsigned value, unsigned write, unsigned ref, unsigned op)
{
  flush_batch();
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glStencilMask(write);
  /* The reference holds both the value tested (under `test`) and the one written (under `write`). */
  glStencilFunc(GL_EQUAL, GLint(value | (ref & ~test)), test);
  glStencilOp(GL_KEEP, GL_KEEP, GLenum(op));
  gl::UseProgram(stencil_program_);
  gl::BindVertexArray(quad_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
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
        stencil_where(own, own, bit, bit, GL_REPLACE);
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
          stencil_where(operand, operand, scratch, scratch, GL_REPLACE);
        }
        else if (row.combine == MaskCombine::Difference) {
          stencil_where(operand, operand, scratch, 0, GL_REPLACE);
        }
        else {
          stencil_where(operand, 0, scratch, 0, GL_REPLACE);
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
        stencil_where(scratch, inside, result, result, GL_REPLACE);
        break;
      case MaskMode::Subtract:
        stencil_where(scratch, inside, result, 0, GL_REPLACE);
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
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glStencilMask(0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  const Transform2D to_document = document_to_pixels_.inverted();
  const float2 a = to_document.apply({0.0f, 0.0f}), b = to_document.apply({float(width_), float(height_)});
  const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
  constexpr float alpha = 0.9f;
  gl::UseProgram(color_program_);
  for (const bool shows : {true, false}) {
    if (required == 0) {
      if (!shows) {
        continue;
      }
      glStencilFunc(GL_ALWAYS, 0, 0);
    }
    else {
      glStencilFunc(shows ? GL_EQUAL : GL_NOTEQUAL, GLint(required), required);
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
            stencil_where(more, 0, result, 0, GL_REPLACE);
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
  flush_batch();
  /* Hidden layers still mask: a mask shape is often kept hidden. */
  if (layer.is_group()) {
    for (const Layer &child : layer.children) {
      draw_coverage(doc, child, Transform2D(), bit, set);
    }
    return;
  }
  const Transform2D transform = layer_world_transform(doc, layer.id);
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glStencilMask(bit);
  /* With coverage_test_, only where those bits are set (the mask layer's own masks). */
  glStencilFunc(coverage_test_ ? GL_EQUAL : GL_ALWAYS, GLint((set ? bit : 0u) | coverage_test_),
                coverage_test_ ? coverage_test_ : bit);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
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
    h = hash_mix(hash_mix(h, uint64_t(p.corner)), uint64_t(p.detail.level));
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

static void upload_kept(unsigned int &vao, unsigned int &vbo, const void *data, size_t bytes, bool brush);

void SceneRenderer::draw_dabs_step(PaintStep &step, const Transform2D &transform, float opacity)
{
  /* The brush's images (a brush reloaded since gets its textures again). */
  if (const Brush *brush = brush_find(step.brush)) {
    coverage_alpha_ = step.coverage_alpha;
    draw_dabs(step.dabs, brush_tip_images(*brush, step.settings), step.own_colors, step.merged_alpha, transform,
              opacity, &step.dab_vaos, &step.dab_vbos);
    coverage_alpha_ = 0.5f;
  }
}

void SceneRenderer::draw_steps(std::vector<PaintStep> &steps, const Transform2D &transform, float opacity,
                               uint64_t key)
{
  if (key != 0 && !coverage_pass_ && batch_program_) {
    const Transform2D to_clip = Transform2D::translate({-1.0f, -1.0f}) *
                                Transform2D::scaling({2.0f / float(width_), 2.0f / float(height_)}) *
                                document_to_pixels_ * transform;
    for (size_t i = 0; i < steps.size(); i++) {
      PaintStep &step = steps[i];
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
      batch_.push_back({&step, (key ^ (uint64_t(i) + 1) * 0x9e3779b97f4a7c15ull) * 1099511628211ull, to_clip, opacity,
                        required});
    }
    return;
  }
  flush_batch();
  /* In colour, a layer's lines leave out the masks that end them (End Lines at Edge, Brush), and its
   * fills the Lines Only ones. */
  const bool lines_apart = !coverage_pass_ && (line_free_stencil_ | fill_free_stencil_) != 0;
  for (PaintStep &step : steps) {
    if (lines_apart) {
      use_color_stencil(step.is_line ? Paint::Line : Paint::Fill);
    }
    if (!step.is_dabs) {
      if (!step.vertices.empty()) {
        gl::UseProgram(color_program_);
        gl::Uniform1f(gl::GetUniformLocation(color_program_, "u_opacity"), opacity);
        set_matrix(color_program_, transform);
        if (step.vao == 0) {
          upload_kept(step.vao, step.vbo, step.vertices.data(), step.vertices.size() * sizeof(Vertex), false);
        }
        gl::BindVertexArray(step.vao);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(step.vertices.size()));
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

void SceneRenderer::draw_drawing(const Document &doc, const Layer &layer, const Transform2D &layer_transform,
                                 float opacity)
{
  /* Modifiers (Mirror, the bones) draw from an evaluated copy, made once per render; otherwise the
   * drawing itself. A drawing the bones carry as one piece is drawn moved instead, so turning a bone
   * draws again what was made of it before rather than making it anew. */
  const bool modified = layer_has_modifier_effect(doc, layer, final_render_);
  const VectorDrawing *source = &layer_drawing(doc, layer);
  Transform2D rigid;
  if (modified) {
    auto it = evaluated_.find(layer.id);
    if (it == evaluated_.end()) {
      Evaluated e;
      e.drawing = layer_evaluated_drawing(doc, layer, final_render_, &e.rigid);
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
  /* Zooming reuses what was drawn: the pixel size goes in steps of 2^(1/8) (9%), so a zoom redoes a
   * drawing's triangles and stamps only every few wheel notches, not on each. */
  const float pixel = std::exp2(std::round(std::log2(1.0f / std::max(to_pixels.scale(), 1e-6f)) * 8.0f) / 8.0f);
  constexpr float feather = 0.0f;

  /* What it's drawn as is kept, and drawn again while what makes it stays the same. Brushed lines
   * add their brushes (a brush that isn't there draws a solid line, so it counts once it's found),
   * the setup shape their stamps are laid out on, and with line boil the frame. */
  uint64_t key = hash_mix(0x9e3779b97f4a7c15ull, layer.id);
  bool brushed = false, boils = false;
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
  key = hash_mix(key, uint64_t(quality_));
  key = hash_mix(key, uint64_t(coverage_pass_) | uint64_t(mask_pass_.stage) << 1 |
                          uint64_t(mask_pass_.exclude_strokes) << 4 | uint64_t(mask_pass_.filled) << 5 |
                          uint64_t(mask_pass_.extend) << 6 | uint64_t(mask_pass_.exclude_fills) << 9 |
                          uint64_t(mask_pass_.inside_stroke) << 10);
  key = hash_float(key, coverage_pass_ ? mask_pass_.grow : 0.0f);
  /* End Lines at Edge: where the masks' rows with it on let the lines show (in the colour pass; a
   * layer lends its shapes to masks as drawn). Its shapes and where this layer is go in the key. */
  LineCutter cutter;
  if (!coverage_pass_) {
    cutter = line_cutter(doc, layer, final_render_);
  }
  if (!cutter.empty()) {
    key = hash_float(hash_mix(key, cutter.flat ? 0x5eedu : 0xf1a7u), cutter.reach);
    for (const auto &rows : cutter.levels) {
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
  /* A point of a line (layer space) shows by those rows. */
  const auto line_shows = [&](float2 p) { return line_cutter_shows(cutter, transform.apply(p)); };
  BrushCut brush_cut;
  brush_cut.shows = line_shows;
  brush_cut.taper = !cutter.flat;
  brush_cut.reach = cutter.reach;
  /* Everything but the drawing's shape and place, then those. */
  const uint64_t settings = key;
  key = hash_mix(key, geometry_hash(layer, drawing, modified));
  const uint64_t any_zoom = key;
  key = hash_float(key, pixel);
  if (auto it = tessellated_.find(key); it != tessellated_.end()) {
    it->second.used = render_serial_;
    tessellated_any_zoom_[any_zoom] = key;
    draw_steps(it->second.steps, transform, opacity, key);
    return;
  }
  /* Moved whole (every point the same way, as when all of it is dragged): what was made of it before,
   * moved. Not with lines ended at masks' edges or brushed ones, which depend on where it is. A 16th of a
   * pixel apart is the same. */
  const bool movable = cutter.empty() && !brushed && !drawing.points.empty();
  const uint64_t anywhere = movable ? hash_mix(hash_float(settings, pixel), drawing_geometry_hash(drawing, pixel / 16.0f)) : 0;
  if (movable) {
    if (auto placed = tessellated_anywhere_.find(anywhere); placed != tessellated_anywhere_.end()) {
      if (auto it = tessellated_.find(placed->second.key); it != tessellated_.end()) {
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
  if (interactive_ && !final_render_ &&
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_start_).count() > budget_ms)
  {
    if (auto other = tessellated_any_zoom_.find(any_zoom); other != tessellated_any_zoom_.end()) {
      if (auto it = tessellated_.find(other->second); it != tessellated_.end()) {
        it->second.used = render_serial_;
        draw_steps(it->second.steps, transform, opacity, other->second);
        return;
      }
    }
  }
  std::vector<PaintStep> steps;

  std::vector<Vertex> vertices;
  std::vector<float2> outline;
  std::vector<float> radii;
  std::vector<bool> visible;
  std::vector<StrokeJoin> joins;
  std::vector<float2> run;
  std::vector<float> run_radii;
  std::vector<StrokeJoin> run_joins;

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
    batch(!coverage_pass_);
    /* A line thinner than a pixel is drawn a pixel wide and as much fainter (in colour; a mask takes
     * it as it is), so its weight stays true however far the view is zoomed out rather than every
     * hairline looking a pixel thick. */
    const float widest = half.empty() ? 0.0f : *std::max_element(half.begin(), half.end());
    const float min_half = pixel * 0.5f;
    if (!coverage_pass_ && widest > 0.0f && widest < min_half) {
      const float k = min_half / widest;
      std::vector<float> wider = half;
      for (float &w : wider) {
        w *= k;
      }
      Color faint = shape.style.line;
      faint.a /= k;
      tessellate_stroke(pts, wider, is_closed, faint, feather, vertices, &stroke_joins);
      return;
    }
    tessellate_stroke(pts, half, is_closed, shape.style.line, feather, vertices, &stroke_joins);
  };

  for (int shape_index = 0; shape_index < int(drawing.shapes.size()); shape_index++) {
    /* Each connected run of the shape's edges draws on its own (see shape_runs). */
    const std::vector<Shape> runs = shape_runs(drawing, drawing.shapes[shape_index]);
    /* With a fill rule its closed runs fill together, holes and all (once, at the first run that fills);
     * the lines still go round each run. */
    const Shape &whole = drawing.shapes[shape_index];
    const bool fills_together = whole.fill_rule != FillRule::Loops && runs.size() > 1;
    bool filled_together = false;
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
      shape_outline(drawing, shape, quality_, outline, &radii, &closed, &visible, &joins,
                    pixel * 3.0f / float(quality_));
      if (outline.size() < 2) {
        continue;
      }
      /* A mask's Extend: the edges that mask swept to far off in the layer's own direction, so the
       * shape and everything beyond it that way masks. A filled shape (or Filled Shape) sweeps its
       * whole outline; an outline-only one just the edges it draws, so a line with hidden edges (an
       * upper lip drawn from a whole mouth shape) cuts along what shows. */
      const MaskPass::Stage stage = coverage_pass_ ? mask_pass_.stage : MaskPass::All;
      const float grow = coverage_pass_ ? mask_pass_.grow * pixel : 0.0f;
      const bool is_area = !(coverage_pass_ && mask_pass_.exclude_fills) &&
                           ((shape.has_fill && closed) || (coverage_pass_ && mask_pass_.filled));
      /* Inside the Stroke: the area ends at its line's inner edge: the line's half width in from
       * the outline at each point (as wide as the line is there), less any grow, and wherever the
       * line itself is drawn (a brushed line's dabs, turned or jittered, reach past its width). */
      const bool inside_stroke = coverage_pass_ && mask_pass_.inside_stroke && shape.has_outline &&
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
      if (coverage_pass_ && mask_pass_.extend != 0 && stage != MaskPass::Lines && !line_as_drawn) {
        static const float2 DIRS[] = {{0, 0}, {0, 1}, {0, -1}, {-1, 0}, {1, 0}};
        const float2 reach = DIRS[std::clamp(mask_pass_.extend, 0, 4)] * (pixel * 100000.0f);
        const bool area = !mask_pass_.exclude_fills && ((shape.has_fill && closed) || mask_pass_.filled);
        const bool strokes = shape.has_outline && !mask_pass_.exclude_strokes;
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
          tessellate_fill(outline, shape.style.fill, feather, vertices);
        }
        else if (!filled_together) {
          filled_together = true;
          tessellate_fill_pieces(
              fill_area(shape_fill_loops(drawing, whole, quality_, pixel * 3.0f / float(quality_)), whole.fill_rule),
              shape.style.fill, feather, vertices);
        }
        if (grow > 0.0f && !inside_stroke) {
          /* A mask's Grow: a rim that wide around the area. */
          tessellate_stroke(outline, std::vector<float>(outline.size(), grow), true, shape.style.fill, feather,
                            vertices);
        }
      }
      if (shape.has_outline && shape.style.line_width > 0.0f &&
          (line_as_drawn || !(coverage_pass_ && mask_pass_.exclude_strokes)) && stage != MaskPass::Areas)
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
          std::vector<Shape> setup_runs;
          if (!shape.merge_made && shape_index < int(setup.shapes.size()) &&
              setup.shapes.size() == drawing.shapes.size())
          {
            setup_runs = shape_runs(setup, setup.shapes[shape_index]);
          }
          const BrushPath rest = run_index < setup_runs.size() ? brush_path(setup, setup_runs[run_index], extra) : shown;
          brush_stroke(shown, rest, shape.style.brush, shape.style.line, aspects, pixel * 0.5f, seed, dabs,
                       cutter.empty() ? nullptr : &brush_cut, !coverage_pass_);
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
            step.is_line = !coverage_pass_;
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
          r = line_grow < 0.0f || !coverage_pass_ ? std::max(r, 0.0f) : std::max(r, pixel * 0.5f);
          any_width |= r > 0.0f;
        }
        if (!any_width) {
          continue;
        }
        const int n = int(outline.size());
        const int pieces = closed ? n : n - 1;
        /* End Lines at Edge: whether each point shows (all of them without it). */
        std::vector<char> shows(size_t(n), 1);
        bool cut_any = false;
        if (!cutter.empty() && !coverage_pass_) {
          for (int i = 0; i < n; i++) {
            shows[size_t(i)] = line_shows(outline[size_t(i)]);
            cut_any = cut_any || !shows[size_t(i)];
          }
        }
        int hidden_at = -1;
        for (int i = 0; i < pieces && hidden_at < 0; i++) {
          hidden_at = visible[i] ? -1 : i;
        }
        if (hidden_at < 0 && !cut_any) {
          stroke(shape, outline, radii, closed, joins);
        }
        else {
          /* Hidden edges (and End Lines at Edge) break the outline into open runs. A closed
           * outline starts just after a hidden piece, or at a point cut away, so no run wraps
           * around its start. */
          int start = 0;
          if (closed) {
            start = hidden_at >= 0 ? hidden_at + 1 : 0;
            if (hidden_at < 0) {
              for (int i = 0; i < n; i++) {
                if (!shows[size_t(i)]) {
                  start = i;
                  break;
                }
              }
            }
          }
          run.clear();
          run_radii.clear();
          run_joins.clear();
          const auto flush_run = [&] {
            if (run.size() >= 2) {
              stroke(shape, run, run_radii, false, run_joins);
            }
            run.clear();
            run_radii.clear();
            run_joins.clear();
          };
          /* Where the line crosses the edge between points i and j, found by halving. An end made
           * there is round (Ends: Brush) or flat (Flat). */
          const StrokeJoin cut_join = cutter.flat ? StrokeJoin::Sharp : StrokeJoin::Smooth;
          const auto crossing = [&](int i, int j, float2 *r_point, float *r_radius) {
            const bool from = shows[size_t(i)] != 0;
            float lo = 0.0f, hi = 1.0f;
            for (int step = 0; step < 12; step++) {
              const float mid = (lo + hi) * 0.5f;
              const bool here = line_shows(outline[size_t(i)] + (outline[size_t(j)] - outline[size_t(i)]) * mid);
              (here == from ? lo : hi) = mid;
            }
            const float t = (lo + hi) * 0.5f;
            *r_point = outline[size_t(i)] + (outline[size_t(j)] - outline[size_t(i)]) * t;
            *r_radius = radii[size_t(i)] + (radii[size_t(j)] - radii[size_t(i)]) * t;
          };
          for (int k = 0; k < pieces; k++) {
            const int i = (start + k) % n, j = (i + 1) % n;
            if (!visible[i]) {
              flush_run();
              continue;
            }
            const bool a = shows[size_t(i)] != 0, b = shows[size_t(j)] != 0;
            if (!a && !b) {
              flush_run();
              continue;
            }
            if (a && run.empty()) {
              run.push_back(outline[i]);
              run_radii.push_back(radii[i]);
              run_joins.push_back(joins[i]);
            }
            if (a && b) {
              run.push_back(outline[j]);
              run_radii.push_back(radii[j]);
              run_joins.push_back(joins[j]);
              continue;
            }
            float2 at;
            float r = 0.0f;
            crossing(i, j, &at, &r);
            if (!cutter.flat) {
              /* A round end stops its radius short of the edge, back along the line, so it just
               * touches it (Reach 0); Reach moves it on, past the edge from 0.5. */
              const float stop = r * (1.0f - 2.0f * cutter.reach);
              const float2 back = a ? outline[size_t(i)] - at : outline[size_t(j)] - at;
              const float len = length(back);
              if (len > 1e-9f) {
                at = at + back * (std::clamp(stop, -len, len) / len);
              }
            }
            if (a) {
              /* Leaving: the run ends at the edge. */
              run.push_back(at);
              run_radii.push_back(r);
              run_joins.push_back(cut_join);
              flush_run();
            }
            else {
              /* Coming back: a run starts at the edge. */
              flush_run();
              run.push_back(at);
              run_radii.push_back(r);
              run_joins.push_back(cut_join);
              run.push_back(outline[j]);
              run_radii.push_back(radii[j]);
              run_joins.push_back(joins[j]);
            }
          }
          flush_run();
        }
      }
    }
  }
  flush();
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
struct BatchVertex {
  float x, y;
  float r, g, b, a;
  float slot;
};

void SceneRenderer::flush_batch()
{
  if (batch_.empty()) {
    return;
  }
  constexpr size_t SLOTS = 32;
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glStencilMask(0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  gl::UseProgram(batch_program_);
  const GLint slots_at = gl::GetUniformLocation(batch_program_, "u_slots");
  std::vector<BatchVertex> vertices;
  float slots[SLOTS * 8];
  for (size_t i = 0; i < batch_.size();) {
    const unsigned required = batch_[i].required;
    size_t j = i;
    uint64_t chunk_key = 1469598103934665603ull;
    while (j < batch_.size() && j - i < SLOTS && batch_[j].required == required) {
      chunk_key = (chunk_key ^ batch_[j].id) * 1099511628211ull;
      j++;
    }
    Chunk &chunk = chunks_[chunk_key];
    chunk.used = render_serial_;
    if (!chunk.vao) {
      vertices.clear();
      for (size_t k = i; k < j; k++) {
        for (const Vertex &v : batch_[k].step->vertices) {
          vertices.push_back({v.x, v.y, v.r, v.g, v.b, v.a, float(k - i)});
        }
      }
      gl::GenVertexArrays(1, &chunk.vao);
      gl::GenBuffers(1, &chunk.vbo);
      gl::BindVertexArray(chunk.vao);
      gl::BindBuffer(GL_ARRAY_BUFFER, chunk.vbo);
      gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(BatchVertex)), vertices.data(),
                     GL_STATIC_DRAW);
      const GLsizei stride = sizeof(BatchVertex);
      gl::EnableVertexAttribArray(0);
      gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(BatchVertex, x)));
      gl::EnableVertexAttribArray(1);
      gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(BatchVertex, r)));
      gl::EnableVertexAttribArray(2);
      gl::VertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offsetof(BatchVertex, slot)));
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
    gl::Uniform4fv(slots_at, GLsizei((j - i) * 2), slots);
    if (required == 0) {
      glStencilFunc(GL_ALWAYS, 0, 0);
    }
    else {
      glStencilFunc(GL_EQUAL, GLint(required), required);
    }
    gl::BindVertexArray(chunk.vao);
    glDrawArrays(GL_TRIANGLES, 0, chunk.vertices);
    i = j;
  }
  batch_.clear();
  /* As use_color_stencil last left it, for what draws next. */
  if (color_required_ == 0) {
    glStencilFunc(GL_ALWAYS, 0, 0);
  }
  else {
    glStencilFunc(GL_EQUAL, GLint(color_required_), color_required_);
  }
}

void SceneRenderer::set_matrix(unsigned int program, const Transform2D &transform)
{
  /* Layer space -> pixels -> clip space. */
  const Transform2D to_clip = Transform2D::translate({-1.0f, -1.0f}) *
                              Transform2D::scaling({2.0f / float(width_), 2.0f / float(height_)}) *
                              document_to_pixels_ * transform;
  const float matrix[9] = {to_clip.a, to_clip.b, 0.0f, to_clip.c, to_clip.d, 0.0f, to_clip.tx, to_clip.ty, 1.0f};
  gl::UniformMatrix3fv(gl::GetUniformLocation(program, "u_matrix"), 1, GL_FALSE, matrix);
}

unsigned int SceneRenderer::brush_texture(const BrushImage &image)
{
  if (auto it = brush_textures_.find(image.id); it != brush_textures_.end()) {
    return it->second;
  }
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               image.rgba.data());
  gl::GenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  brush_textures_[image.id] = texture;
  return texture;
}

/* A buffer and layout for kept vertices, uploaded once: the colour program's (x, y, rgba) or the brush
 * program's (x, y, u, v, rgba). */
static void upload_kept(unsigned int &vao, unsigned int &vbo, const void *data, size_t bytes, bool brush)
{
  gl::GenVertexArrays(1, &vao);
  gl::GenBuffers(1, &vbo);
  gl::BindVertexArray(vao);
  gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
  gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes), data, GL_STATIC_DRAW);
  if (brush) {
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(BrushVertex), reinterpret_cast<void *>(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(BrushVertex), reinterpret_cast<void *>(2 * sizeof(float)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(BrushVertex), reinterpret_cast<void *>(4 * sizeof(float)));
  }
  else {
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(0));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(2 * sizeof(float)));
  }
}

void SceneRenderer::free_step(PaintStep &step)
{
  if (step.vao) {
    gl::DeleteVertexArrays(1, &step.vao);
    gl::DeleteBuffers(1, &step.vbo);
  }
  for (size_t i = 0; i < step.dab_vaos.size(); i++) {
    if (step.dab_vaos[i]) {
      gl::DeleteVertexArrays(1, &step.dab_vaos[i]);
      gl::DeleteBuffers(1, &step.dab_vbos[i]);
    }
  }
  step.vao = step.vbo = 0;
  step.dab_vaos.clear();
  step.dab_vbos.clear();
}

void SceneRenderer::draw_dabs(const BrushDabs &dabs, const std::vector<BrushImage> &images, bool own_colors,
                              bool merged, const Transform2D &transform, float opacity, std::vector<unsigned int> *vaos,
                              std::vector<unsigned int> *vbos)
{
  flush_batch();
  const Target *outer = bound_;
  Target *own = nullptr;
  /* Merged alpha: the dabs go into an image of their own, keeping the strongest one where
   * they overlap, which is then laid over the layer. (Mask coverage needs no merging.) */
  if (merged && !coverage_pass_ && outer) {
    own = acquire_target();
    bind_target(own);
    glDisable(GL_STENCIL_TEST);
    clear(Color{0, 0, 0, 0});
    gl::BlendEquation(GL_MAX);
  }
  gl::UseProgram(brush_program_);
  set_matrix(brush_program_, transform);
  gl::Uniform1i(gl::GetUniformLocation(brush_program_, "u_image"), 0);
  gl::Uniform1f(gl::GetUniformLocation(brush_program_, "u_opacity"), own ? 1.0f : opacity);
  gl::Uniform1f(gl::GetUniformLocation(brush_program_, "u_own_colors"), own_colors ? 1.0f : 0.0f);
  gl::Uniform1f(gl::GetUniformLocation(brush_program_, "u_alpha_test"), coverage_pass_ ? coverage_alpha_ : 0.0f);
  gl::ActiveTexture(GL_TEXTURE0);
  gl::BindVertexArray(brush_vao_);
  gl::BindBuffer(GL_ARRAY_BUFFER, brush_vbo_);
  if (vaos && vbos) {
    vaos->resize(dabs.per_image.size(), 0);
    vbos->resize(dabs.per_image.size(), 0);
  }
  for (size_t i = 0; i < dabs.per_image.size() && i < images.size(); i++) {
    const std::vector<BrushVertex> &list = dabs.per_image[i];
    if (list.empty()) {
      continue;
    }
    glBindTexture(GL_TEXTURE_2D, brush_texture(images[i]));
    if (vaos && vbos) {
      if ((*vaos)[i] == 0) {
        upload_kept((*vaos)[i], (*vbos)[i], list.data(), list.size() * sizeof(BrushVertex), true);
      }
      gl::BindVertexArray((*vaos)[i]);
    }
    else {
      gl::BindVertexArray(brush_vao_);
      gl::BindBuffer(GL_ARRAY_BUFFER, brush_vbo_);
      gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(list.size() * sizeof(BrushVertex)), list.data(), GL_STREAM_DRAW);
    }
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(list.size()));
  }
  if (own) {
    set_blend_over();
    resolve(own);
    bind_target(outer);
    glEnable(GL_STENCIL_TEST);
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
    glGenTextures(1, &tex.texture);
  }
  glBindTexture(GL_TEXTURE_2D, tex.texture);
  if (tex.serial != frame->serial) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (tex.width != frame->width || tex.height != frame->height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame->width, frame->height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   frame->rgba.data());
      tex.width = frame->width;
      tex.height = frame->height;
    }
    else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height, GL_RGBA, GL_UNSIGNED_BYTE,
                      frame->rgba.data());
    }
    gl::GenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
  gl::UseProgram(media_program_);
  set_matrix(media_program_, transform);
  gl::Uniform1i(gl::GetUniformLocation(media_program_, "u_image"), 0);
  gl::Uniform1f(gl::GetUniformLocation(media_program_, "u_opacity"), opacity);
  gl::Uniform1f(gl::GetUniformLocation(media_program_, "u_grey"), layer.media.greyscale ? 1.0f : 0.0f);
  const Color &tint = layer.media.tint;
  gl::Uniform4f(gl::GetUniformLocation(media_program_, "u_tint"), tint.r, tint.g, tint.b, layer.media.tint_amount);
  gl::ActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex.texture);
  gl::BindVertexArray(brush_vao_);
  gl::BindBuffer(GL_ARRAY_BUFFER, brush_vbo_);
  gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizeof(v)), v, GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, 6);
}

void SceneRenderer::draw_vertices(const std::vector<Vertex> &vertices, const Transform2D &transform)
{
  flush_batch();
  set_matrix(color_program_, transform);
  gl::BindVertexArray(vao_);
  gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
  gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices.size()));
}

void SceneRenderer::composite(const Target *source, float opacity)
{
  flush_batch();
  gl::UseProgram(composite_program_);
  gl::ActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, source->texture);
  gl::Uniform1i(gl::GetUniformLocation(composite_program_, "u_source"), 0);
  gl::Uniform1f(gl::GetUniformLocation(composite_program_, "u_opacity"), opacity);
  gl::BindVertexArray(quad_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace oanim
