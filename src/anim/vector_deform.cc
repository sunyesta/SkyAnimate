#include "anim/vector_deform.hh"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>

#include "anim/deform.hh"
#include "anim/subdivide.hh"
#include "core/parallel.hh"
#include "data/document.hh"
#include "data/grid.hh"

namespace oanim {

const char *deform_kind_name(DeformKind kind)
{
  switch (kind) {
    case DeformKind::Grid:
      return "Grid";
    case DeformKind::Cage:
      return "Cage";
    case DeformKind::Line:
      return "Line";
    case DeformKind::Mesh:
      return "Mesh";
    case DeformKind::Wire:
      return "Wire";
    case DeformKind::None:
      break;
  }
  return "";
}

/* How finely a cage's or line's outline is followed: pieces per segment (a Curve Deform's line, LINE_STEPS). */
static constexpr int OUTLINE_STEPS = 16;
static constexpr int LINE_STEPS = 64;
/* Shorter at rest and a line is no shape (Stretch would spread what follows it by thousands). */
static constexpr float MIN_LINE_LENGTH = 1e-4f;

/* The layers whose deform modifiers are being worked out, innermost last: a shape bent (through others) by
 * the layer it bends is a loop, left alone. */
thread_local std::vector<uint64_t> g_deforming;

/* VectorDeformWireScope: how many are alive on this thread. */
thread_local int g_wire = 0;
/* Reading a cage's or a mesh's shape: Resample Self leaves it the points it was drawn with. */
static thread_local bool g_shape_read = false;

VectorDeformWireScope::VectorDeformWireScope()
{
  g_wire++;
}

VectorDeformWireScope::~VectorDeformWireScope()
{
  g_wire--;
}

/* VectorDeformProbeScope: its kind, 0 when there is none. */
thread_local int g_probe = 0;

VectorDeformProbeScope::VectorDeformProbeScope(int kind)
{
  g_probe = kind;
}

VectorDeformProbeScope::~VectorDeformProbeScope()
{
  g_probe = 0;
}

/* Every curve point's handles as the curve draws them, stored, so they bend with the deform too. */
static void handles_explicit(VectorDrawing &drawing)
{
  for (size_t c = 0; c < drawing.curves.size(); c++) {
    Curve &curve = drawing.curves[c];
    std::vector<std::array<float2, 4>> ctrl(size_t(std::max(curve.segment_count(), 0)));
    for (int s = 0; s < curve.segment_count(); s++) {
      curve_segment_bezier(drawing, curve, s, ctrl[size_t(s)].data());
    }
    const int n = int(curve.points.size());
    for (int s = 0; s < curve.segment_count(); s++) {
      CurvePoint &from = curve.points[size_t(s)], &to = curve.points[size_t((s + 1) % n)];
      from.handle_out = ctrl[size_t(s)][1] - ctrl[size_t(s)][0];
      to.handle_in = ctrl[size_t(s)][2] - ctrl[size_t(s)][3];
      from.handles_set = to.handles_set = true;
    }
  }
}

static float smooth01(float x)
{
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

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

/* The grid of `drawing` whose nodes `shape` runs through, or null. */
static const DrawingGrid *shape_grid(const VectorDrawing &drawing, const Shape &shape)
{
  if (shape.edges.empty() || shape.edges[0].curve >= int(drawing.curves.size())) {
    return nullptr;
  }
  int a, b;
  curve_segment_points(drawing.curves[size_t(shape.edges[0].curve)], shape.edges[0].segment, &a, &b);
  const uint64_t id = drawing.points[size_t(a)].id;
  for (const DrawingGrid &grid : drawing.grids) {
    if (std::find(grid.nodes.begin(), grid.nodes.end(), id) != grid.nodes.end()) {
      return &grid;
    }
  }
  return nullptr;
}

struct RigidLattice;
struct PointBones;

/* A modifier's shape at rest and as it is now, both in the bent layer's space. */
struct DeformShape {
  DeformKind kind = DeformKind::None;
  GridSurface rest_grid, now_grid;
  /* A cage's outline (closed) or a line (open), sample for sample. */
  std::vector<float2> rest, now;
  /* Where Automatic weights measure from: the rest outline (a grid's: its border), closed unless a line. */
  std::vector<float2> border;
  /* A line's: how far along it each sample is, at rest and now. */
  std::vector<float> rest_at, now_at;
  /* Stretch: what lies along a line spreads along it as it grows (else it keeps its length). */
  bool stretch = true;
  /* A network's cells (Grid without a DrawingGrid, or Mesh). */
  struct Cell {
    /* Four-sided: its sides as Beziers, around it (corner k to k + 1), at rest and now. */
    bool quad = false;
    std::array<std::array<float2, 4>, 4> rest_sides{}, now_sides{};
    /* Its outline, sample for sample, at rest and now. */
    std::vector<float2> rest, now;
    float size = 1.0f;
    /* One of its sides is on the mesh's outline. */
    bool outer = false;
  };
  std::vector<Cell> cells;
  /* A grid's or four-sided cells' outline as its sides (the cells' sides no other cell shares), each a Bezier at
   * rest and now: what points outside follow, each the side's way near it. */
  std::vector<std::array<std::array<float2, 4>, 2>> edge_sides;
  /* A grid's or network's outline (and its holes' outlines), sample for sample at rest and now: what the points
   * outside it follow. */
  std::vector<std::vector<float2>> outline_rest, outline_now;
  /* A Mesh Deform's lines (an open line, or a mesh's dangling lines): each line its segments in order, each a Bezier
   * at rest and now (a line runs on through points where only two of its segments meet). */
  struct WireEdge {
    std::array<float2, 4> rest{}, now{};
    int from = 0, to = 0;
  };
  /* Each line also as dense samples of it now, on past both ends straight along the way each end runs (as far again
   * as the line is long), and how far along them each is: what the points far from it follow it smoothed by. */
  struct WireLine {
    std::vector<WireEdge> edges;
    std::vector<float2> dense;
    std::vector<float> at;
    /* The same at rest (as many samples, the same way), for the far points' smoothing to be measured from. */
    std::vector<float2> dense_rest;
    std::vector<float> at_rest;
    /* How many samples come before the line's start, and how far apart they are on average. */
    int before = 0;
    float spacing = 1.0f;
  };
  std::vector<WireLine> wires;
  /* A cage's Method (Cage Deform): how what is in it follows. */
  CageMethod cage_method = CageMethod::MeanValue;
  /* Envelope: each side (the outline from one of its points to the next), sample for sample at rest and now, how far
   * along it each sample is, and the way it runs at each sample (rest and now). */
  struct EnvelopeSide {
    std::vector<float2> rest, now, rest_way, now_way;
    std::vector<float> rest_at, now_at;
    /* On a mesh, its bounds at rest: most sides are too far off to measure. And how much say what is left of the way
     * between its ends gives it. */
    float2 lo, hi;
    float stays = 1.0f;
    /* With a Range: how far each sample has moved from where the cage as a whole puts it. */
    std::vector<float> moved;
  };
  std::vector<EnvelopeSide> envelope_sides;
  /* How sharply the nearest side leads (the power its nearness is raised to), from Blend; and how far from the
   * outline a side's say is down to half, the cage as a whole (Rigid's) taking the rest. */
  float envelope_power = 8.0f;
  float envelope_reach = 1.0f;
  /* On a cage with dents, how far each side really is from a place, by the lattice (RigidLattice::detours). */
  std::shared_ptr<const RigidLattice> envelope_walks;
  /* Range (Modifier::deform_range; 1, the whole cage), and how far that reaches from a side that has moved. */
  float envelope_range = 1.0f;
  float envelope_far = 1.0f;
  /* Mesh Deform's Envelope (mesh_envelope): these are a mesh's outline, and the mesh inside its outer cells at rest, its
   * lines and the nodes off the outline, where the sides' say has faded out. */
  bool envelope_mesh = false;
  struct RingLine {
    std::vector<float2> samples;
    float2 lo, hi;
  };
  std::vector<RingLine> envelope_ring;
  /* Mean Value and Rigid: how each corner of the outline turns (a complex number of length 1), for what lies past it,
   * which keeps its distance from the outline. */
  std::vector<float2> cage_turn;
  float cage_band = 0.0f;
  /* Rigid: how far the inside moves from where mean value coordinates put it, on a lattice over the cage (`step`
   * apart from `origin`, `nx` by `ny`), fading out past the outline. */
  std::vector<float2> lattice;
  float2 lattice_origin = float2(0.0f, 0.0f);
  float lattice_step = 1.0f;
  int lattice_nx = 0, lattice_ny = 0;
  /* Where the cage has a narrow gap: the lattice cut along it, and the nodes each side of it reads (rigid_offset). */
  std::shared_ptr<const RigidLattice> lattice_cut;
  std::vector<std::array<float2, 16>> lattice_stencils;
  /* How the cage has moved as a whole, taken by most of its outline (cage_whole_fit): `whole_from` goes to `whole_to`,
   * and what is around it turns and scales by `whole_turn`, a complex number. */
  float2 whole_from = float2(0.0f, 0.0f), whole_to = float2(0.0f, 0.0f), whole_turn = float2(1.0f, 0.0f);
  /* Mesh Deform's Points: the mesh's lines and what is worked out from them at rest (PointBones), each line now in the
   * same order, and how sharply the nearest line leads (from Blend). */
  std::shared_ptr<const PointBones> points;
  std::vector<std::array<float2, 4>> points_now;
  float points_power = 1.0f;
};

/* A cell's Coons patch from its four sides (corner k to k + 1 around it), as a grid's cell is: the bottom side
 * runs corner 0 to 1, the right 1 to 2, the top 3 to 2, the left 0 to 3. */
static float2 cell_patch(const std::array<std::array<float2, 4>, 4> &sides, float2 st)
{
  const auto at = [](const std::array<float2, 4> &c, float t) {
    const float u = 1.0f - t;
    return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
  };
  /* Each side fading out smoothly toward the one opposite (as GridSurface's patch). */
  const float s = st.x, t = st.y;
  const float fs = s * s * (3.0f - 2.0f * s), ft = t * t * (3.0f - 2.0f * t);
  const float2 bottom = at(sides[0], s), right = at(sides[1], t), top = at(sides[2], 1.0f - s),
               left = at(sides[3], 1.0f - t);
  const float2 c0 = sides[0][0], c1 = sides[1][0], c2 = sides[2][0], c3 = sides[3][0];
  return bottom * (1.0f - ft) + top * ft + left * (1.0f - fs) + right * fs -
         (c0 * ((1.0f - fs) * (1.0f - ft)) + c1 * (fs * (1.0f - ft)) + c3 * ((1.0f - fs) * ft) + c2 * (fs * ft));
}

/* The patch at `st`; outside 0..1, on straight from its edge (as GridSurface::eval). */
/* The patch's derivatives along s and t at `st` (exact: it is cubic along its sides and blends them linearly). */
static void cell_patch_partials(const std::array<std::array<float2, 4>, 4> &sides, float2 st, float2 &r_s, float2 &r_t)
{
  const auto at = [](const std::array<float2, 4> &c, float t) {
    const float u = 1.0f - t;
    return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
  };
  const auto way = [](const std::array<float2, 4> &c, float t) {
    const float u = 1.0f - t;
    return (c[1] - c[0]) * (3.0f * u * u) + (c[2] - c[1]) * (6.0f * u * t) + (c[3] - c[2]) * (3.0f * t * t);
  };
  const float s = st.x, t = st.y;
  const float fs = s * s * (3.0f - 2.0f * s), ft = t * t * (3.0f - 2.0f * t);
  const float dfs = 6.0f * s * (1.0f - s), dft = 6.0f * t * (1.0f - t);
  const float2 c0 = sides[0][0], c1 = sides[1][0], c2 = sides[2][0], c3 = sides[3][0];
  r_s = way(sides[0], s) * (1.0f - ft) - way(sides[2], 1.0f - s) * ft +
        (at(sides[1], t) - at(sides[3], 1.0f - t) - (c1 - c0) * (1.0f - ft) - (c2 - c3) * ft) * dfs;
  r_t = way(sides[1], t) * fs - way(sides[3], 1.0f - t) * (1.0f - fs) +
        (at(sides[2], 1.0f - s) - at(sides[0], s) - (c3 - c0) * (1.0f - fs) - (c2 - c1) * fs) * dft;
}

static float2 cell_eval(const std::array<std::array<float2, 4>, 4> &sides, float2 st)
{
  const float2 in(std::clamp(st.x, 0.0f, 1.0f), std::clamp(st.y, 0.0f, 1.0f));
  const float2 at = cell_patch(sides, in);
  if (in == st) {
    return at;
  }
  const float h = 1e-3f;
  const float sx = in.x > 0.5f ? -h : h, sy = in.y > 0.5f ? -h : h;
  const float2 ds = (cell_patch(sides, {in.x + sx, in.y}) - at) / sx;
  const float2 dt = (cell_patch(sides, {in.x, in.y + sy}) - at) / sy;
  return at + ds * (st.x - in.x) + dt * (st.y - in.y);
}

/* Where `p` is in a four-sided cell (Newton's method on its patch, as GridSurface::locate). */
static bool cell_locate(const std::array<std::array<float2, 4>, 4> &sides, float size, float2 p, float2 &r_st)
{
  /* As GridSurface::locate: no nearer than these numbers can tell. */
  const float near_enough = std::max(size * 1e-6f, (std::fabs(p.x) + std::fabs(p.y) + size) * 4e-7f);
  float2 st(0.5f, 0.5f);
  for (int it = 0; it < 40; it++) {
    const float2 f = cell_eval(sides, st) - p;
    if (length(f) < near_enough) {
      r_st = st;
      return true;
    }
    const float h = 1e-3f;
    const float2 ds = (cell_eval(sides, {st.x + h, st.y}) - cell_eval(sides, {st.x - h, st.y})) / (2.0f * h);
    const float2 dt = (cell_eval(sides, {st.x, st.y + h}) - cell_eval(sides, {st.x, st.y - h})) / (2.0f * h);
    const float det = ds.x * dt.y - dt.x * ds.y;
    if (std::fabs(det) < 1e-12f) {
      return false;
    }
    float2 step((dt.y * f.x - dt.x * f.y) / det, (-ds.y * f.x + ds.x * f.y) / det);
    const float reach = length(step);
    if (reach > 2.0f) {
      step = step * (2.0f / reach);
    }
    st = st - step;
  }
  return false;
}

/* Samples per segment of a wire's dense samples. */
static constexpr int WIRE_STEPS = 64;

static DeformShape::WireLine wire_line(std::vector<DeformShape::WireEdge> edges)
{
  /* Segments with next to no length at rest (points stacked on one another) are left out: they have no way to
   * turn what lies by them with (it fell back to an arbitrary one, so a drawing beside them buckled once the
   * points came apart), and nothing lies nearer them than the segments either side. */
  const auto extent = [](const std::array<float2, 4> &c) {
    return std::max({length(c[1] - c[0]), length(c[2] - c[0]), length(c[3] - c[0])});
  };
  float span = 0.0f;
  for (const DeformShape::WireEdge &e : edges) {
    span += extent(e.rest);
  }
  if (edges.size() > 1) {
    std::vector<DeformShape::WireEdge> kept;
    for (const DeformShape::WireEdge &e : edges) {
      if (extent(e.rest) > span * 1e-3f) {
        kept.push_back(e);
      }
    }
    if (!kept.empty()) {
      edges = std::move(kept);
    }
  }
  const auto at3 = [](const std::array<float2, 4> &c, float t) {
    const float u = 1.0f - t;
    return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
  };
  const auto way3 = [&](const std::array<float2, 4> &c, float t) {
    float2 d = at3(c, std::min(t + 1e-3f, 1.0f)) - at3(c, std::max(t - 1e-3f, 0.0f));
    const float len = length(d);
    return len > 1e-12f ? d / len : float2(1.0f, 0.0f);
  };
  DeformShape::WireLine line;
  /* The line now or at rest, densely, carried on straight past both ends (`before` samples, `spacing` apart). */
  const auto densely = [&](bool now, int before, std::vector<float2> &r_dense, std::vector<float> &r_at) {
    std::vector<float2> middle;
    for (const DeformShape::WireEdge &e : edges) {
      for (int k = 0; k < WIRE_STEPS; k++) {
        middle.push_back(at3(now ? e.now : e.rest, float(k) / float(WIRE_STEPS)));
      }
    }
    middle.push_back(now ? edges.back().now[3] : edges.back().rest[3]);
    float total = 0.0f;
    for (size_t i = 1; i < middle.size(); i++) {
      total += length(middle[i] - middle[i - 1]);
    }
    const float spacing = std::max(total / float(middle.size() - 1), 1e-6f);
    if (before < 0) {
      line.spacing = spacing;
      line.before = before = std::min(int(total / spacing) + 2, 4096);
    }
    const float2 start_way = way3(now ? edges.front().now : edges.front().rest, 0.0f);
    const float2 end_way = way3(now ? edges.back().now : edges.back().rest, 1.0f);
    for (int k = before; k >= 1; k--) {
      r_dense.push_back(middle.front() - start_way * (spacing * float(k)));
    }
    r_dense.insert(r_dense.end(), middle.begin(), middle.end());
    for (int k = 1; k <= before; k++) {
      r_dense.push_back(middle.back() + end_way * (spacing * float(k)));
    }
    r_at.assign(r_dense.size(), 0.0f);
    for (size_t i = 1; i < r_dense.size(); i++) {
      r_at[i] = r_at[i - 1] + length(r_dense[i] - r_dense[i - 1]);
    }
  };
  densely(true, -1, line.dense, line.at);
  densely(false, line.before, line.dense_rest, line.at_rest);
  line.edges = std::move(edges);
  return line;
}

/* The cells a network of segments encloses: each face of it, found by walking round with the face on the left
 * (at each point, on along the next segment clockwise from the one arrived by), those going round
 * anticlockwise; the outside, going round the other way, is left out. False when it encloses none. */
static bool network_cells(const VectorDrawing &rest, const Shape &rest_shape, const VectorDrawing *bent,
                          std::vector<DeformShape::Cell> &r_cells, DeformShape &r_outlines)
{
  struct Half {
    int from = 0, to = 0;
    std::array<float2, 4> rest_ctrl{}, now_ctrl{};
  };
  std::vector<Half> halves;
  for (const ShapeEdge &e : rest_shape.edges) {
    if (e.curve < 0 || e.curve >= int(rest.curves.size()) || (bent && e.curve >= int(bent->curves.size()))) {
      return false;
    }
    Half h;
    curve_segment_points(rest.curves[size_t(e.curve)], e.segment, &h.from, &h.to);
    if (h.from == h.to) {
      continue;
    }
    curve_segment_bezier(rest, rest.curves[size_t(e.curve)], e.segment, h.rest_ctrl.data());
    if (bent) {
      curve_segment_bezier(*bent, bent->curves[size_t(e.curve)], e.segment, h.now_ctrl.data());
    }
    Half back = h;
    std::swap(back.from, back.to);
    back.rest_ctrl = {h.rest_ctrl[3], h.rest_ctrl[2], h.rest_ctrl[1], h.rest_ctrl[0]};
    back.now_ctrl = {h.now_ctrl[3], h.now_ctrl[2], h.now_ctrl[1], h.now_ctrl[0]};
    halves.push_back(h);
    halves.push_back(back);
  }
  if (halves.empty()) {
    return false;
  }
  /* Which way each half leaves its point. */
  const auto leaving = [](const std::array<float2, 4> &c) {
    for (int k = 1; k < 4; k++) {
      if (length(c[size_t(k)] - c[0]) > 1e-9f) {
        return std::atan2(c[size_t(k)].y - c[0].y, c[size_t(k)].x - c[0].x);
      }
    }
    return 0.0f;
  };
  std::map<int, std::vector<int>> out;
  for (int i = 0; i < int(halves.size()); i++) {
    out[halves[size_t(i)].from].push_back(i);
  }
  for (auto &[point, list] : out) {
    std::sort(list.begin(), list.end(), [&](int x, int y) {
      return leaving(halves[size_t(x)].rest_ctrl) < leaving(halves[size_t(y)].rest_ctrl);
    });
  }
  const auto next = [&](int h) {
    const std::vector<int> &list = out[halves[size_t(h)].to];
    const int twin = h ^ 1;
    const auto it = std::find(list.begin(), list.end(), twin);
    const int k = int(it - list.begin());
    return list[size_t((k - 1 + int(list.size())) % int(list.size()))];
  };
  std::vector<bool> used(halves.size(), false);
  /* Which walk each half was on: a segment walked both ways in one walk has the same face on both sides, a line
   * left dangling (a wire), not a side between cells. */
  std::vector<int> walk_of(halves.size(), -1);
  for (int start = 0; start < int(halves.size()); start++) {
    if (used[size_t(start)]) {
      continue;
    }
    std::vector<int> loop;
    int h = start;
    while (!used[size_t(h)] && loop.size() <= halves.size()) {
      used[size_t(h)] = true;
      walk_of[size_t(h)] = start;
      loop.push_back(h);
      h = next(h);
    }
    if (h != start || loop.size() < 2) {
      continue;
    }
    DeformShape::Cell cell;
    for (const int k : loop) {
      for (int j = 0; j < OUTLINE_STEPS; j++) {
        const float t = float(j) / float(OUTLINE_STEPS);
        const auto at = [t](const std::array<float2, 4> &c) {
          const float u = 1.0f - t;
          return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
        };
        cell.rest.push_back(at(halves[size_t(k)].rest_ctrl));
        cell.now.push_back(at(bent ? halves[size_t(k)].now_ctrl : halves[size_t(k)].rest_ctrl));
      }
    }
    float area = 0.0f;
    float2 lo = cell.rest[0], hi = cell.rest[0];
    for (size_t i = 0; i < cell.rest.size(); i++) {
      area += cross(cell.rest[i], cell.rest[(i + 1) % cell.rest.size()]);
      lo = float2(std::min(lo.x, cell.rest[i].x), std::min(lo.y, cell.rest[i].y));
      hi = float2(std::max(hi.x, cell.rest[i].x), std::max(hi.y, cell.rest[i].y));
    }
    /* A walk out along loose lines and back encloses nothing (whatever rounding leaves of its area): every segment
     * of it walked both ways. Neither a cell nor an outline; its segments are wires. */
    if (std::all_of(loop.begin(), loop.end(), [&](int k) { return walk_of[size_t(k ^ 1)] == start; })) {
      continue;
    }
    const float least = 1e-6f * std::max((hi.x - lo.x) * (hi.x - lo.x) + (hi.y - lo.y) * (hi.y - lo.y), 1e-12f);
    if (area < -least) {
      /* Round the outside (or a hole) the other way: an outline. */
      r_outlines.outline_rest.push_back(cell.rest);
      r_outlines.outline_now.push_back(cell.now);
      continue;
    }
    if (area <= least) {
      continue;
    }
    cell.size = std::max(std::max(hi.x - lo.x, hi.y - lo.y), 1e-6f);
    if (loop.size() == 4) {
      cell.quad = true;
      for (int k = 0; k < 4; k++) {
        cell.rest_sides[size_t(k)] = halves[size_t(loop[size_t(k)])].rest_ctrl;
        cell.now_sides[size_t(k)] = bent ? halves[size_t(loop[size_t(k)])].now_ctrl : cell.rest_sides[size_t(k)];
      }
    }
    r_cells.push_back(std::move(cell));
  }
  std::vector<DeformShape::WireEdge> loose;
  for (size_t h = 0; h + 1 < halves.size(); h += 2) {
    if (walk_of[h] == walk_of[h + 1]) {
      loose.push_back({halves[h].rest_ctrl, bent ? halves[h].now_ctrl : halves[h].rest_ctrl, halves[h].from,
                       halves[h].to});
    }
  }
  /* The loose segments joined into lines: on through each point where just two of them meet. */
  std::map<int, std::vector<int>> at_point;
  for (int i = 0; i < int(loose.size()); i++) {
    at_point[loose[size_t(i)].from].push_back(i);
    at_point[loose[size_t(i)].to].push_back(i);
  }
  std::vector<bool> taken(loose.size(), false);
  const auto reversed = [](DeformShape::WireEdge e) {
    std::swap(e.from, e.to);
    e.rest = {e.rest[3], e.rest[2], e.rest[1], e.rest[0]};
    e.now = {e.now[3], e.now[2], e.now[1], e.now[0]};
    return e;
  };
  /* Lines start at a point that isn't a two-way joint (an end or a branch); loops of loose segments anywhere. */
  for (const int pass : {0, 1}) {
    for (int i = 0; i < int(loose.size()); i++) {
      if (taken[size_t(i)]) {
        continue;
      }
      DeformShape::WireEdge e = loose[size_t(i)];
      if (pass == 0) {
        const bool from_joint = at_point[e.from].size() == 2, to_joint = at_point[e.to].size() == 2;
        if (from_joint && to_joint) {
          continue;
        }
        if (from_joint) {
          e = reversed(e);
        }
      }
      std::vector<DeformShape::WireEdge> line = {e};
      taken[size_t(i)] = true;
      for (;;) {
        const std::vector<int> &here = at_point[line.back().to];
        if (here.size() != 2) {
          break;
        }
        const int k = here[0] == i ? here[1] : here[0];
        if (taken[size_t(k)]) {
          break;
        }
        taken[size_t(k)] = true;
        DeformShape::WireEdge f = loose[size_t(k)];
        if (f.from != line.back().to) {
          f = reversed(f);
        }
        line.push_back(f);
        i = k;
      }
      r_outlines.wires.push_back(wire_line(std::move(line)));
    }
  }
  return !r_cells.empty() || !r_outlines.wires.empty();
}

static std::vector<float> lengths_along(const std::vector<float2> &v)
{
  std::vector<float> at(v.size(), 0.0f);
  for (size_t i = 1; i < v.size(); i++) {
    at[i] = at[i - 1] + length(v[i] - v[i - 1]);
  }
  return at;
}

/* The shape's space to the layer's, at rest or (`now`) as it is. A group's deform goes by way of the group as it stood at
 * setup, so what moves the group itself leaves the deform as it was and moves what is in it on top; when only the
 * shape's change of shape counts, its rest is where it now is. */
static Transform2D shape_to_layer(const Document &doc, const Layer &layer, const Modifier &m, uint64_t target, bool now)
{
  if (m.deform_from_group == 0) {
    return now ? layer_relative_transform(doc, layer.id, target) : layer_setup_relative_transform(doc, layer.id, target);
  }
  return layer_relative_transform(doc, layer.id, m.deform_from_group) *
         (now || m.deform_rest_posed ? layer_relative_to_setup(doc, m.deform_from_group, target)
                                     : layer_setup_relative_transform(doc, m.deform_from_group, target));
}

static bool shape_rest(const Document &doc, const Layer &layer, const Modifier &m, VectorDrawing &r_rest,
                       const Layer **r_target, bool render)
{
  const Layer *target = layer_find(doc, m.deform_layer);
  if (!target || target->id == layer.id || !target->has_drawing() || m.deform_shape == 0) {
    return false;
  }
  r_rest = layer_setup_drawing(doc, *target, render);
  if (m.deform_rest_posed) {
    layer_pose_drawing(doc, *target, r_rest);
  }
  /* From the groups the two share down (layer_relative_transform), as the shape now is. */
  transform_drawing(r_rest, shape_to_layer(doc, layer, m, target->id, false));
  *r_target = target;
  return drawing_shape_index(r_rest, m.deform_shape) >= 0;
}

static bool border_of_grid(const GridSurface &g, std::vector<float2> &r)
{
  r.clear();
  const int steps = 8;
  for (int i = 0; i < g.columns; i++) {
    for (int k = 0; k < steps; k++) {
      r.push_back(g.eval(i, 0, {float(k) / float(steps), 0.0f}));
    }
  }
  for (int j = 0; j < g.rows; j++) {
    for (int k = 0; k < steps; k++) {
      r.push_back(g.eval(g.columns - 1, j, {1.0f, float(k) / float(steps)}));
    }
  }
  for (int i = g.columns - 1; i >= 0; i--) {
    for (int k = 0; k < steps; k++) {
      r.push_back(g.eval(i, g.rows - 1, {1.0f - float(k) / float(steps), 1.0f}));
    }
  }
  for (int j = g.rows - 1; j >= 0; j--) {
    for (int k = 0; k < steps; k++) {
      r.push_back(g.eval(0, j, {0.0f, 1.0f - float(k) / float(steps)}));
    }
  }
  return r.size() >= 3;
}

/* `now` false: the rest alone (enough for kinds and Automatic weights). */
/* The outline's sides of a grid (its border segments) or of four-sided cells (the sides no other cell shares), at
 * rest and now: what the points outside follow (DeformShape::edge_sides). */
static void gather_edge_sides(DeformShape &r)
{
  r.edge_sides.clear();
  if (r.cells.empty()) {
    const GridSurface &a = r.rest_grid, &b = r.now_grid.across.empty() ? r.rest_grid : r.now_grid;
    const int cols = a.columns, rows = a.rows;
    if (cols <= 0 || rows <= 0 || b.columns != cols || b.rows != rows) {
      return;
    }
    for (int c = 0; c < cols; c++) {
      r.edge_sides.push_back({a.across[size_t(c)], b.across[size_t(c)]});
      r.edge_sides.push_back({a.across[size_t(rows * cols + c)], b.across[size_t(rows * cols + c)]});
    }
    for (int row = 0; row < rows; row++) {
      r.edge_sides.push_back({a.up[size_t(row)], b.up[size_t(row)]});
      r.edge_sides.push_back({a.up[size_t(cols * rows + row)], b.up[size_t(cols * rows + row)]});
    }
    return;
  }
  if (!std::all_of(r.cells.begin(), r.cells.end(), [](const DeformShape::Cell &c) { return c.quad; })) {
    return;
  }
  const auto same_ends = [](const std::array<float2, 4> &x, const std::array<float2, 4> &y, float tol) {
    return (length(x[0] - y[0]) < tol && length(x[3] - y[3]) < tol) ||
           (length(x[0] - y[3]) < tol && length(x[3] - y[0]) < tol);
  };
  for (size_t i = 0; i < r.cells.size(); i++) {
    const DeformShape::Cell &c = r.cells[i];
    for (int k = 0; k < 4; k++) {
      const std::array<float2, 4> &side = c.rest_sides[size_t(k)];
      bool shared = false;
      for (size_t j = 0; j < r.cells.size() && !shared; j++) {
        for (int m = 0; m < 4 && j != i && !shared; m++) {
          shared = same_ends(side, r.cells[j].rest_sides[size_t(m)], c.size * 1e-5f);
        }
      }
      if (!shared) {
        r.edge_sides.push_back({side, c.now_sides[size_t(k)]});
        r.cells[i].outer = true;
      }
    }
  }
}

/* Blend 0 to 1: the nearest side leading as the cube of nearness to as nearness itself (past the cube, two sides
 * setting a point's distance from each at a closing corner pull it apart faster than the corner gives room, and
 * it folds). */
static float envelope_power_of(float blend)
{
  return 3.0f * std::pow(1.0f / 3.0f, std::clamp(blend, 0.0f, 1.0f));
}

static void cage_method_build(DeformShape &r);
static void mesh_envelope_build(DeformShape &r, float blend);
static void points_build_grid(DeformShape &r, const Modifier &m);
static void points_build_network(DeformShape &r, const Modifier &m, const VectorDrawing &rest_drawing,
                                 const Shape &rest_shape, const VectorDrawing &bent);

/* The shape at rest and (`now`) as it is, in the layer's space: what a deform is built from. */
static bool deform_shape_inputs(const Document &doc, const Layer &layer, const Modifier &m, bool now, bool render,
                                VectorDrawing &r_rest, VectorDrawing &r_bent)
{
  const Layer *target = nullptr;
  if (!shape_rest(doc, layer, m, r_rest, &target, render)) {
    return false;
  }
  if (now) {
    /* The layer itself again: bent by a reference of itself, whose Match Original bends it the same way. */
    if (std::find(g_deforming.begin(), g_deforming.end(), target->id) != g_deforming.end() ||
        std::find(g_deforming.begin(), g_deforming.end(), layer.id) != g_deforming.end())
    {
      return false;
    }
    /* The shape as it draws, whoever is asking (the edit wire too, or a line and what follows it part ways). When it
     * is bent by a deform of its own, that deform's Precision adds points: a line is followed by distance along it and
     * takes them, but a cage's or a mesh's shape is read sample for sample against its rest, so it gets its own points
     * only, each segment bent as near as one Bezier can be. */
    const int wire = g_wire;
    const bool read = g_shape_read;
    g_wire = m.type == ModifierType::CurveDeform ? 0 : 1;
    g_shape_read = m.type != ModifierType::CurveDeform;
    g_deforming.push_back(layer.id);
    {
      const GroupDeformReading reading(layer.id);
      r_bent = layer_evaluated_drawing(doc, *target, render);
    }
    g_deforming.pop_back();
    g_wire = wire;
    g_shape_read = read;
    /* From the groups the two share down: a shared group moving (the character walked, its top group scaled) doesn't
     * change the shape bent by, so its build is kept. */
    transform_drawing(r_bent, shape_to_layer(doc, layer, m, target->id, true));
    if (drawing_shape_index(r_bent, m.deform_shape) < 0) {
      return false;
    }
  }
  return true;
}

/* The point at place `i` of a chain of `shape`'s edges: where edge `i` starts, or at the end where the last one ends. */
static int chain_point(const VectorDrawing &drawing, const std::vector<OrientedEdge> &chain, size_t i)
{
  const OrientedEdge &o = chain[std::min(i, chain.size() - 1)];
  int from, to;
  curve_segment_points(drawing.curves[size_t(o.edge.curve)], o.edge.segment, &from, &to);
  return (i < chain.size()) != o.reversed ? from : to;
}

/* The stretch of `shape` from point `from` to point `to` (ids): round a closed shape the way its chain runs, so the two
 * swapped are the other side; on an open one between them. False when it has no such points. */
static bool shape_part(const VectorDrawing &drawing, const Shape &shape, uint64_t from, uint64_t to, Shape &r_part)
{
  std::vector<OrientedEdge> chain;
  bool closed = false;
  if (from == to || !shape_chain(drawing, shape, chain, &closed)) {
    return false;
  }
  const size_t n = chain.size();
  size_t a = n + 1, b = n + 1;
  for (size_t i = 0; i < n + (closed ? 0 : 1); i++) {
    const uint64_t id = drawing.points[size_t(chain_point(drawing, chain, i))].id;
    a = id == from ? i : a;
    b = id == to ? i : b;
  }
  if (a > n || b > n) {
    return false;
  }
  if (!closed && a > b) {
    std::swap(a, b);
  }
  r_part.edges.clear();
  for (size_t i = a; i != b; i = closed ? (i + 1) % n : i + 1) {
    r_part.edges.push_back(chain[i].edge);
  }
  return true;
}

static bool deform_shape_make(const Modifier &m, bool now, const VectorDrawing &rest, const VectorDrawing &bent,
                              DeformShape &r)
{
  /* Follow Deform takes whatever the shape is: as the deform that fits it. */
  if (m.type == ModifierType::FollowDeform) {
    Modifier as = m;
    for (const ModifierType type : {ModifierType::CurveDeform, ModifierType::CageDeform, ModifierType::MeshDeform}) {
      as.type = type;
      DeformShape tried;
      if (deform_shape_make(as, now, rest, bent, tried)) {
        r = std::move(tried);
        return true;
      }
    }
    return false;
  }
  const Shape &rest_shape = rest.shapes[size_t(drawing_shape_index(rest, m.deform_shape))];
  const Shape *bent_shape = now ? &bent.shapes[size_t(drawing_shape_index(bent, m.deform_shape))] : nullptr;
  /* Each deform takes its own shapes: Mesh Deform a grid or a network of cells (or a closed four-sided shape, as
   * one cell), Cage Deform one closed shape, Curve Deform one open line. */
  const bool mesh = m.type == ModifierType::MeshDeform;
  if (const DrawingGrid *grid = shape_grid(rest, rest_shape)) {
    if (!mesh || !r.rest_grid.build(rest, *grid) || (now && !r.now_grid.build(bent, *grid))) {
      return false;
    }
    if (m.deform_smooth) {
      r.rest_grid.smooth();
      if (now) {
        r.now_grid.smooth();
      }
    }
    r.kind = DeformKind::Grid;
    std::vector<float2> outline_now;
    if (!border_of_grid(r.rest_grid, r.border) || !border_of_grid(now ? r.now_grid : r.rest_grid, outline_now)) {
      return false;
    }
    r.outline_rest = {r.border};
    r.outline_now = {outline_now};
    gather_edge_sides(r);
    if (now && m.deform_mesh_method == MeshMethod::Envelope) {
      mesh_envelope_build(r, m.deform_blend);
    }
    if (now && m.deform_mesh_method == MeshMethod::Points) {
      points_build_grid(r, m);
    }
    return true;
  }
  /* A Part: one stretch of the shape, followed as an open line of its own. */
  Shape rest_part, bent_part;
  const bool part = m.type == ModifierType::CurveDeform && m.deform_part_from != 0;
  if (part && (!shape_part(rest, rest_shape, m.deform_part_from, m.deform_part_to, rest_part) ||
               (now && !shape_part(bent, *bent_shape, m.deform_part_from, m.deform_part_to, bent_part))))
  {
    return false;
  }
  const Shape &line = part ? rest_part : rest_shape;
  const Shape *bent_line = part ? &bent_part : bent_shape;
  /* Branching (more than two of its segments meeting at a point): a network, never one line or loop. */
  std::map<int, int> meeting;
  bool branches = false;
  for (const ShapeEdge &e : line.edges) {
    if (e.curve >= 0 && e.curve < int(rest.curves.size())) {
      int from, to;
      curve_segment_points(rest.curves[size_t(e.curve)], e.segment, &from, &to);
      branches = ++meeting[from] > 2 || branches;
      branches = ++meeting[to] > 2 || branches;
    }
  }
  bool closed = false;
  const bool one_run = !branches && shape_outline(rest, line, OUTLINE_STEPS, r.rest, nullptr, &closed);
  if (!one_run || (mesh && closed && rest_shape.edges.size() == 4) || (mesh && !closed)) {
    /* A network drawn by hand (or a closed four-sided shape, one cell): its cells; a Grid when every one has four
     * sides. */
    r.rest.clear();
    if (!mesh || !network_cells(rest, rest_shape, now ? &bent : nullptr, r.cells, r)) {
      return false;
    }
    r.kind = r.cells.empty() ? DeformKind::Wire
             : std::all_of(r.cells.begin(), r.cells.end(), [](const DeformShape::Cell &c) { return c.quad; })
                 ? DeformKind::Grid
                 : DeformKind::Mesh;
    gather_edge_sides(r);
    if (now && m.deform_mesh_method == MeshMethod::Envelope) {
      mesh_envelope_build(r, m.deform_blend);
    }
    if (now && m.deform_mesh_method == MeshMethod::Points) {
      points_build_network(r, m, rest, rest_shape, bent);
    }
    return true;
  }
  if (m.type != (closed ? ModifierType::CageDeform : ModifierType::CurveDeform)) {
    return false;
  }
  /* A line is followed more finely than a cage (its way turns what lies far off it, which a coarse one would turn by
   * small corners as it moved). */
  const int steps = closed ? OUTLINE_STEPS : LINE_STEPS;
  if (!closed) {
    shape_outline(rest, line, steps, r.rest, nullptr, &closed);
  }
  /* A closed outline ends where it starts: one sample is enough. */
  if (closed && r.rest.size() > 1 && length(r.rest.back() - r.rest.front()) < 1e-6f) {
    r.rest.pop_back();
  }
  if (now) {
    bool bent_closed = false;
    if (!shape_outline(bent, *bent_line, steps, r.now, nullptr, &bent_closed)) {
      return false;
    }
    if (closed && r.now.size() > r.rest.size()) {
      r.now.pop_back();
    }
    /* A line is measured by distance along it, at rest and now apart, so it may have more samples now. */
    if ((closed && r.now.size() != r.rest.size()) || bent_closed != closed) {
      return false;
    }
  }
  if (r.rest.size() < (closed ? 3u : 2u)) {
    return false;
  }
  r.kind = closed ? DeformKind::Cage : DeformKind::Line;
  r.border = r.rest;
  r.stretch = m.deform_stretch;
  if (closed && now) {
    r.cage_method = m.deform_cage_method;
    r.envelope_power = envelope_power_of(m.deform_blend);
    r.envelope_range = m.deform_range;
    cage_method_build(r);
  }
  if (!closed) {
    r.rest_at = lengths_along(r.rest);
    if (r.rest_at.back() < MIN_LINE_LENGTH) {
      return false;
    }
    /* A line collapsed to a point has no way it runs: the line at rest, that small, where it collapsed, so what follows
     * it stays there rather than jumping back to where it was drawn. */
    if (now && lengths_along(r.now).back() < 1e-6f) {
      const float2 at = r.now.front();
      const float shrink = 2e-6f / r.rest_at.back();
      r.now.resize(r.rest.size());
      for (size_t i = 0; i < r.rest.size(); i++) {
        r.now[i] = at + (r.rest[i] - r.rest.front()) * shrink;
      }
    }
    r.now_at = lengths_along(now ? r.now : r.rest);
  }
  return true;
}

static bool deform_shape_build(const Document &doc, const Layer &layer, const Modifier &m, bool now, bool render,
                               DeformShape &r)
{
  VectorDrawing rest, bent;
  return deform_shape_inputs(doc, layer, m, now, render, rest, bent) && deform_shape_make(m, now, rest, bent, r);
}

/* Builds kept: a deform's shape built again only when the shape (at rest or now) or the modifier has changed. A
 * Cage's Rigid and Envelope solve their lattice in the build, which takes several milliseconds, every frame drawn
 * otherwise (a frame held, the view panned, another layer edited). Each build has a serial: what was bent with it
 * can be kept too (vector_deform_apply). */
struct KeptBuild {
  Modifier m;
  VectorDrawing rest, bent;
  std::shared_ptr<const DeformShape> shape;
  uint64_t serial = 0;
};
/* A bent drawing kept: the drawing before, as it was before the bones, the weights and settings it was bent with,
 * and the build; and the drawing after. */
struct KeptBend {
  Modifier m;
  uint64_t build = 0;
  bool render = false;
  int wire = 0;
  VectorDrawing drawing, before_bones;
  PointGroup group;
  bool grouped = false;
  VectorDrawing result;
};
static std::mutex g_kept_mutex;
static std::map<std::tuple<uint64_t, std::string, bool>, KeptBuild> g_kept_builds;
static std::map<std::tuple<uint64_t, std::string, bool>, KeptBend> g_kept_bends;
static uint64_t g_kept_serial = 0;

static std::shared_ptr<const DeformShape> deform_shape_kept(const Document &doc, const Layer &layer, const Modifier &m,
                                                            bool render, uint64_t &r_serial)
{
  VectorDrawing rest, bent;
  if (!deform_shape_inputs(doc, layer, m, true, render, rest, bent)) {
    return nullptr;
  }
  const auto key = std::make_tuple(layer.id, m.name, render);
  {
    std::lock_guard lock(g_kept_mutex);
    auto it = g_kept_builds.find(key);
    if (it != g_kept_builds.end() && it->second.m == m && it->second.rest == rest && it->second.bent == bent) {
      r_serial = it->second.serial;
      return it->second.shape;
    }
  }
  auto shape = std::make_shared<DeformShape>();
  if (!deform_shape_make(m, true, rest, bent, *shape)) {
    return nullptr;
  }
  std::lock_guard lock(g_kept_mutex);
  if (g_kept_builds.size() > 512) {
    g_kept_builds.clear();
  }
  KeptBuild &kept = g_kept_builds[key];
  kept.m = m;
  kept.rest = std::move(rest);
  kept.bent = std::move(bent);
  kept.shape = shape;
  kept.serial = r_serial = ++g_kept_serial;
  return shape;
}

/* Mean value coordinates of `p` against polygon `v` (Floater; Hormann's signed form, so a point outside or
 * in a dent gets them too). A point on the outline gets its edge's two ends. */
static bool mean_value(const std::vector<float2> &v, float2 p, std::vector<float> &r_w)
{
  /* In double: far from the outline the angles are tiny and the weights nearly cancel, so float lost a thousandth of
   * the way there (a point far off moved a little with an outline that hadn't). */
  const size_t n = v.size();
  r_w.assign(n, 0.0f);
  /* Each thread's own, kept (this runs for every point bent, several times over). */
  thread_local std::vector<double> sx, sy, r, t, w;
  sx.resize(n);
  sy.resize(n);
  r.resize(n);
  t.resize(n);
  w.resize(n);
  double scale = 0.0;
  for (size_t i = 0; i < n; i++) {
    sx[i] = double(v[i].x) - p.x;
    sy[i] = double(v[i].y) - p.y;
    r[i] = std::sqrt(sx[i] * sx[i] + sy[i] * sy[i]);
    scale = std::max(scale, r[i]);
  }
  const double eps = std::max(scale, 1e-6) * 1e-6;
  for (size_t i = 0; i < n; i++) {
    if (r[i] < eps) {
      r_w[i] = 1.0f;
      return true;
    }
  }
  for (size_t i = 0; i < n; i++) {
    const size_t j = (i + 1) % n;
    const double a = sx[i] * sy[j] - sy[i] * sx[j], d = sx[i] * sx[j] + sy[i] * sy[j];
    if (std::fabs(a) <= eps * (r[i] + r[j]) && d < 0.0) {
      const double u = r[i] / (r[i] + r[j]);
      r_w[i] = float(1.0 - u);
      r_w[j] = float(u);
      return true;
    }
    /* tan of half the angle the edge spans at p, signed. */
    t[i] = std::fabs(a) > 1e-300 ? (r[i] * r[j] - d) / a : 0.0;
  }
  double sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    w[i] = (t[(i + n - 1) % n] + t[i]) / r[i];
    sum += w[i];
  }
  if (std::fabs(sum) < 1e-300) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    r_w[i] = float(w[i] / sum);
  }
  return true;
}

