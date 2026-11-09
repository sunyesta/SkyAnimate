#include "anim/motion_path.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "anim/animation.hh"
#include "anim/controller.hh"
#include "assets/asset_library.hh"
#include "anim/deform.hh"
#include "anim/dynamics.hh"
#include "anim/sculpt.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/document.hh"
#include "data/vector.hh"
#include "core/parallel.hh"
#include "core/profile.hh"

namespace oanim {

namespace {

constexpr size_t MAX_POINTS = 24;

int bone_index(const Layer &layer, uint64_t bone)
{
  for (size_t i = 0; i < layer.bones.size(); i++) {
    if (layer.bones[i].id == bone) {
      return int(i);
    }
  }
  return -1;
}

int point_index(const Layer &layer, uint64_t point)
{
  for (size_t i = 0; i < layer.drawing.points.size(); i++) {
    if (layer.drawing.points[i].id == point) {
      return int(i);
    }
  }
  return -1;
}

bool target_exists(const Document &doc, const MotionPathTarget &t)
{
  const Layer *layer = layer_find(doc, t.layer);
  if (!layer) {
    return false;
  }
  if (t.bone) {
    return bone_index(*layer, t.bone) >= 0;
  }
  if (t.point) {
    return point_index(*layer, t.point) >= 0;
  }
  return !t.drawing || layer->has_drawing() || (key_together_allowed(*layer) && layer->key_together);
}

/* A Key Whole Drawing layer's shape channels (every point's position and width, every curve point's curvature and
 * handles), the animated ones; `f` returns false to stop. */
template<typename F> static void drawing_channels(Document &doc, Layer &layer, F &&f)
{
  /* A Key Together group: every channel it keys as one. */
  if (key_together_allowed(layer) && layer.key_together) {
    for (const ChannelRef &ref : key_together_channels(doc, layer)) {
      if ((ref.scalar && ref.scalar->animated() && !f(*ref.scalar)) ||
          (ref.vector && ref.vector->animated() && !f(*ref.vector)))
      {
        return;
      }
    }
    return;
  }
  if (!layer.has_drawing() || layer.is_reference()) {
    return;
  }
  for (MeshPoint &p : layer.drawing.points) {
    if ((p.pos_keys.animated() && !f(p.pos_keys)) || (p.radius_keys.animated() && !f(p.radius_keys))) {
      return;
    }
  }
  for (Curve &curve : layer.drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      if ((cp.curvature_keys.animated() && !f(cp.curvature_keys)) ||
          (cp.handle_in_keys.animated() && !f(cp.handle_in_keys)) ||
          (cp.handle_out_keys.animated() && !f(cp.handle_out_keys)))
      {
        return;
      }
    }
  }
}

static bool drawing_animated(const Document &doc, const Layer &layer)
{
  bool any = false;
  drawing_channels(const_cast<Document &>(doc), const_cast<Layer &>(layer), [&](auto &) {
    any = true;
    return false;
  });
  return any;
}

/* Where `t` is in document space, as `doc` shows it now. */
bool target_position(const Document &doc, const MotionPathTarget &t, float2 &r_at)
{
  const Layer *layer = layer_find(doc, t.layer);
  if (!layer) {
    return false;
  }
  float2 at = {0.0f, 0.0f};
  if (t.bone) {
    const int i = bone_index(*layer, t.bone);
    if (i < 0) {
      return false;
    }
    const std::vector<BonePose> poses = armature_pose(*layer, false);
    /* A controller's head is its knob. */
    at = t.head ? poses[size_t(i)].knob.apply({0, 0}) : poses[size_t(i)].tail;
  }
  else if (t.point) {
    const int i = point_index(*layer, t.point);
    if (i < 0) {
      return false;
    }
    at = layer->drawing.points[size_t(i)].pos;
    const std::vector<Transform2D> skins = layer_point_skins(doc, *layer);
    if (size_t(i) < skins.size()) {
      at = skins[size_t(i)].apply(at);
    }
  }
  r_at = layer_world_transform(doc, layer->id).apply(at);
  return true;
}

/* A target's own values as evaluated: a bone's position, size and angle, a point's position, a layer's. */
struct OwnValues {
  float2 position, scale;
  float angle = 0.0f;
};
bool own_values(const Document &doc, const MotionPathTarget &t, OwnValues &r);
void own_values_set(Document &doc, const MotionPathTarget &t, const OwnValues &v);

bool g_path_tool = true;
/* While a MotionPathOnFrame has the document on another frame: the document as shown, what Own Motion holds. (Each
 * thread's: places worked out side by side are handed it, held_places.) */
thread_local const Document *g_shown = nullptr;

/* Document space to the space the paths are drawn relative to, as `doc` shows it now. */
Transform2D relative_space(const Document &doc)
{
  switch (doc.path_relative) {
    case PathRelative::World:
      break;
    case PathRelative::Camera:
      return camera_to_frame(doc);
    case PathRelative::Parent: {
      const Layer *layer = layer_find(doc, doc.path_relative_layer);
      if (!layer) {
        break;
      }
      Transform2D to_doc = layer_world_transform(doc, layer->id);
      if (doc.path_relative_bone) {
        const int i = bone_index(*layer, doc.path_relative_bone);
        if (i >= 0) {
          to_doc = to_doc * armature_pose(*layer, false)[size_t(i)].pose;
        }
      }
      return to_doc.inverted();
    }
  }
  return Transform2D();
}

}  // namespace

template<typename F> static bool with_path_channel(Document &doc, const MotionPathTarget &t, F &&f);
template<typename F> static bool with_path_channels(Document &doc, const MotionPathTarget &t, F &&f);
template<typename F> static bool with_timing_channels(Document &doc, const MotionPathTarget &t, F &&f);

/* A bone's tip goes by its angle (turned to follow the mouse, timed by its angle keys), unless the bone only moves:
 * no angle keys, a head of its own with position keys. Then the tip goes as its head does, by those. */
static bool tip_turns(const Document &doc, const MotionPathTarget &t)
{
  if (!t.bone || t.head || t.drawing) {
    return false;
  }
  const Layer *layer = layer_find(doc, t.layer);
  const int i = layer ? bone_index(*layer, t.bone) : -1;
  if (i < 0) {
    return true;
  }
  const Bone &b = layer->bones[size_t(i)];
  return !b.angle_keys.keys.empty() || b.connected || b.position_keys.keys.empty();
}

const MotionPathTarget *motion_path_style(const Document &doc, const MotionPathTarget &target)
{
  for (const MotionPathTarget &t : doc.path_targets) {
    if (t.same(target)) {
      return &t;
    }
  }
  return nullptr;
}

MotionPathTarget &motion_path_style_ensure(Document &doc, const MotionPathTarget &target)
{
  for (MotionPathTarget &t : doc.path_targets) {
    if (t.same(target)) {
      return t;
    }
  }
  MotionPathTarget t;
  t.layer = target.layer;
  t.bone = target.bone;
  t.point = target.point;
  t.head = target.head;
  t.drawing = target.drawing;
  doc.path_targets.push_back(t);
  return doc.path_targets.back();
}

std::vector<MotionPathTarget> motion_path_targets(const Document &doc, const std::vector<Layer *> &editing,
                                                  bool edit_layer)
{
  std::vector<MotionPathTarget> out;
  const auto add = [&](MotionPathTarget t) {
    for (const MotionPathTarget &o : out) {
      if (o.same(t)) {
        return;
      }
    }
    if (const MotionPathTarget *style = motion_path_style(doc, t)) {
      t = *style;
    }
    out.push_back(t);
  };
  /* The active layer and the picked ones, of any type (a bone layer's bones aren't drawings, so `editing` leaves it
   * out), trace their selected bones or points, else their origins; the drawings edited with them (a group's, with
   * Edit Descendants) only their selected points, so a group doesn't light up a path per child. */
  std::vector<const Layer *> own;
  if (const Layer *active = layer_find(doc, doc.active_layer)) {
    own.push_back(active);
  }
  for (const Layer *layer : document_layers_flat(doc)) {
    if (layer->selected && layer->shown() && std::find(own.begin(), own.end(), layer) == own.end()) {
      own.push_back(layer);
    }
  }
  std::vector<const Layer *> layers = own;
  for (const Layer *layer : editing) {
    if (!edit_layer && layer && std::find(layers.begin(), layers.end(), layer) == layers.end()) {
      layers.push_back(layer);
    }
  }
  size_t points = 0;
  for (const Layer *layer : layers) {
    bool any = false;
    if (edit_layer) {
      /* Edit Layer: the layer itself, its origin. */
    }
    else if (layer->is_bone_layer()) {
      /* A bone traces its tip and its head, but a connected head only rides on its parent's tip; a pin traces one
       * path, at the pin. */
      for (const Bone &b : layer->bones) {
        if (b.selected) {
          if (!bone_is_point(b)) {
            add({layer->id, b.id, 0});
          }
          if (!b.connected || bone_is_point(b)) {
            MotionPathTarget head{layer->id, b.id, 0};
            head.head = true;
            add(head);
          }
          any = true;
        }
      }
    }
    else if (layer->has_drawing() && !layer->key_whole_drawing) {
      /* A point with one of its handles picked traces too: the handle is part of it. */
      std::vector<bool> handle_picked(layer->drawing.points.size(), false);
      for (const Curve &c : layer->drawing.curves) {
        for (const CurvePoint &cp : c.points) {
          if ((cp.select_in || cp.select_out) && size_t(cp.point) < handle_picked.size()) {
            handle_picked[size_t(cp.point)] = true;
          }
        }
      }
      for (size_t i = 0; i < layer->drawing.points.size(); i++) {
        const MeshPoint &p = layer->drawing.points[i];
        if ((p.selected || handle_picked[i]) && p.id && points < MAX_POINTS) {
          add({layer->id, 0, p.id});
          points++;
          any = true;
        }
      }
    }
    if (!any && std::find(own.begin(), own.end(), layer) != own.end()) {
      add({layer->id, 0, 0});
    }
  }
  for (const MotionPathTarget &t : doc.path_targets) {
    if (t.pinned && target_exists(doc, t)) {
      add(t);
    }
  }
  return out;
}

MotionPathTarget motion_path_timing_owner(const Document &doc, const MotionPathTarget &target)
{
  Document &d = const_cast<Document &>(doc);
  const Layer *group = key_together_group(d, target.layer);
  if (!group || (target.drawing && target.layer == group->id) || !drawing_animated(doc, *group)) {
    return target;
  }
  MotionPathTarget t;
  t.layer = group->id;
  t.drawing = true;
  return t;
}

std::vector<MotionPathTarget> motion_path_timing_targets(const Document &doc, const std::vector<Layer *> &editing,
                                                         bool edit_layer)
{
  std::vector<MotionPathTarget> out = motion_path_targets(doc, editing, edit_layer);
  if (edit_layer) {
    /* The layer itself: no drawing's timing, but inside a folded group the group's, keyed as one. */
    std::vector<MotionPathTarget> owned;
    for (const MotionPathTarget &t : out) {
      const MotionPathTarget o = motion_path_timing_owner(doc, t);
      if (std::none_of(owned.begin(), owned.end(), [&](const MotionPathTarget &x) { return x.same(o); })) {
        owned.push_back(o.same(t) ? t : o);
      }
    }
    return owned;
  }
  std::vector<const Layer *> layers;
  if (const Layer *active = layer_find(doc, doc.active_layer)) {
    layers.push_back(active);
  }
  for (const Layer *layer : document_layers_flat(doc)) {
    if (layer->selected && layer->shown()) {
      layers.push_back(layer);
    }
  }
  layers.insert(layers.end(), editing.begin(), editing.end());
  Document &d = const_cast<Document &>(doc);
  for (const Layer *layer : layers) {
    if (!layer) {
      continue;
    }
    /* Keyed together with the rest of a group: the group's row. */
    if (const Layer *group = key_together_group(d, layer->id)) {
      layer = group;
    }
    else if (!layer->key_whole_drawing || !layer->has_drawing()) {
      continue;
    }
    if (!drawing_animated(doc, *layer)) {
      continue;
    }
    MotionPathTarget t;
    t.layer = layer->id;
    t.drawing = true;
    if (std::none_of(out.begin(), out.end(), [&](const MotionPathTarget &o) { return o.same(t); })) {
      out.push_back(t);
    }
  }
  /* Inside a Key Together group: its timing is the group's row. */
  std::erase_if(out, [&](const MotionPathTarget &t) {
    const Layer *group = key_together_group(d, t.layer);
    return group && !(t.drawing && t.layer == group->id);
  });
  /* A whole drawing's layer that doesn't move itself: its own row would only show the drawing's keys again, with no
   * timing of its own. */
  std::erase_if(out, [&](const MotionPathTarget &t) {
    if (t.drawing || t.bone || t.point) {
      return false;
    }
    const bool drawing_row = std::any_of(out.begin(), out.end(), [&](const MotionPathTarget &o) {
      return o.drawing && o.layer == t.layer;
    });
    return drawing_row && motion_path_stretches(doc, t).empty();
  });
  /* Nothing of its own to retime: a point only a bone moves. */
  std::erase_if(out, [&](const MotionPathTarget &t) { return motion_path_keys(doc, t).empty(); });
  return out;
}

std::vector<int> motion_path_keys(const Document &doc, const MotionPathTarget &target)
{
  if (target.drawing) {
    /* The drawing's own keys: its points share them. */
    std::vector<int> keys;
    if (Layer *layer = layer_find(const_cast<Document &>(doc), target.layer)) {
      drawing_channels(const_cast<Document &>(doc), *layer, [&](auto &ch) {
        for (const auto &k : ch.keys) {
          if (k.frame > 0) {
            keys.push_back(k.frame);
          }
        }
        return false;
      });
    }
    return keys;
  }
  /* Only its own keys: a bone's moves, turns and sizes (and its knob's keys, which play the actions moving it), a
   * point's moves, a layer's turns, moves and sizes. What moves it from elsewhere (a bone moving a point, a parent
   * bone or layer) has a row of its own: its keys here would split stretches this row can't retime. Frame 0 is the
   * setup, not a key of the animation. */
  std::set<int> keys;
  const auto add = [&](const auto &ch) {
    for (const auto &k : ch.keys) {
      if (k.frame > 0) {
        keys.insert(k.frame);
      }
    }
  };
  const Layer *layer = layer_find(doc, target.layer);
  if (!layer) {
    return {};
  }
  if (target.bone) {
    const int i = bone_index(*layer, target.bone);
    if (i < 0) {
      return {};
    }
    const Bone &b = layer->bones[size_t(i)];
    add(b.position_keys);
    add(b.angle_keys);
    add(b.scale_keys);
    std::set<std::string> actions;
    for (const auto &[name, action_keys] : b.position_keys.actions) {
      actions.insert(name);
    }
    for (const auto &[name, action_keys] : b.angle_keys.actions) {
      actions.insert(name);
    }
    for (const auto &[name, action_keys] : b.scale_keys.actions) {
      actions.insert(name);
    }
    for (const std::string &name : actions) {
      if (const Bone *knob = controller_of_action(*layer, name)) {
        add(knob->position_keys);
      }
    }
  }
  else if (target.point) {
    const int i = point_index(*layer, target.point);
    if (i >= 0) {
      add(layer->drawing.points[size_t(i)].pos_keys);
    }
  }
  else {
    /* A layer's path: only the turns, moves and sizes that move it (its opacity or drawing keys aren't keys of its
     * path, and its diamonds' menu reaches only these). */
    KeyFilter filter;
    filter.kinds = KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM;
    for (const ChannelRef &ref : layer_filtered_channels(*const_cast<Layer *>(layer), filter)) {
      if (ref.scalar) {
        add(*ref.scalar);
      }
      if (ref.vector) {
        add(*ref.vector);
      }
    }
  }
  return {keys.begin(), keys.end()};
}

bool motion_path_frame_outside(const Document &doc, int frame)
{
  return (doc.frame_start > 0 && frame < doc.frame_start) || frame > std::max(doc.frame_start, doc.frame_end);
}

bool motion_path_range(const Document &doc, const std::vector<MotionPathTarget> &targets, int &r_first, int &r_last)
{
  if (targets.empty()) {
    return false;
  }
  std::set<int> keys;
  for (const MotionPathTarget &t : targets) {
    for (const int f : motion_path_keys(doc, t)) {
      keys.insert(f);
    }
  }
  /* From the scene's Start: frame 0 is the setup pose, not part of the shot. With no key on Start, back to the last
   * key before it (frame 0's, the setup's, when there is none), so the stretch the shot opens in can still be
   * shaped; what is before Start is drawn faded. */
  int start = std::max(0, doc.frame_start);
  if (start > 0 && !keys.count(start)) {
    const auto it = keys.lower_bound(start);
    start = it == keys.begin() ? 0 : *std::prev(it);
  }
  /* To the scene's End; with no key on End, on to the first key after it, so the stretch the shot ends in can be
   * shaped too (drawn faded past End); with none after, End. */
  int end = std::max(start, doc.frame_end);
  if (!keys.count(end)) {
    if (const auto it = keys.upper_bound(end); it != keys.end()) {
      end = *it;
    }
  }
  r_first = start;
  r_last = end;
  if (doc.path_all) {
    return true;
  }
  const int now = doc.frame_current;
  /* The `path_before`th key before the shown frame (one on it doesn't count) and the `path_after`th after it; short
   * of keys, the range's ends. */
  int k = 0;
  for (auto it = keys.lower_bound(now); it != keys.begin() && k < doc.path_before;) {
    --it;
    k++;
    if (k == doc.path_before) {
      r_first = std::max(start, *it);
    }
  }
  if (doc.path_before == 0) {
    r_first = std::clamp(now, start, end);
  }
  k = 0;
  for (auto it = keys.upper_bound(now); it != keys.end() && k < doc.path_after; ++it) {
    k++;
    if (k == doc.path_after) {
      r_last = std::min(end, *it);
    }
  }
  if (doc.path_after == 0) {
    r_last = std::clamp(now, start, end);
  }
  if (r_last < r_first) {
    std::swap(r_first, r_last);
  }
  return true;
}

void motion_path_tool_on(bool on)
{
  g_path_tool = on;
}

bool motion_path_own_motion(const Document &doc, const MotionPathTarget &target)
{
  /* Checked, every path the tool shows is its own motion, picked or not (one that turned real as something on it was
   * picked, or let go, confused). A drawing's path is its layer's origin, which its own (shape) keys never move;
   * outside the tool paths are only looked at, the real ones. */
  return doc.path_own_motion && !target.drawing && g_path_tool;
}

static void path_shape_fill(const Document &doc, MotionPath &p);

/* With Own Motion, a bone's own constraints (a pupil's Look At keeping it in the eye) are part of where it really goes,
 * not of its own motion: turned off on the copy its path is worked out on, so the dots, the line, the handles and the
 * control points all lie on the curve its keys shape. */
static void own_constraints_off(Document &copy, const MotionPathTarget &t)
{
  Layer *rig = t.bone ? layer_find(copy, t.layer) : nullptr;
  const int b = rig ? bone_index(*rig, t.bone) : -1;
  if (b < 0) {
    return;
  }
  for (BoneConstraint &c : rig->bones[size_t(b)].constraints) {
    c.enabled = false;
  }
}

static std::atomic<uint64_t> g_places{0};
static std::atomic<int> g_places_threads{0};
static std::atomic<uint64_t> g_path_frames{0};

MotionPathPlaces motion_path_places()
{
  return {g_places.load(), g_places_threads.load(), g_path_frames.load()};
}

void motion_path_places_clear()
{
  g_places = 0;
  g_places_threads = 0;
  g_path_frames = 0;
}

/* How many threads work `count` places out side by side: one a core at most. Each place copies the document and works
 * a frame of it out, which goes no faster on a core's second thread (measured on six cores: four to six threads twice
 * as fast as one, twelve slower than six). */
static size_t place_lanes(size_t count)
{
  return std::min(count, size_t(std::max(2, parallel_threads() / 2)));
}

/* `fn(i, scratch)` for each i below `count`, side by side on the pool's threads (each works whole frames of a rig out on
 * a copy of `doc` in `scratch`, a document each thread keeps for its share: copied into again, it keeps what it has
 * room for). One at a time where bones have dynamics (see
 * dynamics_in). */
static void places_side_by_side(const Document &doc, size_t count,
                                const std::function<void(size_t, Document &)> &fn)
{
  if (count < 2 || dynamics_in(doc)) {
    Document scratch;
    for (size_t i = 0; i < count; i++) {
      fn(i, scratch);
    }
    return;
  }
  const size_t lanes = place_lanes(count);
  std::atomic<size_t> next{0};
  std::atomic<int> threads{0};
  parallel_for(lanes, 1, [&](size_t, size_t) {
    Document scratch;
    bool took = false;
    for (size_t i = next++; i < count; i = next++) {
      fn(i, scratch);
      took = true;
    }
    threads += took ? 1 : 0;
  });
  for (int was = g_places_threads; was < threads && !g_places_threads.compare_exchange_weak(was, threads);) {
  }
}

std::vector<MotionPath> motion_paths_make(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                          int first, int last, MotionPathFrames *kept)
{
  OANIM_PROFILE("motion_paths_make");
  std::vector<MotionPath> paths;
  if (targets.empty() || last < first) {
    return paths;
  }
  for (size_t i = 0; i < targets.size(); i++) {
    MotionPath p;
    p.target = targets[i];
    p.color = motion_path_color(doc, targets[i], i);
    p.first = first;
    for (const int f : motion_path_keys(doc, targets[i])) {
      if (f >= first && f <= last) {
        p.keys.push_back(f);
      }
    }
    paths.push_back(std::move(p));
  }
  /* Drawn relative to something: each frame's place taken into its space then, and back out of it as it is now. */
  const bool relative = doc.path_relative != PathRelative::World;
  const Transform2D back = relative ? relative_space(doc).inverted() : Transform2D();
  /* Own Motion: a path being edited shows only the target's own values from each frame, everything else held as the
   * frame shown. */
  std::vector<bool> own(paths.size(), false);
  for (size_t i = 0; i < paths.size(); i++) {
    own[i] = paths[i].own_motion = motion_path_own_motion(doc, paths[i].target);
  }
  /* Edits not yet keyed stay on the frame they were made on: each frame from its keys alone. The frames worked out
   * side by side in runs, each run on its own copy; then gathered in order (a frame where the target isn't found
   * keeps where it was the frame before). Kept for the caller while the document stays the same. */
  using Place = MotionPathFrames::Place;
  const size_t frames = size_t(last - first + 1);
  MotionPathFrames made;
  MotionPathFrames &f_kept = kept ? *kept : made;
  if (!kept || !f_kept.made || f_kept.made_for != f_kept.document || f_kept.targets != targets || f_kept.own != own ||
      f_kept.first != first || f_kept.last != last || f_kept.relative != doc.path_relative ||
      f_kept.relative_layer != doc.path_relative_layer || f_kept.relative_bone != doc.path_relative_bone)
  {
    f_kept.made = false;
    f_kept.places.assign(frames, std::vector<Place>(paths.size()));
    f_kept.intos.assign(frames, Transform2D());
    const size_t runs = dynamics_in(doc) ? 1 : place_lanes(frames);
    places_side_by_side(doc, runs, [&](size_t run, Document &copy) {
      copy = doc;
      for (size_t k = frames * run / runs; k < frames * (run + 1) / runs; k++) {
        const int f = first + int(k);
        copy.frame_current = f;
        animation_evaluate(copy, f);
        g_path_frames++;
        ArmaturePoseCacheScope poses;
        f_kept.intos[k] = relative ? relative_space(copy) : Transform2D();
        for (size_t i = 0; i < paths.size(); i++) {
          Place &q = f_kept.places[k][i];
          q.found = target_position(copy, paths[i].target, q.at);
          OwnValues v;
          if (own[i] && own_values(copy, paths[i].target, v)) {
            q.got = true;
            q.own_position = v.position;
            q.own_scale = v.scale;
            q.own_angle = v.angle;
          }
        }
      }
    });
    f_kept.made = true;
    f_kept.made_for = f_kept.document;
    f_kept.targets = targets;
    f_kept.own = own;
    f_kept.first = first;
    f_kept.last = last;
    f_kept.relative = doc.path_relative;
    f_kept.relative_layer = doc.path_relative_layer;
    f_kept.relative_bone = doc.path_relative_bone;
  }
  const std::vector<std::vector<Place>> &found = f_kept.places;
  const std::vector<Transform2D> &intos = f_kept.intos;
  /* Own Motion: each frame's own values put on the document as shown (its own constraints off), where they take the
   * target. A bone is posed with the rest of its bones as they are shown, as path_position_in does, with no copy of
   * the document; a point or a layer on copies of it, side by side. */
  const Document &shown = g_shown ? *g_shown : doc;
  std::vector<std::vector<float2>> own_at(paths.size());
  std::vector<std::vector<char>> own_found(paths.size());
  std::vector<size_t> on_copies;
  for (size_t i = 0; i < paths.size(); i++) {
    if (!own[i]) {
      continue;
    }
    own_at[i].assign(frames, float2(0.0f, 0.0f));
    own_found[i].assign(frames, 0);
    const MotionPathTarget &t = paths[i].target;
    if (!t.bone) {
      on_copies.push_back(i);
      continue;
    }
    const Layer *rig = layer_find(shown, t.layer);
    const int b = rig ? bone_index(*rig, t.bone) : -1;
    if (b < 0) {
      continue;
    }
    const Transform2D to_doc = layer_world_transform(shown, rig->id);
    std::vector<Bone> bones = rig->bones;
    for (BoneConstraint &c : bones[size_t(b)].constraints) {
      c.enabled = false;
    }
    for (size_t k = 0; k < frames; k++) {
      const Place &q = found[k][i];
      if (!q.got) {
        continue;
      }
      Bone &bone = bones[size_t(b)];
      bone.position = q.own_position;
      bone.scale = q.own_scale;
      bone.angle = q.own_angle;
      const std::vector<BonePose> poses = armature_pose_of(*rig, bones, false);
      own_at[i][k] = to_doc.apply(t.head ? poses[size_t(b)].knob.apply({0, 0}) : poses[size_t(b)].tail);
      own_found[i][k] = 1;
    }
  }
  if (!on_copies.empty()) {
    const size_t runs = dynamics_in(doc) ? 1 : place_lanes(frames);
    places_side_by_side(doc, runs, [&](size_t run, Document &copy) {
      for (const size_t i : on_copies) {
        copy = shown;
        own_constraints_off(copy, paths[i].target);
        for (size_t k = frames * run / runs; k < frames * (run + 1) / runs; k++) {
          const Place &q = found[k][i];
          if (!q.got) {
            continue;
          }
          own_values_set(copy, paths[i].target, {q.own_position, q.own_scale, q.own_angle});
          ArmaturePoseCacheScope poses;
          float2 at;
          own_found[i][k] = target_position(copy, paths[i].target, at) ? 1 : 0;
          own_at[i][k] = at;
        }
      }
    });
  }
  /* (Where an Own Motion path really goes: what a frame it isn't found on falls back to.) */
  std::vector<std::vector<float2>> really(paths.size());
  for (size_t k = 0; k < frames; k++) {
    const Transform2D &into = intos[k];
    for (size_t i = 0; i < paths.size(); i++) {
      MotionPath &p = paths[i];
      const Place &q = found[k][i];
      std::vector<float2> &real = p.own_motion ? really[i] : p.points;
      const float2 at = !q.found && !real.empty() ? real.back() : q.at;
      real.push_back(relative ? back.apply(into.apply(at)) : at);
      if (relative && !p.own_motion) {
        p.spaces.push_back(back * into);
      }
      if (p.own_motion) {
        /* Everything else held as shown, so drawn relative to anything it stays where it is. */
        p.points.push_back(!q.got            ? at
                           : own_found[i][k] ? own_at[i][k]
                           : !p.points.empty() ? p.points.back()
                                               : at);
      }
    }
  }
  for (MotionPath &p : paths) {
    path_shape_fill(doc, p);
  }
  for (MotionPath &p : paths) {
    float fastest = 0.0f;
    p.speed.assign(p.points.size(), 0.0f);
    for (size_t k = 1; k < p.points.size(); k++) {
      p.speed[k] = length(p.points[k] - p.points[k - 1]);
      fastest = std::max(fastest, p.speed[k]);
    }
    if (p.speed.size() > 1) {
      p.speed[0] = p.speed[1];
    }
    for (float &s : p.speed) {
      s = fastest > 1e-6f ? s / fastest : 0.0f;
    }
  }
  return paths;
}

