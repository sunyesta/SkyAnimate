#include "gpu/scene_renderer.hh"

#include <algorithm>
#include <string>

#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "assets/brushes.hh"
#include "core/log.hh"
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
  gl::BindFramebuffer(GL_FRAMEBUFFER, target->draw_framebuffer);
  glViewport(0, 0, width_, height_);
  bound_ = target;
}

void SceneRenderer::resolve(const Target *target)
{
  /* Average each pixel's samples into the target's image. */
  gl::BindFramebuffer(GL_READ_FRAMEBUFFER, target->draw_framebuffer);
  gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, target->resolve_framebuffer);
  gl::BlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}

void SceneRenderer::clear(const Color &color)
{
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glStencilMask(0xFF);
  glClearColor(color.r * color.a, color.g * color.a, color.b * color.a, color.a);
  glClearStencil(0);
  glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

unsigned int SceneRenderer::render(const Document &doc, int width, int height, const Transform2D &document_to_pixels,
                                   int quality, int samples, const Color *background)
{
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
  mask_clauses_.clear();
  coverage_test_ = 0;
  mask_pass_ = {};

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_STENCIL_TEST);
  set_blend_over();

  output_ = acquire_target();
  bind_target(output_);
  clear(background ? Color{background->r, background->g, background->b, 1.0f} : Color{0, 0, 0, 0});
  draw_layers(doc, doc.layers, Transform2D(), output_, 1.0f);
  /* Media layers In Front: over everything, unmasked. */
  required_stencil_ = 0;
  for (const Layer *l : document_layers_flat(doc)) {
    if (l->is_media() && l->media.in_front && layer_shown_through_parents(doc, *l)) {
      use_color_stencil();
      draw_media(doc, *l, layer_world_transform(doc, l->id), layer_opacity_through_parents(doc, *l));
    }
  }
  resolve(output_);

  glDisable(GL_STENCIL_TEST);
  glStencilMask(0xFF);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl::BindVertexArray(0);
  gl::UseProgram(0);
  return output_->texture;
}

