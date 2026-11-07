#include "anim/shape_limit.hh"

#include <algorithm>

#include "anim/deform.hh"
#include "anim/follow_point.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "core/profile.hh"

namespace oanim {

/* A shape's outline as drawn (layer space): `id`, or the first closed one when 0. */
static std::vector<float2> drawn_outline(const VectorDrawing &drawing, uint64_t id)
{
  for (const Shape &shape : drawing.shapes) {
    if (id != 0 && shape.id != id) {
      continue;
    }
    std::vector<float2> points;
    bool closed = false;
    if (shape_outline(drawing, shape, 12, points, nullptr, &closed) && closed && points.size() >= 3) {
      return points;
    }
    if (id != 0) {
      break;
    }
  }
  return {};
}

/* The convex hull (Andrew's monotone chain): a footprint keeps its outline, fewer points to test. */
static std::vector<float2> convex_hull(std::vector<float2> p)
{
  if (p.size() < 3) {
    return p;
  }
  std::sort(p.begin(), p.end(), [](float2 a, float2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
  const auto cross = [](float2 o, float2 a, float2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
  std::vector<float2> h(2 * p.size());
  size_t k = 0;
  for (size_t i = 0; i < p.size(); i++) {
    while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0.0f) {
      k--;
    }
    h[k++] = p[i];
  }
  for (size_t i = p.size() - 1, t = k + 1; i > 0; i--) {
    while (k >= t && cross(h[k - 2], h[k - 1], p[i - 1]) <= 0.0f) {
      k--;
    }
    h[k++] = p[i - 1];
  }
  h.resize(k > 1 ? k - 1 : k);
  return h;
}

void shape_limits_update(Document &doc)
{
  OANIM_PROFILE("shape_limits_update");
  for (const Layer *rig_c : document_layers_flat(doc)) {
    if (!rig_c->is_bone_layer()) {
      continue;
    }
    Layer &rig = *layer_find(doc, rig_c->id);
    bool any = false;
    for (const Bone &b : rig.bones) {
      for (const BoneConstraint &c : b.constraints) {
        any = any || c.type == BoneConstraintType::LimitShape || c.type == BoneConstraintType::LookAt;
      }
    }
    if (!any) {
      continue;
    }
    const Transform2D to_rig = layer_world_transform(doc, rig.id).inverted();
    for (Bone &b : rig.bones) {
      for (BoneConstraint &c : b.constraints) {
        c.shape_error.clear();
        /* Limit to Shape, and Look At with its own eye shape. */
        if (c.type != BoneConstraintType::LimitShape && !(c.type == BoneConstraintType::LookAt && c.shape_layer != 0)) {
          if (c.type == BoneConstraintType::LookAt) {
            c.shape_region.clear();
            c.shape_footprint.clear();
            c.look_reach = {};
          }
          continue;
        }
        const Layer *target = layer_find(doc, c.shape_layer);
        const bool loop = target && target->has_drawing() && layer_moved_by_bone(doc, *target, rig.id, b.id);
        if (loop) {
          c.shape_error = c.type == BoneConstraintType::LookAt ? "Loop: this bone moves its Eye Shape"
                                                               : "Loop: this bone moves the shape it stays inside";
        }
        if (!target || !target->has_drawing() || !c.enabled || loop) {
          c.shape_region.clear();
          c.shape_footprint.clear();
          c.look_reach = {};
          continue;
        }
        const Transform2D to = to_rig * layer_world_transform(doc, target->id);
        std::vector<float2> region;
        OANIM_PROFILE("shape_limit_region");
        for (const float2 p : drawn_outline(layer_evaluated_drawing(doc, *target, false), c.shape_id)) {
          region.push_back(to.apply(p));
        }
        if (c.type == BoneConstraintType::LookAt) {
          /* The ball the pupil sits on: half the eye shape's width and height at frame 0, as drawn there. */
          OANIM_PROFILE("shape_limit_reach");
          const Transform2D rest = layer_setup_world_transform(doc, rig.id).inverted() *
                                   layer_setup_world_transform(doc, target->id);
          float2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
          for (const float2 p : drawn_outline(layer_setup_drawing(doc, *target, false), c.shape_id)) {
            const float2 q = rest.apply(p);
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y)};
          }
          c.look_reach = hi.x > lo.x ? (hi - lo) * 0.5f : float2();
        }
        if (c.shape_fit != ShapeFit::Whole) {
          c.shape_region = std::move(region);
          c.shape_footprint.clear();
          continue;
        }
        /* What the bone carries (the fit layer, or the drawings following this bone) around its head,
         * worked out with the pose those drawings are drawn with: the limits as they were until now. */
        const std::vector<BonePose> now = armature_pose(rig, doc.frame_current == 0);
        const BonePose *pose = bone_pose_find(now, b.id);
        if (!pose) {
          continue;
        }
        std::vector<float2> carried;
        OANIM_PROFILE("shape_limit_footprint");
        for (const Layer *l : document_layers_flat(doc)) {
          const bool picked = c.fit_layer != 0 ? l->id == c.fit_layer : l->parent_bone == b.id;
          if (!picked || !l->has_drawing() || !l->shown_in_render() || layer_armature(doc, l->id) != &rig) {
            continue;
          }
          const VectorDrawing drawn = layer_evaluated_drawing(doc, *l, false);
          const Transform2D lt = to_rig * layer_world_transform(doc, l->id);
          for (const Shape &shape : drawn.shapes) {
            std::vector<float2> points;
            bool closed = false;
            if (shape.hidden || !shape_outline(drawn, shape, 8, points, nullptr, &closed)) {
              continue;
            }
            for (const float2 p : points) {
              carried.push_back(lt.apply(p) - pose->head);
            }
          }
        }
        c.shape_region = std::move(region);
        c.shape_footprint = convex_hull(carried);
      }
    }
  }
}

}  // namespace oanim
