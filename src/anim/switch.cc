#include "anim/switch.hh"

#include <cmath>
#include <set>

#include "data/document.hh"

namespace oanim {

/* Values are layer ids, small whole numbers that floats hold exactly (as Draw Order's). */
static uint64_t value_id(float value)
{
  return value > 0.5f ? uint64_t(std::llround(value)) : 0;
}

static bool is_rig(float value)
{
  return value < -0.5f;
}

const Layer *switch_child(const Layer &sw, float value)
{
  const uint64_t id = value_id(value);
  if (!id) {
    return nullptr;
  }
  for (const Layer &child : sw.children) {
    if (child.id == id) {
      return &child;
    }
  }
  return nullptr;
}

float switch_default(const Layer &sw)
{
  const Channel<float> &ch = sw.switch_keys;
  if (ch.animated()) {
    return ch.evaluate(0);
  }
  /* Not animated: the plain value is the data, unless an edit not yet recorded changed it. */
  return ch.has_last ? ch.last : sw.switch_value;
}

std::string switch_value_label(const Layer &sw, float value)
{
  if (is_rig(value)) {
    return "Rig";
  }
  const uint64_t id = value_id(value);
  if (!id) {
    return "None";
  }
  if (const Layer *child = switch_child(sw, value)) {
    return child->name;
  }
  return std::to_string(id) + " (missing)";
}

bool switch_value_from_name(const Layer &sw, const std::string &name, float *r_value)
{
  if (name.empty() || name == "NONE") {
    *r_value = 0.0f;
    return true;
  }
  if (name == "RIG") {
    *r_value = SWITCH_RIG;
    return true;
  }
  for (const Layer &child : sw.children) {
    if (child.name == name) {
      *r_value = float(child.id);
      return true;
    }
  }
  return false;
}

/* The hand key holding at `frame` (after frame 0), unless it is a Rig key; false when there is none. */
static bool hand_value(const Layer &sw, int frame, float *r_value)
{
  const Key<float> *held = nullptr;
  for (const Key<float> &k : sw.switch_keys.keys) {
    if (k.frame > frame) {
      break;
    }
    if (k.frame > 0) {
      held = &k;
    }
  }
  if (!held || is_rig(held->value)) {
    return false;
  }
  *r_value = held->value;
  return true;
}

static uint64_t g_preview_switch = 0, g_preview_child = 0;
static bool g_preview = false;

void switch_preview_set(uint64_t sw, uint64_t child)
{
  g_preview = true;
  g_preview_switch = sw;
  g_preview_child = child;
}

void switch_preview_clear()
{
  g_preview = false;
}

bool switch_preview_active(uint64_t *r_sw, uint64_t *r_child)
{
  if (r_sw) {
    *r_sw = g_preview_switch;
  }
  if (r_child) {
    *r_child = g_preview_child;
  }
  return g_preview;
}

int switch_keys_showing(const Layer &sw, uint64_t child)
{
  int count = 0;
  const auto shows = [&](float v) { return value_id(v) == child; };
  for (const Key<float> &k : sw.switch_keys.keys) {
    count += shows(k.value) ? 1 : 0;
  }
  for (const auto &[name, keys] : sw.switch_keys.actions) {
    for (const Key<float> &k : keys) {
      count += shows(k.value) ? 1 : 0;
    }
  }
  if (!sw.switch_keys.animated() && shows(sw.switch_value)) {
    count++;
  }
  return count;
}

/* `out`: switched out as renders show it; `edit_out`: as the viewport does, where the child holding
 * the layer being edited (`editing`: it and the layers holding it) shows in place of the pick. */
static void resolve(std::vector<Layer> &layers, int frame, bool out, bool edit_out, const std::set<uint64_t> &editing)
{
  for (Layer &layer : layers) {
    layer.switched_out = out;
    layer.edit_out = edit_out;
    if (!layer.is_switch()) {
      resolve(layer.children, frame, out, edit_out, editing);
      continue;
    }
    float value;
    const Channel<float> &keys = layer.switch_keys;
    if (frame > 0 && keys.animated() && !is_rig(layer.switch_value) &&
        std::fabs(layer.switch_value - keys.evaluate(frame)) > 0.5f)
    {
      /* A pick not keyed (Auto Keying off): shown, as a hand key would be, until the frame changes. */
      value = layer.switch_value;
    }
    else if (!hand_value(layer, frame, &value)) {
      value = layer.switch_rig_set && !is_rig(layer.switch_rig) ? layer.switch_rig : switch_default(layer);
    }
    /* A child that is gone shows the default, and the default gone shows none. */
    const Layer *shown = switch_child(layer, value);
    if (!shown && value_id(value)) {
      shown = switch_child(layer, switch_default(layer));
    }
    layer.switch_shown = shown ? shown->id : 0;
    uint64_t edit_shown = layer.switch_shown;
    for (const Layer &child : layer.children) {
      if (editing.count(child.id)) {
        edit_shown = child.id;
      }
    }
    if (g_preview && g_preview_switch == layer.id) {
      edit_shown = g_preview_child;
    }
    for (Layer &child : layer.children) {
      const bool was_out = child.switched_out, was_edit_out = child.edit_out;
      child.switched_out = out || child.id != layer.switch_shown;
      child.edit_out = edit_out || child.id != edit_shown;
      if (child.switched_out != was_out || child.edit_out != was_edit_out) {
        /* Shown or hidden now: views draw it anew. */
        layer_touch(child);
        layer_touch(layer);
      }
      resolve(child.children, frame, child.switched_out, child.edit_out, editing);
    }
  }
}

void switches_resolve(Document &doc, int frame)
{
  std::set<uint64_t> editing;
  for (const Layer *l = layer_find(doc, doc.active_layer); l; l = layer_parent(doc, l->id)) {
    editing.insert(l->id);
  }
  resolve(doc.layers, frame, false, false, editing);
}

void switch_set(Document &doc, Layer &sw, float value)
{
  Channel<float> &ch = sw.switch_keys;
  const int frame = doc.frame_current;
  if (frame != 0 && !doc.auto_key) {
    /* Auto Keying off: shown, not keyed, as any property edited then. Keyed, it shows until the frame
     * changes (I keys it); not keyed yet, it is the switch's value everywhere. */
    sw.switch_value = value;
    if (!ch.animated()) {
      ch.last = value;
      ch.has_last = true;
    }
    switches_resolve(doc, frame);
    return;
  }
  if (frame != 0 && !ch.animated() && doc.action.empty()) {
    /* The default stays what it was (an action's keys start after frame 0, the shared setup). */
    ch.set_key(0, switch_default(sw)).interp = Interp::Step;
  }
  if (frame != 0 || ch.animated()) {
    ch.set_key(frame, value).interp = Interp::Step;
  }
  if (frame == 0 || !ch.animated()) {
    sw.switch_value = value;
  }
  else {
    sw.switch_value = ch.evaluate(frame);
  }
  ch.last = sw.switch_value;
  ch.has_last = true;
  switches_resolve(doc, frame);
}

void switch_set_hold(Document &doc, Layer &sw, float value)
{
  Channel<float> &ch = sw.switch_keys;
  const Key<float> *start = nullptr;
  for (const Key<float> &k : ch.keys) {
    if (k.frame > doc.frame_current) {
      break;
    }
    start = &k;
  }
  if (!start || (start->frame == 0 && doc.action.empty())) {
    /* The hold from frame 0: the default. */
    const int now = doc.frame_current;
    doc.frame_current = 0;
    switch_set(doc, sw, value);
    doc.frame_current = now;
    sw.switch_value = ch.animated() ? ch.evaluate(now) : value;
    ch.last = sw.switch_value;
    switches_resolve(doc, now);
    return;
  }
  ch.set_key(start->frame, value).interp = Interp::Step;
  sw.switch_value = ch.evaluate(doc.frame_current);
  ch.last = sw.switch_value;
  ch.has_last = true;
  switches_resolve(doc, doc.frame_current);
}

int switch_replace_keys(Document &doc, Layer &sw, float from, float to)
{
  const auto same = [&](float v) { return std::fabs(v - from) < 0.5f; };
  int count = 0;
  const auto replace = [&](std::vector<Key<float>> &keys) {
    for (Key<float> &k : keys) {
      if (same(k.value)) {
        k.value = to;
        count++;
      }
    }
  };
  Channel<float> &ch = sw.switch_keys;
  replace(ch.keys);
  for (auto &[name, keys] : ch.actions) {
    replace(keys);
  }
  if (!ch.animated() && same(sw.switch_value)) {
    sw.switch_value = to;
    count++;
  }
  sw.switch_value = ch.animated() ? ch.evaluate(doc.frame_current) : sw.switch_value;
  ch.last = sw.switch_value;
  ch.has_last = true;
  switches_resolve(doc, doc.frame_current);
  return count;
}

SwitchSource switch_source(const Layer &sw, int frame)
{
  float value;
  if (hand_value(sw, frame, &value)) {
    return SwitchSource::Hand;
  }
  return sw.switch_rig_set && !is_rig(sw.switch_rig) ? SwitchSource::Rig : SwitchSource::Default;
}

bool switch_keyed_by_actions(const Layer &sw)
{
  for (const auto &[name, keys] : sw.switch_keys.actions) {
    if (!name.empty() && !keys.empty()) {
      return true;
    }
  }
  return false;
}

Layer *switch_of(Document &doc, uint64_t layer)
{
  for (Layer *l = layer_find(doc, layer); l; l = layer_parent(doc, l->id)) {
    if (l->is_switch()) {
      return l;
    }
  }
  return nullptr;
}

const Layer *switch_child_holding(const Document &doc, const Layer &sw, uint64_t layer)
{
  for (const Layer *l = layer_find(doc, layer); l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    for (const Layer &child : sw.children) {
      if (child.id == l->id) {
        return &child;
      }
    }
  }
  return nullptr;
}

void switch_child_added(Layer &sw, uint64_t child)
{
  if (!sw.is_switch() || sw.children.size() != 1 || sw.switch_keys.animated() || value_id(sw.switch_value)) {
    return;
  }
  sw.switch_value = float(child);
  sw.switch_keys.last = sw.switch_value;
  sw.switch_keys.has_last = true;
}

void switch_remap(Layer &layer, const std::map<uint64_t, uint64_t> &remap)
{
  if (!layer.is_switch()) {
    return;
  }
  const auto map = [&](float &value) {
    if (const auto it = remap.find(value_id(value)); it != remap.end()) {
      value = float(it->second);
    }
  };
  map(layer.switch_value);
  map(layer.switch_keys.last);
  for (Key<float> &k : layer.switch_keys.keys) {
    map(k.value);
  }
  for (auto &[name, keys] : layer.switch_keys.actions) {
    for (Key<float> &k : keys) {
      map(k.value);
    }
  }
  layer.switch_shown = 0;
}

}  // namespace oanim
