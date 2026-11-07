#include "anim/follow_point.hh"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "anim/vector_deform.hh"
#include "data/bones.hh"
#include "data/document.hh"

namespace oanim {

/* Bones by their bone layer's id. */
using BoneSets = std::map<uint64_t, std::set<uint64_t>>;

struct Follower {
  Layer *rig = nullptr;
  Bone *bone = nullptr;
  BoneConstraint *c = nullptr;
  Layer *owner = nullptr;
  const Modifier *modifier = nullptr;
  /* What moves the deform's shape, what moves the layer it's on, and the two with what they read. */
  BoneSets shape_bones, owner_bones, bones;
  std::vector<int> after;
  /* 0 not yet, 1 being worked out, 2 done. */
  int state = 0;
};

/* The bones that move `layer`: those it and its groups follow, and with `points` those weighted on its points; and,
 * through whatever bends it, those that move the shapes bending it. */
static void gather_bones(const Document &doc, const Layer &layer, bool points, std::set<uint64_t> &seen, BoneSets &r)
{
  if (!seen.insert(layer.id).second) {
    return;
  }
  Document &d = const_cast<Document &>(doc);
  for (const Layer *l = &layer; l; l = layer_parent(d, l->id)) {
    if (const Layer *rig = layer_armature(doc, l->id)) {
      if (l->parent_bone != 0) {
        r[rig->id].insert(l->parent_bone);
      }
      /* A layer on a Parent Bone goes by that alone: the weights its points still hold don't count. */
      else if (points && l == &layer && l->has_drawing()) {
        for (const MeshPoint &p : l->drawing.points) {
          for (const BoneWeight &w : p.weights) {
            if (w.weight > 0.0f) {
              r[rig->id].insert(w.bone);
            }
          }
        }
      }
    }
    for (const Modifier &m : l->modifiers) {
      const Layer *shape = modifier_is_deform(m.type) ? layer_find(doc, m.deform_layer) : nullptr;
      if (shape) {
        gather_bones(doc, *shape, true, seen, r);
      }
    }
    if (const Layer *source = layer_find(doc, l->reference_source)) {
      gather_bones(doc, *source, true, seen, r);
    }
  }
}

/* With what those bones hang on: their parents, what their constraints aim at, and the IK that turns them. */
static void close_bones(const Document &doc, BoneSets &sets)
{
  for (auto &[rig_id, bones] : sets) {
    const Layer *rig = layer_find(doc, rig_id);
    if (!rig) {
      continue;
    }
    std::map<uint64_t, const Bone *> by_id;
    for (const Bone &b : rig->bones) {
      by_id[b.id] = &b;
    }
    const auto add = [&](uint64_t id) { return by_id.count(id) && bones.insert(id).second; };
    for (bool grew = true; grew;) {
      grew = false;
      for (const Bone &b : rig->bones) {
        if (bones.count(b.id)) {
          grew |= add(b.parent);
          for (const BoneConstraint &c : b.constraints) {
            grew |= add(c.target);
          }
        }
        for (const BoneConstraint &c : b.constraints) {
          if (c.type != BoneConstraintType::IK || !c.enabled) {
            continue;
          }
          const Bone *up = &b;
          bool turned = false;
          for (int n = 0; up && n < 256 && (c.chain == 0 || n < c.chain); n++) {
            turned = turned || bones.count(up->id) != 0;
            const auto it = by_id.find(up->parent);
            up = it == by_id.end() ? nullptr : it->second;
          }
          if (turned) {
            grew |= add(b.id);
            grew |= add(c.target);
          }
        }
      }
    }
  }
}

static bool above(const Layer &rig, const Bone &bone, const std::set<uint64_t> &bones)
{
  const Bone *b = bone_find(rig, bone.parent);
  for (int n = 0; b && n < 256; n++) {
    if (bones.count(b->id)) {
      return true;
    }
    b = bone_find(rig, b->parent);
  }
  return false;
}

/* Each follower after those it reads through what moves its shape and its bone. One on a loop is refused. */
static void order_followers(std::vector<Follower> &all, int i, std::vector<int> &stack, std::vector<int> &r_order)
{
  Follower &f = all[size_t(i)];
  f.state = 1;
  stack.push_back(i);
  for (const int next : f.after) {
    Follower &g = all[size_t(next)];
    if (g.state == 1) {
      for (auto it = std::find(stack.begin(), stack.end(), next); it != stack.end(); ++it) {
        all[size_t(*it)].c->follow_error = "Loop: it moves what it follows, through the bones that follow it";
      }
    }
    else if (g.state == 0) {
      order_followers(all, next, stack, r_order);
    }
  }
  stack.pop_back();
  f.state = 2;
  r_order.push_back(i);
}

/* How far each way from a place the deform is read to see how it turns there. */
static constexpr float FOLLOW_TURN_STEP = 0.002f;

/* How a follower reads its deform on this frame. */
struct PointReader {
  Modifier run;
  std::shared_ptr<const DeformShape> shape;
  Transform2D now, rest, skin;
  bool carried = false;
  /* The shape at rest and now, when a point of it is followed. */
  const std::pair<VectorDrawing, VectorDrawing> *points = nullptr;