/* Where `p` is against a line: how far along it (measured along the line; below 0 or past its length beyond
 * its ends, on along the end pieces) and how far across it (left of the way it runs). */
struct LinePlace {
  float along = 0.0f, across = 0.0f;
};

static float2 line_normal(const std::vector<float2> &v, int i)
{
  const int n = int(v.size());
  float2 d(0.0f, 0.0f);
  if (i > 0) {
    d = d + normalize(v[size_t(i)] - v[size_t(i - 1)]);
  }
  if (i < n - 1) {
    d = d + normalize(v[size_t(i + 1)] - v[size_t(i)]);
  }
  const float len = length(d);
  d = len > 1e-9f ? d / len : float2(1.0f, 0.0f);
  return {-d.y, d.x};
}

/* The line's place and frame `s` along it (past an end, on along the end piece). */
static void line_frame_at(const std::vector<float2> &v, const std::vector<float> &at, float s, float2 &r_at,
                          float2 &r_tangent, float2 &r_normal)
{
  const int pieces = int(v.size()) - 1;
  int piece = int(std::upper_bound(at.begin(), at.end(), s) - at.begin()) - 1;
  piece = std::clamp(piece, 0, pieces - 1);
  const float len = at[size_t(piece + 1)] - at[size_t(piece)];
  const float t = len > 1e-12f ? (s - at[size_t(piece)]) / len : 0.0f;
  const float2 a = v[size_t(piece)], b = v[size_t(piece + 1)];
  r_at = a + (b - a) * t;
  const float tc = std::clamp(t, 0.0f, 1.0f);
  const float2 n = line_normal(v, piece) * (1.0f - tc) + line_normal(v, piece + 1) * tc;
  const float nl = length(n);
  r_normal = nl > 1e-9f ? n / nl : float2(0.0f, 1.0f);
  r_tangent = {r_normal.y, -r_normal.x};
}

static LinePlace line_locate(const std::vector<float2> &v, const std::vector<float> &at, float2 p)
{
  float best_dist = FLT_MAX, best_s = 0.0f;
  const int pieces = int(v.size()) - 1;
  for (int i = 0; i < pieces; i++) {
    const float2 a = v[size_t(i)], d = v[size_t(i + 1)] - a;
    const float dd = dot(d, d);
    float t = dd > 1e-20f ? dot(p - a, d) / dd : 0.0f;
    /* Past an end, on along its piece. */
    t = std::clamp(t, i == 0 ? -FLT_MAX : 0.0f, i == pieces - 1 ? FLT_MAX : 1.0f);
    const float dist = length(a + d * t - p);
    if (dist < best_dist) {
      best_dist = dist;
      best_s = at[size_t(i)] + t * std::sqrt(dd);
    }
  }
  float2 on, tangent, normal;
  line_frame_at(v, at, best_s, on, tangent, normal);
  return {best_s + dot(p - on, tangent), dot(p - on, normal)};
}

/* The point's near places on a line: on each part of it nearer the point than the parts either side (past an end, on
 * along the end piece), each as line_locate gives it, and how far the point is from each (`r_dists`). */
static std::vector<LinePlace> line_places(const std::vector<float2> &v, const std::vector<float> &at, float2 p,
                                          std::vector<float> &r_dists)
{
  const int pieces = int(v.size()) - 1;
  std::vector<float> dist(static_cast<size_t>(pieces)), ts(static_cast<size_t>(pieces));
  for (int i = 0; i < pieces; i++) {
    const float2 a = v[size_t(i)], d = v[size_t(i + 1)] - a;
    const float dd = dot(d, d);
    float t = dd > 1e-20f ? dot(p - a, d) / dd : 0.0f;
    t = std::clamp(t, i == 0 ? -FLT_MAX : 0.0f, i == pieces - 1 ? FLT_MAX : 1.0f);
    ts[size_t(i)] = t;
    dist[size_t(i)] = length(a + d * t - p);
  }
  std::vector<LinePlace> places;
  r_dists.clear();
  for (int i = 0; i < pieces; i++) {
    const bool below_prev = i == 0 || dist[size_t(i)] < dist[size_t(i - 1)];
    const bool below_next = i == pieces - 1 || dist[size_t(i)] <= dist[size_t(i + 1)];
    if (!below_prev || !below_next) {
      continue;
    }
    const float len = at[size_t(i + 1)] - at[size_t(i)];
    const float s = at[size_t(i)] + ts[size_t(i)] * len;
    float2 on, tangent, normal;
    line_frame_at(v, at, s, on, tangent, normal);
    places.push_back({s + dot(p - on, tangent), dot(p - on, normal)});
    r_dists.push_back(dist[size_t(i)]);
  }
  if (places.empty()) {
    places.push_back(line_locate(v, at, p));
    r_dists.push_back(0.0f);
  }
  return places;
}

/* A line's place and way `here` along it, averaged over a stretch `reach` either side (the nearer the more, a smooth
 * bump): the line as a point that far from it should follow it, its sharp bends rounded. False when the stretch
 * holds none of it. */
static bool line_smoothed(const std::vector<float2> &v, const std::vector<float> &at, float here, float reach,
                          float2 &r_at, float2 &r_way)
{
  if (reach <= 0.0f || v.size() < 2) {
    return false;
  }
  /* The samples within reach: from where `here` is, outward (the lengths only grow). */
  const size_t mid = size_t(std::max<ptrdiff_t>(
      0, std::upper_bound(at.begin(), at.end(), here) - at.begin() - 1));
  size_t lo = mid, hi = std::min(mid + 1, v.size() - 1);
  while (lo > 0 && at[lo] > here - reach) {
    lo--;
  }
  while (hi < v.size() - 1 && at[hi] < here + reach) {
    hi++;
  }
  float2 sum_at(0.0f, 0.0f), sum_way(0.0f, 0.0f);
  float weight = 0.0f;
  for (size_t k = lo; k < hi; k++) {
    const float m = (at[k] + at[k + 1]) * 0.5f;
    const float x = (m - here) / reach;
    if (std::fabs(x) >= 1.0f) {
      continue;
    }
    const float wk = (1.0f - x * x) * (1.0f - x * x) * (at[k + 1] - at[k]);
    sum_at = sum_at + (v[k] + v[k + 1]) * (0.5f * wk);
    sum_way = sum_way + (v[k + 1] - v[k]) * wk;
    weight += wk;
  }
  if (weight <= 1e-12f || length(sum_way) <= 1e-12f) {
    return false;
  }
  r_at = sum_at / weight;
  r_way = normalize(sum_way);
  return true;
}

/* How far across a line a point may sit on the inside of its bend there: no further than the bend's radius,
 * past which it would fold over to the far side. Up to half the radius it keeps its distance; beyond, it eases
 * toward the radius, keeping its order. As the line turns right back on itself neither side is the inside (the turn
 * goes from all the way left to all the way right as it passes a half turn), so this fades out by then: a line dragged
 * into a kink or a loop would otherwise throw what lies beside it across in one step. */
static float across_in_bend(const std::vector<float2> &v, const std::vector<float> &at, float s, float across)
{
  const float total = at.back();
  const float h = std::clamp(std::fabs(across) * 0.5f, total * 1e-3f, total * 0.25f);
  float2 p0, t0, n0, p1, t1, n1;
  line_frame_at(v, at, s - h, p0, t0, n0);
  line_frame_at(v, at, s + h, p1, t1, n1);
  const float turn = std::atan2(cross(t0, t1), dot(t0, t1));
  const float bend = turn / (2.0f * h);
  /* Turning left, the inside is on the left (positive across). */
  if (bend * across <= 0.0f || std::fabs(bend) < 1e-6f) {
    return across;
  }
  const float radius = 1.0f / std::fabs(bend), half = radius * 0.5f, d = std::fabs(across);
  if (d <= half) {
    return across;
  }
  const float kept = half + half * std::tanh((d - half) / half);
  const float pi = 3.14159265f;
  const float fade = smooth01((std::fabs(turn) - 0.75f * pi) / (0.25f * pi));
  return std::copysign(kept + (d - kept) * fade, across);
}

static float polyline_distance(const std::vector<float2> &v, bool closed, float2 p);
static bool inside(const std::vector<float2> &v, float2 p);
static bool mean_value(const std::vector<float2> &v, float2 p, std::vector<float> &r_w);

/* Whether `p` is outside a grid's or network's cells (in none of them). */
/* A four-sided cell's place for `p` (its patch's s and t) when `p` is in it or on its sides: judged by the side
 * curves themselves, not by the polygon of their samples, which a bulging side passes outside of (so a point
 * drawn on a side follows the patch, exactly onto the bent side, not the outline around the cells). */
static bool quad_cell_at(const DeformShape::Cell &c, float2 p, float2 &r_st)
{
  constexpr float edge = 1e-3f;
  if (!inside(c.rest, p) && polyline_distance(c.rest, true, p) > c.size * 0.05f) {
    return false;
  }
  float2 st;
  if (!cell_locate(c.rest_sides, c.size, p, st) || st.x < -edge || st.x > 1.0f + edge || st.y < -edge ||
      st.y > 1.0f + edge)
  {
    return false;
  }
  r_st = st;
  return true;
}

/* Whether `p` lies on side `k` of a four-sided cell, and where along it (`r_t`, the side's own parameter): the
 * nearest place on the side curve (Newton's method from a few starts), within a hair of the cell's size. On a side
 * a point and a handle along the side follow the bent side itself, which is exact, also at a corner where two
 * sides run on in line (there the patch's own directions are parallel and solving through them breaks down). */
/* The nearest place on Bezier `b` to `p` (`r_t`, Newton's method from a few starts) and how far it is. */
static float bezier_nearest(const std::array<float2, 4> &b, float2 p, float &r_t)
{
  const auto at = [&](float t) {
    const float u = 1.0f - t;
    return b[0] * (u * u * u) + b[1] * (3.0f * u * u * t) + b[2] * (3.0f * u * t * t) + b[3] * (t * t * t);
  };
  const auto way = [&](float t) {
    const float u = 1.0f - t;
    return (b[1] - b[0]) * (3.0f * u * u) + (b[2] - b[1]) * (6.0f * u * t) + (b[3] - b[2]) * (3.0f * t * t);
  };
  const auto bend = [&](float t) {
    return (b[2] - b[1] * 2.0f + b[0]) * (6.0f * (1.0f - t)) + (b[3] - b[2] * 2.0f + b[1]) * (6.0f * t);
  };
  float best = FLT_MAX, best_t = 0.0f;
  for (const float start : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
    float t = start;
    for (int it = 0; it < 12; it++) {
      const float2 off = at(t) - p, d1 = way(t);
      const float f = dot(off, d1), df = dot(d1, d1) + dot(off, bend(t));
      if (std::fabs(df) < 1e-20f) {
        break;
      }
      t = std::clamp(t - f / df, 0.0f, 1.0f);
    }
    const float dist = length(at(t) - p);
    if (dist < best) {
      best = dist;
      best_t = t;
    }
  }
  r_t = best_t;
  return best;
}

static bool cell_side_at(const DeformShape::Cell &c, int k, float2 p, float &r_t)
{
  return bezier_nearest(c.rest_sides[size_t(k)], p, r_t) <= c.size * 1e-5f;
}

/* A point outside a grid or four-sided cells: carried by each side of the outline as that side is at the place on
 * it nearest the point (where it went, turned as the side runs there, its distance kept; just past the side, leaving
 * it as the inside does), the sides blended by the
 * inverse eighth power of distance (as a line's branches are), so the nearest lead and a side far away doesn't
 * count (it carries a point far from it over a long lever, so even a little would swing it): moving one side moves
 * only what lies beyond it, and on the outline it is exact. */
