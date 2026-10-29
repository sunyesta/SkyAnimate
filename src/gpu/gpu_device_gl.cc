/* The render-command layer on OpenGL 3.3 (4.0 or GL_ARB_sample_shading for Program::Run).
 *
 * Each draw's state is compared with what was last set and only what differs is sent; begin() forgets what was set,
 * since the UI draws with OpenGL between renders. */

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>

#include "core/log.hh"
#include "gpu/brush_stroke.hh"
#include "gpu/gl.hh"
#include "gpu/gpu_device.hh"
#include "gpu/tessellate.hh"

namespace oanim::gpu {

static const char *COLOR_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
uniform mat3 u_matrix;
uniform float u_depth;
out vec4 v_color;
void main()
{
  vec3 p = u_matrix * vec3(a_pos, 1.0);
  gl_Position = vec4(p.xy, u_depth * 2.0 - 1.0, 1.0);
  v_color = a_color;
}
)";

/* Batched steps: each vertex's slot picks its step's place (clip space) and opacity. */
static const char *BATCH_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in float a_slot;
uniform vec4 u_slots[64];
uniform float u_depth;
out vec4 v_color;
void main()
{
  int s = int(a_slot + 0.5) * 2;
  vec4 m = u_slots[s];
  vec4 t = u_slots[s + 1];
  gl_Position = vec4(m.x * a_pos.x + m.z * a_pos.y + t.x, m.y * a_pos.x + m.w * a_pos.y + t.y, u_depth * 2.0 - 1.0,
                     1.0);
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
uniform float u_depth;
out vec2 v_uv;
out vec4 v_color;
void main()
{
  vec3 p = u_matrix * vec3(a_pos, 1.0);
  gl_Position = vec4(p.xy, u_depth * 2.0 - 1.0, 1.0);
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
uniform float u_depth;
out vec2 v_uv;
void main()
{
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, u_depth * 2.0 - 1.0, 1.0);
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

/* A layer run's image laid over the target, each sample over the same sample (gl_SampleID runs it per sample). */
static const char *RUN_FRAGMENT_SHADER = R"(#version 400 core
uniform sampler2DMS u_run;
out vec4 frag_color;
void main()
{
  frag_color = texelFetch(u_run, ivec2(gl_FragCoord.xy), gl_SampleID);
}
)";
/* The same where the context is OpenGL 3.3 with per-sample shading as an extension. */
static const char *RUN_FRAGMENT_SHADER_ARB = R"(#version 330 core
#extension GL_ARB_sample_shading : require
uniform sampler2DMS u_run;
out vec4 frag_color;
void main()
{
  frag_color = texelFetch(u_run, ivec2(gl_FragCoord.xy), gl_SampleID);
}
)";

