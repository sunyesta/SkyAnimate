/* Mask operators: a layer's masks are cards (data/layer.hh LayerMask), each starting with everything
 * or nothing shown and its steps, naming other layers, painting on it top to bottom (Add white,
 * Subtract black). Add, remove, reorder and change cards and their steps, Join Lines, Clip to Below
 * (Ctrl Alt G), and picking the mask layer by clicking it (mask.pick, with the layer eyedropper). */

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

/* The Shape a step masks with, by name: AS_DRAWN (fills and strokes), FILLS_ONLY or STROKES_ONLY;
 * Fill Every Shape (`filled`) is set apart. */
static bool mask_shape_set(const std::string &shape, MaskStep *m)
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
static bool mask_use_set(const std::string &use, MaskStep *m)
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

/* A step's settings every operator that makes or changes steps takes: mode=, invert=, combine=
 * (UNION, DIFFERENCE, INTERSECTION: how its layers make the shape), grow=, shape=
 * (and use=, fills_only=, strokes_only=), filled=, inside_stroke=, extend=, and the older outside=
 * (Subtract; inside is Add for a first step, else Subtract of the outside, as before rows),
 * mode=INTERSECT (Subtract of the outside) and expand= (grow a pixel). `first`: the card's first step
 * (for the outside= rule). */
static bool mask_step_args(Context &C, MaskStep *m, bool first)
{
  if (op_has_arg(C, "outside")) {
    m->invert = false;
    mask_step_from_old_mode("", op_arg_bool(C, "outside"), first, m);
  }
  if (op_has_arg(C, "invert")) {
    m->invert = op_arg_bool(C, "invert");
  }
  if (op_has_arg(C, "mode")) {
    const std::string mode = op_arg(C, "mode");
    if (mode == "INTERSECT") {
      /* Keep only inside (outside with invert=1): hide the other side. */
      m->mode = MaskMode::Subtract;
      m->invert = !op_arg_bool(C, "invert");
    }
    else if (!mask_mode_from_identifier(mode, &m->mode)) {
      report(ReportType::Error, "mode is ADD or SUBTRACT (invert=1 for the outside)");
      return false;
    }
  }
  if (op_has_arg(C, "combine") && !mask_combine_from_identifier(op_arg(C, "combine"), &m->combine)) {
    report(ReportType::Error, "combine is UNION, DIFFERENCE or INTERSECTION");
    return false;
  }
  if (op_has_arg(C, "expand")) {
    m->grow = op_arg_bool(C, "expand") ? 1.0f : 0.0f;
  }
  if (op_has_arg(C, "grow")) {
    m->grow = std::clamp(op_arg_float(C, "grow", 0.0f), -64.0f, 64.0f);
  }
  if (op_has_arg(C, "fills_only")) {
    m->fills_only = op_arg_bool(C, "fills_only");
    m->strokes_only = m->strokes_only && !m->fills_only;
  }
  if (op_has_arg(C, "strokes_only")) {
    m->strokes_only = op_arg_bool(C, "strokes_only");
    m->fills_only = m->fills_only && !m->strokes_only;
  }
  if (op_has_arg(C, "filled")) {
    m->filled = op_arg_bool(C, "filled");
  }
  if (op_has_arg(C, "inside_stroke")) {
    m->inside_stroke = op_arg_bool(C, "inside_stroke");
  }
  if (op_has_arg(C, "use") && !mask_use_set(op_arg(C, "use"), m)) {
    return false;
  }
  if (op_has_arg(C, "shape") && !mask_shape_set(op_arg(C, "shape"), m)) {
    return false;
  }
  if (op_has_arg(C, "extend") && !mask_extend_from_identifier(op_arg(C, "extend"), &m->extend)) {
    report(ReportType::Error, "extend is NONE, UP, DOWN, LEFT or RIGHT");
    return false;
  }
  return true;
}

/* A card's settings: name=, start= (EVERYTHING or NOTHING), cuts= (ALL, FILLS, LINES; lines_only=1 as
 * before), end_lines=, ends= (BRUSH or FLAT), reach=, enabled=, render=, toggle=1. */
static bool mask_card_args(Context &C, LayerMask *m)
{
  if (op_has_arg(C, "name")) {
    m->name = op_arg(C, "name");
  }
  if (op_has_arg(C, "start")) {
    const std::string start = op_arg(C, "start");
    if (start != "EVERYTHING" && start != "NOTHING") {
      report(ReportType::Error, "start is EVERYTHING or NOTHING");
      return false;
    }
    m->start_shown = start == "EVERYTHING";
  }
  if (op_has_arg(C, "lines_only")) {
    m->cuts = op_arg_bool(C, "lines_only") ? MaskCuts::Lines : MaskCuts::All;
  }
  if (op_has_arg(C, "cuts") && !mask_cuts_from_identifier(op_arg(C, "cuts"), &m->cuts)) {
    report(ReportType::Error, "cuts is ALL, FILLS or LINES");
    return false;
  }
  if (op_has_arg(C, "end_lines")) {
    m->end_lines = op_arg_bool(C, "end_lines");
  }
  if (op_has_arg(C, "ends") && !mask_ends_set(op_arg(C, "ends"), m)) {
    return false;
  }
  if (op_has_arg(C, "reach")) {
    m->line_reach = std::clamp(op_arg_float(C, "reach", 0.0f), -2.0f, 2.0f);
  }
  if (op_has_arg(C, "enabled")) {
    m->enabled = op_arg_bool(C, "enabled");
  }
  if (op_has_arg(C, "render")) {
    m->render = op_arg_bool(C, "render");
  }
  if (op_arg_bool(C, "toggle")) {
    m->enabled = !m->enabled;
  }
  return true;
}

