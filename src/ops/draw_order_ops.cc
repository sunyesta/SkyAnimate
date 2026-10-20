/* Draw order operators: Bring Forward / Send Backward (Ctrl ] and Ctrl [), Bring to Front / Send to
 * Back (with Shift), In Front Of... and Behind... (pick the other layer), Normal, and setting a
 * layer's rule directly. On frame 0 they set the setup order; on any other frame they key it. */

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <string>

#include "anim/animation.hh"
#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace sky {

/* What an operator acts on: its `layer` argument, else the active layer and the picked ones. */
static std::vector<Layer *> draw_order_layers_of(Context &C)
{
  Document &doc = *C.document;
  std::vector<Layer *> layers;
  if (op_has_arg(C, "layer")) {
    if (Layer *l = layer_by_name_or_id(doc, op_arg(C, "layer"))) {
      layers.push_back(l);
    }
    return layers;
  }
  for (const Layer *l : document_layers_flat(doc)) {
    if (l->id == doc.active_layer || l->selected) {
      layers.push_back(layer_find(doc, l->id));
    }
  }
  return layers;
}

static bool draw_order_poll(Context &C)
{
  return C.document && document_active_layer(*C.document) != nullptr;
}

static bool drawn(const Layer &l)
{
  return l.visible;
}

/* A drawing layer's box in document space, as drawn now. */
static bool leaf_bounds(const Document &doc, const Layer &layer, float2 *r_min, float2 *r_max)
{
  if (layer.type != LayerType::Vector) {
    return false;
  }
  const VectorDrawing drawing = layer_has_modifier_effect(doc, layer, false) ? layer_evaluated_drawing(doc, layer, false)
                                                                              : layer_drawing(doc, layer);
  const Transform2D to_document = layer_world_transform(doc, layer.id);
  bool any = false;
  for (const MeshPoint &p : drawing.points) {
    const float2 q = to_document.apply(p.pos);
    *r_min = any ? float2(std::min(r_min->x, q.x), std::min(r_min->y, q.y)) : q;
    *r_max = any ? float2(std::max(r_max->x, q.x), std::max(r_max->y, q.y)) : q;
    any = true;
  }
  return any;
}

std::string draw_order_undo_name(const Document &doc, const Layer &layer, const DrawOrderRule &rule)
{
  if (rule.mode == DrawOrderMode::Normal) {
    return layer.name + ": Normal Draw Order";
  }
  return layer.name + " " + draw_order_label(doc, draw_order_value(rule));
}

/* Gives `layer` the rule; false (and a report) when it can't have it. */
static bool set_rule(Context &C, Layer &layer, const DrawOrderRule &rule)
{
  Document &doc = *C.document;
  if (rule.mode != DrawOrderMode::Normal) {
    const Layer *target = layer_find(doc, rule.target);
    if (!target || rule.target == SIMULATION_LAYER_ID) {
      report(ReportType::Warning, "There's no such layer to go in front of or behind");
      return false;
    }
    if (layer_inside(doc, rule.target, layer.id) || layer_inside(doc, layer.id, rule.target)) {
      report_at_mouse(ReportType::Warning, "\"" + layer.name + "\" and \"" + target->name + "\": one holds the other",
                      "A layer goes in front of or behind a layer outside it", "Can't order a layer with its group");
      return false;
    }
  }
  layer_draw_order_set(doc, layer, rule);
  return true;
}

/* The layer `layer` goes in front of (dir 1) or behind (dir -1) to move one step: the next drawn
 * layer that overlaps it (so every step shows), else just the next one. `extreme`: past them all.
 * 0 when it's already as far as it goes. */
static uint64_t step_target(const Document &doc, const Layer &layer, int dir, bool extreme)
{
  const std::vector<const Layer *> order = draw_order_layers(doc, drawn);
  int lo = INT32_MAX, hi = -1;
  float2 bmin, bmax;
  bool has_bounds = false;
  for (int i = 0; i < int(order.size()); i++) {
    if (!layer_inside(doc, order[size_t(i)]->id, layer.id)) {
      continue;
    }
    lo = std::min(lo, i);
    hi = std::max(hi, i);
    float2 a, b;
    if (leaf_bounds(doc, *order[size_t(i)], &a, &b)) {
      bmin = has_bounds ? float2(std::min(bmin.x, a.x), std::min(bmin.y, a.y)) : a;
      bmax = has_bounds ? float2(std::max(bmax.x, b.x), std::max(bmax.y, b.y)) : b;
      has_bounds = true;
    }
  }
  if (hi < 0) {
    return 0;
  }
  const auto outside = [&](int i) { return !layer_inside(doc, order[size_t(i)]->id, layer.id); };
  const auto overlaps = [&](int i) {
    float2 a, b;
    if (!has_bounds || !leaf_bounds(doc, *order[size_t(i)], &a, &b)) {
      return false;
    }
    return a.x <= bmax.x && b.x >= bmin.x && a.y <= bmax.y && b.y >= bmin.y;
  };
  uint64_t first = 0;
  if (dir > 0) {
    for (int i = int(order.size()) - 1; extreme && i > hi; i--) {
      if (outside(i)) {
        return order[size_t(i)]->id;
      }
    }
    for (int i = hi + 1; i < int(order.size()); i++) {
      if (!outside(i)) {
        continue;
      }
      first = first ? first : order[size_t(i)]->id;
      if (overlaps(i)) {
        return order[size_t(i)]->id;
      }
    }
    return first;
  }
  for (int i = 0; extreme && i < lo; i++) {
    if (outside(i)) {
      return order[size_t(i)]->id;
    }
  }
  for (int i = lo - 1; i >= 0; i--) {
    if (!outside(i)) {
      continue;
    }
    first = first ? first : order[size_t(i)]->id;
    if (overlaps(i)) {
      return order[size_t(i)]->id;
    }
  }
  return first;
}

