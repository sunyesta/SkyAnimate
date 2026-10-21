#include "data/material.hh"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>

#include "data/document.hh"

namespace oanim {

Material *material_find(Document &doc, uint64_t id)
{
  for (Material &m : doc.materials) {
    if (m.id == id && id != 0) {
      return &m;
    }
  }
  return nullptr;
}

const Material *material_find(const Document &doc, uint64_t id)
{
  return material_find(const_cast<Document &>(doc), id);
}

Material *material_find_name(Document &doc, const std::string &name_or_id)
{
  for (Material &m : doc.materials) {
    if (m.name == name_or_id) {
      return &m;
    }
  }
  char *end = nullptr;
  const unsigned long long id = std::strtoull(name_or_id.c_str(), &end, 10);
  if (end && *end == '\0' && !name_or_id.empty()) {
    return material_find(doc, uint64_t(id));
  }
  return nullptr;
}

std::string material_unique_name(const Document &doc, const std::string &wanted, uint64_t except)
{
  const auto taken = [&](const std::string &name) {
    for (const Material &m : doc.materials) {
      if (m.name == name && m.id != except) {
        return true;
      }
    }
    return false;
  };
  const std::string base_wanted = wanted.empty() ? std::string("Material") : wanted;
  if (!taken(base_wanted)) {
    return base_wanted;
  }
  /* "Skin.003" counts on from its own number. */
  std::string base = base_wanted;
  const size_t dot = base.rfind('.');
  if (dot != std::string::npos && dot + 4 == base.size() &&
      base.find_first_not_of("0123456789", dot + 1) == std::string::npos)
  {
    base = base.substr(0, dot);
  }
  for (int n = 1; n < 100000; n++) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%03d", n);
    if (!taken(base + suffix)) {
      return base + suffix;
    }
  }
  return base;
}

static void for_each_shape_in(std::vector<Layer> &layers, const std::function<void(Layer &, VectorDrawing &, Shape &)> &fn)
{
  for (Layer &layer : layers) {
    /* A reference shows its original's drawing (its own looks on top): only originals count. */
    if (!layer.is_reference()) {
      for (Shape &shape : layer.drawing.shapes) {
        fn(layer, layer.drawing, shape);
      }
    }
    for_each_shape_in(layer.children, fn);
  }
}

void document_for_each_shape(Document &doc, const std::function<void(Layer &, VectorDrawing &, Shape &)> &fn)
{
  for_each_shape_in(doc.layers, fn);
}

int material_users(const Document &doc, uint64_t id)
{
  int users = 0;
  document_for_each_shape(const_cast<Document &>(doc), [&](Layer &, VectorDrawing &, Shape &shape) {
    users += shape.material == id ? 1 : 0;
  });
  return users;
}

Material &material_add(Document &doc, const std::string &name, const Style &style)
{
  Material m;
  m.id = doc.next_material_id++;
  m.name = material_unique_name(doc, name);
  m.style = style;
  doc.materials.push_back(std::move(m));
  return doc.materials.back();
}

void material_set_style(Document &doc, Material &material, const Style &style)
{
  material.style = style;
  document_for_each_shape(doc, [&](Layer &, VectorDrawing &, Shape &shape) {
    if (shape.material == material.id) {
      shape.style = style;
    }
  });
}

bool document_sync_materials(Document &doc)
{
  bool changed = false;
  /* First the edits: a linked shape no longer matching its material changed it. */
  std::map<uint64_t, Style> edits;
  document_for_each_shape(doc, [&](Layer &, VectorDrawing &, Shape &shape) {
    if (shape.material == 0) {
      return;
    }
    const Material *m = material_find(doc, shape.material);
    if (!m) {
      /* Its material is gone: the look stays the shape's own. */
      shape.material = 0;
      changed = true;
      return;
    }
    if (!(shape.style == m->style) && !edits.contains(m->id)) {
      edits[m->id] = shape.style;
    }
  });
  for (const auto &[id, style] : edits) {
    material_find(doc, id)->style = style;
    changed = true;
  }
  if (edits.empty() && !changed) {
    return false;
  }
  /* Then every linked shape follows. */
  document_for_each_shape(doc, [&](Layer &, VectorDrawing &, Shape &shape) {
    if (const Material *m = material_find(doc, shape.material); m && !(shape.style == m->style)) {
      shape.style = m->style;
    }
  });
  return true;
}

}  // namespace oanim
