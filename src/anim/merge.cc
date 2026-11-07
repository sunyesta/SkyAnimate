#include "anim/merge.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>

#pragma warning(push, 0)
#include <clipper2/clipper.h>
#pragma warning(pop)

#include "anim/deform.hh"
#include "data/document.hh"
#include "data/fill_area.hh"

namespace oanim {

namespace c2 = Clipper2Lib;

/* Clipper works in fixed point: 5 decimals of a unit is far finer than anything drawn. */
static constexpr int PRECISION = 5;

bool merge_takes_shape(const VectorDrawing &drawing, const Shape &shape, const std::vector<uint64_t> &ids)
{
  if (!ids.empty() && std::find(ids.begin(), ids.end(), shape.id) == ids.end()) {
    return false;
  }
  std::vector<OrientedEdge> chain;
  bool closed = false;
  return !shape.edges.empty() && shape_chain(drawing, shape, chain, &closed) && closed;
}

namespace {

/* A shape's outline as a polygon, with the line's width at each corner (absolute, layer units). */
struct Polygon {
  c2::PathD path;
  std::vector<float> widths;
  int owner = 0;
};

void add_shape_polygons(const VectorDrawing &drawing, const Shape &shape, const Transform2D &to_layer, int owner,
                        std::vector<Polygon> &r_polygons)
{
  const float scale = to_layer.scale();
  std::vector<float2> points;
  std::vector<float> radii;
  /* With a fill rule its loops fill together: their pieces go in as they fill, outer rings one way round
   * and holes the other, so the holes stay holes. */
  if (shape_fills_together(drawing, shape)) {
    const float width = shape.style.line_width * scale;
    for (const FillPiece &piece : fill_area(shape_fill_loops(drawing, shape, 12, 0.0002f), shape.fill_rule)) {
      for (const std::vector<float2> &ring : piece.rings) {
        Polygon poly;
        poly.owner = owner;
        for (const float2 &q : ring) {
          const float2 p = to_layer.apply(q);
          poly.path.push_back({double(p.x), double(p.y)});
          poly.widths.push_back(width);
        }
        /* A flip (a mirrored source) turns every ring: turn them back. */
        if ((c2::Area(poly.path) < 0.0) != (&ring != &piece.rings.front())) {
          std::reverse(poly.path.begin(), poly.path.end());
        }
        r_polygons.push_back(std::move(poly));
      }
    }
    return;
  }
  for (const Shape &run : shape_runs(drawing, shape)) {
    bool closed = false;
    points.clear();
    radii.clear();
    /* Fine enough that the curves stay smooth at any zoom a merge is looked at. */
    shape_outline(drawing, run, 12, points, &radii, &closed, nullptr, nullptr, 0.0002f);
    if (!closed || points.size() < 3) {
      continue;
    }
    Polygon poly;
    poly.owner = owner;
    for (size_t i = 0; i < points.size(); i++) {
      const float2 p = to_layer.apply(points[i]);
      poly.path.push_back({double(p.x), double(p.y)});
      poly.widths.push_back((i < radii.size() ? radii[i] : 1.0f) * shape.style.line_width * scale);
    }
    /* All the same way round: a shape drawn (or flipped, or mirrored) the other way would count
     * against the others where they overlap, cutting it out instead of joining it. */
    if (c2::Area(poly.path) < 0.0) {
      std::reverse(poly.path.begin(), poly.path.end());
      std::reverse(poly.widths.begin(), poly.widths.end());
    }
    r_polygons.push_back(std::move(poly));
  }
}

bool segments_cross(const c2::PointD &a, const c2::PointD &b, const c2::PointD &c, const c2::PointD &d,
                    c2::PointD *r_at)
{
  const double rx = b.x - a.x, ry = b.y - a.y, sx = d.x - c.x, sy = d.y - c.y;
  const double den = rx * sy - ry * sx;
  if (std::fabs(den) < 1e-18) {
    return false;
  }
  const double t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / den;
  const double u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
  if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) {
    return false;
  }
  *r_at = {a.x + rx * t, a.y + ry * t};
  return true;
}

c2::RectD bounds(const c2::PathD &path)
{
  c2::RectD r(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
              -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max());
  for (const c2::PointD &p : path) {
    r.left = std::min(r.left, p.x);
    r.top = std::min(r.top, p.y);
    r.right = std::max(r.right, p.x);
    r.bottom = std::max(r.bottom, p.y);
  }
  return r;
}

/* Where the outlines of different shapes cross: the corners Smoothing rounds. */
std::vector<c2::PointD> junctions(const std::vector<Polygon> &polygons)
{
  std::vector<c2::PointD> found;
  for (size_t i = 0; i < polygons.size(); i++) {
    const c2::RectD bi = bounds(polygons[i].path);
    for (size_t j = i + 1; j < polygons.size(); j++) {
      const c2::RectD bj = bounds(polygons[j].path);
      if (bi.right < bj.left || bj.right < bi.left || bi.bottom < bj.top || bj.bottom < bi.top) {
        continue;
      }
      const c2::PathD &p = polygons[i].path, &q = polygons[j].path;
      for (size_t a = 0; a < p.size(); a++) {
        const c2::PointD &a0 = p[a], &a1 = p[(a + 1) % p.size()];
        const double lx = std::min(a0.x, a1.x), hx = std::max(a0.x, a1.x);
        const double ly = std::min(a0.y, a1.y), hy = std::max(a0.y, a1.y);
        if (hx < bj.left || lx > bj.right || hy < bj.top || ly > bj.bottom) {
          continue;
        }
        for (size_t b = 0; b < q.size(); b++) {
          const c2::PointD &b0 = q[b], &b1 = q[(b + 1) % q.size()];
          if (std::max(b0.x, b1.x) < lx || std::min(b0.x, b1.x) > hx || std::max(b0.y, b1.y) < ly ||
              std::min(b0.y, b1.y) > hy)
          {
            continue;
          }
          c2::PointD at;
          if (segments_cross(a0, a1, b0, b1, &at)) {
            found.push_back(at);
          }
        }
      }
    }
  }
  return found;
}

c2::PathsD circles(const std::vector<c2::PointD> &centers, double radius)
{
  c2::PathsD out;
  constexpr int SIDES = 48;
  for (const c2::PointD &c : centers) {
    c2::PathD circle;
    for (int k = 0; k < SIDES; k++) {
      const double a = 2.0 * PI * k / SIDES;
      circle.push_back({c.x + radius * std::cos(a), c.y + radius * std::sin(a)});
    }
    out.push_back(std::move(circle));
  }
  return c2::Union(out, c2::FillRule::NonZero, PRECISION);
}

/* The merged outlines: the boolean, then the corners where outlines met rounded, only there (a
 * shape's own drawn corners stay as they are). */
c2::PathsD merge_paths(const std::vector<Polygon> &polygons, MergeOperation operation, double smoothing,
                       bool corners_only)
{
  c2::PathsD own, others;
  for (const Polygon &p : polygons) {
    (p.owner == 0 ? own : others).push_back(p.path);
  }
  own = c2::Union(own, c2::FillRule::NonZero, PRECISION);
  others = c2::Union(others, c2::FillRule::NonZero, PRECISION);
  if (corners_only) {
    /* The union's rounded corners, added to the layer's own shapes alone. */
    const c2::PathsD all = merge_paths(polygons, MergeOperation::Union, smoothing, false);
    const c2::PathsD corners = c2::Difference(all, c2::Union(own, others, c2::FillRule::NonZero, PRECISION),
                                              c2::FillRule::NonZero, PRECISION);
    /* None (no Smoothing, or nothing met): empty, and the shapes stay as drawn. */
    if (std::all_of(corners.begin(), corners.end(), [](const c2::PathD &p) { return std::fabs(c2::Area(p)) < 1e-12; })) {
      return {};
    }
    return c2::Union(own, corners, c2::FillRule::NonZero, PRECISION);
  }
  c2::PathsD result;
  switch (operation) {
    case MergeOperation::Union:
      result = c2::Union(own, others, c2::FillRule::NonZero, PRECISION);
      break;
    case MergeOperation::Subtract:
      result = c2::Difference(own, others, c2::FillRule::NonZero, PRECISION);
      break;
    case MergeOperation::Intersect:
      result = c2::Intersect(own, others, c2::FillRule::NonZero, PRECISION);
      break;
  }
  if (smoothing <= 0.0 || result.empty()) {
    return result;
  }
  const std::vector<c2::PointD> joins = junctions(polygons);
  if (joins.empty()) {
    return result;
  }
  /* A fillet of radius r reaches r / tan(half the angle) from its corner: four radii covers all but
   * the sharpest. */
  const c2::PathsD near = circles(joins, smoothing * 4.0);
  const double arc = smoothing * 0.01;
  if (operation == MergeOperation::Union) {
    /* Growing then shrinking fills the inner corners in, rounded. */
    const c2::PathsD grown = c2::InflatePaths(result, smoothing, c2::JoinType::Round, c2::EndType::Polygon, 2.0,
                                              PRECISION, arc);
    const c2::PathsD closed = c2::InflatePaths(grown, -smoothing, c2::JoinType::Round, c2::EndType::Polygon, 2.0,
                                               PRECISION, arc);
    const c2::PathsD fillets = c2::Intersect(closed, near, c2::FillRule::NonZero, PRECISION);
    return c2::Union(result, fillets, c2::FillRule::NonZero, PRECISION);
  }
  /* Shrinking then growing rounds the points off where an outline was cut. */
  const c2::PathsD shrunk = c2::InflatePaths(result, -smoothing, c2::JoinType::Round, c2::EndType::Polygon, 2.0,
                                             PRECISION, arc);
  const c2::PathsD opened = c2::InflatePaths(shrunk, smoothing, c2::JoinType::Round, c2::EndType::Polygon, 2.0,
                                             PRECISION, arc);
  const c2::PathsD tips = c2::Intersect(c2::Difference(result, opened, c2::FillRule::NonZero, PRECISION), near,
                                        c2::FillRule::NonZero, PRECISION);
  return c2::Difference(result, tips, c2::FillRule::NonZero, PRECISION);
}

uint64_t mix(uint64_t h, uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h * 0xff51afd7ed558ccdull;
}

uint64_t mix_double(uint64_t h, double v)
{
  uint64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  return mix(h, bits);
}

/* The last merges worked out, by what went in: drawing, picking and masks all ask for the same one
 * each frame. One for each thread: a render bends its drawings on several at once. */
const c2::PathsD &merge_paths_cached(const std::vector<Polygon> &polygons, MergeOperation operation, double smoothing,
                                     bool corners_only)
{
  uint64_t key = mix(uint64_t(operation) | uint64_t(corners_only) << 4, polygons.size());
  key = mix_double(key, smoothing);
  for (const Polygon &p : polygons) {
    key = mix(key, uint64_t(p.owner) + p.path.size() * 3);
    for (const c2::PointD &q : p.path) {
      key = mix_double(mix_double(key, q.x), q.y);
    }
  }
  static thread_local std::map<uint64_t, c2::PathsD> cache;
  if (auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }
  if (cache.size() > 64) {
    cache.clear();
  }
  return cache.emplace(key, merge_paths(polygons, operation, smoothing, corners_only)).first->second;
}

/* Everything of a drawing a merge reads: its points, curves and shapes. */
uint64_t drawing_merge_hash(const VectorDrawing &d)
{
  uint64_t h = mix(d.points.size(), d.curves.size() * 31 + d.shapes.size());
  for (const MeshPoint &p : d.points) {
    h = mix_double(mix_double(mix_double(h, p.pos.x), p.pos.y), p.radius);
    h = mix(h, p.id);
  }
  for (const Curve &c : d.curves) {
    h = mix(h, c.points.size() * 2 + (c.closed ? 1 : 0));
    for (const CurvePoint &cp : c.points) {
      h = mix(h, uint64_t(cp.point) * 4 + uint64_t(cp.handles_set) * 2 + uint64_t(cp.edge_hidden));
      for (const float v : {cp.handle_in.x, cp.handle_in.y, cp.handle_out.x, cp.handle_out.y, cp.curvature,
                            cp.weight_in, cp.weight_out, cp.offset_in, cp.offset_out})
      {
        h = mix_double(h, v);
      }
    }
  }
  for (const Shape &s : d.shapes) {
    h = mix(h, s.id * 8 + uint64_t(s.has_fill) * 4 + uint64_t(s.has_outline) * 2 + uint64_t(s.hidden));
    h = mix_double(h, s.style.line_width);
    for (const ShapeEdge &e : s.edges) {
      h = mix(h, uint64_t(e.curve) << 32 | uint64_t(uint32_t(e.segment)));
    }
  }
  return h;
}

}  // namespace

