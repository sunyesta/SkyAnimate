#include "io/svg_export.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <vector>

#include <SDL3/SDL.h>
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
  const LayerMask *mask = nullptr;
  bool start_shown = true;
  MaskCuts cuts = MaskCuts::All;
  /* End Lines at Edge with Ends: Brush: the lines are ended at its edge, not cut by it. */
  bool brush_ends = false;
  std::vector<MaskStep> steps;
};

/* The masks applying to what is drawn, by what they cut (each a mask id; all must show). */
struct Masks {
  std::vector<std::string> both, fills, lines;
};

std::string num(float v)
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

std::string hex(const Color &c)
{
  const auto byte = [](float v) { return int(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  char text[8];
  std::snprintf(text, sizeof(text), "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
  return text;
}

std::string base64(const uint8_t *data, size_t size)
{
  static const char *ABC = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t n = uint32_t(data[i]) << 16 | (i + 1 < size ? uint32_t(data[i + 1]) << 8 : 0u) |
                       (i + 2 < size ? uint32_t(data[i + 2]) : 0u);
    out += ABC[n >> 18 & 63];
    out += ABC[n >> 12 & 63];
    out += i + 1 < size ? ABC[n >> 6 & 63] : '=';
    out += i + 2 < size ? ABC[n & 63] : '=';
  }
  return out;
}

class SvgWriter {
 public:
  SvgWriter(const Document &doc, int width, int height) : doc_(doc), width_(width), height_(height)
  {
    /* As Render Frame: the camera's frame, 2 units tall, fills the picture; SVG's y runs down. */
    const float scale = float(height) * 0.5f;
    to_px_ = Transform2D{1, 0, 0, -1, 0, float(height)} *
             Transform2D{scale, 0, 0, scale, float(width) * 0.5f, float(height) * 0.5f} * camera_to_frame(doc);
    quality_ = std::clamp(doc.render_quality, 1, 64);
  }

  std::string write(bool transparent)
  {
    std::string body;
    if (!transparent) {
      body += "<rect width=\"" + std::to_string(width_) + "\" height=\"" + std::to_string(height_) + "\" fill=\"" +
              hex(doc_.background) + "\"/>\n";
    }
    draw_layers(doc_.layers, 1.0f, Masks(), body);
    /* Media layers In Front: over everything, unmasked. */
    for (const Layer *l : document_layers_flat(doc_)) {
      if (l->is_media() && l->media.in_front && shown_through_parents(*l)) {
        float opacity = 1.0f;
        for (const Layer *p = l; p; p = layer_parent(const_cast<Document &>(doc_), p->id)) {
          opacity *= p->opacity;
        }
        picture(*l, opacity, "", body);
      }
    }
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\"" +
           std::to_string(width_) + "\" height=\"" + std::to_string(height_) + "\" viewBox=\"0 0 " +
           std::to_string(width_) + " " + std::to_string(height_) + "\">\n";
    if (!defs_.empty()) {
      out += "<defs>\n" + defs_ + "</defs>\n";
    }
    out += body;
    out += "</svg>\n";
    return out;
  }

  SvgExportReport report;

 private:
  const Document &doc_;
  int width_, height_;
  Transform2D to_px_;
  int quality_ = 32;
  std::string defs_;
  int next_id_ = 1;
  std::map<std::pair<uint64_t, int>, std::string> card_ids_;
  std::map<std::vector<std::string>, std::string> combined_ids_;

  /* --- What shows --------------------------------------------------------------------------- */

  static bool shown(const Layer &layer) { return layer.visible && layer.render_visible; }

  bool shown_through_parents(const Layer &layer) const
  {
    for (const Layer *l = &layer; l; l = layer_parent(const_cast<Document &>(doc_), l->id)) {
      if (!shown(*l) || l->opacity <= 0.0f) {
        return false;
      }
    }
    return true;
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

  std::string new_id(const char *prefix) { return prefix + std::to_string(next_id_++); }

  /* --- Geometry ------------------------------------------------------------------------------ */

  /* A connected run of a shape's edges as Bezier path data (pixels), closed when it goes round. */
  static void bezier_path(const VectorDrawing &drawing, const Shape &run, const Transform2D &to_px, bool close,
                          std::string &d)
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
      for (float2 &p : ctrl) {
        p = to_px.apply(p);
      }
      if (first) {
        d += "M" + num(ctrl[0].x) + " " + num(ctrl[0].y);
        first = false;
      }
      d += "C" + num(ctrl[1].x) + " " + num(ctrl[1].y) + " " + num(ctrl[2].x) + " " + num(ctrl[2].y) + " " +
           num(ctrl[3].x) + " " + num(ctrl[3].y);
    }
    if (!first && (closed || close)) {
      d += "Z";
    }
  }

  /* A line as the renderer strokes it (the triangles it draws, without soft edges), made one outline. */
  static std::string stroke_path(const std::vector<float2> &points, const std::vector<float> &half_widths,
                                 bool closed, const std::vector<StrokeJoin> &joins, const Transform2D &to_px)
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
    const c2::Paths64 outline = c2::Union(triangles, c2::FillRule::NonZero);
    std::string d;
    for (const c2::Path64 &ring : outline) {
      if (ring.size() < 3) {
        continue;
      }
      for (size_t i = 0; i < ring.size(); i++) {
        d += (i == 0 ? "M" : i == 1 ? "L" : " ") + num(float(double(ring[i].x) / UNITS)) + " " +
             num(float(double(ring[i].y) / UNITS));
      }
      d += "Z";
    }
    return d;
  }

  /* A polygon (layer space) as path data. */
  static std::string polygon_path(const std::vector<float2> &polygon, const Transform2D &to_px)
  {
    std::string d;
    for (size_t i = 0; i < polygon.size(); i++) {
      const float2 p = to_px.apply(polygon[i]);
      d += (i == 0 ? "M" : i == 1 ? "L" : " ") + num(p.x) + " " + num(p.y);
    }
    return d.empty() ? d : d + "Z";
  }

  static void element(std::string &to, const std::string &d, const std::string &fill, float alpha,
                      const char *rule = nullptr, const std::string &mask = {})
  {
    if (d.empty()) {
      return;
    }
    to += "<path d=\"" + d + "\" fill=\"" + fill + "\"";
    if (alpha < 0.9995f) {
      to += " fill-opacity=\"" + num(std::max(alpha, 0.0f) + 0.0f) + "\"";
    }
    if (rule) {
      to += std::string(" fill-rule=\"") + rule + "\"";
    }
    if (!mask.empty()) {
      to += " mask=\"url(#" + mask + ")\"";
    }
    to += "/>\n";
  }

  /* --- Drawing layers in colour ----------------------------------------------------------------- */

  void draw_layers(const std::vector<Layer> &scope, float opacity, const Masks &outer, std::string &to)
  {
    /* Bottom first, in Draw Order, each with the masks of it and the groups holding it. */
    for (const DrawStep &step : draw_order_steps(doc_, scope, [](const Layer &l) { return shown(l); })) {
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
          const std::string id = card_mask(*l, card, index);
          (fills && lines ? masks.both : fills ? masks.fills : masks.lines).push_back(id);
        }
      }
      draw_layer(*step.layer, opacity, masks, to);
    }
  }

  void draw_layer(const Layer &layer, float opacity, const Masks &masks, std::string &to)
  {
    if (!shown(layer) || layer.opacity <= 0.0f) {
      return;
    }
    const float layer_opacity = opacity * layer.opacity;
    const std::string both = combined(masks.both);
    if (!both.empty()) {
      to += "<g mask=\"url(#" + both + ")\">\n";
    }
    if (layer.is_media()) {
      if (!layer.media.in_front) {
        picture(layer, layer_opacity, combined(masks.fills), to);
      }
    }
    else if (!layer.is_group()) {
      /* A see-through layer fades as a whole (GPU Geometry), so its lines don't show its fills through them,
       * unless its lines and fills are masked apart; Classic fades each shape. */
      const bool whole = doc_.shading.gpu_geometry && layer_opacity < 1.0f && masks.fills.empty() &&
                         masks.lines.empty();
      if (whole) {
        to += "<g opacity=\"" + num(layer_opacity) + "\">\n";
      }
      drawing_colour(layer, whole ? 1.0f : layer_opacity, combined(masks.fills), combined(masks.lines), to);
      if (whole) {
        to += "</g>\n";
      }
    }
    else if (layer.opacity < 1.0f) {
      /* A faded group as a whole, so its overlapping layers don't show through each other. */
      to += "<g opacity=\"" + num(layer_opacity) + "\">\n";
      Masks inner;
      inner.fills = masks.fills;
      inner.lines = masks.lines;
      draw_layers(layer.children, 1.0f, inner, to);
      to += "</g>\n";
    }
    else {
      Masks inner;
      inner.fills = masks.fills;
      inner.lines = masks.lines;
      draw_layers(layer.children, layer_opacity, inner, to);
    }
    if (!both.empty()) {
      to += "</g>\n";
    }
  }

  /* A drawing layer's shapes in their colours, each shape's fill then its line, as the renderer draws them. */
  void drawing_colour(const Layer &layer, float opacity, const std::string &fill_mask, const std::string &line_mask,
                      std::string &to)
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
        if (has_fill && closed) {
          std::string d;
          const char *rule = nullptr;
          if (!fills_together) {
            bezier_path(drawing, run, to_px, true, d);
          }
          else if (!filled_together) {
            filled_together = true;
            for (const Shape &r : runs) {
              std::vector<OrientedEdge> chain;
              bool loop = false;
              shape_chain(drawing, r, chain, &loop);
              if (loop) {
                bezier_path(drawing, r, to_px, true, d);
              }
            }
            rule = whole.fill_rule == FillRule::EvenOdd ? "evenodd" : "nonzero";
          }
          if (!d.empty()) {
            element(to, d, hex(run.style.fill), run.style.fill.a * opacity, rule, fill_mask);
            report.fills++;
          }
        }
        if (has_outline && run.style.line_width > 0.0f) {
          if (!run.style.brush.name.empty() && brush_find(run.style.brush.name)) {
            report.brushed++;
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
                      element(to, stroke_path(pts, half, is_closed, stroke_joins, to_px), hex(run.style.line),
                              run.style.line.a * opacity, nullptr, line_mask);
                      report.lines++;
                    });
        }
      }
    }
  }

  /* A media layer's picture now, as a PNG, where it shows. */
  void picture(const Layer &layer, float opacity, const std::string &mask, std::string &to)
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
    std::vector<uint8_t> rgba = frame->rgba;
    const float grey = layer.media.greyscale ? 1.0f : 0.0f;
    const Color &tint = layer.media.tint;
    const float tint_amount = layer.media.tint_amount;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      const float a = float(rgba[i + 3]) / 255.0f;
      float c[3];
      for (int k = 0; k < 3; k++) {
        c[k] = a > 0.0f ? std::min(float(rgba[i + size_t(k)]) / 255.0f / a, 1.0f) : 0.0f;
      }
      const float l = c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f;
      const float t[3] = {tint.r, tint.g, tint.b};
      for (int k = 0; k < 3; k++) {
        c[k] = c[k] + (l - c[k]) * grey;
        c[k] = c[k] + (t[k] + (1.0f - t[k]) * l - c[k]) * tint_amount;
        rgba[i + size_t(k)] = uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
      }
    }
    SDL_Surface *surface =
        SDL_CreateSurfaceFrom(frame->width, frame->height, SDL_PIXELFORMAT_ABGR8888, rgba.data(), frame->width * 4);
    SDL_IOStream *io = SDL_IOFromDynamicMem();
    std::string png;
    if (surface && io && SDL_SavePNG_IO(surface, io, false)) {
      const auto *data = static_cast<const uint8_t *>(
          SDL_GetPointerProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
      const Sint64 size = SDL_GetIOSize(io);
      if (data && size > 0) {
        png = base64(data, size_t(size));
      }
    }
    if (io) {
      SDL_CloseIO(io);
    }
    if (surface) {
      SDL_DestroySurface(surface);
    }
    if (png.empty()) {
      return;
    }
    /* The unit square onto the picture's corners (top left at 0, 0; the rows run down from the top). */
    const float2 o = q[3], x = q[2] - q[3], y = q[0] - q[3];
    to += "<g";
    if (!mask.empty()) {
      to += " mask=\"url(#" + mask + ")\"";
    }
    if (opacity < 0.9995f) {
      to += " opacity=\"" + num(opacity) + "\"";
    }
    to += "><image x=\"0\" y=\"0\" width=\"1\" height=\"1\" preserveAspectRatio=\"none\" transform=\"matrix(" +
          num(x.x) + " " + num(x.y) + " " + num(y.x) + " " + num(y.y) + " " + num(o.x) + " " + num(o.y) +
          ")\" xlink:href=\"data:image/png;base64," + png + "\"/></g>\n";
    report.pictures++;
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
      for (MaskStep s : m.steps) {
        std::erase_if(s.layers, [&](uint64_t id) { return id == 0 || id == layer.id || !layer_find(doc_, id); });
        if (!s.layers.empty()) {
          card.steps.push_back(std::move(s));
        }
      }
      if (!card.steps.empty()) {
        cards.push_back(std::move(card));
      }
    }
    return cards;
  }

  /* A mask element holding `content` (pixels, white showing), over well past the picture. */
  std::string add_mask(const std::string &content)
  {
    const std::string id = new_id("m");
    defs_ += "<mask id=\"" + id + "\" maskUnits=\"userSpaceOnUse\" x=\"" + std::to_string(-width_) + "\" y=\"" +
             std::to_string(-height_) + "\" width=\"" + std::to_string(width_ * 3) + "\" height=\"" +
             std::to_string(height_ * 3) + "\">\n" + content + "</mask>\n";
    report.masks++;
    return id;
  }

  std::string rect(bool white, const std::string &mask = {}) const
  {
    std::string r = "<rect x=\"" + std::to_string(-width_) + "\" y=\"" + std::to_string(-height_) + "\" width=\"" +
                    std::to_string(width_ * 3) + "\" height=\"" + std::to_string(height_ * 3) + "\" fill=\"" +
                    (white ? "#fff" : "#000") + "\"";
    if (!mask.empty()) {
      r += " mask=\"url(#" + mask + ")\"";
    }
    return r + "/>\n";
  }

  /* Where all of `ids` show (one mask), or "" for none. */
  std::string combined(const std::vector<std::string> &ids)
  {
    if (ids.empty()) {
      return {};
    }
    if (ids.size() == 1) {
      return ids.front();
    }
    auto it = combined_ids_.find(ids);
    if (it != combined_ids_.end()) {
      return it->second;
    }
    std::string content;
    for (size_t i = 1; i < ids.size(); i++) {
      content += "<g mask=\"url(#" + ids[i] + ")\">\n";
    }
    content += rect(true, ids.front());
    for (size_t i = 1; i < ids.size(); i++) {
      content += "</g>\n";
    }
    const std::string id = add_mask(content);
    combined_ids_.emplace(ids, id);
    return id;
  }

  /* A card painted as its steps paint it: from its Start With, each step's shape (or, inverted, everything but
   * it) painted white (Add) or black (Subtract). */
  std::string card_mask(const Layer &owner, const Card &card, int index)
  {
    const auto key = std::make_pair(owner.id, index);
    auto it = card_ids_.find(key);
    if (it != card_ids_.end()) {
      return it->second;
    }
    std::string content = rect(card.start_shown);
    for (const MaskStep &step : card.steps) {
      std::string shape = step_shape(step);
      if (step.invert) {
        shape = add_mask(rect(true) + rect(false, shape));
      }
      content += rect(step.mode == MaskMode::Add, shape);
    }
    const std::string id = add_mask(content);
    card_ids_.emplace(key, id);
    return id;
  }

  /* A step's shape as a mask (white inside): its layers together, or the first less the others, or where they all
   * are; grown, shrunk or stretched as the step says. */
  std::string step_shape(const MaskStep &step)
  {
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
    const auto layer_shape = [&](const Layer &source, std::string &to) {
      for (const Pass::Stage stage : stages) {
        Pass p = pass;
        p.stage = stage;
        coverage(source, p, to);
      }
    };
    std::string content;
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
      std::vector<std::string> operands;
      for (const uint64_t id : step.layers) {
        if (const Layer *source = layer_find(doc_, id)) {
          std::string shape;
          layer_shape(*source, shape);
          operands.push_back(add_mask(shape));
        }
      }
      if (step.combine == MaskCombine::Difference) {
        content += rect(true, operands.front());
        for (size_t i = 1; i < operands.size(); i++) {
          content += rect(false, operands[i]);
        }
      }
      else {
        for (size_t i = 1; i < operands.size(); i++) {
          content += "<g mask=\"url(#" + operands[i] + ")\">\n";
        }
        content += rect(true, operands.front());
        for (size_t i = 1; i < operands.size(); i++) {
          content += "</g>\n";
        }
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
        std::string shape;
        layer_shape(l, shape);
        content += rect(true, add_mask(shape));
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
    return add_mask(content);
  }

  /* Clip to Below's base: where it shows, as its fills do (its cards and its groups' that mask only lines left
   * out). Hidden layers don't show, so they don't clip. */
  void clip_base(const Layer &layer, bool fills_only, std::vector<const Layer *> &chain, std::string &to)
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
      std::vector<std::string> ids;
      for (const Layer *l : chain) {
        int index = -1;
        for (const Card &card : cards_of(*l)) {
          index++;
          if (card.cuts != MaskCuts::Lines) {
            ids.push_back(card_mask(*l, card, index));
          }
        }
      }
      const std::string mask = combined(ids);
      if (!mask.empty()) {
        to += "<g mask=\"url(#" + mask + ")\">\n";
      }
      Pass pass;
      pass.exclude_strokes = fills_only;
      coverage(layer, pass, to);
      if (!mask.empty()) {
        to += "</g>\n";
      }
    }
    chain.pop_back();
  }

  /* A mask layer's shapes as drawn (before any masks of its own), white; a group's layers, hidden ones too. The
   * renderer's mask pass, shape by shape. */
  void coverage(const Layer &layer, const Pass &pass, std::string &to)
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
    const std::string paint = pass.stage == Pass::Bands ? "#000" : "#fff";
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
          std::string d;
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
            d += polygon_path(quad, to_px);
          }
          element(to, d, paint, 1.0f);
        }
        if (is_area && pass.stage != Pass::Lines && !line_as_drawn) {
          std::string d;
          const char *rule = nullptr;
          if (!fills_together) {
            bezier_path(drawing, run, to_px, true, d);
          }
          else if (!filled_together) {
            filled_together = true;
            for (const Shape &r : runs) {
              std::vector<OrientedEdge> chain;
              bool loop = false;
              shape_chain(drawing, r, chain, &loop);
              if (loop) {
                bezier_path(drawing, r, to_px, true, d);
              }
            }
            rule = whole.fill_rule == FillRule::EvenOdd ? "evenodd" : "nonzero";
          }
          element(to, d, paint, 1.0f, rule);
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

std::string svg_frame(const Document &doc, const SvgExport &options, SvgExportReport *r_report)
{
  /* Every layer a rig moves asks for its pose, and every layer its parents and masks: worked out once. */
  const ArmaturePoseCacheScope pose_cache;
  const LayerIndexScope layer_index(doc);
  const int width = options.width > 0 ? options.width : std::max(doc.width, 1);
  const int height = options.height > 0 ? options.height : std::max(doc.height, 1);
  SvgWriter writer(doc, width, height);
  std::string text = writer.write(options.transparent);
  if (r_report) {
    *r_report = writer.report;
  }
  return text;
}

bool svg_export_frame(const Document &doc, const std::string &path, const SvgExport &options, std::string *r_error,
                      SvgExportReport *r_report)
{
  const std::string text = svg_frame(doc, options, r_report);
  std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())),
                    std::ios::binary | std::ios::trunc);
  out.write(text.data(), std::streamsize(text.size()));
  out.close();
  if (!out) {
    if (r_error) {
      *r_error = "Could not write " + path;
    }
    return false;
  }
  return true;
}

}  // namespace oanim