bool SceneRenderer::read_pixels(std::vector<uint8_t> &r_rgba, int *r_width, int *r_height)
{
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

void SceneRenderer::use_color_stencil()
{
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glStencilMask(0);
  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
  if (required_stencil_ == 0) {
    glStencilFunc(GL_ALWAYS, 0, 0);
  }
  else {
    glStencilFunc(GL_EQUAL, GLint(required_stencil_), required_stencil_);
  }
}

void SceneRenderer::stencil_fill(unsigned bit, bool set)
{
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  glStencilMask(bit);
  glStencilFunc(GL_ALWAYS, set ? GLint(bit) : 0, bit);
  glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
  gl::UseProgram(stencil_program_);
  gl::BindVertexArray(quad_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

bool SceneRenderer::shown(const Layer &layer) const
{
  /* A Guide is for working against: never in renders. */
  return layer.visible && (!final_render_ || (layer.render_visible && !(layer.is_media() && layer.media.guide)));
}

bool SceneRenderer::layer_shown_through_parents(const Document &doc, const Layer &layer) const
{
  for (const Layer *l = &layer; l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    if (!shown(*l) || l->opacity <= 0.0f) {
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
  const unsigned outer_required = required_stencil_;
  for (const DrawStep &step : steps) {
    required_stencil_ = outer_required | apply_masks(doc, step.masked);
    draw_layer(doc, *step.layer, Transform2D(), target, opacity);
  }
  required_stencil_ = outer_required;
}

void SceneRenderer::draw_layer(const Document &doc, const Layer &layer, const Transform2D & /*parent*/, Target *target,
                               float opacity)
{
  if (!shown(layer) || layer.opacity <= 0.0f) {
    return;
  }
  /* Where the layer is, bones included: a Parent Bone (its own, or a group's above it) moves it,
   * as everything else that places layers sees it. */
  const Transform2D transform = layer_world_transform(doc, layer.id);
  const float layer_opacity = opacity * layer.opacity;

  if (layer.is_media()) {
    if (!layer.media.in_front) {
      use_color_stencil();
      draw_media(doc, layer, transform, layer_opacity);
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
    const unsigned outer_required = required_stencil_;
    const std::vector<MaskClause> outer_clauses = std::move(mask_clauses_);
    mask_clauses_.clear();
    required_stencil_ = 0;
    draw_layers(doc, layer.children, transform, group, 1.0f);
    required_stencil_ = outer_required;
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

/* The masks that are on, of `masked` (outermost first), as clauses; ones naming no layer that's
 * still there are left out. */
static std::vector<SceneRenderer::MaskClause> clauses_of(const Document &doc, const std::vector<const Layer *> &masked,
                                                          bool render)
{
  std::vector<SceneRenderer::MaskClause> clauses;
  for (const Layer *l : masked) {
    for (const LayerMask &m : l->masks) {
      /* Each mask can be on in the viewport and in renders apart. */
      if (!(render ? m.render : m.enabled)) {
        continue;
      }
      SceneRenderer::MaskClause c;
      c.outside = m.outside;
      c.expand = m.expand;
      c.fills_only = m.fills_only;
      c.filled = m.filled;
      c.extend = int(m.extend);
      for (const uint64_t id : m.layers) {
        if (id != 0 && layer_find(doc, id)) {
          c.layers.push_back(id);
        }
      }
      if (!c.layers.empty()) {
        clauses.push_back(std::move(c));
      }
    }
  }
  return clauses;
}

unsigned SceneRenderer::apply_masks(const Document &doc, const std::vector<const Layer *> &masked)
{
  const std::vector<MaskClause> clauses = clauses_of(doc, masked, final_render_);
  /* Six bits for a layer's masks; the top two are for its mask layers' own masks. */
  const size_t count = std::min<size_t>(clauses.size(), 6);
  unsigned required = 0;
  for (size_t k = 0; k < count; k++) {
    required |= 1u << k;
  }
  if (clauses == mask_clauses_) {
    /* The same as the last thing drawn (its neighbour, usually): the stencil already holds them. */
    return required;
  }
  for (size_t k = 0; k < count; k++) {
    const unsigned bit = 1u << k;
    /* Inside: start empty and add the layers' shapes; Outside: start full and cut them out. */
    stencil_fill(bit, clauses[k].outside);
    for (const uint64_t id : clauses[k].layers) {
      if (const Layer *source = layer_find(doc, id)) {
        mask_pass_ = {clauses[k].expand, clauses[k].fills_only, clauses[k].filled, clauses[k].extend};
        draw_mask_source(doc, *source, bit, !clauses[k].outside);
        mask_pass_ = {};
      }
    }
  }
  mask_clauses_ = clauses;
  return required;
}

void SceneRenderer::draw_mask_source(const Document &doc, const Layer &source, unsigned bit, bool set)
{
  /* The mask layer's own masks count (one level): its shape goes in only where they hold. */
  const std::vector<MaskClause> own = clauses_of(doc, {&source}, final_render_);
  const MaskPass pass = mask_pass_;
  unsigned test = 0;
  for (size_t k = 0; k < own.size() && k < 2; k++) {
    const unsigned own_bit = 1u << (7 - k);
    stencil_fill(own_bit, own[k].outside);
    mask_pass_ = {own[k].expand, own[k].fills_only, own[k].filled, own[k].extend};
    for (const uint64_t id : own[k].layers) {
      if (const Layer *l = layer_find(doc, id)) {
        draw_coverage(doc, *l, Transform2D(), own_bit, !own[k].outside);
      }
    }
    test |= own_bit;
  }
  mask_pass_ = pass;
  coverage_test_ = test;
  draw_coverage(doc, source, Transform2D(), bit, set);
  coverage_test_ = 0;
}

void SceneRenderer::draw_coverage(const Document &doc, const Layer &layer, const Transform2D & /*parent*/, unsigned bit,
                                  bool set)
{
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

void SceneRenderer::draw_drawing(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity)
{
  /* Modifiers (Mirror, the bones) draw from an evaluated copy; otherwise the drawing itself. */
  VectorDrawing evaluated;
  const bool modified = layer_has_modifier_effect(doc, layer, final_render_);
  if (modified) {
    evaluated = layer_evaluated_drawing(doc, layer, final_render_);
  }
  const VectorDrawing &drawing = modified ? evaluated : layer_drawing(doc, layer);
  if (drawing.shapes.empty()) {
    return;
  }
  const Transform2D to_pixels = document_to_pixels_ * transform;
  /* One pixel in layer space. Edges are hard: multisampling smooths them, and a soft edge
   * strip would bring back seams where edges meet. */
  const float pixel = 1.0f / std::max(to_pixels.scale(), 1e-6f);
  constexpr float feather = 0.0f;

  std::vector<Vertex> vertices;
  std::vector<float2> outline;
  std::vector<float> radii;
  std::vector<bool> visible;
  std::vector<StrokeJoin> joins;
  std::vector<float2> run;
  std::vector<float> run_radii;
  std::vector<StrokeJoin> run_joins;

  /* The colour batch so far: drawn before a brushed line so shapes keep their stacking order. */
  const auto flush = [&] {
    if (!vertices.empty()) {
      gl::UseProgram(color_program_);
      gl::Uniform1f(gl::GetUniformLocation(color_program_, "u_opacity"), opacity);
      draw_vertices(vertices, transform);
      vertices.clear();
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
    tessellate_stroke(pts, half, is_closed, shape.style.line, feather, vertices, &stroke_joins);
  };

  for (int shape_index = 0; shape_index < int(drawing.shapes.size()); shape_index++) {
    const Shape &shape = drawing.shapes[shape_index];
    if (shape.hidden) {
      continue;
    }
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
    if (coverage_pass_ && mask_pass_.extend != 0) {
      static const float2 DIRS[] = {{0, 0}, {0, 1}, {0, -1}, {-1, 0}, {1, 0}};
      const float2 reach = DIRS[std::clamp(mask_pass_.extend, 0, 4)] * (pixel * 100000.0f);
      const bool area = (shape.has_fill && closed) || mask_pass_.filled;
      const bool strokes = shape.has_outline && !mask_pass_.exclude_strokes;
      const size_t n = outline.size();
      for (size_t i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        if (!area && !(strokes && i < visible.size() && visible[i])) {
          continue;
        }
        const float2 a = outline[i], b = outline[(i + 1) % n];
        tessellate_fill({a, b, b + reach, a + reach}, shape.style.fill, feather, vertices);
      }
    }
    /* A mask's Filled Shape: any shape masks with its inside, an open one closed straight across. */
    const bool fill_for_mask = coverage_pass_ && mask_pass_.filled;
    if ((shape.has_fill && closed) || fill_for_mask) {
      tessellate_fill(outline, shape.style.fill, feather, vertices);
      if (mask_pass_.expand) {
        /* Expand mask by a pixel: a one-pixel rim around the fill. */
        tessellate_stroke(outline, std::vector<float>(outline.size(), pixel), true, shape.style.fill, feather,
                          vertices);
      }
    }
    if (shape.has_outline && shape.style.line_width > 0.0f && !mask_pass_.exclude_strokes) {
      /* A brushed line, if its brush is in the library and has images; otherwise solid. */
      brush = brush_find(shape.style.brush.name);
      images = brush ? &brush_images(*brush) : nullptr;
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
        dabs.count = 0;
        seed = brush_seed(shape.style.brush, layer.id, shape_index, doc.frame_current);
        /* Dabs stick to the artwork: laid out on the line as on the setup frame, put on it as it
         * is now (hidden edges get none). */
        if (!setup_made) {
          setup = layer_setup_drawing(doc, layer, final_render_);
          setup_made = true;
        }
        const float extra = mask_pass_.expand ? pixel : 0.0f;
        const BrushPath shown = brush_path(drawing, shape, extra);
        const BrushPath rest = shape_index < int(setup.shapes.size()) && setup.shapes.size() == drawing.shapes.size()
                                   ? brush_path(setup, setup.shapes[shape_index], extra)
                                   : shown;
        brush_stroke(shown, rest, shape.style.brush, shape.style.line, aspects, pixel * 0.5f, seed, dabs);
        if (dabs.count > 0) {
          flush();
          draw_dabs(dabs, *images, brush->is_color && !shape.style.brush.tint, shape.style.brush.merged_alpha,
                    transform, opacity);
        }
        continue;
      }
      /* Half widths for the tessellator: the shape's width scaled by each point's radius, a pixel
       * wider when the mask expands, and never thinner than a pixel so hairlines stay visible. */
      for (float &r : radii) {
        r = std::max(r * shape.style.line_width * 0.5f + (mask_pass_.expand ? pixel : 0.0f), pixel * 0.5f);
      }
      const int n = int(outline.size());
      const int pieces = closed ? n : n - 1;
      int hidden_at = -1;
      for (int i = 0; i < pieces && hidden_at < 0; i++) {
        hidden_at = visible[i] ? -1 : i;
      }
      if (hidden_at < 0) {
        stroke(shape, outline, radii, closed, joins);
      }
      else {
        /* Hidden edges break the outline into open runs. A closed outline starts just after a
         * hidden piece so no run wraps around its start. */
        const int start = closed ? hidden_at + 1 : 0;
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
        for (int k = 0; k < pieces; k++) {
          const int i = (start + k) % n, j = (i + 1) % n;
          if (!visible[i]) {
            flush_run();
            continue;
          }
          if (run.empty()) {
            run.push_back(outline[i]);
            run_radii.push_back(radii[i]);
            run_joins.push_back(joins[i]);
          }
          run.push_back(outline[j]);
          run_radii.push_back(radii[j]);
          run_joins.push_back(joins[j]);
        }
        flush_run();
      }
    }
  }
  flush();
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

void SceneRenderer::draw_dabs(const BrushDabs &dabs, const std::vector<BrushImage> &images, bool own_colors,
                              bool merged, const Transform2D &transform, float opacity)
{
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
  gl::Uniform1f(gl::GetUniformLocation(brush_program_, "u_alpha_test"), coverage_pass_ ? 0.5f : 0.0f);
  gl::ActiveTexture(GL_TEXTURE0);
  gl::BindVertexArray(brush_vao_);
  gl::BindBuffer(GL_ARRAY_BUFFER, brush_vbo_);
  for (size_t i = 0; i < dabs.per_image.size() && i < images.size(); i++) {
    const std::vector<BrushVertex> &list = dabs.per_image[i];
    if (list.empty()) {
      continue;
    }
    glBindTexture(GL_TEXTURE_2D, brush_texture(images[i]));
    gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(list.size() * sizeof(BrushVertex)), list.data(), GL_STREAM_DRAW);
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
  const MediaShown shown_now = media_shown_at(doc, layer, doc.frame_current);
  if (!shown_now.clip || opacity <= 0.0f) {
    return;
  }
  const MediaClip &clip = *shown_now.clip;
  const std::string file = media_clip_file(clip);
  /* A render waits for the exact frame; the viewport shows the nearest one until it comes. */
  const MediaFramePtr frame = media_frame(file, clip.sequence, shown_now.seconds, final_render_);
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
  set_matrix(color_program_, transform);
  gl::BindVertexArray(vao_);
  gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
  gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices.size()));
}

void SceneRenderer::composite(const Target *source, float opacity)
{
  gl::UseProgram(composite_program_);
  gl::ActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, source->texture);
  gl::Uniform1i(gl::GetUniformLocation(composite_program_, "u_source"), 0);
  gl::Uniform1f(gl::GetUniformLocation(composite_program_, "u_opacity"), opacity);
  gl::BindVertexArray(quad_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace oanim
