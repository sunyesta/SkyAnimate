#pragma once

/* Brush shapes: the stamp images a brushed line is painted with (dabs placed along it by the
 * brush's settings, which live in the shape's style and in presets, never in the shape).
 *
 * - <library>/Brush Shapes/Name.png: a shape of one image.
 * - <library>/Brush Shapes/Name/: a folder of PNGs, a multi-image shape, used in turn (or at
 *   random) along the line.
 * - The built-in Round tip, drawn by the app (BRUSH_ROUND_TIP).
 *
 * Images are dark ink on white (darkness is coverage), or coloured with alpha for colour brushes.
 * A library from before (version 1, Moho's brushes in Brushes/) is upgraded when it's read. */

#include <cstdint>
#include <string>
#include <vector>

#include "data/vector.hh"

namespace oanim {

struct BrushImage {
  int width = 0, height = 0;
  /* Premultiplied RGBA. Ink brushes are white with coverage as alpha (the line colour tints
   * them); colour brushes keep their colours. */
  std::vector<uint8_t> rgba;
  /* Unique for the life of the app: lets the GPU side cache a texture per image. */
  uint64_t id = 0;
};

struct Brush {
  std::string name;
  /* The settings a shape starts a line with when none are given (the defaults, named for it). */
  BrushSettings defaults;
  /* Image files (UTF-8 paths), in the order a multi-brush uses them. */
  std::vector<std::string> files;
  /* Or the images themselves (PNG bytes): a shape a file preset carries (assets/brush_presets.hh). */
  std::vector<std::string> data;
  /* Loaded on first use (brush_images). */
  mutable std::vector<BrushImage> images;
  mutable bool loaded = false;
  /* Any image has colour of its own: drawn in its colours unless the style tints it. */
  mutable bool is_color = false;
  /* The built-in Round tip: its image is drawn by the app, hard or soft by the settings' hardness. */
  bool round_tip = false;
};

/* The built-in Round tip's name (brush_find finds it; it's in no library, so it's never missing). */
inline constexpr const char *BRUSH_ROUND_TIP = "Round (built-in)";
/* A shape's name as the UI shows it ("Round" for the built-in tip). */
std::string brush_display_name(const std::string &name);
/* The images a line with `settings` is drawn with: the brush's, or the Round tip at its hardness. */
const std::vector<BrushImage> &brush_tip_images(const Brush &brush, const BrushSettings &settings);

/* The shapes in the asset library, sorted by name. */
const std::vector<Brush> &brushes();
const Brush *brush_find(const std::string &name);
/* Rescans the library's Brush Shapes (upgrading a version 1 library first) and its presets. */
void brushes_reload();
/* Reads a Brush Shapes folder into `r_brushes`. */
void brushes_scan(const std::string &folder, std::vector<Brush> &r_brushes);
/* The brush's images, loaded (and scaled to at most 256 pixels) on first use. Empty if none
 * could be read. */
const std::vector<BrushImage> &brush_images(const Brush &brush);
/* The shapes file presets carry, found by brush_find when the library has none by that name. */
void brushes_set_packed(std::vector<Brush> packed);
/* A shape's images as file names and PNG bytes (read from its files, or its own copies). False when
 * there's no such shape or a file can't be read. */
bool brush_shape_bytes(const std::string &name, std::vector<std::string> &r_files, std::vector<std::string> &r_data);
/* Whether the library (not a file preset) has a shape by this name. */
const Brush *brush_find_in_library(const std::string &name);

}  // namespace oanim
