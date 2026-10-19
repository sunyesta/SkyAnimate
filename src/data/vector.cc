#include "data/vector.hh"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <set>

namespace sky {

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

float2 curve_handle(const VectorDrawing &drawing, const Curve &curve, int i, bool pre)
{
  const int n = int(curve.points.size());
  const CurvePoint &point = curve.points[i];
  const float2 here = drawing.points[point.point].pos;
  switch (point.handle_type) {
    case HandleType::Aligned:
    case HandleType::Free:
      return here + (pre ? point.handle_in : point.handle_out);
    case HandleType::Vector: {
      /* A third of the way to the neighbour on that side; none past an open curve's end. */
      const int other = pre ? i - 1 : i + 1;
      if (n < 2 || (!curve.closed && (other < 0 || other >= n))) {
        return here;
      }
      const float2 there = drawing.points[curve.points[(other + n) % n].point].pos;
      return here + (there - here) / 3.0f;
    }
    case HandleType::Auto:
      break;
  }
  /* Moho's GetBezierHandle(): the handle points along the line from the previous to the next
   * point, its length a share of the distance to the neighbour on that side. */
  const auto pos = [&](int index) { return drawing.points[curve.points[index].point].pos; };
  if (n < 2) {
    return pos(i);
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
    return pos(i);
  }
  float2 dir = normalize(pos(post_id) - pos(pre_id));
  if (pre) {
    dir = -dir;
  }
  const CurvePoint &cp = curve.points[i];
  const float len = distance(pos(second_id), pos(i)) * cp.curvature * (pre ? cp.weight_in : cp.weight_out);
  return pos(i) + rotate(dir * len, pre ? cp.offset_in : cp.offset_out);
}

void curve_point_set_handle_type(VectorDrawing &drawing, Curve &curve, int i, HandleType type)
{
  CurvePoint &cp = curve.points[i];
  if (cp.handle_type == type) {
    return;
  }
  const float2 here = drawing.points[cp.point].pos;
  const float2 in = curve_handle(drawing, curve, i, true) - here;
  const float2 out = curve_handle(drawing, curve, i, false) - here;
  if (type == HandleType::Aligned || type == HandleType::Free) {
    cp.handle_in = in;
    cp.handle_out = out;
    if (type == HandleType::Aligned && length(out) > 1e-9f) {
      cp.handle_in = -normalize(out) * length(in);
    }
    else if (type == HandleType::Aligned && length(in) > 1e-9f) {
      cp.handle_out = -normalize(in) * length(out);
    }
  }
  if (type == HandleType::Auto && cp.curvature <= 0.0f) {
    cp.curvature = CURVATURE_SMOOTH;
  }
  cp.handle_type = type;
}

void curve_point_move_handle(VectorDrawing &drawing, Curve &curve, int i, bool pre, float2 pos, bool keep_other)
{
  CurvePoint &cp = curve.points[i];
  if (cp.handle_type == HandleType::Auto) {
    curve_point_set_handle_type(drawing, curve, i, HandleType::Aligned);
  }
  else if (cp.handle_type == HandleType::Vector) {
    curve_point_set_handle_type(drawing, curve, i, HandleType::Free);
  }
  const float2 offset = pos - drawing.points[cp.point].pos;
  (pre ? cp.handle_in : cp.handle_out) = offset;
  if (cp.handle_type == HandleType::Aligned && !keep_other && length(offset) > 1e-9f) {
    float2 &other = pre ? cp.handle_out : cp.handle_in;
    other = -normalize(offset) * length(other);
  }
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
  if (shape.edges.empty()) {
    return false;
  }

  struct Oriented {
    ShapeEdge edge;
    bool reversed;
  };
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

  for (const Oriented &o : chain) {
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
  *r_closed = (first_from == last_to) && chain.size() > 1;
  if (!*r_closed) {
    /* Open runs end on their last point. */
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

/* ---------------------------------------------------------------------------------
 * Editing helpers
 */

int drawing_add_point(VectorDrawing &drawing, float2 pos)
{
  drawing.points.push_back({pos});
  return int(drawing.points.size()) - 1;
}

/* Removes shapes that use any of the marked curves, then renumbers curve indices. */
static void shapes_remap_curves(VectorDrawing &drawing, const std::vector<int> &curve_remap,
                                const std::vector<bool> &curve_changed)
{
  std::erase_if(drawing.shapes, [&](const Shape &shape) {
    return std::any_of(shape.edges.begin(), shape.edges.end(), [&](const ShapeEdge &e) {
      return curve_changed[e.curve] || curve_remap[e.curve] < 0;
    });
  });
  for (Shape &shape : drawing.shapes) {
    for (ShapeEdge &e : shape.edges) {
      e.curve = curve_remap[e.curve];
    }
  }
}

void drawing_remove_points(VectorDrawing &drawing, const std::vector<bool> &remove)
{
  std::vector<int> point_remap(drawing.points.size(), -1);
  std::vector<MeshPoint> points;
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (!remove[i]) {
      point_remap[i] = int(points.size());
      points.push_back(drawing.points[i]);
    }
  }

  std::vector<int> curve_remap(drawing.curves.size(), -1);
  std::vector<bool> curve_changed(drawing.curves.size(), false);
  std::vector<Curve> curves;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve curve = drawing.curves[c];
    const size_t before = curve.points.size();
    std::erase_if(curve.points, [&](const CurvePoint &cp) { return remove[cp.point]; });
    curve_changed[c] = curve.points.size() != before;
    for (CurvePoint &cp : curve.points) {
      cp.point = point_remap[cp.point];
    }
    if (curve.closed && curve.points.size() < 3) {
      curve.closed = false;
    }
    if (curve.points.size() >= 2) {
      curve_remap[c] = int(curves.size());
      curves.push_back(std::move(curve));
    }
  }
  shapes_remap_curves(drawing, curve_remap, curve_changed);
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
  shapes_remap_curves(drawing, curve_remap, std::vector<bool>(drawing.curves.size(), false));
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
    if (d <= best_dist) {
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

  /* A new point on a smooth curve keeps it closest to its old path when it is smooth too. */
  CurvePoint cp;
  cp.point = point;
  const int n = int(curve.points.size());
  CurvePoint &from = curve.points[edge.segment];
  CurvePoint &to = curve.points[(edge.segment + 1) % n];
  const bool stored = from.handle_type == HandleType::Aligned || from.handle_type == HandleType::Free ||
                      to.handle_type == HandleType::Aligned || to.handle_type == HandleType::Free;
  if (stored) {
    /* Hand-set handles: split the Bezier exactly (de Casteljau), so the line doesn't move. The
     * new point is Aligned; the ends' facing handles shorten to match. */
    const auto lerp2 = [](float2 a, float2 b, float u) { return a + (b - a) * u; };
    const float2 p01 = lerp2(ctrl[0], ctrl[1], t), p12 = lerp2(ctrl[1], ctrl[2], t), p23 = lerp2(ctrl[2], ctrl[3], t);
    const float2 p012 = lerp2(p01, p12, t), p123 = lerp2(p12, p23, t);
    const float2 mid = drawing.points[point].pos;
    for (CurvePoint *end : {&from, &to}) {
      if (end->handle_type == HandleType::Auto || end->handle_type == HandleType::Vector) {
        curve_point_set_handle_type(drawing, curve, end == &from ? edge.segment : (edge.segment + 1) % n,
                                    HandleType::Free);
      }
    }
    from.handle_out = p01 - ctrl[0];
    to.handle_in = p23 - ctrl[3];
    cp.handle_type = HandleType::Aligned;
    cp.handle_in = p012 - mid;
    cp.handle_out = p123 - mid;
  }
  else if (from.handle_type == HandleType::Vector && to.handle_type == HandleType::Vector) {
    /* Splitting a straight side keeps it straight. */
    cp.handle_type = HandleType::Vector;
  }
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

void drawing_reverse_curve(VectorDrawing &drawing, int curve_index)
{
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

int drawing_shape_from_selection(VectorDrawing &drawing, bool fill, const Style &style)
{
  /* Segments with both ends selected form a graph over the points. */
  std::vector<ShapeEdge> segments;
  std::vector<std::pair<int, int>> ends;
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    const Curve &curve = drawing.curves[c];
    for (int s = 0; s < curve.segment_count(); s++) {
      int a, b;
      curve_segment_points(curve, s, &a, &b);
      if (drawing.points[a].selected && drawing.points[b].selected) {
        segments.push_back({int(c), s});
        ends.emplace_back(a, b);
      }
    }
  }
  if (segments.empty()) {
    return -1;
  }

  /* Shortest cycle: for each segment, the shortest path between its ends without it. */
  std::vector<int> best_cycle;
  for (size_t skip = 0; skip < segments.size(); skip++) {
    const auto [start, goal] = ends[skip];
    std::map<int, std::pair<int, int>> came_from; /* point -> (previous point, segment index) */
    std::deque<int> queue = {start};
    came_from[start] = {-1, -1};
    while (!queue.empty() && !came_from.contains(goal)) {
      const int p = queue.front();
      queue.pop_front();
      for (size_t e = 0; e < segments.size(); e++) {
        if (e == skip) {
          continue;
        }
        int other = -1;
        if (ends[e].first == p) {
          other = ends[e].second;
        }
        else if (ends[e].second == p) {
          other = ends[e].first;
        }
        if (other >= 0 && !came_from.contains(other)) {
          came_from[other] = {p, int(e)};
          queue.push_back(other);
        }
      }
    }
    if (!came_from.contains(goal)) {
      continue;
    }
    std::vector<int> cycle = {int(skip)};
    for (int p = goal; came_from[p].second >= 0; p = came_from[p].first) {
      cycle.push_back(came_from[p].second);
    }
    if (best_cycle.empty() || cycle.size() < best_cycle.size()) {
      best_cycle = std::move(cycle);
    }
  }

  Shape shape;
  shape.style = style;
  if (!best_cycle.empty()) {
    for (int e : best_cycle) {
      shape.edges.push_back(segments[e]);
    }
    shape.has_fill = fill;
  }
  else {
    /* No closed loop: an outline along the selected segments, as Moho allows for open lines. */
    shape.edges = segments;
    shape.has_fill = false;
    shape.has_outline = true;
    std::vector<float2> outline;
    bool closed;
    if (!shape_outline(drawing, shape, 2, outline, nullptr, &closed)) {
      return -1;
    }
  }
  shape.name = "Shape " + std::to_string(drawing.shapes.size() + 1);
  drawing.shapes.push_back(std::move(shape));
  return int(drawing.shapes.size()) - 1;
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
  const float delta = std::clamp(mean, 0.0f, 1.0f) - current;
  for (MeshPoint &p : drawing.points) {
    if (p.selected) {
      p.radius = std::clamp(p.radius + delta, 0.0f, 1.0f);
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
    cp.curvature = curvature[k];
    /* A box's corners are Vector handles: straight sides, handles still there to pull. */
    cp.handle_type = kind == PrimitiveKind::Box ? HandleType::Vector : HandleType::Auto;
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

int drawing_set_selected_handle_type(VectorDrawing &drawing, HandleType type)
{
  int changed = 0;
  for (Curve &curve : drawing.curves) {
    for (int i = 0; i < int(curve.points.size()); i++) {
      CurvePoint &cp = curve.points[i];
      if (curve_point_selected(drawing, cp) && cp.handle_type != type) {
        curve_point_set_handle_type(drawing, curve, i, type);
        changed++;
      }
    }
  }
  return changed;
}

int drawing_selected_handle_type(const VectorDrawing &drawing)
{
  int found = -1;
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      if (!curve_point_selected(drawing, cp)) {
        continue;
      }
      const int type = int(cp.handle_type);
      if (found == -1) {
        found = type;
      }
      else if (found != type) {
        return -2;
      }
    }
  }
  return found;
}

const char *handle_type_identifier(HandleType type)
{
  switch (type) {
    case HandleType::Vector: return "VECTOR";
    case HandleType::Aligned: return "ALIGNED";
    case HandleType::Free: return "FREE";
    case HandleType::Auto: break;
  }
  return "AUTO";
}

const char *handle_type_name(HandleType type)
{
  switch (type) {
    case HandleType::Vector: return "Vector";
    case HandleType::Aligned: return "Aligned";
    case HandleType::Free: return "Free";
    case HandleType::Auto: break;
  }
  return "Automatic";
}

bool handle_type_from_identifier(const std::string &identifier, HandleType *r_type)
{
  for (const HandleType type : {HandleType::Auto, HandleType::Vector, HandleType::Aligned, HandleType::Free}) {
    if (identifier == handle_type_identifier(type)) {
      *r_type = type;
      return true;
    }
  }
  return false;
}

}  // namespace sky
