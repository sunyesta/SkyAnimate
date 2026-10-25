#include "anim/onion.hh"

#include <algorithm>
#include <functional>
#include <set>

#include "anim/animation.hh"
#include "anim/deform.hh"
#include "data/document.hh"
#include "data/vector.hh"

namespace oanim {

namespace {

/* The layer and every group above it shown and rendered (a hidden group hides what is in it; guides,
 * such as bones' custom shapes and camera boxes, aren't part of the drawing). */
bool shown_through_parents(const Document &doc, const Layer &layer)
{
  if (!layer.visible || !layer.render_visible) {
    return false;
  }
  for (const Layer *p = layer_parent(const_cast<Document &>(doc), layer.id); p;
       p = layer_parent(const_cast<Document &>(doc), p->id))
  {
    if (!p->visible || !p->render_visible) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::vector<uint64_t> onion_layers(const Document &doc, const std::vector<Layer *> &editing)
{
  std::vector<uint64_t> ids;
  if (doc.onion_all_layers) {
    for (const Layer *l : document_layers_flat(doc)) {
      if (l->has_drawing() && shown_through_parents(doc, *l)) {
        ids.push_back(l->id);
      }
    }
    return ids;
  }
  const auto add = [&](const Layer &l) {
    if (l.has_drawing() && std::find(ids.begin(), ids.end(), l.id) == ids.end()) {
      ids.push_back(l.id);
    }
  };
  for (const Layer *l : editing) {
    if (l) {
      add(*l);
    }
  }
  /* A bone layer or group picked (posing with the bones, say): the drawings inside it. */
  const std::function<void(const Layer &)> inside = [&](const Layer &group) {
    for (const Layer &child : group.children) {
      if (child.visible && child.render_visible) {
        add(child);
        inside(child);
      }
    }
  };
  for (const Layer *l : document_layers_flat(doc)) {
    if ((l->selected || l->id == doc.active_layer) && !l->has_drawing() && shown_through_parents(doc, *l)) {
      inside(*l);
    }
  }
  return ids;
}

std::vector<OnionGhost> onion_ghosts(const Document &doc, const std::vector<uint64_t> &layers)
{
  std::vector<OnionGhost> ghosts;
  if (!doc.onion_skin || layers.empty()) {
    return ghosts;
  }
  const int now = doc.frame_current;
  const int first = std::max(1, doc.frame_start), last = std::max(first, doc.frame_end);
  if (doc.onion_mode == OnionMode::Frames) {
    const int step = std::max(1, doc.onion_step);
    for (int k = 1; k <= doc.onion_before && now - k * step >= first; k++) {
      ghosts.push_back({now - k * step, false, k});
    }
    for (int k = 1; k <= doc.onion_after && now + k * step <= last; k++) {
      ghosts.push_back({now + k * step, true, k});
    }
    return ghosts;
  }
  /* The keys of the layers, and of the bone layers and groups that move them. */
  std::set<int> keys;
  std::set<uint64_t> seen;
  for (const uint64_t id : layers) {
    for (const Layer *l = layer_find(doc, id); l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
      if (!seen.insert(l->id).second) {
        break;
      }
      for (const int f : layer_key_frames(*l)) {
        if (f >= first && f <= last && f != now) {
          keys.insert(f);
        }
      }
    }
  }
  int k = 0;
  for (auto it = keys.lower_bound(now); it != keys.begin() && k < doc.onion_before;) {
    --it;
    ghosts.push_back({*it, false, ++k});
  }
  k = 0;
  for (auto it = keys.upper_bound(now); it != keys.end() && k < doc.onion_after; ++it) {
    ghosts.push_back({*it, true, ++k});
  }
  return ghosts;
}

std::vector<OnionWires> onion_wires(const Document &doc, const std::vector<uint64_t> &layers,
                                    const std::vector<OnionGhost> &ghosts)
{
  std::vector<OnionWires> out;
  if (ghosts.empty() || layers.empty()) {
    return out;
  }
  /* Edits not yet keyed stay on the frame they were made on: the copy shows each ghost frame from
   * its keys alone. */
  Document copy = doc;
  for (const OnionGhost &ghost : ghosts) {
    copy.frame_current = ghost.frame;
    animation_evaluate(copy, ghost.frame);
    OnionWires wires;
    wires.ghost = ghost;
    for (const uint64_t id : layers) {
      const Layer *layer = layer_find(copy, id);
      if (!layer || !layer->has_drawing() || !shown_through_parents(copy, *layer)) {
        continue;
      }
      const VectorDrawing drawing = layer_evaluated_drawing(copy, *layer, false);
      const Transform2D to_document = layer_world_transform(copy, id);
      /* Only the segments a shown shape draws (its fill's edge or its line): not construction curves
       * or hidden shapes. Runs of them along a curve make one line each. */
      std::set<std::pair<int, int>> drawn;
      for (const Shape &shape : drawing.shapes) {
        if (!shape.hidden && (shape.has_fill || shape.has_outline)) {
          for (const ShapeEdge &e : shape.edges) {
            drawn.insert({e.curve, e.segment});
          }
        }
      }
      for (int ci = 0; ci < int(drawing.curves.size()); ci++) {
        const Curve &curve = drawing.curves[size_t(ci)];
        if (curve.points.size() < 2) {
          continue;
        }
        std::vector<float2> line;
        const auto finish = [&]() {
          if (line.size() > 1) {
            wires.lines.push_back(std::move(line));
          }
          line.clear();
        };
        for (int s = 0; s < curve.segment_count(); s++) {
          if (!drawn.contains({ci, s})) {
            finish();
            continue;
          }
          float2 ctrl[4];
          curve_segment_bezier(drawing, curve, s, ctrl);
          for (float2 &c : ctrl) {
            c = to_document.apply(c);
          }
          const int steps = bezier_steps(ctrl, 4, 0.0005f);
          for (int i = line.empty() ? 0 : 1; i <= steps; i++) {
            line.push_back(bezier_eval(ctrl, float(i) / float(steps)));
          }
        }
        finish();
      }
    }
    out.push_back(std::move(wires));
  }
  return out;
}

float onion_ghost_alpha(const Document &doc, const OnionGhost &ghost)
{
  if (!doc.onion_fade) {
    return 0.8f;
  }
  const int count = std::max(1, ghost.after ? doc.onion_after : doc.onion_before);
  /* The nearest strong, the furthest still readable. */
  return 0.85f - 0.6f * float(ghost.distance - 1) / float(std::max(1, count - 1));
}

}  // namespace oanim
