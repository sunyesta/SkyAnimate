#pragma once

/* Draw tool presets: the kinds of pen the shelf along the bottom of the viewport offers while Draw is the
 * tool (Blender's asset shelf), each with how it makes a line.
 *
 * - Built-in presets come with the app: Ink, Pencil, Marker, Tapered, Mouse, the Fill Pen and Fill Marker, the
 *   Bezier Pen; under Erasers the Fill Eraser, Pressure Fill Eraser, Stroke Eraser and Whole Line Eraser; and
 *   under Trim the Vector Trimmer.
 * - Saved presets live in the asset library, <library>/Draw Presets/<Name>.oadraw (JSON), for every
 *   project, and show under Mine.
 *
 * Picking one copies its settings into the tool, as Blender's presets do: nothing stays linked. Colours are
 * no part of a preset: changing pen doesn't change what colour is being drawn with. */

#include <string>
#include <vector>

#include "data/ink_stroke.hh"

namespace oanim {

struct DrawPreset {
  std::string name;
  DrawMode mode = DrawMode::Freehand;
  InkSettings ink;
  /* Document units, as Style::line_width. */
  float line_width = 0.008f;
  /* "Pens", "Erasers", "Trim" (built-in) or "Mine" (saved). */
  std::string category;
  bool builtin = false;
};

inline constexpr const char *DRAW_CATEGORY_PENS = "Pens";
inline constexpr const char *DRAW_CATEGORY_ERASERS = "Erasers";
inline constexpr const char *DRAW_CATEGORY_TRIM = "Trim";
inline constexpr const char *DRAW_CATEGORY_MINE = "Mine";

/* Every preset: the built-in ones, then the library's sorted by name. */
const std::vector<DrawPreset> &draw_presets();
const DrawPreset *draw_preset_find(const std::string &name);
/* Rereads <library>/Draw Presets. */
void draw_presets_reload();
/* Saves `preset` to the library under its name (made unique: "Ink.001"). Returns the name it was saved
 * under, or "" with `r_error` (no library, or it can't be written). */
std::string draw_preset_save(const DrawPreset &preset, std::string *r_error);
/* Deletes a saved preset (built-in ones can't be). */
bool draw_preset_remove(const std::string &name, std::string *r_error);

}  // namespace oanim
