#include "anim/animation.hh"

#include "anim/drivers.hh"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <functional>
#include <string_view>
#include <cstdlib>
#include <cmath>
#include <climits>
#include <memory>
#include <cstdio>
#include <set>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <unordered_set>

#include "anim/controller.hh"
#include "anim/deform.hh"
#include "core/log.hh"
#include "core/parallel.hh"
#include "anim/dynamics.hh"
#include "anim/follow_point.hh"
#include "anim/shape_limit.hh"
#include "anim/reference.hh"
#include "anim/switch.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/document.hh"

namespace oanim {

/* Calls f(channel, value) for every channel of a layer (not its children). Without `sync` the
 * values aren't worked out (the points' and bones' changes from their setup): for reading keys only,
 * which the timeline does every frame. */
template<bool sync = true, typename F> static void layer_for_each_channel(Layer &layer, F &&f)
{
  for (CustomChannel &c : layer.custom_channels) {
    f(c.keys, c.value);
  }
  for (ActionPlay &a : layer.action_plays) {
    f(a.frame_keys, a.frame);
  }
  /* Visibility is a stepped number beside the plain `visible` flag. */
  if constexpr (sync) {
    layer.visible_value = layer.visible ? 1.0f : 0.0f;
  }
  f(layer.visible_keys, layer.visible_value);
  if constexpr (sync) {
    layer.visible = layer.visible_value > 0.5f;
    /* Keys pasted or opened from a file hold too. */
    for (Key<float> &k : layer.visible_keys.keys) {
      k.interp = Interp::Step;
    }
  }
  f(layer.draw_order_keys, layer.draw_order);
  if constexpr (sync) {
    for (Key<float> &k : layer.draw_order_keys.keys) {
      k.interp = Interp::Step;
    }
  }
  if (layer.is_switch()) {
    f(layer.switch_keys, layer.switch_value);
    if constexpr (sync) {
      for (Key<float> &k : layer.switch_keys.keys) {
        k.interp = Interp::Step;
      }
    }
  }
  f(layer.position_keys, layer.position);
  f(layer.rotation_keys, layer.rotation);
  f(layer.scale_keys, layer.scale);
  f(layer.shear_keys, layer.shear);
  f(layer.opacity_keys, layer.opacity);
  /* Before any bone's channels are touched: taking action bone actions off goes bone by bone, and a limit read halfway
   * would
   * see the bone's parent without what they add, the shape with it. */
  if constexpr (sync) {
    for (Bone &bone : layer.bones) {
      bone_shape_limit_affect(layer, bone);
    }
  }
  for (Bone &bone : layer.bones) {
    /* A bone's keys are its Transform and Angle: the change from its setup pose. */
    if constexpr (sync) {
      bone_changes_from_values(bone);
    }
    f(bone.position_keys, bone.transform);
    f(bone.angle_keys, bone.turn);
    if constexpr (sync) {
      bone_values_from_changes(bone);
    }
    f(bone.scale_keys, bone.scale);
    f(bone.depth_keys, bone.depth);
    for (BoneConstraint &c : bone.constraints) {
      f(c.influence_keys, c.influence);
      f(c.follow_shift_keys, c.follow_shift);
      f(c.follow_shift2_keys, c.follow_shift2);
      f(c.follow_shift3_keys, c.follow_shift3);
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
        f(s->squash_limit_keys, s->squash_limit);
      }
    }
  }
  for (LayerMask &m : layer.masks) {
    f(m.line_reach_keys, m.line_reach);
  }
  for (Modifier &m : layer.modifiers) {
    if (m.type == ModifierType::Boolean) {
      f(m.merge_smoothing_keys, m.merge_smoothing);
    }
    if (m.type == ModifierType::Bevel) {
      f(m.bevel_amount_keys, m.bevel_amount);
    }
    if (modifier_is_deform(m.type)) {
      f(m.deform_strength_keys, m.deform_strength);
    }
    if (m.type == ModifierType::FollowDeform) {
      f(m.deform_shift_keys, m.deform_shift);
      f(m.deform_shift2_keys, m.deform_shift2);
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
  if constexpr (sync) {
    drawing_changes_from_values(layer.drawing);
  }
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
  if constexpr (sync) {
    drawing_values_from_changes(layer.drawing);
  }
}

template<typename F> static void layers_for_each_channel(std::vector<Layer> &layers, F &&f)
{
  for (Layer &layer : layers) {
    layer_for_each_channel(layer, f);
    layers_for_each_channel(layer.children, f);
  }
}

/* While action bone actions are worked out again (animation_refresh_action_bones), the layers with any action bone
 * action
 * data, found once: the rest are passed over without a look. */
static const std::unordered_set<const Layer *> *g_action_bone_layers = nullptr;

/* As layers_for_each_channel, but only on the layers having a channel `wanted` says yes to. Looking costs
 * little; the walk itself turns every drawing's points into their changes from the setup pose and back,
 * which on a big rig is most of what action bone actions cost each time an action bone turns. */
template<typename P, typename F> static void layers_for_each_channel_if(std::vector<Layer> &layers, P &&wanted, F &&f)
{
  for (Layer &layer : layers) {
    if (g_action_bone_layers && !g_action_bone_layers->contains(&layer)) {
      layers_for_each_channel_if(layer.children, wanted, f);
      continue;
    }
    bool any = false;
    layer_for_each_channel<false>(layer, [&](auto &ch, auto & /*value*/) { any = any || wanted(ch); });
    if (any) {
      layer_for_each_channel(layer, f);
    }
    layers_for_each_channel_if(layer.children, wanted, f);
  }
}

/* A channel with something an action bone action added to take off (or marked as played into). */
static constexpr auto action_bones_added = [](const auto &ch) {
  return ch.action_bone_live || !(ch.from_action_bones == std::decay_t<decltype(ch.from_action_bones)>{});
};

/* The layers with action bone action data: something added to take off, or keys in an action. An action bone being
 * turned has its actions worked out again every update, and a big rig's thousands of other layers have
 * nothing to do with them. */
static void find_action_bone_layers(std::vector<Layer> &layers, std::unordered_set<const Layer *> &r_found)
{
  for (Layer &layer : layers) {
    bool any = false;
    layer_for_each_channel<false>(layer, [&](auto &ch, auto & /*value*/) {
      any = any || action_bones_added(ch) || !ch.actions.empty();
    });
    if (any) {
      r_found.insert(&layer);
    }
    find_action_bone_layers(layer.children, r_found);
  }
}

static void find_action_bone_layers(Document &doc, std::unordered_set<const Layer *> &r_found)
{
  find_action_bone_layers(doc.layers, r_found);
}

/* While a bone drag runs (animation_action_bone_drag_begin): its document and the layers found as it began. The
 * drag works action bone actions out again itself every update, its layers staying where they are. */
static const Document *g_drag_doc = nullptr;
static std::unordered_set<const Layer *> g_drag_layers;

/* Drawings a switch hides that come out the same on every frame (still): no keys, no action data, nothing action bone
 * actions added, no edit waiting, nothing in them worked out from elsewhere (bones, references, drivers, media,
 * switches). Found after a frame is evaluated, and kept while the edits the app can see coming (its stamp) and the
 * keys stay as they were: a frame change of the main loop's (playing) then neither records nor evaluates them,
 * and one shows on any frame as it is. On the 360 rig most of the document is such drawings. */
struct StillDrawings {
  const Document *doc = nullptr;
  uint64_t stamp = 0;
  bool valid = false;
  std::unordered_set<uint64_t> ids;
};
static StillDrawings g_still;
/* While a frame change of the main loop's is recorded and evaluated: the still drawings to leave as they are. */
static const std::unordered_set<uint64_t> *g_still_skip = nullptr;

/* A still drawing hidden now (in renders and the viewport alike): left as it is. */
static bool still_skipped(const Layer &layer)
{
  return g_still_skip && layer.switched_out && layer.edit_out && g_still_skip->contains(layer.id);
}

static bool layer_still(const Layer &layer, const std::unordered_set<uint64_t> &driven)
{
  if (layer.is_bone_layer() || layer.is_reference() || layer.is_media() || layer.is_switch() ||
      driven.contains(layer.id))
  {
    return false;
  }
  bool still = true;
  layer_for_each_channel<false>(const_cast<Layer &>(layer), [&](auto &ch, auto &value) {
    still = still && !ch.animated() && ch.actions.empty() && !action_bones_added(ch) && ch.has_last && ch.last == value;
  });
  if (!still) {
    return false;
  }
  return std::all_of(layer.children.begin(), layer.children.end(),
                     [&](const Layer &child) { return layer_still(child, driven); });
}

static void find_still_drawings(const Document &doc, std::unordered_set<uint64_t> &r_ids)
{
  std::unordered_set<uint64_t> driven;
  for (const Driver &d : doc.drivers) {
    driven.insert(d.target.layer);
  }
  const auto walk = [&](auto &&self, const std::vector<Layer> &layers) -> void {
    for (const Layer &layer : layers) {
      if (layer.is_switch()) {
        for (const Layer &child : layer.children) {
          if (layer_still(child, driven)) {
            r_ids.insert(child.id);
          }
        }
      }
      self(self, layer.children);
    }
  };
  walk(walk, doc.layers);
}

/* Turns a change to `ch` since it was last evaluated into a key at `frame` (auto-key on). Replace
 * (Blender's auto-keying mode) only changes a key already on the frame. Returns whether it keyed. */
template<typename T> static bool record(Channel<T> &ch, const T &value, int frame, bool auto_key, bool replace = false)
{
  if (!ch.has_last) {
    /* New data (a point just drawn): it is simply there, on every frame. */
    ch.last = value;
    ch.has_last = true;
    return false;
  }
  if (value == ch.last) {
    return false;
  }
  if (ch.keyed_by_hand && !ch.animated()) {
    /* Not keyed yet: the change is just there; `last` keeps the value from before it, for the
     * setup key if it gets keyed by hand now. */
    return false;
  }
  /* Keys hold the value without what action bone actions add. */
  const T base = value - ch.from_action_bones;
  if (frame == 0) {
    /* The setup frame: the frame-0 key, or the value itself while it isn't animated. */
    if (ch.animated()) {
      ch.set_key(0, base);
    }
  }
  else if (auto_key && (ch.animated() || !ch.keyed_by_hand) && (!replace || ch.key_at(frame))) {
    if (!ch.animated()) {
      ch.set_key(0, ch.last - ch.from_action_bones);
    }
    ch.set_key(frame, base);
    ch.last = value;
    return true;
  }
  ch.last = value;
  return false;
}

/* Auto-Freeze Keys: a key at `frame` holding what the channel has now, unless it has one there. */
template<typename T> static void freeze(Channel<T> &ch, const T &value, int frame)
{
  if (ch.key_at(frame)) {
    return;
  }
  const T base = value - ch.from_action_bones;
  if (!ch.animated()) {
    ch.set_key(0, base);
  }
  ch.set_key(frame, base);
  ch.last = value;
  ch.has_last = true;
}

template<typename T> static void evaluate(Channel<T> &ch, T &value, int frame)
{
  if (ch.animated()) {
    value = ch.evaluate(frame);
  }
  else {
    /* Unkeyed, the plain value is the data: take off what action bone actions added last time. */
    value -= ch.from_action_bones;
  }
  ch.from_action_bones = T{};
  ch.action_bone_live = false;
  ch.last = value;
  ch.has_last = true;
}

static void apply_action_bone_actions(Document &doc, int frame);
static void controllers_rest(Document &doc);
static void controllers_link(Document &doc, Layer &owner);
static float controller_dial_turn(const Layer &owner, const Bone &knob, const Bone &dial, float2 at);
static bool controller_in_action(const Document &doc, const Layer &owner);

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

void simulation_values_out(Document &doc)
{
  simulation_out(doc);
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

/* The channels of a drawing's shape, with the values recording keys (changes from the setup pose). */
template<typename F> static void drawing_shape_channels(VectorDrawing &drawing, F &&f)
{
  /* Detail points (data/detail.hh) ride their segments: no keys of their own. */
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      continue;
    }
    f(p.pos_keys, p.transform);
    f(p.radius_keys, p.radius);
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      if (cp.point >= 0 && size_t(cp.point) < drawing.points.size() && drawing.points[size_t(cp.point)].detail.level > 0) {
        continue;
      }
      f(cp.curvature_keys, cp.curvature);
      f(cp.handle_in_keys, cp.change_in);
      f(cp.handle_out_keys, cp.change_out);
    }
  }
}

/* A key's timing (all but its value, path handles, colour and selection). */
struct KeyTiming {
  Interp interp = Interp::Smooth;
  KeyType type = KeyType::Key;
  int hold = 0, interval = 1;
  Ease ease = Ease::Quad;
  float overshoot = 0.0f, elastic_amplitude = 0.0f, elastic_period = 0.0f, noise_amplitude = 0.0f,
        noise_frequency = 0.0f;
  int cycle_back = 0, cycle_frame = 0;
  bool cycle_absolute = false;
  std::vector<float> speeds;
  BezierHandles bezier = BEZIER_DEFAULT;
  bool operator==(const KeyTiming &) const = default;
};

template<typename T> static KeyTiming key_timing(const Key<T> &k)
{
  return {k.interp,         k.type,           k.hold,            k.interval,      k.ease,
          k.overshoot,      k.elastic_amplitude, k.elastic_period, k.noise_amplitude, k.noise_frequency,
          k.cycle_back,     k.cycle_frame,    k.cycle_absolute,  k.speeds,          k.bezier};
}

template<typename T> static void key_timing_set(Key<T> &k, const KeyTiming &t)
{
  k.interp = t.interp;
  k.type = k.frame == 0 ? KeyType::Key : t.type;
  k.hold = t.hold;
  k.interval = t.interval;
  k.ease = t.ease;
  k.overshoot = t.overshoot;
  k.elastic_amplitude = t.elastic_amplitude;
  k.elastic_period = t.elastic_period;
  k.noise_amplitude = t.noise_amplitude;
  k.noise_frequency = t.noise_frequency;
  k.cycle_back = t.cycle_back;
  k.cycle_frame = t.cycle_frame;
  k.cycle_absolute = t.cycle_absolute;
  k.speeds = t.speeds;
  k.bezier = t.bezier;
}

/* A key's value and timing (with_value) or its timing alone, as a number: a filler key edited since it was added no
 * longer has the one it was given (Key::filler_self, filler_out). */
static uint32_t sig_add(uint32_t h, const void *data, size_t size)
{
  const auto *b = static_cast<const unsigned char *>(data);
  for (size_t i = 0; i < size; i++) {
    h = (h ^ b[i]) * 16777619u;
  }
  return h;
}

template<typename T> static uint32_t key_sig(const Key<T> &k, bool with_value)
{
  uint32_t h = 2166136261u;
  if (with_value) {
    h = sig_add(h, &k.value, sizeof(T));
  }
  const int ints[] = {int(k.interp), k.hold, k.interval, int(k.ease), k.cycle_back, k.cycle_frame, k.cycle_absolute,
                      int(k.type)};
  const float floats[] = {k.overshoot, k.elastic_amplitude, k.elastic_period, k.noise_amplitude, k.noise_frequency};
  h = sig_add(h, ints, sizeof(ints));
  h = sig_add(h, floats, sizeof(floats));
  h = sig_add(h, k.bezier.data(), sizeof(k.bezier));
  return k.speeds.empty() ? h : sig_add(h, k.speeds.data(), k.speeds.size() * sizeof(float));
}

/* Filler keys edited since they were added (their value or timing, or the timing of the stretch after them while the
 * key after is the same) become the channel's own. */
template<typename K> static void fillers_check(std::vector<K> &keys)
{
  for (size_t i = 0; i < keys.size(); i++) {
    auto &k = keys[i];
    /* Not taken yet (just opened): as it is now. */
    if (!k.filler || k.filler_self == 0) {
      continue;
    }
    const bool next = i + 1 < keys.size();
    const int next_frame = next ? keys[i + 1].frame : INT32_MIN;
    if (key_sig(k, true) != k.filler_self ||
        (next_frame == k.filler_next && next && key_sig(keys[i + 1], false) != k.filler_out))
    {
      k.filler = false;
    }
  }
}

/* What each filler key is now, to tell later whether it was edited. */
template<typename C> static void fillers_mark(C &ch)
{
  for (size_t i = 0; i < ch.keys.size(); i++) {
    auto &k = ch.keys[i];
    if (!k.filler) {
      continue;
    }
    const bool next = i + 1 < ch.keys.size();
    k.filler_self = std::max(1u, key_sig(k, true));
    k.filler_next = next ? ch.keys[i + 1].frame : INT32_MIN;
    k.filler_out = next ? key_sig(ch.keys[i + 1], false) : 0;
  }
}

/* Keys a set of channels as one (Key Whole Drawing: a drawing's shape; Key Together: everything in a group): every
 * channel keyed on every frame any is, each new key holding what the channel has there and taking the timing most keys
 * on that frame have; with `settle` (as it is turned on), the keys already there take that timing too. A frame keyed
 * last time (`owner`'s frames, in the same action) that a channel there then (Channel::synced) misses now had its
 * key deleted: it goes from them all. Channels gone (a point deleted) and new ones (an eraser's cut ends) say
 * nothing. `visit(f)` calls f(channel, value, unit) for each channel; `changes(true)` turns the
 * values into the changes keys hold, `changes(false)` back. With `fillers`, the keys added are fillers
 * (Key::filler). Returns how many keys already there had their timing changed. */
template<typename Visit, typename Changes>
static int keys_together_sync(Layer &owner, Visit &&visit, Changes &&changes, bool settle, const std::string &action,
                              bool fillers)
{
  /* Each frame keyed anywhere: how many channels have it, and the timings its keys have there (how many of each, in
   * the order first met). */
  struct Frame {
    int count = 0;
    std::vector<std::pair<KeyTiming, int>> votes;
  };
  std::map<int, Frame> frames;
  bool all_alike = true;
  size_t keyed_count = 0, channels = 0;
  /* Frames from another action's channels (or none yet) say nothing about what was deleted here. */
  static const std::set<int> none;
  const std::set<int> &before = settle || owner.key_whole_action != action ? none : owner.key_whole_frames;
  std::set<int> deleted_set;
  visit([&](auto &ch, auto & /*value*/, const void * /*unit*/) {
    if (fillers) {
      fillers_check(ch.keys);
    }
    channels++;
    keyed_count += ch.keys.size();
    /* Keyed when they last agreed: each of that time's frames it misses now was deleted (frame 0, the setup, never). */
    if (!before.empty() && (fillers ? ch.together_synced : ch.synced)) {
      for (const int frame : before) {
        if (frame != 0 && !ch.key_at(frame)) {
          deleted_set.insert(frame);
        }
      }
    }
    for (const auto &k : ch.keys) {
      Frame &f = frames[k.frame];
      f.count++;
      const KeyTiming t = key_timing(k);
      auto it = std::find_if(f.votes.begin(), f.votes.end(), [&](const auto &v) { return v.first == t; });
      if (it == f.votes.end()) {
        f.votes.push_back({t, 1});
        all_alike = all_alike && f.votes.size() == 1;
      }
      else {
        it->second++;
      }
    }
  });
  const auto remember = [&] {
    owner.key_whole_action = action;
    owner.key_whole_frames.clear();
    visit([&](auto &ch, auto & /*value*/, const void * /*unit*/) {
      if (fillers) {
        fillers_mark(ch);
      }
      for (const auto &k : ch.keys) {
        owner.key_whole_frames.insert(k.frame);
      }
      (fillers ? ch.together_synced : ch.synced) = true;
    });
  };
  /* Nothing keyed, or every channel already keyed on every frame alike: nothing to do (the usual case, kept cheap). */
  if (frames.empty() || (all_alike && keyed_count == frames.size() * channels)) {
    remember();
    return 0;
  }
  const std::vector<int> deleted(deleted_set.begin(), deleted_set.end());
  for (const int frame : deleted) {
    frames.erase(frame);
  }
  std::map<int, const KeyTiming *> most;
  for (const auto &[frame, f] : frames) {
    const auto best = std::max_element(f.votes.begin(), f.votes.end(),
                                       [](const auto &a, const auto &b) { return a.second < b.second; });
    most[frame] = &best->first;
  }
  int settled = 0;
  changes(true);
  visit([&](auto &ch, auto &value, const void * /*unit*/) {
    for (const int frame : deleted) {
      channel_delete_key(ch, value, frame);
    }
    const bool was_animated = ch.animated();
    /* Its last key of its own: a filler before it is inside motion it already has. */
    int own_last = INT32_MIN;
    for (const auto &k : ch.keys) {
      if (!k.filler) {
        own_last = std::max(own_last, k.frame);
      }
    }
    for (const auto &[frame, timing] : most) {
      if (auto *k = ch.key_at(frame)) {
        if (settle && key_timing(*k) != *timing) {
          key_timing_set(*k, *timing);
          settled++;
        }
        continue;
      }
      /* What it has there now: between its keys, or (not keyed) the value it holds everywhere. */
      const auto held = was_animated ? ch.evaluate(frame) : value - ch.from_action_bones;
      auto &k = ch.set_key(frame, held);
      key_timing_set(k, *timing);
      k.filler = fillers;
      k.filler_inside = fillers && was_animated && frame < own_last;
    }
  });
  changes(false);
  remember();
  return settled;
}

/* Key Together turned off: the filler keys never edited go (a unit's keys on a frame stay together: a whole drawing's
 * stay if any of them is its own), and the rest become the channels' own. Those in actions not being edited (kept in
 * Channel::actions) go too. Returns how many went, and how many fillers stayed. */
template<typename Visit, typename Changes>
static std::pair<int, int> keys_together_release(Visit &&visit, Changes &&changes)
{
  std::set<std::pair<const void *, int>> own;
  /* The same in each action put away: (unit, action, frame). */
  std::set<std::tuple<const void *, std::string, int>> own_away;
  int fillers = 0;
  visit([&](auto &ch, auto & /*value*/, const void *unit) {
    fillers_check(ch.keys);
    for (const auto &k : ch.keys) {
      fillers += k.filler ? 1 : 0;
      if (!k.filler) {
        own.insert({unit, k.frame});
      }
    }
    for (auto &[name, keys] : ch.actions) {
      fillers_check(keys);
      for (const auto &k : keys) {
        fillers += k.filler ? 1 : 0;
        if (!k.filler) {
          own_away.insert({unit, name, k.frame});
        }
      }
    }
  });
  int removed = 0;
  bool away_changed = false;
  visit([&](auto &ch, auto & /*value*/, const void *unit) {
    for (auto it = ch.actions.begin(); it != ch.actions.end();) {
      auto &keys = it->second;
      const size_t before = keys.size();
      std::erase_if(keys, [&](const auto &k) { return k.filler && !own_away.count({unit, it->first, k.frame}); });
      removed += int(before - keys.size());
      away_changed = away_changed || keys.size() != before;
      for (auto &k : keys) {
        k.filler = false;
      }
      it = keys.empty() ? ch.actions.erase(it) : std::next(it);
    }
  });
  if (away_changed) {
    channel_keys_changed();
  }
  changes(true);
  visit([&](auto &ch, auto &value, const void *unit) {
    std::vector<int> go;
    for (const auto &k : ch.keys) {
      if (k.filler && !own.count({unit, k.frame})) {
        go.push_back(k.frame);
      }
    }
    /* Frame 0 last, and only when nothing else is left: a channel keyed holds a key there. */
    for (auto it = go.rbegin(); it != go.rend(); ++it) {
      if (*it != 0 || ch.keys.size() == 1) {
        ch.remove_key(*it);
        removed++;
      }
    }
    if (!ch.animated() && ch.keys.size() <= 1) {
      if (const auto *k = ch.key_at(0); k && k->filler) {
        value = k->value + ch.from_action_bones;
        ch.remove_key(0);
        removed++;
      }
    }
    for (auto &k : ch.keys) {
      k.filler = false;
    }
  });
  changes(false);
  /* Out of the group: new to any it joins. */
  visit([&](auto &ch, auto & /*value*/, const void * /*unit*/) { ch.together_synced = false; });
  return {removed, std::max(0, fillers - removed)};
}

int drawing_key_whole(Layer &layer, bool settle, const std::string &action)
{
  if (!layer.has_drawing() || layer.is_reference()) {
    return 0;
  }
  VectorDrawing &drawing = layer.drawing;
  return keys_together_sync(
      layer,
      [&](auto &&f) { drawing_shape_channels(drawing, [&](auto &ch, auto &value) { f(ch, value, &ch); }); },
      [&](bool to_changes) {
        if (to_changes) {
          drawing_changes_from_values(drawing);
        }
        else {
          drawing_values_from_changes(drawing);
        }
      },
      settle, action, false);
}

bool key_together_allowed(const Layer &layer)
{
  return layer.type == LayerType::Group || layer.type == LayerType::Bone;
}

Layer *key_together_group(Document &doc, uint64_t layer_id)
{
  Layer *found = nullptr;
  for (Layer *l = layer_find(doc, layer_id); l; l = layer_parent(doc, l->id)) {
    if (l->key_together && key_together_allowed(*l)) {
      found = l;
    }
  }
  return found;
}

/* The group and every layer inside it, flat. */
static std::vector<Layer *> together_layers(Document &doc, Layer &group)
{
  std::vector<Layer *> out;
  for (const Layer *l : document_layers_flat(doc)) {
    for (Layer *a = layer_find(doc, l->id); a; a = layer_parent(doc, a->id)) {
      if (a == &group) {
        out.push_back(layer_find(doc, l->id));
        break;
      }
    }
  }
  return out;
}

