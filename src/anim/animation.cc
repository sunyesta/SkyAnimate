#include "anim/animation.hh"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <climits>
#include <cstdio>
#include <set>

#include "anim/deform.hh"
#include "anim/dynamics.hh"
#include "anim/reference.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/document.hh"

namespace oanim {

/* Calls f(channel, value) for every channel of a layer (not its children). */
template<typename F> static void layer_for_each_channel(Layer &layer, F &&f)
{
  for (CustomChannel &c : layer.custom_channels) {
    f(c.keys, c.value);
  }
  /* Visibility is a stepped number beside the plain `visible` flag. */
  layer.visible_value = layer.visible ? 1.0f : 0.0f;
  f(layer.visible_keys, layer.visible_value);
  layer.visible = layer.visible_value > 0.5f;
  /* Keys pasted or opened from a file hold too. */
  for (Key<float> &k : layer.visible_keys.keys) {
    k.interp = Interp::Step;
  }
  f(layer.draw_order_keys, layer.draw_order);
  for (Key<float> &k : layer.draw_order_keys.keys) {
    k.interp = Interp::Step;
  }
  f(layer.position_keys, layer.position);
  f(layer.rotation_keys, layer.rotation);
  f(layer.scale_keys, layer.scale);
  f(layer.opacity_keys, layer.opacity);
  for (Bone &bone : layer.bones) {
    /* A bone's keys are its Transform and Angle: the change from its setup pose. */
    bone_changes_from_values(bone);
    f(bone.position_keys, bone.transform);
    f(bone.angle_keys, bone.turn);
    bone_values_from_changes(bone);
    f(bone.scale_keys, bone.scale);
    for (BoneConstraint &c : bone.constraints) {
      f(c.influence_keys, c.influence);
      f(c.weight_keys, c.weight);
      f(c.wind_catch_keys, c.wind_catch);
      f(c.gravity_keys, c.gravity);
      f(c.wind_keys, c.wind);
      f(c.wind_direction_keys, c.wind_direction);
      f(c.wind_gusts_keys, c.wind_gusts);
      f(c.wind_gust_speed_keys, c.wind_gust_speed);
      f(c.wind_turbulence_keys, c.wind_turbulence);
      for (BoneSpring *s : {&c.angle_spring, &c.position_spring, &c.scale_spring}) {
        f(s->settle_speed_keys, s->settle_speed);
        f(s->bounce_keys, s->bounce);
        f(s->inertia_keys, s->inertia);
        f(s->stretch_keys, s->stretch);
        f(s->limit_keys, s->limit);
      }
    }
  }
  /* A reference's drawing is built from its original's: its own channels are its offsets. */
  for (auto &[id, o] : layer.reference.points) {
    f(o.offset_keys, o.offset);
  }
  for (auto &[key, o] : layer.reference.handles) {
    f(o.offset_keys, o.offset);
  }
  if (layer.is_reference()) {
    return;
  }
  /* A point's keys are its Transform and a handle's its Angle and Length: the change from the setup
   * pose. */
  drawing_changes_from_values(layer.drawing);
  for (MeshPoint &p : layer.drawing.points) {
    f(p.pos_keys, p.transform);
    f(p.radius_keys, p.radius);
  }
  for (Curve &curve : layer.drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      f(cp.curvature_keys, cp.curvature);
      f(cp.handle_in_keys, cp.change_in);
      f(cp.handle_out_keys, cp.change_out);
    }
  }
  drawing_values_from_changes(layer.drawing);
}

template<typename F> static void layers_for_each_channel(std::vector<Layer> &layers, F &&f)
{
  for (Layer &layer : layers) {
    layer_for_each_channel(layer, f);
    layers_for_each_channel(layer.children, f);
  }
}

template<typename T> static void record(Channel<T> &ch, const T &value, int frame, bool auto_key)
{
  if (!ch.has_last) {
    /* New data (a point just drawn): it is simply there, on every frame. */
    ch.last = value;
    ch.has_last = true;
    return;
  }
  if (value == ch.last) {
    return;
  }
  if (ch.keyed_by_hand && !ch.animated()) {
    /* Not keyed yet: the change is just there; `last` keeps the value from before it, for the
     * setup key if it gets keyed by hand now. */
    return;
  }
  /* Keys hold the value without what smart actions add. */
  const T base = value - ch.smart;
  if (frame == 0) {
    /* The setup frame: the frame-0 key, or the value itself while it isn't animated. */
    if (ch.animated()) {
      ch.set_key(0, base);
    }
  }
  else if (auto_key && (ch.animated() || !ch.keyed_by_hand)) {
    if (!ch.animated()) {
      ch.set_key(0, ch.last - ch.smart);
    }
    ch.set_key(frame, base);
  }
  ch.last = value;
}

template<typename T> static void evaluate(Channel<T> &ch, T &value, int frame)
{
  if (ch.animated()) {
    value = ch.evaluate(frame);
  }
  else {
    /* Unkeyed, the plain value is the data: take off what smart actions added last time. */
    value -= ch.smart;
  }
  ch.smart = T{};
  ch.last = value;
  ch.has_last = true;
}

static void apply_smart_actions(Document &doc);

/* --- The document's Simulation settings, animatable --------------------------------------------- */

static const std::pair<const char *, float Document::*> SIMULATION_PROPS[] = {
    {"gravity", &Document::gravity},
    {"wind", &Document::wind},
    {"wind_direction", &Document::wind_direction},
    {"wind_gusts", &Document::wind_gusts},
    {"wind_gust_speed", &Document::wind_gust_speed},
    {"wind_turbulence", &Document::wind_turbulence},
};

Layer &document_simulation(Document &doc)
{
  Layer &sim = doc.simulation;
  sim.id = SIMULATION_LAYER_ID;
  sim.name = "Simulation";
  sim.type = LayerType::Group;
  for (const auto &[path, field] : SIMULATION_PROPS) {
    const auto it = std::find_if(sim.custom_channels.begin(), sim.custom_channels.end(),
                                 [&](const CustomChannel &c) { return c.path == path; });
    if (it == sim.custom_channels.end()) {
      sim.custom_channels.push_back({path, doc.*field, doc.*field, Channel<float>::by_hand()});
    }
  }
  return sim;
}

bool simulation_path(const std::string &path)
{
  for (const auto &[name, field] : SIMULATION_PROPS) {
    if (path == name) {
      return true;
    }
  }
  return false;
}