/* What a merge makes, apart from the drawing it goes in: the shapes it takes (by place) and each
 * outline's corners. Kept by what goes in, so a merge asked for again (drawing, picking and masks ask
 * each frame; zooming every frame) is only put into the drawing as it is now (its looks, ids). */
struct MergeOutline {
  struct Corner {
    float2 pos;
    float radius = 1.0f;
    float2 handle_in, handle_out;
  };
  std::vector<int> taken;
  std::vector<std::vector<Corner>> pieces;
  /* Whether each piece is a hole (it runs the other way round). */
  std::vector<bool> holes;
};

static MergeOutline merge_outline(const VectorDrawing &drawing, const std::vector<uint64_t> &own,
                                  const std::vector<MergeSource> &sources, MergeOperation operation, float smoothing,
                                  bool corners_only)
{
  MergeOutline out;
  std::vector<Polygon> polygons;
  for (int s = 0; s < int(drawing.shapes.size()); s++) {
    const Shape &shape = drawing.shapes[s];
    if (!shape.hidden && merge_takes_shape(drawing, shape, own)) {
      out.taken.push_back(s);
      add_shape_polygons(drawing, shape, Transform2D(), 0, polygons);
    }
  }
  if (out.taken.empty()) {
    return out;
  }
  for (const MergeSource &source : sources) {
    for (const Shape &shape : source.drawing->shapes) {
      if (!shape.hidden && merge_takes_shape(*source.drawing, shape, source.shapes)) {
        add_shape_polygons(*source.drawing, shape, source.to_layer, 1, polygons);
      }
    }
  }
  const Shape &look = drawing.shapes[out.taken.front()];
  const c2::PathsD &paths = merge_paths_cached(polygons, operation, std::max(smoothing, 0.0f), corners_only);
  if (corners_only && paths.empty()) {
    out.taken.clear();
    return out;
  }

  /* Each outline a closed curve through its corners: smooth where it bends gently, sharp where it
   * turns hard (the shapes' own corners, and where they met with no Smoothing). */
  const float line_width = std::max(look.style.line_width, 1e-9f);
  double largest = 0.0;
  for (const c2::PathD &raw : paths) {
    largest = std::max(largest, c2::Area(raw));
  }
  for (const c2::PathD &raw : paths) {
    /* A sliver the booleans leave where outlines run together. Holes (running the other way) are kept:
     * the result fills with them cut out. */
    if (std::fabs(c2::Area(raw)) <= std::max(1e-12, largest * 1e-5)) {
      continue;
    }
    const c2::PathD path = c2::SimplifyPath(raw, 0.00002, true);
    const int n = int(path.size());
    if (n < 3) {
      continue;
    }
    std::vector<MergeOutline::Corner> piece;
    for (int i = 0; i < n; i++) {
      const float2 p(float(path[i].x), float(path[i].y));
      const float2 prev(float(path[(i + n - 1) % n].x), float(path[(i + n - 1) % n].y));
      const float2 next(float(path[(i + 1) % n].x), float(path[(i + 1) % n].y));
      /* The line as wide as where it came from. */
      float width = line_width, best = std::numeric_limits<float>::max();
      for (const Polygon &poly : polygons) {
        for (size_t k = 0; k < poly.path.size(); k++) {
          const float d = float((poly.path[k].x - p.x) * (poly.path[k].x - p.x) +
                                (poly.path[k].y - p.y) * (poly.path[k].y - p.y));
          if (d < best) {
            best = d;
            width = poly.widths[k];
          }
        }
      }
      MergeOutline::Corner corner;
      corner.pos = p;
      corner.radius = std::clamp(width / line_width, 0.0f, POINT_RADIUS_MAX);
      const float2 in_dir = p - prev, out_dir = next - p;
      const float lin = length(in_dir), lout = length(out_dir);
      const float turn = (lin > 1e-9f && lout > 1e-9f) ? std::acos(std::clamp(dot(in_dir, out_dir) / (lin * lout), -1.0f, 1.0f))
                                                     : PI;
      if (turn < 40.0f * PI / 180.0f) {
        const float2 t = normalize(next - prev);
        corner.handle_in = t * (-lin / 3.0f);
        corner.handle_out = t * (lout / 3.0f);
      }
      piece.push_back(corner);
    }
    out.pieces.push_back(std::move(piece));
    out.holes.push_back(c2::Area(raw) < 0.0);
  }
  return out;
}

