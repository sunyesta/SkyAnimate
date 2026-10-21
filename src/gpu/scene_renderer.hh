#pragma once

/* Draws a document's layers into an offscreen image, with Moho's masking.
 *
 * Edges are anti-aliased by multisampling: each pixel holds several samples, each either inside
 * a shape or not, averaged only at the very end. Masks live in the stencil buffer, per sample,
 * so a masked layer ends exactly where its mask's edge falls and two edges that meet leave no
 * seam (no "matte lines"). Each level of nested masking group uses one stencil bit.
 *
 * Colours are premultiplied throughout. A group with less than full opacity is drawn into its
 * own image and faded as a whole. */

#include <cstdint>
#include <map>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct Document;
struct Layer;
struct Vertex;
struct BrushDabs;
struct BrushImage;

class SceneRenderer {
 public:
  ~SceneRenderer();

  /* Renders into a texture of `width` x `height` pixels. `document_to_pixels` maps document
   * space to pixels with the origin at the bottom left. `quality` = pieces drawn per curve
   * segment; `samples` = anti-aliasing samples per pixel (rounded down to 1, 2, 4, 8 or 16
   * and to what the GPU supports). `background`, if given, makes this a final render: it fills
   * the image first and layers hidden from renders are left out. Otherwise the image starts
   * transparent and every visible layer is drawn (the viewport). Returns the OpenGL texture, which stays valid
   * until the next call, or 0 if OpenGL isn't available. */
  unsigned int render(const Document &doc, int width, int height, const Transform2D &document_to_pixels,
                      int quality, int samples, const Color *background = nullptr);

  /* The last render's pixels, top row first, RGBA, 8 bits each (premultiplied). */
  bool read_pixels(std::vector<uint8_t> &r_rgba, int *r_width, int *r_height);

  /* The anti-aliasing samples the last render used. */
  int samples() const { return samples_; }

 private:
  struct Target {
    /* Drawn into: multisampled colour and depth-stencil. */
    unsigned int draw_framebuffer = 0;
    unsigned int color = 0;
    unsigned int depth_stencil = 0;
    /* The resolved image, averaged from the samples. */
    unsigned int resolve_framebuffer = 0;
    unsigned int texture = 0;
    bool in_use = false;
  };

  bool init();
  Target *acquire_target();
  void release_target(Target *target);
  void free_targets();
  void bind_target(const Target *target);
  void resolve(const Target *target);
  void clear(const Color &color);

  /* Draws a list of layers (top first) into the bound target, bottom first. */
  void draw_layers(const Document &doc, const std::vector<Layer> &layers, const Transform2D &parent,
                   Target *target, float opacity);
  void draw_layer(const Document &doc, const Layer &layer, const Transform2D &transform, Target *target,
                  float opacity);
  /* Sets the stencil to the masks of `masked` (the layers whose masks apply to what is drawn next)
   * and returns the stencil bits a sample needs. */
  unsigned apply_masks(const Document &doc, const std::vector<const Layer *> &masked);
  /* A mask layer's shape into one stencil bit (set or cleared), where its own masks hold. */
  void draw_mask_source(const Document &doc, const Layer &source, unsigned bit, bool set);
  /* Writes a layer's coverage (and its children's) into one stencil bit, set or cleared. */
  void draw_coverage(const Document &doc, const Layer &layer, const Transform2D &parent, unsigned bit, bool set);
  /* Sets or clears one stencil bit everywhere. */
  void stencil_fill(unsigned bit, bool set);
  /* Stencil test for colour drawing: every bit in required_stencil_ must be set. */
  void use_color_stencil();
  /* Draws a vector layer's shapes into the bound target. */
  void draw_drawing(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity);
  void draw_vertices(const std::vector<Vertex> &vertices, const Transform2D &transform);
  /* Draws brush dabs. `own_colors`: a colour brush shown in its own colours. Merged alpha
   * draws them into an image of their own first, keeping the strongest dab where they overlap. */
  void draw_dabs(const BrushDabs &dabs, const std::vector<BrushImage> &images, bool own_colors, bool merged,
                 const Transform2D &transform, float opacity);
  unsigned int brush_texture(const BrushImage &image);
  void set_matrix(unsigned int program, const Transform2D &transform);
  /* Blends `source`'s resolved image onto the bound target. */
  void composite(const Target *source, float opacity);

  /* A layer this render draws: visible, and shown in renders when this is a final render (a Media
   * layer's Guide never is). */
  bool shown(const Layer &layer) const;
  bool layer_shown_through_parents(const Document &doc, const Layer &layer) const;
  float layer_opacity_through_parents(const Document &doc, const Layer &layer) const;
  /* A Media layer's picture on this frame (the nearest decoded one in the viewport until the exact
   * one comes; a final render waits for it). */
  void draw_media(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity);
  struct MediaTexture {
    unsigned int texture = 0;
    uint64_t serial = 0;
    int width = 0, height = 0;
  };
  /* One texture per Media layer, refilled when its frame changes. */
  std::map<uint64_t, MediaTexture> media_textures_;
  unsigned int media_program_ = 0;

  bool final_render_ = false;
  /* Drawing a mask's coverage into the stencil (no colour): brush dabs count where they're
   * at least half covered. */
  bool coverage_pass_ = false;
  const Target *bound_ = nullptr;
  /* One texture per brush image, by BrushImage::id. */
  std::map<uint64_t, unsigned int> brush_textures_;
  unsigned int brush_program_ = 0;
  unsigned int brush_vao_ = 0;
  unsigned int brush_vbo_ = 0;
  /* Pieces per curve segment for the render in progress. */
  int quality_ = 12;
  int samples_ = 1;
  /* Set while a layer draws its part of a group's mask: its masking options apply then. */
  struct MaskPass {
    bool expand = false;
    bool exclude_strokes = false;
    /* Every shape fills, filled or not (a mask's Filled Shape). */
    bool filled = false;
    /* A mask's Extend (MaskExtend): the shapes swept to far off that way. */
    int extend = 0;
  } mask_pass_;
  /* Stencil bits a colour sample must have set (the masks it's inside). */
  unsigned required_stencil_ = 0;
  /* Stencil bits a mask shape is only written where they are set (its own masks), or 0. */
  unsigned coverage_test_ = 0;

 public:
  /* One mask as the stencil holds it: inside (or outside) any of these layers. */
  struct MaskClause {
    bool outside = false;
    std::vector<uint64_t> layers;
    bool expand = false;
    bool fills_only = false;
    bool filled = false;
    int extend = 0;
    bool operator==(const MaskClause &) const = default;
  };

 private:
  /* The masks the stencil holds now (bit k: clause k), so neighbours with the same masks reuse it. */
  std::vector<MaskClause> mask_clauses_;
  bool initialized_ = false;
  bool failed_ = false;
  unsigned int color_program_ = 0;
  unsigned int composite_program_ = 0;
  unsigned int stencil_program_ = 0;
  unsigned int vao_ = 0;
  unsigned int vbo_ = 0;
  unsigned int quad_vao_ = 0;
  int width_ = 0;
  int height_ = 0;
  int target_samples_ = 0;
  Transform2D document_to_pixels_;
  std::vector<Target *> targets_;
  Target *output_ = nullptr;
};

}  // namespace oanim
