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
#include "anim/sculpt.hh"
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

std::vector<MotionPathTarget> motion_path_timing_targets(const Document &doc, const std::vector<Layer *> &editing,
                                                         bool edit_layer)
{
  std::vector<MotionPathTarget> out = motion_path_targets(doc, editing, edit_layer);
  if (edit_layer) {
    /* The layer itself: no drawing's timing. */
    return out;
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

static void path_shape_fill(const Document &doc, MotionPath &p);

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
        case PathBrush::Pinch:
          /* Slower under the brush (its dots crowd together toward the middle); Ctrl, faster (they spread). */
          v *= dab.invert ? 1.0f + 0.5f * std::min(f * strength, 1.0f) : 1.0f - 0.5f * std::min(f * strength, 1.0f);
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
    const float progress = (want - a) / (z - a);
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
    const float progress = q;
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
  return with_path_channels(doc, t, [&](auto &ch) { f(ch); });
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
  /* A position key added between two: handles along the path's way there, as long as its speed there makes them (a
   * third of each stretch, as a Bezier's), so adding it bends nothing: Auto would take a slope from its new
   * neighbours and crease the path. */
  Channel<float2> *pos = handle_channel(doc, target);
  const Key<float2> *before = nullptr, *after = nullptr;
  float2 speed{0.0f, 0.0f};
  if (pos && !pos->key_at(frame)) {
    for (const Key<float2> &k : pos->keys) {
      if (k.frame < frame) {
        before = &k;
      }
      else if (!after) {
        after = &k;
      }
    }
    if (before && after) {
      speed = (pos->evaluate(frame + 1) - pos->evaluate(frame - 1)) * 0.5f;
    }
  }
  const int span_in = before ? frame - before->frame : 0, span_out = after ? after->frame - frame : 0;
  with_path_channel(doc, target, [&](auto &ch) {
    if (!ch.key_at(frame)) {
      ch.set_key(frame, ch.evaluate(frame));
      added = true;
    }
  });
  if (added && pos && span_in > 0 && span_out > 0) {
    if (Key<float2> *k = pos->key_at(frame)) {
      k->path_in = speed * (-float(span_in) / 3.0f);
      k->path_out = speed * (float(span_out) / 3.0f);
      k->path_handles = PathHandles::Aligned;
      channel_keys_changed();
    }
  }
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

std::vector<MotionPathControl> motion_path_controls(const Document &doc, const MotionPathTarget &target)
{
  std::vector<MotionPathControl> out;
  const Channel<float2> *ch = handle_channel(const_cast<Document &>(doc), target);
  if (!ch) {
    return out;
  }
  for (size_t i = 1; i < ch->keys.size(); i++) {
    const Key<float2> &a = ch->keys[i - 1], &b = ch->keys[i];
    for (size_t j = 0; j < b.path_controls.size(); j++) {
      MotionPathControl c;
      c.end = b.frame;
      c.index = int(j);
      const float2 v = path_control_value(a.value, b.value, b.path_controls[j]);
      c.near_frame = stretch_nearest_frame(*ch, i, v);
      float2 cx, cy, unused_x, unused_y;
      if (!value_jacobian(doc, target, c.near_frame, v, c.at, cx, cy) ||
          !value_jacobian(doc, target, c.near_frame, ch->evaluate(c.near_frame), c.curve, unused_x, unused_y))
      {
        continue;
      }
      /* The handles as the path runs along them, shown through how the channel shows there (as the point). */
      const StretchPoints s = stretch_points(ch->keys, i);
      c.in_tip = c.at + cx * s.in[j + 1].x + cy * s.in[j + 1].y;
      c.out_tip = c.at + cx * s.out[j + 1].x + cy * s.out[j + 1].y;
      const PathControlHandles h = path_control_handles_of(b, j);
      c.set = h.set;
      c.aligned = h.aligned;
      out.push_back(c);
    }
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
  Key<float2> &a = ch->keys[i - 1], &b = ch->keys[i];
  const float2 v = ch->evaluate(frame);
  /* In order along the stretch: before the first one whose nearest dot comes later. */
  size_t at = b.path_controls.size();
  for (size_t j = 0; j < b.path_controls.size(); j++) {
    if (stretch_nearest_frame(*ch, i, path_control_value(a.value, b.value, b.path_controls[j])) > frame) {
      at = j;
      break;
    }
  }
  b.path_controls.insert(b.path_controls.begin() + std::ptrdiff_t(at), path_control_stored(a.value, b.value, v));
  /* Its handles worked out smooth until moved; the others' stay with theirs. */
  b.path_control_handles.resize(b.path_controls.size() - 1);
  b.path_control_handles.insert(b.path_control_handles.begin() + std::ptrdiff_t(at), PathControlHandles{});
  channel_keys_changed();
  if (r_end) {
    *r_end = b.frame;
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
  if (!value_jacobian(doc, target, near_frame, v, p, cx, cy)) {
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
  if (!value_jacobian(doc, target, near_frame, v, p, cx, cy)) {
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

int motion_path_handles_transform(Document &doc, const std::vector<MotionPathHandlePick> &picks, char kind,
                                  float2 offset, float angle, float factor)
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
    for (const MotionPathControl &mc : motion_path_controls(doc, picks[i].target)) {
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
    const auto mc = std::find_if(controls.begin(), controls.end(), [&](const MotionPathControl &x) {
      return x.end == p.frame && x.index == p.control;
    });
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
float2 map_unvector(const KeyMap &m, float2 d)
{
  const float det = m.cx.x * m.cy.y - m.cy.x * m.cx.y;
  if (std::abs(det) < 1e-12f) {
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
  if (n < 1 || b.interp == Interp::Step || b.interp == Interp::Pose || b.interp == Interp::Cycle) {
    return {};
  }
  if (b.interp == Interp::Custom && !b.speeds.empty()) {
    /* Already by length along it. */
    return b.speeds;
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
      tg.maps.push_back(m);
    }
    if (!ok) {
      continue;
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
    for (size_t i = 0; i < n; i++) {
      const Key<float2> &k = tg.keys[i];
      if (i > 0) {
        const StretchPoints s = stretch_points(tg.keys, i);
        for (size_t c = 1; c + 1 < s.p.size(); c++) {
          const float along = tg.keys[i].path_controls[c - 1].x;
          const KeyMap m = map_blend(tg.maps[i - 1], tg.maps[i], along);
          add(int(i), int(c - 1), map_apply(m, s.p[c]), map_vector(m, s.in[c]), map_vector(m, s.out[c]),
              !shown(tg.keys[i - 1].frame) && !shown(k.frame), path_control_handles_of(k, c - 1).aligned);
        }
      }
      float2 in{0.0f, 0.0f}, out_h{0.0f, 0.0f};
      if (i > 0) {
        in = map_vector(tg.maps[i], stretch_points(tg.keys, i).in.back());
      }
      if (i + 1 < n) {
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
      tg.speeds[i] = stretch_length_speeds(*ch, i);
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
      has_in[k] = k > 0;
      has_out[k] = k + 1 < n;
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
      keys[k].path_in = key_in;
      keys[k].path_out = key_out;
      if (keys[k].path_handles == PathHandles::Auto) {
        keys[k].path_handles = PathHandles::Aligned;
      }
    }
    /* Each stretch reshaped keeps its timing: the dots stay at the same share of the way along it. */
    for (size_t i = 1; i < n; i++) {
      if ((moved[i] || moved[i - 1]) && !tg.speeds[i].empty()) {
        keys[i].interp = Interp::Custom;
        keys[i].speeds = tg.speeds[i];
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
