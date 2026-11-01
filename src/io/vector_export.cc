#include "io/vector_export.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>

#pragma warning(push, 0)
#include <clipper2/clipper.h>
#pragma warning(pop)

#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "anim/layer_pick.hh"
#include "assets/brushes.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/document.hh"
#include "data/layer.hh"
#include "data/media_layer.hh"
#include "data/vector.hh"
#include "gpu/brush_stroke.hh"
#include "gpu/tessellate.hh"
#include "media/media.hh"

namespace oanim {

namespace c2 = Clipper2Lib;

namespace {

/* What of a mask layer's shapes a step takes (as the renderer's mask pass). */
struct Pass {
  /* Grown (or, negative, shrunk) by this many pixels. */
  float grow = 0.0f;
  /* Everything; the areas; the bands around them that shrinking clears; the lines. */
  enum Stage { All, Areas, Bands, Lines } stage = All;
  bool exclude_strokes = false;
  bool exclude_fills = false;
  bool filled = false;
  bool inside_stroke = false;
  int extend = 0;
};

/* A mask card as it applies in renders: its steps with only the layers still there. */
struct Card {
  bool start_shown = true;
  MaskCuts cuts = MaskCuts::All;
  /* End Lines at Edge with Ends: Brush: the lines are ended at its edge, not cut by it. */
  bool brush_ends = false;
  std::vector<MaskStep> steps;
};

/* The masks applying to what is drawn, by what they cut (each a mask; all must show). */
struct Masks {
  std::vector<int> both, fills, lines;
};

const Color WHITE = {1, 1, 1, 1}, BLACK = {0, 0, 0, 1};

/* Brushed lines' stamps painted into a picture as the GPU draws them (each dab two textured triangles, its colour
 * premultiplied): laid over each other, or with Merged Alpha the strongest kept. */
class DabCanvas {
 public:
  DabCanvas(int x0, int y0, int width, int height) : x0_(x0), y0_(y0), width_(width), height_(height)
  {
    rgba_.assign(size_t(width) * size_t(height) * 4, 0.0f);
  }

