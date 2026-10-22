#include "anim/bevel.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace oanim {

namespace {

float2 lerp2(float2 a, float2 b, float t)
{
  return a + (b - a) * t;
}

/* The part of a Bezier between t0 and t1, exactly (de Casteljau). */
void bezier_part(const float2 c[4], float t0, float t1, float2 r[4])
{
  /* From t0 on... */
  float2 a[4] = {c[0], c[1], c[2], c[3]};
  if (t0 > 0.0f) {
    const float2 p01 = lerp2(c[0], c[1], t0), p12 = lerp2(c[1], c[2], t0), p23 = lerp2(c[2], c[3], t0);
    const float2 p012 = lerp2(p01, p12, t0), p123 = lerp2(p12, p23, t0);
    a[0] = lerp2(p012, p123, t0);
    a[1] = p123;
    a[2] = p23;
    a[3] = c[3];
  }
  /* ...up to t1, measured again along what's left. */
  const float u = t0 < 1.0f ? std::clamp((t1 - t0) / (1.0f - t0), 0.0f, 1.0f) : 1.0f;
  if (u < 1.0f) {
    const float2 p01 = lerp2(a[0], a[1], u), p12 = lerp2(a[1], a[2], u), p23 = lerp2(a[2], a[3], u);
    const float2 p012 = lerp2(p01, p12, u), p123 = lerp2(p12, p23, u);
    r[0] = a[0];
    r[1] = p01;
    r[2] = p012;
    r[3] = lerp2(p012, p123, u);
    return;
  }
  for (int k = 0; k < 4; k++) {
    r[k] = a[k];
  }
}

/* Where along a segment (t) the curve is `distance` from one of its ends (`from_end`: from its last
 * point), found by halving: the point a bevel cuts it at. */
float t_at_distance(const float2 c[4], float distance, bool from_end)
{
  const float2 end = from_end ? c[3] : c[0];
  float lo = 0.0f, hi = 1.0f;
  for (int k = 0; k < 30; k++) {
    const float mid = 0.5f * (lo + hi);
    const float t = from_end ? 1.0f - mid : mid;
    if (length(bezier_eval(c, t) - end) < distance) {
      lo = mid;
    }
    else {
      hi = mid;
    }
  }
  const float mid = 0.5f * (lo + hi);
  return from_end ? 1.0f - mid : mid;
}

/* Which way a segment leaves its first point, and arrives at its last: along the handle, or toward
 * the far end when the handle has no length. */
float2 leaving(const float2 c[4])
{
  const float2 d = length(c[1] - c[0]) > 1e-9f ? c[1] - c[0] : length(c[2] - c[0]) > 1e-9f ? c[2] - c[0] : c[3] - c[0];
  return normalize(d);
}

float2 arriving(const float2 c[4])
{
  const float2 d = length(c[3] - c[2]) > 1e-9f ? c[3] - c[2] : length(c[3] - c[1]) > 1e-9f ? c[3] - c[1] : c[3] - c[0];
  return normalize(d);
}

}  // namespace