/* Calls f(channel, value, unit) for each channel a Key Together group keys as one: each layer's turn, move, size and
 * opacity, its bones', and its drawing's shape (a whole drawing's channels one unit). */
/* The channels of one layer that Key Together keys (see together_channels). */
template<typename F> static void layer_together_channels(Layer &l, std::set<const void *> &seen, F &&f)
{
  KeyFilter filter;
  filter.kinds = KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM | KEY_OPACITY | KEY_BONE_ANGLE | KEY_BONE_TRANSFORM;
  for (const ChannelRef &ref : layer_filtered_channels(l, filter)) {
    if (ref.scalar && ref.scalar_value && seen.insert(ref.scalar).second) {
      f(*ref.scalar, *ref.scalar_value, static_cast<const void *>(ref.scalar));
    }
    if (ref.vector && ref.vector_value && seen.insert(ref.vector).second) {
      f(*ref.vector, *ref.vector_value, static_cast<const void *>(ref.vector));
    }
  }
  if (l.type == LayerType::Vector && l.has_drawing() && !l.is_reference()) {
    const void *whole = l.key_whole_drawing ? static_cast<const void *>(&l) : nullptr;
    drawing_shape_channels(l.drawing, [&](auto &ch, auto &value) {
      f(ch, value, whole ? whole : static_cast<const void *>(&ch));
    });
  }
}

template<typename F> static void together_channels(Document &doc, Layer &group, F &&f)
{
  std::set<const void *> seen;
  for (Layer *l : together_layers(doc, group)) {
    layer_together_channels(*l, seen, f);
  }
}

static void layers_changes(const std::vector<Layer *> &layers, bool to_changes)
{
  for (Layer *l : layers) {
    if (l->type == LayerType::Vector && l->has_drawing() && !l->is_reference()) {
      if (to_changes) {
        drawing_changes_from_values(l->drawing);
      }
      else {
        drawing_values_from_changes(l->drawing);
      }
    }
  }
}

static void together_changes(Document &doc, Layer &group, bool to_changes)
{
  layers_changes(together_layers(doc, group), to_changes);
}

std::vector<ChannelRef> key_together_channels(Document &doc, Layer &group)
{
  std::vector<ChannelRef> out;
  together_channels(doc, group, [&](auto &ch, auto &value, const void * /*unit*/) {
    ChannelRef ref;
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, float>) {
      ref.scalar = &ch;
      ref.scalar_value = &value;
    }
    else {
      ref.vector = &ch;
      ref.vector_value = &value;
    }
    out.push_back(ref);
  });
  return out;
}

int key_together_sync(Document &doc, Layer &group, bool settle)
{
  /* A layer new to the group (moved in, made or pasted inside it) misses its frames without any being deleted: its
   * channels aren't Channel::together_synced yet. */
  const std::vector<Layer *> inside = together_layers(doc, group);
  const int changed = keys_together_sync(
      group, [&](auto &&f) { together_channels(doc, group, f); },
      [&](bool to_changes) { together_changes(doc, group, to_changes); }, settle,
      doc.action.empty() ? std::string() : std::to_string(doc.action_layer) + "/" + doc.action, true);
  group.key_together_members.clear();
  for (const Layer *l : inside) {
    group.key_together_members.insert(l->id);
  }
  return changed;
}

/* Layers moved out of the group (still in the document, the layers inside one going with it): the fillers they were
 * given and never edited go, as when the group is turned off; their own keys stay. */
static std::pair<int, int> key_together_leavers_release(Document &doc, Layer &group)
{
  const std::vector<Layer *> inside = together_layers(doc, group);
  std::vector<uint64_t> gone;
  std::vector<Layer *> left;
  for (const uint64_t id : group.key_together_members) {
    if (Layer *l = layer_find(doc, id); !l || std::find(inside.begin(), inside.end(), l) == inside.end()) {
      gone.push_back(id);
      if (l) {
        left.push_back(l);
      }
    }
  }
  for (const uint64_t id : gone) {
    group.key_together_members.erase(id);
  }
  if (left.empty()) {
    return {0, 0};
  }
  return keys_together_release(
      [&](auto &&f) {
        std::set<const void *> seen;
        for (Layer *l : left) {
          layer_together_channels(*l, seen, f);
        }
      },
      [&](bool to_changes) { layers_changes(left, to_changes); });
}

std::pair<int, int> key_together_release(Document &doc, Layer &group)
{
  group.key_together_members.clear();
  return keys_together_release([&](auto &&f) { together_channels(doc, group, f); },
                               [&](bool to_changes) { together_changes(doc, group, to_changes); });
}

void animation_record(Document &doc)
{
  /* Switches linked by name: new same-named switches join, renames reach partners. */
  for (const std::string &text : switch_links_update(doc)) {
    report(ReportType::Info, text);
  }
  if (doc.evaluated_frame < 0) {
    /* Nothing evaluated yet: the plain values are where everything starts. */
    animation_evaluate(doc, doc.frame_current);
    return;
  }
  /* A driven value is the driver's: an edit to it isn't kept (nor keyed). Worked out for the frame
   * being recorded, not the one being gone to (frame 0 would otherwise take another frame's pose
   * as the setup). */
  {
    const int now = doc.frame_current;
    doc.frame_current = doc.evaluated_frame;
    drivers_evaluate(doc);
    doc.frame_current = now;
  }
  /* So is a linked dial's the knob's: an edit to it is the knob's to take or refuse, never a key of its own. */
  if (doc.evaluated_frame != 0) {
    for (const Layer *flat : document_layers_flat(doc)) {
      if (flat->is_bone_layer()) {
        controllers_link(doc, *layer_find(doc, flat->id));
      }
    }
  }
  const int frame = doc.evaluated_frame;
  const bool auto_key = doc.auto_key;
  /* New points get their weights, so they follow the bones like their neighbours. */
  document_weight_new_points(doc, false, still_skipped);
  /* Every layer, children too: the passes below work on each on its own, over the CPU's cores (a big document has
   * thousands). */
  std::vector<Layer *> flat;
  const auto gather = [&](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      if (still_skipped(layer)) {
        continue;
      }
      flat.push_back(&layer);
      self(self, layer.children);
    }
  };
  gather(gather, doc.layers);
  /* New points get their handles now, from the setup pose, before anything counts as an edit. */
  parallel_for(flat.size(), 16, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      if (!flat[i]->is_reference()) {
        drawing_init_handles(flat[i]->drawing);
      }
    }
  });
  /* Every point and shape has its id; edits on references become their overrides (their offsets are
   * then keyed below like any edit). */
  const auto ids = [](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      if (still_skipped(layer)) {
        continue;
      }
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
  parallel_for(flat.size(), 16, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      Layer &layer = *flat[i];
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
    }
  });
  const bool replace = doc.auto_key_mode == AutoKeyMode::Replace;
  /* The channels an edit keyed, for Auto-Freeze Keys (only then: keying every point of a big drawing puts
   * hundreds of thousands here), sorted once recording is done. */
  const bool freezing = auto_key && doc.auto_freeze_keys && !replace && frame != 0;
  std::vector<const void *> keyed_list;
  struct Keyed {
    const std::vector<const void *> &list;
    bool empty() const { return list.empty(); }
    size_t count(const void *ch) const { return std::binary_search(list.begin(), list.end(), ch) ? 1 : 0; }
  } keyed{keyed_list};
  /* A media or text layer with Don't Auto Key on is changed but never auto-keyed. Each layer's channels on their
   * own (what each keyed gathered apart, then together). */
  std::vector<std::vector<const void *>> keyed_by_layer(flat.size());
  parallel_for(flat.size(), 16, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      Layer &layer = *flat[i];
      const bool keys = auto_key && !(layer.is_media() && layer.media.no_auto_key) &&
                        !(layer.is_text() && layer.text.no_auto_key);
      layer_for_each_channel(layer, [&](auto &ch, auto &value) {
        if (record(ch, value, frame, keys, replace) && freezing) {
          keyed_by_layer[i].push_back(&ch);
        }
      });
    }
  });
  for (const std::vector<const void *> &list : keyed_by_layer) {
    keyed_list.insert(keyed_list.end(), list.begin(), list.end());
  }
  /* Key Whole Drawing: a point keyed here keys every point here; turned on, the keys agree from then on (their
   * timing once). */
  std::vector<int> settled(flat.size(), 0);
  /* Which action: two bone layers' actions can share a name. */
  const std::string action_key = doc.action.empty() ? std::string() :
                                                      std::to_string(doc.action_layer) + "/" + doc.action;
  parallel_for(flat.size(), 16, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      Layer &layer = *flat[i];
      if (!layer.key_whole_drawing || layer.type != LayerType::Vector) {
        layer.key_whole_settled = false;
        continue;
      }
      settled[i] = drawing_key_whole(layer, !layer.key_whole_settled, action_key);
      layer.key_whole_settled = true;
    }
  });
  for (size_t i = 0; i < flat.size(); i++) {
    if (settled[i] > 0) {
      report(ReportType::Info, flat[i]->name + ": Key Whole Drawing gave " + std::to_string(settled[i]) +
                                   " keys the timing most of its points have on their frame");
    }
  }
  /* Key Together: everything in the group keyed as one (a group inside one that is, by the outer one); turned off,
   * the keys added only to hold a layer in place go. Those turned off first (a group inside one turned on among
   * them), so no group lets go of fillers another just added. */
  std::vector<Layer *> together_on;
  for (Layer *group : flat) {
    if (!key_together_allowed(*group)) {
      continue;
    }
    const Layer *parent = layer_parent(doc, group->id);
    const bool inside = parent && key_together_group(doc, parent->id);
    if (group->key_together && !inside) {
      together_on.push_back(group);
    }
    else if (group->key_together_settled) {
      group->key_together_settled = false;
      if (group->key_together) {
        /* On inside one turned on: handed over. Its fillers hold its layers where they were and stay, the outer
         * group's now (it lets them go when turned off), so turning the outer one on moves nothing. Its layers are new
         * to the outer one, which reads no frame they miss as deleted. */
        group->key_together_members.clear();
        together_channels(doc, *group, [](auto &ch, auto & /*value*/, const void * /*unit*/) {
          ch.together_synced = false;
        });
        continue;
      }
      const auto [removed, kept] = key_together_release(doc, *group);
      if (removed + kept > 0) {
        report(ReportType::Info, group->name + ": Key Together off: removed " + std::to_string(removed) +
                                     " filler keys" +
                                     (kept > 0 ? ", kept " + std::to_string(kept) + " you edited" : std::string()));
      }
    }
  }
  /* Layers moved out of a group let go first, before any group (the one they joined, say) syncs. */
  for (Layer *group : together_on) {
    const auto [removed, kept] = key_together_leavers_release(doc, *group);
    if (removed + kept > 0) {
      report(ReportType::Info, group->name + ": layers moved out: removed " + std::to_string(removed) +
                                   " filler keys" +
                                   (kept > 0 ? ", kept " + std::to_string(kept) + " you edited" : std::string()));
    }
  }
  for (Layer *group : together_on) {
    const int changed = key_together_sync(doc, *group, !group->key_together_settled);
    group->key_together_settled = true;
    if (changed > 0) {
      report(ReportType::Info, group->name + ": Key Together gave " + std::to_string(changed) +
                                   " keys the timing most of its layers have on their frame");
    }
  }
  simulation_in(doc);
  layer_for_each_channel(doc.simulation, [&](auto &ch, auto &value) { record(ch, value, frame, auto_key, replace); });
  layer_for_each_channel(document_camera(doc),
                         [&](auto &ch, auto &value) { record(ch, value, frame, auto_key, replace); });
  /* Auto-Freeze Keys (Moho's): a bone keyed here keys every bone of its bone layer shown here, so
   * the pose holds on this frame whatever is changed elsewhere later; and a point keyed here (where it
   * is, its width, curvature or handles) keys every point of its drawing, so the whole shape holds. Not
   * in Replace, which adds no keys. */
  std::sort(keyed_list.begin(), keyed_list.end());
  if (freezing && !keyed.empty()) {
    const auto freeze_points = [&](Layer &layer) {
      if (layer.is_reference()) {
        bool shaped = false;
        for (auto &[id, o] : layer.reference.points) {
          shaped = shaped || keyed.count(&o.offset_keys);
        }
        for (auto &[key, o] : layer.reference.handles) {
          shaped = shaped || keyed.count(&o.offset_keys);
        }
        if (shaped) {
          for (auto &[id, o] : layer.reference.points) {
            freeze(o.offset_keys, o.offset, frame);
          }
          for (auto &[key, o] : layer.reference.handles) {
            freeze(o.offset_keys, o.offset, frame);
          }
        }
        return;
      }
      VectorDrawing &drawing = layer.drawing;
      bool shaped = std::any_of(drawing.points.begin(), drawing.points.end(), [&](const MeshPoint &p) {
        return keyed.count(&p.pos_keys) || keyed.count(&p.radius_keys);
      });
      for (const Curve &curve : drawing.curves) {
        for (const CurvePoint &cp : curve.points) {
          shaped = shaped || keyed.count(&cp.curvature_keys) || keyed.count(&cp.handle_in_keys) ||
                   keyed.count(&cp.handle_out_keys);
        }
      }
      if (!shaped) {
        return;
      }
      /* Keys hold the change from the setup shape, as recording takes them. */
      drawing_changes_from_values(drawing);
      for (MeshPoint &p : drawing.points) {
        freeze(p.pos_keys, p.transform, frame);
        freeze(p.radius_keys, p.radius, frame);
      }
      for (Curve &curve : drawing.curves) {
        for (CurvePoint &cp : curve.points) {
          freeze(cp.curvature_keys, cp.curvature, frame);
          freeze(cp.handle_in_keys, cp.change_in, frame);
          freeze(cp.handle_out_keys, cp.change_out, frame);
        }
      }
    };
    const auto freeze_layers = [&](auto &&self, std::vector<Layer> &layers) -> void {
      for (Layer &layer : layers) {
        const bool posed = std::any_of(layer.bones.begin(), layer.bones.end(), [&](const Bone &b) {
          return keyed.count(&b.position_keys) || keyed.count(&b.angle_keys) || keyed.count(&b.scale_keys);
        });
        if (posed) {
          for (Bone &bone : layer.bones) {
            if (!bone_shown(layer, bone)) {
              continue;
            }
            bone_changes_from_values(bone);
            freeze(bone.position_keys, bone.transform, frame);
            /* A linked dial's turn is its knob's key (a key of its own would be folded onto the knob, at rest), and a
             * controller has nothing to turn or scale. */
            if (bone_is_controller(bone) || controller_of_dial(layer, bone)) {
              continue;
            }
            freeze(bone.angle_keys, bone.turn, frame);
            freeze(bone.scale_keys, bone.scale, frame);
          }
        }
        freeze_points(layer);
        self(self, layer.children);
      }
    };
    freeze_layers(freeze_layers, doc.layers);
  }
}

/* Every layer's channels evaluated on `frame`. Just after animation_record (`recorded`: every value is as the
 * edits left it, and recorded), a layer it would change nothing on is left as it is: no keys, nothing an action bone
 * action added, each channel's value its last recorded one. Most of a big rig's layers are like that, and
 * evaluating one turns its drawing's points into their changes from the setup pose and back. Bone layers always,
 * as evaluating also settles their bones. Otherwise every layer: evaluating also drops edits not recorded. */
