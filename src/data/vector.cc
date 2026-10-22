#include "data/vector.hh"

#include "data/detail.hh"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <queue>
#include <set>

namespace oanim {

namespace {
/* Structural edits on a drawing with detail keep its look (see drawing_detail_edit_begin). */
struct DetailScope {
  VectorDrawing &drawing;
  DetailEdit edit;
  explicit DetailScope(VectorDrawing &d) : drawing(d), edit(drawing_detail_edit_begin(d)) {}
  ~DetailScope() { drawing_detail_edit_end(drawing, edit); }
  DetailScope(const DetailScope &) = delete;
  DetailScope &operator=(const DetailScope &) = delete;
};
}  // namespace

int Curve::segment_count() const
{
  const int n = int(points.size());
  if (n < 2) {
    return 0;
  }
  return closed ? n : n - 1;
}

/* ---------------------------------------------------------------------------------
 * Curve geometry
 */

/* A handle worked out from the neighbours, for a point without its own yet. `mode` 0: smooth
 * (Moho's GetBezierHandle(), from curvature, weights and offsets), or straight when the curvature
 * is 0; 1: straight (a third of the way to the neighbour); 2: smooth even where curvature is 0. */
static float2 computed_handle(const VectorDrawing &drawing, const Curve &curve, int i, bool pre, int mode)
{
  const int n = int(curve.points.size());
  const CurvePoint &cp = curve.points[i];
  const float2 here = drawing.points[cp.point].pos;
  if (mode == 1 || (mode == 0 && cp.curvature <= 0.0f)) {
    /* A third of the way to the neighbour on that side; none past an open curve's end. */
    const int other = pre ? i - 1 : i + 1;
    if (n < 2 || (!curve.closed && (other < 0 || other >= n))) {
      return here;
    }
    const float2 there = drawing.points[curve.points[(other + n) % n].point].pos;
    return here + (there - here) / 3.0f;
  }
  const auto pos = [&](int index) { return drawing.points[curve.points[index].point].pos; };
  if (n < 2) {
    return here;
  }
  const auto in_range = [&](int p) {
    if (curve.closed) {
      return (p % n + n) % n;
    }
    return std::clamp(p, 0, n - 1);
  };
  const int post_id = in_range(i + 1);
  const int pre_id = in_range(i - 1);
  const int second_id = pre ? pre_id : post_id;
  if (second_id == i) {
    return here;
  }
  /* The tangent bisects the directions to the next point and from the previous one (as Moho draws
   * it), not the line between the neighbours: a point with one neighbour close by (a cheek's
   * corner) keeps the curve's own direction there. */
  const float2 to_next = pos(post_id) - here, from_prev = here - pos(pre_id);
  const float next_len = length(to_next), prev_len = length(from_prev);
  float2 dir = (next_len > 1e-9f ? to_next / next_len : float2()) + (prev_len > 1e-9f ? from_prev / prev_len : float2());
  if (length(dir) < 1e-9f) {
    /* Neighbours straight back the way it came: across, as the old line between them was. */
    dir = pos(post_id) - pos(pre_id);
  }
  dir = normalize(dir);
  if (pre) {
    dir = -dir;
  }
  const float curvature = mode == 2 ? std::max(cp.curvature, CURVATURE_SMOOTH) : cp.curvature;
  const float len = distance(pos(second_id), here) * curvature * (pre ? cp.weight_in : cp.weight_out);
  return here + rotate(dir * len, pre ? cp.offset_in : cp.offset_out);
}

float2 curve_handle(const VectorDrawing &drawing, const Curve &curve, int i, bool pre)
{
  const CurvePoint &cp = curve.points[i];
  if (!cp.handles_set) {
    return computed_handle(drawing, curve, i, pre, 0);
  }
  return drawing.points[cp.point].pos + (pre ? cp.handle_in : cp.handle_out);
}

/* Stores handle offsets, keeping a channel without keys from treating it as an edit. */
static void set_handles(const VectorDrawing &drawing, CurvePoint &cp, float2 in, float2 out, bool quiet)
{
  (void)drawing;
  cp.handle_in = in;
  cp.handle_out = out;
  cp.handles_set = true;
  if (quiet) {
    /* Not an edit: without keys, this is the setup shape (no change from it). */
    for (const bool pre : {true, false}) {
      Channel<float2> &ch = pre ? cp.handle_in_keys : cp.handle_out_keys;
      if (!ch.animated()) {
        (pre ? cp.setup_in : cp.setup_out) = pre ? in : out;
        (pre ? cp.change_in : cp.change_out) = {};
        ch.last = {};
        ch.has_last = true;
      }
    }
  }
}


/* The frame-0 key, or for a value without keys the one from before any edit not yet recorded
 * (an edit away from frame 0 is about to become a key, not the setup). */
template<typename T> static T setup_of(const Channel<T> &ch, const T &value)
{
  return ch.animated() ? ch.evaluate(0) : (ch.has_last ? ch.last : value);
}

/* Just the points' setup-pose (frame 0) positions, light enough to build once per change. */
static VectorDrawing setup_points(const VectorDrawing &drawing)
{
  VectorDrawing setup;
  setup.points.resize(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    setup.points[i].pos = point_setup_pos(drawing.points[i]);
  }
  return setup;
}

static Curve setup_curve(const Curve &curve)
{
  Curve c;
  c.closed = curve.closed;
  c.points = curve.points;
  for (CurvePoint &cp : c.points) {
    cp.curvature = setup_of(cp.curvature_keys, cp.curvature);
    cp.handles_set = false;
  }
  return c;
}

/* Curve point i's handles (offsets from it) as worked out from its neighbours in the setup
 * pose: what a point that never had its own handles looks like on frame 0. */
static void setup_handles(const VectorDrawing &setup, const Curve &curve, int i, float2 *r_in, float2 *r_out)
{
  const Curve c = setup_curve(curve);
  const float2 here = setup.points[c.points[i].point].pos;
  *r_in = computed_handle(setup, c, i, true, 0) - here;
  *r_out = computed_handle(setup, c, i, false, 0) - here;
}

/* Makes sure a point's handles are stored (so they can be moved by hand): what it shows now.
 * A channel without keys remembers the setup-pose handles as its value before this edit, so a
 * first key away from frame 0 keys frame 0 with the setup shape (anim/animation.hh). */
static void own_handles(VectorDrawing &drawing, Curve &curve, int i)
{
  CurvePoint &cp = curve.points[i];
  if (cp.handles_set) {
    return;
  }
  const float2 here = drawing.points[cp.point].pos;
  const float2 in = computed_handle(drawing, curve, i, true, 0) - here;
  const float2 out = computed_handle(drawing, curve, i, false, 0) - here;
  float2 setup_in, setup_out;
  setup_handles(setup_points(drawing), curve, i, &setup_in, &setup_out);
  cp.handle_in = in;
  cp.handle_out = out;
  cp.handles_set = true;
  /* Without keys, the setup shape is what the neighbours give in the setup pose, and what it
   * shows now (posed) is a change from it, as if just edited. */
  for (const bool pre : {true, false}) {
    Channel<float2> &ch = pre ? cp.handle_in_keys : cp.handle_out_keys;
    if (!ch.animated()) {
      float2 &setup = pre ? cp.setup_in : cp.setup_out;
      setup = pre ? setup_in : setup_out;
      (pre ? cp.change_in : cp.change_out) = handle_change(setup, pre ? in : out, 0.0f);
      ch.last = {};
      ch.has_last = true;
    }
  }
}

void drawing_init_handles(VectorDrawing &drawing)
{
  /* Handles are setup data: worked out from the setup pose, they hold on every frame. */
  VectorDrawing setup;
  bool built = false;
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (cp.handles_set) {
        continue;
      }
      if (!built) {
        setup = setup_points(drawing);
        built = true;
      }
      float2 in, out;
      setup_handles(setup, curve, i, &in, &out);
      /* Smooth handles come out in line; keep them so. */
      cp.aligned = cp.curvature > 0.0f;
      set_handles(drawing, cp, in, out, true);
    }
  }
}

void curve_point_move_handle(VectorDrawing &drawing, Curve &curve, int i, bool pre, float2 pos, bool keep_other)
{
  own_handles(drawing, curve, i);
  CurvePoint &cp = curve.points[i];
  const float2 offset = pos - drawing.points[cp.point].pos;
  (pre ? cp.handle_in : cp.handle_out) = offset;
  if (cp.aligned && !keep_other && length(offset) > 1e-9f) {
    float2 &other = pre ? cp.handle_out : cp.handle_in;
    other = -normalize(offset) * length(other);
  }
}

void curve_point_align(VectorDrawing &drawing, Curve &curve, int i, bool turn_out)
{
  own_handles(drawing, curve, i);
  CurvePoint &cp = curve.points[i];
  const float2 in = cp.handle_in, out = cp.handle_out;
  if (turn_out ? length(in) > 1e-9f : length(out) <= 1e-9f) {
    if (length(in) > 1e-9f) {
      cp.handle_out = -normalize(in) * length(out);
    }
  }
  else if (length(out) > 1e-9f) {
    cp.handle_in = -normalize(out) * length(in);
  }
  cp.aligned = true;
}

void curve_segment_points(const Curve &curve, int segment, int *r_from, int *r_to)
{
  const int n = int(curve.points.size());
  *r_from = curve.points[segment].point;
  *r_to = curve.points[(segment + 1) % n].point;
}

void curve_segment_bezier(const VectorDrawing &drawing, const Curve &curve, int segment, float2 r_ctrl[4])
{
  const int n = int(curve.points.size());
  const int next = (segment + 1) % n;
  r_ctrl[0] = drawing.points[curve.points[segment].point].pos;
  r_ctrl[1] = curve_handle(drawing, curve, segment, false);
  r_ctrl[2] = curve_handle(drawing, curve, next, true);
  r_ctrl[3] = drawing.points[curve.points[next].point].pos;
}

float2 bezier_eval(const float2 c[4], float t)
{
  const float u = 1.0f - t;
  return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
}

int bezier_steps(const float2 ctrl[4], int min_steps, float tolerance)
{
  if (tolerance <= 0.0f) {
    return min_steps;
  }
  /* Wang's formula: n pieces keep a cubic within `tolerance` of its chords when
   * n >= sqrt(3/4 * L / tolerance), L the larger second difference of the control points. */
  const float L = std::max(length(ctrl[0] - ctrl[1] * 2.0f + ctrl[2]), length(ctrl[1] - ctrl[2] * 2.0f + ctrl[3]));
  const int n = int(std::ceil(std::sqrt(0.75f * L / tolerance)));
  return std::clamp(n, min_steps, 1024);
}

void curve_segment_sample(const VectorDrawing &drawing, const Curve &curve, int segment, int steps,
                          std::vector<float2> &r_points, float tolerance)
{
  float2 ctrl[4];
  curve_segment_bezier(drawing, curve, segment, ctrl);
  steps = bezier_steps(ctrl, steps, tolerance);
  for (int i = 0; i < steps; i++) {
    r_points.push_back(bezier_eval(ctrl, float(i) / float(steps)));
  }
}

bool drawing_ensure_ids(VectorDrawing &drawing)
{
  for (const MeshPoint &p : drawing.points) {
    drawing.next_id = std::max(drawing.next_id, p.id + 1);
  }
  for (const Shape &s : drawing.shapes) {
    drawing.next_id = std::max(drawing.next_id, s.id + 1);
  }
  bool changed = false;
  std::set<uint64_t> seen;
  for (MeshPoint &p : drawing.points) {
    if (p.id == 0 || !seen.insert(p.id).second) {
      p.id = drawing.next_id++;
      seen.insert(p.id);
      changed = true;
    }
  }
  seen.clear();
  for (Shape &s : drawing.shapes) {
    if (s.id == 0 || !seen.insert(s.id).second) {
      s.id = drawing.next_id++;
      seen.insert(s.id);
      changed = true;
    }
  }
  return changed;
}

int drawing_point_index(const VectorDrawing &drawing, uint64_t id)
{
  for (int i = 0; i < int(drawing.points.size()); i++) {
    if (drawing.points[i].id == id) {
      return i;
    }
  }
  return -1;
}

