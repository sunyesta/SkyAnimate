#include "anim/follow_point.hh"

#include <algorithm>
#include <map>
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
            grew |= add(c.eyeball_center);
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

static void follower_update(Document &doc, Follower &f)
{
  BoneConstraint &c = *f.c;
  const Modifier &m = *f.modifier;
  const Transform2D now = layer_relative_transform(doc, f.rig->id, f.owner->id);
  const Transform2D rest = layer_setup_relative_transform(doc, f.rig->id, f.owner->id);
  const auto shape_bones = f.shape_bones.find(f.rig->id);
  const bool carried = shape_bones != f.shape_bones.end() && above(*f.rig, *f.bone, shape_bones->second);
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
  Modifier run = m;
  run.deform_rest_posed = carried || owner_carried;
  run.name = m.name + (run.deform_rest_posed ? "#posed" : "#point");
  run.deform_custom = false;
  if (!f.owner->has_drawing()) {
    run.deform_from_group = f.owner->id;
  }
  float2 at = c.follow_point;
  if (carried) {
    /* Where the bones above put the point: the shape's rest is posed by them too, so only its change of shape is left. */
    const std::vector<BonePose> poses = armature_pose(*f.rig, false);
    if (const BonePose *parent = bone_pose_find(poses, f.bone->parent)) {
      at = now.inverted().apply(parent->skin.apply(rest.apply(at)));
    }
  }
  const float2 bent = vector_deform_point(doc, *f.owner, run, at);
  c.follow_offset = now.apply(bent) - (carried ? now.apply(at) : c.follow_rest);
  c.follow_now = layer_world_transform(doc, f.rig->id).apply(now.apply(bent));
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
        c.follow_error.clear();
        Layer *owner = nullptr;
        const Modifier *m = follow_point_modifier(doc, c, &owner);
        if (!m) {
          if (c.follow_layer != 0) {
            c.follow_error = "Its deform is gone";
          }
          continue;
        }
        c.follow_now = layer_world_transform(doc, owner->id).apply(c.follow_point);
        /* Layers' setup is what they are now (only the bones rest), so frame 0's place is kept from frame 0. */
        if (doc.frame_current == 0) {
          c.follow_rest = layer_relative_transform(doc, rig.id, owner->id).apply(c.follow_point);
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
  for (Follower &f : all) {
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
          (g.rig == f.rig && bone_is_descendant(*f.rig, f.bone->id, g.bone->id)))
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
  for (const int i : order) {
    if (all[size_t(i)].c->follow_error.empty()) {
      follower_update(doc, all[size_t(i)]);
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

void follow_point_aim(Document &doc, Layer &rig, const Bone &bone, BoneConstraint &c, Layer *owner, Modifier *m)
{
  c.follow_layer = 0;
  c.follow_modifier = 0;
  if (!owner || !m) {
    return;
  }
  if (m->id == 0) {
    for (const Modifier &other : owner->modifiers) {
      m->id = std::max(m->id, other.id);
    }
    m->id++;
  }
  c.follow_layer = owner->id;
  c.follow_modifier = m->id;
  const std::vector<BonePose> poses = armature_pose(rig, true);
  if (const BonePose *pose = bone_pose_find(poses, bone.id)) {
    follow_point_place(doc, rig, c, *owner, layer_setup_relative_transform(doc, rig.id, owner->id).inverted().apply(pose->head));
  }
}

void follow_point_place(const Document &doc, const Layer &rig, BoneConstraint &c, const Layer &owner, float2 point)
{
  c.follow_point = point;
  c.follow_rest = layer_setup_relative_transform(doc, rig.id, owner.id).apply(point);
}

}  // namespace oanim