/* The settings and their channels made to agree: a setting edited since they last did goes into
 * its channel (to be recorded); a channel changed since (its keys removed, say) goes into the
 * setting. */
static void simulation_in(Document &doc)
{
  for (CustomChannel &c : document_simulation(doc).custom_channels) {
    for (const auto &[name, field] : SIMULATION_PROPS) {
      if (c.path != name) {
        continue;
      }
      if (doc.*field != c.synced) {
        c.value = doc.*field;
      }
      else {
        doc.*field = c.value;
      }
      c.synced = c.value;
    }
  }
}

/* The channels' values for the frame into the settings (after evaluating). */
static void simulation_out(Document &doc)
{
  for (CustomChannel &c : doc.simulation.custom_channels) {
    for (const auto &[name, field] : SIMULATION_PROPS) {
      if (c.path == name) {
        doc.*field = c.value;
        c.synced = c.value;
      }
    }
  }
}

float simulation_value_at(const Document &doc, const char *path, float plain, int frame)
{
  for (const CustomChannel &c : doc.simulation.custom_channels) {
    if (c.path == path && c.keys.animated()) {
      return c.keys.evaluate(std::max(frame, 0));
    }
  }
  return plain;
}

void animation_record(Document &doc)
{
  if (doc.evaluated_frame < 0) {
    /* Nothing evaluated yet: the plain values are where everything starts. */
    animation_evaluate(doc, doc.frame_current);
    return;
  }
  const int frame = doc.evaluated_frame;
  const bool auto_key = doc.auto_key;
  /* New points get their weights, so they follow the bones like their neighbours. */
  document_weight_new_points(doc);
  /* New points get their handles now, from the setup pose, before anything counts as an edit. */
  const auto init = [](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      if (!layer.is_reference()) {
        drawing_init_handles(layer.drawing);
      }
      self(self, layer.children);
    }
  };
  init(init, doc.layers);
  /* Every point and shape has its id; edits on references become their overrides (their offsets are
   * then keyed below like any edit). */
  const auto ids = [](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      if (!layer.is_reference()) {
        drawing_ensure_ids(layer.drawing);
      }
      self(self, layer.children);
    }
  };
  ids(ids, doc.layers);
  references_capture(doc);
  /* Points added to an animated curve take their neighbours' motion. Frame 0 is the setup pose:
   * bones, points and handles moved there move their setup, and every pose with it. */
  const auto setups = [frame](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      if (!layer.is_reference()) {
        drawing_new_points_follow(layer.drawing, frame);
      }
      if (frame == 0) {
        for (Bone &bone : layer.bones) {
          bone_take_setup(bone);
        }
        if (!layer.is_reference()) {
          drawing_take_setup(layer.drawing);
        }
      }
      self(self, layer.children);
    }
  };
  setups(setups, doc.layers);
  layers_for_each_channel(doc.layers, [&](auto &ch, auto &value) { record(ch, value, frame, auto_key); });
  simulation_in(doc);
  layer_for_each_channel(doc.simulation, [&](auto &ch, auto &value) { record(ch, value, frame, auto_key); });
  layer_for_each_channel(document_camera(doc), [&](auto &ch, auto &value) { record(ch, value, frame, auto_key); });
}