/* A layer's own image laid over the target sample for sample, faded (a see-through layer as a whole). */
static const char *FADE_SAMPLES_FRAGMENT_SHADER = R"(#version 400 core
uniform sampler2DMS u_run;
uniform float u_opacity;
out vec4 frag_color;
void main()
{
  frag_color = texelFetch(u_run, ivec2(gl_FragCoord.xy), gl_SampleID) * u_opacity;
}
)";
static const char *FADE_SAMPLES_FRAGMENT_SHADER_ARB = R"(#version 330 core
#extension GL_ARB_sample_shading : require
uniform sampler2DMS u_run;
uniform float u_opacity;
out vec4 frag_color;
void main()
{
  frag_color = texelFetch(u_run, ivec2(gl_FragCoord.xy), gl_SampleID) * u_opacity;
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

/* Handles index these; slot 0 is never used, so a 0 handle is none. Freed slots are given out again. */
template<typename T> struct Slots {
  std::vector<T> items = std::vector<T>(1);
  std::vector<uint32_t> free;
  uint32_t add(const T &item)
  {
    if (!free.empty()) {
      const uint32_t id = free.back();
      free.pop_back();
      items[id] = item;
      return id;
    }
    items.push_back(item);
    return uint32_t(items.size() - 1);
  }
  T &operator[](uint32_t id) { return items[id]; }
  const T &operator[](uint32_t id) const { return items[id]; }
  void remove(uint32_t id)
  {
    items[id] = T();
    free.push_back(id);
  }
};

/* What a draw state's parts were last set to, or unknown (begin). */
static constexpr GLuint UNKNOWN = ~GLuint(0);

class GLDevice final : public Device {
 public:
  ~GLDevice() override;
  bool init() override;
  const Caps &caps() const override { return caps_; }
  void begin() override;
  void end() override;
  Mesh make_mesh(Layout layout, const void *data, size_t bytes, const uint32_t *indices = nullptr,
                 size_t index_count = 0) override;
  void free_mesh(Mesh &mesh) override;
  Texture make_texture() override;
  void upload_texture(Texture texture, int width, int height, const uint8_t *rgba) override;
  void free_texture(Texture &texture) override;
  DepthStencil make_depth_stencil(int width, int height, int samples) override;
  void free_depth_stencil(DepthStencil &depth_stencil) override;
  Target make_target(const TargetDesc &desc) override;
  void free_target(Target &target) override;
  bool has_sample_texture(Target target) const override { return targets_[target.id].color_texture; }
  Texture resolved_texture(Target target) const override { return targets_[target.id].resolved; }
  Texture sample_texture(Target target) const override { return targets_[target.id].samples; }
  unsigned int native_texture(Target target) const override
  {
    return textures_[targets_[target.id].resolved.id].name;
  }
  void set_scissor(bool on, int x0, int y0, int x1, int y1) override;
  void bind_target(Target target) override;
  void clear(const float premultiplied[4]) override;
  void draw(const Draw &draw) override;
  void resolve(Target target) override;
  void read_pixels(Target target, std::vector<uint8_t> &r_rgba) override;
  void set_timing(bool on) override { timing_ = on; }
  std::vector<Timing> timings() override;

 private:
  /* Timing: a timestamp each time the kind of draw changes. */
  void mark(const char *kind);
  static const char *kind_of(const Draw &d);
  bool timing_ = false;
  const char *last_kind_ = nullptr;
  std::vector<std::pair<const char *, GLuint>> marks_;
  /* The CPU's time in the device's calls and how many, by kind. */
  struct CallTime {
    const char *kind;
    double ms;
    int calls;
  };
  std::vector<CallTime> call_times_;
  void add_call_time(const char *kind, std::chrono::steady_clock::time_point start);
  std::vector<GLuint> queries_;
  struct GLMesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
  };
  struct GLTexture {
    GLuint name = 0;
    GLenum kind = GL_TEXTURE_2D;
    int width = 0, height = 0;
    /* A target's own (freed with it). */
    bool of_target = false;
  };
  struct GLTarget {
    GLuint draw_framebuffer = 0, color = 0;
    bool color_texture = false;
    GLuint depth_stencil = 0;
    bool own_depth_stencil = true;
    GLuint resolve_framebuffer = 0;
    Texture resolved, samples;
    int width = 0, height = 0;
  };
  /* Each program's uniforms (-1: it hasn't that one). */
  struct Uniforms {
    GLint matrix = -1, opacity = -1, slots = -1, own_colors = -1, alpha_test = -1, grey = -1, tint = -1, depth = -1;
  };
  static constexpr int PROGRAMS = int(Program::FadeSamples) + 1;

  void layout_attributes(Layout layout);
  void use_program(Program program);
  void bind_vertex_array(GLuint vao);
  void bind_array_buffer(GLuint vbo);
  void bind_texture(GLenum kind, GLuint name);
  void bind_framebuffer(GLuint framebuffer);
  void set_state(const DrawState &state);

  Caps caps_;
  bool initialized_ = false;
  GLuint programs_[PROGRAMS] = {};
  Uniforms uniforms_[PROGRAMS];
  /* Vertices uploaded for one draw, a buffer for each layout. */
  GLMesh stream_[4];
  GLuint empty_vao_ = 0;
  Slots<GLMesh> meshes_;
  Slots<GLTexture> textures_;
  Slots<GLTarget> targets_;
  Slots<GLuint> depth_stencils_;

  /* As last set. */
  GLuint program_ = UNKNOWN, vao_ = UNKNOWN, array_buffer_ = UNKNOWN, framebuffer_ = UNKNOWN;
  GLuint texture_2d_ = UNKNOWN, texture_ms_ = UNKNOWN;
  int viewport_[2] = {-1, -1};
  /* The scissor as last set (begin: off, box unknown). */
  bool scissor_on_ = false;
  int scissor_[4] = {0, 0, 0, 0};
  bool state_known_ = false;
  DrawState state_;
};

