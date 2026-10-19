#include "anim/deform.hh"

#include <algorithm>
#include <cmath>

#include "data/bones.hh"
#include "data/document.hh"

namespace sky {

/* --- Mirror --------------------------------------------------------------------------------- */

static VectorDrawing mirror_once(const VectorDrawing &src, int axis, const Modifier &m,
                                 const std::function<uint64_t(uint64_t)> &flip_bone)
{
  const float c = axis == 0 ? m.center.x : m.center.y;
  const auto reflect = [&](float2 p) { return axis == 0 ? float2(2.0f * c - p.x, p.y) : float2(p.x, 2.0f * c - p.y); };
  const auto reflect_vector = [&](float2 v) { return axis == 0 ? float2(-v.x, v.y) : float2(v.x, -v.y); };
  const auto on_axis = [&](float2 p) { return m.merge && std::fabs((axis == 0 ? p.x : p.y) - c) <= m.merge_distance; };

  VectorDrawing out;
  out.points = src.points;
  const int n = int(src.points.size());
  /* Where each point's reflection is: itself when it sits on the axis. */
  std::vector<int> map(n);
  for (int i = 0; i < n; i++) {
    if (on_axis(src.points[i].pos)) {
      map[i] = i;
      continue;
    }
    MeshPoint q = src.points[i];
    q.pos = reflect(q.pos);
    q.selected = false;
    if (m.mirror_bones && q.bone != 0) {
      q.bone = flip_bone(q.bone);
    }
    if (m.mirror_bones) {
      for (BoneWeight &w : q.weights) {
        w.bone = flip_bone(w.bone);
      }
    }
    q.pos_keys = {};
    q.radius_keys = {};
    map[i] = int(out.points.size());
    out.points.push_back(q);
  }
  /* A curve point's reflection; `reversed` when it runs the other way along its new curve. */
  const auto mirrored = [&](const CurvePoint &cp, bool reversed) {
    CurvePoint r = cp;
    r.point = map[cp.point];
    r.handle_in = reflect_vector(cp.handle_in);
    r.handle_out = reflect_vector(cp.handle_out);
    r.offset_in = -cp.offset_in;
    r.offset_out = -cp.offset_out;
    if (reversed) {
      std::swap(r.handle_in, r.handle_out);
      std::swap(r.weight_in, r.weight_out);
      std::swap(r.offset_in, r.offset_out);
    }
    r.select_in = r.select_out = false;
    r.edge_hidden = false;
    r.curvature_keys = {};
    r.handle_in_keys = {};
    r.handle_out_keys = {};
    return r;
  };

  const int nc = int(src.curves.size());
  /* For each curve: how its old segments are numbered now (shift) and where their reflections
   * are (on the same curve when merged, else on a separate mirrored curve). */
  struct CurveMap {
    bool merged = false;
    int shift = 0;
    int mirror_curve = -1;
    int m = 0;
    bool first_on = false;
  };
  std::vector<CurveMap> cmap(nc);
  out.curves = src.curves;
  for (int ci = 0; ci < nc; ci++) {
    const Curve &cu = src.curves[ci];
    const int mcount = int(cu.points.size());
    CurveMap &cm = cmap[ci];
    cm.m = mcount;
    if (mcount < 2) {
      continue;
    }
    const bool first_on = !cu.closed && map[cu.points.front().point] == cu.points.front().point;
    const bool last_on = !cu.closed && map[cu.points.back().point] == cu.points.back().point;
    Curve &dst = out.curves[ci];
    if (first_on && last_on) {
      /* Both ends on the axis: one closed curve, the reflection running back to the start. */
      for (int j = mcount - 2; j >= 1; j--) {
        CurvePoint r = mirrored(cu.points[j], true);
        r.edge_hidden = cu.points[j - 1].edge_hidden;
        dst.points.push_back(r);
      }
      dst.points[mcount - 1].edge_hidden = cu.points[mcount - 2].edge_hidden;
      dst.closed = true;
      cm.merged = true;
    }
    else if (last_on) {
      /* The end on the axis: the reflection continues the line from there. */
      for (int j = mcount - 2; j >= 0; j--) {
        CurvePoint r = mirrored(cu.points[j], true);
        r.edge_hidden = j >= 1 ? cu.points[j - 1].edge_hidden : false;
        dst.points.push_back(r);
      }
      dst.points[mcount - 1].edge_hidden = cu.points[mcount - 2].edge_hidden;
      cm.merged = true;
    }
    else if (first_on) {
      /* The start on the axis: the reflection comes first and leads into it. */
      std::vector<CurvePoint> lead;
      for (int j = mcount - 1; j >= 1; j--) {
        CurvePoint r = mirrored(cu.points[j], true);
        r.edge_hidden = cu.points[j - 1].edge_hidden;
        lead.push_back(r);
      }
      dst.points.insert(dst.points.begin(), lead.begin(), lead.end());
      cm.merged = true;
      cm.first_on = true;
      cm.shift = mcount - 1;
    }
    else {
      Curve copy;
      copy.closed = cu.closed;
      for (const CurvePoint &cp : cu.points) {
        CurvePoint r = mirrored(cp, false);
        r.edge_hidden = cp.edge_hidden;
        copy.points.push_back(r);
      }
      cm.mirror_curve = int(out.curves.size());
      out.curves.push_back(std::move(copy));
    }
  }
  const auto mirror_edge = [&](const ShapeEdge &e) -> ShapeEdge {
    const CurveMap &cm = cmap[e.curve];
    if (!cm.merged) {
      return {cm.mirror_curve, e.segment};
    }
    if (cm.first_on) {
      return {e.curve, cm.m - 2 - e.segment};
    }
    return {e.curve, 2 * cm.m - 3 - e.segment};
  };

  for (const Shape &shape : src.shapes) {
    bool any_merged = false;
    bool usable = true;
    for (const ShapeEdge &e : shape.edges) {
      if (e.curve < 0 || e.curve >= nc || cmap[e.curve].m < 2) {
        usable = false;
        break;
      }
      any_merged |= cmap[e.curve].merged;
    }
    if (!usable) {
      out.shapes.push_back(shape);
      continue;
    }
    if (any_merged) {
      /* The shape reaches the axis: it grows to take in its reflection. */
      Shape grown = shape;
      grown.edges.clear();
      for (const ShapeEdge &e : shape.edges) {
        grown.edges.push_back({e.curve, e.segment + cmap[e.curve].shift});
      }
      for (const ShapeEdge &e : shape.edges) {
        grown.edges.push_back(mirror_edge(e));
      }
      out.shapes.push_back(std::move(grown));
    }
    else {
      out.shapes.push_back(shape);
      Shape copy = shape;
      for (ShapeEdge &e : copy.edges) {
        e = mirror_edge(e);
      }
      out.shapes.push_back(std::move(copy));
    }
  }
  return out;
}

VectorDrawing drawing_mirror(const VectorDrawing &drawing, const Modifier &mirror,
                             const std::function<uint64_t(uint64_t)> &flip_bone)
{
  VectorDrawing out = drawing;
  if (mirror.axis_x) {
    out = mirror_once(out, 0, mirror, flip_bone);
  }
  if (mirror.axis_y) {
    out = mirror_once(out, 1, mirror, flip_bone);
  }
  return out;
}

/* --- Armature ------------------------------------------------------------------------------- */

namespace {

struct Deformer {
  const Layer *armature = nullptr;
  std::vector<BonePose> poses;
  /* Layer space <-> bone layer space. */
  Transform2D to_armature, from_armature;
  bool flexi = true;
};

}  // namespace

static bool deformer_init(const Document &doc, const Layer &layer, Deformer &r)
{
  r.armature = layer_armature(doc, layer.id);
  if (!r.armature || r.armature->bones.empty() || layer.parent_bone != 0) {
    /* A layer bound to one bone moves as a whole (its transform), not point by point. */
    return false;
  }
  r.poses = armature_pose(*r.armature, doc.frame_current == 0);
  r.to_armature = layer_world_transform(doc, r.armature->id).inverted() * layer_world_transform(doc, layer.id);
  r.from_armature = r.to_armature.inverted();
  r.flexi = layer.flexi_binding;
  return true;
}

/* Which bones move a point and how much (summing to 1, or empty: it doesn't move). */
static std::vector<BoneWeight> point_weights(const Deformer &d, const MeshPoint &p)
{
  std::vector<BoneWeight> weights;
  if (!p.weights.empty()) {
    /* Painted: the weights themselves, shared out to add up to 1. */
    float total = 0.0f;
    for (const BoneWeight &w : p.weights) {
      if (w.weight > 0.0f && bone_pose_find(d.poses, w.bone)) {
        weights.push_back(w);
        total += w.weight;
      }
    }
    for (BoneWeight &w : weights) {
      w.weight /= total;
    }
    return weights;
  }
  if (p.bone != 0 && bone_pose_find(d.poses, p.bone)) {
    return {{p.bone, 1.0f}};
  }
  if (!d.flexi) {
    return weights;
  }
  /* Flexi-binding: every bone pulls, the nearer (in the setup pose) the harder; a bone's
   * strength widens its reach. */
  const float2 q = d.to_armature.apply(p.pos);
  float total = 0.0f;
  for (size_t i = 0; i < d.poses.size(); i++) {
    const BonePose &bp = d.poses[i];
    const Bone &bone = d.armature->bones[i];
    const float reach = std::max(bone.strength * std::max(distance(bp.setup_head, bp.setup_tail), 1e-3f), 1e-4f);
    const float dist = distance_to_segment(q, bp.setup_head, bp.setup_tail) / reach;
    const float w = 1.0f / std::pow(dist * dist + 1e-4f, 2.0f);
    weights.push_back({bone.id, w});
    total += w;
  }
  for (BoneWeight &w : weights) {
    w.weight /= total;
  }
  return weights;
}

/* A point's move from its setup position, in layer space. */
static Transform2D point_skin(const Deformer &d, const MeshPoint &p)
{
  const std::vector<BoneWeight> weights = point_weights(d, p);
  if (weights.empty()) {
    return {};
  }
  Transform2D blended{0, 0, 0, 0, 0, 0};
  for (const BoneWeight &w : weights) {
    const Transform2D &skin = bone_pose_find(d.poses, w.bone)->skin;
    blended.a += skin.a * w.weight;
    blended.b += skin.b * w.weight;
    blended.c += skin.c * w.weight;
    blended.d += skin.d * w.weight;
    blended.tx += skin.tx * w.weight;
    blended.ty += skin.ty * w.weight;
  }
  return d.from_armature * blended * d.to_armature;
}

static void deform(const Deformer &d, VectorDrawing &drawing)
{
  std::vector<Transform2D> skins(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    skins[i] = point_skin(d, drawing.points[i]);
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      /* Stored handles turn and stretch with their point. */
      cp.handle_in = skins[cp.point].apply_vector(cp.handle_in);
      cp.handle_out = skins[cp.point].apply_vector(cp.handle_out);
    }
  }
  for (size_t i = 0; i < drawing.points.size(); i++) {
    drawing.points[i].pos = skins[i].apply(drawing.points[i].pos);
  }
}

