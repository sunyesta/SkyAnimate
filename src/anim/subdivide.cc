#include "anim/subdivide.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "data/detail.hh"
#include "data/vector.hh"

namespace oanim {

namespace {

/* The part of cubic `c` from t0 to t1, as a cubic of its own (de Casteljau twice). */
void bezier_part(const float2 c[4], float t0, float t1, float2 r[4])
{
  const auto split = [](const float2 in[4], float t, float2 left[4], float2 right[4]) {
    const float2 a = in[0] + (in[1] - in[0]) * t, b = in[1] + (in[2] - in[1]) * t, d = in[2] + (in[3] - in[2]) * t;
    const float2 ab = a + (b - a) * t, bd = b + (d - b) * t;
    const float2 m = ab + (bd - ab) * t;
    left[0] = in[0], left[1] = a, left[2] = ab, left[3] = m;
    right[0] = m, right[1] = bd, right[2] = d, right[3] = in[3];
  };
  float2 left[4], right[4], tail[4];
  split(c, t0, left, tail);
  if (t0 >= 1.0f) {
    std::copy(tail, tail + 4, r);
    return;
  }
  split(tail, (t1 - t0) / (1.0f - t0), r, right);
}

float bezier_length(const float2 c[4])
{
  float total = 0.0f;
  float2 prev = c[0];
  for (int k = 1; k <= 16; k++) {
    const float2 p = bezier_eval(c, float(k) / 16.0f);
    total += distance(prev, p);
    prev = p;
  }
  return total;
}

/* Where to cut cubic `c` into `k` pieces of equal length along it: k + 1 parameters, 0 to 1. */
std::vector<float> even_cuts(const float2 c[4], int k)
{
  constexpr int SAMPLES = 64;
  std::array<float, SAMPLES + 1> along{};
  float2 prev = c[0];
  for (int i = 1; i <= SAMPLES; i++) {
    const float2 p = bezier_eval(c, float(i) / float(SAMPLES));
    along[size_t(i)] = along[size_t(i - 1)] + distance(prev, p);
    prev = p;
  }
  const float total = along[SAMPLES];
  std::vector<float> cuts(size_t(k) + 1, 0.0f);
  cuts.back() = 1.0f;
  int i = 0;
  for (int j = 1; j < k; j++) {
    const float want = total * float(j) / float(k);
    while (i < SAMPLES - 1 && along[size_t(i + 1)] < want) {
      i++;
    }
    const float span = along[size_t(i + 1)] - along[size_t(i)];
    const float f = span > 1e-12f ? (want - along[size_t(i)]) / span : 0.0f;
    cuts[size_t(j)] = (float(i) + f) / float(SAMPLES);
  }
  return cuts;
}

/* The drawing as it is in the setup pose (positions and stored handles), to measure segments by. */
VectorDrawing setup_shape(const VectorDrawing &drawing)
{
  VectorDrawing setup = drawing;
  for (MeshPoint &p : setup.points) {
    p.pos = point_setup_pos(p);
  }
  for (Curve &curve : setup.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.handle_in = cp.setup_in;
      cp.handle_out = cp.setup_out;
    }
  }
  return setup;
}

bool in_group(const VectorDrawing &drawing, const PointGroup *group, int point)
{
  if (!group) {
    return true;
  }
  const auto it = group->weights.find(drawing.points[size_t(point)].id);
  return it != group->weights.end() && it->second > 0.0f;
}

const PointGroup *group_of(const VectorDrawing &drawing, const Modifier &m)
{
  if (m.subdiv_group.empty()) {
    return nullptr;
  }
  for (const PointGroup &g : drawing.groups) {
    if (g.name == m.subdiv_group) {
      return &g;
    }
  }
  return nullptr;
}

int pieces_for(const VectorDrawing &drawing, const VectorDrawing *setup, const Curve &curve, int c, int segment,
               const Modifier &m, bool render, const PointGroup *group)
{
  const int n = int(curve.points.size());
  const int a = segment, b = (segment + 1) % n;
  if (!m.subdiv_group.empty() &&
      (!group || (!in_group(drawing, group, curve.points[size_t(a)].point) &&
                  !in_group(drawing, group, curve.points[size_t(b)].point))))
  {
    return 1;
  }
  int pieces = m.subdiv_pieces;
  if (m.subdiv_by_length) {
    const float spacing = std::max(render && m.subdiv_render_spacing > 0.0f ? m.subdiv_render_spacing : m.subdiv_spacing,
                                   1e-5f);
    float2 ctrl[4];
    curve_segment_bezier(*setup, setup->curves[size_t(c)], segment, ctrl);
    pieces = int(std::ceil(bezier_length(ctrl) / spacing - 1e-4f));
  }
  return std::clamp(pieces, 1, std::max(1, m.subdiv_max_pieces));
}

std::vector<BoneWeight> blend(const std::vector<BoneWeight> &a, const std::vector<BoneWeight> &b, float t)
{
  std::map<uint64_t, float> sum;
  for (const BoneWeight &w : a) {
    sum[w.bone] += w.weight * (1.0f - t);
  }
  for (const BoneWeight &w : b) {
    sum[w.bone] += w.weight * t;
  }
  std::vector<BoneWeight> out;
  for (const auto &[bone, weight] : sum) {
    if (weight > 0.0f) {
      out.push_back({bone, weight});
    }
  }
  return out;
}

}  // namespace