static GLenum gl_compare(Compare compare)
{
  switch (compare) {
    case Compare::Equal:
      return GL_EQUAL;
    case Compare::NotEqual:
      return GL_NOTEQUAL;
    case Compare::Less:
      return GL_LESS;
    case Compare::LessEqual:
      return GL_LEQUAL;
    case Compare::Always:
      break;
  }
  return GL_ALWAYS;
}

static GLenum gl_op(StencilOp op)
{
  switch (op) {
    case StencilOp::Replace:
      return GL_REPLACE;
    case StencilOp::Zero:
      return GL_ZERO;
    case StencilOp::Keep:
      break;
  }
  return GL_KEEP;
}

GLDevice::~GLDevice()
{
  if (!initialized_ || !gl::DeleteProgram) {
    return;
  }
  for (uint32_t id = 1; id < targets_.items.size(); id++) {
    if (targets_[id].draw_framebuffer) {
      Target t{id};
      free_target(t);
    }
  }
  for (uint32_t id = 1; id < depth_stencils_.items.size(); id++) {
    if (depth_stencils_[id]) {
      gl::DeleteRenderbuffers(1, &depth_stencils_[id]);
    }
  }
  for (uint32_t id = 1; id < textures_.items.size(); id++) {
    if (textures_[id].name) {
      glDeleteTextures(1, &textures_[id].name);
    }
  }
  for (uint32_t id = 1; id < meshes_.items.size(); id++) {
    if (meshes_[id].vao) {
      gl::DeleteVertexArrays(1, &meshes_[id].vao);
      gl::DeleteBuffers(1, &meshes_[id].vbo);
      if (meshes_[id].ebo) {
        gl::DeleteBuffers(1, &meshes_[id].ebo);
      }
    }
  }
  for (GLMesh &stream : stream_) {
    if (stream.vao) {
      gl::DeleteVertexArrays(1, &stream.vao);
      gl::DeleteBuffers(1, &stream.vbo);
    }
  }
  gl::DeleteVertexArrays(1, &empty_vao_);
  for (const GLuint program : programs_) {
    if (program) {
      gl::DeleteProgram(program);
    }
  }
}

bool GLDevice::init()
{
  if (!gl::load()) {
    return false;
  }
  const auto add = [this](Program p, const char *vertex, const char *fragment) {
    const GLuint program = link(vertex, fragment);
    programs_[int(p)] = program;
    if (!program) {
      return;
    }
    Uniforms &u = uniforms_[int(p)];
    u.matrix = gl::GetUniformLocation(program, "u_matrix");
    u.depth = gl::GetUniformLocation(program, "u_depth");
    u.opacity = gl::GetUniformLocation(program, "u_opacity");
    u.slots = gl::GetUniformLocation(program, "u_slots");
    u.own_colors = gl::GetUniformLocation(program, "u_own_colors");
    u.alpha_test = gl::GetUniformLocation(program, "u_alpha_test");
    u.grey = gl::GetUniformLocation(program, "u_grey");
    u.tint = gl::GetUniformLocation(program, "u_tint");
    /* Every program samples texture unit 0. */
    gl::UseProgram(program);
    for (const char *sampler : {"u_image", "u_source", "u_run"}) {
      const GLint at = gl::GetUniformLocation(program, sampler);
      if (at >= 0) {
        gl::Uniform1i(at, 0);
      }
    }
    gl::UseProgram(0);
  };
  add(Program::Color, COLOR_VERTEX_SHADER, COLOR_FRAGMENT_SHADER);
  add(Program::Batch, BATCH_VERTEX_SHADER, BATCH_FRAGMENT_SHADER);
  add(Program::Composite, COMPOSITE_VERTEX_SHADER, COMPOSITE_FRAGMENT_SHADER);
  add(Program::Stencil, COMPOSITE_VERTEX_SHADER, STENCIL_FRAGMENT_SHADER);
  add(Program::Brush, BRUSH_VERTEX_SHADER, BRUSH_FRAGMENT_SHADER);
  add(Program::Media, BRUSH_VERTEX_SHADER, MEDIA_FRAGMENT_SHADER);
  /* Layer runs need per-sample shading (OpenGL 4.0, or the extension). */
  GLint major = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  caps_.sample_textures = gl::TexImage2DMultisample != nullptr;
  if (caps_.sample_textures) {
    add(Program::Run, COMPOSITE_VERTEX_SHADER, major >= 4 ? RUN_FRAGMENT_SHADER : RUN_FRAGMENT_SHADER_ARB);
    add(Program::FadeSamples, COLOR_VERTEX_SHADER,
        major >= 4 ? FADE_SAMPLES_FRAGMENT_SHADER : FADE_SAMPLES_FRAGMENT_SHADER_ARB);
    caps_.sample_shading = programs_[int(Program::Run)] != 0 && programs_[int(Program::FadeSamples)] != 0;
  }
  caps_.batching = programs_[int(Program::Batch)] != 0;
  GLint max_samples = 1;
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  caps_.max_samples = int(max_samples);
  for (const Program p : {Program::Color, Program::Composite, Program::Stencil, Program::Brush, Program::Media}) {
    if (!programs_[int(p)]) {
      return false;
    }
  }
  for (const Layout layout : {Layout::Color, Layout::Batch, Layout::Brush}) {
    GLMesh &stream = stream_[int(layout)];
    gl::GenVertexArrays(1, &stream.vao);
    gl::GenBuffers(1, &stream.vbo);
    gl::BindVertexArray(stream.vao);
    gl::BindBuffer(GL_ARRAY_BUFFER, stream.vbo);
    layout_attributes(layout);
  }
  gl::GenVertexArrays(1, &empty_vao_);
  gl::BindVertexArray(0);
  initialized_ = true;
  return true;
}