static float2 mesh_inside_map(const DeformShape &d, float2 p, std::vector<float> &scratch, bool *r_outer = nullptr);

static float2 sides_follow(const DeformShape &d, float2 p, std::vector<float> &scratch)
{
  const auto way = [](const std::array<float2, 4> &b, float t) {
    const float u = 1.0f - t;
    return (b[1] - b[0]) * (3.0f * u * u) + (b[2] - b[1]) * (6.0f * u * t) + (b[3] - b[2]) * (3.0f * t * t);
  };
  const auto at = [](const std::array<float2, 4> &b, float t) {
    const float u = 1.0f - t;
    return b[0] * (u * u * u) + b[1] * (3.0f * u * u * t) + b[2] * (3.0f * u * t * t) + b[3] * (t * t * t);
  };
  double sum_x = 0.0, sum_y = 0.0, weight = 0.0;
  /* How near each side is, first: only those near enough to count look inside the mesh below, the dearest part. */
  thread_local std::vector<std::pair<float, float>> nearest_on;
  nearest_on.resize(d.edge_sides.size());
  float nearest = FLT_MAX;
  for (size_t s = 0; s < d.edge_sides.size(); s++) {
    float t;
    const float dist = bezier_nearest(d.edge_sides[s][0], p, t);
    nearest_on[s] = {dist, t};
    nearest = std::min(nearest, dist);
  }
  for (size_t s = 0; s < d.edge_sides.size(); s++) {
    const std::array<std::array<float2, 4>, 2> &side = d.edge_sides[s];
    const auto [dist, t] = nearest_on[s];
    /* A side with no handles has no way at its very ends: as it runs just along from there. Near a row of the mesh every
     * side's nearest place can be an end, and with none to go by the point would stay where it is. */
    const auto way_there = [&](const std::array<float2, 4> &b) {
      const float2 w = way(b, t);
      return length(w) >= 1e-10f ? w : way(b, std::clamp(t, 0.05f, 0.95f));
    };
    const float2 rest_way = way_there(side[0]), now_way = way_there(side[1]);
    const float rest_len = length(rest_way), now_len = length(now_way);
    if (rest_len < 1e-10f || now_len < 1e-10f) {
      continue;
    }
    /* The offset from the side, along its way and across it, then the same now: turned with the side, never
     * stretched (a side growing longer would otherwise push what lies past it out as much, and the drawing
     * would swell as the deformer moves). */
    const float2 off = p - at(side[0], t), rest_dir = rest_way / rest_len, now_dir = now_way / now_len;
    const float along = dot(off, rest_dir), across = cross(rest_dir, off);
    float2 q = at(side[1], t) + now_dir * along + float2(-now_dir.y, now_dir.x) * across;
    if (dist < 1e-7f) {
      return q;
    }
    /* Just past the side it slides along it as the inside does there (from two points just inside), easing over
     * to not at all as it gets further out: no crease where a line crosses the outline, and no swelling. */
    const float chord = std::max(length(side[0][3] - side[0][0]), 1e-6f);
    const float ease = chord * 0.25f, out = std::fabs(across);
    /* (Two and a half times as far off, a side has under a thousandth of the nearest's say.) */
    if (out < ease * 6.0f && dist < nearest * 2.5f) {
      const float outward = across >= 0.0f ? 1.0f : -1.0f;
      const float2 normal = float2(-rest_dir.y, rest_dir.x) * outward;
      const float step = chord * 2e-3f;
      const float2 base = at(side[0], t);
      const float2 in1 = mesh_inside_map(d, base - normal * step, scratch);
      const float2 in2 = mesh_inside_map(d, base - normal * (2.0f * step), scratch);
      const float2 leaving = (in1 - in2) / step;
      const float2 kept = float2(-now_dir.y, now_dir.x) * outward;
      /* Toward the side's ends it fades out: at a corner the inside's bend is ill-shaped (see mesh_inside_map),
       * and the sides on either side would each make something else of it. */
      const float mid = smooth01(std::min(t, 1.0f - t) / 0.2f);
      /* Only how the inside slides along the side (its shear): across it the point keeps its distance, so a
       * thin band drawn hugging the outline stays thin however the inside stretches across that side. */
      float2 slide = leaving - kept;
      slide = slide - kept * dot(slide, kept);
      q = q + slide * (out * std::exp(-out / ease) * mid);
    }
    const double d2 = double(dist) * dist, d4 = d2 * d2;
    const double w = 1.0 / (d4 * d4);
    sum_x += q.x * w;
    sum_y += q.y * w;
    weight += w;
  }
  return weight > 0.0 ? float2(float(sum_x / weight), float(sum_y / weight)) : p;
}

/* The way side `k` of a cell runs at `t` (its parameter's derivative). */
static float2 cell_side_way(const std::array<std::array<float2, 4>, 4> &sides, int k, float t)
{
  const std::array<float2, 4> &b = sides[size_t(k)];
  const float u = 1.0f - t;
  return (b[1] - b[0]) * (3.0f * u * u) + (b[2] - b[1]) * (6.0f * u * t) + (b[3] - b[2]) * (3.0f * t * t);
}

static float2 cell_side_point(const std::array<std::array<float2, 4>, 4> &sides, int k, float t)
{
  const std::array<float2, 4> &b = sides[size_t(k)];
  const float u = 1.0f - t;
  return b[0] * (u * u * u) + b[1] * (3.0f * u * u * t) + b[2] * (3.0f * u * t * t) + b[3] * (t * t * t);
}

static bool outside_mesh(const DeformShape &d, float2 p)
{
  if (d.outline_rest.empty()) {
    return false;
  }
  if (d.cells.empty()) {
    return !inside(d.border, p);
  }
  return std::none_of(d.cells.begin(), d.cells.end(), [&](const DeformShape::Cell &c) {
    float2 st;
    return c.quad ? quad_cell_at(c, p, st) : inside(c.rest, p);
  });
}

/* A point outside a grid or network follows its outline as a cage, from outside (mean value coordinates against
 * it, as Cage Deform's inside): smooth everywhere, exact on the outline (where the cells put it, so nothing jumps
 * at the edge), and far away it moves as the outline does as a whole. In a hole of the network, that hole's
 * outline; anywhere else, the outline round the outside. */
static float2 outline_follow(const DeformShape &d, float2 p, std::vector<float> &scratch)
{
  int loop = -1;
  float biggest = -1.0f;
  for (int l = 0; l < int(d.outline_rest.size()); l++) {
    const std::vector<float2> &v = d.outline_rest[size_t(l)];
    if (v.size() < 3 || d.outline_now[size_t(l)].size() != v.size()) {
      continue;
    }
    float area = 0.0f;
    for (size_t i = 0; i < v.size(); i++) {
      area += cross(v[i], v[(i + 1) % v.size()]);
    }
    area = std::fabs(area);
    if (inside(v, p) && d.outline_rest.size() > 1) {
      /* A hole holding it (the outside outline holds everything, so take the smallest that does). */
      if (loop < 0 || biggest < 0.0f || area < biggest) {
        loop = l;
        biggest = area;
      }
      continue;
    }
    if (biggest < 0.0f && (loop < 0 || area > -biggest)) {
      loop = l;
      biggest = -area;
    }
  }
  if (loop < 0 || !mean_value(d.outline_rest[size_t(loop)], p, scratch)) {
    return p;
  }
  float2 q(0.0f, 0.0f);
  for (size_t i = 0; i < d.outline_now[size_t(loop)].size(); i++) {
    q = q + d.outline_now[size_t(loop)][i] * scratch[i];
  }
  return q;
}

static float2 bezier3(const std::array<float2, 4> &c, float t)
{
  const float u = 1.0f - t;
  return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
}

/* Which way a Bezier runs at `t` (unit; from nearby points where its handle has no length). */
static float2 bezier3_way(const std::array<float2, 4> &c, float t)
{
  const float u = 1.0f - t;
  float2 d = (c[1] - c[0]) * (3.0f * u * u) + (c[2] - c[1]) * (6.0f * u * t) + (c[3] - c[2]) * (3.0f * t * t);
  if (length(d) < 1e-9f) {
    d = bezier3(c, std::min(t + 0.01f, 1.0f)) - bezier3(c, std::max(t - 0.01f, 0.0f));
  }
  const float len = length(d);
  return len > 1e-12f ? d / len : float2(1.0f, 0.0f);
}

/* Where a point pinned to a wire (a line of a Mesh Deform) goes: from its nearest place on the line at rest, the same
 * offset along and across the way the line runs there, now turned as it turns there. Past an end it rides that end.
 * A point far from the line follows the line smoothed over a stretch as long as it is far (its place and way averaged
 * there), so a sharp bend is a gentle one to it and offsets on the inside never cross; on the line it follows the
 * line exactly; past the ends the line carries on straight for it. `r_dist`: how far the point is from its place,
 * at rest. */
static float2 wire_pin_at(const DeformShape::WireLine &line, size_t best, float t, float2 p, float *r_dist)
{
  const DeformShape::WireEdge &w = line.edges[best];
  const float2 at = bezier3(w.rest, t), way = bezier3_way(w.rest, t), side(-way.y, way.x);
  const float2 off = p - at;
  const float along = dot(off, way);
  const float across = dot(off, side);
  *r_dist = length(off);
  float2 now_at = bezier3(w.now, t), now_way = bezier3_way(w.now, t);
  float2 rest_at = at, rest_way = way;
  const float extension = line.spacing * float(line.before);
  const float reach = std::min(std::fabs(across) * 1.5f, extension);
  if (reach > line.spacing * 0.1f && line.dense.size() >= 2 && line.dense_rest.size() == line.dense.size()) {
    const float place = float(line.before) + float(best * WIRE_STEPS) + t * float(WIRE_STEPS);
    const size_t i = std::min(size_t(place), line.dense.size() - 2);
    const float f = smooth01(reach / (3.0f * line.spacing));
    /* The line rounded over the stretch, now and at rest alike (the stretch as long, the samples the same ones), so
     * a far point moves by how the rounded line moved: a wire that hasn't moved moves nothing. */
    const auto round = [&](const std::vector<float2> &dense, const std::vector<float> &at_of, float2 &r_at,
                           float2 &r_way) {
      const float here = at_of[i] + (at_of[i + 1] - at_of[i]) * (place - float(i));
      const float stretch = at_of.back() / std::max(line.at.back(), 1e-12f);
      float2 round_at, round_way;
      if (line_smoothed(dense, at_of, here, reach * stretch, round_at, round_way)) {
        /* From the exact place to the rounded one as the stretch grows past a few samples (shorter, the samples
         * are too coarse to average). */
        r_at = r_at + (round_at - r_at) * f;
        r_way = normalize(r_way + (round_way - r_way) * f);
      }
    };
    round(line.dense, line.at, now_at, now_way);
    round(line.dense_rest, line.at_rest, rest_at, rest_way);
  }
  /* How far the point is from where the rounded line puts it at rest, carried in the line's frame: a wire that hasn't
   * moved moves nothing, one moved or turned whole carries everything exactly with it. */
  const float2 now_side(-now_way.y, now_way.x), rest_side(-rest_way.y, rest_way.x);
  const float2 left = p - (rest_at + rest_way * along + rest_side * across);
  return now_at + now_way * (along + dot(left, rest_way)) + now_side * (across + dot(left, rest_side));
}

/* A point pinned to a wire: at its nearest place on the line, or, where the line folds back or winds so that it has
 * more than one near place (a nearest place on each branch), the pins at each blended, the nearer the more (by the
 * eighth power of nearness), so a point between two branches follows both and nothing jumps from one to the other.
 * `r_dist`: how far it is from the nearest. */
static float2 wire_pin(const DeformShape::WireLine &wire, float2 p, float *r_dist)
{
  const std::vector<DeformShape::WireEdge> &line = wire.edges;
  /* The places along the line (samples, segment by segment) nearer than the samples either side of them. */
  struct Place {
    size_t edge;
    float t, dist;
  };
  std::vector<Place> samples;
  for (size_t k = 0; k < line.size(); k++) {
    for (int j = 0; j <= OUTLINE_STEPS; j++) {
      if (k > 0 && j == 0) {
        continue;
      }
      const float t = float(j) / float(OUTLINE_STEPS);
      samples.push_back({k, t, length(bezier3(line[k].rest, t) - p)});
    }
  }
  std::vector<Place> places;
  for (size_t i = 0; i < samples.size(); i++) {
    const bool below_prev = i == 0 || samples[i].dist < samples[i - 1].dist;
    const bool below_next = i + 1 == samples.size() || samples[i].dist <= samples[i + 1].dist;
    if (!below_prev || !below_next) {
      continue;
    }
    /* Refined on its segment (from either side of the sample), its distance with it. */
    Place best = samples[i];
    const auto refine = [&](size_t edge, float t0) {
      float t = t0;
      const std::array<float2, 4> &c = line[edge].rest;
      for (int it = 0; it < 6; it++) {
        const float u = 1.0f - t;
        const float2 b = bezier3(c, t) - p;
        const float2 d1 = (c[1] - c[0]) * (3.0f * u * u) + (c[2] - c[1]) * (6.0f * u * t) +
                          (c[3] - c[2]) * (3.0f * t * t);
        const float2 d2 = (c[2] - c[1] * 2.0f + c[0]) * (6.0f * u) + (c[3] - c[2] * 2.0f + c[1]) * (6.0f * t);
        const float f = dot(b, d1), df = dot(d1, d1) + dot(b, d2);
        if (std::fabs(df) < 1e-12f) {
          break;
        }
        t = std::clamp(t - f / df, 0.0f, 1.0f);
      }
      const float dist = length(bezier3(c, t) - p);
      if (dist < best.dist) {
        best = {edge, t, dist};
      }
    };
    refine(samples[i].edge, samples[i].t);
    if (samples[i].t >= 1.0f && samples[i].edge + 1 < line.size()) {
      refine(samples[i].edge + 1, 0.0f);
    }
    places.push_back(best);
  }
  if (places.empty()) {
    places.push_back(samples.front());
  }
  float nearest = FLT_MAX;
  for (const Place &place : places) {
    nearest = std::min(nearest, place.dist);
  }
  *r_dist = nearest;
  float2 sum(0.0f, 0.0f);
  float weight = 0.0f;
  for (const Place &place : places) {
    float dist = 0.0f;
    const float2 q = wire_pin_at(wire, place.edge, place.t, p, &dist);
    if (places.size() == 1) {
      return q;
    }
    if (dist < 1e-7f) {
      return q;
    }
    const float x = nearest / dist;
    const float w = x * x * x * x * x * x * x * x;
    sum = sum + q * w;
    weight += w;
  }
  return weight > 0.0f ? sum / weight : p;
}

/* A point outside a Mesh Deform's cells (or with no cells, anywhere) pinned to its wires, and following its outline
 * if it has one: each wire segment's pin and the outline's place, the nearer the more (by the fourth power of
 * nearness, so the nearest leads), exact on a wire and on the outline. */
static float2 wires_follow(const DeformShape &d, float2 p, std::vector<float> &scratch)
{
  float2 sum(0.0f, 0.0f);
  float weight = 0.0f;
  const auto add = [&](float2 q, float dist) {
    if (dist < 1e-7f) {
      sum = q * 1e30f;
      weight = 1e30f;
      return true;
    }
    const float w = 1.0f / (dist * dist * dist * dist);
    sum = sum + q * w;
    weight += w;
    return false;
  };
  for (const DeformShape::WireLine &line : d.wires) {
    float dist = 0.0f;
    const float2 q = wire_pin(line, p, &dist);
    if (add(q, dist)) {
      return q;
    }
  }
  if (!d.outline_rest.empty()) {
    float dist = FLT_MAX;
    for (const std::vector<float2> &v : d.outline_rest) {
      dist = std::min(dist, polyline_distance(v, true, p));
    }
    const float2 q = outline_follow(d, p, scratch);
    if (add(q, dist)) {
      return q;
    }
  }
  return weight > 0.0f ? sum / weight : p;
}

/* Where `p` (rest) goes as the shape is now. */
/* Where a point in a grid or in cells goes (or, past them, on from the nearest cell): each cell's patch, or its
 * outline as a cage where a cell isn't four-sided or its patch is badly shaped. */
/* `r_outer`: set when the cell it is in is on the mesh's outline. */
static float2 mesh_inside_map(const DeformShape &d, float2 p, std::vector<float> &scratch, bool *r_outer)
{
  if (d.cells.empty()) {
    const GridSurface &g = d.rest_grid;
    GridPlace place;
    if (!g.locate(p, place)) {
      return p;
    }
    if (r_outer) {
      *r_outer = place.column == 0 || place.row == 0 || place.column == g.columns - 1 || place.row == g.rows - 1;
    }
    return d.now_grid.eval(place.column, place.row, place.st);
  }
  /* The cell holding it, else the nearest (how far outside: a four-sided cell by its patch's place, any
   * other by distance, both as a share of the cell's size). */
  int best = -1;
  float best_out = FLT_MAX;
  float2 best_st(0.0f, 0.0f);
  for (int i = 0; i < int(d.cells.size()); i++) {
    const DeformShape::Cell &c = d.cells[size_t(i)];
    float out = FLT_MAX;
    float2 st(0.0f, 0.0f);
    if (c.quad) {
      if (cell_locate(c.rest_sides, c.size, p, st)) {
        out = std::max({0.0f, -st.x, st.x - 1.0f, -st.y, st.y - 1.0f});
      }
    }
    else {
      out = inside(c.rest, p) ? 0.0f : polyline_distance(c.rest, true, p) / c.size;
    }
    if (out < best_out) {
      best_out = out;
      best = i;
      best_st = st;
    }
  }
  if (best < 0) {
    return p;
  }
  const DeformShape::Cell &c = d.cells[size_t(best)];
  if (r_outer) {
    *r_outer = c.outer;
  }
  /* A cell as a small cage (mean value coordinates on its outline): smooth everywhere, exact on its outline. */
  const auto caged = [&](float2 &r_q) {
    if (!mean_value(c.rest, p, scratch)) {
      return false;
    }
    r_q = float2(0.0f, 0.0f);
    for (size_t i = 0; i < c.now.size(); i++) {
      r_q = r_q + c.now[i] * scratch[i];
    }
    return true;
  };
  if (c.quad) {
    const float2 patched = cell_eval(c.now_sides, best_st);
    /* At a corner where the two sides run on in line (a smooth shape cut into four sides), the patch's own
     * directions are nearly parallel: a sliver of the shape holds a large share of the patch, and bending it
     * there would blow small differences up into kinks. There it eases over to the cell as a cage, by how
     * square the patch's directions are (the sine of the angle between them). */
    float2 ds, dt;
    cell_patch_partials(c.rest_sides, {std::clamp(best_st.x, 0.0f, 1.0f), std::clamp(best_st.y, 0.0f, 1.0f)},
                        ds, dt);
    const float spread = length(ds) * length(dt);
    const float square = spread > 1e-20f ? std::fabs(cross(ds, dt)) / spread : 0.0f;
    const float patch_share = smooth01((square - 0.4f) / 0.55f);
    float2 cage;
    if (patch_share >= 1.0f || !caged(cage)) {
      return patched;
    }
    return cage + (patched - cage) * patch_share;
  }
  float2 q;
  return caged(q) ? q : p;
}

static float polyline_distance(const std::vector<float2> &v, bool closed, float2 p);
static bool inside(const std::vector<float2> &v, float2 p);

/* Past a cage's outline (rest outline `v`): each side carries the point from where its place on the side goes (`at`
 * at each corner, in between along the side), turned as the side is there (`turn`, a complex number at each corner), the nearer sides leading (inverse fourth power of distance), so it joins the inside
 * smoothly; `near` is the point's distance from the outline. */
static float2 outline_carry(const std::vector<float2> &v, const std::vector<float2> &at_corner,
                            const std::vector<float2> &turn, float2 p, float near, float band)
{
  const size_t n = v.size();
  const double floor = std::max(double(near), double(band) * 1e-3);
  double sx = 0.0, sy = 0.0, ws = 0.0;
  for (size_t j = 0; j < n; j++) {
    const size_t k = (j + 1) % n;
    const float2 e = v[k] - v[j];
    const float ee = dot(e, e);
    if (ee < 1e-20f) {
      continue;
    }
    const float t = std::clamp(dot(p - v[j], e) / ee, 0.0f, 1.0f);
    const float2 off = p - (v[j] + e * t);
    const float2 at = at_corner[j] * (1.0f - t) + at_corner[k] * t;
    /* The turn there, from its corners' (so it changes smoothly from side to side). */
    const float2 z = turn[j] * (1.0f - t) + turn[k] * t;
    const float2 q = at + float2(z.x * off.x - z.y * off.y, z.y * off.x + z.x * off.y);
    const double w = std::pow(floor / std::max(double(length(off)), floor), 4.0);
    sx += w * q.x;
    sy += w * q.y;
    ws += w;
  }
  return ws > 0.0 ? float2(float(sx / ws), float(sy / ws)) : p;
}

/* Envelope (Toon Boom Harmony's, without its tearing): every point goes where the cage as a whole puts it (Rigid's
 * way, which squashes least), then each side near it sets its distance from that side back to what it was at rest, so a line along the
 * outline keeps its width and art hugs the cage however a side bends or stretches. Each side's say goes by nearness
 * (each as the nearest side's distance over its own, raised to envelope_power, from Blend), and is full at the
 * side, less further off (reach / (reach + distance)), so deep inside the cage as a whole leads. Sides only set distances, never where a point slides
 * along them, so two sides meeting at a corner never pull a point two ways (which in Harmony's own, where each side
 * has the part of the drawing nearest it, tears the art there). */
static float2 rigid_offset(const DeformShape &d, float2 p);
static float2 cage_whole_at(const DeformShape &d, float2 p);
static bool envelope_walk_at(const DeformShape &d, float2 p, std::array<int, 4> &r_node, std::array<float, 4> &r_share);
static float envelope_detour(const DeformShape &d, size_t side, const std::array<int, 4> &node,
                             const std::array<float, 4> &share);

/* How far beyond the nearest place, as shares of its distance, other places on a side still have a say, and how far is
 * looked along it to see how straight it runs there. */
static constexpr float SIDE_SAY = 0.5f, SIDE_LOOK = 6.0f;

/* A place on a side, the way the side runs there, its share of the say, and how far along which piece of the side it is. */
struct SidePlace {
  float2 at, way;
  float say;
  size_t piece;
  float t;
};

/* The places on a side with a say at `p`: on each piece its nearest, sharing by how near each is, down to none at
 * half as far again as the nearest (and a fiftieth of the side's length, however close `p` is). A side bent into a hook
 * has two stretches about as near, each running its own way, and the nearest of all jumps between them as `p` moves;
 * each here fades instead, so nothing does. `r_straight`: how nearly the side runs one way around `p`, 1 where it does
 * and less the more it doubles back. Returns the distance from the nearest. */
static float side_places(const std::vector<float2> &v, const std::vector<float2> &ways, float length_of, float2 p,
                         std::vector<SidePlace> &r, float *r_straight)
{
  const size_t pieces = v.size() - 1;
  /* Each piece's nearest place, worked out once for both passes (called for every point bent, several times). */
  struct Near {
    float2 at;
    float t, dist;
  };
  thread_local std::vector<Near> near;
  near.resize(pieces);
  float closest = FLT_MAX;
  for (size_t k = 0; k < pieces; k++) {
    const float2 e = v[k + 1] - v[k];
    const float ee = dot(e, e);
    const float t = ee > 1e-20f ? std::clamp(dot(p - v[k], e) / ee, 0.0f, 1.0f) : 0.0f;
    const float2 at = v[k] + e * t;
    near[k] = {at, t, length(at - p)};
    closest = std::min(closest, near[k].dist);
  }
  const float least = std::max(length_of * 0.02f, 1e-9f);
  const float soft = std::max(closest * SIDE_SAY, least), look = std::max(closest * SIDE_LOOK, least);
  r.clear();
  float total = 0.0f, looked = 0.0f;
  float2 ways_near(0.0f, 0.0f);
  for (size_t k = 0; k < pieces; k++) {
    const float x = near[k].dist - closest;
    if (x >= look) {
      continue;
    }
    const float t = near[k].t;
    const float2 mixed = ways[k] * (1.0f - t) + ways[k + 1] * t;
    const float2 way = length(mixed) > 1e-12f ? normalize(mixed) : float2(1.0f, 0.0f);
    const float far = 1.0f - x / look;
    ways_near = ways_near + way * (far * far);
    looked += far * far;
    if (x < soft) {
      const float w = (1.0f - x / soft) * (1.0f - x / soft);
      r.push_back({near[k].at, way, w, k, t});
      total += w;
    }
  }
  for (SidePlace &place : r) {
    place.say /= total;
  }
  *r_straight = looked > 0.0f ? length(ways_near) / looked : 1.0f;
  /* The distance from it (as polyline_distance's). */
  return closest;
}

static float box_distance(float2 lo, float2 hi, float2 p)
{
  return length(float2(std::max({lo.x - p.x, p.x - hi.x, 0.0f}), std::max({lo.y - p.y, p.y - hi.y, 0.0f})));
}

/* How far `p` is from the mesh inside the outer cells. */
static float mesh_ring_distance(const DeformShape &d, float2 p)
{
  float best = FLT_MAX;
  for (const DeformShape::RingLine &line : d.envelope_ring) {
    if (box_distance(line.lo, line.hi, p) < best) {
      best = std::min(best, polyline_distance(line.samples, false, p));
    }
  }
  return best;
}

/* Side `s`'s share of the say at a place `dist` from it, `near` from the nearest side (Blend: the nearest over its own
 * distance, raised to envelope_power), before the sides' shares are added up. `walk_node`: the lattice's nodes around
 * the place, on a cage with dents (envelope_walk_at), null on one without. */
static double envelope_side_share(const DeformShape &d, size_t s, float dist, float near,
                                  const std::array<int, 4> *walk_node, const std::array<float, 4> &walk_share)
{
  const double floor = std::max(double(near), double(d.cage_band) * 1e-3);
  double w = std::pow(floor / std::max(double(dist), floor), double(d.envelope_power));
  /* Across a gap a side has no say, however close it stands. */
  if (walk_node) {
    const float detour = envelope_detour(d, s, *walk_node, walk_share);
    w *= 1.0f - smooth01(detour / (0.5f * dist + d.lattice_step));
  }
  return w;
}

