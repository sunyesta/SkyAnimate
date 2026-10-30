#include "data/fill_area.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>

#pragma warning(push, 0)
#include <clipper2/clipper.h>
#pragma warning(pop)

namespace oanim {

namespace c2 = Clipper2Lib;

/* Clipper works in fixed point: 6 decimals of a unit (the frame is 2 units tall) is far finer than
 * anything drawn, letters at small sizes included. */
static constexpr int PRECISION = 6;

bool shape_fills_together(const VectorDrawing &drawing, const Shape &shape)
{
  return shape.fill_rule != FillRule::Loops && shape_runs(drawing, shape).size() > 1;
}

std::vector<std::vector<float2>> shape_fill_loops(const VectorDrawing &drawing, const Shape &shape, int min_steps,
                                                  float tolerance)
{
  std::vector<std::vector<float2>> loops;
  for (const Shape &run : shape_runs(drawing, shape)) {
    std::vector<float2> outline;
    bool closed = false;
    shape_outline(drawing, run, min_steps, outline, nullptr, &closed, nullptr, nullptr, tolerance);
    if (closed && outline.size() >= 3) {
      loops.push_back(std::move(outline));
    }
  }
  return loops;
}

static double ring_area(const std::vector<float2> &ring)
{
  double area = 0.0;
  for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
    area += double(ring[j].x) * ring[i].y - double(ring[i].x) * ring[j].y;
  }
  return area * 0.5;
}

static std::vector<float2> to_ring(const c2::PathD &path, bool hole)
{
  std::vector<float2> ring;
  ring.reserve(path.size());
  for (const c2::PointD &p : path) {
    ring.emplace_back(float(p.x), float(p.y));
  }
  if (ring.size() >= 3 && (ring_area(ring) > 0.0) == hole) {
    std::reverse(ring.begin(), ring.end());
  }
  return ring;
}

/* An outer ring's node: its piece (itself and its holes), then the pieces inside its holes. */
static void collect(const c2::PolyPathD &outer, std::vector<FillPiece> &r_pieces)
{
  FillPiece piece;
  piece.rings.push_back(to_ring(outer.Polygon(), false));
  for (const auto &hole : outer) {
    piece.rings.push_back(to_ring(hole->Polygon(), true));
  }
  if (piece.rings.front().size() >= 3) {
    r_pieces.push_back(std::move(piece));
  }
  for (const auto &hole : outer) {
    for (const auto &inner : *hole) {
      collect(*inner, r_pieces);
    }
  }
}

const std::vector<FillPiece> &fill_area(const std::vector<std::vector<float2>> &loops, FillRule rule)
{
  uint64_t key = 1469598103934665603ull;
  const auto mix = [&](const void *data, size_t size) {
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; i++) {
      key = (key ^ bytes[i]) * 1099511628211ull;
    }
  };
  mix(&rule, sizeof(rule));
  for (const std::vector<float2> &loop : loops) {
    const size_t n = loop.size();
    mix(&n, sizeof(n));
    mix(loop.data(), n * sizeof(float2));
  }
  thread_local std::unordered_map<uint64_t, std::vector<FillPiece>> cache;
  if (auto found = cache.find(key); found != cache.end()) {
    return found->second;
  }
  if (cache.size() > 2048) {
    cache.clear();
  }

  c2::PathsD paths;
  for (const std::vector<float2> &loop : loops) {
    if (loop.size() < 3) {
      continue;
    }
    c2::PathD path;
    path.reserve(loop.size());
    for (const float2 &p : loop) {
      path.push_back({double(p.x), double(p.y)});
    }
    /* Loops: each fills on its own, so all of them the same way round and joined. */
    if (rule == FillRule::Loops && c2::Area(path) < 0.0) {
      std::reverse(path.begin(), path.end());
    }
    paths.push_back(std::move(path));
  }
  std::vector<FillPiece> pieces;
  if (!paths.empty()) {
    c2::ClipperD clipper(PRECISION);
    clipper.AddSubject(paths);
    c2::PolyTreeD tree;
    clipper.Execute(c2::ClipType::Union, rule == FillRule::EvenOdd ? c2::FillRule::EvenOdd : c2::FillRule::NonZero,
                    tree);
    for (const auto &outer : tree) {
      collect(*outer, pieces);
    }
  }
  return cache.emplace(key, std::move(pieces)).first->second;
}

static c2::PathsD pieces_paths(const std::vector<FillPiece> &pieces)
{
  c2::PathsD paths;
  for (const FillPiece &piece : pieces) {
    for (const std::vector<float2> &ring : piece.rings) {
      c2::PathD path;
      path.reserve(ring.size());
      for (const float2 &p : ring) {
        path.push_back({double(p.x), double(p.y)});
      }
      paths.push_back(std::move(path));
    }
  }
  return paths;
}