void GLDevice::layout_attributes(Layout layout)
{
  const auto attribute = [](GLuint index, GLint size, GLsizei stride, size_t offset) {
    gl::EnableVertexAttribArray(index);
    gl::VertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void *>(offset));
  };
  switch (layout) {
    case Layout::Color:
      attribute(0, 2, sizeof(Vertex), offsetof(Vertex, x));
      attribute(1, 4, sizeof(Vertex), offsetof(Vertex, r));
      break;
    case Layout::Batch:
      attribute(0, 2, sizeof(BatchVertex), offsetof(BatchVertex, x));
      attribute(1, 4, sizeof(BatchVertex), offsetof(BatchVertex, r));
      attribute(2, 1, sizeof(BatchVertex), offsetof(BatchVertex, slot));
      break;
    case Layout::Brush:
      attribute(0, 2, sizeof(BrushVertex), offsetof(BrushVertex, x));
      attribute(1, 2, sizeof(BrushVertex), offsetof(BrushVertex, u));
      attribute(2, 4, sizeof(BrushVertex), offsetof(BrushVertex, r));
      break;
    case Layout::None:
      break;
  }
}

void GLDevice::begin()
{
  marks_.clear();
  call_times_.clear();
  last_kind_ = nullptr;
  program_ = vao_ = array_buffer_ = framebuffer_ = texture_2d_ = texture_ms_ = UNKNOWN;
  viewport_[0] = viewport_[1] = -1;
  glDisable(GL_SCISSOR_TEST);
  scissor_on_ = false;
  /* The UI and other renderers share the context and set their own boxes. */
  scissor_[0] = scissor_[1] = scissor_[2] = scissor_[3] = -1;
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  gl::ActiveTexture(GL_TEXTURE0);
  /* Premultiplied "over" (Blend::Max changes only the equation). */
  glEnable(GL_BLEND);
  gl::BlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  state_known_ = false;
}

void GLDevice::end()
{
  mark("end");
  glDisable(GL_SCISSOR_TEST);
  scissor_on_ = false;
  /* As the UI's drawing expects: stencil off and writable, all colour written, nothing bound. Blending stays on,
   * premultiplied over. */
  glDisable(GL_STENCIL_TEST);
  glStencilMask(0xFF);
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_LESS);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  gl::BlendEquation(GL_FUNC_ADD);
  gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
  gl::BindVertexArray(0);
  gl::UseProgram(0);
  program_ = vao_ = array_buffer_ = framebuffer_ = texture_2d_ = texture_ms_ = UNKNOWN;
  state_known_ = false;
}

void GLDevice::use_program(Program program)
{
  const GLuint name = programs_[int(program)];
  if (program_ != name) {
    gl::UseProgram(name);
    program_ = name;
  }
}

void GLDevice::bind_vertex_array(GLuint vao)
{
  if (vao_ != vao) {
    gl::BindVertexArray(vao);
    vao_ = vao;
  }
}

void GLDevice::bind_array_buffer(GLuint vbo)
{
  if (array_buffer_ != vbo) {
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
    array_buffer_ = vbo;
  }
}