static bool modifier_on(const Modifier &m, bool render)
{
  return render ? m.show_render : m.show_viewport;
}

bool layer_has_modifier_effect(const Document &doc, const Layer &layer, bool render)
{
  for (const Modifier &m : layer.modifiers) {
    if (!modifier_on(m, render)) {
      continue;
    }
    if (m.type == ModifierType::Mirror && (m.axis_x || m.axis_y)) {
      return true;
    }
    if (m.type == ModifierType::Armature && layer.parent_bone == 0) {
      const Layer *armature = layer_armature(doc, layer.id);
      if (armature && !armature->bones.empty()) {
        return true;
      }
    }
  }
  return false;
}

VectorDrawing layer_evaluated_drawing(const Document &doc, const Layer &layer, bool render)
{
  VectorDrawing drawing = layer_drawing(doc, layer);
  const Layer *armature = layer_armature(doc, layer.id);
  const auto flip = [armature](uint64_t bone) { return armature ? bone_mirror_id(*armature, bone) : bone; };
  for (const Modifier &m : layer.modifiers) {
    if (!modifier_on(m, render)) {
      continue;
    }
    if (m.type == ModifierType::Mirror) {
      drawing = drawing_mirror(drawing, m, flip);
    }
    else if (m.type == ModifierType::Armature) {
      Deformer d;
      if (deformer_init(doc, layer, d)) {
        deform(d, drawing);
      }
    }
  }
  return drawing;
}