Transform2D motion_path_drawn(const MotionPath &p, int frame, float2 at)
{
  const int k = frame - p.first;
  if (k < 0 || k >= int(p.points.size())) {
    return Transform2D();
  }
  Transform2D m = k < int(p.spaces.size()) ? p.spaces[size_t(k)] : Transform2D();
  const float2 d = p.points[size_t(k)] - m.apply(at);
  m.tx += d.x;
  m.ty += d.y;
  return m;
}

Color motion_path_color(const Document &doc, const MotionPathTarget &target, size_t index)
{
  if (const MotionPathTarget *style = motion_path_style(doc, target); style && style->has_color) {
    return style->color;
  }
  /* The first the Preferences' path colour; the rest told apart from it, away from the speed colours' red and blue. */
  if (index % 6 == 0) {
    const uint32_t rgb = preferences().path_color;
    return {float((rgb >> 16) & 0xff) / 255.0f, float((rgb >> 8) & 0xff) / 255.0f, float(rgb & 0xff) / 255.0f, 1.0f};
  }
  static const Color colors[] = {{0.95f, 0.85f, 0.35f, 1.0f}, {0.35f, 0.9f, 0.75f, 1.0f}, {0.85f, 0.5f, 0.95f, 1.0f},
                                 {0.6f, 0.95f, 0.4f, 1.0f},   {0.95f, 0.6f, 0.8f, 1.0f},  {0.9f, 0.9f, 0.9f, 1.0f}};
  return colors[index % std::size(colors)];
}

std::string motion_path_label(const Document &doc, const MotionPathTarget &target)
{
  const Layer *layer = layer_find(doc, target.layer);
  if (!layer) {
    return "?";
  }
  if (target.bone) {
    const int i = bone_index(*layer, target.bone);
    return (i >= 0 ? layer->bones[size_t(i)].name : std::string("?")) + (target.head ? " head" : " tip");
  }
  if (target.point) {
    return layer->name + " point " + std::to_string(point_index(*layer, target.point));
  }
  if (target.drawing) {
    return layer->name + (key_together_allowed(*layer) && layer->key_together ? " together" :
                          layer->has_drawing()                                  ? " drawing" :
                                                                                  " together");
  }
  return layer->name;
}

Color motion_path_speed_color(float speed)
{
  const float t = std::clamp(speed, 0.0f, 1.0f);
  const Color still = {0.25f, 0.5f, 1.0f, 1.0f}, fast = {1.0f, 0.22f, 0.18f, 1.0f};
  return {still.r + (fast.r - still.r) * t, still.g + (fast.g - still.g) * t, still.b + (fast.b - still.b) * t, 1.0f};
}

Color motion_path_time_color(float t)
{
  /* Viridis at 0.30, 0.48, 0.70 and 1.0. */
  static const Color STOPS[4] = {{0.231f, 0.322f, 0.545f, 1.0f},
                                 {0.129f, 0.569f, 0.549f, 1.0f},
                                 {0.369f, 0.788f, 0.384f, 1.0f},
                                 {0.992f, 0.906f, 0.145f, 1.0f}};
  const float x = std::clamp(t, 0.0f, 1.0f) * 3.0f;
  const int i = std::min(int(x), 2);
  const float f = x - float(i);
  const Color &a = STOPS[i], &b = STOPS[i + 1];
  return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, 1.0f};
}

Color motion_path_unlit_color()
{
  return {0.85f, 0.85f, 0.85f, 0.55f};
}

bool motion_path_stretch_time(const std::vector<int> &keys, int frame, float &r_t, int *r_start, int *r_end)
{
  const auto after = std::upper_bound(keys.begin(), keys.end(), frame);
  if (after == keys.begin() || after == keys.end() || *(after - 1) == frame) {
    return false;
  }
  const int start = *(after - 1), end = *after;
  r_t = float(frame - start) / float(end - start);
  if (r_start) {
    *r_start = start;
  }
  if (r_end) {
    *r_end = end;
  }
  return true;
}

bool motion_path_stretch_lit(int start, int end, int current, const std::vector<int> &dots)
{
  if (current >= start && current <= end) {
    return true;
  }
  return std::any_of(dots.begin(), dots.end(), [&](int f) { return f > start && f < end; });
}

std::vector<Color> motion_path_dot_colors(const Document &doc, const MotionPath &path, const std::vector<int> &dots)
{
  std::vector<Color> out(path.points.size(), motion_path_unlit_color());
  /* Every key, not only those in the range drawn, so a stretch running off its end still knows its keys. */
  const std::vector<int> keys = motion_path_keys(doc, path.target);
  for (size_t i = 0; i < out.size(); i++) {
    const int frame = path.first + int(i);
    switch (doc.path_dot_colors) {
      case PathDotColors::Speed:
        out[i] = motion_path_speed_color(i < path.speed.size() ? path.speed[i] : 0.0f);
        break;
      case PathDotColors::Path:
        out[i] = path.color;
        break;
      case PathDotColors::TimingCurrent:
      case PathDotColors::TimingAll: {
        float t = 0.0f;
        int start = 0, end = 0;
        if (motion_path_stretch_time(keys, frame, t, &start, &end) &&
            (doc.path_dot_colors == PathDotColors::TimingAll ||
             motion_path_stretch_lit(start, end, doc.frame_current, dots)))
        {
          out[i] = motion_path_time_color(t);
        }
        break;
      }
    }
  }
  return out;
}

std::vector<int> motion_path_lit_dots(const Document &doc, const MotionPathTarget &target,
                                      const std::vector<std::pair<MotionPathTarget, int>> &dots)
{
  std::vector<int> out;
  const MotionPathTarget owner = motion_path_timing_owner(doc, target);
  const bool owned = !owner.same(target);
  for (const auto &[t, f] : dots) {
    if (t.same(target) || (owned && motion_path_timing_owner(doc, t).same(owner))) {
      out.push_back(f);
    }
  }
  return out;
}

/* --- Editing (the Motion Path tool) ----------------------------------------------------------------------------- */

static void stretch_shape_line(const std::vector<Key<float2>> &keys, size_t i, std::vector<float2> &r_line,
                               std::vector<float> &r_along);
static float shape_line_project(const std::vector<float2> &line, const std::vector<float> &along, float2 v,
                                float &r_off);

namespace {

constexpr float PI_F = 3.14159265358979f;

float wrap_angle(float a)
{
  while (a > PI_F) {
    a -= 2.0f * PI_F;
  }
  while (a < -PI_F) {
    a += 2.0f * PI_F;
  }
  return a;
}

/* The plain value that moves a target where it goes: a bone's position (its head; its tip turns by its angle), a
 * point's, a layer's. Null where there is none (a connected bone's head sits on its parent's tail). */
float2 *place_value(Document &doc, const MotionPathTarget &t, Bone **r_bone)
{
  *r_bone = nullptr;
  Layer *layer = layer_find(doc, t.layer);
  if (!layer) {
    return nullptr;
  }
  if (t.bone) {
    const int i = bone_index(*layer, t.bone);
    if (i < 0) {
      return nullptr;
    }
    *r_bone = &layer->bones[size_t(i)];
    return (*r_bone)->connected ? nullptr : &(*r_bone)->position;
  }
  if (t.point) {
    const int i = point_index(*layer, t.point);
    return i < 0 ? nullptr : &layer->drawing.points[size_t(i)].pos;
  }
  return &layer->position;
}

/* Moves `value` so `t` shows at `at`: how it moves the target is found by nudging it (bones, parents and skins move
 * a point, a head or an origin as a straight-line function of it), a few times over for anything that isn't quite.
 * Where it can't get there (a pupil held inside its eye by Look At, at the edge), as near as it gets, the value going
 * no further than it takes to get there, so moving back in starts at once. False if it gets no nearer at all. */
bool solve_position(Document &doc, const MotionPathTarget &t, float2 &value, float2 at)
{
  float2 now;
  if (!target_position(doc, t, now)) {
    return false;
  }
  const float2 first = value;
  const float first_miss = length(at - now);
  float2 best = value;
  float best_miss = first_miss;
  for (int iteration = 0; iteration < 6 && best_miss >= 1e-6f; iteration++) {
    const float2 miss = at - now;
    const float h = 1e-2f;
    const float2 start = value;
    float2 dx, dy;
    value = start + float2{h, 0.0f};
    target_position(doc, t, dx);
    value = start + float2{0.0f, h};
    target_position(doc, t, dy);
    value = start;
    const float2 cx = (dx - now) / h, cy = (dy - now) / h;
    /* Least squares, damped a touch: a way the value can't move it (held at an edge) is left alone. */
    const float xx = dot(cx, cx), xy = dot(cx, cy), yy = dot(cy, cy);
    const float damp = 1e-6f * (xx + yy);
    const float a = xx + damp, d = yy + damp, det = a * d - xy * xy;
    if (xx + yy < 1e-18f || std::abs(det) < 1e-30f) {
      break;
    }
    const float gx = dot(cx, miss), gy = dot(cy, miss);
    value = start + float2{(d * gx - xy * gy) / det, (a * gy - xy * gx) / det};
    if (!target_position(doc, t, now)) {
      break;
    }
    const float m = length(at - now);
    if (m >= best_miss - 1e-9f) {
      break;
    }
    best = value;
    best_miss = m;
  }
  value = best;
  if (best_miss < 1e-6f) {
    return true;
  }
  if (best_miss >= first_miss - 1e-7f) {
    value = first;
    return false;
  }
  /* Short of it: the least change from where it was that still shows where `best` does. */
  float2 reached;
  target_position(doc, t, reached);
  float lo = 0.0f, hi = 1.0f;
  for (int i = 0; i < 16; i++) {
    const float mid = 0.5f * (lo + hi);
    value = first + (best - first) * mid;
    float2 there;
    target_position(doc, t, there);
    (length(there - reached) < 1e-6f ? hi : lo) = mid;
  }
  value = first + (best - first) * hi;
  return true;
}

/* Turns a bone so its tip points at `at`. */
bool solve_tip(Document &doc, const MotionPathTarget &t, Bone &bone, float2 at)
{
  MotionPathTarget head = t;
  head.head = true;
  for (int iteration = 0; iteration < 4; iteration++) {
    float2 h, tip;
    if (!target_position(doc, head, h) || !target_position(doc, t, tip)) {
      return false;
    }
    const float want = wrap_angle(std::atan2(at.y - h.y, at.x - h.x) - std::atan2(tip.y - h.y, tip.x - h.x));
    if (std::abs(want) < 1e-6f) {
      return true;
    }
    /* Which way (and how far) its angle turns the tip as seen: a flipped parent turns it the other way. */
    const float start = bone.angle, nudge = 1e-2f;
    bone.angle = start + nudge;
    float2 moved;
    target_position(doc, t, moved);
    bone.angle = start;
    const float rate =
        wrap_angle(std::atan2(moved.y - h.y, moved.x - h.x) - std::atan2(tip.y - h.y, tip.x - h.x)) / nudge;
    if (std::abs(rate) < 1e-4f) {
      return false;
    }
    bone.angle = start + want / rate;
  }
  return true;
}

/* A target's own values, which a breakdown blends. */
bool own_values(const Document &doc, const MotionPathTarget &t, OwnValues &r)
{
  const Layer *layer = layer_find(doc, t.layer);
  if (!layer) {
    return false;
  }
  if (t.bone) {
    const int i = bone_index(*layer, t.bone);
    if (i < 0) {
      return false;
    }
    const Bone &b = layer->bones[size_t(i)];
    r = {b.position, b.scale, b.angle};
    return true;
  }
  if (t.point) {
    const int i = point_index(*layer, t.point);
    if (i < 0) {
      return false;
    }
    r = {layer->drawing.points[size_t(i)].pos, {1.0f, 1.0f}, 0.0f};
    return true;
  }
  r = {layer->position, layer->scale, layer->rotation};
  return true;
}

void own_values_set(Document &doc, const MotionPathTarget &t, const OwnValues &v)
{
  Layer *layer = layer_find(doc, t.layer);
  if (t.bone) {
    Bone &b = layer->bones[size_t(bone_index(*layer, t.bone))];
    b.position = v.position;
    b.scale = v.scale;
    b.angle = v.angle;
  }
  else if (t.point) {
    layer->drawing.points[size_t(point_index(*layer, t.point))].pos = v.position;
  }
  else {
    layer->position = v.position;
    layer->scale = v.scale;
    layer->rotation = v.angle;
  }
}

/* `copy` as `t`'s path has it on `frame`: evaluated then, or with Own Motion only `t`'s own values from then and
 * everything else held as shown (the document's, or what a MotionPathOnFrame holds), as the path is drawn. */
/* As path_evaluate, `held` the document a MotionPathOnFrame holds as shown (null for none). */
static void path_evaluate_held(Document &copy, const MotionPathTarget &t, int frame, const Document *held)
{
  if (!motion_path_own_motion(copy, t)) {
    animation_evaluate(copy, frame);
    return;
  }
  Document then = copy;
  then.frame_current = frame;
  animation_evaluate(then, frame);
  OwnValues v;
  const bool got = own_values(then, t, v);
  if (held) {
    copy = *held;
  }
  if (got) {
    own_values_set(copy, t, v);
  }
  own_constraints_off(copy, t);
}

void path_evaluate(Document &copy, const MotionPathTarget &t, int frame)
{
  path_evaluate_held(copy, t, frame, g_shown);
}

/* As path_position, on `doc` itself (a copy made for it, which it changes); `held` is the document a MotionPathOnFrame
 * holds as shown (null for none), `as_was` `doc` as it was, shown for Own Motion when none is held. (Given, not read
 * from g_shown: places are worked out on other threads too.) */
static bool path_position_in(Document &doc, const Document *held, const Document &as_was, const MotionPathTarget &t,
                             int frame, float2 &r_at)
{
  const Document &shown = held ? *held : as_was;
  if (t.bone && motion_path_own_motion(doc, t)) {
    /* A bone with Own Motion, as path_evaluate has it, without copying the document twice more: its own values from
     * the frame, posed with the rest of its bones as shown (its own constraints off). */
    doc.frame_current = frame;
    animation_evaluate(doc, frame);
    OwnValues v;
    const bool got = own_values(doc, t, v);
    const Layer *rig = layer_find(shown, t.layer);
    const int i = rig ? bone_index(*rig, t.bone) : -1;
    if (i < 0) {
      return false;
    }
    std::vector<Bone> bones = rig->bones;
    Bone &b = bones[size_t(i)];
    if (got) {
      b.position = v.position;
      b.scale = v.scale;
      b.angle = v.angle;
    }
    for (BoneConstraint &c : b.constraints) {
      c.enabled = false;
    }
    const std::vector<BonePose> poses = armature_pose_of(*rig, bones, false);
    const float2 at = t.head ? poses[size_t(i)].knob.apply({0, 0}) : poses[size_t(i)].tail;
    r_at = layer_world_transform(shown, rig->id).apply(at);
    return true;
  }
  path_evaluate_held(doc, t, frame, held);
  return target_position(doc, t, r_at);
}

/* Where `t` is on `frame` as its path is drawn (see path_evaluate); `doc` stays as it is. */
bool path_position(const Document &doc, const MotionPathTarget &t, int frame, float2 &r_at)
{
  OANIM_PROFILE("path_position");
  Document copy = doc;
  return path_position_in(copy, g_shown, doc, t, frame, r_at);
}

/* A place of `t` worked out by held_places: the frame, the value its position channel is given there, and where `t`
 * then is. */
struct HeldPlace {
  int frame = 0;
  float2 value;
  float2 at;
  bool ok = false;
};

/* The places kept while a MotionPathPlacesScope is open on this thread. */
thread_local MotionPathPlacesKept *t_places_kept = nullptr;

}  // namespace

MotionPathPlacesScope::MotionPathPlacesScope(MotionPathPlacesKept &kept) : was_(t_places_kept)
{
  if (kept.made_for != kept.document) {
    kept.entries.clear();
    kept.made_for = kept.document;
  }
  t_places_kept = &kept;
}

MotionPathPlacesScope::~MotionPathPlacesScope()
{
  t_places_kept = was_;
}

void MotionPathKept::sync(const Document &doc, const uint64_t content_changes, const uint64_t serial)
{
  const uint64_t keys = channel_keys_generation();
  if (doc_ != &doc || changes_ != content_changes || serial_ != serial || keys_ != keys) {
    doc_ = &doc;
    changes_ = content_changes;
    serial_ = serial;
    keys_ = keys;
    frames.document++;
  }
  places.document = frames.document;
}

