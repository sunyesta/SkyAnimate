#include "data/bones.hh"

#include <algorithm>
#include <map>

#include "data/document.hh"

namespace sky {

/* The setup value is the one at frame 0: the frame-0 key when animated, else the value (or,
 * away from frame 0, the value from before an edit not yet keyed). */
template<typename T> static T setup_value(const Channel<T> &ch, const T &value, bool setup_frame)
{
  if (ch.animated()) {
    return ch.evaluate(0);
  }
  return !setup_frame && ch.has_last ? ch.last : value;
}

static Transform2D bone_local(float2 position, float angle, float scale)
{
  return Transform2D::translate(position) * Transform2D::rotate(angle) * Transform2D::scaling({scale, scale});
}

static std::vector<BonePose> bones_pose(const std::vector<Bone> &bones, bool setup_frame);

std::vector<BonePose> armature_pose(const Layer &armature, bool setup_frame)
{
  return bones_pose(armature.bones, setup_frame);
}

static std::vector<BonePose> bones_pose(const std::vector<Bone> &bones, bool setup_frame)
{
  struct {
    const std::vector<Bone> &bones;
  } armature{bones};
  std::vector<BonePose> poses(armature.bones.size());
  std::map<uint64_t, int> index;
  for (int i = 0; i < int(armature.bones.size()); i++) {
    index[armature.bones[i].id] = i;
  }
  std::vector<bool> done(armature.bones.size(), false);
  /* Parents first, whatever order the bones are stored in (a cycle stops at its root). */
  const auto solve = [&](auto &&self, int i, int depth) -> void {
    if (done[i]) {
      return;
    }
    const Bone &bone = armature.bones[i];
    Transform2D parent_setup, parent_pose;
    if (auto it = index.find(bone.parent); it != index.end() && it->second != i && depth < 256) {
      self(self, it->second, depth + 1);
      parent_setup = poses[it->second].setup;
      parent_pose = poses[it->second].pose;
    }
    BonePose &p = poses[i];
    p.id = bone.id;
    p.pose = parent_pose * bone_local(bone.position, bone.angle, bone.scale);
    /* On frame 0 the bones are the setup pose, even mid-drag before the edit is recorded:
     * moving one there rigs, it doesn't move the artwork. */
    p.setup = setup_frame ? p.pose
                          : parent_setup * bone_local(setup_value(bone.position_keys, bone.position, false),
                                                      setup_value(bone.angle_keys, bone.angle, false),
                                                      setup_value(bone.scale_keys, bone.scale, false));
    p.skin = p.pose * p.setup.inverted();
    p.setup_head = p.setup.apply({0, 0});
    p.setup_tail = p.setup.apply({bone.length, 0});
    p.head = p.pose.apply({0, 0});
    p.tail = p.pose.apply({bone.length, 0});
    done[i] = true;
  };
  for (int i = 0; i < int(armature.bones.size()); i++) {
    solve(solve, i, 0);
  }
  return poses;
}

const BonePose *bone_pose_find(const std::vector<BonePose> &poses, uint64_t id)
{
  for (const BonePose &p : poses) {
    if (p.id == id) {
      return &p;
    }
  }
  return nullptr;
}

Bone *bone_find(Layer &armature, uint64_t id)
{
  for (Bone &b : armature.bones) {
    if (b.id == id && id != 0) {
      return &b;
    }
  }
  return nullptr;
}

const Bone *bone_find(const Layer &armature, uint64_t id)
{
  return bone_find(const_cast<Layer &>(armature), id);
}

const Bone *bone_find_by_name(const Layer &armature, const std::string &name)
{
  for (const Bone &b : armature.bones) {
    if (b.name == name) {
      return &b;
    }
  }
  return nullptr;
}

bool bone_is_descendant(const Layer &armature, uint64_t id, uint64_t ancestor)
{
  for (int depth = 0; id != 0 && depth < 256; depth++) {
    if (id == ancestor) {
      return true;
    }
    const Bone *b = bone_find(armature, id);
    id = b ? b->parent : 0;
  }
  return false;
}

void bone_select(Bone &bone, bool select)
{
  bone.selected = bone.select_head = bone.select_tail = select;
}

void armature_deselect_all(Layer &armature)
{
  for (Bone &b : armature.bones) {
    bone_select(b, false);
  }
}

/* The joint flags alone, without the body. */
static bool head_joint(const Layer &armature, const Bone &bone)
{
  if (bone.select_head) {
    return true;
  }
  const Bone *parent = bone.connected ? bone_find(armature, bone.parent) : nullptr;
  return parent && parent->select_tail;
}

static bool tail_joint(const Layer &armature, const Bone &bone)
{
  if (bone.select_tail) {
    return true;
  }
  for (const Bone &child : armature.bones) {
    if (child.connected && child.parent == bone.id && child.select_head) {
      return true;
    }
  }
  return false;
}

bool bone_head_selected(const Layer &armature, const Bone &bone)
{
  const Bone *parent = bone.connected ? bone_find(armature, bone.parent) : nullptr;
  return bone.selected || head_joint(armature, bone) || (parent && parent->selected);
}

bool bone_tail_selected(const Layer &armature, const Bone &bone)
{
  if (bone.selected || tail_joint(armature, bone)) {
    return true;
  }
  for (const Bone &child : armature.bones) {
    if (child.connected && child.parent == bone.id && child.selected) {
      return true;
    }
  }
  return false;
}

void armature_flush_selection(Layer &armature)
{
  std::vector<bool> both(armature.bones.size());
  for (size_t i = 0; i < armature.bones.size(); i++) {
    both[i] = head_joint(armature, armature.bones[i]) && tail_joint(armature, armature.bones[i]);
  }
  for (size_t i = 0; i < armature.bones.size(); i++) {
    armature.bones[i].selected = both[i];
  }
}

/* Parents before children (a cycle is cut where it closes). */
static std::vector<int> bones_parents_first(const Layer &armature)
{
  std::vector<int> order;
  std::vector<int> state(armature.bones.size(), 0);
  std::map<uint64_t, int> index;
  for (int i = 0; i < int(armature.bones.size()); i++) {
    index[armature.bones[i].id] = i;
  }
  const auto visit = [&](auto &&self, int i) -> void {
    if (state[i] != 0) {
      return;
    }
    state[i] = 1;
    if (auto it = index.find(armature.bones[i].parent); it != index.end()) {
      self(self, it->second);
    }
    state[i] = 2;
    order.push_back(i);
  };
  for (int i = 0; i < int(armature.bones.size()); i++) {
    visit(visit, i);
  }
  return order;
}

void armature_set_joints(Layer &armature, const std::map<uint64_t, std::pair<float2, float2>> &targets)
{
  const std::vector<BonePose> poses = armature_pose(armature);
  const size_t n = armature.bones.size();
  std::map<uint64_t, int> index;
  for (int i = 0; i < int(n); i++) {
    index[armature.bones[i].id] = i;
  }
  std::vector<float2> head(n), tail(n);
  std::vector<bool> changed(n, false);
  const std::vector<int> order = bones_parents_first(armature);
  for (const int i : order) {
    const Bone &b = armature.bones[i];
    const auto parent = index.find(b.parent);
    const bool has_parent = parent != index.end() && parent->second != i;
    head[i] = poses[i].head;
    tail[i] = poses[i].tail;
    if (auto it = targets.find(b.id); it != targets.end()) {
      head[i] = it->second.first;
      tail[i] = it->second.second;
      changed[i] = true;
    }
    if (b.connected && has_parent) {
      head[i] = tail[parent->second];
      changed[i] = changed[i] || distance(head[i], poses[i].head) > 1e-9f;
    }
  }
  /* Rebuild parents first: a bone below a changed one is measured again from its parent's new
   * frame, so it stays where it was. */
  std::vector<Transform2D> frame(n);
  std::vector<bool> redo(n, false);
  for (const int i : order) {
    Bone &b = armature.bones[i];
    const auto parent = index.find(b.parent);
    const bool has_parent = parent != index.end() && parent->second != i;
    const Transform2D parent_frame = has_parent ? frame[parent->second] : Transform2D();
    redo[i] = changed[i] || (has_parent && redo[parent->second]);
    if (redo[i]) {
      const Transform2D to_parent = parent_frame.inverted();
      const float2 h = to_parent.apply(head[i]), t = to_parent.apply(tail[i]);
      b.position = h;
      if (distance(h, t) > 1e-7f) {
        /* Whole turns are kept: the nearest angle to the old one, so keys don't jump a turn. */
        float angle = std::atan2(t.y - h.y, t.x - h.x);
        angle += 2.0f * PI * std::round((b.angle - angle) / (2.0f * PI));
        b.angle = angle;
      }
      b.length = distance(h, t) / std::max(std::abs(b.scale), 1e-6f);
    }
    frame[i] = parent_frame * bone_local(b.position, b.angle, b.scale);
  }
}

/* Whether the bone is picked for G as a whole (its body or its base), or by its tip alone. A
 * connected bone's base is its parent's tip, so picking it aims the parent instead. */
static bool grabs_whole(const Bone &b)
{
  return (b.selected || b.select_head) && !b.connected;
}

static bool grabs_tip(const Layer &armature, const Bone &b, bool setup_frame)
{
  if (grabs_whole(b)) {
    return false;
  }
  if (b.select_tail) {
    return true;
  }
  /* Posing, a connected bone can't leave its parent's tip: G turns it to follow the mouse instead.
   * On the setup frame, grabbing it drags the parent's tip. */
  if (!setup_frame) {
    return b.connected && (b.selected || b.select_head);
  }
  for (const Bone &child : armature.bones) {
    if (child.connected && child.parent == b.id && (child.selected || child.select_head)) {
      return true;
    }
  }
  return false;
}

static bool bone_picked(const Bone &b)
{
  return b.selected || b.select_head || b.select_tail;
}

/* True when an ancestor of the bone passes the test. */
template<typename F> static bool ancestor_is(const Layer &armature, const Bone &bone, F &&test)
{
  const Bone *p = bone_find(armature, bone.parent);
  for (int depth = 0; p && depth < 256; depth++) {
    if (test(*p)) {
      return true;
    }
    p = bone_find(armature, p->parent);
  }
  return false;
}

/* A connected child sits on its parent's tip, wherever that is now. */
static void seat_connected_children(Layer &armature)
{
  for (Bone &b : armature.bones) {
    if (const Bone *parent = b.connected ? bone_find(armature, b.parent) : nullptr) {
      b.position = {parent->length, 0.0f};
    }
  }
}

bool armature_clear_pose(Layer &armature, bool position, bool angle, bool scale)
{
  bool any = false;
  for (Bone &b : armature.bones) {
    if (!bone_picked(b)) {
      continue;
    }
    if (position) {
      b.position = setup_value(b.position_keys, b.position, false);
    }
    if (angle) {
      b.angle = setup_value(b.angle_keys, b.angle, false);
    }
    if (scale) {
      b.scale = setup_value(b.scale_keys, b.scale, false);
    }
    any = true;
  }
  return any;
}

bool armature_any_picked(const Layer &armature)
{
  return std::any_of(armature.bones.begin(), armature.bones.end(), bone_picked);
}

std::vector<float2> armature_picked_heads(const Layer &armature, bool setup_frame)
{
  const std::vector<BonePose> poses = armature_pose(armature, setup_frame);
  std::vector<float2> heads;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    if (bone_picked(armature.bones[i])) {
      heads.push_back(poses[i].head);
    }
  }
  return heads;
}