/* The sides near `p` (where it is at rest) setting its distance from each back, from `q` (where it has been put). */
static float2 envelope_sides_apply(const DeformShape &d, float2 p, float2 q)
{
  const size_t n = d.envelope_sides.size();
  float near = FLT_MAX;
  thread_local std::vector<float> dist, across, within;
  thread_local std::vector<float2> fix;
  thread_local std::vector<SidePlace> places;
  dist.resize(n);
  across.resize(n);
  within.resize(n);
  fix.resize(n);
  /* A side is only near what it can get to through the cage (envelope_detour). */
  std::array<int, 4> walk_node;
  std::array<float, 4> walk_share;
  const bool walked = envelope_walk_at(d, p, walk_node, walk_share);
  const bool ranged = d.envelope_range < 1.0f && !d.envelope_mesh;
  float beyond_all = 1.0f;
  /* On a mesh a side has its say within reach of the outer cell here, so one edge cut into many sides counts as it does
   * in few, and the others needn't be measured: past how near some side surely is, and that reach again. */
  float ring = FLT_MAX, beyond = FLT_MAX;
  if (d.envelope_mesh) {
    float nearest = FLT_MAX;
    for (const DeformShape::EnvelopeSide &side : d.envelope_sides) {
      nearest = std::min(nearest, length(side.rest[side.rest.size() / 2] - p));
    }
    ring = mesh_ring_distance(d, p);
    beyond = nearest + (ring < FLT_MAX ? nearest + ring : d.envelope_reach);
  }
  for (size_t s = 0; s < n; s++) {
    const DeformShape::EnvelopeSide &side = d.envelope_sides[s];
    dist[s] = FLT_MAX;
    if (d.envelope_mesh && box_distance(side.lo, side.hi, p) > beyond) {
      continue;
    }
    /* Its distance from the side at rest and where the cage put it from the side now, each measured from the side's
     * near places, each by its say (side_places), so a place never jumps from one to another as the side moves, and
     * the point with it; at rest and now alike, so a side that hasn't moved sets nothing. */
    float straight = 1.0f;
    dist[s] = side_places(side.rest, side.rest_way, side.rest_at.back(), p, places, &straight);
    near = std::min(near, dist[s]);
    across[s] = 0.0f;
    float moved = 0.0f;
    for (const SidePlace &place : places) {
      across[s] += cross(place.way, p - place.at) * place.say;
      if (ranged) {
        moved += (side.moved[place.piece] * (1.0f - place.t) + side.moved[place.piece + 1] * place.t) * place.say;
      }
    }
    if (ranged) {
      /* Range: a side bends what lies within it of the places that have moved. A place moved further than half that
       * takes its reach with it: what lies near it has to get out of the way of what stays, or it folds through it. */
      const float far = std::max(d.envelope_far, moved * 2.0f);
      within[s] = 1.0f - smooth01(dist[s] / far);
      beyond_all *= 1.0f - smooth01(moved / (d.cage_band * 5.0f)) * within[s];
    }
  }
  if (ranged) {
    const float2 whole = cage_whole_at(d, p);
    q = whole + (q - whole) * (1.0f - beyond_all);
  }
  for (size_t s = 0; s < n; s++) {
    if (dist[s] == FLT_MAX) {
      continue;
    }
    const DeformShape::EnvelopeSide &side = d.envelope_sides[s];
    float straight = 1.0f;
    side_places(side.now, side.now_way, side.now_at.back(), q, places, &straight);
    /* Set back to it in full at the side and less further off, as reach / (reach + distance): the distance it ends
     * at then always grows with the distance it had, however hard the cage squeezes there, so nothing folds. The way
     * it is set back is their ways together, left as long as they come out: where a hook's stretches run opposite ways
     * they cancel, so the side says little there rather than turning sharply from one to the other. */
    float now_across = 0.0f;
    float2 push(0.0f, 0.0f);
    for (const SidePlace &place : places) {
      now_across += cross(place.way, q - place.at) * place.say;
      push = push + float2(-place.way.y, place.way.x) * place.say;
    }
    push = push * (across[s] - now_across);
    float keep = d.envelope_reach / (d.envelope_reach + dist[s]);
    /* A side doubling back around here can't be kept a distance from: what lies in a hook narrower than twice that
     * distance would have to cross itself. It lets go, and the cage as a whole carries the point. */
    keep *= smooth01((straight - 0.6f) / 0.35f);
    if (ranged) {
      keep *= within[s];
    }
    if (d.envelope_mesh) {
      /* A stretch can't fold, whatever sets it back. Where a cell squeezes what is in it, though, setting that back as it
       * fades out by the ring (below) can bring what lies further from the side nearer to it, and a curved cell squeezes
       * hardest in its middle: held back as the square of the squeeze, so nothing folds. A side shrinking to nothing has
       * no way it runs left to measure across, so it lets go first. */
      const float squeeze = std::fabs(across[s]) > 1e-9f ? std::clamp(now_across / across[s], 0.0f, 1.0f) : 1.0f;
      keep *= squeeze * squeeze * side.stays;
    }
    fix[s] = push * keep;
  }
  /* On a mesh, full at the outline and none by the ring, where the mesh places everything exactly as drawn. */
  float fade = 1.0f;
  if (ring < FLT_MAX) {
    const float along = near + ring > 1e-12f ? near / (near + ring) : 0.0f;
    fade = 1.0f - along * along * (3.0f - 2.0f * along);
  }
  const float reach = std::max(ring < FLT_MAX ? near + ring : d.envelope_reach, 1e-12f);
  double fx = 0.0, fy = 0.0, ws = 0.0;
  for (size_t s = 0; s < n; s++) {
    if (dist[s] == FLT_MAX) {
      continue;
    }
    double w = envelope_side_share(d, s, dist[s], near, walked ? &walk_node : nullptr, walk_share);
    if (d.envelope_mesh) {
      const float t = std::clamp(1.0f - (dist[s] - near) / reach, 0.0f, 1.0f);
      w *= t * t * (3.0f - 2.0f * t);
    }
    fx += w * fix[s].x;
    fy += w * fix[s].y;
    ws += w;
  }
  return ws > 0.0 ? q + float2(float(fx / ws), float(fy / ws)) * fade : q;
}

/* Where the cage moving as a whole puts `p`. */
static float2 cage_whole_at(const DeformShape &d, float2 p)
{
  const float2 e = p - d.whole_from, t = d.whole_turn;
  return d.whole_to + float2(t.x * e.x - t.y * e.y, t.y * e.x + t.x * e.y);
}

static float2 envelope_map(const DeformShape &d, float2 p, std::vector<float> &scratch)
{
  float2 q = p;
  if (d.lattice_cut) {
    q = cage_whole_at(d, p);
  }
  else if (mean_value(d.rest, p, scratch)) {
    q = float2(0.0f, 0.0f);
    for (size_t i = 0; i < d.now.size(); i++) {
      q = q + d.now[i] * scratch[i];
    }
  }
  if (!d.lattice.empty()) {
    q = q + rigid_offset(d, p);
  }
  return envelope_sides_apply(d, p, q);
}

/* Mesh Deform's Envelope: `q` is where the mesh puts `p`; in the outer cells (`outer`) the outline's sides then set its
 * distance from them back, as Cage Deform's Envelope does, so lines along the edge keep their width. Their say fades out
 * by the mesh inside the outer cells, or they would pull what its inner nodes carry back toward where it was. */
static float2 mesh_envelope(const DeformShape &d, float2 p, float2 q, bool outer)
{
  return outer && d.envelope_mesh ? envelope_sides_apply(d, p, q) : q;
}

/* What a Cage Deform's Method needs, worked out once per frame from the outline at rest and now. */
/* Rigid's lattice over a cage at rest, and all of the solve that depends only on the rest: which cells are used, the
 * nodes and their links, where the outline's samples sit among the nodes, the system's diagonal, and each node's mean
 * value coordinates. Kept by the outline at rest (the same cage drawn on many frames, or several layers bent by it), so
 * a frame only starts from where the cage is now and solves.
 *
 * Square cells can't see a gap narrower than a few of them: both its sides fall in the same cells and whatever moves one
 * drags the other. So a cell is cut into pieces, one for each stretch of the cage in it that isn't joined to the others
 * inside the cell, and pieces in neighbouring cells only share nodes where the cage runs from one to the other: the two
 * sides of a gap have nodes of their own, even where they stand at the same place. Cells outside the cage between two
 * sides of it are left out. A cage with no such gap is cut nowhere, and has the plain lattice. */
struct RigidLattice {
  static constexpr float FIRM = 200.0f, HOLD = 1e-4f;
  struct Link {
    int to;
    float weight;
  };
  struct Hold {
    std::array<int, 4> node{};
    std::array<float, 4> weight{};
    size_t sample = 0;
  };
  float step = 1.0f, fade = 0.0f;
  float2 origin = float2(0.0f, 0.0f);
  int nx = 0, ny = 0;
  std::vector<int> cell;
  std::vector<float2> rest;
  std::vector<std::vector<Link>> links;
  std::vector<Hold> holds;
  /* Empty where it has none. */
  std::vector<std::vector<float>> mean_value;
  /* The system where the nodes go to sit (the same matrix every round, every frame: only what it is solved for
   * moves), as its Cholesky factor, banded: row k holds its `band` entries left of the diagonal, then the diagonal.
   * The nodes go row by row along the lattice, so a node's neighbours are within a row's length of it. */
  int band = 0;
  std::vector<double> factor;
  /* x with A x = b, A the system above (b and x two columns, x and y). */
  void solve(const std::vector<float2> &b, std::vector<float2> &x) const;

  /* A piece of a cell: its nodes (corner c at i + (c & 1), j + (c >> 1)), places in it, and the pieces next to it it is
   * joined to. A piece outside the cage, in a cell along the outline, has its middle for a place. */
  struct Piece {
    int i = 0, j = 0;
    bool outside = false;
    std::array<int, 4> corner{};
    std::vector<float2> at;
    std::vector<int> joined;
    /* The nodes around it a B-spline reads, -1 where none of its side is there, and which stencil they are. */
    std::array<int, 16> nodes{};
    int stencil = -1;
  };
  std::vector<Piece> pieces;
  /* Which pieces a place may be on, for the cells where it matters (none on a lattice with no cuts). */
  std::vector<int> zone_of;
  std::vector<std::vector<int>> zones;
  /* The pieces that are read from stencils, and which of the lattice's places have nodes standing apart. */
  std::vector<int> stencil_pieces;
  std::vector<char> cut_at;
  /* The outline, and which of its pieces are in each cell. */
  std::vector<float2> outline;
  std::vector<std::vector<int>> outline_cells;
  /* Each cell's first piece, -1 where it has none. */
  std::vector<int> cell_piece;
  /* Whether a line between two places crosses the outline. */
  bool crosses(float2 a, float2 b) const;
  /* The piece in a zone that `p` is on: the nearest it can reach without crossing the outline, else the nearest. */
  int piece_at(const std::vector<int> &zone, float2 p) const;
  /* Envelope's sides, as each one's first sample and how many samples: how much further than straight each is from
   * each node, walking through the cage (`detours`, side by side, empty for a side nothing is behind anything from).
   * Worked out once, the first time they are asked for; a cage with no dents has none. */
  mutable std::once_flag walked;
  mutable std::vector<std::vector<float>> detours;
  void walk_sides(const std::vector<std::pair<size_t, size_t>> &sides, float size) const;
  /* The nodes around `p`, each with its share. False where there are none. */
  bool nodes_at(float2 p, std::array<int, 4> &r_node, std::array<float, 4> &r_share) const;
};

bool RigidLattice::crosses(float2 a, float2 b) const
{
  const int columns = nx - 1, rows = ny - 1;
  const float2 ua = (a - origin) / step, ub = (b - origin) / step;
  const int i0 = std::clamp(int(std::floor(std::min(ua.x, ub.x))), 0, columns - 1);
  const int i1 = std::clamp(int(std::floor(std::max(ua.x, ub.x))), 0, columns - 1);
  const int j0 = std::clamp(int(std::floor(std::min(ua.y, ub.y))), 0, rows - 1);
  const int j1 = std::clamp(int(std::floor(std::max(ua.y, ub.y))), 0, rows - 1);
  const size_t n = outline.size();
  const float2 ab = b - a;
  for (int j = j0; j <= j1; j++) {
    for (int i = i0; i <= i1; i++) {
      for (const int k : outline_cells[size_t(j * columns + i)]) {
        const float2 c = outline[size_t(k)], d = outline[(size_t(k) + 1) % n];
        const float2 cd = d - c;
        if ((cross(ab, c - a) > 0.0f) != (cross(ab, d - a) > 0.0f) &&
            (cross(cd, a - c) > 0.0f) != (cross(cd, b - c) > 0.0f))
        {
          return true;
        }
      }
    }
  }
  return false;
}

int RigidLattice::piece_at(const std::vector<int> &zone, float2 p) const
{
  if (zone.size() == 1) {
    return zone[0];
  }
  /* Each piece's nearest place to it, nearest first. */
  thread_local std::vector<std::tuple<float, int, float2>> near;
  near.clear();
  for (const int k : zone) {
    float best = FLT_MAX;
    float2 place = p;
    for (const float2 &q : pieces[size_t(k)].at) {
      const float dist = length(q - p);
      if (dist < best) {
        best = dist;
        place = q;
      }
    }
    near.emplace_back(best, k, place);
  }
  std::sort(near.begin(), near.end(), [](const auto &a, const auto &b) { return std::get<0>(a) < std::get<0>(b); });
  for (const auto &[dist, k, place] : near) {
    if (!crosses(p, place)) {
      return k;
    }
  }
  return std::get<1>(near[0]);
}