int drawing_shape_index(const VectorDrawing &drawing, uint64_t id)
{
  for (int i = 0; i < int(drawing.shapes.size()); i++) {
    if (drawing.shapes[i].id == id) {
      return i;
    }
  }
  return -1;
}

VectorDrawing drawing_setup_pose(const VectorDrawing &drawing)
{
  VectorDrawing setup = drawing;
  for (MeshPoint &p : setup.points) {
    p.pos = point_setup_pos(p);
    p.radius = setup_of(p.radius_keys, p.radius);
  }
  for (Curve &curve : setup.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.curvature = setup_of(cp.curvature_keys, cp.curvature);
      cp.handle_in = handle_setup(cp, true);
      cp.handle_out = handle_setup(cp, false);
    }
  }
  /* Detail in its setup pose, placed on the setup curve. */
  for (MeshPoint &p : setup.points) {
    if (p.detail.level > 0 && (p.pos_keys.has_last || p.pos_keys.animated())) {
      p.detail.pos = p.detail.setup_pos;
    }
  }
  for (Curve &curve : setup.curves) {
    for (CurvePoint &cp : curve.points) {
      PointDetail &dt = setup.points[cp.point].detail;
      if (dt.level > 0) {
        dt.in = cp.handle_in_keys.has_last || cp.handle_in_keys.animated() ? dt.setup_in : dt.in;
        dt.out = cp.handle_out_keys.has_last || cp.handle_out_keys.animated() ? dt.setup_out : dt.out;
      }
    }
  }
  drawing_detail_sync(setup, false);
  return setup;
}

bool shape_chain(const VectorDrawing &drawing, const Shape &shape, std::vector<OrientedEdge> &r_chain,
                 bool *r_closed)
{
  r_chain.clear();
  *r_closed = false;
  if (shape.edges.empty()) {
    return false;
  }
  using Oriented = OrientedEdge;
  /* Chain the edges by shared points, so the order they're stored in doesn't matter. */
  const auto ends = [&](const ShapeEdge &e, int *from, int *to) {
    curve_segment_points(drawing.curves[e.curve], e.segment, from, to);
  };
  std::vector<bool> used(shape.edges.size(), false);
  std::deque<Oriented> chain;
  int first_from, last_to;
  ends(shape.edges[0], &first_from, &last_to);
  chain.push_back({shape.edges[0], false});
  used[0] = true;
  bool grew = true;
  while (grew) {
    grew = false;
    for (size_t i = 0; i < shape.edges.size(); i++) {
      if (used[i]) {
        continue;
      }
      int a, b;
      ends(shape.edges[i], &a, &b);
      if (a == last_to || b == last_to) {
        const bool reversed = (b == last_to);
        chain.push_back({shape.edges[i], reversed});
        last_to = reversed ? a : b;
      }
      else if (a == first_from || b == first_from) {
        const bool reversed = (a == first_from);
        chain.push_front({shape.edges[i], reversed});
        first_from = reversed ? b : a;
      }
      else {
        continue;
      }
      used[i] = true;
      grew = true;
    }
  }
  const bool connected = std::all_of(used.begin(), used.end(), [](bool u) { return u; });
  r_chain.assign(chain.begin(), chain.end());
  *r_closed = (first_from == last_to) && chain.size() > 1;
  return connected;
}

bool shape_outline(const VectorDrawing &drawing, const Shape &shape, int min_steps,
                   std::vector<float2> &r_points, std::vector<float> *r_radii, bool *r_closed,
                   std::vector<bool> *r_visible, std::vector<StrokeJoin> *r_joins, float tolerance)
{
  if (r_joins) {
    r_joins->clear();
  }
  const auto join_at = [&](int point) {
    return drawing.points[point].corner == CornerType::Sharp ? StrokeJoin::Sharp : StrokeJoin::Round;
  };
  r_points.clear();
  if (r_radii) {
    r_radii->clear();
  }
  if (r_visible) {
    r_visible->clear();
  }
  *r_closed = false;
  std::vector<OrientedEdge> chain;
  bool closed = false;
  const bool connected = shape_chain(drawing, shape, chain, &closed);
  if (chain.empty()) {
    return false;
  }
  for (const OrientedEdge &o : chain) {
    const Curve &curve = drawing.curves[o.edge.curve];
    float2 ctrl[4];
    curve_segment_bezier(drawing, curve, o.edge.segment, ctrl);
    int from, to;
    curve_segment_points(curve, o.edge.segment, &from, &to);
    if (o.reversed) {
      std::swap(ctrl[0], ctrl[3]);
      std::swap(ctrl[1], ctrl[2]);
      std::swap(from, to);
    }
    const int steps_per_segment = bezier_steps(ctrl, min_steps, tolerance);
    const float r0 = drawing.points[from].radius, r1 = drawing.points[to].radius;
    const bool visible = !curve.points[o.edge.segment].edge_hidden;
    /* The radius blends by distance along the segment, not by the curve parameter: samples
     * bunch up near points (a peaked segment eases in and out), and blending by parameter
     * would bend a tapered stroke's straight edges there. */
    std::vector<float2> samples(steps_per_segment + 1);
    std::vector<float> along(steps_per_segment + 1, 0.0f);
    for (int i = 0; i <= steps_per_segment; i++) {
      samples[i] = bezier_eval(ctrl, float(i) / float(steps_per_segment));
      if (i > 0) {
        along[i] = along[i - 1] + distance(samples[i - 1], samples[i]);
      }
    }
    const float total = along.back();
    for (int i = 0; i < steps_per_segment; i++) {
      r_points.push_back(samples[i]);
      if (r_radii) {
        r_radii->push_back(r0 + (r1 - r0) * (total > 1e-9f ? along[i] / total : float(i) / float(steps_per_segment)));
      }
      if (r_visible) {
        r_visible->push_back(visible);
      }
      if (r_joins) {
        r_joins->push_back(i == 0 ? join_at(from) : StrokeJoin::Smooth);
      }
    }
  }
  *r_closed = closed;
  if (!*r_closed) {
    /* Open runs end on their last point. */
    int last_to, unused;
    const OrientedEdge &end = chain.back();
    curve_segment_points(drawing.curves[end.edge.curve], end.edge.segment, &unused, &last_to);
    if (end.reversed) {
      last_to = unused;
    }
    r_points.push_back(drawing.points[last_to].pos);
    if (r_radii) {
      r_radii->push_back(drawing.points[last_to].radius);
    }
    if (r_visible) {
      r_visible->push_back(false);
    }
    if (r_joins) {
      r_joins->push_back(join_at(last_to));
    }
  }
  return connected;
}

std::vector<Shape> shape_runs(const VectorDrawing &drawing, const Shape &shape)
{
  std::vector<OrientedEdge> chain;
  bool closed = false;
  if (shape.edges.empty() || shape_chain(drawing, shape, chain, &closed)) {
    return {shape};
  }
  std::vector<Shape> runs;
  Shape rest = shape;
  while (!rest.edges.empty()) {
    shape_chain(drawing, rest, chain, &closed);
    Shape run = shape;
    run.edges.clear();
    for (const OrientedEdge &o : chain) {
      run.edges.push_back(o.edge);
      std::erase(rest.edges, o.edge);
    }
    runs.push_back(std::move(run));
  }
  return runs;
}

/* ---------------------------------------------------------------------------------
 * Editing helpers
 */

int drawing_add_point(VectorDrawing &drawing, float2 pos)
{
  drawing.points.push_back({pos});
  return int(drawing.points.size()) - 1;
}

/* Renumbers the shapes' curves and, where `segment_remap` has an entry for a curve (empty: it kept
 * its segments), their segments: segments that joined into one become one edge. A shape whose
 * outline lost a piece (a curve gone, or a segment, -1) is removed: it no longer goes round. */
static void shapes_remap_curves(VectorDrawing &drawing, const std::vector<int> &curve_remap,
                                const std::vector<std::vector<int>> &segment_remap)
{
  const auto new_segment = [&](const ShapeEdge &e) {
    const std::vector<int> &map = segment_remap[e.curve];
    return map.empty() ? e.segment : (e.segment >= 0 && e.segment < int(map.size()) ? map[e.segment] : -1);
  };
  std::erase_if(drawing.shapes, [&](const Shape &shape) {
    return std::any_of(shape.edges.begin(), shape.edges.end(),
                       [&](const ShapeEdge &e) { return curve_remap[e.curve] < 0 || new_segment(e) < 0; });
  });
  for (Shape &shape : drawing.shapes) {
    std::vector<ShapeEdge> edges;
    for (const ShapeEdge &e : shape.edges) {
      const ShapeEdge moved{curve_remap[e.curve], new_segment(e)};
      if (edges.empty() || !(edges.back() == moved)) {
        edges.push_back(moved);
      }
    }
    while (edges.size() > 1 && edges.back() == edges.front()) {
      edges.pop_back();
    }
    shape.edges = std::move(edges);
  }
}

/* Where each segment of a curve goes when the marked points leave it (`removed`, one per curve
 * point; `closed_after`: still a loop): the two segments either side of a removed point join into
 * one; a segment past the last kept point at an open end, -1. */
static std::vector<int> curve_segment_remap(const std::vector<bool> &removed, bool closed_before, bool closed_after)
{
  const int n = int(removed.size());
  std::vector<int> new_index(n, -1);
  int m = 0;
  for (int i = 0; i < n; i++) {
    if (!removed[i]) {
      new_index[i] = m++;
    }
  }
  const int segments = closed_before ? n : n - 1;
  std::vector<int> map(std::max(segments, 0), -1);
  for (int s = 0; s < segments; s++) {
    /* The segment now starting here: from the last kept point at or before s... */
    int start = -1;
    for (int k = 0; k < n; k++) {
      const int i = s - k;
      if (i < 0 && !closed_before) {
        break;
      }
      const int wrapped = (i + n) % n;
      if (!removed[wrapped]) {
        start = new_index[wrapped];
        break;
      }
    }
    /* ...to the first kept point after it, which must be there. */
    bool end = false;
    for (int k = 1; k <= n; k++) {
      const int i = s + k;
      if (i >= n && !closed_before) {
        break;
      }
      if (!removed[i % n]) {
        end = true;
        break;
      }
    }
    if (start < 0 || !end || m < 2) {
      continue;
    }
    const int new_segments = closed_after ? m : m - 1;
    map[s] = start < new_segments ? start : -1;
  }
  return map;
}

void drawing_remove_points(VectorDrawing &drawing, const std::vector<bool> &remove)
{
  DetailScope detail_scope(drawing);
  std::vector<int> point_remap(drawing.points.size(), -1);
  std::vector<MeshPoint> points;
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (!remove[i]) {
      point_remap[i] = int(points.size());
      points.push_back(drawing.points[i]);
    }
  }

  std::vector<int> curve_remap(drawing.curves.size(), -1);
  std::vector<std::vector<int>> segment_remap(drawing.curves.size());
  std::vector<Curve> curves;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve curve = drawing.curves[c];
    const bool closed_before = curve.closed;
    std::vector<bool> removed;
    for (const CurvePoint &cp : curve.points) {
      removed.push_back(remove[cp.point]);
    }
    std::erase_if(curve.points, [&](const CurvePoint &cp) { return remove[cp.point]; });
    for (CurvePoint &cp : curve.points) {
      cp.point = point_remap[cp.point];
    }
    if (curve.closed && curve.points.size() < 3) {
      curve.closed = false;
    }
    if (curve.points.size() != removed.size()) {
      /* Shapes along it keep going round where the curve joins across the gap. */
      segment_remap[c] = curve_segment_remap(removed, closed_before, curve.closed);
    }
    if (curve.points.size() >= 2) {
      curve_remap[c] = int(curves.size());
      curves.push_back(std::move(curve));
    }
  }
  shapes_remap_curves(drawing, curve_remap, segment_remap);
  drawing.points = std::move(points);
  drawing.curves = std::move(curves);

  /* Points no curve uses any more are left over from deleted neighbours; drop them too. */
  std::vector<bool> used(drawing.points.size(), false);
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      used[cp.point] = true;
    }
  }
  std::vector<bool> orphan(drawing.points.size());
  bool any_orphan = false;
  for (size_t i = 0; i < used.size(); i++) {
    orphan[i] = !used[i];
    any_orphan |= orphan[i];
  }
  if (any_orphan) {
    drawing_remove_points(drawing, orphan);
  }
}