static bool mask_poll(Context &C)
{
  return C.document && document_active_layer(*C.document) != nullptr;
}

static std::string layer_names(const Document &doc, const std::vector<uint64_t> &ids, const char *between)
{
  std::string names;
  for (const uint64_t id : ids) {
    const Layer *l = layer_find(doc, id);
    names += (names.empty() ? "" : between) + (l ? l->name : std::string("(missing)"));
  }
  return names;
}

/* Between a step's layers' names, by how they make its shape: "head + hat", "head - mouth" (the first
 * minus the rest), "head & hat" (where both are). */
static const char *mask_combine_joiner(MaskCombine combine)
{
  switch (combine) {
    case MaskCombine::Difference:
      return " - ";
    case MaskCombine::Intersection:
      return " & ";
    case MaskCombine::Union:
      break;
  }
  return " + ";
}

std::string mask_label(const Document &doc, const LayerMask &m)
{
  static const char *const BEYOND[] = {"", " and above", " and below", " and left of it", " and right of it"};
  std::string text;
  for (const MaskStep &s : m.steps) {
    const std::string names = layer_names(doc, s.layers, s.combine == MaskCombine::Union ? ", " : mask_combine_joiner(s.combine));
    const MaskVerb verb = mask_step_verb(s);
    if (s.whole) {
      text += std::string(text.empty() ? (m.start_shown ? "Show All, " : "Hide All, ") : ", ") +
              (s.mode == MaskMode::Add ? "Show All" : "Hide All");
      continue;
    }
    text += std::string(text.empty() ? (m.start_shown ? "Show All, " : "Hide All, ") : ", ") +
            (verb == MaskVerb::KeepOnly ? "Keep Only " : verb == MaskVerb::Remove ? "- " : s.invert ? "+ outside " : "+ ") +
            (s.group ? (s.name.empty() ? std::string("Group") : s.name) : names.empty() ? std::string("nothing") : names) +
            BEYOND[int(s.extend)];
  }
  return text.empty() ? std::string("Empty mask") : text;
}

std::vector<std::string> mask_row_phrases(const Document &doc, const Layer &layer)
{
  std::vector<std::string> phrases;
  for (const LayerMask &m : layer_mask_cards(doc, layer)) {
    std::string text;
    if (!m.enabled) {
      phrases.push_back(text);
      continue;
    }
    /* A shape hidden whole, when it's built by more than adding: "everything but (mouth, minus cut, within lip)". */
    bool nested = false;
    bool first = true;
    for (const MaskStep &s : m.steps) {
      if (std::none_of(s.layers.begin(), s.layers.end(), [&](uint64_t id) { return layer_find(doc, id) != nullptr; })) {
        continue;
      }
      if (first && !(m.start_shown && mask_step_cuts(s))) {
        break;
      }
      nested = !first && s.mode == MaskMode::Add;
      first = false;
      if (nested) {
        break;
      }
    }
    bool any = false;
    for (const MaskStep &s : m.steps) {
      std::string names;
      for (const uint64_t id : s.layers) {
        if (const Layer *l = layer_find(doc, id)) {
          names += (names.empty() ? "" : mask_combine_joiner(s.combine)) + l->name;
        }
      }
      /* A group kept whole: its own steps, in brackets. */
      if (s.group) {
        std::string inner;
        for (const MaskStep &child : s.children) {
          if (child.whole) {
            inner = child.mode == MaskMode::Add ? "everything" : "nothing";
            continue;
          }
          const std::string words = layer_names(doc, child.layers, mask_combine_joiner(child.combine));
          const MaskVerb verb = mask_step_verb(child);
          inner += (inner.empty() ? "" : ", ") +
                   std::string(inner.empty() && verb == MaskVerb::Add ? "" : verb == MaskVerb::Add ? "plus " : verb == MaskVerb::Remove ? "minus " : "within ") +
                   (child.invert && verb == MaskVerb::Add ? "outside " : "") + words;
        }
        names = "(" + inner + ")";
      }
      if (s.whole) {
        text = s.mode == MaskMode::Add ? "everything" : "nothing";
        any = true;
        continue;
      }
      if (names.empty()) {
        continue;
      }
      /* Strokes Only masks with lines, not insides: "on ring's lines". */
      const char *in = "inside ";
      if (s.strokes_only) {
        names += "'s lines";
        in = "on ";
      }
      /* Extend: "minus mouth and above". */
      static const char *const BEYOND[] = {"", " and above", " and below", " and left of it", " and right of it"};
      names += BEYOND[int(s.extend)];
      const std::string where = (s.invert ? "outside " : "") + names;
      const bool add = s.mode == MaskMode::Add;
      if (!any && add != m.start_shown) {
        /* The first step on everything cuts from it ("everything but head"; hiding what's outside
         * leaves "inside head"); on nothing it shows ("inside head", "outside head"). */
        text = add ? (s.invert ? "outside " : in) + names
               : s.invert ? in + names
                          : (nested ? "everything but (" : "everything but ") + names;
      }
      else {
        if (!any) {
          /* A first step that changes nothing by itself: what it starts from first. */
          text = m.start_shown ? "everything, " : "nothing, ";
        }
        else {
          text += ", ";
        }
        /* Hiding what's outside: "within mouth". Inside a shape hidden whole, each paint means its opposite. */
        const bool adds = nested ? !add : add;
        text += adds ? "plus " + where : s.invert ? "within " + names : "minus " + where;
      }
      any = true;
    }
    if (nested) {
      text += ")";
    }
    if (!any) {
      phrases.push_back(std::string());
      continue;
    }
    /* Affects: "lines: everything but ear"; a named card says its name first ("Ear join: lines: ..."). */
    if (m.cuts != MaskCuts::All && !m.clip) {
      text = (m.cuts == MaskCuts::Lines ? "lines: " : "fills: ") + text;
    }
    if (!m.name.empty()) {
      text = m.name + ": " + text;
    }
    bool flat = false;
    if (mask_card_ends_lines(m, &flat)) {
      /* End Lines at Edge: "..., lines ending at the edge". */
      text += flat ? ", lines cut flat at the edge" : ", lines ending at the edge";
    }
    phrases.push_back(text);
  }
  return phrases;
}

