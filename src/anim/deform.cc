#include "anim/deform.hh"

#include <algorithm>
#include <cmath>

#include "anim/reference.hh"
#include "data/bones.hh"
#include "data/detail.hh"
#include "data/layer.hh"
#include "core/log.hh"
#include "data/document.hh"

namespace oanim {

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
  /* Parent Bone moving the points (Deform: Points): every point all this bone's, whatever its
   * weights. */
  uint64_t whole_bone = 0;
};

}  // namespace

static bool deformer_init(const Document &doc, const Layer &layer, Deformer &r)
{
  r.armature = layer_armature(doc, layer.id);
  if (!r.armature || r.armature->bones.empty() || layer.followed_bone() != 0 || layer_binding_group(doc, layer.id)) {
    /* A layer bound to one bone (or inside a group that is) moves as a whole (its transform), not
     * point by point. */
    return false;
  }
  r.poses = armature_pose(*r.armature, doc.frame_current == 0);
  r.to_armature = layer_world_transform(doc, r.armature->id).inverted() * layer_world_transform(doc, layer.id);
  r.from_armature = r.to_armature.inverted();
  r.whole_bone = layer.points_follow_bone() ? layer.parent_bone : 0;
  return true;
}

/* Which bones move a point and how much (summing to 1, or empty: it doesn't move). */
static std::vector<BoneWeight> point_weights(const Deformer &d, const MeshPoint &p)
{
  if (d.whole_bone != 0) {
    return bone_pose_find(d.poses, d.whole_bone) ? std::vector<BoneWeight>{{d.whole_bone, 1.0f}}
                                                 : std::vector<BoneWeight>{};
  }
  std::vector<BoneWeight> weights;
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

/* Where a point is in the setup pose (layer space). */
static float2 setup_position(const MeshPoint &p)
{
  return point_setup_pos(p);
}

/* Blender's Automatic Weights, for 2D: every bone pulls, the nearer (in the setup pose) the harder,
 * and a bone's strength widens its reach (Moho's flexi-binding, worked out once and kept).
 * `bones` limits it to some bones (empty: all). Pruned, a point keeps its 4 strongest bones and
 * drops weights under 0.01, as Blender does. */
static std::vector<BoneWeight> automatic_weights(const Deformer &d, const MeshPoint &p,
                                                 const std::vector<uint64_t> &bones, bool prune)
{
  const float2 q = d.to_armature.apply(setup_position(p));
  std::vector<BoneWeight> weights;
  float total = 0.0f;
  for (size_t i = 0; i < d.poses.size(); i++) {
    const BonePose &bp = d.poses[i];
    const Bone &bone = d.armature->bones[i];
    if (!bones.empty() && std::find(bones.begin(), bones.end(), bone.id) == bones.end()) {
      continue;
    }
    const float reach = std::max(bone.strength * std::max(distance(bp.setup_head, bp.setup_tail), 1e-3f), 1e-4f);
    const float dist = distance_to_segment(q, bp.setup_head, bp.setup_tail) / reach;
    const float w = 1.0f / std::pow(dist * dist + 1e-4f, 2.0f);
    weights.push_back({bone.id, w});
    total += w;
  }
  if (total <= 0.0f) {
    return {};
  }
  for (BoneWeight &w : weights) {
    w.weight /= total;
  }
  if (prune) {
    std::sort(weights.begin(), weights.end(), [](const BoneWeight &a, const BoneWeight &b) { return a.weight > b.weight; });
    if (weights.size() > 4) {
      weights.resize(4);
    }
    std::erase_if(weights, [](const BoneWeight &w) { return w.weight < 0.01f; });
    float kept = 0.0f;
    for (const BoneWeight &w : weights) {
      kept += w.weight;
    }
    for (BoneWeight &w : weights) {
      w.weight /= kept;
    }
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

/* A reference with Match Original on: its original, whose modifiers and bones shape it. */
static const Layer *matched_original(const Document &doc, const Layer &layer)
{
  if (!layer.is_reference() || !layer.reference.match_original) {
    return nullptr;
  }
  return layer_find(doc, layer.reference_source);
}

bool layer_has_modifier_effect(const Document &doc, const Layer &layer, bool render)
{
  if (matched_original(doc, layer)) {
    return true;
  }
  /* Detail shows its handles final (data/detail.hh). */
  if (layer.type == LayerType::Vector && drawing_has_detail(layer_drawing(doc, layer))) {
    return true;
  }
  for (const Modifier &m : layer.modifiers) {
    if (!modifier_on(m, render)) {
      continue;
    }
    if (m.type == ModifierType::Mirror && (m.axis_x || m.axis_y)) {
      return true;
    }
    if (m.type == ModifierType::Armature && layer.followed_bone() == 0 && !layer_binding_group(doc, layer.id)) {
      const Layer *armature = layer_armature(doc, layer.id);
      if (armature && !armature->bones.empty()) {
        return true;
      }
    }
  }
  return false;
}

/* Moves a drawing's points and handles by `t`. */
static void transform_drawing(VectorDrawing &drawing, const Transform2D &t)
{
  for (MeshPoint &p : drawing.points) {
    p.pos = t.apply(p.pos);
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.handle_in = t.apply_vector(cp.handle_in);
      cp.handle_out = t.apply_vector(cp.handle_out);
    }
  }
}

VectorDrawing layer_evaluated_drawing(const Document &doc, const Layer &layer, bool render)
{
  if (const Layer *original = matched_original(doc, layer)) {
    /* Match Original: shaped by the original's modifiers and posed by its bones, where it is. */
    VectorDrawing drawing = layer.drawing;
    const Layer *bones = layer_armature(doc, original->id);
    const auto flip = [bones](uint64_t bone) { return bones ? bone_mirror_id(*bones, bone) : bone; };
    for (const Modifier &m : original->modifiers) {
      if (!modifier_on(m, render)) {
        continue;
      }
      if (m.type == ModifierType::Mirror) {
        drawing = drawing_mirror(drawing, m, flip);
      }
      else if (m.type == ModifierType::Armature) {
        Deformer d;
        if (deformer_init(doc, *original, d)) {
          deform(d, drawing);
        }
      }
    }
    transform_drawing(drawing, reference_match_transform(doc, layer));
    drawing_detail_final_handles(drawing);
    return drawing;
  }
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
  /* Handles scale as the bones and Mirror move them, so the final ones can come last. */
  drawing_detail_final_handles(drawing);
  return drawing;
}

VectorDrawing layer_setup_drawing(const Document &doc, const Layer &layer, bool render)
{
  if (layer.is_reference()) {
    /* As built on the setup frame; matched, with the original's Mirror and where it is. */
    VectorDrawing drawing = reference_build(doc, layer, true);
    if (const Layer *original = matched_original(doc, layer)) {
      const Layer *bones = layer_armature(doc, original->id);
      const auto flip = [bones](uint64_t bone) { return bones ? bone_mirror_id(*bones, bone) : bone; };
      for (const Modifier &m : original->modifiers) {
        if (modifier_on(m, render) && m.type == ModifierType::Mirror) {
          drawing = drawing_mirror(drawing, m, flip);
        }
      }
      transform_drawing(drawing, reference_match_transform(doc, layer));
      drawing_detail_final_handles(drawing);
      return drawing;
    }
    const Layer *armature = layer_armature(doc, layer.id);
    const auto flip = [armature](uint64_t bone) { return armature ? bone_mirror_id(*armature, bone) : bone; };
    for (const Modifier &m : layer.modifiers) {
      if (modifier_on(m, render) && m.type == ModifierType::Mirror) {
        drawing = drawing_mirror(drawing, m, flip);
      }
    }
    drawing_detail_final_handles(drawing);
    return drawing;
  }
  VectorDrawing drawing = drawing_setup_pose(layer_drawing(doc, layer));
  const Layer *armature = layer_armature(doc, layer.id);
  const auto flip = [armature](uint64_t bone) { return armature ? bone_mirror_id(*armature, bone) : bone; };
  for (const Modifier &m : layer.modifiers) {
    if (modifier_on(m, render) && m.type == ModifierType::Mirror) {
      drawing = drawing_mirror(drawing, m, flip);
    }
  }
  drawing_detail_final_handles(drawing);
  return drawing;
}

std::vector<Transform2D> layer_point_skins(const Document &doc, const Layer &layer)
{
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  std::vector<Transform2D> skins(drawing.points.size());
  if (const Layer *original = matched_original(doc, layer)) {
    /* Match Original: posed by the original's bones, then put where the original is. */
    const Transform2D match = reference_match_transform(doc, layer);
    Deformer d;
    const bool posed = std::any_of(original->modifiers.begin(), original->modifiers.end(),
                                   [](const Modifier &m) { return m.type == ModifierType::Armature && m.show_viewport; }) &&
                       deformer_init(doc, *original, d);
    for (size_t i = 0; i < drawing.points.size(); i++) {
      skins[i] = posed ? match * point_skin(d, drawing.points[i]) : match;
    }
    return skins;
  }
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

/* A layer whose points get weights follows them, not a parent bone. */
static void weights_take_over(Layer &layer)
{
  if (layer.parent_bone != 0) {
    layer.parent_bone = 0;
    report(ReportType::Info, "\"" + layer.name + "\" now follows its weights instead of its parent bone");
  }
}

int layer_weight_paint(const Document &doc, Layer &layer, const WeightPaintBrush &brush)
{
  if (brush.radius > 0.0f && brush.bone != 0) {
    weights_take_over(layer);
  }
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
    if (brush.selected_only && !p.selected) {
      continue;
    }
    if (p.weights.empty()) {
      p.weights = automatic_weights(d, p, {}, true);
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
      /* Painted down to nothing: the point stays put rather than getting automatic weights. */
      p.weights.push_back({brush.bone, 0.0f});
    }
    changed += after != before ? 1 : 0;
  }
  return changed;
}

static bool weightable(const Document &doc, const Layer &layer, Deformer &d)
{
  return layer.type == LayerType::Vector && deformer_init(doc, layer, d);
}

int layer_automatic_weights(const Document &doc, Layer &layer, const std::vector<uint64_t> &bones,
                            bool selected_only)
{
  if (layer.type == LayerType::Vector) {
    weights_take_over(layer);
  }
  Deformer d;
  if (!weightable(doc, layer, d)) {
    return 0;
  }
  int changed = 0;
  for (MeshPoint &p : layer.drawing.points) {
    if (selected_only && !p.selected) {
      continue;
    }
    p.weights = automatic_weights(d, p, bones, true);
    if (p.weights.empty()) {
      p.weights.push_back({bones.empty() ? 0 : bones.front(), 0.0f});
    }
    changed++;
  }
  return changed;
}

/* The weights of the nearest weighted points along the curves through point `index`, blended by
 * how many steps away they are; empty when its curves have none. */
static std::vector<BoneWeight> neighbour_weights(const VectorDrawing &drawing, int index,
                                                 const std::vector<bool> &weighted)
{
  std::vector<std::pair<int, int>> found;  // (point, steps)
  for (const Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    for (int at = 0; at < n; at++) {
      if (curve.points[at].point != index) {
        continue;
      }
      for (const int dir : {-1, 1}) {
        for (int step = 1; step < n; step++) {
          int k = at + dir * step;
          if (curve.closed) {
            k = ((k % n) + n) % n;
          }
          else if (k < 0 || k >= n) {
            break;
          }
          const int point = curve.points[k].point;
          if (weighted[point]) {
            found.push_back({point, step});
            break;
          }
        }
      }
    }
  }
  std::vector<BoneWeight> blended;
  float total = 0.0f;
  for (const auto &[point, steps] : found) {
    const float share = 1.0f / float(steps);
    total += share;
    for (const BoneWeight &w : drawing.points[point].weights) {
      auto it = std::find_if(blended.begin(), blended.end(), [&](const BoneWeight &b) { return b.bone == w.bone; });
      if (it == blended.end()) {
        blended.push_back({w.bone, 0.0f});
        it = std::prev(blended.end());
      }
      it->weight += w.weight * share;
    }
  }
  for (BoneWeight &w : blended) {
    w.weight /= total;
  }
  return blended;
}

static bool weigh_new_points(const Document &doc, Layer &layer, bool exact)
{
  /* A reference matching its original moves by the original's weights. */
  if (layer.is_reference() && layer.reference.match_original) {
    return false;
  }
  Deformer d;
  if (!weightable(doc, layer, d)) {
    return false;
  }
  VectorDrawing &drawing = layer.drawing;
  std::vector<bool> weighted(drawing.points.size());
  bool any_new = false;
  for (size_t i = 0; i < drawing.points.size(); i++) {
    weighted[i] = !drawing.points[i].weights.empty();
    any_new |= !weighted[i];
  }
  if (!any_new) {
    return false;
  }
  std::vector<std::vector<BoneWeight>> found(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (weighted[i]) {
      continue;
    }
    found[i] = exact ? std::vector<BoneWeight>() : neighbour_weights(drawing, int(i), weighted);
    if (found[i].empty()) {
      found[i] = automatic_weights(d, drawing.points[i], {}, !exact);
    }
    if (found[i].empty()) {
      found[i].push_back({0, 0.0f});
    }
  }
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (!weighted[i]) {
      drawing.points[i].weights = std::move(found[i]);
    }
  }
  return true;
}

bool document_weight_new_points(Document &doc, bool exact)
{
  bool changed = false;
  const auto visit = [&](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      changed |= weigh_new_points(doc, layer, exact);
      self(self, layer.children);
    }
  };
  visit(visit, doc.layers);
  return changed;
}

}  // namespace oanim