  /* Where the bones above put a place: the shape's rest is posed by them too, so only its change of shape is left. */
  float2 carried_at(float2 at) const
  {
    return carried ? now.inverted().apply(skin.apply(rest.apply(at))) : at;
  }
  /* Where the deform shows a place, in the bone layer's space. */
  float2 shown(float2 at) const
  {
    return now.apply(vector_deform_point(shape.get(), run, carried_at(at)));
  }
  /* The same for a follower resting at `at` that goes by point `id` of the shape: that point's change, as the deform's
   * Strength says. A point that's gone changes nothing. */
  float2 shown_picked(float2 at, uint64_t id) const
  {
    const int was = points ? drawing_point_index(points->first, id) : -1;
    const int is = points ? drawing_point_index(points->second, id) : -1;
    const float2 change = was >= 0 && is >= 0 ? points->second.points[size_t(is)].pos - points->first.points[size_t(was)].pos
                                              : float2();
    return now.apply(carried_at(at) + change * std::clamp(run.deform_strength, 0.0f, 1.0f));
  }
  /* A way `v` from place `at` as it rested, in the bone layer's space. */
  float2 rested(float2 at, float2 v) const
  {
    return carried ? now.apply_vector(carried_at(at + v) - carried_at(at)) : rest.apply_vector(v);
  }
  /* How the shape has turned at place `at`, from how the deform turns two short ways from it (radians). */
  float turned(float2 at) const
  {
    const float h = FOLLOW_TURN_STEP;
    const float2 x = shown(at + float2(h, 0.0f)) - shown(at - float2(h, 0.0f));
    const float2 y = shown(at + float2(0.0f, h)) - shown(at - float2(0.0f, h));
    const float2 rx = rested(at, float2(2.0f * h, 0.0f)), ry = rested(at, float2(0.0f, 2.0f * h));
    const Transform2D bent{x.x, x.y, y.x, y.y, 0.0f, 0.0f}, was{rx.x, rx.y, ry.x, ry.y, 0.0f, 0.0f};
    const Transform2D a = bent * was.inverted();
    return std::atan2(a.b - a.c, a.a + a.d);
  }
  /* The same at point `id` of the shape: how the curve's way through it has turned, as the deform's Strength says
   * (Harmony's Kinematic Output). A point that's gone, or on no curve, doesn't turn. */
  float turned_picked(float2 at, uint64_t id) const
  {
    float2 was, is;
    if (!points || !point_way(points->first, id, was) || !point_way(points->second, id, is)) {
      return 0.0f;
    }
    was = rested(at, was);
    is = now.apply_vector(is);
    return std::atan2(was.x * is.y - was.y * is.x, dot(was, is)) * std::clamp(run.deform_strength, 0.0f, 1.0f);
  }
  /* Which way the curve runs through point `id` of `drawing`: from its incoming handle to its outgoing one, or from the
   * point before to the point after where those sit on it. */
  static bool point_way(const VectorDrawing &drawing, uint64_t id, float2 &r_way)
  {
    const int index = drawing_point_index(drawing, id);
    if (index < 0) {
      return false;
    }
    for (const Curve &curve : drawing.curves) {
      const int n = int(curve.points.size());
      for (int i = 0; i < n; i++) {
        if (curve.points[size_t(i)].point != index) {
          continue;
        }
        float2 way = curve_handle(drawing, curve, i, false) - curve_handle(drawing, curve, i, true);
        if (length(way) < 1e-7f) {
          const int before = i > 0 ? i - 1 : (curve.closed ? n - 1 : i);
          const int after = i < n - 1 ? i + 1 : (curve.closed ? 0 : i);
          way = drawing.points[size_t(curve.points[size_t(after)].point)].pos -
                drawing.points[size_t(curve.points[size_t(before)].point)].pos;
        }
        if (length(way) < 1e-7f) {
          return false;
        }
        r_way = way;
        return true;
      }
    }
    return false;
  }
};

/* Deforms read so far in this pass: followers usually share them, and a read is what costs. Each reads after whatever
 * moves its shape, so all those sharing one see it the same. */
struct ShapesRead {
  using Key = std::pair<uint64_t, std::string>;
  std::map<Key, std::shared_ptr<const DeformShape>> shapes;
  std::map<Key, std::pair<VectorDrawing, VectorDrawing>> points;
};

static PointReader point_reader(Document &doc, const Follower &f, ShapesRead &read)
{
  PointReader r;
  r.now = layer_relative_transform(doc, f.rig->id, f.owner->id);
  r.rest = layer_setup_relative_transform(doc, f.rig->id, f.owner->id);
  const auto shape_bones = f.shape_bones.find(f.rig->id);
  r.carried = shape_bones != f.shape_bones.end() && above(*f.rig, *f.bone, shape_bones->second);
  /* A layer the shape's bones move takes only the change of shape from the deform, as the drawings in it do. */
  bool owner_carried = false;
  for (const auto &[rig_id, bones] : f.owner_bones) {
    const auto shared = f.shape_bones.find(rig_id);
    owner_carried = owner_carried || (shared != f.shape_bones.end() &&
                                      std::any_of(bones.begin(), bones.end(),
                                                  [&](uint64_t b) { return shared->second.count(b) != 0; }));
  }
  /* Kept apart from what the deform keeps for the drawings it bends. A group's deform has no drawing to run on: it runs
   * on the group, as it does on a drawing inside, so what moves the group stays on top of it. */
  r.run = *f.modifier;
  r.run.deform_rest_posed = r.carried || owner_carried;
  r.run.name = f.modifier->name + (r.run.deform_rest_posed ? "#posed" : "#point");
  r.run.deform_custom = false;
  if (!f.owner->has_drawing()) {
    r.run.deform_from_group = f.owner->id;
  }
  if (r.carried) {
    const std::vector<BonePose> poses = armature_pose(*f.rig, false);
    if (const BonePose *parent = bone_pose_find(poses, f.bone->parent)) {
      r.skin = parent->skin;
    }
    else if (f.bone->outer_bone != 0) {
      r.skin = f.bone->outer_pose * f.bone->outer_setup.inverted();
    }
  }
  const auto key = std::make_pair(f.owner->id, r.run.name);
  const auto kept = read.shapes.find(key);
  r.shape = kept != read.shapes.end() ? kept->second : (read.shapes[key] = vector_deform_shape(doc, *f.owner, r.run));
  if (f.c->follow_point_id != 0 || f.c->follow_point2_id != 0 || f.c->follow_point3_id != 0) {
    const auto [at, fresh] = read.points.try_emplace(key);
    if (fresh) {
      vector_deform_shape_read(doc, *f.owner, r.run, at->second.first, at->second.second);
    }
    r.points = &at->second;
  }
  return r;
}

/* Harmony's guards (its Point Kinematic Output, DeformTransformOut.dll): a point within this squared distance of the first
 * is moved by FOLLOW_NUDGE before the bend is read, so two points never sit on one place. */
static constexpr float FOLLOW_CLOSE_SQ = 1e-5f;
static constexpr float FOLLOW_NUDGE = 1e-4f;
/* How short the line between two points is taken to be at least, as it rested and as it is now; a line bent to
 * nothing points along x, this long. */
static constexpr float FOLLOW_REST_MIN = 1e-4f;
static constexpr float FOLLOW_NOW_MIN = 1e-5f;
static constexpr float FOLLOW_NOW_NONE = 1e-3f;

static float length_sq(float2 v)
{
  return v.x * v.x + v.y * v.y;
}

static void follower_update(Document &doc, Follower &f, ShapesRead &read)
{
  BoneConstraint &c = *f.c;
  const PointReader r = point_reader(doc, f, read);
  const Transform2D to_world = layer_world_transform(doc, f.rig->id);
  const int n = c.follow_count();
  /* Where each point rested, in the bone layer's space. */
  float2 was[3];
  for (int i = 0; i < n; i++) {
    was[i] = r.carried ? r.now.apply(r.carried_at(c.follow_point_at(i))) : c.follow_rest_at(i);
  }
  /* Points too close to the first are moved apart, as Harmony does: the second along x, the third up and then across. */
  float2 nudge[3];
  if (n >= 2 && length_sq(was[1] - was[0]) < FOLLOW_CLOSE_SQ) {
    nudge[1] = {FOLLOW_NUDGE, 0.0f};
  }
  if (n == 3) {
    if (length_sq(was[2] - was[0]) < FOLLOW_CLOSE_SQ) {
      nudge[2] = {0.0f, FOLLOW_NUDGE};
    }
    if (length_sq(was[2] + nudge[2] - was[0]) < FOLLOW_CLOSE_SQ) {
      nudge[2] = nudge[2] + float2(FOLLOW_NUDGE, FOLLOW_NUDGE);
    }
  }
  /* Each point where it is now, in the bone layer's space. */
  const Transform2D to_owner = (r.carried ? r.now : r.rest).inverted();
  float2 here[3];
  for (int i = 0; i < n; i++) {
    const uint64_t id = c.follow_id_at(i);
    const float2 at = c.follow_point_at(i) + to_owner.apply_vector(nudge[i]);
    here[i] = id != 0 ? r.shown_picked(at, id) : r.shown(at + c.follow_shift_at(i));
    was[i] = was[i] + nudge[i];
    c.follow_now_at(i) = to_world.apply(here[i]);
  }
  c.follow_pivot = was[0];
  c.follow_offset = here[0] - was[0];
  if (n == 1) {
    if (c.follow_rotation) {
      c.follow_turn = c.follow_point_id != 0 ? r.turned_picked(c.follow_point, c.follow_point_id)
                                             : r.turned(c.follow_point + c.follow_shift);
    }
    return;
  }
  const float2 v = was[1] - was[0];
  float2 w = here[1] - here[0];
  if (n == 3) {
    const float2 v2 = was[2] - was[0], w2 = here[2] - here[0];
    /* Three on a line give Harmony no change at all: it doesn't follow them, not even their move. */
    const double cross = double(v.x) * double(v2.y) - double(v.y) * double(v2.x);
    if (cross == 0.0 || length_sq(was[2] - was[1]) == 0.0f) {
      c.follow_offset = {};
      return;
    }
    /* The change that takes the three as they rested to where they are: the bone's angle takes its turn, its shape the
     * rest (both scales and the skew). Nearly on a line, that's a big skew, as in Harmony. */
    const Transform2D bent{w.x, w.y, w2.x, w2.y, 0.0f, 0.0f}, rested{v.x, v.y, v2.x, v2.y, 0.0f, 0.0f};
    const Transform2D a = bent * rested.inverted();
    c.follow_turn = std::atan2(a.b - a.c, a.a + a.d);
    c.follow_shape = a * Transform2D::rotate(-c.follow_turn);
    return;
  }
  if (length_sq(w) == 0.0f) {
    w = {FOLLOW_NOW_NONE, 0.0f};
  }
  const float rested = std::max(length(v), FOLLOW_REST_MIN), now = std::max(length(w), FOLLOW_NOW_MIN);
  c.follow_turn = std::atan2(v.x * w.y - v.y * w.x, dot(v, w));
  c.follow_stretch = now / rested;
  if (c.follow_scale && c.follow_scale_mode == FollowScaleMode::AlongLine) {
    /* Stretched along the line as it is now, squashed across it. */
    const float along = c.follow_stretch;
    const float across = std::max((rested / now - 1.0f) * c.follow_volume + 1.0f, 1e-4f);
    const float2 d = normalize(w);
    const float k = along - across;
    c.follow_shape = {across + k * d.x * d.x, k * d.x * d.y, k * d.x * d.y, across + k * d.y * d.y, 0.0f, 0.0f};
  }
}

/* Points of the shape rest where the shape has them. */
static void follow_picked_rest(const Document &doc, BoneConstraint &c, const Layer &owner, const Modifier &m)
{
  if (c.follow_point_id == 0 && c.follow_point2_id == 0 && c.follow_point3_id == 0) {
    return;
  }
  VectorDrawing rest, bent;
  if (!vector_deform_shape_read(doc, owner, m, rest, bent)) {
    return;
  }
  for (int i = 0; i < 3; i++) {
    const uint64_t id = c.follow_id_at(i);
    const int at = id != 0 ? drawing_point_index(rest, id) : -1;
    if (at >= 0) {
      c.follow_point_at(i) = rest.points[size_t(at)].pos;
    }
  }
}

/* What moves the deform's shape and the layer it's on. */
bool layer_moved_by_bone(const Document &doc, const Layer &layer, uint64_t rig, uint64_t bone)
{
  std::set<uint64_t> seen;
  BoneSets sets;
  gather_bones(doc, layer, true, seen, sets);
  close_bones(doc, sets);
  const auto it = sets.find(rig);
  return it != sets.end() && it->second.count(bone) != 0;
}

static void follower_gather(Document &doc, Follower &f)
{
  std::set<uint64_t> seen;
  if (const Layer *shape = layer_find(doc, f.modifier->deform_layer)) {
    gather_bones(doc, *shape, true, seen, f.shape_bones);
  }
  f.bones = f.shape_bones;
  gather_bones(doc, *f.owner, false, seen, f.bones);
  for (const Layer *l = f.owner; l; l = layer_parent(doc, l->id)) {
    const Layer *rig = l->parent_bone != 0 ? layer_armature(doc, l->id) : nullptr;
    if (rig) {
      f.owner_bones[rig->id].insert(l->parent_bone);
    }
  }
  close_bones(doc, f.shape_bones);
  close_bones(doc, f.owner_bones);
  close_bones(doc, f.bones);
}

void follow_points_update(Document &doc)
{
  std::vector<Follower> all;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer()) {
      continue;
    }
    Layer &rig = *layer_find(doc, flat->id);
    for (Bone &b : rig.bones) {
      for (BoneConstraint &c : b.constraints) {
        if (c.type != BoneConstraintType::FollowPoint) {
          continue;
        }
        c.follow_offset = {};
        c.follow_turn = 0.0f;
        c.follow_stretch = 1.0f;
        c.follow_shape = {};
        c.follow_error.clear();
        Layer *owner = nullptr;
        const Modifier *m = follow_point_modifier(doc, c, &owner);
        if (!m) {
          if (c.follow_layer != 0) {
            c.follow_error = "Its deform is gone";
          }
          continue;
        }
        if (doc.frame_current == 0) {
          follow_picked_rest(doc, c, *owner, *m);
        }
        for (int k = 0; k < 3; k++) {
          c.follow_now_at(k) = layer_world_transform(doc, owner->id)
                                   .apply(c.follow_point_at(k) + (c.follow_id_at(k) != 0 ? float2() : c.follow_shift_at(k)));
          /* Layers' setup is what they are now (only the bones rest), so frame 0's place is kept from frame 0. */
          if (doc.frame_current == 0) {
            c.follow_rest_at(k) = layer_relative_transform(doc, rig.id, owner->id).apply(c.follow_point_at(k));
          }
        }
        if (!c.enabled || !m->show_viewport) {
          continue;
        }
        Follower f;
        f.rig = &rig;
        f.bone = &b;
        f.c = &c;
        f.owner = owner;
        f.modifier = m;
        all.push_back(std::move(f));
      }
    }
  }
  if (all.empty()) {
    return;
  }
  /* Followers of one deform hang on the same bones. */
  std::map<std::pair<uint64_t, uint64_t>, const Follower *> gathered;
  for (Follower &f : all) {
    const auto key = std::make_pair(f.owner->id, f.modifier->id);
    if (const auto kept = gathered.find(key); kept != gathered.end()) {
      f.shape_bones = kept->second->shape_bones;
      f.owner_bones = kept->second->owner_bones;
      f.bones = kept->second->bones;
    }
    else {
      follower_gather(doc, f);
      gathered[key] = &f;
    }
    const auto own = f.bones.find(f.rig->id);
    if (own != f.bones.end() && own->second.count(f.bone->id)) {
      f.c->follow_error = "Loop: this bone moves the deform's shape or the layer it's on";
    }
  }
  for (size_t i = 0; i < all.size(); i++) {
    Follower &f = all[i];
    for (size_t j = 0; j < all.size(); j++) {
      const Follower &g = all[j];
      if (i == j) {
        continue;
      }
      const auto bones = f.bones.find(g.rig->id);
      if ((bones != f.bones.end() && bones->second.count(g.bone->id)) ||
          (g.rig == f.rig && g.bone != f.bone && bone_is_descendant(*f.rig, f.bone->id, g.bone->id)))
      {
        f.after.push_back(int(j));
      }
    }
  }
  std::vector<int> order, stack;
  for (int i = 0; i < int(all.size()); i++) {
    if (all[size_t(i)].state == 0) {
      order_followers(all, i, stack, order);
    }
  }
  if (doc.frame_current == 0) {
    return;
  }
  ShapesRead read;
  for (const int i : order) {
    if (all[size_t(i)].c->follow_error.empty()) {
      follower_update(doc, all[size_t(i)], read);
    }
  }
}