void draw_order_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "layer.draw_order_set";
    ot.name = "Set Draw Order";
    ot.description = "Draw a layer (the active and picked ones without layer=) in front of or behind another, "
                     "anywhere in the document: mode NORMAL, FRONT or BEHIND, target a layer's id or name. On "
                     "frame 0 it's the setup order; on other frames it's keyed. Masks keep the Layers editor's order";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = draw_order_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      DrawOrderRule rule;
      if (!draw_order_mode_from_identifier(op_arg(C, "mode", "NORMAL"), &rule.mode)) {
        report(ReportType::Error, "mode is NORMAL, FRONT or BEHIND");
        return OpStatus::Cancelled;
      }
      if (rule.mode != DrawOrderMode::Normal) {
        const Layer *target = layer_by_name_or_id(doc, op_arg(C, "target"));
        if (!target) {
          report(ReportType::Warning, "Name the layer to go in front of or behind (target=)");
          return OpStatus::Cancelled;
        }
        rule.target = target->id;
      }
      animation_record(doc);
      int count = 0;
      std::string undo;
      for (Layer *layer : draw_order_layers_of(C)) {
        if (layer->id != rule.target && set_rule(C, *layer, rule)) {
          undo = draw_order_undo_name(doc, *layer, rule);
          count++;
        }
      }
      if (count == 0) {
        return OpStatus::Cancelled;
      }
      C.undo_name = count == 1 ? undo : "Draw Order";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  for (const auto &[idname, name, dir, extreme, tip] :
       {std::tuple{"layer.draw_order_forward", "Bring Forward", 1, false,
                   "Draw the layer in front of the next layer above it that it overlaps (Ctrl ])"},
        std::tuple{"layer.draw_order_backward", "Send Backward", -1, false,
                   "Draw the layer behind the next layer below it that it overlaps (Ctrl [)"},
        std::tuple{"layer.draw_order_front", "Bring to Front", 1, true, "Draw the layer in front of everything (Ctrl Shift ])"},
        std::tuple{"layer.draw_order_back", "Send to Back", -1, true, "Draw the layer behind everything (Ctrl Shift [)"}})
  {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = std::string(tip) + ". On frame 0 it's the setup order; on other frames it's keyed. Masks keep "
                                        "the Layers editor's order";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = draw_order_poll;
    const int d = dir;
    const bool ext = extreme;
    ot.exec = [d, ext](Context &C) {
      Document &doc = *C.document;
      animation_record(doc);
      int count = 0;
      std::string undo;
      for (Layer *layer : draw_order_layers_of(C)) {
        const uint64_t target = step_target(doc, *layer, d, ext);
        if (target == 0) {
          continue;
        }
        DrawOrderRule rule{d > 0 ? DrawOrderMode::Front : DrawOrderMode::Behind, target};
        /* Where it would be anyway (back where the Layers editor has it): Normal, no rule needed. */
        const float was = layer->draw_order;
        layer->draw_order = 0.0f;
        const std::vector<const Layer *> normal = draw_order_layers(doc, drawn);
        layer->draw_order = draw_order_value(rule);
        if (draw_order_layers(doc, drawn) == normal) {
          rule = DrawOrderRule();
        }
        layer->draw_order = was;
        if (set_rule(C, *layer, rule)) {
          undo = draw_order_undo_name(doc, *layer, rule);
          count++;
        }
      }
      if (count == 0) {
        report(ReportType::Info, d > 0 ? "Already in front of everything it can go in front of"
                                       : "Already behind everything it can go behind");
        return OpStatus::Cancelled;
      }
      C.undo_name = count == 1 ? undo : "Draw Order";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* In Front Of... / Behind...: the viewport (or Layers editor) gives the layer clicked. */
    OperatorType ot;
    ot.idname = "layer.draw_order_pick";
    ot.name = "In Front Of / Behind Layer";
    ot.description = "Click the layer to draw the active layer in front of (mode=FRONT) or behind (mode=BEHIND), "
                     "in the viewport or the Layers editor; the result shows while you point. Esc cancels";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = draw_order_poll;
    ot.exec = [](Context &C) {
      DrawOrderMode mode;
      if (!draw_order_mode_from_identifier(op_arg(C, "mode", "FRONT"), &mode) || mode == DrawOrderMode::Normal) {
        report(ReportType::Error, "mode is FRONT or BEHIND");
        return OpStatus::Cancelled;
      }
      WindowManager::DrawOrderPick &pick = C.wm->draw_order_pick;
      pick = {};
      pick.mode = mode == DrawOrderMode::Front ? 1 : -1;
      for (const Layer *l : draw_order_layers_of(C)) {
        pick.layers.push_back(l->id);
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "layer.draw_order_menu";
    ot.name = "Draw Order";
    ot.description = "The Draw Order menu at the mouse: Bring to Front, Bring Forward, Send Backward, Send to Back, "
                     "In Front Of..., Behind..., Normal";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = draw_order_poll;
    ot.exec = [](Context &C) {
      C.wm->menu_request = "draw_order";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace sky
