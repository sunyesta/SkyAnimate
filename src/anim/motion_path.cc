#include "anim/motion_path.hh"

#include <algorithm>
#include <functional>
#include <set>
#include <string>

#include "anim/animation.hh"
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
  return true;
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
      for (const Bone &b : layer->bones) {
        if (b.selected) {
          add({layer->id, b.id, 0});
          any = true;
        }
      }
    }
    else if (layer->has_drawing()) {
      for (const MeshPoint &p : layer->drawing.points) {
        if (p.selected && p.id && points < MAX_POINTS) {
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

std::vector<int> motion_path_keys(const Document &doc, const MotionPathTarget &target)
{
  /* Everything that moves it: its own layer's keys (a bone's moved by its knob's actions, too) and those of the
   * layers above. Frame 0 is the setup, not a key of the animation. */
  std::set<int> keys;
  for (const Layer *l = layer_find(doc, target.layer); l; l = layer_parent(const_cast<Document &>(doc), l->id)) {
    for (const int f : layer_key_frames(*l)) {
      if (f > 0) {
        keys.insert(f);
      }
    }
  }
  return {keys.begin(), keys.end()};
}

bool motion_path_range(const Document &doc, const std::vector<MotionPathTarget> &targets, int &r_first, int &r_last)
{
  if (targets.empty()) {
    return false;
  }
  const int start = std::max(1, doc.frame_start), end = std::max(start, doc.frame_end);
  r_first = start;
  r_last = end;
  if (doc.path_all) {
    return true;
  }
  std::set<int> keys;
  for (const MotionPathTarget &t : targets) {
    for (const int f : motion_path_keys(doc, t)) {
      keys.insert(f);
    }
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
  /* Edits not yet keyed stay on the frame they were made on: each frame from its keys alone. */
  Document copy = doc;
  for (int f = first; f <= last; f++) {
    copy.frame_current = f;
    animation_evaluate(copy, f);
    ArmaturePoseCacheScope poses;
    const Transform2D into = relative ? relative_space(copy) : Transform2D();
    for (MotionPath &p : paths) {
      float2 at = {0.0f, 0.0f};
      if (!target_position(copy, p.target, at) && !p.points.empty()) {
        at = p.points.back();
      }
      p.points.push_back(relative ? back.apply(into.apply(at)) : at);
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
  /* Away from the speed colours' red and blue. */
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
  return layer->name;
}

Color motion_path_speed_color(float speed)
{
  const float t = std::clamp(speed, 0.0f, 1.0f);
  const Color still = {0.25f, 0.5f, 1.0f, 1.0f}, fast = {1.0f, 0.22f, 0.18f, 1.0f};
  return {still.r + (fast.r - still.r) * t, still.g + (fast.g - still.g) * t, still.b + (fast.b - still.b) * t, 1.0f};
}

}  // namespace oanim