void animation_evaluate(Document &doc, int frame)
{
  layers_for_each_channel(doc.layers, [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  simulation_in(doc);
  layer_for_each_channel(doc.simulation, [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  simulation_out(doc);
  layer_for_each_channel(document_camera(doc), [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  doc.smart_applied = false;
  apply_smart_actions(doc);
  /* References show their originals as they now are. */
  references_build(doc);
  /* Bone dynamics swing from the pose the keys and smart actions give (and set each bone layer's
   * shown frame, for constraints' delays). */
  const int shown = doc.frame_current;
  doc.frame_current = frame;
  dynamics_update(doc);
  doc.frame_current = shown;
  doc.evaluated_frame = frame;
}

void animation_strip_smart(Document &doc)
{
  layers_for_each_channel(doc.layers, [&](auto &ch, auto &value) {
    value -= ch.smart;
    ch.last -= ch.smart;
    ch.smart = {};
  });
  doc.smart_applied = false;
}

void animation_sync(Document &doc)
{
  if (doc.evaluated_frame == doc.frame_current) {
    /* Same frame, but bones may have turned: smart actions and dynamics follow at once (and a
     * setting whose keys just went shows its value). */
    simulation_in(doc);
    animation_refresh_smart(doc);
    /* References follow edits to their originals at once, and edits to them become theirs. */
    references_capture(doc);
    dynamics_update(doc);
    return;
  }
  if (doc.evaluated_frame >= 0) {
    animation_record(doc);
  }
  animation_evaluate(doc, doc.frame_current);
}

/* --- Actions ----------------------------------------------------------------------------------- */

Layer *action_owner(Document &doc, uint64_t layer_id)
{
  Layer *layer = layer_find(doc, layer_id);
  if (!layer) {
    return nullptr;
  }
  if (layer->is_bone_layer()) {
    return layer;
  }
  const Layer *armature = layer_armature(doc, layer_id);
  return armature ? layer_find(doc, armature->id) : nullptr;
}

/* Calls f(channel, value) for every channel of a bone layer and everything inside it. */
template<typename F> static void owner_for_each_channel(Layer &owner, F &&f)
{
  layer_for_each_channel(owner, f);
  layers_for_each_channel(owner.children, f);
}

/* Puts the timeline being edited away under `from` and brings out `to` ("" = mainline). */
template<typename T> static void swap_timeline(Channel<T> &ch, T &value, const std::string &from, const std::string &to)
{
  if (from == to) {
    return;
  }
  /* The setup pose as it is now (frame 0 of what's being edited). */
  const T setup = ch.animated() ? ch.evaluate(0) : value;
  std::vector<Key<T>> away = std::move(ch.keys);
  ch.keys.clear();
  const bool mainline_animated = from.empty() && !away.empty();
  if (!from.empty()) {
    /* An action keeps its keys after frame 0; frame 0 belongs to the setup pose. */
    std::erase_if(away, [](const Key<T> &k) { return k.frame == 0; });
  }
  if (!away.empty()) {
    ch.actions[from] = std::move(away);
  }
  else {
    ch.actions.erase(from);
  }
  if (auto it = ch.actions.find(to); it != ch.actions.end()) {
    ch.keys = std::move(it->second);
    ch.actions.erase(it);
  }
  if (to.empty()) {
    /* Back on the mainline: setup edits made in the action land on its frame 0. */
    if (ch.animated()) {
      if (Key<T> *k = ch.key_at(0)) {
        k->value = setup;
      }
      else {
        ch.set_key(0, setup);
      }
    }
  }
  else if ((ch.animated() || mainline_animated) && !ch.key_at(0)) {
    /* An action starts from the setup pose: frame 0 shows the setup keys of everything animated,
     * as on the mainline. */
    ch.set_key(0, setup);
  }
  value = setup;
}

bool document_set_action(Document &doc, uint64_t owner_id, const std::string &name)
{
  Layer *owner = name.empty() ? nullptr : layer_find(doc, owner_id);
  if (!name.empty() && (!owner || std::find(owner->actions.begin(), owner->actions.end(), name) == owner->actions.end()))
  {
    return false;
  }
  if (doc.action == name && (name.empty() || doc.action_layer == owner_id)) {
    return true;
  }
  const int frame = doc.frame_current;
  animation_record(doc);
  /* Swap on the setup pose, where smart actions add nothing. */
  animation_evaluate(doc, 0);
  /* Leave the action being edited (back to the mainline), then go into the new one. */
  if (!doc.action.empty()) {
    if (Layer *from = layer_find(doc, doc.action_layer)) {
      const std::string from_name = doc.action;
      owner_for_each_channel(*from, [&](auto &ch, auto &value) { swap_timeline(ch, value, from_name, ""); });
    }
  }
  doc.action.clear();
  doc.action_layer = 0;
  if (owner) {
    owner_for_each_channel(*owner, [&](auto &ch, auto &value) { swap_timeline(ch, value, "", name); });
    doc.action = name;
    doc.action_layer = owner->id;
  }
  animation_evaluate(doc, frame);
  return true;
}

bool document_set_action_by_name(Document &doc, const std::string &name)
{
  if (name.empty()) {
    return document_set_action(doc, 0, "");
  }
  if (Layer *owner = action_owner(doc, doc.active_layer)) {
    if (std::find(owner->actions.begin(), owner->actions.end(), name) != owner->actions.end()) {
      return document_set_action(doc, owner->id, name);
    }
  }
  for (const Layer *layer : document_layers_flat(doc)) {
    if (layer->is_bone_layer() && std::find(layer->actions.begin(), layer->actions.end(), name) != layer->actions.end()) {
      return document_set_action(doc, layer->id, name);
    }
  }
  return false;
}

bool document_action_delete(Document &doc, uint64_t owner_id, const std::string &name)
{
  Layer *owner = layer_find(doc, owner_id);
  if (!owner || name.empty() || std::find(owner->actions.begin(), owner->actions.end(), name) == owner->actions.end()) {
    return false;
  }
  if (doc.action == name && doc.action_layer == owner_id) {
    document_set_action(doc, 0, "");
  }
  std::erase(owner->actions, name);
  owner_for_each_channel(*owner, [&](auto &ch, auto & /*value*/) { ch.actions.erase(name); });
  animation_evaluate(doc, doc.frame_current);
  return true;
}

std::string document_action_new(Document &doc, uint64_t owner_id, const std::string &name)
{
  Layer *owner = layer_find(doc, owner_id);
  if (!owner || !owner->is_bone_layer()) {
    return {};
  }
  const std::string base = name.empty() ? "Action" : name;
  std::string unique = base;
  for (int i = 1; std::find(owner->actions.begin(), owner->actions.end(), unique) != owner->actions.end(); i++) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%03d", i);
    unique = base + suffix;
  }
  owner->actions.push_back(unique);
  document_set_action(doc, owner_id, unique);
  return unique;
}

/* --- Smart actions (Moho's smart bones) ------------------------------------------------------- */

const Bone *smart_action_bone(const Layer &owner, const std::string &action)
{
  if (const Bone *bone = bone_find_by_name(owner, action)) {
    return bone;
  }
  if (action.size() > 2 && action.compare(action.size() - 2, 2, " 2") == 0) {
    return bone_find_by_name(owner, action.substr(0, action.size() - 2));
  }
  return nullptr;
}

/* A channel's timeline in action `name` with its setup value at frame 0, sampled at a fractional
 * frame (between the whole frames either side). */
template<typename T> static T smart_sample(const std::vector<Key<T>> &keys, const T &setup, float t)
{
  Channel<T> c;
  c.keys = keys;
  c.set_key(0, setup);
  const int f0 = int(std::floor(t));
  const float frac = t - float(f0);
  const T a = c.evaluate(f0);
  if (frac <= 0.0f) {
    return a;
  }
  const T b = c.evaluate(f0 + 1);
  return a + (b - a) * frac;
}

/* The action frame where the driving bone's keyed angle reaches `angle`: from frame 0 (its setup
 * angle) along its keys; the last frame past the end; 0 on the other side of the setup. */
static float smart_frame(const std::vector<Key<float>> &keys, float setup, float angle)
{
  if (keys.empty()) {
    return 0.0f;
  }
  const float last = float(keys.back().frame);
  const float step = 0.25f;
  float prev = smart_sample(keys, setup, 0.0f);
  for (float s = step; s <= last + 1e-4f; s += step) {
    const float now = smart_sample(keys, setup, s);
    if ((prev - angle) * (now - angle) <= 0.0f && now != prev) {
      return s - step + step * (angle - prev) / (now - prev);
    }
    prev = now;
  }
  const float end = smart_sample(keys, setup, last);
  const bool same_side = (end - setup) * (angle - setup) > 0.0f;
  return same_side && std::abs(angle - setup) > std::abs(end - setup) ? last : 0.0f;
}

static bool has_smart_actions(const Document &doc)
{
  for (const Layer *layer : document_layers_flat(doc)) {
    for (const std::string &name : layer->actions) {
      if (layer->is_bone_layer() && smart_action_bone(*layer, name)) {
        return true;
      }
    }
  }
  return false;
}

void animation_refresh_smart(Document &doc)
{
  if (!doc.smart_applied && !has_smart_actions(doc)) {
    return;
  }
  /* Take off what was added (from the value and its recorded value alike), then work it out again
   * from the bones as they are now. Always, whatever `smart_applied` says: a drag puts the bones
   * back as they were when it began every update, their smart offsets with them (a smart bone
   * turned past its action's end and then back the other way kept the end pose). */
  layers_for_each_channel(doc.layers, [&](auto &ch, auto &value) {
    value -= ch.smart;
    ch.last -= ch.smart;
    ch.smart = {};
  });
  doc.smart_applied = false;
  apply_smart_actions(doc);
}

static void apply_smart_actions(Document &doc)
{
  /* The setup frame (0) is the rig at rest: turning a smart bone there sets its rest angle and plays
   * nothing, as in Moho (and bones don't bend the drawing there either). */
  if (doc.frame_current == 0) {
    return;
  }
  /* Editing a smart action shows its own keys, without any smart action on top. */
  if (!doc.action.empty()) {
    if (const Layer *owner = layer_find(doc, doc.action_layer); owner && smart_action_bone(*owner, doc.action)) {
      return;
    }
  }
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer() || flat->actions.empty()) {
      continue;
    }
    Layer &owner = *layer_find(doc, flat->id);
    /* Every driving bone's angle picks its frame first, before any action moves bones. */
    std::vector<std::pair<std::string, float>> frames;
    for (const std::string &name : owner.actions) {
      if (name == doc.action && owner.id == doc.action_layer) {
        continue;
      }
      const Bone *bone = smart_action_bone(owner, name);
      if (!bone) {
        continue;
      }
      const auto it = bone->angle_keys.actions.find(name);
      if (it == bone->angle_keys.actions.end()) {
        continue;
      }
      /* The driving bone's keys are its Angle (the turn from its setup pose), so its action starts at
       * 0 and the bone's turn now picks the frame; turning an unkeyed bone plays it at once. */
      const float t = smart_frame(it->second, 0.0f, bone->angle - bone->setup_angle);
      if (t > 0.0f) {
        frames.push_back({name, t});
      }
    }
    for (const auto &[name, t] : frames) {
      const Channel<float> *driver = &smart_action_bone(owner, name)->angle_keys;
      owner_for_each_channel(owner, [&](auto &ch, auto &value) {
        if (static_cast<const void *>(&ch) == static_cast<const void *>(driver)) {
          return;
        }
        const auto it = ch.actions.find(name);
        if (it == ch.actions.end()) {
          return;
        }
        /* Unkeyed, the setup is the value as last recorded, not as it is now: an edit not yet
         * recorded (a bone being dragged) stays on top instead of being taken into the offset. */
        const auto setup = ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value) - ch.smart;
        const auto delta = smart_sample(it->second, setup, t) - setup;
        /* The value and what it was last recorded as both move, so an edit stays an edit. */
        value += delta;
        ch.last += delta;
        ch.smart += delta;
        doc.smart_applied = true;
      });
    }
  }
}

std::vector<std::pair<Layer *, std::string>> layers_keyed_at(const std::vector<Layer *> &layers,
                                                             const KeyFilter &filter, int frame)
{
  std::vector<std::pair<Layer *, std::string>> keyed;
  for (Layer *layer : layers) {
    for (const ChannelRef &ref : layer_filtered_channels(*layer, filter)) {
      if (ref.scalar ? ref.scalar->key_at(frame) != nullptr : ref.vector->key_at(frame) != nullptr) {
        keyed.push_back({layer, ref.path});
      }
    }
  }
  return keyed;
}

std::vector<ChannelRef> layer_channels(Layer &layer, bool drawing)
{
  std::vector<ChannelRef> refs;
  const auto add = [&](std::string path, auto &ch, auto &value) {
    ChannelRef ref;
    ref.path = std::move(path);
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, float>) {
      ref.scalar = &ch;
      ref.scalar_value = &value;
    }
    else {
      ref.vector = &ch;
      ref.vector_value = &value;
    }
    refs.push_back(std::move(ref));
  };
  add("position", layer.position_keys, layer.position);
  add("rotation", layer.rotation_keys, layer.rotation);
  add("scale", layer.scale_keys, layer.scale);
  add("opacity", layer.opacity_keys, layer.opacity);
  add("visible", layer.visible_keys, layer.visible_value);
  add("draw_order", layer.draw_order_keys, layer.draw_order);
  for (Bone &bone : layer.bones) {
    const std::string base = "bones[\"" + bone.name + "\"].";
    /* Its keys hold Transform and Angle (see layer_for_each_channel); a change made through them
     * goes back with bone_values_from_changes. */
    bone_changes_from_values(bone);
    add(base + "position", bone.position_keys, bone.transform);
    add(base + "angle", bone.angle_keys, bone.turn);
    add(base + "scale", bone.scale_keys, bone.scale);
    for (size_t i = 0; i < bone.constraints.size(); i++) {
      BoneConstraint &c = bone.constraints[i];
      const std::string cb = base + "constraints[" + std::to_string(i) + "].";
      add(cb + "influence", c.influence_keys, c.influence);
      add(cb + "weight", c.weight_keys, c.weight);
      add(cb + "wind_catch", c.wind_catch_keys, c.wind_catch);
      add(cb + "gravity", c.gravity_keys, c.gravity);
      add(cb + "wind", c.wind_keys, c.wind);
      add(cb + "wind_direction", c.wind_direction_keys, c.wind_direction);
      add(cb + "wind_gusts", c.wind_gusts_keys, c.wind_gusts);
      add(cb + "wind_gust_speed", c.wind_gust_speed_keys, c.wind_gust_speed);
      add(cb + "wind_turbulence", c.wind_turbulence_keys, c.wind_turbulence);
      for (const auto &[name, s] : {std::pair<const char *, BoneSpring *>{"angle_spring.", &c.angle_spring},
                                    {"position_spring.", &c.position_spring},
                                    {"scale_spring.", &c.scale_spring}})
      {
        add(cb + name + "settle_speed", s->settle_speed_keys, s->settle_speed);
        add(cb + name + "bounce", s->bounce_keys, s->bounce);
        add(cb + name + "inertia", s->inertia_keys, s->inertia);
        add(cb + name + "stretch", s->stretch_keys, s->stretch);
        add(cb + name + "limit", s->limit_keys, s->limit);
      }
    }
  }
  for (CustomChannel &c : layer.custom_channels) {
    add(c.path, c.keys, c.value);
  }
  if (!drawing) {
    return refs;
  }
  if (layer.is_reference()) {
    /* A reference's points move by its own offsets (by the point's place in its drawing). */
    for (size_t i = 0; i < layer.drawing.points.size(); i++) {
      if (auto it = layer.reference.points.find(layer.drawing.points[i].id); it != layer.reference.points.end()) {
        add("points[" + std::to_string(i) + "].offset", it->second.offset_keys, it->second.offset);
      }
    }
    /* And its handles' offsets, by the handle's place in its drawing. */
    const VectorDrawing &d = layer.drawing;
    for (size_t c = 0; c < d.curves.size(); c++) {
      const Curve &curve = d.curves[c];
      const int n = int(curve.points.size());
      for (int i = 0; i < n; i++) {
        for (const bool in : {true, false}) {
          const int j = in ? (i > 0 ? i - 1 : (curve.closed ? n - 1 : -1)) : (i + 1 < n ? i + 1 : (curve.closed ? 0 : -1));
          const ReferenceHandleKey key{d.points[curve.points[i].point].id,
                                       j >= 0 && j != i ? d.points[curve.points[j].point].id : 0, in};
          if (auto it = layer.reference.handles.find(key); it != layer.reference.handles.end()) {
            add("curves[" + std::to_string(c) + "].points[" + std::to_string(i) + "]." +
                    (in ? "handle_in_offset" : "handle_out_offset"),
                it->second.offset_keys, it->second.offset);
          }
        }
      }
    }
    return refs;
  }
  /* Their keys hold the Transform and the handles' Angle and Length (see layer_for_each_channel); a
   * change made through them goes back with drawing_values_from_changes. */
  drawing_changes_from_values(layer.drawing);
  for (size_t i = 0; i < layer.drawing.points.size(); i++) {
    MeshPoint &p = layer.drawing.points[i];
    const std::string base = "points[" + std::to_string(i) + "].";
    add(base + "pos", p.pos_keys, p.transform);
    add(base + "radius", p.radius_keys, p.radius);
  }
  for (size_t c = 0; c < layer.drawing.curves.size(); c++) {
    Curve &curve = layer.drawing.curves[c];
    for (size_t i = 0; i < curve.points.size(); i++) {
      CurvePoint &cp = curve.points[i];
      const std::string base = "curves[" + std::to_string(c) + "].points[" + std::to_string(i) + "].";
      add(base + "curvature", cp.curvature_keys, cp.curvature);
      add(base + "handle_in", cp.handle_in_keys, cp.change_in);
      add(base + "handle_out", cp.handle_out_keys, cp.change_out);
    }
  }
  return refs;
}

bool layer_channel(Layer &layer, const std::string &path, ChannelRef *r_ref)
{
  const bool drawing = path.find('[') != std::string::npos;
  for (ChannelRef &ref : layer_channels(layer, drawing)) {
    if (ref.path == path) {
      *r_ref = ref;
      return true;
    }
  }
  return false;
}

int layer_insert_keys(Layer &layer, int frame)
{
  int count = 0;
  layer_for_each_channel(layer, [&](auto &ch, auto &value) {
    if (ch.keyed_by_hand && !ch.animated()) {
      return;
    }
    /* Keys hold the value without what smart actions add. */
    const auto base = value - ch.smart;
    if (!ch.animated() && frame != 0) {
      ch.set_key(0, ch.has_last ? ch.last - ch.smart : base);
    }
    ch.set_key(frame, base);
    ch.last = value;
    ch.has_last = true;
    count++;
  });
  return count;
}

/* As in Blender, a property with no keys left isn't animated: once only the setup key (frame 0)
 * is left, it goes too, and the value stays what frame 0 had. */
template<typename T> static void drop_lone_setup_key(Channel<T> &ch, T &value)
{
  if (ch.keys.size() == 1 && ch.keys.front().frame == 0) {
    value = ch.keys.front().value + ch.smart;
    ch.last = value;
    ch.keys.clear();
  }
}

int layer_toggle_keys(Layer &layer, const std::vector<std::string> &paths, int frame)
{
  std::vector<ChannelRef> refs;
  for (const std::string &path : paths) {
    ChannelRef ref;
    if (layer_channel(layer, path, &ref)) {
      refs.push_back(ref);
    }
  }
  bool keyed = false;
  for (const ChannelRef &ref : refs) {
    keyed |= ref.scalar ? ref.scalar->key_at(frame) != nullptr : ref.vector->key_at(frame) != nullptr;
  }
  int count = 0;
  const auto toggle = [&](auto &ch, auto &value) {
    if (keyed) {
      /* Frame 0 is the setup key: it goes only with the last of the keys. */
      if (frame == 0 && ch.keys.size() > 1) {
        return;
      }
      count += ch.remove_key(frame) ? 1 : 0;
      drop_lone_setup_key(ch, value);
      return;
    }
    const auto base = value - ch.smart;
    if (!ch.animated() && frame != 0) {
      /* The setup key takes the value from before an edit not yet keyed. */
      ch.set_key(0, ch.has_last ? ch.last - ch.smart : base);
    }
    ch.set_key(frame, base);
    ch.last = value;
    ch.has_last = true;
    count++;
  };
  for (const ChannelRef &ref : refs) {
    if (ref.scalar) {
      toggle(*ref.scalar, *ref.scalar_value);
    }
    else {
      toggle(*ref.vector, *ref.vector_value);
    }
  }
  for (Bone &bone : layer.bones) {
    bone_values_from_changes(bone);
  }
  if (!layer.is_reference()) {
    drawing_values_from_changes(layer.drawing);
  }
  return keyed ? -count : count;
}

int layer_key_state(Layer &layer, const std::vector<std::string> &paths, int frame)
{
  int state = 0;
  for (const std::string &path : paths) {
    ChannelRef ref;
    if (!layer_channel(layer, path, &ref)) {
      continue;
    }
    const bool animated = ref.scalar ? ref.scalar->animated() : ref.vector->animated();
    const bool here = ref.scalar ? ref.scalar->key_at(frame) != nullptr : ref.vector->key_at(frame) != nullptr;
    state = std::max(state, here ? 2 : animated ? 1 : 0);
  }
  return state;
}

int layer_delete_keys(Layer &layer, int frame)
{
  int count = 0;
  layer_for_each_channel(layer, [&](auto &ch, auto &value) {
    count += ch.remove_key(frame) ? 1 : 0;
    drop_lone_setup_key(ch, value);
  });
  return count;
}

std::vector<int> layer_key_frames(const Layer &layer)
{
  std::set<int> frames;
  layer_for_each_channel(const_cast<Layer &>(layer), [&](auto &ch, auto & /*value*/) {
    for (const auto &key : ch.keys) {
      frames.insert(key.frame);
    }
  });
  return {frames.begin(), frames.end()};
}

bool layer_channel_pinned(Layer &layer, const std::string &path)
{
  ChannelRef ref;
  return layer_channel(layer, path, &ref) && (ref.scalar ? ref.scalar->pinned : ref.vector->pinned);
}

int layer_pin_channels(Layer &layer, const std::vector<std::string> &paths, bool pinned)
{
  int changed = 0;
  for (const std::string &path : paths) {
    ChannelRef ref;
    if (!layer_channel(layer, path, &ref)) {
      continue;
    }
    bool &flag = ref.scalar ? ref.scalar->pinned : ref.vector->pinned;
    changed += flag != pinned ? 1 : 0;
    flag = pinned;
  }
  return changed;
}

int document_clear_pins(Document &doc)
{
  int cleared = 0;
  const auto unpin = [&](Layer &layer) {
    layer_for_each_channel(layer, [&](auto &ch, auto & /*value*/) {
      cleared += ch.pinned ? 1 : 0;
      ch.pinned = false;
    });
  };
  for (const Layer *layer : document_layers_flat(doc)) {
    unpin(*layer_find(doc, layer->id));
  }
  unpin(doc.simulation);
  unpin(doc.camera);
  return cleared;
}

std::string channel_label(Layer &layer, const std::string &path, bool owner)
{
  static const std::pair<const char *, const char *> NAMES[] = {
      {"position", "Position"},
      {"rotation", "Rotation"},
      {"scale", "Scale"},
      {"opacity", "Opacity"},
      {"visible", "Visibility"},
      {"draw_order", "Draw Order"},
      {"angle", "Angle"},
      {"influence", "Influence"},
      {"gravity", "Gravity"},
      {"wind", "Wind"},
      {"wind_catch", "Wind Catch"},
      {"wind_direction", "Wind Direction"},
      {"wind_gusts", "Gusts"},
      {"wind_gust_speed", "Gust Speed"},
      {"wind_turbulence", "Turbulence"},
      {"settle_speed", "Settle Speed"},
      {"bounce", "Bounce"},
      {"weight", "Weight"},
      {"inertia", "Inertia"},
      {"stretch", "Stretch"},
      {"zoom", "Zoom"},
  };
  const auto name_of = [](const std::string &id) -> std::string {
    for (const auto &[key, name] : NAMES) {
      if (id == key) {
        return name;
      }
    }
    return id;
  };
  if (path.rfind("bones[\"", 0) != 0) {
    /* The camera turns about its centre: its Roll. */
    const std::string name = layer.id == CAMERA_LAYER_ID && path == "rotation" ? std::string("Roll") : name_of(path);
    return owner ? layer.name + ": " + name : name;
  }
  const size_t end = path.find("\"]");
  if (end == std::string::npos) {
    return path;
  }
  const std::string bone_name = path.substr(7, end - 7);
  std::string rest = path.substr(std::min(end + 3, path.size()));
  std::string label;
  if (rest.rfind("constraints[", 0) == 0) {
    /* "IK: Influence", "Dynamics: Angle Bounce", "Dynamics: Max Swing". */
    const int i = std::atoi(rest.c_str() + 12);
    const Bone *bone = bone_find_by_name(layer, bone_name);
    const std::string constraint = bone && i >= 0 && i < int(bone->constraints.size()) ? bone->constraints[i].name
                                                                                        : "Constraint";
    rest = rest.substr(std::min(rest.find('.') + 1, rest.size()));
    std::string setting;
    for (const auto &[prefix, channel, limit] :
         {std::tuple{"angle_spring.", "Angle", "Max Swing"}, std::tuple{"position_spring.", "Position", "Max Distance"},
          std::tuple{"scale_spring.", "Squash & Stretch", "Max Stretch"}})
    {
      if (rest.rfind(prefix, 0) == 0) {
        const std::string what = rest.substr(std::string(prefix).size());
        setting = what == "limit" ? std::string(limit) : std::string(channel) + " " + name_of(what);
      }
    }
    label = constraint + ": " + (setting.empty() ? name_of(rest) : setting);
  }
  else {
    /* A bone's position is its Transform, as the Bone panel names it (the change from setup). */
    label = rest == "position" ? std::string("Transform") : name_of(rest);
  }
  return owner ? bone_name + ": " + label : label;
}

KeyKind channel_kind(const std::string &path)
{
  if (simulation_path(path)) {
    return KEY_SIMULATION;
  }
  if (path.rfind("bones[", 0) == 0 && path.find(".constraints[") != std::string::npos) {
    return KEY_CONSTRAINTS;
  }
  if (path.rfind("points[", 0) == 0 || path.rfind("curves[", 0) == 0) {
    return KEY_POINTS;
  }
  if (path.rfind("bones[", 0) == 0) {
    return path.size() > 6 && path.compare(path.size() - 6, 6, ".angle") == 0 ? KEY_BONE_ANGLE : KEY_BONE_TRANSFORM;
  }
  if (path == "rotation") {
    return KEY_LAYER_ANGLE;
  }
  if (path == "opacity") {
    return KEY_OPACITY;
  }
  if (path == "visible") {
    return KEY_VISIBILITY;
  }
  if (path == "draw_order") {
    return KEY_DRAW_ORDER;
  }
  return KEY_LAYER_TRANSFORM;
}

/* Whether the thing a channel animates is selected: the point (or the curve point's point), or
 * the bone; every channel of the camera while it is picked. Other layer channels have no thing of
 * their own. */
static bool channel_thing_selected(const Layer &layer, const std::string &path)
{
  if (layer.id == CAMERA_LAYER_ID) {
    return layer.selected;
  }
  const auto number_after = [&](size_t at) { return std::atoi(path.c_str() + at); };
  if (path.rfind("points[", 0) == 0) {
    const int i = number_after(7);
    return i >= 0 && i < int(layer.drawing.points.size()) && layer.drawing.points[i].selected;
  }
  if (path.rfind("curves[", 0) == 0) {
    const int c = number_after(7);
    const size_t at = path.find("points[");
    if (c < 0 || c >= int(layer.drawing.curves.size()) || at == std::string::npos) {
      return false;
    }
    const int k = number_after(at + 7);
    const Curve &curve = layer.drawing.curves[c];
    return k >= 0 && k < int(curve.points.size()) && layer.drawing.points[curve.points[k].point].selected;
  }
  if (path.rfind("bones[\"", 0) == 0) {
    const size_t end = path.find("\"]");
    const Bone *bone = end == std::string::npos ? nullptr : bone_find_by_name(layer, path.substr(7, end - 7));
    return bone && (bone->selected || bone->select_head || bone->select_tail);
  }
  return false;
}

bool key_filter_path_matches(const KeyFilter &filter, const std::string &path)
{
  if (filter.path.empty()) {
    return true;
  }
  if (filter.path.back() == '.') {
    return path.rfind(filter.path, 0) == 0;
  }
  return path == filter.path;
}

std::vector<ChannelRef> layer_filtered_channels(Layer &layer, const KeyFilter &filter)
{
  std::vector<ChannelRef> refs;
  /* A drawing's points and curves are only needed when their kind is asked for. */
  const bool drawing = (filter.kinds & KEY_POINTS) &&
                       (filter.path.empty() || filter.path.rfind("points[", 0) == 0 ||
                        filter.path.rfind("curves[", 0) == 0);
  for (ChannelRef &ref : layer_channels(layer, drawing)) {
    const KeyKind kind = channel_kind(ref.path);
    if (!(filter.kinds & kind) || !key_filter_path_matches(filter, ref.path)) {
      continue;
    }
    if (filter.pinned_only && !(ref.scalar ? ref.scalar->pinned : ref.vector->pinned)) {
      continue;
    }
    if (filter.selected_only && !channel_thing_selected(layer, ref.path)) {
      continue;
    }
    refs.push_back(std::move(ref));
  }
  return refs;
}

/* Calls f(channel) for every channel of `layers` that passes the filter. */
template<typename F> static void filtered_for_each(const std::vector<Layer *> &layers, const KeyFilter &filter, F &&f)
{
  for (Layer *layer : layers) {
    if (filter.everything()) {
      layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) { f(ch); });
      continue;
    }
    for (const ChannelRef &ref : layer_filtered_channels(*layer, filter)) {
      if (ref.scalar) {
        f(*ref.scalar);
      }
      else {
        f(*ref.vector);
      }
    }
  }
}