void GLDevice::bind_texture(GLenum kind, GLuint name)
{
  GLuint &bound = kind == GL_TEXTURE_2D ? texture_2d_ : texture_ms_;
  if (bound != name) {
    glBindTexture(kind, name);
    bound = name;
  }
}

void GLDevice::bind_framebuffer(GLuint framebuffer)
{
  if (framebuffer_ != framebuffer) {
    gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    framebuffer_ = framebuffer;
  }
}

void GLDevice::set_state(const DrawState &state)
{
  const bool all = !state_known_;
  const DrawState &was = state_;
  if (all || state.write_color != was.write_color) {
    const GLboolean on = state.write_color ? GL_TRUE : GL_FALSE;
    glColorMask(on, on, on, on);
  }
  if (all || state.blend != was.blend) {
    gl::BlendEquation(state.blend == Blend::Max ? GL_MAX : GL_FUNC_ADD);
  }
  const StencilState &s = state.stencil, &w = was.stencil;
  if (all || s.test != w.test) {
    if (s.test) {
      glEnable(GL_STENCIL_TEST);
    }
    else {
      glDisable(GL_STENCIL_TEST);
    }
  }
  if (all || s.compare != w.compare || s.ref != w.ref || s.read_mask != w.read_mask) {
    glStencilFunc(gl_compare(s.compare), GLint(s.ref), s.read_mask);
  }
  if (all || s.fail != w.fail || s.pass != w.pass || s.winding != w.winding || s.depth_fail != w.depth_fail) {
    if (s.winding) {
      gl::StencilOpSeparate(GL_FRONT, gl_op(s.fail), gl_op(s.depth_fail), GL_INCR_WRAP);
      gl::StencilOpSeparate(GL_BACK, gl_op(s.fail), gl_op(s.depth_fail), GL_DECR_WRAP);
    }
    else {
      glStencilOp(gl_op(s.fail), gl_op(s.depth_fail), gl_op(s.pass));
    }
  }
  if (all || s.write_mask != w.write_mask) {
    glStencilMask(s.write_mask);
  }
  /* Depth is written only with the test on (as Always, if that's all). */
  const bool depth_on = state.depth_test != Compare::Always || state.depth_write;
  const bool depth_was = was.depth_test != Compare::Always || was.depth_write;
  if (all || depth_on != depth_was) {
    if (depth_on) {
      glEnable(GL_DEPTH_TEST);
    }
    else {
      glDisable(GL_DEPTH_TEST);
    }
  }
  if (all || state.depth_test != was.depth_test) {
    glDepthFunc(gl_compare(state.depth_test));
  }
  if (all || state.depth_write != was.depth_write) {
    glDepthMask(state.depth_write ? GL_TRUE : GL_FALSE);
  }
  state_ = state;
  state_known_ = true;
}

void GLDevice::mark(const char *kind)
{
  if (!timing_ || kind == last_kind_ || !gl::QueryCounter) {
    return;
  }
  if (marks_.size() == queries_.size()) {
    const size_t more = std::max<size_t>(64, queries_.size());
    queries_.resize(queries_.size() + more);
    gl::GenQueries(GLsizei(more), queries_.data() + queries_.size() - more);
  }
  const GLuint query = queries_[marks_.size()];
  gl::QueryCounter(query, GL_TIMESTAMP);
  marks_.emplace_back(kind, query);
  last_kind_ = kind;
}

void GLDevice::add_call_time(const char *kind, std::chrono::steady_clock::time_point start)
{
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  for (CallTime &c : call_times_) {
    if (c.kind == kind) {
      c.ms += ms;
      c.calls++;
      return;
    }
  }
  call_times_.push_back({kind, ms, 1});
}

std::vector<Timing> GLDevice::timings()
{
  std::vector<Timing> out;
  const auto find = [&](const std::string &kind) -> Timing & {
    for (Timing &t : out) {
      if (t.kind == kind) {
        return t;
      }
    }
    out.push_back({kind});
    return out.back();
  };
  for (const CallTime &c : call_times_) {
    Timing &t = find(c.kind);
    t.cpu_ms += c.ms;
    t.calls += c.calls;
  }
  if (marks_.size() < 2) {
    return out;
  }
  std::vector<GLuint64> at(marks_.size());
  for (size_t i = 0; i < marks_.size(); i++) {
    gl::GetQueryObjectui64v(marks_[i].second, GL_QUERY_RESULT, &at[i]);
  }
  /* From each mark to the next is the GPU's time on that kind of draw (and any wait for the CPU before the next). */
  for (size_t i = 0; i + 1 < marks_.size(); i++) {
    find(marks_[i].first).gpu_ms += double(at[i + 1] - at[i]) / 1e6;
  }
  return out;
}