Modifier *follow_point_modifier(Document &doc, const BoneConstraint &c, Layer **r_owner)
{
  Layer *owner = layer_find(doc, c.follow_layer);
  if (!owner || c.follow_modifier == 0) {
    return nullptr;
  }
  for (Modifier &m : owner->modifiers) {
    if (m.id == c.follow_modifier && modifier_is_deform(m.type)) {
      *r_owner = owner;
      return &m;
    }
  }
  return nullptr;
}

Modifier *follow_point_find(Document &doc, const std::string &layer, const std::string &modifier, Layer **r_owner)
{
  const bool is_id = !layer.empty() && std::all_of(layer.begin(), layer.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
  const uint64_t id = is_id ? std::strtoull(layer.c_str(), nullptr, 10) : 0;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (flat->name != layer && flat->id != id) {
      continue;
    }
    Layer *owner = layer_find(doc, flat->id);
    for (Modifier &m : owner->modifiers) {
      if (m.name == modifier && modifier_is_deform(m.type)) {
        *r_owner = owner;
        return &m;
      }
    }
  }
  return nullptr;
}

void follow_point_aim(Document &doc, Layer &rig, const Bone &bone, BoneConstraint &c, Layer *owner, Modifier *m)
{
  c.follow_layer = 0;
  c.follow_modifier = 0;
  c.follow_point_id = c.follow_point2_id = c.follow_point3_id = 0;
  if (!owner || !m) {
    return;
  }
  /* From the document's ids, never used twice: a new deform can't take the place of a deleted one. */
  if (m->id == 0) {
    m->id = doc.next_layer_id++;
  }
  c.follow_layer = owner->id;
  c.follow_modifier = m->id;
  const std::vector<BonePose> poses = armature_pose(rig, true);
  if (const BonePose *pose = bone_pose_find(poses, bone.id)) {
    follow_point_place(doc, rig, c, *owner, layer_setup_relative_transform(doc, rig.id, owner->id).inverted().apply(pose->head));
    c.follow_point2 = c.follow_point3 = c.follow_point;
    const int count = c.follow_count();
    c.follow_two = c.follow_three = false;
    follow_point_count(doc, rig, bone, c, count);
  }
}

