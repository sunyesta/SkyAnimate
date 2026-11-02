#include "anim/deform.hh"

#include "anim/controller.hh"

#include <algorithm>
#include <cmath>

#include "anim/bevel.hh"
#include "anim/draw_order.hh"
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

static VectorDrawing stack_evaluated(const Document &doc, const Layer &layer, bool render, Transform2D *r_rigid,
                                     const float2 *probe, const std::vector<StackPlace> *places = nullptr);

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

/* Whether any of a drawing's points has a bone to follow: not the empty entry a point with no weights keeps. */
static bool points_weighted(const Layer &layer)
{
  return std::any_of(layer.drawing.points.begin(), layer.drawing.points.end(), [](const MeshPoint &p) {
    return std::any_of(p.weights.begin(), p.weights.end(), [](const BoneWeight &w) { return w.bone != 0 && w.weight > 0.0f; });
  });
}

/* Whether the bones of `armature` move this drawing: its points, or it (or a group around it) as a whole. Being inside
 * the bone layer isn't enough: with no weights, nothing there moves it. A reference goes by its original's. */
static bool bones_carry(const Document &doc, const Layer &layer, const Layer &armature)
{
  if (&layer == &armature) {
    return true;
  }
  Deformer d;
  if (deformer_init(doc, layer, d) && d.armature == &armature &&
      (d.whole_bone != 0 || layer.is_reference() || points_weighted(layer)))
  {
    return true;
  }
  Document &editable = const_cast<Document &>(doc);
  for (const Layer *l = &layer; l; l = layer_parent(editable, l->id)) {
    if (l->followed_bone() != 0 && layer_armature(doc, l->id) == &armature) {
      return true;
    }
  }
  return false;
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
    /* A controller moves no drawing. */
    if (bone_is_controller(bone)) {
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

/* --- Deforms on groups ---------------------------------------------------------------------- */

/* The drawings reading another as a deform's shape, while they do. */
static thread_local std::vector<uint64_t> g_group_readers;

GroupDeformReading::GroupDeformReading(uint64_t layer)
{
  g_group_readers.push_back(layer);
}

GroupDeformReading::~GroupDeformReading()
{
  g_group_readers.pop_back();
}

struct GroupDeform {
  const Layer *group;
  const Modifier *modifier;
};

/* The deforms on the groups around a drawing, nearest group first (a bone layer: its own first, for its bones). A Match
 * Original reference goes by the groups around its original too, wherever it is itself, as it does by the original's
 * bones. */
static std::vector<GroupDeform> group_deforms(const Document &doc, const Layer &layer, bool render)
{
  std::vector<GroupDeform> found;
  if ((layer.type != LayerType::Vector && !layer.is_bone_layer()) || layer.ignore_group_deforms) {
    return found;
  }
  const Layer *original = matched_original(doc, layer);
  const auto gather = [&](const Layer &from) {
    Document &d = const_cast<Document &>(doc);
    for (const Layer *group = from.is_bone_layer() ? &from : layer_parent(d, from.id); group;
         group = layer_parent(d, group->id))
    {
      if (group->modifiers.empty() ||
          std::any_of(g_group_readers.begin(), g_group_readers.end(),
                      [&](uint64_t reader) { return layer_inside(doc, reader, group->id); }) ||
          std::any_of(found.begin(), found.end(), [&](const GroupDeform &g) { return g.group == group; }))
      {
        continue;
      }
      for (const Modifier &m : group->modifiers) {
        /* The shape itself, kept inside the group, stays as it is drawn. */
        /* A drawing following this shape with a Follow Deform stays rigid on it: the group doesn't bend it as well. */
        const bool followed = std::any_of(layer.modifiers.begin(), layer.modifiers.end(), [&](const Modifier &own) {
          return own.type == ModifierType::FollowDeform && modifier_on(own, render) &&
                 own.deform_layer == m.deform_layer && own.deform_shape == m.deform_shape;
        });
        if (modifier_bends(m.type) && modifier_on(m, render) && m.deform_layer != 0 && m.deform_layer != layer.id &&
            !(original && m.deform_layer == original->id) && !followed)
        {
          found.push_back({group, &m});
        }
      }
    }
  };
  if (original) {
    gather(*original);
  }
  gather(layer);
  return found;
}

bool layer_has_group_deforms(const Document &doc, const Layer &layer, bool render)
{
  return !group_deforms(doc, layer, render).empty();
}

bool layer_has_modifier_effect(const Document &doc, const Layer &layer, bool render)
{
  if (matched_original(doc, layer)) {
    return true;
  }
  if (layer_has_group_deforms(doc, layer, render)) {
    return true;
  }
  /* Detail shows its handles final (data/detail.hh). */
  if (layer.type == LayerType::Vector && drawing_has_detail(layer_drawing(doc, layer))) {
    return true;
  }
  if (layer.type == LayerType::Vector && merge_takes_from(doc, layer, render)) {
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
static VectorDrawing subdivided(const Document &doc, const Layer &layer, VectorDrawing drawing, const Modifier &m,
                                bool render)
{
  std::vector<int> fresh;
  VectorDrawing out = drawing_subdivide_kept(layer.id, m.name, std::move(drawing), m, render, &fresh);
  Deformer d;
  if (m.subdiv_from_bones && !fresh.empty() && layer.type == LayerType::Vector && deformer_init(doc, layer, d)) {
    for (const int i : fresh) {
      out.points[size_t(i)].weights = automatic_weights(d, out.points[size_t(i)], {}, true);
    }
  }
  return out;
}

void layer_pose_drawing(const Document &doc, const Layer &layer, VectorDrawing &drawing)
{
  Deformer d;
  if (deformer_init(doc, layer, d)) {
    deform(d, drawing);
  }
}

/* Follow Deform: the whole drawing goes where the deform puts its point. What the bones have done stays; those that move
 * the shape as well count once, the shape's rest posed by them as for a group's deform. */
static void follow_deform_run(const Document &doc, const Layer &layer, const Modifier &m, VectorDrawing &drawing,
                              const VectorDrawing *before_bones, bool render)
{
  /* With a Limit Method only the points it takes in move, each by its share. */
  const std::vector<float> share = vector_deform_follow_share(doc, layer, m, drawing);
  const auto share_of = [&](size_t i) { return share.empty() ? 1.0f : share[i]; };
  Modifier run = m;
  const Layer *shape = layer_find(doc, m.deform_layer);
  const Layer *armature = shape ? layer_armature(doc, shape->id) : nullptr;
  run.deform_rest_posed = armature && !armature->bones.empty() && bones_carry(doc, layer, *armature);
  float2 carry;
  if (before_bones && !drawing.points.empty() && before_bones->points.size() == drawing.points.size()) {
    float shares = 0.0f;
    for (size_t i = 0; i < drawing.points.size(); i++) {
      carry = carry + (drawing.points[i].pos - before_bones->points[i].pos) * share_of(i);
      shares += share_of(i);
    }
    carry = shares > 0.0f ? carry / shares : float2();
  }
  const Transform2D move = vector_deform_follow(doc, layer, run, carry, render);
  if (share.empty()) {
    transform_drawing(drawing, move);
    return;
  }
  for (size_t i = 0; i < drawing.points.size(); i++) {
    MeshPoint &p = drawing.points[i];
    p.pos = p.pos + (move.apply(p.pos) - p.pos) * share[i];
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      const float s = share[size_t(cp.point)];
      cp.handle_in = cp.handle_in + (move.apply_vector(cp.handle_in) - cp.handle_in) * s;
      cp.handle_out = cp.handle_out + (move.apply_vector(cp.handle_out) - cp.handle_out) * s;
    }
  }
}

/* What the groups around it make of the drawing as its own stack left it. Each runs as a copy of the group's modifier,
 * for this drawing: its automatic weights, and what it keeps apart from the other drawings' under its own name. */
static void group_deforms_run(const Document &doc, const Layer &layer, const std::vector<GroupDeform> &deforms,
                              VectorDrawing &drawing, bool render)
{
  for (const GroupDeform &g : deforms) {
    Modifier run = *g.modifier;
    run.name = g.modifier->name + "@" + std::to_string(g.group->id);
    run.deform_custom = false;
    run.deform_from_group = g.group->id;
    const Layer *shape = layer_find(doc, run.deform_layer);
    const Layer *armature = shape ? layer_armature(doc, shape->id) : nullptr;
    run.deform_rest_posed = armature && !armature->bones.empty() && bones_carry(doc, layer, *armature);
    /* Its new points are marked by the group's modifier (Show Points). */
    const size_t had = drawing.points.size();
    vector_deform_apply(doc, layer, run, drawing, drawing, render);
    for (size_t k = had; k < drawing.points.size(); k++) {
      drawing.points[k].made_by = int(g.modifier - g.group->modifiers.data());
      drawing.points[k].made_by_layer = g.group->id;
    }
  }
}

std::vector<float2> layer_group_deform_points(const Document &doc, const Layer &layer, std::vector<float2> points)
{
  const std::vector<GroupDeform> around = group_deforms(doc, layer, false);
  if (around.empty()) {
    return points;
  }
  VectorDrawing drawing;
  drawing.points.resize(points.size());
  for (size_t i = 0; i < points.size(); i++) {
    drawing.points[i].pos = points[i];
  }
  group_deforms_run(doc, layer, around, drawing, false);
  for (size_t i = 0; i < points.size(); i++) {
    points[i] = drawing.points[i].pos;
  }
  return points;
}

/* How far a step is taken to see how something moves around a place: a small part of a mouth. */
static constexpr float STRETCH_STEP = 0.002f;

/* The turn and stretch between a place and where a step each way from it went. False when it collapses there, with
 * nothing to take a drag back through. */
static bool stretch_from_steps(float2 at, float2 step_x, float2 step_y, Transform2D &r)
{
  const float2 x = (step_x - at) / STRETCH_STEP, y = (step_y - at) / STRETCH_STEP;
  if (std::fabs(x.x * y.y - x.y * y.x) <= 1e-4f) {
    return false;
  }
  r.a = x.x;
  r.b = x.y;
  r.c = y.x;
  r.d = y.y;
  return true;
}

Transform2D layer_group_deform_at(const Document &doc, const Layer &layer, float2 at)
{
  const std::vector<float2> p = layer_group_deform_points(
      doc, layer, {at, at + float2(STRETCH_STEP, 0.0f), at + float2(0.0f, STRETCH_STEP)});
  Transform2D t;
  stretch_from_steps(p[0], p[1], p[2], t);
  const float2 moved = t.apply_vector(at);
  t.tx = p[0].x - moved.x;
  t.ty = p[0].y - moved.y;
  return t;
}

VectorDrawing layer_evaluated_drawing(const Document &doc, const Layer &layer, bool render, Transform2D *r_rigid)
{
  return stack_evaluated(doc, layer, render, r_rigid, nullptr);
}

/* What the whole stack does right around each of the drawing's own points, in `skins`: its points alone moved a small
 * step each way and worked out again, so bones, deforms and each point's weights between them all count. */
static void stack_stretches(const Document &doc, const Layer &layer, std::vector<Transform2D> &skins, size_t count)
{
  const auto stepped = [&](float2 shift, int kind) {
    const VectorDeformProbeScope kept(kind);
    return stack_evaluated(doc, layer, false, nullptr, &shift);
  };
  const VectorDrawing here = stepped({}, 1), x = stepped({STRETCH_STEP, 0.0f}, 2), y = stepped({0.0f, STRETCH_STEP}, 3);
  for (size_t i = 0; i < count && i < here.points.size() && i < x.points.size() && i < y.points.size(); i++) {
    stretch_from_steps(here.points[i].pos, x.points[i].pos, y.points[i].pos, skins[i]);
  }
}

/* The stack, on the layer's drawing. `probe`: on its points alone moved by that, with nothing to fit between them, and
 * without a Boolean's joining, so each is where the rest of the stack puts it whatever becomes of the outline. `places`:
 * with these after them, each a copy of its point. */
static VectorDrawing stack_evaluated(const Document &doc, const Layer &layer, bool render, Transform2D *r_rigid,
                                     const float2 *probe, const std::vector<StackPlace> *places)
{
  if (r_rigid) {
    *r_rigid = Transform2D();
  }
  const std::vector<GroupDeform> around = group_deforms(doc, layer, render);
  if (const Layer *original = matched_original(doc, layer)) {
    /* Match Original: shaped by the original's modifiers and posed by its bones, where it is. */
    VectorDrawing drawing = layer.drawing;
    if (probe) {
      drawing.curves.clear();
      drawing.shapes.clear();
      for (MeshPoint &p : drawing.points) {
        p.pos = p.pos + *probe;
      }
    }
    const Layer *bones = layer_armature(doc, original->id);
    const auto flip = [bones](uint64_t bone) { return bones ? bone_mirror_id(*bones, bone) : bone; };
    /* As the stack below keeps it: what a deform measures from. */
    VectorDrawing before_bones;
    bool bones_moved = false;
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
        drawing = subdivided(doc, *original, std::move(drawing), m, render);
      }
      else if (m.type == ModifierType::Armature) {
        Deformer d;
        if (deformer_init(doc, *original, d)) {
          before_bones = drawing;
          bones_moved = true;
          deform(d, drawing);
        }
      }
      else if (m.type == ModifierType::FollowDeform) {
        follow_deform_run(doc, *original, m, drawing, bones_moved ? &before_bones : nullptr, render);
      }
      else if (modifier_is_deform(m.type)) {
        vector_deform_apply(doc, *original, m, drawing, bones_moved ? before_bones : drawing, render);
      }
    }
    transform_drawing(drawing, reference_match_transform(doc, layer));
    group_deforms_run(doc, layer, around, drawing, render);
    if (probe) {
      return drawing;
    }
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
  if (probe) {
    drawing.curves.clear();
    drawing.shapes.clear();
    for (MeshPoint &p : drawing.points) {
      p.pos = p.pos + *probe;
    }
    for (const StackPlace &place : places ? *places : std::vector<StackPlace>()) {
      MeshPoint q = drawing.points[size_t(place.point)];
      q.pos = place.at + *probe;
      drawing.points.push_back(q);
    }
  }
  const Layer *armature = layer_armature(doc, layer.id);
  const auto flip = [armature](uint64_t bone) { return armature ? bone_mirror_id(*armature, bone) : bone; };
  /* The bones' move can be left out (for the caller to draw with) only when nothing else in the stack
   * works on where the points are. */
  const bool only_bones = around.empty() &&
                          std::all_of(layer.modifiers.begin(), layer.modifiers.end(), [&](const Modifier &m) {
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
      drawing = subdivided(doc, layer, std::move(drawing), m, render);
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
    else if (m.type == ModifierType::FollowDeform) {
      follow_deform_run(doc, layer, m, drawing, bones_moved ? &before_bones : nullptr, render);
    }
    else if (modifier_is_deform(m.type)) {
      vector_deform_apply(doc, layer, m, drawing, bones_moved ? before_bones : drawing, render);
    }
    else if (m.type == ModifierType::Boolean && !probe) {
      /* At its place in the stack, top to bottom: it joins the drawing as the modifiers above leave it
       * (its handles as they finally are), and those below work on the joined outline. */
      drawing_detail_final_handles(drawing);
      merge_modifier_apply(doc, layer, m, drawing, render);
    }
    mark_new();
  }
  group_deforms_run(doc, layer, around, drawing, render);
  if (probe) {
    return drawing;
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

/* Whether a deform modifier, its own or a group's, bends the layer. */
static bool layer_is_deformed(const Document &doc, const Layer &layer)
{
  return layer_has_group_deforms(doc, layer, false) ||
         std::any_of(layer.modifiers.begin(), layer.modifiers.end(), [](const Modifier &m) {
           return m.show_viewport && modifier_is_deform(m.type) && m.deform_layer != 0;
         });
}

/* The layer's own points where the stack puts them. */
static VectorDrawing stack_points(const Document &doc, const Layer &layer)
{
  const VectorDeformProbeScope kept(1);
  const float2 here(0.0f, 0.0f);
  return stack_evaluated(doc, layer, false, nullptr, &here);
}

void layer_points_settle(const Document &doc, Layer &layer, const std::vector<PointTarget> &targets)
{
  /* Nothing bends on the setup frame. */
  if (doc.frame_current == 0 || targets.empty() || matched_original(doc, layer) || !layer_is_deformed(doc, layer)) {
    return;
  }
  /* The same steps at every update of a drag, none chosen over another, so a point changes smoothly with the mouse, each
   * by the stretch where it now is: that can change many times over on the way, at the side of a squeezed cell. Limited
   * to a dozen times the move, more than the worst squeeze wants: past a fold or the end of a line there is nowhere
   * nearer to go. */
  std::vector<float2> from(targets.size());
  for (size_t i = 0; i < targets.size(); i++) {
    from[i] = layer.drawing.points[size_t(targets[i].point)].pos;
  }
  std::vector<StackPlace> places;
  for (int pass = 0; pass < 6; pass++) {
    places.clear();
    for (const PointTarget &t : targets) {
      const float2 at = layer.drawing.points[size_t(t.point)].pos;
      places.push_back({t.point, at});
      places.push_back({t.point, at + float2(STRETCH_STEP, 0.0f)});
      places.push_back({t.point, at + float2(0.0f, STRETCH_STEP)});
    }
    const std::vector<float2> shown = layer_places_shown(doc, layer, places);
    bool done = true;
    for (size_t i = 0; i < targets.size(); i++) {
      const PointTarget &t = targets[i];
      const float2 off = t.at - shown[i * 3];
      if (length(off) <= std::max(1e-6f, 1e-5f * length(t.at))) {
        continue;
      }
      done = false;
      Transform2D stretch;
      const Transform2D back = stretch_from_steps(shown[i * 3], shown[i * 3 + 1], shown[i * 3 + 2], stretch) ? stretch.inverted()
                                                                                                             : t.unskin;
      float2 &stored = layer.drawing.points[size_t(t.point)].pos;
      float2 next = stored + back.apply_vector(off);
      const float went = length(next - from[i]), most = t.moved * 12.0f;
      if (went > most && went > 0.0f) {
        next = from[i] + (next - from[i]) * (most / went);
      }
      stored = next;
    }
    if (done) {
      break;
    }
  }
}

std::vector<float2> layer_places_shown(const Document &doc, const Layer &layer, const std::vector<StackPlace> &places)
{
  const VectorDeformProbeScope kept(4);
  const float2 here(0.0f, 0.0f);
  const size_t count = layer_drawing(doc, layer).points.size();
  const VectorDrawing shown = stack_evaluated(doc, layer, false, nullptr, &here, &places);
  std::vector<float2> r(places.size());
  for (size_t i = 0; i < places.size(); i++) {
    r[i] = count + i < shown.points.size() ? shown.points[count + i].pos : places[i].at;
  }
  return r;
}

Transform2D layer_place_unskin(const Document &doc, const Layer &layer, const StackPlace &place)
{
  const std::vector<float2> p = layer_places_shown(doc, layer,
                                                   {place,
                                                    {place.point, place.at + float2(STRETCH_STEP, 0.0f)},
                                                    {place.point, place.at + float2(0.0f, STRETCH_STEP)}});
  Transform2D t;
  stretch_from_steps(p[0], p[1], p[2], t);
  return t.inverted();
}

bool layer_handles_fitted(const Document &doc, const Layer &layer)
{
  const auto fits = [](const Modifier &m) { return modifier_is_deform(m.type) && m.deform_resample_self; };
  if (doc.frame_current == 0) {
    return false;
  }
  const std::vector<GroupDeform> around = group_deforms(doc, layer, false);
  return std::any_of(around.begin(), around.end(), [&](const GroupDeform &g) { return fits(*g.modifier); }) ||
         std::any_of(layer.modifiers.begin(), layer.modifiers.end(), [&](const Modifier &m) {
           return m.show_viewport && m.deform_layer != 0 && fits(m);
         });
}

void layer_handles_settle(const Document &doc, Layer &layer, const std::vector<HandleTarget> &targets)
{
  if (doc.frame_current == 0 || targets.empty() || matched_original(doc, layer) || !layer_is_deformed(doc, layer)) {
    return;
  }
  const bool fitted = layer_handles_fitted(doc, layer);
  VectorDrawing &drawing = layer.drawing;
  /* The same few steps for every update of a drag, none chosen over another, so the handle changes smoothly with the
   * mouse: what a bent handle does can turn sharply, and the best of several tries could differ from one update to the
   * next by a long way. Adding at most half the move: where a handle shows far shorter than it is stored, a small
   * difference on screen is a long way stored, and chasing it would fling the handle about. (A place of its own goes
   * through the deform as any point does, which is steadier, so it can go further.) */
  std::vector<float2> from(targets.size());
  std::vector<bool> started(targets.size(), false);
  for (int pass = 0; pass < (fitted ? 6 : 3); pass++) {
    /* As the wire shows it, what is being dragged; or, fitted, the end as a place of its own. */
    VectorDrawing shown;
    if (!fitted) {
      const VectorDeformWireScope wire;
      shown = layer_evaluated_drawing(doc, layer, false);
    }
    std::vector<StackPlace> ends;
    for (const HandleTarget &t : targets) {
      const Curve &curve = drawing.curves[size_t(t.curve)];
      ends.push_back({curve.points[size_t(t.index)].point, curve_handle(drawing, curve, t.index, t.in)});
    }
    const std::vector<float2> placed = fitted ? layer_places_shown(doc, layer, ends) : std::vector<float2>();
    for (size_t i = 0; i < targets.size(); i++) {
      const HandleTarget &t = targets[i];
      /* A Resample or a Bevel leaves no such handle to compare. */
      if (!fitted && (size_t(t.curve) >= shown.curves.size() ||
                      shown.curves[size_t(t.curve)].points.size() != drawing.curves[size_t(t.curve)].points.size()))
      {
        continue;
      }
      Curve &curve = drawing.curves[size_t(t.curve)];
      const float2 stored = curve_handle(drawing, curve, t.index, t.in);
      const float2 now = fitted ? placed[i] : curve_handle(shown, shown.curves[size_t(t.curve)], t.index, t.in);
      /* Fitted, by the stretch where the end now is: it can change sharply on the way, at a squeezed cell's side. */
      const Transform2D back = fitted ? layer_place_unskin(doc, layer, ends[i]) : t.unskin;
      const float2 off = back.apply_vector(t.at - now);
      if (!started[i]) {
        started[i] = true;
        from[i] = stored;
      }
      float2 next = stored + off * (fitted ? 1.0f : 0.8f);
      /* Where the deform squeezes or stretches hard at the point, the move is many times what shows, and half of that
       * can swing from one way to another between updates: there it fades out, and the handle just goes as it is moved. */
      const float area = std::fabs(t.unskin.a * t.unskin.d - t.unskin.b * t.unskin.c);
      const float mild = std::clamp((std::min(area, 1.0f / std::max(area, 1e-6f)) - 0.25f) / 0.5f, 0.0f, 1.0f);
      const float went = length(next - from[i]), most = t.moved * (fitted ? 2.0f : 0.5f * mild);
      if (went > most && went > 0.0f) {
        next = from[i] + (next - from[i]) * (most / went);
      }
      curve_point_move_handle(drawing, curve, t.index, t.in, next, t.keep_other);
    }
  }
}

static std::vector<Transform2D> layer_point_skins_bones(const Document &doc, const Layer &layer);

std::vector<Transform2D> layer_point_skins(const Document &doc, const Layer &layer)
{
  std::vector<Transform2D> skins = layer_point_skins_bones(doc, layer);
  const bool grouped = layer_has_group_deforms(doc, layer, false);
  /* On to where the deforms show it. */
  const auto onto_shown = [&](const VectorDrawing &stored) {
    const size_t count = std::min(skins.size(), stored.points.size());
    const VectorDrawing shown = stack_points(doc, layer);
    for (size_t i = 0; i < count && i < shown.points.size(); i++) {
      skins[i] = Transform2D::translate(shown.points[i].pos - skins[i].apply(stored.points[i].pos)) * skins[i];
    }
  };
  if (const Layer *original = matched_original(doc, layer)) {
    const bool deformed = layer_is_deformed(doc, *original);
    if (deformed && doc.frame_current != 0) {
      stack_stretches(doc, layer, skins, std::min(skins.size(), layer.drawing.points.size()));
    }
    if (grouped || deformed) {
      onto_shown(layer.drawing);
    }
    return skins;
  }
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  /* Bent by a deform modifier: on to where it shows. */
  if (layer.type == LayerType::Vector && layer_is_deformed(doc, layer)) {
    /* Turned and stretched as they do there, so a drag lands under the cursor. (Nothing bends on the setup frame.) */
    if (doc.frame_current != 0) {
      stack_stretches(doc, layer, skins, std::min(skins.size(), drawing.points.size()));
    }
    onto_shown(drawing);
  }
  return skins;
}

/* What the bones alone make of each point. */
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
  /* Asked of every layer before every undo step and frame change: whether it has new points first, which
   * costs little, before working out its bones' pose. */
  VectorDrawing &drawing = layer.drawing;
  if (layer.type != LayerType::Vector ||
      std::none_of(drawing.points.begin(), drawing.points.end(), [](const MeshPoint &p) { return p.weights.empty(); }))
  {
    return false;
  }
  Deformer d;
  if (!weightable(doc, layer, d)) {
    return false;
  }
  std::vector<bool> weighted(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    weighted[i] = !drawing.points[i].weights.empty();
  }
  std::vector<std::vector<BoneWeight>> found(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (weighted[i]) {
      continue;
    }
    found[i] = exact ? automatic_weights(d, drawing.points[i], {}, false) : neighbour_weights(drawing, int(i), weighted);
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

bool document_weight_new_points(Document &doc, bool exact, bool (*skip)(const Layer &))
{
  /* Asked before every undo step: each layer's bone layer found by an index, and each bone layer posed once
   * (weighing changes no layers, nor bones). */
  const LayerIndexScope layer_index(doc);
  const ArmaturePoseCacheScope poses;
  bool changed = false;
  const auto visit = [&](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      if (skip && skip(layer)) {
        continue;
      }
      changed |= weigh_new_points(doc, layer, exact);
      self(self, layer.children);
    }
  };
  visit(visit, doc.layers);
  return changed;
}

}  // namespace oanim
