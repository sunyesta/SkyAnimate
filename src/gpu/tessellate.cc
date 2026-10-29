#include "gpu/tessellate.hh"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include <mapbox/earcut.hpp>

namespace oanim {

static Vertex make_vertex(float2 p, Color c, float alpha_scale)
{
  const float a = c.a * alpha_scale;
  return {p.x, p.y, c.r * a, c.g * a, c.b * a, a};
}

/* A stroke's vertex at `p`, of its cross-section about `centre` on the line. */
static Vertex make_vertex(float2 p, float2 centre, Color c, float alpha_scale)
{
  Vertex v = make_vertex(p, c, alpha_scale);
  v.ox = p.x - centre.x;
  v.oy = p.y - centre.y;
  return v;
}

/* The vertices from `first` on, a disc's about `centre`: their offsets from it. */
static void offsets_from(std::vector<Vertex> &vertices, size_t first, float2 centre)
{
  for (size_t i = first; i < vertices.size(); i++) {
    vertices[i].ox = vertices[i].x - centre.x;
    vertices[i].oy = vertices[i].y - centre.y;
  }
}

static void quad(std::vector<Vertex> &out, const Vertex &a, const Vertex &b, const Vertex &c, const Vertex &d)
{
  out.insert(out.end(), {a, b, c, a, c, d});
}

/* Drops points closer than `epsilon` to the previous one, keeping the paired widths and joins in
 * step. A dropped point's corner join passes to the point it duplicates. */
/* Room for `extra` more vertices. Many strokes, discs and fills append to one batch: reserving just what each
 * needs would copy the whole batch every time (a grid's lines, each cap a disc), so it grows by doubling. */
static void reserve_more(std::vector<Vertex> &out, size_t extra)
{
  const size_t need = out.size() + extra;
  if (need > out.capacity()) {
    out.reserve(std::max(need, out.capacity() * 2));
  }
}

static void dedupe(std::vector<float2> &points, std::vector<float> *widths, bool closed, float epsilon,
                   std::vector<StrokeJoin> *joins = nullptr)
{
  std::vector<float2> p;
  std::vector<float> w;
  std::vector<StrokeJoin> j;
  for (size_t i = 0; i < points.size(); i++) {
    if (!p.empty() && distance(p.back(), points[i]) <= epsilon) {
      if (joins) {
        j.back() = std::max(j.back(), (*joins)[i]);
      }
      continue;
    }
    p.push_back(points[i]);
    if (widths) {
      w.push_back((*widths)[i]);
    }
    if (joins) {
      j.push_back((*joins)[i]);
    }
  }
  if (closed && p.size() > 1 && distance(p.front(), p.back()) <= epsilon) {
    p.pop_back();
    if (widths) {
      w.pop_back();
    }
    if (joins) {
      j.front() = std::max(j.front(), j.back());
      j.pop_back();
    }
  }
  points = std::move(p);
  if (widths) {
    *widths = std::move(w);
  }
  if (joins) {
    *joins = std::move(j);
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

/* A soft edge: a strip from the ring outwards (`outward` +1: its right-hand side), fading to
 * transparent. */
static void soft_edge(const std::vector<float2> &poly, float outward, Color color, float aa, std::vector<Vertex> &r_out)
{
  const size_t n = poly.size();
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

void tessellate_fill(const std::vector<float2> &polygon, Color color, float aa, std::vector<Vertex> &r_out)
{
  std::vector<float2> poly = polygon;
  dedupe(poly, nullptr, true, aa * 0.01f);
  const size_t n = poly.size();
  if (n < 3) {
    return;
  }

  /* The same outline is often filled again soon: by each mask pass over a layer, and by every layer
   * still drawn while another one moves. Cutting it into triangles is most of the work, so the cut
   * is kept for a while, by the outline's exact points. */
  uint64_t key = 1469598103934665603ull;
  {
    const auto *bytes = reinterpret_cast<const unsigned char *>(poly.data());
    for (size_t i = 0; i < n * sizeof(float2); i++) {
      key = (key ^ bytes[i]) * 1099511628211ull;
    }
    key = (key ^ n) * 1099511628211ull;
  }
  thread_local std::unordered_map<uint64_t, std::vector<uint32_t>> cuts;
  auto found = cuts.find(key);
  if (found == cuts.end()) {
    if (cuts.size() > 4096) {
      cuts.clear();
    }
    using EarPoint = std::array<float, 2>;
    std::vector<std::vector<EarPoint>> rings(1);
    rings[0].reserve(n);
    for (const float2 &p : poly) {
      rings[0].push_back({p.x, p.y});
    }
    found = cuts.emplace(key, mapbox::earcut<uint32_t>(rings)).first;
  }
  const std::vector<uint32_t> &indices = found->second;
  reserve_more(r_out, indices.size());
  for (const uint32_t i : indices) {
    r_out.push_back(make_vertex(poly[i], color, 1.0f));
  }

  /* Soft edge: a strip from the outline outwards, fading to transparent. None when the caller
   * smooths edges by multisampling instead. */
  if (aa <= 0.0f) {
    return;
  }
  soft_edge(poly, signed_area(poly) > 0.0f ? 1.0f : -1.0f, color, aa, r_out);
}

void tessellate_fill_pieces(const std::vector<FillPiece> &pieces, Color color, float aa, std::vector<Vertex> &r_out)
{
  using EarPoint = std::array<float, 2>;
  for (const FillPiece &piece : pieces) {
    std::vector<std::vector<EarPoint>> rings;
    std::vector<float2> points;
    for (const std::vector<float2> &ring : piece.rings) {
      if (ring.size() < 3) {
        continue;
      }
      rings.emplace_back();
      for (const float2 &p : ring) {
        rings.back().push_back({p.x, p.y});
        points.push_back(p);
      }
    }
    if (rings.empty()) {
      continue;
    }
    /* Kept for a while, as tessellate_fill keeps its cuts. */
    uint64_t key = 0x9e3779b97f4a7c15ull;
    for (const std::vector<EarPoint> &ring : rings) {
      const auto *bytes = reinterpret_cast<const unsigned char *>(ring.data());
      for (size_t i = 0; i < ring.size() * sizeof(EarPoint); i++) {
        key = (key ^ bytes[i]) * 1099511628211ull;
      }
      key = (key ^ ring.size()) * 1099511628211ull;
    }
    thread_local std::unordered_map<uint64_t, std::vector<uint32_t>> cuts;
    auto found = cuts.find(key);
    if (found == cuts.end()) {
      if (cuts.size() > 4096) {
        cuts.clear();
      }
      found = cuts.emplace(key, mapbox::earcut<uint32_t>(rings)).first;
    }
    for (const uint32_t i : found->second) {
      r_out.push_back(make_vertex(points[i], color, 1.0f));
    }
    /* Outer rings run counter-clockwise and holes clockwise: outside the fill is always to the right. */
    if (aa > 0.0f) {
      for (const std::vector<float2> &ring : piece.rings) {
        if (ring.size() >= 3) {
          soft_edge(ring, 1.0f, color, aa, r_out);
        }
      }
    }
  }
}

void tessellate_stencil_fan(const std::vector<const std::vector<float2> *> &rings, Color color, float pad,
                            std::vector<Vertex> &r_out, std::vector<uint32_t> &r_indices)
{
  size_t edges = 0;
  float2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
  for (const std::vector<float2> *ring : rings) {
    edges += ring->size() >= 3 ? ring->size() : 0;
    for (const float2 &p : *ring) {
      lo = float2(std::min(lo.x, p.x), std::min(lo.y, p.y));
      hi = float2(std::max(hi.x, p.x), std::max(hi.y, p.y));
    }
  }
  if (edges == 0) {
    return;
  }
  reserve_more(r_out, edges + 7);
  r_indices.reserve(r_indices.size() + edges * 3);
  /* Only the stencil is drawn: no colour. */
  const Color none{0.0f, 0.0f, 0.0f, 0.0f};
  const float2 pivot((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
  const uint32_t middle = uint32_t(r_out.size());
  r_out.push_back(make_vertex(pivot, none, 1.0f));
  for (const std::vector<float2> *ring : rings) {
    const size_t n = ring->size();
    if (n < 3) {
      continue;
    }
    const uint32_t first = uint32_t(r_out.size());
    for (size_t i = 0; i < n; i++) {
      r_out.push_back(make_vertex((*ring)[i], none, 1.0f));
      r_indices.insert(r_indices.end(), {middle, first + uint32_t(i), first + uint32_t((i + 1) % n)});
    }
  }
  lo = lo - float2(pad, pad);
  hi = hi + float2(pad, pad);
  quad(r_out, make_vertex(lo, color, 1.0f), make_vertex(float2(hi.x, lo.y), color, 1.0f), make_vertex(hi, color, 1.0f),
       make_vertex(float2(lo.x, hi.y), color, 1.0f));
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

/* A round join or cap for a stroke of half width `hw`. Its solid part ends where the stroke's core
 * does (half a soft edge inside the line edge) so its soft edge lines up with the stroke's and
 * no bump shows where they meet. */
static void stroke_disc(float2 center, float hw, Color color, float aa, std::vector<Vertex> &r_out)
{
  const size_t first = r_out.size();
  disc(center, std::max(hw - aa * 0.5f, aa * 0.25f), color, aa, r_out);
  offsets_from(r_out, first, center);
}

/* SVG's default: a sharp corner tighter than this (miter length over half width) is bevelled. */
static constexpr float MITER_LIMIT = 4.0f;

void tessellate_stroke(const std::vector<float2> &input_points, const std::vector<float> &input_half_widths,
                       bool closed, Color color, float aa, std::vector<Vertex> &r_out,
                       const std::vector<StrokeJoin> *input_joins)
{
  const size_t first_vertex = r_out.size();
  std::vector<float2> points = input_points;
  std::vector<float> half_widths = input_half_widths;
  std::vector<StrokeJoin> joins = input_joins ? *input_joins : std::vector<StrokeJoin>(points.size(), StrokeJoin::Smooth);
  /* Points far closer than the line is wide merge: a dot drawn as a zero-length stroke (Moho's
   * freckles) has its points a rounding error apart, and their directions would be noise. */
  float widest = 0.0f;
  for (const float hw : half_widths) {
    widest = std::max(widest, hw);
  }
  dedupe(points, &half_widths, closed, std::max(aa * 0.01f, widest * 1e-3f), &joins);
  const size_t n = points.size();
  if (n == 0) {
    return;
  }
  /* Every vertex made below with the stroke's widest half width (Vertex). */
  struct Widest {
    std::vector<Vertex> &out;
    size_t first;
    float widest;
    ~Widest()
    {
      for (size_t i = first; i < out.size(); i++) {
        out[i].w = widest;
      }
    }
  } mark_widest{r_out, first_vertex, widest};
  if (n == 1) {
    disc(points[0], std::max(half_widths[0], aa * 0.5f), color, aa, r_out);
    offsets_from(r_out, first_vertex, points[0]);
    return;
  }
  for (float &hw : half_widths) {
    hw = std::max(hw, 0.0f);
  }

  /* The stroke is a disc of each point's half width swept along the line. Each segment's two
   * edges are the lines touching the discs at both of its ends, so where the width changes the
   * edges lean in or out: m.d = (r_start - r_end) / length for each edge's unit normal m. Round
   * joins and caps are then exactly those discs, and sharp corners mitre to where the leaning
   * edges actually meet. */
  const size_t segments = closed ? n : n - 1;
  std::vector<float2> dirs(segments), normal_left(segments), normal_right(segments);
  for (size_t i = 0; i < segments; i++) {
    const size_t j = (i + 1) % n;
    const float len = distance(points[i], points[j]);
    const float2 d = (points[j] - points[i]) / len;
    const float lean = std::clamp((half_widths[i] - half_widths[j]) / len, -0.999f, 0.999f);
    const float2 side = perpendicular(d) * std::sqrt(1.0f - lean * lean);
    dirs[i] = d;
    normal_left[i] = side + d * lean;
    normal_right[i] = -side + d * lean;
  }

  /* A stroke cross-section at a point: its edges at `left` and `right` per unit of half width.
   * The solid core stops half a soft edge inside the line edge and fades over one soft edge. */
  struct Side {
    float2 outer_left, core_left, core_right, outer_right;
    float core_alpha;
    float2 centre;
  };
  const auto core_radius = [&](size_t i) { return std::max(half_widths[i] - aa * 0.5f, 0.0f); };
  const auto make_side = [&](size_t i, float2 left, float2 right) {
    const float hw = half_widths[i];
    const float core = core_radius(i), outer = core + aa;
    /* Lines thinner than the soft edge fade instead of vanishing. */
    const float core_alpha = hw < aa * 0.5f ? std::max(hw * 2.0f / aa, 0.05f) : 1.0f;
    const float2 p = points[i];
    return Side{p + left * outer, p + left * core, p + right * core, p + right * outer, core_alpha, p};
  };
  /* Where two edges at unit distance from a point meet (their normals m0 and m1), per unit of
   * half width. Long mitres are cut short; `too_sharp` reports it so a disc can cover the gap. */
  const auto mitre = [](float2 m0, float2 m1, bool *too_sharp) {
    const float denom = 1.0f + dot(m0, m1);
    constexpr float MIN_DENOM = 2.0f * 0.35f * 0.35f; /* Mitres at most 1 / 0.35 long. */
    if (denom >= MIN_DENOM) {
      return (m0 + m1) / denom;
    }
    *too_sharp = true;
    const float2 sum = m0 + m1;
    const float len = length(sum);
    return len > 1e-6f ? sum / len * (1.0f / 0.35f) : m1;
  };

  std::vector<Side> seg_start(segments), seg_end(segments);
  for (size_t i = 0; i < n; i++) {
    const bool has_prev = closed || i > 0;
    const bool has_next = closed || i + 1 < n;
    const size_t next_seg = i % segments, prev_seg = (i + segments - 1) % segments;
    if (!(has_prev && has_next)) {
      const size_t seg = has_next ? next_seg : prev_seg;
      const Side side = make_side(i, normal_left[seg], normal_right[seg]);
      if (has_next) {
        seg_start[next_seg] = side;
      }
      if (has_prev) {
        seg_end[prev_seg] = side;
      }
      continue;
    }
    const float2 d0 = dirs[prev_seg], d1 = dirs[next_seg];
    const bool corner = joins[i] != StrokeJoin::Smooth && dot(d0, d1) < 0.9995f;
    if (!corner) {
      /* Along a curve the two segments share one cross-section where their edges meet. */
      bool too_sharp = false;
      const float2 left = mitre(normal_left[prev_seg], normal_left[next_seg], &too_sharp);
      const float2 right = mitre(normal_right[prev_seg], normal_right[next_seg], &too_sharp);
      if (too_sharp) {
        stroke_disc(points[i], half_widths[i], color, aa, r_out);
      }
      seg_start[next_seg] = seg_end[prev_seg] = make_side(i, left, right);
      continue;
    }
    seg_end[prev_seg] = make_side(i, normal_left[prev_seg], normal_right[prev_seg]);
    seg_start[next_seg] = make_side(i, normal_left[next_seg], normal_right[next_seg]);
    if (joins[i] == StrokeJoin::Round) {
      stroke_disc(points[i], half_widths[i], color, aa, r_out);
      continue;
    }
    /* Sharp: fill the outside of the turn out to where the two outside edges meet, or bevel it
     * past the miter limit. Built from the strips' own core and soft-edge offsets, so the
     * edges carry straight on. */
    const bool left = cross(d0, d1) < 0.0f; /* Turning right: the outside is the left side. */
    const float2 m0 = left ? normal_left[prev_seg] : normal_right[prev_seg];
    const float2 m1 = left ? normal_left[next_seg] : normal_right[next_seg];
    const float denom = 1.0f + dot(m0, m1);
    if (denom < 1e-4f) {
      continue; /* A full reversal: nothing sticks out to fill. */
    }
    const Side &in = seg_end[prev_seg];
    const Side &out = seg_start[next_seg];
    const float2 p = points[i];
    const float2 c0 = left ? in.core_left : in.core_right, c1 = left ? out.core_left : out.core_right;
    const float2 q0 = left ? in.outer_left : in.outer_right, q1 = left ? out.outer_left : out.outer_right;
    const float alpha = in.core_alpha;
    /* Where the width changes the segments' end cross-sections lean away from the point; fill
     * between them and the point (a round join's disc covers this by itself). */
    const float2 i0 = left ? in.core_right : in.core_left, i1 = left ? out.core_right : out.core_left;
    for (const auto &[a, b] : {std::pair{c0, i0}, std::pair{c1, i1}, std::pair{i0, i1}}) {
      r_out.insert(r_out.end(), {make_vertex(p, p, color, alpha), make_vertex(a, p, color, alpha), make_vertex(b, p, color, alpha)});
    }
    const float2 tip = (m0 + m1) / denom;
    /* How far the corner reaches is set by the turn alone, as for a stroke of even width: a
     * tapering stroke's leaning edges can meet much further out, so the point is cut flat
     * there instead of growing a long spike. */
    const float2 n0 = perpendicular(d0), n1 = perpendicular(d1);
    const float2 out_dir = normalize(n0 + n1) * (left ? 1.0f : -1.0f);
    const float reach = 1.0f / std::max(dot(normalize(n0 + n1), n1), 1e-4f);
    if (reach <= MITER_LIMIT) {
      const float core = core_radius(i), outer = core + aa;
      /* Where an edge from `from` heading for `to` crosses the cut line, or `to` if it doesn't
       * reach it. */
      const auto clip = [&](float2 from, float2 to, float limit) {
        const float2 dir = to - from;
        const float from_d = dot(from - p, out_dir), to_d = dot(to - p, out_dir);
        if (to_d <= limit || to_d - from_d < 1e-6f) {
          return to;
        }
        return from + dir * ((limit - from_d) / (to_d - from_d));
      };
      const float2 c_tip = p + tip * core, q_tip = p + tip * outer;
      const float2 ck0 = clip(c0, c_tip, reach * core), ck1 = clip(c1, c_tip, reach * core);
      const float2 qk0 = clip(q0, q_tip, reach * outer), qk1 = clip(q1, q_tip, reach * outer);
      r_out.insert(r_out.end(), {make_vertex(p, p, color, alpha), make_vertex(c0, p, color, alpha),
                                 make_vertex(ck0, p, color, alpha), make_vertex(p, p, color, alpha),
                                 make_vertex(ck0, p, color, alpha), make_vertex(ck1, p, color, alpha),
                                 make_vertex(p, p, color, alpha), make_vertex(ck1, p, color, alpha),
                                 make_vertex(c1, p, color, alpha)});
      if (aa > 0.0f) {
        quad(r_out, make_vertex(c0, p, color, alpha), make_vertex(ck0, p, color, alpha), make_vertex(qk0, p, color, 0.0f),
             make_vertex(q0, p, color, 0.0f));
        quad(r_out, make_vertex(ck0, p, color, alpha), make_vertex(ck1, p, color, alpha), make_vertex(qk1, p, color, 0.0f),
             make_vertex(qk0, p, color, 0.0f));
        quad(r_out, make_vertex(ck1, p, color, alpha), make_vertex(c1, p, color, alpha), make_vertex(q1, p, color, 0.0f),
             make_vertex(qk1, p, color, 0.0f));
      }
    }
    else {
      /* Bevel: a flat edge across the corner, its soft edge joined to the strips' by two slivers. */
      float2 bevel_normal = perpendicular(normalize(c1 - c0));
      if (dot(bevel_normal, m0 + m1) < 0.0f) {
        bevel_normal = -bevel_normal;
      }
      const float2 b0 = c0 + bevel_normal * aa, b1 = c1 + bevel_normal * aa;
      r_out.insert(r_out.end(),
                   {make_vertex(p, p, color, alpha), make_vertex(c0, p, color, alpha), make_vertex(c1, p, color, alpha)});
      if (aa <= 0.0f) {
        continue;
      }
      quad(r_out, make_vertex(c0, p, color, alpha), make_vertex(c1, p, color, alpha), make_vertex(b1, p, color, 0.0f),
           make_vertex(b0, p, color, 0.0f));
      r_out.insert(r_out.end(), {make_vertex(c0, p, color, alpha), make_vertex(q0, p, color, 0.0f),
                                 make_vertex(b0, p, color, 0.0f), make_vertex(c1, p, color, alpha),
                                 make_vertex(b1, p, color, 0.0f), make_vertex(q1, p, color, 0.0f)});
    }
  }

  /* With no soft edge (multisampling smooths the edges instead) the strips either side of the core
   * have no width: only the core is drawn. */
  reserve_more(r_out, segments * (aa > 0.0f ? 18 : 6));
  for (size_t i = 0; i < segments; i++) {
    const Side &a = seg_start[i];
    const Side &b = seg_end[i];
    if (aa > 0.0f) {
      quad(r_out, make_vertex(a.outer_left, a.centre, color, 0.0f), make_vertex(a.core_left, a.centre, color, a.core_alpha),
           make_vertex(b.core_left, b.centre, color, b.core_alpha), make_vertex(b.outer_left, b.centre, color, 0.0f));
    }
    quad(r_out, make_vertex(a.core_left, a.centre, color, a.core_alpha), make_vertex(a.core_right, a.centre, color, a.core_alpha),
         make_vertex(b.core_right, b.centre, color, b.core_alpha), make_vertex(b.core_left, b.centre, color, b.core_alpha));
    if (aa > 0.0f) {
      quad(r_out, make_vertex(a.core_right, a.centre, color, a.core_alpha), make_vertex(a.outer_right, a.centre, color, 0.0f),
           make_vertex(b.outer_right, b.centre, color, 0.0f), make_vertex(b.core_right, b.centre, color, b.core_alpha));
    }
  }

  if (closed) {
    return;
  }
  /* Ends: round caps (the end point's disc, as Moho draws line ends by default), or flat for a
   * Sharp point, with a soft edge across the end. */
  const auto cap = [&](size_t i, const Side &side, float2 outward) {
    if (joins[i] != StrokeJoin::Sharp) {
      stroke_disc(points[i], half_widths[i], color, aa, r_out);
      return;
    }
    if (aa <= 0.0f) {
      return;
    }
    const float2 step = outward * aa;
    quad(r_out, make_vertex(side.core_left, side.centre, color, side.core_alpha), make_vertex(side.core_right, side.centre, color, side.core_alpha),
         make_vertex(side.core_right + step, side.centre, color, 0.0f), make_vertex(side.core_left + step, side.centre, color, 0.0f));
  };
  cap(0, seg_start[0], -dirs[0]);
  cap(n - 1, seg_end[segments - 1], dirs[segments - 1]);
}

}  // namespace oanim

