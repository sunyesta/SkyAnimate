#include "anim/switch.hh"

#include <cmath>

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

static void resolve(std::vector<Layer> &layers, int frame, bool out)
{
  for (Layer &layer : layers) {
    layer.switched_out = out;
    if (!layer.is_switch()) {
      resolve(layer.children, frame, out);
      continue;
    }
    float value;
    if (!hand_value(layer, frame, &value)) {
      value = layer.switch_rig_set && !is_rig(layer.switch_rig) ? layer.switch_rig : switch_default(layer);
    }
    /* A child that is gone shows the default, and the default gone shows none. */
    const Layer *shown = switch_child(layer, value);
    if (!shown && value_id(value)) {
      shown = switch_child(layer, switch_default(layer));
    }
    layer.switch_shown = shown ? shown->id : 0;
    for (Layer &child : layer.children) {
      child.switched_out = out || child.id != layer.switch_shown;
      resolve(child.children, frame, child.switched_out);
    }
  }
}

void switches_resolve(Document &doc, int frame)
{
  resolve(doc.layers, frame, false);
}

void switch_set(Document &doc, Layer &sw, float value)
{
  Channel<float> &ch = sw.switch_keys;
  const int frame = doc.frame_current;
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