KeyShape interp_key_shape(Interp interp)
{
  switch (interp) {
    case Interp::Smooth:
      return KEY_SHAPE_SMOOTH;
    case Interp::Linear:
      return KEY_SHAPE_LINEAR;
    case Interp::Step:
      return KEY_SHAPE_STEP;
    case Interp::EaseIn:
    case Interp::EaseOut:
    case Interp::EaseInOut:
      return KEY_SHAPE_EASE;
    case Interp::Back:
    case Interp::Bounce:
    case Interp::Elastic:
      return KEY_SHAPE_BOUNCY;
    case Interp::Noisy:
    case Interp::Cycle:
    case Interp::Bezier:
    case Interp::Pose:
      break;
  }
  return KEY_SHAPE_SPECIAL;
}

std::map<int, KeyFrameSummary> layers_key_summary(const std::vector<Layer *> &layers, const KeyFilter &filter)
{
  std::map<int, KeyFrameSummary> summary;
  filtered_for_each(layers, filter, [&](auto &ch) {
    for (const auto &key : ch.keys) {
      KeyFrameSummary &s = summary[key.frame];
      s.keys++;
      s.selected += key.selected ? 1 : 0;
      if (!ch.constant) {
        const KeyShape shape = interp_key_shape(key.interp);
        s.shape = s.shape_set && s.shape != shape ? KEY_SHAPE_MIXED : shape;
        s.shape_set = true;
      }
    }
  });
  return summary;
}

