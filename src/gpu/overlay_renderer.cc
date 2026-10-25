#include "gpu/overlay_renderer.hh"

#include <string>
#include <unordered_map>

#include "imgui.h"

#include "core/log.hh"
#include "gpu/gl.hh"

namespace oanim {

/* Each line or mark is one instance: six corners (two triangles) placed by the vertex shader from
 * gl_VertexID, so the buffers hold one entry per line piece or point. */
static const char *LINE_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_from;
layout(location = 1) in vec2 a_to;
layout(location = 2) in vec4 a_color;
layout(location = 3) in float a_width;
uniform mat3 u_to_screen;
uniform vec2 u_display_pos;
uniform vec2 u_display_size;
out vec4 v_color;
out float v_across;
out float v_half;
const vec2 CORNERS[6] = vec2[](vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(0.0, -1.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
void main()
{
  vec2 corner = CORNERS[gl_VertexID];
  vec2 from = (u_to_screen * vec3(a_from, 1.0)).xy;
  vec2 to = (u_to_screen * vec3(a_to, 1.0)).xy;
  vec2 along = to - from;
  float len = length(along);
  vec2 dir = len > 1e-6 ? along / len : vec2(1.0, 0.0);
  vec2 normal = vec2(-dir.y, dir.x);
  /* A pixel of soft edge each side. */
  float half_width = a_width * 0.5;
  float reach = half_width + 1.0;
  vec2 p = mix(from, to, corner.x) + normal * corner.y * reach;
  v_across = corner.y * reach;
  v_half = half_width;
  v_color = a_color;
  vec2 ndc = (p - u_display_pos) / u_display_size * 2.0 - 1.0;
  gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

static const char *LINE_FRAGMENT_SHADER = R"(#version 330 core
in vec4 v_color;
in float v_across;
in float v_half;
out vec4 frag_color;
void main()
{
  float cover = clamp(v_half + 0.5 - abs(v_across), 0.0, 1.0);
  frag_color = vec4(v_color.rgb, v_color.a * cover);
}
)";

static const char *MARK_VERTEX_SHADER = R"(#version 330 core
layout(location = 0) in vec2 a_at;
layout(location = 1) in vec4 a_fill;
layout(location = 2) in vec4 a_outline;
layout(location = 3) in float a_radius;
layout(location = 4) in float a_round;
uniform mat3 u_to_screen;
uniform vec2 u_display_pos;
uniform vec2 u_display_size;
out vec4 v_fill;
out vec4 v_outline;
out vec2 v_local;
out float v_radius;
out float v_round;
const vec2 CORNERS[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
void main()
{
  vec2 corner = CORNERS[gl_VertexID];
  vec2 at = (u_to_screen * vec3(a_at, 1.0)).xy;
  float reach = a_radius + 1.0;
  vec2 p = at + corner * reach;
  v_local = corner * reach;
  v_radius = a_radius;
  v_round = a_round;
  v_fill = a_fill;
  v_outline = a_outline;
  vec2 ndc = (p - u_display_pos) / u_display_size * 2.0 - 1.0;
  gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

static const char *MARK_FRAGMENT_SHADER = R"(#version 330 core
in vec4 v_fill;
in vec4 v_outline;
in vec2 v_local;
in float v_radius;
in float v_round;
out vec4 frag_color;
void main()
{
  /* How far outside the edge (negative inside), in pixels. */
  float d = v_round > 0.5 ? length(v_local) - v_radius : max(abs(v_local.x), abs(v_local.y)) - v_radius;
  /* Dots soft-edged; squares hard, as ImGui fills a rect. A pixel-wide outline: across a dot's edge, just
   * inside a square's. */
  float fill = v_round > 0.5 ? clamp(0.5 - d, 0.0, 1.0) : (d <= 0.0 ? 1.0 : 0.0);
  float band = v_round > 0.5 ? clamp(1.0 - abs(d), 0.0, 1.0) : clamp(1.0 - abs(d + 0.5), 0.0, 1.0);
  float base_a = v_fill.a * fill;
  float line_a = v_outline.a * band;
  float a = line_a + base_a * (1.0 - line_a);
  if (a <= 0.0) {
    discard;
  }
  vec3 rgb = (v_outline.rgb * line_a + v_fill.rgb * base_a * (1.0 - line_a)) / a;
  frag_color = vec4(rgb, a);
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
    report(ReportType::Error, std::string("Overlay shader error: ") + log);
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
    report(ReportType::Error, std::string("Overlay shader link error: ") + log);
    return 0;
  }
  return program;
}

namespace {

struct Kept {
  uint64_t key = 0;
  float2 origin;
  unsigned int line_vao = 0, line_vbo = 0, mark_vao = 0, mark_vbo = 0;
  int lines = 0, marks = 0;
  int used = 0;
};

struct Queued {
  uint64_t layer = 0;
  Transform2D to_screen;
};

struct OverlayState {
  std::unordered_map<uint64_t, Kept> kept;
  /* This frame's draws, one list per flush (a viewport each); taken by the callbacks as ImGui renders. */
  std::vector<std::vector<Queued>> batches;
  std::vector<Queued> queued;
  int frame = -1;
  unsigned int line_program = 0, mark_program = 0;
  bool ready = false;
};

}  // namespace

static OverlayState &state()
{
  static OverlayState s;
  return s;
}

/* A new frame: the last frame's batches have all been drawn. */
static void begin_frame()
{
  OverlayState &s = state();
  if (s.frame != ImGui::GetFrameCount()) {
    s.frame = ImGui::GetFrameCount();
    s.batches.clear();
    s.queued.clear();
  }
}

static void free_kept(Kept &k)
{
  if (k.line_vao) {
    gl::DeleteVertexArrays(1, &k.line_vao);
    gl::DeleteBuffers(1, &k.line_vbo);
  }
  if (k.mark_vao) {
    gl::DeleteVertexArrays(1, &k.mark_vao);
    gl::DeleteBuffers(1, &k.mark_vbo);
  }
  k = Kept();
}

bool overlay_kept(uint64_t layer, uint64_t key)
{
  const auto it = state().kept.find(layer);
  return it != state().kept.end() && it->second.key == key;
}

float2 overlay_kept_origin(uint64_t layer)
{
  const auto it = state().kept.find(layer);
  return it == state().kept.end() ? float2() : it->second.origin;
}

/* Puts `data` in a buffer of its own with a vertex array reading one entry per instance. */
static void upload(unsigned int &vao, unsigned int &vbo, const void *data, size_t bytes)
{
  if (!vao) {
    gl::GenVertexArrays(1, &vao);
    gl::GenBuffers(1, &vbo);
  }
  gl::BindVertexArray(vao);
  gl::BindBuffer(GL_ARRAY_BUFFER, vbo);
  gl::BufferData(GL_ARRAY_BUFFER, GLsizeiptr(bytes), data, GL_STATIC_DRAW);
}

static void attribute(GLuint index, GLint size, GLenum type, bool normalized, GLsizei stride, size_t offset)
{
  gl::EnableVertexAttribArray(index);
  gl::VertexAttribPointer(index, size, type, normalized ? GL_TRUE : GL_FALSE, stride,
                          reinterpret_cast<const void *>(offset));
  gl::VertexAttribDivisor(index, 1);
}

void overlay_keep(uint64_t layer, uint64_t key, const OverlayGeometry &geometry, float2 origin)
{
  Kept &k = state().kept[layer];
  k.key = key;
  k.origin = origin;
  k.lines = int(geometry.lines.size());
  k.marks = int(geometry.marks.size());
  if (!geometry.lines.empty()) {
    upload(k.line_vao, k.line_vbo, geometry.lines.data(), geometry.lines.size() * sizeof(OverlayLine));
    const GLsizei stride = sizeof(OverlayLine);
    attribute(0, 2, GL_FLOAT, false, stride, offsetof(OverlayLine, a));
    attribute(1, 2, GL_FLOAT, false, stride, offsetof(OverlayLine, b));
    attribute(2, 4, GL_UNSIGNED_BYTE, true, stride, offsetof(OverlayLine, color));
    attribute(3, 1, GL_FLOAT, false, stride, offsetof(OverlayLine, width));
  }
  if (!geometry.marks.empty()) {
    upload(k.mark_vao, k.mark_vbo, geometry.marks.data(), geometry.marks.size() * sizeof(OverlayMark));
    const GLsizei stride = sizeof(OverlayMark);
    attribute(0, 2, GL_FLOAT, false, stride, offsetof(OverlayMark, at));
    attribute(1, 4, GL_UNSIGNED_BYTE, true, stride, offsetof(OverlayMark, fill));
    attribute(2, 4, GL_UNSIGNED_BYTE, true, stride, offsetof(OverlayMark, outline));
    attribute(3, 1, GL_FLOAT, false, stride, offsetof(OverlayMark, radius));
    attribute(4, 1, GL_FLOAT, false, stride, offsetof(OverlayMark, round));
  }
  gl::BindVertexArray(0);
  gl::BindBuffer(GL_ARRAY_BUFFER, 0);
}

void overlay_queue(uint64_t layer, const Transform2D &to_screen)
{
  begin_frame();
  OverlayState &s = state();
  if (auto it = s.kept.find(layer); it != s.kept.end()) {
    it->second.used = s.frame;
    s.queued.push_back({layer, to_screen});
  }
}

static void set_matrix(GLuint program, const Transform2D &t)
{
  const float m[9] = {t.a, t.b, 0.0f, t.c, t.d, 0.0f, t.tx, t.ty, 1.0f};
  gl::UniformMatrix3fv(gl::GetUniformLocation(program, "u_to_screen"), 1, GL_FALSE, m);
}

static void draw_batch(const ImDrawList * /*list*/, const ImDrawCmd *cmd)
{
  OverlayState &s = state();
  const size_t index = size_t(reinterpret_cast<uintptr_t>(cmd->UserCallbackData));
  if (index >= s.batches.size()) {
    return;
  }
  if (!s.ready) {
    s.ready = true;
    s.line_program = link(LINE_VERTEX_SHADER, LINE_FRAGMENT_SHADER);
    s.mark_program = link(MARK_VERTEX_SHADER, MARK_FRAGMENT_SHADER);
  }
  if (!s.line_program || !s.mark_program) {
    return;
  }
  const ImDrawData *dd = ImGui::GetDrawData();
  const ImVec2 pos = dd ? dd->DisplayPos : ImVec2(0, 0);
  const ImVec2 size = dd ? dd->DisplaySize : ImGui::GetIO().DisplaySize;
  const ImVec2 scale = dd ? dd->FramebufferScale : ImVec2(1, 1);
  /* ImGui's clipping for where this sits (the viewport's region). */
  const ImVec4 clip = cmd->ClipRect;
  glEnable(GL_SCISSOR_TEST);
  glScissor(GLint((clip.x - pos.x) * scale.x), GLint((size.y - (clip.w - pos.y)) * scale.y),
            GLsizei((clip.z - clip.x) * scale.x), GLsizei((clip.w - clip.y) * scale.y));
  glEnable(GL_BLEND);
  gl::BlendEquation(GL_FUNC_ADD);
  gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  for (GLuint program : {s.line_program, s.mark_program}) {
    gl::UseProgram(program);
    gl::Uniform2f(gl::GetUniformLocation(program, "u_display_pos"), pos.x, pos.y);
    gl::Uniform2f(gl::GetUniformLocation(program, "u_display_size"), size.x, size.y);
  }
  /* Layer by layer, as ImGui drew them: its lines, then its marks. */
  for (const Queued &q : s.batches[index]) {
    const auto it = s.kept.find(q.layer);
    if (it == s.kept.end()) {
      continue;
    }
    const Kept &k = it->second;
    if (k.lines > 0) {
      gl::UseProgram(s.line_program);
      set_matrix(s.line_program, q.to_screen);
      gl::BindVertexArray(k.line_vao);
      gl::DrawArraysInstanced(GL_TRIANGLES, 0, 6, k.lines);
    }
    if (k.marks > 0) {
      gl::UseProgram(s.mark_program);
      set_matrix(s.mark_program, q.to_screen);
      gl::BindVertexArray(k.mark_vao);
      gl::DrawArraysInstanced(GL_TRIANGLES, 0, 6, k.marks);
    }
  }
  gl::BindVertexArray(0);
}

void overlay_flush(ImDrawList *dl)
{
  begin_frame();
  OverlayState &s = state();
  if (s.queued.empty()) {
    return;
  }
  s.batches.push_back(std::move(s.queued));
  s.queued.clear();
  dl->AddCallback(draw_batch, reinterpret_cast<void *>(uintptr_t(s.batches.size() - 1)));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void overlay_collect()
{
  OverlayState &s = state();
  for (auto it = s.kept.begin(); it != s.kept.end();) {
    if (it->second.used + 120 < s.frame) {
      free_kept(it->second);
      it = s.kept.erase(it);
    }
    else {
      ++it;
    }
  }
}

}  // namespace oanim
