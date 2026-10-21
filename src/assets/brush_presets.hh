#pragma once

/* Brush presets: a brush shape (the stamp images, assets/brushes.hh) with every brush setting,
 * saved by name, as Krita's brush presets are kept apart from its brush tips.
 *
 * - Library presets live in the asset library, <library>/Brushes/<Category>/<Name>.oabrush (JSON), for every
 *   project.
 * - File presets (Document::brush_presets) live in the project and are saved with it, each with a
 *   copy of its shape's images, so a file always carries what its presets need (the copies stand in
 *   for a shape the library doesn't have).
 *
 * Loading a preset copies its settings in, as Blender's presets do: nothing stays linked, so
 * deleting a preset never changes what used it. Names never clash: a taken name gets a number
 * ("Ink.001"), and a shape copied into the library under a taken name with other images too. */

#include <functional>
#include <string>
#include <vector>

#include "data/vector.hh"

namespace oanim {

struct Document;

/* The library's presets, sorted by name (read with brushes_reload). */
const std::vector<BrushPreset> &library_brush_presets();
const BrushPreset *library_brush_preset_find(const std::string &name);
/* Rereads <library>/Brushes. */
void library_brush_presets_reload();
/* <library>/Brushes, or "" without a library. */
std::string asset_library_presets_dir();

/* A new file preset from `settings`, named `name` (made unique in the file), with a copy of its
 * shape's images. Returns it. */
BrushPreset &document_brush_preset_add(Document &doc, const std::string &name, const BrushSettings &settings);
BrushPreset *document_brush_preset_find(Document &doc, const std::string &name);
/* "Ink", or "Ink.001"... when `taken` says the name is used. */
std::string brush_preset_unique_name(const std::string &wanted, const std::function<bool(const std::string &)> &taken);

/* Where a brush saved to the library goes when no category is named. */
inline constexpr const char *BRUSH_CATEGORY_DEFAULT = "My Brushes";
/* The library's categories (its folders of brushes, Basic's built-ins included), sorted. */
std::vector<std::string> library_brush_categories();
/* Saves a preset into the library, in `category` (a folder of Brushes, made when new): its shape too
 * when the library lacks it (or has another image under its name: then the copy gets a number and the
 * saved preset points to it; `r_shape_copied` says whether it was copied). Returns the name it was
 * saved under, or "" with `r_error`. */
std::string library_brush_preset_save(const BrushPreset &preset, const std::string &category, std::string *r_error,
                                      bool *r_shape_copied = nullptr);
/* Deletes a library preset's file. */
bool library_brush_preset_remove(const std::string &name, std::string *r_error);

/* The file presets' shape copies, registered so brush_find finds them when the library doesn't have
 * a shape by that name. Call after the document's presets change (opening, adding, removing). */
void brush_presets_register_shapes(const Document &doc);

}  // namespace oanim