void armature_grab_selected(Layer &armature, const std::vector<Bone> &original, float2 delta, bool setup_frame)
{
  armature.bones = original;
  const std::vector<BonePose> poses = armature_pose(armature, setup_frame);
  const auto moves = [&](const Bone &b) { return grabs_whole(b) || grabs_tip(armature, b, setup_frame); };
  std::vector<std::pair<size_t, bool>> todo;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const Bone &b = armature.bones[i];
    /* Children come along with a parent that moves or turns, as in Moho. */
    if (moves(b) && !ancestor_is(armature, b, moves)) {
      todo.push_back({i, grabs_whole(b)});
    }
  }
  for (const auto &[i, whole] : todo) {
    Bone &b = armature.bones[i];
    const BonePose *parent = bone_pose_find(poses, b.parent);
    const Transform2D to_parent = parent ? parent->pose.inverted() : Transform2D();
    if (whole) {
      b.position += to_parent.apply_vector(delta);
      continue;
    }
    /* The tip follows the mouse: the bone turns about its base, and on the setup frame it also
     * grows or shrinks (length isn't animated, so elsewhere it only turns). */
    const float2 tip = to_parent.apply(poses[i].tail + delta);
    const float2 d = tip - b.position;
    if (length(d) > 1e-7f) {
      float angle = std::atan2(d.y, d.x);
      angle += 2.0f * PI * std::round((b.angle - angle) / (2.0f * PI));
      b.angle = angle;
    }
    if (setup_frame) {
      b.length = length(d) / std::max(std::abs(b.scale), 1e-6f);
    }
  }
  if (setup_frame) {
    seat_connected_children(armature);
  }
}