void drawing_remove_curves(VectorDrawing &drawing, const std::vector<bool> &remove)
{
  std::vector<int> curve_remap(drawing.curves.size(), -1);
  std::vector<Curve> curves;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    if (!remove[c]) {
      curve_remap[c] = int(curves.size());
      curves.push_back(drawing.curves[c]);
    }
  }
  shapes_remap_curves(drawing, curve_remap, std::vector<std::vector<int>>(drawing.curves.size()));
  drawing.curves = std::move(curves);
}

CurveEnd drawing_curve_end_at(const VectorDrawing &drawing, int point)
{
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    const Curve &curve = drawing.curves[c];
    if (curve.closed || curve.points.empty()) {
      continue;
    }
    if (curve.points.back().point == point) {
      return {int(c), true};
    }
    if (curve.points.front().point == point) {
      return {int(c), false};
    }
  }
  return {};
}

std::vector<ShapeEdge> drawing_segments_at_point(const VectorDrawing &drawing, int point)
{
  std::vector<ShapeEdge> result;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    const Curve &curve = drawing.curves[c];
    for (int s = 0; s < curve.segment_count(); s++) {
      int a, b;
      curve_segment_points(curve, s, &a, &b);
      if (a == point || b == point) {
        result.push_back({int(c), s});
      }
    }
  }
  return result;
}

int drawing_pick_point(const VectorDrawing &drawing, float2 pos, float radius)
{
  int best = -1;
  float best_dist = radius;
  for (size_t i = 0; i < drawing.points.size(); i++) {
    const float d = distance(drawing.points[i].pos, pos);
    /* Detail above its curve's edit level can't be picked. */
    if (d <= best_dist && !drawing.points[i].detail.hidden) {
      best_dist = d;
      best = int(i);
    }
  }
  return best;
}

static constexpr int PICK_STEPS = 24;

bool drawing_pick_segment(const VectorDrawing &drawing, float2 pos, float radius, ShapeEdge *r_edge, float *r_t)
{
  float best = radius;
  bool found = false;
  std::vector<float2> samples;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    const Curve &curve = drawing.curves[c];
    for (int s = 0; s < curve.segment_count(); s++) {
      float2 ctrl[4];
      curve_segment_bezier(drawing, curve, s, ctrl);
      float2 prev = ctrl[0];
      for (int i = 1; i <= PICK_STEPS; i++) {
        const float2 p = bezier_eval(ctrl, float(i) / PICK_STEPS);
        float t_local;
        const float d = distance_to_segment(pos, prev, p, &t_local);
        if (d <= best) {
          best = d;
          found = true;
          *r_edge = {int(c), s};
          *r_t = (float(i - 1) + t_local) / PICK_STEPS;
        }
        prev = p;
      }
    }
  }
  return found;
}

static bool polygon_contains(const std::vector<float2> &poly, float2 p)
{
  bool inside = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const float2 a = poly[i], b = poly[j];
    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
      inside = !inside;
    }
  }
  return inside;
}

int drawing_pick_shape(const VectorDrawing &drawing, float2 pos, float radius)
{
  std::vector<float2> outline;
  for (int s = int(drawing.shapes.size()) - 1; s >= 0; s--) {
    const Shape &shape = drawing.shapes[s];
    bool closed;
    shape_outline(drawing, shape, 12, outline, nullptr, &closed);
    if (outline.size() < 2) {
      continue;
    }
    if (shape.has_fill && closed && polygon_contains(outline, pos)) {
      return s;
    }
    const size_t n = outline.size();
    for (size_t i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
      if (distance_to_segment(pos, outline[i], outline[(i + 1) % n]) <= radius) {
        return s;
      }
    }
  }
  return -1;
}

int drawing_split_segment(VectorDrawing &drawing, ShapeEdge edge, float t)
{
  Curve &curve = drawing.curves[edge.curve];
  float2 ctrl[4];
  curve_segment_bezier(drawing, curve, edge.segment, ctrl);
  const int point = drawing_add_point(drawing, bezier_eval(ctrl, t));

  /* Split the Bezier exactly (de Casteljau), so the line doesn't move: the ends' facing handles
   * shorten, and the new point gets aligned handles along the curve. */
  CurvePoint cp;
  cp.point = point;
  const int n = int(curve.points.size());
  own_handles(drawing, curve, edge.segment);
  own_handles(drawing, curve, (edge.segment + 1) % n);
  CurvePoint &from = curve.points[edge.segment];
  CurvePoint &to = curve.points[(edge.segment + 1) % n];
  const auto lerp2 = [](float2 a, float2 b, float u) { return a + (b - a) * u; };
  const float2 p01 = lerp2(ctrl[0], ctrl[1], t), p12 = lerp2(ctrl[1], ctrl[2], t), p23 = lerp2(ctrl[2], ctrl[3], t);
  const float2 p012 = lerp2(p01, p12, t), p123 = lerp2(p12, p23, t);
  const float2 mid = drawing.points[point].pos;
  from.handle_out = p01 - ctrl[0];
  to.handle_in = p23 - ctrl[3];
  cp.aligned = true;
  cp.handles_set = true;
  cp.handle_in = p012 - mid;
  cp.handle_out = p123 - mid;
  cp.curvature = 0.5f * (curve.points[edge.segment].curvature + curve.points[(edge.segment + 1) % n].curvature);
  cp.edge_hidden = curve.points[edge.segment].edge_hidden;
  if (cp.curvature > 0.0f) {
    cp.curvature = std::max(cp.curvature, CURVATURE_SMOOTH);
  }
  curve.points.insert(curve.points.begin() + edge.segment + 1, cp);

  /* Segments after the split move up by one; the split segment becomes two. */
  for (Shape &shape : drawing.shapes) {
    std::vector<ShapeEdge> edges;
    for (const ShapeEdge &e : shape.edges) {
      if (e.curve != edge.curve) {
        edges.push_back(e);
      }
      else if (e.segment < edge.segment) {
        edges.push_back(e);
      }
      else if (e.segment == edge.segment) {
        edges.push_back({e.curve, e.segment});
        edges.push_back({e.curve, e.segment + 1});
      }
      else {
        edges.push_back({e.curve, e.segment + 1});
      }
    }
    shape.edges = std::move(edges);
  }
  return point;
}

void drawing_remove_unused_points(VectorDrawing &drawing)
{
  std::vector<int> remap(drawing.points.size(), -1);
  std::vector<bool> used(drawing.points.size(), false);
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      used[cp.point] = true;
    }
  }
  std::vector<MeshPoint> points;
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (used[i]) {
      remap[i] = int(points.size());
      points.push_back(drawing.points[i]);
    }
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.point = remap[cp.point];
    }
  }
  drawing.points = std::move(points);
}

/* Where segment s goes when curve point i is removed from a curve of n points, or -1 if the
 * segment disappears (an end segment of an open curve). */
static int segment_after_removal(int s, int i, int n, bool closed)
{
  if (!closed) {
    if (i == 0) {
      return s == 0 ? -1 : s - 1;
    }
    if (i == n - 1) {
      return s == n - 2 ? -1 : s;
    }
  }
  else if (i == 0) {
    /* The last segment and the first join into the new last segment. */
    return (s == 0 || s == n - 1) ? n - 2 : s - 1;
  }
  /* Segments i-1 and i join into i-1; later ones move down. */
  if (s == i) {
    return i - 1;
  }
  return s > i ? s - 1 : s;
}

void drawing_dissolve_point(VectorDrawing &drawing, int point)
{
  DetailScope detail_scope(drawing);
  std::vector<bool> remove_curve(drawing.curves.size(), false);
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve &curve = drawing.curves[c];
    for (int i = int(curve.points.size()) - 1; i >= 0; i--) {
      if (curve.points[i].point != point) {
        continue;
      }
      const int n = int(curve.points.size());
      for (Shape &shape : drawing.shapes) {
        std::vector<ShapeEdge> edges;
        for (ShapeEdge e : shape.edges) {
          if (e.curve == int(c)) {
            e.segment = segment_after_removal(e.segment, i, n, curve.closed);
            if (e.segment < 0) {
              continue;
            }
          }
          if (std::find(edges.begin(), edges.end(), e) == edges.end()) {
            edges.push_back(e);
          }
        }
        shape.edges = std::move(edges);
      }
      curve.points.erase(curve.points.begin() + i);
      if (curve.closed && curve.points.size() < 3) {
        /* Too few points to stay closed: keep the one segment left as an open line. */
        curve.closed = false;
        for (Shape &shape : drawing.shapes) {
          std::erase_if(shape.edges, [&](const ShapeEdge &e) {
            return e.curve == int(c) && e.segment >= curve.segment_count();
          });
        }
      }
    }
    if (curve.points.size() < 2) {
      remove_curve[c] = true;
    }
  }
  std::erase_if(drawing.shapes, [](const Shape &shape) { return shape.edges.empty(); });
  drawing_remove_curves(drawing, remove_curve);
  drawing_remove_unused_points(drawing);
}

/* Schneider's fit of one cubic from `p0` to `p3`, its handles along `t1` (out of p0) and `t2` (out of
 * p3, back along the curve), to the samples `d`: the two handle lengths, by least squares, with the
 * samples' places along the curve refined by Newton steps. */
static void fit_handle_lengths(float2 p0, float2 p3, float2 t1, float2 t2, const std::vector<float2> &d,
                               float *r_a1, float *r_a2)
{
  const float chord = distance(p0, p3);
  *r_a1 = *r_a2 = chord / 3.0f;
  const size_t n = d.size();
  if (n < 3 || chord < 1e-9f) {
    return;
  }
  /* Places along the curve by distance along the samples. */
  std::vector<float> u(n, 0.0f);
  for (size_t i = 1; i < n; i++) {
    u[i] = u[i - 1] + distance(d[i], d[i - 1]);
  }
  for (float &x : u) {
    x /= std::max(u.back(), 1e-9f);
  }
  float a1 = chord / 3.0f, a2 = chord / 3.0f;
  for (int round = 0; round < 24; round++) {
    double c00 = 0, c01 = 0, c11 = 0, x0 = 0, x1 = 0;
    for (size_t i = 0; i < n; i++) {
      const float t = u[i], s = 1.0f - t;
      const float b0 = s * s * s, b1 = 3 * s * s * t, b2 = 3 * s * t * t, b3 = t * t * t;
      const float2 A1 = t1 * b1, A2 = t2 * b2;
      const float2 rest = d[i] - (p0 * (b0 + b1) + p3 * (b2 + b3));
      c00 += dot(A1, A1);
      c01 += dot(A1, A2);
      c11 += dot(A2, A2);
      x0 += dot(rest, A1);
      x1 += dot(rest, A2);
    }
    const double det = c00 * c11 - c01 * c01;
    if (std::fabs(det) < 1e-12) {
      break;
    }
    a1 = float((x0 * c11 - x1 * c01) / det);
    a2 = float((c00 * x1 - c01 * x0) / det);
    /* A handle turned back or vanishing means no good fit this way: thirds of the chord. */
    if (a1 < chord * 1e-3f || a2 < chord * 1e-3f) {
      a1 = a2 = chord / 3.0f;
      break;
    }
    /* Newton steps: each sample's place moves to the nearest point of the curve as now fitted. */
    const float2 c[4] = {p0, p0 + t1 * a1, p3 + t2 * a2, p3};
    for (size_t i = 1; i + 1 < n; i++) {
      const float t = u[i], s = 1.0f - t;
      const float2 q = bezier_eval(c, t);
      const float2 q1 = (c[1] - c[0]) * (3 * s * s) + (c[2] - c[1]) * (6 * s * t) + (c[3] - c[2]) * (3 * t * t);
      const float2 q2 = (c[2] - c[1] * 2.0f + c[0]) * (6 * s) + (c[3] - c[2] * 2.0f + c[1]) * (6 * t);
      const float num = dot(q - d[i], q1), den = dot(q1, q1) + dot(q - d[i], q2);
      if (std::fabs(den) > 1e-12f) {
        u[i] = std::clamp(t - num / den, 0.0f, 1.0f);
      }
    }
  }
  *r_a1 = a1;
  *r_a2 = a2;
}

