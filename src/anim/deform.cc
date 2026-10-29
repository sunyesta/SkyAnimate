#include "anim/deform.hh"

#include <algorithm>
#include <cmath>

#include "anim/bevel.hh"
#include "anim/merge.hh"
#include "anim/subdivide.hh"
#include "anim/vector_deform.hh"
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
  /* From the groups the two share down, so a shared group moving (the character walked, its top group scaled) leaves
   * the bent drawing exactly as it was, and what is kept of it (a deform's build, the drawing's triangles) stays. */
  r.to_armature = layer_relative_transform(doc, r.armature->id, layer.id);
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
/* Preserve Volume: the bones' skins blended as rigid motions (dual complex numbers, the 2D form of
 * Blender's dual quaternions), their stretch blended apart and applied first, so a point moved by bones
 * turned far apart keeps its distance instead of being pulled in toward them. */
static Transform2D skin_preserve_volume(const Deformer &d, const std::vector<BoneWeight> &weights)
{
  float k11 = 0.0f, k12 = 0.0f, k21 = 0.0f, k22 = 0.0f;
  float q0 = 0.0f, q1 = 0.0f, q2 = 0.0f, q3 = 0.0f;
  float first0 = 0.0f, first1 = 0.0f;
  bool first = true;
  for (const BoneWeight &w : weights) {
    const Transform2D &skin = bone_pose_find(d.poses, w.bone)->skin;
    const float angle = std::atan2(skin.b, skin.a);
    const float co = std::cos(angle), si = std::sin(angle);
    /* The stretch left once the turn is taken out. */
    k11 += w.weight * (co * skin.a + si * skin.b);
    k21 += w.weight * (-si * skin.a + co * skin.b);
    k12 += w.weight * (co * skin.c + si * skin.d);
    k22 += w.weight * (-si * skin.c + co * skin.d);
    /* The turn and move as a dual complex number: half-angle rotation, half the move turned back. */
    float r0 = std::cos(angle * 0.5f), r1 = std::sin(angle * 0.5f);
    float d0 = 0.5f * (r0 * skin.tx + r1 * skin.ty), d1 = 0.5f * (r0 * skin.ty - r1 * skin.tx);
    if (first) {
      first0 = r0, first1 = r1;
      first = false;
    }
    else if (r0 * first0 + r1 * first1 < 0.0f) {
      /* The same turn the short way round. */
      r0 = -r0, r1 = -r1, d0 = -d0, d1 = -d1;
    }
    q0 += w.weight * r0;
    q1 += w.weight * r1;
    q2 += w.weight * d0;
    q3 += w.weight * d1;
  }
  const float norm = std::sqrt(q0 * q0 + q1 * q1);
  if (norm < 1e-8f) {
    return {};
  }
  q0 /= norm, q1 /= norm, q2 /= norm, q3 /= norm;
  const float angle = 2.0f * std::atan2(q1, q0);
  const float co = std::cos(angle), si = std::sin(angle);
  Transform2D out;
  out.a = co * k11 - si * k21;
  out.b = si * k11 + co * k21;
  out.c = co * k12 - si * k22;
  out.d = si * k12 + co * k22;
  out.tx = 2.0f * (q0 * q2 - q1 * q3);
  out.ty = 2.0f * (q1 * q2 + q0 * q3);
  return out;
}

