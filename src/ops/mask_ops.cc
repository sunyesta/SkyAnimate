/* Mask operators: a layer's mask rows name other layers (data/layer.hh LayerMask) and read top to
 * bottom (Add, Subtract, Intersect, Difference). Add, remove, reorder, turn on and off, Clip to
 * Below (Ctrl Alt G), and picking the mask layer by clicking it (mask.pick, with the layer
 * eyedropper). */

#include <algorithm>
#include <cstdlib>
#include <string>

#include "anim/draw_order.hh"
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

/* The Shape a row masks with, by name: AS_DRAWN (fills and strokes), FILLS_ONLY or STROKES_ONLY;
 * Fill Every Shape (`filled`) is set apart. */
static bool mask_shape_set(const std::string &shape, LayerMask *m)
{
  if (shape == "AS_DRAWN" || shape == "FILLS_ONLY" || shape == "STROKES_ONLY") {
    m->fills_only = shape == "FILLS_ONLY";
    m->strokes_only = shape == "STROKES_ONLY";
    return true;
  }
  report(ReportType::Error, "shape is AS_DRAWN, FILLS_ONLY or STROKES_ONLY");
  return false;
}

/* End Lines at Edge's Ends, by name: BRUSH (the brush's taper and fade) or FLAT. */
static bool mask_ends_set(const std::string &ends, LayerMask *m)
{
  if (ends == "BRUSH" || ends == "FLAT") {
    m->end_lines_flat = ends == "FLAT";
    return true;
  }
  report(ReportType::Error, "ends is BRUSH or FLAT");
  return false;
}

/* The Shape and Fill Every Shape together, as the menu had them before (scripts still use it):
 * AS_DRAWN, FILLS_ONLY, STROKES_ONLY (not filled), FILLED (As Drawn, filled) or FILLED_NO_STROKE
 * (Fills Only, filled). */
static bool mask_use_set(const std::string &use, LayerMask *m)
{
  if (use == "FILLED" || use == "FILLED_NO_STROKE") {
    m->filled = true;
    return mask_shape_set(use == "FILLED" ? "AS_DRAWN" : "FILLS_ONLY", m);
  }
  if (use == "AS_DRAWN" || use == "FILLS_ONLY" || use == "STROKES_ONLY") {
    m->filled = false;
    return mask_shape_set(use, m);
  }
  report(ReportType::Error, "use is AS_DRAWN, FILLS_ONLY, STROKES_ONLY, FILLED or FILLED_NO_STROKE");
  return false;
}

/* The row's settings every operator that makes or changes rows takes: mode=, invert=, grow=, and the
 * older outside= (Subtract; inside is Add for the first row, else Intersect, as before rows) and
 * expand= (grow a pixel). `index` is the row's place (for the outside= rule). */
static bool mask_row_args(Context &C, LayerMask *m, int index)
{
  if (op_has_arg(C, "outside")) {
    m->mode = mask_mode_from_inside_outside(op_arg_bool(C, "outside"), index == 0);
  }
  if (op_has_arg(C, "mode") && !mask_mode_from_identifier(op_arg(C, "mode"), &m->mode)) {
    report(ReportType::Error, "mode is ADD, SUBTRACT, INTERSECT or DIFFERENCE");
    return false;
  }
  if (op_has_arg(C, "invert")) {
    m->invert = op_arg_bool(C, "invert");
  }
  if (op_has_arg(C, "expand")) {
    m->grow = op_arg_bool(C, "expand") ? 1.0f : 0.0f;
  }
  if (op_has_arg(C, "reach")) {
    m->line_reach = std::clamp(op_arg_float(C, "reach", 0.0f), -2.0f, 2.0f);
  }
  if (op_has_arg(C, "grow")) {
    m->grow = std::clamp(op_arg_float(C, "grow", 0.0f), -64.0f, 64.0f);
  }
  return true;
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
  static const char *const MODES[] = {"Add ", "Subtract ", "Intersect ", "Difference "};
  return std::string(MODES[int(m.mode)]) + (m.invert ? "outside " : "") +
         (names.empty() ? std::string("nothing") : names) + BEYOND[int(m.extend)];
}