const char *GLDevice::kind_of(const Draw &d)
{
  const StencilState &s = d.state.stencil;
  switch (d.state.program) {
    case Program::Batch:
      return "batches";
    case Program::Brush:
      return d.state.write_color ? "stamps" : "mask shapes";
    case Program::Media:
      return "media";
    case Program::Composite:
    case Program::Run:
    case Program::FadeSamples:
      return "composites";
    case Program::Stencil:
      return d.state.depth_write ? "mask depth" : "mask passes";
    case Program::Color:
      if (s.winding) {
        return "fill fans";
      }
      if (d.state.write_color) {
        return s.test && s.pass == StencilOp::Zero ? "fill covers" : "colour draws";
      }
      return s.pass == StencilOp::Zero && s.fail == StencilOp::Zero ? "fan clears" : "mask shapes";
  }
  return "other";
}

void GLDevice::draw(const Draw &d)
{
  if (d.count <= 0) {
    return;
  }
  const char *kind = kind_of(d);
  mark(kind);
  const auto call_start = timing_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
  set_state(d.state);
  use_program(d.state.program);
  const Uniforms &u = uniforms_[int(d.state.program)];
  if (u.matrix >= 0) {
    const Transform2D &m = d.to_clip;
    const float matrix[9] = {m.a, m.b, 0.0f, m.c, m.d, 0.0f, m.tx, m.ty, 1.0f};
    gl::UniformMatrix3fv(u.matrix, 1, GL_FALSE, matrix);
  }
  if (u.opacity >= 0) {
    gl::Uniform1f(u.opacity, d.opacity);
  }
  if (u.depth >= 0) {
    gl::Uniform1f(u.depth, d.state.depth);
  }
  if (u.own_colors >= 0) {
    gl::Uniform1f(u.own_colors, d.own_colors ? 1.0f : 0.0f);
  }
  if (u.alpha_test >= 0) {
    gl::Uniform1f(u.alpha_test, d.alpha_test);
  }
  if (u.grey >= 0) {
    gl::Uniform1f(u.grey, d.grey);
  }
  if (u.tint >= 0) {
    gl::Uniform4f(u.tint, d.tint[0], d.tint[1], d.tint[2], d.tint[3]);
  }
  if (u.slots >= 0 && d.slots) {
    gl::Uniform4fv(u.slots, GLsizei(d.slot_count * 2), d.slots);
  }
  if (d.texture) {
    const GLTexture &t = textures_[d.texture.id];
    bind_texture(t.kind, t.name);
  }
  if (d.mesh) {
    bind_vertex_array(meshes_[d.mesh.id].vao);
  }
  else if (d.stream) {
    const GLMesh &stream = stream_[int(d.stream_layout)];
    bind_vertex_array(stream.vao);
    bind_array_buffer(stream.vbo);
    gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(d.stream_bytes), d.stream, GL_STREAM_DRAW);
  }
  else {
    bind_vertex_array(empty_vao_);
  }
  if (d.indexed) {
    glDrawElements(GL_TRIANGLES, d.count, GL_UNSIGNED_INT,
                   reinterpret_cast<const void *>(size_t(d.first) * sizeof(uint32_t)));
  }
  else {
    glDrawArrays(GL_TRIANGLES, d.first, d.count);
  }
  if (timing_) {
    add_call_time(kind, call_start);
  }
}

Mesh GLDevice::make_mesh(Layout layout, const void *data, size_t bytes, const uint32_t *indices,
                         size_t index_count)
{
  GLMesh mesh;
  gl::GenVertexArrays(1, &mesh.vao);
  gl::GenBuffers(1, &mesh.vbo);
  bind_vertex_array(mesh.vao);
  bind_array_buffer(mesh.vbo);
  gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes), data, GL_STATIC_DRAW);
  layout_attributes(layout);
  if (indices && index_count > 0) {
    /* Part of the vertex array's state, as it is bound. */
    gl::GenBuffers(1, &mesh.ebo);
    gl::BindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    gl::BufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(index_count * sizeof(uint32_t)), indices, GL_STATIC_DRAW);
  }
  return Mesh{meshes_.add(mesh)};
}

