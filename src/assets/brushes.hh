#pragma once

/* Line brushes, read in Moho's format so Moho's brushes work as they are:
 *
 * - Name.mohobrush: a zip holding brush.json, the brush's default settings and its images:
 *   {"align": false, "jitter": 6.28, "spacing": 0.25, ..., "brushFiles": [{"brushFileRef":
 *   {"relativeTo": "Project", "path": "Name.png"}}]}. A path naming a folder makes a
 *   multi-brush: every PNG in it, used in turn (or at random) along the stroke.
 * - Name.png with no .mohobrush: an older brush. Settings may follow the name as
 *   Name_<align 0|1>_<jitter degrees>_<spacing percent>.png, e.g. Arrow_1_0_200.png.
 *
 * Images are dark ink on white (darkness is coverage), or coloured with alpha for colour
 * brushes. A shape's style keeps its own copy of the settings (BrushSettings), so changing them
 * on one shape leaves the brush and other shapes alone, as in Moho. */

#include <cstdint>
#include <string>
#include <vector>

#include "data/vector.hh"

namespace sky {

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
  BrushSettings defaults;
  /* Image files (UTF-8 paths), in the order a multi-brush uses them. */
  std::vector<std::string> files;
  /* Loaded on first use (brush_images). */
  mutable std::vector<BrushImage> images;
  mutable bool loaded = false;
  /* Any image has colour of its own: drawn in its colours unless the style tints it. */
  mutable bool is_color = false;
};

/* The brushes in the asset library, sorted by name. */
const std::vector<Brush> &brushes();
const Brush *brush_find(const std::string &name);
/* Rescans the library's Brushes folder. */
void brushes_reload();
/* Reads one brush folder into `r_brushes` (used by brushes_reload and the tests). */
void brushes_scan(const std::string &folder, std::vector<Brush> &r_brushes);
/* Saves `settings` as a new brush called `name` in the library (Moho's Save Brush): a .mohobrush
 * with a copy of the images of the brush the settings came from. False with a reason in
 * `r_error` if the name is taken or the files can't be written. */
bool brush_save(const std::string &name, const BrushSettings &settings, std::string *r_error);
/* The brush's images, loaded (and scaled to at most 256 pixels) on first use. Empty if none
 * could be read. */
const std::vector<BrushImage> &brush_images(const Brush &brush);

}  // namespace sky