SelectedKeys layers_selected_keys(const std::vector<Layer *> &layers)
{
  SelectedKeys r;
  bool any_constant = false;
  KeySettings constant;
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      for (const auto &key : ch.keys) {
        if (!key.selected) {
          continue;
        }
        if (ch.constant) {
          /* Visibility: always Step; it only speaks when nothing else is selected. */
          any_constant = true;
          constant = key_settings(key);
          continue;
        }
        const KeySettings s = key_settings(key);
        if (r.count == 0) {
          r.first = s;
        }
        else {
          r.mixed |= s.interp != r.first.interp;
          r.mixed_ease |= s.ease != r.first.ease;
        }
        r.count++;
      }
    });
  }
  if (r.count == 0 && any_constant) {
    r.first = constant;
    r.count = 1;
  }
  return r;
}

int layers_change_selected_keys(const std::vector<Layer *> &layers, const std::function<void(KeySettings &)> &change)
{
  int count = 0;
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      for (auto &key : ch.keys) {
        if (!key.selected) {
          continue;
        }
        KeySettings s = key_settings(key);
        change(s);
        s.hold = std::max(0, s.hold);
        s.interval = std::max(1, s.interval);
        s.cycle_back = std::max(0, s.cycle_back);
        s.elastic_period = std::max(0.02f, s.elastic_period);
        if (ch.constant) {
          s.interp = Interp::Step;
        }
        key_settings_apply(key, s);
        count++;
      }
    });
  }
  return count;
}