/* How far a step is taken to see how the deform moves a place, and how far one drag's step may go. */
static constexpr float FOLLOW_SOLVE_STEP = 0.002f;
static constexpr float FOLLOW_SOLVE_REACH = 0.2f;

float2 follow_point_shift_to(Document &doc, Layer &rig, Bone &bone, BoneConstraint &c, int which, float2 to)
{
  const float2 shift = c.follow_shift_at(which);
  Follower f;
  f.rig = &rig;
  f.bone = &bone;
  f.c = &c;
  f.modifier = follow_point_modifier(doc, c, &f.owner);
  if (!f.modifier) {
    return shift;
  }
  follower_gather(doc, f);
  ShapesRead read;
  const PointReader r = point_reader(doc, f, read);
  const float2 target = layer_world_transform(doc, rig.id).inverted().apply(to);
  const float2 base = c.follow_point_at(which);
  float2 at = base + shift;
  /* Newton's method through the deform: it has no inverse, and a point moved by hand stays close to where it was. */
  for (int i = 0; i < 8; i++) {
    const float2 here = r.shown(at);
    const float2 off = target - here;
    if (length(off) < 1e-5f) {
      break;
    }
    const float2 x = (r.shown(at + float2(FOLLOW_SOLVE_STEP, 0.0f)) - here) / FOLLOW_SOLVE_STEP;
    const float2 y = (r.shown(at + float2(0.0f, FOLLOW_SOLVE_STEP)) - here) / FOLLOW_SOLVE_STEP;
    const float det = x.x * y.y - x.y * y.x;
    float2 step = std::abs(det) > 1e-4f ? float2((y.y * off.x - y.x * off.y) / det, (x.x * off.y - x.y * off.x) / det) : off;
    if (length(step) > FOLLOW_SOLVE_REACH) {
      step = normalize(step) * FOLLOW_SOLVE_REACH;
    }
    at = at + step;
  }
  return at - base;
}