std::string mask_summary(const Document &doc, const Layer &layer)
{
  /* "Shows: inside head, minus mouth; lines: everything but ear": each card in words. */
  std::string text;
  for (const std::string &phrase : mask_row_phrases(doc, layer)) {
    if (!phrase.empty()) {
      text += (text.empty() ? "" : "; ") + phrase;
    }
  }
  return text.empty() ? "Shows everything" : "Shows: " + text;
}

/* verb=: what the step does to the shape the card builds, ADD, REMOVE or KEEP_ONLY, whichever way the card is
 * saved. */
static bool mask_step_verb_arg(Context &C, MaskStep &step)
{
  if (!op_has_arg(C, "verb")) {
    return true;
  }
  MaskVerb verb;
  if (!mask_verb_from_identifier(op_arg(C, "verb"), &verb)) {
    report(ReportType::Error, "verb is ADD, REMOVE or KEEP_ONLY");
    return false;
  }
  mask_step_set_verb(step, verb);
  return true;
}

/* A global area's shape, from `source` to every other step with its number: all but each one's own paint. */
static void mask_area_sync(Document &doc, const MaskStep &source)
{
  if (source.global == 0) {
    return;
  }
  const MaskStep shared = source;
  for (const Layer *found : document_layers_flat(doc)) {
    Layer *layer = layer_find(doc, found->id);
    for (LayerMask &card : layer->masks) {
      for (MaskStep &s : card.steps) {
        if (s.global == shared.global) {
          const MaskMode mode = s.mode;
          const bool invert = s.invert, expanded = s.show_expanded;
          s = shared;
          s.mode = mode;
          s.invert = invert;
          s.show_expanded = expanded;
        }
      }
    }
  }
}

/* The steps of a global area, anywhere, and how many. */
static const MaskStep *mask_area_find(const Document &doc, uint64_t global, int *r_users = nullptr)
{
  const MaskStep *first = nullptr;
  int users = 0;
  for (const Layer *layer : document_layers_flat(doc)) {
    for (const LayerMask &card : layer->masks) {
      for (const MaskStep &s : card.steps) {
        if (s.global == global) {
          first = first ? first : &s;
          users++;
        }
      }
    }
  }
  if (r_users) {
    *r_users = users;
  }
  return first;
}

int mask_area_users(const Document &doc, uint64_t global)
{
  int users = 0;
  mask_area_find(doc, global, &users);
  return global != 0 ? users : 0;
}

std::vector<MaskAreaName> mask_areas(const Document &doc)
{
  std::vector<MaskAreaName> areas;
  for (const Layer *layer : document_layers_flat(doc)) {
    for (const LayerMask &card : layer->masks) {
      for (const MaskStep &s : card.steps) {
        if (s.global != 0 && std::none_of(areas.begin(), areas.end(), [&](const MaskAreaName &a) { return a.id == s.global; })) {
          areas.push_back({s.global, s.name});
        }
      }
    }
  }
  return areas;
}

/* rows=, | between them, else the picked ones: lowest first, each once. */
static std::vector<int> mask_rows_arg(Context &C)
{
  std::vector<int> rows;
  if (!op_has_arg(C, "rows")) {
    rows = C.wm ? C.wm->mask_area_rows : std::vector<int>();
  }
  const std::string list = op_arg(C, "rows");
  for (size_t at = 0; at < list.size();) {
    const size_t bar = std::min(list.find('|', at), list.size());
    rows.push_back(std::atoi(list.substr(at, bar - at).c_str()));
    at = bar + 1;
  }
  std::sort(rows.begin(), rows.end());
  rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
  return rows;
}