int subdivide_pieces(const VectorDrawing &drawing, const Curve &curve, int segment, const Modifier &m, bool render)
{
  const VectorDrawing setup = setup_shape(drawing);
  int c = 0;
  for (; c < int(drawing.curves.size()); c++) {
    if (&drawing.curves[size_t(c)] == &curve) {
      break;
    }
  }
  if (c >= int(drawing.curves.size())) {
    return 1;
  }
  return pieces_for(drawing, &setup, curve, c, segment, m, render, group_of(drawing, m));
}

VectorDrawing drawing_subdivide(const VectorDrawing &source, const Modifier &m, bool render,
                                std::vector<int> *r_new_points)
{
  VectorDrawing drawing = source;
  /* Detail final, then none: the split curve is the whole curve. */
  if (drawing_has_detail(drawing)) {
    drawing_detail_final_handles(drawing);
    for (MeshPoint &p : drawing.points) {
      p.detail = PointDetail();
    }
  }
  const VectorDrawing setup = setup_shape(drawing);
  const PointGroup *group = group_of(drawing, m);
  for (int c = 0; c < int(drawing.curves.size()); c++) {
    const Curve &curve = source.curves[size_t(c)];
    const int n = int(curve.points.size());
    if (n < 2) {
      continue;
    }
    const int segments = curve.segment_count();
    std::vector<int> pieces(static_cast<size_t>(segments), 1);
    bool any = false;
    for (int s = 0; s < segments; s++) {
      pieces[size_t(s)] = pieces_for(drawing, &setup, drawing.curves[size_t(c)], c, s, m, render, group);
      any = any || pieces[size_t(s)] > 1;
    }
    if (!any) {
      continue;
    }
    /* Every segment's control points as they are now (handles worked out where they aren't stored). */
    std::vector<std::array<float2, 4>> ctrl(static_cast<size_t>(segments));
    /* Where each segment is cut: evenly along it as it is in the setup pose, so the cuts stay put on
     * the line from frame to frame. */
    std::vector<std::vector<float>> cuts(static_cast<size_t>(segments));
    for (int s = 0; s < segments; s++) {
      curve_segment_bezier(drawing, drawing.curves[size_t(c)], s, ctrl[size_t(s)].data());
      float2 rest[4];
      curve_segment_bezier(setup, setup.curves[size_t(c)], s, rest);
      cuts[size_t(s)] = even_cuts(rest, pieces[size_t(s)]);
    }
    Curve out = curve;
    out.points.clear();
    std::vector<int> first_of(static_cast<size_t>(segments), 0);
    const auto own = [&](int i) { return drawing.points[size_t(curve.points[size_t(i)].point)]; };
    for (int s = 0; s < segments; s++) {
      const int a = s, b = (s + 1) % n;
      const CurvePoint &ca = curve.points[size_t(a)];
      first_of[size_t(s)] = int(out.points.size());
      const int k = pieces[size_t(s)];
      for (int j = 0; j < k; j++) {
        float2 part[4];
        bezier_part(ctrl[size_t(s)].data(), cuts[size_t(s)][size_t(j)], cuts[size_t(s)][size_t(j + 1)], part);
        if (j == 0) {
          /* Its own point: its handles stored, the out one shortened to the first piece. */
          CurvePoint cp = ca;
          cp.handles_follow_curve = !ca.handles_set;
          cp.handles_set = true;
          const float2 here = drawing.points[size_t(ca.point)].pos;
          cp.handle_in = curve_handle(drawing, drawing.curves[size_t(c)], a, true) - here;
          cp.handle_out = part[1] - part[0];
          out.points.push_back(cp);
        }
        else {
          const float t = float(j) / float(k);
          MeshPoint p = own(a);
          const MeshPoint &pb = own(b);
          p.pos = part[0];
          p.radius = own(a).radius + (pb.radius - own(a).radius) * t;
          p.transform = own(a).transform + (pb.transform - own(a).transform) * t;
          p.setup_pos = p.pos - p.transform;
          p.weights = blend(own(a).weights, pb.weights, t);
          p.selected = false;
          p.detail = PointDetail();
          p.pos_keys = {};
          p.radius_keys = {};
          p.preserve_volume = m.subdiv_preserve_volume;
          p.id = drawing.next_id++;
          drawing.points.push_back(p);
          if (r_new_points) {
            r_new_points->push_back(int(drawing.points.size()) - 1);
          }
          CurvePoint cp = ca;
          cp.point = int(drawing.points.size()) - 1;
          cp.handles_follow_curve = true;
          cp.handles_set = true;
          cp.aligned = true;
          cp.select_in = cp.select_out = false;
          cp.handle_out = part[1] - part[0];
          cp.curvature_keys = {};
          cp.handle_in_keys = {};
          cp.handle_out_keys = {};
          out.points.push_back(cp);
        }
      }
    }
    if (!curve.closed) {
      CurvePoint cp = curve.points[size_t(n - 1)];
      cp.handles_follow_curve = !cp.handles_set;
      cp.handles_set = true;
      const float2 here = drawing.points[size_t(cp.point)].pos;
      cp.handle_in = curve_handle(drawing, drawing.curves[size_t(c)], n - 1, true) - here;
      cp.handle_out = curve_handle(drawing, drawing.curves[size_t(c)], n - 1, false) - here;
      out.points.push_back(cp);
    }
    /* Every in handle from the piece ending there. */
    for (int s = 0; s < segments; s++) {
      const int k = pieces[size_t(s)];
      for (int j = 0; j < k; j++) {
        float2 part[4];
        bezier_part(ctrl[size_t(s)].data(), cuts[size_t(s)][size_t(j)], cuts[size_t(s)][size_t(j + 1)], part);
        const int end = first_of[size_t(s)] + j + 1;
        const int at = end < int(out.points.size()) ? end : 0;
        out.points[size_t(at)].handle_in = part[2] - part[3];
      }
    }
    /* The pieces of each segment take its place in the shapes, in the way the shape runs. */
    for (Shape &shape : drawing.shapes) {
      std::vector<ShapeEdge> edges;
      for (size_t e = 0; e < shape.edges.size(); e++) {
        const ShapeEdge &edge = shape.edges[e];
        if (edge.curve != c || edge.segment < 0 || edge.segment >= segments) {
          edges.push_back(edge);
          continue;
        }
        const int k = pieces[size_t(edge.segment)];
        const ShapeEdge &prev = shape.edges[(e + shape.edges.size() - 1) % shape.edges.size()];
        const bool backward = prev.curve == c && prev.segment == (edge.segment + 1) % segments && segments > 2;
        for (int j = 0; j < k; j++) {
          edges.push_back({c, first_of[size_t(edge.segment)] + (backward ? k - 1 - j : j)});
        }
      }
      shape.edges = std::move(edges);
    }
    drawing.curves[size_t(c)] = std::move(out);
  }
  return drawing;
}

}  // namespace oanim