void armature_rotate_selected(Layer &armature, const std::vector<Bone> &original, float angle)
{
  armature.bones = original;
  /* A picked group turns as one: a child already turns with a picked parent, so it isn't turned
   * again. */
  std::vector<bool> turns(armature.bones.size());
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const Bone &b = armature.bones[i];
    turns[i] = bone_picked(b) && !ancestor_is(armature, b, bone_picked);
  }
  for (size_t i = 0; i < armature.bones.size(); i++) {
    if (turns[i]) {
      armature.bones[i].angle += angle;
    }
  }
}

void armature_scale_selected(Layer &armature, const std::vector<Bone> &original, float factor)
{
  armature.bones = original;
  /* A child already grows with a picked parent; scaling it too would grow it twice over. */
  std::vector<bool> scales(armature.bones.size());
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const Bone &b = armature.bones[i];
    scales[i] = bone_picked(b) && !ancestor_is(armature, b, bone_picked);
  }
  for (size_t i = 0; i < armature.bones.size(); i++) {
    if (scales[i]) {
      armature.bones[i].scale *= factor;
    }
  }
}

bool armature_picked_bounds(const Layer &armature, bool setup_frame, float2 &lo, float2 &hi)
{
  const std::vector<BonePose> poses = armature_pose(armature, setup_frame);
  bool any = false;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    if (!bone_picked(armature.bones[i])) {
      continue;
    }
    for (const float2 p : {poses[i].head, poses[i].tail}) {
      lo = any ? float2(std::min(lo.x, p.x), std::min(lo.y, p.y)) : p;
      hi = any ? float2(std::max(hi.x, p.x), std::max(hi.y, p.y)) : p;
      any = true;
    }
  }
  if (any) {
    /* A flat selection still gets a box with some size, as in the Moho script. */
    const float min_size = 0.01f;
    const float2 c = (lo + hi) * 0.5f;
    if (hi.x - lo.x < min_size) {
      lo.x = c.x - min_size * 0.5f;
      hi.x = c.x + min_size * 0.5f;
    }
    if (hi.y - lo.y < min_size) {
      lo.y = c.y - min_size * 0.5f;
      hi.y = c.y + min_size * 0.5f;
    }
  }
  return any;
}