bool drawing_merge(VectorDrawing &drawing, const std::vector<uint64_t> &own, const std::vector<MergeSource> &sources,
                   MergeOperation operation, float smoothing, bool corners_only)
{
  uint64_t key = mix(drawing_merge_hash(drawing), uint64_t(operation) | uint64_t(corners_only) << 4);
  key = mix_double(key, smoothing);
  for (const uint64_t id : own) {
    key = mix(key, id);
  }
  for (const MergeSource &source : sources) {
    key = mix(key, drawing_merge_hash(*source.drawing));
    for (const float v : {source.to_layer.a, source.to_layer.b, source.to_layer.c, source.to_layer.d,
                          source.to_layer.tx, source.to_layer.ty})
    {
      key = mix_double(key, v);
    }
    key = mix(key, source.shapes.size());
    for (const uint64_t id : source.shapes) {
      key = mix(key, id);
    }
  }
  /* One for each thread, as merge_paths_cached's. */
  static thread_local std::map<uint64_t, MergeOutline> cache;
  auto it = cache.find(key);
  if (it == cache.end()) {
    if (cache.size() > 64) {
      cache.clear();
    }
    it = cache.emplace(key, merge_outline(drawing, own, sources, operation, smoothing, corners_only)).first;
  }
  const MergeOutline &out = it->second;
  if (out.taken.empty()) {
    return false;
  }

  std::vector<Shape> made;
  const Shape look = drawing.shapes[out.taken.front()];
  /* With holes, the pieces are one shape filling even-odd (the holes inside the pieces round them). */
  const bool any_hole = std::find(out.holes.begin(), out.holes.end(), true) != out.holes.end();
  for (const std::vector<MergeOutline::Corner> &piece : out.pieces) {
    Curve curve;
    curve.closed = true;
    for (const MergeOutline::Corner &corner : piece) {
      MeshPoint point;
      point.pos = corner.pos;
      point.setup_pos = corner.pos;
      point.radius = corner.radius;
      point.id = drawing.next_id++;
      drawing.points.push_back(point);
      CurvePoint cp;
      cp.point = int(drawing.points.size()) - 1;
      cp.handles_set = true;
      cp.handle_in = corner.handle_in;
      cp.handle_out = corner.handle_out;
      cp.setup_in = cp.handle_in;
      cp.setup_out = cp.handle_out;
      curve.points.push_back(cp);
    }
    drawing.curves.push_back(std::move(curve));
    const int c = int(drawing.curves.size()) - 1;
    if (any_hole && !made.empty()) {
      for (int s = 0; s < int(piece.size()); s++) {
        made.front().edges.push_back({c, s});
      }
      continue;
    }
    Shape shape = look;
    shape.merge_made = true;
    shape.edges.clear();
    if (any_hole) {
      shape.fill_rule = FillRule::EvenOdd;
    }
    for (int s = 0; s < int(piece.size()); s++) {
      shape.edges.push_back({c, s});
    }
    if (!made.empty()) {
      /* More than one piece: each its own shape, with a number of its own. */
      shape.id = drawing.next_id++;
    }
    made.push_back(std::move(shape));
  }
  const int at = out.taken.front();
  for (auto t = out.taken.rbegin(); t != out.taken.rend(); ++t) {
    drawing.shapes.erase(drawing.shapes.begin() + *t);
  }
  drawing.shapes.insert(drawing.shapes.begin() + std::min(at, int(drawing.shapes.size())), made.begin(), made.end());
  return true;
}