void layers_select_keys(const std::vector<Layer *> &layers, int first, int last, bool select, const KeyFilter &filter)
{
  filtered_for_each(layers, filter, [&](auto &ch) {
    for (auto &key : ch.keys) {
      if (key.frame >= first && key.frame <= last) {
        key.selected = select;
      }
    }
  });
}


void layers_deselect_keys(const std::vector<Layer *> &layers)
{
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      for (auto &key : ch.keys) {
        key.selected = false;
      }
    });
  }
}

bool layers_any_key_selected(const std::vector<Layer *> &layers)
{
  for (const auto &[frame, s] : layers_key_summary(layers)) {
    if (s.selected > 0) {
      return true;
    }
  }
  return false;
}

void layers_move_selected_keys(const std::vector<Layer *> &layers, const std::function<int(int)> &to)
{
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      using KeyT = std::decay_t<decltype(ch.keys.front())>;
      std::vector<KeyT> moved, kept;
      for (const KeyT &key : ch.keys) {
        if (key.selected && key.frame != 0) {
          KeyT k = key;
          k.frame = std::max(1, to(key.frame));
          moved.push_back(k);
        }
        else {
          kept.push_back(key);
        }
      }
      if (moved.empty()) {
        return;
      }
      /* Keys moved onto the same frame: the last one wins; then they replace what was there. */
      std::map<int, KeyT> landed;
      for (const KeyT &k : moved) {
        landed[k.frame] = k;
      }
      std::erase_if(kept, [&](const KeyT &other) { return landed.count(other.frame) > 0; });
      for (const auto &[frame, k] : landed) {
        kept.push_back(k);
      }
      std::sort(kept.begin(), kept.end(), [](const KeyT &a, const KeyT &b) { return a.frame < b.frame; });
      ch.keys = std::move(kept);
    });
  }
}