void follow_point_place(const Document &doc, const Layer &rig, BoneConstraint &c, const Layer &owner, float2 point,
                        int which)
{
  c.follow_point_at(which) = point;
  c.follow_rest_at(which) = layer_setup_relative_transform(doc, rig.id, owner.id).apply(point);
}

void follow_point_pick(Document &doc, const Layer &rig, BoneConstraint &c, int which, uint64_t id)
{
  c.follow_id_at(which) = id;
  Layer *owner = nullptr;
  const Modifier *m = follow_point_modifier(doc, c, &owner);
  if (m && id != 0) {
    follow_picked_rest(doc, c, *owner, *m);
    follow_point_place(doc, rig, c, *owner, c.follow_point_at(which), which);
  }
}

void follow_point_mirror(Document &doc, const Layer &rig, BoneConstraint &c)
{
  Layer *owner = nullptr;
  Modifier *m = follow_point_modifier(doc, c, &owner);
  if (!m) {
    return;
  }
  /* A point of the shape is on the far side: the copy has its own place across from it. */
  c.follow_point_id = c.follow_point2_id = c.follow_point3_id = 0;
  /* As they rested, before the deform changes. */
  const float2 rests[3] = {c.follow_rest, c.follow_rest2, c.follow_rest3};
  const std::string partner = bone_mirror_name(owner->name);
  if (partner != owner->name) {
    const std::vector<const Layer *> flat = document_layers_flat(doc);
    for (const Layer *l : flat) {
      if (l->name != partner) {
        continue;
      }
      Layer *other = layer_find(doc, l->id);
      for (Modifier &candidate : other->modifiers) {
        if (candidate.name == m->name && modifier_is_deform(candidate.type)) {
          if (candidate.id == 0) {
            candidate.id = doc.next_layer_id++;
          }
          c.follow_layer = other->id;
          c.follow_modifier = candidate.id;
          owner = other;
          break;
        }
      }
    }
  }
  const Transform2D to_owner = layer_setup_relative_transform(doc, rig.id, owner->id).inverted();
  for (int i = 0; i < 3; i++) {
    follow_point_place(doc, rig, c, *owner, to_owner.apply(float2(-rests[i].x, rests[i].y)), i);
    c.follow_shift_at(i) = {};
    c.follow_shift_keys_at(i) = {};
  }
}

