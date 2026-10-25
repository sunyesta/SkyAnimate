#include "text/text_layer.hh"

#include <algorithm>
#include <map>

#include "data/document.hh"
#include "data/material.hh"
#include "text/text_outline.hh"

namespace oanim {

void text_layer_rebuild(Layer &layer)
{
  if (layer.text.runs.empty()) {
    layer.text.runs.emplace_back();
  }
  layer.drawing = text_outline_drawing(layer.text);
  layer.text_built = layer.text;
}

static bool sync_in(Document &doc, std::vector<Layer> &layers)
{
  bool changed = false;
  for (Layer &layer : layers) {
    if (layer.is_text()) {
      /* A run linked to a material looks as it does (a material gone leaves the look its own). */
      for (TextRun &run : layer.text.runs) {
        if (run.material == 0) {
          continue;
        }
        if (const Material *m = material_find(doc, run.material)) {
          run.style = m->style;
        }
        else {
          run.material = 0;
        }
      }
      if (!(layer.text == layer.text_built)) {
        text_layer_rebuild(layer);
        changed = true;
      }
    }
    changed |= sync_in(doc, layer.children);
  }
  return changed;
}

bool document_sync_text(Document &doc)
{
  return sync_in(doc, doc.layers);
}

Layer &document_add_text_layer(Document &doc, const std::string &body, float2 position, const std::string &font,
                               float size)
{
  Layer &layer = document_add_layer(doc, LayerType::Text, "");
  const uint64_t id = layer.id;
  TextRun run;
  run.text = body;
  run.font = font;
  run.size = std::max(size, 1e-4f);
  layer.text.runs = {run};
  /* Named after what it says (its first line), numbered only when that name is taken. */
  std::string name = body.substr(0, std::min<size_t>(body.find('\n'), 24));
  if (name.empty()) {
    name = "Text";
  }
  const std::vector<const Layer *> flat = document_layers_flat(doc);
  const bool taken = std::any_of(flat.begin(), flat.end(),
                                 [&](const Layer *l) { return l->id != id && l->name == name; });
  layer.name = taken ? document_layer_unique_name(doc, name) : name;
  /* Where it goes in its group's space: the group's transform undone. */
  Layer &added = *layer_find(doc, id);
  added.position = layer_world_transform(doc, id).inverted().apply(position);
  text_layer_rebuild(added);
  return added;
}

/* The shapes `picked` of a drawing, with only the points and curves they use. */
static VectorDrawing drawing_subset(const VectorDrawing &drawing, const std::vector<int> &picked)
{
  VectorDrawing out;
  std::map<int, int> curves, points;
  for (const int s : picked) {
    Shape shape = drawing.shapes[size_t(s)];
    for (ShapeEdge &e : shape.edges) {
      auto [it, added] = curves.try_emplace(e.curve, int(out.curves.size()));
      if (added) {
        Curve curve = drawing.curves[size_t(e.curve)];
        for (CurvePoint &cp : curve.points) {
          auto [pt, fresh] = points.try_emplace(cp.point, int(out.points.size()));
          if (fresh) {
            out.points.push_back(drawing.points[size_t(cp.point)]);
          }
          cp.point = pt->second;
        }
        out.curves.push_back(std::move(curve));
      }
      e.curve = it->second;
    }
    out.shapes.push_back(std::move(shape));
  }
  out.next_id = drawing.next_id;
  return out;
}

bool text_layer_convert_to_vectors(Document &doc, uint64_t layer_id, const std::string &split)
{
  Layer *found = layer_find(doc, layer_id);
  if (!found || !found->is_text()) {
    return false;
  }
  document_sync_text(doc);
  Layer &layer = *layer_find(doc, layer_id);
  if (split != "LETTERS" && split != "WORDS") {
    layer.type = LayerType::Vector;
    layer.text = {};
    layer.text_built = {};
    return true;
  }
  /* A layer per letter or word, in a group in its place: the group takes its transform and settings. */
  std::vector<int> chars;
  const VectorDrawing drawing = text_outline_drawing(layer.text, &chars);
  const std::u16string text = utf8_to_utf16(text_string(layer.text));
  std::vector<std::vector<int>> pieces;
  std::vector<std::string> names;
  int word = -1, last_word = -2;
  for (int s = 0; s < int(drawing.shapes.size()); s++) {
    if (split == "WORDS") {
      /* Which word: how many runs of letters start before its letter. */
      word = 0;
      for (int k = 1; k <= std::min(chars[size_t(s)], int(text.size()) - 1); k++) {
        const bool space_before = text[size_t(k - 1)] == u' ' || text[size_t(k - 1)] == u'\n' ||
                                  text[size_t(k - 1)] == u'\t';
        const bool space_here = text[size_t(k)] == u' ' || text[size_t(k)] == u'\n' || text[size_t(k)] == u'\t';
        word += space_before && !space_here ? 1 : 0;
      }
      if (word != last_word) {
        pieces.emplace_back();
        size_t end = size_t(chars[size_t(s)]);
        while (end < text.size() && text[end] != u' ' && text[end] != u'\n' && text[end] != u'\t') {
          end++;
        }
        size_t begin = size_t(chars[size_t(s)]);
        while (begin > 0 && text[begin - 1] != u' ' && text[begin - 1] != u'\n' && text[begin - 1] != u'\t') {
          begin--;
        }
        names.push_back(utf16_to_utf8(text.substr(begin, end - begin)));
        last_word = word;
      }
      pieces.back().push_back(s);
    }
    else {
      pieces.push_back({s});
      names.push_back(drawing.shapes[size_t(s)].name);
    }
  }
  layer.type = LayerType::Group;
  layer.text = {};
  layer.text_built = {};
  layer.drawing = {};
  layer.expanded = true;
  layer.children.clear();
  /* Top of the stack first, as the Layers editor lists them: the last letter on top. */
  for (int k = int(pieces.size()) - 1; k >= 0; k--) {
    Layer child;
    child.id = doc.next_layer_id++;
    child.type = LayerType::Vector;
    child.name = names[size_t(k)].empty() ? "Letter" : names[size_t(k)];
    child.drawing = drawing_subset(drawing, pieces[size_t(k)]);
    layer.children.push_back(std::move(child));
  }
  return true;
}

}  // namespace oanim