/* The rows in words, one phrase each as they apply ("everything but head", "minus mouth, lines
 * ending at mouth's edge"), empty for a row that's off or names nothing. One per layer_mask_rows. */
std::vector<std::string> mask_row_phrases(const Document &doc, const Layer &layer)
{
  std::vector<std::string> phrases;
  bool any = false;
  MaskLineEnding above;
  for (const LayerMask &m : layer_mask_rows(doc, layer)) {
    std::string text;
    if (!m.enabled) {
      phrases.push_back(text);
      continue;
    }
    bool flat = false;
    const bool ends_lines = mask_row_ends_lines(m, &above, &flat);
    std::string names;
    for (const uint64_t id : m.layers) {
      if (const Layer *l = layer_find(doc, id)) {
        names += (names.empty() ? "" : " + ") + l->name;
      }
    }
    if (names.empty()) {
      phrases.push_back(text);
      continue;
    }
    /* Strokes Only masks with lines, not insides: "on ring's lines". */
    const char *in = "inside ";
    const std::string shape_names = names;
    if (m.strokes_only) {
      names += "'s lines";
      in = "on ";
    }
    /* Extend: "minus mouth and above". */
    static const char *const BEYOND[] = {"", " and above", " and below", " and left of it", " and right of it"};
    names += BEYOND[int(m.extend)];
    if (m.cuts != MaskCuts::All) {
      /* Cuts: "everything but ear (its lines)". */
      names += m.cuts == MaskCuts::Lines ? " (its lines)" : " (its fills)";
    }
    /* After the first row the shape's name says enough: "minus mouth". */
    const std::string where = (m.invert ? "outside " : "") + names;
    if (!any) {
      /* A first Subtract cuts from everything; cutting what's outside leaves what's inside. */
      text = m.mode != MaskMode::Subtract ? (m.invert ? "outside " : in) + names
             : m.invert                  ? in + names
                                         : "everything but " + names;
    }
    else {
      switch (m.mode) {
        case MaskMode::Add:
          text += "plus " + where;
          break;
        case MaskMode::Subtract:
          text += "minus " + where;
          break;
        case MaskMode::Intersect:
          text += "within " + where;
          break;
        case MaskMode::Difference:
          text += "flipped by " + where;
          break;
      }
    }
    if (ends_lines) {
      /* End Lines at Edge, or an Add row bringing back lines a row above ended: "..., lines ending
       * at head's edge". */
      text += ", lines ending at " + shape_names + "'s edge";
    }
    any = true;
    phrases.push_back(text);
  }
  return phrases;
}

std::string mask_summary(const Document &doc, const Layer &layer)
{
  /* "Shows: inside head, minus mouth, plus tongue": the rows in words, as they apply. */
  std::string text;
  for (const std::string &phrase : mask_row_phrases(doc, layer)) {
    if (!phrase.empty()) {
      text += (text.empty() ? "" : ", ") + phrase;
    }
  }
  return text.empty() ? "Shows everything" : "Shows: " + text;
}