int layers_delete_selected_keys(const std::vector<Layer *> &layers)
{
  int removed = 0;
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto &value) {
      const size_t before = ch.keys.size();
      std::erase_if(ch.keys, [](const auto &key) { return key.selected && key.frame != 0; });
      removed += int(before - ch.keys.size());
      if (ch.keys.size() != before) {
        drop_lone_setup_key(ch, value);
      }
    });
  }
  return removed;
}

void layers_duplicate_selected_keys(const std::vector<Layer *> &layers, int offset)
{
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      using KeyT = std::decay_t<decltype(ch.keys.front())>;
      std::vector<KeyT> copies;
      for (KeyT &key : ch.keys) {
        if (key.selected) {
          KeyT copy = key;
          copy.frame = std::max(1, key.frame + offset);
          copies.push_back(copy);
          key.selected = false;
        }
      }
      for (const KeyT &copy : copies) {
        std::erase_if(ch.keys, [&](const KeyT &k) { return k.frame == copy.frame; });
      }
      for (const KeyT &copy : copies) {
        if (!ch.key_at(copy.frame)) {
          ch.keys.push_back(copy);
        }
      }
      std::sort(ch.keys.begin(), ch.keys.end(), [](const KeyT &a, const KeyT &b) { return a.frame < b.frame; });
    });
  }
}

