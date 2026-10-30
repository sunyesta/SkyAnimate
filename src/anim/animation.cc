#include "anim/animation.hh"

#include "anim/drivers.hh"

#include <algorithm>
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

#include "anim/deform.hh"
#include "core/log.hh"
#include "core/parallel.hh"
#include "anim/dynamics.hh"
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
  for (Bone &bone : layer.bones) {
    /* A bone's keys are its Transform and Angle: the change from its setup pose. */
    if constexpr (sync) {
      bone_shape_limit_affect(layer, bone);
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
      f(c.facing_keys, c.facing);
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
  /* Keys hold the value without what smart actions add. */
  const T base = value - ch.smart;
  if (frame == 0) {
    /* The setup frame: the frame-0 key, or the value itself while it isn't animated. */
    if (ch.animated()) {
      ch.set_key(0, base);
    }
  }
  else if (auto_key && (ch.animated() || !ch.keyed_by_hand) && (!replace || ch.key_at(frame))) {
    if (!ch.animated()) {
      ch.set_key(0, ch.last - ch.smart);
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
  const T base = value - ch.smart;
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
    /* Unkeyed, the plain value is the data: take off what smart actions added last time. */
    value -= ch.smart;
  }
  ch.smart = T{};
  ch.smart_live = false;
  ch.last = value;
  ch.has_last = true;
}

static void apply_smart_actions(Document &doc, int frame);

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
  const int frame = doc.evaluated_frame;
  const bool auto_key = doc.auto_key;
  /* New points get their weights, so they follow the bones like their neighbours. */
  document_weight_new_points(doc);
  /* Every layer, children too: the passes below work on each on its own, over the CPU's cores (a big document has
   * thousands). */
  std::vector<Layer *> flat;
  const auto gather = [&](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
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

void animation_evaluate(Document &doc, int frame)
{
  layers_for_each_channel(doc.layers, [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  simulation_in(doc);
  layer_for_each_channel(doc.simulation, [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  simulation_out(doc);
  layer_for_each_channel(document_camera(doc), [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  doc.smart_applied = false;
  apply_smart_actions(doc, frame);
  /* Drivers on top of keys and smart actions, before references copy their originals. A driven
   * smart bone plays its smart action, and drivers reading what that moves see it. */
  if (drivers_evaluate(doc)) {
    animation_refresh_smart(doc);
    drivers_evaluate(doc);
  }
  /* References show their originals as they now are. */
  references_build(doc);
  /* Bone dynamics swing from the pose the keys and smart actions give (and set each bone layer's
   * shown frame, for constraints' delays). */
  const int shown = doc.frame_current;
  doc.frame_current = frame;
  dynamics_update(doc);
  /* Limit to Shape reads its shapes as drawn now, dynamics included. */
  shape_limits_update(doc);
  doc.frame_current = shown;
  doc.evaluated_frame = frame;
}

void layer_strip_keys(Layer &layer, bool from_now, bool keep_actions)
{
  layer_for_each_channel(layer, [&](auto &ch, auto &value) {
    using T = std::decay_t<decltype(value)>;
    value -= ch.smart;
    if (ch.animated() && !from_now) {
      value = ch.evaluate(0);
    }
    ch.keys.clear();
    if (!keep_actions) {
      ch.actions.clear();
    }
    ch.smart = T{};
    ch.smart_live = false;
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

void animation_strip_smart(Document &doc)
{
  layers_for_each_channel(doc.layers, [&](auto &ch, auto &value) {
    value -= ch.smart;
    ch.last -= ch.smart;
    ch.smart = {};
    ch.smart_live = false;
  });
  doc.smart_applied = false;
}

static uint64_t g_frame_changes = 0;

void animation_sync(Document &doc)
{
  if (doc.evaluated_frame == doc.frame_current) {
    /* Same frame, but bones may have turned: smart actions and dynamics follow at once (and a
     * setting whose keys just went shows its value). */
    simulation_in(doc);
    animation_refresh_smart(doc);
    /* Drivers follow what they read at once (a bone turned on this frame), and smart actions what
     * they pose. */
    if (drivers_evaluate(doc)) {
      animation_refresh_smart(doc);
      drivers_evaluate(doc);
    }
    /* Switches follow children added, removed or picked at once. */
    switches_resolve(doc, doc.frame_current);
    /* References follow edits to their originals at once, and edits to them become theirs. */
    references_capture(doc);
    dynamics_update(doc);
    shape_limits_update(doc);
    return;
  }
  if (doc.evaluated_frame >= 0) {
    animation_record(doc);
    g_frame_changes++;
  }
  animation_evaluate(doc, doc.frame_current);
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
  start.smart_applied = now.smart_applied;
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
  std::erase_if(owner->fix_actions, [&](const FixAction &f) { return f.name == name; });
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
template<typename T>
static T smart_sample(const std::vector<Key<T>> &keys, const T &setup, float t, bool constant = false)
{
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
    if (!smart_action_bone(owner, d)) {
      return "\"" + d + "\" isn't a smart action now";
    }
  }
  return "";
}

/* What each smart action's bone picks: its frame for the bone's turn in `angles` (owner's bones, in order). */
static std::vector<std::pair<std::string, float>> smart_frames_for(const Layer &owner, const std::vector<float> &turns)
{
  std::vector<std::pair<std::string, float>> frames;
  for (const std::string &name : owner.actions) {
    const Bone *driver = smart_action_bone(owner, name);
    if (!driver) {
      continue;
    }
    const auto it = driver->angle_keys.actions.find(name);
    if (it == driver->angle_keys.actions.end()) {
      continue;
    }
    /* The driving bone's keys are its Angle (the turn from its setup pose), so its action starts at
     * 0 and the bone's turn now picks the frame; turning an unkeyed bone plays it at once. */
    const float t = smart_frame(it->second, 0.0f, turns[size_t(driver - owner.bones.data())]);
    if (t > 0.0f) {
      frames.push_back({name, t});
    }
  }
  return frames;
}

std::vector<std::pair<std::string, float>> smart_actions_playing(const Layer &owner)
{
  std::vector<float> turns;
  for (const Bone &b : owner.bones) {
    turns.push_back(b.angle - b.setup_angle);
  }
  return smart_frames_for(owner, turns);
}

float smart_action_length(const Layer &owner, const std::string &name)
{
  const Bone *driver = smart_action_bone(owner, name);
  if (!driver) {
    return 0.0f;
  }
  if (const auto it = driver->angle_keys.actions.find(name); it != driver->angle_keys.actions.end() && !it->second.empty()) {
    return float(it->second.back().frame);
  }
  /* Being edited: its keys are the bone's own. */
  return driver->angle_keys.keys.empty() ? 0.0f : float(driver->angle_keys.keys.back().frame);
}

std::vector<std::pair<std::string, float>> smart_actions_counting(const Layer &owner)
{
  std::vector<std::pair<std::string, float>> counting;
  for (const auto &[name, t] : smart_actions_playing(owner)) {
    const float length = smart_action_length(owner, name);
    if (length <= 0.0f || t / length >= FIX_COUNTS_FROM - 1e-6f) {
      counting.push_back({name, t});
    }
  }
  return counting;
}

/* A smart action's length to measure shares by (1 for one with no keys yet, so nothing divides by 0). */
static float share_length(const Layer &owner, const std::string &name)
{
  const float length = smart_action_length(owner, name);
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

std::string fix_pose_label(const Layer &owner, const FixAction &fix, const FixPose &pose)
{
  const std::vector<float> shares = fix_pose_shares(owner, fix, pose);
  std::string label;
  for (size_t i = 0; i < fix.drivers.size(); i++) {
    label += (label.empty() ? "" : " \xC2\xB7 ") + fix.drivers[i] + " " + std::to_string(int(std::lround(shares[i] * 100.0f))) + "%";
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
  /* Where the drivers are now, as shares of their smart actions. */
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
  /* Past the furthest pose along a driver it holds, as a smart action holds its last frame. */
  for (size_t i = 0; i < n; i++) {
    now[i] = std::min(now[i], furthest[i]);
  }
  /* How much each pose has come in: its drivers' shares of it multiplied (Maya's Multiplication), each
   * at most all of it, so the fix is 0 while any driver rests and all there at or past a pose. */
  float strength = 0.0f;
  for (const std::vector<float> &pose : poses) {
    float in = 1.0f;
    for (size_t i = 0; i < n; i++) {
      in *= pose[i] > 1e-6f ? std::min(now[i] / pose[i], 1.0f) : 1.0f;
    }
    strength = std::max(strength, in);
  }
  if (strength <= 0.0f) {
    return mix;
  }
  /* Which pose's correction: inverse-distance weights (Shepard's), exact at a pose. */
  std::vector<float> weight(poses.size(), 0.0f);
  float total = 0.0f;
  int exact = -1;
  for (size_t k = 0; k < poses.size(); k++) {
    float d2 = 0.0f;
    for (size_t i = 0; i < n; i++) {
      d2 += (now[i] - poses[k][i]) * (now[i] - poses[k][i]);
    }
    if (d2 < 1e-10f) {
      exact = int(k);
      break;
    }
    weight[k] = 1.0f / d2;
    total += weight[k];
  }
  if (exact >= 0) {
    std::fill(weight.begin(), weight.end(), 0.0f);
    weight[size_t(exact)] = 1.0f;
    total = 1.0f;
  }
  for (size_t k = 0; k < poses.size(); k++) {
    const float share = weight[k] / total;
    if (share > 1e-6f) {
      mix.poses.push_back({k, strength * share});
    }
    if (mix.nearest < 0 || share > weight[size_t(mix.nearest)] / total) {
      mix.nearest = int(k);
    }
  }
  mix.strength = strength;
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
  return smart_action_bone(owner, name) || fix_action_find(owner, name);
}

/* Each channel's setup, as apply_smart_actions takes it: its key on frame 0, or (unkeyed) its value less
 * what smart actions add now. */
template<typename C, typename V> static auto channel_setup(const C &ch, const V &value)
{
  return ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value) - ch.smart;
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
    opened.error = "Frame 0 is the rest pose: turn the smart bones on another frame";
    return opened;
  }
  animation_record(doc);
  const std::vector<std::pair<std::string, float>> counting = smart_actions_counting(*owner);
  std::vector<std::pair<std::string, float>> drivers;
  if (drivers_asked.empty()) {
    drivers = counting;
  }
  else {
    for (const std::string &name : drivers_asked) {
      const auto it = std::find_if(counting.begin(), counting.end(), [&](const auto &p) { return p.first == name; });
      if (it == counting.end()) {
        opened.error = "\"" + name + "\" isn't turned (at least " + std::to_string(int(FIX_COUNTS_FROM * 100.0f + 0.5f)) + "% of its smart action)";
        return opened;
      }
      if (std::none_of(drivers.begin(), drivers.end(), [&](const auto &p) { return p.first == name; })) {
        drivers.push_back(*it);
      }
    }
  }
  if (drivers.size() < 2) {
    opened.error = "A fix needs two or more smart actions playing: turn their bones first";
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
  const FixAction *fix = fix_action_find(armature, doc.action);
  return fix && std::any_of(fix->drivers.begin(), fix->drivers.end(),
                            [&](const std::string &d) { return smart_action_bone(armature, d) == &bone; });
}

void fix_actions_tidy(Document &doc)
{
  /* The smart bones of the fix being edited: never picked. */
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
    if (!let_go.empty()) {
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
  owner_for_each_channel(*owner, [&](auto &ch, auto & /*value*/) {
    if (auto it = ch.actions.find(from); it != ch.actions.end()) {
      auto keys = std::move(it->second);
      ch.actions.erase(it);
      ch.actions[to] = std::move(keys);
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
      owner_for_each_channel(*owner, [&](auto &ch, auto & /*value*/) {
        if (auto it = ch.actions.find(old); it != ch.actions.end()) {
          auto keys = std::move(it->second);
          ch.actions.erase(it);
          ch.actions[renamed] = std::move(keys);
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

void smart_actions_add_at(const Layer &owner, int frame, std::vector<Bone> &bones)
{
  if (frame == 0 || owner.actions.empty() || bones.size() != owner.bones.size()) {
    return;
  }
  /* Every driving bone's angle on that frame picks its action's frame first, as on the frame shown. */
  std::vector<float> turns;
  for (size_t i = 0; i < bones.size(); i++) {
    turns.push_back(bones[i].angle - owner.bones[i].setup_angle);
  }
  std::vector<std::pair<std::string, float>> frames = smart_frames_for(owner, turns);
  std::set<const void *> driving;
  for (const auto &[name, t] : frames) {
    driving.insert(&smart_action_bone(owner, name)->angle_keys);
  }
  const auto setup_of = [](const auto &ch, const auto &value) { return channel_setup(ch, value); };
  for (const auto &[name, t] : frames) {
    const Channel<float> *driver = &smart_action_bone(owner, name)->angle_keys;
    for (size_t j = 0; j < bones.size(); j++) {
      const Bone &src = owner.bones[j];
      Bone &b = bones[j];
      if (const auto it = src.position_keys.actions.find(name); it != src.position_keys.actions.end()) {
        const float2 setup = setup_of(src.position_keys, src.transform);
        b.position += rotate(smart_sample(it->second, setup, t) - setup, src.setup_angle);
      }
      if (&src.angle_keys != driver) {
        if (const auto it = src.angle_keys.actions.find(name); it != src.angle_keys.actions.end()) {
          const float setup = setup_of(src.angle_keys, src.turn);
          b.angle += smart_sample(it->second, setup, t) - setup;
        }
      }
      if (const auto it = src.scale_keys.actions.find(name); it != src.scale_keys.actions.end()) {
        const float2 setup = setup_of(src.scale_keys, src.scale);
        b.scale += smart_sample(it->second, setup, t) - setup;
      }
    }
  }
  /* The fixes after them, as their drivers play together (never turning a smart bone). */
  for (const FixAction &fix : owner.fix_actions) {
    if (!fix_action_broken(owner, fix).empty()) {
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
    ch.smart_live = false;
  });
  doc.smart_applied = false;
  apply_smart_actions(doc, doc.frame_current);
}

static void apply_smart_actions_playing(Document &doc, int frame);

static void switches_clear_rig(std::vector<Layer> &layers)
{
  for (Layer &layer : layers) {
    layer.switch_rig_set = false;
    switches_clear_rig(layer.children);
  }
}

/* Smart actions, then who wins on every switch (anim/switch.hh): a switch's pick is never added. */
static void apply_smart_actions(Document &doc, int frame)
{
  switches_clear_rig(doc.layers);
  apply_smart_actions_playing(doc, frame);
  switches_resolve(doc, frame);
}

static void apply_smart_actions_playing(Document &doc, int frame)
{
  /* The setup frame (0) is the rig at rest: turning a smart bone there sets its rest angle and plays
   * nothing, as in Moho (and bones don't bend the drawing there either). `frame`: the one evaluated
   * (switching timelines evaluates frame 0 while another is shown). */
  if (frame == 0) {
    return;
  }
  /* Editing a smart action shows its own keys, without any smart action on top. Editing a fix action
   * shows what it fixes: its drivers held at the pose this frame of it is for (the pose on or before it),
   * the fixes for fewer of them as they play there, its own keys the correction on top. */
  const Layer *edited_owner = doc.action.empty() ? nullptr : layer_find(doc, doc.action_layer);
  if (edited_owner && smart_action_bone(*edited_owner, doc.action)) {
    return;
  }
  const FixAction *edited_fix = edited_owner ? fix_action_find(*edited_owner, doc.action) : nullptr;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer() || flat->actions.empty()) {
      continue;
    }
    Layer &owner = *layer_find(doc, flat->id);
    const bool fixing = edited_fix && owner.id == doc.action_layer;
    /* Every driving bone's angle picks its frame first, before any action moves bones. */
    std::vector<std::pair<std::string, float>> frames;
    if (fixing) {
      if (const int held = fix_pose_held(*edited_fix, frame); held >= 0) {
        const FixPose &pose = edited_fix->poses[size_t(held)];
        for (size_t k = 0; k < edited_fix->drivers.size(); k++) {
          if (smart_action_bone(owner, edited_fix->drivers[k]) && k < pose.at.size() && pose.at[k] > 0.0f) {
            frames.push_back({edited_fix->drivers[k], pose.at[k]});
          }
        }
      }
    }
    else {
      for (const auto &[name, t] : smart_actions_playing(owner)) {
        if (!(name == doc.action && owner.id == doc.action_layer)) {
          frames.push_back({name, t});
        }
      }
    }
    /* The smart bones' own angles: they pick the frames, so nothing else moves them (shown turned only
     * while a fix is edited, its drivers held). */
    std::set<const void *> driving;
    for (const std::string &name : owner.actions) {
      if (const Bone *b = smart_action_bone(owner, name)) {
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
      owner_for_each_channel(owner, [&](auto &ch, auto &value) {
        if (static_cast<const void *>(&ch) == skip || (skip_driving && driving.count(static_cast<const void *>(&ch)))) {
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
              ch.smart_live = true;
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
        const auto setup = ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value) - ch.smart;
        const auto delta = smart_sample(it->second, setup, t, ch.constant) - setup;
        /* The value and what it was last recorded as both move, so an edit stays an edit. */
        value += delta;
        ch.last += delta;
        ch.smart += delta;
        /* Pink only where the action moves it somewhere: an action keys every bone, most held still. */
        ch.smart_live = ch.smart_live || std::any_of(it->second.begin(), it->second.end(), [&](const auto &k) {
          const auto off = k.value - setup;
          if constexpr (std::is_same_v<std::decay_t<decltype(off)>, float>) {
            return std::abs(off) > 1e-6f;
          }
          else {
            return std::abs(off.x) > 1e-6f || std::abs(off.y) > 1e-6f;
          }
        });
        doc.smart_applied = true;
      });
    };
    for (const auto &[name, t] : frames) {
      /* A smart action never moves its own bone (the bone picks its frame), unless held for a fix. */
      play(name, t, fixing ? nullptr : static_cast<const void *>(&smart_action_bone(owner, name)->angle_keys), false);
    }
    /* Then the fixes, after every smart action, as their drivers play together. While one is edited, the
     * fixes for some of its drivers only (a pair inside a three) play under it: it is keyed on top of them. */
    const auto inside_edited = [&](const FixAction &fix) {
      if (!fixing || fix.drivers.size() >= edited_fix->drivers.size()) {
        return false;
      }
      return std::all_of(fix.drivers.begin(), fix.drivers.end(), [&](const std::string &d) {
        return std::find(edited_fix->drivers.begin(), edited_fix->drivers.end(), d) != edited_fix->drivers.end();
      });
    };
    for (const FixAction &fix : owner.fix_actions) {
      if ((fix.name == doc.action && owner.id == doc.action_layer) || !fix_action_broken(owner, fix).empty() ||
          (fixing && !inside_edited(fix)))
      {
        continue;
      }
      const FixMix mix = fix_action_mix(owner, fix, frames);
      if (mix.strength <= 0.0f) {
        continue;
      }
      owner_for_each_channel(owner, [&](auto &ch, auto &value) {
        if (driving.count(static_cast<const void *>(&ch)) || switches.count(static_cast<const void *>(&ch))) {
          return;
        }
        const auto it = ch.actions.find(fix.name);
        if (it == ch.actions.end()) {
          return;
        }
        const auto setup = ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value) - ch.smart;
        auto delta = setup - setup;
        if (!fix_delta(it->second, setup, fix, mix, ch.constant, delta)) {
          return;
        }
        value += delta;
        ch.last += delta;
        ch.smart += delta;
        ch.smart_live = ch.smart_live || !(delta == setup - setup);
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
      if (c.type == BoneConstraintType::LookAt) {
        add(cb + "facing", c.facing_keys, c.facing);
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
    if (field == "facing") return take(c.facing_keys, c.facing);
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

int layer_insert_filtered_keys(Layer &layer, const KeyFilter &filter, int frame)
{
  int count = 0;
  const auto key = [&](auto &ch, auto &value) {
    /* As layer_insert_keys: a setting keyed only by hand (a mask's Reach) waits to be keyed so. */
    if (ch.keyed_by_hand && !ch.animated()) {
      return;
    }
    const auto base = value - ch.smart;
    if (!ch.animated() && frame != 0) {
      ch.set_key(0, ch.has_last ? ch.last - ch.smart : base);
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
    value = ch.keys.front().value + ch.smart;
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

int layer_smart_state(Layer &layer, const std::vector<std::string> &paths)
{
  for (const std::string &path : paths) {
    ChannelRef ref;
    if (layer_channel(layer, path, &ref) && (ref.scalar ? ref.scalar->smart_live : ref.vector->smart_live)) {
      return KEY_STATE_SMART;
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
  for (ChannelRef &ref : layer_channels(layer, drawing)) {
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
    refs.push_back(std::move(ref));
  }
  return refs;
}

/* Calls f(channel) for every channel of `layers` that passes the filter. */
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
        key->value = pos - shown.setup_pos - shown.pos_keys.smart;
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
                         (in ? shown.handle_in_keys.smart : shown.handle_out_keys.smart);
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
      key->value = back.apply(at) - shown.setup_pos - shown.pos_keys.smart;
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