static std::vector<FillPiece> tree_pieces(const c2::PolyTreeD &tree)
{
  std::vector<FillPiece> pieces;
  for (const auto &outer : tree) {
    collect(*outer, pieces);
  }
  return pieces;
}

std::vector<FillPiece> fill_boolean(const std::vector<FillPiece> &a, const std::vector<FillPiece> &b, FillBoolean op)
{
  c2::ClipperD clipper(PRECISION);
  clipper.AddSubject(pieces_paths(a));
  clipper.AddClip(pieces_paths(b));
  c2::PolyTreeD tree;
  /* Rings go round the ways FillPiece says, so nonzero counts holes out. */
  clipper.Execute(op == FillBoolean::Union ? c2::ClipType::Union : c2::ClipType::Difference, c2::FillRule::NonZero, tree);
  return tree_pieces(tree);
}

bool fill_overlap(const std::vector<FillPiece> &a, const std::vector<FillPiece> &b)
{
  if (a.empty() || b.empty()) {
    return false;
  }
  c2::ClipperD clipper(PRECISION);
  clipper.AddSubject(pieces_paths(a));
  clipper.AddClip(pieces_paths(b));
  c2::PathsD out;
  clipper.Execute(c2::ClipType::Intersection, c2::FillRule::NonZero, out);
  return !out.empty();
}

std::vector<FillPiece> fill_sweep(const std::vector<float2> &centers, const std::vector<float> &radii, float tolerance)
{
  c2::PathsD paths;
  const auto add = [&](c2::PathD path) {
    if (c2::Area(path) < 0.0) {
      std::reverse(path.begin(), path.end());
    }
    paths.push_back(std::move(path));
  };
  const size_t n = centers.size();
  for (size_t i = 0; i < n; i++) {
    const double r = radii[i];
    if (r <= 0.0) {
      continue;
    }
    /* A 12-gon at least: a corner sharper than that between two of its sides is a real one (ink_fit_ring). */
    const double step = std::acos(std::clamp(1.0 - double(tolerance) / r, -1.0, 1.0));
    const int sides = std::clamp(int(std::ceil(3.14159265 / std::max(step, 1e-3))), 12, 64);
    c2::PathD disc;
    disc.reserve(sides);
    for (int k = 0; k < sides; k++) {
      const double a = 2.0 * 3.14159265358979 * k / sides;
      disc.push_back({centers[i].x + r * std::cos(a), centers[i].y + r * std::sin(a)});
    }
    add(std::move(disc));
  }
  /* Between each two discs, the band touching both (its edges lean as the radius changes). */
  for (size_t i = 0; i + 1 < n; i++) {
    const float len = distance(centers[i], centers[i + 1]);
    const float r0 = radii[i], r1 = radii[i + 1];
    if (len <= std::abs(r0 - r1) || len <= 0.0f) {
      continue;
    }
    const float2 d = (centers[i + 1] - centers[i]) / len;
    const float lean = (r0 - r1) / len;
    const float2 side = perpendicular(d) * std::sqrt(1.0f - lean * lean);
    const float2 left = side + d * lean, right = d * lean - side;
    const auto point = [](float2 p) { return c2::PointD(p.x, p.y); };
    add({point(centers[i] + right * r0), point(centers[i + 1] + right * r1), point(centers[i + 1] + left * r1),
         point(centers[i] + left * r0)});
  }
  c2::ClipperD clipper(PRECISION);
  clipper.AddSubject(paths);
  c2::PolyTreeD tree;
  clipper.Execute(c2::ClipType::Union, c2::FillRule::NonZero, tree);
  return tree_pieces(tree);
}

bool fill_contains(const std::vector<std::vector<float2>> &loops, FillRule rule, float2 p)
{
  /* The winding number: each loop's crossings of a ray to the right, upward ones +1, downward -1. */
  int total = 0;
  for (const std::vector<float2> &loop : loops) {
    int winding = 0;
    for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
      const float2 a = loop[j], b = loop[i];
      if ((a.y > p.y) != (b.y > p.y)) {
        const float x = (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x;
        if (p.x < x) {
          winding += b.y > a.y ? 1 : -1;
        }
      }
    }
    if (rule == FillRule::Loops && winding != 0) {
      return true;
    }
    total += rule == FillRule::EvenOdd ? (winding & 1) : winding;
  }
  return rule == FillRule::EvenOdd ? (total & 1) != 0 : total != 0;
}

bool shape_fill_contains(const VectorDrawing &drawing, const Shape &shape, float2 p, int min_steps)
{
  return fill_contains(shape_fill_loops(drawing, shape, min_steps), shape.fill_rule, p);
}

}  // namespace oanim