static std::string mask_rows_text(const std::vector<int> &rows)
{
  std::string text;
  for (const int row : rows) {
    text += (text.empty() ? "" : "|") + std::to_string(row);
  }
  return text;
}

/* The card `index=` (and its step `step=`, default 0) of a layer, or null. */
static LayerMask *card_at(Layer &layer, int index)
{
  return index >= 0 && index < int(layer.masks.size()) ? &layer.masks[size_t(index)] : nullptr;
}

void mask_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "mask.add";
    ot.name = "Add Mask";
    ot.description = "Add a mask card to a layer (the active and picked ones without layer=), or with card= a step "
                     "to that card: the layers in sources (ids or names, | between them; together they make one "
                     "shape) painted on the card's mask by mode=ADD (shows inside it) or SUBTRACT (hides inside it); "
                     "invert=1 paints the outside. A new card starts with start=EVERYTHING or NOTHING (without it, "
                     "from its step: Subtract on everything, Add on nothing) and takes cuts= (ALL, FILLS, LINES), "
                     "end_lines=, ends=, reach=. The step takes grow= (pixels; negative shrinks), shape=AS_DRAWN, "
                     "FILLS_ONLY or STROKES_ONLY, filled=1 (every shape filled), inside_stroke=1, extend=UP, DOWN, "
                     "LEFT or RIGHT. outside=1 is Subtract. Hidden layers still mask; the order of layers doesn't "
                     "matter. A layer shows only where all its cards show it";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int into = op_arg_int(C, "card", -1);
      MaskStep step;
      if (!mask_sources(C, &step.layers)) {
        return OpStatus::Cancelled;
      }
      /* whole=SHOW or HIDE: the whole canvas, at the bottom of the stack or the group, where it's of use. */
      const std::string whole = op_arg(C, "whole");
      if (!whole.empty() && (whole != "SHOW" && whole != "HIDE")) {
        report(ReportType::Error, "whole is SHOW or HIDE");
        return OpStatus::Cancelled;
      }
      if (step.layers.empty() && whole.empty()) {
        report(ReportType::Warning, "Name the layers to mask with (sources=)");
        return OpStatus::Cancelled;
      }
      int count = 0;
      std::string undo;
      for (Layer *layer : mask_targets(C)) {
        LayerMask *card = into >= 0 ? card_at(*layer, into) : nullptr;
        if (into >= 0 && !card) {
          continue;
        }
        MaskStep own = step;
        const bool first = !card || card->steps.empty();
        if (!op_has_arg(C, "mode") && !op_has_arg(C, "outside")) {
          /* Without a mode, as before rows: a first step adds, a later one keeps only inside. */
          mask_step_from_old_mode("", false, first, &own);
        }
        if (!mask_step_args(C, &own, first)) {
          return OpStatus::Cancelled;
        }
        if (card && !mask_step_verb_arg(C, own)) {
          return OpStatus::Cancelled;
        }
        std::erase(own.layers, layer->id);
        if (!whole.empty()) {
          own = {};
          own.whole = true;
          own.mode = whole == "SHOW" ? MaskMode::Add : MaskMode::Subtract;
        }
        else if (own.layers.empty()) {
          report(ReportType::Warning, "A layer can't be its own mask");
          continue;
        }
        if (card) {
          /* group=: into that group of the card, else on top of the stack. */
          const int into_group = op_arg_int(C, "group", -1);
          if (into_group >= 0 && into_group < int(card->steps.size()) && card->steps[size_t(into_group)].group) {
            MaskStep &group = card->steps[size_t(into_group)];
            group.children.insert(own.whole ? group.children.begin() : group.children.end(), own);
            mask_area_sync(doc, group);
          }
          else {
            card->steps.insert(own.whole ? card->steps.begin() : card->steps.end(), own);
          }
          undo = "Mask " + layer->name + ": " + mask_label(doc, *card);
          count++;
          continue;
        }
        LayerMask fresh;
        fresh.steps.push_back(own);
        mask_card_start_from_steps(fresh);
        if (!mask_card_args(C, &fresh)) {
          return OpStatus::Cancelled;
        }
        if (std::find(layer->masks.begin(), layer->masks.end(), fresh) != layer->masks.end()) {
          continue;
        }
        layer->masks.push_back(fresh);
        undo = "Mask " + layer->name + ": " + mask_label(doc, fresh);
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
    ot.description = "Remove one of a layer's mask cards (index=, from 0), or all of them without index; with "
                     "step= just that step (the card goes with its last one)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int index = op_arg_int(C, "index", -1);
      const int step = op_arg_int(C, "step", -1);
      const int child = op_arg_int(C, "child", -1);
      int count = 0;
      std::string undo;
      for (Layer *layer : mask_targets(C)) {
        if (index >= 0 && step >= 0 && child >= 0) {
          LayerMask *inside = card_at(*layer, index);
          if (inside && step < int(inside->steps.size()) && child < int(inside->steps[size_t(step)].children.size())) {
            MaskStep &group = inside->steps[size_t(step)];
            group.children.erase(group.children.begin() + child);
            mask_area_sync(doc, group);
            undo = "Remove Masking Area " + layer->name + ": " + mask_label(doc, *inside);
            count++;
          }
          continue;
        }
        if (index < 0) {
          count += int(layer->masks.size());
          layer->masks.clear();
          undo = "Remove " + layer->name + "'s Masks";
          continue;
        }
        LayerMask *card = card_at(*layer, index);
        if (!card) {
          continue;
        }
        if (step >= 0 && step < int(card->steps.size()) && card->steps.size() > 1) {
          card->steps.erase(card->steps.begin() + step);
          undo = "Remove Mask Step " + layer->name + ": " + mask_label(doc, *card);
          count++;
          continue;
        }
        if (step >= int(card->steps.size())) {
          continue;
        }
        undo = "Remove Mask " + layer->name + ": " + mask_label(doc, *card);
        layer->masks.erase(layer->masks.begin() + index);
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
    ot.idname = "mask.set";
    ot.name = "Change Mask";
    ot.description = "Change one of a layer's mask cards (index=): name=, start= (EVERYTHING, NOTHING), cuts= (ALL, FILLS, "
                     "LINES: what of the layer it masks; lines_only=1 as before), end_lines=0/1 (the layer's lines "
                     "end at its shape's edge), ends=BRUSH or FLAT, reach= (line widths past the edge, -2 to 2), "
                     "enabled=0/1 (in the viewport), render=0/1, toggle=1 (on or off in the viewport); and its step "
                     "step= (0 without it; with child=, that one inside a group): verb= (ADD, REMOVE, KEEP_ONLY), mode= "
                     "(ADD, SUBTRACT; a lone step's mode sets Start With too), area_name=, invert=0/1, "
                     "grow=, shape= (AS_DRAWN, FILLS_ONLY, STROKES_ONLY), filled=0/1, inside_stroke=0/1, extend= "
                     "(NONE, UP, DOWN, LEFT, RIGHT), sources= (| between layers' ids or names), outside= and expand= "
                     "as before rows";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const int index = op_arg_int(C, "index", 0);
      const int step_index = op_arg_int(C, "step", 0);
      const int child = op_arg_int(C, "child", -1);
      std::vector<uint64_t> sources;
      if (op_has_arg(C, "sources") && !mask_sources(C, &sources)) {
        return OpStatus::Cancelled;
      }
      int count = 0;
      std::string undo;
      for (Layer *layer : mask_targets(C)) {
        LayerMask *card = card_at(*layer, index);
        if (!card || step_index < 0 || step_index >= int(card->steps.size())) {
          continue;
        }
        MaskStep &top = card->steps[size_t(step_index)];
        if (child >= int(top.children.size())) {
          continue;
        }
        MaskStep &step = child >= 0 ? top.children[size_t(child)] : top;
        const MaskMode was = step.mode;
        if (!mask_step_args(C, &step, step_index == 0) || !mask_card_args(C, card) || !mask_step_verb_arg(C, step)) {
          return OpStatus::Cancelled;
        }
        /* A lone step's mode is the whole card's: Subtract alone starts from everything. */
        if (step.mode != was && card->steps.size() == 1 && !op_has_arg(C, "start") && !op_has_arg(C, "verb")) {
          mask_card_start_from_steps(*card);
        }
        if (op_has_arg(C, "sources")) {
          step.layers = sources;
          std::erase(step.layers, layer->id);
        }
        if (op_has_arg(C, "area_name")) {
          step.name = op_arg(C, "area_name");
        }
        mask_area_sync(doc, top);
        undo = op_has_arg(C, "name") ? "Rename Mask: " + mask_card_name(*card, index)
                                     : std::string(card->enabled ? "Mask " : "Mask Off ") + layer->name + ": " +
                                           mask_label(doc, *card);
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
    ot.description = "Move one of a layer's mask cards (index=) to another place in its list (to=), as dragging its "
                     "grip does; with step=, that step to another place in its card (to=), which can change what "
                     "the card shows";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      const int from = op_arg_int(C, "index", -1), to = op_arg_int(C, "to", -1);
      std::vector<Layer *> targets = mask_targets(C);
      if (targets.empty()) {
        return OpStatus::Cancelled;
      }
      Layer &layer = *targets.front();
      if (op_has_arg(C, "step")) {
        LayerMask *card = card_at(layer, from);
        const int step = op_arg_int(C, "step", -1);
        const int n = card ? int(card->steps.size()) : 0;
        if (step < 0 || step >= n || to < 0 || to >= n || step == to) {
          return OpStatus::Cancelled;
        }
        const MaskStep s = card->steps[size_t(step)];
        card->steps.erase(card->steps.begin() + step);
        card->steps.insert(card->steps.begin() + to, s);
        C.undo_name = "Move Mask Step";
        return OpStatus::Finished;
      }
      const int n = int(layer.masks.size());
      if (from < 0 || from >= n || to < 0 || to >= n || from == to) {
        return OpStatus::Cancelled;
      }
      LayerMask m = layer.masks[size_t(from)];
      layer.masks.erase(layer.masks.begin() + from);
      layer.masks.insert(layer.masks.begin() + to, m);
      C.undo_name = "Move Mask";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.duplicate";
    ot.name = "Duplicate Mask";
    ot.description = "Copy one of a layer's mask cards (index=) to just below it";
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
                     "picked layers, two of them): each gets a card subtracting the other's fills, inside their "
                     "lines, with End Lines at Edge, so their lines end where they meet. The lower one's card masks "
                     "only lines, so its fill covers where they overlap. A card already naming just the other is "
                     "reused";
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
        /* A card of its own: hide the other's fills (inside their lines), ending lines there. */
        LayerMask *card = nullptr;
        for (LayerMask &m : target.masks) {
          if (m.steps.size() == 1 && m.steps.front().layers == std::vector<uint64_t>{source}) {
            card = &m;
            break;
          }
        }
        if (!card) {
          target.masks.emplace_back();
          card = &target.masks.back();
          card->steps.emplace_back();
          card->steps.front().layers = {source};
        }
        MaskStep &step = card->steps.front();
        step.mode = MaskMode::Subtract;
        step.invert = false;
        step.fills_only = true;
        step.strokes_only = false;
        step.filled = false;
        step.inside_stroke = true;
        step.extend = MaskExtend::None;
        card->start_shown = true;
        card->enabled = true;
        card->render = true;
        card->end_lines = true;
        card->end_lines_flat = false;
        card->cuts = k == 0 ? MaskCuts::Lines : MaskCuts::All;
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
    OperatorType ot;
    ot.idname = "mask.group";
    ot.name = "Add Masking Group";
    ot.description = "A new group on top of card index='s stack (verb= ADD, REMOVE or KEEP_ONLY, name=): the areas put in "
                     "it build a shape of their own, which it then paints as a layer's would";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      const int index = op_arg_int(C, "index", 0);
      int count = 0;
      for (Layer *layer : mask_targets(C)) {
        LayerMask *card = card_at(*layer, index);
        if (!card) {
          continue;
        }
        MaskStep group;
        group.group = true;
        group.show_expanded = true;
        group.name = op_arg(C, "name", "Group");
        mask_step_set_verb(group, card->start_shown ? MaskVerb::Remove : MaskVerb::Add);
        if (!mask_step_verb_arg(C, group)) {
          return OpStatus::Cancelled;
        }
        card->steps.push_back(std::move(group));
        count++;
      }
      return count ? OpStatus::Finished : OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.area_move";
    ot.name = "Move Masking Area";
    ot.description = "Move step= of card index= (child= if it's inside a group) to place to= in the stack, or with "
                     "group= to place to= in that group. Groups don't nest";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::vector<Layer *> targets = mask_targets(C);
      LayerMask *card = targets.empty() ? nullptr : card_at(*targets.front(), op_arg_int(C, "index", 0));
      const int step = op_arg_int(C, "step", -1), child = op_arg_int(C, "child", -1);
      int to = op_arg_int(C, "to", 0), group = op_arg_int(C, "group", -1);
      if (!card) {
        return OpStatus::Cancelled;
      }
      std::vector<MaskStep> &steps = card->steps;
      /* rows=: several in the stack together, keeping their order. */
      if (op_has_arg(C, "rows")) {
        const std::vector<int> rows = mask_rows_arg(C);
        for (const int row : rows) {
          const int at = row >> 8;
          if ((row & 0xff) != 0 || at >= int(steps.size()) || at == group || (group >= 0 && steps[size_t(at)].group)) {
            return OpStatus::Cancelled;
          }
        }
        if (rows.empty() || (group >= 0 && (group >= int(steps.size()) || !steps[size_t(group)].group))) {
          return OpStatus::Cancelled;
        }
        std::vector<MaskStep> moved;
        for (const int row : rows) {
          moved.push_back(steps[size_t(row >> 8)]);
        }
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
          const int at = *it >> 8;
          steps.erase(steps.begin() + at);
          group -= group > at ? 1 : 0;
          to -= group < 0 && to > at ? 1 : 0;
        }
        std::vector<MaskStep> &into = group >= 0 ? steps[size_t(group)].children : steps;
        for (MaskStep &s : moved) {
          s.global = group >= 0 ? 0 : s.global;
        }
        into.insert(into.begin() + std::clamp(to, 0, int(into.size())), moved.begin(), moved.end());
        if (group >= 0) {
          mask_area_sync(doc, steps[size_t(group)]);
        }
        if (C.wm) {
          C.wm->mask_area_rows.clear();
          C.wm->mask_area_pick.layer = 0;
        }
        C.undo_name = "Move Masking Areas";
        return OpStatus::Finished;
      }
      if (step < 0 || step >= int(steps.size())) {
        return OpStatus::Cancelled;
      }
      if (child >= int(steps[size_t(step)].children.size()) || (child < 0 && group == step) ||
          (group >= 0 && (group >= int(steps.size()) || !steps[size_t(group)].group)) ||
          (group >= 0 && child < 0 && steps[size_t(step)].group))
      {
        return OpStatus::Cancelled;
      }
      /* Out of its place, keeping `group` and `to` meaning what they meant. */
      MaskStep moved;
      if (child >= 0) {
        std::vector<MaskStep> &kids = steps[size_t(step)].children;
        moved = kids[size_t(child)];
        kids.erase(kids.begin() + child);
        mask_area_sync(doc, steps[size_t(step)]);
        if (group == step && to > child) {
          to--;
        }
      }
      else {
        moved = steps[size_t(step)];
        steps.erase(steps.begin() + step);
        if (group > step) {
          group--;
        }
        if (group < 0 && to > step) {
          to--;
        }
      }
      if (group >= 0) {
        std::vector<MaskStep> &kids = steps[size_t(group)].children;
        moved.global = 0;
        kids.insert(kids.begin() + std::clamp(to, 0, int(kids.size())), moved);
        mask_area_sync(doc, steps[size_t(group)]);
      }
      else {
        steps.insert(steps.begin() + std::clamp(to, 0, int(steps.size())), moved);
      }
      C.undo_name = "Move Masking Area";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.area_delete_hovered";
    ot.name = "Delete Masking Area";
    ot.description = "Delete the masking area under the mouse in the Masking tab, with the others picked if it's one "
                     "of them (the last one takes its mask with it)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document && C.wm && C.wm->mask_area_hover.layer != 0; };
    ot.exec = [](Context &C) {
      const WindowManager::MaskAreaHover hover = C.wm->mask_area_hover;
      const int row = (hover.step << 8) | (hover.child + 1);
      const std::vector<int> &picked = C.wm->mask_area_rows;
      const bool many = C.wm->mask_area_pick.layer == hover.layer && C.wm->mask_area_pick.card == hover.card &&
                        std::find(picked.begin(), picked.end(), row) != picked.end();
      const std::vector<int> rows = many ? picked : std::vector<int>{row};
      const OpStatus status = operator_call(C, "mask.areas_delete", {{"layer", std::to_string(hover.layer)},
                                                                     {"index", std::to_string(hover.card)},
                                                                     {"rows", mask_rows_text(rows)}});
      C.wm->mask_area_hover = {};
      return status;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.areas_delete";
    ot.name = "Delete Masking Areas";
    ot.description = "Delete card index='s masking areas rows= ((step << 8) | (child + 1), | between them; the picked ones "
                     "without it). The last one takes its mask with it, and a group goes with what's in it";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      const std::vector<int> rows = mask_rows_arg(C);
      const std::string index = op_arg(C, "index", "0");
      const std::vector<Layer *> targets = mask_targets(C);
      if (rows.empty() || targets.empty()) {
        return OpStatus::Cancelled;
      }
      /* From the top down, so what's left keeps its place: inside a group before the group. */
      for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        OpArgs args = {{"layer", std::to_string(targets.front()->id)}, {"index", index}, {"step", std::to_string(*it >> 8)}};
        if ((*it & 0xff) != 0) {
          args["child"] = std::to_string((*it & 0xff) - 1);
        }
        operator_call(C, "mask.remove", args);
      }
      if (C.wm) {
        C.wm->mask_area_rows.clear();
        C.wm->mask_area_pick.layer = 0;
      }
      C.undo_name = "Delete Masking Areas";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.areas_set";
    ot.name = "Convert Masking Areas";
    ot.description = "Make card index='s masking areas rows= (the picked ones without it) verb= ADD, REMOVE or KEEP_ONLY";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      const std::vector<int> rows = mask_rows_arg(C);
      const std::vector<Layer *> targets = mask_targets(C);
      if (rows.empty() || targets.empty()) {
        return OpStatus::Cancelled;
      }
      for (const int row : rows) {
        OpArgs args = {{"layer", std::to_string(targets.front()->id)}, {"index", op_arg(C, "index", "0")},
                       {"step", std::to_string(row >> 8)}, {"verb", op_arg(C, "verb")}};
        if ((row & 0xff) != 0) {
          args["child"] = std::to_string((row & 0xff) - 1);
        }
        if (operator_call(C, "mask.set", args) != OpStatus::Finished) {
          return OpStatus::Cancelled;
        }
      }
      C.undo_name = "Convert Masking Areas";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.areas_group";
    ot.name = "Join as Group";
    ot.description = "Join card index='s masking areas rows= (the picked ones without it, Ctrl G in the Masking tab) as a "
                     "group, where the lowest of them was. All + stays a +, all - a -, painting as they did; any other mix "
                     "keeps each one's own and takes its shape from the base, as a new group does. Groups don't nest";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) {
      return mask_poll(C) && (op_has_arg(C, "rows") || !C.wm || (C.wm->mask_tab_shown && !C.wm->mask_area_rows.empty()));
    };
    ot.exec = [](Context &C) {
      std::vector<int> rows = mask_rows_arg(C);
      const std::vector<Layer *> targets = mask_targets(C);
      const int index = op_has_arg(C, "index") || !C.wm ? op_arg_int(C, "index", 0) : C.wm->mask_area_pick.card;
      LayerMask *card = targets.empty() ? nullptr : card_at(*targets.front(), index);
      if (!card || rows.empty()) {
        return OpStatus::Cancelled;
      }
      for (const int row : rows) {
        if ((row & 0xff) != 0 || (row >> 8) >= int(card->steps.size()) || card->steps[size_t(row >> 8)].group) {
          report(ReportType::Warning, "Groups don't go inside groups: pick areas that are in the stack, not in a group");
          return OpStatus::Cancelled;
        }
      }
      MaskStep group;
      group.group = true;
      group.show_expanded = true;
      group.name = "Group";
      const MaskVerb first = mask_step_verb(card->steps[size_t(rows.front() >> 8)]);
      const bool alike = first != MaskVerb::KeepOnly && std::all_of(rows.begin(), rows.end(), [&](int row) {
        return mask_step_verb(card->steps[size_t(row >> 8)]) == first;
      });
      mask_step_set_verb(group, alike ? first : card->start_shown ? MaskVerb::Remove : MaskVerb::Add);
      for (const int row : rows) {
        MaskStep s = card->steps[size_t(row >> 8)];
        s.global = 0;
        if (alike) {
          s.mode = MaskMode::Add;
        }
        group.children.push_back(std::move(s));
      }
      const int low = rows.front() >> 8;
      for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        card->steps.erase(card->steps.begin() + (*it >> 8));
      }
      card->steps.insert(card->steps.begin() + low, std::move(group));
      if (C.wm) {
        C.wm->mask_area_pick = {targets.front()->id, index, low, -1};
        C.wm->mask_area_rows = {low << 8};
      }
      C.undo_name = "Join as Group";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mask.area_global";
    ot.name = "Global Masking Area";
    ot.description = "Step step= of card index=, as a shared material: action=MAKE makes it a global area (name=), "
                     "action=LINK links it to global area global=, keeping its own + or -, action=UNLINK makes it its "
                     "own again, looking the same";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::vector<Layer *> targets = mask_targets(C);
      LayerMask *card = targets.empty() ? nullptr : card_at(*targets.front(), op_arg_int(C, "index", 0));
      const int index = op_arg_int(C, "step", -1);
      if (!card || index < 0 || index >= int(card->steps.size())) {
        return OpStatus::Cancelled;
      }
      MaskStep &step = card->steps[size_t(index)];
      const std::string action = op_arg(C, "action");
      if (action == "MAKE") {
        uint64_t next = 1;
        for (const MaskAreaName &a : mask_areas(doc)) {
          next = std::max(next, a.id + 1);
        }
        step.global = next;
        std::string base = op_arg(C, "name", step.name);
        if (base.empty()) {
          for (const uint64_t id : step.layers) {
            if (const Layer *l = layer_find(doc, id)) {
              base = l->name;
              break;
            }
          }
        }
        std::string name = base.empty() ? std::string("Area") : base;
        const std::vector<MaskAreaName> taken = mask_areas(doc);
        for (int n = 1; std::any_of(taken.begin(), taken.end(), [&](const MaskAreaName &a) { return a.name == name; }); n++) {
          char suffix[16];
          std::snprintf(suffix, sizeof(suffix), ".%03d", n);
          name = base + suffix;
        }
        step.name = name;
      }
      else if (action == "LINK") {
        const uint64_t global = std::strtoull(op_arg(C, "global").c_str(), nullptr, 10);
        const MaskStep *shared = mask_area_find(doc, global);
        if (!shared) {
          report(ReportType::Warning, "There's no such global masking area");
          return OpStatus::Cancelled;
        }
        const MaskMode mode = step.mode;
        const bool invert = step.invert;
        const MaskStep copy = *shared;
        step = copy;
        step.mode = mode;
        step.invert = invert;
      }
      else if (action == "UNLINK") {
        step.global = 0;
      }
      else {
        report(ReportType::Error, "action is MAKE, LINK or UNLINK");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* Inside... / Outside...: the viewport or the Layers editor gives the layer clicked. */
    OperatorType ot;
    ot.idname = "mask.pick";
    ot.name = "Pick Mask Layer";
    ot.description = "Click the layer to mask with (a new card, mode=ADD or SUBTRACT; INSIDE is Add, OUTSIDE is "
                     "Subtract), in the viewport or the Layers editor; with index=, a new step of that card "
                     "instead (verb=ADD, REMOVE or KEEP_ONLY; group= puts it in that group), and with step= too, it "
                     "joins that step. Esc cancels";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = mask_poll;
    ot.exec = [](Context &C) {
      std::string mode = op_arg(C, "mode", "INSIDE");
      MaskMode parsed;
      if (mode == "OUTSIDE") {
        mode = "SUBTRACT";
      }
      if (mode != "INSIDE" && !mask_mode_from_identifier(mode, &parsed)) {
        report(ReportType::Error, "mode is ADD, SUBTRACT, INSIDE or OUTSIDE");
        return OpStatus::Cancelled;
      }
      WindowManager::MaskPick &pick = C.wm->mask_pick;
      pick = {};
      pick.mode = mode == "INSIDE" ? std::string() : mode;
      pick.verb = op_arg(C, "verb");
      pick.group = op_arg_int(C, "group", -1);
      pick.index = op_arg_int(C, "index", -1);
      pick.step = op_arg_int(C, "step", -1);
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
