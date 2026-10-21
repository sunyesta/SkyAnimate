#include "anim/draw_order.hh"

#include <algorithm>
#include <cmath>

#include "data/document.hh"

namespace oanim {

DrawOrderRule draw_order_rule(float value)
{
  DrawOrderRule rule;
  const long long id = std::llround(value);
  if (id > 0) {
    rule = {DrawOrderMode::Front, uint64_t(id)};
  }
  else if (id < 0) {
    rule = {DrawOrderMode::Behind, uint64_t(-id)};
  }
  return rule;
}

float draw_order_value(const DrawOrderRule &rule)
{
  if (rule.mode == DrawOrderMode::Normal || rule.target == 0) {
    return 0.0f;
  }
  return rule.mode == DrawOrderMode::Front ? float(rule.target) : -float(rule.target);
}

const char *draw_order_mode_identifier(DrawOrderMode mode)
{
  return mode == DrawOrderMode::Front ? "FRONT" : mode == DrawOrderMode::Behind ? "BEHIND" : "NORMAL";
}

bool draw_order_mode_from_identifier(const std::string &identifier, DrawOrderMode *r_mode)
{
  for (const DrawOrderMode m : {DrawOrderMode::Normal, DrawOrderMode::Front, DrawOrderMode::Behind}) {
    if (identifier == draw_order_mode_identifier(m)) {
      *r_mode = m;
      return true;
    }
  }
  return false;
}

/* Drawn as one image: a group faded below full opacity. */
static bool isolated(const Layer &layer)
{
  return layer.is_group() && layer.opacity < 1.0f;
}

bool draw_order_any_rule(const std::vector<Layer> &layers)
{
  for (const Layer &l : layers) {
    if (std::llround(l.draw_order) != 0 || draw_order_any_rule(l.children)) {
      return true;
    }
  }
  return false;
}

namespace {

struct Entry {
  DrawStep step;
  /* The ids from the scope down to the layer; for a faded group, every layer inside it too (a rule
   * naming one of them goes by the group). */
  std::vector<uint64_t> chain;
};

struct Owner {
  uint64_t id = 0;
  DrawOrderRule rule;
  int depth = 0;
  int order = 0;
};

bool chain_has(const Entry &e, uint64_t id)
{
  return std::find(e.chain.begin(), e.chain.end(), id) != e.chain.end();
}

void add_inside(const Layer &layer, std::vector<uint64_t> &r_ids)
{
  for (const Layer &child : layer.children) {
    r_ids.push_back(child.id);
    add_inside(child, r_ids);
  }
}

bool holds(const Layer &layer, uint64_t id)
{
  for (const Layer &child : layer.children) {
    if (child.id == id || holds(child, id)) {
      return true;
    }
  }
  return false;
}

/* The first rule inside a faded group that names a layer outside it: it moves the whole group. */
bool rule_leaving(const Layer &group, const Layer &layer, DrawOrderRule *r_rule)
{
  for (const Layer &child : layer.children) {
    const DrawOrderRule rule = draw_order_rule(child.draw_order);
    if (rule.mode != DrawOrderMode::Normal && rule.target != group.id && !holds(group, rule.target)) {
      *r_rule = rule;
      return true;
    }
    if (rule_leaving(group, child, r_rule)) {
      return true;
    }
  }
  return false;
}

struct Flatten {
  const LayerShown &shown;
  std::vector<Entry> entries;
  std::vector<Owner> owners;
  std::vector<uint64_t> chain;
  std::vector<const Layer *> masked;
  int counter = 0;