static bool shown_in(const std::vector<Layer> &list, uint64_t id, bool *r_shown)
{
  for (const Layer &layer : list) {
    if (layer.id == id) {
      *r_shown = layer.shown_in_render();
      return true;
    }
    if (shown_in(layer.children, id, r_shown)) {
      *r_shown = *r_shown && layer.shown_in_render();
      return true;
    }
  }
  return false;
}

bool layer_shown_in_document(const Document &doc, uint64_t id)
{
  bool shown = false;
  return shown_in(doc.layers, id, &shown) && shown;
}

static bool merge_on(const Modifier &m, bool render)
{
  return m.type == ModifierType::Boolean && (render ? m.show_render : m.show_viewport);
}

/* The layers whose Boolean modifiers are being worked out: one asking for its own merge again (two
 * layers merging each other) stops there. On each thread its own. */
static thread_local std::vector<uint64_t> g_merging;

void merge_modifier_apply(const Document &doc, const Layer &layer, const Modifier &m, VectorDrawing &drawing,
                          bool render)
{
  if (std::find(g_merging.begin(), g_merging.end(), layer.id) != g_merging.end()) {
    return;
  }
  g_merging.push_back(layer.id);
  const Transform2D to_layer = layer_world_transform(doc, layer.id).inverted();
  std::vector<VectorDrawing> drawings;
  drawings.reserve(m.merge_targets.size());
  std::vector<MergeSource> sources;
  for (const MergeTarget &t : m.merge_targets) {
    const Layer *target = layer_find(doc, t.layer);
    /* Joined, a hidden layer is out of it; cutting (Subtract, Intersect), it cuts as a mask shape does,
     * hidden or not. */
    if (!target || target->id == layer.id || target->type != LayerType::Vector ||
        std::find(g_merging.begin(), g_merging.end(), target->id) != g_merging.end() ||
        (m.merge_operation == MergeOperation::Union && !layer_shown_in_document(doc, target->id)))
    {
      continue;
    }
    drawings.push_back(layer_evaluated_drawing(doc, *target, render));
    sources.push_back({nullptr, to_layer * layer_world_transform(doc, target->id), t.shapes});
  }
  for (size_t i = 0; i < sources.size(); i++) {
    sources[i].drawing = &drawings[i];
  }
  drawing_merge(drawing, m.merge_shapes, sources, m.merge_operation, m.merge_smoothing, m.merge_corners_only);
  g_merging.pop_back();
}