void GLDevice::free_mesh(Mesh &mesh)
{
  if (!mesh) {
    return;
  }
  GLMesh &m = meshes_[mesh.id];
  if (vao_ == m.vao) {
    vao_ = UNKNOWN;
  }
  if (array_buffer_ == m.vbo) {
    array_buffer_ = UNKNOWN;
  }
  gl::DeleteVertexArrays(1, &m.vao);
  gl::DeleteBuffers(1, &m.vbo);
  if (m.ebo) {
    gl::DeleteBuffers(1, &m.ebo);
  }
  meshes_.remove(mesh.id);
  mesh = Mesh();
}

Texture GLDevice::make_texture()
{
  GLTexture t;
  glGenTextures(1, &t.name);
  return Texture{textures_.add(t)};
}

void GLDevice::upload_texture(Texture texture, int width, int height, const uint8_t *rgba)
{
  GLTexture &t = textures_[texture.id];
  bind_texture(GL_TEXTURE_2D, t.name);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (t.width != width || t.height != height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    t.width = width;
    t.height = height;
  }
  else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  }
  gl::GenerateMipmap(GL_TEXTURE_2D);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void GLDevice::free_texture(Texture &texture)
{
  if (!texture) {
    return;
  }
  GLTexture &t = textures_[texture.id];
  if (texture_2d_ == t.name || texture_ms_ == t.name) {
    texture_2d_ = texture_ms_ = UNKNOWN;
  }
  glDeleteTextures(1, &t.name);
  textures_.remove(texture.id);
  texture = Texture();
}

DepthStencil GLDevice::make_depth_stencil(int width, int height, int samples)
{
  GLuint buffer = 0;
  gl::GenRenderbuffers(1, &buffer);
  gl::BindRenderbuffer(GL_RENDERBUFFER, buffer);
  gl::RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
  return DepthStencil{depth_stencils_.add(buffer)};
}

void GLDevice::free_depth_stencil(DepthStencil &depth_stencil)
{
  if (!depth_stencil) {
    return;
  }
  gl::DeleteRenderbuffers(1, &depth_stencils_[depth_stencil.id]);
  depth_stencils_.remove(depth_stencil.id);
  depth_stencil = DepthStencil();
}

Target GLDevice::make_target(const TargetDesc &desc)
{
  GLTarget t;
  t.width = desc.width;
  t.height = desc.height;
  /* 0 samples asks for a plain (not multisampled) buffer. */
  const int samples = desc.samples > 1 ? desc.samples : 0;
  t.color_texture = samples > 0 && desc.sample_texture && caps_.sample_textures;
  if (t.color_texture) {
    GLTexture color;
    color.kind = GL_TEXTURE_2D_MULTISAMPLE;
    color.of_target = true;
    color.width = desc.width;
    color.height = desc.height;
    glGenTextures(1, &color.name);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, color.name);
    gl::TexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_RGBA8, desc.width, desc.height, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    texture_ms_ = 0;
    t.color = color.name;
    t.samples = Texture{textures_.add(color)};
  }
  else {
    gl::GenRenderbuffers(1, &t.color);
    gl::BindRenderbuffer(GL_RENDERBUFFER, t.color);
    gl::RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, desc.width, desc.height);
  }
  t.own_depth_stencil = !desc.depth_stencil;
  if (t.own_depth_stencil) {
    gl::GenRenderbuffers(1, &t.depth_stencil);
    gl::BindRenderbuffer(GL_RENDERBUFFER, t.depth_stencil);
    gl::RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, desc.width, desc.height);
  }
  else {
    t.depth_stencil = depth_stencils_[desc.depth_stencil.id];
  }
  const GLuint was = framebuffer_;
  gl::GenFramebuffers(1, &t.draw_framebuffer);
  gl::BindFramebuffer(GL_FRAMEBUFFER, t.draw_framebuffer);
  if (t.color_texture) {
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, t.color, 0);
  }
  else {
    gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.color);
  }
  gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth_stencil);
  if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    report(ReportType::Error, desc.resolved ? "Could not create a multisampled image for drawing" :
                                              "Could not create an image for a run of layers");
  }
  if (desc.resolved) {
    GLTexture image;
    image.of_target = true;
    image.width = desc.width;
    image.height = desc.height;
    glGenTextures(1, &image.name);
    glBindTexture(GL_TEXTURE_2D, image.name);
    texture_2d_ = image.name;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, desc.width, desc.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::GenFramebuffers(1, &t.resolve_framebuffer);
    gl::BindFramebuffer(GL_FRAMEBUFFER, t.resolve_framebuffer);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image.name, 0);
    if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      report(ReportType::Error, "Could not create an offscreen image for drawing");
    }
    t.resolved = Texture{textures_.add(image)};
  }
  /* What was bound stays bound. */
  gl::BindFramebuffer(GL_FRAMEBUFFER, was == UNKNOWN ? 0 : was);
  framebuffer_ = was == UNKNOWN ? 0 : was;
  return Target{targets_.add(t)};
}

