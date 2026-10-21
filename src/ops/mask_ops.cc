/* Mask operators: a layer's masks name other layers (data/layer.hh LayerMask). Add, remove, turn
 * on and off, Inside/Outside, Mask With Layer Below (Ctrl Alt G), and picking the mask layer by
 * clicking it (mask.pick, with the layer eyedropper). */

#include <algorithm>
#include <cstdlib>
#include <string>

#include "core/log.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

Layer *layer_by_name_or_id(Document &doc, const std::string &text)
{
  if (text.empty()) {
    return nullptr;
  }
  char *end = nullptr;
  const unsigned long long id = std::strtoull(text.c_str(), &end, 10);
  if (end && *end == '\0' && id != 0) {
    return layer_find(doc, uint64_t(id));
  }
  for (const Layer *l : document_layers_flat(doc)) {
    if (l->name == text) {
      return layer_find(doc, l->id);
    }
  }
  return nullptr;
}

/* The layers an operator acts on: its `layer` argument, else the active and picked layers. */
static std::vector<Layer *> mask_targets(Context &C)
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

/* `sources` ("|"-separated ids or names) as layer ids; false (and a report) for one that isn't there. */
static bool mask_sources(Context &C, std::vector<uint64_t> *r_ids)
{
  Document &doc = *C.document;
  const std::string list = op_arg(C, "sources");
  size_t at = 0;
  while (at <= list.size()) {
    const size_t bar = std::min(list.find('|', at), list.size());
    const std::string part = list.substr(at, bar - at);
    at = bar + 1;
    if (part.empty()) {
      continue;
    }
    const Layer *l = layer_by_name_or_id(doc, part);
    if (!l || l->id == SIMULATION_LAYER_ID) {
      report(ReportType::Warning, "There's no layer \"" + part + "\" to mask with");
      return false;
    }
    if (std::find(r_ids->begin(), r_ids->end(), l->id) == r_ids->end()) {
      r_ids->push_back(l->id);
    }
  }
  return true;
}

/* Mask With, by name: AS_DRAWN, FILLS_ONLY, FILLED (filled, with its stroke) or FILLED_NO_STROKE. */
static bool mask_use_set(const std::string &use, LayerMask *m)
{
  if (use == "AS_DRAWN" || use == "FILLS_ONLY" || use == "FILLED" || use == "FILLED_NO_STROKE") {
    m->filled = use == "FILLED" || use == "FILLED_NO_STROKE";
    m->fills_only = use == "FILLS_ONLY" || use == "FILLED_NO_STROKE";
    return true;
  }
  report(ReportType::Error, "use is AS_DRAWN, FILLS_ONLY, FILLED or FILLED_NO_STROKE");
  return false;
}

static bool mask_poll(Context &C)
{
  return C.document && document_active_layer(*C.document) != nullptr;
}

std::string mask_label(const Document &doc, const LayerMask &m)
{
  std::string names;
  for (const uint64_t id : m.layers) {
    const Layer *l = layer_find(doc, id);
    names += (names.empty() ? "" : ", ") + (l ? l->name : std::string("(missing)"));
  }
  static const char *const BEYOND[] = {"", " and above", " and below", " and left of it", " and right of it"};
  return std::string(m.outside ? "Outside " : "Inside ") + (names.empty() ? std::string("nothing") : names) +
         BEYOND[int(m.extend)];
}

