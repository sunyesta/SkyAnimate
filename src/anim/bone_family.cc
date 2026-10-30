#include "anim/bone_family.hh"

#include <algorithm>
#include <cmath>
#include <set>

#include "data/bones.hh"
#include "data/document.hh"

namespace oanim {

/* The bone layer a bone layer is a sub bone group of, or null. */
static Layer *enclosing_armature(Document &doc, const Layer &armature)
{
  if (!armature.sub_bone_group) {
    return nullptr;
  }
  const Layer *outer = layer_armature(doc, armature.id);
  return outer ? layer_find(doc, outer->id) : nullptr;
}

std::vector<Layer *> armature_family(Document &doc, uint64_t armature)
{
  Layer *self = layer_find(doc, armature);
  if (!self || !self->is_bone_layer()) {
    return {};
  }
  Layer *root = self;
  std::set<uint64_t> seen{root->id};
  while (Layer *outer = enclosing_armature(doc, *root)) {
    if (!seen.insert(outer->id).second) {
      break;
    }
    root = outer;
  }
  std::vector<Layer *> family{root};
  std::set<uint64_t> in{root->id};
  const auto walk = [&](auto &&self_walk, std::vector<Layer> &layers) -> void {
    for (Layer &l : layers) {
      if (!l.shown()) {
        continue;
      }
      if (l.is_bone_layer() && l.sub_bone_group) {
        const Layer *outer = layer_armature(doc, l.id);
        if (outer && in.contains(outer->id)) {
          family.push_back(&l);
          in.insert(l.id);
        }
      }
      self_walk(self_walk, l.children);
    }
  };
  walk(walk, root->children);
  if (!in.contains(self->id)) {
    family.push_back(self);
  }
  return family;
}

std::pair<uint64_t, uint64_t> bone_swap_root(const Document &doc, uint64_t layer, uint64_t bone)
{
  std::set<std::pair<uint64_t, uint64_t>> seen;
  std::pair<uint64_t, uint64_t> at{layer, bone};
  while (seen.insert(at).second) {
    const Layer *l = layer_find(doc, at.first);
    const Bone *b = l ? bone_find(*l, at.second) : nullptr;
    if (!b || b->swap_layer == 0) {
      break;
    }
    const Layer *to = layer_find(doc, b->swap_layer);
    if (!to || !bone_find(*to, b->swap_bone)) {
      break;
    }
    at = {b->swap_layer, b->swap_bone};
  }
  return at;
}

uint64_t bone_counterpart(const Document &doc, uint64_t layer, uint64_t bone, const Layer &to)
{
  const auto root = bone_swap_root(doc, layer, bone);
  for (const Bone &b : to.bones) {
    if (bone_swap_root(doc, to.id, b.id) == root) {
      return b.id;
    }
  }
  return 0;
}

Layer *switch_child_armature(Layer &child)
{
  if (child.is_bone_layer()) {
    return &child;
  }
  for (Layer &l : child.children) {
    if (Layer *found = switch_child_armature(l)) {
      return found;
    }
  }
  return nullptr;
}

int switch_swaps_auto_link(Document &doc, Layer &sw)
{
  Layer *hub = nullptr;
  int made = 0;
  for (Layer &child : sw.children) {
    Layer *armature = switch_child_armature(child);
    if (!armature || armature->bones.empty()) {
      continue;
    }
    if (!hub) {
      hub = armature;
      continue;
    }
    for (Bone &b : armature->bones) {
      if (b.swap_layer != 0) {
        continue;
      }
      for (const Bone &h : hub->bones) {
        if (h.name == b.name) {
          b.swap_layer = hub->id;
          b.swap_bone = h.id;
          made++;
          break;
        }
      }
    }
  }
  (void)doc;
  return made;
}

static const BonePose *pose_of(const std::vector<BonePose> &poses, uint64_t id)
{
  for (const BonePose &p : poses) {
    if (p.id == id) {
      return &p;
    }
  }
  return nullptr;
}

int armature_match_pose(Document &doc, Layer &to, const Layer &from)
{
  const Transform2D to_world = layer_world_transform(doc, to.id);
  const Transform2D from_world = layer_world_transform(doc, from.id);
  const bool mirrored = to_world.a * to_world.d - to_world.b * to_world.c < 0.0f;
  const std::vector<BonePose> from_poses = armature_pose(from, false);
  int count = 0;
  /* Roots before their children (the order of `bones`), each worked out on its parent as matched. */
  for (Bone &b : to.bones) {
    const uint64_t partner = bone_counterpart(doc, to.id, b.id, from);
    const BonePose *fp = partner ? pose_of(from_poses, partner) : nullptr;
    if (!fp) {
      continue;
    }
    std::vector<BonePose> now = armature_pose(to, false);
    const BonePose *tp = pose_of(now, b.id);
    if (!tp) {
      continue;
    }
    const float2 want = from_world.apply_vector(fp->tail - fp->head);
    const float2 have = to_world.apply_vector(tp->tail - tp->head);
    if (length(want) > 1e-6f && length(have) > 1e-6f) {
      const float turn = std::atan2(cross(have, want), dot(have, want));
      b.angle += mirrored ? -turn : turn;
    }
    if (b.parent == 0) {
      /* A root goes where its counterpart's head is. */
      now = armature_pose(to, false);
      tp = pose_of(now, b.id);
      const float2 shift = from_world.apply(fp->head) - to_world.apply(tp->head);
      b.position = b.position + to_world.inverted().apply_vector(shift);
    }
    count++;
  }
  return count;
}

}  // namespace oanim
