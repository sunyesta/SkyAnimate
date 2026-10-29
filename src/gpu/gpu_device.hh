#pragma once

/* The render-command layer: what the scene renderer asks of the GPU, without naming a graphics API.
 *
 * Every draw carries its whole state (program, blending, colour writes, the stencil test and what it
 * writes), so nothing one draw leaves behind changes the next; the backend works out what to change
 * from what it last set. Resources are handles the backend owns: meshes (vertices kept on the GPU),
 * textures and targets (images drawn into). The renderer's shaders are a fixed list, written in each
 * backend's own language.
 *
 * Commands run at once, in the order given. The OpenGL backend (gpu_device_gl.cc) is the only one so
 * far; another (WebGPU) would go behind the same calls. */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math.hh"

namespace oanim::gpu {

/* The renderer's shaders. */
enum class Program : uint8_t {
  /* Triangles in a colour each (Vertex), placed by `to_clip`, faded by `opacity`. */
  Color,
  /* Batched steps (BatchVertex): each vertex's slot picks its step's place and opacity in `slots`. */
  Batch,
  /* Brush dabs (BrushVertex): `texture` tinted by each dab's colour, or in its own colours. */
  Brush,
  /* A Media layer's picture (BrushVertex): greyscale and tint. */
  Media,
  /* A full-screen triangle: `texture` (a target's resolved image) faded by `opacity`. */
  Composite,
  /* A full-screen triangle writing no colour (stencil passes). */
  Stencil,
  /* A full-screen triangle laying a target's samples (`texture`, a sample texture) over the same samples.
   * Only where Caps::sample_shading. */
  Run,
};

enum class Blend : uint8_t {
  /* Premultiplied "over". */
  Over,
  /* The larger of what is there and what is drawn, per channel (merged brush dabs). */
  Max,
};

/* For the stencil: (held & read_mask) against (ref & read_mask). For depth: the draw's depth against what is held
 * (Less: drawn where its depth is less). */
enum class Compare : uint8_t { Always, Equal, NotEqual, Less, LessEqual };
/* Replace writes `ref`'s bits, Zero clears them (both under `write_mask`). */
enum class StencilOp : uint8_t { Keep, Replace, Zero };

struct StencilState {
  bool test = true;
  /* Passes where (stencil & read_mask) compares so with (ref & read_mask). */
  Compare compare = Compare::Always;
  unsigned ref = 0, read_mask = 0;
  /* What happens to the `write_mask` bits where the test fails, and where it passes. */
  StencilOp fail = StencilOp::Keep, pass = StencilOp::Keep;
  unsigned write_mask = 0;
  /* Counting winding instead of `pass`: where the test passes, a triangle facing one way adds one and one facing the
   * other way takes one (wrapping round), so a fan of a path's triangles leaves its winding number there. */
  bool winding = false;
  /* Where the stencil test passes but the depth test doesn't. */
  StencilOp depth_fail = StencilOp::Keep;
  bool operator==(const StencilState &) const = default;
};

struct DrawState {
  Program program = Program::Color;
  bool write_color = true;
  Blend blend = Blend::Over;
  StencilState stencil;
  /* Every vertex's depth, 0 to 1: what the depth test compares with what is held, and what a depth write leaves. */
  float depth = 0.5f;
  Compare depth_test = Compare::Always;
  bool depth_write = false;
  bool operator==(const DrawState &) const = default;
};

/* A batched step's vertex: its colour and the slot that places it (Program::Batch). */
struct BatchVertex {
  float x, y;
  float r, g, b, a;
  float slot;
};

/* How a mesh's vertices are laid out. */
enum class Layout : uint8_t {
  None,
  /* Vertex: x, y, r, g, b, a. */
  Color,
  /* BatchVertex: x, y, r, g, b, a, slot. */
  Batch,
  /* BrushVertex: x, y, u, v, r, g, b, a. */
  Brush,
};

/* Handles: 0 is none. */
struct Mesh {
  uint32_t id = 0;
  explicit operator bool() const { return id != 0; }
};
struct Texture {
  uint32_t id = 0;
  explicit operator bool() const { return id != 0; }
};
struct Target {
  uint32_t id = 0;
  explicit operator bool() const { return id != 0; }
};
/* A depth-stencil buffer several targets can share (layer runs' images). */
struct DepthStencil {
  uint32_t id = 0;
  explicit operator bool() const { return id != 0; }
};

struct Draw {
  DrawState state;
  /* The vertices: a kept mesh, or `stream` (uploaded for this draw alone, laid out as `stream_layout`). Neither: the
   * full-screen triangle. */
  Mesh mesh;
  const void *stream = nullptr;
  size_t stream_bytes = 0;
  Layout stream_layout = Layout::None;
  int first = 0, count = 0;
  /* Sampled by Brush, Media, Composite and Run. */
  Texture texture;
  /* Layer space to clip space (Color, Brush, Media). */
  Transform2D to_clip;
  float opacity = 1.0f;
  /* Brush: a colour brush in its own colours; dabs less opaque than this left out (a mask's coverage). */
  bool own_colors = false;
  float alpha_test = 0.0f;
  /* Media. */
  float grey = 0.0f;
  float tint[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  /* Batch: two vec4s a slot (its 2x2 matrix, then its offset and opacity). */
  const float *slots = nullptr;
  int slot_count = 0;
};

struct TargetDesc {
  int width = 0, height = 0;
  /* Samples a pixel; 0 or 1: a plain image. */
  int samples = 0;
  /* Keep the samples as a texture Run can read (where Caps::sample_textures), not only a buffer. */
  bool sample_texture = true;
  /* With an image the samples resolve into (resolve, texture_of, read_pixels). */
  bool resolved = true;
  /* Drawn with this depth-stencil instead of one of its own. */
  DepthStencil depth_stencil;
};

struct Caps {
  int max_samples = 1;
  /* Multisampled colour can be a texture. */
  bool sample_textures = false;
  /* Program::Run works (per-sample shading). */
  bool sample_shading = false;
  /* Program::Batch works. */
  bool batching = false;
};

class Device {
 public:
  virtual ~Device() = default;