void drawing_dissolve_points(VectorDrawing &drawing, const std::vector<int> &points)
{
  /* Points are followed by id through the removals. */
  drawing_ensure_ids(drawing);
  std::vector<bool> gone(drawing.points.size(), false);
  for (const int p : points) {
    if (p >= 0 && p < int(gone.size())) {
      gone[p] = true;
    }
  }
  /* First, while the curves are as they were: each span from a kept point over removed ones to the
   * next kept point, sampled, and the handle lengths that fit one segment to it. */
  struct Fit {
    uint64_t from = 0, to = 0;
    float2 out, in;
  };
  std::vector<Fit> fits;
  /* Detail points that go on their own (see below). */
  std::vector<uint64_t> detail_ids;
  for (Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    /* The spans: from a kept point over removed ones to the next kept point. */
    struct Span {
      int a = 0, b = 0, steps = 0;
    };
    std::vector<Span> spans;
    for (int a = 0; a < n; a++) {
      if (gone[curve.points[a].point] || (!curve.closed && a == n - 1)) {
        continue;
      }
      /* The run of removed points after a, and the kept point that ends it. */
      int b = a, steps = 0;
      do {
        b = (b + 1) % n;
        steps++;
      } while (gone[curve.points[b].point] && steps < n && (curve.closed || b < n - 1));
      if (steps < 2 || gone[curve.points[b].point] || b == a || (!curve.closed && b < a)) {
        continue;
      }
      /* Removed points that are all detail above both kept ones (the knife's, at a detail level) ride
       * on the curve of the level below, which goes on as it was: nothing to fit. */
      const int ends = std::max(drawing.points[curve.points[a].point].detail.level,
                                drawing.points[curve.points[b].point].detail.level);
      bool detail_only = true;
      for (int k = 1; k < steps; k++) {
        detail_only &= drawing.points[curve.points[(a + k) % n].point].detail.level > ends;
      }
      if (!detail_only) {
        spans.push_back({a, b, steps});
      }
      else {
        for (int k = 1; k < steps; k++) {
          detail_ids.push_back(drawing.points[curve.points[(a + k) % n].point].id);
        }
      }
    }
    /* The ends of a span keep their handles as they show now (worked out from their neighbours, they
     * would change with a neighbour gone). */
    for (const Span &s : spans) {
      own_handles(drawing, curve, s.a);
      own_handles(drawing, curve, s.b);
    }
    for (const Span &s : spans) {
      const int a = s.a, b = s.b, steps = s.steps;
      std::vector<float2> samples;
      float2 t1, t2;
      for (int k = 0; k < steps; k++) {
        float2 ctrl[4];
        curve_segment_bezier(drawing, curve, (a + k) % n, ctrl);
        if (k == 0) {
          t1 = length(ctrl[1] - ctrl[0]) > 1e-9f ? ctrl[1] - ctrl[0] : ctrl[3] - ctrl[0];
        }
        if (k == steps - 1) {
          t2 = length(ctrl[2] - ctrl[3]) > 1e-9f ? ctrl[2] - ctrl[3] : ctrl[0] - ctrl[3];
        }
        for (int j = k == 0 ? 0 : 1; j <= 24; j++) {
          samples.push_back(bezier_eval(ctrl, float(j) / 24.0f));
        }
      }
      if (length(t1) < 1e-9f || length(t2) < 1e-9f) {
        continue;
      }
      t1 = normalize(t1);
      t2 = normalize(t2);
      float a1 = 0.0f, a2 = 0.0f;
      fit_handle_lengths(samples.front(), samples.back(), t1, t2, samples, &a1, &a2);
      fits.push_back({drawing.points[curve.points[a].point].id, drawing.points[curve.points[b].point].id, t1 * a1, t2 * a2});
    }
  }
  /* Then the points go: detail riding on the level below as Delete Higher removes it (that level's
   * handles kept, so the curve is exactly what it was before the knife), the rest one at a time,
   * highest index first as each dissolve renumbers the ones after it... */
  std::vector<uint64_t> other_ids;
  for (int i = 0; i < int(gone.size()); i++) {
    if (gone[i] && std::find(detail_ids.begin(), detail_ids.end(), drawing.points[i].id) == detail_ids.end()) {
      other_ids.push_back(drawing.points[i].id);
    }
  }
  if (!detail_ids.empty()) {
    drawing_detail_remove_points(drawing, detail_ids);
  }
  std::vector<int> order;
  for (const uint64_t id : other_ids) {
    if (const int p = drawing_point_index(drawing, id); p >= 0) {
      order.push_back(p);
    }
  }
  std::sort(order.begin(), order.end());
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    drawing_dissolve_point(drawing, *it);
  }
  /* ...and the joined segments take the fitted handles (the other handle of each point stays). */
  for (const Fit &f : fits) {
    const int from = drawing_point_index(drawing, f.from), to = drawing_point_index(drawing, f.to);
    if (from < 0 || to < 0) {
      continue;
    }
    for (Curve &curve : drawing.curves) {
      const int n = int(curve.points.size());
      for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        if (curve.points[i].point != from || curve.points[j].point != to || (!curve.closed && j == 0)) {
          continue;
        }
        curve_point_move_handle(drawing, curve, i, false, drawing.points[from].pos + f.out, true);
        curve_point_move_handle(drawing, curve, j, true, drawing.points[to].pos + f.in, true);
      }
    }
  }
}

void drawing_reverse_curve(VectorDrawing &drawing, int curve_index)
{
  DetailScope detail_scope(drawing);
  Curve &curve = drawing.curves[curve_index];
  const int segments = curve.segment_count();
  std::vector<bool> hidden(segments);
  for (int s = 0; s < segments; s++) {
    hidden[s] = curve.points[s].edge_hidden;
  }
  std::reverse(curve.points.begin(), curve.points.end());
  for (CurvePoint &cp : curve.points) {
    std::swap(cp.weight_in, cp.weight_out);
    std::swap(cp.offset_in, cp.offset_out);
    std::swap(cp.handle_in, cp.handle_out);
    std::swap(cp.select_in, cp.select_out);
    /* The setup pose and the animation go with the handles. */
    std::swap(cp.setup_in, cp.setup_out);
    std::swap(cp.change_in, cp.change_out);
    std::swap(cp.handle_in_keys, cp.handle_out_keys);
  }
  if (curve.closed) {
    /* Keep the same first point: [p0, p(n-1), ..., p1]. */
    std::rotate(curve.points.rbegin(), curve.points.rbegin() + 1, curve.points.rend());
  }
  /* Either way, old segment s now runs the other way as segment (segments - 1 - s). */
  for (int k = 0; k < int(curve.points.size()); k++) {
    curve.points[k].edge_hidden = k < segments && hidden[segments - 1 - k];
  }
  for (Shape &shape : drawing.shapes) {
    for (ShapeEdge &e : shape.edges) {
      if (e.curve == curve_index) {
        e.segment = segments - 1 - e.segment;
      }
    }
  }
}

/* Where a shape's outline stops, if it does: the points at either end of its run. */
static bool shape_open_ends(const VectorDrawing &drawing, const Shape &shape, int *r_from, int *r_to)
{
  std::vector<OrientedEdge> chain;
  bool closed = false;
  if (shape.edges.empty() || !shape_chain(drawing, shape, chain, &closed) || closed) {
    return false;
  }
  int a, b;
  curve_segment_points(drawing.curves[chain.front().edge.curve], chain.front().edge.segment, &a, &b);
  *r_from = chain.front().reversed ? b : a;
  curve_segment_points(drawing.curves[chain.back().edge.curve], chain.back().edge.segment, &a, &b);
  *r_to = chain.back().reversed ? a : b;
  return true;
}

bool drawing_make_segment(VectorDrawing &drawing, int a, int b)
{
  const int n_points = int(drawing.points.size());
  if (a == b || a < 0 || b < 0 || a >= n_points || b >= n_points) {
    return false;
  }
  /* The open curves each point ends; both ends of one curve close it. */
  const auto ends_of = [&](int p) {
    std::vector<CurveEnd> ends;
    for (int c = 0; c < int(drawing.curves.size()); c++) {
      const Curve &curve = drawing.curves[c];
      if (curve.closed || curve.points.size() < 2) {
        continue;
      }
      if (curve.points.front().point == p) {
        ends.push_back({c, false});
      }
      if (curve.points.back().point == p) {
        ends.push_back({c, true});
      }
    }
    return ends;
  };
  const std::vector<CurveEnd> ends_a = ends_of(a), ends_b = ends_of(b);
  if (ends_a.empty() || ends_b.empty()) {
    return false;
  }
  CurveEnd end_a = ends_a.front(), end_b = ends_b.front();
  for (const CurveEnd &ea : ends_a) {
    for (const CurveEnd &eb : ends_b) {
      if (ea.curve == eb.curve && ea.at_end != eb.at_end) {
        end_a = ea;
        end_b = eb;
      }
    }
  }
  const bool close = end_a.curve == end_b.curve;
  if (close && drawing.curves[end_a.curve].points.size() < 3) {
    return false;
  }
  if (!close && std::find_if(ends_b.begin(), ends_b.end(), [&](const CurveEnd &e) { return e.curve != end_a.curve; }) ==
                    ends_b.end())
  {
    return false;
  }
  if (!close && end_b.curve == end_a.curve) {
    end_b = *std::find_if(ends_b.begin(), ends_b.end(), [&](const CurveEnd &e) { return e.curve != end_a.curve; });
  }
  DetailScope detail_scope(drawing);
  /* The shapes that stop at either point, before anything moves. */
  int shape_a = -1, shape_b = -1;
  for (int s = 0; s < int(drawing.shapes.size()); s++) {
    int from, to;
    if (!shape_open_ends(drawing, drawing.shapes[s], &from, &to)) {
      continue;
    }
    if (close) {
      /* Closing: the one that runs from one end to the other. */
      if (shape_a < 0 && ((from == a && to == b) || (from == b && to == a))) {
        shape_a = s;
      }
      continue;
    }
    if (shape_a < 0 && (from == a || to == a)) {
      shape_a = s;
    }
    if (shape_b < 0 && (from == b || to == b)) {
      shape_b = s;
    }
  }

  /* A handle with nothing to say about the new segment's way leans a third of the way along it. */
  const auto lean = [&](CurvePoint &cp, bool out, int toward) {
    float2 &h = out ? cp.handle_out : cp.handle_in;
    if (cp.handles_set && length(h) < 1e-6f) {
      h = (drawing.points[toward].pos - drawing.points[cp.point].pos) / 3.0f;
    }
  };
  ShapeEdge added;
  if (close) {
    Curve &curve = drawing.curves[end_a.curve];
    curve.closed = true;
    curve.points.back().edge_hidden = false;
    lean(curve.points.back(), true, curve.points.front().point);
    lean(curve.points.front(), false, curve.points.back().point);
    added = {end_a.curve, curve.segment_count() - 1};
  }
  else {
    /* `a` last on its curve, `b` first on its own: then one runs on into the other. */
    if (!end_a.at_end) {
      drawing_reverse_curve(drawing, end_a.curve);
    }
    if (end_b.at_end) {
      drawing_reverse_curve(drawing, end_b.curve);
    }
    const int ca = end_a.curve, cb = end_b.curve;
    Curve &first = drawing.curves[ca];
    const int na = int(first.points.size());
    first.points.back().edge_hidden = false;
    lean(first.points.back(), true, b);
    lean(drawing.curves[cb].points.front(), false, a);
    first.points.insert(first.points.end(), drawing.curves[cb].points.begin(), drawing.curves[cb].points.end());
    drawing.curves.erase(drawing.curves.begin() + cb);
    const int joined = ca > cb ? ca - 1 : ca;
    for (Shape &shape : drawing.shapes) {
      for (ShapeEdge &e : shape.edges) {
        if (e.curve == cb) {
          e = {joined, na + e.segment};
        }
        else if (e.curve == ca) {
          e.curve = joined;
        }
        else if (e.curve > cb) {
          e.curve--;
        }
      }
    }
    added = {joined, na - 1};
  }
  /* The shapes run on along it: one each side become one. */
  int grown = shape_a >= 0 ? shape_a : shape_b;
  if (grown >= 0) {
    drawing.shapes[grown].edges.push_back(added);
    if (shape_a >= 0 && shape_b >= 0 && shape_a != shape_b) {
      Shape &into = drawing.shapes[shape_a];
      const std::vector<ShapeEdge> more = drawing.shapes[shape_b].edges;
      into.edges.insert(into.edges.end(), more.begin(), more.end());
      drawing.shapes.erase(drawing.shapes.begin() + shape_b);
      grown = shape_b < shape_a ? shape_a - 1 : shape_a;
    }
    std::vector<OrientedEdge> chain;
    bool closed = false;
    if (shape_chain(drawing, drawing.shapes[grown], chain, &closed) && closed) {
      /* Finished: it goes all the way round. */
      drawing.shapes[grown].has_fill = true;
    }
  }
  return true;
}