static void evaluate_layers(std::vector<Layer> &layers, int frame, bool recorded,
                            std::unordered_set<const Layer *> *r_action_bones)
{
  for (Layer &layer : layers) {
    if (recorded && still_skipped(layer)) {
      continue;
    }
    bool needed = !recorded || layer.is_bone_layer();
    bool actions = false;
    if (!needed || r_action_bones) {
      layer_for_each_channel<false>(layer, [&](auto &ch, auto &value) {
        needed = needed || ch.animated() || action_bones_added(ch) || !ch.has_last || !(ch.last == value);
        actions = actions || !ch.actions.empty();
      });
    }
    if (needed) {
      layer_for_each_channel(layer, [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
    }
    /* What action bone actions play into, found on the way (evaluating took off what they had added). */
    if (r_action_bones && actions) {
      r_action_bones->insert(&layer);
    }
    evaluate_layers(layer.children, frame, recorded, r_action_bones);
  }
}

static void evaluate_frame(Document &doc, int frame, bool recorded);

void animation_evaluate(Document &doc, int frame)
{
  evaluate_frame(doc, frame, false);
}

static void evaluate_frame(Document &doc, int frame, bool recorded)
{
  std::unordered_set<const Layer *> action_bone_layers;
  evaluate_layers(doc.layers, frame, recorded, recorded ? &action_bone_layers : nullptr);
  if (frame == 0) {
    controllers_rest(doc);
  }
  simulation_in(doc);
  layer_for_each_channel(doc.simulation, [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  simulation_out(doc);
  layer_for_each_channel(document_camera(doc), [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  doc.action_bones_applied = false;
  g_action_bone_layers = recorded ? &action_bone_layers : nullptr;
  apply_action_bone_actions(doc, frame);
  g_action_bone_layers = nullptr;
  /* Drivers on top of keys and action bone actions, before references copy their originals. A driven
   * action bone plays its action bone action, and drivers reading what that moves see it. */
  if (drivers_evaluate(doc)) {
    animation_refresh_action_bones(doc);
    drivers_evaluate(doc);
  }
  /* References show their originals as they now are. */
  references_build(doc);
  /* Bone dynamics swing from the pose the keys and action bone actions give (and set each bone layer's
   * shown frame, for constraints' delays). */
  const int shown = doc.frame_current;
  doc.frame_current = frame;
  bone_outer_parents_update(doc);
  dynamics_update(doc);
  /* Follow Point and Limit to Shape read what's drawn now, dynamics included. */
  follow_points_update(doc);
  shape_limits_update(doc);
  doc.frame_current = shown;
  doc.evaluated_frame = frame;
}

void layer_strip_keys(Layer &layer, bool from_now, bool keep_actions)
{
  layer_for_each_channel(layer, [&](auto &ch, auto &value) {
    using T = std::decay_t<decltype(value)>;
    value -= ch.from_action_bones;
    if (ch.animated() && !from_now) {
      value = ch.evaluate(0);
    }
    ch.keys.clear();
    if (!keep_actions) {
      ch.actions.clear();
    }
    ch.from_action_bones = T{};
    ch.action_bone_live = false;
    ch.last = value;
    ch.has_last = true;
  });
  if (from_now) {
    /* The pose now becomes the setup: the points' changes from it are 0, and recording must see
     * that as how they were, not as an edit. */
    drawing_take_setup(layer.drawing);
    layer_for_each_channel(layer, [](auto &ch, auto &value) {
      ch.last = value;
      ch.has_last = true;
    });
  }
  for (Layer &child : layer.children) {
    layer_strip_keys(child, from_now, keep_actions);
  }
}

void animation_strip_action_bones(Document &doc)
{
  layers_for_each_channel_if(doc.layers, action_bones_added, [&](auto &ch, auto &value) {
    value -= ch.from_action_bones;
    ch.last -= ch.from_action_bones;
    ch.from_action_bones = {};
    ch.action_bone_live = false;
  });
  doc.action_bones_applied = false;
}

static uint64_t g_frame_changes = 0;

/* What action bone actions are worked out from, as one number: the frame, the timeline being edited, each layer as it
 * is
 * (its revision: an edit the app can't follow touches every layer), and the action bones' angles. */
static uint64_t action_bone_inputs(const Document &doc)
{
  uint64_t h = 0xcbf29ce484222325ull;
  const auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
  mix(uint64_t(reinterpret_cast<uintptr_t>(&doc)));
  mix(uint64_t(uint32_t(doc.frame_current)));
  mix(std::hash<std::string>()(doc.action));
  mix(doc.action_layer);
  mix(doc.action_bones_applied ? 1 : 2);
  const auto walk = [&](auto &&self, const std::vector<Layer> &layers) -> void {
    for (const Layer &layer : layers) {
      mix(layer.id);
      mix(layer.revision.value);
      if (layer.is_bone_layer()) {
        for (const std::string &name : layer.actions) {
          if (const Bone *b = action_bone_of(layer, name)) {
            mix(uint64_t(std::bit_cast<uint32_t>(b->angle)));
            mix(uint64_t(std::bit_cast<uint32_t>(b->turn)));
          }
        }
        for (const Bone &b : layer.bones) {
          if (bone_is_controller(b)) {
            mix(uint64_t(std::bit_cast<uint32_t>(b.position.x)));
            mix(uint64_t(std::bit_cast<uint32_t>(b.position.y)));
          }
        }
      }
      self(self, layer.children);
    }
  };
  walk(walk, doc.layers);
  return h;
}

void animation_sync(Document &doc)
{
  if (doc.evaluated_frame == doc.frame_current) {
    /* Same frame, but bones may have turned: action bone actions and dynamics follow at once (and a
     * setting whose keys just went shows its value). Not when nothing they are worked out from has
     * changed since: on a big rig that is most of what an idle frame costs. */
    simulation_in(doc);
    static uint64_t refreshed = 0;
    if (g_drag_doc == &doc) {
      /* A bone drag works them out itself, every update. */
      refreshed = 0;
    }
    else if (const uint64_t inputs = action_bone_inputs(doc); inputs != refreshed) {
      animation_refresh_action_bones(doc);
      refreshed = action_bone_inputs(doc);
    }
    /* Drivers follow what they read at once (a bone turned on this frame), and action bone actions what
     * they pose. */
    if (drivers_evaluate(doc)) {
      animation_refresh_action_bones(doc);
      drivers_evaluate(doc);
    }
    /* Switches follow children added, removed or picked at once. */
    switches_resolve(doc, doc.frame_current);
    /* References follow edits to their originals at once, and edits to them become theirs. */
    references_capture(doc);
    bone_outer_parents_update(doc);
    dynamics_update(doc);
    follow_points_update(doc);
    shape_limits_update(doc);
    return;
  }
  const bool recorded = doc.evaluated_frame >= 0;
  if (recorded) {
    animation_record(doc);
    g_frame_changes++;
  }
  evaluate_frame(doc, doc.frame_current, recorded);
}

void animation_sync_playing(Document &doc, uint64_t edits)
{
  if (doc.evaluated_frame == doc.frame_current) {
    animation_sync(doc);
    return;
  }
  const auto stamp_now = [&]() {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const uint64_t v : {uint64_t(reinterpret_cast<uintptr_t>(&doc)), edits, channel_keys_generation()}) {
      h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
  };
  const bool kept = g_still.valid && g_still.doc == &doc && g_still.stamp == stamp_now();
  g_still_skip = kept ? &g_still.ids : nullptr;
  animation_sync(doc);
  g_still_skip = nullptr;
  if (!kept) {
    /* Just evaluated in full: which hidden drawings are still, as of now. */
    g_still.ids.clear();
    find_still_drawings(doc, g_still.ids);
    g_still.doc = &doc;
    g_still.stamp = stamp_now();
    g_still.valid = true;
  }
}

/* --- Recording while playing ------------------------------------------------------------------ */

namespace {

struct FollowedChannel {
  void *channel;
  void *value;
  const std::type_info *type;
};

template<typename Walk> std::vector<FollowedChannel> followed_channels(Walk &&walk)
{
  std::vector<FollowedChannel> refs;
  walk([&](auto &ch, auto &value) { refs.push_back({&ch, &value, &typeid(value)}); });
  return refs;
}

/* See animation_follow_playback. */
template<typename Walk> bool follow_playback(Walk &&start, Walk &&dragged, Walk &&now, std::vector<char> &held)
{
  const std::vector<FollowedChannel> s = followed_channels(start), d = followed_channels(dragged), n = followed_channels(now);
  if (s.size() != d.size() || s.size() != n.size()) {
    return false;
  }
  for (size_t i = 0; i < s.size(); i++) {
    if (*s[i].type != *d[i].type || *s[i].type != *n[i].type) {
      return false;
    }
  }
  if (held.size() != s.size()) {
    held.assign(s.size(), 0);
  }
  size_t i = 0;
  start([&](auto &ch, auto &value) {
    using C = std::remove_reference_t<decltype(ch)>;
    using V = std::remove_reference_t<decltype(value)>;
    const C &d_ch = *static_cast<const C *>(d[i].channel);
    const V &d_value = *static_cast<const V *>(d[i].value);
    const C &n_ch = *static_cast<const C *>(n[i].channel);
    const V &n_value = *static_cast<const V *>(n[i].value);
    /* Held by the drag: changed from what the frame just left had it at, now or before. */
    held[i] = held[i] || !d_ch.has_last || !(d_value == d_ch.last);
    ch = n_ch;
    if (!held[i]) {
      value = n_value;
    }
    i++;
  });
  return true;
}

}  // namespace

bool animation_follow_playback(Document &start, Document &dragged, Document &now, std::vector<char> &held)
{
  const auto walk = [](Document &doc) {
    return [&doc](auto &&f) {
      layers_for_each_channel(doc.layers, f);
      layer_for_each_channel(document_camera(doc), f);
    };
  };
  if (!follow_playback(walk(start), walk(dragged), walk(now), held)) {
    return false;
  }
  /* It is now on the new frame: an edit made from it (the drag confirmed) is that frame's, not the
   * one the drag began on. */
  start.frame_current = now.frame_current;
  start.evaluated_frame = now.evaluated_frame;
  start.action_bones_applied = now.action_bones_applied;
  return true;
}

bool animation_follow_playback(std::vector<Bone> &start, std::vector<Bone> &dragged, std::vector<Bone> &now,
                               std::vector<char> &held)
{
  /* Bone layers to walk, holding the bones for the while. */
  Layer s, d, n;
  s.bones = std::move(start);
  d.bones = std::move(dragged);
  n.bones = std::move(now);
  const auto walk = [](Layer &layer) { return [&layer](auto &&f) { layer_for_each_channel(layer, f); }; };
  const bool done = follow_playback(walk(s), walk(d), walk(n), held);
  start = std::move(s.bones);
  dragged = std::move(d.bones);
  now = std::move(n.bones);
  return done;
}

uint64_t animation_frame_changes()
{
  return g_frame_changes;
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

std::vector<Layer *> action_owners(Document &doc, uint64_t layer_id)
{
  std::vector<Layer *> owners;
  for (Layer *owner = action_owner(doc, layer_id); owner;) {
    owners.push_back(owner);
    const Layer *around = layer_armature(doc, owner->id);
    owner = around ? layer_find(doc, around->id) : nullptr;
  }
  return owners;
}

/* Calls f(channel, value) for every channel of a bone layer and everything inside it. */
template<typename F> static void owner_for_each_channel(Layer &owner, F &&f)
{
  layer_for_each_channel(owner, f);
  layers_for_each_channel(owner.children, f);
}

/* As owner_for_each_channel, on the layers with keys in action `action` only (the rest have nothing to play). */
template<typename F> static void owner_for_each_channel_with(Layer &owner, const std::string &action, F &&f)
{
  const auto in_action = [&](const auto &ch) { return ch.actions.find(action) != ch.actions.end(); };
  bool any = false;
  layer_for_each_channel<false>(owner, [&](auto &ch, auto & /*value*/) { any = any || in_action(ch); });
  if (any) {
    layer_for_each_channel(owner, f);
  }
  layers_for_each_channel_if(owner.children, in_action, f);
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
    /* An action keeps its keys after frame 0; frame 0 belongs to the setup pose, all but its path handles, which are
     * the action's own. */
    ch.action_setup.erase(from);
    if constexpr (std::is_same_v<T, float2>) {
      for (const Key<T> &k : away) {
        if (k.frame == 0 && k.path_handles != PathHandles::Auto) {
          ch.action_setup[from] = k;
        }
      }
    }
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
    Key<T> &k = ch.set_key(0, setup);
    if constexpr (std::is_same_v<T, float2>) {
      if (auto it = ch.action_setup.find(to); it != ch.action_setup.end()) {
        k.path_handles = it->second.path_handles;
        k.path_in = it->second.path_in;
        k.path_out = it->second.path_out;
      }
    }
  }
  if (!to.empty()) {
    ch.action_setup.erase(to);
  }
  value = setup;
}

void drawing_timeline_swap(VectorDrawing &drawing, const std::string &from, const std::string &to)
{
  Layer holder;
  holder.type = LayerType::Vector;
  holder.drawing = std::move(drawing);
  layer_for_each_channel(holder, [&](auto &ch, auto &value) { swap_timeline(ch, value, from, to); });
  drawing = std::move(holder.drawing);
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
  /* Swap on the setup pose, where action bone actions add nothing. */
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
  for (Layer *owner : action_owners(doc, doc.active_layer)) {
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
  std::erase_if(owner->fix_actions, [&](const FixAction &f) { return f.name == name; });
  std::erase_if(owner->action_plays, [&](const ActionPlay &a) { return a.name == name; });
  for (Bone &bone : owner->bones) {
    std::replace(bone.controller_actions.begin(), bone.controller_actions.end(), name, std::string());
  }
  owner_for_each_channel(*owner, [&](auto &ch, auto & /*value*/) {
    ch.actions.erase(name);
    ch.action_setup.erase(name);
  });
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

/* --- Action bone actions (Moho's smart bones) ------------------------------------------------------- */

const Bone *action_bone_of(const Layer &owner, const std::string &action)
{
  if (const Bone *dial = controller_dial_of_action(owner, action)) {
    return dial;
  }
  return controller_of_action(owner, action);
}

/* A channel's timeline in action `name` with its setup value at frame 0, sampled at a fractional
 * frame (between the whole frames either side). */
template<typename T>
static T action_bone_sample(const std::vector<Key<T>> &keys, const T &setup, float t, bool constant = false)
{
  const ChannelKeysQuiet scratch;
  Channel<T> c;
  c.keys = keys;
  /* A yes/no channel (visibility, Draw Order) holds each key until the next, between frames too: the
   * bone turning past a key's frame switches it, never a blend of the two. */
  c.constant = constant;
  c.set_key(0, setup);
  const int f0 = int(std::floor(t));
  const float frac = t - float(f0);
  const T a = c.evaluate(f0);
  if (frac <= 0.0f || constant) {
    return a;
  }
  /* A Step key waits for its frame, between frames too: a bone turning past it jumps, never a blend of the two. */
  if (const Key<T> *next = c.key_at(f0 + 1); next && next->interp == Interp::Step) {
    return a;
  }
  const T b = c.evaluate(f0 + 1);
  return a + (b - a) * frac;
}

/* The action frame where the driving bone's keyed angle reaches `angle`: from frame 0 (its setup
 * angle) along its keys; the last frame past the end; 0 on the other side of the setup. */
static float action_bone_frame(const std::vector<Key<float>> &keys, float setup, float angle)
{
  if (keys.empty()) {
    return 0.0f;
  }
  const float last = float(keys.back().frame);
  const float step = 0.25f;
  float prev = action_bone_sample(keys, setup, 0.0f);
  for (float s = step; s <= last + 1e-4f; s += step) {
    const float now = action_bone_sample(keys, setup, s);
    if ((prev - angle) * (now - angle) <= 0.0f && now != prev) {
      return s - step + step * (angle - prev) / (now - prev);
    }
    prev = now;
  }
  const float end = action_bone_sample(keys, setup, last);
  const bool same_side = (end - setup) * (angle - setup) > 0.0f;
  return same_side && std::abs(angle - setup) > std::abs(end - setup) ? last : 0.0f;
}

const FixAction *fix_action_find(const Layer &owner, const std::string &name)
{
  for (const FixAction &f : owner.fix_actions) {
    if (f.name == name) {
      return &f;
    }
  }
  return nullptr;
}

std::string fix_action_broken(const Layer &owner, const FixAction &fix)
{
  if (fix.drivers.empty()) {
    return "it has no drivers";
  }
  for (const std::string &d : fix.drivers) {
    if (std::find(owner.actions.begin(), owner.actions.end(), d) == owner.actions.end()) {
      return "\"" + d + "\" is gone";
    }
    if (!action_bone_of(owner, d)) {
      return "\"" + d + "\" isn't an action bone action now";
    }
  }
  return "";
}

/* Moho's 360 action bone: an action whose driving bone's last key is exactly one whole turn from its setup
 * angle (either way) goes round and round: the bone's turn counts modulo a whole turn, so turning on past
 * the end starts it over and turning back past the setup plays it from the end (a 360 head turn on a dial).
 * Whole turns are within a tenth of a degree. */
static bool action_bone_keys_full_turn(const std::vector<Key<float>> &keys)
{
  return !keys.empty() && std::fabs(std::fabs(keys.back().value) - 2.0f * PI) < 0.1f * PI / 180.0f;
}

static float action_bone_turn_wrapped(const std::vector<Key<float>> &keys, float turn, bool loops)
{
  if (!loops || keys.empty() || std::fabs(keys.back().value) < 1e-6f) {
    return turn;
  }
  if (!action_bone_keys_full_turn(keys)) {
    /* Looped by hand: turned on past its end it starts over, never 0 (the end pose, not the setup). */
    const float along = turn / keys.back().value;
    return along <= 1.0f ? turn : turn - keys.back().value * std::ceil(along - 1.0f);
  }
  const float full = 2.0f * PI;
  /* Into [0, a turn) for an action keyed the positive way, (-a turn, 0] for one keyed the other. */
  return keys.back().value > 0.0f ? turn - std::floor(turn / full) * full : turn - std::ceil(turn / full) * full;
}

bool action_bone_action_full_turn(const Layer &owner, const std::string &name)
{
  const Bone *driver = action_bone_of(owner, name);
  if (!driver) {
    return false;
  }
  const auto it = driver->angle_keys.actions.find(name);
  return it != driver->angle_keys.actions.end() && action_bone_keys_full_turn(it->second);
}

/* What each action bone action's bone picks: its frame for the bone's turn in `turns` (owner's bones, in order), or a
 * controller's for where its knob is in `knobs` (frames). */
static std::vector<std::pair<std::string, float>> action_bone_frames_for(const Layer &owner,
                                                                         const std::vector<float> &turns,
                                                                         const std::vector<float2> &knobs,
                                                                         bool with_muted = false)
{
  std::vector<std::pair<std::string, float>> frames;
  for (const std::string &name : owner.actions) {
    const Bone *driver = action_bone_of(owner, name);
    if (!driver || (!with_muted && action_muted(owner, name))) {
      continue;
    }
    if (bone_is_controller(*driver)) {
      int side = 0;
      controller_of_action(owner, name, &side);
      const float2 knob = knobs[size_t(driver - owner.bones.data())];
      const float along = (controller_side_axis(side) == 0 ? knob.x : knob.y) * controller_side_sign(side);
      /* Through the side's timing: how far out the knob is picks the frame. */
      if (const float t = controller_side_frame(*driver, side, along); t > 0.0f) {
        frames.push_back({name, t});
      }
      continue;
    }
    const auto it = driver->angle_keys.actions.find(name);
    if (it == driver->angle_keys.actions.end()) {
      continue;
    }
    const float turn = turns[size_t(driver - owner.bones.data())];
    /* A linked bone turned only by its knob plays the knob's frame: a turn can stand for more than one frame of
     * an action that turns out and back, and the knob says which. */
    int side = 0;
    if (const Bone *knob = controller_of_action(owner, name, &side);
        knob && controller_side_dial(owner, *knob, side) == driver)
    {
      const float2 at = knobs[size_t(knob - owner.bones.data())];
      if (std::fabs(turn - controller_dial_turn(owner, *knob, *driver, at)) < 1e-5f) {
        const float along = (controller_side_axis(side) == 0 ? at.x : at.y) * controller_side_sign(side);
        if (const float t = controller_side_frame(*knob, side, along); t > 0.0f) {
          frames.push_back({name, t});
        }
        continue;
      }
    }
    /* The driving bone's keys are its Angle (the turn from its setup pose), so its action starts at
     * 0 and the bone's turn now picks the frame; turning an unkeyed bone plays it at once. */
    const float t = action_bone_frame(it->second, 0.0f,
                                      action_bone_turn_wrapped(it->second, turn, action_loops(owner, name)));
    if (t > 0.0f) {
      frames.push_back({name, t});
    }
  }
  return frames;
}

std::vector<std::pair<std::string, float>> action_bone_actions_playing(const Layer &owner)
{
  std::vector<float> turns;
  std::vector<float2> knobs;
  for (const Bone &b : owner.bones) {
    turns.push_back(b.angle - b.setup_angle);
    knobs.push_back(controller_knob_played(owner, b));
  }
  return action_bone_frames_for(owner, turns, knobs);
}

/* Whether action bone action `by` turns the bone of action bone action `name`. Actions key every bone, most held
 * still. */
static bool action_bone_action_turns(const Layer &owner, const std::string &by, const std::string &name)
{
  /* An action played at its own Frame: `by` keys that Frame (one action playing another). */
  if (by != name && action_frame_by(owner, name) == ActionFrameBy::Own) {
    const ActionPlay *a = action_play_find(owner, name);
    if (!a) {
      return false;
    }
    const auto it = a->frame_keys.actions.find(by);
    return it != a->frame_keys.actions.end() &&
           std::any_of(it->second.begin(), it->second.end(), [](const Key<float> &k) { return std::abs(k.value) > 1e-6f; });
  }
  const Bone *bone = action_bone_of(owner, name);
  if (!bone || by == name) {
    return false;
  }
  const auto moves_knob = [&](const Bone &knob) {
    const auto it = knob.position_keys.actions.find(by);
    return it != knob.position_keys.actions.end() &&
           std::any_of(it->second.begin(), it->second.end(), [](const Key<float2> &k) {
             return std::abs(k.value.x) > 1e-6f || std::abs(k.value.y) > 1e-6f;
           });
  };
  if (bone_is_controller(*bone)) {
    /* Its own sides' actions key its timing, not a move. */
    if (std::find(bone->controller_actions.begin(), bone->controller_actions.end(), by) == bone->controller_actions.end() &&
        moves_knob(*bone))
    {
      return true;
    }
    /* An independent knob plays where it sits in its box: an action moving the box or its Knob Parent (or what either
     * hangs from) moves it there, so it plays first (a head turn before the eyes riding it). */
    /* `by` itself, or a fix it drives (played as soon as its drivers have). */
    std::vector<const std::string *> movers = {&by};
    for (const FixAction &fix : owner.fix_actions) {
      if (std::find(fix.drivers.begin(), fix.drivers.end(), by) != fix.drivers.end()) {
        movers.push_back(&fix.name);
      }
    }
    const auto moves = [&](const Bone &b) {
      return std::any_of(movers.begin(), movers.end(), [&](const std::string *action) {
        const auto pos = b.position_keys.actions.find(*action);
        const auto ang = b.angle_keys.actions.find(*action);
        return (pos != b.position_keys.actions.end() &&
                std::any_of(pos->second.begin(), pos->second.end(), [](const Key<float2> &k) {
                  return std::abs(k.value.x) > 1e-6f || std::abs(k.value.y) > 1e-6f;
                })) ||
               (ang != b.angle_keys.actions.end() && std::any_of(ang->second.begin(), ang->second.end(), [](const Key<float> &k) {
                  return std::abs(k.value) > 1e-6f;
                }));
      });
    };
    const auto moves_from = [&](uint64_t id) {
      for (int depth = 0; id != 0 && depth < 256; depth++) {
        const Bone *up = bone_find(owner, id);
        if (!up) {
          return false;
        }
        if (moves(*up)) {
          return true;
        }
        id = up->parent;
      }
      return false;
    };
    if (bone->controller_knob_independent && (moves_from(bone->parent) || moves_from(bone->controller_knob_parent))) {
      return true;
    }
    /* Its constraints move it too: an action moving one's target (or what it hangs from) plays first. */
    for (const BoneConstraint &c : bone->constraints) {
      if (c.enabled && c.influence > 0.0f && controller_constraint_works(c.type) && c.target != 0 && moves_from(c.target)) {
        return true;
      }
    }
    return false;
  }
  /* A linked bone turns as its knob moves: an action moving the knob turns it. */
  int side = 0;
  if (const Bone *knob = controller_of_action(owner, name, &side);
      knob && controller_side_dial(owner, *knob, side) == bone && moves_knob(*knob))
  {
    return true;
  }
  const auto it = bone->angle_keys.actions.find(by);
  return it != bone->angle_keys.actions.end() && std::any_of(it->second.begin(), it->second.end(), [](const Key<float> &k) {
           return std::abs(k.value) > 1e-6f;
         });
}

/* Plays the action bone actions `frames_now` says are playing, an action bone action that turns another's bone (a dial
 * made of two
 * head turns) before that one, which then picks its frame by the turn it was given. Actions turning each other play as
 * they are, each once. Returns them all with their frames, as played. */
template<typename Frames, typename Play>
static std::vector<std::pair<std::string, float>> action_bone_actions_in_order(const Layer &owner, Frames &&frames_now,
                                                                               Play &&play)
{
  std::vector<std::pair<std::string, float>> played;
  bool nested = false;
  for (const std::string &a : owner.actions) {
    for (const std::string &b : owner.actions) {
      nested = nested || action_bone_action_turns(owner, a, b);
    }
  }
  const auto done = [&](const std::string &name) {
    return std::any_of(played.begin(), played.end(), [&](const auto &p) { return p.first == name; });
  };
  for (size_t round = 0; round <= owner.actions.size(); round++) {
    std::vector<std::pair<std::string, float>> ready = frames_now();
    std::erase_if(ready, [&](const auto &p) { return done(p.first); });
    std::vector<std::pair<std::string, float>> now;
    for (const auto &candidate : ready) {
      const bool waits = std::any_of(ready.begin(), ready.end(), [&](const auto &other) {
        return action_bone_action_turns(owner, other.first, candidate.first);
      });
      if (!waits) {
        now.push_back(candidate);
      }
    }
    for (const auto &[name, t] : now.empty() ? ready : now) {
      play(name, t);
      played.push_back({name, t});
    }
    if (ready.empty() || !nested) {
      break;
    }
  }
  return played;
}

float action_bone_action_length(const Layer &owner, const std::string &name)
{
  const Bone *driver = action_bone_of(owner, name);
  if (!driver) {
    return 0.0f;
  }
  if (bone_is_controller(*driver)) {
    int side = 0;
    controller_of_action(owner, name, &side);
    return controller_side_frames(*driver, side);
  }
  if (const auto it = driver->angle_keys.actions.find(name); it != driver->angle_keys.actions.end() && !it->second.empty()) {
    return float(it->second.back().frame);
  }
  /* Being edited: its keys are the bone's own. */
  return driver->angle_keys.keys.empty() ? 0.0f : float(driver->angle_keys.keys.back().frame);
}

/* --- The Actions panel ------------------------------------------------------------------------------------------ */

std::string action_frame_path(const std::string &name)
{
  return "actions[\"" + name + "\"].frame";
}

const ActionPlay *action_play_find(const Layer &owner, const std::string &name)
{
  for (const ActionPlay &a : owner.action_plays) {
    if (a.name == name) {
      return &a;
    }
  }
  return nullptr;
}

ActionPlay *action_play_find(Layer &owner, const std::string &name)
{
  return const_cast<ActionPlay *>(action_play_find(std::as_const(owner), name));
}

ActionPlay &action_play_ensure(Layer &owner, const std::string &name)
{
  if (ActionPlay *a = action_play_find(owner, name)) {
    return *a;
  }
  ActionPlay &a = owner.action_plays.emplace_back();
  a.name = name;
  /* At rest until now: setting its Frame is an edit to record (a key with auto-keying), not new data. */
  a.frame_keys.last = 0.0f;
  a.frame_keys.has_last = true;
  return a;
}

ActionFrameBy action_frame_by(const Layer &owner, const std::string &name, const Bone **r_holder)
{
  const Bone *holder = nullptr;
  ActionFrameBy by = ActionFrameBy::Own;
  if (fix_action_find(owner, name)) {
    by = ActionFrameBy::Fix;
  }
  else if ((holder = controller_of_action(owner, name))) {
    by = ActionFrameBy::Knob;
  }
  else if ((holder = controller_dial_of_action(owner, name))) {
    by = ActionFrameBy::Bone;
  }
  if (r_holder) {
    *r_holder = holder;
  }
  return by;
}

bool action_muted(const Layer &owner, const std::string &name)
{
  const ActionPlay *a = action_play_find(owner, name);
  return a && a->mute;
}

bool action_loops(const Layer &owner, const std::string &name)
{
  if (const ActionPlay *a = action_play_find(owner, name); a && a->loop != ActionLoop::Auto) {
    return a->loop == ActionLoop::On;
  }
  return action_bone_action_full_turn(owner, name);
}

float action_length(const Layer &owner, const std::string &name, bool edited)
{
  if (const ActionFrameBy by = action_frame_by(owner, name); by == ActionFrameBy::Bone || by == ActionFrameBy::Knob) {
    return action_bone_action_length(owner, name);
  }
  int last = 0;
  owner_for_each_channel(const_cast<Layer &>(owner), [&](auto &ch, auto & /*value*/) {
    if (edited) {
      last = ch.keys.empty() ? last : std::max(last, ch.keys.back().frame);
    }
    else if (const auto it = ch.actions.find(name); it != ch.actions.end() && !it->second.empty()) {
      last = std::max(last, it->second.back().frame);
    }
  });
  return float(last);
}

/* The frame of its action an own Frame plays: past the end the last frame, or looped round (never 0, the end pose). */
static float action_own_frame(float frame, float length, bool loops)
{
  if (frame <= 0.0f || length <= 0.0f) {
    return std::max(frame, 0.0f);
  }
  return !loops ? std::min(frame, length) : frame - length * std::ceil(frame / length - 1.0f);
}

/* The lengths of the actions playing at their own Frame, found in one pass over the bone layer when first asked for
 * (a big rig's channels are many, and the actions are played in rounds). */
struct ActionOwnLengths {
  bool found = false;
  std::map<std::string, float> of;
};

/* The actions playing at their own Frame, as action_bone_frames_for gives action bone actions: `at` a frame to work
 * their Frame out on (dynamics), -1 for the value it has now. */
static void action_own_frames(const Layer &owner, int at, ActionOwnLengths &lengths,
                              std::vector<std::pair<std::string, float>> &r_frames)
{
  for (const ActionPlay &a : owner.action_plays) {
    if (a.mute || action_frame_by(owner, a.name) != ActionFrameBy::Own ||
        std::find(owner.actions.begin(), owner.actions.end(), a.name) == owner.actions.end())
    {
      continue;
    }
    const float frame = at >= 0 && a.frame_keys.animated() ? a.frame_keys.evaluate(at) : a.frame;
    if (frame <= 0.0f) {
      continue;
    }
    if (!lengths.found) {
      lengths.found = true;
      owner_for_each_channel(const_cast<Layer &>(owner), [&](auto &ch, auto & /*value*/) {
        for (const auto &[name, keys] : ch.actions) {
          if (!keys.empty()) {
            float &last = lengths.of[name];
            last = std::max(last, float(keys.back().frame));
          }
        }
      });
    }
    const auto it = lengths.of.find(a.name);
    r_frames.push_back({a.name, action_own_frame(frame, it == lengths.of.end() ? 0.0f : it->second, action_loops(owner, a.name))});
  }
}

float action_frame_shown(const Layer &owner, const std::string &name)
{
  switch (action_frame_by(owner, name)) {
    case ActionFrameBy::Own: {
      const ActionPlay *a = action_play_find(owner, name);
      return a ? a->frame : 0.0f;
    }
    case ActionFrameBy::Fix:
      return fix_action_strength(owner, *fix_action_find(owner, name), action_bone_actions_playing(owner));
    default: {
      /* Where its bone or knob has it, muted or not. */
      std::vector<float> turns;
      std::vector<float2> knobs;
      for (const Bone &b : owner.bones) {
        turns.push_back(b.angle - b.setup_angle);
        knobs.push_back(controller_knob_played(owner, b));
      }
      for (const auto &[n, t] : action_bone_frames_for(owner, turns, knobs, true)) {
        if (n == name) {
          return t;
        }
      }
      return 0.0f;
    }
  }
}

bool action_frame_set(Layer &owner, const std::string &name, float frame)
{
  frame = std::max(frame, 0.0f);
  const Bone *holder = nullptr;
  switch (action_frame_by(owner, name, &holder)) {
    case ActionFrameBy::Fix:
      return false;
    case ActionFrameBy::Own:
      action_play_ensure(owner, name).frame = frame;
      return true;
    case ActionFrameBy::Knob: {
      /* Along its side, the other axis left where it is (the knob keeps inside its box). */
      Bone &knob = const_cast<Bone &>(*holder);
      int side = 0;
      controller_of_action(owner, name, &side);
      float2 at = controller_knob_seen(knob);
      (controller_side_axis(side) == 0 ? at.x : at.y) = controller_side_reach(knob, side, frame) * controller_side_sign(side);
      controller_knob_put(knob, at);
      return true;
    }
    case ActionFrameBy::Bone: {
      /* The turn its keys have on that frame. */
      Bone &bone = const_cast<Bone &>(*holder);
      const auto it = bone.angle_keys.actions.find(name);
      if (it == bone.angle_keys.actions.end() || it->second.empty()) {
        return false;
      }
      const float t = action_own_frame(frame, float(it->second.back().frame), action_loops(owner, name));
      bone.angle = bone.setup_angle + action_bone_sample(it->second, 0.0f, t);
      return true;
    }
  }
  return false;
}

std::vector<std::pair<std::string, float>> action_bone_actions_counting(const Layer &owner)
{
  std::vector<std::pair<std::string, float>> counting;
  for (const auto &[name, t] : action_bone_actions_playing(owner)) {
    const float length = action_bone_action_length(owner, name);
    if (length <= 0.0f || t / length >= FIX_COUNTS_FROM - 1e-6f) {
      counting.push_back({name, t});
    }
  }
  return counting;
}

/* An action bone action's length to measure shares by (1 for one with no keys yet, so nothing divides by 0). */
static float share_length(const Layer &owner, const std::string &name)
{
  const float length = action_bone_action_length(owner, name);
  return length > 1e-6f ? length : 1.0f;
}

std::vector<float> fix_pose_shares(const Layer &owner, const FixAction &fix, const FixPose &pose)
{
  std::vector<float> shares;
  for (size_t i = 0; i < fix.drivers.size(); i++) {
    const float at = i < pose.at.size() ? pose.at[i] : 0.0f;
    shares.push_back(std::clamp(at / share_length(owner, fix.drivers[i]), 0.0f, 1.0f));
  }
  return shares;
}

std::string action_frame_text(float frame)
{
  char text[32];
  if (std::fabs(frame - std::round(frame)) < 0.05f) {
    std::snprintf(text, sizeof(text), "%d", int(std::lround(frame)));
  }
  else {
    std::snprintf(text, sizeof(text), "%.1f", frame);
  }
  return text;
}

std::string fix_pose_label(const Layer & /*owner*/, const FixAction &fix, const FixPose &pose)
{
  /* As it is kept: each action bone action's frame. */
  std::string label;
  for (size_t i = 0; i < fix.drivers.size(); i++) {
    label += (label.empty() ? "" : " \xC2\xB7 ") + fix.drivers[i] + " " +
             action_frame_text(i < pose.at.size() ? pose.at[i] : 0.0f);
  }
  return label;
}

FixMix fix_action_mix(const Layer &owner, const FixAction &fix, const std::vector<std::pair<std::string, float>> &playing)
{
  FixMix mix;
  const size_t n = fix.drivers.size();
  if (n == 0 || fix.poses.empty()) {
    return mix;
  }
  /* Where the drivers are now, as shares of their action bone actions. */
  std::vector<float> now(n, 0.0f);
  for (size_t i = 0; i < n; i++) {
    for (const auto &[name, t] : playing) {
      if (name == fix.drivers[i]) {
        now[i] = std::clamp(t / share_length(owner, name), 0.0f, 1.0f);
      }
    }
  }
  std::vector<std::vector<float>> poses;
  std::vector<float> furthest(n, 0.0f);
  for (const FixPose &pose : fix.poses) {
    poses.push_back(fix_pose_shares(owner, fix, pose));
    for (size_t i = 0; i < n; i++) {
      furthest[i] = std::max(furthest[i], poses.back()[i]);
    }
  }
  /* Past the furthest pose along a driver it holds, as an action bone action holds its last frame. */
  for (size_t i = 0; i < n; i++) {
    now[i] = std::min(now[i], furthest[i]);
  }
  /* A grid (Live2D's keyforms): along each driver its rest and every share a pose has. Inside a cell the
   * correction blends linearly from the cell's corners, so moving one driver only ever blends the poses on that
   * line, and one pose alone fades as its drivers' shares multiplied (Maya's Multiplication). */
  constexpr float SAME = 1e-5f;
  std::vector<std::vector<float>> axes(n, std::vector<float>{0.0f});
  for (const std::vector<float> &pose : poses) {
    for (size_t i = 0; i < n; i++) {
      axes[i].push_back(pose[i]);
    }
  }
  for (std::vector<float> &axis : axes) {
    std::sort(axis.begin(), axis.end());
    axis.erase(std::unique(axis.begin(), axis.end(), [&](float a, float b) { return b - a < SAME; }), axis.end());
  }
  /* A corner as weights of the poses: 0 where any driver rests, a pose where one was made (several on one
   * corner share it). */
  const auto made = [&](const std::vector<size_t> &node, std::vector<float> &r_weights) {
    for (size_t i = 0; i < n; i++) {
      if (node[i] == 0) {
        return true;
      }
    }
    std::vector<size_t> on;
    for (size_t k = 0; k < poses.size(); k++) {
      bool here = true;
      for (size_t i = 0; i < n && here; i++) {
        here = std::abs(poses[k][i] - axes[i][node[i]]) < SAME;
      }
      if (here) {
        on.push_back(k);
      }
    }
    for (const size_t k : on) {
      r_weights[k] += 1.0f / float(on.size());
    }
    return !on.empty();
  };
  /* A corner no pose was made on: along each driver, in between the nearest made corners either side (holding
   * the one below past the last), averaging the drivers that have both sides (or, with none, all of them). */
  const auto corner = [&](const std::vector<size_t> &node) {
    std::vector<float> weights(poses.size(), 0.0f);
    if (made(node, weights)) {
      return weights;
    }
    std::vector<std::vector<float>> between, held;
    for (size_t i = 0; i < n; i++) {
      std::vector<size_t> lo = node, hi = node;
      std::vector<float> below(poses.size(), 0.0f), above(poses.size(), 0.0f);
      do {
        lo[i]--;
      } while (!made(lo, below));
      bool found = false;
      while (!found && hi[i] + 1 < axes[i].size()) {
        hi[i]++;
        found = made(hi, above);
      }
      if (!found) {
        held.push_back(below);
        continue;
      }
      const float t = (axes[i][node[i]] - axes[i][lo[i]]) / (axes[i][hi[i]] - axes[i][lo[i]]);
      for (size_t k = 0; k < poses.size(); k++) {
        below[k] += (above[k] - below[k]) * t;
      }
      between.push_back(below);
    }
    const std::vector<std::vector<float>> &from = between.empty() ? held : between;
    for (const std::vector<float> &w : from) {
      for (size_t k = 0; k < poses.size(); k++) {
        weights[k] += w[k] / float(from.size());
      }
    }
    return weights;
  };
  /* The cell `now` is in, and how far across it along each driver. */
  std::vector<size_t> cell(n, 0);
  std::vector<float> across(n, 0.0f);
  for (size_t i = 0; i < n; i++) {
    const std::vector<float> &axis = axes[i];
    size_t j = 0;
    while (j + 2 < axis.size() && now[i] >= axis[j + 1]) {
      j++;
    }
    cell[i] = j;
    across[i] = axis.size() < 2 ? 0.0f : std::clamp((now[i] - axis[j]) / (axis[j + 1] - axis[j]), 0.0f, 1.0f);
  }
  std::vector<float> weight(poses.size(), 0.0f);
  for (size_t bits = 0; bits < (size_t(1) << n); bits++) {
    std::vector<size_t> node = cell;
    float part = 1.0f;
    for (size_t i = 0; i < n; i++) {
      const bool up = (bits >> i) & 1;
      if (up && axes[i].size() < 2) {
        part = 0.0f;
        break;
      }
      node[i] += up ? 1 : 0;
      part *= up ? across[i] : 1.0f - across[i];
    }
    if (part <= 0.0f) {
      continue;
    }
    const std::vector<float> w = corner(node);
    for (size_t k = 0; k < poses.size(); k++) {
      weight[k] += w[k] * part;
    }
  }
  float strength = 0.0f;
  for (size_t k = 0; k < poses.size(); k++) {
    strength += weight[k];
    if (weight[k] > 1e-6f) {
      mix.poses.push_back({k, weight[k]});
    }
  }
  if (strength <= 1e-6f) {
    mix.poses.clear();
    return mix;
  }
  /* The nearest: the pose with the most say, the closer one on a tie. */
  float best_d2 = 0.0f;
  for (size_t k = 0; k < poses.size(); k++) {
    float d2 = 0.0f;
    for (size_t i = 0; i < n; i++) {
      d2 += (now[i] - poses[k][i]) * (now[i] - poses[k][i]);
    }
    if (mix.nearest < 0 || weight[k] > weight[size_t(mix.nearest)] + 1e-6f ||
        (std::abs(weight[k] - weight[size_t(mix.nearest)]) <= 1e-6f && d2 < best_d2))
    {
      mix.nearest = int(k);
      best_d2 = d2;
    }
  }
  mix.strength = std::min(strength, 1.0f);
  return mix;
}

float fix_action_strength(const Layer &owner, const FixAction &fix, const std::vector<std::pair<std::string, float>> &playing)
{
  return fix_action_mix(owner, fix, playing).strength;
}

int fix_pose_near(const Layer &owner, const FixAction &fix, const std::vector<std::pair<std::string, float>> &playing)
{
  FixPose probe;
  for (const std::string &d : fix.drivers) {
    const auto it = std::find_if(playing.begin(), playing.end(), [&](const auto &p) { return p.first == d; });
    probe.at.push_back(it == playing.end() ? 0.0f : it->second);
  }
  const std::vector<float> here = fix_pose_shares(owner, fix, probe);
  int best = -1;
  float best_d = 0.0f;
  for (size_t k = 0; k < fix.poses.size(); k++) {
    const std::vector<float> there = fix_pose_shares(owner, fix, fix.poses[k]);
    float far = 0.0f;
    for (size_t i = 0; i < here.size(); i++) {
      far = std::max(far, std::abs(here[i] - there[i]));
    }
    if (far <= FIX_COUNTS_FROM + 1e-6f && (best < 0 || far < best_d)) {
      best = int(k);
      best_d = far;
    }
  }
  return best;
}

int fix_pose_held(const FixAction &fix, int frame)
{
  int held = -1;
  for (size_t k = 0; k < fix.poses.size(); k++) {
    if (fix.poses[k].frame <= frame && (held < 0 || fix.poses[k].frame > fix.poses[size_t(held)].frame)) {
      held = int(k);
    }
  }
  /* Before the first pose: the first. */
  for (size_t k = 0; held < 0 && k < fix.poses.size(); k++) {
    held = int(k);
  }
  for (size_t k = 0; k < fix.poses.size(); k++) {
    if (fix.poses[k].frame > frame && fix.poses[size_t(held)].frame > frame && fix.poses[k].frame < fix.poses[size_t(held)].frame) {
      held = int(k);
    }
  }
  return held;
}

int fix_pose_on_frame(const FixAction &fix, int frame)
{
  for (size_t k = 0; k < fix.poses.size(); k++) {
    if (fix.poses[k].frame == frame) {
      return int(k);
    }
  }
  return -1;
}

/* What a fix adds to one channel as `mix` plays it: each pose's key on its frame, less the setup, times its
 * part (a pose with no key there adds nothing). A yes/no channel takes its nearest pose's key, once the fix
 * is half there. */
template<typename T>
static bool fix_delta(const std::vector<Key<T>> &keys, const T &setup, const FixAction &fix, const FixMix &mix,
                      bool constant, T &r_delta)
{
  r_delta = setup - setup;
  bool any = false;
  const auto key_on = [&](size_t pose) -> const Key<T> * {
    for (const Key<T> &key : keys) {
      if (key.frame == fix.poses[pose].frame) {
        return &key;
      }
    }
    return nullptr;
  };
  if (constant) {
    if (mix.nearest >= 0 && mix.strength >= 0.5f) {
      if (const Key<T> *key = key_on(size_t(mix.nearest))) {
        r_delta = key->value - setup;
        any = true;
      }
    }
    return any;
  }
  for (const auto &[pose, part] : mix.poses) {
    if (const Key<T> *key = key_on(pose)) {
      r_delta = r_delta + (key->value - setup) * part;
      any = true;
    }
  }
  return any;
}

bool action_adds(const Layer &owner, const std::string &name)
{
  return action_bone_of(owner, name) || fix_action_find(owner, name);
}

/* Each channel's setup, as apply_action_bone_actions takes it: its key on frame 0, or (unkeyed) its value less
 * what action bone actions add now. */
template<typename C, typename V> static auto channel_setup(const C &ch, const V &value)
{
  return ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value) - ch.from_action_bones;
}

/* Adds a pose to `fix` (not being edited) with its drivers at frames `at`, on the frame after its last pose,
 * keyed where the fix moves anything there now so the fix plays on as it did. Returns its index. */
static size_t fix_pose_insert(Layer &owner, FixAction &fix, const std::vector<float> &at)
{
  std::vector<std::pair<std::string, float>> here;
  for (size_t i = 0; i < fix.drivers.size() && i < at.size(); i++) {
    here.push_back({fix.drivers[i], at[i]});
  }
  const FixMix before = fix_action_mix(owner, fix, here);
  FixPose pose;
  pose.at = at;
  pose.at.resize(fix.drivers.size(), 0.0f);
  pose.frame = 10;
  for (const FixPose &p : fix.poses) {
    pose.frame = std::max(pose.frame, p.frame + 10);
  }
  owner_for_each_channel(owner, [&](auto &ch, auto &value) {
    const auto it = ch.actions.find(fix.name);
    if (it == ch.actions.end() || before.strength <= 0.0f) {
      return;
    }
    const auto setup = channel_setup(ch, value);
    auto delta = setup - setup;
    if (fix_delta(it->second, setup, fix, before, ch.constant, delta) && !(delta == setup - setup)) {
      std::remove_cvref_t<decltype(ch)> keys;
      keys.keys = it->second;
      keys.set_key(pose.frame, setup + delta);
      it->second = keys.keys;
    }
  });
  fix.poses.push_back(pose);
  return fix.poses.size() - 1;
}

/* Opens fix `name` of `owner` on pose `pose`'s frame. */
static void fix_open_pose(Document &doc, uint64_t owner_id, const std::string &name, size_t pose)
{
  document_set_action(doc, owner_id, name);
  const Layer *owner = layer_find(doc, owner_id);
  const FixAction *fix = owner ? fix_action_find(*owner, name) : nullptr;
  if (fix && pose < fix->poses.size()) {
    doc.frame_current = fix->poses[pose].frame;
  }
  animation_evaluate(doc, doc.frame_current);
}

/* Leaves fix `name` of `owner` if it is being edited (its keys go back among the actions), to change its
 * poses; returns whether it was, to open it again after. */
static bool fix_leave(Document &doc, uint64_t owner_id, const std::string &name)
{
  if (doc.action != name || doc.action_layer != owner_id) {
    return false;
  }
  document_set_action(doc, owner_id, "");
  return true;
}

FixOpened document_fix_pose_here(Document &doc, uint64_t owner_id, const std::vector<std::string> &drivers_asked)
{
  FixOpened opened;
  Layer *owner = layer_find(doc, owner_id);
  if (!owner || !owner->is_bone_layer()) {
    opened.error = "Fix actions belong to a bone layer";
    return opened;
  }
  if (doc.frame_current == 0) {
    opened.error = "Frame 0 is the rest pose: turn the action bones on another frame";
    return opened;
  }
  animation_record(doc);
  const std::vector<std::pair<std::string, float>> counting = action_bone_actions_counting(*owner);
  std::vector<std::pair<std::string, float>> drivers;
  if (drivers_asked.empty()) {
    drivers = counting;
  }
  else {
    for (const std::string &name : drivers_asked) {
      const auto it = std::find_if(counting.begin(), counting.end(), [&](const auto &p) { return p.first == name; });
      if (it == counting.end()) {
        opened.error = "\"" + name + "\" isn't turned (at least " + std::to_string(int(FIX_COUNTS_FROM * 100.0f + 0.5f)) +
                       "% of its action bone action)";
        return opened;
      }
      if (std::none_of(drivers.begin(), drivers.end(), [&](const auto &p) { return p.first == name; })) {
        drivers.push_back(*it);
      }
    }
  }
  if (drivers.size() < 2) {
    opened.error = "A fix needs two or more action bones playing: turn them first";
    return opened;
  }
  std::sort(drivers.begin(), drivers.end());
  std::vector<std::string> names;
  for (const auto &[name, t] : drivers) {
    names.push_back(name);
  }
  /* The fix of exactly these, if there is one. */
  FixAction *fix = nullptr;
  for (FixAction &f : owner->fix_actions) {
    std::vector<std::string> d = f.drivers;
    std::sort(d.begin(), d.end());
    fix = d == names ? &f : fix;
  }
  if (!fix) {
    FixAction made;
    std::string base;
    for (const std::string &name : names) {
      base += (base.empty() ? "" : " \xC3\x97 ") + name;
    }
    made.name = base;
    made.drivers = names;
    for (int i = 1; std::find(owner->actions.begin(), owner->actions.end(), made.name) != owner->actions.end(); i++) {
      char suffix[16];
      std::snprintf(suffix, sizeof(suffix), ".%03d", i);
      made.name = base + suffix;
    }
    owner->actions.push_back(made.name);
    owner->fix_actions.push_back(made);
    fix = &owner->fix_actions.back();
    opened.new_fix = true;
  }
  /* Each driver's frame now, in the fix's order. */
  std::vector<float> at;
  for (const std::string &d : fix->drivers) {
    const auto it = std::find_if(drivers.begin(), drivers.end(), [&](const auto &p) { return p.first == d; });
    at.push_back(it == drivers.end() ? 0.0f : it->second);
  }
  /* A pose of it this close already is the one to edit. */
  opened.pose = fix_pose_near(*owner, *fix, drivers);
  if (opened.pose < 0) {
    opened.pose = int(fix_pose_insert(*owner, *fix, at));
    opened.new_pose = true;
  }
  opened.name = fix->name;
  fix_open_pose(doc, owner_id, opened.name, size_t(opened.pose));
  return opened;
}

int document_fix_pose_add(Document &doc, uint64_t owner_id, const std::string &name, const std::vector<float> &shares)
{
  Layer *owner = layer_find(doc, owner_id);
  if (!owner || !fix_action_find(*owner, name)) {
    return -1;
  }
  animation_record(doc);
  fix_leave(doc, owner_id, name);
  owner = layer_find(doc, owner_id);
  FixAction *fix = const_cast<FixAction *>(fix_action_find(*owner, name));
  std::vector<float> at;
  for (size_t i = 0; i < fix->drivers.size(); i++) {
    const float share = i < shares.size() ? std::clamp(shares[i], 0.0f, 1.0f) : 0.0f;
    at.push_back(share * share_length(*owner, fix->drivers[i]));
  }
  const size_t pose = fix_pose_insert(*owner, *fix, at);
  fix_open_pose(doc, owner_id, name, pose);
  return int(pose);
}

bool bone_held_by_fix(const Document &doc, const Layer &armature, const Bone &bone)
{
  if (doc.action.empty() || doc.action_layer != armature.id) {
    return false;
  }
  /* A controller's knob shows an action of its own, its frame or the pose: it isn't moved there. */
  const auto holds = [&](const std::string &action) {
    return bone_is_controller(bone) && std::find(bone.controller_actions.begin(), bone.controller_actions.end(),
                                                 action) != bone.controller_actions.end();
  };
  /* In a side's own action it moves along that side (keying its timing): held only in a fix. */
  const FixAction *fix = fix_action_find(armature, doc.action);
  return fix && std::any_of(fix->drivers.begin(), fix->drivers.end(),
                            [&](const std::string &d) { return action_bone_of(armature, d) == &bone || holds(d); });
}

void fix_actions_tidy(Document &doc)
{
  /* The action bones of the fix being edited: never picked. */
  if (Layer *owner = doc.action.empty() ? nullptr : layer_find(doc, doc.action_layer)) {
    std::string let_go;
    int count = 0;
    for (Bone &b : owner->bones) {
      if ((b.selected || b.select_head || b.select_tail) && bone_held_by_fix(doc, *owner, b)) {
        bone_select(b, false);
        b.select_head = b.select_tail = false;
        let_go += (let_go.empty() ? "" : ", ") + b.name;
        count++;
      }
    }
    if (!let_go.empty() && !fix_action_find(*owner, doc.action)) {
      report(ReportType::Info, let_go + " shows this action's frame: go back to the mainline to move it");
    }
    else if (!let_go.empty()) {
      report(ReportType::Info, let_go + (count == 1 ? " is" : " are") + " held at the fix's pose: pick another pose on "
                               "the ruler or the pose map, or go back to the mainline to turn " + (count == 1 ? "it" : "them"));
    }
  }
  /* Poses whose keys are all gone. */
  std::vector<std::pair<uint64_t, std::string>> emptied;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer() || flat->fix_actions.empty()) {
      continue;
    }
    Layer &owner = *layer_find(doc, flat->id);
    for (FixAction &fix : owner.fix_actions) {
      const bool editing = doc.action == fix.name && doc.action_layer == owner.id;
      size_t removed = 0;
      for (size_t k = fix.poses.size(); k-- > 0;) {
        FixPose &pose = fix.poses[k];
        bool has_keys = false;
        owner_for_each_channel(owner, [&](auto &ch, auto & /*value*/) {
          if (has_keys) {
            return;
          }
          const auto *keys = editing ? &ch.keys : nullptr;
          if (!editing) {
            if (const auto it = ch.actions.find(fix.name); it != ch.actions.end()) {
              keys = &it->second;
            }
          }
          has_keys = keys && std::any_of(keys->begin(), keys->end(), [&](const auto &key) { return key.frame == pose.frame; });
        });
        if (has_keys) {
          pose.keyed = true;
        }
        else if (pose.keyed) {
          fix.poses.erase(fix.poses.begin() + std::ptrdiff_t(k));
          removed++;
        }
      }
      if (removed > 0) {
        if (fix.poses.empty()) {
          emptied.push_back({owner.id, fix.name});
        }
        else {
          report(ReportType::Info, "Removed " + std::to_string(removed) + " pose" + (removed == 1 ? "" : "s") + " of \"" +
                                       fix.name + "\": its keys were deleted");
        }
      }
    }
  }
  /* A fix left with no poses fixes nothing: it goes. */
  for (const auto &[owner_id, name] : emptied) {
    document_action_delete(doc, owner_id, name);
    report(ReportType::Info, "Deleted fix \"" + name + "\": the keys of its last pose were deleted");
  }
}

bool document_fix_pose_remove(Document &doc, uint64_t owner_id, const std::string &name, size_t pose)
{
  Layer *owner = layer_find(doc, owner_id);
  const FixAction *found = owner ? fix_action_find(*owner, name) : nullptr;
  if (!found || pose >= found->poses.size() || found->poses.size() < 2) {
    return false;
  }
  animation_record(doc);
  const bool editing = fix_leave(doc, owner_id, name);
  owner = layer_find(doc, owner_id);
  FixAction *fix = const_cast<FixAction *>(fix_action_find(*owner, name));
  const int frame = fix->poses[pose].frame;
  owner_for_each_channel(*owner, [&](auto &ch, auto & /*value*/) {
    if (auto it = ch.actions.find(name); it != ch.actions.end()) {
      std::erase_if(it->second, [&](const auto &key) { return key.frame == frame; });
    }
  });
  fix->poses.erase(fix->poses.begin() + std::ptrdiff_t(pose));
  if (editing) {
    fix_open_pose(doc, owner_id, name, std::min(pose, fix->poses.size() - 1));
  }
  else {
    animation_evaluate(doc, doc.frame_current);
  }
  return true;
}

bool document_action_rename(Document &doc, uint64_t owner_id, const std::string &from, const std::string &to)
{
  Layer *owner = layer_find(doc, owner_id);
  if (!owner || from.empty() || to.empty() || from == to ||
      std::find(owner->actions.begin(), owner->actions.end(), from) == owner->actions.end() ||
      std::find(owner->actions.begin(), owner->actions.end(), to) != owner->actions.end())
  {
    return false;
  }
  std::replace(owner->actions.begin(), owner->actions.end(), from, to);
  for (Bone &bone : owner->bones) {
    std::replace(bone.controller_actions.begin(), bone.controller_actions.end(), from, to);
  }
  for (ActionPlay &a : owner->action_plays) {
    a.name = a.name == from ? to : a.name;
  }
  owner_for_each_channel(*owner, [&](auto &ch, auto & /*value*/) {
    if (auto it = ch.actions.find(from); it != ch.actions.end()) {
      auto keys = std::move(it->second);
      ch.actions.erase(it);
      ch.actions[to] = std::move(keys);
    }
    if (auto it = ch.action_setup.find(from); it != ch.action_setup.end()) {
      auto key = std::move(it->second);
      ch.action_setup.erase(it);
      ch.action_setup[to] = std::move(key);
    }
  });
  const auto joined = [](const std::vector<std::string> &names) {
    std::string out;
    for (const std::string &n : names) {
      out += (out.empty() ? "" : " \xC3\x97 ") + n;
    }
    return out;
  };
  for (FixAction &f : owner->fix_actions) {
    /* Renamed itself: by hand, so it keeps the name it was given. */
    const bool named_after_drivers = f.name != from && f.name == joined(f.drivers);
    if (f.name == from) {
      f.name = to;
    }
    std::replace(f.drivers.begin(), f.drivers.end(), from, to);
    /* Still named after its drivers: it follows them (head_turn x head_up). */
    const std::string renamed = joined(f.drivers);
    if (named_after_drivers && renamed != f.name &&
        std::find(owner->actions.begin(), owner->actions.end(), renamed) == owner->actions.end())
    {
      const std::string old = f.name;
      std::replace(owner->actions.begin(), owner->actions.end(), old, renamed);
      for (ActionPlay &a : owner->action_plays) {
        a.name = a.name == old ? renamed : a.name;
      }
      owner_for_each_channel(*owner, [&](auto &ch, auto & /*value*/) {
        if (auto it = ch.actions.find(old); it != ch.actions.end()) {
          auto keys = std::move(it->second);
          ch.actions.erase(it);
          ch.actions[renamed] = std::move(keys);
        }
        if (auto it = ch.action_setup.find(old); it != ch.action_setup.end()) {
          auto key = std::move(it->second);
          ch.action_setup.erase(it);
          ch.action_setup[renamed] = std::move(key);
        }
      });
      if (doc.action == old && doc.action_layer == owner_id) {
        doc.action = renamed;
      }
      f.name = renamed;
    }
  }
  if (doc.action == from && doc.action_layer == owner_id) {
    doc.action = to;
  }
  return true;
}

void armature_bone_renamed(Document &doc, uint64_t owner_id, const std::string &from, const std::string &to)
{
  if (from == to) {
    return;
  }
  for (const char *other : {"", " 2"}) {
    document_action_rename(doc, owner_id, from + other, to + other);
  }
}

void action_bone_actions_add_at(const Layer &owner, int frame, std::vector<Bone> &bones)
{
  if (frame == 0 || owner.actions.empty() || bones.size() != owner.bones.size()) {
    return;
  }
  /* Dials linked to a controller have no keys: where they are on that frame is the knob's to say. */
  for (const Bone &knob : owner.bones) {
    if (!bone_is_controller(knob)) {
      continue;
    }
    const float2 at = bone_transform_from_position(knob, bones[size_t(&knob - owner.bones.data())].position);
    for (int side = 0; side < CONTROLLER_SIDES; side++) {
      if (const Bone *dial = controller_side_dial(owner, knob, side)) {
        bones[size_t(dial - owner.bones.data())].angle = dial->setup_angle + controller_dial_turn(owner, knob, *dial, at);
      }
    }
  }
  /* Every driving bone's angle on that frame picks its action's frame first, as on the frame shown. */
  ActionOwnLengths lengths;
  const auto playing_now = [&] {
    std::vector<float> turns;
    std::vector<float2> knobs;
    for (size_t i = 0; i < bones.size(); i++) {
      turns.push_back(bones[i].angle - owner.bones[i].setup_angle);
      knobs.push_back(bone_transform_from_position(owner.bones[i], bones[i].position));
    }
    std::vector<std::pair<std::string, float>> now = action_bone_frames_for(owner, turns, knobs);
    action_own_frames(owner, frame, lengths, now);
    return now;
  };
  const auto setup_of = [](const auto &ch, const auto &value) { return channel_setup(ch, value); };
  const std::vector<std::pair<std::string, float>> frames =
      action_bone_actions_in_order(owner, playing_now, [&](const std::string &name, float t) {
    const Bone *by = action_bone_of(owner, name);
    const Channel<float> *driver = by ? &by->angle_keys : nullptr;
    for (size_t j = 0; j < bones.size(); j++) {
      const Bone &src = owner.bones[j];
      Bone &b = bones[j];
      if (const auto it = src.position_keys.actions.find(name); it != src.position_keys.actions.end()) {
        const float2 setup = setup_of(src.position_keys, src.transform);
        b.position += rotate(action_bone_sample(it->second, setup, t) - setup, src.setup_angle);
      }
      if (&src.angle_keys != driver) {
        if (const auto it = src.angle_keys.actions.find(name); it != src.angle_keys.actions.end()) {
          const float setup = setup_of(src.angle_keys, src.turn);
          b.angle += action_bone_sample(it->second, setup, t) - setup;
        }
      }
      if (const auto it = src.scale_keys.actions.find(name); it != src.scale_keys.actions.end()) {
        const float2 setup = setup_of(src.scale_keys, src.scale);
        b.scale += action_bone_sample(it->second, setup, t) - setup;
      }
    }
  });
  std::set<const void *> driving;
  for (const auto &[name, t] : frames) {
    if (const Bone *by = action_bone_of(owner, name)) {
      driving.insert(&by->angle_keys);
    }
  }
  /* The fixes after them, as their drivers play together (never turning an action bone). */
  for (const FixAction &fix : owner.fix_actions) {
    if (!fix_action_broken(owner, fix).empty() || action_muted(owner, fix.name)) {
      continue;
    }
    const FixMix mix = fix_action_mix(owner, fix, frames);
    if (mix.strength <= 0.0f) {
      continue;
    }
    for (size_t j = 0; j < bones.size(); j++) {
      const Bone &src = owner.bones[j];
      Bone &b = bones[j];
      if (const auto it = src.position_keys.actions.find(fix.name); it != src.position_keys.actions.end()) {
        float2 d;
        if (fix_delta(it->second, setup_of(src.position_keys, src.transform), fix, mix, false, d)) {
          b.position += rotate(d, src.setup_angle);
        }
      }
      if (!driving.count(&src.angle_keys)) {
        if (const auto it = src.angle_keys.actions.find(fix.name); it != src.angle_keys.actions.end()) {
          float d;
          if (fix_delta(it->second, setup_of(src.angle_keys, src.turn), fix, mix, false, d)) {
            b.angle += d;
          }
        }
      }
      if (const auto it = src.scale_keys.actions.find(fix.name); it != src.scale_keys.actions.end()) {
        float2 d;
        if (fix_delta(it->second, setup_of(src.scale_keys, src.scale), fix, mix, false, d)) {
          b.scale += d;
        }
      }
    }
  }
}

static bool has_action_bone_actions(const Document &doc)
{
  for (const Layer *layer : document_layers_flat(doc)) {
    if (layer->is_bone_layer() && !layer->action_plays.empty() && !layer->actions.empty()) {
      return true;
    }
    for (const std::string &name : layer->actions) {
      if (layer->is_bone_layer() && action_bone_of(*layer, name)) {
        return true;
      }
    }
  }
  return false;
}

void animation_refresh_action_bones(Document &doc)
{
  if (!doc.action_bones_applied && !has_action_bone_actions(doc)) {
    return;
  }
  /* Take off what was added (from the value and its recorded value alike), then work it out again
   * from the bones as they are now. Always, whatever `action_bones_applied` says: a drag puts the bones
   * back as they were when it began every update, their action bone offsets with them (an action bone
   * turned past its action's end and then back the other way kept the end pose). Only the few layers
   * with action bone action data are looked at: found now, or kept by a bone drag. */
  std::unordered_set<const Layer *> involved;
  if (g_drag_doc == &doc) {
    g_action_bone_layers = &g_drag_layers;
  }
  else {
    find_action_bone_layers(doc, involved);
    g_action_bone_layers = &involved;
  }
  layers_for_each_channel_if(doc.layers, action_bones_added, [&](auto &ch, auto &value) {
    value -= ch.from_action_bones;
    ch.last -= ch.from_action_bones;
    ch.from_action_bones = {};
    ch.action_bone_live = false;
  });
  doc.action_bones_applied = false;
  apply_action_bone_actions(doc, doc.frame_current);
  g_action_bone_layers = nullptr;
}

void animation_action_bone_drag_begin(Document &doc)
{
  g_drag_layers.clear();
  find_action_bone_layers(doc, g_drag_layers);
  g_drag_doc = &doc;
}

std::unordered_set<uint64_t> animation_action_bone_drag_layers()
{
  std::unordered_set<uint64_t> ids;
  for (const Layer *l : g_drag_layers) {
    ids.insert(l->id);
  }
  return ids;
}

void animation_action_bone_drag_end()
{
  g_drag_doc = nullptr;
  g_drag_layers.clear();
}

static void apply_action_bone_actions_playing(Document &doc, int frame);

static void switches_clear_rig(std::vector<Layer> &layers)
{
  for (Layer &layer : layers) {
    layer.switch_rig_set = false;
    switches_clear_rig(layer.children);
  }
}

/* Action bone actions, then who wins on every switch (anim/switch.hh): a switch's pick is never added. */
static void apply_action_bone_actions(Document &doc, int frame)
{
  /* Keys can't change while a bone is dragged. */
  if (g_drag_doc != &doc) {
    controllers_update(doc);
  }
  switches_clear_rig(doc.layers);
  /* Each action plays into the layers with action bone action data only, found once for them all. */
  std::unordered_set<const Layer *> involved;
  const bool own = g_action_bone_layers == nullptr;
  if (own) {
    find_action_bone_layers(doc, involved);
    g_action_bone_layers = &involved;
  }
  apply_action_bone_actions_playing(doc, frame);
  if (own) {
    g_action_bone_layers = nullptr;
  }
  switches_resolve(doc, frame);
}

static void apply_action_bone_actions_playing(Document &doc, int frame)
{
  /* The setup frame (0) is the rig at rest: turning an action bone there sets its rest angle and plays
   * nothing, as in Moho (and bones don't bend the drawing there either). `frame`: the one evaluated
   * (switching timelines evaluates frame 0 while another is shown). */
  if (frame == 0) {
    return;
  }
  /* Editing an action bone action, the others play under it, for it to key their bones. Editing a fix action
   * shows what it fixes: its drivers held at the pose this frame of it is for (the pose on or before it),
   * the fixes for fewer of them as they play there, its own keys the correction on top. */
  const Layer *edited_owner = doc.action.empty() ? nullptr : layer_find(doc, doc.action_layer);
  const FixAction *edited_fix = edited_owner ? fix_action_find(*edited_owner, doc.action) : nullptr;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer() || flat->actions.empty()) {
      continue;
    }
    Layer &owner = *layer_find(doc, flat->id);
    const bool fixing = edited_fix && owner.id == doc.action_layer;
    controllers_link(doc, owner);
    /* Every driving bone's angle picks its frame first, before any action moves bones (but those that
     * turn it: see action_bone_actions_in_order). */
    std::vector<std::pair<std::string, float>> frames;
    if (fixing) {
      if (const int held = fix_pose_held(*edited_fix, frame); held >= 0) {
        const FixPose &pose = edited_fix->poses[size_t(held)];
        for (size_t k = 0; k < edited_fix->drivers.size(); k++) {
          if (action_bone_of(owner, edited_fix->drivers[k]) && k < pose.at.size() && pose.at[k] > 0.0f) {
            frames.push_back({edited_fix->drivers[k], pose.at[k]});
          }
        }
      }
    }
    ActionOwnLengths lengths;
    const auto playing_now = [&] {
      std::vector<std::pair<std::string, float>> now;
      std::vector<std::pair<std::string, float>> all = action_bone_actions_playing(owner);
      action_own_frames(owner, -1, lengths, all);
      for (const auto &[name, t] : all) {
        if (!(name == doc.action && owner.id == doc.action_layer)) {
          now.push_back({name, t});
        }
      }
      return now;
    };
    /* The action bones' own angles: they pick the frames, so nothing else moves them (shown turned only
     * while a fix is edited, its drivers held). */
    std::set<const void *> driving;
    for (const std::string &name : owner.actions) {
      if (const Bone *b = action_bone_of(owner, name)) {
        driving.insert(&b->angle_keys);
      }
    }
    /* Switches pick rather than add: an action has a say once its bone has turned past one of its
     * switch keys, and the first listed with a say wins. */
    std::map<const void *, Layer *> switches;
    const auto gather_switches = [&](auto &&self, std::vector<Layer> &layers) -> void {
      for (Layer &l : layers) {
        if (l.is_switch()) {
          switches[&l.switch_keys] = &l;
        }
        self(self, l.children);
      }
    };
    gather_switches(gather_switches, owner.children);
    const auto play = [&](const std::string &name, float t, const void *skip, bool skip_driving) {
      /* A side's action keys its own knob's timing (how far out each frame plays): never a move of the knob. The
       * action may be a linked dial's own, so the knob is the controller it is a side of. */
      const Bone *by_knob = controller_of_action(owner, name, nullptr);
      const void *timing = by_knob ? static_cast<const void *>(&by_knob->position_keys) : nullptr;
      owner_for_each_channel_with(owner, name, [&](auto &ch, auto &value) {
        if (static_cast<const void *>(&ch) == skip || static_cast<const void *>(&ch) == timing ||
            (skip_driving && driving.count(static_cast<const void *>(&ch))))
        {
          return;
        }
        if (const auto sw = switches.find(static_cast<const void *>(&ch)); sw != switches.end()) {
          if constexpr (std::is_same_v<std::decay_t<decltype(ch)>, Channel<float>>) {
            const auto it = ch.actions.find(name);
            if (it == ch.actions.end() || sw->second->switch_rig_set) {
              return;
            }
            const Key<float> *passed = nullptr;
            for (const Key<float> &k : it->second) {
              if (float(k.frame) > t + 1e-4f) {
                break;
              }
              if (k.frame > 0) {
                passed = &k;
              }
            }
            if (passed && passed->value > SWITCH_RIG + 0.5f) {
              sw->second->switch_rig = passed->value;
              sw->second->switch_rig_set = true;
              ch.action_bone_live = true;
            }
          }
          return;
        }
        const auto it = ch.actions.find(name);
        if (it == ch.actions.end()) {
          return;
        }
        /* Unkeyed, the setup is the value as last recorded, not as it is now: an edit not yet
         * recorded (a bone being dragged) stays on top instead of being taken into the offset. */
        const auto setup = ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value) - ch.from_action_bones;
        const auto delta = action_bone_sample(it->second, setup, t, ch.constant) - setup;
        /* The value and what it was last recorded as both move, so an edit stays an edit. */
        value += delta;
        ch.last += delta;
        ch.from_action_bones += delta;
        /* Pink only where the action moves it somewhere: an action keys every bone, most held still. */
        ch.action_bone_live = ch.action_bone_live ||
                              std::any_of(it->second.begin(), it->second.end(), [&](const auto &k) {
          const auto off = k.value - setup;
          if constexpr (std::is_same_v<std::decay_t<decltype(off)>, float>) {
            return std::abs(off) > 1e-6f;
          }
          else {
            return std::abs(off.x) > 1e-6f || std::abs(off.y) > 1e-6f;
          }
        });
        doc.action_bones_applied = true;
      });
    };
    /* Then the fixes, after every action bone action, as their drivers play together. While one is edited, the
     * fixes for some of its drivers only (a pair inside a three) play under it: it is keyed on top of them. */
    const auto inside_edited = [&](const FixAction &fix) {
      if (!fixing || fix.drivers.size() >= edited_fix->drivers.size()) {
        return false;
      }
      return std::all_of(fix.drivers.begin(), fix.drivers.end(), [&](const std::string &d) {
        return std::find(edited_fix->drivers.begin(), edited_fix->drivers.end(), d) != edited_fix->drivers.end();
      });
    };
    std::set<std::string> fixes_played;
    const auto play_fix = [&](const FixAction &fix, const std::vector<std::pair<std::string, float>> &with) {
      fixes_played.insert(fix.name);
      if ((fix.name == doc.action && owner.id == doc.action_layer) || !fix_action_broken(owner, fix).empty() ||
          action_muted(owner, fix.name) || (fixing && !inside_edited(fix)))
      {
        return;
      }
      const FixMix mix = fix_action_mix(owner, fix, with);
      if (mix.strength <= 0.0f) {
        return;
      }
      owner_for_each_channel_with(owner, fix.name, [&](auto &ch, auto &value) {
        if (driving.count(static_cast<const void *>(&ch)) || switches.count(static_cast<const void *>(&ch))) {
          return;
        }
        const auto it = ch.actions.find(fix.name);
        if (it == ch.actions.end()) {
          return;
        }
        const auto setup = ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value) - ch.from_action_bones;
        auto delta = setup - setup;
        if (!fix_delta(it->second, setup, fix, mix, ch.constant, delta)) {
          return;
        }
        value += delta;
        ch.last += delta;
        ch.from_action_bones += delta;
        ch.action_bone_live = ch.action_bone_live || !(delta == setup - setup);
        doc.action_bones_applied = true;
      });
    };
    if (fixing) {
      for (const auto &[name, t] : frames) {
        play(name, t, nullptr, false);
      }
    }
    else {
      /* What each linked bone's knob gave it as linked: an action that moves a knob (one a master controller plays)
       * turns its bones on from there, before they pick their frames. */
      std::vector<std::tuple<Bone *, Bone *, float>> given;
      if (!controller_in_action(doc, owner)) {
        for (Bone &knob : owner.bones) {
          for (int side = 0; side < CONTROLLER_SIDES && bone_is_controller(knob); side++) {
            Bone *dial = controller_side_dial(owner, knob, side);
            if (dial && std::none_of(given.begin(), given.end(), [&](const auto &g) { return std::get<1>(g) == dial; })) {
              given.emplace_back(&knob, dial, controller_dial_turn(owner, knob, *dial, controller_knob_played(owner, knob)));
            }
          }
        }
      }
      /* An action bone action never moves its own bone (the bone picks its frame), unless held for a fix. */
      std::vector<std::pair<std::string, float>> so_far;
      frames = action_bone_actions_in_order(owner, playing_now, [&](const std::string &name, float t) {
        const Bone *by = action_bone_of(owner, name);
        play(name, t, by ? static_cast<const void *>(&by->angle_keys) : nullptr, false);
        so_far.push_back({name, t});
        /* A fix as soon as all its drivers have played (it plays only while they all do): an independent knob riding
         * what it moves (a head turn's fix) picks its frames after it. */
        for (const FixAction &fix : owner.fix_actions) {
          if (!fixes_played.count(fix.name) &&
              std::all_of(fix.drivers.begin(), fix.drivers.end(), [&](const std::string &d) {
                return std::any_of(so_far.begin(), so_far.end(), [&](const auto &p) { return p.first == d; });
              }))
          {
            play_fix(fix, so_far);
          }
        }
        for (auto &[knob, dial, was] : given) {
          const float now = controller_dial_turn(owner, *knob, *dial, controller_knob_played(owner, *knob));
          if (now != was) {
            Channel<float> &ch = dial->angle_keys;
            ch.last += now - was;
            ch.from_action_bones += now - was;
            ch.has_last = true;
            dial->turn += now - was;
            dial->angle += now - was;
            was = now;
            doc.action_bones_applied = true;
          }
        }
      });
    }
    for (const FixAction &fix : owner.fix_actions) {
      if (!fixes_played.count(fix.name)) {
        play_fix(fix, frames);
      }
    }
  }
}

/* --- 2 Axis Controllers (anim/controller.hh) ------------------------------------------------------- */

/* The dial's keys in a side's action: its angle from 0 at frame 0 on, what the knob's frames turn it to. */
static const std::vector<Key<float>> &controller_side_curve(const Bone &dial, const std::string &action)
{
  return dial.angle_keys.actions.at(action);
}

/* The frame of a side's action a dial's turn stands for. */
static float controller_frame_of_turn(const Bone &dial, const std::string &action, float turn)
{
  const std::vector<Key<float>> &curve = controller_side_curve(dial, action);
  return action_bone_frame(curve, 0.0f, action_bone_turn_wrapped(curve, turn, action_bone_keys_full_turn(curve)));
}

static float controller_dial_turn(const Layer &owner, const Bone &knob, const Bone &dial, float2 at)
{
  float turn = 0.0f;
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    if (controller_side_dial(owner, knob, side) != &dial) {
      continue;
    }
    const float along = (controller_side_axis(side) == 0 ? at.x : at.y) * controller_side_sign(side);
    if (const float t = controller_side_frame(knob, side, along); t > 0.0f) {
      turn += action_bone_sample(controller_side_curve(dial, knob.controller_actions[side]), 0.0f, t);
    }
  }
  return turn;
}