  void run(const std::vector<Layer> &list)
  {
    /* Bottom first, as drawn. */
    for (int i = int(list.size()) - 1; i >= 0; i--) {
      const Layer &l = list[size_t(i)];
      if (!shown(l) || l.opacity <= 0.0f) {
        continue;
      }
      const bool has_masks = !l.masks.empty() || l.clip_to_below;
      if (has_masks) {
        masked.push_back(&l);
      }
      chain.push_back(l.id);
      DrawOrderRule rule = draw_order_rule(l.draw_order);
      if (rule.mode == DrawOrderMode::Normal && isolated(l)) {
        rule_leaving(l, l, &rule);
      }
      if (rule.mode != DrawOrderMode::Normal) {
        owners.push_back({l.id, rule, int(chain.size()), counter});
      }
      counter++;
      if (l.is_group() && !isolated(l)) {
        run(l.children);
      }
      else {
        Entry e;
        e.step.layer = &l;
        e.step.masked = masked;
        e.chain = chain;
        if (isolated(l)) {
          add_inside(l, e.chain);
        }
        entries.push_back(std::move(e));
      }
      chain.pop_back();
      if (has_masks) {
        masked.pop_back();
      }
    }
  }
};

}  // namespace

std::vector<DrawStep> draw_order_steps(const Document & /*doc*/, const std::vector<Layer> &scope,
                                       const LayerShown &shown)
{
  Flatten f{shown};
  f.run(scope);
  std::vector<Entry> &list = f.entries;
  /* Outer rules first, so a group moves whole and a rule inside it then moves its own part; ties
   * in the Layers editor's order, bottom first. */
  std::stable_sort(f.owners.begin(), f.owners.end(), [](const Owner &a, const Owner &b) {
    return a.depth != b.depth ? a.depth < b.depth : a.order < b.order;
  });
  for (const Owner &o : f.owners) {
    bool moving = false, related = false, found = false;
    for (const Entry &e : list) {
      const bool mine = chain_has(e, o.id);
      moving |= mine;
      related |= mine && chain_has(e, o.rule.target);
      found |= !mine && chain_has(e, o.rule.target);
    }
    if (!moving || !found || related) {
      /* Nothing of it drawn, its target isn't drawn here, or one holds the other: as it is. */
      continue;
    }
    std::vector<Entry> block, rest;
    for (Entry &e : list) {
      (chain_has(e, o.id) ? block : rest).push_back(std::move(e));
    }
    int first = -1, last = -1;
    for (int i = 0; i < int(rest.size()); i++) {
      if (chain_has(rest[size_t(i)], o.rule.target)) {
        first = first < 0 ? i : first;
        last = i;
      }
    }
    const int at = o.rule.mode == DrawOrderMode::Front ? last + 1 : first;
    rest.insert(rest.begin() + at, std::make_move_iterator(block.begin()), std::make_move_iterator(block.end()));
    list = std::move(rest);
  }
  std::vector<DrawStep> steps;
  steps.reserve(list.size());
  for (Entry &e : list) {
    steps.push_back(std::move(e.step));
  }
  return steps;
}

static void expand(const Document &doc, const std::vector<Layer> &scope, const LayerShown &shown,
                   std::vector<const Layer *> &r_out)
{
  for (const DrawStep &s : draw_order_steps(doc, scope, shown)) {
    if (isolated(*s.layer)) {
      expand(doc, s.layer->children, shown, r_out);
    }
    else {
      r_out.push_back(s.layer);
    }
  }
}

std::vector<const Layer *> draw_order_layers(const Document &doc, const LayerShown &shown)
{
  std::vector<const Layer *> out;
  expand(doc, doc.layers, shown, out);
  return out;
}

std::string draw_order_label(const Document &doc, float value)
{
  const DrawOrderRule rule = draw_order_rule(value);
  if (rule.mode == DrawOrderMode::Normal) {
    return "Normal";
  }
  const Layer *target = layer_find(doc, rule.target);
  const std::string name = target ? target->name : "a deleted layer";
  return std::string(rule.mode == DrawOrderMode::Front ? "In Front Of " : "Behind ") + name +
         (target ? "" : " (missing)");
}

void layer_draw_order_set(Document &doc, Layer &layer, const DrawOrderRule &rule)
{
  const float value = draw_order_value(rule);
  Channel<float> &ch = layer.draw_order_keys;
  const int frame = doc.frame_current;
  if (frame != 0 && !ch.animated()) {
    /* The setup keeps what it had (Normal, or its setting at rest). */
    ch.set_key(0, ch.has_last ? ch.last : layer.draw_order);
  }
  if (frame != 0 || ch.animated()) {
    ch.set_key(frame, value);
  }
  layer.draw_order = value;
  ch.last = value;
  ch.has_last = true;
}

bool layer_inside(const Document &doc, uint64_t layer, uint64_t ancestor)
{
  for (const Layer *l = layer_find(doc, layer); l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    if (l->id == ancestor) {
      return true;
    }
  }
  return false;
}

const Layer *draw_order_faded_group(const Document &doc, uint64_t layer)
{
  const Layer *outer = nullptr;
  const Layer *l = layer_find(doc, layer);
  for (l = l ? layer_parent(const_cast<Document &>(doc), l->id) : nullptr; l;
       l = layer_parent(const_cast<Document &>(doc), l->id))
  {
    if (isolated(*l)) {
      outer = l;
    }
  }
  return outer;
}

}  // namespace oanim