void mask_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "mask.add";
    ot.name = "Add Mask";
    ot.description = "Add a mask row to a layer (the active and picked ones without layer=): the layers in "
                     "sources (ids or names, | between them; together they make one shape) combined with the rows "
                     "above by mode=ADD, SUBTRACT, INTERSECT or DIFFERENCE (without it: Add for the first row, "
                     "Intersect after; outside=1 is Subtract). A first row of Subtract or Intersect starts from "
                     "everything. invert=1 uses what's outside the shape; grow= grows it by pixels (negative "
                     "shrinks; expand=1 is a pixel); shape=AS_DRAWN, FILLS_ONLY or STROKES_ONLY (or fills_only=1, "
                     "strokes_only=1), filled=1 fills every shape (an outline too; use=FILLED or FILLED_NO_STROKE sets "
                     "both, as the menu once did); extend=UP, DOWN, LEFT or RIGHT stretches their shapes to the edge that way. "
                     "Hidden layers still mask; the order of layers doesn't matter";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      LayerMask m;
      m.fills_only = op_arg_bool(C, "fills_only");
      m.strokes_only = !m.fills_only && op_arg_bool(C, "strokes_only");
      m.filled = op_arg_bool(C, "filled");
      m.inside_stroke = op_arg_bool(C, "inside_stroke");
      m.end_lines = op_arg_bool(C, "end_lines");
      m.cuts = op_arg_bool(C, "lines_only") ? MaskCuts::Lines : MaskCuts::All;
      if (op_has_arg(C, "cuts") && !mask_cuts_from_identifier(op_arg(C, "cuts"), &m.cuts)) {
        report(ReportType::Error, "cuts is ALL, FILLS or LINES");
        return OpStatus::Cancelled;
      }
      if (op_has_arg(C, "ends") && !mask_ends_set(op_arg(C, "ends"), &m)) {
        return OpStatus::Cancelled;
      }
      if (op_has_arg(C, "use") && !mask_use_set(op_arg(C, "use"), &m)) {
        return OpStatus::Cancelled;
      }
      if (op_has_arg(C, "shape") && !mask_shape_set(op_arg(C, "shape"), &m)) {
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
        if (!op_has_arg(C, "mode") && !op_has_arg(C, "outside")) {
          own.mode = layer->masks.empty() ? MaskMode::Add : MaskMode::Intersect;
        }
        if (!mask_row_args(C, &own, int(layer->masks.size()))) {
          return OpStatus::Cancelled;
        }
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
    ot.description = "Change one of a layer's mask rows (index=): mode= (ADD, SUBTRACT, INTERSECT, DIFFERENCE), "
                     "invert=0/1, grow= (pixels, negative shrinks), enabled=0/1 (in the viewport), render=0/1 (in "
                     "renders), outside=0/1 and expand=0/1 (as before rows), shape= (AS_DRAWN, FILLS_ONLY, "
                     "STROKES_ONLY), filled=0/1 (Fill Every Shape), inside_stroke=0/1 (with Fills Only: cut at "
                     "the lines' inner edge), end_lines=0/1 (the layer's lines end at the edge), ends=BRUSH or FLAT, "
                     "reach= (line widths past the edge, -2 to 2), cuts= (ALL, FILLS or LINES: what of the "
                     "layer the row masks; lines_only=0/1 as before), "
                     "fills_only=0/1, strokes_only=0/1, use= (AS_DRAWN, "
                     "FILLS_ONLY, STROKES_ONLY, FILLED, FILLED_NO_STROKE), extend= (NONE, UP, DOWN, LEFT, RIGHT), sources= (| between "
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
        if (!mask_row_args(C, &m, index)) {
          return OpStatus::Cancelled;
        }
        if (op_has_arg(C, "enabled")) {
          m.enabled = op_arg_bool(C, "enabled");
        }
        if (op_has_arg(C, "render")) {
          m.render = op_arg_bool(C, "render");
        }
        if (op_has_arg(C, "fills_only")) {
          m.fills_only = op_arg_bool(C, "fills_only");
          m.strokes_only = m.strokes_only && !m.fills_only;
        }
        if (op_has_arg(C, "strokes_only")) {
          m.strokes_only = op_arg_bool(C, "strokes_only");
          m.fills_only = m.fills_only && !m.strokes_only;
        }
        if (op_has_arg(C, "filled")) {
          m.filled = op_arg_bool(C, "filled");
        }
        if (op_has_arg(C, "inside_stroke")) {
          m.inside_stroke = op_arg_bool(C, "inside_stroke");
        }
        if (op_has_arg(C, "end_lines")) {
          m.end_lines = op_arg_bool(C, "end_lines");
        }
        if (op_has_arg(C, "lines_only")) {
          m.cuts = op_arg_bool(C, "lines_only") ? MaskCuts::Lines : MaskCuts::All;
        }
        if (op_has_arg(C, "cuts") && !mask_cuts_from_identifier(op_arg(C, "cuts"), &m.cuts)) {
          report(ReportType::Error, "cuts is ALL, FILLS or LINES");
          return OpStatus::Cancelled;
        }
        if (op_has_arg(C, "ends") && !mask_ends_set(op_arg(C, "ends"), &m)) {
          return OpStatus::Cancelled;
        }
        if (op_has_arg(C, "use") && !mask_use_set(op_arg(C, "use"), &m)) {
          return OpStatus::Cancelled;
        }
        if (op_has_arg(C, "shape") && !mask_shape_set(op_arg(C, "shape"), &m)) {
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
    ot.description = "Move one of a layer's mask rows (index=) to another place in its list (to=), as dragging its "
                     "grip does (rows apply top to bottom, so this can change what shows)";
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
      C.undo_name = "Move Mask Row";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.duplicate";
    ot.name = "Duplicate Mask";
    ot.description = "Copy one of a layer's mask rows (index=) to just below it";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      const int index = op_arg_int(C, "index", -1);
      std::vector<Layer *> targets = mask_targets(C);
      if (targets.empty() || index < 0 || index >= int(targets.front()->masks.size())) {
        return OpStatus::Cancelled;
      }
      Layer &layer = *targets.front();
      LayerMask copy = layer.masks[size_t(index)];
      /* Drivers follow the original. */
      copy.uid = 0;
      layer.masks.insert(layer.masks.begin() + index + 1, copy);
      C.undo_name = "Duplicate Mask " + layer.name + ": " + mask_label(*C.document, copy);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* Two layers that meet (a head and an ear) each cut the other's lines where they join, so they
     * read as one shape: what took two rows and five settings each. */
    OperatorType ot;
    ot.idname = "mask.join_lines";
    ot.name = "Join Lines";
    ot.description = "Join two layers' lines (layers= their ids or names, | between them; else the active and "
                     "picked layers, two of them): each gets a row subtracting the other's fills, inside their "
                     "lines, with End Lines at Edge, so their lines end where they meet. The lower one's row cuts "
                     "only lines, so its fill covers where they overlap. A row already naming the other is reused";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<Layer *> pair;
      if (op_has_arg(C, "layers")) {
        const std::string list = op_arg(C, "layers");
        size_t at = 0;
        while (at <= list.size()) {
          const size_t bar = std::min(list.find('|', at), list.size());
          if (Layer *l = layer_by_name_or_id(doc, list.substr(at, bar - at))) {
            if (std::find(pair.begin(), pair.end(), l) == pair.end()) {
              pair.push_back(l);
            }
          }
          at = bar + 1;
        }
      }
      else {
        pair = mask_targets(C);
      }
      if (pair.size() != 2) {
        report(ReportType::Warning, "Join Lines takes two layers (pick both)");
        return OpStatus::Cancelled;
      }
      /* Which is drawn lower: it keeps its whole fill. */
      const std::vector<const Layer *> order = draw_order_layers(doc, [](const Layer &) { return true; });
      const auto place = [&](const Layer *l) {
        return int(std::find(order.begin(), order.end(), l) - order.begin());
      };
      if (place(pair[0]) > place(pair[1])) {
        std::swap(pair[0], pair[1]);
      }
      for (int k = 0; k < 2; k++) {
        Layer &target = *pair[size_t(k)];
        const uint64_t source = pair[size_t(1 - k)]->id;
        LayerMask *row = nullptr;
        for (LayerMask &m : target.masks) {
          if (m.layers == std::vector<uint64_t>{source}) {
            row = &m;
            break;
          }
        }
        if (!row) {
          target.masks.emplace_back();
          row = &target.masks.back();
          row->layers = {source};
        }
        row->mode = MaskMode::Subtract;
        row->invert = false;
        row->enabled = true;
        row->render = true;
        row->fills_only = true;
        row->strokes_only = false;
        row->filled = false;
        row->inside_stroke = true;
        row->end_lines = true;
        row->end_lines_flat = false;
        row->extend = MaskExtend::None;
        row->cuts = k == 0 ? MaskCuts::Lines : MaskCuts::All;
      }
      C.undo_name = "Join Lines: " + pair[0]->name + " and " + pair[1]->name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  /* Clip to Below on the targets: set 1 or 0, or -1 to turn it over (on unless all of them are). */
  static const auto clip_to_below = [](Context &C, int set) {
    Document &doc = *C.document;
    std::vector<Layer *> targets = mask_targets(C);
    if (targets.empty()) {
      return OpStatus::Cancelled;
    }
    bool all_on = true;
    for (const Layer *l : targets) {
      all_on &= l->clip_to_below;
    }
    const bool on = set >= 0 ? set == 1 : !all_on;
    int count = 0;
    std::string undo;
    for (Layer *layer : targets) {
      if (layer->clip_to_below == on) {
        continue;
      }
      if (on && !layer_clip_base(doc, *layer)) {
        report(ReportType::Info, layer->name + " has no layer below to clip to");
        continue;
      }
      layer->clip_to_below = on;
      const Layer *base = layer_clip_base(doc, *layer);
      undo = on ? "Clip " + layer->name + " to " + (base ? base->name : std::string("Below"))
                : "Release " + layer->name + "'s Clip";
      count++;
    }
    if (count == 0) {
      return OpStatus::Cancelled;
    }
    C.undo_name = count == 1 ? undo : (on ? "Clip to Below" : "Release Clips");
    return OpStatus::Finished;
  };
  {
    OperatorType ot;
    ot.idname = "mask.clip_to_below";
    ot.name = "Clip to Below";
    ot.description = "Show the layer only where the layer below it shows (Photoshop's clipping mask; Ctrl Alt G "
                     "turns it on and off). Several layers in a row can clip to one: they clip to the first layer "
                     "below them that doesn't. set=0/1 instead of turning it over; exclude_strokes=0/1 alone sets "
                     "Exclude Strokes (clip to the base's fills only, not its lines)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      if (op_has_arg(C, "exclude_strokes") && !op_has_arg(C, "set")) {
        const bool exclude = op_arg_bool(C, "exclude_strokes");
        int count = 0;
        for (Layer *layer : mask_targets(C)) {
          count += layer->clip_exclude_strokes != exclude ? 1 : 0;
          layer->clip_exclude_strokes = exclude;
        }
        if (count == 0) {
          return OpStatus::Cancelled;
        }
        C.undo_name = exclude ? "Clip Excluding Strokes" : "Clip Including Strokes";
        return OpStatus::Finished;
      }
      if (op_has_arg(C, "exclude_strokes")) {
        for (Layer *layer : mask_targets(C)) {
          layer->clip_exclude_strokes = op_arg_bool(C, "exclude_strokes");
        }
      }
      return clip_to_below(C, op_has_arg(C, "set") ? int(op_arg_bool(C, "set")) : -1);
    };
    operators().add(std::move(ot));
  }
  {
    /* The older name: turns Clip to Below on. */
    OperatorType ot;
    ot.idname = "mask.with_layer_below";
    ot.name = "Mask With Layer Below";
    ot.description = "Clip to Below, on (see mask.clip_to_below)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) { return clip_to_below(C, 1); };
    operators().add(std::move(ot));
  }
  {
    /* Inside... / Outside...: the viewport or the Layers editor gives the layer clicked. */
    OperatorType ot;
    ot.idname = "mask.pick";
    ot.name = "Pick Mask Layer";
    ot.description = "Click the layer to mask with (a new row, mode=ADD, SUBTRACT, INTERSECT or DIFFERENCE; INSIDE "
                     "is Add for a first row and Intersect after, OUTSIDE is Subtract), in the viewport or the "
                     "Layers editor; with index=, it joins that row instead. Esc cancels";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      std::string mode = op_arg(C, "mode", "INSIDE");
      MaskMode parsed;
      if (mode == "OUTSIDE") {
        mode = "SUBTRACT";
      }
      if (mode != "INSIDE" && !mask_mode_from_identifier(mode, &parsed)) {
        report(ReportType::Error, "mode is ADD, SUBTRACT, INTERSECT, DIFFERENCE, INSIDE or OUTSIDE");
        return OpStatus::Cancelled;
      }
      WindowManager::MaskPick &pick = C.wm->mask_pick;
      pick = {};
      pick.mode = mode == "INSIDE" ? std::string() : mode;
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