  void triangle(const BrushVertex v[3], const Transform2D &to_canvas, const BrushImage &image, bool own_colors,
                bool merged, float opacity)
  {
    float2 p[3];
    for (int k = 0; k < 3; k++) {
      p[k] = to_canvas.apply(float2(v[k].x, v[k].y)) - float2(float(x0_), float(y0_));
    }
    const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[1].y - p[0].y) * (p[2].x - p[0].x);
    if (std::fabs(area) < 1e-12f) {
      return;
    }
    const int xa = std::max(int(std::floor(std::min({p[0].x, p[1].x, p[2].x}))), 0);
    const int xb = std::min(int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))), width_ - 1);
    const int ya = std::max(int(std::floor(std::min({p[0].y, p[1].y, p[2].y}))), 0);
    const int yb = std::min(int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))), height_ - 1);
    for (int y = ya; y <= yb; y++) {
      for (int x = xa; x <= xb; x++) {
        const float2 c(float(x) + 0.5f, float(y) + 0.5f);
        const auto edge = [&](float2 a, float2 b) { return ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / area; };
        const float w0 = edge(p[1], p[2]), w1 = edge(p[2], p[0]), w2 = edge(p[0], p[1]);
        if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
          continue;
        }
        const float u = v[0].u * w0 + v[1].u * w1 + v[2].u * w2;
        const float t = v[0].v * w0 + v[1].v * w1 + v[2].v * w2;
        float texel[4];
        sample(image, u, t, texel);
        const float vc[4] = {v[0].r * w0 + v[1].r * w1 + v[2].r * w2, v[0].g * w0 + v[1].g * w1 + v[2].g * w2,
                             v[0].b * w0 + v[1].b * w1 + v[2].b * w2, v[0].a * w0 + v[1].a * w1 + v[2].a * w2};
        float src[4];
        for (int k = 0; k < 4; k++) {
          src[k] = (own_colors ? texel[k] * vc[3] : vc[k] * texel[3]) * opacity;
        }
        float *dst = &rgba_[(size_t(y) * size_t(width_) + size_t(x)) * 4];
        for (int k = 0; k < 4; k++) {
          dst[k] = merged ? std::max(dst[k], src[k]) : src[k] + dst[k] * (1.0f - src[3]);
        }
      }
    }
  }

  /* Straight colours, bytes. */
  VectorPicture picture() const
  {
    VectorPicture out;
    out.width = width_;
    out.height = height_;
    out.rgba.resize(rgba_.size());
    for (size_t i = 0; i < rgba_.size(); i += 4) {
      const float a = std::clamp(rgba_[i + 3], 0.0f, 1.0f);
      for (int k = 0; k < 3; k++) {
        out.rgba[i + size_t(k)] = uint8_t(std::clamp(a > 0.0f ? rgba_[i + size_t(k)] / a : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
      out.rgba[i + 3] = uint8_t(a * 255.0f + 0.5f);
    }
    return out;
  }

  /* Whether anything was painted, and the part painted (pixels of the canvas). */
  bool painted(int *r_x0, int *r_y0, int *r_x1, int *r_y1) const
  {
    int x0 = width_, y0 = height_, x1 = -1, y1 = -1;
    for (int y = 0; y < height_; y++) {
      for (int x = 0; x < width_; x++) {
        if (rgba_[(size_t(y) * size_t(width_) + size_t(x)) * 4 + 3] > 0.5f / 255.0f) {
          x0 = std::min(x0, x);
          x1 = std::max(x1, x);
          y0 = std::min(y0, y);
          y1 = std::max(y1, y);
        }
      }
    }
    *r_x0 = x0;
    *r_y0 = y0;
    *r_x1 = x1;
    *r_y1 = y1;
    return x1 >= x0;
  }

  int x0_, y0_, width_, height_;

 private:
  std::vector<float> rgba_;

  /* Bilinear, clamped to the edge (as the brush textures are sampled). Premultiplied. */
  static void sample(const BrushImage &image, float u, float v, float r[4])
  {
    const float fx = u * float(image.width) - 0.5f, fy = v * float(image.height) - 0.5f;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const float ax = fx - float(ix), ay = fy - float(iy);
    for (int k = 0; k < 4; k++) {
      r[k] = 0.0f;
    }
    for (int dy = 0; dy < 2; dy++) {
      for (int dx = 0; dx < 2; dx++) {
        const int x = std::clamp(ix + dx, 0, image.width - 1), y = std::clamp(iy + dy, 0, image.height - 1);
        const float w = (dx ? ax : 1.0f - ax) * (dy ? ay : 1.0f - ay);
        const uint8_t *t = &image.rgba[(size_t(y) * size_t(image.width) + size_t(x)) * 4];
        for (int k = 0; k < 4; k++) {
          r[k] += float(t[k]) / 255.0f * w;
        }
      }
    }
  }
};

class SceneWalker {
 public:
  SceneWalker(const Document &doc, const VectorSceneOptions &options, VectorScene &scene)
      : doc_(doc), options_(options), scene_(scene)
  {
    const int width = scene.width, height = scene.height;
    /* As Render Frame: the camera's frame, 2 units tall, fills the picture; y runs down. */
    const float scale = float(height) * 0.5f;
    to_px_ = Transform2D{1, 0, 0, -1, 0, float(height)} *
             Transform2D{scale, 0, 0, scale, float(width) * 0.5f, float(height) * 0.5f} * camera_to_frame(doc);
    quality_ = std::clamp(doc.render_quality, 1, 64);
  }

  void walk()
  {
    if (!options_.transparent) {
      VectorNode background;
      background.kind = VectorNode::Path;
      background.path = rect_path(0.0f, 0.0f, float(scene_.width), float(scene_.height));
      background.color = doc_.background;
      background.color.a = 1.0f;
      scene_.nodes.push_back(std::move(background));
    }
    draw_layers(doc_.layers, 1.0f, Masks(), scene_.nodes);
    /* Media layers In Front: over everything, unmasked. */
    for (const Layer *l : document_layers_flat(doc_)) {
      if (l->is_media() && l->media.in_front && drawn_through_parents(*l)) {
        float opacity = 1.0f;
        for (const Layer *p = l; p; p = layer_parent(const_cast<Document &>(doc_), p->id)) {
          opacity *= p->opacity;
        }
        VectorNode group = layer_group(*l);
        picture(*l, opacity, -1, group.children);
        scene_.nodes.push_back(std::move(group));
      }
    }
  }

 private:
  const Document &doc_;
  const VectorSceneOptions &options_;
  VectorScene &scene_;
  Transform2D to_px_;
  int quality_ = 32;
  std::map<std::pair<uint64_t, int>, int> card_ids_;
  std::map<std::vector<int>, int> combined_ids_;

  /* --- What shows --------------------------------------------------------------------------- */

  static bool shown(const Layer &layer) { return layer.shown_in_render() && layer.render_visible; }

  /* What is drawn: what shows, or with hidden layers everything. */
  /* A child a switch isn't showing on this frame isn't part of its picture, hidden layers or not. */
  bool drawn(const Layer &layer) const { return !layer.switched_out && (options_.hidden_layers || shown(layer)); }

  bool drawn_through_parents(const Layer &layer) const
  {
    for (const Layer *l = &layer; l; l = layer_parent(const_cast<Document &>(doc_), l->id)) {
      if (!drawn(*l) || l->opacity <= 0.0f) {
        return false;
      }
    }
    return true;
  }

  VectorNode layer_group(const Layer &layer) const
  {
    VectorNode group;
    group.layer = layer.id;
    group.layer_shown = shown(layer);
    return group;
  }

  /* A layer's drawing as drawn this frame (its modifiers and bones), and where it goes. */
  const VectorDrawing &drawing_of(const Layer &layer, VectorDrawing &evaluated, Transform2D *r_transform) const
  {
    Transform2D rigid;
    const VectorDrawing *drawing = &layer_drawing(doc_, layer);
    if (layer_has_modifier_effect(doc_, layer, true)) {
      evaluated = layer_evaluated_drawing(doc_, layer, true, &rigid);
      drawing = &evaluated;
    }
    *r_transform = layer_world_transform(doc_, layer.id) * rigid;
    return *drawing;
  }

  /* --- Geometry ------------------------------------------------------------------------------ */

  static VectorPath rect_path(float x0, float y0, float x1, float y1)
  {
    VectorPath p;
    p.ops = {VectorPath::Move, VectorPath::Line, VectorPath::Line, VectorPath::Line, VectorPath::Close};
    p.points = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    return p;
  }

  /* A connected run of a shape's edges as Beziers (pixels), closed. */
  static void bezier_path(const VectorDrawing &drawing, const Shape &run, const Transform2D &to_px, VectorPath &path)
  {
    std::vector<OrientedEdge> chain;
    bool closed = false;
    shape_chain(drawing, run, chain, &closed);
    bool first = true;
    for (const OrientedEdge &o : chain) {
      float2 ctrl[4];
      curve_segment_bezier(drawing, drawing.curves[size_t(o.edge.curve)], o.edge.segment, ctrl);
      if (o.reversed) {
        std::swap(ctrl[0], ctrl[3]);
        std::swap(ctrl[1], ctrl[2]);
      }
      if (first) {
        path.ops.push_back(VectorPath::Move);
        path.points.push_back(to_px.apply(ctrl[0]));
        first = false;
      }
      path.ops.push_back(VectorPath::Cubic);
      for (int k = 1; k < 4; k++) {
        path.points.push_back(to_px.apply(ctrl[k]));
      }
    }
    if (!first) {
      path.ops.push_back(VectorPath::Close);
    }
  }

  /* A line as the renderer strokes it (the triangles it draws, without soft edges), made one outline. */
  static VectorPath stroke_path(const std::vector<float2> &points, const std::vector<float> &half_widths, bool closed,
                                const std::vector<StrokeJoin> &joins, const Transform2D &to_px)
  {
    std::vector<Vertex> vertices;
    tessellate_stroke(points, half_widths, closed, Color{1, 1, 1, 1}, 0.0f, vertices, &joins);
    /* Clipper works in whole numbers: thousandths of a pixel. */
    constexpr double UNITS = 1000.0;
    c2::Paths64 triangles;
    triangles.reserve(vertices.size() / 3);
    for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
      c2::Path64 t;
      for (size_t k = 0; k < 3; k++) {
        const float2 p = to_px.apply(float2(vertices[i + k].x, vertices[i + k].y));
        t.emplace_back(int64_t(std::llround(double(p.x) * UNITS)), int64_t(std::llround(double(p.y) * UNITS)));
      }
      const double area = c2::Area(t);
      if (area == 0.0) {
        continue;
      }
      if (area < 0.0) {
        std::reverse(t.begin(), t.end());
      }
      triangles.push_back(std::move(t));
    }
    VectorPath path;
    for (const c2::Path64 &ring : c2::Union(triangles, c2::FillRule::NonZero)) {
      if (ring.size() < 3) {
        continue;
      }
      for (size_t i = 0; i < ring.size(); i++) {
        path.ops.push_back(i == 0 ? VectorPath::Move : VectorPath::Line);
        path.points.emplace_back(float(double(ring[i].x) / UNITS), float(double(ring[i].y) / UNITS));
      }
      path.ops.push_back(VectorPath::Close);
    }
    return path;
  }

  static void polygon_path(const std::vector<float2> &polygon, const Transform2D &to_px, VectorPath &path)
  {
    for (size_t i = 0; i < polygon.size(); i++) {
      path.ops.push_back(i == 0 ? VectorPath::Move : VectorPath::Line);
      path.points.push_back(to_px.apply(polygon[i]));
    }
    if (!polygon.empty()) {
      path.ops.push_back(VectorPath::Close);
    }
  }

  static void element(std::vector<VectorNode> &to, VectorPath path, const Color &color, float alpha,
                      bool even_odd = false, int mask = -1)
  {
    if (path.empty()) {
      return;
    }
    VectorNode n;
    n.kind = VectorNode::Path;
    n.path = std::move(path);
    n.color = color;
    n.alpha = alpha;
    n.even_odd = even_odd;
    n.mask = mask;
    to.push_back(std::move(n));
  }

  /* A shape's fill: its run, or (filling together) every closed run of it with its fill rule. */
  static VectorPath fill_path(const VectorDrawing &drawing, const Shape &whole, const Shape &run,
                              const std::vector<Shape> &runs, bool together, const Transform2D &to_px)
  {
    VectorPath path;
    if (!together) {
      bezier_path(drawing, run, to_px, path);
      return path;
    }
    for (const Shape &r : runs) {
      std::vector<OrientedEdge> chain;
      bool loop = false;
      shape_chain(drawing, r, chain, &loop);
      if (loop) {
        bezier_path(drawing, r, to_px, path);
      }
    }
    (void)whole;
    return path;
  }

  /* --- Drawing layers in colour ----------------------------------------------------------------- */

  void draw_layers(const std::vector<Layer> &scope, float opacity, const Masks &outer, std::vector<VectorNode> &to)
  {
    /* Bottom first, in Draw Order, each with the masks of it and the groups holding it. */
    for (const DrawStep &step : draw_order_steps(doc_, scope, [this](const Layer &l) { return drawn(l); })) {
      Masks masks = outer;
      for (const Layer *l : step.masked) {
        int index = -1;
        for (const Card &card : cards_of(*l)) {
          index++;
          const bool fills = card.cuts != MaskCuts::Lines;
          const bool lines = card.cuts != MaskCuts::Fills && !card.brush_ends;
          if (!fills && !lines) {
            continue;
          }
          const int id = card_mask(*l, card, index);
          (fills && lines ? masks.both : fills ? masks.fills : masks.lines).push_back(id);
        }
      }
      draw_layer(*step.layer, opacity, masks, to);
    }
  }

  void draw_layer(const Layer &layer, float opacity, const Masks &masks, std::vector<VectorNode> &to)
  {
    if (!drawn(layer) || layer.opacity <= 0.0f) {
      return;
    }
    const float layer_opacity = opacity * layer.opacity;
    VectorNode group = layer_group(layer);
    group.mask = combined(masks.both);
    if (layer.is_media()) {
      if (!layer.media.in_front) {
        picture(layer, layer_opacity, combined(masks.fills), group.children);
      }
    }
    else if (!layer.is_group()) {
      /* A see-through layer fades as a whole (GPU Geometry), so its lines don't show its fills through them,
       * unless its lines and fills are masked apart; Classic fades each shape. */
      const bool whole = doc_.shading.gpu_geometry && layer_opacity < 1.0f && masks.fills.empty() &&
                         masks.lines.empty();
      if (whole) {
        group.opacity = layer_opacity;
      }
      drawing_colour(layer, whole ? 1.0f : layer_opacity, combined(masks.fills), combined(masks.lines),
                     group.children);
    }
    else {
      /* A faded group as a whole, so its overlapping layers don't show through each other. */
      Masks inner;
      inner.fills = masks.fills;
      inner.lines = masks.lines;
      const bool faded = layer.opacity < 1.0f;
      if (faded) {
        group.opacity = layer_opacity;
      }
      draw_layers(layer.children, faded ? 1.0f : layer_opacity, inner, group.children);
    }
    if (!group.children.empty()) {
      to.push_back(std::move(group));
    }
  }

  /* A drawing layer's shapes in their colours, each shape's fill then its line, as the renderer draws them. */
  void drawing_colour(const Layer &layer, float opacity, int fill_mask, int line_mask, std::vector<VectorNode> &to)
  {
    VectorDrawing evaluated;
    Transform2D transform;
    const VectorDrawing &drawing = drawing_of(layer, evaluated, &transform);
    if (drawing.shapes.empty()) {
      return;
    }
    const Transform2D to_px = to_px_ * transform;
    const float pixel = 1.0f / std::max(to_px.scale(), 1e-6f);
    const LineCutter cutter = line_cutter(doc_, layer, true);
    /* The drawing as on the setup frame, which brushed lines lay their stamps out on (made when one needs it). */
    VectorDrawing setup;
    bool setup_made = false;
    std::vector<float2> outline;
    std::vector<float> radii;
    std::vector<bool> visible;
    std::vector<StrokeJoin> joins;
    for (int shape_index = 0; shape_index < int(drawing.shapes.size()); shape_index++) {
      const Shape &whole = drawing.shapes[size_t(shape_index)];
      const std::vector<Shape> runs = shape_runs(drawing, whole);
      const bool fills_together = whole.fill_rule != FillRule::Loops && runs.size() > 1;
      bool filled_together = false;
      std::vector<Shape> setup_runs;
      bool setup_runs_made = false;
      for (size_t run_index = 0; run_index < runs.size(); run_index++) {
        const Shape &run = runs[run_index];
        if (run.hidden || (run.merged_fill && run.merged_line)) {
          continue;
        }
        const bool has_fill = run.has_fill && !run.merged_fill;
        const bool has_outline = run.has_outline && !run.merged_line;
        bool closed = false;
        shape_outline(drawing, run, quality_, outline, &radii, &closed, &visible, &joins,
                      pixel * 3.0f / float(quality_));
        if (outline.size() < 2) {
          continue;
        }
        if (has_fill && closed && (!fills_together || !filled_together)) {
          filled_together = fills_together;
          element(to, fill_path(drawing, whole, run, runs, fills_together, to_px), run.style.fill, opacity, fills_together && whole.fill_rule == FillRule::EvenOdd, fill_mask);
          scene_.fills++;
        }
        if (!has_outline || run.style.line_width <= 0.0f) {
          continue;
        }
        /* A brushed line, if its brush is in the library and has images: a picture of its stamps. */
        const Brush *brush = run.style.brush.name.empty() ? nullptr : brush_find(run.style.brush.name);
        const std::vector<BrushImage> *images = brush ? &brush_tip_images(*brush, run.style.brush) : nullptr;
        if (images && !images->empty()) {
          if (!setup_made) {
            setup = layer_setup_drawing(doc_, layer, true);
            setup_made = true;
          }
          if (!setup_runs_made && !run.merge_made && shape_index < int(setup.shapes.size()) &&
              setup.shapes.size() == drawing.shapes.size())
          {
            setup_runs = shape_runs(setup, setup.shapes[size_t(shape_index)]);
          }
          setup_runs_made = true;
          const BrushPath shown_path = brush_path(drawing, run, 0.0f);
          const BrushPath rest = run_index < setup_runs.size() ? brush_path(setup, setup_runs[run_index], 0.0f)
                                                                : shown_path;
          brushed_line(layer, run, shape_index, *brush, *images, shown_path, rest, cutter, transform, opacity,
                       line_mask, to);
          continue;
        }
        bool any_width = false;
        for (float &r : radii) {
          r = std::max(r * run.style.line_width * 0.5f, 0.0f);
          any_width |= r > 0.0f;
        }
        if (!any_width) {
          continue;
        }
        line_runs(outline, radii, visible, joins, closed, cutter.empty() ? nullptr : &cutter, transform,
                  [&](const std::vector<float2> &pts, const std::vector<float> &half, bool is_closed,
                      const std::vector<StrokeJoin> &stroke_joins) {
                    element(to, stroke_path(pts, half, is_closed, stroke_joins, to_px), run.style.line, opacity, false, line_mask);
                    scene_.lines++;
                  });
      }
    }
  }

  /* A brushed line's stamps, laid out as the renderer lays them, painted into a picture where they fall. */
  void brushed_line(const Layer &layer, const Shape &run, int shape_index, const Brush &brush,
                    const std::vector<BrushImage> &images, const BrushPath &shown_path, const BrushPath &rest,
                    const LineCutter &cutter, const Transform2D &transform, float opacity, int line_mask,
                    std::vector<VectorNode> &to)
  {
    const Transform2D to_px = to_px_ * transform;
    const float pixel = 1.0f / std::max(to_px.scale(), 1e-6f);
    std::vector<float> aspects;
    for (const BrushImage &image : images) {
      aspects.push_back(float(image.width) / float(std::max(image.height, 1)));
    }
    BrushCut cut;
    cut.shows = [&](float2 p) { return line_cutter_shows(cutter, transform.apply(p)); };
    cut.taper = !cutter.flat;
    cut.reach = cutter.reach;
    BrushDabs dabs;
    brush_stroke(shown_path, rest, run.style.brush, run.style.line, aspects, pixel * 0.5f,
                 brush_seed(run.style.brush, layer.id, shape_index, doc_.frame_current), dabs,
                 cutter.empty() ? nullptr : &cut, true);
    if (dabs.count <= 0) {
      return;
    }
    /* The stamps' bounds, in picture pixels, within the frame (a little past it). */
    const float s = std::max(options_.picture_scale, 0.25f);
    const Transform2D to_canvas = Transform2D::scaling({s, s}) * to_px;
    float2 lo(1e30f, 1e30f), hi(-1e30f, -1e30f);
    for (const auto &list : dabs.per_image) {
      for (const BrushVertex &v : list) {
        const float2 p = to_canvas.apply(float2(v.x, v.y));
        lo = float2(std::min(lo.x, p.x), std::min(lo.y, p.y));
        hi = float2(std::max(hi.x, p.x), std::max(hi.y, p.y));
      }
    }
    const float margin = 8.0f * s;
    const int x0 = int(std::floor(std::max(lo.x, -margin))), y0 = int(std::floor(std::max(lo.y, -margin)));
    const int x1 = int(std::ceil(std::min(hi.x, float(scene_.width) * s + margin)));
    const int y1 = int(std::ceil(std::min(hi.y, float(scene_.height) * s + margin)));
    if (x1 <= x0 || y1 <= y0) {
      return;
    }
    DabCanvas canvas(x0, y0, x1 - x0, y1 - y0);
    const bool own_colors = brush.is_color && !run.style.brush.tint;
    const bool merged = run.style.brush.merged_alpha;
    for (size_t i = 0; i < dabs.per_image.size() && i < images.size(); i++) {
      const std::vector<BrushVertex> &list = dabs.per_image[i];
      for (size_t k = 0; k + 2 < list.size(); k += 3) {
        canvas.triangle(&list[k], to_canvas, images[i], own_colors, merged, merged ? 1.0f : opacity);
      }
    }
    int px0, py0, px1, py1;
    if (!canvas.painted(&px0, &py0, &px1, &py1)) {
      return;
    }
    /* Cropped to what was painted. */
    VectorPicture full = canvas.picture();
    VectorPicture picture;
    picture.width = px1 - px0 + 1;
    picture.height = py1 - py0 + 1;
    picture.rgba.resize(size_t(picture.width) * size_t(picture.height) * 4);
    for (int y = 0; y < picture.height; y++) {
      std::copy_n(&full.rgba[(size_t(y + py0) * size_t(full.width) + size_t(px0)) * 4], size_t(picture.width) * 4,
                  &picture.rgba[size_t(y) * size_t(picture.width) * 4]);
    }
    VectorNode n;
    n.kind = VectorNode::Picture;
    n.picture = int(scene_.pictures.size());
    const float left = float(x0 + px0) / s, top = float(y0 + py0) / s;
    n.place = Transform2D{float(picture.width) / s, 0, 0, float(picture.height) / s, left, top};
    n.alpha = merged ? opacity : 1.0f;
    n.mask = line_mask;
    scene_.pictures.push_back(std::move(picture));
    to.push_back(std::move(n));
    scene_.brushed++;
  }

  /* A media layer's picture now, where it shows. */
  void picture(const Layer &layer, float opacity, int mask, std::vector<VectorNode> &to)
  {
    const MediaShown now = media_shown_at(doc_, layer, doc_.frame_current);
    if (!now.clip || opacity <= 0.0f) {
      return;
    }
    const MediaFramePtr frame = media_frame(media_clip_file(*now.clip), now.clip->sequence, now.seconds, true);
    if (!frame || frame->rgba.empty() || frame->width <= 0 || frame->height <= 0) {
      return;
    }
    const MediaInfo info = media_clip_info(*now.clip);
    float2 q[4];
    media_quad(doc_, layer, info.source_width > 0 ? info.source_width : frame->width,
               info.source_height > 0 ? info.source_height : frame->height, q);
    const Transform2D to_px = to_px_ * layer_world_transform(doc_, layer.id);
    for (float2 &p : q) {
      p = to_px.apply(p);
    }
    /* Straight colours, greyed and tinted as the renderer's media shader does. */
    VectorPicture picture;
    picture.width = frame->width;
    picture.height = frame->height;
    picture.rgba = frame->rgba;
    const float grey = layer.media.greyscale ? 1.0f : 0.0f;
    const Color &tint = layer.media.tint;
    const float tint_amount = layer.media.tint_amount;
    for (size_t i = 0; i + 3 < picture.rgba.size(); i += 4) {
      const float a = float(picture.rgba[i + 3]) / 255.0f;
      float c[3];
      for (int k = 0; k < 3; k++) {
        c[k] = a > 0.0f ? std::min(float(picture.rgba[i + size_t(k)]) / 255.0f / a, 1.0f) : 0.0f;
      }
      const float l = c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
      const float t[3] = {tint.r, tint.g, tint.b};
      for (int k = 0; k < 3; k++) {
        c[k] = c[k] + (l - c[k]) * grey;
        c[k] = c[k] + (t[k] + (1.0f - t[k]) * l - c[k]) * tint_amount;
        picture.rgba[i + size_t(k)] = uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
    /* The unit square onto the picture's corners (its top left at 0, 0; the rows run down from the top). */
    const float2 o = q[3], x = q[2] - q[3], y = q[0] - q[3];
    VectorNode n;
    n.kind = VectorNode::Picture;
    n.picture = int(scene_.pictures.size());
    n.place = Transform2D{x.x, x.y, y.x, y.y, o.x, o.y};
    n.alpha = opacity;
    n.mask = mask;
    scene_.pictures.push_back(std::move(picture));
    to.push_back(std::move(n));
  }

  /* --- Masks ------------------------------------------------------------------------------------ */

  /* A layer's cards on in renders, Clip to Below's among them, each with the layers its steps still name. */
  std::vector<Card> cards_of(const Layer &layer) const
  {
    std::vector<Card> cards;
    for (const LayerMask &m : layer_mask_cards(doc_, layer)) {
      if (!m.render) {
        continue;
      }
      Card card;
      card.start_shown = m.start_shown;
      card.cuts = m.clip ? MaskCuts::All : m.cuts;
      bool flat = false;
      card.brush_ends = mask_card_ends_lines(m, &flat) && !flat;
      const auto live = [&](MaskStep &s) {
        std::erase_if(s.layers, [&](uint64_t id) { return id == 0 || id == layer.id || !layer_find(doc_, id); });
      };
      for (MaskStep s : m.steps) {
        live(s);
        for (MaskStep &child : s.children) {
          live(child);
        }
        std::erase_if(s.children, mask_step_empty);
        if (!mask_step_empty(s)) {
          card.steps.push_back(std::move(s));
        }
      }
      if (!card.steps.empty()) {
        cards.push_back(std::move(card));
      }
    }
    return cards;
  }

  int add_mask(std::vector<VectorNode> content)
  {
    scene_.masks.push_back(VectorMask{std::move(content)});
    return int(scene_.masks.size()) - 1;
  }

  /* Well past the picture all round, white or black, shown where `mask` is. */
  VectorNode rect(bool white, int mask = -1) const
  {
    VectorNode n;
    n.kind = VectorNode::Path;
    n.path = rect_path(-float(scene_.width), -float(scene_.height), 2.0f * float(scene_.width),
                       2.0f * float(scene_.height));
    n.color = white ? WHITE : BLACK;
    n.mask = mask;
    return n;
  }

  /* Where all of `ids` show (one mask), or -1 for none. */
  int combined(const std::vector<int> &ids)
  {
    if (ids.empty()) {
      return -1;
    }
    if (ids.size() == 1) {
      return ids.front();
    }
    auto it = combined_ids_.find(ids);
    if (it != combined_ids_.end()) {
      return it->second;
    }
    const int id = add_mask({nested(ids)});
    combined_ids_.emplace(ids, id);
    return id;
  }

  /* White where every one of `ids` shows: a white rect in the first, in groups masked by the others. */
  VectorNode nested(const std::vector<int> &ids) const
  {
    VectorNode node = rect(true, ids.front());
    for (size_t i = 1; i < ids.size(); i++) {
      VectorNode g;
      g.mask = ids[i];
      g.children.push_back(std::move(node));
      node = std::move(g);
    }
    return node;
  }

  /* A card painted as its steps paint it: from its Start With, each step's shape (or, inverted, everything but
   * it) painted white (Add) or black (Subtract). */
  int card_mask(const Layer &owner, const Card &card, int index)
  {
    const auto key = std::make_pair(owner.id, index);
    auto it = card_ids_.find(key);
    if (it != card_ids_.end()) {
      return it->second;
    }
    const int id = steps_mask(card.steps, card.start_shown);
    card_ids_.emplace(key, id);
    return id;
  }

  /* Steps painted on white or black, as a mask. */
  int steps_mask(const std::vector<MaskStep> &steps, bool start)
  {
    std::vector<VectorNode> content;
    content.push_back(rect(start));
    for (const MaskStep &step : steps) {
      if (step.whole) {
        content.push_back(rect(step.mode == MaskMode::Add));
        continue;
      }
      int shape = step_shape(step);
      if (step.invert) {
        shape = add_mask({rect(true), rect(false, shape)});
      }
      content.push_back(rect(step.mode == MaskMode::Add, shape));
    }
    return add_mask(std::move(content));
  }

  /* A step's shape as a mask (white inside): its layers together, or the first less the others, or where they all
   * are; grown, shrunk or stretched as the step says. */
  int step_shape(const MaskStep &step)
  {
    if (step.group) {
      return steps_mask(step.children, false);
    }
    const bool inside_stroke = step.fills_only && step.inside_stroke;
    const bool shrink = step.grow < 0.0f || inside_stroke;
    Pass pass;
    pass.grow = step.grow;
    pass.exclude_strokes = step.fills_only;
    pass.exclude_fills = step.strokes_only;
    pass.filled = step.filled;
    pass.inside_stroke = inside_stroke;
    pass.extend = int(step.extend);
    /* Shrinking draws the areas, then clears a band around each area's edge, then draws the lines (thinner). */
    const std::vector<Pass::Stage> stages = shrink ? std::vector<Pass::Stage>{Pass::Areas, Pass::Bands, Pass::Lines}
                                                   : std::vector<Pass::Stage>{Pass::All};
    const auto layer_shape = [&](const Layer &source) {
      std::vector<VectorNode> shape;
      for (const Pass::Stage stage : stages) {
        Pass p = pass;
        p.stage = stage;
        coverage(source, p, shape);
      }
      return add_mask(std::move(shape));
    };
    std::vector<VectorNode> content;
    if (step.clip) {
      /* Clip to Below: what the base shows, not every shape in it. */
      for (const uint64_t id : step.layers) {
        if (const Layer *base = layer_find(doc_, id)) {
          std::vector<const Layer *> chain;
          clip_base(*base, step.fills_only, chain, content);
        }
      }
    }
    else if (step.combine != MaskCombine::Union && step.layers.size() > 1) {
      std::vector<int> operands;
      for (const uint64_t id : step.layers) {
        if (const Layer *source = layer_find(doc_, id)) {
          operands.push_back(layer_shape(*source));
        }
      }
      if (step.combine == MaskCombine::Difference) {
        content.push_back(rect(true, operands.front()));
        for (size_t i = 1; i < operands.size(); i++) {
          content.push_back(rect(false, operands[i]));
        }
      }
      else {
        content.push_back(nested(operands));
      }
    }
    else if (inside_stroke) {
      /* Each layer's lines trim only its own fills: a layer at a time, then together. */
      std::function<void(const Layer &)> each = [&](const Layer &l) {
        if (l.is_group()) {
          for (const Layer &child : l.children) {
            each(child);
          }
          return;
        }
        content.push_back(rect(true, layer_shape(l)));
      };
      for (const uint64_t id : step.layers) {
        if (const Layer *source = layer_find(doc_, id)) {
          each(*source);
        }
      }
    }
    else {
      for (const Pass::Stage stage : stages) {
        for (const uint64_t id : step.layers) {
          if (const Layer *source = layer_find(doc_, id)) {
            Pass p = pass;
            p.stage = stage;
            coverage(*source, p, content);
          }
        }
      }
    }
    return add_mask(std::move(content));
  }

  /* Clip to Below's base: where it shows, as its fills do (its cards and its groups' that mask only lines left
   * out). Hidden layers don't show, so they don't clip. */
  void clip_base(const Layer &layer, bool fills_only, std::vector<const Layer *> &chain, std::vector<VectorNode> &to)
  {
    if (!shown(layer) || layer.opacity <= 0.0f) {
      return;
    }
    chain.push_back(&layer);
    if (layer.is_group()) {
      for (const Layer &child : layer.children) {
        clip_base(child, fills_only, chain, to);
      }
    }
    else {
      std::vector<int> ids;
      for (const Layer *l : chain) {
        int index = -1;
        for (const Card &card : cards_of(*l)) {
          index++;
          if (card.cuts != MaskCuts::Lines) {
            ids.push_back(card_mask(*l, card, index));
          }
        }
      }
      VectorNode group;
      group.mask = combined(ids);
      Pass pass;
      pass.exclude_strokes = fills_only;
      coverage(layer, pass, group.children);
      to.push_back(std::move(group));
    }
    chain.pop_back();
  }

  /* A mask layer's shapes as drawn (before any masks of its own), white; a group's layers, hidden ones too. The
   * renderer's mask pass, shape by shape. */
  void coverage(const Layer &layer, const Pass &pass, std::vector<VectorNode> &to)
  {
    if (layer.is_group()) {
      for (const Layer &child : layer.children) {
        coverage(child, pass, to);
      }
      return;
    }
    if (layer.is_media()) {
      return;
    }
    VectorDrawing evaluated;
    Transform2D transform;
    const VectorDrawing &drawing = drawing_of(layer, evaluated, &transform);
    const Transform2D to_px = to_px_ * transform;
    const float pixel = 1.0f / std::max(to_px.scale(), 1e-6f);
    /* The bands that shrinking clears are painted black. */
    const Color paint = pass.stage == Pass::Bands ? BLACK : WHITE;
    const float grow = pass.grow * pixel;
    std::vector<float2> outline;
    std::vector<float> radii;
    std::vector<bool> visible;
    std::vector<StrokeJoin> joins;
    for (const Shape &whole : drawing.shapes) {
      const std::vector<Shape> runs = shape_runs(drawing, whole);
      const bool fills_together = whole.fill_rule != FillRule::Loops && runs.size() > 1;
      bool filled_together = false;
      for (const Shape &run : runs) {
        if (run.hidden || (run.merged_fill && run.merged_line)) {
          continue;
        }
        const bool has_fill = run.has_fill && !run.merged_fill;
        const bool has_outline = run.has_outline && !run.merged_line;
        bool closed = false;
        shape_outline(drawing, run, quality_, outline, &radii, &closed, &visible, &joins,
                      pixel * 3.0f / float(quality_));
        if (outline.size() < 2) {
          continue;
        }
        const bool is_area = !pass.exclude_fills && ((has_fill && closed) || pass.filled);
        /* Inside the Stroke: the area ends at its line's inner edge. */
        const bool inside_stroke = pass.inside_stroke && has_outline && run.style.line_width > 0.0f;
        bool line_as_drawn = false;
        if (pass.stage == Pass::Bands) {
          if (is_area) {
            std::vector<float> band(outline.size(), std::max(-grow, 0.0f));
            if (inside_stroke) {
              for (size_t k = 0; k < band.size(); k++) {
                const float r = (k < radii.size() ? radii[k] : 1.0f) * run.style.line_width * 0.5f;
                band[k] = std::max(r - grow, 0.0f);
              }
            }
            element(to, stroke_path(outline, band, true, std::vector<StrokeJoin>(outline.size()), to_px), paint, 1.0f);
          }
          if (!(is_area && inside_stroke)) {
            continue;
          }
          line_as_drawn = true;
        }
        if (pass.extend != 0 && pass.stage != Pass::Lines && !line_as_drawn) {
          /* Extend: the edges swept to far off in the layer's own direction. */
          static const float2 DIRS[] = {{0, 0}, {0, 1}, {0, -1}, {-1, 0}, {1, 0}};
          const float2 reach = DIRS[std::clamp(pass.extend, 0, 4)] * (pixel * 100000.0f);
          const bool area = !pass.exclude_fills && ((has_fill && closed) || pass.filled);
          const bool strokes = has_outline && !pass.exclude_strokes;
          const size_t n = outline.size();
          VectorPath sweep;
          for (size_t i = 0; i + 1 < n + (closed ? 1 : 0); i++) {
            if (!area && !(strokes && i < visible.size() && visible[i])) {
              continue;
            }
            const float2 a = outline[i], b = outline[(i + 1) % n];
            /* Each quad wound the same way, so together they fill as one. */
            std::vector<float2> quad = {a, b, b + reach, a + reach};
            const float2 e = b - a;
            if (e.x * reach.y - e.y * reach.x < 0.0f) {
              std::reverse(quad.begin(), quad.end());
            }
            polygon_path(quad, to_px, sweep);
          }
          element(to, std::move(sweep), paint, 1.0f);
        }
        if (is_area && pass.stage != Pass::Lines && !line_as_drawn) {
          if (!fills_together || !filled_together) {
            filled_together = fills_together;
            element(to, fill_path(drawing, whole, run, runs, fills_together, to_px), paint, 1.0f,
                    fills_together && whole.fill_rule == FillRule::EvenOdd);
          }
          if (grow > 0.0f && !inside_stroke) {
            /* Grow: a rim that wide around the area. */
            element(to,
                    stroke_path(outline, std::vector<float>(outline.size(), grow), true,
                                std::vector<StrokeJoin>(outline.size()), to_px),
                    paint, 1.0f);
          }
        }
        if (has_outline && run.style.line_width > 0.0f && (line_as_drawn || !pass.exclude_strokes) &&
            pass.stage != Pass::Areas)
        {
          const float line_grow = line_as_drawn ? 0.0f : grow;
          /* A mask keeps hairlines a pixel wide; a line shrunk away is gone. */
          bool any_width = false;
          for (float &r : radii) {
            r = r * run.style.line_width * 0.5f + line_grow;
            r = line_grow < 0.0f ? std::max(r, 0.0f) : std::max(r, pixel * 0.5f);
            any_width |= r > 0.0f;
          }
          if (!any_width) {
            continue;
          }
          line_runs(outline, radii, visible, joins, closed, nullptr, transform,
                    [&](const std::vector<float2> &pts, const std::vector<float> &half, bool is_closed,
                        const std::vector<StrokeJoin> &stroke_joins) {
                      element(to, stroke_path(pts, half, is_closed, stroke_joins, to_px), paint, 1.0f);
                    });
        }
      }
    }
  }
};

}  // namespace

std::string vector_number(float v)
{
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f", double(v));
  std::string s = text;
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') {
      s.pop_back();
    }
    if (s.back() == '.') {
      s.pop_back();
    }
  }
  return s == "-0" ? "0" : s;
}

VectorScene vector_scene(const Document &doc, const VectorSceneOptions &options)
{
  /* Every layer a rig moves asks for its pose, and every layer its parents and masks: worked out once. */
  const ArmaturePoseCacheScope pose_cache;
  const LayerIndexScope layer_index(doc);
  VectorScene scene;
  scene.width = options.width > 0 ? options.width : std::max(doc.width, 1);
  scene.height = options.height > 0 ? options.height : std::max(doc.height, 1);
  SceneWalker(doc, options, scene).walk();
  return scene;
}

}  // namespace oanim