/* A channel's keys go through `map` onto another, whatever else the key says kept, the key at frame 0 left to the
 * setup. Returns how many. */
template<typename From, typename To, typename Map>
static int controller_keys_copy(const std::vector<Key<From>> &keys, Channel<To> &to, Map &&map)
{
  int count = 0;
  for (const Key<From> &k : keys) {
    if (k.frame == 0) {
      continue;
    }
    if (!to.animated()) {
      to.set_key(0, To{});
    }
    to.set_key(k.frame, map(k.frame, k.value));
    if (Key<To> *made = to.key_at(k.frame)) {
      made->interp = k.interp;
      made->hold = k.hold;
      made->interval = k.interval;
      made->ease = k.ease;
      made->overshoot = k.overshoot;
      made->elastic_amplitude = k.elastic_amplitude;
      made->elastic_period = k.elastic_period;
      made->bezier = k.bezier;
    }
    count++;
  }
  return count;
}

/* A linked dial's keys on the mainline move to the knob (those it had when linked, or keyed by hand since). */
static int controller_fold_dial_keys(Layer &owner, Bone &knob, Bone &dial)
{
  if (dial.angle_keys.keys.empty()) {
    return 0;
  }
  int side_of[2] = {-1, -1};
  int axis = 0;
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    if (controller_side_dial(owner, knob, side) == &dial) {
      axis = controller_side_axis(side);
      side_of[side % 2] = side;
    }
  }
  const std::vector<Key<float>> keys = std::move(dial.angle_keys.keys);
  dial.angle_keys.keys.clear();
  dial.angle_keys.last -= dial.turn - dial.angle_keys.from_action_bones;
  dial.turn = dial.angle_keys.from_action_bones;
  dial.angle = dial.setup_angle + dial.turn;
  return controller_keys_copy(keys, knob.position_keys, [&](int frame, float turn) {
    float along = 0.0f;
    for (const int side : side_of) {
      if (side >= 0) {
        const float t = controller_frame_of_turn(dial, knob.controller_actions[side], turn);
        along = t > 0.0f ? controller_side_reach(knob, side, t) * controller_side_sign(side) : along;
      }
    }
    float2 value = knob.position_keys.animated() ? knob.position_keys.evaluate(frame) : float2();
    (axis == 0 ? value.x : value.y) = along;
    return value;
  });
}