void armature_map_selected(Layer &armature, const std::vector<Bone> &original, const Transform2D &map,
                           bool setup_frame)
{
  armature.bones = original;
  const std::vector<BonePose> poses = armature_pose(armature, setup_frame);
  const size_t n = armature.bones.size();
  std::map<uint64_t, int> index;
  for (int i = 0; i < int(n); i++) {
    index[armature.bones[i].id] = i;
  }
  /* The map's average stretch, for bones with no length to measure. */
  const float stretch = (length(map.apply_vector({1, 0})) + length(map.apply_vector({0, 1}))) * 0.5f;
  std::vector<Transform2D> frame(n);
  for (const int i : bones_parents_first(armature)) {
    Bone &b = armature.bones[i];
    const auto parent = index.find(b.parent);
    const bool has_parent = parent != index.end() && parent->second != i;
    const Transform2D parent_frame = has_parent ? frame[parent->second] : Transform2D();
    const bool picked = bone_picked(original[i]);
    const bool child_of_picked = has_parent && bone_picked(original[parent->second]);
    if (picked || child_of_picked) {
      /* Where the base and a point along the bone go: through the map when picked; an unpicked
       * child of a picked bone stays where it was. */
      const float reach = b.length > 1e-4f ? b.length : 1.0f;
      const Transform2D &was = poses[i].pose;
      float2 head = was.apply({0, 0}), tip = was.apply({reach, 0});
      if (picked) {
        head = map.apply(head);
        tip = map.apply(tip);
      }
      const Transform2D to_parent = parent_frame.inverted();
      const float2 h = to_parent.apply(head), t = to_parent.apply(tip);
      b.position = h;
      const float dist = distance(h, t);
      if (dist > 1e-9f) {
        float angle = std::atan2(t.y - h.y, t.x - h.x);
        angle += 2.0f * PI * std::round((original[i].angle - angle) / (2.0f * PI));
        b.angle = angle;
      }
      if (b.length > 1e-4f) {
        /* The setup pose takes a new length; a pose (length isn't animated) takes a scale. */
        if (setup_frame) {
          b.length = dist / std::max(std::abs(b.scale), 1e-6f);
        }
        else {
          b.scale = dist / b.length;
        }
      }
      else if (!setup_frame) {
        b.scale = picked ? original[i].scale * stretch : original[i].scale;
      }
    }
    frame[i] = parent_frame * bone_local(b.position, b.angle, b.scale);
  }
}

