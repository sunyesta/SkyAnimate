#include "data/grid.hh"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "data/vector.hh"

namespace oanim {

VectorDrawing grid_drawing(float2 a, float2 b, int columns, int rows, const Style &style)
{
  VectorDrawing drawing;
  columns = std::clamp(columns, 1, GRID_CELLS_MAX);
  rows = std::clamp(rows, 1, GRID_CELLS_MAX);
  const float2 lo(std::min(a.x, b.x), std::min(a.y, b.y)), hi(std::max(a.x, b.x), std::max(a.y, b.y));
  if (hi.x - lo.x < 1e-5f || hi.y - lo.y < 1e-5f) {
    return drawing;
  }
  DrawingGrid grid;
  grid.columns = columns;
  grid.rows = rows;
  for (int j = 0; j <= rows; j++) {
    for (int i = 0; i <= columns; i++) {
      MeshPoint p;
      p.pos = {lo.x + (hi.x - lo.x) * float(i) / float(columns), lo.y + (hi.y - lo.y) * float(j) / float(rows)};
      p.id = drawing.next_id++;
      drawing.points.push_back(p);
      grid.nodes.push_back(p.id);
    }
  }
  /* A line along each row, then one up each column; straight (their handles point along them). All of them
   * are one outline shape, each line a run of it (as a Moho stroke shape of loose lines). */
  Shape shape;
  shape.name = grid.name;
  shape.style = style;
  shape.has_outline = true;
  shape.has_fill = false;
  const auto line = [&](auto &&node_at, int count) {
    Curve curve;
    curve.closed = false;
    for (int k = 0; k < count; k++) {
      CurvePoint cp;
      cp.point = node_at(k);
      cp.curvature = CURVATURE_PEAKED;
      curve.points.push_back(cp);
    }
    drawing.curves.push_back(std::move(curve));
    for (int s = 0; s < count - 1; s++) {
      shape.edges.push_back({int(drawing.curves.size()) - 1, s});
    }
  };
  for (int j = 0; j <= rows; j++) {
    line([&](int i) { return j * (columns + 1) + i; }, columns + 1);
  }
  for (int i = 0; i <= columns; i++) {
    line([&](int j) { return j * (columns + 1) + i; }, rows + 1);
  }
  drawing.shapes.push_back(std::move(shape));
  drawing.grids.push_back(std::move(grid));
  return drawing;
}

bool grid_valid(const VectorDrawing &drawing, const DrawingGrid &grid)
{
  if (grid.columns < 1 || grid.rows < 1 || grid.nodes.size() != size_t((grid.columns + 1) * (grid.rows + 1))) {
    return false;
  }
  for (const uint64_t id : grid.nodes) {
    if (drawing_point_index(drawing, id) < 0) {
      return false;
    }
  }
  return true;
}

bool GridSurface::build(const VectorDrawing &drawing, const DrawingGrid &grid)
{
  columns = grid.columns;
  rows = grid.rows;
  if (!grid_valid(drawing, grid)) {
    return false;
  }
  const int w = columns + 1;
  std::vector<int> index(grid.nodes.size());
  for (size_t k = 0; k < grid.nodes.size(); k++) {
    index[k] = drawing_point_index(drawing, grid.nodes[k]);
  }
  /* Each segment by the two points it joins (the first, if more do): every edge looks one up, and a grid being animated
   * is built again each frame. */
  const auto pair_key = [](int a, int b) {
    return uint64_t(uint32_t(std::min(a, b))) << 32 | uint64_t(uint32_t(std::max(a, b)));
  };
  std::unordered_map<uint64_t, std::pair<const Curve *, int>> segments;
  for (const Curve &curve : drawing.curves) {
    for (int s = 0; s < curve.segment_count(); s++) {
      int from, to;
      curve_segment_points(curve, s, &from, &to);
      segments.emplace(pair_key(from, to), std::make_pair(&curve, s));
    }
  }
  /* The Bezier from point a to point b: the curve segment joining them (either way round), else straight. */
  const auto edge = [&](int a, int b, std::array<float2, 4> &r) {
    if (const auto it = segments.find(pair_key(a, b)); it != segments.end()) {
      const auto [curve, s] = it->second;
      int from, to;
      curve_segment_points(*curve, s, &from, &to);
      float2 ctrl[4];
      curve_segment_bezier(drawing, *curve, s, ctrl);
      if (from == a) {
        r = {ctrl[0], ctrl[1], ctrl[2], ctrl[3]};
      }
      else {
        r = {ctrl[3], ctrl[2], ctrl[1], ctrl[0]};
      }
      return;
    }
    const float2 pa = drawing.points[size_t(a)].pos, pb = drawing.points[size_t(b)].pos;
    r = {pa, pa + (pb - pa) / 3.0f, pa + (pb - pa) * (2.0f / 3.0f), pb};
  };
  across.assign(size_t(columns * (rows + 1)), {});
  up.assign(size_t((columns + 1) * rows), {});
  for (int j = 0; j <= rows; j++) {
    for (int i = 0; i < columns; i++) {
      edge(index[size_t(j * w + i)], index[size_t(j * w + i + 1)], across[size_t(j * columns + i)]);
    }
  }
  for (int i = 0; i <= columns; i++) {
    for (int j = 0; j < rows; j++) {
      edge(index[size_t(j * w + i)], index[size_t((j + 1) * w + i)], up[size_t(i * rows + j)]);
    }
  }
  lo = hi = drawing.points[size_t(index[0])].pos;
  for (const int k : index) {
    const float2 p = drawing.points[size_t(k)].pos;
    lo = float2(std::min(lo.x, p.x), std::min(lo.y, p.y));
    hi = float2(std::max(hi.x, p.x), std::max(hi.y, p.y));
  }
  cell_lo.clear();
  cell_hi.clear();
  for (int j = 0; j < rows; j++) {
    for (int i = 0; i < columns; i++) {
      float2 a = across[size_t(j * columns + i)][0], b = a;
      for (const std::array<float2, 4> *side :
           {&across[size_t(j * columns + i)], &across[size_t((j + 1) * columns + i)], &up[size_t(i * rows + j)],
            &up[size_t((i + 1) * rows + j)]})
      {
        for (const float2 &q : *side) {
          a = float2(std::min(a.x, q.x), std::min(a.y, q.y));
          b = float2(std::max(b.x, q.x), std::max(b.y, q.y));
        }
      }
      /* A patch can bulge a little past its sides. */
      const float2 margin = (b - a) * 0.1f;
      cell_lo.push_back(a - margin);
      cell_hi.push_back(b + margin);
    }
  }
  return true;
}

static float2 bezier_at(const std::array<float2, 4> &c, float t)
{
  const float u = 1.0f - t;
  return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
}

float2 GridSurface::patch(int column, int row, float2 st) const
{
  const float s = st.x, t = st.y;
  const std::array<float2, 4> &bottom = across[size_t(row * columns + column)];
  const std::array<float2, 4> &top = across[size_t((row + 1) * columns + column)];
  const std::array<float2, 4> &left = up[size_t(column * rows + row)];
  const std::array<float2, 4> &right = up[size_t((column + 1) * rows + row)];
  /* Coons: the two surfaces between opposite sides, less the one between the corners, each side fading out
   * smoothly toward the side opposite (3x^2 - 2x^3, not in a straight line): a side's move reaches the middle
   * at half and dies away with no slope at the far side, so moving one side barely moves what lies by the others
   * (and past them, the slope there is theirs alone). */
  const float2 p00 = bottom[0], p10 = bottom[3], p01 = top[0], p11 = top[3];
  const float fs = s * s * (3.0f - 2.0f * s), ft = t * t * (3.0f - 2.0f * t);
  return bezier_at(bottom, s) * (1.0f - ft) + bezier_at(top, s) * ft + bezier_at(left, t) * (1.0f - fs) +
         bezier_at(right, t) * fs -
         (p00 * ((1.0f - fs) * (1.0f - ft)) + p10 * (fs * (1.0f - ft)) + p01 * ((1.0f - fs) * ft) + p11 * (fs * ft));
}

float2 GridSurface::eval(int column, int row, float2 st) const
{
  const float2 in(std::clamp(st.x, 0.0f, 1.0f), std::clamp(st.y, 0.0f, 1.0f));
  const float2 at = patch(column, row, in);
  if (in == st) {
    return at;
  }
  /* Past the grid's edge: on straight, along the patch's slope there (one-sided, from inside). */
  const float h = 1e-3f;
  const float sx = in.x > 0.5f ? -h : h, sy = in.y > 0.5f ? -h : h;
  const float2 ds = (patch(column, row, {in.x + sx, in.y}) - at) / sx;
  const float2 dt = (patch(column, row, {in.x, in.y + sy}) - at) / sy;
  return at + ds * (st.x - in.x) + dt * (st.y - in.y);
}

bool GridSurface::locate(float2 p, GridPlace &r_place) const
{
  if (across.empty()) {
    return false;
  }
  const float scale = std::max(std::max(hi.x - lo.x, hi.y - lo.y), 1e-6f);
  /* Near enough, and no nearer than these numbers can tell: a small grid far from the origin (a mouth's) would otherwise
   * wander around its answer, and be taken for a place in some other cell. */
  const float near_enough = std::max(scale * 1e-6f, (std::fabs(p.x) + std::fabs(p.y) + scale) * 4e-7f);
  float best = FLT_MAX;
  bool found = false;
  const auto try_cell = [&](int column, int row) {
    /* Newton's method on the cell's patch, from where the place is in its bounds: most cells are nearly boxes, so it is
     * all but there, and this runs for every place bent. */
    const float2 a = cell_lo[size_t(row * columns + column)], b = cell_hi[size_t(row * columns + column)];
    float2 st(std::clamp((p.x - a.x) / std::max(b.x - a.x, 1e-12f), 0.05f, 0.95f),
              std::clamp((p.y - a.y) / std::max(b.y - a.y, 1e-12f), 0.05f, 0.95f));
    bool converged = false;
    for (int it = 0; it < 40; it++) {
      const float2 here = eval(column, row, st);
      const float2 f = here - p;
      if (length(f) < near_enough) {
        converged = true;
        break;
      }
      const float h = 1e-3f;
      const float2 ds = (eval(column, row, {st.x + h, st.y}) - here) / h;
      const float2 dt = (eval(column, row, {st.x, st.y + h}) - here) / h;
      const float det = ds.x * dt.y - dt.x * ds.y;
      if (std::fabs(det) < 1e-12f) {
        break;
      }
      float2 step((dt.y * f.x - dt.x * f.y) / det, (-ds.y * f.x + ds.x * f.y) / det);
      /* Not too far at once: a folded cell can throw it a long way. */
      const float reach = length(step);
      if (reach > 2.0f) {
        step = step * (2.0f / reach);
      }
      st = st - step;
    }
    if (!converged) {
      return;
    }
    /* How far outside the cell the place is (0 inside): the cell holding it, else the nearest. */
    const float out = std::max({0.0f, -st.x, st.x - 1.0f, -st.y, st.y - 1.0f});
    if (out < best) {
      best = out;
      r_place = {column, row, st};
      found = true;
    }
  };
  const auto in_bounds = [&](int column, int row) {
    const float2 a = cell_lo[size_t(row * columns + column)], b = cell_hi[size_t(row * columns + column)];
    return p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y;
  };
  /* Only the cells whose bounds hold it, for the first that holds it: a place outside them all looks at the others too,
   * for the nearest. */
  for (int row = 0; row < rows; row++) {
    for (int column = 0; column < columns; column++) {
      if (in_bounds(column, row)) {
        try_cell(column, row);
        if (found && best <= 0.0f) {
          return true;
        }
      }
    }
  }
  for (int row = 0; row < rows; row++) {
    for (int column = 0; column < columns; column++) {
      if (!in_bounds(column, row)) {
        try_cell(column, row);
      }
    }
  }
  return found;
}

bool grid_locate(const VectorDrawing &rest, const DrawingGrid &grid, float2 p, GridPlace &r_place)
{
  GridSurface surface;
  return surface.build(rest, grid) && surface.locate(p, r_place);
}

float2 grid_place(const VectorDrawing &bent, const DrawingGrid &grid, const GridPlace &place)
{
  GridSurface surface;
  if (!surface.build(bent, grid) || place.column >= surface.columns || place.row >= surface.rows) {
    return {};
  }
  return surface.eval(place.column, place.row, place.st);
}

float2 grid_map(const VectorDrawing &rest, const VectorDrawing &bent, const DrawingGrid &grid, float2 p)
{
  GridSurface from, to;
  GridPlace place;
  if (!from.build(rest, grid) || !to.build(bent, grid) || !from.locate(p, place)) {
    return p;
  }
  return to.eval(place.column, place.row, place.st);
}

}  // namespace oanim