/* The dial gets keys of its own that move it as the knob does. */
static void controller_bake_dial(const Layer &owner, const Bone &knob, Bone &dial)
{
  controller_keys_copy(knob.position_keys.keys, dial.angle_keys,
                       [&](int, float2 at) { return controller_dial_turn(owner, knob, dial, at); });
}

static void report_held(const std::string &text)
{
  /* A drag that's refused goes on trying every update: said once, and again only after a moment. */
  static std::string said;
  static std::chrono::steady_clock::time_point when;
  const auto now = std::chrono::steady_clock::now();
  if (text != said || now - when > std::chrono::seconds(2)) {
    report(ReportType::Warning, text);
    said = text;
    when = now;
  }
}

/* Whether `owner`'s channels are part of the action being edited: it owns it, or sits inside what does. */
static bool controller_in_action(const Document &doc, const Layer &owner)
{
  if (doc.action.empty()) {
    return false;
  }
  const Layer *edited = layer_find(doc, doc.action_layer);
  const auto inside = [&](auto &&self, const Layer &layer) -> bool {
    return layer.id == owner.id ||
           std::any_of(layer.children.begin(), layer.children.end(), [&](const Layer &c) { return self(self, c); });
  };
  return edited && inside(inside, *edited);
}

/* The link, before actions play so a dial picks its frame from it. What the knob gives a dial is added as an action's
 * amount is: the dial's plain value stays its setup, so what other actions key on it (held at 0, most of them) goes on
 * meaning a change from there. An edit to a dial since it was last worked out goes into the knob, if the knob can take
 * it: it has one place on an axis, so a dial can't leave rest while the knob is on the other side's. Then every dial
 * goes where the knob says, so one refused goes back, and one turned past the ends of its action stops there. */
static void controllers_link(Document &doc, Layer &owner)
{
  if (controller_in_action(doc, owner)) {
    return;
  }
  for (Bone &knob : owner.bones) {
    if (!bone_is_controller(knob)) {
      continue;
    }
    bone_changes_from_values(knob);
    bool moved = false;
    for (int axis = 0; axis < 2; axis++) {
      const int plus = axis * 2, minus = plus + 1;
      Bone *dial[2] = {controller_side_dial(owner, knob, plus), controller_side_dial(owner, knob, minus)};
      if (!dial[0] && !dial[1]) {
        continue;
      }
      float &along = axis == 0 ? knob.transform.x : knob.transform.y;
      bool edited[2] = {false, false};
      /* What the knob gave each before, where it wants to go, and the frame of its action that is. */
      float before[2] = {0.0f, 0.0f}, target[2] = {0.0f, 0.0f}, wants[2] = {0.0f, 0.0f};
      for (int s = 0; s < 2; s++) {
        if (dial[s]) {
          controller_fold_dial_keys(owner, knob, *dial[s]);
          bone_changes_from_values(*dial[s]);
          before[s] = controller_dial_turn(owner, knob, *dial[s], knob.transform);
          const float change = dial[s]->turn - dial[s]->angle_keys.last;
          edited[s] = dial[s]->angle_keys.has_last && std::fabs(change) > 1e-5f;
          if (edited[s]) {
            target[s] = before[s] + change;
            wants[s] = controller_frame_of_turn(*dial[s], knob.controller_actions[plus + s], target[s]);
          }
        }
      }
      if (dial[0] == dial[1]) {
        /* One bone turning both ways can't disagree with itself. */
        if (edited[0]) {
          along = wants[0] > 0.0f ? controller_side_reach(knob, plus, wants[0]) : -controller_side_reach(knob, minus, wants[1]);
          moved = true;
        }
      }
      else if (edited[0] && edited[1] && std::fabs(target[0]) > 1e-5f && std::fabs(target[1]) > 1e-5f) {
        /* Back to rest together is fine (Alt R), and falls through below. */
        report_held(dial[0]->name + " and " + dial[1]->name + " can't both turn: the knob has one place on this axis");
      }
      else if (wants[0] > 0.0f && along < -1e-4f) {
        report_held(dial[0]->name + " is held: the knob is on " + knob.controller_actions[minus]);
      }
      else if (wants[1] > 0.0f && along > 1e-4f) {
        report_held(dial[1]->name + " is held: the knob is on " + knob.controller_actions[plus]);
      }
      else if (wants[0] > 0.0f) {
        /* The frame it wants, as far out as the side's timing plays it. */
        along = controller_side_reach(knob, plus, wants[0]);
        moved = true;
      }
      else if (wants[1] > 0.0f) {
        along = -controller_side_reach(knob, minus, wants[1]);
        moved = true;
      }
      else if ((edited[0] && along > 0.0f) || (edited[1] && along < 0.0f)) {
        along = 0.0f;
        moved = true;
      }
      for (int s = 0; s < (dial[0] == dial[1] ? 1 : 2); s++) {
        if (Bone *d = dial[s]) {
          /* Worked out again (nothing added now), or on top of what was added, as recording finds it. */
          Channel<float> &ch = d->angle_keys;
          const float was = ch.from_action_bones != 0.0f ? before[s] : 0.0f;
          const float now = controller_dial_turn(owner, knob, *d, knob.transform);
          ch.last += now - was;
          ch.from_action_bones += now - was;
          ch.has_last = true;
          d->turn = ch.last;
          d->angle = d->setup_angle + d->turn;
          doc.action_bones_applied = true;
        }
      }
    }
    if (moved) {
      knob.position = bone_position_from_transform(knob, knob.transform);
    }
  }
}