namespace {

/* Kinds of copy held_places works places out on, to keep them by (0: not kept). */
enum { PLACES_UNKEPT = 0, PLACES_KEY_VALUE = 1, PLACES_HELD_KEY = 2 };

/* Each of `places` worked out on its own copy of `copy`, given its value by `set` (on the copy's channel), side by side
 * (places_side_by_side). `copy` stays as it is: the document as shown for Own Motion, as path_position has it. As
 * path_position_in, in two steps: the frame worked out (its own values with Own Motion, else where `t` is), which a
 * MotionPathPlacesScope keeps by `kind`, then placed with the rest as shown. */
static void held_places(const Document &copy, const MotionPathTarget &t, std::vector<HeldPlace> &places,
                        const std::function<bool(Document &, const HeldPlace &)> &set, int kind = PLACES_UNKEPT)
{
  OANIM_PROFILE("held_places");
  const Document *held = g_shown;
  using Entry = MotionPathPlacesKept::Entry;
  MotionPathPlacesKept *kept = kind != PLACES_UNKEPT ? t_places_kept : nullptr;
  std::vector<Entry> worked(places.size());
  std::vector<char> known(places.size(), 0);
  for (size_t i = 0; kept && i < places.size(); i++) {
    for (const Entry &e : kept->entries) {
      if (e.kind == kind && e.frame == places[i].frame && e.value == places[i].value && e.target == t) {
        worked[i] = e;
        known[i] = 1;
        break;
      }
    }
  }
  places_side_by_side(copy, places.size(), [&](size_t i, Document &mine) {
    HeldPlace &p = places[i];
    Entry &w = worked[i];
    if (!known[i]) {
      g_places++;
      mine = copy;
      mine.frame_current = p.frame;
      w.set = set(mine, p);
      w.own = motion_path_own_motion(mine, t);
      if (w.set) {
        animation_evaluate(mine, p.frame);
        if (w.own) {
          OwnValues v;
          w.got = own_values(mine, t, v);
          w.own_position = v.position;
          w.own_scale = v.scale;
          w.own_angle = v.angle;
          w.ok = true;
        }
        else {
          w.ok = target_position(mine, t, w.at);
        }
      }
    }
  });
  /* Then placed: Own Motion's own values put on the document as shown, its own constraints off. A bone posed with the
   * rest of its bones as shown, with no copy of the document; a point or a layer on a copy of it, one a thread. */
  const Document &shown = held ? *held : copy;
  std::vector<size_t> on_copies;
  for (size_t i = 0; i < places.size(); i++) {
    HeldPlace &p = places[i];
    const Entry &w = worked[i];
    p.ok = false;
    if (!w.set || !w.ok) {
      continue;
    }
    if (!w.own) {
      p.at = w.at;
      p.ok = true;
      continue;
    }
    if (!t.bone) {
      on_copies.push_back(i);
      continue;
    }
    const Layer *rig = layer_find(shown, t.layer);
    const int b = rig ? bone_index(*rig, t.bone) : -1;
    if (b < 0) {
      continue;
    }
    std::vector<Bone> bones = rig->bones;
    Bone &bone = bones[size_t(b)];
    if (w.got) {
      bone.position = w.own_position;
      bone.scale = w.own_scale;
      bone.angle = w.own_angle;
    }
    for (BoneConstraint &c : bone.constraints) {
      c.enabled = false;
    }
    const std::vector<BonePose> poses = armature_pose_of(*rig, bones, false);
    const float2 at = t.head ? poses[size_t(b)].knob.apply({0, 0}) : poses[size_t(b)].tail;
    p.at = layer_world_transform(shown, rig->id).apply(at);
    p.ok = true;
  }
  OwnValues as_shown;
  if (!on_copies.empty() && own_values(shown, t, as_shown)) {
    const size_t count = on_copies.size(), runs = place_lanes(count);
    places_side_by_side(copy, runs, [&](size_t run, Document &mine) {
      mine = shown;
      own_constraints_off(mine, t);
      for (size_t j = count * run / runs; j < count * (run + 1) / runs; j++) {
        HeldPlace &p = places[on_copies[j]];
        const Entry &w = worked[on_copies[j]];
        /* (With nothing held, the copy as given its value, not worked out.) */
        if (!held) {
          mine.frame_current = p.frame;
          set(mine, p);
        }
        own_values_set(mine, t, w.got ? OwnValues{w.own_position, w.own_scale, w.own_angle} : as_shown);
        p.ok = target_position(mine, t, p.at);
      }
    });
  }
  for (size_t i = 0; kept && i < places.size(); i++) {
    if (!known[i]) {
      Entry &e = worked[i];
      e.target = t;
      e.kind = kind;
      e.frame = places[i].frame;
      e.value = places[i].value;
      kept->entries.push_back(std::move(e));
    }
  }
}

/* The position keys that carry `t`'s path handles, or null (a bone's tip, a connected bone's head). */
Channel<float2> *handle_channel(Document &doc, const MotionPathTarget &t)
{
  Layer *layer = layer_find(doc, t.layer);
  if (!layer || t.drawing) {
    return nullptr;
  }
  if (t.bone) {
    const int i = bone_index(*layer, t.bone);
    if (i < 0 || layer->bones[size_t(i)].connected || tip_turns(doc, t)) {
      return nullptr;
    }
    return &layer->bones[size_t(i)].position_keys;
  }
  if (t.point) {
    const int i = point_index(*layer, t.point);
    return i < 0 ? nullptr : &layer->drawing.points[size_t(i)].pos_keys;
  }
  return &layer->position_keys;
}

/* How `t` shows on `frame` as its position key there changes: where it is (r_at) and how far it moves for a step of
 * the key's value in x (r_cx) and y (r_cy). Keys hold values in their own space (a bone's turned with its rest, a
 * point's from where it sits at rest), so this is found by nudging the key on a copy. */
bool key_jacobian(const Document &doc, const MotionPathTarget &t, int frame, float2 &r_at, float2 &r_cx, float2 &r_cy)
{
  Document copy = doc;
  Channel<float2> *ch = handle_channel(copy, t);
  Key<float2> *key = ch ? ch->key_at(frame) : nullptr;
  if (!key) {
    return false;
  }
  copy.frame_current = frame;
  const float2 start = key->value;
  const float h = 1e-2f;
  std::vector<HeldPlace> places(3);
  places[0].value = start;
  places[1].value = start + float2{h, 0.0f};
  places[2].value = start + float2{0.0f, h};
  for (HeldPlace &p : places) {
    p.frame = frame;
  }
  held_places(copy, t, places, [&](Document &mine, const HeldPlace &p) {
    Channel<float2> *c = handle_channel(mine, t);
    Key<float2> *k = c ? c->key_at(frame) : nullptr;
    if (k) {
      k->value = p.value;
    }
    return k != nullptr;
  }, PLACES_KEY_VALUE);
  r_at = places[0].at;
  r_cx = (places[1].at - r_at) / h;
  r_cy = (places[2].at - r_at) / h;
  return places[0].ok && places[1].ok && places[2].ok;
}

/* The target's own keyed frames (after frame 0, the setup). */
std::vector<int> own_key_frames(const Document &doc, const MotionPathTarget &t)
{
  std::vector<int> frames;
  for (const auto &[frame, summary] : layers_key_summary(motion_path_key_layers(doc, t), motion_path_key_filter(doc, t))) {
    if (frame > 0) {
      frames.push_back(frame);
    }
  }
  return frames;
}

/* Smooth Arc's curve: where key `keys[i]` would be on a curve over time (a parabola, or a line through two) fitted
 * to up to two keys either side of it. */
bool arc_fit(const Document &doc, const MotionPathTarget &target, const std::vector<int> &keys, size_t i,
             float2 &r_at)
{
  const int k = keys[i];
  std::vector<std::pair<double, float2>> near;
  for (size_t j = (i >= 2 ? i - 2 : 0); j < std::min(keys.size(), i + 3); j++) {
    float2 at;
    if (j != i && motion_path_position_at(doc, target, keys[j], at)) {
      near.push_back({double(keys[j] - k), at});
    }
  }
  if (near.size() < 2) {
    return false;
  }
  /* Least squares p(t) = c0 + c1 t + c2 t^2 with t = 0 on the key: c0 is where it goes. */
  const int terms = near.size() >= 3 ? 3 : 2;
  double m[3][3] = {}, rx[3] = {}, ry[3] = {};
  for (const auto &[t, p] : near) {
    const double basis[3] = {1.0, t, t * t};
    for (int r = 0; r < terms; r++) {
      for (int c = 0; c < terms; c++) {
        m[r][c] += basis[r] * basis[c];
      }
      rx[r] += basis[r] * p.x;
      ry[r] += basis[r] * p.y;
    }
  }
  for (int c = 0; c < terms; c++) {
    int pivot = c;
    for (int r = c + 1; r < terms; r++) {
      if (std::abs(m[r][c]) > std::abs(m[pivot][c])) {
        pivot = r;
      }
    }
    for (int cc = 0; cc < 3; cc++) {
      std::swap(m[c][cc], m[pivot][cc]);
    }
    std::swap(rx[c], rx[pivot]);
    std::swap(ry[c], ry[pivot]);
    if (std::abs(m[c][c]) < 1e-12) {
      return false;
    }
    for (int r = 0; r < terms; r++) {
      if (r != c) {
        const double f = m[r][c] / m[c][c];
        for (int cc = 0; cc < terms; cc++) {
          m[r][cc] -= f * m[c][cc];
        }
        rx[r] -= f * rx[c];
        ry[r] -= f * ry[c];
      }
    }
  }
  r_at = float2{float(rx[0] / m[0][0]), float(ry[0] / m[0][0])};
  return true;
}

/* A stretch timed by its mode alone, whatever way it goes (Smooth and Noisy are timed by how they move). */
static bool interp_timed_by_mode(Interp interp)
{
  return interp == Interp::Linear || interp == Interp::EaseIn || interp == Interp::EaseOut ||
         interp == Interp::EaseInOut || interp == Interp::Back || interp == Interp::Bounce || interp == Interp::Elastic ||
         interp == Interp::Bezier;
}

/* How far along its path (by length, as a Custom stretch plays: stretch_path_point) each frame of the stretch arriving
 * at keys[i] is, from `start` over `n` frames, into `r_u`. False when a frame is off the path (Noisy) or the places
 * found don't go forward (a path crossing itself): then the frames' straight steps measure it. */
static bool stretch_path_progress(const Channel<float2> &ch, size_t i, int start, int n, std::vector<float> &r_u)
{
  std::vector<float2> line;
  std::vector<float> along;
  stretch_shape_line(ch.keys, i, line, along);
  const float total = along.empty() ? 0.0f : along.back();
  if (total < 1e-9f) {
    return false;
  }
  r_u.assign(size_t(n + 1), 0.0f);
  for (int j = 0; j <= n; j++) {
    float off = 0.0f;
    const float at = shape_line_project(line, along, ch.evaluate(start + j), off);
    if (off > total * 1e-3f || (j > 0 && at < r_u[size_t(j - 1)] * total - total * 1e-4f)) {
      return false;
    }
    r_u[size_t(j)] = at / total;
  }
  return true;
}

/* A stretch's speeds as it moves now: one for each frame from `start`, the change since the frame before (along the
 * path for a position), as a share of the even speed. */
template<typename T> std::vector<float> stretch_speeds(const Channel<T> &ch, size_t i, int start, int n)
{
  const Key<T> &b = ch.keys[i];
  std::vector<float> s(size_t(n), 1.0f);
  /* Timed by its mode alone (a controlled stretch is timed by how it moves). */
  bool shaped = interp_timed_by_mode(b.interp);
  if constexpr (std::is_same_v<T, float2>) {
    shaped = shaped && b.path_controls.empty();
  }
  if (b.interp == Interp::Custom && !b.timing.empty()) {
    /* Its curve at each frame (whatever length the stretch is now: the curve stretches with it). */
    s = timing_speeds(b.timing, n);
  }
  else if (shaped) {
    /* Its mode's progress, signed: Back and Elastic go past the key and come back to it. */
    const KeySettings settings = key_settings(b);
    float before = 0.0f;
    for (int j = 0; j < n; j++) {
      const float now = interp_curve(settings, float(j + 1) / float(n));
      s[size_t(j)] = (now - before) * float(n);
      before = now;
    }
  }
  else {
    if constexpr (std::is_same_v<T, float2>) {
      /* A position by how far along its path each frame is, as Custom plays it (not by straight steps, which cut a
       * turn's corner). */
      std::vector<float> u;
      if (stretch_path_progress(ch, i, start, n, u)) {
        for (int j = 0; j < n; j++) {
          s[size_t(j)] = std::max(u[size_t(j + 1)] - u[size_t(j)], 0.0f) * float(n);
        }
        float sum = 0.0f;
        for (const float v : s) {
          sum += v;
        }
        if (sum > 1e-9f) {
          for (float &v : s) {
            v *= float(n) / sum;
          }
          return s;
        }
      }
    }
    T before = ch.evaluate(start);
    for (int j = 0; j < n; j++) {
      const T now = ch.evaluate(start + j + 1);
      if constexpr (std::is_same_v<T, float>) {
        s[size_t(j)] = std::abs(now - before);
      }
      else {
        s[size_t(j)] = length(now - before);
      }
      before = now;
    }
  }
  /* A Custom or shaped stretch's speeds keep their signs (going back past a key); measured ones are how far it went. */
  const bool custom = (b.interp == Interp::Custom && !b.timing.empty()) || shaped;
  float total = 0.0f;
  for (float &v : s) {
    if (!custom) {
      v = std::max(v, 0.0f);
    }
    total += v;
  }
  if (total <= 1e-9f) {
    std::fill(s.begin(), s.end(), 1.0f);
    return s;
  }
  for (float &v : s) {
    /* No floor: a frame held still measures 0, as it plays (a slide elsewhere mustn't set it going). */
    v = v * float(n) / total;
  }
  return s;
}

/* The brush on one channel: each stretch with frames under it becomes Custom, the speeds of those frames changed and
 * the rest of the stretch taking up the difference, so it still arrives on its key's frame. */
template<typename T>
int paint_speeds(Channel<T> &ch, const std::map<int, float> &falloff, PathBrush brush, const PathDab &dab)
{
  int changed = 0;
  for (size_t i = 1; i < ch.keys.size(); i++) {
    const Key<T> &a = ch.keys[i - 1], &b = ch.keys[i];
    const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
    const int n = b.frame - start;
    if (n < 1 || b.interp == Interp::Step || b.interp == Interp::Pose || b.interp == Interp::Cycle || a.interp == Interp::Cycle) {
      continue;
    }
    std::vector<float> w(size_t(n), 0.0f);
    bool any = false;
    for (int j = 0; j < n; j++) {
      if (const auto it = falloff.find(start + j + 1); it != falloff.end() && it->second > 0.0f) {
        w[size_t(j)] = it->second;
        any = true;
      }
    }
    if (!any) {
      continue;
    }
    std::vector<float> s = stretch_speeds(ch, i, start, n);
    if constexpr (std::is_same_v<T, float2>) {
      /* A straight stretch keeps its shape as Custom (which runs along Smooth's curve while Auto). */
      if (!interp_path_smooth(b.interp)) {
        key_path_handles_freeze(ch.keys, i - 1);
        key_path_handles_freeze(ch.keys, i);
      }
    }
    const std::vector<float> was = s;
    const PathBrush mode = brush;
    const float strength = std::max(dab.strength, 0.0f);
    for (int j = 0; j < n; j++) {
      const float f = w[size_t(j)];
      if (f <= 0.0f) {
        continue;
      }
      float &v = s[size_t(j)];
      const float neighbours =
          0.5f * (was[size_t(std::max(j - 1, 0))] + was[size_t(std::min(j + 1, n - 1))]);
      switch (mode) {
        case PathBrush::BlendSpeed:
          v += (dab.invert ? (v - neighbours) : (neighbours - v)) * std::min(f * strength, 1.0f);
          break;
        case PathBrush::Pinch:
          /* Slower under the brush (its dots crowd together toward the middle); Ctrl, faster (they spread). */
          v *= dab.invert ? 1.0f + 0.5f * std::min(f * strength, 1.0f) : 1.0f - 0.5f * std::min(f * strength, 1.0f);
          break;
        default:
          break;
      }
      /* A frame going back (overshoot, a bounce) keeps going back; the rest never stop dead. */
      v = was[size_t(j)] < 0.0f ? v : std::max(v, 0.02f);
    }
    /* Arriving on its frame: the frames not under the brush take up the difference, or all of them where those can't
     * (the brushed ones would leave them next to nothing). */
    float brushed = 0.0f, rest = 0.0f;
    for (int j = 0; j < n; j++) {
      (w[size_t(j)] > 0.0f ? brushed : rest) += s[size_t(j)];
    }
    if (rest > 1e-6f && float(n) - brushed > 0.05f * rest) {
      const float scale = (float(n) - brushed) / rest;
      for (int j = 0; j < n; j++) {
        if (w[size_t(j)] <= 0.0f) {
          s[size_t(j)] = s[size_t(j)] < 0.0f ? s[size_t(j)] * scale : std::max(s[size_t(j)] * scale, 0.02f);
        }
      }
    }
    else {
      const float scale = float(n) / std::max(brushed + rest, 1e-9f);
      for (float &v : s) {
        v = v < 0.0f ? v * scale : std::max(v * scale, 0.02f);
      }
    }
    key_speeds_set(ch.keys, i, std::move(s));
    changed++;
  }
  return changed;
}

/* The stretch of `ch` that `frame` is strictly inside (after any hold): the arriving key's index, the stretch's first
 * frame and how many frames it moves over. False on a key, while held, or in a stretch that doesn't move. */
template<typename T> bool stretch_of(const Channel<T> &ch, int frame, size_t &r_i, int &r_start, int &r_n)
{
  for (size_t i = 1; i < ch.keys.size(); i++) {
    const Key<T> &a = ch.keys[i - 1], &b = ch.keys[i];
    if (frame >= b.frame) {
      continue;
    }
    const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
    if (frame <= start || b.interp == Interp::Step || b.interp == Interp::Pose || b.interp == Interp::Cycle ||
        a.interp == Interp::Cycle)
    {
      return false;
    }
    r_i = i;
    r_start = start;
    r_n = b.frame - start;
    return true;
  }
  return false;
}

/* On 2s or 3s (the start key's interval) a held frame plays the pose of the frame its step began on: `frame` made
 * that one. False when that is the stretch's start (the key's own pose, nothing to slide). */
template<typename T> static bool played_frame(const Channel<T> &ch, size_t i, int start, int &frame)
{
  const int interval = std::max(ch.keys[i - 1].interval, 1);
  frame = start + (frame - start) / interval * interval;
  return frame > start;
}

/* The stretch made Custom as it moves now (a straight position stretch keeping its line): its speeds. */
template<typename T> std::vector<float> stretch_make_custom(Channel<T> &ch, size_t i, int start, int n)
{
  std::vector<float> s = stretch_speeds(ch, i, start, n);
  if constexpr (std::is_same_v<T, float2>) {
    if (!interp_path_smooth(ch.keys[i].interp)) {
      key_path_handles_freeze(ch.keys, i - 1);
      key_path_handles_freeze(ch.keys, i);
    }
  }
  key_speeds_set(ch.keys, i, s);
  return s;
}

std::vector<float> speeds_progress(const std::vector<float> &s);
std::vector<float> progress_speeds(const std::vector<float> &u);

/* Progress `u` (0 at the stretch's start to 1 at its key, a value a frame) put back in order around dot `k0`, which
 * stays: none passing another or the dots `lo` and `hi` that bound them (the keys, by default), each frame at least
 * `gap` on. */
void order_progress(std::vector<float> &u, int k0, float gap, int lo = 0, int hi = -1)
{
  hi = hi < 0 ? int(u.size()) - 1 : hi;
  for (int k = k0 + 1; k < hi; k++) {
    u[size_t(k)] = std::max(u[size_t(k)], u[size_t(k - 1)] + gap);
  }
  for (int k = k0 - 1; k > lo; k--) {
    u[size_t(k)] = std::min(u[size_t(k)], u[size_t(k + 1)] - gap);
  }
  for (int k = hi - 1; k > k0; k--) {
    u[size_t(k)] = std::min(u[size_t(k)], u[size_t(k + 1)] - gap);
  }
  for (int k = lo + 1; k < k0; k++) {
    u[size_t(k)] = std::max(u[size_t(k)], u[size_t(k - 1)] + gap);
  }
}

/* A stretch's progress at each of its frames from its speeds, and back. */
std::vector<float> speeds_progress(const std::vector<float> &s)
{
  const int n = int(s.size());
  std::vector<float> u(size_t(n + 1), 0.0f);
  for (int k = 1; k <= n; k++) {
    u[size_t(k)] = u[size_t(k - 1)] + s[size_t(k - 1)];
  }
  for (float &v : u) {
    v /= std::max(u[size_t(n)], 1e-9f);
  }
  return u;
}

std::vector<float> progress_speeds(const std::vector<float> &u)
{
  const int n = int(u.size()) - 1;
  std::vector<float> out(static_cast<size_t>(n), 0.0f);
  for (int k = 0; k < n; k++) {
    out[size_t(k)] = (u[size_t(k + 1)] - u[size_t(k)]) * float(n);
  }
  return out;
}

/* Speeds `s` (a stretch of n frames) with dot `k0` slid to progress `want` (0-1 of the way along), the dots around it
 * following by the falloff, all kept in order and off the keys. */
std::vector<float> slid_speeds(const std::vector<float> &s, int k0, float want, const PathSlide &slide)
{
  const int n = int(s.size());
  std::vector<float> u = speeds_progress(s);
  if (!slide.keep_order) {
    /* Free: past the key and back (overshoot), or back before the start. */
    want = std::clamp(want, -1.0f, 2.0f);
    const float d = want - u[size_t(k0)];
    for (int k = 1; k < n; k++) {
      const float w = k == k0                ? 1.0f
                      : slide.proportional ? proportional_weight(slide.falloff, float(std::abs(k - k0)), slide.size, k)
                                           : 0.0f;
      u[size_t(k)] += d * w;
    }
    u[size_t(k0)] = want;
    return progress_speeds(u);
  }
  /* Kept in order within the run of dots going forward that it is in, between the dots that bound it: the keys, or
   * where an overshoot turns (past its key and back, or back before the start), which stays as it is. A dot held
   * still (on 2s or 3s, or a pause) is going forward, not turning. */
  const auto turns = [&](int j) {
    return j <= 0 || j >= n || !(u[size_t(j - 1)] <= u[size_t(j)] && u[size_t(j)] <= u[size_t(j + 1)]);
  };
  if (turns(k0)) {
    /* On an overshoot's turn or its way back: it alone, between the dots either side. */
    const float a = u[size_t(k0 - 1)], b = u[size_t(k0 + 1)];
    u[size_t(k0)] = std::clamp(want, std::min(a, b), std::max(a, b));
    return progress_speeds(u);
  }
  int lo = k0 - 1, hi = k0 + 1;
  while (!turns(lo)) {
    lo--;
  }
  while (!turns(hi)) {
    hi++;
  }
  /* Each frame keeps at least the speed brushes' least. */
  const float gap = 0.02f / float(n);
  want = std::clamp(want, u[size_t(lo)] + float(k0 - lo) * gap, u[size_t(hi)] - float(hi - k0) * gap);
  const float d = want - u[size_t(k0)];
  for (int k = lo + 1; k < hi; k++) {
    const float w = k == k0                ? 1.0f
                    : slide.proportional ? proportional_weight(slide.falloff, float(std::abs(k - k0)), slide.size, k)
                                         : 0.0f;
    u[size_t(k)] += d * w;
  }
  u[size_t(k0)] = want;
  order_progress(u, k0, gap, lo, hi);
  return progress_speeds(u);
}

/* Alt S with Edit on one channel: the dots around `pivot` (a dot, or a key: the stretches either side) brought
 * `factor` as far from it along the path, those within the reach (by the falloff with proportional editing, else
 * all alike), never past the keys. Returns how many stretches changed. */
template<typename T> int scale_stretches(Channel<T> &ch, int pivot, float factor, const PathSlide &slide)
{
  int changed = 0;
  for (size_t i = 1; i < ch.keys.size(); i++) {
    const int a_frame = ch.keys[i - 1].frame, b_frame = ch.keys[i].frame;
    const int start = std::min(a_frame + std::max(ch.keys[i - 1].hold, 0), b_frame);
    const int n = b_frame - start;
    const Interp interp = ch.keys[i].interp;
    if (n < 1 || interp == Interp::Step || interp == Interp::Pose || interp == Interp::Cycle ||
        ch.keys[i - 1].interp == Interp::Cycle)
    {
      continue;
    }
    int k0 = 0;
    if (pivot > start && pivot < b_frame) {
      k0 = pivot - start;
    }
    else if (pivot == b_frame) {
      k0 = n;
    }
    else if (pivot == a_frame) {
      k0 = 0;
    }
    else {
      continue;
    }
    std::vector<float> u = speeds_progress(stretch_make_custom(ch, i, start, n));
    /* An overshoot (dots out of order) is kept as it is scaled. */
    const bool ordered = std::is_sorted(u.begin(), u.end());
    const float at = u[size_t(k0)];
    for (int k = 1; k < n; k++) {
      const float d = float(std::abs(start + k - pivot));
      const float w = slide.proportional ? proportional_weight(slide.falloff, d, slide.size, k)
                                         : (d < slide.size ? 1.0f : 0.0f);
      u[size_t(k)] = at + (u[size_t(k)] - at) * (1.0f + (factor - 1.0f) * w);
    }
    if (ordered) {
      order_progress(u, k0, 0.02f / float(n));
    }
    key_speeds_set(ch.keys, i, progress_speeds(u));
    changed++;
  }
  return changed;
}

/* How `t` shows on `frame` as the value of its position channel there changes (as key_jacobian, for a frame with or
 * without a key): the channel is held at `value` on a copy and nudged. Without `r_cx` and `r_cy` only where it shows
 * (a third of the work: on a rig each is a whole frame worked out). */
/* `doc` with `t`'s position channel held on one key (value_jacobian's), as a place's copies start from; false without
 * one. */
static bool held_copy(const Document &doc, const MotionPathTarget &t, Document &r_copy)
{
  r_copy = doc;
  Channel<float2> *ch = handle_channel(r_copy, t);
  if (!ch || ch->keys.empty()) {
    return false;
  }
  Key<float2> held = ch->keys.front();
  held.interp = Interp::Linear;
  held.timing = {};
  held.path_handles = PathHandles::Auto;
  ch->keys = {held};
  return true;
}

/* A held copy's places (held_places), its one key on each place's frame with its value. */
static void held_copy_places(const Document &copy, const MotionPathTarget &t, std::vector<HeldPlace> &places)
{
  held_places(copy, t, places, [&](Document &mine, const HeldPlace &p) {
    Channel<float2> *ch = handle_channel(mine, t);
    if (!ch || ch->keys.size() != 1) {
      return false;
    }
    ch->keys[0].frame = p.frame;
    ch->keys[0].value = p.value;
    return true;
  }, PLACES_HELD_KEY);
}

constexpr float VALUE_STEP = 1e-2f;

bool value_jacobian(const Document &doc, const MotionPathTarget &t, int frame, float2 value, float2 &r_at, float2 *r_cx,
                    float2 *r_cy)
{
  OANIM_PROFILE("value_jacobian");
  Document copy;
  if (!held_copy(doc, t, copy)) {
    return false;
  }
  const bool slopes = r_cx && r_cy;
  std::vector<HeldPlace> places(slopes ? 3 : 1);
  places[0].value = value;
  if (slopes) {
    places[1].value = value + float2{VALUE_STEP, 0.0f};
    places[2].value = value + float2{0.0f, VALUE_STEP};
  }
  for (HeldPlace &p : places) {
    p.frame = frame;
  }
  held_copy_places(copy, t, places);
  r_at = places[0].at;
  if (!slopes) {
    return places[0].ok;
  }
  *r_cx = (places[1].at - r_at) / VALUE_STEP;
  *r_cy = (places[2].at - r_at) / VALUE_STEP;
  return places[0].ok && places[1].ok && places[2].ok;
}

/* The progress (0-1 by length) along the stretch arriving at keys[i] nearest `v` (the keys' own space), looked for
 * from `from` to `to` (past 1: on past the key along its end's direction, where an overshoot goes). */
float nearest_progress(const std::vector<Key<float2>> &keys, size_t i, float2 v, float from = 0.0f, float to = 1.0f)
{
  constexpr int SAMPLES = 128;
  const float step = (to - from) / SAMPLES;
  float best = from, best_d = 1e30f;
  for (int j = 0; j <= SAMPLES; j++) {
    const float u = from + float(j) * step;
    const float2 q = stretch_path_point(keys, i, u) - v;
    const float d = q.x * q.x + q.y * q.y;
    if (d < best_d) {
      best_d = d;
      best = u;
    }
  }
  /* Narrowed down between the samples either side. */
  float lo = std::max(from, best - step), hi = std::min(to, best + step);
  for (int it = 0; it < 30; it++) {
    const float a = lo + (hi - lo) / 3.0f, b = hi - (hi - lo) / 3.0f;
    const float2 qa = stretch_path_point(keys, i, a) - v, qb = stretch_path_point(keys, i, b) - v;
    if (qa.x * qa.x + qa.y * qa.y < qb.x * qb.x + qb.y * qb.y) {
      hi = b;
    }
    else {
      lo = a;
    }
  }
  return 0.5f * (lo + hi);
}

}  // namespace

/* The curve a stretch's dots are placed on (the keys' own space), as a line dense enough to draw: through its control
 * points (each a vertex, so a crease stays a corner), else its Bezier. With the length along it at each vertex. */
static void stretch_shape_line(const std::vector<Key<float2>> &keys, size_t i, std::vector<float2> &r_line,
                               std::vector<float> &r_along)
{
  r_line.clear();
  if (!keys[i].path_controls.empty()) {
    r_line = stretch_control_line(keys, i, 96);
  }
  else {
    constexpr int SAMPLES = 256;
    for (int j = 0; j <= SAMPLES; j++) {
      r_line.push_back(stretch_path_point(keys, i, float(j) / SAMPLES));
    }
  }
  r_along.assign(r_line.size(), 0.0f);
  for (size_t j = 1; j < r_line.size(); j++) {
    r_along[j] = r_along[j - 1] + length(r_line[j] - r_line[j - 1]);
  }
}