bool drawing_selection_bounds(const VectorDrawing &drawing, float2 *r_min, float2 *r_max)
{
  bool any = false;
  float2 lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
  float2 hi(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
  for (const MeshPoint &p : drawing.points) {
    if (p.selected) {
      any = true;
      lo = {std::min(lo.x, p.pos.x), std::min(lo.y, p.pos.y)};
      hi = {std::max(hi.x, p.pos.x), std::max(hi.y, p.pos.y)};
    }
  }
  *r_min = lo;
  *r_max = hi;
  return any;
}

bool drawing_duplicate_selected(VectorDrawing &drawing)
{
  DetailScope detail_scope(drawing);
  const int old_point_count = int(drawing.points.size());
  const int old_curve_count = int(drawing.curves.size());
  std::vector<int> point_copy(old_point_count, -1);
  const auto copy_point = [&](int p) {
    if (point_copy[p] < 0) {
      MeshPoint point = drawing.points[p];
      drawing.points.push_back(point);
      point_copy[p] = int(drawing.points.size()) - 1;
    }
    return point_copy[p];
  };
  /* (curve, segment) in the original to (curve, segment) in the copy, for shapes. */
  std::map<std::pair<int, int>, ShapeEdge> segment_copy;

  for (int c = 0; c < old_curve_count; c++) {
    const Curve curve = drawing.curves[c];
    const int n = int(curve.points.size());
    const auto selected = [&](int k) { return drawing.points[curve.points[k].point].selected; };
    int selected_count = 0;
    for (int k = 0; k < n; k++) {
      selected_count += selected(k);
    }
    if (selected_count < 2) {
      continue;
    }
    if (curve.closed && selected_count == n) {
      Curve copy = curve;
      for (CurvePoint &cp : copy.points) {
        cp.point = copy_point(cp.point);
      }
      drawing.curves.push_back(std::move(copy));
      for (int s = 0; s < curve.segment_count(); s++) {
        segment_copy[{c, s}] = {int(drawing.curves.size()) - 1, s};
      }
      continue;
    }
    /* Start each run at a selected point whose predecessor isn't selected; on a closed curve
     * a run may wrap past the last point. */
    for (int start = 0; start < n; start++) {
      const int prev = curve.closed ? (start + n - 1) % n : start - 1;
      if (!selected(start) || (prev >= 0 && selected(prev))) {
        continue;
      }
      Curve copy;
      std::vector<int> segments;
      int k = start;
      for (int steps = 0; steps < n && selected(k); steps++) {
        CurvePoint cp = curve.points[k];
        cp.point = copy_point(cp.point);
        copy.points.push_back(cp);
        const int next = k + 1;
        if (next >= n && !curve.closed) {
          break;
        }
        if (selected(next % n) && steps + 1 < n) {
          segments.push_back(k);
        }
        k = next % n;
      }
      if (copy.points.size() < 2) {
        continue;
      }
      drawing.curves.push_back(std::move(copy));
      const int new_curve = int(drawing.curves.size()) - 1;
      for (int i = 0; i < int(segments.size()); i++) {
        segment_copy[{c, segments[i]}] = {new_curve, i};
      }
    }
  }
  if (int(drawing.curves.size()) == old_curve_count) {
    drawing.points.resize(old_point_count);
    return false;
  }

  const int old_shape_count = int(drawing.shapes.size());
  for (int s = 0; s < old_shape_count; s++) {
    Shape copy = drawing.shapes[s];
    bool covered = !copy.edges.empty();
    for (ShapeEdge &edge : copy.edges) {
      auto it = segment_copy.find({edge.curve, edge.segment});
      if (it == segment_copy.end()) {
        covered = false;
        break;
      }
      edge = it->second;
    }
    if (covered) {
      drawing.shapes.push_back(std::move(copy));
    }
  }

  for (int p = 0; p < int(drawing.points.size()); p++) {
    drawing.points[p].selected = p >= old_point_count;
  }
  return true;
}

std::vector<bool> drawing_selected_shapes(const VectorDrawing &drawing)
{
  std::vector<bool> selected(drawing.shapes.size(), false);
  for (size_t s = 0; s < drawing.shapes.size(); s++) {
    for (const ShapeEdge &edge : drawing.shapes[s].edges) {
      int a, b;
      curve_segment_points(drawing.curves[edge.curve], edge.segment, &a, &b);
      if (drawing.points[a].selected || drawing.points[b].selected) {
        selected[s] = true;
        break;
      }
    }
  }
  return selected;
}

bool drawing_arrange_shapes(VectorDrawing &drawing, ShapeArrange arrange)
{
  std::vector<bool> selected = drawing_selected_shapes(drawing);
  const int n = int(drawing.shapes.size());
  std::vector<int> order(n);
  for (int s = 0; s < n; s++) {
    order[s] = s;
  }
  switch (arrange) {
    case ShapeArrange::Forward:
      /* From the top down, so a block of selected shapes moves up together. */
      for (int s = n - 2; s >= 0; s--) {
        if (selected[order[s]] && !selected[order[s + 1]]) {
          std::swap(order[s], order[s + 1]);
        }
      }
      break;
    case ShapeArrange::Backward:
      for (int s = 1; s < n; s++) {
        if (selected[order[s]] && !selected[order[s - 1]]) {
          std::swap(order[s], order[s - 1]);
        }
      }
      break;
    case ShapeArrange::ToFront:
      std::stable_partition(order.begin(), order.end(), [&](int s) { return !selected[s]; });
      break;
    case ShapeArrange::ToBack:
      std::stable_partition(order.begin(), order.end(), [&](int s) { return selected[s]; });
      break;
  }
  bool changed = false;
  for (int s = 0; s < n; s++) {
    changed = changed || order[s] != s;
  }
  if (!changed) {
    return false;
  }
  std::vector<Shape> shapes;
  shapes.reserve(n);
  for (const int s : order) {
    shapes.push_back(std::move(drawing.shapes[s]));
  }
  drawing.shapes = std::move(shapes);
  return true;
}

float drawing_selected_mean_radius(const VectorDrawing &drawing)
{
  float sum = 0.0f;
  int count = 0;
  for (const MeshPoint &p : drawing.points) {
    if (p.selected) {
      sum += p.radius;
      count++;
    }
  }
  return count ? sum / float(count) : -1.0f;
}

void drawing_set_selected_mean_radius(VectorDrawing &drawing, float mean)
{
  const float current = drawing_selected_mean_radius(drawing);
  if (current < 0.0f) {
    return;
  }
  const float delta = std::clamp(mean, 0.0f, POINT_RADIUS_MAX) - current;
  for (MeshPoint &p : drawing.points) {
    if (p.selected) {
      p.radius = std::clamp(p.radius + delta, 0.0f, POINT_RADIUS_MAX);
    }
  }
}

/* ---------------------------------------------------------------------------------
 * Shape tool primitives
 */

const char *primitive_name(PrimitiveKind kind)
{
  switch (kind) {
    case PrimitiveKind::Box:
      return "Box";
    case PrimitiveKind::Circle:
      return "Circle";
  }
  return "Shape";
}

const char *primitive_identifier(PrimitiveKind kind)
{
  switch (kind) {
    case PrimitiveKind::Box:
      return "BOX";
    case PrimitiveKind::Circle:
      return "CIRCLE";
  }
  return "BOX";
}

bool primitive_from_identifier(const std::string &identifier, PrimitiveKind *r_kind)
{
  for (const PrimitiveKind kind : PRIMITIVE_KINDS) {
    if (identifier == primitive_identifier(kind)) {
      *r_kind = kind;
      return true;
    }
  }
  return false;
}

VectorDrawing primitive_drawing(PrimitiveKind kind, const std::vector<float2> &points, const Style &style,
                                bool fill)
{
  VectorDrawing drawing;
  constexpr float EPS = 1e-5f;
  /* Moho's circle curvature: four smooth points make a circle when each handle is 0.5523 of
   * the radius (the usual Bezier circle), and Moho measures handles from the neighbour. */
  constexpr float BEZIER_CIRCLE = 0.5523f;

  std::vector<float2> pos;
  std::vector<float> curvature;
  bool is_closed = false;
  const float2 a = points.empty() ? float2() : points.front();
  const float2 b = points.empty() ? float2() : points.back();

  switch (kind) {
    case PrimitiveKind::Box: {
      if (points.size() < 2 || std::abs(b.x - a.x) < EPS || std::abs(b.y - a.y) < EPS) {
        return drawing;
      }
      pos = {a, {b.x, a.y}, b, {a.x, b.y}};
      curvature.assign(4, CURVATURE_PEAKED);
      is_closed = true;
      break;
    }
    case PrimitiveKind::Circle: {
      const float rx = std::abs(b.x - a.x) * 0.5f, ry = std::abs(b.y - a.y) * 0.5f;
      if (points.size() < 2 || rx < EPS || ry < EPS) {
        return drawing;
      }
      const float2 c = (a + b) * 0.5f;
      const float neighbour = std::sqrt(rx * rx + ry * ry);
      pos = {{c.x + rx, c.y}, {c.x, c.y + ry}, {c.x - rx, c.y}, {c.x, c.y - ry}};
      const float on_x = BEZIER_CIRCLE * ry / neighbour, on_y = BEZIER_CIRCLE * rx / neighbour;
      curvature = {on_x, on_y, on_x, on_y};
      is_closed = true;
      break;
    }
  }

  Curve curve;
  curve.closed = is_closed;
  for (size_t k = 0; k < pos.size(); k++) {
    drawing.points.push_back({pos[k]});
    CurvePoint cp;
    cp.point = int(k);
    /* Box corners have curvature 0: their handles point straight along the sides. */
    cp.curvature = curvature[k];
    curve.points.push_back(cp);
  }
  drawing.curves.push_back(std::move(curve));

  Shape shape;
  shape.name = primitive_name(kind);
  shape.style = style;
  shape.has_outline = true;
  shape.has_fill = fill && is_closed;
  for (int seg = 0; seg < drawing.curves[0].segment_count(); seg++) {
    shape.edges.push_back({0, seg});
  }
  drawing.shapes.push_back(std::move(shape));
  return drawing;
}

void drawing_append_selected(VectorDrawing &dst, const VectorDrawing &src)
{
  for (MeshPoint &p : dst.points) {
    p.selected = false;
  }
  const int point_offset = int(dst.points.size());
  const int curve_offset = int(dst.curves.size());
  for (MeshPoint p : src.points) {
    p.selected = true;
    dst.points.push_back(p);
  }
  for (Curve curve : src.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.point += point_offset;
    }
    dst.curves.push_back(std::move(curve));
  }
  for (Shape shape : src.shapes) {
    for (ShapeEdge &edge : shape.edges) {
      edge.curve += curve_offset;
    }
    shape.name += " " + std::to_string(dst.shapes.size() + 1);
    dst.shapes.push_back(std::move(shape));
  }
}

