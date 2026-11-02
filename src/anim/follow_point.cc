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
  }
  const auto key = std::make_pair(f.owner->id, r.run.name);
  const auto kept = read.shapes.find(key);
  r.shape = kept != read.shapes.end() ? kept->second : (read.shapes[key] = vector_deform_shape(doc, *f.owner, r.run));
  if (f.c->follow_point_id != 0 || f.c->follow_point2_id != 0) {
    const auto [at, fresh] = read.points.try_emplace(key);
    if (fresh) {
      vector_deform_shape_read(doc, *f.owner, r.run, at->second.first, at->second.second);
    }
    r.points = &at->second;
  }
  return r;
}

static void follower_update(Document &doc, Follower &f, ShapesRead &read)
{
  BoneConstraint &c = *f.c;
  const PointReader r = point_reader(doc, f, read);
  const Transform2D to_world = layer_world_transform(doc, f.rig->id);
  const auto shown = [&](float2 point, float2 shift, uint64_t id) {
    return id != 0 ? r.shown_picked(point, id) : r.shown(point + shift);
  };
  const float2 here = shown(c.follow_point, c.follow_shift, c.follow_point_id);
  const float2 start = r.now.apply(r.carried_at(c.follow_point));
  c.follow_offset = here - (r.carried ? start : c.follow_rest);
  c.follow_now = to_world.apply(here);
  if (!c.follow_two) {
    return;
  }
  const float2 here2 = shown(c.follow_point2, c.follow_shift2, c.follow_point2_id);
  c.follow_now2 = to_world.apply(here2);
  const float2 was = r.carried ? r.now.apply(r.carried_at(c.follow_point2)) - start : c.follow_rest2 - c.follow_rest;
  const float2 is = here2 - here;
  if (length(was) > 1e-6f && length(is) > 1e-6f) {
    c.follow_turn = std::atan2(was.x * is.y - was.y * is.x, dot(was, is));
    c.follow_stretch = length(is) / length(was);
  }
}

/* Points of the shape rest where the shape has them. */
static void follow_picked_rest(const Document &doc, BoneConstraint &c, const Layer &owner, const Modifier &m)
{
  if (c.follow_point_id == 0 && c.follow_point2_id == 0) {
    return;
  }
  VectorDrawing rest, bent;
  if (!vector_deform_shape_read(doc, owner, m, rest, bent)) {
    return;
  }
  for (const bool second : {false, true}) {
    const uint64_t id = second ? c.follow_point2_id : c.follow_point_id;
    const int at = id != 0 ? drawing_point_index(rest, id) : -1;
    if (at >= 0) {
      (second ? c.follow_point2 : c.follow_point) = rest.points[size_t(at)].pos;
    }
  }
}

/* What moves the deform's shape and the layer it's on. */
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
        c.follow_now = layer_world_transform(doc, owner->id).apply(c.follow_point + (c.follow_point_id != 0 ? float2() : c.follow_shift));
        c.follow_now2 = layer_world_transform(doc, owner->id).apply(c.follow_point2 + (c.follow_point2_id != 0 ? float2() : c.follow_shift2));
        /* Layers' setup is what they are now (only the bones rest), so frame 0's place is kept from frame 0. */
        if (doc.frame_current == 0) {
          c.follow_rest = layer_relative_transform(doc, rig.id, owner->id).apply(c.follow_point);
          c.follow_rest2 = layer_relative_transform(doc, rig.id, owner->id).apply(c.follow_point2);
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
  c.follow_point_id = c.follow_point2_id = 0;
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
    c.follow_point2 = c.follow_point;
    if (c.follow_two) {
      c.follow_two = false;
      follow_point_second(doc, rig, bone, c, true);
    }
  }
}

/* How far a step is taken to see how the deform moves a place, and how far one drag's step may go. */
static constexpr float FOLLOW_SOLVE_STEP = 0.002f;
static constexpr float FOLLOW_SOLVE_REACH = 0.2f;

float2 follow_point_shift_to(Document &doc, Layer &rig, Bone &bone, BoneConstraint &c, bool second, float2 to)
{
  const float2 shift = second ? c.follow_shift2 : c.follow_shift;
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
  const float2 base = second ? c.follow_point2 : c.follow_point;
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
                        bool second)
{
  (second ? c.follow_point2 : c.follow_point) = point;
  (second ? c.follow_rest2 : c.follow_rest) = layer_setup_relative_transform(doc, rig.id, owner.id).apply(point);
}

void follow_point_pick(Document &doc, const Layer &rig, BoneConstraint &c, bool second, uint64_t id)
{
  (second ? c.follow_point2_id : c.follow_point_id) = id;
  Layer *owner = nullptr;
  const Modifier *m = follow_point_modifier(doc, c, &owner);
  if (m && id != 0) {
    follow_picked_rest(doc, c, *owner, *m);
    follow_point_place(doc, rig, c, *owner, second ? c.follow_point2 : c.follow_point, second);
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
  c.follow_point_id = c.follow_point2_id = 0;
  const float2 rest(-c.follow_rest.x, c.follow_rest.y), rest2(-c.follow_rest2.x, c.follow_rest2.y);
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
  follow_point_place(doc, rig, c, *owner, to_owner.apply(rest));
  follow_point_place(doc, rig, c, *owner, to_owner.apply(rest2), true);
  c.follow_shift = c.follow_shift2 = {};
  c.follow_shift_keys = {};
  c.follow_shift2_keys = {};
}

/* How far from the first a second point starts, in the bone layer: spread out, the angle stays steady. */
static constexpr float FOLLOW_SECOND_REACH = 0.1f;

void follow_point_second(const Document &doc, const Layer &rig, const Bone &bone, BoneConstraint &c, bool on)
{
  c.follow_two = on;
  Layer *owner = nullptr;
  if (!on || length(c.follow_point2 - c.follow_point) > 1e-6f ||
      !follow_point_modifier(const_cast<Document &>(doc), c, &owner))
  {
    return;
  }
  const std::vector<BonePose> poses = armature_pose(rig, true);
  const BonePose *pose = bone_pose_find(poses, bone.id);
  if (!pose) {
    return;
  }
  const float2 along = pose->tail - pose->head;
  const float2 to = c.follow_rest + (length(along) > 1e-6f ? normalize(along) : float2(1.0f, 0.0f)) *
                                        std::max(length(along), FOLLOW_SECOND_REACH);
  follow_point_place(doc, rig, c, *owner, layer_setup_relative_transform(doc, rig.id, owner->id).inverted().apply(to), true);
}

}  // namespace oanim
