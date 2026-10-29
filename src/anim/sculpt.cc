#include "anim/sculpt.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

#include "anim/deform.hh"
#include "data/detail.hh"
#include "data/document.hh"
#include "data/layer_mirror.hh"
#include "data/vector.hh"

namespace oanim {

static constexpr const char *BRUSH_IDS[SCULPT_BRUSH_COUNT] = {"GRAB", "PUSH", "SMOOTH", "THICKNESS", "PINCH", "TWIST",
                                                                   "SURFACE_SMOOTH"};
static constexpr const char *BRUSH_NAMES[SCULPT_BRUSH_COUNT] = {"Grab", "Push", "Smooth", "Thickness", "Pinch", "Twist",
                                                                     "Surface Smooth"};
static constexpr const char *FALLOFF_IDS[3] = {"SMOOTH", "SPHERE", "CONSTANT"};

const char *sculpt_brush_id(SculptBrush brush)
{
  return BRUSH_IDS[std::clamp(int(brush), 0, SCULPT_BRUSH_COUNT - 1)];
}

const char *sculpt_brush_name(SculptBrush brush)
{
  return BRUSH_NAMES[std::clamp(int(brush), 0, SCULPT_BRUSH_COUNT - 1)];
}

bool sculpt_brush_from_id(const std::string &id, SculptBrush *r_brush)
{
  for (int i = 0; i < SCULPT_BRUSH_COUNT; i++) {
    if (id == BRUSH_IDS[i]) {
      *r_brush = SculptBrush(i);
      return true;
    }
  }
  return false;
}

const char *sculpt_falloff_id(SculptFalloff falloff)
{
  return FALLOFF_IDS[std::clamp(int(falloff), 0, 2)];
}

bool sculpt_falloff_from_id(const std::string &id, SculptFalloff *r_falloff)
{
  for (int i = 0; i < 3; i++) {
    if (id == FALLOFF_IDS[i]) {
      *r_falloff = SculptFalloff(i);
      return true;
    }
  }
  return false;
}

float sculpt_falloff(SculptFalloff falloff, float t)
{
  if (t >= 1.0f) {
    return 0.0f;
  }
  t = std::max(t, 0.0f);
  switch (falloff) {
    case SculptFalloff::Smooth:
      /* 1 - smoothstep. */
      return 1.0f - t * t * (3.0f - 2.0f * t);
    case SculptFalloff::Sphere:
      return std::sqrt(1.0f - t * t);
    case SculptFalloff::Constant:
      return 1.0f;
  }
  return 0.0f;
}

namespace {

bool sculptable(const Layer &layer)
{
  return layer.has_drawing() && (!layer.is_reference() || layer.reference.edit_points);
}

float weight_at(const SculptDab &dab, float2 p)
{
  return sculpt_falloff(dab.falloff, distance(p, dab.center) / std::max(dab.radius, 1e-9f));
}

/* The curve's segments: segment s runs from point s to point s + 1 (the last of a closed curve back to
 * the first). */
int segment_count(const SculptStroke::Curve &c)
{
  const int n = int(c.points.size());
  return c.closed ? n : n - 1;
}

/* The samples of segment s, from its first point's sample to its last's (both in). */
std::vector<int> segment_samples(const SculptStroke::Curve &c, int s)
{
  const int n = int(c.points.size());
  const int first = c.anchor[size_t(s)];
  const int last = c.anchor[size_t((s + 1) % n)];
  const int total = int(c.pos.size());
  std::vector<int> out;
  for (int k = first;; k = (k + 1) % total) {
    out.push_back(k);
    if (k == last && out.size() > 1) {
      break;
    }
  }
  return out;
}

float2 normalized(float2 v)
{
  const float len = length(v);
  return len > 1e-12f ? v / len : float2();
}

}  // namespace

/* --- Surface Smooth's target: the line resampled evenly and noise-reduced ------------------------------------- */

/* The Whittaker-Eilers smoother of the third order (Whittaker 1923; Eilers, "A perfect smoother", 2003): the values z
 * nearest `y` (by the weights `w`) whose third differences are smallest, minimizing
 * sum w (z - y)^2 + lambda sum (third difference of z)^2, by solving (W + lambda D'D) z = W y, D the third-difference
 * matrix; a banded system (three either side of the diagonal), so a Cholesky factor in linear time. The third order
 * resists a change of bend, not the bend: an arc or a parabola goes through unchanged, wiggles shorter than its
 * cutoff are taken out. */
static void whittaker3(std::vector<double> &y, const std::vector<double> &w, double lambda)
{
  const int n = int(y.size());
  if (n < 5) {
    return;
  }
  constexpr int band = 3;
  /* u[i][k]: the upper factor's entry (i, i + k); first the matrix itself, its upper band. */
  std::vector<std::array<double, band + 1>> u(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    u[size_t(i)][0] = w[size_t(i)];
  }
  const double d3[4] = {-1.0, 3.0, -3.0, 1.0};
  for (int i = 0; i + 3 < n; i++) {
    for (int r = 0; r < 4; r++) {
      for (int c = r; c < 4; c++) {
        u[size_t(i + r)][size_t(c - r)] += lambda * d3[r] * d3[c];
      }
    }
  }
  const auto at = [&](int row, int col) -> double & { return u[size_t(row)][size_t(col - row)]; };
  for (int j = 0; j < n; j++) {
    double diag = at(j, j);
    for (int k = std::max(0, j - band); k < j; k++) {
      diag -= at(k, j) * at(k, j);
    }
    at(j, j) = std::sqrt(std::max(diag, 1e-300));
    for (int i = j + 1; i <= std::min(n - 1, j + band); i++) {
      double v = at(j, i);
      for (int k = std::max(0, i - band); k < j; k++) {
        v -= at(k, j) * at(k, i);
      }
      at(j, i) = v / at(j, j);
    }
  }
  /* U'U z = W y: forward through U', back through U. */
  std::vector<double> x(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    double v = w[size_t(i)] * y[size_t(i)];
    for (int k = std::max(0, i - band); k < i; k++) {
      v -= at(k, i) * x[size_t(k)];
    }
    x[size_t(i)] = v / at(i, i);
  }
  for (int i = n - 1; i >= 0; i--) {
    double v = x[size_t(i)];
    for (int k = i + 1; k <= std::min(n - 1, i + band); k++) {
      v -= at(i, k) * y[size_t(k)];
    }
    y[size_t(i)] = v / at(i, i);
  }
}

/* Smooths one span of a curve, its anchors `first`..`last` (indices into its points; `last` is `first + n` round a
 * loop), and writes the targets: the span resampled at even steps along its length, those samples smoothed (wiggles
 * shorter than about `length` along the line go), then each anchor put where its own place along the line went and
 * each segment's handles fitted to the smoothed line between its two anchors (their directions the smoothed line's
 * there, so the points stay smooth). An open span's ends stay (a curve's end, or a corner); a loop is smoothed all
 * round. */
static void smooth_span(const SculptStroke::Curve &c, int first, int last, bool loop, float lump,
                        std::vector<float2> &r_pos, std::vector<float2> &r_in, std::vector<float2> &r_out)
{
  const int n = int(c.points.size());
  const auto at = [&](int i) { return ((i % n) + n) % n; };
  const auto anchor = [&](int i) { return c.pos[size_t(c.anchor[size_t(at(i))])]; };
  /* The span as it is, finely: its segments' curves, and how far along it each anchor is. */
  std::vector<float2> fine;
  std::vector<double> fine_at;
  std::vector<double> anchor_at;
  const int per = 24;
  double along = 0.0;
  for (int i = first; i < last; i++) {
    const float2 seg[4] = {anchor(i), anchor(i) + c.handle_out[size_t(at(i))],
                           anchor(i + 1) + c.handle_in[size_t(at(i + 1))], anchor(i + 1)};
    anchor_at.push_back(along);
    for (int k = 0; k < per; k++) {
      const float2 q = bezier_eval(seg, float(k) / float(per));
      if (!fine.empty()) {
        along += double(distance(q, fine.back()));
      }
      fine.push_back(q);
      fine_at.push_back(along);
    }
  }
  along += double(distance(anchor(last), fine.back()));
  fine.push_back(anchor(last));
  fine_at.push_back(along);
  anchor_at.push_back(along);
  const double total = along;
  if (total < 1e-9) {
    return;
  }
  /* Even steps: a dozen to the shortest wiggle that goes (finer where the points are close, within reason). */
  const double spacing = total / double(last - first);
  double step = std::min(double(lump) / 12.0, spacing / 6.0);
  step = std::max({step, double(lump) / 64.0, total / 4000.0});
  const int steps = std::max(8, int(std::ceil(total / step)));
  step = total / double(steps);
  /* The place on the fine line `d` along it. */
  size_t seek = 0;
  const auto fine_point = [&](double d) {
    while (seek + 1 < fine_at.size() && fine_at[seek + 1] < d) {
      seek++;
    }
    while (seek > 0 && fine_at[seek] > d) {
      seek--;
    }
    const size_t b = std::min(seek + 1, fine.size() - 1);
    const double gap = fine_at[b] - fine_at[seek];
    const float u = gap > 1e-12 ? float((d - fine_at[seek]) / gap) : 0.0f;
    return fine[seek] + (fine[b] - fine[seek]) * u;
  };
  /* The samples: for a loop one round (the last would be the first again), else both ends. */
  const int count = loop ? steps : steps + 1;
  std::vector<double> xs, ys;
  for (int k = 0; k < count; k++) {
    const float2 q = fine_point(double(k) * step);
    xs.push_back(q.x);
    ys.push_back(q.y);
  }
  /* The cutoff: the smoother halves a wiggle whose wavelength is 2 pi lambda^(1/6) samples. */
  const double lambda = std::pow(double(lump) / (2.0 * double(PI) * step), 6.0);
  if (loop) {
    /* Round a loop: smoothed with copies of itself either side (far enough that its open ends don't reach), then
     * the middle round taken. */
    const int pad = std::min(count * 3, int(std::ceil(3.0 * double(lump) / step)) + 8);
    std::vector<double> px, py, pw(size_t(count + 2 * pad), 1.0);
    for (int k = -pad; k < count + pad; k++) {
      const int m = ((k % count) + count) % count;
      px.push_back(xs[size_t(m)]);
      py.push_back(ys[size_t(m)]);
    }
    whittaker3(px, pw, lambda);
    whittaker3(py, pw, lambda);
    for (int k = 0; k < count; k++) {
      xs[size_t(k)] = px[size_t(k + pad)];
      ys[size_t(k)] = py[size_t(k + pad)];
    }
  }
  else {
    /* Its ends stay: weighed far above everything else. */
    std::vector<double> w(size_t(count), 1.0);
    w.front() = w.back() = 1e9 * std::max(1.0, lambda);
    whittaker3(xs, w, lambda);
    whittaker3(ys, w, lambda);
  }
  /* The smoothed line `d` along it (Catmull-Rom through the samples, so it bends smoothly between them) and which
   * way it runs there. */
  const auto sample = [&](int k) {
    if (loop) {
      k = ((k % count) + count) % count;
    }
    else {
      k = std::clamp(k, 0, count - 1);
    }
    return float2{float(xs[size_t(k)]), float(ys[size_t(k)])};
  };
  const auto smooth_at = [&](double d, float2 *r_dir) {
    const double f = std::clamp(d / step, 0.0, double(loop ? count : count - 1));
    int k = int(std::floor(f));
    if (!loop && k >= count - 1) {
      k = count - 2;
    }
    const float t = float(f - double(k));
    const float2 p0 = sample(k - 1), p1 = sample(k), p2 = sample(k + 1), p3 = sample(k + 2);
    const float t2 = t * t, t3 = t2 * t;
    const float2 pos = (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
                        (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) * 0.5f;
    if (r_dir) {
      const float2 d1 = ((p2 - p0) + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * (2.0f * t) +
                         (p1 * 3.0f - p0 - p2 * 3.0f + p3) * (3.0f * t2)) * 0.5f;
      *r_dir = normalized(d1);
    }
    return pos;
  };
  /* Each anchor where its place along the line went; a span's open ends stay exactly. */
  std::vector<float2> placed(anchor_at.size()), dir(anchor_at.size());
  for (size_t j = 0; j < anchor_at.size(); j++) {
    placed[j] = smooth_at(anchor_at[j], &dir[j]);
  }
  if (!loop) {
    placed.front() = anchor(first);
    placed.back() = anchor(last);
  }
  for (int i = first; i <= last; i++) {
    if (!(loop && i == last)) {
      r_pos[size_t(at(i))] = placed[size_t(i - first)];
    }
  }
  /* Each segment's handles: its directions the smoothed line's at its ends, its lengths fitted to the smoothed line
   * between them (Schneider's fit). */
  for (int i = first; i < last; i++) {
    const size_t j = size_t(i - first);
    std::vector<float2> trace;
    const int pieces = 12;
    for (int k = 0; k <= pieces; k++) {
      trace.push_back(smooth_at(anchor_at[j] + (anchor_at[j + 1] - anchor_at[j]) * double(k) / double(pieces), nullptr));
    }
    trace.front() = placed[j];
    trace.back() = placed[j + 1];
    const float2 d1 = dir[j], d2 = -dir[j + 1];
    float a1 = 0.0f, a2 = 0.0f;
    bezier_fit_handle_lengths(placed[j], placed[j + 1], d1, d2, trace, &a1, &a2);
    r_out[size_t(at(i))] = d1 * a1;
    r_in[size_t(at(i + 1))] = d2 * a2;
  }
}

void sculpt_surface_target(SculptStroke::Curve &c, float lump)
{
  const int n = int(c.points.size());
  c.target_pos.resize(size_t(n));
  c.target_in = c.handle_in;
  c.target_out = c.handle_out;
  for (int i = 0; i < n; i++) {
    c.target_pos[size_t(i)] = c.pos[size_t(c.anchor[size_t(i)])];
  }
  if (n < 2 || lump <= 0.0f) {
    return;
  }
  /* Corners stay corners (Curve::corner: points not set smooth that are peaked or bent): they break the line into
   * spans smoothed apart, each keeping its ends. */
  const auto corner = [&](int i) { return size_t(i) < c.corner.size() && c.corner[size_t(i)]; };
  std::vector<int> breaks;
  for (int i = 0; i < n; i++) {
    if ((!c.closed && (i == 0 || i == n - 1)) || corner(i)) {
      breaks.push_back(i);
    }
  }
  std::sort(breaks.begin(), breaks.end());
  breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());
  if (!c.closed) {
    for (size_t b = 0; b + 1 < breaks.size(); b++) {
      smooth_span(c, breaks[b], breaks[b + 1], false, lump, c.target_pos, c.target_in, c.target_out);
    }
    return;
  }
  if (!breaks.empty()) {
    for (size_t b = 0; b < breaks.size(); b++) {
      const int first = breaks[b];
      const int last = b + 1 < breaks.size() ? breaks[b + 1] : breaks[0] + n;
      smooth_span(c, first, last, false, lump, c.target_pos, c.target_in, c.target_out);
    }
    return;
  }
  if (n >= 3) {
    smooth_span(c, 0, n, true, lump, c.target_pos, c.target_in, c.target_out);
  }
}

SculptStroke sculpt_stroke_begin(const Document &doc, const std::vector<uint64_t> &layers, const SculptDab &dab,
                                 const SculptStroke *fitted, const SculptBox *fit_within)
{
  SculptStroke stroke;
  /* A few samples across the brush, so it works on the line at its own scale. */
  const float spacing = std::max(dab.radius / 6.0f, 1e-6f);
  for (const uint64_t id : layers) {
    const Layer *layer = layer_find(doc, id);
    if (!layer || !sculptable(*layer)) {
      continue;
    }
    const VectorDrawing &drawing = layer->drawing;
    SculptStroke::Layer s;
    s.id = id;
    const Transform2D to_document = layer_world_transform(doc, id);
    std::vector<Transform2D> skins = layer_point_skins(doc, *layer);
    skins.resize(drawing.points.size());
    s.to_shown.resize(skins.size());
    for (size_t i = 0; i < skins.size(); i++) {
      s.to_shown[i] = to_document * skins[i];
    }
    /* Its own axis as it shows: where its points are taken (as the head bone carries a face whole). */
    s.mirror_self = dab.mirror_x && !layer_mirror_partner(const_cast<Document &>(doc), id);
    if (s.mirror_self) {
      const Transform2D shown = s.to_shown.empty() ? to_document : s.to_shown[0];
      s.mirror = shown * Transform2D::scaling({-1.0f, 1.0f}) * shown.inverted();
    }
    SculptDab mirrored = dab;
    mirrored.center = s.mirror.apply(dab.center);
    /* The drawing where its points show (moved by the bones, as the viewport has it: stored handles turn
     * with their point, the others are worked out from where the points went), then the curves as the
     * level being edited shows them; their points keep the drawing's indices. */
    VectorDrawing posed = drawing;
    for (size_t i = 0; i < posed.points.size(); i++) {
      posed.points[i].pos = skins[i].apply(posed.points[i].pos);
    }
    for (Curve &curve : posed.curves) {
      for (CurvePoint &cp : curve.points) {
        cp.handle_in = skins[size_t(cp.point)].apply_vector(cp.handle_in);
        cp.handle_out = skins[size_t(cp.point)].apply_vector(cp.handle_out);
      }
    }
    const VectorDrawing edited = drawing_has_detail(posed) ? drawing_detail_shown_curves(posed) : posed;
    std::vector<std::vector<float>> factors_in, factors_out;
    drawing_detail_shown_handle_factors(posed, factors_in, factors_out);
    for (int ci = 0; ci < int(edited.curves.size()) && ci < int(drawing.curves.size()); ci++) {
      const Curve &shown = edited.curves[size_t(ci)];
      const Curve &real = drawing.curves[size_t(ci)];
      SculptStroke::Curve c;
      c.curve = ci;
      c.closed = shown.closed;
      /* Each shown point's place on the real curve. */
      int from = 0;
      for (const CurvePoint &cp : shown.points) {
        int found = -1;
        for (int k = from; k < int(real.points.size()); k++) {
          if (real.points[size_t(k)].point == cp.point) {
            found = k;
            break;
          }
        }
        if (found < 0) {
          break;
        }
        c.points.push_back(found);
        from = found + 1;
      }
      const int n = int(c.points.size());
      if (n < 2 || n != int(shown.points.size())) {
        continue;
      }
      const auto selection = [&](int i) {
        const MeshPoint &p = drawing.points[size_t(shown.points[size_t(i)].point)];
        return !dab.selected_only || p.selected ? 1.0f : 0.0f;
      };
      const int segments = c.closed ? n : n - 1;
      c.anchor.resize(size_t(n));
      c.handle_in.resize(size_t(n));
      c.handle_out.resize(size_t(n));
      c.factor_in = factors_in[size_t(ci)];
      c.factor_out = factors_out[size_t(ci)];
      for (int seg = 0; seg < segments; seg++) {
        const int a = seg, b = (seg + 1) % n;
        const int pa = shown.points[size_t(a)].point, pb = shown.points[size_t(b)].point;
        float2 ctrl[4];
        curve_segment_bezier(edited, shown, seg, ctrl);
        float2 doc_ctrl[4];
        for (int k = 0; k < 4; k++) {
          doc_ctrl[k] = to_document.apply(ctrl[k]);
        }
        c.handle_out[size_t(a)] = doc_ctrl[1] - doc_ctrl[0];
        c.handle_in[size_t(b)] = doc_ctrl[2] - doc_ctrl[3];
        /* The line resampled at even steps along it (as a Grease Pencil stroke's points lie), each sample keeping
         * its place in the segment's own parameter, so the fitting below can give the segment back exactly. A
         * short segment still has a few. */
        const int table = 64;
        float lengths[table + 1];
        lengths[0] = 0.0f;
        float2 last = doc_ctrl[0];
        for (int k = 1; k <= table; k++) {
          const float2 q = bezier_eval(doc_ctrl, float(k) / float(table));
          lengths[k] = lengths[k - 1] + distance(q, last);
          last = q;
        }
        const float seg_length = lengths[table];
        const int count = std::clamp(int(std::ceil(seg_length / spacing)), 4, 256);
        c.anchor[size_t(a)] = int(c.pos.size());
        const float wa = drawing.points[size_t(pa)].radius, wb = drawing.points[size_t(pb)].radius;
        const float sa = selection(a), sb = selection(b);
        int at_table = 0;
        for (int k = 0; k < count; k++) {
          /* The parameter as far along as k / count of the segment's length. */
          const float want = seg_length * float(k) / float(count);
          while (at_table < table && lengths[at_table + 1] < want) {
            at_table++;
          }
          const float span = lengths[std::min(at_table + 1, table)] - lengths[at_table];
          const float t = seg_length > 1e-12f ?
                              (float(at_table) + (span > 1e-12f ? (want - lengths[at_table]) / span : 0.0f)) /
                                  float(table) :
                              float(k) / float(count);
          c.pos.push_back(bezier_eval(doc_ctrl, t));
          c.param.push_back(t);
          c.width.push_back(wa + (wb - wa) * t);
          c.select.push_back(sa + (sb - sa) * t);
        }
        if (!c.closed && seg == segments - 1) {
          c.anchor[size_t(b)] = int(c.pos.size());
          c.pos.push_back(doc_ctrl[3]);
          c.param.push_back(0.0f);
          c.width.push_back(wb);
          c.select.push_back(sb);
        }
      }
      c.in_line.resize(size_t(n));
      for (int i = 0; i < n; i++) {
        const float2 hi = normalized(c.handle_in[size_t(i)]), ho = normalized(c.handle_out[size_t(i)]);
        c.in_line[size_t(i)] = length(hi) > 0.0f && length(ho) > 0.0f && dot(hi, ho) < -0.99999f;
      }
      /* Corners, which Surface Smooth keeps: a point not set smooth (aligned) that is peaked (Moho's no curvature)
       * or whose handles bend more than 25 degrees. A smooth point is never one, whatever its handles: one with a
       * handle of no length (just extruded, say) or a sharp kink is a lump to smooth, as the artist marked it. */
      c.corner.resize(size_t(n));
      for (int i = 0; i < n; i++) {
        const CurvePoint &sp = shown.points[size_t(i)];
        const float2 hi = normalized(c.handle_in[size_t(i)]), ho = normalized(c.handle_out[size_t(i)]);
        const bool bent = length(hi) > 0.0f && length(ho) > 0.0f && dot(-hi, ho) < std::cos(25.0f * PI / 180.0f);
        c.corner[size_t(i)] = !sp.aligned && (sp.curvature <= 0.0f || bent);
      }
      c.width_start = c.width;
      if (dab.brush == SculptBrush::SurfaceSmooth) {
        /* The fit `fitted` has for this curve when the curve is just as it was then (its points and handles
         * where they were: not changed since), else fitted now; so after a stroke only the curves it changed are
         * fitted again. */
        const SculptStroke::Curve *same = nullptr;
        const auto unchanged = [&](const SculptStroke::Curve &fc) {
          if (fc.curve != c.curve || fc.points != c.points || fc.target_pos.size() != c.points.size() ||
              fc.handle_in != c.handle_in || fc.handle_out != c.handle_out)
          {
            return false;
          }
          for (size_t i = 0; i < c.points.size(); i++) {
            if (!(fc.pos[size_t(fc.anchor[i])] == c.pos[size_t(c.anchor[i])])) {
              return false;
            }
          }
          return true;
        };
        if (fitted) {
          for (const SculptStroke::Layer &fl : fitted->layers) {
            if (fl.id != id) {
              continue;
            }
            for (const SculptStroke::Curve &fc : fl.curves) {
              if (unchanged(fc)) {
                same = &fc;
                break;
              }
            }
          }
        }
        /* Off the view (with `fit_within`): left for later. */
        bool off_view = false;
        if (!same && fit_within && !c.pos.empty()) {
          float2 lo = c.pos[0], hi = c.pos[0];
          for (const float2 &p : c.pos) {
            lo = float2{std::min(lo.x, p.x), std::min(lo.y, p.y)};
            hi = float2{std::max(hi.x, p.x), std::max(hi.y, p.y)};
          }
          off_view = hi.x < fit_within->lo.x || lo.x > fit_within->hi.x || hi.y < fit_within->lo.y ||
                     lo.y > fit_within->hi.y;
        }
        if (same) {
          c.target_pos = same->target_pos;
          c.target_in = same->target_in;
          c.target_out = same->target_out;
        }
        else if (!off_view) {
          sculpt_surface_target(c, sculpt_surface_tolerance(dab));
        }
      }
      if (dab.brush == SculptBrush::Grab) {
        c.grab.resize(c.pos.size());
        for (size_t k = 0; k < c.pos.size(); k++) {
          c.grab[k] = weight_at(dab, c.pos[k]) * c.select[k];
        }
        if (s.mirror_self) {
          c.grab_mirror.resize(c.pos.size());
          for (size_t k = 0; k < c.pos.size(); k++) {
            c.grab_mirror[k] = weight_at(mirrored, c.pos[k]) * c.select[k];
          }
        }
      }
      s.curves.push_back(std::move(c));
    }
    if (!s.curves.empty()) {
      stroke.layers.push_back(std::move(s));
    }
  }
  return stroke;
}

/* X mirror within a layer: each point the dab changed and its image across the layer's axis, found where
 * the points rested before the dab (on the setup frame the dab moves the rest itself), whatever is
 * selected (sculpting doesn't go by the selection). A point the dab changed on both sides keeps its own. */
static std::vector<MirrorPair> sculpt_mirror_pairs(const std::vector<float2> &rest, const std::vector<int> &changed,
                                                   const std::vector<bool> &point_changed, float brush_side)
{
  std::vector<MirrorPair> pairs;
  for (const int a : changed) {
    const float2 here = rest[size_t(a)];
    const float eps = 1e-4f + 1e-4f * (std::fabs(here.x) + std::fabs(here.y));
    const float2 target{-here.x, here.y};
    if (distance(target, here) <= eps) {
      continue; /* On the axis: its own image. */
    }
    for (int b = 0; b < int(rest.size()); b++) {
      if (b == a || distance(rest[size_t(b)], target) > eps) {
        continue;
      }
      /* Brushed on one side only: that side leads. Both (the mirrored dab did the other): the side the
       * brush itself is on leads, once for the pair, so the two stay exact mirror images. */
      if (!point_changed[size_t(b)]) {
        pairs.push_back({a, b, true, false});
      }
      else if ((here.x >= 0.0f) == (brush_side >= 0.0f)) {
        pairs.push_back({a, b, true, false});
      }
      break;
    }
  }
  return pairs;
}

/* Blender's gaussian_blur_1D (geometry/intern/smooth_curves.cc, as its Grease Pencil Smooth uses it; smooth ends
 * and keep shape off): a binomial kernel, the discrete Gaussian, as `iterations` rounds of averaging neighbours
 * would give, each value then going `influence` of the way to its blurred one. An open line's ends stay. */
static void gaussian_blur_1d(std::vector<float2> &values, const std::vector<float> &influence, int iterations,
                             bool cyclic)
{
  const std::vector<float2> src = values;
  const long total = long(src.size());
  if (total < 2) {
    return;
  }
  const long last = total - 1;
  const auto fixed = [&](long index) { return !cyclic && (index == 0 || index == last); };
  const int n_half = (iterations * iterations) / 4 + 2 * iterations + 12;
  double w = 1.0;
  std::vector<double> total_weight(size_t(total), 0.0);
  std::vector<float2> dst(static_cast<size_t>(total));
  for (int step = 0; step < iterations; step++) {
    const long offset = iterations - step;
    for (long index = 0; index < total; index++) {
      if (fixed(index)) {
        continue;
      }
      double w_before = w, w_after = w;
      long before = index - offset, after = index + offset;
      if (cyclic) {
        before = (before % total + total) % total;
        after = after % total;
      }
      else {
        if (before < 0) {
          w_before *= double(-before) / double(index);
        }
        before = std::max(before, 0L);
        if (after > last) {
          w_after *= double(after - last) / double(last - index);
        }
        after = std::min(after, last);
      }
      const float2 cval = src[size_t(index)];
      dst[size_t(index)] += (src[size_t(before)] - cval) * float(w_before);
      dst[size_t(index)] += (src[size_t(after)] - cval) * float(w_after);
      total_weight[size_t(index)] += w_before + w_after;
    }
    w *= double(n_half + offset) / double(n_half + 1 - offset);
  }
  for (long index = 0; index < total; index++) {
    if (fixed(index)) {
      continue;
    }
    total_weight[size_t(index)] += w;
    values[size_t(index)] = src[size_t(index)] +
                            dst[size_t(index)] * float(double(influence[size_t(index)]) / total_weight[size_t(index)]);
  }
}

int sculpt_stroke_dab(Document &doc, SculptStroke &stroke, const SculptDab &dab)
{
  /* No top to it, as Blender's (typed past 1): Grab and Push go further than the mouse; the brushes that work on
   * while held do more per step, never past where a full step would take a point (Pinch to the centre, Smooth
   * all the way to the smoothed line). */
  const float strength = std::max(dab.strength, 0.0f);
  /* Brushes that work on while held: a full step (amount 1) goes this far. */
  const float step = strength * std::clamp(dab.amount, 0.0f, 1.0f);
  const float sign = dab.invert ? -1.0f : 1.0f;
  int count = 0;
  /* X mirror: a layer named for a side (Ear_L) mirrors into its partner (Ear_R), as moves do. */
  std::vector<uint64_t> ids;
  for (const SculptStroke::Layer &sl : stroke.layers) {
    ids.push_back(sl.id);
  }
  const PartnerMirror partners = layer_mirror_partners_begin(doc, ids, dab.mirror_x);
  /* Thickness: the points whose width changed, by layer, for their mirror images. */
  std::vector<std::pair<uint64_t, std::vector<int>>> widened;
  for (SculptStroke::Layer &sl : stroke.layers) {
    Layer *layer = layer_find(doc, sl.id);
    if (!layer || !sculptable(*layer)) {
      continue;
    }
    VectorDrawing &drawing = layer->drawing;
    std::vector<bool> point_changed(drawing.points.size(), false);
    /* Where the points rest before this dab, for the X mirror's pairs. */
    const bool mirror_self = dab.mirror_x && layer_mirror_x_self(doc, sl.id, true);
    std::vector<float2> rest;
    /* The drawing as it was before this dab: what mirrors is the change, so a pose that isn't symmetric (a
     * turned head) stays as it is, only changed alike on both sides. */
    VectorDrawing before_dab;
    if (mirror_self) {
      for (const MeshPoint &p : drawing.points) {
        rest.push_back(point_setup_pos(p));
      }
      before_dab = drawing;
    }
    for (SculptStroke::Curve &c : sl.curves) {
      if (c.curve >= int(drawing.curves.size())) {
        continue;
      }
      Curve &real = drawing.curves[size_t(c.curve)];
      const size_t total = c.pos.size();
      const int n = int(c.points.size());
      if (n < 2 || c.points.back() >= int(real.points.size())) {
        continue;
      }
      std::vector<float2> moved_to = c.pos;
      std::vector<bool> width_changed(total, false);
      /* The brush as dabbed and, mirrored within the layer, again (its turn the other way): each works on
       * the line as the one before left it, weighed where the line was. */
      /* Surface Smooth works on the points and handles themselves, as each side leaves them. */
      std::vector<float2> fair_in = c.handle_in, fair_out = c.handle_out;
      for (int side = 0; side < (sl.mirror_self ? 2 : 1); side++) {
        SculptDab brush = dab;
        float turn_sign = sign;
        const std::vector<float> *grab = &c.grab;
        if (side == 1) {
          brush.center = sl.mirror.apply(dab.center);
          brush.move = sl.mirror.apply_vector(dab.move);
          turn_sign = -sign;
          grab = &c.grab_mirror;
        }
        std::vector<float> w(total, 0.0f);
        if (brush.brush != SculptBrush::Grab) {
          for (size_t k = 0; k < total; k++) {
            w[k] = weight_at(brush, c.pos[k]) * c.select[k];
          }
        }
        switch (brush.brush) {
          case SculptBrush::Grab:
            for (size_t k = 0; k < total && k < grab->size(); k++) {
              moved_to[k] = moved_to[k] + brush.move * ((*grab)[k] * strength);
            }
            break;
          case SculptBrush::Push:
            for (size_t k = 0; k < total; k++) {
              moved_to[k] = moved_to[k] + brush.move * (w[k] * strength);
            }
            break;
          case SculptBrush::Pinch:
            for (size_t k = 0; k < total; k++) {
              moved_to[k] = moved_to[k] + (brush.center - moved_to[k]) * std::min(w[k] * step * 0.5f * sign, 1.0f);
            }
            break;
          case SculptBrush::Twist:
            for (size_t k = 0; k < total; k++) {
              moved_to[k] = brush.center + rotate(moved_to[k] - brush.center, w[k] * step * 0.5f * turn_sign);
            }
            break;
          case SculptBrush::Smooth: {
            /* Blender's Grease Pencil Smooth on the resampled line: each dab blurs the samples twice over
             * (gaussian_blur_1D, two iterations, ends kept, no keep shape), each moving strength x falloff of the way
             * to its blurred place. It works on while the brush is held, moving or not: each frame is its share of a
             * dab (a tenth of a second a whole one). */
            std::vector<float> influence(total);
            for (size_t k = 0; k < total; k++) {
              influence[k] = std::min(w[k] * step, 1.0f);
            }
            gaussian_blur_1d(moved_to, influence, 2, c.closed);
            break;
          }
          case SculptBrush::SurfaceSmooth: {
            /* Toward the line fitted at the stroke's start (sculpt_surface_target): each point under the brush
             * and its handles go strength x falloff x the frame's share of the way, so the effect builds up while
             * held and stops at the fit, never past it. Neighbours' handles toward it move with their own point.
             * The samples then follow the new curve, which the fitting below gives back exactly. */
            if (c.target_pos.size() != size_t(n)) {
              break;
            }
            std::vector<bool> changed(size_t(n), false);
            std::vector<float2> P(static_cast<size_t>(n));
            for (int i = 0; i < n; i++) {
              P[size_t(i)] = moved_to[size_t(c.anchor[size_t(i)])];
            }
            const auto mix = [](float2 x, float2 y, float u) { return x + (y - x) * u; };
            for (int i = 0; i < n; i++) {
              const float f = std::min(w[size_t(c.anchor[size_t(i)])] * step, 1.0f);
              if (f <= 0.0f) {
                continue;
              }
              P[size_t(i)] = mix(P[size_t(i)], c.target_pos[size_t(i)], f);
              fair_in[size_t(i)] = mix(fair_in[size_t(i)], c.target_in[size_t(i)], f);
              fair_out[size_t(i)] = mix(fair_out[size_t(i)], c.target_out[size_t(i)], f);
              changed[size_t(i)] = true;
            }
            /* The samples of every segment that changed follow the new curve, at their own parameters. */
            for (int s = 0; s < segment_count(c); s++) {
              const int a = s, b = (s + 1) % n;
              if (!changed[size_t(a)] && !changed[size_t(b)]) {
                continue;
              }
              const float2 ctrl[4] = {P[size_t(a)], P[size_t(a)] + fair_out[size_t(a)],
                                      P[size_t(b)] + fair_in[size_t(b)], P[size_t(b)]};
              const std::vector<int> idx = segment_samples(c, s);
              for (size_t k = 0; k < idx.size(); k++) {
                moved_to[size_t(idx[k])] = bezier_eval(ctrl, k + 1 == idx.size() ? 1.0f : c.param[size_t(idx[k])]);
              }
            }
            break;
          }
          case SculptBrush::Thickness:
            for (size_t k = 0; k < total; k++) {
              if (w[k] > 0.0f) {
                c.width[k] = std::clamp(c.width[k] + w[k] * step * 0.25f * sign, 0.0f, 20.0f);
                width_changed[k] = true;
              }
            }
            break;
        }
      }
      std::vector<bool> moved(total, false);
      for (size_t k = 0; k < total; k++) {
        moved[k] = !(moved_to[k] == c.pos[k]);
      }
      c.pos = moved_to;

      /* The segments the dab touched are fitted again; the others stay exactly as they are. */
      const int segments = segment_count(c);
      std::vector<std::vector<int>> samples(static_cast<size_t>(segments));
      std::vector<bool> refit(size_t(segments), false);
      for (int s = 0; s < segments; s++) {
        samples[size_t(s)] = segment_samples(c, s);
        for (const int k : samples[size_t(s)]) {
          refit[size_t(s)] = refit[size_t(s)] || moved[size_t(k)];
        }
      }
      const auto real_cp = [&](int i) -> CurvePoint & { return real.points[size_t(c.points[size_t(i)])]; };
      const auto point_of = [&](int i) { return real_cp(i).point; };
      const auto held = [&](int i) { return dab.selected_only && !drawing.points[size_t(point_of(i))].selected; };
      /* Each touched segment's handles are fitted to its samples, every point going where its sample went;
       * a held point (Selected Only) keeps its place and handles. Document space until written. */
      std::vector<float2> new_out(static_cast<size_t>(n)), new_in(static_cast<size_t>(n));
      std::vector<bool> has_out(size_t(n), false), has_in(size_t(n), false);
      const auto stored_handle = [&](int i, bool pre) {
        return pre ? c.handle_in[size_t(i)] : c.handle_out[size_t(i)];
      };
      for (int s = 0; s < segments; s++) {
        if (!refit[size_t(s)]) {
          continue;
        }
        const int a = s, b = (s + 1) % n;
        std::vector<float2> pts;
        std::vector<float> params;
        for (const int k : samples[size_t(s)]) {
          pts.push_back(c.pos[size_t(k)]);
          params.push_back(c.param[size_t(k)]);
        }
        params.back() = 1.0f; /* The next point's sample: the segment's end. */
        const float2 kept1 = stored_handle(a, false), kept2 = stored_handle(b, true);
        float2 h1, h2;
        bezier_fit_handles(pts, params, pts.front(), pts.back(), held(a) ? &kept1 : nullptr, held(b) ? &kept2 : nullptr,
                    std::max(length(kept1), length(kept2)), &h1, &h2);
        new_out[size_t(a)] = h1;
        has_out[size_t(a)] = true;
        new_in[size_t(b)] = h2;
        has_in[size_t(b)] = true;
      }
      /* A smooth (aligned) point whose handles were in line keeps them so: opposite the handle on a side left as it was,
       * else along the average of the two fitted ones. */
      for (int i = 0; i < n; i++) {
        if (held(i) || !real_cp(i).aligned || !c.in_line[size_t(i)] || !(has_out[size_t(i)] || has_in[size_t(i)])) {
          continue;
        }
        const bool both_sides = c.closed || (i > 0 && i < n - 1);
        if (!both_sides) {
          continue;
        }
        float2 &out = new_out[size_t(i)], &in = new_in[size_t(i)];
        if (has_out[size_t(i)] && has_in[size_t(i)]) {
          const float2 along = normalized(normalized(out) - normalized(in));
          if (length(along) > 0.0f) {
            out = along * length(out);
            in = -along * length(in);
          }
        }
        else if (has_out[size_t(i)]) {
          const float2 opposite = -normalized(stored_handle(i, true));
          if (length(opposite) > 0.0f) {
            out = opposite * std::max(dot(out, opposite), length(out) * 0.25f);
          }
        }
        else {
          const float2 opposite = -normalized(stored_handle(i, false));
          if (length(opposite) > 0.0f) {
            in = opposite * std::max(dot(in, opposite), length(in) * 0.25f);
          }
        }
      }
      for (int i = 0; i < n; i++) {
        if (held(i) || !(has_out[size_t(i)] || has_in[size_t(i)])) {
          continue;
        }
        CurvePoint &cp = real_cp(i);
        const Transform2D back = sl.to_shown[size_t(cp.point)].inverted();
        if (has_out[size_t(i)]) {
          c.handle_out[size_t(i)] = new_out[size_t(i)];
        }
        if (has_in[size_t(i)]) {
          c.handle_in[size_t(i)] = new_in[size_t(i)];
        }
        /* Both stored, the side not fitted as it shows (a handle the curve worked out becomes a stored one). */
        cp.handle_in = back.apply_vector(c.handle_in[size_t(i)]) / std::max(c.factor_in[size_t(i)], 1e-6f);
        cp.handle_out = back.apply_vector(c.handle_out[size_t(i)]) / std::max(c.factor_out[size_t(i)], 1e-6f);
        cp.handles_set = true;
        drawing.points[size_t(cp.point)].pos = back.apply(c.pos[size_t(c.anchor[size_t(i)])]);
        point_changed[size_t(cp.point)] = true;
      }

      /* Thickness: each point's width changes by how much its stretch of the line did on average
       * (half way to the points either side). */
      if (dab.brush == SculptBrush::Thickness) {
        for (int i = 0; i < n; i++) {
          if (held(i)) {
            continue;
          }
          float sum = 0.0f;
          int taken = 0;
          bool any = false;
          for (const int s : {i - 1, i}) {
            if (s < 0 && !c.closed) {
              continue;
            }
            if (s >= segments) {
              continue;
            }
            const std::vector<int> &idx = samples[size_t((s + segments) % segments)];
            const bool before = s < i;
            const size_t half = idx.size() / 2;
            for (size_t j = before ? half : 0; j < (before ? idx.size() : half + 1); j++) {
              const size_t k = size_t(idx[j]);
              sum += c.width[k] - c.width_start[k];
              any = any || width_changed[k];
              taken++;
            }
          }
          if (any && taken > 0) {
            const int k = c.anchor[size_t(i)];
            MeshPoint &p = drawing.points[size_t(point_of(i))];
            p.radius = std::clamp(c.width_start[size_t(k)] + sum / float(taken), 0.0f, 20.0f);
            point_changed[size_t(point_of(i))] = true;
          }
        }
      }
    }
    std::vector<int> changed;
    for (size_t i = 0; i < point_changed.size(); i++) {
      if (point_changed[i]) {
        changed.push_back(int(i));
      }
    }
    count += int(changed.size());
    if (changed.empty()) {
      continue;
    }
    if (mirror_self && before_dab.curves.size() == drawing.curves.size() && before_dab.points.size() == drawing.points.size()) {
      const auto reflect = [](float2 v) { return float2{-v.x, v.y}; };
      /* A point on the axis (its own image) moves only along it, its handles changing as each other's
       * mirror image (the one the dab changed more leads). */
      for (int ci = 0; ci < int(drawing.curves.size()); ci++) {
        for (int k = 0; k < int(drawing.curves[size_t(ci)].points.size()); k++) {
          CurvePoint &cp = drawing.curves[size_t(ci)].points[size_t(k)];
          const float2 at = rest[size_t(cp.point)];
          if (!point_changed[size_t(cp.point)] || std::fabs(at.x) > 1e-4f + 1e-4f * std::fabs(at.y)) {
            continue;
          }
          const CurvePoint &old = before_dab.curves[size_t(ci)].points[size_t(k)];
          drawing.points[size_t(cp.point)].pos.x = before_dab.points[size_t(cp.point)].pos.x;
          const float2 d_in = cp.handle_in - old.handle_in, d_out = cp.handle_out - old.handle_out;
          if (length(d_in) >= length(d_out)) {
            cp.handle_out = old.handle_out + reflect(d_in);
          }
          else {
            cp.handle_in = old.handle_in + reflect(d_out);
          }
        }
      }
      /* Each pair: the side under the brush leads, its change mirrored onto the other. */
      const float brush_side = (sl.to_shown.empty() ? Transform2D() : sl.to_shown[0]).inverted().apply(dab.center).x;
      const std::vector<MirrorPair> pairs = sculpt_mirror_pairs(rest, changed, point_changed, brush_side);
      if (!pairs.empty()) {
        /* What a mirror image of the leading side would be, before and now: the image takes the difference. */
        VectorDrawing image_was = before_dab, image_now = drawing;
        drawing_apply_mirror(image_was, pairs);
        drawing_apply_mirror(image_now, pairs);
        std::vector<bool> is_image(drawing.points.size(), false);
        for (const MirrorPair &m : pairs) {
          is_image[size_t(m.dst)] = true;
          MeshPoint &dst = drawing.points[size_t(m.dst)];
          dst.pos = before_dab.points[size_t(m.dst)].pos + (image_now.points[size_t(m.dst)].pos - image_was.points[size_t(m.dst)].pos);
          if (dab.brush == SculptBrush::Thickness) {
            dst.radius = std::clamp(before_dab.points[size_t(m.dst)].radius +
                                        (drawing.points[size_t(m.src)].radius - before_dab.points[size_t(m.src)].radius),
                                    0.0f, 20.0f);
          }
          point_changed[size_t(m.dst)] = true;
        }
        for (int ci = 0; ci < int(drawing.curves.size()); ci++) {
          for (int k = 0; k < int(drawing.curves[size_t(ci)].points.size()); k++) {
            CurvePoint &cp = drawing.curves[size_t(ci)].points[size_t(k)];
            if (!is_image[size_t(cp.point)]) {
              continue;
            }
            const CurvePoint &old = before_dab.curves[size_t(ci)].points[size_t(k)];
            const CurvePoint &a = image_was.curves[size_t(ci)].points[size_t(k)];
            const CurvePoint &b = image_now.curves[size_t(ci)].points[size_t(k)];
            cp.handle_in = old.handle_in + (b.handle_in - a.handle_in);
            cp.handle_out = old.handle_out + (b.handle_out - a.handle_out);
            cp.handles_set = cp.handles_set || b.handles_set;
          }
        }
      }
    }
    if (dab.brush == SculptBrush::Thickness) {
      widened.push_back({sl.id, changed});
    }
    /* Finer detail rides on the points moved. */
    if (drawing_has_detail(drawing)) {
      drawing_detail_sync(drawing, true);
    }
  }
  count += layer_mirror_partners_end(doc, partners);
  /* Widths aren't part of a move: the partner's images take them here. */
  if (dab.mirror_x) {
    for (const auto &[id, points] : widened) {
      Layer *layer = layer_find(doc, id);
      Layer *partner = layer ? layer_mirror_partner(doc, id) : nullptr;
      if (!partner || !partner->has_drawing() || partner->is_reference()) {
        continue;
      }
      const std::vector<int> images = layer_mirror_images(doc, *layer, *partner, points);
      for (size_t k = 0; k < points.size() && k < images.size(); k++) {
        if (images[k] >= 0 && images[k] < int(partner->drawing.points.size())) {
          partner->drawing.points[size_t(images[k])].radius = layer->drawing.points[size_t(points[k])].radius;
          count++;
        }
      }
    }
  }
  return count;
}

}  // namespace oanim