VectorDrawing drawing_bevel(const VectorDrawing &source, const Modifier &m)
{
  if (m.bevel_amount <= 0.0f) {
    return source;
  }
  VectorDrawing drawing = source;
  const PointGroup *group = nullptr;
  if (m.bevel_limit == BevelLimit::Group) {
    for (const PointGroup &g : drawing.groups) {
      if (g.name == m.bevel_group) {
        group = &g;
      }
    }
    if (!group) {
      return drawing;
    }
  }
  /* A point on several curves (welded) stays: a bevel there would pull them apart. */
  std::vector<int> curves_through(drawing.points.size(), 0);
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      curves_through[size_t(cp.point)]++;
    }
  }
  /* What Profile makes of the handles, as a share of the way to the corner: 0.25 straight, 0.5 round
   * (a circle's arc), 1 square; below 0.25 the round arc turned inward. */
  const float profile = std::clamp(m.bevel_profile, 0.0f, 1.0f);

  for (int c = 0; c < int(drawing.curves.size()); c++) {
    const Curve original = drawing.curves[size_t(c)];
    const int n = int(original.points.size());
    const int segments = original.segment_count();
    if (n < 2 || segments < 2) {
      continue;
    }
    std::vector<std::array<float2, 4>> ctrl(static_cast<size_t>(segments));
    for (int s = 0; s < segments; s++) {
      curve_segment_bezier(drawing, original, s, ctrl[size_t(s)].data());
    }
    /* The corners it cuts, and how far along each side (straight distance from the corner). */
    std::vector<float> amount(size_t(n), 0.0f), off_in(size_t(n), 0.0f), off_out(size_t(n), 0.0f);
    bool any = false;
    for (int k = 0; k < n; k++) {
      if (!original.closed && (k == 0 || k == n - 1)) {
        continue;
      }
      const int p = original.points[size_t(k)].point;
      if (curves_through[size_t(p)] != 1) {
        continue;
      }
      const auto &in = ctrl[size_t((k - 1 + segments) % segments)];
      const auto &out = ctrl[size_t(k % segments)];
      float weight = 1.0f;
      if (group) {
        const auto it = group->weights.find(drawing.points[size_t(p)].id);
        if (it == group->weights.end() || it->second <= 0.0f) {
          continue;
        }
        weight = it->second;
      }
      const float turn = std::acos(std::clamp(dot(arriving(in.data()), leaving(out.data())), -1.0f, 1.0f));
      if (m.bevel_limit == BevelLimit::Angle && turn <= m.bevel_angle) {
        continue;
      }
      /* The Amount as a distance along each side: the corner's inner angle turns a Width (the cut's
       * length) or a Depth (the cut's distance from the corner) into one. */
      const float half = std::max((PI - turn) * 0.5f, 1e-3f);
      const float a = m.bevel_amount * weight;
      switch (m.bevel_width) {
        case BevelWidth::Offset:
          off_in[size_t(k)] = off_out[size_t(k)] = a;
          break;
        case BevelWidth::Width:
          off_in[size_t(k)] = off_out[size_t(k)] = a / (2.0f * std::max(std::sin(half), 1e-3f));
          break;
        case BevelWidth::Depth:
          off_in[size_t(k)] = off_out[size_t(k)] = a / std::max(std::cos(half), 1e-3f);
          break;
        case BevelWidth::Percent:
          off_in[size_t(k)] = std::min(a, 100.0f) * 0.01f * length(in[3] - in[0]);
          off_out[size_t(k)] = std::min(a, 100.0f) * 0.01f * length(out[3] - out[0]);
          break;
      }
      if (off_in[size_t(k)] <= 0.0f || off_out[size_t(k)] <= 0.0f) {
        continue;
      }
      amount[size_t(k)] = 1.0f;
      any = true;
    }
    if (!any) {
      continue;
    }
    /* Each segment trimmed where the bevels at its ends cut it: no further than its middle when
     * both ends are cut (Clamp Overlap), or than its far end. */
    std::vector<float> t0(size_t(segments), 0.0f), t1(size_t(segments), 1.0f);
    for (int s = 0; s < segments; s++) {
      const int a = s, b = (s + 1) % n;
      const float chord = length(ctrl[size_t(s)][3] - ctrl[size_t(s)][0]);
      const bool both = amount[size_t(a)] > 0.0f && amount[size_t(b)] > 0.0f;
      const float most = chord * (m.bevel_clamp && both ? 0.5f : 0.999f);
      if (amount[size_t(a)] > 0.0f) {
        t0[size_t(s)] = t_at_distance(ctrl[size_t(s)].data(), std::min(off_out[size_t(a)], most), false);
      }
      if (amount[size_t(b)] > 0.0f) {
        t1[size_t(s)] = std::max(t_at_distance(ctrl[size_t(s)].data(), std::min(off_in[size_t(b)], most), true),
                                 t0[size_t(s)]);
      }
    }
    std::vector<std::array<float2, 4>> part(static_cast<size_t>(segments));
    for (int s = 0; s < segments; s++) {
      bezier_part(ctrl[size_t(s)].data(), t0[size_t(s)], t1[size_t(s)], part[size_t(s)].data());
    }

    /* The curve again: a cut corner as two points (A on the way in, B on the way out), joined by the
     * bevel; every point's handles from the trimmed segments either side. */
    Curve curve = original;
    curve.points.clear();
    std::vector<int> first_of(static_cast<size_t>(n)), bevel_segment(size_t(n), -1);
    for (int k = 0; k < n; k++) {
      const CurvePoint &cp = original.points[size_t(k)];
      const int in_s = original.closed || k > 0 ? (k - 1 + segments) % segments : -1;
      const int out_s = original.closed || k < n - 1 ? k % segments : -1;
      const auto with_handles = [&](CurvePoint p, float2 pos, float2 in, float2 out) {
        p.handles_set = true;
        p.aligned = false;
        p.handle_in = in - pos;
        p.handle_out = out - pos;
        p.setup_in = p.handle_in;
        p.setup_out = p.handle_out;
        return p;
      };
      if (amount[size_t(k)] <= 0.0f) {
        const float2 pos = drawing.points[size_t(cp.point)].pos;
        first_of[size_t(k)] = int(curve.points.size());
        curve.points.push_back(with_handles(cp, pos, in_s >= 0 ? part[size_t(in_s)][2] : pos + cp.handle_in,
                                            out_s >= 0 ? part[size_t(out_s)][1] : pos + cp.handle_out));
        continue;
      }
      const float2 corner = drawing.points[size_t(cp.point)].pos;
      const float2 a = part[size_t(in_s)][3], b = part[size_t(out_s)][0];
      /* The bevel's handles, toward the corner: a circle's share for its turn (4/3 tan(turn/4) /
       * tan(turn/2) of the way), less for straighter, more for squarer, turned inward below 0.25. */
      const float turn = std::acos(std::clamp(dot(normalize(corner - a), normalize(b - corner)), -1.0f, 1.0f));
      const float round = turn > 1e-4f ? 4.0f / 3.0f * std::tan(turn * 0.25f) / std::tan(turn * 0.5f) : 0.0f;
      float share = profile >= 0.5f ? round + (1.0f - round) * (profile - 0.5f) / 0.5f
                    : profile >= 0.25f ? round * (profile - 0.25f) / 0.25f
                                       : -round * (0.25f - profile) / 0.25f;
      float2 ha = a + (corner - a) * share, hb = b + (corner - b) * share;
      if (share < 0.0f) {
        /* Inward: the round arc mirrored across the cut. */
        const float2 axis = normalize(b - a);
        const auto mirror = [&](float2 q, float2 from) {
          const float2 rel = q - from;
          const float2 along = axis * dot(rel, axis);
          return from + along - (rel - along);
        };
        ha = mirror(a + (corner - a) * -share, a);
        hb = mirror(b + (corner - b) * -share, b);
      }
      /* A keeps the corner point (its id, keys and weights); B is a copy of it. */
      MeshPoint copy = drawing.points[size_t(cp.point)];
      drawing.points[size_t(cp.point)].pos = a;
      copy.pos = b;
      copy.id = drawing.next_id++;
      drawing.points.push_back(copy);
      const int b_point = int(drawing.points.size()) - 1;
      first_of[size_t(k)] = int(curve.points.size());
      bevel_segment[size_t(k)] = int(curve.points.size());
      CurvePoint pa = with_handles(cp, a, part[size_t(in_s)][2], ha);
      /* The bevel shows where both sides did. */
      pa.edge_hidden = original.points[size_t(in_s)].edge_hidden && cp.edge_hidden;
      curve.points.push_back(pa);
      CurvePoint pb = cp;
      pb.point = b_point;
      curve.points.push_back(with_handles(pb, b, hb, part[size_t(out_s)][1]));
    }
    /* A point's own segment now starts at the last of its points (B, for a cut corner). */
    std::vector<int> segment_of(static_cast<size_t>(n));
    for (int k = 0; k < n; k++) {
      segment_of[size_t(k)] = bevel_segment[size_t(k)] >= 0 ? bevel_segment[size_t(k)] + 1 : first_of[size_t(k)];
    }
    for (Shape &shape : drawing.shapes) {
      std::vector<bool> has(size_t(segments), false);
      bool on_curve = false;
      for (ShapeEdge &e : shape.edges) {
        if (e.curve == c && e.segment >= 0 && e.segment < segments) {
          has[size_t(e.segment)] = true;
          e.segment = segment_of[size_t(e.segment)];
          on_curve = true;
        }
      }
      if (!on_curve) {
        continue;
      }
      /* The bevel joins a shape running through the corner. */
      for (int k = 0; k < n; k++) {
        if (bevel_segment[size_t(k)] >= 0 && has[size_t((k - 1 + segments) % segments)] && has[size_t(k % segments)]) {
          shape.edges.push_back({c, bevel_segment[size_t(k)]});
        }
      }
    }
    drawing.curves[size_t(c)] = std::move(curve);
  }
  return drawing;
}

}  // namespace oanim