/* Where along `line` (by length) the point nearest `v` is, and how far off it `v` is. */
static float shape_line_project(const std::vector<float2> &line, const std::vector<float> &along, float2 v,
                                float &r_off)
{
  float best = along.empty() ? 0.0f : along.front(), best_d = 1e30f;
  for (size_t j = 1; j < line.size(); j++) {
    const float2 a = line[j - 1], d = line[j] - a;
    const float dd = dot(d, d);
    const float t = dd > 1e-14f ? std::clamp(dot(v - a, d) / dd, 0.0f, 1.0f) : 0.0f;
    const float s = along[j - 1] + (along[j] - along[j - 1]) * t;
    const float dist = length(v - (a + d * t));
    if (dist < best_d) {
      best_d = dist;
      best = s;
    }
  }
  r_off = best_d;
  return best;
}

static void path_shape_fill(const Document &doc, MotionPath &p)
{
  const size_t n = p.points.size();
  p.shape.assign(n, {});
  p.shape_known.assign(n, false);
  const Channel<float2> *ch = n >= 2 ? handle_channel(const_cast<Document &>(doc), p.target) : nullptr;
  if (!ch || ch->keys.size() < 2) {
    return;
  }
  const std::vector<Key<float2>> &keys = ch->keys;
  std::vector<float2> values(n);
  for (size_t i = 0; i < n; i++) {
    values[i] = ch->evaluate(p.first + int(i));
  }
  /* From the keys' space to the drawing's, per step: the turn, scale and move that puts the two values on the two dots
   * as drawn (so a parent moving, turning or scaling is followed, and the line always meets its dots); mirrored where
   * the drawing is (a parent flipped), told from the first three dots not in a line. */
  const auto cross = [](float2 a, float2 b) { return a.x * b.y - a.y * b.x; };
  bool mirrored = false;
  for (size_t i = 2; i < n; i++) {
    const float cv = cross(values[i - 1] - values[i - 2], values[i] - values[i - 2]);
    const float cp = cross(p.points[i - 1] - p.points[i - 2], p.points[i] - p.points[i - 2]);
    const float sv = length(values[i] - values[i - 2]), sp = length(p.points[i] - p.points[i - 2]);
    if (std::abs(cv) > 1e-4f * sv * sv && std::abs(cp) > 1e-4f * sp * sp) {
      mirrored = (cv > 0.0f) != (cp > 0.0f);
      break;
    }
  }
  const auto conj = [](float2 z) { return float2{z.x, -z.y}; };
  const auto mul = [](float2 a, float2 b) { return float2{a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x}; };
  size_t stretch = 0;
  std::vector<float2> line;
  std::vector<float> along;
  for (size_t i = 1; i < n; i++) {
    const int f1 = p.first + int(i);
    /* The stretch this step is in: arriving at the first key on or after its end. */
    const auto it = std::lower_bound(keys.begin(), keys.end(), f1,
                                     [](const Key<float2> &k, int f) { return k.frame < f; });
    if (it == keys.begin() || it == keys.end()) {
      continue;
    }
    const size_t si = size_t(it - keys.begin());
    if (si != stretch) {
      stretch = si;
      stretch_shape_line(keys, si, line, along);
    }
    const float2 v0 = values[i - 1], v1 = values[i];
    if (length(v1 - v0) < 1e-7f) {
      /* Still: nothing between. */
      p.shape_known[i] = true;
      continue;
    }
    const float tol = 1e-4f + 1e-3f * along.back();
    float off0, off1;
    const float s0 = shape_line_project(line, along, v0, off0);
    const float s1 = shape_line_project(line, along, v1, off1);
    if (off0 > tol || off1 > tol || std::abs(s1 - s0) < 1e-9f) {
      /* Not on it (past its key, noise, a cycle, offsets from elsewhere): drawn through the dots instead. */
      continue;
    }
    const float2 d = mirrored ? conj(v1 - v0) : v1 - v0;
    const float dd = dot(d, d);
    const float2 z = mul(p.points[i] - p.points[i - 1], float2{d.x, -d.y} * (1.0f / dd));
    /* The vertices between, in the way the dot goes (backward too: a dot backing up along it). */
    const float margin = 1e-6f * std::max(along.back(), 1e-6f), lo = std::min(s0, s1), hi = std::max(s0, s1);
    const size_t first = p.shape[i].size();
    for (size_t j = 0; j < line.size(); j++) {
      if (along[j] > lo + margin && along[j] < hi - margin) {
        const float2 q = line[j] - v0;
        p.shape[i].push_back(p.points[i - 1] + mul(z, mirrored ? conj(q) : q));
      }
    }
    if (s1 < s0) {
      std::reverse(p.shape[i].begin() + std::ptrdiff_t(first), p.shape[i].end());
    }
    p.shape_known[i] = true;
  }
}

const char *path_brush_id(PathBrush brush)
{
  static const char *const IDS[PATH_BRUSH_COUNT] = {"EDIT", "GRAB", "SMOOTH", "BLEND_SPEED", "PINCH", "SLIDE",
                                                       "SURFACE_SMOOTH"};
  return IDS[int(brush)];
}

const char *path_brush_name(PathBrush brush)
{
  static const char *const NAMES[PATH_BRUSH_COUNT] = {"Edit", "Grab", "Smooth", "Blend Speed", "Pinch", "Slide",
                                                         "Surface Smooth"};
  return NAMES[int(brush)];
}

bool path_brush_from_id(const std::string &id, PathBrush *r_brush)
{
  for (int i = 0; i < PATH_BRUSH_COUNT; i++) {
    if (id == path_brush_id(PathBrush(i))) {
      *r_brush = PathBrush(i);
      return true;
    }
  }
  return false;
}

float path_brush_falloff(float distance, float radius)
{
  if (radius <= 0.0f || distance >= radius) {
    return 0.0f;
  }
  const float x = distance / radius;
  return 1.0f - x * x * (3.0f - 2.0f * x);
}

std::vector<PathGrabKey> motion_path_grab_keys(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                               int first, int last, float2 center, float radius)
{
  std::vector<PathGrabKey> keys;
  for (const MotionPath &p : motion_paths_make(doc, targets, first, last)) {
    for (const int f : own_key_frames(doc, p.target)) {
      if (f < p.first || f - p.first >= int(p.points.size())) {
        continue;
      }
      const float w = path_brush_falloff(length(p.points[size_t(f - p.first)] - center), radius);
      float2 start;
      if (w > 0.0f && motion_path_position_at(doc, p.target, f, start)) {
        keys.push_back({p.target, f, start, w});
      }
    }
  }
  return keys;
}

int motion_path_grab(Document &doc, const std::vector<PathGrabKey> &keys, float2 offset, float strength)
{
  const int shown = doc.frame_current;
  int moved = 0;
  for (const PathGrabKey &k : keys) {
    doc.frame_current = k.frame;
    animation_sync(doc);
    if (motion_path_place(doc, k.target, k.start + offset * (k.weight * strength))) {
      motion_path_record(doc);
      moved++;
    }
  }
  doc.frame_current = shown;
  animation_sync(doc);
  return moved;
}

int motion_path_smooth_dab(Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                           const PathDab &dab)
{
  struct Move {
    MotionPathTarget target;
    int frame;
    float2 to;
    float weight;
  };
  std::vector<Move> moves;
  for (const MotionPath &p : motion_paths_make(doc, targets, first, last)) {
    const std::vector<int> keys = motion_path_keys(doc, p.target);
    for (const int f : own_key_frames(doc, p.target)) {
      if (f < p.first || f - p.first >= int(p.points.size())) {
        continue;
      }
      const float w =
          std::min(path_brush_falloff(length(p.points[size_t(f - p.first)] - dab.center), dab.radius) * dab.strength,
                   1.0f);
      const auto at = std::find(keys.begin(), keys.end(), f);
      float2 now, fit;
      if (w <= 0.0f || at == keys.end() || !motion_path_position_at(doc, p.target, f, now)) {
        continue;
      }
      if (!arc_fit(doc, p.target, keys, size_t(at - keys.begin()), fit)) {
        fit = now;
      }
      moves.push_back({p.target, f, now + (fit - now) * w, w});
    }
  }
  const int shown = doc.frame_current;
  int changed = 0;
  for (const Move &m : moves) {
    doc.frame_current = m.frame;
    animation_sync(doc);
    if (motion_path_place(doc, m.target, m.to)) {
      motion_path_record(doc);
      changed++;
    }
  }
  /* Dragged handles ease back toward Auto, becoming Auto once there. */
  for (const Move &m : moves) {
    Channel<float2> *ch = handle_channel(doc, m.target);
    Key<float2> *key = ch ? ch->key_at(m.frame) : nullptr;
    if (!key || key->path_handles == PathHandles::Auto) {
      continue;
    }
    const size_t i = size_t(key - ch->keys.data());
    std::vector<Key<float2>> automatic = ch->keys;
    automatic[i].path_handles = PathHandles::Auto;
    float2 in, out;
    key_path_handles(automatic, i, in, out);
    key->path_in = key->path_in + (in - key->path_in) * m.weight;
    key->path_out = key->path_out + (out - key->path_out) * m.weight;
    if (length(key->path_in - in) + length(key->path_out - out) < 1e-4f) {
      key->path_handles = PathHandles::Auto;
    }
  }
  doc.frame_current = shown;
  animation_evaluate(doc, shown);
  return changed;
}

bool motion_path_slide_pick(const Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                            float2 at, float radius, MotionPathTarget &r_target, int &r_frame, int &r_reach,
                            float2 &r_offset)
{
  float best = radius;
  bool found = false;
  for (const MotionPath &p : motion_paths_make(doc, targets, first, last)) {
    for (size_t i = 0; i < p.points.size(); i++) {
      const int f = p.first + int(i);
      const float d = length(p.points[i] - at);
      if (d > best || f <= 0 || std::binary_search(p.keys.begin(), p.keys.end(), f)) {
        continue;
      }
      best = d;
      found = true;
      r_target = p.target;
      r_frame = f;
      r_reach = 1;
      for (size_t j = 0; j < p.points.size(); j++) {
        if (length(p.points[j] - at) <= radius) {
          r_reach = std::max(r_reach, std::abs(p.first + int(j) - f));
        }
      }
      float2 own;
      r_offset = motion_path_position_at(doc, p.target, f, own) ? p.points[i] - own : float2{0.0f, 0.0f};
    }
  }
  return found;
}

int motion_path_speed_dab(Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                          PathBrush brush, const PathDab &dab)
{
  if (!path_brush_is_speed(brush)) {
    return 0;
  }
  int changed = 0;
  for (const MotionPath &p : motion_paths_make(doc, targets, first, last)) {
    std::map<int, float> falloff;
    for (size_t k = 0; k < p.points.size(); k++) {
      const float w = path_brush_falloff(length(p.points[k] - dab.center), dab.radius);
      if (w > 0.0f) {
        falloff[p.first + int(k)] = w;
      }
    }
    if (falloff.empty()) {
      continue;
    }
    /* The channel that moves the dot: a bone's angle for its tip, else the position. */
    Layer *layer = layer_find(doc, p.target.layer);
    if (layer && tip_turns(doc, p.target)) {
      for (Bone &b : layer->bones) {
        if (b.id == p.target.bone) {
          changed += paint_speeds(b.angle_keys, falloff, brush, dab);
        }
      }
    }
    else if (Channel<float2> *ch = handle_channel(doc, p.target)) {
      changed += paint_speeds(*ch, falloff, brush, dab);
    }
  }
  animation_evaluate(doc, doc.frame_current);
  return changed;
}

bool motion_path_slide(Document &doc, const MotionPathTarget &target, int frame, float2 at, const PathSlide &slide)
{
  OANIM_PROFILE("motion_path_slide");
  Layer *layer = layer_find(doc, target.layer);
  if (!layer) {
    return false;
  }
  size_t i = 0;
  int start = 0, n = 0;
  if (tip_turns(doc, target)) {
    /* A tip: its bone's angle, the stretch's progress how far the angle has turned between its keys. */
    const int b = bone_index(*layer, target.bone);
    if (b < 0) {
      return false;
    }
    Channel<float> &ch = layer->bones[size_t(b)].angle_keys;
    if (!stretch_of(ch, frame, i, start, n) || !played_frame(ch, i, start, frame)) {
      return false;
    }
    Document copy = doc;
    copy.frame_current = frame;
    path_evaluate(copy, target, frame);
    Bone &posed = layer_find(copy, target.layer)->bones[size_t(b)];
    const float before = posed.angle;
    if (!solve_tip(copy, target, posed, at)) {
      return false;
    }
    const float want = ch.evaluate(frame) + (posed.angle - before);
    const float a = ch.keys[i - 1].value, z = ch.keys[i].value;
    if (std::abs(z - a) < 1e-9f) {
      return false;
    }
    const float progress = (want - a) / (z - a);
    if (!stretch_of(ch, frame, i, start, n)) {
      return false;
    }
    const std::vector<float> s = stretch_make_custom(ch, i, start, n);
    key_speeds_set(ch.keys, i, slid_speeds(s, frame - start, progress, slide));
  }
  else {
    Channel<float2> *ch = handle_channel(doc, target);
    if (!ch || !stretch_of(*ch, frame, i, start, n) || !played_frame(*ch, i, start, frame)) {
      return false;
    }
    /* The mouse in the keys' own space, by how the target moves with the channel's value there. */
    const float2 value = ch->evaluate(frame);
    float2 shown, cx, cy;
    if (!value_jacobian(doc, target, frame, value, shown, &cx, &cy)) {
      return false;
    }
    /* Least squares, damped: where the value hardly moves it one way (a pupil Look At holds against its eye's edge),
     * the mouse along that way doesn't fling the value off. */
    const float xx = dot(cx, cx), xy = dot(cx, cy), yy = dot(cy, cy);
    const float damp = xx * yy - xy * xy > 1e-3f * (xx + yy) * (xx + yy) ? 0.0f : 1e-3f * (xx + yy);
    const float a = xx + damp, d = yy + damp, det = a * d - xy * xy;
    if (xx + yy < 1e-18f || std::abs(det) < 1e-30f) {
      return false;
    }
    const float2 miss = at - shown;
    const float gx = dot(cx, miss), gy = dot(cy, miss);
    const float2 v = value + float2{(d * gx - xy * gy) / det, (a * gy - xy * gx) / det};
    /* A dot past its key (overshoot) or back before the start slides on along the line it goes there by. */
    const std::vector<float> now = speeds_progress(stretch_speeds(*ch, i, start, n));
    const float was = now[size_t(std::clamp(frame - start, 0, n))];
    const float progress = nearest_progress(ch->keys, i, v, std::clamp(std::min(0.0f, was - 0.5f), -1.0f, 0.0f),
                                            std::clamp(std::max(1.0f, was + 0.5f), 1.0f, 2.0f));
    if (!stretch_of(*ch, frame, i, start, n)) {
      return false;
    }
    const std::vector<float> s = stretch_make_custom(*ch, i, start, n);
    key_speeds_set(ch->keys, i, slid_speeds(s, frame - start, progress, slide));
  }
  animation_evaluate(doc, doc.frame_current);
  return true;
}

bool motion_path_even_spacing(Document &doc, const MotionPathTarget &target, int frame)
{
  bool done = false;
  with_timing_channels(doc, target, [&](auto &ch) {
    size_t i = 0;
    int start = 0, n = 0;
    if (stretch_of(ch, frame, i, start, n)) {
      stretch_make_custom(ch, i, start, n);
      key_speeds_set(ch.keys, i, std::vector<float>(size_t(n), 1.0f));
      done = true;
    }
  });
  animation_evaluate(doc, doc.frame_current);
  return done;
}

int motion_path_scale_spacing(Document &doc, const MotionPathTarget &target, int frame, float factor,
                              const PathSlide &slide)
{
  factor = std::max(factor, 0.0f);
  int changed = 0;
  bool first = true;
  with_timing_channels(doc, target, [&](auto &ch) {
    /* A drawing's channels each change alike: counted once. */
    const int c = scale_stretches(ch, frame, factor, slide);
    changed = first ? c : changed;
    first = false;
  });
  animation_evaluate(doc, doc.frame_current);
  return changed;
}

float motion_path_dot_speed(const Document &doc, const MotionPathTarget &target, int frame)
{
  size_t i = 0;
  int start = 0, n = 0;
  std::vector<float> s;
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) {
    if (stretch_of(ch, frame, i, start, n)) {
      s = stretch_speeds(ch, i, start, n);
    }
  });
  if (s.empty()) {
    return 0.0f;
  }
  /* The steps into and out of the dot, averaged. */
  const int k = frame - start;
  return 0.5f * (s[size_t(k - 1)] + s[size_t(std::min(k, n - 1))]);
}

/* Of a drawing's animated channels (drawing_channels' order), the one that goes furthest: its timing is the drawing's
 * (one keyed in place has no motion to time). -1 with none. */
static int drawing_lead_channel(Document &doc, Layer &layer)
{
  int best = -1, index = 0;
  float most = -1.0f;
  drawing_channels(doc, layer, [&](auto &ch) {
    float travel = 0.0f;
    for (size_t i = 1; i < ch.keys.size(); i++) {
      if constexpr (std::is_same_v<std::decay_t<decltype(ch.keys[i].value)>, float>) {
        travel += std::abs(ch.keys[i].value - ch.keys[i - 1].value);
      }
      else {
        travel += length(ch.keys[i].value - ch.keys[i - 1].value);
      }
    }
    if (travel > most) {
      most = travel;
      best = index;
    }
    index++;
    return true;
  });
  return best;
}

/* Calls `f` with the channel that moves the target's dot: a tip's bone angle, else its position. */
template<typename F> static bool with_path_channel(Document &doc, const MotionPathTarget &t, F &&f)
{
  Layer *layer = layer_find(doc, t.layer);
  if (!layer) {
    return false;
  }
  if (t.drawing) {
    /* The drawing: its lead channel stands for all (they share their keys' frames and timing). */
    const int best = drawing_lead_channel(doc, *layer);
    int index = 0;
    drawing_channels(const_cast<Document &>(doc), *layer, [&](auto &ch) {
      if (index++ == best) {
        f(ch);
        return false;
      }
      return true;
    });
    return best >= 0;
  }
  if (tip_turns(doc, t)) {
    const int b = bone_index(*layer, t.bone);
    if (b < 0) {
      return false;
    }
    f(layer->bones[size_t(b)].angle_keys);
    return true;
  }
  if (Channel<float2> *ch = handle_channel(doc, t)) {
    f(*ch);
    return true;
  }
  return false;
}

/* Calls `f` with each channel holding a key behind the target's diamond on `frame` (see motion_path_key_settings). */
template<typename F> static int with_diamond_channels(Document &doc, const MotionPathTarget &t, int frame, F &&f)
{
  int count = 0;
  const auto visit = [&](auto &ch) {
    if (ch.key_at(frame)) {
      f(ch);
      count++;
    }
  };
  with_path_channels(doc, t, visit);
  if (count > 0 || t.drawing) {
    return count;
  }
  /* Not its own key: what moves it there, its layer's turn, move or size, else a group's above. */
  KeyFilter filter;
  filter.kinds = KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM;
  for (Layer *l = layer_find(doc, t.layer); l && count == 0; l = layer_parent(doc, l->id)) {
    for (const ChannelRef &ref : layer_filtered_channels(*l, filter)) {
      if (ref.scalar) {
        visit(*ref.scalar);
      }
      if (ref.vector) {
        visit(*ref.vector);
      }
    }
  }
  return count;
}

int motion_path_key_settings(Document &doc, const MotionPathTarget &target, int frame,
                             const std::function<void(KeySettings &)> &change, KeySettings *r_first)
{
  bool first = true;
  return with_diamond_channels(doc, target, frame, [&](auto &ch) {
    auto &key = *ch.key_at(frame);
    KeySettings s = key_settings(key);
    if (first && r_first) {
      *r_first = s;
    }
    first = false;
    if (!change) {
      return;
    }
    change(s);
    s.hold = std::max(0, s.hold);
    s.interval = std::max(1, s.interval);
    s.cycle_back = std::max(0, s.cycle_back);
    s.elastic_period = std::max(0.02f, s.elastic_period);
    if (ch.constant) {
      s.interp = Interp::Step;
    }
    key_settings_apply(key, s);
  });
}

/* Takes out a key, the control points either side of it staying where they are. */
template<typename T> static bool path_key_remove(Channel<T> &ch, int frame)
{
  if constexpr (std::is_same_v<T, float2>) {
    if (const Key<float2> *k = ch.key_at(frame)) {
      key_controls_join(ch.keys, size_t(k - ch.keys.data()));
    }
  }
  return ch.remove_key(frame);
}

int motion_path_key_delete(Document &doc, const MotionPathTarget &target, int frame)
{
  if (frame == 0) {
    return 0;
  }
  std::vector<std::function<void()>> removals;
  const int count = with_diamond_channels(doc, target, frame, [&](auto &ch) {
    removals.push_back([&ch, frame] { path_key_remove(ch, frame); });
  });
  for (const auto &remove : removals) {
    remove();
  }
  return count;
}

/* Calls `f` with each channel whose keys are the target's own keys on its path (see motion_path_keys): a diamond
 * on the path is all of them on its frame, a turn's key as much as a move's. */
template<typename F> static void with_own_key_channels(Document &doc, const MotionPathTarget &t, F &&f)
{
  Layer *layer = layer_find(doc, t.layer);
  if (!layer) {
    return;
  }
  if (t.drawing) {
    drawing_channels(doc, *layer, [&](auto &ch) {
      f(ch);
      return true;
    });
    return;
  }
  if (t.bone) {
    const int i = bone_index(*layer, t.bone);
    if (i >= 0) {
      Bone &b = layer->bones[size_t(i)];
      f(b.position_keys);
      f(b.angle_keys);
      f(b.scale_keys);
    }
    return;
  }
  if (t.point) {
    const int i = point_index(*layer, t.point);
    if (i >= 0) {
      f(layer->drawing.points[size_t(i)].pos_keys);
    }
    return;
  }
  KeyFilter filter;
  filter.kinds = KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM;
  for (const ChannelRef &ref : layer_filtered_channels(*layer, filter)) {
    if (ref.scalar) {
      f(*ref.scalar);
    }
    if (ref.vector) {
      f(*ref.vector);
    }
  }
}

std::vector<std::pair<MotionPathTarget, int>> motion_path_selected_keys(const Document &doc,
                                                                        const std::vector<MotionPathTarget> &targets)
{
  std::vector<std::pair<MotionPathTarget, int>> out;
  for (const MotionPathTarget &t : targets) {
    std::set<int> frames;
    with_own_key_channels(const_cast<Document &>(doc), t, [&](auto &ch) {
      for (const auto &k : ch.keys) {
        if (k.selected && k.frame > 0) {
          frames.insert(k.frame);
        }
      }
    });
    for (const int f : frames) {
      out.push_back({t, f});
    }
  }
  return out;
}

bool motion_path_is_key(const Document &doc, const MotionPathTarget &target, int frame)
{
  bool found = false;
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) { found = ch.key_at(frame) != nullptr; });
  if (!found && frame > 0) {
    with_own_key_channels(const_cast<Document &>(doc), target, [&](auto &ch) { found = found || ch.key_at(frame); });
  }
  return found;
}

bool motion_path_key_selected(const Document &doc, const MotionPathTarget &target, int frame)
{
  bool selected = false;
  with_own_key_channels(const_cast<Document &>(doc), target, [&](auto &ch) {
    const auto *k = ch.key_at(frame);
    selected = selected || (k && k->selected);
  });
  return selected;
}

bool motion_path_stretch_at(const Document &doc, const MotionPathTarget &target, int frame, int &r_start, int &r_end)
{
  Layer *layer = layer_find(const_cast<Document &>(doc), target.layer);
  if (!layer) {
    return false;
  }
  /* Frame 0 is the setup: its key starts the first stretch. */
  const std::set<int> frames = layers_filtered_key_frames(motion_path_key_layers(doc, target),
                                                         motion_path_key_filter(doc, target));
  const auto it = frames.lower_bound(std::max(frame, 1));
  if (it == frames.end() || it == frames.begin()) {
    return false;
  }
  r_end = *it;
  r_start = *std::prev(it);
  return true;
}