std::vector<ShapeEdge> drawing_selected_edges(const VectorDrawing &drawing)
{
  std::vector<ShapeEdge> edges;
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    for (int s = 0; s < drawing.curves[c].segment_count(); s++) {
      int a, b;
      curve_segment_points(drawing.curves[c], s, &a, &b);
      if (drawing.points[a].selected && drawing.points[b].selected) {
        edges.push_back({c, s});
      }
    }
  }
  return edges;
}

void drawing_set_edges_hidden(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges, bool hidden)
{
  for (const ShapeEdge &e : edges) {
    drawing.curves[e.curve].points[e.segment].edge_hidden = hidden;
  }
}

void drawing_set_selected_corner(VectorDrawing &drawing, CornerType corner)
{
  for (MeshPoint &p : drawing.points) {
    if (p.selected) {
      p.corner = corner;
    }
  }
}

std::vector<int> drawing_selected_points(const VectorDrawing &drawing)
{
  std::vector<int> points;
  for (int i = 0; i < int(drawing.points.size()); i++) {
    if (drawing.points[i].selected) {
      points.push_back(i);
    }
  }
  return points;
}

std::vector<int> shape_points(const VectorDrawing &drawing, const Shape &shape)
{
  std::vector<int> points;
  for (const ShapeEdge &e : shape.edges) {
    if (e.curve < 0 || e.curve >= int(drawing.curves.size())) {
      continue;
    }
    int a, b;
    curve_segment_points(drawing.curves[e.curve], e.segment, &a, &b);
    for (const int p : {a, b}) {
      if (std::find(points.begin(), points.end(), p) == points.end()) {
        points.push_back(p);
      }
    }
  }
  return points;
}

bool drawing_points_mean(const VectorDrawing &drawing, const std::vector<int> &points, float2 *r_mean)
{
  if (points.empty()) {
    return false;
  }
  float2 sum;
  for (const int p : points) {
    sum += drawing.points[p].pos;
  }
  *r_mean = sum / float(points.size());
  return true;
}

void drawing_points_move(VectorDrawing &drawing, const std::vector<int> &points, float2 delta)
{
  for (const int p : points) {
    drawing.points[p].pos += delta;
  }
}

void drawing_deselect_handles(VectorDrawing &drawing)
{
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.select_in = cp.select_out = false;
    }
  }
}

bool curve_point_selected(const VectorDrawing &drawing, const CurvePoint &cp)
{
  return drawing.points[cp.point].selected || cp.select_in || cp.select_out;
}

int drawing_align_selected_handles(VectorDrawing &drawing, bool align)
{
  int changed = 0;
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (!curve_point_selected(drawing, cp)) {
        continue;
      }
      if (align) {
        /* With just the out handle picked, it is the one that turns into line (the handle you
         * pick is the one you are aligning); otherwise the in handle does. */
        const bool only_out = cp.select_out && !cp.select_in && !drawing.points[cp.point].selected;
        curve_point_align(drawing, curve, i, only_out);
        changed++;
      }
      else if (cp.aligned) {
        own_handles(drawing, curve, i);
        curve.points[i].aligned = false;
        changed++;
      }
    }
  }
  return changed;
}

int drawing_selected_alignment(const VectorDrawing &drawing)
{
  int found = -1;
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      if (!curve_point_selected(drawing, cp)) {
        continue;
      }
      const int a = cp.aligned ? 1 : 0;
      if (found == -1) {
        found = a;
      }
      else if (found != a) {
        return -2;
      }
    }
  }
  return found;
}

int drawing_set_selected_handles_shape(VectorDrawing &drawing, bool peak)
{
  int changed = 0;
  for (Curve &curve : drawing.curves) {
    std::vector<std::pair<float2, float2>> handles(curve.points.size());
    for (int i = 0; i < int(curve.points.size()); i++) {
      const float2 here = drawing.points[curve.points[i].point].pos;
      handles[i] = {computed_handle(drawing, curve, i, true, peak ? 1 : 2) - here,
                    computed_handle(drawing, curve, i, false, peak ? 1 : 2) - here};
    }
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (!drawing.points[cp.point].selected) {
        continue;
      }
      own_handles(drawing, curve, i);
      cp.handle_in = handles[i].first;
      cp.handle_out = handles[i].second;
      cp.aligned = !peak;
      cp.curvature = peak ? CURVATURE_PEAKED : std::max(cp.curvature, CURVATURE_SMOOTH);
      changed++;
    }
  }
  return changed;
}

void drawing_repair_stale_handle_keys(VectorDrawing &drawing)
{
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (!cp.handles_set) {
        continue;
      }
      Key<float2> *in = cp.handle_in_keys.key_at(0);
      Key<float2> *out = cp.handle_out_keys.key_at(0);
      if (!in || !out || !(in->value == float2()) || !(out->value == float2())) {
        continue;
      }
      /* Both handles zero on frame 0: the stale values the bug keyed. Use the handles the point
       * had in the setup pose, worked out from its neighbours. */
      CurvePoint saved = cp;
      cp.handles_set = false;
      cp.handle_in_keys = {};
      cp.handle_out_keys = {};
      float2 setup_in, setup_out;
      setup_handles(setup_points(drawing), curve, i, &setup_in, &setup_out);
      cp = saved;
      cp.handle_in_keys.key_at(0)->value = setup_in;
      cp.handle_out_keys.key_at(0)->value = setup_out;
    }
  }
}

int drawing_delete_edges(VectorDrawing &drawing, const std::vector<ShapeEdge> &edges)
{
  DetailScope detail_scope(drawing);
  const auto doomed = [&](int c, int seg) {
    return std::find(edges.begin(), edges.end(), ShapeEdge{c, seg}) != edges.end();
  };
  /* Where each old segment is now: (new curve, new segment), or curve -1 when deleted. */
  std::vector<std::vector<ShapeEdge>> moved(drawing.curves.size());
  std::vector<Curve> curves;
  int deleted = 0;
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    const Curve &curve = drawing.curves[c];
    const int segs = curve.segment_count();
    moved[c].assign(std::max(segs, 0), ShapeEdge{-1, -1});
    int first_doomed = -1;
    for (int s = 0; s < segs && first_doomed < 0; s++) {
      first_doomed = doomed(c, s) ? s : -1;
    }
    if (first_doomed < 0) {
      for (int s = 0; s < segs; s++) {
        moved[c][s] = {int(curves.size()), s};
      }
      curves.push_back(curve);
      continue;
    }
    /* Walk the segments, starting just after a deleted one on a closed curve, and cut a new open
     * curve at every deleted segment. */
    const int n = int(curve.points.size());
    const int start = curve.closed ? first_doomed + 1 : 0;
    Curve run;
    const auto flush = [&] {
      if (run.points.size() >= 2) {
        run.closed = false;
        run.points.back().edge_hidden = false;
        curves.push_back(run);
      }
      run = Curve();
    };
    for (int k = 0; k < segs; k++) {
      const int s = (start + k) % std::max(segs, 1);
      if (doomed(c, s)) {
        deleted++;
        flush();
        continue;
      }
      if (run.points.empty()) {
        run.points.push_back(curve.points[s]);
      }
      moved[c][s] = {int(curves.size()), int(run.points.size()) - 1};
      run.points.push_back(curve.points[(s + 1) % n]);
    }
    flush();
  }
  /* Shapes: their surviving segments, renumbered; a shape is kept if anything is left. */
  std::vector<Shape> shapes;
  for (Shape shape : drawing.shapes) {
    std::vector<ShapeEdge> kept;
    bool lost = false;
    for (const ShapeEdge &e : shape.edges) {
      const bool known = e.curve >= 0 && e.curve < int(moved.size()) && e.segment >= 0 &&
                         e.segment < int(moved[e.curve].size());
      if (known && moved[e.curve][e.segment].curve >= 0) {
        kept.push_back(moved[e.curve][e.segment]);
      }
      else {
        lost = true;
      }
    }
    if (kept.empty()) {
      continue;
    }
    shape.edges = kept;
    shapes.push_back(std::move(shape));
    if (lost) {
      /* A gap in its outline: it can't fill unless what is left still closes. */
      VectorDrawing probe;
      probe.points = drawing.points;
      probe.curves = curves;
      std::vector<float2> outline;
      bool closed = false;
      shape_outline(probe, shapes.back(), 1, outline, nullptr, &closed);
      if (!closed) {
        shapes.back().has_fill = false;
      }
    }
  }
  drawing.curves = std::move(curves);
  drawing.shapes = std::move(shapes);
  /* Points always stay, even one left on no curve (Moho keeps such points too). */
  return deleted;
}

static float2 reflect2(float2 v, bool x, bool y)
{
  return {x ? -v.x : v.x, y ? -v.y : v.y};
}

float2 point_setup_pos(const MeshPoint &point)
{
  /* Not seen by the keys yet (just drawn): where it is, is its setup. */
  return point.pos_keys.has_last || point.pos_keys.animated() ? point.setup_pos : point.pos;
}

float2 handle_setup(const CurvePoint &cp, bool in)
{
  const Channel<float2> &ch = in ? cp.handle_in_keys : cp.handle_out_keys;
  return ch.has_last || ch.animated() ? (in ? cp.setup_in : cp.setup_out) : (in ? cp.handle_in : cp.handle_out);
}

static float wrap_turn(float a)
{
  return a - 2.0f * PI * std::floor((a + PI) / (2.0f * PI));
}

float2 handle_change(float2 setup, float2 handle, float near_turn)
{
  const float setup_length = length(setup), handle_length = length(handle);
  const float setup_angle = setup_length > 1e-9f ? std::atan2(setup.y, setup.x) : 0.0f;
  const float handle_angle = handle_length > 1e-9f ? std::atan2(handle.y, handle.x) : setup_angle + near_turn;
  return {near_turn + wrap_turn(handle_angle - setup_angle - near_turn), handle_length - setup_length};
}

float2 handle_from_change(float2 setup, float2 change)
{
  if (change == float2()) {
    return setup;
  }
  const float setup_length = length(setup);
  const float angle = (setup_length > 1e-9f ? std::atan2(setup.y, setup.x) : 0.0f) + change.x;
  const float l = std::max(0.0f, setup_length + change.y);
  return {std::cos(angle) * l, std::sin(angle) * l};
}

void drawing_changes_from_values(VectorDrawing &drawing)
{
  /* Detail edits since it was last placed become its offsets; its keys hold their change from the
   * setup. */
  drawing_detail_sync(drawing, true);
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      PointDetail &dt = p.detail;
      if (!p.pos_keys.has_last && !p.pos_keys.animated()) {
        dt.setup_pos = dt.pos;
        p.setup_pos = p.pos;
        p.transform = {};
      }
      else if (dt.pos != dt.setup_pos + p.transform) {
        p.transform = dt.pos - dt.setup_pos;
      }
      continue;
    }
    if (!p.pos_keys.has_last && !p.pos_keys.animated()) {
      p.setup_pos = p.pos;
      p.transform = {};
    }
    else if (p.pos != p.setup_pos + p.transform) {
      p.transform = p.pos - p.setup_pos;
    }
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      PointDetail &dt = drawing.points[cp.point].detail;
      for (const bool in : {true, false}) {
        const Channel<float2> &ch = in ? cp.handle_in_keys : cp.handle_out_keys;
        float2 &change = in ? cp.change_in : cp.change_out;
        if (dt.level > 0) {
          const float2 offset = in ? dt.in : dt.out;
          float2 &detail_setup = in ? dt.setup_in : dt.setup_out;
          if (!ch.has_last && !ch.animated()) {
            detail_setup = offset;
            (in ? cp.setup_in : cp.setup_out) = in ? cp.handle_in : cp.handle_out;
            change = {};
          }
          else if (offset != detail_setup + change) {
            change = offset - detail_setup;
          }
          continue;
        }
        const float2 handle = in ? cp.handle_in : cp.handle_out;
        float2 &setup = in ? cp.setup_in : cp.setup_out;
        if (!ch.has_last && !ch.animated()) {
          setup = handle;
          change = {};
        }
        else if (handle != handle_from_change(setup, change)) {
          change = handle_change(setup, handle, change.x);
        }
      }
    }
  }
}