void armature_select_joints_in(Layer &armature, bool setup_frame, const std::function<bool(float2)> &inside,
                               bool extend, bool deselect)
{
  const std::vector<BonePose> poses = armature_pose(armature, setup_frame);
  if (!extend && !deselect) {
    armature_deselect_all(armature);
  }
  uint64_t first = 0;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    Bone &b = armature.bones[i];
    const bool h = inside(poses[i].head), t = inside(poses[i].tail);
    if (h) {
      b.select_head = !deselect;
      if (Bone *parent = b.connected && deselect ? bone_find(armature, b.parent) : nullptr) {
        parent->select_tail = false;
      }
    }
    if (t) {
      b.select_tail = !deselect;
      if (deselect) {
        for (Bone &child : armature.bones) {
          if (child.connected && child.parent == b.id) {
            child.select_head = false;
          }
        }
      }
    }
    if ((h || t) && !deselect && first == 0) {
      first = b.id;
    }
  }
  armature_flush_selection(armature);
  const Bone *active = bone_find(armature, armature.active_bone);
  if (first && (!active || !bone_picked(*active))) {
    armature.active_bone = first;
  }
}

void armature_select_bones_in(Layer &armature, bool setup_frame, const std::function<bool(float2)> &inside,
                              bool extend, bool deselect)
{
  const std::vector<BonePose> poses = armature_pose(armature, setup_frame);
  if (!extend && !deselect) {
    armature_deselect_all(armature);
  }
  uint64_t first = 0;
  for (size_t i = 0; i < armature.bones.size(); i++) {
    /* Any of eleven points along the bone, as Moho's gizmo tests. */
    bool hit = false;
    for (int j = 0; j <= 10 && !hit; j++) {
      hit = inside(poses[i].head + (poses[i].tail - poses[i].head) * (float(j) / 10.0f));
    }
    if (hit) {
      bone_select(armature.bones[i], !deselect);
      if (!deselect && first == 0) {
        first = armature.bones[i].id;
      }
    }
  }
  const Bone *active = bone_find(armature, armature.active_bone);
  if (first && (!active || !bone_picked(*active))) {
    armature.active_bone = first;
  }
}

void armature_mirror_edits(Layer &armature, const std::vector<Bone> &original, bool setup_frame)
{
  const std::vector<BonePose> before = bones_pose(original, setup_frame);
  const std::vector<BonePose> after = armature_pose(armature, setup_frame);
  const size_t n = armature.bones.size();
  if (before.size() != n) {
    return;
  }
  std::map<uint64_t, int> index;
  for (int i = 0; i < int(n); i++) {
    index[armature.bones[i].id] = i;
  }
  std::vector<bool> changed(n);
  for (size_t i = 0; i < n; i++) {
    changed[i] = distance(before[i].head, after[i].head) > 1e-7f || distance(before[i].tail, after[i].tail) > 1e-7f;
  }
  /* Each bone that moved drives its other side, unless that side moved too (both picked). */
  std::vector<int> source(n, -1);
  for (size_t i = 0; i < n; i++) {
    if (!changed[i]) {
      continue;
    }
    const auto m = index.find(bone_mirror_id(armature, armature.bones[i].id));
    if (m != index.end() && m->second != int(i) && !changed[m->second]) {
      source[m->second] = int(i);
    }
  }
  const auto reflect = [](float2 p) { return float2(-p.x, p.y); };
  std::vector<Transform2D> frame(n);
  for (const int i : bones_parents_first(armature)) {
    Bone &b = armature.bones[i];
    const auto parent = index.find(b.parent);
    const Transform2D parent_frame = parent != index.end() && parent->second != i ? frame[parent->second]
                                                                                 : Transform2D();
    if (source[i] >= 0) {
      const Bone &src = armature.bones[source[i]];
      const Transform2D to_parent = parent_frame.inverted();
      const float2 h = to_parent.apply(reflect(after[source[i]].head));
      const float2 t = to_parent.apply(reflect(after[source[i]].tail));
      b.position = h;
      if (distance(h, t) > 1e-7f) {
        float angle = std::atan2(t.y - h.y, t.x - h.x);
        angle += 2.0f * PI * std::round((b.angle - angle) / (2.0f * PI));
        b.angle = angle;
      }
      b.scale = src.scale;
      /* Length isn't animated: only the setup pose takes the other side's. */
      if (setup_frame) {
        b.length = src.length;
      }
    }
    frame[i] = parent_frame * bone_local(b.position, b.angle, b.scale);
  }
}