/* How far from the first a second or third point starts, in the bone layer: spread out, the angle stays steady. */
static constexpr float FOLLOW_SECOND_REACH = 0.1f;

void follow_point_count(const Document &doc, const Layer &rig, const Bone &bone, BoneConstraint &c, int count)
{
  count = std::clamp(count, 1, 3);
  c.follow_two = count >= 2;
  c.follow_three = count >= 3;
  Layer *owner = nullptr;
  if (!follow_point_modifier(const_cast<Document &>(doc), c, &owner)) {
    return;
  }
  const std::vector<BonePose> poses = armature_pose(rig, true);
  const BonePose *pose = bone_pose_find(poses, bone.id);
  if (!pose) {
    return;
  }
  const float2 along = pose->tail - pose->head;
  const float2 way = length(along) > 1e-6f ? normalize(along) : float2(1.0f, 0.0f);
  const float reach = std::max(length(along), FOLLOW_SECOND_REACH);
  const Transform2D to_owner = layer_setup_relative_transform(doc, rig.id, owner->id).inverted();
  /* One never put anywhere (still on the first) starts along the bone (the second) or square to it (the third). */
  for (int i = 1; i < count; i++) {
    if (c.follow_id_at(i) != 0 || length(c.follow_point_at(i) - c.follow_point) > 1e-6f) {
      continue;
    }
    const float2 to = c.follow_rest + (i == 1 ? way : float2(-way.y, way.x)) * reach;
    follow_point_place(doc, rig, c, *owner, to_owner.apply(to), i);
  }
}

}  // namespace oanim