  /* False if the GPU can't do what the renderer needs (then nothing else may be called). */
  virtual bool init() = 0;
  virtual const Caps &caps() const = 0;

  /* Around a render: begin puts the GPU in a known state (anything else may have changed it since), end leaves it
   * as the rest of the app expects. */
  virtual void begin() = 0;
  virtual void end() = 0;

  virtual Mesh make_mesh(Layout layout, const void *data, size_t bytes) = 0;
  virtual void free_mesh(Mesh &mesh) = 0;

  /* An RGBA image (8 bits a channel, its first row at v 0) with mipmaps, filtered, clamped at its edges. Upload
   * again for a new picture, of any size. */
  virtual Texture make_texture() = 0;
  virtual void upload_texture(Texture texture, int width, int height, const uint8_t *rgba) = 0;
  virtual void free_texture(Texture &texture) = 0;

  virtual DepthStencil make_depth_stencil(int width, int height, int samples) = 0;
  virtual void free_depth_stencil(DepthStencil &depth_stencil) = 0;
  virtual Target make_target(const TargetDesc &desc) = 0;
  virtual void free_target(Target &target) = 0;
  /* Whether its samples are a texture (Run can read them). */
  virtual bool has_sample_texture(Target target) const = 0;
  /* Its resolved image, and its samples, as textures to draw from. */
  virtual Texture resolved_texture(Target target) const = 0;
  virtual Texture sample_texture(Target target) const = 0;
  /* The resolved image as the API names it (an OpenGL texture), for the UI to show. */
  virtual unsigned int native_texture(Target target) const = 0;

  /* Draws go into this target, all of it. */
  virtual void bind_target(Target target) = 0;
  /* The bound target's colour to `premultiplied`, its stencil and depth to 0. */
  virtual void clear(const float premultiplied[4]) = 0;
  virtual void draw(const Draw &draw) = 0;
  /* Averages the target's samples into its resolved image. */
  virtual void resolve(Target target) = 0;
  /* The resolved image's pixels, RGBA, bottom row first. */
  virtual void read_pixels(Target target, std::vector<uint8_t> &r_rgba) = 0;
};

std::unique_ptr<Device> make_gl_device();

}  // namespace oanim::gpu
