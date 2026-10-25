#pragma once

/* Materials: shared looks for shapes (Blender's materials, Moho's library styles, Illustrator's
 * global swatches). A shape either has its own look (Shape::style alone) or is linked to one
 * material (Shape::material), which then sets everything about it: fill colour, line colour, line
 * width and brush. Fill and Outline on/off and the points' widths stay the shape's.
 *
 * A linked shape keeps a copy of its material's look in Shape::style, so drawing, picking and the
 * reference system read shapes as before. document_sync_materials keeps the copies in step: a
 * linked shape whose look was changed (the Shape panel, the brush dialog, Python) changes its
 * material, and every shape linked to it follows. Nothing is deleted on its own: a material nothing
 * uses stays until Remove Unused; deleting one leaves its shapes their look as their own. */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "data/vector.hh"

namespace oanim {

struct Document;
struct Layer;

struct Material {
  uint64_t id = 0;
  std::string name;
  Style style;
  bool operator==(const Material &) const = default;
};

Material *material_find(Document &doc, uint64_t id);
const Material *material_find(const Document &doc, uint64_t id);
/* By name, or by id written as a number. */
Material *material_find_name(Document &doc, const std::string &name_or_id);
/* "Skin", or "Skin.001", "Skin.002"... when taken (as Blender names data-blocks); `base` may end in a
 * number suffix already. */
std::string material_unique_name(const Document &doc, const std::string &base, uint64_t except = 0);
/* How many shapes use it (in the layers' own drawings; a reference's copies follow their original). */
int material_users(const Document &doc, uint64_t id);
Material &material_add(Document &doc, const std::string &name, const Style &style);
/* Sets a material's look and every shape linked to it at once (an edit made on the material itself). */
void material_set_style(Document &doc, Material &material, const Style &style);

/* Keeps linked shapes in step (see above): a linked shape whose look differs from its material's is an
 * edit to the material, then every shape linked to it takes the material's look. Links to materials
 * that are gone are dropped (the shape keeps its look). Returns whether anything changed. */
bool document_sync_materials(Document &doc);

/* Every shape (in the layers' own drawings) with its layer, for walking them all. */
void document_for_each_shape(Document &doc, const std::function<void(Layer &, VectorDrawing &, Shape &)> &fn);

}  // namespace oanim