std::vector<MotionPathPickedStretch> motion_path_picked_stretches(
    const Document &doc, const std::vector<MotionPathTarget> &key_targets,
    const std::vector<std::pair<MotionPathTarget, int>> &dots, const std::vector<MotionPathHandlePick> &handles)
{
  std::vector<MotionPathPickedStretch> out;
  const auto add = [&](const MotionPathTarget &t, int frame) {
    MotionPathPickedStretch p{t, 0, 0};
    if (motion_path_stretch_at(doc, t, frame, p.start, p.end) && std::find(out.begin(), out.end(), p) == out.end()) {
      out.push_back(p);
      return true;
    }
    return false;
  };
  /* A dot or handle on the path of a layer in a folded group: the group's stretch, keyed as one. */
  for (const auto &[t, f] : dots) {
    add(motion_path_timing_owner(doc, t), f);
  }
  for (const MotionPathHandlePick &hp : handles) {
    add(motion_path_timing_owner(doc, hp.target), hp.out ? hp.frame + 1 : hp.frame);
  }
  for (const auto &[t, f] : motion_path_selected_keys(doc, key_targets)) {
    /* The side the frame shown is on; where there is none (a path's end), the other. */
    const bool after = doc.frame_current > f;
    MotionPathPickedStretch p{t, 0, 0};
    const bool arriving = motion_path_stretch_at(doc, t, f, p.start, p.end) && p.end == f;
    const bool leaving = motion_path_stretch_at(doc, t, f + 1, p.start, p.end) && p.start == f;
    if ((after && leaving) || (!after && !arriving && leaving)) {
      add(t, f + 1);
    }
    else if (arriving) {
      add(t, f);
    }
  }
  return out;
}

int motion_path_picked_spans(const std::vector<MotionPathPickedStretch> &picks)
{
  std::set<std::pair<int, int>> spans;
  for (const MotionPathPickedStretch &p : picks) {
    spans.insert({p.start, p.end});
  }
  return int(spans.size());
}

std::set<const void *> motion_path_dot_channels(const Document &doc, const MotionPathTarget &target)
{
  std::set<const void *> out;
  if (target.drawing) {
    if (Layer *layer = layer_find(const_cast<Document &>(doc), target.layer)) {
      drawing_channels(const_cast<Document &>(doc), *layer, [&](auto &ch) {
        out.insert(&ch);
        return true;
      });
    }
    return out;
  }
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) { out.insert(&ch); });
  return out;
}

bool motion_path_key_is_breakdown(const Document &doc, const MotionPathTarget &target, int frame)
{
  bool breakdown = false;
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) {
    const auto *k = ch.key_at(frame);
    breakdown = k && k->type == KeyType::Breakdown;
  });
  return breakdown;
}

void motion_path_key_select(Document &doc, const MotionPathTarget &target, int frame, bool select)
{
  with_own_key_channels(doc, target, [&](auto &ch) {
    if (auto *k = ch.key_at(frame)) {
      k->selected = select;
    }
  });
}

int motion_path_keys_transform(Document &doc, const std::vector<std::pair<MotionPathTarget, int>> &keys, char kind,
                               float2 offset, float angle, float factor, const float2 *pivot)
{
  if (keys.empty()) {
    return 0;
  }
  /* Where each is on its own frame, and their centre. */
  std::vector<float2> at(keys.size());
  float2 centre{0.0f, 0.0f};
  for (size_t i = 0; i < keys.size(); i++) {
    motion_path_position_at(doc, keys[i].first, keys[i].second, at[i]);
    centre = centre + at[i];
  }
  centre = pivot ? *pivot : centre / float(keys.size());
  const float c = std::cos(angle), s = std::sin(angle);
  const auto turn = [&](float2 v) {
    if (kind == 'R') {
      return float2{v.x * c - v.y * s, v.x * s + v.y * c};
    }
    if (kind == 'S') {
      return v * factor;
    }
    return v;
  };
  int changed = 0;
  for (size_t i = 0; i < keys.size(); i++) {
    const MotionPathTarget &t = keys[i].first;
    const int frame = keys[i].second;
    const float2 to = kind == 'G' ? at[i] + offset : centre + turn(at[i] - centre);
    if (length(to - at[i]) > 1e-7f) {
      const MotionPathOnFrame on(doc, frame);
      if (motion_path_place(doc, t, to)) {
        motion_path_record(doc);
        changed++;
      }
    }
    if (kind == 'G') {
      continue;
    }
    /* The handles turn and scale with it. */
    float2 key, in, out;
    PathHandles mode = PathHandles::Auto;
    if (!motion_path_handles(doc, t, frame, key, in, out, &mode)) {
      continue;
    }
    const bool broken = mode == PathHandles::Broken;
    const float2 new_out = key + turn(out - key), new_in = key + turn(in - key);
    if (length(out - key) > 1e-7f) {
      motion_path_handle_set(doc, t, frame, true, new_out, broken);
    }
    if (length(in - key) > 1e-7f) {
      motion_path_handle_set(doc, t, frame, false, new_in, true);
      if (!broken) {
        /* Set apart, so each keeps its own length; in line again, as they were. */
        Channel<float2> *ch = handle_channel(doc, t);
        if (Key<float2> *k = ch ? ch->key_at(frame) : nullptr) {
          k->path_handles = PathHandles::Aligned;
        }
      }
    }
    changed++;
  }
  animation_evaluate(doc, doc.frame_current);
  return changed;
}

int motion_path_handles_type(Document &doc, const std::vector<MotionPathTarget> &targets, PathHandles mode)
{
  int changed = 0;
  for (const MotionPathTarget &t : targets) {
    Channel<float2> *ch = handle_channel(doc, t);
    if (!ch) {
      continue;
    }
    for (size_t i = 0; i < ch->keys.size(); i++) {
      Key<float2> &k = ch->keys[i];
      /* The setup key too: its handles are dragged like any key's, so they can be set back. */
      if (!k.selected || k.frame < 0) {
        continue;
      }
      if (mode == PathHandles::Auto) {
        if (k.path_handles != PathHandles::Auto) {
          k.path_handles = PathHandles::Auto;
          changed++;
        }
        continue;
      }
      float2 in, out;
      key_path_handles(ch->keys, i, in, out);
      if (mode == PathHandles::Aligned) {
        /* In line across the key, along the way from the in handle's end to the out handle's. */
        const float li = length(in), lo = length(out);
        float2 dir = out - in;
        if (length(dir) < 1e-9f) {
          dir = lo > 1e-9f ? out : float2{1.0f, 0.0f};
        }
        dir = dir / length(dir);
        in = dir * -li;
        out = dir * lo;
      }
      if (k.path_handles != mode || length(k.path_in - in) + length(k.path_out - out) > 1e-7f) {
        k.path_in = in;
        k.path_out = out;
        k.path_handles = mode;
        changed++;
      }
    }
  }
  animation_evaluate(doc, doc.frame_current);
  return changed;
}

/* As with_path_channel, but a timing change to the drawing goes to every channel of it alike: the timing the first
 * channel (the one its row shows) ends up with, since an edit relative to the timing there (a slide, a speed, a
 * blend) would otherwise start from each channel's own. */
template<typename F> static bool with_timing_channels(Document &doc, const MotionPathTarget &t, F &&f)
{
  return with_path_channels(doc, t, [&](auto &ch) { f(ch); });
}

template<typename F> static bool with_path_channels(Document &doc, const MotionPathTarget &target, F &&f)
{
  /* A path inside a folded Key Together group: the group's timing, keyed as one, led by the channel moving this
   * path's dot (the edit is made on its dots). */
  const MotionPathTarget t = motion_path_timing_owner(doc, target);
  const void *own = nullptr;
  if (!t.same(target)) {
    with_path_channel(doc, target, [&](auto &ch) { own = &ch; });
  }
  if (!t.drawing) {
    return with_path_channel(doc, t, f);
  }
  Layer *layer = layer_find(doc, t.layer);
  bool found = false;
  using Timing = std::tuple<int, Interp, TimingCurve>;
  std::vector<Timing> changed;
  std::vector<int> first_before, first_after;
  if (layer) {
    /* The lead channel first (see with_path_channel): what the tool does to it, the others follow. */
    int lead = -1;
    if (own) {
      int i = 0;
      drawing_channels(doc, *layer, [&](auto &ch) {
        if (static_cast<const void *>(&ch) == own) {
          lead = i;
          return false;
        }
        i++;
        return true;
      });
    }
    if (lead < 0) {
      lead = drawing_lead_channel(doc, *layer);
    }
    int index = 0;
    const auto visit = [&](auto &ch) {
      if (!found) {
        std::vector<Timing> before;
        for (const auto &k : ch.keys) {
          before.push_back({k.frame, k.interp, k.timing});
          first_before.push_back(k.frame);
        }
        f(ch);
        found = true;
        for (const auto &k : ch.keys) {
          Timing now{k.frame, k.interp, k.timing};
          first_after.push_back(k.frame);
          if (std::find(before.begin(), before.end(), now) == before.end()) {
            changed.push_back(std::move(now));
          }
        }
        return true;
      }
      std::vector<int> frames_before;
      for (const auto &k : ch.keys) {
        frames_before.push_back(k.frame);
      }
      f(ch);
      /* Breakdowns moved by the tool land where the lead channel's did, so the drawing's keys stay together. */
      if (frames_before == first_before && ch.keys.size() == first_after.size()) {
        for (size_t j = 0; j < ch.keys.size(); j++) {
          ch.keys[j].frame = first_after[j];
        }
      }
      for (const auto &[frame, interp, timing] : changed) {
        if (auto *k = ch.key_at(frame); k && k->interp == interp) {
          k->timing = timing;
        }
      }
      return true;
    };
    drawing_channels(doc, *layer, [&](auto &ch) { return index++ == lead ? visit(ch) : true; });
    index = 0;
    drawing_channels(doc, *layer, [&](auto &ch) { return index++ == lead ? true : visit(ch); });
  }
  return found;
}

std::vector<MotionPathStretch> motion_path_stretches(const Document &doc, const MotionPathTarget &target)
{
  std::vector<MotionPathStretch> out;
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) {
    for (size_t i = 1; i < ch.keys.size(); i++) {
      const auto &a = ch.keys[i - 1], &b = ch.keys[i];
      const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
      const int n = b.frame - start;
      if (n < 1 || b.interp == Interp::Step || b.interp == Interp::Pose || b.interp == Interp::Cycle || a.interp == Interp::Cycle) {
        continue;
      }
      MotionPathStretch st;
      st.start = start;
      st.end = b.frame;
      st.progress = speeds_progress(stretch_speeds(ch, i, start, n));
      st.interval = std::max(a.interval, 1);
      out.push_back(std::move(st));
    }
  });
  return out;
}

std::vector<int> motion_path_overshoot_frames(const Document &doc, const MotionPathTarget &target)
{
  std::vector<int> out;
  for (const MotionPathStretch &s : motion_path_stretches(doc, target)) {
    for (size_t k = 1; k + 1 < s.progress.size(); k++) {
      if (s.progress[k] > 1.0f + 1e-4f || s.progress[k] < -1e-4f) {
        out.push_back(s.start + int(k));
      }
    }
  }
  return out;
}

bool motion_path_slide_progress(Document &doc, const MotionPathTarget &target, int frame, float progress,
                                const PathSlide &slide_in)
{
  PathSlide slide = slide_in;
  slide.keep_order = false;
  bool slid = false;
  with_timing_channels(doc, target, [&](auto &ch) {
    size_t i = 0;
    int start = 0, n = 0, played = frame;
    if (!stretch_of(ch, frame, i, start, n) || !played_frame(ch, i, start, played)) {
      return;
    }
    const std::vector<float> s = stretch_make_custom(ch, i, start, n);
    key_speeds_set(ch.keys, i, slid_speeds(s, played - start, progress, slide));
    slid = true;
  });
  animation_evaluate(doc, doc.frame_current);
  return slid;
}

int motion_path_timing_blend(Document &doc, const MotionPathTarget &target, int frame, float smoothing, int repeat,
                             float radius)
{
  int changed = 0;
  smoothing = std::clamp(smoothing, 0.0f, 1.0f);
  repeat = std::clamp(repeat, 1, 100);
  radius = std::max(radius, 0.0f);
  bool counted = false;
  with_timing_channels(doc, target, [&](auto &ch) {
    /* The drawing's channels each change alike: counted once. */
    const bool count = !counted;
    counted = true;
    for (size_t i = 1; i < ch.keys.size(); i++) {
      const int start = std::min(ch.keys[i - 1].frame + std::max(ch.keys[i - 1].hold, 0), ch.keys[i].frame);
      const int n = ch.keys[i].frame - start;
      const Interp interp = ch.keys[i].interp;
      if (n < 2 || interp == Interp::Step || interp == Interp::Pose || interp == Interp::Cycle ||
          ch.keys[i - 1].interp == Interp::Cycle)
      {
        continue;
      }
      /* The reach eases to nothing a frame past `radius`, so radius 0 is the clicked dot alone. */
      std::vector<float> w(size_t(n + 1), 0.0f);
      bool any = false;
      for (int k = 1; k < n; k++) {
        w[size_t(k)] = path_brush_falloff(float(std::abs(start + k - frame)), radius + 1.0f);
        any |= w[size_t(k)] > 0.0f;
      }
      if (!any) {
        continue;
      }
      std::vector<float> u = speeds_progress(stretch_make_custom(ch, i, start, n));
      for (int r = 0; r < repeat; r++) {
        const std::vector<float> was = u;
        for (int k = 1; k < n; k++) {
          const float mid = 0.5f * (was[size_t(k - 1)] + was[size_t(k + 1)]);
          u[size_t(k)] = was[size_t(k)] + (mid - was[size_t(k)]) * smoothing * w[size_t(k)];
        }
      }
      key_speeds_set(ch.keys, i, progress_speeds(u));
      changed += count ? 1 : 0;
    }
  });
  animation_evaluate(doc, doc.frame_current);
  return changed;
}

int motion_path_timing_blend_stroke(Document &doc, const MotionPathTarget &target, const std::vector<int> &frames,
                                    float smoothing, int repeat, float radius)
{
  int changed = 0;
  for (const int frame : frames) {
    changed += motion_path_timing_blend(doc, target, frame, smoothing, repeat, radius) > 0 ? 1 : 0;
  }
  return changed;
}

std::string path_frames_string(const std::vector<int> &frames)
{
  std::string text;
  for (const int f : frames) {
    text += (text.empty() ? "" : ",") + std::to_string(f);
  }
  return text;
}

std::vector<int> path_frames_parse(const std::string &text)
{
  std::vector<int> frames;
  for (size_t at = 0; at < text.size();) {
    const size_t end = std::min(text.find(',', at), text.size());
    const std::string part = text.substr(at, end - at);
    if (!part.empty()) {
      frames.push_back(std::atoi(part.c_str()));
    }
    at = end + 1;
  }
  return frames;
}

bool path_shape_from_identifier(const std::string &id, PathShape &r_shape)
{
  static const std::pair<const char *, PathShape> ids[] = {{"EVEN", PathShape::Even},
                                                            {"EASE_IN", PathShape::EaseIn},
                                                            {"EASE_OUT", PathShape::EaseOut},
                                                            {"EASE_IN_OUT", PathShape::EaseInOut}};
  for (const auto &[name, shape] : ids) {
    if (id == name) {
      r_shape = shape;
      return true;
    }
  }
  return false;
}

static float path_shape_progress(PathShape shape, float t)
{
  switch (shape) {
    case PathShape::Even:
      return t;
    case PathShape::EaseIn:
      return t * t;
    case PathShape::EaseOut:
      return 1.0f - (1.0f - t) * (1.0f - t);
    case PathShape::EaseInOut:
      return t * t * (3.0f - 2.0f * t);
  }
  return t;
}

bool motion_path_stretch_shape(Document &doc, const MotionPathTarget &target, int frame, PathShape shape)
{
  bool done = false;
  with_timing_channels(doc, target, [&](auto &ch) {
    size_t i = 0;
    int start = 0, n = 0;
    if (!stretch_of(ch, frame, i, start, n)) {
      return;
    }
    stretch_make_custom(ch, i, start, n);
    std::vector<float> u(size_t(n + 1), 0.0f);
    for (int k = 0; k <= n; k++) {
      u[size_t(k)] = path_shape_progress(shape, float(k) / float(n));
    }
    /* Each frame at least the speed brushes' least on, so no dot sits on a key. */
    order_progress(u, 0, 0.02f / float(n));
    key_speeds_set(ch.keys, i, progress_speeds(u));
    done = true;
  });
  animation_evaluate(doc, doc.frame_current);
  return done;
}

bool motion_path_timing_curve(const Document &doc, const MotionPathTarget &target, int frame, TimingCurve &r_curve,
                              int *r_start, int *r_frames)
{
  bool found = false;
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) {
    size_t i = 0;
    int start = 0, n = 0;
    if (found || !stretch_of(ch, frame, i, start, n)) {
      return;
    }
    const auto &b = ch.keys[i];
    r_curve = b.interp == Interp::Custom ? b.timing : timing_fit(speeds_progress(stretch_speeds(ch, i, start, n)));
    if (r_start) {
      *r_start = start;
    }
    if (r_frames) {
      *r_frames = n;
    }
    found = true;
  });
  return found;
}

bool motion_path_timing_curve_set(Document &doc, const MotionPathTarget &target, int frame, const TimingCurve &curve)
{
  if (!curve.points.empty() && !timing_valid(curve)) {
    return false;
  }
  bool done = false;
  with_timing_channels(doc, target, [&](auto &ch) {
    size_t i = 0;
    int start = 0, n = 0;
    if (!stretch_of(ch, frame, i, start, n)) {
      return;
    }
    stretch_make_custom(ch, i, start, n);
    key_timing_set(ch.keys, i, curve);
    done = true;
  });
  animation_evaluate(doc, doc.frame_current);
  return done;
}

bool motion_path_dot_speed_set(Document &doc, const MotionPathTarget &target, int frame, float speed)
{
  bool done = false;
  with_timing_channels(doc, target, [&](auto &ch) {
    size_t i = 0;
    int start = 0, n = 0;
    if (!stretch_of(ch, frame, i, start, n)) {
      return;
    }
    std::vector<float> s = stretch_make_custom(ch, i, start, n);
    /* The steps into and out of the dot (motion_path_dot_speed's average) set; the others scaled to make up the
     * stretch's frames again (speeds average 1 over it). */
    const size_t a = size_t(frame - start - 1), b = size_t(frame - start);
    float rest = 0.0f;
    for (size_t k = 0; k < s.size(); k++) {
      rest += k == a || k == b ? 0.0f : s[k];
    }
    const float least = 0.02f;
    const float most = (float(n) - least * float(n - 2)) * 0.5f;
    /* Under 0 it goes back (past its key, say), as far as the stretch's frames. */
    speed = std::clamp(speed, -float(n), std::max(most, least));
    const float left = float(n) - 2.0f * speed;
    if (rest <= 1e-6f || left <= 0.0f) {
      return;
    }
    for (size_t k = 0; k < s.size(); k++) {
      s[k] = k == a || k == b ? speed : s[k] > 0.0f ? std::max(s[k] * left / rest, least) : s[k] * left / rest;
    }
    key_speeds_set(ch.keys, i, s);
    done = true;
  });
  animation_evaluate(doc, doc.frame_current);
  return done;
}

bool motion_path_timing_stretch(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                const MotionPathTarget &want, int want_end, int picked_on, MotionPathTarget &r_target,
                                MotionPathStretch &r_stretch)
{
  if (targets.empty()) {
    return false;
  }
  /* The one picked on; else the first with timing to show. */
  r_target = targets.front();
  for (const MotionPathTarget &t : targets) {
    if (!motion_path_stretches(doc, t).empty()) {
      r_target = t;
      break;
    }
  }
  for (const MotionPathTarget &t : targets) {
    if (t.same(want)) {
      r_target = t;
    }
  }
  const std::vector<MotionPathStretch> stretches = motion_path_stretches(doc, r_target);
  if (stretches.empty()) {
    return false;
  }
  const MotionPathStretch *pick = nullptr;
  /* Picked or stepped to: shown till the frame changes, then it follows the frame again. */
  for (const MotionPathStretch &s : stretches) {
    if (s.end == want_end && doc.frame_current == picked_on) {
      pick = &s;
    }
  }
  for (const MotionPathStretch &s : stretches) {
    if (!pick && doc.frame_current > s.start - 1 && doc.frame_current <= s.end) {
      pick = &s;
    }
  }
  r_stretch = pick ? *pick : stretches.front();
  return true;
}

bool key_add_in_place(Channel<float2> &pos, int frame)
{
  if (frame < 1 || pos.key_at(frame)) {
    return false;
  }
  int bi = -1;
  for (size_t i = 0; i < pos.keys.size(); i++) {
    if (pos.keys[i].frame > frame) {
      bi = int(i);
      break;
    }
  }
  if (bi <= 0) {
    /* Before the first key or after the last: it holds what the channel holds there. */
    pos.set_key(frame, pos.evaluate(frame));
    return true;
  }
  /* The keys either side keep the handles they have (their Auto ones taken as their own), so the stretches beyond them
   * stay as they are: Auto would take new slopes from the new neighbour. */
  const bool handles = pos.keys[size_t(bi - 1)].path_handles != PathHandles::Auto ||
                       pos.keys[size_t(bi)].path_handles != PathHandles::Auto;
  key_path_handles_freeze(pos.keys, size_t(bi - 1));
  key_path_handles_freeze(pos.keys, size_t(bi));
  const Key<float2> &a = pos.keys[size_t(bi - 1)], &b = pos.keys[size_t(bi)];
  /* Handles along the path's way there, as long as its speed there makes them (a third of each stretch, as a
   * Bezier's), so adding it bends nothing. */
  const float2 speed = (pos.evaluate(frame + 1) - pos.evaluate(frame - 1)) * 0.5f;
  const int span_in = frame - a.frame, span_out = b.frame - frame;
  /* On a stretch that runs along a curve of its own (dragged handles, control points) or is timed along it (Linear's
   * way with handles, the eases, Custom...): that curve split at the key and its timing shared out between the two, so
   * no dot moves. A plain Smooth or Linear stretch already keeps its way with the handles above. */
  StretchPoints old_points;
  std::vector<float> old_u;
  int old_k = 0;
  bool split = false, curve_timed = false;
  int over = 0;
  float old_length = 0.0f;
  /* The new key ends the first half: it arrives as the old stretch did (its interpolation and settings), and plays on
   * as the key before it did (its interval), holding nothing. */
  const KeySettings arriving = key_settings(b);
  const int interval = a.interval;
  const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
  const bool controls = !b.path_controls.empty();
  const bool moves = b.interp != Interp::Step && b.interp != Interp::Pose && b.interp != Interp::Cycle &&
                     b.interp != Interp::Noisy && a.interp != Interp::Cycle && frame > start;
  const bool plain = (b.interp == Interp::Smooth || b.interp == Interp::Linear) && !handles && !controls;
  if (moves && !plain) {
    const int n = b.frame - start;
    old_points = stretch_points(pos.keys, size_t(bi));
    old_u = speeds_progress(stretch_speeds(pos, size_t(bi), start, n));
    old_length = stretch_path_length(pos.keys, size_t(bi));
    old_k = frame - start;
    /* Smooth along dragged handles is timed by the curve itself, which the split keeps; the rest by length, shared
     * out (all the way to the ends: a dot nearly there still has its share to keep). */
    curve_timed = b.interp == Interp::Smooth && !controls;
    const float uk = old_u[size_t(old_k)];
    /* Past the key (an overshoot) or back before the start: off the curve, on its straight way on. */
    over = uk > 1.0f + 1e-4f ? 1 : uk < -1e-4f ? -1 : 0;
    split = curve_timed || (uk > 1e-3f && uk < 1.0f - 1e-3f) || over != 0;
  }
  /* A plain stretch's Bezier as it runs now (its keys' handles frozen above): split exactly where the dot is, at its
   * own parameter (Smooth goes by it, and Linear's handles a third of the way make it the share of the way). */
  float2 whole_curve[4];
  {
    float2 unused, out, in;
    key_path_handles(pos.keys, size_t(bi - 1), unused, out);
    key_path_handles(pos.keys, size_t(bi), in, unused);
    whole_curve[0] = a.value;
    whole_curve[1] = a.value + out;
    whole_curve[2] = b.value + in;
    whole_curve[3] = b.value;
  }
  const float curve_t = b.frame > start ? float(frame - start) / float(b.frame - start) : 0.0f;
  pos.set_key(frame, pos.evaluate(frame));
  const size_t ki = size_t(bi);
  Key<float2> *k = &pos.keys[ki];
  const auto settle = [&]() {
    k = &pos.keys[ki];
    key_settings_apply(*k, arriving);
    /* Inside the start key's hold: it holds on to where the hold ended, so the move still starts there. */
    k->hold = std::max(start - frame, 0);
    k->interval = interval;
    k->type = KeyType::Key;
  };
  if (over != 0 && key_overshoot_split(pos.keys, ki, old_points, over > 0)) {
    settle();
    /* Each dot as far along its new stretch as it went along the old way: by length, the old stretch's past its ends
     * (the straight way on), the new curves' own measure on them. */
    const int n = int(old_u.size()) - 1;
    const float uk = old_u[size_t(old_k)];
    const float l1 = std::max(stretch_path_length(pos.keys, ki), 1e-9f);
    const float l2 = std::max(stretch_path_length(pos.keys, ki + 1), 1e-9f);
    std::vector<float> u1(size_t(old_k + 1)), u2(size_t(n - old_k + 1));
    if (over > 0) {
      const float e = l2, curve = std::max(l1 - e, 1e-9f);
      for (int j = 0; j <= old_k; j++) {
        const float u = old_u[size_t(j)];
        const float along = u < 0.0f ? u * old_length : u <= 1.0f ? u * curve : curve + (u - 1.0f) * old_length;
        u1[size_t(j)] = along / l1;
      }
      for (int j = 0; j <= n - old_k; j++) {
        const float u = old_u[size_t(old_k + j)];
        u2[size_t(j)] = u >= 1.0f ? (uk - u) * old_length / e : 1.0f + (1.0f - u) * old_length / e;
      }
    }
    else {
      const float e = l1, curve = std::max(l2 - e, 1e-9f);
      for (int j = 0; j <= old_k; j++) {
        u1[size_t(j)] = -old_u[size_t(j)] * old_length / e;
      }
      for (int j = 0; j <= n - old_k; j++) {
        const float u = old_u[size_t(old_k + j)];
        const float along = u < 0.0f ? e + u * old_length : u <= 1.0f ? e + u * curve : l2 + (u - 1.0f) * old_length;
        u2[size_t(j)] = along / l2;
      }
    }
    u1.front() = 0.0f;
    u1.back() = 1.0f;
    u2.front() = 0.0f;
    u2.back() = 1.0f;
    key_speeds_set(pos.keys, ki, progress_speeds(u1));
    key_speeds_set(pos.keys, ki + 1, progress_speeds(u2));
  }
  else if (split && over == 0 && key_curve_split(pos.keys, ki, old_points)) {
    settle();
    if (!curve_timed) {
      const int n = int(old_u.size()) - 1;
      const float at = old_u[size_t(old_k)];
      /* Along each curve the same share of the way; past an end (an overshoot past the key, or back before the
       * start), on along it as far as before, by each one's own length. */
      const float l1 = stretch_path_length(pos.keys, ki), l2 = stretch_path_length(pos.keys, ki + 1);
      const float beyond1 = l1 > 1e-9f ? old_length / l1 : 1.0f / at;
      const float beyond2 = l2 > 1e-9f ? old_length / l2 : 1.0f / (1.0f - at);
      std::vector<float> u1(size_t(old_k + 1)), u2(size_t(n - old_k + 1));
      for (int j = 0; j <= old_k; j++) {
        const float u = old_u[size_t(j)];
        u1[size_t(j)] = u < 0.0f ? u * beyond1 : u / at;
      }
      for (int j = 0; j <= n - old_k; j++) {
        const float u = old_u[size_t(old_k + j)];
        u2[size_t(j)] = u > 1.0f ? 1.0f + (u - 1.0f) * beyond2 : (u - at) / (1.0f - at);
      }
      key_speeds_set(pos.keys, ki, progress_speeds(u1));
      key_speeds_set(pos.keys, ki + 1, progress_speeds(u2));
    }
  }
  else {
    settle();
    /* Its control points stay where they are. */
    key_controls_split(pos.keys, ki);
    if (span_in > 0 && span_out > 0 && frame <= start) {
      /* In the hold, where nothing moves: the new key sets off along the way the start key's handle took. */
      k->path_out = whole_curve[1] - whole_curve[0];
      k->path_in = -k->path_out;
      k->path_handles = PathHandles::Aligned;
    }
    else if (span_in > 0 && span_out > 0 && plain) {
      /* Cut in two at the dot's place on the curve (de Casteljau): the halves run where the whole did, each by its own
       * parameter as the whole did by its (whatever the hold before it or the frames it plays on). */
      const float t = curve_t;
      const float2 *w = whole_curve;
      const float2 q1 = w[0] + (w[1] - w[0]) * t, q2 = w[1] + (w[2] - w[1]) * t, q3 = w[2] + (w[3] - w[2]) * t;
      const float2 r1 = q1 + (q2 - q1) * t, r2 = q2 + (q3 - q2) * t;
      const float2 m = r1 + (r2 - r1) * t;
      k->path_in = r1 - m;
      k->path_out = r2 - m;
      k->path_handles = PathHandles::Aligned;
      pos.keys[ki - 1].path_out = q1 - whole_curve[0];
      pos.keys[ki + 1].path_in = q3 - whole_curve[3];
    }
    else if (span_in > 0 && span_out > 0) {
      k->path_in = speed * (-float(span_in) / 3.0f);
      k->path_out = speed * (float(span_out) / 3.0f);
      k->path_handles = PathHandles::Aligned;
      /* The curve cut there: the handles of the keys either side shorten to the part of it each now has (as a
       * Bezier's cut in two), so the halves run where the whole did. */
      const float whole = float(span_in + span_out);
      pos.keys[ki - 1].path_out *= float(span_in) / whole;
      pos.keys[ki + 1].path_in *= float(span_out) / whole;
    }
  }
  channel_keys_changed();
  return true;
}

