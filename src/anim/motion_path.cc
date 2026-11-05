#include "anim/motion_path.hh"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <string>

#include "anim/animation.hh"
#include "anim/controller.hh"
#include "assets/asset_library.hh"
#include "anim/deform.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/document.hh"
#include "data/vector.hh"

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
    at = t.head ? poses[size_t(i)].head : poses[size_t(i)].tail;
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

/* The targets of the dots picked on paths, set by the window (see motion_path_dots_picked). */
std::vector<MotionPathTarget> g_dots_picked;
bool g_path_tool = true;
/* While a MotionPathOnFrame has the document on another frame: the document as shown, what Own Motion holds. */
const Document *g_shown = nullptr;

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
  doc.path_targets.push_back(t);
  return doc.path_targets.back();
}

std::vector<MotionPathTarget> motion_path_targets(const Document &doc, const std::vector<Layer *> &editing)
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
    if (layer && std::find(layers.begin(), layers.end(), layer) == layers.end()) {
      layers.push_back(layer);
    }
  }
  size_t points = 0;
  for (const Layer *layer : layers) {
    bool any = false;
    if (layer->is_bone_layer()) {
      /* A bone traces its tip and its head, but a connected head only rides on its parent's tip. */
      for (const Bone &b : layer->bones) {
        if (b.selected) {
          add({layer->id, b.id, 0});
          if (!b.connected) {
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

std::vector<MotionPathTarget> motion_path_timing_targets(const Document &doc, const std::vector<Layer *> &editing)
{
  std::vector<MotionPathTarget> out = motion_path_targets(doc, editing);
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

void motion_path_dots_picked(const std::vector<MotionPathTarget> &targets)
{
  g_dots_picked = targets;
}

void motion_path_tool_on(bool on)
{
  g_path_tool = on;
}

bool motion_path_own_motion(const Document &doc, const MotionPathTarget &target)
{
  /* A drawing's path is its layer's origin, which its own (shape) keys never move; outside the tool nothing is being
   * edited. */
  if (!doc.path_own_motion || target.drawing || !g_path_tool) {
    return false;
  }
  if (std::any_of(g_dots_picked.begin(), g_dots_picked.end(), [&](const MotionPathTarget &t) { return t.same(target); }))
  {
    return true;
  }
  const Layer *layer = layer_find(doc, target.layer);
  if (!layer) {
    return false;
  }
  const auto picked = [](const auto &ch) {
    return std::any_of(ch.keys.begin(), ch.keys.end(), [](const auto &k) { return k.selected; });
  };
  if (target.bone) {
    const int i = bone_index(*layer, target.bone);
    if (i < 0) {
      return false;
    }
    const Bone &b = layer->bones[size_t(i)];
    return picked(b.position_keys) || picked(b.angle_keys) || picked(b.scale_keys);
  }
  if (target.point) {
    const int i = point_index(*layer, target.point);
    return i >= 0 && picked(layer->drawing.points[size_t(i)].pos_keys);
  }
  KeyFilter filter;
  filter.kinds = KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM;
  for (const ChannelRef &ref : layer_filtered_channels(*const_cast<Layer *>(layer), filter)) {
    if ((ref.scalar && picked(*ref.scalar)) || (ref.vector && picked(*ref.vector))) {
      return true;
    }
  }
  return false;
}

std::vector<MotionPath> motion_paths_make(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                          int first, int last)
{
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
  /* Own Motion: a path being edited is worked out on a copy held as the frame shown, only the target's own values
   * taken from each frame; where it really goes is its ghost. */
  std::vector<std::unique_ptr<Document>> held(paths.size());
  for (size_t i = 0; i < paths.size(); i++) {
    if (motion_path_own_motion(doc, paths[i].target)) {
      paths[i].own_motion = true;
      held[i] = std::make_unique<Document>(g_shown ? *g_shown : doc);
    }
  }
  /* Edits not yet keyed stay on the frame they were made on: each frame from its keys alone. */
  Document copy = doc;
  for (int f = first; f <= last; f++) {
    copy.frame_current = f;
    animation_evaluate(copy, f);
    ArmaturePoseCacheScope poses;
    const Transform2D into = relative ? relative_space(copy) : Transform2D();
    for (size_t i = 0; i < paths.size(); i++) {
      MotionPath &p = paths[i];
      std::vector<float2> &real = p.own_motion ? p.ghost : p.points;
      float2 at = {0.0f, 0.0f};
      if (!target_position(copy, p.target, at) && !real.empty()) {
        at = real.back();
      }
      real.push_back(relative ? back.apply(into.apply(at)) : at);
      if (p.own_motion) {
        /* Everything else held as shown, so drawn relative to anything it stays where it is. */
        OwnValues v;
        float2 own = at;
        if (own_values(copy, p.target, v)) {
          own_values_set(*held[i], p.target, v);
          if (!target_position(*held[i], p.target, own) && !p.points.empty()) {
            own = p.points.back();
          }
        }
        p.points.push_back(own);
      }
    }
  }
  for (MotionPath &p : paths) {
    /* Nothing else moves it: no ghost to tell apart. */
    bool apart = false;
    for (size_t k = 0; k < p.ghost.size() && k < p.points.size() && !apart; k++) {
      apart = length(p.ghost[k] - p.points[k]) > 1e-5f;
    }
    if (!apart) {
      p.ghost.clear();
    }
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

std::vector<uint64_t> motion_path_ghost_layers(const Document &doc, const std::vector<MotionPathTarget> &targets)
{
  std::vector<uint64_t> ids;
  const auto add = [&](const Layer &l) {
    if (l.has_drawing() && std::find(ids.begin(), ids.end(), l.id) == ids.end()) {
      ids.push_back(l.id);
    }
  };
  const std::function<void(const Layer &)> inside = [&](const Layer &group) {
    for (const Layer &child : group.children) {
      if (child.visible && child.render_visible) {
        add(child);
        inside(child);
      }
    }
  };
  for (const MotionPathTarget &t : targets) {
    if (const Layer *layer = layer_find(doc, t.layer)) {
      add(*layer);
      inside(*layer);
    }
  }
  return ids;
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
    return layer->name + (layer->has_drawing() ? " drawing" : " together");
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

/* --- Editing (the Motion Path tool) ----------------------------------------------------------------------------- */

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
 * a point, a head or an origin as a straight-line function of it), a few times over for anything that isn't quite. */
bool solve_position(Document &doc, const MotionPathTarget &t, float2 &value, float2 at)
{
  for (int iteration = 0; iteration < 4; iteration++) {
    float2 now;
    if (!target_position(doc, t, now)) {
      return false;
    }
    const float2 miss = at - now;
    if (length(miss) < 1e-6f) {
      return true;
    }
    const float h = 1e-2f;
    const float2 start = value;
    float2 dx, dy;
    value = start + float2{h, 0.0f};
    target_position(doc, t, dx);
    value = start + float2{0.0f, h};
    target_position(doc, t, dy);
    value = start;
    const float2 cx = (dx - now) / h, cy = (dy - now) / h;
    const float det = cx.x * cy.y - cx.y * cy.x;
    if (std::abs(det) < 1e-9f) {
      return false;
    }
    value = start + float2{(miss.x * cy.y - miss.y * cy.x) / det, (cx.x * miss.y - cx.y * miss.x) / det};
  }
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
void path_evaluate(Document &copy, const MotionPathTarget &t, int frame)
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
  if (g_shown) {
    copy = *g_shown;
  }
  if (got) {
    own_values_set(copy, t, v);
  }
}

/* Where `t` is on `frame` as its path is drawn (see path_evaluate); `doc` stays as it is. */
bool path_position(const Document &doc, const MotionPathTarget &t, int frame, float2 &r_at)
{
  Document copy = doc;
  path_evaluate(copy, t, frame);
  return target_position(copy, t, r_at);
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
  float2 dx, dy;
  bool ok = path_position(copy, t, frame, r_at);
  key->value = start + float2{h, 0.0f};
  ok = ok && path_position(copy, t, frame, dx);
  key->value = start + float2{0.0f, h};
  ok = ok && path_position(copy, t, frame, dy);
  r_cx = (dx - r_at) / h;
  r_cy = (dy - r_at) / h;
  return ok;
}

/* The target's own keyed frames (after frame 0, the setup). */
std::vector<int> own_key_frames(const Document &doc, const MotionPathTarget &t)
{
  std::vector<int> frames;
  if (Layer *layer = layer_find(const_cast<Document &>(doc), t.layer)) {
    for (const auto &[frame, summary] : layers_key_summary({layer}, motion_path_key_filter(doc, t))) {
      if (frame > 0) {
        frames.push_back(frame);
      }
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

/* A stretch's speeds as it moves now: one for each frame from `start`, the change since the frame before (along the
 * path for a position), as a share of the even speed. */
template<typename T> std::vector<float> stretch_speeds(const Channel<T> &ch, size_t i, int start, int n)
{
  const Key<T> &b = ch.keys[i];
  std::vector<float> s(size_t(n), 1.0f);
  if (b.interp == Interp::Custom && !b.speeds.empty()) {
    /* Painted for a stretch of another length: each frame takes the one it falls in. */
    for (int j = 0; j < n; j++) {
      s[size_t(j)] = b.speeds[std::min(b.speeds.size() - 1, size_t((float(j) + 0.5f) * float(b.speeds.size()) / float(n)))];
    }
  }
  else {
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
  /* A Custom stretch's speeds keep their signs (going back past a key); measured ones are how far it went. */
  const bool custom = b.interp == Interp::Custom && !b.speeds.empty();
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
    v = v < 0.0f ? v * float(n) / total : std::max(v * float(n) / total, 0.02f);
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
    if (n < 1 || b.interp == Interp::Step || b.interp == Interp::Pose || b.interp == Interp::Cycle) {
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
        default:
          break;
      }
      v = std::max(v, 0.02f);
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
          s[size_t(j)] = std::max(s[size_t(j)] * scale, 0.02f);
        }
      }
    }
    else {
      const float scale = float(n) / std::max(brushed + rest, 1e-9f);
      for (float &v : s) {
        v = std::max(v * scale, 0.02f);
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
    if (frame <= start || b.interp == Interp::Step || b.interp == Interp::Pose || b.interp == Interp::Cycle) {
      return false;
    }
    r_i = i;
    r_start = start;
    r_n = b.frame - start;
    return true;
  }
  return false;
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

/* --- Breakdowns (KeyType::Breakdown) in the timing tools ----------------------------------------------------------
 * While one is alive, `ch` has each span from main key to main key across breakdowns as one stretch, as it moves now
 * (its main key after Custom, its speeds how far it goes each frame), so a timing tool shapes the span as one. Gone,
 * the span's breakdowns come back, each on the frame where the new timing reaches its pose (nearest, kept in order and
 * off the keys either side), the stretches between them Custom to go the span's timing between their keys. A span
 * the tool left alone comes back as it was. */
template<typename T> struct SpanFlat {
  struct Span {
    /* Its main keys (indices in `original`), first frame (after the main key before's hold) and frames. */
    size_t m0 = 0, m1 = 0;
    int start = 0, n = 0;
    /* How far along the span each breakdown's pose is (0-1, by how far the value goes). */
    std::vector<float> at;
    /* What the flat stretch was given, and the index of its key there. */
    std::vector<float> speeds;
    size_t flat = 0;
  };
  Channel<T> &ch;
  std::vector<Key<T>> original;
  /* Each flat key's index in `original`. */
  std::vector<size_t> kept;
  std::vector<Span> spans;

  explicit SpanFlat(Channel<T> &c) : ch(c)
  {
    const std::vector<Key<T>> &keys = ch.keys;
    if (std::none_of(keys.begin(), keys.end(), [](const Key<T> &k) { return k.type == KeyType::Breakdown; })) {
      return;
    }
    for (size_t m0 = 0; m0 + 1 < keys.size();) {
      size_t m1 = m0 + 1;
      while (m1 + 1 < keys.size() && keys[m1].type == KeyType::Breakdown) {
        m1++;
      }
      if (m1 - m0 > 1) {
        Span sp;
        sp.m0 = m0;
        sp.m1 = m1;
        sp.start = std::min(keys[m0].frame + std::max(keys[m0].hold, 0), keys[m1].frame);
        sp.n = keys[m1].frame - sp.start;
        bool moves = sp.n >= 2;
        for (size_t j = m0 + 1; j <= m1 && moves; j++) {
          const Interp interp = keys[j].interp;
          moves = !(interp == Interp::Step || interp == Interp::Pose || interp == Interp::Cycle) &&
                  (j == m1 || keys[j].frame > sp.start);
        }
        if (moves) {
          /* How far it goes each frame (along the path for a position), and where each breakdown is on the way. */
          std::vector<float> d(size_t(sp.n), 0.0f);
          T before = ch.evaluate(sp.start);
          float total = 0.0f;
          for (int j = 0; j < sp.n; j++) {
            const T now = ch.evaluate(sp.start + j + 1);
            if constexpr (std::is_same_v<T, float>) {
              d[size_t(j)] = std::abs(now - before);
            }
            else {
              d[size_t(j)] = length(now - before);
            }
            total += d[size_t(j)];
            before = now;
          }
          if (total > 1e-9f) {
            float done = 0.0f;
            size_t next = m0 + 1;
            for (int j = 0; j < sp.n && next < m1; j++) {
              done += d[size_t(j)];
              if (sp.start + j + 1 == keys[next].frame) {
                sp.at.push_back(done / total);
                next++;
              }
            }
            for (float &v : d) {
              v = std::max(v * float(sp.n) / total, 0.02f);
            }
            sp.speeds = std::move(d);
            spans.push_back(std::move(sp));
          }
        }
      }
      m0 = m1;
    }
    if (spans.empty()) {
      return;
    }
    original = keys;
    std::vector<Key<T>> flat;
    size_t s = 0;
    for (size_t i = 0; i < original.size(); i++) {
      while (s < spans.size() && spans[s].m1 < i) {
        s++;
      }
      if (s < spans.size() && i > spans[s].m0 && i < spans[s].m1) {
        continue;
      }
      Key<T> k = original[i];
      if (s < spans.size() && i == spans[s].m1) {
        k.interp = Interp::Custom;
        k.speeds = spans[s].speeds;
        spans[s].flat = flat.size();
      }
      kept.push_back(i);
      flat.push_back(std::move(k));
    }
    ch.keys = std::move(flat);
  }

  SpanFlat(const SpanFlat &) = delete;
  SpanFlat &operator=(const SpanFlat &) = delete;

  /* The span whose flat stretch arrives at flat key `i`, or null. */
  const Span *span_at(size_t i) const
  {
    for (const Span &sp : spans) {
      if (sp.flat == i) {
        return &sp;
      }
    }
    return nullptr;
  }

  /* The breakdowns' frames of a span. */
  std::vector<int> breakdown_frames(const Span &sp) const
  {
    std::vector<int> out;
    for (size_t j = sp.m0 + 1; j < sp.m1; j++) {
      out.push_back(original[j].frame);
    }
    return out;
  }

  /* Progress `q` (0-1) along the stretch arriving at key `i` of the channel as it was, as progress along its span (as
   * the flat stretch has it); unchanged outside spans. */
  float span_progress(size_t i, float q) const
  {
    for (const Span &sp : spans) {
      if (i > sp.m0 && i <= sp.m1) {
        const float a = i - 1 == sp.m0 ? 0.0f : sp.at[i - 2 - sp.m0];
        const float b = i == sp.m1 ? 1.0f : sp.at[i - 1 - sp.m0];
        return a + (b - a) * q;
      }
    }
    return q;
  }

  ~SpanFlat()
  {
    if (spans.empty()) {
      return;
    }
    std::vector<Key<T>> flat = std::move(ch.keys);
    if (flat.size() != kept.size()) {
      /* Keys added or taken away: not a timing change; the breakdowns as they were. */
      ch.keys = std::move(original);
      return;
    }
    /* The keys as they were, with handles frozen where a stretch made Custom must keep its line. */
    std::vector<Key<T>> frozen = original;
    std::vector<Key<T>> out;
    bool changed = false;
    size_t s = 0;
    for (size_t fi = 0; fi < flat.size(); fi++) {
      const size_t oi = kept[fi];
      while (s < spans.size() && spans[s].m1 < oi) {
        s++;
      }
      const bool span_end = s < spans.size() && oi == spans[s].m1;
      const bool span_start = std::any_of(spans.begin(), spans.end(), [&](const Span &sp) { return sp.m0 == oi; });
      Key<T> k = flat[fi];
      if (span_start || span_end) {
        /* Its path handles are the span's stretches', not the flat one's. */
        k.path_handles = original[oi].path_handles;
        k.path_in = original[oi].path_in;
        k.path_out = original[oi].path_out;
      }
      if (!span_end) {
        out.push_back(std::move(k));
        continue;
      }
      const Span &sp = spans[s];
      if (k.interp != Interp::Custom || k.speeds == sp.speeds) {
        /* Left alone: as it was. */
        for (size_t j = sp.m0 + 1; j < sp.m1; j++) {
          out.push_back(original[j]);
        }
        Key<T> end = original[sp.m1];
        end.value = k.value;
        end.selected = k.selected;
        out.push_back(std::move(end));
        continue;
      }
      changed = true;
      /* The span's new progress, a value a frame (its speeds might be for another length). */
      std::vector<float> sv(size_t(sp.n), 1.0f);
      for (int j = 0; j < sp.n; j++) {
        sv[size_t(j)] =
            k.speeds[std::min(k.speeds.size() - 1, size_t((float(j) + 0.5f) * float(k.speeds.size()) / float(sp.n)))];
      }
      const std::vector<float> u = speeds_progress(sv);
      /* Each breakdown on the frame its pose is reached. */
      const size_t count = sp.m1 - sp.m0 - 1;
      std::vector<int> at_frame(count);
      int lo = std::max(original[sp.m0].frame - sp.start, 0) + 1;
      for (size_t b = 0; b < count; b++) {
        const int hi = sp.n - int(count - b);
        int best = std::clamp(lo, 1, std::max(hi, 1));
        for (int j = lo; j <= hi; j++) {
          if (std::abs(u[size_t(j)] - sp.at[b]) < std::abs(u[size_t(best)] - sp.at[b])) {
            best = j;
          }
        }
        at_frame[b] = best;
        lo = best + 1;
      }
      if constexpr (std::is_same_v<T, float2>) {
        /* Straight stretches (by the span's Linear or Ease, or their own) keep their line as Custom. */
        const Interp mode = original[sp.m1].interp;
        const bool span_timed = mode == Interp::Linear || mode == Interp::EaseIn || mode == Interp::EaseOut ||
                                mode == Interp::EaseInOut;
        for (size_t j = sp.m0 + 1; j <= sp.m1; j++) {
          if (span_timed || !interp_path_smooth(original[j].interp)) {
            key_path_handles_freeze(frozen, j - 1);
            key_path_handles_freeze(frozen, j);
          }
        }
        if (!out.empty()) {
          out.back().path_handles = frozen[sp.m0].path_handles;
          out.back().path_in = frozen[sp.m0].path_in;
          out.back().path_out = frozen[sp.m0].path_out;
        }
      }
      /* Each stretch going the part of the span's timing between its keys. */
      int from = 0;
      for (size_t b = 0; b <= count; b++) {
        const int to = b < count ? at_frame[b] : sp.n;
        const int seg_start = b == 0 ? 0 : std::min(from + std::max(original[sp.m0 + b].hold, 0), to);
        const int seg_n = to - seg_start;
        Key<T> key = b < count ? frozen[sp.m0 + 1 + b] : k;
        if (b == count) {
          key.path_handles = frozen[sp.m1].path_handles;
          key.path_in = frozen[sp.m1].path_in;
          key.path_out = frozen[sp.m1].path_out;
        }
        key.frame = sp.start + to;
        if (seg_n >= 1) {
          const float u0 = u[size_t(seg_start)], u1 = u[size_t(to)];
          std::vector<float> q(size_t(seg_n + 1));
          for (int j = 0; j <= seg_n; j++) {
            q[size_t(j)] = std::abs(u1 - u0) > 1e-9f ? (u[size_t(seg_start + j)] - u0) / (u1 - u0) :
                                                       float(j) / float(seg_n);
          }
          key.interp = Interp::Custom;
          key.speeds = progress_speeds(q);
        }
        out.push_back(std::move(key));
        from = to;
      }
    }
    ch.keys = std::move(out);
    if (changed) {
      channel_keys_changed();
    }
  }
};

/* Progress `u` (0 at the stretch's start to 1 at its key, a value a frame) put back in order around dot `k0`, which
 * stays: none passing another or a key, each frame at least `gap` on. */
void order_progress(std::vector<float> &u, int k0, float gap)
{
  const int n = int(u.size()) - 1;
  for (int k = k0 + 1; k < n; k++) {
    u[size_t(k)] = std::max(u[size_t(k)], u[size_t(k - 1)] + gap);
  }
  for (int k = k0 - 1; k >= 1; k--) {
    u[size_t(k)] = std::min(u[size_t(k)], u[size_t(k + 1)] - gap);
  }
  for (int k = n - 1; k > k0; k--) {
    u[size_t(k)] = std::min(u[size_t(k)], u[size_t(k + 1)] - gap);
  }
  for (int k = 1; k < k0; k++) {
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
  /* Each frame keeps at least the speed brushes' least. */
  const float gap = 0.02f / float(n);
  want = std::clamp(want, float(k0) * gap, 1.0f - float(n - k0) * gap);
  const float d = want - u[size_t(k0)];
  for (int k = 1; k < n; k++) {
    const float w = k == k0                ? 1.0f
                    : slide.proportional ? proportional_weight(slide.falloff, float(std::abs(k - k0)), slide.size, k)
                                         : 0.0f;
    u[size_t(k)] += d * w;
  }
  u[size_t(k0)] = want;
  order_progress(u, k0, gap);
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
    if (n < 1 || interp == Interp::Step || interp == Interp::Pose || interp == Interp::Cycle) {
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
 * without a key): the channel is held at `value` on a copy and nudged. */
bool value_jacobian(const Document &doc, const MotionPathTarget &t, int frame, float2 value, float2 &r_at, float2 &r_cx,
                    float2 &r_cy)
{
  Document copy = doc;
  Channel<float2> *ch = handle_channel(copy, t);
  if (!ch || ch->keys.empty()) {
    return false;
  }
  Key<float2> held = ch->keys.front();
  held.frame = frame;
  held.interp = Interp::Linear;
  held.speeds.clear();
  held.path_handles = PathHandles::Auto;
  ch->keys = {held};
  copy.frame_current = frame;
  const float h = 1e-2f;
  float2 dx, dy;
  ch->keys[0].value = value;
  bool ok = path_position(copy, t, frame, r_at);
  ch->keys[0].value = value + float2{h, 0.0f};
  ok = ok && path_position(copy, t, frame, dx);
  ch->keys[0].value = value + float2{0.0f, h};
  ok = ok && path_position(copy, t, frame, dy);
  r_cx = (dx - r_at) / h;
  r_cy = (dy - r_at) / h;
  return ok;
}

/* The progress (0-1 by length) along the stretch arriving at keys[i] nearest `v` (the keys' own space). */
float nearest_progress(const std::vector<Key<float2>> &keys, size_t i, float2 v)
{
  constexpr int SAMPLES = 128;
  float best = 0.0f, best_d = 1e30f;
  for (int j = 0; j <= SAMPLES; j++) {
    const float u = float(j) / SAMPLES;
    const float2 q = stretch_path_point(keys, i, u) - v;
    const float d = q.x * q.x + q.y * q.y;
    if (d < best_d) {
      best_d = d;
      best = u;
    }
  }
  /* Narrowed down between the samples either side. */
  float lo = std::max(0.0f, best - 1.0f / SAMPLES), hi = std::min(1.0f, best + 1.0f / SAMPLES);
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

const char *path_brush_id(PathBrush brush)
{
  static const char *const IDS[PATH_BRUSH_COUNT] = {"EDIT", "GRAB", "SMOOTH", "BLEND_SPEED"};
  return IDS[int(brush)];
}

const char *path_brush_name(PathBrush brush)
{
  static const char *const NAMES[PATH_BRUSH_COUNT] = {"Edit", "Grab", "Smooth", "Blend Speed"};
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
          SpanFlat flat(b.angle_keys);
          changed += paint_speeds(b.angle_keys, falloff, brush, dab);
        }
      }
    }
    else if (Channel<float2> *ch = handle_channel(doc, p.target)) {
      SpanFlat flat(*ch);
      changed += paint_speeds(*ch, falloff, brush, dab);
    }
  }
  animation_evaluate(doc, doc.frame_current);
  return changed;
}

bool motion_path_slide(Document &doc, const MotionPathTarget &target, int frame, float2 at, const PathSlide &slide)
{
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
    if (!stretch_of(ch, frame, i, start, n)) {
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
    /* Across breakdowns: the span's stretch, the progress as the span's. */
    SpanFlat flat(ch);
    const float progress = flat.span_progress(i, (want - a) / (z - a));
    if (!stretch_of(ch, frame, i, start, n)) {
      return false;
    }
    const std::vector<float> s = stretch_make_custom(ch, i, start, n);
    key_speeds_set(ch.keys, i, slid_speeds(s, frame - start, progress, slide));
  }
  else {
    Channel<float2> *ch = handle_channel(doc, target);
    if (!ch || !stretch_of(*ch, frame, i, start, n)) {
      return false;
    }
    /* The mouse in the keys' own space, by how the target moves with the channel's value there. */
    const float2 value = ch->evaluate(frame);
    float2 shown, cx, cy;
    if (!value_jacobian(doc, target, frame, value, shown, cx, cy)) {
      return false;
    }
    const float det = cx.x * cy.y - cx.y * cy.x;
    if (std::abs(det) < 1e-12f) {
      return false;
    }
    const float2 miss = at - shown;
    const float2 v = value + float2{(miss.x * cy.y - miss.y * cy.x) / det, (cx.x * miss.y - cx.y * miss.x) / det};
    const float q = nearest_progress(ch->keys, i, v);
    /* Across breakdowns: the span's stretch, the progress as the span's. */
    SpanFlat flat(*ch);
    const float progress = flat.span_progress(i, q);
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
    SpanFlat flat(ch);
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

/* Calls `f` with the channel that moves the target's dot: a tip's bone angle, else its position. */
template<typename F> static bool with_path_channel(Document &doc, const MotionPathTarget &t, F &&f)
{
  Layer *layer = layer_find(doc, t.layer);
  if (!layer) {
    return false;
  }
  if (t.drawing) {
    /* The drawing: its first animated channel stands for all (they share their keys' frames and timing). */
    bool found = false;
    drawing_channels(const_cast<Document &>(doc), *layer, [&](auto &ch) {
      f(ch);
      found = true;
      return false;
    });
    return found;
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

int motion_path_key_delete(Document &doc, const MotionPathTarget &target, int frame)
{
  if (frame == 0) {
    return 0;
  }
  std::vector<std::function<void()>> removals;
  const int count = with_diamond_channels(doc, target, frame, [&](auto &ch) {
    removals.push_back([&ch, frame] { ch.remove_key(frame); });
  });
  for (const auto &remove : removals) {
    remove();
  }
  return count;
}

std::vector<std::pair<MotionPathTarget, int>> motion_path_selected_keys(const Document &doc,
                                                                        const std::vector<MotionPathTarget> &targets)
{
  std::vector<std::pair<MotionPathTarget, int>> out;
  for (const MotionPathTarget &t : targets) {
    with_path_channel(const_cast<Document &>(doc), t, [&](auto &ch) {
      for (const auto &k : ch.keys) {
        if (k.selected && k.frame > 0) {
          out.push_back({t, k.frame});
        }
      }
    });
  }
  return out;
}

bool motion_path_is_key(const Document &doc, const MotionPathTarget &target, int frame)
{
  bool found = false;
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) { found = ch.key_at(frame) != nullptr; });
  return found;
}

bool motion_path_key_selected(const Document &doc, const MotionPathTarget &target, int frame)
{
  bool selected = false;
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) {
    const auto *k = ch.key_at(frame);
    selected = k && k->selected;
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
  const std::set<int> frames = layers_filtered_key_frames({layer}, motion_path_key_filter(doc, target));
  const auto it = frames.lower_bound(std::max(frame, 1));
  if (it == frames.end() || it == frames.begin()) {
    return false;
  }
  r_end = *it;
  r_start = *std::prev(it);
  return true;
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
  with_path_channel(doc, target, [&](auto &ch) {
    if (auto *k = ch.key_at(frame)) {
      k->selected = select;
    }
  });
}

int motion_path_keys_transform(Document &doc, const std::vector<std::pair<MotionPathTarget, int>> &keys, char kind,
                               float2 offset, float angle, float factor)
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
  centre = centre / float(keys.size());
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
      if (!k.selected || k.frame <= 0) {
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
  return with_path_channels(doc, t, [&](auto &ch) {
    SpanFlat flat(ch);
    f(ch);
  });
}

template<typename F> static bool with_path_channels(Document &doc, const MotionPathTarget &t, F &&f)
{
  if (!t.drawing) {
    return with_path_channel(doc, t, f);
  }
  Layer *layer = layer_find(doc, t.layer);
  bool found = false;
  using Timing = std::tuple<int, Interp, std::vector<float>>;
  std::vector<Timing> changed;
  std::vector<int> first_before, first_after;
  if (layer) {
    drawing_channels(const_cast<Document &>(doc), *layer, [&](auto &ch) {
      if (!found) {
        std::vector<Timing> before;
        for (const auto &k : ch.keys) {
          before.push_back({k.frame, k.interp, k.speeds});
          first_before.push_back(k.frame);
        }
        f(ch);
        found = true;
        for (const auto &k : ch.keys) {
          Timing now{k.frame, k.interp, k.speeds};
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
      /* Breakdowns moved by the tool land where the first channel's did, so the drawing's keys stay together. */
      if (frames_before == first_before && ch.keys.size() == first_after.size()) {
        for (size_t j = 0; j < ch.keys.size(); j++) {
          ch.keys[j].frame = first_after[j];
        }
      }
      for (const auto &[frame, interp, speeds] : changed) {
        if (auto *k = ch.key_at(frame); k && k->interp == interp && k->speeds.size() == speeds.size()) {
          k->speeds = speeds;
        }
      }
      return true;
    });
  }
  return found;
}

std::vector<MotionPathStretch> motion_path_stretches(const Document &doc, const MotionPathTarget &target)
{
  std::vector<MotionPathStretch> out;
  with_path_channel(const_cast<Document &>(doc), target, [&](auto &ch) {
    SpanFlat flat(ch);
    for (size_t i = 1; i < ch.keys.size(); i++) {
      const auto &a = ch.keys[i - 1], &b = ch.keys[i];
      const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
      const int n = b.frame - start;
      if (n < 1 || b.interp == Interp::Step || b.interp == Interp::Pose || b.interp == Interp::Cycle) {
        continue;
      }
      MotionPathStretch st;
      st.start = start;
      st.end = b.frame;
      st.progress = speeds_progress(stretch_speeds(ch, i, start, n));
      if (const auto *sp = flat.span_at(i)) {
        st.breakdowns = flat.breakdown_frames(*sp);
      }
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
    int start = 0, n = 0;
    if (!stretch_of(ch, frame, i, start, n)) {
      return;
    }
    const std::vector<float> s = stretch_make_custom(ch, i, start, n);
    key_speeds_set(ch.keys, i, slid_speeds(s, frame - start, progress, slide));
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
      if (n < 2 || interp == Interp::Step || interp == Interp::Pose || interp == Interp::Cycle) {
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

bool motion_path_key_add(Document &doc, const MotionPathTarget &target, int frame)
{
  if (frame < 1) {
    return false;
  }
  bool added = false;
  with_path_channel(doc, target, [&](auto &ch) {
    if (!ch.key_at(frame)) {
      ch.set_key(frame, ch.evaluate(frame));
      added = true;
    }
  });
  animation_evaluate(doc, doc.frame_current);
  return added;
}

int motion_path_key_type_set(Document &doc, const MotionPathTarget &target, int frame, KeyType type)
{
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
  for (const ChannelRef &ref : layer_filtered_channels(*layer, motion_path_key_filter(doc, target))) {
    set(ref.scalar);
    set(ref.vector);
  }
  if (count > 0) {
    channel_keys_changed();
  }
  return count;
}

int motion_path_keys_delete(Document &doc, const std::vector<MotionPathTarget> &targets)
{
  int deleted = 0;
  for (const MotionPathTarget &t : targets) {
    with_path_channel(doc, t, [&](auto &ch) {
      std::vector<int> frames;
      for (const auto &k : ch.keys) {
        if (k.selected && k.frame > 0) {
          frames.push_back(k.frame);
        }
      }
      for (const int f : frames) {
        deleted += ch.remove_key(f) ? 1 : 0;
      }
    });
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

}  // namespace oanim
