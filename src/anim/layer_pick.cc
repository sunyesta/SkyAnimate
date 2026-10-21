#include "anim/layer_pick.hh"

#include <algorithm>
#include <set>

#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "data/document.hh"
#include "data/vector.hh"

namespace oanim {

/* Even-odd: whether `p` is inside the closed polygon. */
static bool inside_polygon(const std::vector<float2> &poly, float2 p)
{
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const float2 a = poly[i], b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      in = !in;
    }
  }
  return in;
}

/* Whether a drawing covers `p` (in its layer's space): a fill containing it, or a line within its
 * half width plus `extra`. `fills_only` leaves lines out, `filled` fills every shape (a mask's
 * Shape options). Each connected run of a shape's edges counts, as drawn. */
static bool drawing_covers(const VectorDrawing &drawing, float2 p, float extra, bool fills_only, bool filled)
{
  std::vector<float2> outline;
  for (const Shape &whole : drawing.shapes) {
    if (whole.hidden) {
      continue;
    }
    for (const Shape &shape : shape_runs(drawing, whole)) {
      bool closed = false;
      shape_outline(drawing, shape, 12, outline, nullptr, &closed);
      if (outline.size() < 2) {
        continue;
      }
      if (((shape.has_fill && closed) || filled) && outline.size() >= 3 && inside_polygon(outline, p)) {
        return true;
      }
      if (fills_only || !shape.has_outline) {
        continue;
      }
      const float reach = shape.style.line_width * 0.5f + extra;
      if (reach <= 0.0f) {
        continue;
      }
      const size_t n = outline.size();
      for (size_t i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
        if (distance_to_segment(p, outline[i], outline[(i + 1) % n]) <= reach) {
          return true;
        }
      }
    }
  }
  return false;
}

/* Whether a layer (a drawing, or anything in a group) covers `at`, in document space. `shown_only`:
 * hidden layers don't count (a clip base); otherwise they do (a mask layer is often hidden). */
static bool layer_covers(const Document &doc, const Layer &layer, float2 at, float extra, bool fills_only,
                         bool filled, bool shown_only)
{
  if (shown_only && !layer.visible) {
    return false;
  }
  if (layer.is_group()) {
    for (const Layer &child : layer.children) {
      if (layer_covers(doc, child, at, extra, fills_only, filled, shown_only)) {
        return true;
      }
    }
    return false;
  }
  if (layer.type != LayerType::Vector) {
    return false;
  }
  const VectorDrawing drawing = layer_has_modifier_effect(doc, layer, false) ? layer_evaluated_drawing(doc, layer, false)
                                                                             : layer_drawing(doc, layer);
  const Transform2D to_document = layer_world_transform(doc, layer.id);
  const float scale = std::max(length(to_document.apply_vector({1.0f, 0.0f})), 1e-9f);
  return drawing_covers(drawing, to_document.inverted().apply(at), extra / scale, fills_only, filled);
}

static bool rows_show_at(const Document &doc, const Layer &owner, const std::vector<LayerMask> &rows, float2 at,
                         int depth);

/* A layer's own rows (and Clip to Below) that are on in the viewport, without rows naming nothing. */
static std::vector<LayerMask> live_rows(const Document &doc, const Layer &layer)
{
  std::vector<LayerMask> rows;
  for (LayerMask m : layer_mask_rows(doc, layer)) {
    if (!m.enabled) {
      continue;
    }
    std::erase_if(m.layers, [&](uint64_t id) { return id == layer.id || !layer_find(doc, id); });
    if (!m.layers.empty()) {
      rows.push_back(std::move(m));
    }
  }
  return rows;
}

/* Clip to Below's base at `at`, as it shows: its shown layers, each where its own masks let it. */
static bool clip_base_shows(const Document &doc, const Layer &base, float2 at, bool fills_only, int depth)
{
  if (!base.visible) {
    return false;
  }
  if (base.is_group()) {
    if (depth < 2 && !rows_show_at(doc, base, live_rows(doc, base), at, depth + 1)) {
      return false;
    }
    for (const Layer &child : base.children) {
      if (clip_base_shows(doc, child, at, fills_only, depth)) {
        return true;
      }
    }
    return false;
  }
  return layer_covers(doc, base, at, 0.0f, fills_only, false, true) &&
         (depth >= 2 || rows_show_at(doc, base, live_rows(doc, base), at, depth + 1));
}

/* Rows read top to bottom, as the renderer evaluates them (data/layer.hh). A row's mask layers count
 * their own masks one level deep, as the renderer does. */