/* A key on `frame` of a scalar channel (none there yet) holding what it has there, as key_add_in_place puts one in: it
 * arrives as the stretch it splits did, plays on as the key before did, and holds what is left of that key's hold. */
bool scalar_key_add_in_place(Channel<float> &ch, int frame)
{
  if (frame < 1 || ch.key_at(frame)) {
    return false;
  }
  const auto b = std::find_if(ch.keys.begin(), ch.keys.end(), [&](const Key<float> &k) { return k.frame > frame; });
  if (b == ch.keys.begin() || b == ch.keys.end()) {
    ch.set_key(frame, ch.evaluate(frame));
    return true;
  }
  const Key<float> &a = *(b - 1);
  const KeySettings arriving = key_settings(*b);
  const int interval = a.interval;
  const int start = std::min(a.frame + std::max(a.hold, 0), b->frame);
  Key<float> &k = ch.set_key(frame, ch.evaluate(frame));
  key_settings_apply(k, arriving);
  k.hold = std::max(start - frame, 0);
  k.interval = interval;
  k.type = KeyType::Key;
  return true;
}

bool motion_path_key_add(Document &doc, const MotionPathTarget &target, int frame, KeyType type, int *r_frame)
{
  if (frame < 1) {
    return false;
  }
  /* The position's key goes in where its dot is, keeping the stretch's way and timing; the other channels the path
   * keys take their values there too. */
  Channel<float2> *pos = handle_channel(doc, target);
  if (pos) {
    /* On 2s or 3s a held frame's dot is the pose of the frame its step began on (drawn on it, and the one picked
     * there): the key goes on that frame, as a slide of the held dot slides that frame. A key there already (the
     * stretch's start) is the pose itself. */
    for (size_t i = 1; i < pos->keys.size(); i++) {
      const Key<float2> &a = pos->keys[i - 1], &b = pos->keys[i];
      if (frame > a.frame && frame < b.frame) {
        const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
        const int interval = std::max(a.interval, 1);
        if (frame > start) {
          frame = start + (frame - start) / interval * interval;
        }
        break;
      }
    }
    if (pos->key_at(frame)) {
      return false;
    }
  }
  if (r_frame) {
    *r_frame = frame;
  }
  bool added = pos && key_add_in_place(*pos, frame);
  const auto typed = [&](auto &ch) {
    if (auto *k = ch.key_at(frame)) {
      k->type = type;
    }
  };
  if (pos && added) {
    typed(*pos);
  }
  Layer *layer = layer_find(doc, target.layer);
  if (layer && (target.point || target.drawing) && layer->key_whole_drawing && layer->type == LayerType::Vector) {
    /* Key Whole Drawing: every channel of the shape keyed here, each in place on its own stretch (its own way and
     * timing kept), so no other point moves either (left to the drawing's keying, they would take this point's
     * timing). */
    drawing_channels(doc, *layer, [&](auto &ch) {
      if (static_cast<void *>(&ch) == static_cast<void *>(pos) || ch.key_at(frame)) {
        return true;
      }
      if constexpr (std::is_same_v<std::decay_t<decltype(ch.keys[0].value)>, float2>) {
        added = key_add_in_place(ch, frame) || added;
      }
      else {
        added = scalar_key_add_in_place(ch, frame) || added;
      }
      typed(ch);
      return true;
    });
    animation_evaluate(doc, doc.frame_current);
    return added;
  }
  with_path_channel(doc, target, [&](auto &ch) {
    if (static_cast<void *>(&ch) == static_cast<void *>(pos) || ch.key_at(frame)) {
      return;
    }
    Key<std::decay_t<decltype(ch.keys[0].value)>> &k = ch.set_key(frame, ch.evaluate(frame));
    if constexpr (std::is_same_v<std::decay_t<decltype(ch.keys[0].value)>, float2>) {
      /* The control points of the stretch it splits stay where they are. */
      key_controls_split(ch.keys, size_t(&k - ch.keys.data()));
    }
    added = true;
  });
  animation_evaluate(doc, doc.frame_current);
  return added;
}

int motion_path_key_type_set(Document &doc, const MotionPathTarget &target_in, int frame, KeyType type)
{
  /* In a folded group, the group's key on the frame: every layer's. */
  const MotionPathTarget target = motion_path_timing_owner(doc, target_in);
  Layer *layer = layer_find(doc, target.layer);
  if (frame <= 0 || !layer) {
    return 0;
  }
  int count = 0;
  const auto set = [&](auto *ch) {
    if (auto *k = ch ? ch->key_at(frame) : nullptr) {
      k->type = type;
      count++;
    }
  };
  const KeyFilter filter = motion_path_key_filter(doc, target);
  for (Layer *l : motion_path_key_layers(doc, target)) {
    for (const ChannelRef &ref : layer_filtered_channels(*l, filter)) {
      set(ref.scalar);
      set(ref.vector);
    }
  }
  if (count > 0) {
    channel_keys_changed();
  }
  return count;
}

int motion_path_keys_delete(Document &doc, const std::vector<MotionPathTarget> &targets)
{
  int deleted = 0;
  for (const auto &[t, frame] : motion_path_selected_keys(doc, targets)) {
    /* The diamond goes: every key of the target's own on its frame. */
    with_own_key_channels(doc, t, [&, f = frame](auto &ch) { deleted += path_key_remove(ch, f) ? 1 : 0; });
  }
  animation_evaluate(doc, doc.frame_current);
  return deleted;
}

bool motion_path_has_handles(const Document &doc, const MotionPathTarget &target)
{
  return handle_channel(const_cast<Document &>(doc), target) != nullptr;
}

void motion_path_handle_sides(const MotionPath &p, int frame, bool &r_in, bool &r_out)
{
  r_in = frame > p.first;
  r_out = frame < p.first + int(p.points.size()) - 1;
}

bool motion_path_handle_stub_shown(const MotionPath &p, int frame, bool out, float2 key, float2 end, float2 &r_direction)
{
  bool show_in = false, show_out = false;
  motion_path_handle_sides(p, frame, show_in, show_out);
  return (out ? show_out : show_in) && length(end - key) < 1e-7f && motion_path_handle_stub(p, frame, out, r_direction);
}

bool motion_path_handle_stub(const MotionPath &p, int frame, bool out, float2 &r_direction)
{
  const int n = int(p.points.size()), at = frame - p.first;
  if (at < 0 || at >= n) {
    return false;
  }
  const float2 key = p.points[size_t(at)];
  for (int i = at + (out ? 1 : -1); i >= 0 && i < n; i += out ? 1 : -1) {
    const float2 d = p.points[size_t(i)] - key;
    const float len = length(d);
    if (len > 1e-6f) {
      r_direction = d / len;
      return true;
    }
  }
  return false;
}

bool motion_path_handles(const Document &doc, const MotionPathTarget &target, int frame, float2 &r_key, float2 &r_in,
                         float2 &r_out, PathHandles *r_mode)
{
  OANIM_PROFILE("motion_path_handles");
  const Channel<float2> *ch = handle_channel(const_cast<Document &>(doc), target);
  const Key<float2> *key = ch ? ch->key_at(frame) : nullptr;
  float2 cx, cy;
  if (!key || !key_jacobian(doc, target, frame, r_key, cx, cy)) {
    return false;
  }
  float2 in, out;
  key_path_handles(ch->keys, size_t(key - ch->keys.data()), in, out);
  r_in = r_key + cx * in.x + cy * in.y;
  r_out = r_key + cx * out.x + cy * out.y;
  if (r_mode) {
    *r_mode = key->path_handles;
  }
  return true;
}

bool motion_path_handle_set(Document &doc, const MotionPathTarget &target, int frame, bool out, float2 at,
                            bool broken)
{
  Channel<float2> *ch = handle_channel(doc, target);
  Key<float2> *key = ch ? ch->key_at(frame) : nullptr;
  float2 p, cx, cy;
  if (!key || !key_jacobian(doc, target, frame, p, cx, cy)) {
    return false;
  }
  const float det = cx.x * cy.y - cx.y * cy.x;
  if (std::abs(det) < 1e-12f) {
    return false;
  }
  const float2 miss = at - p;
  const float2 handle{(miss.x * cy.y - miss.y * cy.x) / det, (cx.x * miss.y - cx.y * miss.x) / det};
  key_path_handle_set(ch->keys, size_t(key - ch->keys.data()), out, handle, broken);
  return true;
}

/* The stretch of `ch` that `frame` is inside (strictly between its keys): the index of the key ending it. */
static size_t stretch_inside(const Channel<float2> &ch, int frame)
{
  for (size_t i = 1; i < ch.keys.size(); i++) {
    if (ch.keys[i - 1].frame < frame && frame < ch.keys[i].frame) {
      return i;
    }
  }
  return 0;
}

/* The frame inside stretch `i` whose value is nearest `v`. */
static int stretch_nearest_frame(const Channel<float2> &ch, size_t i, float2 v)
{
  int best = ch.keys[i - 1].frame + 1;
  float best_d = 1e30f;
  for (int f = ch.keys[i - 1].frame + 1; f < ch.keys[i].frame; f++) {
    const float d = length(ch.evaluate(f) - v);
    if (d < best_d) {
      best_d = d;
      best = f;
    }
  }
  return best;
}

std::vector<MotionPathControl> motion_path_controls(const Document &doc, const MotionPathTarget &target,
                                                    const std::function<bool(const MotionPathControl &)> &tips)
{
  OANIM_PROFILE("motion_path_controls");
  std::vector<MotionPathControl> out;
  const Channel<float2> *ch = handle_channel(const_cast<Document &>(doc), target);
  if (!ch) {
    return out;
  }
  /* Every place wanted (each control point where it is, the path on its dot's frame, and nudged for its tips: as
   * value_jacobian has them) worked out together, side by side. */
  struct Wanted {
    MotionPathControl c;
    size_t key = 0, j = 0, place = 0;
    bool tips = false;
  };
  std::vector<Wanted> wanted;
  std::vector<HeldPlace> places;
  for (size_t i = 1; i < ch->keys.size(); i++) {
    const Key<float2> &a = ch->keys[i - 1], &b = ch->keys[i];
    for (size_t j = 0; j < b.path_controls.size(); j++) {
      Wanted w;
      w.key = i;
      w.j = j;
      w.c.end = b.frame;
      w.c.index = int(j);
      const float2 v = path_control_value(a.value, b.value, b.path_controls[j]);
      w.c.near_frame = stretch_nearest_frame(*ch, i, v);
      w.tips = !tips || tips(w.c);
      w.place = places.size();
      const int f = w.c.near_frame;
      places.push_back({f, v});
      places.push_back({f, ch->evaluate(f)});
      if (w.tips) {
        places.push_back({f, v + float2{VALUE_STEP, 0.0f}});
        places.push_back({f, v + float2{0.0f, VALUE_STEP}});
      }
      wanted.push_back(w);
    }
  }
  Document copy;
  if (wanted.empty() || !held_copy(doc, target, copy)) {
    return out;
  }
  held_copy_places(copy, target, places);
  for (Wanted &w : wanted) {
    MotionPathControl &c = w.c;
    const HeldPlace *p = &places[w.place];
    if (!p[0].ok || !p[1].ok || (w.tips && (!p[2].ok || !p[3].ok))) {
      continue;
    }
    c.at = p[0].at;
    c.curve = p[1].at;
    c.in_tip = c.out_tip = c.at;
    if (w.tips) {
      /* The handles as the path runs along them, shown through how the channel shows there (as the point). */
      const float2 cx = (p[2].at - c.at) / VALUE_STEP, cy = (p[3].at - c.at) / VALUE_STEP;
      const StretchPoints s = stretch_points(ch->keys, w.key);
      c.in_tip = c.at + cx * s.in[w.j + 1].x + cy * s.in[w.j + 1].y;
      c.out_tip = c.at + cx * s.out[w.j + 1].x + cy * s.out[w.j + 1].y;
    }
    const PathControlHandles h = path_control_handles_of(ch->keys[w.key], w.j);
    c.set = h.set;
    c.aligned = h.aligned;
    out.push_back(c);
  }
  return out;
}

bool motion_path_control_add(Document &doc, const MotionPathTarget &target, int frame, int *r_end, int *r_index)
{
  Channel<float2> *ch = handle_channel(doc, target);
  const size_t i = ch ? stretch_inside(*ch, frame) : 0;
  if (!i) {
    return false;
  }
  /* The curve split at the dot, so the path keeps its shape and every dot stays: the dot's frame is timed along it by
   * length, which the split keeps. */
  /* Smooth is timed by its curve's own parameter, which the split changes (its end handles now run to the control
   * point): its dots' share of the way kept as speeds instead, as the curve split at a new key shares out its
   * timing. */
  const Key<float2> &b = ch->keys[i], &a = ch->keys[i - 1];
  const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
  std::vector<float> u;
  if (b.interp == Interp::Smooth && b.frame - start >= 1) {
    for (int f = start; f <= b.frame; f++) {
      u.push_back(stretch_smooth_progress(ch->keys, i, f));
    }
  }
  size_t at = 0;
  if (!key_control_split(ch->keys, i, ch->evaluate(frame), &at)) {
    return false;
  }
  if (u.size() >= 2) {
    u.front() = 0.0f;
    u.back() = 1.0f;
    key_speeds_set(ch->keys, i, progress_speeds(u));
  }
  channel_keys_changed();
  if (r_end) {
    *r_end = ch->keys[i].frame;
  }
  if (r_index) {
    *r_index = int(at);
  }
  animation_evaluate(doc, doc.frame_current);
  return true;
}

bool motion_path_control_set(Document &doc, const MotionPathTarget &target, int end, int index, int near_frame, float2 at)
{
  Channel<float2> *ch = handle_channel(doc, target);
  Key<float2> *b = ch ? ch->key_at(end) : nullptr;
  if (!b || b == ch->keys.data() || index < 0 || index >= int(b->path_controls.size())) {
    return false;
  }
  const Key<float2> &a = *(b - 1);
  const float2 v = path_control_value(a.value, b->value, b->path_controls[size_t(index)]);
  float2 p, cx, cy;
  if (!value_jacobian(doc, target, near_frame, v, p, &cx, &cy)) {
    return false;
  }
  const float det = cx.x * cy.y - cx.y * cy.x;
  if (std::abs(det) < 1e-12f) {
    return false;
  }
  const float2 miss = at - p;
  const float2 step{(miss.x * cy.y - miss.y * cy.x) / det, (cx.x * miss.y - cx.y * miss.x) / det};
  b->path_controls[size_t(index)] = path_control_stored(a.value, b->value, v + step);
  channel_keys_changed();
  animation_evaluate(doc, doc.frame_current);
  return true;
}

/* The stretch arriving at keys[i] timed so each frame's dot is where `was` (one a frame from frame `from`) had it:
 * Custom, as far along its path now as the nearest place to there, fitted. */
static void stretch_timing_keep(Channel<float2> &ch, size_t i, const std::vector<float2> &was, int from)
{
  const Key<float2> &a = ch.keys[i - 1], &b = ch.keys[i];
  const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
  if (b.frame - start < 1) {
    return;
  }
  std::vector<float> u;
  for (int f = start; f <= b.frame; f++) {
    u.push_back(stretch_path_progress(ch.keys, i, was[size_t(f - from)]));
  }
  u.front() = 0.0f;
  u.back() = 1.0f;
  key_timing_set(ch.keys, i, timing_fit(u));
}

static std::vector<float2> channel_values(const Channel<float2> &ch, int from, int to)
{
  std::vector<float2> was;
  for (int f = from; f <= to; f++) {
    was.push_back(ch.evaluate(f));
  }
  return was;
}

bool motion_path_key_to_control_allowed(const Document &doc, const MotionPathTarget &target, int frame,
                                        std::string *r_why)
{
  const auto no = [&](const char *why) {
    if (r_why) {
      *r_why = why;
    }
    return false;
  };
  Channel<float2> *ch = handle_channel(const_cast<Document &>(doc), target);
  if (!ch) {
    return no("Only a position key can be a control point");
  }
  const Key<float2> *k = ch->key_at(frame);
  if (!k) {
    return no("No position key there");
  }
  const size_t i = size_t(k - ch->keys.data());
  if (frame == 0 || i == 0 || i + 1 >= ch->keys.size()) {
    return no("Only a key with a key either side of it");
  }
  if (ch->keys[i - 1].interp == Interp::Cycle || k->interp == Interp::Cycle) {
    return no("Not where a Cycle plays");
  }
  const Layer *layer = layer_find(doc, target.layer);
  if (target.point && layer && layer->key_whole_drawing) {
    return no("Key Whole Drawing keeps a key on every point on this frame");
  }
  return true;
}

bool motion_path_key_to_control(Document &doc, const MotionPathTarget &target, int frame)
{
  if (!motion_path_key_to_control_allowed(doc, target, frame, nullptr)) {
    return false;
  }
  Channel<float2> &ch = *handle_channel(doc, target);
  const size_t i = size_t(ch.key_at(frame) - ch.keys.data());
  const int from = ch.keys[i - 1].frame;
  const std::vector<float2> was = channel_values(ch, from, ch.keys[i + 1].frame);
  /* On 2s or 3s the frames step as they did, kept in the timing itself: the joined stretch on 1s. */
  const bool stepped = ch.keys[i - 1].interval > 1 || ch.keys[i].interval > 1;
  if (!key_curve_join(ch.keys, i)) {
    return false;
  }
  if (stepped) {
    ch.keys[i - 1].interval = 1;
  }
  stretch_timing_keep(ch, i, was, from);
  channel_keys_changed();
  animation_evaluate(doc, doc.frame_current);
  return true;
}

bool motion_path_control_to_key(Document &doc, const MotionPathTarget &target, int end, int index, int *r_frame)
{
  Channel<float2> *ch = handle_channel(doc, target);
  Key<float2> *b = ch ? ch->key_at(end) : nullptr;
  if (!b || b == ch->keys.data() || index < 0 || index >= int(b->path_controls.size())) {
    return false;
  }
  const size_t i = size_t(b - ch->keys.data());
  const Key<float2> &a = ch->keys[i - 1];
  const int from = a.frame, start = std::min(a.frame + std::max(a.hold, 0), b->frame);
  const std::vector<float2> was = channel_values(*ch, from, b->frame);
  /* On the frame whose dot is nearest it, after the hold (so the stretch into it has time to move). */
  const float2 v = path_control_value(a.value, b->value, b->path_controls[size_t(index)]);
  int frame = 0;
  float best = std::numeric_limits<float>::max();
  for (int f = start + 1; f < b->frame; f++) {
    const float d = length(was[size_t(f - from)] - v);
    if (d < best) {
      best = d;
      frame = f;
    }
  }
  if (frame == 0) {
    return false;
  }
  const bool stepped = a.interval > 1;
  const size_t k = key_control_to_key(ch->keys, i, size_t(index), frame);
  if (!k) {
    return false;
  }
  if (stepped) {
    ch->keys[k - 1].interval = 1;
    ch->keys[k].interval = 1;
  }
  stretch_timing_keep(*ch, k, was, from);
  stretch_timing_keep(*ch, k + 1, was, from);
  channel_keys_changed();
  if (r_frame) {
    *r_frame = frame;
  }
  animation_evaluate(doc, doc.frame_current);
  return true;
}

int motion_path_controls_to_keys(Document &doc, const std::vector<MotionPathHandlePick> &picks)
{
  std::vector<std::pair<MotionPathTarget, float2>> where;
  for (const MotionPathHandlePick &p : picks) {
    Channel<float2> *ch = p.control >= 0 && p.control_handle == 0 ? handle_channel(doc, p.target) : nullptr;
    const Key<float2> *b = ch ? ch->key_at(p.frame) : nullptr;
    if (b && b != ch->keys.data() && p.control < int(b->path_controls.size())) {
      where.push_back({p.target, path_control_value((b - 1)->value, b->value, b->path_controls[size_t(p.control)])});
    }
  }
  int done = 0;
  for (const auto &[t, v] : where) {
    Channel<float2> *ch = handle_channel(doc, t);
    int end = 0, index = -1;
    float best = 1e-4f;
    for (size_t i = 1; ch && i < ch->keys.size(); i++) {
      const Key<float2> &a = ch->keys[i - 1], &b = ch->keys[i];
      for (size_t j = 0; j < b.path_controls.size(); j++) {
        const float d = length(path_control_value(a.value, b.value, b.path_controls[j]) - v);
        if (d < best) {
          best = d;
          end = b.frame;
          index = int(j);
        }
      }
    }
    done += index >= 0 && motion_path_control_to_key(doc, t, end, index) ? 1 : 0;
  }
  return done;
}

