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
 * half width plus `extra`. `fills_only` leaves lines out, `strokes_only` fills, `filled` fills every
 * shape (a mask's Shape and Fill Every Shape). Each connected run of a shape's edges counts, as
 * drawn. */
static bool drawing_covers(const VectorDrawing &drawing, float2 p, float extra, bool fills_only, bool filled,
                           bool strokes_only = false, bool inside_stroke = false)
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
      if (!strokes_only && ((shape.has_fill && closed) || filled) && outline.size() >= 3 && inside_polygon(outline, p)) {
        /* Inside the Stroke: not within the line's inner half. */
        bool in_line = false;
        if (inside_stroke && shape.has_outline) {
          const float r = shape.style.line_width * 0.5f;
          const size_t n = outline.size();
          for (size_t i = 0; i < n && !in_line; i++) {
            in_line = distance_to_segment(p, outline[i], outline[(i + 1) % n]) < r;
          }
        }
        if (!in_line) {
          return true;
        }
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
                         bool filled, bool shown_only, bool strokes_only = false, bool inside_stroke = false)
{
  if (shown_only && !layer.visible) {
    return false;
  }
  if (layer.is_group()) {
    for (const Layer &child : layer.children) {
      if (layer_covers(doc, child, at, extra, fills_only, filled, shown_only, strokes_only, inside_stroke)) {
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
  return drawing_covers(drawing, to_document.inverted().apply(at), extra / scale, fills_only, filled, strokes_only,
                        inside_stroke);
}

static bool rows_show_at(const Document &doc, const Layer &owner, const std::vector<LayerMask> &rows, float2 at,
                         int depth);

/* A layer's cards (and Clip to Below's) that are on in the viewport, without steps naming nothing
 * (nor cards that mask only lines: a layer's fills, which picking and clip bases go by, ignore them). */
static std::vector<LayerMask> live_rows(const Document &doc, const Layer &layer)
{
  std::vector<LayerMask> cards;
  for (LayerMask m : layer_mask_cards(doc, layer)) {
    if (!m.enabled || (m.cuts == MaskCuts::Lines && !m.clip)) {
      continue;
    }
    for (MaskStep &step : m.steps) {
      std::erase_if(step.layers, [&](uint64_t id) { return id == layer.id || !layer_find(doc, id); });
    }
    std::erase_if(m.steps, [](const MaskStep &step) { return step.layers.empty(); });
    if (!m.steps.empty()) {
      cards.push_back(std::move(m));
    }
  }
  return cards;
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

/* Cards, each its steps painted on its start, all of which must show, as the renderer evaluates them
 * (data/layer.hh). A step's mask layers lend their shapes as drawn, before their own masks, as the
 * renderer does; Clip to Below's base counts as it shows. */
static bool rows_show_at(const Document &doc, const Layer & /*owner*/, const std::vector<LayerMask> &cards, float2 at,
                         int depth)
{
  for (const LayerMask &card : cards) {
    bool shows = card.start_shown;
    for (const MaskStep &step : card.steps) {
      bool inside = false;
      for (const uint64_t id : step.layers) {
        const Layer *source = layer_find(doc, id);
        if (!source) {
          continue;
        }
        if (step.clip) {
          inside = clip_base_shows(doc, *source, at, step.fills_only, depth);
        }
        else {
          inside = layer_covers(doc, *source, at, 0.0f, step.fills_only, step.filled, false, step.strokes_only,
                                step.fills_only && step.inside_stroke);
        }
        if (inside) {
          break;
        }
      }
      if (step.invert) {
        inside = !inside;
      }
      if (inside) {
        shows = step.mode == MaskMode::Add;
      }
    }
    if (!shows) {
      return false;
    }
  }
  return true;
}

LineCutter line_cutter(const Document &doc, const Layer &layer, bool render)
{
  LineCutter cutter;
  bool any_brush = false;
  for (const Layer *l = &layer; l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    /* Each card that ends lines is a level of its own: the lines show where every one of them shows. */
    for (const LayerMask &card : l->masks) {
      bool flat = false;
      if (!(render ? card.render : card.enabled) || !mask_card_ends_lines(card, &flat)) {
        continue;
      }
      std::vector<LineCutter::Row> rows;
      for (const MaskStep &m : card.steps) {
        LineCutter::Row row;
        row.mode = int(m.mode);
        row.invert = m.invert;
        row.fills_only = m.fills_only;
        row.strokes_only = m.strokes_only;
        row.inside_stroke = m.fills_only && m.inside_stroke;
        const auto add = [&](auto &&self, const Layer &source) -> void {
          if (source.is_group()) {
            for (const Layer &child : source.children) {
              self(self, child);
            }
            return;
          }
          if (source.type != LayerType::Vector) {
            return;
          }
          const VectorDrawing drawing = layer_has_modifier_effect(doc, source, render)
                                            ? layer_evaluated_drawing(doc, source, render)
                                            : layer_drawing(doc, source);
          const Transform2D to_document = layer_world_transform(doc, source.id);
          const float scale = length(to_document.apply_vector({1.0f, 0.0f}));
          for (const Shape &whole : drawing.shapes) {
            if (whole.hidden) {
              continue;
            }
            for (const Shape &shape : shape_runs(drawing, whole)) {
              LineCutter::Piece piece;
              shape_outline(drawing, shape, 12, piece.outline, nullptr, &piece.closed);
              if (piece.outline.size() < 2) {
                continue;
              }
              for (float2 &p : piece.outline) {
                p = to_document.apply(p);
              }
              piece.area = ((shape.has_fill && piece.closed) || m.filled) && piece.outline.size() >= 3;
              piece.line = shape.has_outline && shape.style.line_width > 0.0f;
              piece.half = shape.style.line_width * 0.5f * scale;
              row.pieces.push_back(std::move(piece));
            }
          }
        };
        for (const uint64_t id : m.layers) {
          if (const Layer *source = layer_find(const_cast<Document &>(doc), id); source && id != l->id) {
            add(add, *source);
          }
        }
        rows.push_back(std::move(row));
      }
      if (rows.empty()) {
        continue;
      }
      if (!any_brush && !flat) {
        cutter.reach = card.line_reach;
      }
      any_brush = any_brush || !flat;
      rows.front().start_shown = card.start_shown;
      cutter.levels.push_back(std::move(rows));
    }
  }
  cutter.flat = !cutter.levels.empty() && !any_brush;
  return cutter;
}

static bool cutter_row_covers(const LineCutter::Row &row, float2 p)
{
  for (const LineCutter::Piece &piece : row.pieces) {
    const size_t n = piece.outline.size();
    const auto near_line = [&](float reach) {
      for (size_t i = 0; i + 1 < n + (piece.closed ? 1 : 0); i++) {
        if (distance_to_segment(p, piece.outline[i], piece.outline[(i + 1) % n]) <= reach) {
          return true;
        }
      }
      return false;
    };
    if (!row.strokes_only && piece.area && inside_polygon(piece.outline, p) &&
        !(row.inside_stroke && piece.line && near_line(piece.half)))
    {
      return true;
    }
    if (!row.fills_only && piece.line && piece.half > 0.0f && near_line(piece.half)) {
      return true;
    }
  }
  return false;
}

bool line_cutter_shows(const LineCutter &cutter, float2 at)
{
  for (const std::vector<LineCutter::Row> &rows : cutter.levels) {
    bool shows = rows.front().start_shown;
    for (const LineCutter::Row &row : rows) {
      bool inside = cutter_row_covers(row, at);
      if (row.invert) {
        inside = !inside;
      }
      if (inside) {
        shows = MaskMode(row.mode) == MaskMode::Add;
      }
    }
    if (!shows) {
      return false;
    }
  }
  return true;
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