void controllers_update(Document &doc)
{
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer() ||
        std::none_of(flat->bones.begin(), flat->bones.end(), [](const Bone &b) { return bone_is_controller(b); }))
    {
      continue;
    }
    Layer &owner = *layer_find(doc, flat->id);
    std::map<std::string, std::vector<int>> found;
    for (const Bone &knob : owner.bones) {
      for (const std::string &action : knob.controller_actions) {
        if (!action.empty()) {
          found[action];
        }
      }
    }
    /* Being edited, an action's keys are the channels' own. */
    const bool edited = !doc.action.empty() && doc.action_layer == owner.id && found.count(doc.action);
    const auto gather = [&](auto &&self, Layer &layer) -> void {
      layer_for_each_channel<false>(layer, [&](auto &ch, auto & /*value*/) {
        if (edited) {
          for (const auto &k : ch.keys) {
            found[doc.action].push_back(k.frame);
          }
        }
        for (auto &[action, frames] : found) {
          if (const auto it = ch.actions.find(action); it != ch.actions.end()) {
            for (const auto &k : it->second) {
              frames.push_back(k.frame);
            }
          }
        }
      });
      for (Layer &child : layer.children) {
        self(self, child);
      }
    };
    gather(gather, owner);
    for (Bone &knob : owner.bones) {
      if (!bone_is_controller(knob)) {
        continue;
      }
      for (int side = 0; side < CONTROLLER_SIDES; side++) {
        std::vector<int> &keys = knob.controller_keys[side];
        keys.clear();
        const std::string &action = knob.controller_actions[side];
        if (action.empty()) {
          continue;
        }
        keys = found[action];
        std::erase(keys, 0);
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        /* A dial's action ends where the dial does. */
        if (const Bone *dial = controller_side_dial(owner, knob, side)) {
          const int end = controller_side_curve(*dial, action).back().frame;
          std::erase_if(keys, [&](int frame) { return frame > end; });
          if (keys.empty() || keys.back() != end) {
            keys.push_back(end);
          }
        }
      }
      /* Its timing: its own keys in each side's action, how far out each frame plays (being edited, the action's keys
       * are the channel's own). A key at the rest point or going back is held where the one before was. */
      for (int side = 0; side < CONTROLLER_SIDES; side++) {
        std::vector<float2> &timing = knob.controller_timing[side];
        timing.clear();
        const std::string &action = knob.controller_actions[side];
        if (action.empty()) {
          continue;
        }
        const bool own = doc.action == action && doc.action_layer == owner.id;
        const auto it = knob.position_keys.actions.find(action);
        const std::vector<Key<float2>> *keys = own ? &knob.position_keys.keys :
                                               it != knob.position_keys.actions.end() ? &it->second : nullptr;
        float most = 0.0f;
        for (size_t k = 0; keys && k < keys->size(); k++) {
          const Key<float2> &key = (*keys)[k];
          const float along = (controller_side_axis(side) == 0 ? key.value.x : key.value.y) * controller_side_sign(side);
          if (key.frame <= 0) {
            continue;
          }
          most = std::max(most, along);
          timing.push_back({float(key.frame), most});
        }
      }
      if (knob.controller_size_auto) {
        controller_fit_size(knob);
      }
      /* Editing one of its actions, the knob sits at its frame (what hangs from it with it); in a side's own action
       * it moves along that side, keying its timing. */
      int edit_side = -1;
      for (int side = 0; side < CONTROLLER_SIDES && doc.action_layer == owner.id; side++) {
        if (!doc.action.empty() && knob.controller_actions[side] == doc.action) {
          edit_side = side;
        }
      }
      const bool editing = doc.action_layer == owner.id && (edit_side >= 0 || bone_held_by_fix(doc, owner, knob));
      knob.controller_knob_edit_side = edit_side;
      knob.controller_knob_editing = editing;
      knob.controller_knob_edit_at = editing ? controller_knob_shown(doc, owner, knob) : float2();
      /* Auto keying in a side's own action, the frame a move keys on (see controller_knob_set). */
      knob.controller_knob_edit_frame = edit_side >= 0 && doc.auto_key ? doc.frame_current : 0;
      /* From a place seen in the box to its keys, and its constraints' pull (see controller_knob_put). */
      knob.controller_frames_to_keys = Transform2D();
      knob.controller_knob_pull_now = float2();
      if (doc.frame_current != 0 && (knob.controller_knob_independent || controller_constrained(knob))) {
        const std::vector<BonePose> poses = armature_pose(owner, false);
        if (const BonePose *p = bone_pose_find(poses, knob.id)) {
          knob.controller_knob_pull_now = p->knob_pull;
          if (knob.controller_knob_independent) {
            const float size = std::max(knob.controller_frame_size, 1e-6f);
            const auto keyed = [&](float2 to) {
              return rotate(p->knob_parent_skin.inverted().apply(controller_box_point(knob, *p, to)) - p->setup_head,
                            -knob.controller_box_angle) / size;
            };
            const float2 o = keyed({0, 0}), x = keyed({1, 0}), y = keyed({0, 1});
            knob.controller_frames_to_keys = {x.x - o.x, x.y - o.y, y.x - o.x, y.y - o.y, o.x, o.y};
          }
        }
      }
    }
  }
}

/* Frame 0 is the rig at rest: no knob away from its rest point, unkeyed or not. */
static void controllers_rest(Document &doc)
{
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer()) {
      continue;
    }
    Layer &owner = *layer_find(doc, flat->id);
    for (Bone &knob : owner.bones) {
      if (!bone_is_controller(knob)) {
        continue;
      }
      if (!knob.position_keys.animated()) {
        knob.transform = {};
        knob.position = knob.setup_position;
        knob.position_keys.last = {};
      }
    }
  }
}

float2 controller_knob_shown(const Document &doc, const Layer &owner, const Bone &knob)
{
  if (doc.action.empty() || doc.action_layer != owner.id) {
    /* Staying in place, where it sits in the box as that has moved. */
    return doc.frame_current != 0 ? controller_knob_played(owner, knob) : controller_knob(knob);
  }
  float2 at;
  const auto put = [&](int side, float frames) {
    (controller_side_axis(side) == 0 ? at.x : at.y) = controller_side_reach(knob, side, frames) * controller_side_sign(side);
  };
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    if (knob.controller_actions[side] == doc.action) {
      put(side, std::min(float(doc.frame_current), controller_side_frames(knob, side)));
    }
  }
  if (const FixAction *fix = fix_action_find(owner, doc.action)) {
    if (const int held = fix_pose_held(*fix, doc.frame_current); held >= 0) {
      const FixPose &pose = fix->poses[size_t(held)];
      for (size_t k = 0; k < fix->drivers.size() && k < pose.at.size(); k++) {
        int side = 0;
        if (controller_of_action(owner, fix->drivers[k], &side) == &knob) {
          put(side, pose.at[k]);
        }
      }
    }
  }
  return at;
}

float2 controller_knob_playing(const Layer &owner, const Bone &knob)
{
  float2 at = controller_knob_played(owner, knob);
  /* Only dials can differ: what the controller plays itself goes by the knob. */
  const std::vector<std::pair<std::string, float>> playing = action_bone_actions_playing(owner);
  for (int axis = 0; axis < 2; axis++) {
    float along = 0.0f;
    bool linked = false;
    for (int side = axis * 2; side < axis * 2 + 2; side++) {
      linked = linked || controller_side_dial(owner, knob, side);
      for (const auto &[name, t] : playing) {
        if (name == knob.controller_actions[side]) {
          along += controller_side_reach(knob, side, t) * controller_side_sign(side);
        }
      }
    }
    if (linked) {
      (axis == 0 ? at.x : at.y) = along;
    }
  }
  return at;
}

std::vector<float2> controller_fix_poses(const Layer &owner, const Bone &knob)
{
  std::vector<float2> poses;
  for (const FixAction &fix : owner.fix_actions) {
    for (const FixPose &pose : fix.poses) {
      float2 at;
      bool ours = false;
      for (size_t k = 0; k < fix.drivers.size() && k < pose.at.size(); k++) {
        for (int side = 0; side < CONTROLLER_SIDES; side++) {
          if (knob.controller_actions[side] == fix.drivers[k]) {
            (controller_side_axis(side) == 0 ? at.x : at.y) +=
                controller_side_reach(knob, side, pose.at[k]) * controller_side_sign(side);
            ours = true;
          }
        }
      }
      if (ours) {
        poses.push_back(at);
      }
    }
  }
  return poses;
}

void armature_knob_snap(const Layer &armature, Bone &knob)
{
  controller_knob_snap(knob, controller_fix_poses(armature, knob));
}

std::vector<std::pair<size_t, size_t>> controller_fix_poses_at(const Layer &owner, const Bone &knob, float2 at)
{
  std::vector<std::pair<size_t, size_t>> found;
  for (size_t f = 0; f < owner.fix_actions.size(); f++) {
    const FixAction &fix = owner.fix_actions[f];
    const auto side_of = [&](const std::string &driver) {
      for (int side = 0; side < CONTROLLER_SIDES; side++) {
        if (knob.controller_actions[side] == driver) {
          return side;
        }
      }
      return -1;
    };
    if (fix.drivers.empty() ||
        std::any_of(fix.drivers.begin(), fix.drivers.end(), [&](const std::string &d) { return side_of(d) < 0; }))
    {
      continue;
    }
    for (size_t p = 0; p < fix.poses.size(); p++) {
      float2 there;
      for (size_t k = 0; k < fix.drivers.size() && k < fix.poses[p].at.size(); k++) {
        const int side = side_of(fix.drivers[k]);
        (controller_side_axis(side) == 0 ? there.x : there.y) +=
            controller_side_reach(knob, side, fix.poses[p].at[k]) * controller_side_sign(side);
      }
      if (std::fabs(there.x - at.x) < 0.05f && std::fabs(there.y - at.y) < 0.05f) {
        found.push_back({f, p});
      }
    }
  }
  return found;
}

bool controller_fix_pose_move(Layer &owner, const Bone &knob, size_t fix, size_t pose, float2 to)
{
  FixAction &f = owner.fix_actions[fix];
  std::vector<float> at = f.poses[pose].at;
  for (size_t k = 0; k < f.drivers.size() && k < at.size(); k++) {
    int side = -1;
    for (int s = 0; s < CONTROLLER_SIDES; s++) {
      side = knob.controller_actions[s] == f.drivers[k] ? s : side;
    }
    if (side < 0) {
      return false;
    }
    at[k] = controller_side_frame(knob, side, (controller_side_axis(side) == 0 ? to.x : to.y) * controller_side_sign(side));
    const float length = action_bone_action_length(owner, f.drivers[k]);
    if (at[k] < FIX_COUNTS_FROM * std::max(length, 1.0f) - 1e-4f) {
      return false;
    }
  }
  f.poses[pose].at = at;
  return true;
}

bool controller_dial_held(const Layer &owner, const Bone &dial)
{
  const Bone *knob = controller_of_dial(owner, dial);
  if (!knob) {
    return false;
  }
  const float2 at = controller_knob(*knob);
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    const Bone *here = controller_side_dial(owner, *knob, side);
    const Bone *there = controller_side_dial(owner, *knob, controller_side_opposite(side));
    const float along = (controller_side_axis(side) == 0 ? at.x : at.y) * controller_side_sign(side);
    if (here == &dial && there != &dial && along < -1e-4f) {
      return true;
    }
  }
  return false;
}

/* A bone's own action bone actions, the one that turns it the plain way first. */
static std::array<std::string, 2> own_actions(const Layer &owner, const Bone &bone)
{
  std::array<std::string, 2> own;
  const auto has = [&](const std::string &name) {
    return std::find(owner.actions.begin(), owner.actions.end(), name) != owner.actions.end();
  };
  if (has(bone.name)) {
    own[0] = bone.name;
  }
  if (has(bone.name + " 2")) {
    own[1] = bone.name + " 2";
  }
  return own;
}

std::string bone_convert_to_controller(Document &doc, uint64_t owner_id, uint64_t bone_id)
{
  Layer *owner = layer_find(doc, owner_id);
  Bone *bone = owner ? bone_find(*owner, bone_id) : nullptr;
  if (!bone || bone_is_controller(*bone)) {
    return "";
  }
  std::string note;
  const std::array<std::string, 2> own = own_actions(*owner, *bone);
  /* Its actions are about to be its own sides: another controller playing them lets go first, its keys for them
   * becoming the bone's (and then its knob's), so nothing stops playing. An action has one controller. */
  for (const Bone &other : owner->bones) {
    for (int side = 0; side < CONTROLLER_SIDES; side++) {
      const std::string &action = other.controller_actions[side];
      if (&other != bone && bone_is_controller(other) && !action.empty() && (action == own[0] || action == own[1])) {
        std::string why, moved;
        controller_side_set(doc, owner_id, other.id, side, "", "", &why, &moved);
        note = action + " is no longer " + other.name + "'s " + controller_side_name(side) + ": it is " + bone->name +
               "'s now";
      }
    }
  }
  /* Actions that turn it have nothing left to turn. */
  for (const auto &[action, keys] : bone->angle_keys.actions) {
    if (action != own[0] && action != own[1] &&
        std::any_of(keys.begin(), keys.end(), [](const Key<float> &k) { return std::abs(k.value) > 1e-6f; }))
    {
      note = action + " turns " + bone->name + ", and it can't turn a controller. Add a new bone for the controller "
             "instead and it goes on working.";
    }
  }
  /* Nothing connects to its knob: children on its tip hang from the knob with an offset, where they are (the knob
   * rests on the setup frame); so does any other child. */
  for (Bone &child : owner->bones) {
    if (child.parent == bone_id) {
      child.connected = false;
      child.follow_knob = true;
    }
  }
  bone = bone_find(*owner, bone_id);
  /* Moves in its own actions would be its sides' timing now: they go (the knob doesn't move in them). */
  for (const std::string &action : own) {
    if (!action.empty()) {
      bone->position_keys.actions.erase(action);
    }
  }
  std::vector<Key<float>> angle_keys = std::move(bone->angle_keys.keys);
  bone->angle_keys.keys.clear();
  /* A pin turned controller is a box like any other. */
  bone->type = BoneType::Controller2D;
  bone->controller_actions = {};
  bone->controller_actions[CONTROLLER_RIGHT] = own[0];
  bone->controller_actions[CONTROLLER_LEFT] = own[1];
  bone->controller_size_auto = true;
  bone->controller_box_angle = 0.0f;
  controller_fit_size(*bone);
  if (!own[0].empty() || !own[1].empty()) {
    /* Its turns on the mainline are the knob's now. */
    bone->position_keys.keys.clear();
    const auto frame_of = [&](const std::string &action, float turn) {
      const auto it = bone->angle_keys.actions.find(action);
      return it == bone->angle_keys.actions.end()
                 ? 0.0f
                 : action_bone_frame(it->second, 0.0f,
                                     action_bone_turn_wrapped(it->second, turn, action_bone_keys_full_turn(it->second)));
    };
    controller_keys_copy(angle_keys, bone->position_keys, [&](int, float turn) {
      const float right = frame_of(own[0], turn);
      return float2(right > 0.0f ? right : -frame_of(own[1], turn), 0.0f);
    });
  }
  else {
    /* Moves it was keyed with stay, as frames, and come back the same: its size stays as it is for that. */
    bone->angle_keys.keys = std::move(angle_keys);
    bone->controller_size_auto = bone->position_keys.keys.empty();
    for (Key<float2> &k : bone->position_keys.keys) {
      k.value = k.value / bone->controller_frame_size;
    }
  }
  bone->angle = bone->setup_angle;
  bone->turn = 0.0f;
  bone->angle_keys.last = 0.0f;
  animation_evaluate(doc, doc.frame_current);
  return note;
}

void controller_release(Document &doc, uint64_t owner_id, uint64_t knob_id)
{
  Layer *owner = layer_find(doc, owner_id);
  Bone *knob = owner ? bone_find(*owner, knob_id) : nullptr;
  if (!knob || !bone_is_controller(*knob)) {
    return;
  }
  std::vector<Bone *> dials;
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    Bone *dial = controller_side_dial(*owner, *knob, side);
    if (dial && std::find(dials.begin(), dials.end(), dial) == dials.end()) {
      dials.push_back(dial);
    }
  }
  for (Bone *dial : dials) {
    controller_bake_dial(*owner, *knob, *dial);
  }
}

std::string bone_convert_to_regular(Document &doc, uint64_t owner_id, uint64_t bone_id)
{
  Layer *owner = layer_find(doc, owner_id);
  Bone *bone = owner ? bone_find(*owner, bone_id) : nullptr;
  if (bone && bone_is_pin(*bone)) {
    /* Its length was kept: the bone it was. */
    bone->type = BoneType::Regular;
    return "";
  }
  if (!bone || !bone_is_controller(*bone)) {
    return "";
  }
  std::array<bool, CONTROLLER_SIDES> linked{};
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    linked[side] = controller_side_dial(*owner, *bone, side) != nullptr;
  }
  controller_release(doc, owner_id, bone_id);
  const std::array<std::string, 2> own = own_actions(*owner, *bone);
  const bool own_right = !own[0].empty() && bone->controller_actions[CONTROLLER_RIGHT] == own[0];
  const bool own_left = !own[1].empty() && bone->controller_actions[CONTROLLER_LEFT] == own[1];
  if (own_right || own_left) {
    /* Back into the turns they came from. */
    const auto turn_at = [&](const std::string &action, float t) {
      const auto it = bone->angle_keys.actions.find(action);
      return it == bone->angle_keys.actions.end() ? 0.0f : action_bone_sample(it->second, 0.0f, t);
    };
    bone->angle_keys.keys.clear();
    controller_keys_copy(bone->position_keys.keys, bone->angle_keys, [&](int, float2 at) {
      /* How far out is a frame through the side's timing. */
      return at.x > 0.0f ? turn_at(own[0], controller_side_frame(*bone, CONTROLLER_RIGHT, at.x)) :
                           turn_at(own[1], controller_side_frame(*bone, CONTROLLER_LEFT, -at.x));
    });
    bone->position_keys.keys.clear();
  }
  else {
    /* What the knob did to linked bones is their keys now: only the frames of the other sides stay, as moves. */
    bool dropped = false;
    for (Key<float2> &k : bone->position_keys.keys) {
      for (int side = 0; side < CONTROLLER_SIDES; side++) {
        float &along = controller_side_axis(side) == 0 ? k.value.x : k.value.y;
        if (linked[side] && along * controller_side_sign(side) > 0.0f) {
          along = 0.0f;
          dropped = true;
        }
      }
      k.value = k.value * bone->controller_frame_size;
    }
    if (dropped && std::all_of(bone->position_keys.keys.begin(), bone->position_keys.keys.end(),
                               [](const Key<float2> &k) { return length(k.value) < 1e-9f; }))
    {
      bone->position_keys.keys.clear();
    }
  }
  /* Its keys in its sides' actions were their timing, never a move: as a bone they'd move it. */
  for (const std::string &action : bone->controller_actions) {
    if (!action.empty()) {
      bone->position_keys.actions.erase(action);
    }
  }
  bone->type = BoneType::Regular;
  bone->controller_actions = {};
  bone->controller_keys = {};
  bone->controller_timing = {};
  bone->controller_knob_editing = false;
  bone->controller_knob_edit_side = -1;
  bone->controller_knob_edit_frame = 0;
  bone->controller_knob_edit_at = {};
  bone->position = bone->setup_position;
  bone->transform = {};
  bone->position_keys.last = {};
  animation_evaluate(doc, doc.frame_current);
  return "";
}

std::string bone_convert_to_pin(Document &doc, uint64_t owner_id, uint64_t bone_id)
{
  Layer *owner = layer_find(doc, owner_id);
  Bone *bone = owner ? bone_find(*owner, bone_id) : nullptr;
  if (!bone || bone_is_pin(*bone)) {
    return "";
  }
  std::string note;
  if (bone_is_controller(*bone)) {
    note = bone_convert_to_regular(doc, owner_id, bone_id);
    bone = bone_find(*owner, bone_id);
  }
  /* Children on its tip hang with an offset instead, right where they are (the tip is kept, hidden). */
  for (Bone &child : owner->bones) {
    if (child.parent == bone_id && child.connected) {
      child.connected = false;
    }
  }
  bone = bone_find(*owner, bone_id);
  bone->type = BoneType::Pin;
  animation_evaluate(doc, doc.frame_current);
  return note;
}

bool controller_side_keys_clash(const Layer &owner, const Bone &knob, int side, const std::string &action)
{
  const Bone *mine = controller_dial_of_action(owner, action);
  const auto keyed = [](const Bone *dial) {
    return dial && std::any_of(dial->angle_keys.keys.begin(), dial->angle_keys.keys.end(),
                               [](const Key<float> &k) { return k.frame != 0 && std::abs(k.value) > 1e-6f; });
  };
  const auto knob_keyed = [&] {
    return std::any_of(knob.position_keys.keys.begin(), knob.position_keys.keys.end(), [&](const Key<float2> &k) {
      return k.frame != 0 && std::abs(controller_side_axis(side) == 0 ? k.value.x : k.value.y) > 1e-6f;
    });
  };
  return keyed(mine) && knob_keyed();
}