std::vector<MotionPathTarget> motion_path_key_to_control_targets(const Document &doc, const std::vector<Layer *> &layers,
                                                                const KeyFilter &filter, int frame,
                                                                std::string *r_why)
{
  std::vector<MotionPathTarget> out;
  for (Layer *layer : layers) {
    std::set<const void *> shown;
    for (const ChannelRef &ref : layer_filtered_channels(*layer, filter)) {
      if (ref.vector) {
        shown.insert(ref.vector);
      }
    }
    std::vector<MotionPathTarget> candidates;
    MotionPathTarget t;
    t.layer = layer->id;
    candidates.push_back(t);
    for (const Bone &bone : layer->bones) {
      MotionPathTarget b = t;
      b.bone = bone.id;
      b.head = true;
      candidates.push_back(b);
    }
    for (const MeshPoint &p : layer->drawing.points) {
      MotionPathTarget pt = t;
      pt.point = p.id;
      candidates.push_back(pt);
    }
    for (const MotionPathTarget &c : candidates) {
      const Channel<float2> *ch = handle_channel(const_cast<Document &>(doc), c);
      if (!ch || !shown.count(ch) || !ch->key_at(frame)) {
        continue;
      }
      std::string why;
      if (motion_path_key_to_control_allowed(doc, c, frame, &why)) {
        out.push_back(c);
      }
      else if (r_why && r_why->empty()) {
        *r_why = why;
      }
    }
  }
  return out;
}

int motion_path_controls_delete(Document &doc, const std::vector<MotionPathHandlePick> &picks)
{
  /* Highest index first, so the others keep theirs. */
  std::vector<MotionPathHandlePick> sorted;
  for (const MotionPathHandlePick &p : picks) {
    if (p.control >= 0 && p.control_handle == 0) {
      sorted.push_back(p);
    }
  }
  std::sort(sorted.begin(), sorted.end(), [](const auto &x, const auto &y) { return x.control > y.control; });
  int deleted = 0;
  for (const MotionPathHandlePick &p : sorted) {
    Channel<float2> *ch = handle_channel(doc, p.target);
    Key<float2> *b = ch ? ch->key_at(p.frame) : nullptr;
    if (b && p.control < int(b->path_controls.size())) {
      b->path_controls.erase(b->path_controls.begin() + p.control);
      if (p.control < int(b->path_control_handles.size())) {
        b->path_control_handles.erase(b->path_control_handles.begin() + p.control);
      }
      deleted++;
    }
  }
  if (deleted) {
    channel_keys_changed();
    animation_evaluate(doc, doc.frame_current);
  }
  return deleted;
}

/* Control point `j` of the stretch arriving at keys[i]: its handles made its own (as the path runs along them now). */
static PathControlHandles &control_handles_own(std::vector<Key<float2>> &keys, size_t i, size_t j)
{
  Key<float2> &a = keys[i - 1], &b = keys[i];
  b.path_control_handles.resize(b.path_controls.size());
  PathControlHandles &h = b.path_control_handles[j];
  if (!h.set) {
    const StretchPoints s = stretch_points(keys, i);
    h.in = path_control_offset_stored(a.value, b.value, s.in[j + 1]);
    h.out = path_control_offset_stored(a.value, b.value, s.out[j + 1]);
    h.set = true;
  }
  return h;
}

/* The stretch ending on `end` and its control point `index`: the key's index, or 0 with none. */
static size_t control_stretch(Channel<float2> *ch, int end, int index)
{
  Key<float2> *b = ch ? ch->key_at(end) : nullptr;
  if (!b || b == ch->keys.data() || index < 0 || index >= int(b->path_controls.size())) {
    return 0;
  }
  return size_t(b - ch->keys.data());
}

bool motion_path_control_handle_set(Document &doc, const MotionPathTarget &target, int end, int index, bool out,
                                    int near_frame, float2 at, bool alone)
{
  Channel<float2> *ch = handle_channel(doc, target);
  const size_t i = control_stretch(ch, end, index);
  if (!i) {
    return false;
  }
  const size_t j = size_t(index);
  const Key<float2> &a = ch->keys[i - 1], &b = ch->keys[i];
  const float2 v = path_control_value(a.value, b.value, b.path_controls[j]);
  float2 p, cx, cy;
  if (!value_jacobian(doc, target, near_frame, v, p, &cx, &cy)) {
    return false;
  }
  const float det = cx.x * cy.y - cx.y * cy.x;
  if (std::abs(det) < 1e-12f) {
    return false;
  }
  /* The tip in the channel's own space, through how it shows at the point. */
  const float2 miss = at - p;
  const float2 offset{(miss.x * cy.y - miss.y * cy.x) / det, (cx.x * miss.y - cx.y * miss.x) / det};
  PathControlHandles &h = control_handles_own(ch->keys, i, j);
  float2 in = path_control_offset_value(a.value, b.value, h.in), out_h = path_control_offset_value(a.value, b.value, h.out);
  (out ? out_h : in) = offset;
  if (alone) {
    h.aligned = false;
  }
  else if (h.aligned && length(offset) > 1e-9f) {
    float2 &other = out ? in : out_h;
    other = -normalize(offset) * length(other);
  }
  h.in = path_control_offset_stored(a.value, b.value, in);
  h.out = path_control_offset_stored(a.value, b.value, out_h);
  channel_keys_changed();
  animation_evaluate(doc, doc.frame_current);
  return true;
}

int motion_path_controls_align(Document &doc, const std::vector<MotionPathHandlePick> &picks, bool align)
{
  /* Each control point once: which of its parts are picked. */
  struct One {
    MotionPathTarget target;
    int end = 0, index = 0;
    bool point = false, in = false, out = false;
  };
  std::vector<One> ones;
  for (const MotionPathHandlePick &p : picks) {
    if (p.control < 0) {
      continue;
    }
    auto it = std::find_if(ones.begin(), ones.end(), [&](const One &o) {
      return o.target.same(p.target) && o.end == p.frame && o.index == p.control;
    });
    if (it == ones.end()) {
      ones.push_back({p.target, p.frame, p.control});
      it = ones.end() - 1;
    }
    (p.control_handle == 0 ? it->point : p.control_handle == 1 ? it->in : it->out) = true;
  }
  int changed = 0;
  for (const One &o : ones) {
    Channel<float2> *ch = handle_channel(doc, o.target);
    const size_t i = control_stretch(ch, o.end, o.index);
    if (!i) {
      continue;
    }
    const Key<float2> &a = ch->keys[i - 1], &b = ch->keys[i];
    if (!align) {
      if (path_control_handles_of(b, size_t(o.index)).aligned) {
        control_handles_own(ch->keys, i, size_t(o.index)).aligned = false;
        changed++;
      }
      continue;
    }
    PathControlHandles &h = control_handles_own(ch->keys, i, size_t(o.index));
    float2 in = path_control_offset_value(a.value, b.value, h.in), out = path_control_offset_value(a.value, b.value, h.out);
    const float in_len = length(in), out_len = length(out);
    if (in_len > 1e-9f && out_len > 1e-9f) {
      if (o.point || (o.in && o.out)) {
        /* Both turn, halfway each (curve_point_align_both). */
        float2 back = in / in_len - out / out_len;
        if (length(back) < 1e-4f) {
          /* Folded back on each other: the way from the point before to the one after. */
          const StretchPoints s = stretch_points(ch->keys, i);
          back = s.p[size_t(o.index)] - s.p[size_t(o.index) + 2];
        }
        if (length(back) > 1e-9f) {
          back = normalize(back);
          in = back * in_len;
          out = -back * out_len;
        }
      }
      else if (o.out) {
        /* The handle picked is the one that turns into line. */
        out = -normalize(in) * out_len;
      }
      else {
        in = -normalize(out) * in_len;
      }
    }
    h.in = path_control_offset_stored(a.value, b.value, in);
    h.out = path_control_offset_stored(a.value, b.value, out);
    h.aligned = true;
    changed++;
  }
  if (changed) {
    channel_keys_changed();
    animation_evaluate(doc, doc.frame_current);
  }
  return changed;
}

int motion_path_control_handles_reset(Document &doc, const std::vector<MotionPathHandlePick> &picks)
{
  int changed = 0;
  for (const MotionPathHandlePick &p : picks) {
    if (p.control < 0 || p.control_handle == 0) {
      continue;
    }
    Channel<float2> *ch = handle_channel(doc, p.target);
    const size_t i = control_stretch(ch, p.frame, p.control);
    if (!i) {
      continue;
    }
    Key<float2> &b = ch->keys[i];
    if (size_t(p.control) < b.path_control_handles.size() && b.path_control_handles[size_t(p.control)] != PathControlHandles{}) {
      b.path_control_handles[size_t(p.control)] = PathControlHandles{};
      changed++;
    }
  }
  if (changed) {
    channel_keys_changed();
    animation_evaluate(doc, doc.frame_current);
  }
  return changed;
}

bool motion_path_pick_valid(const Document &doc, const MotionPathHandlePick &pick)
{
  if (pick.control >= 0) {
    const Channel<float2> *ch = handle_channel(const_cast<Document &>(doc), pick.target);
    const Key<float2> *b = ch ? ch->key_at(pick.frame) : nullptr;
    return b && pick.control < int(b->path_controls.size());
  }
  float2 key, in, out;
  return motion_path_handles(doc, pick.target, pick.frame, key, in, out);
}

/* A key's dot as drawn, else where it is on its frame. */
static float2 key_dot(const Document &doc, const std::vector<MotionPath> &paths, const MotionPathTarget &target,
                      int frame)
{
  for (const MotionPath &p : paths) {
    if (p.target.same(target) && frame >= p.first && frame - p.first < int(p.points.size())) {
      return p.points[size_t(frame - p.first)];
    }
  }
  float2 at{0.0f, 0.0f};
  motion_path_position_at(doc, target, frame, at);
  return at;
}

float2 motion_path_handles_pivot(const Document &doc, const std::vector<MotionPath> &paths,
                                 const std::vector<MotionPathHandlePick> &picks,
                                 const std::vector<std::pair<MotionPathTarget, int>> &keys)
{
  float2 middle{0.0f, 0.0f};
  int points = 0;
  std::optional<float2> first_point;
  for (const MotionPathHandlePick &hp : picks) {
    if (hp.control < 0) {
      continue;
    }
    const MotionPath *path = nullptr;
    for (const MotionPath &p : paths) {
      if (p.target.same(hp.target)) {
        path = &p;
      }
    }
    /* (Where they are, not their tips.) */
    for (const MotionPathControl &mc :
         motion_path_controls(doc, hp.target, [](const MotionPathControl &) { return false; }))
    {
      if (mc.end != hp.frame || mc.index != hp.control) {
        continue;
      }
      const float2 at = path ? motion_path_drawn(*path, mc.near_frame, mc.curve).apply(mc.at) : mc.at;
      if (hp.control_handle == 0) {
        middle = middle + at;
        points++;
      }
      else if (!first_point) {
        first_point = at;
      }
    }
  }
  /* Keys picked with them: the middle of every key and control point. */
  for (const auto &[t, f] : keys) {
    middle = middle + key_dot(doc, paths, t, f);
    points++;
  }
  if (points) {
    return middle * (1.0f / float(points));
  }
  if (picks.empty()) {
    return middle;
  }
  if (picks.front().control >= 0 && first_point) {
    return *first_point;
  }
  /* A key handle's key. */
  return key_dot(doc, paths, picks.front().target, picks.front().frame);
}

int motion_path_handles_transform(Document &doc, const std::vector<MotionPathHandlePick> &picks, char kind,
                                  float2 offset, float angle, float factor, const float2 *pivot)
{
  /* Where every tip goes, from where they all are before any moves (a key's two handles picked turn together). */
  std::vector<float2> to(picks.size());
  std::vector<bool> ok(picks.size(), false);
  const float c = std::cos(angle), s = std::sin(angle);
  /* Control points: moved, or turned and scaled about their middle. */
  std::vector<MotionPathControl> controls;
  std::vector<size_t> control_of;
  float2 middle{0.0f, 0.0f};
  /* And control point handles: each tip about its point. */
  std::vector<std::pair<size_t, float2>> control_tips;
  for (size_t i = 0; i < picks.size(); i++) {
    if (picks[i].control < 0) {
      continue;
    }
    /* (Only the picked one's tips are wanted, and only where they move apart from it: a handle picked, or the point
     * turned or scaled. Each costs two more frames worked out.) */
    const MotionPathHandlePick &pick = picks[i];
    for (const MotionPathControl &mc : motion_path_controls(doc, pick.target, [&](const MotionPathControl &c) {
           return (pick.control_handle || kind != 'G') && c.end == pick.frame && c.index == pick.control;
         }))
    {
      if (mc.end != picks[i].frame || mc.index != picks[i].control) {
        continue;
      }
      if (picks[i].control_handle) {
        const float2 tip = picks[i].control_handle == 2 ? mc.out_tip : mc.in_tip, d = tip - mc.at;
        control_tips.push_back({i, kind == 'G'   ? tip + offset
                                   : kind == 'R' ? mc.at + float2{d.x * c - d.y * s, d.x * s + d.y * c}
                                                 : mc.at + d * factor});
        controls.push_back(mc);
        control_of.push_back(i);
        continue;
      }
      controls.push_back(mc);
      control_of.push_back(i);
      middle = middle + mc.at;
    }
  }
  const size_t moved_points = size_t(std::count_if(control_of.begin(), control_of.end(),
                                                   [&](size_t i) { return picks[i].control_handle == 0; }));
  if (moved_points) {
    middle = middle * (1.0f / float(moved_points));
  }
  if (pivot) {
    middle = *pivot;
  }
  for (size_t i = 0; i < picks.size(); i++) {
    float2 key, in, out;
    if (picks[i].control >= 0 || !motion_path_handles(doc, picks[i].target, picks[i].frame, key, in, out)) {
      continue;
    }
    const float2 tip = picks[i].out ? out : in, d = tip - key;
    to[i] = kind == 'G' ? tip + offset
            : kind == 'R' ? key + float2{d.x * c - d.y * s, d.x * s + d.y * c}
                          : key + d * factor;
    ok[i] = true;
  }
  int changed = 0;
  for (size_t i = 0; i < picks.size(); i++) {
    if (ok[i] && motion_path_handle_set(doc, picks[i].target, picks[i].frame, picks[i].out, to[i], false)) {
      changed++;
    }
  }
  for (const auto &[i, tip] : control_tips) {
    const MotionPathHandlePick &p = picks[i];
    /* Both of a control point's handles picked: each where it goes, so the second isn't turned by the first. */
    const bool both = std::any_of(control_tips.begin(), control_tips.end(), [&](const auto &o) {
      return o.first != i && picks[o.first].target.same(p.target) && picks[o.first].frame == p.frame &&
             picks[o.first].control == p.control;
    });
    /* Its own control point (another path's can share its end frame and index). */
    const auto mc = controls.begin() + (std::find(control_of.begin(), control_of.end(), i) - control_of.begin());
    changed += motion_path_control_handle_set(doc, p.target, p.frame, p.control, p.control_handle == 2,
                                              mc->near_frame, tip, both)
                   ? 1
                   : 0;
    if (both && mc->aligned) {
      /* Moved together, so still as they were: in line. */
      Key<float2> *k = handle_channel(doc, p.target)->key_at(p.frame);
      k->path_control_handles[size_t(p.control)].aligned = true;
    }
  }
  for (size_t j = 0; j < controls.size(); j++) {
    const MotionPathControl &mc = controls[j];
    if (picks[control_of[j]].control_handle) {
      continue;
    }
    const float2 d = mc.at - middle;
    const float2 goes = kind == 'G'   ? mc.at + offset
                        : kind == 'R' ? middle + float2{d.x * c - d.y * s, d.x * s + d.y * c}
                                      : middle + d * factor;
    const MotionPathHandlePick &p = picks[control_of[j]];
    if (!motion_path_control_set(doc, p.target, mc.end, mc.index, mc.near_frame, goes)) {
      continue;
    }
    changed++;
    if (kind == 'G') {
      /* Kept relative to the point, its handles ride along. */
      continue;
    }
    /* As a vector point's: its handles turn and scale with it (alone, about it); a handle picked itself went its own
     * way above. */
    bool turned = false;
    for (const int part : {1, 2}) {
      const bool own_pick = std::any_of(control_tips.begin(), control_tips.end(), [&](const auto &o) {
        const MotionPathHandlePick &q = picks[o.first];
        return q.target.same(p.target) && q.frame == mc.end && q.control == mc.index && q.control_handle == part;
      });
      const float2 h = (part == 2 ? mc.out_tip : mc.in_tip) - mc.at;
      if (own_pick || length(h) < 1e-9f) {
        continue;
      }
      const float2 tip = goes + (kind == 'R' ? float2{h.x * c - h.y * s, h.x * s + h.y * c} : h * factor);
      turned |= motion_path_control_handle_set(doc, p.target, mc.end, mc.index, part == 2, mc.near_frame, tip, true);
    }
    if (turned) {
      /* Each where it goes: as in line as they were. */
      Key<float2> *k = handle_channel(doc, p.target)->key_at(mc.end);
      k->path_control_handles[size_t(mc.index)].aligned = mc.aligned;
    }
  }
  animation_evaluate(doc, doc.frame_current);
  return changed;
}

bool motion_path_picks_with_keys(const std::vector<MotionPathHandlePick> &picks)
{
  return std::all_of(picks.begin(), picks.end(),
                     [](const MotionPathHandlePick &p) { return p.control >= 0 && p.control_handle == 0; });
}

int motion_path_picks_transform(Document &doc, const std::vector<std::pair<MotionPathTarget, int>> &keys,
                                const std::vector<MotionPathHandlePick> &picks, char kind, float2 offset, float angle,
                                float factor, const float2 *pivot)
{
  if (picks.empty()) {
    return motion_path_keys_transform(doc, keys, kind, offset, angle, factor);
  }
  if (keys.empty() || !motion_path_picks_with_keys(picks)) {
    return motion_path_handles_transform(doc, picks, kind, offset, angle, factor);
  }
  /* About the middle of them all, worked out before anything moves. The control points first: stored where they are,
   * the keys moving after leaves them where they went. */
  const float2 about = pivot ? *pivot : motion_path_handles_pivot(doc, {}, picks, keys);
  const int changed = motion_path_handles_transform(doc, picks, kind, offset, angle, factor, &about);
  return changed + motion_path_keys_transform(doc, keys, kind, offset, angle, factor, &about);
}

int motion_path_handles_type_at(Document &doc, const std::vector<MotionPathHandlePick> &picks, PathHandles mode)
{
  /* motion_path_handles_type on these keys alone: selected for it, the selection put back after. */
  std::vector<std::pair<Key<float2> *, bool>> was;
  std::vector<MotionPathTarget> targets;
  for (const MotionPathHandlePick &p : picks) {
    Channel<float2> *ch = p.control < 0 ? handle_channel(doc, p.target) : nullptr;
    if (!ch) {
      continue;
    }
    if (std::none_of(targets.begin(), targets.end(), [&](const MotionPathTarget &t) { return t.same(p.target); })) {
      targets.push_back(p.target);
      for (Key<float2> &k : ch->keys) {
        was.push_back({&k, k.selected});
        k.selected = false;
      }
    }
  }
  for (const MotionPathHandlePick &p : picks) {
    if (Channel<float2> *ch = p.control < 0 ? handle_channel(doc, p.target) : nullptr) {
      if (Key<float2> *k = ch->key_at(p.frame)) {
        k->selected = true;
      }
    }
  }
  const int changed = motion_path_handles_type(doc, targets, mode);
  for (auto &[k, sel] : was) {
    k->selected = sel;
  }
  return changed;
}

int motion_path_handles_auto(Document &doc, const MotionPathTarget &target)
{
  Channel<float2> *ch = handle_channel(doc, target);
  int changed = 0;
  if (ch) {
    for (Key<float2> &k : ch->keys) {
      if (k.selected && k.path_handles != PathHandles::Auto) {
        k.path_handles = PathHandles::Auto;
        changed++;
      }
    }
  }
  return changed;
}

bool motion_path_position(const Document &doc, const MotionPathTarget &target, float2 &r_at)
{
  return target_position(doc, target, r_at);
}

bool motion_path_position_at(const Document &doc, const MotionPathTarget &target, int frame, float2 &r_at)
{
  Document copy = doc;
  copy.frame_current = frame;
  return path_position(copy, target, frame, r_at);
}

bool motion_path_place(Document &doc, const MotionPathTarget &target, float2 at)
{
  if (g_shown && motion_path_own_motion(doc, target)) {
    /* Own Motion: `at` is where the path is drawn, everything else held as shown; put there on that, the values found
     * are its own, the same on the frame being edited. */
    Document held = *g_shown;
    OwnValues v;
    if (!own_values(doc, target, v)) {
      return false;
    }
    own_values_set(held, target, v);
    /* Its own constraints left out, as on its path: a pupil's Look At would stop it short of where it is drawn going. */
    own_constraints_off(held, target);
    held.frame_current = doc.frame_current;
    const Document *outer = g_shown;
    g_shown = nullptr;
    const bool ok = motion_path_place(held, target, at);
    g_shown = outer;
    if (!ok || !own_values(held, target, v)) {
      return false;
    }
    own_values_set(doc, target, v);
    return true;
  }
  Bone *bone = nullptr;
  float2 *value = place_value(doc, target, &bone);
  if (bone && tip_turns(doc, target)) {
    return solve_tip(doc, target, *bone, at);
  }
  return value && solve_position(doc, target, *value, at);
}

void motion_path_neighbour_keys(const Document &doc, const MotionPathTarget &target, int frame, int &r_before,
                                int &r_after)
{
  r_before = r_after = 0;
  for (const int f : motion_path_keys(doc, target)) {
    if (f < frame) {
      r_before = f;
    }
    else if (f > frame && !r_after) {
      r_after = f;
    }
  }
}

bool motion_path_straighten(const Document &doc, const MotionPathTarget &target, float2 &at)
{
  int before = 0, after = 0;
  motion_path_neighbour_keys(doc, target, doc.frame_current, before, after);
  float2 a, b;
  if (!before || !after || !motion_path_position_at(doc, target, before, a) ||
      !motion_path_position_at(doc, target, after, b))
  {
    return false;
  }
  const float2 ab = b - a;
  const float len2 = ab.x * ab.x + ab.y * ab.y;
  const float t = len2 > 1e-12f ? std::clamp(((at.x - a.x) * ab.x + (at.y - a.y) * ab.y) / len2, 0.0f, 1.0f) : 0.0f;
  at = a + ab * t;
  return true;
}

bool motion_path_breakdown(Document &doc, const MotionPathTarget &target, float factor)
{
  int before = 0, after = 0;
  motion_path_neighbour_keys(doc, target, doc.frame_current, before, after);
  if (!before || !after) {
    return false;
  }
  OwnValues a, b;
  Document copy = doc;
  copy.frame_current = before;
  animation_evaluate(copy, before);
  const bool got_a = own_values(copy, target, a);
  copy.frame_current = after;
  animation_evaluate(copy, after);
  if (!got_a || !own_values(copy, target, b)) {
    return false;
  }
  const float f = std::clamp(factor, 0.0f, 1.0f);
  own_values_set(doc, target,
                 {a.position + (b.position - a.position) * f, a.scale + (b.scale - a.scale) * f,
                  a.angle + (b.angle - a.angle) * f});
  return true;
}

std::vector<Layer *> motion_path_key_layers(const Document &doc, const MotionPathTarget &target)
{
  Document &d = const_cast<Document &>(doc);
  Layer *layer = layer_find(d, target.layer);
  if (!layer) {
    return {};
  }
  if (target.drawing && key_together_allowed(*layer) && layer->key_together) {
    return key_together_layers(d, *layer);
  }
  return {layer};
}

KeyFilter motion_path_key_filter(const Document &doc, const MotionPathTarget &target)
{
  KeyFilter filter;
  const Layer *layer = layer_find(doc, target.layer);
  if (layer && target.bone) {
    const int i = bone_index(*layer, target.bone);
    filter.kinds = KEY_BONE_ANGLE | KEY_BONE_TRANSFORM;
    filter.path = "bones[\"" + (i >= 0 ? layer->bones[size_t(i)].name : std::string()) + "\"].";
  }
  else if (layer && target.point) {
    filter.kinds = KEY_POINTS;
    filter.path = "points[" + std::to_string(std::max(0, point_index(*layer, target.point))) + "].";
  }
  else if (target.drawing && layer && key_together_allowed(*layer) && layer->key_together) {
    /* A folded group: every channel it keys as one, on the layers inside it. */
    auto channels = std::make_shared<std::set<const void *>>();
    drawing_channels(const_cast<Document &>(doc), const_cast<Layer &>(*layer), [&](auto &ch) {
      channels->insert(&ch);
      return true;
    });
    filter.channels = std::move(channels);
  }
  else if (target.drawing) {
    /* A drawing keyed as a whole: all its points' keys there. */
    filter.kinds = KEY_POINTS;
  }
  else {
    filter.kinds = KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM;
  }
  return filter;
}