std::vector<std::pair<int, int>> layers_hold_spans(const std::vector<Layer *> &layers, const KeyFilter &filter)
{
  std::vector<int> frames;
  for (const auto &[frame, s] : layers_key_summary(layers, filter)) {
    frames.push_back(frame);
  }
  std::vector<std::pair<int, int>> spans;
  for (size_t i = 0; i + 1 < frames.size(); i++) {
    const int a = frames[i], b = frames[i + 1];
    bool holds = true, any = false;
    filtered_for_each(layers, filter, [&](auto &ch) {
      const auto *ka = ch.key_at(a);
      const auto *kb = ch.key_at(b);
      if (!holds || (!ka && !kb)) {
        return;
      }
      if (!ka || !kb || !(ka->value == kb->value)) {
        holds = false;
        return;
      }
      any = true;
    });
    if (holds && any) {
      spans.push_back({a, b});
    }
  }
  return spans;
}

/* --- Copy and paste keys ----------------------------------------------------------------------- */

namespace {

struct ClipboardChannel {
  uint64_t layer = 0;
  std::string layer_name;
  std::string path;
  std::vector<Key<float>> scalar;
  std::vector<Key<float2>> vector;
};

struct KeyClipboard {
  std::vector<ClipboardChannel> channels;
  int first_frame = 0;
};

}  // namespace

static KeyClipboard &key_clipboard()
{
  static KeyClipboard clipboard;
  return clipboard;
}

bool key_clipboard_empty()
{
  return key_clipboard().channels.empty();
}

int layers_copy_selected_keys(const std::vector<Layer *> &layers)
{
  KeyClipboard copied;
  int count = 0;
  int first = INT32_MAX;
  for (Layer *layer : layers) {
    for (const ChannelRef &ref : layer_channels(*layer)) {
      ClipboardChannel c{layer->id, layer->name, ref.path, {}, {}};
      const auto take = [&](const auto &channel, auto &into) {
        for (const auto &key : channel.keys) {
          if (key.selected) {
            into.push_back(key);
            first = std::min(first, key.frame);
            count++;
          }
        }
      };
      if (ref.scalar) {
        take(*ref.scalar, c.scalar);
      }
      else {
        take(*ref.vector, c.vector);
      }
      if (!c.scalar.empty() || !c.vector.empty()) {
        copied.channels.push_back(std::move(c));
      }
    }
  }
  if (count == 0) {
    return 0;
  }
  copied.first_frame = first;
  key_clipboard() = std::move(copied);
  return count;
}

int layers_paste_keys(const std::vector<Layer *> &layers, int frame)
{
  const KeyClipboard &clipboard = key_clipboard();
  if (clipboard.channels.empty() || layers.empty()) {
    return 0;
  }
  std::set<uint64_t> sources;
  for (const ClipboardChannel &c : clipboard.channels) {
    sources.insert(c.layer);
  }
  /* Where a copied channel goes: the same layer, else one with the same name, else (keys copied
   * from a single layer) the first layer that has the channel. */
  const auto target = [&](const ClipboardChannel &c, ChannelRef *r_ref) -> bool {
    for (Layer *layer : layers) {
      if (layer->id == c.layer && layer_channel(*layer, c.path, r_ref)) {
        return true;
      }
    }
    for (Layer *layer : layers) {
      if (layer->name == c.layer_name && layer_channel(*layer, c.path, r_ref)) {
        return true;
      }
    }
    if (sources.size() == 1) {
      for (Layer *layer : layers) {
        if (layer_channel(*layer, c.path, r_ref)) {
          return true;
        }
      }
    }
    return false;
  };
  layers_deselect_keys(layers);
  const int offset = frame - clipboard.first_frame;
  int count = 0;
  for (const ClipboardChannel &c : clipboard.channels) {
    ChannelRef ref;
    if (!target(c, &ref)) {
      continue;
    }
    const auto put = [&](auto &channel, const auto &keys) {
      for (const auto &key : keys) {
        const int at = key.frame + offset;
        if (at < 0) {
          continue;
        }
        auto &placed = channel.set_key(at, key.value);
        placed = key;
        placed.frame = at;
        placed.selected = true;
        count++;
      }
    };
    if (ref.scalar && !c.scalar.empty()) {
      put(*ref.scalar, c.scalar);
    }
    else if (ref.vector && !c.vector.empty()) {
      put(*ref.vector, c.vector);
    }
  }
  return count;
}

}  // namespace oanim