bool RigidLattice::nodes_at(float2 p, std::array<int, 4> &r_node, std::array<float, 4> &r_share) const
{
  const float2 u = (p - origin) / step;
  const int i = int(std::floor(u.x)), j = int(std::floor(u.y));
  if (i < 0 || i + 1 >= nx || j < 0 || j + 1 >= ny) {
    return false;
  }
  const size_t c = size_t(j * (nx - 1) + i);
  const int zone = zone_of.empty() ? -1 : zone_of[c];
  const int k = zone >= 0 ? piece_at(zones[size_t(zone)], p) : cell_piece[c];
  if (k < 0) {
    return false;
  }
  const Piece &piece = pieces[size_t(k)];
  const float fx = std::clamp(u.x - float(piece.i), 0.0f, 1.0f), fy = std::clamp(u.y - float(piece.j), 0.0f, 1.0f);
  r_node = piece.corner;
  r_share = {(1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy};
  return true;
}

void RigidLattice::walk_sides(const std::vector<std::pair<size_t, size_t>> &sides, float size) const
{
  std::call_once(walked, [&]() {
    /* A cage with no dents has nothing behind anything. */
    const size_t n = outline.size();
    int left = 0, right = 0;
    for (size_t k = 0; k < n; k++) {
      const float turn = cross(outline[(k + 1) % n] - outline[k], outline[(k + 2) % n] - outline[(k + 1) % n]);
      left += turn > 1e-12f ? 1 : 0;
      right += turn < -1e-12f ? 1 : 0;
    }
    if (left == 0 || right == 0) {
      return;
    }
    /* From each node to the others of each piece it is in. */
    const size_t count = rest.size();
    std::vector<std::vector<std::pair<int, float>>> ways(count);
    for (const Piece &piece : pieces) {
      for (int a = 0; a < 4; a++) {
        for (int b = 0; b < 4; b++) {
          if (a != b) {
            const int from = piece.corner[size_t(a)], to = piece.corner[size_t(b)];
            ways[size_t(from)].emplace_back(to, length(rest[size_t(from)] - rest[size_t(to)]));
          }
        }
      }
    }
    std::vector<int> side_of(n, -1);
    for (size_t s = 0; s < sides.size(); s++) {
      for (size_t k = 0; k < sides[s].second; k++) {
        side_of[(sides[s].first + k) % n] = int(s);
      }
    }
    detours.resize(sides.size());
    parallel_for(sides.size(), 1, [&](size_t begin, size_t end) {
      for (size_t s = begin; s < end; s++) {
        std::vector<float> walk(count, FLT_MAX);
        using Step = std::pair<float, int>;
        std::priority_queue<Step, std::vector<Step>, std::greater<Step>> open;
        for (const Hold &hold : holds) {
          if (side_of[hold.sample] != int(s)) {
            continue;
          }
          for (const int node : hold.node) {
            const float dist = length(rest[size_t(node)] - outline[hold.sample]);
            if (dist < walk[size_t(node)]) {
              walk[size_t(node)] = dist;
              open.emplace(dist, node);
            }
          }
        }
        while (!open.empty()) {
          const auto [dist, node] = open.top();
          open.pop();
          if (dist > walk[size_t(node)]) {
            continue;
          }
          for (const auto &[to, len] : ways[size_t(node)]) {
            if (dist + len < walk[size_t(to)]) {
              walk[size_t(to)] = dist + len;
              open.emplace(dist + len, to);
            }
          }
        }
        std::vector<float2> samples;
        for (size_t k = 0; k < sides[s].second; k++) {
          samples.push_back(outline[(sides[s].first + k) % n]);
        }
        /* Walking from node to node runs up to 8% long, and starts a step away from anywhere. */
        std::vector<float> detour(count, 0.0f);
        bool any = false;
        for (size_t k = 0; k < count; k++) {
          const float straight = polyline_distance(samples, false, rest[k]);
          detour[k] = std::max(std::min(walk[k], size * 8.0f) - straight * 1.08f - step, 0.0f);
          any = any || detour[k] > 0.0f;
        }
        if (any) {
          detours[s] = std::move(detour);
        }
      }
    });
  });
}

static bool envelope_walk_at(const DeformShape &d, float2 p, std::array<int, 4> &r_node, std::array<float, 4> &r_share)
{
  return d.envelope_walks && d.envelope_walks->nodes_at(p, r_node, r_share);
}

static float envelope_detour(const DeformShape &d, size_t side, const std::array<int, 4> &node,
                             const std::array<float, 4> &share)
{
  const std::vector<float> &detour = d.envelope_walks->detours[side];
  if (detour.empty()) {
    return 0.0f;
  }
  float sum = 0.0f;
  for (size_t c = 0; c < 4; c++) {
    sum += detour[size_t(node[c])] * share[c];
  }
  return sum;
}

/* Rigid: how far the inside moves from mean value coordinates there (a cubic B-spline over the lattice). */
static float2 rigid_offset(const DeformShape &d, float2 p)
{
  const float2 u = (p - d.lattice_origin) / d.lattice_step;
  const int i0 = int(std::floor(u.x)), j0 = int(std::floor(u.y));
  const float fx = u.x - float(i0), fy = u.y - float(j0);
  const auto basis = [](float t, float r_w[4]) {
    const float s = 1.0f - t;
    r_w[0] = s * s * s / 6.0f;
    r_w[1] = (3.0f * t * t * t - 6.0f * t * t + 4.0f) / 6.0f;
    r_w[2] = (-3.0f * t * t * t + 3.0f * t * t + 3.0f * t + 1.0f) / 6.0f;
    r_w[3] = t * t * t / 6.0f;
  };
  float wx[4], wy[4];
  /* By a gap: only what is on this side of it. */
  if (d.lattice_cut && i0 >= 0 && i0 + 1 < d.lattice_nx && j0 >= 0 && j0 + 1 < d.lattice_ny) {
    const RigidLattice &L = *d.lattice_cut;
    const int zone = L.zone_of[size_t(j0 * (d.lattice_nx - 1) + i0)];
    if (zone >= 0) {
      const RigidLattice::Piece &piece = L.pieces[size_t(L.piece_at(L.zones[size_t(zone)], p))];
      basis(std::clamp(u.x - float(piece.i), 0.0f, 1.0f), wx);
      basis(std::clamp(u.y - float(piece.j), 0.0f, 1.0f), wy);
      const std::array<float2, 16> &stencil = d.lattice_stencils[size_t(piece.stencil)];
      float2 sum(0.0f, 0.0f);
      for (int b = 0; b < 4; b++) {
        for (int a = 0; a < 4; a++) {
          sum = sum + stencil[size_t(b * 4 + a)] * (wx[a] * wy[b]);
        }
      }
      return sum;
    }
  }
  basis(fx, wx);
  basis(fy, wy);
  float2 sum(0.0f, 0.0f);
  for (int b = 0; b < 4; b++) {
    const int j = j0 - 1 + b;
    if (j < 0 || j >= d.lattice_ny) {
      continue;
    }
    for (int a = 0; a < 4; a++) {
      const int i = i0 - 1 + a;
      if (i >= 0 && i < d.lattice_nx) {
        sum = sum + d.lattice[size_t(j * d.lattice_nx + i)] * (wx[a] * wy[b]);
      }
    }
  }
  return sum;
}

void RigidLattice::solve(const std::vector<float2> &b, std::vector<float2> &x) const
{
  const int count = int(rest.size());
  const int w = band + 1;
  std::vector<double> yx(static_cast<size_t>(count)), yy(static_cast<size_t>(count));
  for (int k = 0; k < count; k++) {
    double sx = double(b[size_t(k)].x), sy = double(b[size_t(k)].y);
    const double *row = &factor[size_t(k) * size_t(w)];
    for (int i = std::max(0, k - band); i < k; i++) {
      const double l = row[i - k + band];
      sx -= l * yx[size_t(i)];
      sy -= l * yy[size_t(i)];
    }
    yx[size_t(k)] = sx / row[band];
    yy[size_t(k)] = sy / row[band];
  }
  for (int k = count - 1; k >= 0; k--) {
    double sx = yx[size_t(k)], sy = yy[size_t(k)];
    for (int i = k + 1; i <= std::min(count - 1, k + band); i++) {
      const double l = factor[size_t(i) * size_t(w) + size_t(k - i + band)];
      sx -= l * yx[size_t(i)];
      sy -= l * yy[size_t(i)];
    }
    const double d = factor[size_t(k) * size_t(w) + size_t(band)];
    yx[size_t(k)] = sx / d;
    yy[size_t(k)] = sy / d;
  }
  x.resize(static_cast<size_t>(count));
  for (int k = 0; k < count; k++) {
    x[size_t(k)] = float2(float(yx[size_t(k)]), float(yy[size_t(k)]));
  }
}

static std::shared_ptr<const RigidLattice> rigid_lattice(const std::vector<float2> &outline, float2 lo, float2 hi,
                                                         float size)
{
  static std::mutex mutex;
  static std::vector<std::pair<std::vector<float2>, std::shared_ptr<const RigidLattice>>> kept;
  {
    std::lock_guard lock(mutex);
    for (const auto &[at_rest, lattice] : kept) {
      if (at_rest == outline) {
        return lattice;
      }
    }
  }
  auto made = std::make_shared<RigidLattice>();
  RigidLattice &L = *made;
  const size_t n = outline.size();
  const float h = size / 32.0f;
  const float fade = 14.0f * h;
  L.step = h;
  L.fade = fade;
  L.origin = lo - float2(fade + 2.0f * h, fade + 2.0f * h);
  L.nx = int((hi.x - lo.x + 2.0f * fade) / h) + 5;
  L.ny = int((hi.y - lo.y + 2.0f * fade) / h) + 5;
  const int nx = L.nx, ny = L.ny;
  const auto lattice_at = [&](int i, int j) { return L.origin + float2(float(i), float(j)) * h; };
  std::vector<char> used(static_cast<size_t>((nx - 1) * (ny - 1)), 0);
  for (int j = 0; j + 1 < ny; j++) {
    for (int i = 0; i + 1 < nx; i++) {
      const float2 c = lattice_at(i, j) + float2(0.5f * h, 0.5f * h);
      used[size_t(j * (nx - 1) + i)] = inside(outline, c) || polyline_distance(outline, true, c) < 0.75f * h;
    }
  }
  const int columns = nx - 1, rows = ny - 1;
  const size_t cells = size_t(columns) * size_t(rows);
  /* Which of the outline's pieces are in each cell. */
  L.outline = outline;
  L.outline_cells.resize(cells);
  for (size_t k = 0; k < n; k++) {
    const float2 a = (outline[k] - L.origin) / h, b = (outline[(k + 1) % n] - L.origin) / h;
    const int i0 = std::clamp(int(std::floor(std::min(a.x, b.x))), 0, columns - 1);
    const int i1 = std::clamp(int(std::floor(std::max(a.x, b.x))), 0, columns - 1);
    const int j0 = std::clamp(int(std::floor(std::min(a.y, b.y))), 0, rows - 1);
    const int j1 = std::clamp(int(std::floor(std::max(a.y, b.y))), 0, rows - 1);
    for (int j = j0; j <= j1; j++) {
      for (int i = i0; i <= i1; i++) {
        L.outline_cells[size_t(j * columns + i)].push_back(int(k));
      }
    }
  }
  /* A cell outside the cage with its outline near on two sides, facing each other. */
  const auto between_sides = [&](float2 c) {
    std::vector<char> near(n, 0);
    size_t start = n;
    for (size_t k = 0; k < n; k++) {
      const float2 a = outline[k], e = outline[(k + 1) % n] - a;
      const float ee = dot(e, e);
      const float t = ee > 1e-20f ? std::clamp(dot(c - a, e) / ee, 0.0f, 1.0f) : 0.0f;
      near[k] = length(a + e * t - c) < 2.0f * h;
      start = !near[k] ? k : start;
    }
    if (start == n) {
      return false;
    }
    /* Each run of near pieces around the outline, that the cell can see the nearest place of. */
    int facing = 0;
    for (size_t step = 1; step <= n;) {
      size_t k = (start + step) % n;
      if (!near[k]) {
        step++;
        continue;
      }
      float best = FLT_MAX;
      float2 place = c;
      for (; step <= n && near[(start + step) % n]; step++) {
        k = (start + step) % n;
        const float2 a = outline[k], e = outline[(k + 1) % n] - a;
        const float ee = dot(e, e);
        const float t = ee > 1e-20f ? std::clamp(dot(c - a, e) / ee, 0.0f, 1.0f) : 0.0f;
        const float dist = length(a + e * t - c);
        if (dist < best) {
          best = dist;
          place = a + e * t;
        }
      }
      facing += L.crosses(c, place + (c - place) * 1e-3f) ? 0 : 1;
    }
    return facing >= 2;
  };
  /* The pieces of each cell in use. */
  std::vector<std::vector<int>> in_cell(cells);
  std::vector<char> whole(cells, 0);
  constexpr int ACROSS = 4;
  for (int j = 0; j < rows; j++) {
    for (int i = 0; i < columns; i++) {
      const size_t c = size_t(j * columns + i);
      if (!used[c]) {
        continue;
      }
      const float2 corner = lattice_at(i, j);
      const float2 middle = corner + float2(0.5f * h, 0.5f * h);
      std::vector<float2> places;
      bool all_in = true;
      if (!L.outline_cells[c].empty() || inside(outline, middle)) {
        for (int b = 0; b < ACROSS; b++) {
          for (int a = 0; a < ACROSS; a++) {
            const float2 q = corner + float2((float(a) + 0.5f) / ACROSS * h, (float(b) + 0.5f) / ACROSS * h);
            if (L.outline_cells[c].empty() || inside(outline, q)) {
              places.push_back(q);
            }
            else {
              all_in = false;
            }
          }
        }
      }
      const auto add_piece = [&](bool outside, std::vector<float2> at) {
        RigidLattice::Piece piece;
        piece.i = i;
        piece.j = j;
        piece.outside = outside;
        piece.at = std::move(at);
        in_cell[c].push_back(int(L.pieces.size()));
        L.pieces.push_back(std::move(piece));
      };
      if (places.empty()) {
        if (!between_sides(middle)) {
          add_piece(true, {middle});
        }
        continue;
      }
      whole[c] = all_in && L.outline_cells[c].empty();
      if (whole[c]) {
        add_piece(false, std::move(places));
        continue;
      }
      /* Places a straight line joins are one piece. */
      std::vector<char> taken(places.size(), 0);
      for (size_t first = 0; first < places.size(); first++) {
        if (taken[first]) {
          continue;
        }
        std::vector<float2> at{places[first]};
        taken[first] = 1;
        for (size_t reached = 0; reached < at.size(); reached++) {
          for (size_t k = first + 1; k < places.size(); k++) {
            if (!taken[k] && !L.crosses(at[reached], places[k])) {
              taken[k] = 1;
              at.push_back(places[k]);
            }
          }
        }
        add_piece(false, std::move(at));
      }
    }
  }
  /* Pieces either side of a side of a cell share that side's two nodes if the cage runs from one to the other. */
  std::vector<int> root(L.pieces.size() * 4);
  std::iota(root.begin(), root.end(), 0);
  const auto find = [&](int k) {
    while (root[size_t(k)] != k) {
      root[size_t(k)] = root[size_t(root[size_t(k)])];
      k = root[size_t(k)];
    }
    return k;
  };
  const auto nearest_piece = [&](const std::vector<int> &of, float2 p) {
    return *std::min_element(of.begin(), of.end(), [&](int a, int b) {
      const auto near = [&](int k) {
        float best = FLT_MAX;
        for (const float2 &q : L.pieces[size_t(k)].at) {
          best = std::min(best, length(q - p));
        }
        return best;
      };
      return near(a) < near(b);
    });
  };
  const auto cage_runs_between = [&](int a, int b, bool plain) {
    const RigidLattice::Piece &p = L.pieces[size_t(a)], &q = L.pieces[size_t(b)];
    if (plain || (p.outside && q.outside)) {
      return true;
    }
    if (p.outside != q.outside) {
      return false;
    }
    for (const float2 &from : p.at) {
      for (const float2 &to : q.at) {
        if (!L.crosses(from, to)) {
          return true;
        }
      }
    }
    return false;
  };
  const auto join_cells = [&](size_t c, size_t e, int corner_c0, int corner_e0, int corner_c1, int corner_e1) {
    const bool plain = whole[c] && whole[e];
    for (const int a : in_cell[c]) {
      for (const int b : in_cell[e]) {
        const RigidLattice::Piece &p = L.pieces[size_t(a)], &q = L.pieces[size_t(b)];
        /* What lies outside the cage goes with the stretch of the cage next to it, in the cell beside it. */
        const bool to_nearest = p.outside != q.outside &&
                                (p.outside ? b == nearest_piece(in_cell[e], p.at[0]) :
                                             a == nearest_piece(in_cell[c], q.at[0]));
        if (to_nearest || cage_runs_between(a, b, plain)) {
          root[size_t(find(a * 4 + corner_c0))] = find(b * 4 + corner_e0);
          root[size_t(find(a * 4 + corner_c1))] = find(b * 4 + corner_e1);
          L.pieces[size_t(a)].joined.push_back(b);
          L.pieces[size_t(b)].joined.push_back(a);
        }
      }
    }
  };
  for (int j = 0; j < rows; j++) {
    for (int i = 0; i < columns; i++) {
      const size_t c = size_t(j * columns + i);
      if (i + 1 < columns) {
        join_cells(c, c + 1, 1, 0, 3, 2);
      }
      if (j + 1 < rows) {
        join_cells(c, c + size_t(columns), 2, 0, 3, 1);
      }
    }
  }
  /* The nodes, row by row along the lattice (however many stand at a place). */
  std::vector<std::array<int, 3>> corners;
  corners.reserve(L.pieces.size() * 4);
  for (size_t k = 0; k < L.pieces.size(); k++) {
    const RigidLattice::Piece &piece = L.pieces[k];
    for (int c = 0; c < 4; c++) {
      corners.push_back({(piece.j + (c >> 1)) * nx + piece.i + (c & 1), find(int(k) * 4 + c), int(k) * 4 + c});
    }
  }
  std::sort(corners.begin(), corners.end());
  bool cut = false;
  L.cut_at.assign(static_cast<size_t>(nx * ny), 0);
  for (size_t k = 0; k < corners.size(); k++) {
    if (k == 0 || corners[k][0] != corners[k - 1][0] || corners[k][1] != corners[k - 1][1]) {
      if (k > 0 && corners[k][0] == corners[k - 1][0]) {
        L.cut_at[size_t(corners[k][0])] = 1;
        cut = true;
      }
      const int at = corners[k][0];
      L.rest.push_back(lattice_at(at % nx, at / nx));
      L.cell.push_back(at);
    }
    L.pieces[size_t(corners[k][2] / 4)].corner[size_t(corners[k][2] % 4)] = int(L.rest.size()) - 1;
  }
  const int count = int(L.rest.size());
  /* Each side of a cell weighs a half for each cell in use it is a side of. */
  L.links.resize(static_cast<size_t>(count));
  std::map<std::pair<int, int>, float> sides;
  for (const RigidLattice::Piece &piece : L.pieces) {
    for (const auto &[a, b] : {std::pair(0, 1), std::pair(2, 3), std::pair(0, 2), std::pair(1, 3)}) {
      sides[std::minmax(piece.corner[size_t(a)], piece.corner[size_t(b)])] += 0.5f;
    }
  }
  for (const auto &[pair, weight] : sides) {
    L.links[size_t(pair.first)].push_back({pair.second, weight});
    L.links[size_t(pair.second)].push_back({pair.first, weight});
  }
  /* Cells left out between two sides: nothing is known at their corners. */
  for (int j = 0; j < rows; j++) {
    for (int i = 0; i < columns; i++) {
      if (used[size_t(j * columns + i)] && in_cell[size_t(j * columns + i)].empty()) {
        cut = true;
        for (int c = 0; c < 4; c++) {
          L.cut_at[size_t((j + (c >> 1)) * nx + i + (c & 1))] = 1;
        }
      }
    }
  }
  /* The outline's samples, each between the four nodes round it, of the piece on its side of a gap. */
  double twice_area = 0.0;
  for (size_t k = 0; k < n; k++) {
    twice_area += double(cross(outline[k], outline[(k + 1) % n]));
  }
  for (size_t k = 0; k < n; k++) {
    const float2 u = (outline[k] - L.origin) / h;
    const int i = int(std::floor(u.x)), j = int(std::floor(u.y));
    if (i < 0 || i >= columns || j < 0 || j >= rows) {
      continue;
    }
    std::vector<int> choices = in_cell[size_t(j * columns + i)];
    const float2 way = outline[(k + 1) % n] - outline[(k + n - 1) % n];
    const float2 in = normalize(twice_area > 0.0 ? float2(-way.y, way.x) : float2(way.y, -way.x));
    const float2 just_in = outline[k] + in * (h * 1e-3f);
    /* The only piece in its cell can be across a gap from it, its own side too thin there to have a piece. */
    const auto on_its_side = [&](const RigidLattice::Piece &piece) {
      if (piece.outside) {
        return true;
      }
      const float2 place = *std::min_element(piece.at.begin(), piece.at.end(), [&](const float2 &a, const float2 &b) {
        return length(a - just_in) < length(b - just_in);
      });
      return !L.crosses(just_in, place);
    };
    if (choices.size() != 1 || !on_its_side(L.pieces[size_t(choices[0])])) {
      choices.clear();
      for (int b = std::max(j - 1, 0); b <= std::min(j + 1, rows - 1); b++) {
        for (int a = std::max(i - 1, 0); a <= std::min(i + 1, columns - 1); a++) {
          const std::vector<int> &around = in_cell[size_t(b * columns + a)];
          choices.insert(choices.end(), around.begin(), around.end());
        }
      }
      if (choices.empty()) {
        continue;
      }
      choices = {L.piece_at(choices, just_in)};
    }
    const RigidLattice::Piece &piece = L.pieces[size_t(choices[0])];
    const float fx = u.x - float(piece.i), fy = u.y - float(piece.j);
    RigidLattice::Hold hold;
    hold.sample = k;
    for (int c = 0; c < 4; c++) {
      hold.node[size_t(c)] = piece.corner[size_t(c)];
      hold.weight[size_t(c)] = ((c & 1) ? fx : 1.0f - fx) * ((c >> 1) ? fy : 1.0f - fy);
    }
    L.holds.push_back(hold);
  }
  if (cut) {
    /* Cells whose B-spline reads a place that is cut: what is there reads its own side's nodes. */
    L.zone_of.assign(cells, -1);
    for (int j = 0; j < rows; j++) {
      for (int i = 0; i < columns; i++) {
        bool near_cut = false;
        for (int b = std::max(j - 1, 0); b <= std::min(j + 2, ny - 1); b++) {
          for (int a = std::max(i - 1, 0); a <= std::min(i + 2, nx - 1); a++) {
            near_cut = near_cut || L.cut_at[size_t(b * nx + a)];
          }
        }
        if (!near_cut) {
          continue;
        }
        std::vector<int> zone = in_cell[size_t(j * columns + i)];
        if (zone.empty()) {
          for (int b = std::max(j - 2, 0); b <= std::min(j + 2, rows - 1); b++) {
            for (int a = std::max(i - 2, 0); a <= std::min(i + 2, columns - 1); a++) {
              const std::vector<int> &around = in_cell[size_t(b * columns + a)];
              zone.insert(zone.end(), around.begin(), around.end());
            }
          }
        }
        if (!zone.empty()) {
          L.zone_of[size_t(j * columns + i)] = int(L.zones.size());
          for (const int k : zone) {
            RigidLattice::Piece &piece = L.pieces[size_t(k)];
            if (piece.stencil < 0) {
              piece.stencil = int(L.stencil_pieces.size());
              L.stencil_pieces.push_back(k);
            }
          }
          L.zones.push_back(std::move(zone));
        }
      }
    }
    /* The nodes around each, walking from it through the pieces it is joined to. */
    for (const int k : L.stencil_pieces) {
      RigidLattice::Piece &piece = L.pieces[size_t(k)];
      piece.nodes.fill(-1);
      std::vector<int> reached{k};
      for (size_t at = 0; at < reached.size(); at++) {
        const RigidLattice::Piece &there = L.pieces[size_t(reached[at])];
        for (int c = 0; c < 4; c++) {
          const int a = there.i + (c & 1) - (piece.i - 1), b = there.j + (c >> 1) - (piece.j - 1);
          if (a >= 0 && a < 4 && b >= 0 && b < 4 && piece.nodes[size_t(b * 4 + a)] < 0) {
            piece.nodes[size_t(b * 4 + a)] = there.corner[size_t(c)];
          }
        }
        for (const int next : there.joined) {
          const RigidLattice::Piece &other = L.pieces[size_t(next)];
          if (std::abs(other.i - piece.i) <= 2 && std::abs(other.j - piece.j) <= 2 &&
              std::find(reached.begin(), reached.end(), next) == reached.end())
          {
            reached.push_back(next);
          }
        }
      }
    }
  }
  else {
    L.outline_cells.clear();
    L.cut_at.clear();
    for (RigidLattice::Piece &piece : L.pieces) {
      piece.at.clear();
      piece.joined.clear();
    }
  }
  L.cell_piece.assign(cells, -1);
  for (size_t c = 0; c < cells; c++) {
    if (!in_cell[c].empty()) {
      L.cell_piece[c] = in_cell[c][0];
    }
  }
  /* The system where the nodes go to sit (each node held a little where it is, pulled by its links and by the outline's
   * samples round it), then its factor, once for the lattice. */
  for (int k = 0; k < count; k++) {
    for (const RigidLattice::Link &l : L.links[size_t(k)]) {
      L.band = std::max(L.band, std::abs(k - l.to));
    }
  }
  for (const RigidLattice::Hold &hold : L.holds) {
    for (int c = 0; c < 4; c++) {
      for (int d = 0; d < 4; d++) {
        L.band = std::max(L.band, std::abs(hold.node[size_t(c)] - hold.node[size_t(d)]));
      }
    }
  }
  const int w = L.band + 1;
  std::vector<double> &a = L.factor;
  a.assign(size_t(count) * size_t(w), 0.0);
  /* Entry (k, j), j <= k. */
  const auto at = [&](int k, int j) -> double & { return a[size_t(k) * size_t(w) + size_t(j - k + L.band)]; };
  for (int k = 0; k < count; k++) {
    at(k, k) += double(RigidLattice::HOLD);
    for (const RigidLattice::Link &l : L.links[size_t(k)]) {
      at(k, k) += double(l.weight);
      if (l.to < k) {
        at(k, l.to) -= double(l.weight);
      }
    }
  }
  for (const RigidLattice::Hold &hold : L.holds) {
    for (int c = 0; c < 4; c++) {
      for (int d = 0; d < 4; d++) {
        const int p = hold.node[size_t(c)], q = hold.node[size_t(d)];
        if (q <= p) {
          at(p, q) += double(RigidLattice::FIRM) * double(hold.weight[size_t(c)]) * double(hold.weight[size_t(d)]);
        }
      }
    }
  }
  for (int k = 0; k < count; k++) {
    for (int j = std::max(0, k - L.band); j <= k; j++) {
      double s = at(k, j);
      for (int i = std::max(0, std::max(k, j) - L.band); i < j; i++) {
        s -= at(k, i) * at(j, i);
      }
      at(k, j) = j == k ? std::sqrt(std::max(s, 1e-300)) : s / at(j, j);
    }
  }
  L.mean_value.resize(static_cast<size_t>(count));
  parallel_for(size_t(count), 32, [&](size_t begin, size_t end) {
    for (size_t k = begin; k < end; k++) {
      std::vector<float> w;
      if (mean_value(outline, L.rest[k], w)) {
        L.mean_value[k] = std::move(w);
      }
    }
  });
  std::lock_guard lock(mutex);
  if (kept.size() >= 16) {
    kept.erase(kept.begin());
  }
  kept.emplace_back(outline, made);
  return made;
}

/* A side's lengths along it and the way it runs at each sample. False when it has no length. */
static bool envelope_side_finish(DeformShape::EnvelopeSide &side, float size)
{
  side.rest_at = lengths_along(side.rest);
  side.now_at = lengths_along(side.now);
  if (side.rest_at.back() < size * 1e-6f) {
    return false;
  }
  /* The way it runs at each sample: its two pieces' ways together (the end ones, their piece's). */
  const auto ways = [](const std::vector<float2> &v, std::vector<float2> &r_way) {
    const size_t m = v.size();
    r_way.assign(m, float2(1.0f, 0.0f));
    for (size_t k = 0; k < m; k++) {
      float2 w(0.0f, 0.0f);
      if (k > 0 && length(v[k] - v[k - 1]) > 1e-12f) {
        w = w + normalize(v[k] - v[k - 1]);
      }
      if (k + 1 < m && length(v[k + 1] - v[k]) > 1e-12f) {
        w = w + normalize(v[k + 1] - v[k]);
      }
      if (length(w) > 1e-12f) {
        r_way[k] = normalize(w);
      }
      else if (k > 0) {
        r_way[k] = r_way[k - 1];
      }
    }
  };
  ways(side.rest, side.rest_way);
  ways(side.now, side.now_way);
  return true;
}

static void bounds_of(const std::vector<float2> &v, float2 &r_lo, float2 &r_hi)
{
  r_lo = r_hi = v[0];
  for (const float2 &q : v) {
    r_lo = float2(std::min(r_lo.x, q.x), std::min(r_lo.y, q.y));
    r_hi = float2(std::max(r_hi.x, q.x), std::max(r_hi.y, q.y));
  }
}

/* A side with its nodes dragged close has handles too long for it, and runs back on itself along the way between its
 * ends: which way it runs would flip between its nearest places, and across with it. Never backwards along there,
 * samples sit still instead, which leaves one that doesn't go back as it was, and changes with it smoothly. */
static void side_unfold(std::vector<float2> &v)
{
  const float2 start = v.front();
  const float len = length(v.back() - start);
  if (len < 1e-12f) {
    return;
  }
  const float2 way = (v.back() - start) / len;
  float reached = 0.0f;
  for (float2 &q : v) {
    const float along = dot(q - start, way);
    reached = std::clamp(along, reached, len);
    q = q + way * (reached - along);
  }
}

/* The outline's sides for Mesh Deform's Envelope (of four-sided cells: what the points outside follow too), and the
 * mesh inside the outer cells: every line with neither end on the outline, and the inner end of those with one. */
static void mesh_envelope_build(DeformShape &r, float blend)
{
  if (r.edge_sides.empty()) {
    return;
  }
  r.envelope_mesh = true;
  r.envelope_power = envelope_power_of(blend);
  float2 lo = r.edge_sides[0][0][0], hi = lo;
  for (const std::array<std::array<float2, 4>, 2> &side : r.edge_sides) {
    for (const float2 &q : side[0]) {
      lo = float2(std::min(lo.x, q.x), std::min(lo.y, q.y));
      hi = float2(std::max(hi.x, q.x), std::max(hi.y, q.y));
    }
  }
  const float size = std::max(hi.x - lo.x, hi.y - lo.y);
  for (const std::array<std::array<float2, 4>, 2> &side : r.edge_sides) {
    /* Fewer samples than a cage's side has: each is a cell's width, and this runs for every point bent. */
    DeformShape::EnvelopeSide e;
    for (int k = 0; k <= 6; k++) {
      const float t = float(k) / 6.0f;
      e.rest.push_back(bezier3(side[0], t));
      e.now.push_back(bezier3(side[1], t));
    }
    side_unfold(e.rest);
    side_unfold(e.now);
    if (envelope_side_finish(e, size)) {
      bounds_of(e.rest, e.lo, e.hi);
      const float was = length(e.rest.back() - e.rest.front()), is = length(e.now.back() - e.now.front());
      e.stays = was > 1e-12f ? std::min(1.0f, 4.0f * is / was) : 0.0f;
      r.envelope_sides.push_back(std::move(e));
    }
  }
  /* As far as the mesh is big, as a cage's. */
  r.envelope_reach = size;
  r.cage_band = size;
  /* The sides of the outer cells: nothing further in is ever nearer. */
  std::vector<std::array<float2, 4>> lines;
  const GridSurface &g = r.rest_grid;
  for (int row = 0; r.cells.empty() && row < g.rows; row++) {
    for (int column = 0; column < g.columns; column++) {
      if (row == 0 || column == 0 || row == g.rows - 1 || column == g.columns - 1) {
        lines.push_back(g.across[size_t(row * g.columns + column)]);
        lines.push_back(g.across[size_t((row + 1) * g.columns + column)]);
        lines.push_back(g.up[size_t(column * g.rows + row)]);
        lines.push_back(g.up[size_t((column + 1) * g.rows + row)]);
      }
    }
  }
  for (const DeformShape::Cell &c : r.cells) {
    if (c.outer) {
      lines.insert(lines.end(), c.rest_sides.begin(), c.rest_sides.end());
    }
  }
  const auto on_outline = [&](float2 p) {
    return std::any_of(r.edge_sides.begin(), r.edge_sides.end(), [&](const auto &side) {
      return length(side[0][0] - p) < size * 1e-5f || length(side[0][3] - p) < size * 1e-5f;
    });
  };
  for (const std::array<float2, 4> &line : lines) {
    const bool from = on_outline(line[0]), to = on_outline(line[3]);
    if (from && to) {
      continue;
    }
    DeformShape::RingLine ring;
    if (from || to) {
      ring.samples.assign(2, from ? line[3] : line[0]);
    }
    else {
      for (int k = 0; k <= 4; k++) {
        ring.samples.push_back(bezier3(line, float(k) / 4.0f));
      }
    }
    bounds_of(ring.samples, ring.lo, ring.hi);
    r.envelope_ring.push_back(std::move(ring));
  }
}

/* Mesh Deform's Points: every line of the mesh (each segment between two of its points) is a bone. What lies near a
 * line keeps its place by it, turned as the line turns there, so a point's handles turn what lies by the point and a
 * curved line carries what lies along it. Each place is shared by the lines near it: the nearest leads (by Blend, as
 * Envelope's sides), and a line's say fades out by Falloff past the nearest one's distance. Distances are measured
 * through the mesh, from the cell a place is in across cells and along lines, so a line never reaches across a gap
 * in the mesh. All of it is worked out from the mesh at rest: the weights never change as it moves. */
struct PointBones {
  /* Samples along each line that distances are measured between: line `s` has nodes s * (STEPS + 1) on. */
  static constexpr int STEPS = 8;
  std::vector<std::array<float2, 4>> rest;
  /* Each cell's lines, its outline at rest and its bounds. */
  std::vector<std::vector<int>> cell_lines;
  std::vector<std::vector<float2>> cell_outline;
  std::vector<float2> cell_lo, cell_hi;
  float size = 1.0f, reach = 1.0f;
  std::vector<float2> nodes;
  /* From each node, the lines within reach through the mesh and how far the nearest of their nodes is. */
  std::vector<std::vector<std::pair<int, float>>> near;
};

/* The mesh at rest for Points: its lines, each cell's lines (indices into `lines`) and its outline. Kept by the lines,
 * the cells and the reach: the same mesh at rest bends many frames. */
static std::shared_ptr<const PointBones> point_bones(const std::vector<std::array<float2, 4>> &lines,
                                                     const std::vector<std::vector<int>> &cell_lines,
                                                     const std::vector<std::vector<float2>> &cell_outlines,
                                                     float falloff)
{
  static std::mutex mutex;
  static std::vector<std::shared_ptr<const PointBones>> kept;
  float2 lo = lines[0][0], hi = lo;
  for (const std::array<float2, 4> &line : lines) {
    for (const float2 &q : line) {
      lo = float2(std::min(lo.x, q.x), std::min(lo.y, q.y));
      hi = float2(std::max(hi.x, q.x), std::max(hi.y, q.y));
    }
  }
  const float size = std::max(std::max(hi.x - lo.x, hi.y - lo.y), 1e-6f);
  /* Never quite none: a place half way between two lines would jump from one to the other. */
  const float reach = size * std::max(std::clamp(falloff, 0.0f, 1.0f), 0.01f);
  {
    std::lock_guard lock(mutex);
    for (const std::shared_ptr<const PointBones> &b : kept) {
      if (b->rest == lines && b->cell_lines == cell_lines && b->reach == reach) {
        return b;
      }
    }
  }
  auto made = std::make_shared<PointBones>();
  PointBones &B = *made;
  B.rest = lines;
  B.cell_lines = cell_lines;
  B.size = size;
  B.reach = reach;
  constexpr int per = PointBones::STEPS + 1;
  const size_t count = lines.size() * per;
  B.nodes.resize(count);
  for (size_t s = 0; s < lines.size(); s++) {
    for (int k = 0; k < per; k++) {
      B.nodes[s * per + size_t(k)] = bezier3(lines[s], float(k) / float(PointBones::STEPS));
    }
  }
  float widest = 0.0f;
  for (const std::vector<float2> &outline : cell_outlines) {
    float2 c_lo = outline.empty() ? lo : outline[0], c_hi = c_lo;
    for (const float2 &q : outline) {
      c_lo = float2(std::min(c_lo.x, q.x), std::min(c_lo.y, q.y));
      c_hi = float2(std::max(c_hi.x, q.x), std::max(c_hi.y, q.y));
    }
    widest = std::max(widest, length(c_hi - c_lo));
    B.cell_outline.push_back(outline);
    B.cell_lo.push_back(c_lo);
    B.cell_hi.push_back(c_hi);
  }
  /* The links: along each line, where lines meet, and straight across each cell between the nodes round it. */
  std::vector<std::vector<std::pair<int, float>>> links(count);
  const auto link = [&](size_t a, size_t b) {
    const float d = length(B.nodes[a] - B.nodes[b]);
    links[a].emplace_back(int(b), d);
    links[b].emplace_back(int(a), d);
  };
  for (size_t s = 0; s < lines.size(); s++) {
    for (int k = 0; k + 1 < per; k++) {
      link(s * per + size_t(k), s * per + size_t(k) + 1);
    }
  }
  for (size_t a = 0; a < lines.size(); a++) {
    for (size_t b = a + 1; b < lines.size(); b++) {
      for (const int ea : {0, per - 1}) {
        for (const int eb : {0, per - 1}) {
          if (length(B.nodes[a * per + size_t(ea)] - B.nodes[b * per + size_t(eb)]) < size * 1e-5f) {
            link(a * per + size_t(ea), b * per + size_t(eb));
          }
        }
      }
    }
  }
  for (const std::vector<int> &cell : cell_lines) {
    for (size_t i = 0; i < cell.size(); i++) {
      for (size_t j = i + 1; j < cell.size(); j++) {
        for (int a = 0; a < per; a++) {
          for (int b = 0; b < per; b++) {
            link(size_t(cell[i]) * per + size_t(a), size_t(cell[j]) * per + size_t(b));
          }
        }
      }
    }
  }
  /* From each node, as far as a line can still have a say: the reach past the nearest line, which is at most a cell
   * across (a little more for what lies outside the mesh). */
  const float cutoff = reach + widest + size * 0.05f;
  B.near.resize(count);
  parallel_for(count, 16, [&](size_t begin, size_t end) {
    std::vector<float> best(count, FLT_MAX);
    std::vector<float> line_best(lines.size(), FLT_MAX);
    std::vector<int> reached;
    using Item = std::pair<float, int>;
    for (size_t from = begin; from < end; from++) {
      std::priority_queue<Item, std::vector<Item>, std::greater<>> todo;
      best[from] = 0.0f;
      reached.assign(1, int(from));
      todo.emplace(0.0f, int(from));
      while (!todo.empty()) {
        const auto [d, n] = todo.top();
        todo.pop();
        if (d > best[size_t(n)]) {
          continue;
        }
        line_best[size_t(n) / per] = std::min(line_best[size_t(n) / per], d);
        for (const auto &[to, step] : links[size_t(n)]) {
          const float further = d + step;
          if (further < best[size_t(to)] && further <= cutoff) {
            if (best[size_t(to)] == FLT_MAX) {
              reached.push_back(to);
            }
            best[size_t(to)] = further;
            todo.emplace(further, to);
          }
        }
      }
      for (size_t s = 0; s < lines.size(); s++) {
        if (line_best[s] < FLT_MAX) {
          B.near[from].emplace_back(int(s), line_best[s]);
          line_best[s] = FLT_MAX;
        }
      }
      for (const int n : reached) {
        best[size_t(n)] = FLT_MAX;
      }
    }
  });
  std::lock_guard lock(mutex);
  if (kept.size() >= 16) {
    kept.erase(kept.begin());
  }
  kept.push_back(made);
  return made;
}

/* What each line has to say at `p` (at rest): (line, share before they are added up, where along the line). The
 * nearest line leads as Blend says, and each line's say fades out by Falloff past the nearest one's distance. */
struct PointShare {
  int line;
  double weight;
  float t;
};
static void point_shares(const PointBones &B, float power, float2 p, std::vector<PointShare> &r)
{
  r.clear();
  constexpr int per = PointBones::STEPS + 1;
  const size_t lines = B.rest.size();
  thread_local std::vector<float> dist, place;
  dist.assign(lines, FLT_MAX);
  place.assign(lines, -1.0f);
  /* The cell it is in: its lines are as near as they are, and the others as far as from those through the mesh. */
  int cell = -1;
  for (size_t c = 0; c < B.cell_outline.size() && cell < 0; c++) {
    if (p.x >= B.cell_lo[c].x && p.y >= B.cell_lo[c].y && p.x <= B.cell_hi[c].x && p.y <= B.cell_hi[c].y &&
        inside(B.cell_outline[c], p))
    {
      cell = int(c);
    }
  }
  thread_local std::vector<int> from;
  from.clear();
  if (cell >= 0) {
    for (const int s : B.cell_lines[size_t(cell)]) {
      dist[size_t(s)] = bezier_nearest(B.rest[size_t(s)], p, place[size_t(s)]);
      from.push_back(s);
    }
  }
  else {
    /* Outside every cell (past the mesh's outline, or by a line of a mesh with no cells): from the nearest lines. */
    float nearest = FLT_MAX;
    for (size_t s = 0; s < lines; s++) {
      dist[s] = bezier_nearest(B.rest[s], p, place[s]);
      nearest = std::min(nearest, dist[s]);
    }
    for (size_t s = 0; s < lines; s++) {
      if (dist[s] <= nearest * 1.5f + B.size * 1e-4f) {
        from.push_back(int(s));
      }
    }
  }
  for (const int s : from) {
    for (int k = 0; k < per; k++) {
      const size_t node = size_t(s) * per + size_t(k);
      const float there = length(p - B.nodes[node]);
      for (const auto &[line, d] : B.near[node]) {
        dist[size_t(line)] = std::min(dist[size_t(line)], there + d);
      }
    }
  }
  const float nearest = *std::min_element(dist.begin(), dist.end());
  const double floor = std::max(double(nearest), double(B.size) * 1e-5);
  for (size_t s = 0; s < lines; s++) {
    if (dist[s] >= nearest + B.reach) {
      continue;
    }
    const double w = (1.0 - double(smooth01((dist[s] - nearest) / B.reach))) *
                     std::pow(floor / std::max(double(dist[s]), floor), double(power));
    if (w < 1e-9) {
      continue;
    }
    if (place[s] < 0.0f) {
      bezier_nearest(B.rest[s], p, place[s]);
    }
    r.push_back({int(s), w, place[s]});
  }
}

/* Where Points puts `p`: each line near it carrying it from its place by the line, turned as the line is there, never
 * stretched (as sides_follow). */
static float2 point_bones_map(const DeformShape &d, float2 p)
{
  const PointBones &B = *d.points;
  thread_local std::vector<PointShare> shares;
  point_shares(B, d.points_power, p, shares);
  const auto way = [](const std::array<float2, 4> &b, float t) {
    const float u = 1.0f - t;
    const float2 w = (b[1] - b[0]) * (3.0f * u * u) + (b[2] - b[1]) * (6.0f * u * t) + (b[3] - b[2]) * (3.0f * t * t);
    if (length(w) >= 1e-10f) {
      return w;
    }
    /* No handles: as it runs just along from there. */
    const float s = std::clamp(t, 0.05f, 0.95f), v = 1.0f - s;
    return (b[1] - b[0]) * (3.0f * v * v) + (b[2] - b[1]) * (6.0f * v * s) + (b[3] - b[2]) * (3.0f * s * s);
  };
  double x = 0.0, y = 0.0, total = 0.0;
  for (const PointShare &share : shares) {
    const std::array<float2, 4> &rest = B.rest[size_t(share.line)], &now = d.points_now[size_t(share.line)];
    const float2 rest_way = way(rest, share.t), now_way = way(now, share.t);
    const float rest_len = length(rest_way), now_len = length(now_way);
    const float2 on = bezier3(now, share.t);
    float2 q = on;
    if (rest_len > 1e-10f && now_len > 1e-10f) {
      const float2 off = p - bezier3(rest, share.t), rest_dir = rest_way / rest_len, now_dir = now_way / now_len;
      q = on + now_dir * dot(off, rest_dir) + float2(-now_dir.y, now_dir.x) * cross(rest_dir, off);
    }
    x += share.weight * q.x;
    y += share.weight * q.y;
    total += share.weight;
  }
  return total > 0.0 ? float2(float(x / total), float(y / total)) : p;
}

/* Points on a drawn grid: its rows' and columns' segments, each cell's four. */
static void points_build_grid(DeformShape &r, const Modifier &m)
{
  const GridSurface &g = r.rest_grid, &n = r.now_grid.across.empty() ? r.rest_grid : r.now_grid;
  const int cols = g.columns, rows = g.rows;
  if (cols <= 0 || rows <= 0 || n.columns != cols || n.rows != rows) {
    return;
  }
  std::vector<std::array<float2, 4>> rest = g.across, now = n.across;
  rest.insert(rest.end(), g.up.begin(), g.up.end());
  now.insert(now.end(), n.up.begin(), n.up.end());
  const int up = int(g.across.size());
  std::vector<std::vector<int>> cell_lines;
  std::vector<std::vector<float2>> outlines;
  for (int row = 0; row < rows; row++) {
    for (int c = 0; c < cols; c++) {
      /* Round it: along the bottom, up the right, back along the top, down the left. */
      const int bottom = row * cols + c, top = (row + 1) * cols + c;
      const int left = up + c * rows + row, right = up + (c + 1) * rows + row;
      cell_lines.push_back({bottom, right, top, left});
      std::vector<float2> outline;
      for (const auto &[line, forward] : {std::pair{bottom, true}, {right, true}, {top, false}, {left, false}}) {
        for (int k = 0; k < PointBones::STEPS; k++) {
          const float t = float(k) / float(PointBones::STEPS);
          outline.push_back(bezier3(rest[size_t(line)], forward ? t : 1.0f - t));
        }
      }
      outlines.push_back(std::move(outline));
    }
  }
  r.points = point_bones(rest, cell_lines, outlines, m.deform_falloff);
  r.points_now = std::move(now);
  r.points_power = envelope_power_of(m.deform_blend);
}

/* Points on a network drawn by hand: its segments (as the shape has them), and each cell's, the segments its outline
 * runs along. */
static void points_build_network(DeformShape &r, const Modifier &m, const VectorDrawing &rest_drawing,
                                 const Shape &rest_shape, const VectorDrawing &bent)
{
  /* As network_cells reads them: the bent drawing has the same curves. */
  std::vector<std::array<float2, 4>> rest, now;
  for (const ShapeEdge &e : rest_shape.edges) {
    if (e.curve < 0 || e.curve >= int(rest_drawing.curves.size()) || e.curve >= int(bent.curves.size())) {
      return;
    }
    int from, to;
    curve_segment_points(rest_drawing.curves[size_t(e.curve)], e.segment, &from, &to);
    if (from == to) {
      continue;
    }
    std::array<float2, 4> a, b;
    curve_segment_bezier(rest_drawing, rest_drawing.curves[size_t(e.curve)], e.segment, a.data());
    curve_segment_bezier(bent, bent.curves[size_t(e.curve)], e.segment, b.data());
    rest.push_back(a);
    now.push_back(b);
  }
  if (rest.empty()) {
    return;
  }
  std::vector<std::vector<int>> cell_lines;
  std::vector<std::vector<float2>> outlines;
  for (const DeformShape::Cell &c : r.cells) {
    std::vector<int> lines;
    for (size_t s = 0; s < rest.size(); s++) {
      if (polyline_distance(c.rest, true, bezier3(rest[s], 0.5f)) < c.size * 1e-3f) {
        lines.push_back(int(s));
      }
    }
    cell_lines.push_back(std::move(lines));
    outlines.push_back(c.rest);
  }
  r.points = point_bones(rest, cell_lines, outlines, m.deform_falloff);
  r.points_now = std::move(now);
  r.points_power = envelope_power_of(m.deform_blend);
}

/* How the cage has moved as a whole: the turn, scale and move that takes most of its outline from rest to now, each
 * sample's say going as one over how far it ends from there (iteratively reweighted, so least absolute distance): the
 * part of a cage that is being pushed about doesn't drag that along, as a least squares fit would. */
static void cage_whole_fit(DeformShape &r, float size)
{
  const size_t n = r.rest.size();
  std::vector<double> say(n, 1.0);
  for (int round = 0; round < 40; round++) {
    double total = 0.0, fx = 0.0, fy = 0.0, tx = 0.0, ty = 0.0;
    for (size_t k = 0; k < n; k++) {
      total += say[k];
      fx += say[k] * r.rest[k].x;
      fy += say[k] * r.rest[k].y;
      tx += say[k] * r.now[k].x;
      ty += say[k] * r.now[k].y;
    }
    fx /= total, fy /= total, tx /= total, ty /= total;
    double c = 0.0, s = 0.0, ee = 0.0;
    for (size_t k = 0; k < n; k++) {
      const double ex = r.rest[k].x - fx, ey = r.rest[k].y - fy, gx = r.now[k].x - tx, gy = r.now[k].y - ty;
      c += say[k] * (ex * gx + ey * gy);
      s += say[k] * (ex * gy - ey * gx);
      ee += say[k] * (ex * ex + ey * ey);
    }
    r.whole_from = float2(float(fx), float(fy));
    r.whole_to = float2(float(tx), float(ty));
    r.whole_turn = ee > 1e-20 ? float2(float(c / ee), float(s / ee)) : float2(1.0f, 0.0f);
    for (size_t k = 0; k < n; k++) {
      say[k] = 1.0 / std::max(double(length(r.now[k] - cage_whole_at(r, r.rest[k]))), double(size) * 1e-4);
    }
  }
}

static void cage_method_build(DeformShape &r)
{
  const size_t n = r.rest.size();
  if (n < 3 || r.now.size() != n) {
    return;
  }
  float2 lo = r.rest[0], hi = r.rest[0];
  for (const float2 &q : r.rest) {
    lo = float2(std::min(lo.x, q.x), std::min(lo.y, q.y));
    hi = float2(std::max(hi.x, q.x), std::max(hi.y, q.y));
  }
  const float size = std::max(hi.x - lo.x, hi.y - lo.y);
  if (size < 1e-6f) {
    return;
  }
  std::vector<std::pair<size_t, size_t>> side_samples;
  if (r.cage_method == CageMethod::Envelope) {
    /* The sides: the outline cut at its points (OUTLINE_STEPS samples to a segment). */
    const size_t steps = n % size_t(OUTLINE_STEPS) == 0 ? size_t(OUTLINE_STEPS) : n;
    for (size_t first = 0; first < n; first += steps) {
      DeformShape::EnvelopeSide side;
      for (size_t k = 0; k <= steps; k++) {
        side.rest.push_back(r.rest[(first + k) % n]);
        side.now.push_back(r.now[(first + k) % n]);
      }
      if (envelope_side_finish(side, size)) {
        r.envelope_sides.push_back(std::move(side));
        side_samples.emplace_back(first, steps + 1);
      }
    }
    /* As far as the cage is big: a band along a side keeps nearly all its width (one a tenth as wide as the cage, 90%
     * or more), and what is deep inside still mostly follows the cage as a whole. */
    r.envelope_reach = size;
    /* Range runs to no limit at all at its top, so nothing jumps as it gets there: half way, as far as the cage is big. */
    r.envelope_far = std::max(size * r.envelope_range / std::max(1.0f - r.envelope_range, 1e-4f), size * 2e-3f);
    if (r.envelope_range < 1.0f) {
      cage_whole_fit(r, size);
      for (DeformShape::EnvelopeSide &side : r.envelope_sides) {
        side.moved.resize(side.rest.size());
        for (size_t k = 0; k < side.rest.size(); k++) {
          side.moved[k] = length(side.now[k] - cage_whole_at(r, side.rest[k]));
        }
      }
    }
    /* On to the rest: the cage as a whole, under the sides, is Rigid's. */
  }
  /* Past the outline: how each corner turns (its two sides' turns, as lengths of 1: a band along the outline keeps its
   * width however the side stretches). */
  std::vector<float2> side_turn(n);
  for (size_t j = 0; j < n; j++) {
    const float2 e = r.rest[(j + 1) % n] - r.rest[j], en = r.now[(j + 1) % n] - r.now[j];
    const float2 z(dot(en, e), cross(e, en));
    side_turn[j] = length(z) > 1e-20f ? normalize(z) : float2(1.0f, 0.0f);
  }
  r.cage_turn.resize(n);
  for (size_t j = 0; j < n; j++) {
    const float2 z = side_turn[(j + n - 1) % n] + side_turn[j];
    r.cage_turn[j] = length(z) > 1e-12f ? normalize(z) : side_turn[j];
  }
  r.cage_band = size * 1e-3f;
  if (r.cage_method == CageMethod::MeanValue) {
    return;
  }
  /* Rigid (as rigid as possible, Sorkine and Alexa 2007): a lattice of square cells over the cage (those in it or
   * crossing its outline), each node keeping how it sits with its four neighbours (the square cells' own weights),
   * turned as they turn together, and the outline's samples held firmly (between the nodes round them) where the
   * outline goes now. */
  const std::shared_ptr<const RigidLattice> lattice = rigid_lattice(r.rest, lo, hi, size);
  const RigidLattice &L = *lattice;
  const float h = L.step;
  const float fade = L.fade;
  r.lattice_step = h;
  r.lattice_origin = L.origin;
  r.lattice_nx = L.nx;
  r.lattice_ny = L.ny;
  const int nx = L.nx, ny = L.ny;
  const int count = int(L.rest.size());
  if (count == 0) {
    return;
  }
  const std::vector<int> &cell = L.cell;
  const std::vector<float2> &rest = L.rest;
  const std::vector<std::vector<RigidLattice::Link>> &links = L.links;
  const std::vector<RigidLattice::Hold> &holds = L.holds;
  /* By a narrow gap mean value coordinates turn wild (weights of opposite signs on two sides nearly at one place, so
   * what lies there can be thrown off by many times the move) and change too fast for offsets over a lattice to take
   * back: a lattice that has been cut starts from the cage as a whole instead. */
  const bool cut = !L.stencil_pieces.empty();
  if (cut) {
    r.lattice_cut = lattice;
    cage_whole_fit(r, size);
  }
  if (!side_samples.empty()) {
    L.walk_sides(side_samples, size);
    if (!L.detours.empty()) {
      r.envelope_walks = lattice;
    }
  }
  /* Starting from mean value coordinates (what the offsets are measured from). */
  std::vector<float2> start(static_cast<size_t>(count)), pos(static_cast<size_t>(count));
  for (int k = 0; k < count; k++) {
    float2 q = cut ? cage_whole_at(r, rest[size_t(k)]) : rest[size_t(k)];
    const std::vector<float> &w = L.mean_value[size_t(k)];
    if (!cut && !w.empty()) {
      q = float2(0.0f, 0.0f);
      for (size_t i = 0; i < n; i++) {
        q = q + r.now[i] * w[i];
      }
    }
    start[size_t(k)] = q;
    pos[size_t(k)] = q;
  }
  constexpr float FIRM = RigidLattice::FIRM, HOLD = RigidLattice::HOLD;
  std::vector<float2> turn(static_cast<size_t>(count)), rhs(static_cast<size_t>(count));
  const auto rotate = [](float2 t, float2 e) { return float2(t.x * e.x - t.y * e.y, t.y * e.x + t.x * e.y); };
  for (int iteration = 0; iteration < 20; iteration++) {
    /* Each node's best turn of how its neighbours sit around it. */
    for (int k = 0; k < count; k++) {
      float c = 0.0f, s = 0.0f;
      for (const RigidLattice::Link &l : links[size_t(k)]) {
        const float2 e = rest[size_t(k)] - rest[size_t(l.to)], f = pos[size_t(k)] - pos[size_t(l.to)];
        c += l.weight * dot(e, f);
        s += l.weight * cross(e, f);
      }
      const float len = std::sqrt(c * c + s * s);
      turn[size_t(k)] = len > 1e-20f ? float2(c, s) / len : float2(1.0f, 0.0f);
    }
    /* Then where the nodes go to sit so (conjugate gradients from where they are). */
    for (int k = 0; k < count; k++) {
      float2 b = start[size_t(k)] * HOLD;
      for (const RigidLattice::Link &l : links[size_t(k)]) {
        const float2 e = rest[size_t(k)] - rest[size_t(l.to)];
        b = b + (rotate(turn[size_t(k)], e) + rotate(turn[size_t(l.to)], e)) * (0.5f * l.weight);
      }
      rhs[size_t(k)] = b;
    }
    for (const RigidLattice::Hold &hold : holds) {
      for (int c = 0; c < 4; c++) {
        float2 &out = rhs[size_t(hold.node[size_t(c)])];
        out = out + r.now[hold.sample] * (FIRM * hold.weight[size_t(c)]);
      }
    }
    /* Solved exactly, by the lattice's factor (conjugate gradients, a fixed number of steps from where the nodes were,
     * took nearly all the build's time). */
    L.solve(rhs, pos);
  }
  /* The offsets onto the lattice; past the nodes, carried on outward as they were at the edge of them, fading out
   * over a few steps, so the B-spline over the lattice joins them to nothing smoothly. */
  r.lattice.assign(static_cast<size_t>(nx * ny), float2(0.0f, 0.0f));
  std::vector<char> known(static_cast<size_t>(nx * ny), 0);
  for (int k = 0; k < count; k++) {
    /* Where nodes stand apart they are read from the stencils below. */
    if (L.cut_at.empty() || !L.cut_at[size_t(cell[size_t(k)])]) {
      r.lattice[size_t(cell[size_t(k)])] = pos[size_t(k)] - start[size_t(k)];
      known[size_t(cell[size_t(k)])] = 1;
    }
  }
  for (int ring = 0; ring < int(fade / h); ring++) {
    const std::vector<char> was = known;
    for (int j = 0; j < ny; j++) {
      for (int i = 0; i < nx; i++) {
        if (was[size_t(j * nx + i)]) {
          continue;
        }
        float2 sum(0.0f, 0.0f);
        int around = 0;
        for (int b = std::max(j - 1, 0); b <= std::min(j + 1, ny - 1); b++) {
          for (int a = std::max(i - 1, 0); a <= std::min(i + 1, nx - 1); a++) {
            if (was[size_t(b * nx + a)]) {
              sum = sum + r.lattice[size_t(b * nx + a)];
              around++;
            }
          }
        }
        if (around > 0) {
          r.lattice[size_t(j * nx + i)] = sum * (0.92f / float(around));
          known[size_t(j * nx + i)] = 1;
        }
      }
    }
  }
  if (L.stencil_pieces.empty()) {
    return;
  }
  /* What each piece by a gap reads: its side's nodes, the plain lattice where nothing stands apart, and where neither
   * is there, carried on from around it as past the nodes above. */
  r.lattice_stencils.resize(L.stencil_pieces.size());
  std::vector<char> has_node(static_cast<size_t>(nx * ny), 0);
  for (const int at : cell) {
    has_node[size_t(at)] = 1;
  }
  for (size_t s = 0; s < L.stencil_pieces.size(); s++) {
    const RigidLattice::Piece &piece = L.pieces[size_t(L.stencil_pieces[s])];
    std::array<float2, 16> &stencil = r.lattice_stencils[s];
    std::array<char, 16> have{};
    for (int b = 0; b < 4; b++) {
      for (int a = 0; a < 4; a++) {
        const size_t k = size_t(b * 4 + a);
        const int i = piece.i - 1 + a, j = piece.j - 1 + b;
        stencil[k] = float2(0.0f, 0.0f);
        if (piece.nodes[k] >= 0) {
          stencil[k] = pos[size_t(piece.nodes[k])] - start[size_t(piece.nodes[k])];
          have[k] = 1;
        }
        /* A node there it isn't joined to is the other side's. */
        else if (i >= 0 && i < nx && j >= 0 && j < ny && !L.cut_at[size_t(j * nx + i)] && !has_node[size_t(j * nx + i)]) {
          stencil[k] = r.lattice[size_t(j * nx + i)];
          have[k] = 1;
        }
      }
    }
    for (int ring = 0; ring < 3; ring++) {
      const std::array<char, 16> was = have;
      for (int b = 0; b < 4; b++) {
        for (int a = 0; a < 4; a++) {
          if (was[size_t(b * 4 + a)]) {
            continue;
          }
          float2 sum(0.0f, 0.0f);
          int around = 0;
          for (int y = std::max(b - 1, 0); y <= std::min(b + 1, 3); y++) {
            for (int x = std::max(a - 1, 0); x <= std::min(a + 1, 3); x++) {
              if (was[size_t(y * 4 + x)]) {
                sum = sum + stencil[size_t(y * 4 + x)];
                around++;
              }
            }
          }
          if (around > 0) {
            stencil[size_t(b * 4 + a)] = sum * (0.92f / float(around));
            have[size_t(b * 4 + a)] = 1;
          }
        }
      }
    }
  }
}

static float2 deform_map(const DeformShape &d, float2 p, std::vector<float> &scratch)
{
  switch (d.kind) {
    case DeformKind::Grid:
    case DeformKind::Mesh:
    case DeformKind::Wire: {
      if (d.points) {
        return point_bones_map(d, p);
      }
      if (!d.wires.empty() && (d.cells.empty() || outside_mesh(d, p))) {
        return wires_follow(d, p, scratch);
      }
      /* On a four-sided cell's side: onto the bent side, at the same place along it. */
      for (const DeformShape::Cell &c : d.cells) {
        float2 st;
        if (c.quad && quad_cell_at(c, p, st)) {
          for (int k = 0; k < 4; k++) {
            float t;
            if (cell_side_at(c, k, p, t)) {
              return mesh_envelope(d, p, cell_side_point(c.now_sides, k, t), c.outer);
            }
          }
        }
      }
      if (outside_mesh(d, p)) {
        return d.edge_sides.empty() ? outline_follow(d, p, scratch) : sides_follow(d, p, scratch);
      }
      bool outer = false;
      const float2 q = mesh_inside_map(d, p, scratch, &outer);
      return mesh_envelope(d, p, q, outer);
    }
    case DeformKind::Cage: {
      if (d.cage_method == CageMethod::Envelope && !d.envelope_sides.empty()) {
        return envelope_map(d, p, scratch);
      }
      if (!d.cage_turn.empty() && !inside(d.rest, p)) {
        return outline_carry(d.rest, d.now, d.cage_turn, p, polyline_distance(d.rest, true, p), d.cage_band);
      }
      if (d.lattice_cut) {
        return cage_whole_at(d, p) + rigid_offset(d, p);
      }
      if (!mean_value(d.rest, p, scratch)) {
        return p;
      }
      float2 q(0.0f, 0.0f);
      for (size_t i = 0; i < d.now.size(); i++) {
        q = q + d.now[i] * scratch[i];
      }
      if (d.cage_method == CageMethod::Rigid && !d.lattice.empty()) {
        q = q + rigid_offset(d, p);
      }
      return q;
    }
    case DeformKind::Line: {
      /* Carried from one place on the line: its place along and across the line, worked out on the line at rest and
       * now alike (the same frame and bend easing), and how far the point is from where that puts it at rest carried
       * along in the line's frame, so a line that hasn't moved moves nothing and one moved or turned whole carries
       * everything exactly with it. */
      const auto carry = [&](const LinePlace &place) {
        const float s = d.stretch ? place.along * d.now_at.back() / d.rest_at.back() : place.along;
        /* The way it runs, for a point far from it, taken over a stretch about as long as the point is far (the chord
         * across it): the line's samples turn by small corners, and turning a far point by each one at once made it
         * hitch as the line moved. Near the line, exactly the way it runs there. */
        const float reach = std::min(std::fabs(place.across) * 0.5f, d.rest_at.back() * 0.25f);
        const float rest_spacing = d.rest_at.back() / float(std::max<size_t>(d.rest.size() - 1, 1));
        const auto frame = [reach, rest_spacing](const std::vector<float2> &v, const std::vector<float> &at, float here,
                                                 float scale, float2 &r_on, float2 &r_tangent, float2 &r_normal) {
          line_frame_at(v, at, here, r_on, r_tangent, r_normal);
          /* The same at rest and now, however many samples each has. */
          const float spacing = rest_spacing * scale;
          const float h = reach * scale;
          if (h > spacing * 0.5f) {
            float2 a, b, ta, na, tb, nb;
            line_frame_at(v, at, here - h, a, ta, na);
            line_frame_at(v, at, here + h, b, tb, nb);
            if (length(b - a) > 1e-12f) {
              /* Where the line has doubled back the chord is short, and flips as the line passes through itself: there
               * the way it runs here. */
              const float f = smooth01(h / (4.0f * spacing)) * smooth01(length(b - a) / (0.2f * h));
              r_tangent = normalize(r_tangent + (normalize(b - a) - r_tangent) * f);
              r_normal = float2(-r_tangent.y, r_tangent.x);
            }
          }
        };
        float2 on, tangent, normal, rest_on, rest_tangent, rest_normal;
        frame(d.now, d.now_at, s, d.stretch ? d.now_at.back() / d.rest_at.back() : 1.0f, on, tangent, normal);
        frame(d.rest, d.rest_at, place.along, 1.0f, rest_on, rest_tangent, rest_normal);
        const float2 now_place = on + normal * across_in_bend(d.now, d.now_at, s, place.across);
        const float2 rest_place = rest_on + rest_normal * across_in_bend(d.rest, d.rest_at, place.along, place.across);
        const float2 left = p - rest_place;
        return now_place + tangent * dot(left, rest_tangent) + normal * dot(left, rest_normal);
      };
      /* Where the line winds so that the point has a near place on more than one part of it, carried from each and
       * blended by nearness (the eighth power), as a wire's points are: a point about as near two parts of the line
       * eases from one to the other rather than tearing the drawing apart where the nearest one changes. */
      std::vector<float> dists;
      const std::vector<LinePlace> places = line_places(d.rest, d.rest_at, p, dists);
      if (places.size() == 1) {
        return carry(places[0]);
      }
      const float nearest = *std::min_element(dists.begin(), dists.end());
      float2 sum(0.0f, 0.0f);
      double weight = 0.0;
      for (size_t i = 0; i < places.size(); i++) {
        if (dists[i] <= nearest * (1.0f + 1e-6f) && nearest < 1e-9f) {
          return carry(places[i]);
        }
        const double w = std::pow(double(nearest) / std::max(double(dists[i]), 1e-30), 8.0);
        sum = sum + carry(places[i]) * float(w);
        weight += w;
      }
      return weight > 0.0 ? sum / float(weight) : carry(places[0]);
    }
    case DeformKind::None:
      break;
  }
  return p;
}

static float polyline_distance(const std::vector<float2> &v, bool closed, float2 p)
{
  float best = FLT_MAX;
  const size_t n = v.size();
  for (size_t i = 0; i + (closed ? 0 : 1) < n; i++) {
    const float2 a = v[i], d = v[(i + 1) % n] - a;
    const float dd = dot(d, d);
    const float t = dd > 1e-20f ? std::clamp(dot(p - a, d) / dd, 0.0f, 1.0f) : 0.0f;
    best = std::min(best, length(a + d * t - p));
  }
  return best;
}

static bool inside(const std::vector<float2> &v, float2 p)
{
  int winding = 0;
  const size_t n = v.size();
  for (size_t i = 0; i < n; i++) {
    const float2 a = v[i], b = v[(i + 1) % n];
    if (a.y <= p.y) {
      winding += b.y > p.y && cross(b - a, p - a) > 0.0f ? 1 : 0;
    }
    else {
      winding -= b.y <= p.y && cross(b - a, p - a) < 0.0f ? 1 : 0;
    }
  }
  return winding != 0;
}

static const PointGroup *deform_group(const Layer &layer, const Modifier &m)
{
  if (!m.deform_custom || m.deform_group.empty()) {
    return nullptr;
  }
  for (const PointGroup &g : layer.drawing.groups) {
    if (g.name == m.deform_group) {
      return &g;
    }
  }
  return nullptr;
}

/* A point's weight: its Custom one, else the automatic one, which is full for every point of the layer (the
 * whole drawing follows the shape, past it too, as Blender's deform modifiers bend the whole object). */
static float point_weight(const PointGroup *group, const MeshPoint &p)
{
  if (group && p.id != 0) {
    if (auto it = group->weights.find(p.id); it != group->weights.end()) {
      return std::clamp(it->second, 0.0f, 1.0f);
    }
  }
  return 1.0f;
}

bool vector_deform_takes_part(const Document &doc, const Modifier &m)
{
  const Layer *target = layer_find(doc, m.deform_layer);
  const int index = target && target->has_drawing() ? drawing_shape_index(target->drawing, m.deform_shape) : -1;
  if (index < 0 || shape_grid(target->drawing, target->drawing.shapes[size_t(index)])) {
    return false;
  }
  std::vector<OrientedEdge> chain;
  bool closed = false;
  return shape_chain(target->drawing, target->drawing.shapes[size_t(index)], chain, &closed);
}

bool vector_deform_part_from_selection(Document &doc, Modifier &m)
{
  if (!vector_deform_takes_part(doc, m)) {
    return false;
  }
  VectorDrawing &drawing = layer_find(doc, m.deform_layer)->drawing;
  drawing_ensure_ids(drawing);
  std::vector<OrientedEdge> chain;
  bool closed = false;
  shape_chain(drawing, drawing.shapes[size_t(drawing_shape_index(drawing, m.deform_shape))], chain, &closed);
  /* The points along the shape, and the selected ones' one run among them. */
  const size_t n = chain.size() + (closed ? 0 : 1);
  const auto id_at = [&](size_t i) { return drawing.points[size_t(chain_point(drawing, chain, i))].id; };
  const auto selected = [&](size_t i) { return drawing.points[size_t(chain_point(drawing, chain, i))].selected; };
  size_t count = 0, starts = 0, first = 0;
  for (size_t i = 0; i < n; i++) {
    if (selected(i)) {
      count++;
      const bool after_selected = i > 0 ? selected(i - 1) : closed && selected(n - 1);
      if (!after_selected) {
        starts++;
        first = i;
      }
    }
  }
  if (count < 2 || starts != 1 || (closed && count == n)) {
    return false;
  }
  m.deform_part_from = id_at(first);
  m.deform_part_to = id_at((first + count - 1) % n);
  return true;
}

ModifierType vector_deform_fitting_type(const Document &doc, const Layer &layer, const Modifier &m)
{
  Modifier test = m;
  for (const ModifierType type :
       {m.type, ModifierType::CurveDeform, ModifierType::CageDeform, ModifierType::MeshDeform})
  {
    test.type = type;
    DeformShape d;
    if (deform_shape_build(doc, layer, test, false, false, d)) {
      return type;
    }
  }
  return m.type;
}

void vector_deform_resolve_legacy(Document &doc)
{
  const auto visit = [&](auto &&self, std::vector<Layer> &list) -> void {
    for (Layer &layer : list) {
      for (Modifier &m : layer.modifiers) {
        if (!modifier_is_deform(m.type) || !m.deform_legacy) {
          continue;
        }
        m.deform_legacy = false;
        const bool default_name = m.name.rfind("Vector Deform", 0) == 0;
        /* As the single Vector Deform took them: a line, else a single closed shape as a cage, else cells. */
        for (const ModifierType type :
             {ModifierType::CurveDeform, ModifierType::CageDeform, ModifierType::MeshDeform})
        {
          m.type = type;
          if (vector_deform_kind(doc, layer, m) != DeformKind::None) {
            break;
          }
        }
        if (default_name) {
          m.name = deform_type_name(m.type) + m.name.substr(13);
        }
      }
      self(self, layer.children);
    }
  };
  visit(visit, doc.layers);
}

bool vector_deform_drawn_grid(const Document &doc, const Layer &layer, const Modifier &m)
{
  DeformShape d;
  return deform_shape_build(doc, layer, m, false, false, d) && d.kind == DeformKind::Grid && d.cells.empty();
}

DeformKind vector_deform_kind(const Document &doc, const Layer &layer, const Modifier &m)
{
  DeformShape d;
  return deform_shape_build(doc, layer, m, false, false, d) ? d.kind : DeformKind::None;
}

/* The deform as kept (deform_shape_kept compares modifiers): the point isn't part of what's built, so moving it builds
 * nothing. */
static Modifier follow_for_build(const Modifier &m)
{
  Modifier run = m;
  run.deform_point = run.deform_point2 = run.deform_shift = run.deform_shift2 = {};
  run.deform_shift_keys = {};
  run.deform_shift2_keys = {};
  run.deform_show_point = false;
  run.deform_point_id = run.deform_point2_id = 0;
  return run;
}

/* Where its two points rest and are now, in the layer's space: one picked on the shape exactly as the shape has it, its
 * own through the deform. `posed` is what the bones have done to a rest that moves with them. A picked point that's gone
 * stays where it was. False with nothing to read. */
static bool follow_places(const Document &doc, const Layer &layer, const Modifier &m, float2 posed, bool render,
                          std::array<float2, 2> &r_rest, std::array<float2, 2> &r_now)
{
  r_rest = {m.deform_point + posed, m.deform_point2 + posed};
  r_now = r_rest;
  if (vector_deform_loops(doc, layer, m)) {
    return false;
  }
  const std::array<uint64_t, 2> ids = {m.deform_point_id, m.deform_point2_id};
  const std::array<float2, 2> shifts = {m.deform_shift, m.deform_shift2};
  VectorDrawing rest, bent;
  const bool read = (ids[0] != 0 || ids[1] != 0) && deform_shape_inputs(doc, layer, m, true, render, rest, bent);
  std::shared_ptr<const DeformShape> built;
  uint64_t build = 0;
  if (doc.frame_current != 0 && m.deform_strength > 0.0f && (ids[0] == 0 || (m.deform_two && ids[1] == 0))) {
    built = deform_shape_kept(doc, layer, follow_for_build(m), render, build);
  }
  std::vector<float> scratch;
  for (const int i : {0, 1}) {
    if (ids[size_t(i)] != 0) {
      const int at_rest = read ? drawing_point_index(rest, ids[size_t(i)]) : -1;
      const int at_now = read ? drawing_point_index(bent, ids[size_t(i)]) : -1;
      if (at_rest >= 0 && at_now >= 0) {
        r_rest[size_t(i)] = rest.points[size_t(at_rest)].pos;
        r_now[size_t(i)] = bent.points[size_t(at_now)].pos;
      }
    }
    else if (built) {
      r_now[size_t(i)] = deform_map(*built, r_rest[size_t(i)] + shifts[size_t(i)], scratch);
    }
  }
  return read || built;
}

Transform2D vector_deform_follow(const Document &doc, const Layer &layer, const Modifier &m, float2 carry, bool render)
{
  Transform2D t;
  if (doc.frame_current == 0 || m.deform_strength <= 0.0f || vector_deform_loops(doc, layer, m)) {
    return t;
  }
  std::array<float2, 2> rest, now;
  if (!follow_places(doc, layer, m, m.deform_rest_posed ? carry : float2(), render, rest, now)) {
    return t;
  }
  const float strength = std::clamp(m.deform_strength, 0.0f, 1.0f);
  const float2 move = (now[0] - rest[0]) * strength;
  float angle = 0.0f, size = 1.0f;
  if (m.deform_two) {
    const float2 was = rest[1] - rest[0];
    const float2 is = now[1] - now[0];
    if (length(was) > 1e-6f && length(is) > 1e-6f) {
      if (m.deform_rotation) {
        angle = std::atan2(was.x * is.y - was.y * is.x, dot(was, is)) * strength;
      }
      if (m.deform_scale) {
        size = 1.0f + (length(is) / length(was) - 1.0f) * strength;
      }
    }
  }
  /* Round the point, where the bones have put it. */
  const float2 pivot = rest[0] + (m.deform_rest_posed ? float2() : carry);
  t.a = std::cos(angle) * size;
  t.b = std::sin(angle) * size;
  t.c = -t.b;
  t.d = t.a;
  const float2 turned = t.apply_vector(pivot);
  t.tx = pivot.x + move.x - turned.x;
  t.ty = pivot.y + move.y - turned.y;
  return t;
}

float2 vector_deform_follow_rest(const Document &doc, const Layer &layer, const Modifier &m, bool second)
{
  std::array<float2, 2> rest, now;
  follow_places(doc, layer, m, {}, false, rest, now);
  return rest[second ? 1 : 0];
}

float2 vector_deform_follow_now(const Document &doc, const Layer &layer, const Modifier &m, bool second)
{
  std::array<float2, 2> rest, now;
  follow_places(doc, layer, m, {}, false, rest, now);
  return now[second ? 1 : 0];
}

float2 vector_deform_follow_shift(const Document &doc, const Layer &layer, const Modifier &m, bool second, float2 to)
{
  const float2 shift = second ? m.deform_shift2 : m.deform_shift;
  const std::shared_ptr<const DeformShape> built = vector_deform_shape(doc, layer, follow_for_build(m));
  if (!built || layer.drawing.points.empty()) {
    return shift;
  }
  std::vector<float> scratch;
  const auto bent = [&](float2 at) { return deform_map(*built, at, scratch); };
  /* What the rest of the stack adds on top of the deform: its bones, say. */
  const float2 above = layer_places_shown(doc, layer, {{0, m.deform_point}})[0] - bent(m.deform_point + m.deform_shift);
  const float2 target = to - above;
  const float2 base = second ? m.deform_point2 : m.deform_point;
  float2 at = base + shift;
  const float step = 0.002f;
  for (int i = 0; i < 8; i++) {
    const float2 here = bent(at);
    const float2 off = target - here;
    if (length(off) < 1e-5f) {
      break;
    }
    const float2 x = (bent(at + float2(step, 0.0f)) - here) / step;
    const float2 y = (bent(at + float2(0.0f, step)) - here) / step;
    const float det = x.x * y.y - x.y * y.x;
    float2 move = std::abs(det) > 1e-4f ? float2((y.y * off.x - y.x * off.y) / det, (x.x * off.y - x.y * off.x) / det) : off;
    if (length(move) > 0.2f) {
      move = normalize(move) * 0.2f;
    }
    at = at + move;
  }
  return at - base;
}

void vector_deform_follow_two(Modifier &m, bool on)
{
  m.deform_two = on;
  if (on && length(m.deform_point2 - m.deform_point) < 1e-6f) {
    m.deform_point2 = m.deform_point + float2(0.1f, 0.0f);
  }
}

void vector_deform_follow_pick(const Document &doc, const Layer &layer, Modifier &m, bool second, uint64_t id)
{
  (second ? m.deform_point2 : m.deform_point) = vector_deform_follow_rest(doc, layer, m, second);
  (second ? m.deform_point2_id : m.deform_point_id) = id;
  if (id != 0) {
    (second ? m.deform_point2 : m.deform_point) = vector_deform_follow_rest(doc, layer, m, second);
  }
}

bool vector_deform_shape_read(const Document &doc, const Layer &layer, const Modifier &m, VectorDrawing &r_rest,
                              VectorDrawing &r_bent)
{
  return !vector_deform_loops(doc, layer, m) && deform_shape_inputs(doc, layer, m, true, false, r_rest, r_bent);
}

std::shared_ptr<const DeformShape> vector_deform_shape(const Document &doc, const Layer &layer, const Modifier &m)
{
  if (doc.frame_current == 0 || m.deform_strength <= 0.0f || vector_deform_loops(doc, layer, m)) {
    return nullptr;
  }
  uint64_t build = 0;
  return deform_shape_kept(doc, layer, m, false, build);
}

float2 vector_deform_point(const DeformShape *shape, const Modifier &m, float2 at)
{
  if (!shape) {
    return at;
  }
  std::vector<float> scratch;
  return at + (deform_map(*shape, at, scratch) - at) * std::clamp(m.deform_strength, 0.0f, 1.0f);
}

/* A line running into a cage from its outline, for the viewport's views of an Envelope: from rest sample `sample`. */
struct EnvelopeViewLine {
  size_t sample;
  float2 way;
};

/* `m`'s Envelope cage, if it is one, and the lines a view of it runs in from its outline: one from each sample, and more
 * at a corner, or they would leave a wedge between them. `r_size`: how big the cage is. */
static std::shared_ptr<const DeformShape> envelope_view_lines(const Document &doc, const Layer &layer,
                                                              const Modifier &m, std::vector<EnvelopeViewLine> &r,
                                                              float &r_size)
{
  uint64_t build = 0;
  std::shared_ptr<const DeformShape> shape =
      vector_deform_loops(doc, layer, m) ? nullptr : deform_shape_kept(doc, layer, m, false, build);
  if (!shape || shape->kind != DeformKind::Cage || shape->cage_method != CageMethod::Envelope) {
    return nullptr;
  }
  const std::vector<float2> &v = shape->rest;
  const size_t n = v.size();
  double twice_area = 0.0;
  float2 lo = v[0], hi = v[0];
  for (size_t k = 0; k < n; k++) {
    twice_area += double(cross(v[k], v[(k + 1) % n]));
    lo = float2(std::min(lo.x, v[k].x), std::min(lo.y, v[k].y));
    hi = float2(std::max(hi.x, v[k].x), std::max(hi.y, v[k].y));
  }
  r_size = std::max(hi.x - lo.x, hi.y - lo.y);
  const auto inward = [&](float2 way) {
    way = normalize(way);
    return twice_area > 0.0 ? float2(-way.y, way.x) : float2(way.y, -way.x);
  };
  r.clear();
  for (size_t k = 0; k < n; k++) {
    const float2 before = inward(v[k] - v[(k + n - 1) % n]), after = inward(v[(k + 1) % n] - v[k]);
    const float turn = std::atan2(cross(before, after), dot(before, after));
    const int fans = std::clamp(int(std::fabs(turn) / 0.25f), 1, 8);
    for (int f = 0; f < fans; f++) {
      const float t = turn * (float(f) + 0.5f) / float(fans);
      const float c = std::cos(t), s = std::sin(t);
      r.push_back({k, float2(before.x * c - before.y * s, before.y * c + before.x * s)});
    }
  }
  return shape;
}

bool vector_deform_range_view(const Document &doc, const Layer &layer, const Modifier &m, DeformRangeView &r)
{
  r = {};
  std::vector<EnvelopeViewLine> lines;
  float size = 0.0f;
  const std::shared_ptr<const DeformShape> shape = envelope_view_lines(doc, layer, m, lines, size);
  if (!shape) {
    return false;
  }
  const DeformShape &d = *shape;
  const bool bent = doc.frame_current != 0 && m.deform_strength > 0.0f;
  const std::vector<float2> &v = d.rest;
  constexpr int STEPS = 16;
  std::vector<std::vector<int>> rows(lines.size());
  std::vector<char> at_limit(lines.size(), 0);
  for (size_t l = 0; l < lines.size(); l++) {
    const float2 from = v[lines[l].sample];
    /* As Range bends: further where a place has moved further. */
    const float moved = d.envelope_range < 1.0f ?
                            length(d.now[lines[l].sample] - cage_whole_at(d, from)) :
                            0.0f;
    const float far = std::max(d.envelope_far, moved * 2.0f), go = std::min(far, size);
    for (int step = 0; step <= STEPS; step++) {
      const float t = go * float(step) / float(STEPS);
      const float2 p = from + lines[l].way * t;
      /* Each line stops where another side is nearer: what it would reach past there isn't this side's to reach, and
       * this way it can't run out of the cage or over a gap. */
      if (step > 0 && (!inside(v, p) || polyline_distance(v, true, p) < t * 0.95f)) {
        break;
      }
      rows[l].push_back(int(r.at.size()));
      r.at.push_back(bent ? vector_deform_point(&d, m, p) : p);
      r.say.push_back(1.0f - smooth01(t / far));
      at_limit[l] = step == STEPS && far <= size;
    }
  }
  for (size_t l = 0; l < lines.size(); l++) {
    const size_t next = (l + 1) % lines.size();
    const size_t both = std::min(rows[l].size(), rows[next].size());
    for (size_t k = 0; k + 1 < both; k++) {
      r.quads.push_back({rows[l][k], rows[next][k], rows[next][k + 1], rows[l][k + 1]});
    }
    if (at_limit[l] && at_limit[next]) {
      r.limit.push_back({rows[l].back(), rows[next].back()});
    }
  }
  return true;
}

bool vector_deform_blend_view(const Document &doc, const Layer &layer, const Modifier &m, DeformRangeView &r)
{
  r = {};
  std::vector<EnvelopeViewLine> lines;
  float size = 0.0f;
  const std::shared_ptr<const DeformShape> shape = envelope_view_lines(doc, layer, m, lines, size);
  if (!shape) {
    return false;
  }
  const DeformShape &d = *shape;
  const bool bent = doc.frame_current != 0 && m.deform_strength > 0.0f;
  const bool ranged = d.envelope_range < 1.0f;
  const std::vector<float2> &v = d.rest;
  const size_t n = d.envelope_sides.size();
  std::vector<float> dist(n);
  std::vector<SidePlace> places;
  /* How much a place is shared: the next side's share over the leader's (1 where two have an even say: only the two,
   * or deep in a cage, where every side is about as near, it would all be shared), as much as the sides have a say
   * there at all (as they set a place back: by reach, and Range). */
  const auto shared_at = [&](float2 p) {
    std::array<int, 4> walk_node;
    std::array<float, 4> walk_share;
    const bool walked = envelope_walk_at(d, p, walk_node, walk_share);
    float near = FLT_MAX, within = 1.0f;
    for (size_t s = 0; s < n; s++) {
      const DeformShape::EnvelopeSide &side = d.envelope_sides[s];
      float straight = 1.0f;
      dist[s] = side_places(side.rest, side.rest_way, side.rest_at.back(), p, places, &straight);
      if (dist[s] < near) {
        near = dist[s];
        if (ranged) {
          float moved = 0.0f;
          for (const SidePlace &place : places) {
            moved += (side.moved[place.piece] * (1.0f - place.t) + side.moved[place.piece + 1] * place.t) * place.say;
          }
          within = 1.0f - smooth01(dist[s] / std::max(d.envelope_far, moved * 2.0f));
        }
      }
    }
    double lead = 0.0, second = 0.0;
    for (size_t s = 0; s < n; s++) {
      const double w = envelope_side_share(d, s, dist[s], near, walked ? &walk_node : nullptr, walk_share);
      second = std::max(second, std::min(lead, w));
      lead = std::max(lead, w);
    }
    const float even = lead > 0.0 ? float(second / lead) : 0.0f;
    return even * d.envelope_reach / (d.envelope_reach + near) * within;
  };
  /* Each line runs in until another side of the outline is nearer, found closely: that is where two sides hand over
   * most evenly, so the view has to reach it from both. */
  constexpr int SEEK = 32, STEPS = 16;
  const auto stops = [&](size_t l, float t) {
    const float2 p = v[lines[l].sample] + lines[l].way * t;
    return !inside(v, p) || polyline_distance(v, true, p) < t * 0.99f;
  };
  std::vector<int> row_first(lines.size());
  for (size_t l = 0; l < lines.size(); l++) {
    float lo = 0.0f, hi = size;
    for (int step = 1; step <= SEEK; step++) {
      const float t = size * float(step) / float(SEEK);
      if (stops(l, t)) {
        hi = t;
        break;
      }
      lo = t;
    }
    for (int halve = 0; halve < 10 && hi > lo; halve++) {
      const float mid = 0.5f * (lo + hi);
      (stops(l, mid) ? hi : lo) = mid;
    }
    row_first[l] = int(r.at.size());
    for (int step = 0; step <= STEPS; step++) {
      const float2 p = v[lines[l].sample] + lines[l].way * (lo * float(step) / float(STEPS));
      r.at.push_back(bent ? vector_deform_point(&d, m, p) : p);
      r.say.push_back(std::clamp(shared_at(p), 0.0f, 1.0f));
    }
  }
  for (size_t l = 0; l < lines.size(); l++) {
    const int a = row_first[l], b = row_first[(l + 1) % lines.size()];
    for (int k = 0; k < STEPS; k++) {
      r.quads.push_back({a + k, b + k, b + k + 1, a + k + 1});
    }
  }
  return true;
}

bool vector_deform_falloff_view(const Document &doc, const Layer &layer, const Modifier &m, DeformRangeView &r)
{
  r = {};
  uint64_t build = 0;
  const std::shared_ptr<const DeformShape> shape =
      vector_deform_loops(doc, layer, m) ? nullptr : deform_shape_kept(doc, layer, m, false, build);
  if (!shape || !shape->points || shape->points->cell_outline.empty()) {
    return false;
  }
  const DeformShape &d = *shape;
  const PointBones &B = *d.points;
  const bool bent = doc.frame_current != 0 && m.deform_strength > 0.0f;
  float2 lo = B.cell_lo[0], hi = B.cell_hi[0];
  for (size_t c = 0; c < B.cell_lo.size(); c++) {
    lo = float2(std::min(lo.x, B.cell_lo[c].x), std::min(lo.y, B.cell_lo[c].y));
    hi = float2(std::max(hi.x, B.cell_hi[c].x), std::max(hi.y, B.cell_hi[c].y));
  }
  /* A grid of places over the mesh, those in its cells shaded by how evenly the lines share them (the next line's share
   * over the leader's, as Blend's view): the wider Falloff, the further they share. */
  constexpr int ACROSS = 64;
  const float step = std::max(hi.x - lo.x, hi.y - lo.y) / float(ACROSS);
  const int nx = int((hi.x - lo.x) / step) + 2, ny = int((hi.y - lo.y) / step) + 2;
  std::vector<int> index(size_t(nx * ny), -1);
  std::vector<PointShare> shares;
  for (int j = 0; j < ny; j++) {
    for (int i = 0; i < nx; i++) {
      const float2 p = lo + float2(float(i), float(j)) * step;
      bool in = false;
      for (size_t c = 0; c < B.cell_outline.size() && !in; c++) {
        in = inside(B.cell_outline[c], p);
      }
      if (!in) {
        continue;
      }
      point_shares(B, d.points_power, p, shares);
      double lead = 0.0, second = 0.0;
      for (const PointShare &share : shares) {
        second = std::max(second, std::min(lead, share.weight));
        lead = std::max(lead, share.weight);
      }
      index[size_t(j * nx + i)] = int(r.at.size());
      r.at.push_back(bent ? vector_deform_point(&d, m, p) : p);
      r.say.push_back(lead > 0.0 ? float(second / lead) : 0.0f);
    }
  }
  for (int j = 0; j + 1 < ny; j++) {
    for (int i = 0; i + 1 < nx; i++) {
      const int a = index[size_t(j * nx + i)], b = index[size_t(j * nx + i + 1)],
                c = index[size_t((j + 1) * nx + i + 1)], e = index[size_t((j + 1) * nx + i)];
      if (a >= 0 && b >= 0 && c >= 0 && e >= 0) {
        r.quads.push_back({a, b, c, e});
      }
    }
  }
  return true;
}

bool vector_deform_loops(const Document &doc, const Layer &layer, const Modifier &m)
{
  /* Follow the shapes' layers' own deform modifiers back; reaching `layer` is a loop. */
  std::vector<uint64_t> seen;
  std::vector<uint64_t> todo = {m.deform_layer};
  while (!todo.empty()) {
    const uint64_t id = todo.back();
    todo.pop_back();
    if (id == layer.id) {
      return true;
    }
    if (std::find(seen.begin(), seen.end(), id) != seen.end()) {
      continue;
    }
    seen.push_back(id);
    if (const Layer *l = layer_find(doc, id)) {
      for (const Modifier &other : l->modifiers) {
        if (modifier_is_deform(other.type) && other.deform_layer != 0) {
          todo.push_back(other.deform_layer);
        }
      }
    }
  }
  return false;
}

/* A handle bent by the deform as it bends right at its point (its way and stretch there: a tiny step along the
 * handle and back, taken through the deform and scaled back up), so the pieces either side of a point leave it the
 * same way and a bent line has no jogs where its pieces meet. `q0`: where the point went. */
static float2 bent_handle(const DeformShape &d, float2 p0, float2 q0, float2 handle, std::vector<float> &scratch)
{
  const float len = length(handle);
  if (len < 1e-9f) {
    return handle;
  }
  /* At a point in a four-sided cell or on its sides: turned and stretched as the cell's patch does there, from its
   * own derivatives (a step either way would leave the cell at its sides and corners), so a line drawn on a side
   * leaves its points exactly as the bent side does. */
  /* (Points carries by the lines, not the cells' patches.) */
  static const std::vector<DeformShape::Cell> no_cells;
  for (const DeformShape::Cell &c : d.points ? no_cells : d.cells) {
    float2 st;
    if (!c.quad || !quad_cell_at(c, p0, st)) {
      continue;
    }
    /* Where Envelope's sides have a say it isn't the patch that places it. */
    if (c.outer && d.envelope_mesh) {
      break;
    }
    /* Along a side it sits on: as the bent side runs there (the handle's share of the side's own way, kept). */
    for (int k = 0; k < 4; k++) {
      float t;
      if (!cell_side_at(c, k, p0, t)) {
        continue;
      }
      const float2 rest_way = cell_side_way(c.rest_sides, k, t);
      const float way_len = length(rest_way);
      if (way_len < 1e-12f || std::fabs(cross(handle, rest_way)) > len * way_len * 1e-4f) {
        continue;
      }
      return cell_side_way(c.now_sides, k, t) * (dot(handle, rest_way) / (way_len * way_len));
    }
    float2 rest_s, rest_t, now_s, now_t;
    cell_patch_partials(c.rest_sides, st, rest_s, rest_t);
    cell_patch_partials(c.now_sides, st, now_s, now_t);
    const float det = cross(rest_s, rest_t);
    /* Where the patch's two directions are (nearly) parallel, solving through them breaks down. */
    if (std::fabs(det) < 1e-3f * length(rest_s) * length(rest_t)) {
      break;
    }
    /* The handle in the patch's own directions, then those directions as they are now. */
    const float a = cross(handle, rest_t) / det, b = cross(rest_s, handle) / det;
    return now_s * a + now_t * b;
  }
  const float step = std::min(5e-3f, len) / len;
  (void)q0;
  return (deform_map(d, p0 + handle * step, scratch) - deform_map(d, p0 - handle * step, scratch)) / (2.0f * step);
}

static float2 bezier_point(const float2 c[4], float t)
{
  const float u = 1.0f - t;
  return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
}

/* The part of Bezier `c` from t0 to t1 (de Casteljau). */
static void bezier_part(const float2 c[4], float t0, float t1, float2 r[4])
{
  /* `out`: the part of `in` after t. */
  const auto after = [](const float2 in[4], float t, float2 out[4]) {
    const float2 a = in[0] + (in[1] - in[0]) * t, b = in[1] + (in[2] - in[1]) * t, e = in[2] + (in[3] - in[2]) * t;
    const float2 ab = a + (b - a) * t, be = b + (e - b) * t;
    out[0] = ab + (be - ab) * t;
    out[1] = be;
    out[2] = e;
    out[3] = in[3];
  };
  float2 tail[4];
  after(c, t0, tail);
  const float keep = t0 < 1.0f ? (t1 - t0) / (1.0f - t0) : 1.0f;
  /* The tail's front, up to `keep` of it: the tail reversed, its part after 1 - keep, reversed back. */
  const float2 back[4] = {tail[3], tail[2], tail[1], tail[0]};
  float2 front[4];
  after(back, 1.0f - keep, front);
  r[0] = front[3];
  r[1] = front[2];
  r[2] = front[1];
  r[3] = front[0];
}

/* Precision: how many pieces each segment of `base` needs so that, bent by moving its points and handles, it
 * strays no further than `tolerance` from where the deform takes the segment itself (Illustrator's Fidelity):
 * one where the deform barely bends it, more where it bends hard. `weights`: each point's share of the
 * deform; a segment counts by its ends' larger. */
/* r_blend: for each segment cut, how far its last halving has come in (0 to 1): from just past Precision (the
 * pieces before it strayed only a little more than it) to twice it, so a cut fades in as the bend grows rather than
 * popping in (precision_fade). */
static std::vector<std::vector<int>> precision_pieces(const DeformShape &d, const VectorDrawing &base,
                                                      const std::vector<float> &weights, float tolerance,
                                                      std::vector<float> &scratch, bool *r_any,
                                                      std::vector<std::vector<float>> &r_blend)
{
  constexpr int MAX_PIECES = 64;
  std::vector<std::vector<int>> pieces(base.curves.size());
  *r_any = false;
  /* Every segment on its own: shared over the cores. */
  std::vector<std::pair<size_t, int>> segments;
  for (size_t c = 0; c < base.curves.size(); c++) {
    const Curve &curve = base.curves[c];
    pieces[c].assign(size_t(std::max(curve.segment_count(), 0)), 1);
    r_blend.emplace_back(size_t(std::max(curve.segment_count(), 0)), 1.0f);
    for (int seg = 0; seg < curve.segment_count(); seg++) {
      segments.emplace_back(c, seg);
    }
  }
  (void)scratch;
  parallel_for(segments.size(), 1, [&](size_t begin, size_t end) {
    std::vector<float> own;
    for (size_t i = begin; i < end; i++) {
      const auto [c, seg] = segments[i];
      const Curve &curve = base.curves[c];
      int a, b;
      curve_segment_points(curve, seg, &a, &b);
      const float w = std::max(weights[size_t(a)], weights[size_t(b)]);
      if (w <= 0.0f) {
        continue;
      }
      float2 ctrl[4];
      curve_segment_bezier(base, curve, seg, ctrl);
      int n = 1;
      float coarser = 0.0f;
      for (; n < MAX_PIECES; n *= 2) {
        float error = 0.0f;
        for (int k = 0; k < n && error <= tolerance; k++) {
          float2 part[4], bent[4];
          bezier_part(ctrl, float(k) / float(n), float(k + 1) / float(n), part);
          bent[0] = deform_map(d, part[0], own);
          bent[3] = deform_map(d, part[3], own);
          bent[1] = bent[0] + bent_handle(d, part[0], bent[0], part[1] - part[0], own);
          bent[2] = bent[3] + bent_handle(d, part[3], bent[3], part[2] - part[3], own);
          for (const float u : {0.25f, 0.5f, 0.75f}) {
            const float2 truly = deform_map(d, bezier_point(part, u), own);
            error = std::max(error, length(bezier_point(bent, u) - truly) * w);
          }
        }
        if (error <= tolerance) {
          break;
        }
        coarser = error;
      }
      pieces[c][size_t(seg)] = n;
      /* Eased in and out, so it starts and finishes coming in without a jolt. */
      r_blend[c][size_t(seg)] = n > 1 ? smooth01(std::clamp((coarser - tolerance) / tolerance, 0.0f, 1.0f)) : 1.0f;
    }
  });
  for (const auto &[c, seg] : segments) {
    *r_any = *r_any || pieces[c][size_t(seg)] > 1;
  }
  return pieces;
}

/* Precision's cuts fading in: a segment's last halving, just past Precision, leaves each pair of pieces exactly as
 * the one piece they were (`coarse`: the drawing bent with that halving left out; the point between them where that
 * piece has it, its handles and the pair's outer ones as that piece split there), easing over to the pieces bent
 * each on its own (`drawing`) as the bend grows to twice Precision. So a cut coming or going as the shape moves
 * changes nothing at that moment (a thin band doesn't jump thicker and back). Handles are eased as they draw, and
 * kept so. `whole`: the drawing before any cut. */
static void precision_fade(const VectorDrawing &whole, const std::vector<std::vector<int>> &pieces,
                           const std::vector<std::vector<int>> &coarse_pieces,
                           const std::vector<std::vector<float>> &blend, const VectorDrawing &coarse,
                           VectorDrawing &drawing)
{
  const VectorDrawing setup = subdivide_setup_shape(whole);
  for (size_t c = 0; c < whole.curves.size() && c < drawing.curves.size() && c < coarse.curves.size(); c++) {
    const Curve &was = whole.curves[c], &rough = coarse.curves[c];
    Curve &curve = drawing.curves[c];
    const int fine_count = int(curve.points.size()), rough_count = int(rough.points.size());
    /* Every handle as it draws (a cut's points work theirs out from their neighbours), before any is eased. */
    std::vector<float2> drawn_in(static_cast<size_t>(fine_count)), drawn_out(static_cast<size_t>(fine_count));
    for (int i = 0; i < fine_count; i++) {
      const float2 at = drawing.points[size_t(curve.points[size_t(i)].point)].pos;
      drawn_in[size_t(i)] = curve_handle(drawing, curve, i, true) - at;
      drawn_out[size_t(i)] = curve_handle(drawing, curve, i, false) - at;
    }
    int first = 0, rough_first = 0;
    for (int seg = 0; seg < was.segment_count(); seg++) {
      const int n = pieces[c][size_t(seg)], half = coarse_pieces[c][size_t(seg)];
      const float k = blend[c][size_t(seg)];
      const int start = first, rough_start = rough_first;
      first += n;
      rough_first += half;
      if (n < 2 || half * 2 != n || k >= 1.0f) {
        continue;
      }
      float2 rest[4];
      curve_segment_bezier(setup, setup.curves[c], seg, rest);
      const std::vector<float> cuts = subdivide_even_cuts(rest, n);
      for (int j = 0; j < half; j++) {
        const int ia = (start + 2 * j) % fine_count, im = (start + 2 * j + 1) % fine_count,
                  ib = (start + 2 * j + 2) % fine_count;
        /* The one piece the pair was, as it draws, split where the pair meets. */
        float2 one[4];
        curve_segment_bezier(coarse, rough, (rough_start + j) % std::max(rough_count, 1), one);
        const float span = cuts[size_t(2 * j + 2)] - cuts[size_t(2 * j)];
        const float u = span > 1e-12f ? (cuts[size_t(2 * j + 1)] - cuts[size_t(2 * j)]) / span : 0.5f;
        float2 left[4], right[4];
        bezier_part(one, 0.0f, u, left);
        bezier_part(one, u, 1.0f, right);
        CurvePoint &a = curve.points[size_t(ia)], &mid = curve.points[size_t(im)], &b = curve.points[size_t(ib)];
        for (CurvePoint *cp : {&a, &mid, &b}) {
          const size_t i = size_t(cp - curve.points.data());
          cp->handle_in = drawn_in[i];
          cp->handle_out = drawn_out[i];
          cp->handles_set = true;
          cp->handles_follow_curve = false;
        }
        const auto ease = [k](float2 whole_piece, float2 own) { return whole_piece + (own - whole_piece) * k; };
        float2 &mid_pos = drawing.points[size_t(mid.point)].pos;
        a.handle_out = ease(left[1] - left[0], a.handle_out);
        mid.handle_in = ease(left[2] - left[3], mid.handle_in);
        mid.handle_out = ease(right[1] - right[0], mid.handle_out);
        b.handle_in = ease(right[2] - right[3], b.handle_in);
        mid_pos = ease(left[3], mid_pos);
      }
    }
  }
}

/* How far the samples `pts` stray from the cubic q0, q0 + h1, q3 + h2, q3 (to a fine polyline of it), as the root
 * mean square: which of two fits to a bent segment follows it closer. Not the farthest one alone, which changes
 * abruptly as another sample becomes the farthest (and the blend of the fits by it with it). */
static float bezier_stray(float2 q0, float2 h1, float2 h2, float2 q3, const std::vector<float2> &pts)
{
  const float2 c[4] = {q0, q0 + h1, q3 + h2, q3};
  std::vector<float2> line(129);
  for (int k = 0; k <= 128; k++) {
    line[size_t(k)] = bezier_point(c, float(k) / 128.0f);
  }
  double sum = 0.0;
  for (const float2 &q : pts) {
    const double dist = polyline_distance(line, false, q);
    sum += dist * dist;
  }
  return pts.empty() ? 0.0f : float(std::sqrt(sum / double(pts.size())));
}

/* As a sculpt stroke fits a line back to its samples: each segment sampled evenly in its own
 * parameter (on the drawing as it was, `base`, and as it is), each sample bent by the deform as far as its share
 * (its ends' weights, eased between), then each segment's handles fitted to its bent samples, the points where theirs
 * went (`mapped`). A handle keeps the way the deform turns it at its point (bent_handle; as far as the point's
 * weight), so the two handles of a smooth point stay in line and a bent line has no kinks at its points; only how
 * long each is is fitted (least squares, the samples' places along the curve refined: Schneider's fit). A point the
 * deform doesn't move keeps its handles. Only the handles are set here; the points move after. Not `always` (all but a
 * shape being read, below): a segment whose handles as bent already follow the bend (within a thousandth of its chord, or a
 * quarter of Precision `tolerance`, checked at three places along it) keeps them, the fit easing in only as they stray
 * further (to fully at four times that), so it costs little where the bend is gentle and never switches on with a
 * jolt. */
static void resample_fit(const DeformShape &d, const VectorDrawing &base, const std::vector<float> &weights,
                         const std::vector<float2> &mapped, VectorDrawing &drawing, std::vector<float> &scratch,
                         bool always, float tolerance)
{
  /* Samples per segment. */
  constexpr int steps = 16;
  /* Each segment on its own (it sets only the handles leaving and reaching it): shared over the cores. */
  std::vector<std::pair<size_t, int>> pieces;
  for (size_t c = 0; c < drawing.curves.size() && c < base.curves.size(); c++) {
    const int segments = std::min(drawing.curves[c].segment_count(), base.curves[c].segment_count());
    for (int s = 0; s < segments; s++) {
      pieces.emplace_back(c, s);
    }
  }
  (void)scratch;
  parallel_for(pieces.size(), 2, [&](size_t begin, size_t end) {
    std::vector<float2> inner;
    std::vector<float> scratch;
    for (size_t piece = begin; piece < end; piece++) {
      const auto [c, s] = pieces[piece];
      Curve &curve = drawing.curves[c];
      const Curve &was = base.curves[c];
      const int n = int(curve.points.size());
      const int a = s, b = (s + 1) % n;
      CurvePoint &from = curve.points[size_t(a)], &to = curve.points[size_t(b)];
      const float wa = weights[size_t(from.point)], wb = weights[size_t(to.point)];
      if (wa <= 0.0f && wb <= 0.0f) {
        continue;
      }
      float2 rest[4], now[4];
      curve_segment_bezier(base, was, s, rest);
      curve_segment_bezier(drawing, curve, s, now);
      /* Where each end goes. */
      const float2 q0 = now[0] + ((wa > 0.0f ? mapped[size_t(from.point)] : now[0]) - now[0]) * wa;
      const float2 q3 = now[3] + ((wb > 0.0f ? mapped[size_t(to.point)] : now[3]) - now[3]) * wb;
      /* Each handle as the deform bends it at its point, as far as the point's weight (none for a handle of no
       * length). */
      const auto bent = [&](float2 p, float2 rest_handle, float2 now_handle, float w) {
        return length(rest_handle) > 1e-9f ? now_handle + (bent_handle(d, p, p, rest_handle, scratch) - now_handle) * w
                                           : now_handle;
      };
      const float2 b1 = bent(rest[0], rest[1] - rest[0], now[1] - now[0], wa);
      const float2 b2 = bent(rest[3], rest[2] - rest[3], now[2] - now[3], wb);
      /* Where it is bent truly, at `t`. */
      const auto truly = [&](float t) {
        const float w = wa + (wb - wa) * smooth01(t);
        const float2 here = bezier_point(now, t);
        const float2 moved = w > 0.0f ? deform_map(d, bezier_point(rest, t), scratch) : here;
        return here + (moved - here) * w;
      };
      float ease = 1.0f;
      if (!always) {
        const float2 as_bent[4] = {q0, q0 + b1, q3 + b2, q3};
        float off = 0.0f;
        for (const float t : {0.25f, 0.5f, 0.75f}) {
          off = std::max(off, length(bezier_point(as_bent, t) - truly(t)));
        }
        const float within = std::max({length(q3 - q0) * 1e-3f, tolerance * 0.25f, 1e-9f});
        ease = smooth01((off - within) / (3.0f * within));
        if (ease <= 0.0f) {
          if (wa > 0.0f) {
            from.handle_out = b1;
          }
          if (wb > 0.0f) {
            to.handle_in = b2;
          }
          continue;
        }
      }
      /* The bent samples, end to end. */
      inner.clear();
      inner.push_back(q0);
      for (int k = 1; k < steps; k++) {
        inner.push_back(truly(float(k) / float(steps)));
      }
      inner.push_back(q3);
      /* Their ways (a handle of no length: the way the bent segment leaves its point), and how long each is fitted. */
      const float2 t1 = normalize(length(b1) > 1e-12f ? b1 : inner[1] - q0);
      const float2 t2 = normalize(length(b2) > 1e-12f ? b2 : inner[inner.size() - 2] - q3);
      float a1 = length(b1), a2 = length(b2);
      bezier_fit_handle_lengths(q0, q3, t1, t2, inner, &a1, &a2, true);
      /* The fitted lengths and the handles as bent, each the more the closer it follows the samples (the fit's
       * places along the curve are refined, not exact, and either can be the closer): blended by how far each strays
       * (by the fourth power: the clearly closer one all but alone), not one picked, so a small move never flips the segment from one to the other. */
      const float2 f1 = t1 * std::max(a1, 0.0f), f2 = t2 * std::max(a2, 0.0f);
      const float fit_stray = bezier_stray(q0, f1, f2, q3, inner), bent_stray = bezier_stray(q0, b1, b2, q3, inner);
      const double fit4 = std::pow(double(fit_stray), 4.0), bent4 = std::pow(double(bent_stray), 4.0),
                   both = fit4 + bent4;
      const float fitted = (both > 1e-300 ? float(bent4 / both) : 0.5f) * ease;
      const float2 h1 = b1 + (f1 - b1) * fitted, h2 = b2 + (f2 - b2) * fitted;
      if (wa > 0.0f) {
        from.handle_out = h1;
      }
      if (wb > 0.0f) {
        to.handle_in = h2;
      }
    }
  });
}

/* A deform's Custom weights go by point, and points made since they were painted have none: those cut into a segment
 * (by a Resample modifier above, Resample Self or Precision) take theirs from its two ends, eased between. `own`: how
 * many of the drawing's points were there before the cuts. */
static void for_each_cut(const VectorDrawing &split, size_t own,
                         const std::function<void(size_t point, size_t from, size_t to, float along)> &cut)
{
  for (const Curve &curve : split.curves) {
    const int n = int(curve.points.size());
    const auto point = [&](int i) { return size_t(curve.points[size_t(i % n)].point); };
    for (int i = 0; i < n; i++) {
      if (point(i) >= own) {
        continue;
      }
      int pieces = 1;
      while (pieces < n && i + pieces < (curve.closed ? i + n : n) && point(i + pieces) >= own) {
        pieces++;
      }
      if (pieces == 1 || (!curve.closed && i + pieces >= n)) {
        continue;
      }
      for (int k = 1; k < pieces; k++) {
        cut(point(i + k), point(i), point(i + pieces), smooth01(float(k) / float(pieces)));
      }
    }
  }
}

static PointGroup group_with_cuts(const PointGroup &group, const VectorDrawing &split, size_t own)
{
  PointGroup r = group;
  for_each_cut(split, own, [&](size_t point, size_t from, size_t to, float along) {
    const float a = point_weight(&group, split.points[from]), b = point_weight(&group, split.points[to]);
    r.weights[split.points[point].id] = a + (b - a) * along;
  });
  return r;
}

std::vector<float> vector_deform_follow_share(const Document &doc, const Layer &layer, const Modifier &m,
                                              const VectorDrawing &drawing)
{
  if (m.follow_limit == FollowLimit::None) {
    return {};
  }
  const VectorDrawing &stored = layer_drawing(doc, layer);
  std::vector<float> own(stored.points.size(), 0.0f);
  bool found = false;
  if (m.follow_limit == FollowLimit::Shape) {
    if (const int index = drawing_shape_index(stored, m.follow_limit_shape); index >= 0) {
      found = true;
      for (const int p : shape_points(stored, stored.shapes[size_t(index)])) {
        own[size_t(p)] = 1.0f;
      }
    }
  }
  else {
    for (const PointGroup &g : stored.groups) {
      if (g.name != m.follow_limit_group) {
        continue;
      }
      found = true;
      for (size_t i = 0; i < own.size(); i++) {
        if (auto it = g.weights.find(stored.points[i].id); it != g.weights.end()) {
          own[i] = std::clamp(it->second, 0.0f, 1.0f);
        }
      }
    }
  }
  std::vector<float> share(drawing.points.size(), 0.0f);
  if (!found) {
    return share;
  }
  std::map<uint64_t, float> by_id;
  for (size_t i = 0; i < own.size(); i++) {
    own[i] = m.follow_limit_invert ? 1.0f - own[i] : own[i];
    if (i < share.size()) {
      share[i] = own[i];
    }
    by_id[stored.points[i].id] = own[i];
  }
  /* The stack's copies of a point (a place worked out as the point is) go as it does. */
  for (size_t i = own.size(); i < share.size(); i++) {
    if (auto it = by_id.find(drawing.points[i].id); it != by_id.end()) {
      share[i] = it->second;
    }
  }
  for_each_cut(drawing, own.size(), [&](size_t point, size_t from, size_t to, float along) {
    share[point] = share[from] + (share[to] - share[from]) * along;
  });
  return share;
}

float2 vector_deform_follow_middle(const Document &doc, const Layer &layer, const Modifier &m)
{
  const VectorDrawing setup = layer_setup_drawing(doc, layer, false);
  const std::vector<float> share = vector_deform_follow_share(doc, layer, m, setup);
  const auto middle = [&](bool all) {
    float2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
    for (size_t i = 0; i < setup.points.size(); i++) {
      if (all || share.empty() || share[i] > 0.0f) {
        lo = {std::min(lo.x, setup.points[i].pos.x), std::min(lo.y, setup.points[i].pos.y)};
        hi = {std::max(hi.x, setup.points[i].pos.x), std::max(hi.y, setup.points[i].pos.y)};
      }
    }
    return hi.x >= lo.x ? std::optional<float2>((lo + hi) * 0.5f) : std::nullopt;
  };
  /* With nothing to move yet, the drawing's: not where the point happens to be, which would pass for a point that
   * was never put anywhere. */
  return middle(false).value_or(middle(true).value_or(float2()));
}

void vector_deform_follow_limit_changed(const Document &doc, const Layer &layer, const Modifier &was, Modifier &m)
{
  const float2 from = vector_deform_follow_middle(doc, layer, was);
  if (m.deform_point_id != 0 || length(m.deform_point - from) > 1e-5f) {
    return;
  }
  /* A second point keeps its way from the first. */
  const float2 move = vector_deform_follow_middle(doc, layer, m) - from;
  m.deform_point = m.deform_point + move;
  if (m.deform_point2_id == 0) {
    m.deform_point2 = m.deform_point2 + move;
  }
}

/* The drawing bent (base: as it was before the bones, the same points): each point where the deform puts it, each
 * handle turned and stretched as the deform bends it there (or fitted), each as far as its weight. */
static void bend_drawing(const DeformShape &d, const PointGroup *group, float strength, bool resample,
                         const VectorDrawing &base, VectorDrawing &drawing, std::vector<float> &scratch,
                         bool always_fit = true, float tolerance = 0.0f)
{
  std::vector<float> weights(drawing.points.size());
  std::vector<float2> mapped(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    weights[i] = point_weight(group, drawing.points[i]) * strength;
  }
  /* Each point on its own: shared over the cores. */
  parallel_for(drawing.points.size(), 16, [&](size_t begin, size_t end) {
    std::vector<float> own;
    for (size_t i = begin; i < end; i++) {
      if (weights[i] > 0.0f) {
        mapped[i] = deform_map(d, base.points[i].pos, own);
      }
    }
  });
  if (resample) {
    resample_fit(d, base, weights, mapped, drawing, scratch, always_fit, tolerance);
  }
  /* Every curve point's handles, each on its own: shared over the cores. */
  std::vector<std::pair<size_t, size_t>> handle_points;
  for (size_t c = 0; c < drawing.curves.size() && !resample; c++) {
    for (size_t k = 0; k < drawing.curves[c].points.size() && k < base.curves[c].points.size(); k++) {
      handle_points.emplace_back(c, k);
    }
  }
  parallel_for(handle_points.size(), 16, [&](size_t begin, size_t end) {
    std::vector<float> own;
    for (size_t i = begin; i < end; i++) {
      const auto [c, k] = handle_points[i];
      CurvePoint &cp = drawing.curves[c].points[k];
      const float w = weights[size_t(cp.point)];
      /* Handles still worked out from the neighbours follow the points by themselves. */
      if (w <= 0.0f || !(cp.handles_set || cp.handles_follow_curve)) {
        continue;
      }
      const CurvePoint &was = base.curves[c].points[k];
      const float2 p0 = base.points[size_t(cp.point)].pos, q0 = mapped[size_t(cp.point)];
      const float2 in = bent_handle(d, p0, q0, was.handle_in, own);
      const float2 out = bent_handle(d, p0, q0, was.handle_out, own);
      cp.handle_in = cp.handle_in + (in - cp.handle_in) * w;
      cp.handle_out = cp.handle_out + (out - cp.handle_out) * w;
    }
  });
  if (g_wire > 0 && !resample) {
    /* The wire: each deformed segment's handles fitted to the bent segment itself (their directions as
     * the deform turned them, their lengths by least squares), the nearest one Bezier comes to it, unless the
     * handles as bent already follow it closer (on a cell's sides they are exact). */
    for (size_t c = 0; c < drawing.curves.size(); c++) {
      Curve &curve = drawing.curves[c];
      const Curve &was = base.curves[c];
      const int n = int(curve.points.size());
      for (int s = 0; s < curve.segment_count() && s < was.segment_count(); s++) {
        CurvePoint &from = curve.points[size_t(s)], &to = curve.points[size_t((s + 1) % n)];
        const float w = std::min(weights[size_t(from.point)], weights[size_t(to.point)]);
        if (w <= 0.0f) {
          continue;
        }
        const float out_len = length(from.handle_out), in_len = length(to.handle_in);
        if (out_len < 1e-9f || in_len < 1e-9f) {
          continue;
        }
        float2 ctrl[4];
        curve_segment_bezier(base, was, s, ctrl);
        std::vector<float2> samples;
        for (int k = 1; k < 12; k++) {
          samples.push_back(deform_map(d, bezier_point(ctrl, float(k) / 12.0f), scratch));
        }
        /* The bent segment's own directions at its ends (a long handle's mapped end lies inside a bend). */
        const float2 q0 = deform_map(d, ctrl[0], scratch), q3 = deform_map(d, ctrl[3], scratch);
        const float2 d1 = deform_map(d, bezier_point(ctrl, 1e-3f), scratch) - q0;
        const float2 d2 = deform_map(d, bezier_point(ctrl, 1.0f - 1e-3f), scratch) - q3;
        if (length(d1) < 1e-12f || length(d2) < 1e-12f) {
          continue;
        }
        const float2 t1 = normalize(d1), t2 = normalize(d2);
        float a1 = out_len, a2 = in_len;
        bezier_fit_handle_lengths(mapped[size_t(from.point)], mapped[size_t(to.point)], t1, t2, samples, &a1, &a2,
                                  true);
        const float2 q3_now = mapped[size_t(to.point)], q0_now = mapped[size_t(from.point)];
        /* Fitted and as bent, each the more the closer it follows (as Resample Self's, by the fourth power). */
        const float2 f1 = t1 * std::max(a1, 0.0f), f2 = t2 * std::max(a2, 0.0f);
        const float fit_stray = bezier_stray(q0_now, f1, f2, q3_now, samples);
        const float bent_stray = bezier_stray(q0_now, from.handle_out, to.handle_in, q3_now, samples);
        const double fit4 = std::pow(double(fit_stray), 4.0), bent4 = std::pow(double(bent_stray), 4.0),
                   both = fit4 + bent4;
        const float fitted = (both > 1e-300 ? float(bent4 / both) : 0.5f) * w;
        from.handle_out = from.handle_out + (f1 - from.handle_out) * fitted;
        to.handle_in = to.handle_in + (f2 - to.handle_in) * fitted;
      }
    }
  }
  for (size_t i = 0; i < drawing.points.size(); i++) {
    if (weights[i] > 0.0f) {
      drawing.points[i].pos = drawing.points[i].pos + (mapped[i] - drawing.points[i].pos) * weights[i];
    }
  }
}

void vector_deform_apply(const Document &doc, const Layer &layer, const Modifier &m, VectorDrawing &drawing,
                         const VectorDrawing &before_bones, bool render)
{
  /* Resample Self splits the drawing as the modifier called Resample does and bends the pieces, here so that it can be
   * left out by what reads this drawing sample for sample: a cage's or a mesh's shape gets the points it was
   * drawn with, each segment fitted to the bend instead. */
  const bool cut_self = m.deform_resample_self && !g_shape_read;
  /* The same with the wire or without it, and kept for both, or the edit wire and the render would each split and bend
   * every segment each time. */
  struct WireLeft {
    int was;
    bool off;
    ~WireLeft()
    {
      if (off) {
        g_wire = was;
      }
    }
  } wire_left{g_wire, cut_self};
  if (cut_self) {
    g_wire = 0;
  }
  /* Frame 0 is the rest: nothing bends there. (While the shape is being edited there its frame-0 keys still
   * hold where it was until the edit is recorded, so measuring would bend the drawing by the edit.) */
  if (doc.frame_current == 0 || m.deform_strength <= 0.0f || vector_deform_loops(doc, layer, m)) {
    /* Split there too, which doesn't change how it looks, so the pieces are the same on every frame. */
    if (cut_self && m.deform_layer != 0) {
      drawing = drawing_subdivide_kept(layer.id, m.name, std::move(drawing), m, render);
    }
    return;
  }
  uint64_t build = 0;
  const std::shared_ptr<const DeformShape> built = deform_shape_kept(doc, layer, m, render, build);
  if (!built) {
    return;
  }
  const DeformShape &d = *built;
  const PointGroup *group = deform_group(layer, m);
  /* The same drawing bent by the same build as before: what it made then. Kept apart with and without the wire, so a
   * layer both drawn and followed as a cage's or mesh's shape doesn't bend afresh each time. */
  const std::string kept_as = m.name + std::string(size_t(g_probe), '\t');
  const auto bend_key = std::make_tuple(layer.id, g_wire > 0 ? kept_as + "\n" : kept_as, render);
  const auto same_bend = [&](const KeptBend &k) {
    return k.build == build && k.render == render && k.wire == g_wire && k.m == m && k.drawing == drawing &&
           k.before_bones == before_bones && k.grouped == (group != nullptr) && (!group || k.group == *group);
  };
  {
    std::lock_guard lock(g_kept_mutex);
    auto it = g_kept_bends.find(bend_key);
    if (it != g_kept_bends.end() && same_bend(it->second)) {
      drawing = it->second.result;
      return;
    }
  }
  KeptBend kept;
  kept.m = m;
  kept.build = build;
  kept.render = render;
  kept.wire = g_wire;
  kept.drawing = drawing;
  kept.before_bones = before_bones;
  kept.grouped = group != nullptr;
  if (group) {
    kept.group = *group;
  }
  /* Kept when done (every way out below). */
  struct Keep {
    KeptBend &kept;
    VectorDrawing &drawing;
    std::tuple<uint64_t, std::string, bool> key;
    ~Keep()
    {
      kept.result = drawing;
      std::lock_guard lock(g_kept_mutex);
      if (g_kept_bends.size() > 512) {
        g_kept_bends.clear();
      }
      g_kept_bends[key] = std::move(kept);
    }
  } keep{kept, drawing, bend_key};
  const bool same = before_bones.points.size() == drawing.points.size() &&
                    before_bones.curves.size() == drawing.curves.size();
  VectorDrawing base = same ? before_bones : drawing;
  const float strength = std::clamp(m.deform_strength, 0.0f, 1.0f);
  std::vector<float> scratch;
  const bool resample = m.deform_resample_self;
  /* What the weights go by, with the cuts' own as they are made. */
  const PointGroup *weighed = group;
  PointGroup cut_group;
  const auto weigh_cuts = [&](const VectorDrawing &cut, size_t own) {
    if (weighed && cut.points.size() > own) {
      cut_group = group_with_cuts(*weighed, cut, own);
      weighed = &cut_group;
    }
  };
  weigh_cuts(drawing, layer_drawing(doc, layer).points.size());
  if (cut_self) {
    const size_t own = drawing.points.size();
    const bool alike = &before_bones == &drawing;
    drawing = drawing_subdivide_kept(layer.id, m.name, std::move(drawing), m, render);
    if (same && !alike) {
      base = drawing_subdivide_kept(layer.id, m.name + "\trest", std::move(base), m, render);
    }
    if (!same || alike || base.points.size() != drawing.points.size()) {
      base = drawing;
    }
    weigh_cuts(drawing, own);
  }
  /* Precision's cuts, how far each has faded in, and the drawing before them (precision_fade). */
  std::vector<std::vector<int>> pieces;
  std::vector<std::vector<float>> fade;
  VectorDrawing whole, whole_drawing;
  if (g_wire > 0 || resample) {
    handles_explicit(base);
    handles_explicit(drawing);
  }
  else if (m.deform_precision > 0.0f) {
    /* Points where the deform would bend a segment more than its points and handles can follow, on the
     * drawing as it was and as it is alike (the same cuts). */
    std::vector<float> share(base.points.size());
    for (size_t i = 0; i < base.points.size(); i++) {
      share[i] = point_weight(weighed, base.points[i]) * strength;
    }
    bool any = false;
    pieces = precision_pieces(d, base, share, m.deform_precision, scratch, &any, fade);
    if (any) {
      whole = base;
      whole_drawing = drawing;
      Modifier split;
      split.type = ModifierType::Resample;
      base = drawing_subdivide(base, split, render, nullptr, &pieces);
      drawing = drawing_subdivide(drawing, split, render, nullptr, &pieces);
      if (base.points.size() != drawing.points.size()) {
        base = drawing;
      }
    }
  }
  /* With Precision off too, each segment's handles fitted to it bent where the handles as bent stray from it (each
   * handle the way the deform turns it at its point, its length fitted, blended with the length as bent by how
   * closely each follows the bend): the handles as bent come from how the deform stretches right at their point, and
   * where it changes quickly along the segment they overshoot. (With Precision on, it has cut every segment till the
   * handles as bent follow the bend within it.) The wire, as shown while editing, fits its own (below). */
  const bool fit = resample || (g_wire == 0 && m.deform_precision <= 0.0f);
  if (fit && !resample) {
    handles_explicit(base);
    handles_explicit(drawing);
  }
  const PointGroup *before_precision = weighed;
  PointGroup kept_group;
  if (weighed && !whole.points.empty()) {
    kept_group = *weighed;
    before_precision = &kept_group;
    weigh_cuts(drawing, whole_drawing.points.size());
  }
  bend_drawing(d, weighed, strength, fit, base, drawing, scratch, resample && !cut_self,
               cut_self ? 0.0f : m.deform_precision);
  /* A cut fading in: the drawing also bent without it, and the two eased between (precision_fade). */
  if (!whole.points.empty() && base.points.size() == drawing.points.size()) {
    std::vector<std::vector<int>> coarse_pieces = pieces;
    bool fading = false;
    for (size_t c = 0; c < pieces.size(); c++) {
      for (size_t seg = 0; seg < pieces[c].size(); seg++) {
        if (pieces[c][seg] > 1 && fade[c][seg] < 1.0f) {
          coarse_pieces[c][seg] = pieces[c][seg] / 2;
          fading = true;
        }
      }
    }
    if (fading) {
      VectorDrawing coarse_base = whole, coarse = whole_drawing;
      Modifier split;
      split.type = ModifierType::Resample;
      coarse_base = drawing_subdivide(coarse_base, split, render, nullptr, &coarse_pieces);
      coarse = drawing_subdivide(coarse, split, render, nullptr, &coarse_pieces);
      if (coarse_base.points.size() == coarse.points.size()) {
        if (fit && !resample) {
          handles_explicit(coarse_base);
          handles_explicit(coarse);
        }
        PointGroup coarse_group;
        if (before_precision) {
          coarse_group = group_with_cuts(*before_precision, coarse, whole_drawing.points.size());
        }
        bend_drawing(d, before_precision ? &coarse_group : nullptr, strength, fit, coarse_base, coarse, scratch, false,
                     m.deform_precision);
        precision_fade(whole, pieces, coarse_pieces, fade, coarse, drawing);
      }
    }
  }
}

std::vector<float> vector_deform_auto_weights(const Document &doc, const Layer &layer, const Modifier &m)
{
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  std::vector<float> weights(drawing.points.size(), 0.0f);
  DeformShape d;
  if (!deform_shape_build(doc, layer, m, false, false, d)) {
    return weights;
  }
  for (size_t i = 0; i < drawing.points.size(); i++) {
    weights[i] = 1.0f;
  }
  return weights;
}

std::vector<float> vector_deform_weights(const Document &doc, const Layer &layer, const Modifier &m)
{
  const VectorDrawing &drawing = layer_drawing(doc, layer);
  std::vector<float> weights(drawing.points.size(), 0.0f);
  DeformShape d;
  if (!deform_shape_build(doc, layer, m, false, false, d)) {
    return weights;
  }
  const PointGroup *group = deform_group(layer, m);
  for (size_t i = 0; i < drawing.points.size(); i++) {
    weights[i] = point_weight(group, drawing.points[i]);
  }
  return weights;
}

static PointGroup &group_for(Layer &layer, Modifier &m)
{
  if (m.deform_group.empty()) {
    m.deform_group = m.name;
  }
  for (PointGroup &g : layer.drawing.groups) {
    if (g.name == m.deform_group) {
      return g;
    }
  }
  PointGroup g;
  g.name = m.deform_group;
  layer.drawing.groups.push_back(std::move(g));
  return layer.drawing.groups.back();
}

void vector_deform_fill_group(const Document &doc, Layer &layer, Modifier &m)
{
  const std::vector<float> weights = vector_deform_auto_weights(doc, layer, m);
  PointGroup &group = group_for(layer, m);
  group.weights.clear();
  for (size_t i = 0; i < layer.drawing.points.size() && i < weights.size(); i++) {
    if (layer.drawing.points[i].id != 0) {
      group.weights[layer.drawing.points[i].id] = weights[i];
    }
  }
}

int vector_deform_paint(const Document &doc, Layer &layer, const Modifier &m, const WeightPaintBrush &brush)
{
  if (brush.radius <= 0.0f || !m.deform_custom) {
    return 0;
  }
  const std::vector<float> start = vector_deform_weights(doc, layer, m);
  Modifier &own = const_cast<Modifier &>(m);
  PointGroup &group = group_for(layer, own);
  int changed = 0;
  for (size_t index = 0; index < layer.drawing.points.size(); index++) {
    const MeshPoint &p = layer.drawing.points[index];
    if (p.id == 0 || (brush.selected_only && !p.selected)) {
      continue;
    }
    const bool shown = brush.positions && index < brush.positions->size();
    const float dist = distance(shown ? (*brush.positions)[index] : p.pos, brush.center);
    if (dist > brush.radius) {
      continue;
    }
    /* Blender's smooth falloff, as the bones' brush. */
    const float amount = std::clamp(brush.strength, 0.0f, 1.0f) * smooth01(1.0f - dist / brush.radius);
    const auto it = group.weights.find(p.id);
    const float before = it != group.weights.end() ? it->second : start[index];
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
    group.weights[p.id] = after;
    changed += after != before || it == group.weights.end() ? 1 : 0;
  }
  return changed;
}

}  // namespace oanim


