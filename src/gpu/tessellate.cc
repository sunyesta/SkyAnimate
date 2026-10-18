#include "gpu/tessellate.hh"

#include <algorithm>
#include <array>
#include <cstdint>

#include <mapbox/earcut.hpp>

namespace sky {

static Vertex make_vertex(float2 p, Color c, float alpha_scale)
{
  const float a = c.a * alpha_scale;
  return {p.x, p.y, c.r * a, c.g * a, c.b * a, a};
}

static void quad(std::vector<Vertex> &out, const Vertex &a, const Vertex &b, const Vertex &c, const Vertex &d)
{
  out.insert(out.end(), {a, b, c, a, c, d});
}

/* Drops points closer than `epsilon` to the previous one, keeping the paired widths in step. */
static void dedupe(std::vector<float2> &points, std::vector<float> *widths, bool closed, float epsilon)
{
  std::vector<float2> p;
  std::vector<float> w;
  for (size_t i = 0; i < points.size(); i++) {
    if (!p.empty() && distance(p.back(), points[i]) <= epsilon) {
      continue;
    }
    p.push_back(points[i]);
    if (widths) {
      w.push_back((*widths)[i]);
    }
  }
  if (closed && p.size() > 1 && distance(p.front(), p.back()) <= epsilon) {
    p.pop_back();
    if (widths) {
      w.pop_back();
    }
  }
  points = std::move(p);
  if (widths) {
    *widths = std::move(w);
  }
}

static float signed_area(const std::vector<float2> &poly)
{
  float area = 0.0f;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    area += cross(poly[j], poly[i]);
  }
  return area * 0.5f;
}

void tessellate_fill(const std::vector<float2> &polygon, Color color, float aa, std::vector<Vertex> &r_out)
{
  std::vector<float2> poly = polygon;
  dedupe(poly, nullptr, true, aa * 0.01f);
  const size_t n = poly.size();
  if (n < 3) {
    return;
  }

  using EarPoint = std::array<float, 2>;
  std::vector<std::vector<EarPoint>> rings(1);
  rings[0].reserve(n);
  for (const float2 &p : poly) {
    rings[0].push_back({p.x, p.y});
  }
  const std::vector<uint32_t> indices = mapbox::earcut<uint32_t>(rings);
  for (const uint32_t i : indices) {
    r_out.push_back(make_vertex(poly[i], color, 1.0f));
  }

  /* Soft edge: a strip from the outline outwards, fading to transparent. */
  const float outward = signed_area(poly) > 0.0f ? 1.0f : -1.0f;
  std::vector<float2> outer(n);
  for (size_t i = 0; i < n; i++) {
    const float2 prev = poly[(i + n - 1) % n], cur = poly[i], next = poly[(i + 1) % n];
    const float2 n0 = normalize(float2(cur.y - prev.y, prev.x - cur.x)) * outward;
    const float2 n1 = normalize(float2(next.y - cur.y, cur.x - next.x)) * outward;
    const float2 avg = normalize(n0 + n1);
    const float miter = 1.0f / std::max(dot(avg, n1), 0.4f);
    outer[i] = cur + avg * (aa * miter);
  }
  for (size_t i = 0; i < n; i++) {
    const size_t j = (i + 1) % n;
    quad(r_out, make_vertex(poly[i], color, 1.0f), make_vertex(poly[j], color, 1.0f),
         make_vertex(outer[j], color, 0.0f), make_vertex(outer[i], color, 0.0f));
  }
}

static void disc(float2 center, float radius, Color color, float aa, std::vector<Vertex> &r_out)
{
  const int segments = std::clamp(int(radius / std::max(aa, 1e-6f) * 1.5f), 8, 48);
  std::vector<float2> circle(segments);
  for (int i = 0; i < segments; i++) {
    const float angle = 2.0f * PI * float(i) / float(segments);
    circle[i] = center + float2(std::cos(angle), std::sin(angle)) * radius;
  }
  tessellate_fill(circle, color, aa, r_out);
}

void tessellate_stroke(const std::vector<float2> &input_points, const std::vector<float> &input_half_widths,
                       bool closed, Color color, float aa, std::vector<Vertex> &r_out)
{
  std::vector<float2> points = input_points;
  std::vector<float> half_widths = input_half_widths;
  dedupe(points, &half_widths, closed, aa * 0.01f);
  const size_t n = points.size();
  if (n == 0) {
    return;
  }
  if (n == 1) {
    disc(points[0], std::max(half_widths[0], aa * 0.5f), color, aa, r_out);
    return;
  }

  const size_t segments = closed ? n : n - 1;
  std::vector<float2> dirs(segments);
  for (size_t i = 0; i < segments; i++) {
    dirs[i] = normalize(points[(i + 1) % n] - points[i]);
  }

  struct Side {
    float2 outer_left, core_left, core_right, outer_right;
    float core_alpha;
  };
  std::vector<Side> sides(n);
  for (size_t i = 0; i < n; i++) {
    float2 normal;
    float miter = 1.0f;
    const bool has_prev = closed || i > 0;
    const bool has_next = closed || i + 1 < n;
    if (has_prev && has_next) {
      const float2 d0 = dirs[(i + segments - 1) % segments], d1 = dirs[i % segments];
      const float2 n0 = perpendicular(d0), n1 = perpendicular(d1);
      normal = normalize(n0 + n1);
      miter = 1.0f / std::max(dot(normal, n1), 0.35f);
      /* Sharp corners get a disc so the join stays round instead of spiking. */
      if (dot(d0, d1) < 0.3f) {
        disc(points[i], half_widths[i], color, aa, r_out);
      }
    }
    else {
      normal = perpendicular(has_next ? dirs[i] : dirs[i - 1]);
    }
    const float hw = std::max(half_widths[i], 0.0f);
    const float core = std::max(hw - aa * 0.5f, 0.0f);
    const float outer = core + aa;
    /* Lines thinner than the soft edge fade instead of vanishing. */
    const float core_alpha = hw < aa * 0.5f ? std::max(hw * 2.0f / aa, 0.05f) : 1.0f;
    const float2 p = points[i];
    sides[i] = {p + normal * (outer * miter), p + normal * (core * miter), p - normal * (core * miter),
                p - normal * (outer * miter), core_alpha};
  }

  for (size_t i = 0; i < segments; i++) {
    const Side &a = sides[i];
    const Side &b = sides[(i + 1) % n];
    quad(r_out, make_vertex(a.outer_left, color, 0.0f), make_vertex(a.core_left, color, a.core_alpha),
         make_vertex(b.core_left, color, b.core_alpha), make_vertex(b.outer_left, color, 0.0f));
    quad(r_out, make_vertex(a.core_left, color, a.core_alpha), make_vertex(a.core_right, color, a.core_alpha),
         make_vertex(b.core_right, color, b.core_alpha), make_vertex(b.core_left, color, b.core_alpha));
    quad(r_out, make_vertex(a.core_right, color, a.core_alpha), make_vertex(a.outer_right, color, 0.0f),
         make_vertex(b.outer_right, color, 0.0f), make_vertex(b.core_right, color, b.core_alpha));
  }

  /* Round caps, as Moho draws line ends by default. */
  if (!closed) {
    disc(points.front(), half_widths.front(), color, aa, r_out);
    disc(points.back(), half_widths.back(), color, aa, r_out);
  }
}

}  // namespace sky
