#include "anim/motion_path.hh"

#include <algorithm>
#include <cmath>
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
struct OwnValues {
  float2 position, scale;
  float angle = 0.0f;
};

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

}  // namespace

bool motion_path_position(const Document &doc, const MotionPathTarget &target, float2 &r_at)
{
  return target_position(doc, target, r_at);
}

bool motion_path_position_at(const Document &doc, const MotionPathTarget &target, int frame, float2 &r_at)
{
  Document copy = doc;
  copy.frame_current = frame;
  animation_evaluate(copy, frame);
  return target_position(copy, target, r_at);
}

bool motion_path_place(Document &doc, const MotionPathTarget &target, float2 at)
{
  Bone *bone = nullptr;
  float2 *value = place_value(doc, target, &bone);
  if (bone && !target.head) {
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
  else {
    filter.kinds = KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM;
  }
  return filter;
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
  /* Where each in-between key should be: on a curve over time (a parabola, or a line through two) fitted to up to two
   * keys either side of it, all worked out before any moves. */
  std::vector<std::pair<int, float2>> moves;
  for (size_t i = 0; i < keys.size(); i++) {
    const int k = keys[i];
    if (k <= first || k >= last) {
      continue;
    }
    std::vector<std::pair<double, float2>> near;
    for (size_t j = (i >= 2 ? i - 2 : 0); j < std::min(keys.size(), i + 3); j++) {
      float2 at;
      if (j != i && motion_path_position_at(doc, target, keys[j], at)) {
        near.push_back({double(keys[j] - k), at});
      }
    }
    if (near.size() < 2) {
      continue;
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
    bool solved = true;
    for (int c = 0; c < terms && solved; c++) {
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
        solved = false;
        break;
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
    if (solved) {
      moves.push_back({k, float2{float(rx[0] / m[0][0]), float(ry[0] / m[0][0])}});
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