void drawing_values_from_changes(VectorDrawing &drawing)
{
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      p.detail.pos = p.detail.setup_pos + p.transform;
      continue;
    }
    p.pos = p.setup_pos + p.transform;
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      if (PointDetail &dt = drawing.points[cp.point].detail; dt.level > 0) {
        dt.in = dt.setup_in + cp.change_in;
        dt.out = dt.setup_out + cp.change_out;
        continue;
      }
      cp.handle_in = handle_from_change(cp.setup_in, cp.change_in);
      cp.handle_out = handle_from_change(cp.setup_out, cp.change_out);
    }
  }
  /* Detail placed from its offsets, on the curve as it now is. */
  drawing_detail_sync(drawing, false);
}

void drawing_take_setup(VectorDrawing &drawing)
{
  drawing_detail_sync(drawing, true);
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      p.detail.setup_pos = p.detail.pos - p.pos_keys.smart;
      p.transform = p.detail.pos - p.detail.setup_pos;
      p.setup_pos = p.pos;
      continue;
    }
    p.setup_pos = p.pos - p.pos_keys.smart;
    p.transform = p.pos - p.setup_pos;
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.setup_in = cp.handle_in;
      cp.setup_out = cp.handle_out;
      cp.change_in = cp.change_out = {};
      if (PointDetail &dt = drawing.points[cp.point].detail; dt.level > 0) {
        dt.setup_in = dt.in;
        dt.setup_out = dt.out;
      }
    }
  }
}

int drawing_clear_selected_pose(VectorDrawing &drawing, bool positions, bool handles)
{
  /* Back to the setup pose plus what smart actions add now (their part of the channel), so a cleared
   * point lands where the smart bones put it rather than keying a change that cancels them. */
  int changed = 0;
  /* Detail goes back to its setup offsets, and rides its curve from there. */
  drawing_detail_sync(drawing, true);
  bool detail = false;
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      MeshPoint &p = drawing.points[cp.point];
      PointDetail &dt = p.detail;
      if (dt.level == 0) {
        continue;
      }
      const float2 rest = dt.setup_pos + p.pos_keys.smart;
      if (positions && p.selected && dt.pos != rest && (p.pos_keys.has_last || p.pos_keys.animated())) {
        dt.pos = rest;
        changed++;
        detail = true;
      }
      for (const bool in : {true, false}) {
        const Channel<float2> &ch = in ? cp.handle_in_keys : cp.handle_out_keys;
        float2 &offset = in ? dt.in : dt.out;
        const float2 setup = (in ? dt.setup_in : dt.setup_out) + ch.smart;
        if (handles && (p.selected || (in ? cp.select_in : cp.select_out)) && offset != setup &&
            (ch.has_last || ch.animated()))
        {
          offset = setup;
          changed++;
          detail = true;
        }
      }
    }
  }
  if (detail) {
    drawing_detail_sync(drawing, false);
  }
  for (MeshPoint &p : drawing.points) {
    if (p.detail.level > 0) {
      continue;
    }
    const float2 rest = point_setup_pos(p) + p.pos_keys.smart;
    if (positions && p.selected && p.pos != rest) {
      p.pos = rest;
      changed++;
    }
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      if (!handles) {
        break;
      }
      const bool point = drawing.points[cp.point].selected;
      if (drawing.points[cp.point].detail.level > 0) {
        continue;
      }
      for (const bool in : {true, false}) {
        float2 &handle = in ? cp.handle_in : cp.handle_out;
        /* A handle's smart part is a change of its Angle and Length. */
        const float2 rest = handle_from_change(handle_setup(cp, in), (in ? cp.handle_in_keys : cp.handle_out_keys).smart);
        if (cp.handles_set && (point || (in ? cp.select_in : cp.select_out)) && handle != rest) {
          handle = rest;
          changed++;
        }
      }
    }
  }
  return changed;
}

void drawing_new_points_follow(VectorDrawing &drawing, int frame)
{
  const auto is_new = [&](int pi) {
    const MeshPoint &p = drawing.points[pi];
    /* New detail rides its curve: it moves with it already. */
    return !p.pos_keys.has_last && !p.pos_keys.animated() && p.detail.level == 0;
  };
  std::vector<bool> done(drawing.points.size(), false);
  for (const Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    for (int i = 0; i < n; i++) {
      const int pi = curve.points[i].point;
      if (done[pi] || !is_new(pi)) {
        continue;
      }
      /* The nearest recorded point each way along the curve, and how far along the curve it is. */
      const auto neighbour = [&](int step, float *r_distance) -> int {
        float d = 0.0f;
        int j = i;
        for (int walked = 0; walked < n; walked++) {
          const int k = j + step;
          if (k < 0 || k >= n) {
            if (!curve.closed) {
              return -1;
            }
          }
          const int next = (k + n) % n;
          d += distance(drawing.points[curve.points[j].point].pos, drawing.points[curve.points[next].point].pos);
          j = next;
          if (j == i) {
            return -1;
          }
          if (!is_new(curve.points[j].point)) {
            *r_distance = d;
            return curve.points[j].point;
          }
        }
        return -1;
      };
      float d_prev = 0.0f, d_next = 0.0f;
      const int prev = neighbour(-1, &d_prev), next = neighbour(1, &d_next);
      const bool prev_moves = prev >= 0 && drawing.points[prev].pos_keys.animated();
      const bool next_moves = next >= 0 && drawing.points[next].pos_keys.animated();
      if (!prev_moves && !next_moves) {
        continue;
      }
      /* Between two, blended by distance along the curve; past an end, the end's motion. */
      const float t = prev >= 0 && next >= 0 && d_prev + d_next > 1e-9f ? d_prev / (d_prev + d_next) : (prev >= 0 ? 0.0f : 1.0f);
      const auto change_of = [&](int q, int f) -> float2 {
        if (q < 0) {
          return {};
        }
        const MeshPoint &m = drawing.points[q];
        return m.pos_keys.animated() ? m.pos_keys.evaluate(f) : m.pos - m.setup_pos;
      };
      const auto change_at = [&](int f) { return change_of(prev, f) * (1.0f - t) + change_of(next, f) * t; };
      std::set<int> frames{0};
      for (const int q : {prev, next}) {
        if (q >= 0) {
          for (const Key<float2> &k : drawing.points[q].pos_keys.keys) {
            frames.insert(k.frame);
          }
        }
      }
      MeshPoint &p = drawing.points[pi];
      const float2 now = change_at(std::max(frame, 0));
      p.setup_pos = p.pos - now;
      for (const int f : frames) {
        Key<float2> &key = p.pos_keys.set_key(f, change_at(f));
        /* Timed as the neighbour it follows most. */
        const int lead = t <= 0.5f ? (prev_moves ? prev : next) : (next_moves ? next : prev);
        if (const Key<float2> *src = drawing.points[lead].pos_keys.key_at(f)) {
          key_settings_apply(key, key_settings(*src));
        }
      }
      p.transform = now;
      p.pos_keys.last = now;
      p.pos_keys.has_last = true;
      done[pi] = true;
    }
  }
}

int drawing_find_mirror_point(const VectorDrawing &drawing, int point, bool x, bool y)
{
  const float2 here = point_setup_pos(drawing.points[point]);
  const float2 target = reflect2(here, x, y);
  const float eps = 1e-4f + 1e-4f * (std::fabs(here.x) + std::fabs(here.y));
  if (distance(target, here) <= eps) {
    return -1;
  }
  for (int j = 0; j < int(drawing.points.size()); j++) {
    if (j != point && distance(point_setup_pos(drawing.points[j]), target) <= eps) {
      return j;
    }
  }
  return -1;
}

void drawing_mirror_handles(VectorDrawing &drawing, int src, int dst, bool x, bool y)
{
  const auto neighbour = [&](const Curve &curve, int i, int dir) {
    const int n = int(curve.points.size());
    const int k = i + dir;
    if (!curve.closed && (k < 0 || k >= n)) {
      return -1;
    }
    return curve.points[(k % n + n) % n].point;
  };
  const auto near = [&](int a, float2 b) {
    return a >= 0 && distance(point_setup_pos(drawing.points[a]), b) <= 1e-4f + 1e-4f * (std::fabs(b.x) + std::fabs(b.y));
  };
  for (const Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      if (curve.points[i].point != src) {
        continue;
      }
      const CurvePoint &from = curve.points[i];
      const int prev = neighbour(curve, i, -1), next = neighbour(curve, i, 1);
      for (Curve &other : drawing.curves) {
        for (int j = 0; j < int(other.points.size()); j++) {
          if (other.points[j].point != dst) {
            continue;
          }
          /* Same direction when the neighbours mirror onto the same sides, else swapped. */
          const int oprev = neighbour(other, j, -1), onext = neighbour(other, j, 1);
          bool swap;
          if (prev >= 0 && near(oprev, reflect2(point_setup_pos(drawing.points[prev]), x, y))) {
            swap = false;
          }
          else if (prev >= 0 && near(onext, reflect2(point_setup_pos(drawing.points[prev]), x, y))) {
            swap = true;
          }
          else if (next >= 0 && near(onext, reflect2(point_setup_pos(drawing.points[next]), x, y))) {
            swap = false;
          }
          else if (next >= 0 && near(oprev, reflect2(point_setup_pos(drawing.points[next]), x, y))) {
            swap = true;
          }
          else {
            continue;
          }
          CurvePoint &to = other.points[j];
          to.aligned = from.aligned;
          to.handles_set = from.handles_set;
          to.curvature = from.curvature;
          to.handle_in = reflect2(swap ? from.handle_out : from.handle_in, x, y);
          to.handle_out = reflect2(swap ? from.handle_in : from.handle_out, x, y);
        }
      }
    }
  }
}

std::vector<MirrorPair> drawing_mirror_pairs(const VectorDrawing &drawing, const std::vector<int> &moving, bool mirror_x,
                                             bool mirror_y)
{
  std::vector<MirrorPair> pairs;
  for (const int src : moving) {
    const float2 here = point_setup_pos(drawing.points[src]);
    const float eps = 1e-4f + 1e-4f * (std::fabs(here.x) + std::fabs(here.y));
    for (const auto &[x, y] : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
      if ((x && !mirror_x) || (y && !mirror_y)) {
        continue;
      }
      const float2 target = reflect2(here, x, y);
      if (distance(target, here) <= eps) {
        continue; /* On the axis: its own mirror. */
      }
      for (int j = 0; j < int(drawing.points.size()); j++) {
        if (!drawing.points[j].selected && distance(point_setup_pos(drawing.points[j]), target) <= eps) {
          pairs.push_back({src, j, x, y});
          break;
        }
      }
    }
  }
  return pairs;
}

void drawing_apply_mirror(VectorDrawing &drawing, const std::vector<MirrorPair> &pairs)
{
  for (const MirrorPair &m : pairs) {
    drawing.points[m.dst].pos = reflect2(drawing.points[m.src].pos, m.x, m.y);
    drawing_mirror_handles(drawing, m.src, m.dst, m.x, m.y);
  }
}

void drawing_mirror_selected_handles(VectorDrawing &drawing, bool mirror_x, bool mirror_y)
{
  if (!mirror_x && !mirror_y) {
    return;
  }
  std::vector<int> sources;
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      if (curve_point_selected(drawing, cp) &&
          std::find(sources.begin(), sources.end(), cp.point) == sources.end()) {
        sources.push_back(cp.point);
      }
    }
  }
  for (const int src : sources) {
    for (const auto &[x, y] : {std::pair{true, false}, std::pair{false, true}, std::pair{true, true}}) {
      if ((x && !mirror_x) || (y && !mirror_y)) {
        continue;
      }
      const int dst = drawing_find_mirror_point(drawing, src, x, y);
      if (dst >= 0 && std::find(sources.begin(), sources.end(), dst) == sources.end()) {
        drawing_mirror_handles(drawing, src, dst, x, y);
      }
    }
  }
}

/* --- Proportional editing ----------------------------------------------------------------- */

