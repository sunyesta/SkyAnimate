#include "gpu/scene_renderer.hh"

#include <algorithm>
#include <string>

#include "core/log.hh"
#include "data/document.hh"
#include "gpu/gl.hh"
#include "gpu/tessellate.hh"

namespace sky {

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

/* A full-screen triangle from gl_VertexID, sampling the source and (optionally) the mask. */
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
uniform sampler2D u_mask;
uniform int u_use_mask;
uniform float u_opacity;
out vec4 frag_color;
void main()
{
  vec4 color = texture(u_source, v_uv) * u_opacity;
  if (u_use_mask == 1) {
    color *= texture(u_mask, v_uv).a;
  }
  frag_color = color;
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

/* How colours combine in the bound target: normal "over", or carving coverage out of a mask. */
enum class BlendMode { Over, Subtract };

static void set_blend(BlendMode mode)
{
  glEnable(GL_BLEND);
  gl::BlendEquation(GL_FUNC_ADD);
  if (mode == BlendMode::Over) {
    gl::BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  }
  else {
    gl::BlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
  }
}

SceneRenderer::~SceneRenderer()
{
  for (Target *target : targets_) {
    if (gl::DeleteFramebuffers) {
      gl::DeleteFramebuffers(1, &target->framebuffer);
      glDeleteTextures(1, &target->texture);
    }
    delete target;
  }
  if (initialized_ && gl::DeleteProgram) {
    gl::DeleteProgram(color_program_);
    gl::DeleteProgram(composite_program_);
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
  if (!color_program_ || !composite_program_) {
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
  target->width = width_;
  target->height = height_;
  glGenTextures(1, &target->texture);
  glBindTexture(GL_TEXTURE_2D, target->texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl::GenFramebuffers(1, &target->framebuffer);
  gl::BindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
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

void SceneRenderer::bind_target(const Target *target)
{
  gl::BindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
  glViewport(0, 0, target->width, target->height);
}

void SceneRenderer::clear(float alpha)
{
  glClearColor(alpha, alpha, alpha, alpha);
  glClear(GL_COLOR_BUFFER_BIT);
}

unsigned int SceneRenderer::render(const Document &doc, int width, int height, const Transform2D &document_to_pixels,
                                   int quality)
{
  quality_ = std::clamp(quality, 1, 64);
  if (!init() || width <= 0 || height <= 0) {
    return 0;
  }
  /* Offscreen images all match the viewport size; rebuild them when it changes. */
  if (width != width_ || height != height_) {
    for (Target *target : targets_) {
      gl::DeleteFramebuffers(1, &target->framebuffer);
      glDeleteTextures(1, &target->texture);
      delete target;
    }
    targets_.clear();
    width_ = width;
    height_ = height;
  }
  for (Target *target : targets_) {
    target->in_use = false;
  }
  document_to_pixels_ = document_to_pixels;

  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  set_blend(BlendMode::Over);

  output_ = acquire_target();
  bind_target(output_);
  clear(0.0f);
  draw_layers(doc, doc.layers, Transform2D(), output_, 1.0f);

  gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl::BindVertexArray(0);
  gl::UseProgram(0);
  return output_->texture;
}

void SceneRenderer::draw_layers(const Document &doc, const std::vector<Layer> &layers, const Transform2D &parent,
                                Target *target, float opacity)
{
  /* The list is top first; draw from the bottom up. */
  for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
    draw_layer(doc, *it, parent, target, opacity);
  }
}

void SceneRenderer::draw_layer(const Document &doc, const Layer &layer, const Transform2D &parent, Target *target,
                               float opacity)
{
  if (!layer.visible || layer.opacity <= 0.0f) {
    return;
  }
  const Transform2D transform = parent * layer.local_transform();
  const float layer_opacity = opacity * layer.opacity;

  if (!layer.is_group()) {
    draw_drawing(doc, layer, transform, layer_opacity);
    return;
  }
  if (layer.group_mask != GroupMask::None) {
    draw_masked_group(doc, layer, transform, target, layer_opacity);
    return;
  }
  if (layer.opacity < 1.0f) {
    /* Fade the group as a whole, so its overlapping children don't show through each other. */
    Target *group = acquire_target();
    bind_target(group);
    clear(0.0f);
    draw_layers(doc, layer.children, transform, group, 1.0f);
    bind_target(target);
    composite(group, nullptr, layer_opacity);
    release_target(group);
    return;
  }
  draw_layers(doc, layer.children, transform, target, layer_opacity);
}

void SceneRenderer::draw_masked_group(const Document &doc, const Layer &group, const Transform2D &transform,
                                      Target *target, float opacity)
{
  Target *color = acquire_target();
  Target *mask = acquire_target();
  bind_target(mask);
  clear(group.group_mask == GroupMask::ShowAll ? 1.0f : 0.0f);
  bind_target(color);
  clear(0.0f);

  for (auto it = group.children.rbegin(); it != group.children.rend(); ++it) {
    const Layer &child = *it;
    if (!child.visible) {
      continue;
    }
    if (child.mask_clears()) {
      bind_target(mask);
      clear(0.0f);
    }
    if (child.mask_adds() || child.mask_subtracts()) {
      bind_target(mask);
      set_blend(child.mask_subtracts() ? BlendMode::Subtract : BlendMode::Over);
      draw_layer(doc, child, transform, mask, 1.0f);
      set_blend(BlendMode::Over);
    }
    if (child.mask_hides_layer()) {
      continue;
    }
    if (child.mask_mode == MaskMode::Masked) {
      Target *layer = acquire_target();
      bind_target(layer);
      clear(0.0f);
      draw_layer(doc, child, transform, layer, 1.0f);
      bind_target(color);
      composite(layer, mask, 1.0f);
      release_target(layer);
    }
    else {
      bind_target(color);
      draw_layer(doc, child, transform, color, 1.0f);
    }
  }

  bind_target(target);
  composite(color, nullptr, opacity);
  release_target(color);
  release_target(mask);
}

void SceneRenderer::draw_drawing(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity)
{
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  if (drawing.shapes.empty()) {
    return;
  }
  const Transform2D to_pixels = document_to_pixels_ * transform;
  /* One pixel in layer space, for the soft edge. */
  const float aa = 1.0f / std::max(to_pixels.scale(), 1e-6f);

  std::vector<Vertex> vertices;
  std::vector<float2> outline;
  std::vector<float> radii;
  std::vector<bool> visible;
  std::vector<float2> run;
  std::vector<float> run_radii;
  for (const Shape &shape : drawing.shapes) {
    bool closed = false;
    shape_outline(drawing, shape, quality_, outline, &radii, &closed, &visible);
    if (outline.size() < 2) {
      continue;
    }
    if (shape.has_fill && closed) {
      tessellate_fill(outline, shape.style.fill, aa, vertices);
    }
    if (shape.has_outline && shape.style.line_width > 0.0f) {
      /* Half widths for the tessellator: the shape's width scaled by each point's radius. */
      for (float &r : radii) {
        r *= shape.style.line_width * 0.5f;
      }
      const int n = int(outline.size());
      const int pieces = closed ? n : n - 1;
      int hidden_at = -1;
      for (int i = 0; i < pieces && hidden_at < 0; i++) {
        hidden_at = visible[i] ? -1 : i;
      }
      if (hidden_at < 0) {
        tessellate_stroke(outline, radii, closed, shape.style.line, aa, vertices);
      }
      else {
        /* Hidden edges break the outline into open runs. A closed outline starts just after a
         * hidden piece so no run wraps around its start. */
        const int start = closed ? hidden_at + 1 : 0;
        run.clear();
        run_radii.clear();
        const auto flush = [&] {
          if (run.size() >= 2) {
            tessellate_stroke(run, run_radii, false, shape.style.line, aa, vertices);
          }
          run.clear();
          run_radii.clear();
        };
        for (int k = 0; k < pieces; k++) {
          const int i = (start + k) % n, j = (i + 1) % n;
          if (!visible[i]) {
            flush();
            continue;
          }
          if (run.empty()) {
            run.push_back(outline[i]);
            run_radii.push_back(radii[i]);
          }
          run.push_back(outline[j]);
          run_radii.push_back(radii[j]);
        }
        flush();
      }
    }
  }
  if (vertices.empty()) {
    return;
  }
  gl::UseProgram(color_program_);
  gl::Uniform1f(gl::GetUniformLocation(color_program_, "u_opacity"), opacity);
  draw_vertices(vertices, transform);
}

void SceneRenderer::draw_vertices(const std::vector<Vertex> &vertices, const Transform2D &transform)
{
  /* Layer space -> pixels -> clip space. */
  const Transform2D to_clip = Transform2D::translate({-1.0f, -1.0f}) *
                              Transform2D::scaling({2.0f / float(width_), 2.0f / float(height_)}) *
                              document_to_pixels_ * transform;
  const float matrix[9] = {to_clip.a, to_clip.b, 0.0f, to_clip.c, to_clip.d, 0.0f, to_clip.tx, to_clip.ty, 1.0f};
  gl::UniformMatrix3fv(gl::GetUniformLocation(color_program_, "u_matrix"), 1, GL_FALSE, matrix);
  gl::BindVertexArray(vao_);
  gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
  gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices.size()));
}

void SceneRenderer::composite(const Target *source, const Target *mask, float opacity)
{
  gl::UseProgram(composite_program_);
  gl::ActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, source->texture);
  gl::Uniform1i(gl::GetUniformLocation(composite_program_, "u_source"), 0);
  gl::ActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, mask ? mask->texture : source->texture);
  gl::Uniform1i(gl::GetUniformLocation(composite_program_, "u_mask"), 1);
  gl::Uniform1i(gl::GetUniformLocation(composite_program_, "u_use_mask"), mask ? 1 : 0);
  gl::Uniform1f(gl::GetUniformLocation(composite_program_, "u_opacity"), opacity);
  gl::ActiveTexture(GL_TEXTURE0);
  gl::BindVertexArray(quad_vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace sky