void GLDevice::free_target(Target &target)
{
  if (!target) {
    return;
  }
  GLTarget &t = targets_[target.id];
  if (framebuffer_ == t.draw_framebuffer || framebuffer_ == t.resolve_framebuffer) {
    framebuffer_ = UNKNOWN;
  }
  gl::DeleteFramebuffers(1, &t.draw_framebuffer);
  if (t.resolve_framebuffer) {
    gl::DeleteFramebuffers(1, &t.resolve_framebuffer);
  }
  if (t.color_texture) {
    free_texture(t.samples);
  }
  else {
    gl::DeleteRenderbuffers(1, &t.color);
  }
  if (t.own_depth_stencil) {
    gl::DeleteRenderbuffers(1, &t.depth_stencil);
  }
  free_texture(t.resolved);
  targets_.remove(target.id);
  target = Target();
}

void GLDevice::set_scissor(bool on, int x0, int y0, int x1, int y1)
{
  if (on != scissor_on_) {
    if (on) {
      glEnable(GL_SCISSOR_TEST);
    }
    else {
      glDisable(GL_SCISSOR_TEST);
    }
    scissor_on_ = on;
  }
  if (on && (x0 != scissor_[0] || y0 != scissor_[1] || x1 != scissor_[2] || y1 != scissor_[3])) {
    glScissor(x0, y0, std::max(x1 - x0, 0), std::max(y1 - y0, 0));
    scissor_[0] = x0;
    scissor_[1] = y0;
    scissor_[2] = x1;
    scissor_[3] = y1;
  }
}

void GLDevice::bind_target(Target target)
{
  const GLTarget &t = targets_[target.id];
  bind_framebuffer(t.draw_framebuffer);
  if (viewport_[0] != t.width || viewport_[1] != t.height) {
    glViewport(0, 0, t.width, t.height);
    viewport_[0] = t.width;
    viewport_[1] = t.height;
  }
}

void GLDevice::clear(const float premultiplied[4])
{
  mark("clears");
  if (!state_known_ || !state_.write_color) {
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (state_known_) {
      state_.write_color = true;
    }
  }
  glStencilMask(0xFF);
  glDepthMask(GL_TRUE);
  if (state_known_) {
    state_.stencil.write_mask = 0xFF;
    state_.depth_write = true;
  }
  glClearColor(premultiplied[0], premultiplied[1], premultiplied[2], premultiplied[3]);
  glClearStencil(0);
  glClearDepth(0.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void GLDevice::resolve(Target target)
{
  mark("resolves");
  /* Average each pixel's samples into the target's image. */
  const GLTarget &t = targets_[target.id];
  gl::BindFramebuffer(GL_READ_FRAMEBUFFER, t.draw_framebuffer);
  gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, t.resolve_framebuffer);
  gl::BlitFramebuffer(0, 0, t.width, t.height, 0, 0, t.width, t.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  framebuffer_ = UNKNOWN;
}

void GLDevice::read_pixels(Target target, std::vector<uint8_t> &r_rgba)
{
  const GLTarget &t = targets_[target.id];
  r_rgba.resize(size_t(t.width) * size_t(t.height) * 4);
  gl::BindFramebuffer(GL_READ_FRAMEBUFFER, t.resolve_framebuffer);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, t.width, t.height, GL_RGBA, GL_UNSIGNED_BYTE, r_rgba.data());
  gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
  framebuffer_ = UNKNOWN;
}

std::unique_ptr<Device> make_gl_device()
{
  return std::make_unique<GLDevice>();
}

}  // namespace oanim::gpu