static const char *const FALLOFF_NAMES[PROPORTIONAL_FALLOFF_COUNT][2] = {
    {"Smooth", "SMOOTH"}, {"Sphere", "SPHERE"}, {"Root", "ROOT"},         {"Inverse Square", "INVERSE_SQUARE"},
    {"Sharp", "SHARP"},   {"Linear", "LINEAR"}, {"Constant", "CONSTANT"}, {"Random", "RANDOM"}};

const char *proportional_falloff_name(ProportionalFalloff falloff)
{
  return FALLOFF_NAMES[int(falloff)][0];
}

const char *proportional_falloff_identifier(ProportionalFalloff falloff)
{
  return FALLOFF_NAMES[int(falloff)][1];
}

bool proportional_falloff_from_identifier(const std::string &id, ProportionalFalloff *r_falloff)
{
  for (int i = 0; i < PROPORTIONAL_FALLOFF_COUNT; i++) {
    if (id == FALLOFF_NAMES[i][1]) {
      *r_falloff = ProportionalFalloff(i);
      return true;
    }
  }
  return false;
}

std::vector<float> drawing_proportional_distances(const VectorDrawing &drawing, const std::vector<float2> &positions,
                                                  bool connected)
{
  const int n = int(drawing.points.size());
  std::vector<float> dist(n, std::numeric_limits<float>::infinity());
  std::vector<int> selected;
  for (int i = 0; i < n; i++) {
    if (drawing.points[i].selected) {
      dist[i] = 0.0f;
      selected.push_back(i);
    }
  }
  if (selected.empty() || int(positions.size()) < n) {
    return dist;
  }
  if (!connected) {
    for (int i = 0; i < n; i++) {
      for (const int s : selected) {
        dist[i] = std::min(dist[i], distance(positions[i], positions[s]));
      }
    }
    return dist;
  }
  /* Along the curves: out from the selection to each point's neighbours, nearest first. */
  std::vector<std::vector<int>> next(n);
  for (const Curve &curve : drawing.curves) {
    const int m = int(curve.points.size());
    for (int k = 0; k < m; k++) {
      const int a = curve.points[k].point;
      const int b = k + 1 < m ? curve.points[k + 1].point : (curve.closed && m > 2 ? curve.points[0].point : -1);
      if (b >= 0 && a != b && a < n && b < n) {
        next[a].push_back(b);
        next[b].push_back(a);
      }
    }
  }
  using Entry = std::pair<float, int>;
  std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
  for (const int s : selected) {
    queue.push({0.0f, s});
  }
  while (!queue.empty()) {
    const auto [d, i] = queue.top();
    queue.pop();
    if (d > dist[i]) {
      continue;
    }
    for (const int j : next[i]) {
      const float through = d + distance(positions[i], positions[j]);
      if (through < dist[j]) {
        dist[j] = through;
        queue.push({through, j});
      }
    }
  }
  return dist;
}

float proportional_weight(ProportionalFalloff falloff, float distance, float size, int point)
{
  if (distance <= 0.0f) {
    return 1.0f;
  }
  if (!(distance < size)) {
    return 0.0f;
  }
  /* As Blender's: 1 at the selection, 0 at the edge of the size. */
  const float d = 1.0f - distance / size;
  switch (falloff) {
    case ProportionalFalloff::Smooth:
      return 3.0f * d * d - 2.0f * d * d * d;
    case ProportionalFalloff::Sphere:
      return std::sqrt(std::max(0.0f, 2.0f * d - d * d));
    case ProportionalFalloff::Root:
      return std::sqrt(d);
    case ProportionalFalloff::InverseSquare:
      return d * (2.0f - d);
    case ProportionalFalloff::Sharp:
      return d * d;
    case ProportionalFalloff::Linear:
      return d;
    case ProportionalFalloff::Constant:
      return 1.0f;
    case ProportionalFalloff::Random: {
      /* The same amount for a point every time (a hash of its index). */
      uint32_t x = uint32_t(point) * 2654435761u + 0x9e3779b9u;
      x ^= x >> 16;
      x *= 0x7feb352du;
      x ^= x >> 15;
      x *= 0x846ca68bu;
      x ^= x >> 16;
      return float(x >> 8) / float(1u << 24) * d;
    }
  }
  return d;
}

}  // namespace oanim

namespace oanim {

SymmetrizeResult drawing_symmetrize(VectorDrawing &drawing, bool right_to_left, float threshold, bool setup_frame,
                                    const std::map<uint64_t, uint64_t> *weight_flip)
{
  SymmetrizeResult result;
  const auto place = [&](int i) { return setup_frame ? drawing.points[i].pos : point_setup_pos(drawing.points[i]); };
  const float thr = std::max(threshold, 0.0f);
  std::vector<int> right, left, centre;
  for (int i = 0; i < int(drawing.points.size()); i++) {
    if (!drawing.points[i].selected) {
      continue;
    }
    const float x = place(i).x;
    (x > thr ? right : x < -thr ? left : centre).push_back(i);
  }
  result.right = int(right.size());
  result.left = int(left.size());
  result.centre = int(centre.size());
  /* Pairs: the nearest reflection first, each point used once. */
  struct Candidate {
    float d;
    int r, l;
  };
  std::vector<Candidate> candidates;
  for (const int r : right) {
    for (const int l : left) {
      candidates.push_back({distance(reflect2(place(r), true, false), place(l)), r, l});
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) { return a.d < b.d; });
  std::set<int> used;
  std::vector<std::pair<int, int>> pairs;
  for (const Candidate &c : candidates) {
    if (!used.count(c.r) && !used.count(c.l)) {
      used.insert(c.r);
      used.insert(c.l);
      pairs.push_back({c.r, c.l});
    }
  }
  if (right.size() != left.size() || (pairs.empty() && centre.empty())) {
    for (const int i : right) {
      if (!used.count(i)) {
        result.unmatched.push_back(i);
      }
    }
    for (const int i : left) {
      if (!used.count(i)) {
        result.unmatched.push_back(i);
      }
    }
    return result;
  }

  /* Every handle involved is stored, so it can be copied and set. */
  std::set<int> involved(centre.begin(), centre.end());
  for (const auto &[r, l] : pairs) {
    involved.insert(r);
    involved.insert(l);
  }
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      if (involved.count(curve.points[i].point)) {
        own_handles(drawing, curve, i);
      }
    }
  }
  const auto neighbour = [&](const Curve &curve, int i, int dir) {
    const int n = int(curve.points.size());
    const int k = i + dir;
    if (!curve.closed && (k < 0 || k >= n)) {
      return -1;
    }
    return curve.points[(k % n + n) % n].point;
  };
  const auto find = [&](int point, Curve **r_curve) -> int {
    for (Curve &curve : drawing.curves) {
      for (int i = 0; i < int(curve.points.size()); i++) {
        if (curve.points[i].point == point) {
          *r_curve = &curve;
          return i;
        }
      }
    }
    return -1;
  };

  for (const auto &[r, l] : pairs) {
    const int src = right_to_left ? r : l, dst = right_to_left ? l : r;
    /* The handles first, while the neighbours are where they were: the copy runs the same way
     * along its curve when the source's previous point reflects nearer its previous one. */
    Curve *src_curve = nullptr;
    const int si = find(src, &src_curve);
    if (si >= 0) {
      const CurvePoint from = src_curve->points[si];
      const int sp = neighbour(*src_curve, si, -1), sn = neighbour(*src_curve, si, 1);
      for (Curve &curve : drawing.curves) {
        for (int j = 0; j < int(curve.points.size()); j++) {
          if (curve.points[j].point != dst) {
            continue;
          }
          const int dp = neighbour(curve, j, -1), dn = neighbour(curve, j, 1);
          const auto gap = [&](int a, int b) {
            return a < 0 || b < 0 ? std::numeric_limits<float>::max() : distance(reflect2(place(a), true, false), place(b));
          };
          const int s = sp >= 0 ? sp : sn;
          const bool swap = s >= 0 && (s == sp ? gap(sp, dn) < gap(sp, dp) : gap(sn, dp) < gap(sn, dn));
          CurvePoint &to = curve.points[j];
          to.handle_in = reflect2(swap ? from.handle_out : from.handle_in, true, false);
          to.handle_out = reflect2(swap ? from.handle_in : from.handle_out, true, false);
          to.weight_in = swap ? from.weight_out : from.weight_in;
          to.weight_out = swap ? from.weight_in : from.weight_out;
          to.offset_in = -(swap ? from.offset_out : from.offset_in);
          to.offset_out = -(swap ? from.offset_in : from.offset_out);
          to.curvature = from.curvature;
          to.aligned = from.aligned;
          to.handles_set = true;
        }
      }
    }
    MeshPoint &to = drawing.points[dst];
    const MeshPoint &from = drawing.points[src];
    if (setup_frame) {
      to.pos = reflect2(from.pos, true, false);
    }
    else {
      to.pos = point_setup_pos(to) + reflect2(from.pos - point_setup_pos(from), true, false);
    }
    to.radius = from.radius;
    to.corner = from.corner;
    if (weight_flip) {
      to.weights = from.weights;
      for (BoneWeight &w : to.weights) {
        if (const auto it = weight_flip->find(w.bone); it != weight_flip->end()) {
          w.bone = it->second;
        }
      }
    }
  }

  /* The axis's own points: on it, their handles mirroring each other (flat when within 10 degrees
   * of flat). */
  const float nearly_flat = std::tan(10.0f * PI / 180.0f);
  for (const int c : centre) {
    MeshPoint &p = drawing.points[c];
    p.pos.x = setup_frame ? 0.0f : point_setup_pos(p).x;
    for (Curve &curve : drawing.curves) {
      for (CurvePoint &cp : curve.points) {
        if (cp.point != c || !cp.handles_set) {
          continue;
        }
        const auto flatten = [&](float2 &h) {
          if (std::fabs(h.y) <= nearly_flat * std::fabs(h.x)) {
            h.y = 0.0f;
          }
        };
        const bool in_right = cp.handle_in.x > 0.0f, out_right = cp.handle_out.x > 0.0f;
        if (in_right != out_right && std::fabs(cp.handle_in.x) > 1e-9f && std::fabs(cp.handle_out.x) > 1e-9f) {
          /* One handle each way: the source side's sets both. */
          float2 &source = (in_right == right_to_left) ? cp.handle_in : cp.handle_out;
          float2 &image = (in_right == right_to_left) ? cp.handle_out : cp.handle_in;
          flatten(source);
          image = reflect2(source, true, false);
        }
        else {
          flatten(cp.handle_in);
          flatten(cp.handle_out);
        }
      }
    }
  }
  result.done = true;
  return result;
}

}  // namespace oanim

namespace oanim {

void drawing_mirror_setup_x(VectorDrawing &drawing)
{
  const auto flip = [](float2 v) { return float2(-v.x, v.y); };
  const auto across = [](float2 v) { return float2(v.x, -v.y); };
  for (MeshPoint &p : drawing.points) {
    /* From what shows on the setup frame. */
    p.pos = flip(p.pos);
    p.setup_pos = p.pos;
    p.transform = {};
    p.pos_keys = {};
    p.radius_keys = {};
    p.selected = false;
    /* Along its segment stays along it; across it changes side, the segment's turn reversed. */
    PointDetail &d = p.detail;
    d.pos = across(d.pos);
    d.in = across(d.in);
    d.out = across(d.out);
    d.setup_pos = d.pos;
    d.setup_in = d.in;
    d.setup_out = d.out;
    d.built = false;
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      /* Handles not stored yet follow the (flipped) neighbours by themselves. */
      if (cp.handles_set) {
        cp.handle_in = flip(cp.handle_in);
        cp.handle_out = flip(cp.handle_out);
        cp.setup_in = cp.handle_in;
        cp.setup_out = cp.handle_out;
      }
      cp.offset_in = -cp.offset_in;
      cp.offset_out = -cp.offset_out;
      cp.change_in = {};
      cp.change_out = {};
      cp.curvature_keys = {};
      cp.handle_in_keys = {};
      cp.handle_out_keys = {};
      cp.select_in = cp.select_out = false;
    }
  }
}

}  // namespace oanim