bool controller_side_set(Document &doc, uint64_t owner_id, uint64_t knob_id, int side, const std::string &action,
                         const std::string &keep, std::string *r_why, std::string *r_note)
{
  Layer *owner = layer_find(doc, owner_id);
  Bone *knob = owner ? bone_find(*owner, knob_id) : nullptr;
  if (!knob || !bone_is_controller(*knob) || side < 0 || side >= CONTROLLER_SIDES) {
    *r_why = "That isn't a side of a controller";
    return false;
  }
  if (!doc.action.empty()) {
    *r_why = "Go back to the mainline to pick a controller's actions";
    return false;
  }
  if (knob->controller_actions[side] == action) {
    return true;
  }
  if (!action.empty()) {
    if (std::find(owner->actions.begin(), owner->actions.end(), action) == owner->actions.end()) {
      *r_why = "There is no action \"" + action + "\"";
      return false;
    }
    if (fix_action_find(*owner, action)) {
      *r_why = action + " is a fix: it plays by itself when its actions do";
      return false;
    }
    int in_side = 0;
    if (const Bone *in = controller_of_action(*owner, action, &in_side)) {
      *r_why = action + " is already " + in->name + "'s " + controller_side_name(in_side);
      return false;
    }
    if (const Bone *dial = controller_dial_of_action(*owner, action)) {
      int axis = 0;
      const Bone *linked = controller_of_dial(*owner, *dial, &axis);
      if (linked && linked != knob) {
        *r_why = dial->name + " is already linked to " + linked->name;
        return false;
      }
      if (linked && axis != controller_side_axis(side)) {
        *r_why = dial->name + " has one angle, so it can only be on one axis of " + knob->name;
        return false;
      }
    }
    if (keep != "THIS" && keep != "OTHER" && controller_side_keys_clash(*owner, *knob, side, action)) {
      *r_why = "Both have keys, and the knob has one place on an axis: say which to keep";
      return false;
    }
  }
  const int axis = controller_side_axis(side);
  const float sign = controller_side_sign(side);
  /* What it held lets go, moving as it did, and those keys leave the knob. */
  if (Bone *was = controller_side_dial(*owner, *knob, side)) {
    controller_bake_dial(*owner, *knob, *was);
    for (Key<float2> &k : knob->position_keys.keys) {
      float &along = axis == 0 ? k.value.x : k.value.y;
      along = along * sign > 0.0f ? 0.0f : along;
    }
  }
  Bone *mine = action.empty() ? nullptr : controller_dial_of_action(*owner, action);
  if (mine && keep == "THIS") {
    for (Key<float2> &k : knob->position_keys.keys) {
      (axis == 0 ? k.value.x : k.value.y) = 0.0f;
    }
  }
  else if (mine && keep == "OTHER") {
    mine->angle_keys.keys.clear();
  }
  knob->controller_actions[side] = action;
  controllers_update(doc);
  r_note->clear();
  if (mine && controller_side_dial(*owner, *knob, side) == mine) {
    if (const int moved = controller_fold_dial_keys(*owner, *knob, *mine); moved > 0) {
      *r_note = "Moved " + std::to_string(moved) + (moved == 1 ? " key" : " keys") + " from " + mine->name + " onto " + knob->name;
    }
  }
  animation_evaluate(doc, doc.frame_current);
  return true;
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
  add("shear", layer.shear_keys, layer.shear);
  add("opacity", layer.opacity_keys, layer.opacity);
  add("visible", layer.visible_keys, layer.visible_value);
  add("draw_order", layer.draw_order_keys, layer.draw_order);
  if (layer.is_switch()) {
    add("switch", layer.switch_keys, layer.switch_value);
  }
  for (Bone &bone : layer.bones) {
    const std::string base = "bones[\"" + bone.name + "\"].";
    /* Its keys hold Transform and Angle (see layer_for_each_channel); a change made through them
     * goes back with bone_values_from_changes. */
    bone_changes_from_values(bone);
    add(base + "position", bone.position_keys, bone.transform);
    add(base + "angle", bone.angle_keys, bone.turn);
    add(base + "scale", bone.scale_keys, bone.scale);
    add(base + "depth", bone.depth_keys, bone.depth);
    for (size_t i = 0; i < bone.constraints.size(); i++) {
      BoneConstraint &c = bone.constraints[i];
      const std::string cb = base + "constraints[" + std::to_string(i) + "].";
      add(cb + "influence", c.influence_keys, c.influence);
      if (c.type == BoneConstraintType::FollowPoint) {
        add(cb + "follow_shift", c.follow_shift_keys, c.follow_shift);
        add(cb + "follow_shift2", c.follow_shift2_keys, c.follow_shift2);
        add(cb + "follow_shift3", c.follow_shift3_keys, c.follow_shift3);
      }
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
        if (s == &c.scale_spring) {
          add(cb + name + "squash_limit", s->squash_limit_keys, s->squash_limit);
        }
      }
    }
  }
  for (CustomChannel &c : layer.custom_channels) {
    add(c.path, c.keys, c.value);
  }
  for (ActionPlay &a : layer.action_plays) {
    add(action_frame_path(a.name), a.frame_keys, a.frame);
  }
  /* A mask row's End Lines at Edge Reach. */
  for (size_t i = 0; i < layer.masks.size(); i++) {
    add("masks[" + std::to_string(i) + "].reach", layer.masks[i].line_reach_keys, layer.masks[i].line_reach);
  }
  /* A Boolean modifier's Smoothing. */
  for (size_t i = 0; i < layer.modifiers.size(); i++) {
    if (layer.modifiers[i].type == ModifierType::Boolean) {
      add("modifiers[" + std::to_string(i) + "].smoothing", layer.modifiers[i].merge_smoothing_keys,
          layer.modifiers[i].merge_smoothing);
    }
    if (layer.modifiers[i].type == ModifierType::Bevel) {
      add("modifiers[" + std::to_string(i) + "].amount", layer.modifiers[i].bevel_amount_keys,
          layer.modifiers[i].bevel_amount);
    }
    if (modifier_is_deform(layer.modifiers[i].type)) {
      add("modifiers[" + std::to_string(i) + "].strength", layer.modifiers[i].deform_strength_keys,
          layer.modifiers[i].deform_strength);
    }
    if (layer.modifiers[i].type == ModifierType::FollowDeform) {
      add("modifiers[" + std::to_string(i) + "].shift", layer.modifiers[i].deform_shift_keys, layer.modifiers[i].deform_shift);
      add("modifiers[" + std::to_string(i) + "].shift2", layer.modifiers[i].deform_shift2_keys, layer.modifiers[i].deform_shift2);
    }
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

/* layer_channel for the channels drivers name most (the layer's own, a bone's and its constraints',
 * custom ones), found by their path without listing every channel of the layer: a rig with many
 * drivers asks for them many times a frame (their settings, which aren't channels, too). False when
 * it isn't a channel; `r_final` says whether that's the answer (any bone's path: the full list has no
 * other bone channels) or the full list should be looked through. */
static bool layer_channel_direct(Layer &layer, const std::string &path, ChannelRef *r_ref, bool *r_final)
{
  *r_final = true;
  const auto take = [&](auto &ch, auto &value) {
    *r_ref = ChannelRef();
    r_ref->path = path;
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, float>) {
      r_ref->scalar = &ch;
      r_ref->scalar_value = &value;
    }
    else {
      r_ref->vector = &ch;
      r_ref->vector_value = &value;
    }
    return true;
  };
  if (path == "position") return take(layer.position_keys, layer.position);
  if (path == "rotation") return take(layer.rotation_keys, layer.rotation);
  if (path == "scale") return take(layer.scale_keys, layer.scale);
  if (path == "shear") return take(layer.shear_keys, layer.shear);
  if (path == "opacity") return take(layer.opacity_keys, layer.opacity);
  if (path == "visible") return take(layer.visible_keys, layer.visible_value);
  if (path == "draw_order") return take(layer.draw_order_keys, layer.draw_order);
  if (path == "switch" && layer.is_switch()) return take(layer.switch_keys, layer.switch_value);
  if (path.rfind("bones[\"", 0) == 0) {
    const size_t end = path.find("\"].", 7);
    if (end == std::string::npos) {
      return false;
    }
    const std::string_view name(path.data() + 7, end - 7);
    Bone *bone = nullptr;
    for (Bone &b : layer.bones) {
      if (b.name == name) {
        bone = &b;
        break;
      }
    }
    if (!bone) {
      return false;
    }
    const std::string_view rest(path.data() + end + 3, path.size() - end - 3);
    /* Its keys hold Transform and Angle, as layer_channels has them. */
    bone_changes_from_values(*bone);
    if (rest == "position") return take(bone->position_keys, bone->transform);
    if (rest == "angle") return take(bone->angle_keys, bone->turn);
    if (rest == "scale") return take(bone->scale_keys, bone->scale);
    if (rest == "depth") return take(bone->depth_keys, bone->depth);
    constexpr std::string_view prefix = "constraints[";
    if (rest.substr(0, prefix.size()) != prefix) {
      return false;
    }
    const size_t close = rest.find("].");
    if (close == std::string_view::npos) {
      return false;
    }
    const size_t i = size_t(std::strtoull(std::string(rest.substr(prefix.size(), close - prefix.size())).c_str(), nullptr, 10));
    if (i >= bone->constraints.size()) {
      return false;
    }
    BoneConstraint &c = bone->constraints[i];
    const std::string_view field = rest.substr(close + 2);
    if (field == "influence") return take(c.influence_keys, c.influence);
    if (field == "follow_shift") return take(c.follow_shift_keys, c.follow_shift);
    if (field == "follow_shift2") return take(c.follow_shift2_keys, c.follow_shift2);
    if (field == "follow_shift3") return take(c.follow_shift3_keys, c.follow_shift3);
    if (field == "weight") return take(c.weight_keys, c.weight);
    if (field == "wind_catch") return take(c.wind_catch_keys, c.wind_catch);
    if (field == "gravity") return take(c.gravity_keys, c.gravity);
    if (field == "wind") return take(c.wind_keys, c.wind);
    if (field == "wind_direction") return take(c.wind_direction_keys, c.wind_direction);
    if (field == "wind_gusts") return take(c.wind_gusts_keys, c.wind_gusts);
    if (field == "wind_gust_speed") return take(c.wind_gust_speed_keys, c.wind_gust_speed);
    if (field == "wind_turbulence") return take(c.wind_turbulence_keys, c.wind_turbulence);
    for (const auto &[spring_name, spring] : {std::pair<std::string_view, BoneSpring *>{"angle_spring.", &c.angle_spring},
                                              {"position_spring.", &c.position_spring},
                                              {"scale_spring.", &c.scale_spring}})
    {
      if (field.substr(0, spring_name.size()) != spring_name) {
        continue;
      }
      const std::string_view what = field.substr(spring_name.size());
      if (what == "settle_speed") return take(spring->settle_speed_keys, spring->settle_speed);
      if (what == "bounce") return take(spring->bounce_keys, spring->bounce);
      if (what == "inertia") return take(spring->inertia_keys, spring->inertia);
      if (what == "stretch") return take(spring->stretch_keys, spring->stretch);
      if (what == "limit") return take(spring->limit_keys, spring->limit);
      if (what == "squash_limit") return take(spring->squash_limit_keys, spring->squash_limit);
    }
    return false;
  }
  for (CustomChannel &c : layer.custom_channels) {
    if (c.path == path) {
      return take(c.keys, c.value);
    }
  }
  *r_final = false;
  return false;
}

bool layer_channel(Layer &layer, const std::string &path, ChannelRef *r_ref)
{
  bool final = false;
  if (layer_channel_direct(layer, path, r_ref, &final)) {
    return true;
  }
  if (final) {
    return false;
  }
  /* Only a point's or curve's channel needs the drawing's (which works out every point's change). */
  const bool drawing = path.rfind("points[", 0) == 0 || path.rfind("curves[", 0) == 0;
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
    /* Keys hold the value without what action bone actions add. */
    const auto base = value - ch.from_action_bones;
    if (!ch.animated() && frame != 0) {
      ch.set_key(0, ch.has_last ? ch.last - ch.from_action_bones : base);
    }
    ch.set_key(frame, base);
    ch.last = value;
    ch.has_last = true;
    count++;
  });
  return count;
}

int layer_insert_filtered_keys(Layer &layer, const KeyFilter &filter, int frame)
{
  int count = 0;
  const auto key = [&](auto &ch, auto &value) {
    /* As layer_insert_keys: a setting keyed only by hand (a mask's Reach) waits to be keyed so. */
    if (ch.keyed_by_hand && !ch.animated()) {
      return;
    }
    const auto base = value - ch.from_action_bones;
    if (!ch.animated() && frame != 0) {
      ch.set_key(0, ch.has_last ? ch.last - ch.from_action_bones : base);
    }
    ch.set_key(frame, base);
    ch.last = value;
    ch.has_last = true;
    count++;
  };
  for (const ChannelRef &ref : layer_filtered_channels(layer, filter)) {
    if (ref.scalar) {
      key(*ref.scalar, *ref.scalar_value);
    }
    else if (ref.vector) {
      key(*ref.vector, *ref.vector_value);
    }
  }
  return count;
}

/* As in Blender, a property with no keys left isn't animated: once only the setup key (frame 0)
 * is left, it goes too, and the value stays what frame 0 had. */
template<typename T> static void drop_lone_setup_key(Channel<T> &ch, T &value)
{
  if (ch.keys.size() == 1 && ch.keys.front().frame == 0) {
    value = ch.keys.front().value + ch.from_action_bones;
    ch.last = value;
    ch.keys.clear();
  }
}

template<typename T> static bool delete_key(Channel<T> &ch, T &value, int frame)
{
  if (frame == 0 && ch.keys.size() > 1) {
    return false;
  }
  const bool removed = ch.remove_key(frame);
  drop_lone_setup_key(ch, value);
  return removed;
}

bool channel_delete_key(Channel<float> &ch, float &value, int frame)
{
  return delete_key(ch, value, frame);
}

bool channel_delete_key(Channel<float2> &ch, float2 &value, int frame)
{
  return delete_key(ch, value, frame);
}