MotionPathOnFrame::MotionPathOnFrame(Document &doc, int frame) : doc(doc), shown(doc.frame_current)
{
  /* Own Motion holds everything else as shown: kept as it is before going to the other frame. */
  if (doc.path_own_motion) {
    held = std::make_unique<Document>(doc);
    outer = g_shown;
    g_shown = held.get();
  }
  doc.frame_current = frame;
  animation_sync(doc);
}

MotionPathOnFrame::~MotionPathOnFrame()
{
  if (held) {
    g_shown = outer;
  }
  doc.frame_current = shown;
  animation_evaluate(doc, shown);
}

void motion_path_record(Document &doc)
{
  /* Moving a path's dot always keys it, as the tool promises, whatever auto-key says. */
  const bool auto_key = doc.auto_key;
  const AutoKeyMode mode = doc.auto_key_mode;
  doc.auto_key = true;
  doc.auto_key_mode = AutoKeyMode::AddReplace;
  animation_record(doc);
  doc.auto_key = auto_key;
  doc.auto_key_mode = mode;
}

int motion_path_smooth_arc(Document &doc, const MotionPathTarget &target, int first, int last)
{
  const std::vector<int> keys = motion_path_keys(doc, target);
  /* Where each in-between key should be, all worked out before any moves. */
  std::vector<std::pair<int, float2>> moves;
  for (size_t i = 0; i < keys.size(); i++) {
    float2 at;
    if (keys[i] > first && keys[i] < last && arc_fit(doc, target, keys, i, at)) {
      moves.push_back({keys[i], at});
    }
  }
  const int shown = doc.frame_current;
  int moved = 0;
  for (const auto &[k, at] : moves) {
    doc.frame_current = k;
    animation_sync(doc);
    if (motion_path_place(doc, target, at)) {
      motion_path_record(doc);
      moved++;
    }
  }
  doc.frame_current = shown;
  animation_sync(doc);
  return moved;
}

/* --- Path sculpting: the vector Sculpt tool's Grab, Smooth and Surface Smooth on motion paths -------------------- */

namespace {

/* Where a stretch's value `v` (between keys with maps `a` and `b`, `t` of the way along) shows, and back. */
struct KeyMap {
  float2 value, at, cx, cy;
};
float2 map_apply(const KeyMap &m, float2 v)
{
  const float2 d = v - m.value;
  return m.at + m.cx * d.x + m.cy * d.y;
}
float2 map_vector(const KeyMap &m, float2 d)
{
  return m.cx * d.x + m.cy * d.y;
}
/* Whether a step of the key's value moves where the target shows both ways (so map_unvector can undo it). */
bool map_invertible(const KeyMap &m)
{
  return std::abs(m.cx.x * m.cy.y - m.cy.x * m.cx.y) >= 1e-12f;
}
float2 map_unvector(const KeyMap &m, float2 d)
{
  const float det = m.cx.x * m.cy.y - m.cy.x * m.cx.y;
  if (!map_invertible(m)) {
    return float2{0.0f, 0.0f};
  }
  return float2{(d.x * m.cy.y - d.y * m.cy.x) / det, (m.cx.x * d.y - m.cx.y * d.x) / det};
}
KeyMap map_blend(const KeyMap &a, const KeyMap &b, float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  /* Each key's map moved to the same value, then mixed. */
  KeyMap m;
  m.value = a.value + (b.value - a.value) * t;
  m.at = map_apply(a, m.value) + (map_apply(b, m.value) - map_apply(a, m.value)) * t;
  m.cx = a.cx + (b.cx - a.cx) * t;
  m.cy = a.cy + (b.cy - a.cy) * t;
  return m;
}

/* A stretch's progress at each frame from its start (after the hold) to its key, by length along it: what the dots
 * keep while the path is sculpted. Empty for a stretch that doesn't move along its path (Step, Pose, Cycle). */
std::vector<float> stretch_length_speeds(const Channel<float2> &ch, size_t i)
{
  const Key<float2> &a = ch.keys[i - 1], &b = ch.keys[i];
  const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
  const int n = b.frame - start;
  if (n < 1 || b.interp == Interp::Step || b.interp == Interp::Pose || b.interp == Interp::Cycle || a.interp == Interp::Cycle) {
    return {};
  }
  if (b.interp == Interp::Custom && !b.timing.empty()) {
    /* Already by length along it. */
    return timing_speeds(b.timing, n);
  }
  if (interp_timed_by_mode(b.interp)) {
    /* Its mode's progress by length along it, signed: Back, Bounce and Elastic go past the key and back. */
    const KeySettings settings = key_settings(b);
    std::vector<float> u(size_t(n + 1), 0.0f);
    for (int j = 1; j <= n; j++) {
      u[size_t(j)] = interp_curve(settings, float(j) / float(n));
    }
    return progress_speeds(u);
  }
  /* The stretch without its control points (they lie on it by the same share) and without its interval. */
  Channel<float2> plain = ch;
  plain.keys[i].path_controls.clear();
  plain.keys[i - 1].interval = 1;
  std::vector<float> u(size_t(n + 1), 0.0f);
  u[size_t(n)] = 1.0f;
  for (int j = 1; j < n; j++) {
    u[size_t(j)] = nearest_progress(plain.keys, i, plain.evaluate(start + j));
  }
  for (int j = 1; j <= n; j++) {
    u[size_t(j)] = std::max(u[size_t(j)], u[size_t(j - 1)]);
  }
  return progress_speeds(u);
}

}  // namespace

struct PathSculptState {
  Document drawing;
  SculptStroke stroke;
  struct Target {
    MotionPathTarget target;
    std::vector<Key<float2>> keys;
    std::vector<KeyMap> maps;
    /* Each of the drawing's points: its key, or (control >= 0) the control point of the stretch arriving at it. */
    std::vector<int> key, control;
    std::vector<float2> pos;
    std::vector<float2> in, out;
    /* Each stretch's speeds keeping its timing by length (index: the key it arrives at). */
    std::vector<std::vector<float>> speeds;
    uint64_t layer = 0;
    /* The setup key's point (frame 0, shown), or -1: the brush reshapes its out handle but never moves it. */
    int pinned = -1;
    /* The keys the line runs between (indices into `keys`): the path as shown, from the key it starts on to the one it
     * ends on, so the line's ends are the path's (Smooth and Surface Smooth keep them, as a drawn curve's). */
    size_t lo = 0, hi = 0;
  };
  std::vector<Target> targets;
};

bool PathSculptStroke::empty() const
{
  return !state || state->targets.empty();
}

bool motion_path_sculptable(const Document &doc, const MotionPathTarget &target)
{
  const Channel<float2> *ch = handle_channel(const_cast<Document &>(doc), target);
  return ch && ch->keys.size() >= 2;
}

/* Drawn relative to something: from document space on each of `frames` to where the paths draw it (as
 * motion_paths_make works it out). Empty when drawn as they are. */
static std::map<int, Transform2D> drawn_spaces(const Document &doc, const std::set<int> &frames)
{
  std::map<int, Transform2D> out;
  if (doc.path_relative == PathRelative::World || frames.empty()) {
    return out;
  }
  const Transform2D back = relative_space(doc).inverted();
  Document copy = doc;
  for (const int f : frames) {
    copy.frame_current = f;
    animation_evaluate(copy, f);
    ArmaturePoseCacheScope poses;
    out[f] = back * relative_space(copy);
  }
  return out;
}

PathSculptStroke motion_path_sculpt_begin(const Document &doc, const std::vector<MotionPathTarget> &targets, int first,
                                          int last, const SculptDab &dab)
{
  PathSculptStroke out;
  auto st = std::make_shared<PathSculptState>();
  std::vector<uint64_t> ids;
  for (const MotionPathTarget &t : targets) {
    const Channel<float2> *ch = handle_channel(const_cast<Document &>(doc), t);
    if (!ch || ch->keys.size() < 2) {
      continue;
    }
    PathSculptState::Target tg;
    tg.target = t;
    tg.keys = ch->keys;
    bool ok = true;
    for (const Key<float2> &k : tg.keys) {
      KeyMap m;
      m.value = k.value;
      ok = ok && key_jacobian(doc, t, k.frame, m.at, m.cx, m.cy);
      /* A key whose value doesn't move the target (a tip its position keys don't carry): nothing the brush does to the
       * line could be given back to the keys, so the target is left out, as dragging its handles or keys is refused. */
      ok = ok && map_invertible(m);
      tg.maps.push_back(m);
    }
    if (!ok) {
      continue;
    }
    if (!motion_path_own_motion(doc, t)) {
      /* Sculpted where it is drawn: relative to something, each key's frame drawn turned and scaled as that is. */
      std::set<int> frames;
      for (const Key<float2> &k : tg.keys) {
        frames.insert(k.frame);
      }
      const std::map<int, Transform2D> spaces = drawn_spaces(doc, frames);
      for (size_t i = 0; i < tg.keys.size() && !spaces.empty(); i++) {
        const Transform2D &s = spaces.at(tg.keys[i].frame);
        KeyMap &m = tg.maps[i];
        m.at = s.apply(m.at);
        m.cx = s.apply_vector(m.cx);
        m.cy = s.apply_vector(m.cy);
      }
    }
    /* The path as a drawing: its keys and control points in order, with the handles the path runs along. */
    Layer &layer = document_add_layer(st->drawing, LayerType::Vector, "path");
    tg.layer = layer.id;
    Curve curve;
    const auto add = [&](int key, int control, float2 at, float2 in, float2 out_h, bool held, bool aligned) {
      MeshPoint p;
      p.pos = p.setup_pos = at;
      p.selected = !held;
      layer.drawing.points.push_back(p);
      CurvePoint cp;
      cp.point = int(layer.drawing.points.size()) - 1;
      cp.handle_in = in;
      cp.handle_out = out_h;
      cp.handles_set = true;
      cp.aligned = aligned;
      curve.points.push_back(cp);
      tg.key.push_back(key);
      tg.control.push_back(control);
      tg.pos.push_back(at);
      tg.in.push_back(in);
      tg.out.push_back(out_h);
    };
    /* Frame 0's key (the setup) too while shown: only its out handle changes, its point pinned. */
    const auto shown = [&](int f) { return f >= first && f <= last; };
    const size_t n = tg.keys.size();
    /* Only the path shown: from the last key on or before its first frame to the first on or after its last. The keys
     * beyond (the setup's and those before, when the path shows a later stretch) aren't part of the line, or the
     * path's first key would be a point in its middle, smoothed away from where the path starts. */
    tg.lo = 0;
    tg.hi = n - 1;
    for (size_t i = 0; i < n; i++) {
      if (tg.keys[i].frame <= first) {
        tg.lo = i;
      }
    }
    for (size_t i = n; i-- > 0;) {
      if (tg.keys[i].frame >= last) {
        tg.hi = i;
      }
    }
    tg.hi = std::max(tg.hi, std::min(tg.lo + 1, n - 1));
    for (size_t i = tg.lo; i <= tg.hi; i++) {
      const Key<float2> &k = tg.keys[i];
      if (i > tg.lo) {
        const StretchPoints s = stretch_points(tg.keys, i);
        for (size_t c = 1; c + 1 < s.p.size(); c++) {
          const float along = tg.keys[i].path_controls[c - 1].x;
          const KeyMap m = map_blend(tg.maps[i - 1], tg.maps[i], along);
          add(int(i), int(c - 1), map_apply(m, s.p[c]), map_vector(m, s.in[c]), map_vector(m, s.out[c]),
              !shown(tg.keys[i - 1].frame) && !shown(k.frame), path_control_handles_of(k, c - 1).aligned);
        }
      }
      float2 in{0.0f, 0.0f}, out_h{0.0f, 0.0f};
      if (i > 0 && i > tg.lo) {
        in = map_vector(tg.maps[i], stretch_points(tg.keys, i).in.back());
      }
      if (i + 1 < n && i < tg.hi) {
        out_h = map_vector(tg.maps[i], stretch_points(tg.keys, i + 1).out.front());
      }
      if (k.frame == 0 && shown(0)) {
        tg.pinned = int(tg.pos.size());
      }
      add(int(i), -1, tg.maps[i].at, in, out_h, !shown(k.frame), k.path_handles != PathHandles::Broken);
    }
    layer.drawing.curves.push_back(curve);
    tg.speeds.resize(n);
    for (size_t i = 1; i < n; i++) {
      /* Noisy already goes by length along whatever shape it has, its jitter on top: left Noisy, it keeps both (as
       * Custom, measured with the jitter in, it would lose it). */
      if (ch->keys[i].interp != Interp::Noisy) {
        tg.speeds[i] = stretch_length_speeds(*ch, i);
      }
    }
    ids.push_back(layer.id);
    st->targets.push_back(std::move(tg));
  }
  if (st->targets.empty()) {
    return out;
  }
  st->drawing.frame_current = 0;
  SculptDab d = dab;
  /* The setup key (frame 0) and keys off the paths shown stay: the brush works on the rest. */
  d.selected_only = true;
  d.mirror_x = false;
  st->stroke = sculpt_stroke_begin(st->drawing, ids, d);
  out.state = std::move(st);
  return out;
}

const SculptStroke *motion_path_sculpt_line(const PathSculptStroke &stroke)
{
  return stroke.empty() ? nullptr : &stroke.state->stroke;
}

PathSculptStroke motion_path_surface_fit(const Document &doc, const std::vector<MotionPathTarget> &targets, int first,
                                         int last, const PathBrushSettings &settings)
{
  OANIM_PROFILE("motion_path_surface_fit");
  std::vector<MotionPathTarget> own;
  for (const MotionPathTarget &t : targets) {
    if (!t.drawing && motion_path_sculptable(doc, t)) {
      own.push_back(t);
    }
  }
  /* The fit doesn't go by the brush's size (only how finely the line is sampled for a dab does): a few samples. */
  return motion_path_sculpt_begin(doc, own, first, last,
                                  motion_path_sculpt_dab_make(PathBrush::SurfaceSmooth, settings, {0.0f, 0.0f}, 1e6f));
}

bool motion_path_stretch_reset(Document &doc, const MotionPathTarget &target, int frame, bool shape, bool timing)
{
  Channel<float2> *ch = target.drawing ? nullptr : handle_channel(doc, target);
  if (!ch) {
    /* A path with no place of its own to shape (a drawing's: its points' timing; a bone turning its tip): only its
     * timing. */
    return timing && motion_path_even_spacing(doc, target, frame);
  }
  std::vector<Key<float2>> &keys = ch->keys;
  /* The stretch: the key it arrives at is the first on or after `frame`, one after a key before it. */
  size_t i = 0;
  while (i < keys.size() && keys[i].frame < frame) {
    i++;
  }
  if (i == 0 || i >= keys.size()) {
    return false;
  }
  const int end = keys[i].frame;
  Key<float2> &a = keys[i - 1], &b = keys[i];
  if (shape) {
    /* A stretch's dots go by their share of its length whatever its shape, except Smooth's, timed by its curve's own
     * parameter: made Custom first with the speeds it goes along now, as the shape brushes do, so its timing stays. */
    if (!timing && b.interp == Interp::Smooth) {
      std::vector<float> speeds = stretch_length_speeds(*ch, i);
      if (!speeds.empty()) {
        key_speeds_set(keys, i, std::move(speeds));
      }
    }
    /* Every handle as it is now (Auto ones worked out from the keys around them), so the stretches beside keep
     * theirs. */
    float2 a_in, a_out, b_in, b_out;
    key_path_handles(keys, i - 1, a_in, a_out);
    key_path_handles(keys, i, b_in, b_out);
    const float2 way = b.value - a.value;
    const auto set_side = [](Key<float2> &k, float2 in, float2 out, bool out_side) {
      const float2 other = out_side ? in : out, mine = out_side ? out : in;
      if (k.path_handles != PathHandles::Broken) {
        /* In line still (or nothing on the other side to keep in line): Aligned; else a corner. */
        const bool lined_up = length(other) < 1e-9f || length(mine) < 1e-9f ||
                              (std::abs(other.x * mine.y - other.y * mine.x) <= 1e-4f * length(other) * length(mine) &&
                               dot(other, mine) < 0.0f);
        k.path_handles = lined_up ? PathHandles::Aligned : PathHandles::Broken;
      }
      k.path_in = in;
      k.path_out = out;
    };
    set_side(a, a_in, way / 3.0f, true);
    set_side(b, -way / 3.0f, b_out, false);
    b.path_controls.clear();
    b.path_control_handles.clear();
  }
  if (timing) {
    if (shape) {
      b.interp = Interp::Linear;
      b.timing = {};
    }
    else if (!motion_path_even_spacing(doc, target, end - 1)) {
      return false;
    }
  }
  channel_keys_changed();
  animation_evaluate(doc, doc.frame_current);
  return true;
}

int motion_path_sculpt_dab(Document &doc, PathSculptStroke &stroke, const SculptDab &dab)
{
  if (stroke.empty()) {
    return 0;
  }
  PathSculptState &st = *stroke.state;
  SculptDab d = dab;
  d.selected_only = true;
  d.mirror_x = false;
  /* Every path the stroke has changed so far is given back, whether or not this dab moved it (a Grab can start again
   * from the document as it was). */
  sculpt_stroke_dab(st.drawing, st.stroke, d);
  /* The setup key's point back where it was: the setup pose never moves, only how the path leaves it. */
  for (const PathSculptState::Target &tg : st.targets) {
    Layer *layer = layer_find(st.drawing, tg.layer);
    if (tg.pinned >= 0 && layer && !layer->drawing.curves.empty()) {
      MeshPoint &mp = layer->drawing.points[size_t(layer->drawing.curves.front().points[size_t(tg.pinned)].point)];
      mp.pos = mp.setup_pos = tg.pos[size_t(tg.pinned)];
    }
  }
  int changed = 0;
  for (const PathSculptState::Target &tg : st.targets) {
    const Layer *layer = layer_find(st.drawing, tg.layer);
    Channel<float2> *ch = handle_channel(doc, tg.target);
    if (!layer || !ch || layer->drawing.curves.empty() || ch->keys.size() != tg.keys.size()) {
      continue;
    }
    const Curve &curve = layer->drawing.curves.front();
    std::vector<Key<float2>> keys = tg.keys;
    const size_t n = keys.size();
    std::vector<bool> moved(n, false);
    std::vector<float2> in(n), out(n);
    std::vector<bool> has_in(n, false), has_out(n, false);
    std::vector<std::vector<float2>> controls(n);
    std::vector<std::vector<int>> control_points(n);
    /* Keys first (control points are kept relative to them). */
    for (size_t j = 0; j < curve.points.size() && j < tg.key.size(); j++) {
      const CurvePoint &cp = curve.points[j];
      const float2 at = layer->drawing.points[size_t(cp.point)].pos;
      const bool same = length(at - tg.pos[j]) < 1e-7f && length(cp.handle_in - tg.in[j]) < 1e-7f &&
                        length(cp.handle_out - tg.out[j]) < 1e-7f;
      const size_t k = size_t(tg.key[j]);
      if (tg.control[j] >= 0) {
        control_points[k].push_back(int(j));
        if (!same) {
          moved[k] = moved[k - 1] = true;
        }
        continue;
      }
      if (same) {
        continue;
      }
      const KeyMap &m = tg.maps[k];
      keys[k].value = tg.keys[k].value + map_unvector(m, at - tg.pos[j]);
      in[k] = map_unvector(m, cp.handle_in);
      out[k] = map_unvector(m, cp.handle_out);
      /* The line's ends have one handle on it; the other shapes a stretch off the path, below. */
      has_in[k] = k > tg.lo;
      has_out[k] = k < tg.hi;
      moved[k] = true;
    }
    if (std::none_of(moved.begin(), moved.end(), [](bool b) { return b; })) {
      continue;
    }
    /* Control points: where they went, kept relative to the keys as they are now. */
    for (size_t k = 1; k < n; k++) {
      if (!moved[k] && !moved[k - 1]) {
        continue;
      }
      for (size_t c = 0; c < control_points[k].size() && c < keys[k].path_controls.size(); c++) {
        const int j = control_points[k][c];
        const float2 at = layer->drawing.points[size_t(curve.points[size_t(j)].point)].pos;
        const float along = tg.keys[k].path_controls[c].x;
        const KeyMap m = map_blend(tg.maps[k - 1], tg.maps[k], along);
        const float2 was = path_control_value(tg.keys[k - 1].value, tg.keys[k].value, tg.keys[k].path_controls[c]);
        keys[k].path_controls[c] = path_control_stored(keys[k - 1].value, keys[k].value,
                                                       was + map_unvector(m, at - tg.pos[size_t(j)]));
        /* Its handles, once the brush has moved it or them: its own from then on, as the sculpted line has them. */
        const CurvePoint &cp = curve.points[size_t(j)];
        if (length(at - tg.pos[size_t(j)]) > 1e-7f || length(cp.handle_in - tg.in[size_t(j)]) > 1e-7f ||
            length(cp.handle_out - tg.out[size_t(j)]) > 1e-7f)
        {
          keys[k].path_control_handles.resize(keys[k].path_controls.size());
          PathControlHandles &h = keys[k].path_control_handles[c];
          h.in = path_control_offset_stored(keys[k - 1].value, keys[k].value, map_unvector(m, cp.handle_in));
          h.out = path_control_offset_stored(keys[k - 1].value, keys[k].value, map_unvector(m, cp.handle_out));
          h.set = true;
        }
      }
    }
    /* The keys' handles as the fitted curve has them. A stretch with control points runs through them on its own
     * line, its keys' handles a share of its first and last piece (stretch_control_line): given back that way. */
    for (size_t k = 0; k < n; k++) {
      if (!moved[k]) {
        continue;
      }
      const auto piece_share = [&](size_t arriving, bool first_piece) {
        std::vector<float2> p{keys[arriving - 1].value};
        for (const float2 c : keys[arriving].path_controls) {
          p.push_back(path_control_value(keys[arriving - 1].value, keys[arriving].value, c));
        }
        p.push_back(keys[arriving].value);
        if (p.size() <= 2) {
          return 1.0f;
        }
        float sum = 0.0f;
        for (size_t j = 1; j < p.size(); j++) {
          sum += length(p[j] - p[j - 1]);
        }
        const float piece = first_piece ? length(p[1] - p[0]) : length(p[p.size() - 1] - p[p.size() - 2]);
        return piece > 1e-9f ? sum / piece : 1.0f;
      };
      float2 key_in, key_out, unused;
      key_path_handles(tg.keys, k, key_in, key_out);
      key_in = has_in[k] ? in[k] * piece_share(k, false) : key_in;
      key_out = has_out[k] ? out[k] * piece_share(k + 1, true) : key_out;
      (void)unused;
      /* A key the line ends on whose handles go in line: the one off the path turns with the one the brush shaped,
       * keeping its length, as the other side of a smooth point. */
      if (keys[k].path_handles != PathHandles::Broken && has_in[k] != has_out[k]) {
        const float2 shaped = has_out[k] ? key_out : key_in;
        float2 &other = has_out[k] ? key_in : key_out;
        if (length(shaped) > 1e-9f && length(other) > 1e-9f) {
          other = normalize(shaped) * -length(other);
        }
      }
      keys[k].path_in = key_in;
      keys[k].path_out = key_out;
      if (keys[k].path_handles == PathHandles::Auto) {
        keys[k].path_handles = PathHandles::Aligned;
      }
    }
    /* Each stretch reshaped keeps its timing: the dots stay at the same share of the way along it. One off the path
     * only when its shape changed (its end key moved, or its handle turned with the other). */
    const auto reshaped = [&](size_t i) {
      if (i > tg.lo && i <= tg.hi) {
        return true;
      }
      float2 was_in, was_out, now_in, now_out, unused;
      key_path_handles(tg.keys, i - 1, unused, was_out);
      key_path_handles(keys, i - 1, unused, now_out);
      key_path_handles(tg.keys, i, was_in, unused);
      key_path_handles(keys, i, now_in, unused);
      return length(keys[i - 1].value - tg.keys[i - 1].value) > 1e-7f ||
             length(keys[i].value - tg.keys[i].value) > 1e-7f || length(now_out - was_out) > 1e-7f ||
             length(now_in - was_in) > 1e-7f;
    };
    for (size_t i = 1; i < n; i++) {
      if ((moved[i] || moved[i - 1]) && reshaped(i) && !tg.speeds[i].empty()) {
        keys[i].interp = Interp::Custom;
        /* A Custom stretch already goes by share of the way along: its own curve (points placed by hand) stays. */
        keys[i].timing = tg.keys[i].interp == Interp::Custom ? tg.keys[i].timing : timing_from_speeds(tg.speeds[i]);
      }
    }
    ch->keys = std::move(keys);
    changed++;
  }
  if (changed) {
    channel_keys_changed();
    animation_evaluate(doc, doc.frame_current);
  }
  return changed;
}

SculptDab motion_path_sculpt_dab_make(PathBrush brush, const PathBrushSettings &settings, float2 center, float radius)
{
  SculptDab d;
  d.brush = path_brush_sculpt(brush);
  d.center = center;
  d.radius = std::max(radius, 1e-6f);
  d.strength = settings.strength;
  d.falloff = settings.falloff;
  d.surface_sensitivity = settings.sensitivity;
  return d;
}

}  // namespace oanim
