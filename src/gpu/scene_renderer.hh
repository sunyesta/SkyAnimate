#pragma once

/* Draws a document's layers into an offscreen image, with Moho's masking.
 *
 * Colours are premultiplied throughout. A masking group (or a group with less than full
 * opacity) is drawn into its own image and composited; inside a masking group, a mask image
 * collects the layers that add to or subtract from the mask, and masked layers are clipped by it. */

#include <cstdint>
#include <vector>

#include "core/math.hh"

namespace sky {

struct Document;
struct Layer;
struct Vertex;

class SceneRenderer {
 public:
  ~SceneRenderer();

  /* Renders into a texture of `width` x `height` pixels. `document_to_pixels` maps document
   * space to pixels with the origin at the bottom left. Returns the OpenGL texture, which stays
   * valid until the next call. 0 if OpenGL isn't available. `quality` = pieces drawn per curve
   * segment (the document's viewport or render quality). */
  unsigned int render(const Document &doc, int width, int height, const Transform2D &document_to_pixels,
                      int quality);

 private:
  struct Target {
    unsigned int framebuffer = 0;
    unsigned int texture = 0;
    int width = 0;
    int height = 0;
    bool in_use = false;
  };

  bool init();
  Target *acquire_target();
  void release_target(Target *target);
  void bind_target(const Target *target);
  void clear(float alpha);

  /* Draws a list of layers (top first) into the bound target, bottom first. */
  void draw_layers(const Document &doc, const std::vector<Layer> &layers, const Transform2D &parent,
                   Target *target, float opacity);
  void draw_layer(const Document &doc, const Layer &layer, const Transform2D &transform, Target *target,
                  float opacity);
  void draw_masked_group(const Document &doc, const Layer &group, const Transform2D &transform, Target *target,
                         float opacity);
  /* Draws a vector layer's shapes into the bound target. mask_only draws coverage alone. */
  void draw_drawing(const Document &doc, const Layer &layer, const Transform2D &transform, float opacity);
  void draw_vertices(const std::vector<Vertex> &vertices, const Transform2D &transform);
  /* Blends `source` onto the bound target, optionally clipped by `mask`'s alpha. */
  void composite(const Target *source, const Target *mask, float opacity);

  /* Pieces per curve segment for the render in progress. */
  int quality_ = 12;
  bool initialized_ = false;
  bool failed_ = false;
  unsigned int color_program_ = 0;
  unsigned int composite_program_ = 0;
  unsigned int vao_ = 0;
  unsigned int vbo_ = 0;
  unsigned int quad_vao_ = 0;
  int width_ = 0;
  int height_ = 0;
  Transform2D document_to_pixels_;
  std::vector<Target *> targets_;
  Target *output_ = nullptr;
};

}  // namespace sky