std::vector<Transform2D> layer_point_skins(const Document &doc, const Layer &layer)
{
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  std::vector<Transform2D> skins(drawing.points.size());
  const bool on = std::any_of(layer.modifiers.begin(), layer.modifiers.end(), [](const Modifier &m) {
    return m.type == ModifierType::Armature && m.show_viewport;
  });
  Deformer d;
  if (on && deformer_init(doc, layer, d)) {
    for (size_t i = 0; i < drawing.points.size(); i++) {
      skins[i] = point_skin(d, drawing.points[i]);
    }
  }
  return skins;
}

std::vector<float> layer_point_bone_weights(const Document &doc, const Layer &layer, uint64_t bone)
{
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  std::vector<float> result(drawing.points.size(), 0.0f);
  Deformer d;
  if (!deformer_init(doc, layer, d)) {
    return result;
  }
  for (size_t i = 0; i < drawing.points.size(); i++) {
    for (const BoneWeight &w : point_weights(d, drawing.points[i])) {
      if (w.bone == bone) {
        result[i] = w.weight;
      }
    }
  }
  return result;
}

int layer_weight_paint(const Document &doc, Layer &layer, const WeightPaintBrush &brush)
{
  Deformer d;
  if (brush.radius <= 0.0f || !deformer_init(doc, layer, d) || !bone_pose_find(d.poses, brush.bone)) {
    return 0;
  }
  int changed = 0;
  for (size_t index = 0; index < layer.drawing.points.size(); index++) {
    MeshPoint &p = layer.drawing.points[index];
    const bool shown = brush.positions && index < brush.positions->size();
    const float dist = distance(shown ? (*brush.positions)[index] : p.pos, brush.center);
    if (dist > brush.radius) {
      continue;
    }
    /* Blender's smooth falloff. */
    const float x = 1.0f - dist / brush.radius;
    const float falloff = x * x * (3.0f - 2.0f * x);
    const float amount = std::clamp(brush.strength, 0.0f, 1.0f) * falloff;
    if (p.weights.empty()) {
      p.weights = point_weights(d, p);
    }
    auto it = std::find_if(p.weights.begin(), p.weights.end(), [&](const BoneWeight &w) { return w.bone == brush.bone; });
    if (it == p.weights.end()) {
      p.weights.push_back({brush.bone, 0.0f});
      it = std::prev(p.weights.end());
    }
    const float before = it->weight;
    float after = before;
    switch (brush.mode) {
      case WeightPaintMode::Draw:
        after = before + (std::clamp(brush.weight, 0.0f, 1.0f) - before) * amount;
        break;
      case WeightPaintMode::Add:
        after = std::min(1.0f, before + brush.weight * amount);
        break;
      case WeightPaintMode::Subtract:
        after = std::max(0.0f, before - brush.weight * amount);
        break;
    }
    it->weight = after;
    if (brush.normalize) {
      /* The other bones share what's left, keeping their proportions. */
      float others = 0.0f;
      for (const BoneWeight &w : p.weights) {
        others += w.bone == brush.bone ? 0.0f : w.weight;
      }
      const float rest = 1.0f - after;
      for (BoneWeight &w : p.weights) {
        if (w.bone == brush.bone) {
          continue;
        }
        w.weight = others > 1e-6f ? w.weight / others * rest : 0.0f;
      }
    }
    std::erase_if(p.weights, [](const BoneWeight &w) { return w.weight <= 1e-5f; });
    if (p.weights.empty()) {
      /* Painted down to nothing: the point stays put rather than going back to flexi. */
      p.weights.push_back({brush.bone, 0.0f});
    }
    changed += after != before ? 1 : 0;
  }
  return changed;
}

}  // namespace sky