static Transform2D point_skin(const Deformer &d, const MeshPoint &p)
{
  const std::vector<BoneWeight> weights = point_weights(d, p);
  if (weights.empty()) {
    return {};
  }
  if (p.preserve_volume && weights.size() > 1) {
    return d.from_armature * skin_preserve_volume(d, weights) * d.to_armature;
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

/* Whether every skin is the same turn and move (no stretch): the bones carry the whole drawing as one
 * piece, and `r_move` is how. */
static bool skins_rigid(const std::vector<Transform2D> &skins, Transform2D &r_move)
{
  if (skins.empty()) {
    return false;
  }
  const Transform2D &t = skins.front();
  constexpr float eps = 1e-5f;
  if (std::abs(t.a * t.a + t.b * t.b - 1.0f) > eps * 10.0f || std::abs(t.c * t.c + t.d * t.d - 1.0f) > eps * 10.0f ||
      std::abs(t.a * t.c + t.b * t.d) > eps * 10.0f)
  {
    return false;
  }
  for (const Transform2D &o : skins) {
    if (std::abs(o.a - t.a) > eps || std::abs(o.b - t.b) > eps || std::abs(o.c - t.c) > eps ||
        std::abs(o.d - t.d) > eps || std::abs(o.tx - t.tx) > eps || std::abs(o.ty - t.ty) > eps)
    {
      return false;
    }
  }
  r_move = t;
  return true;
}

/* Moves the drawing's points and handles as the bones have them. With `r_rigid`, a drawing the bones
 * carry as one piece (only turned and moved) is left as it is and the move goes there instead. */
static void deform(const Deformer &d, VectorDrawing &drawing, Transform2D *r_rigid = nullptr)
{
  std::vector<Transform2D> skins(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    skins[i] = point_skin(d, drawing.points[i]);
  }
  if (r_rigid && skins_rigid(skins, *r_rigid)) {
    return;
  }
  std::vector<float2> moved(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    moved[i] = skins[i].apply(drawing.points[i].pos);
  }
  for (Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    for (int k = 0; k < n; k++) {
      CurvePoint &cp = curve.points[size_t(k)];
      if (cp.handles_follow_curve && n > 1) {
        /* Turned and stretched as the line through its neighbours is (Resample's handles for points
         * whose handles the curve works out). */
        const int prev = curve.closed ? (k - 1 + n) % n : std::max(k - 1, 0);
        const int next = curve.closed ? (k + 1) % n : std::min(k + 1, n - 1);
        const int a = curve.points[size_t(prev)].point, b = curve.points[size_t(next)].point;
        const float2 was = drawing.points[size_t(b)].pos - drawing.points[size_t(a)].pos;
        const float2 now = moved[size_t(b)] - moved[size_t(a)];
        const float len_was = length(was);
        if (len_was > 1e-9f) {
          const float turn = std::atan2(was.x * now.y - was.y * now.x, dot(was, now));
          const float stretch = length(now) / len_was;
          const float co = std::cos(turn) * stretch, si = std::sin(turn) * stretch;
          const auto follow = [&](float2 h) { return float2{co * h.x - si * h.y, si * h.x + co * h.y}; };
          cp.handle_in = follow(cp.handle_in);
          cp.handle_out = follow(cp.handle_out);
          continue;
        }
      }
      /* Stored handles turn and stretch with their point. */
      cp.handle_in = skins[cp.point].apply_vector(cp.handle_in);
      cp.handle_out = skins[cp.point].apply_vector(cp.handle_out);
    }
  }
  for (size_t i = 0; i < drawing.points.size(); i++) {
    drawing.points[i].pos = moved[i];
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

static bool look_warp_find(const Document &doc, const Layer &layer, BonePose *r_pose = nullptr,
                           Transform2D *r_to_armature = nullptr, bool *r_carried = nullptr);

bool layer_has_modifier_effect(const Document &doc, const Layer &layer, bool render)
{
  if (matched_original(doc, layer)) {
    return true;
  }
  /* Detail shows its handles final (data/detail.hh). */
  if (layer.type == LayerType::Vector && drawing_has_detail(layer_drawing(doc, layer))) {
    return true;
  }
  if (layer.type == LayerType::Vector && merge_takes_from(doc, layer, render)) {
    return true;
  }
  if (layer.type == LayerType::Vector && look_warp_find(doc, layer)) {
    return true;
  }
  for (const Modifier &m : layer.modifiers) {
    if (!modifier_on(m, render)) {
      continue;
    }
    if (m.type == ModifierType::Mirror && (m.axis_x || m.axis_y)) {
      return true;
    }
    if (m.type == ModifierType::Boolean || (m.type == ModifierType::Bevel && m.bevel_amount > 0.0f) ||
        m.type == ModifierType::Resample || (modifier_is_deform(m.type) && m.deform_layer != 0))
    {
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

/* Look At's warp: a layer following a pupil bone (or in a group that does) is laid on its eyeball
 * (bone_look_warp); `r_carried`: one left as drawn (a shine) rides on it whole, keeping its shape.
 * Without `r_pose`, only whether the bone has a Look At. */
static bool look_warp_find(const Document &doc, const Layer &layer, BonePose *r_pose, Transform2D *r_to_armature,
                           bool *r_carried)
{
  /* Following it as a whole or point by point (Deform: Points), or in a group that follows it. */
  const uint64_t own = layer.followed_bone() != 0 ? layer.followed_bone() : layer.points_follow_bone() ? layer.parent_bone : 0;
  const Layer *group = own != 0 ? nullptr : layer_binding_group(doc, layer.id);
  const uint64_t bone_id = group ? group->followed_bone() : own;
  const Layer *armature = bone_id != 0 ? layer_armature(doc, layer.id) : nullptr;
  const Bone *bone = armature ? bone_find(*armature, bone_id) : nullptr;
  if (!bone || std::none_of(bone->constraints.begin(), bone->constraints.end(), [](const BoneConstraint &c) {
        return c.type == BoneConstraintType::LookAt && c.enabled;
      }))
  {
    return false;
  }
  if (!r_pose) {
    return true;
  }
  const std::vector<BonePose> poses = armature_pose(*armature, doc.frame_current == 0);
  const BonePose *p = bone_pose_find(poses, bone_id);
  if (!p || !p->look) {
    return false;
  }
  *r_carried = !bone_look_warps(*armature, *p, layer.id, group ? group->id : 0);
  *r_pose = *p;
  *r_to_armature = layer_relative_transform(doc, armature->id, layer.id);
  return true;
}

/* The warp onto the eyeball `p` about one point (layer space, moved to bone layer space by
 * `to_armature`), as a skin: where the point goes, and how its handles turn and shorten there. */
static Transform2D look_warp_skin(const BonePose &p, const Transform2D &to_armature, float2 point)
{
  const Transform2D from_armature = to_armature.inverted();
  const auto warp = [&](float2 q) { return from_armature.apply(bone_look_warp(p, to_armature.apply(q))); };
  const float step = std::max(p.look_size.x, p.look_size.y) * 1e-3f / std::max(to_armature.scale(), 1e-9f);
  const float2 at = warp(point);
  const float2 dx = (warp(point + float2{step, 0}) - warp(point - float2{step, 0})) / (2.0f * step);
  const float2 dy = (warp(point + float2{0, step}) - warp(point - float2{0, step})) / (2.0f * step);
  const Transform2D local{dx.x, dx.y, dy.x, dy.y, 0.0f, 0.0f};
  return Transform2D::translate(at) * local * Transform2D::translate(float2{} - point);
}

static std::vector<Transform2D> layer_point_skins_bones(const Document &doc, const Layer &layer);

/* The oval the warped drawings following the eye's bone (pupil, iris) make of themselves: the
 * affine map from where their points are before the warp to after it that fits best (least squares,
 * bone layer space). Identity when there are none. */
static Transform2D look_oval(const Document &doc, const BonePose &eye)
{
  std::vector<std::pair<float2, float2>> pairs;
  for (const Layer *l : document_layers_flat(doc)) {
    BonePose e;
    Transform2D to_armature;
    bool carried = false;
    if (l->type != LayerType::Vector || !look_warp_find(doc, *l, &e, &to_armature, &carried) || carried ||
        e.id != eye.id)
    {
      continue;
    }
    const VectorDrawing &drawing = layer_drawing(doc, *l);
    const std::vector<Transform2D> skins = layer_point_skins_bones(doc, *l);
    for (size_t i = 0; i < drawing.points.size() && i < skins.size(); i++) {
      const float2 before = to_armature.apply(skins[i].apply(drawing.points[i].pos));
      pairs.emplace_back(before, bone_look_warp(eye, before));
    }
  }
  if (pairs.size() < 3) {
    return Transform2D();
  }
  float2 mb, ma;
  for (const auto &[b, a] : pairs) {
    mb += b;
    ma += a;
  }
  mb = mb / float(pairs.size());
  ma = ma / float(pairs.size());
  /* A = (sum a b^T) (sum b b^T)^-1, about the middles. */
  float bxx = 0, bxy = 0, byy = 0, axbx = 0, axby = 0, aybx = 0, ayby = 0;
  for (const auto &[b0, a0] : pairs) {
    const float2 b = b0 - mb, a = a0 - ma;
    bxx += b.x * b.x;
    bxy += b.x * b.y;
    byy += b.y * b.y;
    axbx += a.x * b.x;
    axby += a.x * b.y;
    aybx += a.y * b.x;
    ayby += a.y * b.y;
  }
  const float det = bxx * byy - bxy * bxy;
  if (std::abs(det) < 1e-20f) {
    return Transform2D::translate(ma - mb);
  }
  const float ixx = byy / det, ixy = -bxy / det, iyy = bxx / det;
  const Transform2D linear{axbx * ixx + axby * ixy, aybx * ixx + ayby * ixy, axbx * ixy + axby * iyy,
                           aybx * ixy + ayby * iyy, 0.0f, 0.0f};
  return Transform2D::translate(ma) * linear * Transform2D::translate(float2{} - mb);
}

/* How a layer carried on the eye (a shine) moves, layer space: its middle stays at the same place in
 * the pupil's oval as the pupil narrows (look_oval), and it keeps its own shape. */
static float2 look_carry(const Document &doc, const BonePose &p, const Transform2D &to_armature,
                         const std::vector<float2> &points)
{
  if (points.empty()) {
    return {};
  }
  float2 middle;
  for (const float2 q : points) {
    middle += q;
  }
  middle = to_armature.apply(middle / float(points.size()));
  return to_armature.inverted().apply_vector(look_oval(doc, p).apply(middle) - middle);
}

static void look_warp_drawing(const Document &doc, VectorDrawing &drawing, const BonePose &p,
                              const Transform2D &to_armature, bool carried)
{
  if (carried) {
    std::vector<float2> at;
    for (const MeshPoint &mp : drawing.points) {
      at.push_back(mp.pos);
    }
    const float2 move = look_carry(doc, p, to_armature, at);
    for (MeshPoint &mp : drawing.points) {
      mp.pos += move;
    }
    return;
  }
  std::vector<Transform2D> skins(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    skins[i] = look_warp_skin(p, to_armature, drawing.points[i].pos);
    drawing.points[i].pos = skins[i].apply(drawing.points[i].pos);
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      if (cp.point >= 0 && size_t(cp.point) < skins.size()) {
        cp.handle_in = skins[size_t(cp.point)].apply_vector(cp.handle_in);
        cp.handle_out = skins[size_t(cp.point)].apply_vector(cp.handle_out);
      }
    }
  }
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

/* Resample, with From Bones: the new points weighted from the nearest bones of `layer`'s bone layer. */
static VectorDrawing subdivided(const Document &doc, const Layer &layer, const VectorDrawing &drawing,
                                const Modifier &m, bool render)
{
  std::vector<int> fresh;
  VectorDrawing out = drawing_subdivide(drawing, m, render, &fresh);
  Deformer d;
  if (m.subdiv_from_bones && !fresh.empty() && layer.type == LayerType::Vector && deformer_init(doc, layer, d)) {
    for (const int i : fresh) {
      out.points[size_t(i)].weights = automatic_weights(d, out.points[size_t(i)], {}, true);
    }
  }
  return out;
}

VectorDrawing layer_evaluated_drawing(const Document &doc, const Layer &layer, bool render, Transform2D *r_rigid)
{
  if (r_rigid) {
    *r_rigid = Transform2D();
  }
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
      else if (m.type == ModifierType::Bevel) {
        drawing = drawing_bevel(drawing, m);
      }
      else if (m.type == ModifierType::Resample) {
        drawing = subdivided(doc, *original, drawing, m, render);
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
    /* Its own Merges still come last (an outline copy of a fill joining another layer's line). */
    for (const Modifier &m : layer.modifiers) {
      if (modifier_on(m, render) && m.type == ModifierType::Boolean) {
        merge_modifier_apply(doc, layer, m, drawing, render);
      }
    }
    merge_mark_taken(doc, layer, drawing, render);
    return drawing;
  }
  VectorDrawing drawing = layer_drawing(doc, layer);
  const Layer *armature = layer_armature(doc, layer.id);
  const auto flip = [armature](uint64_t bone) { return armature ? bone_mirror_id(*armature, bone) : bone; };
  /* The bones' move can be left out (for the caller to draw with) only when nothing else in the stack
   * works on where the points are. */
  BonePose eye;
  Transform2D eye_to_armature;
  bool eye_carried = false;
  const bool on_eye = layer.type == LayerType::Vector &&
                      look_warp_find(doc, layer, &eye, &eye_to_armature, &eye_carried);
  const bool only_bones = !on_eye && std::all_of(layer.modifiers.begin(), layer.modifiers.end(), [&](const Modifier &m) {
    return !modifier_on(m, render) || m.type == ModifierType::Armature;
  });
  /* The drawing as it was before the bones moved it: what a deform modifier below them measures from (each
   * point's weight shares it between the deform and the bones). Kept only while a deform modifier follows. */
  VectorDrawing before_bones;
  bool bones_moved = false;
  const bool deforms = std::any_of(layer.modifiers.begin(), layer.modifiers.end(), [&](const Modifier &m) {
    return modifier_on(m, render) && modifier_is_deform(m.type);
  });
  for (size_t index = 0; index < layer.modifiers.size(); index++) {
    const Modifier &m = layer.modifiers[index];
    if (!modifier_on(m, render)) {
      continue;
    }
    /* A modifier's new points come after the drawing's (Show Points marks them by it). */
    const size_t had = drawing.points.size();
    const auto mark_new = [&]() {
      for (size_t k = had; k < drawing.points.size(); k++) {
        drawing.points[k].made_by = int(index);
      }
    };
    if (m.type == ModifierType::Mirror) {
      drawing = drawing_mirror(drawing, m, flip);
    }
    else if (m.type == ModifierType::Bevel) {
      drawing = drawing_bevel(drawing, m);
    }
    else if (m.type == ModifierType::Resample) {
      drawing = subdivided(doc, layer, drawing, m, render);
    }
    else if (m.type == ModifierType::Armature) {
      Deformer d;
      if (deformer_init(doc, layer, d)) {
        if (deforms) {
          before_bones = drawing;
          bones_moved = true;
        }
        deform(d, drawing, only_bones ? r_rigid : nullptr);
      }
    }
    else if (modifier_is_deform(m.type)) {
      vector_deform_apply(doc, layer, m, drawing, bones_moved ? before_bones : drawing, render);
    }
    else if (m.type == ModifierType::Boolean) {
      /* At its place in the stack, top to bottom: it joins the drawing as the modifiers above leave it
       * (its handles as they finally are), and those below work on the joined outline. */
      drawing_detail_final_handles(drawing);
      merge_modifier_apply(doc, layer, m, drawing, render);
    }
    mark_new();
  }
  /* A pupil's drawings on its eyeball, as the Look At turns it (after the stack: on what it shows). */
  if (on_eye) {
    look_warp_drawing(doc, drawing, eye, eye_to_armature, eye_carried);
  }
  /* Handles scale as the bones and Mirror move them, so the final ones can come last. */
  drawing_detail_final_handles(drawing);
  /* Shapes another layer's Boolean draws as part of its outline don't draw here too. */
  merge_mark_taken(doc, layer, drawing, render);
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
        else if (modifier_on(m, render) && m.type == ModifierType::Bevel) {
          drawing = drawing_bevel(drawing, m);
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
      else if (modifier_on(m, render) && m.type == ModifierType::Bevel) {
        drawing = drawing_bevel(drawing, m);
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
    else if (modifier_on(m, render) && m.type == ModifierType::Bevel) {
      drawing = drawing_bevel(drawing, m);
    }
    else if (modifier_on(m, render) && m.type == ModifierType::Resample) {
      drawing = drawing_subdivide(drawing, m, render);
    }
  }
  drawing_detail_final_handles(drawing);
  return drawing;
}

std::vector<Transform2D> layer_point_skins(const Document &doc, const Layer &layer)
{
  std::vector<Transform2D> skins = layer_point_skins_bones(doc, layer);
  if (matched_original(doc, layer)) {
    return skins;
  }
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  /* Following a pupil: on its eyeball too. */
  BonePose eye;
  Transform2D to_armature;
  bool carried = false;
  if (layer.type == LayerType::Vector && look_warp_find(doc, layer, &eye, &to_armature, &carried)) {
    if (carried) {
      std::vector<float2> at;
      for (size_t i = 0; i < drawing.points.size(); i++) {
        at.push_back(skins[i].apply(drawing.points[i].pos));
      }
      const Transform2D move = Transform2D::translate(look_carry(doc, eye, to_armature, at));
      for (Transform2D &skin : skins) {
        skin = move * skin;
      }
    }
    else {
      for (size_t i = 0; i < drawing.points.size(); i++) {
        skins[i] = look_warp_skin(eye, to_armature, skins[i].apply(drawing.points[i].pos)) * skins[i];
      }
    }
  }
  /* Bent by a deform modifier: on to where it shows. Only where the stack keeps the points' indices (no Bevel or
   * Boolean, which make the outline anew). */
  bool deformed = false, renumbered = false;
  for (const Modifier &m : layer.modifiers) {
    if (!m.show_viewport) {
      continue;
    }
    deformed = deformed || (modifier_is_deform(m.type) && m.deform_layer != 0);
    renumbered = renumbered || m.type == ModifierType::Bevel || m.type == ModifierType::Boolean;
  }
  if (deformed && !renumbered && layer.type == LayerType::Vector) {
    const VectorDrawing shown = layer_evaluated_drawing(doc, layer, false);
    for (size_t i = 0; i < drawing.points.size() && i < shown.points.size(); i++) {
      skins[i] = Transform2D::translate(shown.points[i].pos - skins[i].apply(drawing.points[i].pos)) * skins[i];
    }
  }
  return skins;
}

/* What the bones alone make of each point (layer_point_skins without the eyeball warp). */
static std::vector<Transform2D> layer_point_skins_bones(const Document &doc, const Layer &layer)
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

std::vector<Transform2D> layer_point_skins_posed(const Document &doc, const Layer &layer,
                                                 const std::vector<BonePose> &poses)
{
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  std::vector<Transform2D> skins(drawing.points.size());
  Deformer d;
  if (!layer.is_reference() && deformer_init(doc, layer, d)) {
    d.poses = poses;
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
  /* Asked before every undo step: each layer's bone layer found by an index (weighing changes no layers). */
  const LayerIndexScope layer_index(doc);
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