std::string bone_mirror_name(const std::string &name)
{
  /* A number Blender adds to keep names unique stays at the end: "arm_L.001" <-> "arm_R.001". */
  if (const size_t dot = name.find_last_of('.'); dot != std::string::npos && dot > 0 && dot + 1 < name.size() &&
                                                 name.find_first_not_of("0123456789", dot + 1) == std::string::npos)
  {
    const std::string base = name.substr(0, dot);
    const std::string flipped = bone_mirror_name(base);
    if (flipped != base) {
      return flipped + name.substr(dot);
    }
  }
  /* Suffixes first (_L, .L, -L, " L", either case), then Left/Right anywhere. */
  static const std::pair<const char *, const char *> suffixes[] = {
      {"_L", "_R"}, {".L", ".R"}, {"-L", "-R"}, {" L", " R"}, {"_l", "_r"}, {".l", ".r"}, {"-l", "-r"}, {" l", " r"}};
  for (const auto &[l, r] : suffixes) {
    for (const auto &[from, to] : {std::pair{l, r}, std::pair{r, l}}) {
      const std::string f = from;
      if (name.size() > f.size() && name.compare(name.size() - f.size(), f.size(), f) == 0) {
        return name.substr(0, name.size() - f.size()) + to;
      }
    }
  }
  static const std::pair<const char *, const char *> words[] = {{"Left", "Right"}, {"left", "right"}, {"LEFT", "RIGHT"}};
  for (const auto &[l, r] : words) {
    for (const auto &[from, to] : {std::pair{l, r}, std::pair{r, l}}) {
      if (const size_t at = name.find(from); at != std::string::npos) {
        return name.substr(0, at) + to + name.substr(at + std::string(from).size());
      }
    }
  }
  return name;
}

uint64_t bone_mirror_id(const Layer &armature, uint64_t id)
{
  const Bone *bone = bone_find(armature, id);
  if (!bone) {
    return id;
  }
  const std::string other = bone_mirror_name(bone->name);
  if (other == bone->name) {
    return id;
  }
  const Bone *mirror = bone_find_by_name(armature, other);
  return mirror ? mirror->id : id;
}

static const Layer *armature_in(const std::vector<Layer> &list, const Layer *armature, uint64_t id)
{
  for (const Layer &layer : list) {
    if (layer.id == id) {
      return armature;
    }
    if (const Layer *found = armature_in(layer.children, layer.is_bone_layer() ? &layer : armature, id)) {
      return found;
    }
  }
  return nullptr;
}

const Layer *layer_armature(const Document &doc, uint64_t layer_id)
{
  return armature_in(doc.layers, nullptr, layer_id);
}

static bool sync_in(std::vector<Layer> &list, bool inside)
{
  bool changed = false;
  for (Layer &layer : list) {
    if (!layer.is_group()) {
      const auto is_armature = [](const Modifier &m) { return m.type == ModifierType::Armature; };
      const long count = long(std::count_if(layer.modifiers.begin(), layer.modifiers.end(), is_armature));
      if (inside && count != 1) {
        std::erase_if(layer.modifiers, is_armature);
        Modifier armature;
        armature.type = ModifierType::Armature;
        armature.name = "Armature";
        layer.modifiers.push_back(armature);
        changed = true;
      }
      else if (!inside && count > 0) {
        std::erase_if(layer.modifiers, is_armature);
        changed = true;
      }
    }
    changed |= sync_in(layer.children, inside || layer.is_bone_layer());
  }
  return changed;
}

bool document_sync_armature_modifiers(Document &doc)
{
  return sync_in(doc.layers, false);
}

}  // namespace sky
