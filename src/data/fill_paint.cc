#include "data/fill_paint.hh"

#include <algorithm>

#include "data/ink_stroke.hh"

namespace oanim {

/* What a shape's fill covers. */
static std::vector<FillPiece> shape_area(const VectorDrawing &drawing, const Shape &shape, float pixel)
{
  return fill_area(shape_fill_loops(drawing, shape, 12, pixel * 0.1f), shape.fill_rule);
}

/* Whether rebuilding a shape's curves would lose something: keys, or detail levels' points. */
static bool shape_keyed(const VectorDrawing &drawing, const Shape &shape)
{
  for (const ShapeEdge &edge : shape.edges) {
    for (const CurvePoint &cp : drawing.curves[edge.curve].points) {
      const MeshPoint &p = drawing.points[cp.point];
      if (p.pos_keys.animated() || p.radius_keys.animated() || p.detail.level > 0 || cp.curvature_keys.animated() ||
          cp.handle_in_keys.animated() || cp.handle_out_keys.animated())
      {
        return true;
      }
    }
  }
  return false;
}

/* Whether another shape runs along one of a shape's curves. */
static bool shape_shares_curve(const VectorDrawing &drawing, size_t index)
{
  const Shape &shape = drawing.shapes[index];
  for (size_t i = 0; i < drawing.shapes.size(); i++) {
    if (i == index) {
      continue;
    }
    for (const ShapeEdge &other : drawing.shapes[i].edges) {
      for (const ShapeEdge &edge : shape.edges) {
        if (other.curve == edge.curve) {
          return true;
        }
      }
    }
  }
  return false;
}

/* The pieces as shapes on new curves, each like `like`, filled and without an outline. */
static std::vector<Shape> pieces_to_shapes(VectorDrawing &drawing, const std::vector<FillPiece> &pieces, const Shape &like,
                                           float pixel, float tolerance)
{
  std::vector<Shape> shapes;
  for (const FillPiece &piece : pieces) {
    Shape shape = like;
    shape.edges.clear();
    shape.id = 0;
    shape.has_fill = true;
    shape.has_outline = false;
    shape.fill_rule = piece.rings.size() > 1 ? FillRule::EvenOdd : FillRule::Loops;
    for (const std::vector<float2> &ring : piece.rings) {
      const std::vector<InkKnot> knots = ink_fit_ring(ring, tolerance, pixel);
      if (knots.size() < 3) {
        continue;
      }
      Curve curve;
      curve.closed = true;
      for (const InkKnot &knot : knots) {
        CurvePoint cp{drawing_add_point(drawing, knot.pos)};
        cp.curvature = knot.corner ? CURVATURE_PEAKED : CURVATURE_SMOOTH;
        cp.aligned = !knot.corner;
        curve_point_set_handles(cp, knot.handle_in, knot.handle_out);
        curve.points.push_back(cp);
      }
      drawing.curves.push_back(std::move(curve));
      for (int s = 0; s < drawing.curves.back().segment_count(); s++) {
        shape.edges.push_back({int(drawing.curves.size()) - 1, s});
      }
    }
    if (!shape.edges.empty()) {
      shapes.push_back(std::move(shape));
    }
  }
  return shapes;
}

/* Takes away the curves the shapes in `was` ran along that no shape runs along now, and their points. */
static void remove_curves_left(VectorDrawing &drawing, const std::vector<Shape> &was)
{
  std::vector<bool> used(drawing.curves.size(), false), remove(drawing.curves.size(), false);
  for (const Shape &shape : drawing.shapes) {
    for (const ShapeEdge &edge : shape.edges) {
      used[edge.curve] = true;
    }
  }
  for (const Shape &shape : was) {
    for (const ShapeEdge &edge : shape.edges) {
      remove[edge.curve] = !used[edge.curve];
    }
  }
  drawing_remove_curves(drawing, remove);
  drawing_remove_unused_points(drawing);
}

void drawing_fill_paint(VectorDrawing &drawing, const std::vector<FillPiece> &area, const Style &style, float pixel,
                        float tolerance)
{
  std::vector<FillPiece> blob = area;
  std::vector<Shape> merged;
  /* Where the topmost merged shape was, counted without the merged ones under it. */
  int place = -1;
  std::vector<size_t> taken;
  for (size_t i = drawing.shapes.size(); i-- > 0;) {
    const Shape &shape = drawing.shapes[i];
    if (shape.hidden || !shape.has_fill) {
      continue;
    }
    const std::vector<FillPiece> covers = shape_area(drawing, shape, pixel);
    if (!fill_overlap(blob, covers)) {
      continue;
    }
    const bool same = !shape.has_outline && shape.material == 0 && shape.style.fill == style.fill;
    if (!same || shape_keyed(drawing, shape) || shape_shares_curve(drawing, i)) {
      break;
    }
    blob = fill_boolean(blob, covers, FillBoolean::Union);
    taken.push_back(i);
  }
  Shape like;
  like.style = style;
  like.name = "Fill " + std::to_string(drawing.shapes.size() + 1);
  if (!taken.empty()) {
    like.name = drawing.shapes[taken.front()].name;
    like.style = drawing.shapes[taken.front()].style;
    like.id = drawing.shapes[taken.front()].id;
    place = int(taken.front()) - int(taken.size()) + 1;
    for (const size_t i : taken) {
      merged.push_back(drawing.shapes[i]);
      drawing.shapes.erase(drawing.shapes.begin() + i);
    }
  }
  std::vector<Shape> made = pieces_to_shapes(drawing, blob, like, pixel, tolerance);
  /* The blob is what the shape it grew from became: references know it by its number. */
  if (!made.empty() && !taken.empty()) {
    made.front().id = like.id;
  }
  drawing.shapes.insert(place < 0 ? drawing.shapes.end() : drawing.shapes.begin() + place, made.begin(), made.end());
  remove_curves_left(drawing, merged);
}

bool drawing_fill_erase(VectorDrawing &drawing, const std::vector<FillPiece> &area, float pixel, float tolerance,
                        bool selected_only, int *r_skipped)
{
  const std::vector<bool> selected = drawing_selected_shapes(drawing);
  std::vector<Shape> kept, gone;
  bool cut = false;
  const std::vector<Shape> shapes = drawing.shapes;
  for (size_t i = 0; i < shapes.size(); i++) {
    const Shape &shape = shapes[i];
    if (shape.hidden || !shape.has_fill || (selected_only && !selected[i])) {
      kept.push_back(shape);
      continue;
    }
    const std::vector<FillPiece> covers = shape_area(drawing, shape, pixel);
    if (!fill_overlap(covers, area)) {
      kept.push_back(shape);
      continue;
    }
    if (shape_keyed(drawing, shape)) {
      *r_skipped += 1;
      kept.push_back(shape);
      continue;
    }
    cut = true;
    std::vector<Shape> made = pieces_to_shapes(drawing, fill_boolean(covers, area, FillBoolean::Subtract), shape, pixel,
                                               tolerance);
    if (shape.has_outline) {
      /* The line stays, on its own curves, over what is left of the fill. */
      kept.insert(kept.end(), made.begin(), made.end());
      kept.push_back(shape);
      kept.back().has_fill = false;
    }
    else {
      if (!made.empty()) {
        made.front().id = shape.id;
      }
      kept.insert(kept.end(), made.begin(), made.end());
      gone.push_back(shape);
    }
  }
  drawing.shapes = std::move(kept);
  remove_curves_left(drawing, gone);
  return cut;
}

}  // namespace oanim