int layer_toggle_keys(Layer &layer, const std::vector<std::string> &paths, int frame, int mode)
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
  if (mode != 0) {
    keyed = mode < 0;
  }
  int count = 0;
  const auto toggle = [&](auto &ch, auto &value) {
    if (keyed) {
      count += delete_key(ch, value, frame) ? 1 : 0;
      return;
    }
    const auto base = value - ch.from_action_bones;
    if (!ch.animated() && frame != 0) {
      /* The setup key takes the value from before an edit not yet keyed. */
      ch.set_key(0, ch.has_last ? ch.last - ch.from_action_bones : base);
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

int layer_action_bone_state(Layer &layer, const std::vector<std::string> &paths)
{
  for (const std::string &path : paths) {
    ChannelRef ref;
    if (layer_channel(layer, path, &ref) &&
        (ref.scalar ? ref.scalar->action_bone_live : ref.vector->action_bone_live))
    {
      return KEY_STATE_ACTION_BONE;
    }
  }
  return 0;
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
  layer_for_each_channel(layer, [&](auto &ch, auto &value) { count += delete_key(ch, value, frame) ? 1 : 0; });
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

bool layer_has_keys(const Layer &layer)
{
  bool any = false;
  layer_for_each_channel<false>(const_cast<Layer &>(layer), [&](auto &ch, auto & /*value*/) { any |= !ch.keys.empty(); });
  return any;
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
      {"shear", "Shear"},
      {"depth", "Depth"},
      {"opacity", "Opacity"},
      {"visible", "Visibility"},
      {"draw_order", "Draw Order"},
      {"switch", "Switch"},
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
  if (path.rfind("masks[", 0) == 0) {
    /* "Mask 1: Reach". */
    const std::string name = "Mask " + std::to_string(std::atoi(path.c_str() + 6) + 1) + ": Reach";
    return owner ? layer.name + ": " + name : name;
  }
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
        setting = what == "limit"          ? std::string(limit)
                  : what == "squash_limit" ? std::string("Max Squash")
                                           : std::string(channel) + " " + name_of(what);
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
  if (path == "visible" || path == "switch") {
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
static bool channel_thing_selected(const Layer &layer, const std::string &path, const KeyFilter &filter)
{
  if (layer.id == CAMERA_LAYER_ID) {
    return layer.selected;
  }
  if ((path.rfind("points[", 0) == 0 || path.rfind("curves[", 0) == 0) && filter.point_layers &&
      !filter.point_layers->contains(layer.id))
  {
    return false;
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
  /* Pinned ones are asked of every layer every frame (the timeline's Pinned row): most have none. */
  if (filter.pinned_only) {
    bool any = false;
    layer_for_each_channel<false>(layer, [&](auto &ch, auto & /*value*/) { any |= ch.pinned; });
    if (!any) {
      return refs;
    }
  }
  /* A drawing's points and curves are only needed when their kind is asked for. */
  const bool drawing = (filter.kinds & KEY_POINTS) &&
                       (filter.path.empty() || filter.path.rfind("points[", 0) == 0 ||
                        filter.path.rfind("curves[", 0) == 0);
  const std::vector<ChannelRef> channels = layer_channels(layer, drawing);
  for (const ChannelRef &ref : channels) {
    const KeyKind kind = channel_kind(ref.path);
    if (!(filter.kinds & kind) || !key_filter_path_matches(filter, ref.path)) {
      continue;
    }
    if (filter.pinned_only && !(ref.scalar ? ref.scalar->pinned : ref.vector->pinned)) {
      continue;
    }
    if (filter.selected_only && !channel_thing_selected(layer, ref.path, filter)) {
      continue;
    }
    if (filter.picked_bones_only && ref.path.rfind("bones[\"", 0) == 0 && !channel_thing_selected(layer, ref.path, filter)) {
      continue;
    }
    refs.push_back(ref);
  }
  if (filter.pinned_only) {
    return refs;
  }
  /* A dial linked to a controller keeps no keys of its own: asked for its angle, the knob's are the ones, so its row in
   * the timeline is the same keys and an edit to them is an edit to both. */
  for (const Bone &knob : layer.bones) {
    if (!bone_is_controller(knob)) {
      continue;
    }
    const std::string knob_path = "bones[\"" + knob.name + "\"].position";
    for (int side = 0; side < CONTROLLER_SIDES; side++) {
      const Bone *dial = controller_side_dial(layer, knob, side);
      if (!dial) {
        continue;
      }
      const std::string dial_path = "bones[\"" + dial->name + "\"].angle";
      if (!(filter.kinds & channel_kind(dial_path)) || !key_filter_path_matches(filter, dial_path) ||
          (filter.selected_only && !channel_thing_selected(layer, dial_path, filter)) ||
          (filter.picked_bones_only && !channel_thing_selected(layer, dial_path, filter)) ||
          std::any_of(refs.begin(), refs.end(), [&](const ChannelRef &r) { return r.path == knob_path; }))
      {
        continue;
      }
      for (const ChannelRef &ref : channels) {
        if (ref.path == knob_path) {
          refs.push_back(ref);
        }
      }
    }
  }
  return refs;
}

/* Calls f(channel) for every channel of `layers` that passes the filter. */
template<typename F> static void filtered_for_each(const std::vector<Layer *> &layers, const KeyFilter &filter, F &&f);

bool layers_channel_among(const std::vector<Layer *> &layers, const KeyFilter &filter,
                          const std::set<const void *> &channels)
{
  bool found = false;
  if (!channels.empty()) {
    filtered_for_each(layers, filter, [&](auto &ch) { found = found || channels.count(&ch) > 0; });
  }
  return found;
}

template<typename F> static void filtered_for_each(const std::vector<Layer *> &layers, const KeyFilter &filter, F &&f)
{
  for (Layer *layer : layers) {
    if (filter.everything()) {
      layer_for_each_channel<false>(*layer, [&](auto &ch, auto & /*value*/) { f(ch); });
      continue;
    }
    /* A drawing's channels are all points' (KEY_POINTS), so without a path they are gone through as they
     * are, not named one by one: the timeline asks this of every row each frame, and a big rig has
     * hundreds of thousands. The layer's own channels (a few) go by name. */
    if (filter.path.empty() && !filter.pinned_only && !layer->is_reference() && layer->id != CAMERA_LAYER_ID) {
      KeyFilter own = filter;
      own.kinds &= ~uint32_t(KEY_POINTS);
      if (own.kinds != 0) {
        for (const ChannelRef &ref : layer_filtered_channels(*layer, own)) {
          if (ref.scalar) {
            f(*ref.scalar);
          }
          else {
            f(*ref.vector);
          }
        }
      }
      if (!(filter.kinds & KEY_POINTS) ||
          (filter.selected_only && filter.point_layers && !filter.point_layers->contains(layer->id)))
      {
        continue;
      }
      VectorDrawing &drawing = layer->drawing;
      for (MeshPoint &p : drawing.points) {
        if (!filter.selected_only || p.selected) {
          f(p.pos_keys);
          f(p.radius_keys);
        }
      }
      for (Curve &curve : drawing.curves) {
        for (CurvePoint &cp : curve.points) {
          if (!filter.selected_only ||
              (cp.point >= 0 && cp.point < int(drawing.points.size()) && drawing.points[cp.point].selected))
          {
            f(cp.curvature_keys);
            f(cp.handle_in_keys);
            f(cp.handle_out_keys);
          }
        }
      }
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
    case Interp::Bezier:
      return KEY_SHAPE_EASE;
    case Interp::Back:
    case Interp::Bounce:
    case Interp::Elastic:
      return KEY_SHAPE_BOUNCY;
    case Interp::Noisy:
    case Interp::Cycle:
    case Interp::Pose:
    case Interp::Custom:
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
      s.breakdowns += key.type == KeyType::Breakdown ? 1 : 0;
      s.fillers += key.filler ? 1 : 0;
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
    layer_for_each_channel<false>(*layer, [&](auto &ch, auto & /*value*/) {
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
          /* The stretch arriving at it, after the key before's hold. */
          const size_t i = size_t(&key - ch.keys.data());
          if (i > 0) {
            const auto &before = ch.keys[i - 1];
            r.first_span = std::max(key.frame - std::min(before.frame + std::max(before.hold, 0), key.frame), 0);
          }
        }
        else {
          r.mixed |= s.interp != r.first.interp;
          r.mixed_ease |= s.ease != r.first.ease;
          r.mixed_type |= s.type != r.first.type;
          /* A few of the other curves, for the settings graph to draw behind the first's. */
          const auto curve_same = [&](const KeySettings &o) {
            return o.interp == s.interp && o.ease == s.ease && o.bezier == s.bezier && o.overshoot == s.overshoot;
          };
          if (!curve_same(r.first) && r.others.size() < 6 &&
              std::none_of(r.others.begin(), r.others.end(), curve_same))
          {
            r.others.push_back(s);
          }
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

std::vector<bool *> layers_key_selected_flags(const std::vector<Layer *> &layers, int frame, const KeyFilter &filter)
{
  std::vector<bool *> out;
  filtered_for_each(layers, filter, [&](auto &ch) {
    for (auto &key : ch.keys) {
      if (key.frame == frame) {
        out.push_back(&key.selected);
      }
    }
  });
  return out;
}

std::set<int> layers_filtered_key_frames(const std::vector<Layer *> &layers, const KeyFilter &filter)
{
  std::set<int> out;
  filtered_for_each(layers, filter, [&](auto &ch) {
    for (const auto &key : ch.keys) {
      out.insert(key.frame);
    }
  });
  return out;
}

std::set<const void *> layers_filtered_channel_set(const std::vector<Layer *> &layers, const KeyFilter &filter)
{
  std::set<const void *> out;
  filtered_for_each(layers, filter, [&](auto &ch) { out.insert(&ch); });
  return out;
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


bool layers_key_selected_outside(const std::vector<Layer *> &layers, const std::set<const void *> &channels)
{
  bool any = false;
  for (Layer *layer : layers) {
    layer_for_each_channel<false>(*layer, [&](auto &ch, auto & /*value*/) {
      if (any || ch.constant) {
        return;
      }
      const bool among = channels.count(&ch) > 0;
      for (const auto &key : ch.keys) {
        if (key.selected && (!among || key.frame == 0)) {
          any = true;
          return;
        }
      }
    });
    if (any) {
      break;
    }
  }
  return any;
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

void layers_move_selected_keys(const std::vector<Layer *> &layers, const std::function<int(int)> &to, bool ripple)
{
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      using KeyT = std::decay_t<decltype(ch.keys.front())>;
      /* Ripple: the keys after the last one moved go along by as much, so the rest keeps its timing. */
      if (ripple) {
        int last = -1, shift = 0;
        for (const KeyT &key : ch.keys) {
          if (key.selected && key.frame != 0) {
            last = key.frame;
            shift = std::max(1, to(key.frame)) - key.frame;
          }
        }
        if (last >= 0 && shift != 0) {
          for (KeyT &key : ch.keys) {
            if (!key.selected && key.frame > last) {
              key.frame += shift;
            }
          }
        }
      }
      std::vector<KeyT> moved, kept;
      const auto dest = [&](const KeyT &key) {
        return key.selected && key.frame != 0 ? std::max(1, to(key.frame)) : key.frame;
      };
      for (size_t i = 0; i < ch.keys.size(); i++) {
        const KeyT &key = ch.keys[i];
        if (key.selected && key.frame != 0) {
          KeyT k = key;
          k.frame = dest(key);
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

/* --- Time Together ------------------------------------------------------------------------ */

namespace {

/* A channel of either kind. */
struct AnyChannel {
  Channel<float> *s = nullptr;
  Channel<float2> *v = nullptr;
  const void *id() const { return s ? static_cast<const void *>(s) : static_cast<const void *>(v); }
};

template<typename F> void any_visit(const AnyChannel &c, F &&f)
{
  if (c.s) {
    f(*c.s);
  }
  else {
    f(*c.v);
  }
}

template<typename T> void any_add(std::vector<AnyChannel> &out, Channel<T> &ch)
{
  if constexpr (std::is_same_v<T, float>) {
    out.push_back({&ch, nullptr});
  }
  else {
    out.push_back({nullptr, &ch});
  }
}

void layers_walk(std::vector<Layer> &layers, const std::function<void(Layer &)> &f)
{
  for (Layer &l : layers) {
    f(l);
    layers_walk(l.children, f);
  }
}

/* An edited stretch of a channel and how its frames go: from `lo` to `hi` (old frames), through `map` (old frame to
 * new, in order, the ends included); past `hi`, keys move by `after` when `open` (Ripple, or the channel's last key
 * moved). */
struct Stretch {
  int lo = 0, hi = 0;
  std::vector<std::pair<int, int>> map;
  bool open = false;
  int after = 0;
  bool operator==(const Stretch &) const = default;
  float to(int f) const
  {
    if (f > hi) {
      return float(f + (open ? after : 0));
    }
    for (size_t i = 1; i < map.size(); i++) {
      const auto [a, na] = map[i - 1];
      const auto [b, nb] = map[i];
      if (f <= b) {
        return b == a ? float(nb) : float(na) + float(nb - na) * float(f - a) / float(b - a);
      }
    }
    return float(map.back().second);
  }
};

/* The stretches a channel's selected keys edit when they move to `to(frame)`: from the unmoved key before each run of
 * selected keys to the unmoved key after it (or the run's own end key where there is none, or with Ripple on the
 * last run). */
template<typename KeyT>
std::vector<Stretch> edited_stretches(const std::vector<KeyT> &keys, const std::function<int(int)> &to, bool ripple)
{
  std::vector<Stretch> out;
  const auto moves = [&](const KeyT &k) { return k.selected && k.frame != 0; };
  const auto dest = [&](const KeyT &k) { return moves(k) ? std::max(1, to(k.frame)) : k.frame; };
  size_t last_moved = keys.size();
  for (size_t i = 0; i < keys.size(); i++) {
    if (moves(keys[i])) {
      last_moved = i;
    }
  }
  for (size_t i = 0; i < keys.size();) {
    if (!moves(keys[i])) {
      i++;
      continue;
    }
    size_t j = i;
    while (j + 1 < keys.size() && moves(keys[j + 1])) {
      j++;
    }
    Stretch st;
    if (i > 0) {
      st.lo = keys[i - 1].frame;
      st.map.push_back({st.lo, st.lo});
    }
    else {
      st.lo = keys[i].frame;
    }
    for (size_t k = i; k <= j; k++) {
      st.map.push_back({keys[k].frame, dest(keys[k])});
    }
    const bool rippled = ripple && j == last_moved;
    if (j + 1 < keys.size() && !rippled) {
      st.hi = keys[j + 1].frame;
      st.map.push_back({st.hi, st.hi});
    }
    else {
      st.hi = keys[j].frame;
      st.open = true;
      st.after = dest(keys[j]) - keys[j].frame;
    }
    if (st.hi > st.lo) {
      out.push_back(std::move(st));
    }
    i = j + 1;
  }
  return out;
}

/* Whether the channel is keyed on both ends of the stretch: it is timed with it. */
template<typename Ch> bool stretch_linked(const Ch &ch, const Stretch &st)
{
  return ch.keys.size() >= 2 && ch.key_at(st.lo) && ch.key_at(st.hi);
}

/* Moves the channel's keys through the stretches (each key by the first it falls in), to whole frames, keeping each a
 * frame apart from the next (keys still landing together: the later wins). */
template<typename Ch> void stretches_apply(Ch &ch, const std::vector<const Stretch *> &sts)
{
  using KeyT = std::decay_t<decltype(ch.keys.front())>;
  const size_t n = ch.keys.size();
  std::vector<int> to(n);
  std::vector<bool> mapped(n, false);
  for (size_t i = 0; i < n; i++) {
    const int f = ch.keys[i].frame;
    to[i] = f;
    if (f == 0) {
      continue;
    }
    for (const Stretch *st : sts) {
      if (f > st->lo && (f <= st->hi || st->open)) {
        to[i] = std::max(1, int(std::lround(st->to(f))));
        mapped[i] = true;
        break;
      }
    }
  }
  /* Rounding can land two on one frame: forward, then back from the unmoved keys. */
  for (size_t i = 1; i < n; i++) {
    if (mapped[i] && to[i] <= to[i - 1]) {
      to[i] = to[i - 1] + 1;
    }
  }
  for (size_t i = n - 1; i-- > 0;) {
    if (mapped[i] && to[i] >= to[i + 1] && to[i + 1] - 1 > (i > 0 ? to[i - 1] : -1)) {
      to[i] = to[i + 1] - 1;
    }
  }
  std::map<int, KeyT> landed;
  for (size_t i = 0; i < n; i++) {
    KeyT k = ch.keys[i];
    k.frame = to[i];
    landed[k.frame] = std::move(k);
  }
  ch.keys.clear();
  for (auto &[f, k] : landed) {
    ch.keys.push_back(std::move(k));
  }
}

bool any_selected(const AnyChannel &c)
{
  bool sel = false;
  any_visit(c, [&](auto &ch) {
    for (const auto &k : ch.keys) {
      sel = sel || (k.selected && k.frame != 0);
    }
  });
  return sel;
}

/* The channels Time Together can link (see time_together_candidates). */
std::vector<AnyChannel> together_candidates(Document &doc)
{
  std::vector<AnyChannel> out;
  layers_walk(doc.layers, [&](Layer &l) {
    if (!l.selected) {
      return;
    }
    any_add(out, l.position_keys);
    any_add(out, l.rotation_keys);
    any_add(out, l.scale_keys);
    any_add(out, l.shear_keys);
    any_add(out, l.opacity_keys);
    for (Bone &b : l.bones) {
      if (b.selected) {
        any_add(out, b.position_keys);
        any_add(out, b.angle_keys);
        any_add(out, b.scale_keys);
        any_add(out, b.depth_keys);
      }
    }
    if (l.has_drawing() && !l.is_reference()) {
      drawing_shape_channels(l.drawing, [&](auto &ch, auto & /*value*/) { any_add(out, ch); });
    }
  });
  return out;
}

}  // namespace

std::vector<const void *> time_together_candidates(Document &doc)
{
  std::vector<const void *> ids;
  for (const AnyChannel &c : together_candidates(doc)) {
    ids.push_back(c.id());
  }
  return ids;
}

/* The stretches the selected keys of `layers` edit, moved by `to`. */
static std::vector<Stretch> selected_stretches(const std::vector<Layer *> &layers, const std::function<int(int)> &to,
                                               bool ripple)
{
  std::vector<Stretch> all;
  for (Layer *layer : layers) {
    layer_for_each_channel<false>(*layer, [&](auto &ch, auto & /*value*/) {
      for (Stretch &st : edited_stretches(ch.keys, to, ripple)) {
        if (std::find(all.begin(), all.end(), st) == all.end()) {
          all.push_back(std::move(st));
        }
      }
    });
  }
  return all;
}

std::set<const void *> time_together_linked(Document &doc, const std::vector<Layer *> &layers)
{
  std::set<const void *> out;
  if (!doc.time_together) {
    return out;
  }
  const std::vector<Stretch> sts = selected_stretches(layers, [](int f) { return f; }, false);
  if (sts.empty()) {
    return out;
  }
  for (const AnyChannel &c : together_candidates(doc)) {
    if (any_selected(c)) {
      continue;
    }
    any_visit(c, [&](auto &ch) {
      for (const Stretch &st : sts) {
        if (stretch_linked(ch, st)) {
          out.insert(c.id());
          break;
        }
      }
    });
  }
  return out;
}

int keys_move_selected(Document &doc, const std::vector<Layer *> &layers, const std::function<int(int)> &to,
                       bool ripple, bool together)
{
  std::vector<Stretch> sts;
  std::vector<AnyChannel> linked;
  if (together && doc.time_together) {
    sts = selected_stretches(layers, to, ripple);
    if (!sts.empty()) {
      for (const AnyChannel &c : together_candidates(doc)) {
        if (!any_selected(c)) {
          linked.push_back(c);
        }
      }
    }
  }
  layers_move_selected_keys(layers, to, ripple);
  int followed = 0;
  for (const AnyChannel &c : linked) {
    any_visit(c, [&](auto &ch) {
      std::vector<const Stretch *> mine;
      for (const Stretch &st : sts) {
        if (stretch_linked(ch, st)) {
          mine.push_back(&st);
        }
      }
      if (!mine.empty()) {
        stretches_apply(ch, mine);
        followed++;
      }
    });
  }
  return followed;
}

std::pair<int, int> keys_span_frames_add(Document &doc, Layer &layer, int frame, int amount, bool together)
{
  if (amount == 0) {
    return {0, 0};
  }
  std::vector<Stretch> sts;
  std::set<const void *> done;
  int changed = 0;
  layer_for_each_channel<false>(layer, [&](auto &ch, auto & /*value*/) {
    const auto &keys = ch.keys;
    for (size_t i = 0; i + 1 < keys.size(); i++) {
      const int a = keys[i].frame, b = keys[i + 1].frame;
      if (a <= frame && frame < b) {
        if (b - a + amount < 1) {
          return;
        }
        Stretch st;
        st.lo = a;
        st.hi = b;
        st.map = {{a, a}, {b, b + amount}};
        st.open = true;
        st.after = amount;
        stretches_apply(ch, {&st});
        done.insert(static_cast<const void *>(&ch));
        changed++;
        if (std::find(sts.begin(), sts.end(), st) == sts.end()) {
          sts.push_back(std::move(st));
        }
        return;
      }
    }
  });
  int followed = 0;
  if (together && doc.time_together && !sts.empty()) {
    for (const AnyChannel &c : together_candidates(doc)) {
      if (done.count(c.id())) {
        continue;
      }
      any_visit(c, [&](auto &ch) {
        for (const Stretch &st : sts) {
          if (stretch_linked(ch, st)) {
            stretches_apply(ch, {&st});
            followed++;
            return;
          }
        }
      });
    }
  }
  return {changed, followed};
}

std::pair<int, int> keys_match_timing(Document &doc, Layer &source)
{
  std::set<int> frames;
  std::set<const void *> own;
  layer_for_each_channel<false>(source, [&](auto &ch, auto & /*value*/) {
    own.insert(static_cast<const void *>(&ch));
    for (const auto &k : ch.keys) {
      frames.insert(k.frame);
    }
  });
  if (frames.size() < 2) {
    return {0, 0};
  }
  const std::vector<int> from(frames.begin(), frames.end());
  const float f0 = float(from.front()), span = float(from.back() - from.front());
  int matched = 0, merged = 0;
  for (const AnyChannel &c : together_candidates(doc)) {
    if (own.count(c.id())) {
      continue;
    }
    any_visit(c, [&](auto &ch) {
      using KeyT = std::decay_t<decltype(ch.keys.front())>;
      if (ch.keys.size() < 2) {
        return;
      }
      /* The source's frames stretched over this channel's first to last key. */
      const int t0 = ch.keys.front().frame, t1 = ch.keys.back().frame;
      std::vector<int> to;
      for (const int f : from) {
        const int g = int(std::lround(float(t0) + (float(f) - f0) / span * float(t1 - t0)));
        if (to.empty() || g != to.back()) {
          to.push_back(g);
        }
      }
      std::vector<int> now;
      for (const auto &k : ch.keys) {
        now.push_back(k.frame);
      }
      if (now == to) {
        return;
      }
      const auto old = ch;
      std::map<int, KeyT> placed;
      std::map<int, int> distance;
      if (ch.keys.size() == to.size()) {
        /* As many keys as frames: each goes to its frame, pose and timing kept. */
        for (size_t i = 0; i < to.size(); i++) {
          KeyT k = ch.keys[i];
          k.frame = to[i];
          placed[k.frame] = k;
        }
      }
      else {
        /* Each key to the nearest frame (the nearest key wins a frame two reach)... */
        for (const KeyT &k : ch.keys) {
          int best = to.front();
          for (const int g : to) {
            if (std::abs(g - k.frame) < std::abs(best - k.frame)) {
              best = g;
            }
          }
          const int d = std::abs(best - k.frame);
          if (placed.count(best) && distance[best] <= d) {
            merged++;
            continue;
          }
          if (placed.count(best)) {
            merged++;
          }
          KeyT moved = k;
          moved.frame = best;
          placed[best] = moved;
          distance[best] = d;
        }
        /* ...and the frames no key reached keyed with what the channel held there. */
        for (const int g : to) {
          if (!placed.count(g)) {
            KeyT k = ch.keys.front();
            k.frame = g;
            k.value = old.evaluate(g);
            k.selected = false;
            k.filler = false;
            k.filler_inside = false;
            placed[g] = k;
          }
        }
      }
      ch.keys.clear();
      for (auto &[f, k] : placed) {
        ch.keys.push_back(k);
      }
      matched++;
    });
  }
  return {matched, merged};
}

int keys_offset_clamp(const std::vector<Layer *> &layers, int offset, bool ripple)
{
  int lo = INT32_MIN, hi = INT32_MAX;
  for (Layer *layer : layers) {
    layer_for_each_channel<false>(*layer, [&](auto &ch, auto & /*value*/) {
      const auto &keys = ch.keys;
      size_t last = keys.size();
      for (size_t i = 0; i < keys.size(); i++) {
        if (keys[i].selected && keys[i].frame != 0) {
          last = i;
        }
      }
      for (size_t i = 0; i < keys.size(); i++) {
        if (!keys[i].selected || keys[i].frame == 0) {
          continue;
        }
        /* Never onto frame 0 or past the unselected key before; the one after too, unless it goes along. */
        const int before = i > 0 && !keys[i - 1].selected ? keys[i - 1].frame : (i == 0 ? 0 : INT32_MIN);
        if (before != INT32_MIN) {
          lo = std::max(lo, before + 1 - keys[i].frame);
        }
        lo = std::max(lo, 1 - keys[i].frame);
        if (i + 1 < keys.size() && !keys[i + 1].selected && !(ripple && i == last)) {
          hi = std::min(hi, keys[i + 1].frame - 1 - keys[i].frame);
        }
      }
    });
  }
  if (lo > hi) {
    return 0;
  }
  return std::clamp(offset, lo, hi);
}

TimingSnapshot time_together_timing_snapshot(Document &doc)
{
  TimingSnapshot snap;
  layers_walk(doc.layers, [&](Layer &l) {
    layer_for_each_channel<false>(l, [&](auto &ch, auto & /*value*/) {
      if (ch.keys.empty()) {
        return;
      }
      auto &keys = snap.keys[static_cast<const void *>(&ch)];
      for (const auto &k : ch.keys) {
        keys.push_back({k.frame, key_sig(k, false)});
      }
    });
  });
  return snap;
}

/* A channel's key whose timing changed since `before`, with the frames of the keys either side (-1: none). */
struct TimingEdit {
  int frame, prev, next;
  AnyChannel from;
};

int time_together_timing_follow(Document &doc, const TimingSnapshot &before)
{
  std::vector<TimingEdit> edits;
  std::set<const void *> edited;
  layers_walk(doc.layers, [&](Layer &l) {
    layer_for_each_channel<false>(l, [&](auto &ch, auto & /*value*/) {
      const auto it = before.keys.find(static_cast<const void *>(&ch));
      if (it == before.keys.end()) {
        return;
      }
      for (size_t i = 0; i < ch.keys.size(); i++) {
        const auto &k = ch.keys[i];
        const auto was = std::find_if(it->second.begin(), it->second.end(),
                                      [&](const auto &p) { return p.first == k.frame; });
        if (was == it->second.end() || was->second == key_sig(k, false)) {
          continue;
        }
        AnyChannel from;
        if constexpr (std::is_same_v<std::decay_t<decltype(ch)>, Channel<float>>) {
          from.s = &ch;
        }
        else {
          from.v = &ch;
        }
        edits.push_back({k.frame, i > 0 ? ch.keys[i - 1].frame : -1,
                         i + 1 < ch.keys.size() ? ch.keys[i + 1].frame : -1, from});
        edited.insert(static_cast<const void *>(&ch));
      }
    });
  });
  if (edits.empty()) {
    return 0;
  }
  int followed = 0;
  for (const AnyChannel &c : together_candidates(doc)) {
    if (edited.count(c.id())) {
      continue;
    }
    bool changed = false;
    any_visit(c, [&](auto &ch) {
      for (const TimingEdit &e : edits) {
        auto *k = ch.key_at(e.frame);
        /* Keyed on the same frames around it, and nothing between: the same stretches, timed alike. */
        if (!k || (e.prev >= 0 && !ch.key_at(e.prev)) || (e.next >= 0 && !ch.key_at(e.next))) {
          continue;
        }
        const int lo = e.prev >= 0 ? e.prev : e.frame, hi = e.next >= 0 ? e.next : e.frame;
        if (std::any_of(ch.keys.begin(), ch.keys.end(),
                        [&](const auto &o) { return o.frame > lo && o.frame < hi && o.frame != e.frame; }))
        {
          continue;
        }
        KeyTiming t;
        any_visit(e.from, [&](auto &src) { t = key_timing(*src.key_at(e.frame)); });
        if (key_timing(*k) != t) {
          key_timing_set(*k, t);
          changed = true;
        }
      }
    });
    followed += changed ? 1 : 0;
  }
  return followed;
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
  std::map<int, size_t> index;
  for (const auto &[frame, s] : layers_key_summary(layers, filter)) {
    index[frame] = frames.size();
    frames.push_back(frame);
  }
  if (frames.size() < 2) {
    return {};
  }
  /* For each gap between neighbouring keyed frames: whether every channel keyed on either side has the
   * same value on both (`holds`), and whether any does (`any`). Each channel's keys are gone through
   * once (a recording keys every frame, and the timeline asks for every row on every redraw). */
  std::vector<char> holds(frames.size() - 1, 1), any(frames.size() - 1, 0);
  filtered_for_each(layers, filter, [&](auto &ch) {
    const auto &keys = ch.keys;
    for (size_t j = 0; j < keys.size(); j++) {
      const auto it = index.find(keys[j].frame);
      if (it == index.end()) {
        continue;
      }
      const size_t at = it->second;
      /* The gap after it: the channel's next key must be on the next keyed frame, holding the value. */
      if (at + 1 < frames.size()) {
        if (j + 1 < keys.size() && keys[j + 1].frame == frames[at + 1]) {
          if (keys[j + 1].value == keys[j].value) {
            any[at] = 1;
          }
          else {
            holds[at] = 0;
          }
        }
        else {
          holds[at] = 0;
        }
      }
      /* The gap before it, unless the key before already spoke for it. */
      if (at > 0 && !(j > 0 && keys[j - 1].frame == frames[at - 1])) {
        holds[at - 1] = 0;
      }
    }
  });
  std::vector<std::pair<int, int>> spans;
  for (size_t i = 0; i + 1 < frames.size(); i++) {
    if (holds[i] && any[i]) {
      spans.push_back({frames[i], frames[i + 1]});
    }
  }
  return spans;
}

/* --- Ignore Bones ------------------------------------------------------------------------------ */

bool layer_bones_move_points(const Document &doc, const Layer &layer)
{
  (void)doc;
  if (!layer.has_drawing() || layer.is_reference()) {
    return false;
  }
  return layer.points_follow_bone() ||
         std::any_of(layer.modifiers.begin(), layer.modifiers.end(),
                     [](const Modifier &m) { return m.type == ModifierType::Armature; });
}

int keys_ignore_bones(Document &doc, const Document &source, const std::vector<std::pair<uint64_t, uint64_t>> &layers,
                      const std::vector<std::pair<int, int>> &frames)
{
  int changed = 0;
  /* Each frame worked out on a copy (the document keeps the frame it shows). */
  std::map<int, Document> sources, results;
  const auto at = [](std::map<int, Document> &cache, const Document &from, int frame) -> Document & {
    auto it = cache.find(frame);
    if (it == cache.end()) {
      it = cache.emplace(frame, from).first;
      it->second.frame_current = frame;
      animation_evaluate(it->second, frame);
    }
    return it->second;
  };
  for (const auto &[from, to] : frames) {
    if (to <= 0 || from < 0) {
      continue;
    }
    for (const auto &[id, source_id] : layers) {
      Layer *layer = layer_find(doc, id);
      if (!layer || !layer_bones_move_points(doc, *layer)) {
        continue;
      }
      const Document &was = at(sources, source, from);
      const Document &now = at(results, doc, to);
      const Layer *before = layer_find(was, source_id);
      const Layer *after = layer_find(now, id);
      if (!before || !after || before->is_reference() ||
          before->drawing.points.size() != layer->drawing.points.size() ||
          before->drawing.curves.size() != layer->drawing.curves.size())
      {
        continue;
      }
      std::vector<Transform2D> skins_before = layer_point_skins(was, *before);
      std::vector<Transform2D> skins_after = layer_point_skins(now, *after);
      skins_before.resize(layer->drawing.points.size());
      skins_after.resize(layer->drawing.points.size());
      /* From where a point is stored on the new frame to where it showed on the source frame, through
       * the bones of each: null where the new frame's bones flatten it. */
      const auto back = [&](int point, Transform2D *r) {
        const Transform2D &t = skins_after[size_t(point)];
        if (std::fabs(t.a * t.d - t.b * t.c) < 1e-8f) {
          return false;
        }
        *r = t.inverted() * skins_before[size_t(point)];
        return true;
      };
      for (size_t i = 0; i < layer->drawing.points.size(); i++) {
        MeshPoint &p = layer->drawing.points[i];
        Key<float2> *key = p.pos_keys.key_at(to);
        Transform2D t;
        if (!key || !key->selected || p.detail.level > 0 || !back(int(i), &t)) {
          continue;
        }
        const MeshPoint &shown = after->drawing.points[i];
        const float2 pos = t.apply(before->drawing.points[i].pos);
        key->value = pos - shown.setup_pos - shown.pos_keys.from_action_bones;
        changed++;
      }
      for (size_t c = 0; c < layer->drawing.curves.size(); c++) {
        Curve &curve = layer->drawing.curves[c];
        const Curve &curve_before = before->drawing.curves[c];
        const Curve &curve_after = after->drawing.curves[c];
        if (curve.points.size() != curve_before.points.size() || curve.points.size() != curve_after.points.size()) {
          continue;
        }
        for (size_t j = 0; j < curve.points.size(); j++) {
          CurvePoint &cp = curve.points[j];
          Transform2D t;
          /* A handle the curve works out for itself follows the points it is worked out from. */
          if (!curve_before.points[j].handles_set || layer->drawing.points[size_t(cp.point)].detail.level > 0 ||
              !back(cp.point, &t))
          {
            continue;
          }
          for (const bool in : {true, false}) {
            Key<float2> *key = (in ? cp.handle_in_keys : cp.handle_out_keys).key_at(to);
            if (!key || !key->selected) {
              continue;
            }
            const CurvePoint &shown = curve_after.points[j];
            const float2 handle = t.apply_vector(in ? curve_before.points[j].handle_in : curve_before.points[j].handle_out);
            /* A handle's key is its turn and stretch from the setup (data/vector.hh). */
            key->value = handle_change(in ? shown.setup_in : shown.setup_out, handle, key->value.x) -
                         (in ? shown.handle_in_keys.from_action_bones : shown.handle_out_keys.from_action_bones);
            changed++;
          }
        }
      }
    }
  }
  return changed;
}

int keys_keep_place(Document &doc, const Document &before, const std::vector<uint64_t> &layers,
                    const std::vector<std::pair<int, int>> &frames)
{
  int changed = 0;
  std::set<int> done;
  for (const auto &[from, to] : frames) {
    (void)from;
    if (to <= 0 || !done.insert(to).second) {
      continue;
    }
    Document was = before, now = doc;
    was.frame_current = now.frame_current = to;
    animation_evaluate(was, to);
    animation_evaluate(now, to);
    /* The copied points, where each shows now and showed before (document space). */
    struct Copied {
      Layer *layer;
      size_t point;
      Transform2D to_shown;
    };
    std::vector<Copied> copied;
    float2 centre_was, centre_now;
    for (const uint64_t id : layers) {
      Layer *layer = layer_find(doc, id);
      const Layer *layer_was = layer_find(was, id);
      const Layer *layer_now = layer_find(now, id);
      if (!layer || !layer_was || !layer_now || layer->is_reference() ||
          layer_was->drawing.points.size() != layer->drawing.points.size())
      {
        continue;
      }
      std::vector<Transform2D> skins_was = layer_point_skins(was, *layer_was);
      std::vector<Transform2D> skins_now = layer_point_skins(now, *layer_now);
      skins_was.resize(layer->drawing.points.size());
      skins_now.resize(layer->drawing.points.size());
      const Transform2D world_was = layer_world_transform(was, id), world_now = layer_world_transform(now, id);
      for (size_t i = 0; i < layer->drawing.points.size(); i++) {
        const MeshPoint &p = layer->drawing.points[i];
        const Key<float2> *key = p.pos_keys.key_at(to);
        if (!key || !key->selected || p.detail.level > 0) {
          continue;
        }
        const Transform2D to_shown = world_now * skins_now[i];
        centre_was += (world_was * skins_was[i]).apply(layer_was->drawing.points[i].pos);
        centre_now += to_shown.apply(layer_now->drawing.points[i].pos);
        copied.push_back({layer, i, to_shown});
      }
    }
    if (copied.empty()) {
      continue;
    }
    const float2 shift = (centre_was - centre_now) / float(copied.size());
    for (const Copied &c : copied) {
      const MeshPoint &shown = layer_find(now, c.layer->id)->drawing.points[c.point];
      const float2 at = c.to_shown.apply(shown.pos) + shift;
      const Transform2D back = c.to_shown.inverted();
      Key<float2> *key = c.layer->drawing.points[c.point].pos_keys.key_at(to);
      key->value = back.apply(at) - shown.setup_pos - shown.pos_keys.from_action_bones;
      changed++;
    }
  }
  return changed;
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
  std::shared_ptr<const Document> source;
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

void key_clipboard_set_source(const Document &doc)
{
  key_clipboard().source = std::make_shared<const Document>(doc);
}

const Document *key_clipboard_source()
{
  return key_clipboard().source.get();
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

int layers_paste_keys(const std::vector<Layer *> &layers, int frame,
                      std::vector<std::pair<uint64_t, uint64_t>> *r_layers, std::vector<std::pair<int, int>> *r_frames)
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
  Layer *found = nullptr;
  const auto target = [&](const ClipboardChannel &c, ChannelRef *r_ref) -> bool {
    for (Layer *layer : layers) {
      if (layer->id == c.layer && layer_channel(*layer, c.path, r_ref)) {
        found = layer;
        return true;
      }
    }
    for (Layer *layer : layers) {
      if (layer->name == c.layer_name && layer_channel(*layer, c.path, r_ref)) {
        found = layer;
        return true;
      }
    }
    if (sources.size() == 1) {
      for (Layer *layer : layers) {
        if (layer_channel(*layer, c.path, r_ref)) {
          found = layer;
          return true;
        }
      }
    }
    return false;
  };
  layers_deselect_keys(layers);
  const int offset = frame - clipboard.first_frame;
  int count = 0;
  /* The layers whose points or bones took a pose on the setup frame: their values follow the changes. */
  std::set<Layer *> posed_setup;
  /* Written once every channel is found: finding one works every point's change out from its value
   * again, which would undo a change written before it. */
  std::vector<std::pair<float *, float>> setup_scalars;
  std::vector<std::pair<float2 *, float2>> setup_vectors;
  for (const ClipboardChannel &c : clipboard.channels) {
    ChannelRef ref;
    if (!target(c, &ref)) {
      continue;
    }
    if (r_layers && std::find(r_layers->begin(), r_layers->end(), std::pair{found->id, c.layer}) == r_layers->end()) {
      r_layers->push_back({found->id, c.layer});
    }
    if (r_frames) {
      const auto note = [&](int from) {
        const std::pair<int, int> p{from, from + frame - clipboard.first_frame};
        if (std::find(r_frames->begin(), r_frames->end(), p) == r_frames->end()) {
          r_frames->push_back(p);
        }
      };
      for (const auto &k : c.scalar) {
        note(k.frame);
      }
      for (const auto &k : c.vector) {
        note(k.frame);
      }
    }
    /* A point's, handle's or bone's keys hold the change from the setup pose, so on the setup frame
     * (where the change is nothing) a key pasted is a pose to take: its change goes into the value, as
     * a move there would (animation_record makes it the setup; Keep Poses keeps the other frames). */
    const bool change = ref.path.rfind("bones[", 0) == 0
                            ? (ref.path.size() > 9 && ref.path.compare(ref.path.size() - 9, 9, ".position") == 0) ||
                                  (ref.path.size() > 6 && ref.path.compare(ref.path.size() - 6, 6, ".angle") == 0)
                            : (ref.path.rfind("points[", 0) == 0 && ref.path.size() > 4 &&
                               ref.path.compare(ref.path.size() - 4, 4, ".pos") == 0) ||
                                  (ref.path.rfind("curves[", 0) == 0 && (ref.path.find(".handle_in") != std::string::npos ||
                                                                         ref.path.find(".handle_out") != std::string::npos));
    Layer *owner = nullptr;
    for (Layer *layer : layers) {
      ChannelRef probe;
      if (change && layer_channel(*layer, ref.path, &probe) &&
          (probe.scalar ? (void *)probe.scalar : (void *)probe.vector) == (ref.scalar ? (void *)ref.scalar : (void *)ref.vector))
      {
        owner = layer;
      }
    }
    const auto put = [&](auto &channel, const auto &keys, auto *value) {
      for (const auto &key : keys) {
        const int at = key.frame + offset;
        if (at < 0) {
          continue;
        }
        if (at == 0 && change && owner && value) {
          if constexpr (std::is_same_v<std::decay_t<decltype(*value)>, float>) {
            setup_scalars.push_back({value, key.value});
          }
          else {
            setup_vectors.push_back({value, key.value});
          }
          posed_setup.insert(owner);
          /* Its frame 0 key (if it has one) is picked, as a pasted key would be. */
          if (auto *k = channel.key_at(0)) {
            k->selected = true;
          }
          count++;
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
      put(*ref.scalar, c.scalar, ref.scalar_value);
    }
    else if (ref.vector && !c.vector.empty()) {
      put(*ref.vector, c.vector, ref.vector_value);
    }
  }
  for (const auto &[at, v] : setup_scalars) {
    *at = v;
  }
  for (const auto &[at, v] : setup_vectors) {
    *at = v;
  }
  for (Layer *layer : posed_setup) {
    drawing_values_from_changes(layer->drawing);
    for (Bone &bone : layer->bones) {
      bone_values_from_changes(bone);
    }
  }
  return count;
}

}  // namespace oanim
