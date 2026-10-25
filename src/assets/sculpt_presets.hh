#pragma once

/* Sculpt brush presets: a sculpt brush (anim/sculpt.hh) with its radius, strength and falloff, picked
 * from the shelf along the bottom of the viewport while the Sculpt tool is on (Blender's asset shelf).
 *
 * - Built-in presets come with the app, in two groups: Move (Grab, Push, Pinch, Twist) and Shape
 *   (Smooth, Thickness).
 * - Saved presets live in the asset library, <library>/Sculpt Brushes/<Name>.oasculpt (JSON), for
 *   every project, and show under Mine.
 *
 * Picking one copies its settings into the tool, as Blender's presets do: nothing stays linked. */

#include <string>
#include <vector>

#include "anim/sculpt.hh"

namespace oanim {

struct SculptPreset {
  std::string name;
  SculptBrush brush = SculptBrush::Grab;
  /* Screen pixels, as the tool's radius. */
  float radius = 50.0f;
  float strength = 0.5f;
  SculptFalloff falloff = SculptFalloff::Smooth;
  /* "Move", "Shape" (built-in) or "Mine" (saved). */
  std::string category;
  bool builtin = false;
};

inline constexpr const char *SCULPT_CATEGORY_MOVE = "Move";
inline constexpr const char *SCULPT_CATEGORY_SHAPE = "Shape";
inline constexpr const char *SCULPT_CATEGORY_MINE = "Mine";

/* Every preset: the built-in ones, then the library's sorted by name. */
const std::vector<SculptPreset> &sculpt_presets();
const SculptPreset *sculpt_preset_find(const std::string &name);
/* The built-in preset named after `brush` ("Grab"). */
const SculptPreset &sculpt_preset_default(SculptBrush brush);
/* Rereads <library>/Sculpt Brushes. */
void sculpt_presets_reload();
/* Saves `preset` to the library under its name (made unique: "Grab.001"). Returns the name it was
 * saved under, or "" with `r_error` (no library, or it can't be written). */
std::string sculpt_preset_save(const SculptPreset &preset, std::string *r_error);
/* Deletes a saved preset (built-in ones can't be). */
bool sculpt_preset_remove(const std::string &name, std::string *r_error);

}  // namespace oanim