static bool rows_show_at(const Document &doc, const Layer & /*owner*/, const std::vector<LayerMask> &rows, float2 at,
                         int depth)
{
  if (rows.empty()) {
    return true;
  }
  bool shows = rows.front().mode == MaskMode::Subtract || rows.front().mode == MaskMode::Intersect;
  for (const LayerMask &row : rows) {
    bool inside = false;
    for (const uint64_t id : row.layers) {
      const Layer *source = layer_find(doc, id);
      if (!source) {
        continue;
      }
      if (row.clip) {
        inside = clip_base_shows(doc, *source, at, row.fills_only, depth);
      }
      else {
        inside = layer_covers(doc, *source, at, 0.0f, row.fills_only, row.filled, false) &&
                 (depth > 0 || rows_show_at(doc, *source, live_rows(doc, *source), at, depth + 1));
      }
      if (inside) {
        break;
      }
    }
    if (row.invert) {
      inside = !inside;
    }
    switch (row.mode) {
      case MaskMode::Add:
        shows = shows || inside;
        break;
      case MaskMode::Subtract:
        shows = shows && !inside;
        break;
      case MaskMode::Intersect:
        shows = shows && inside;
        break;
      case MaskMode::Difference:
        shows = shows != inside;
        break;
    }
  }
  return shows;
}

bool layer_masks_show_at(const Document &doc, const Layer &layer, float2 at)
{
  for (const Layer *l = &layer; l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    if (!rows_show_at(doc, *l, live_rows(doc, *l), at, 0)) {
      return false;
    }
  }
  return true;
}

std::vector<uint64_t> layers_at(const Document &doc, float2 at, const LayerPickOptions &options)
{
  /* Local View's layers with what's inside them and the groups holding them. */
  std::set<uint64_t> local;
  for (const uint64_t id : options.local_view) {
    const Layer *layer = layer_find(doc, id);
    if (!layer) {
      continue;
    }
    const auto inside = [&](auto &&self, const Layer &l) -> void {
      local.insert(l.id);
      for (const Layer &child : l.children) {
        self(self, child);
      }
    };
    inside(inside, *layer);
    for (const Layer *p = layer_parent(const_cast<Document &>(doc), id); p; p = layer_parent(const_cast<Document &>(doc), p->id)) {
      local.insert(p->id);
    }
  }
  const std::vector<const Layer *> order = draw_order_layers(doc, [&](const Layer &l) {
    return l.visible && (l.render_visible || options.show_guides) && (local.empty() || local.contains(l.id));
  });
  std::vector<uint64_t> hits;
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const Layer &layer = **it;
    if (layer.type != LayerType::Vector) {
      continue;
    }
    if (layer_covers(doc, layer, at, options.tolerance, false, false, true) && layer_masks_show_at(doc, layer, at)) {
      hits.push_back(layer.id);
    }
  }
  return hits;
}

uint64_t layer_pick_group(const Document &doc, uint64_t id)
{
  uint64_t top = id;
  for (const Layer *p = layer_parent(const_cast<Document &>(doc), id); p; p = layer_parent(const_cast<Document &>(doc), p->id)) {
    top = p->id;
  }
  return top;
}

bool layer_pick_apply(Document &doc, uint64_t id, LayerPickMode mode)
{
  Layer *layer = layer_find(doc, id);
  if (!layer || id == SIMULATION_LAYER_ID || id == CAMERA_LAYER_ID) {
    return false;
  }
  const auto picked = [&](const Layer &l) { return l.selected || l.id == doc.active_layer; };
  const uint64_t selection_before = document_selection_hash(doc);
  const bool was_picked = picked(*layer);
  if (mode == LayerPickMode::Set) {
    for (const Layer *l : document_layers_flat(doc)) {
      layer_find(doc, l->id)->selected = false;
    }
    doc.camera.selected = false;
    doc.active_layer = id;
  }
  else if (mode == LayerPickMode::Toggle && !was_picked) {
    /* Added: the layer that was active stays picked, and the new one is active. */
    if (Layer *previous = layer_find(doc, doc.active_layer); previous && previous->id != id) {
      previous->selected = true;
    }
    layer->selected = false;
    doc.active_layer = id;
  }
  else if (was_picked) {
    /* Taken out: another picked layer becomes active (the only one picked stays, as some layer is
     * always active). */
    layer->selected = false;
    if (doc.active_layer == id) {
      for (const Layer *l : document_layers_flat(doc)) {
        if (l->selected && l->id != id) {
          doc.active_layer = l->id;
          layer_find(doc, l->id)->selected = false;
          break;
        }
      }
    }
  }
  return document_selection_hash(doc) != selection_before;
}

}  // namespace oanim