/* Calls `f(owner, modifier, target)` for each Boolean, on and shown, taking shapes from `layer`. */
template<typename F> static void each_merge_into(const Document &doc, const std::vector<Layer> &list,
                                                 const Layer &layer, bool render, F &&f)
{
  for (const Layer &owner : list) {
    for (const Modifier &m : owner.modifiers) {
      if (!merge_on(m, render) || owner.id == layer.id) {
        continue;
      }
      /* Corners Only takes nothing in. */
      if (m.merge_corners_only) {
        continue;
      }
      for (const MergeTarget &t : m.merge_targets) {
        if (t.layer == layer.id && layer_shown_in_document(doc, owner.id) &&
            (m.merge_operation != MergeOperation::Union || layer_shown_in_document(doc, layer.id)))
        {
          f(owner, m, t);
        }
      }
    }
    each_merge_into(doc, owner.children, layer, render, f);
  }
}

bool merge_takes_from(const Document &doc, const Layer &layer, bool render)
{
  if (layer_index_no_booleans(doc)) {
    return false;
  }
  bool any = false;
  each_merge_into(doc, doc.layers, layer, render, [&](const Layer &, const Modifier &, const MergeTarget &) {
    any = true;
  });
  return any;
}

void merge_mark_taken(const Document &doc, const Layer &layer, VectorDrawing &drawing, bool render)
{
  each_merge_into(doc, doc.layers, layer, render, [&](const Layer &owner, const Modifier &m, const MergeTarget &t) {
    /* Only when the owner really has a shape of its own to merge them into; and only what it
     * draws (a fill layer takes the fills, an outline layer the lines). */
    const VectorDrawing &own = layer_drawing(doc, owner);
    const auto look = std::find_if(own.shapes.begin(), own.shapes.end(), [&](const Shape &s) {
      return !s.hidden && merge_takes_shape(own, s, m.merge_shapes);
    });
    if (look == own.shapes.end()) {
      return;
    }
    for (Shape &shape : drawing.shapes) {
      if (merge_takes_shape(drawing, shape, t.shapes)) {
        shape.merged_fill |= look->has_fill;
        shape.merged_line |= look->has_outline;
      }
    }
  });
}

}  // namespace oanim