void mask_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "mask.add";
    ot.name = "Add Mask";
    ot.description = "Mask a layer (the active and picked ones without layer=) with other layers: it shows only "
                     "inside them, or with outside=1 only outside them. sources: layers' ids or names, | between "
                     "them (inside or outside any of them); expand=1 grows their shapes a pixel, fills_only=1 leaves "
                     "their strokes out, filled=1 fills every shape (an outline too); or use=AS_DRAWN, FILLS_ONLY, FILLED "
                     "or FILLED_NO_STROKE for both; extend=UP, DOWN, LEFT or RIGHT stretches their shapes to the edge that way. "
                     "Hidden layers still mask; the order of layers doesn't matter";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      LayerMask m;
      m.outside = op_arg_bool(C, "outside");
      m.expand = op_arg_bool(C, "expand");
      m.fills_only = op_arg_bool(C, "fills_only");
      m.filled = op_arg_bool(C, "filled");
      if (op_has_arg(C, "use") && !mask_use_set(op_arg(C, "use"), &m)) {
        return OpStatus::Cancelled;
      }
      if (op_has_arg(C, "extend") && !mask_extend_from_identifier(op_arg(C, "extend"), &m.extend)) {
        report(ReportType::Error, "extend is NONE, UP, DOWN, LEFT or RIGHT");
        return OpStatus::Cancelled;
      }
      if (!mask_sources(C, &m.layers)) {
        return OpStatus::Cancelled;
      }
      if (m.layers.empty()) {
        report(ReportType::Warning, "Name the layers to mask with (sources=)");
        return OpStatus::Cancelled;
      }
      int count = 0;
      std::string undo;
      for (Layer *layer : mask_targets(C)) {
        LayerMask own = m;
        std::erase(own.layers, layer->id);
        if (own.layers.empty()) {
          report(ReportType::Warning, "A layer can't be its own mask");
          continue;
        }
        if (std::find(layer->masks.begin(), layer->masks.end(), own) != layer->masks.end()) {
          continue;
        }
        layer->masks.push_back(own);
        undo = "Mask " + layer->name + ": " + mask_label(doc, own);
        count++;
      }
      if (count == 0) {
        return OpStatus::Cancelled;
      }
      C.undo_name = count == 1 ? undo : "Add Mask";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.remove";
    ot.name = "Remove Mask";
    ot.description = "Remove one of a layer's masks (index=, from 0), or all of them without index";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int index = op_arg_int(C, "index", -1);
      int count = 0;
      std::string undo;
      for (Layer *layer : mask_targets(C)) {
        if (index < 0) {
          count += int(layer->masks.size());
          layer->masks.clear();
          undo = "Remove " + layer->name + "'s Masks";
        }
        else if (index < int(layer->masks.size())) {
          undo = "Remove Mask " + layer->name + ": " + mask_label(doc, layer->masks[size_t(index)]);
          layer->masks.erase(layer->masks.begin() + index);
          count++;
        }
      }
      if (count == 0) {
        return OpStatus::Cancelled;
      }
      C.undo_name = undo;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.set";
    ot.name = "Change Mask";
    ot.description = "Change one of a layer's masks (index=): outside=0/1, enabled=0/1 (in the viewport), "
                     "render=0/1 (in renders), expand=0/1, fills_only=0/1, filled=0/1, use= (AS_DRAWN, "
                     "FILLS_ONLY, FILLED, FILLED_NO_STROKE), extend= (NONE, UP, DOWN, LEFT, RIGHT), sources= (| between "
                     "layers' ids or names), toggle=1 "
                     "turns it off or on in the viewport";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int index = op_arg_int(C, "index", 0);
      std::vector<uint64_t> sources;
      if (op_has_arg(C, "sources") && !mask_sources(C, &sources)) {
        return OpStatus::Cancelled;
      }
      int count = 0;
      std::string undo;
      for (Layer *layer : mask_targets(C)) {
        if (index < 0 || index >= int(layer->masks.size())) {
          continue;
        }
        LayerMask &m = layer->masks[size_t(index)];
        if (op_has_arg(C, "outside")) {
          m.outside = op_arg_bool(C, "outside");
        }
        if (op_has_arg(C, "enabled")) {
          m.enabled = op_arg_bool(C, "enabled");
        }
        if (op_has_arg(C, "render")) {
          m.render = op_arg_bool(C, "render");
        }
        if (op_has_arg(C, "expand")) {
          m.expand = op_arg_bool(C, "expand");
        }
        if (op_has_arg(C, "fills_only")) {
          m.fills_only = op_arg_bool(C, "fills_only");
        }
        if (op_has_arg(C, "filled")) {
          m.filled = op_arg_bool(C, "filled");
        }
        if (op_has_arg(C, "use") && !mask_use_set(op_arg(C, "use"), &m)) {
          return OpStatus::Cancelled;
        }
        if (op_has_arg(C, "extend") && !mask_extend_from_identifier(op_arg(C, "extend"), &m.extend)) {
          report(ReportType::Error, "extend is NONE, UP, DOWN, LEFT or RIGHT");
          return OpStatus::Cancelled;
        }
        if (op_arg_bool(C, "toggle")) {
          m.enabled = !m.enabled;
        }
        if (op_has_arg(C, "sources")) {
          m.layers = sources;
          std::erase(m.layers, layer->id);
        }
        undo = std::string(m.enabled ? "Mask " : "Mask Off ") + layer->name + ": " + mask_label(doc, m);
        count++;
      }
      if (count == 0) {
        return OpStatus::Cancelled;
      }
      C.undo_name = undo;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.move";
    ot.name = "Move Mask";
    ot.description = "Move one of a layer's masks (index=) to another place in its list (to=), as dragging its grip "
                     "does (the order is only for tidiness: every mask applies)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      const int from = op_arg_int(C, "index", -1), to = op_arg_int(C, "to", -1);
      std::vector<Layer *> targets = mask_targets(C);
      if (targets.empty()) {
        return OpStatus::Cancelled;
      }
      Layer &layer = *targets.front();
      const int n = int(layer.masks.size());
      if (from < 0 || from >= n || to < 0 || to >= n || from == to) {
        return OpStatus::Cancelled;
      }
      LayerMask m = layer.masks[size_t(from)];
      layer.masks.erase(layer.masks.begin() + from);
      layer.masks.insert(layer.masks.begin() + to, m);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.with_layer_below";
    ot.name = "Mask With Layer Below";
    ot.description = "Show the layer only inside the layer just below it (Photoshop's clipping mask; Ctrl Alt G)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      int count = 0;
      std::string undo;
      for (Layer *layer : mask_targets(C)) {
        int index = 0;
        const std::vector<Layer> *container = layer_container(doc, layer->id, &index);
        if (!container || index + 1 >= int(container->size())) {
          continue;
        }
        const Layer &below = (*container)[size_t(index + 1)];
        const LayerMask m{false, {below.id}, true};
        if (std::find(layer->masks.begin(), layer->masks.end(), m) != layer->masks.end()) {
          continue;
        }
        layer->masks.push_back(m);
        undo = "Mask " + layer->name + " With " + below.name;
        count++;
      }
      if (count == 0) {
        report(ReportType::Info, "No layer below to mask with (or it already does)");
        return OpStatus::Cancelled;
      }
      C.undo_name = count == 1 ? undo : "Mask With Layer Below";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* Inside... / Outside...: the viewport or the Layers editor gives the layer clicked. */
    OperatorType ot;
    ot.idname = "mask.pick";
    ot.name = "Pick Mask Layer";
    ot.description = "Click the layer to mask with (mode=INSIDE or OUTSIDE), in the viewport or the Layers editor; "
                     "with index=, it joins that mask instead. Esc cancels";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      const std::string mode = op_arg(C, "mode", "INSIDE");
      if (mode != "INSIDE" && mode != "OUTSIDE") {
        report(ReportType::Error, "mode is INSIDE or OUTSIDE");
        return OpStatus::Cancelled;
      }
      WindowManager::MaskPick &pick = C.wm->mask_pick;
      pick = {};
      pick.outside = mode == "OUTSIDE";
      pick.index = op_arg_int(C, "index", -1);
      for (const Layer *l : mask_targets(C)) {
        pick.layers.push_back(l->id);
      }
      pick.active = !pick.layers.empty();
      return pick.active ? OpStatus::Finished : OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace oanim
