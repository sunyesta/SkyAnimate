#include "anim/timing_curve.hh"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <utility>

namespace oanim {

namespace {

constexpr float GAP = 1e-4f;

/* A handle pointing the way it should (`out` right, in left) and reaching at most `dx` across. */
float2 handle_within(float2 h, bool out, float dx)
{
  h.x = out ? std::max(h.x, 0.0f) : std::min(h.x, 0.0f);
  if (std::abs(h.x) > dx && std::abs(h.x) > 0.0f) {
    h *= dx / std::abs(h.x);
  }
  return h;
}

struct Segment {
  float2 p0, p1, p2, p3;
};

/* Segment `i` (point i to i + 1) with its handles as the curve uses them. */
Segment segment(const TimingCurve &c, size_t i)
{
  const TimingPoint &a = c.points[i], &b = c.points[i + 1];
  const float dx = std::max(b.co.x - a.co.x, 0.0f);
  float2 out = handle_within(a.out, true, dx), in = handle_within(b.in, false, dx);
  const float reach = out.x - in.x;
  if (reach > dx && reach > 0.0f) {
    out *= dx / reach;
    in *= dx / reach;
  }
  return {a.co, a.co + out, b.co + in, b.co};
}

float2 bezier(const Segment &s, float u)
{
  const float v = 1.0f - u;
  return s.p0 * (v * v * v) + s.p1 * (3.0f * v * v * u) + s.p2 * (3.0f * v * u * u) + s.p3 * (u * u * u);
}

/* Where along segment `s` it is at time `x`: straight across when its handles are a third of it each (as a fit makes
 * them), else found by halving (the handles kept within it, its time only goes forward). */
float segment_param(const Segment &s, float x)
{
  const float dx = s.p3.x - s.p0.x;
  if (dx <= 1e-9f) {
    return 0.0f;
  }
  const float third = dx / 3.0f;
  if (std::abs(s.p1.x - s.p0.x - third) < 1e-6f && std::abs(s.p3.x - s.p2.x - third) < 1e-6f) {
    return std::clamp((x - s.p0.x) / dx, 0.0f, 1.0f);
  }
  float lo = 0.0f, hi = 1.0f;
  for (int it = 0; it < 30; it++) {
    const float mid = 0.5f * (lo + hi);
    (bezier(s, mid).x < x ? lo : hi) = mid;
  }
  return 0.5f * (lo + hi);
}

/* The segment holding time `x`. */
size_t segment_at(const TimingCurve &c, float x)
{
  size_t i = 0;
  while (i + 2 < c.points.size() && c.points[i + 1].co.x <= x) {
    i++;
  }
  return i;
}

float sign_of(float v)
{
  return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
}

/* The slope at each frame of `u` (frames 1/n apart): from the frames either side, closely (five frames' worth where
 * there are as many, so a smooth timing needs few points), held back where it would carry the curve past a frame's
 * neighbour between frames (Fritsch and Carlson's bound: a hold stays flat, a turn back doesn't wobble). */
std::vector<float> frame_slopes(const std::vector<float> &u)
{
  const int n = int(u.size()) - 1;
  const float h = 1.0f / float(n);
  std::vector<float> d(static_cast<size_t>(n)), m(static_cast<size_t>(n + 1), 0.0f);
  for (int k = 0; k < n; k++) {
    d[size_t(k)] = (u[size_t(k + 1)] - u[size_t(k)]) / h;
  }
  if (n == 1) {
    m[0] = m[1] = d[0];
    return m;
  }
  const auto U = [&](int k) { return u[size_t(k)]; };
  for (int k = 0; k <= n; k++) {
    float s;
    if (n >= 4) {
      /* Fourth order: central inside, leaning in at the frames next to the keys and at the keys. */
      if (k >= 2 && k <= n - 2) {
        s = (U(k - 2) - 8.0f * U(k - 1) + 8.0f * U(k + 1) - U(k + 2)) / (12.0f * h);
      }
      else if (k == 1) {
        s = (-3.0f * U(0) - 10.0f * U(1) + 18.0f * U(2) - 6.0f * U(3) + U(4)) / (12.0f * h);
      }
      else if (k == n - 1) {
        s = -(-3.0f * U(n) - 10.0f * U(n - 1) + 18.0f * U(n - 2) - 6.0f * U(n - 3) + U(n - 4)) / (12.0f * h);
      }
      else if (k == 0) {
        s = (-25.0f * U(0) + 48.0f * U(1) - 36.0f * U(2) + 16.0f * U(3) - 3.0f * U(4)) / (12.0f * h);
      }
      else {
        s = -(-25.0f * U(n) + 48.0f * U(n - 1) - 36.0f * U(n - 2) + 16.0f * U(n - 3) - 3.0f * U(n - 4)) / (12.0f * h);
      }
    }
    else if (k > 0 && k < n) {
      s = (U(k + 1) - U(k - 1)) / (2.0f * h);
    }
    else if (k == 0) {
      s = (-3.0f * U(0) + 4.0f * U(1) - U(2)) / (2.0f * h);
    }
    else {
      s = (3.0f * U(n) - 4.0f * U(n - 1) + U(n - 2)) / (2.0f * h);
    }
    /* The bound: no steeper than three times the step on either side, and the way the steps go. */
    if (k == 0 || k == n) {
      const float dk = d[size_t(k == 0 ? 0 : n - 1)];
      s = s * dk <= 0.0f ? 0.0f : sign_of(dk) * std::min(std::abs(s), 3.0f * std::abs(dk));
    }
    else {
      const float a = d[size_t(k - 1)], b = d[size_t(k)];
      const float most = 3.0f * std::min(std::abs(a), std::abs(b));
      if (a * b > 0.0f) {
        s = s * a <= 0.0f ? 0.0f : sign_of(a) * std::min(std::abs(s), most);
      }
      else {
        s = std::clamp(s, -most, most);
      }
    }
    m[size_t(k)] = s;
  }
  return m;
}

float brush_falloff(float distance, float radius)
{
  if (radius <= 0.0f || distance >= radius) {
    return 0.0f;
  }
  const float x = distance / radius;
  return 1.0f - x * x * (3.0f - 2.0f * x);
}


/* A piece of curve from `a` to `b` (each a place and the slope its handle goes at), its handles `alpha` and `beta` long
 * in time. */
Segment span_segment(float2 a, float slope_a, float2 b, float slope_b, float alpha, float beta)
{
  return {a, a + float2(alpha, slope_a * alpha), b - float2(beta, slope_b * beta), b};
}

/* How far the piece misses the most of `samples` (time, along) from index `from` to `to`, both left out. */
float span_miss(const Segment &s, const std::vector<float2> &samples, size_t from, size_t to)
{
  float worst = 0.0f;
  for (size_t i = from + 1; i < to; i++) {
    worst = std::max(worst, std::abs(bezier(s, segment_param(s, samples[i].x)).y - samples[i].y));
  }
  return worst;
}

struct SpanFit {
  float alpha = 0.0f, beta = 0.0f, miss = 0.0f;
};

/* The handle lengths that make the piece from `a` to `b` go through the samples between best, how far along each is at
 * its time (least squares, by Levenberg and Marquardt's steps from a third of the way each), kept within the piece
 * (each at most across it, the two together too, so time only goes forward). */
SpanFit span_fit(float2 a, float slope_a, float2 b, float slope_b, const std::vector<float2> &samples, size_t from,
                 size_t to)
{
  const float len = b.x - a.x;
  SpanFit best{len / 3.0f, len / 3.0f, 0.0f};
  best.miss = span_miss(span_segment(a, slope_a, b, slope_b, best.alpha, best.beta), samples, from, to);
  if (to <= from + 1 || !(len > 1e-7f) || best.miss <= 1e-7f) {
    return best;
  }
  const auto within = [len](float &alpha, float &beta) {
    alpha = std::clamp(alpha, len * 0.02f, len);
    beta = std::clamp(beta, len * 0.02f, len);
    if (alpha + beta > len) {
      const float k = len / (alpha + beta);
      alpha *= k;
      beta *= k;
    }
  };
  /* How far each sample is off the piece, and their squares' sum. */
  const auto off = [&](float alpha, float beta, std::vector<double> &r) {
    const Segment s = span_segment(a, slope_a, b, slope_b, alpha, beta);
    r.resize(to - from - 1);
    double sum = 0.0;
    for (size_t i = from + 1; i < to; i++) {
      const double d = double(bezier(s, segment_param(s, samples[i].x)).y - samples[i].y);
      r[i - from - 1] = d;
      sum += d * d;
    }
    return sum;
  };
  float alpha = best.alpha, beta = best.beta;
  std::vector<double> r, ra, rb, tried;
  double sum = off(alpha, beta, r);
  double damp = 1e-3;
  const float h = len * 1e-3f;
  for (int it = 0; it < 12 && sum > 1e-14; it++) {
    float pa = alpha + h, pb = beta;
    within(pa, pb);
    off(pa, pb, ra);
    float qa = alpha, qb = beta + h;
    within(qa, qb);
    off(qa, qb, rb);
    const double da = double(pa - alpha), db = double(qb - beta);
    double a11 = 0.0, a12 = 0.0, a22 = 0.0, g1 = 0.0, g2 = 0.0;
    for (size_t k = 0; k < r.size(); k++) {
      const double ja = da != 0.0 ? (ra[k] - r[k]) / da : 0.0, jb = db != 0.0 ? (rb[k] - r[k]) / db : 0.0;
      a11 += ja * ja;
      a12 += ja * jb;
      a22 += jb * jb;
      g1 += ja * r[k];
      g2 += jb * r[k];
    }
    bool moved = false;
    while (damp < 1e8) {
      const double m11 = a11 * (1.0 + damp) + 1e-18, m22 = a22 * (1.0 + damp) + 1e-18;
      const double det = m11 * m22 - a12 * a12;
      if (!(std::abs(det) > 0.0)) {
        damp *= 10.0;
        continue;
      }
      float na = alpha - float((g1 * m22 - g2 * a12) / det), nb = beta - float((m11 * g2 - a12 * g1) / det);
      if (!std::isfinite(na) || !std::isfinite(nb)) {
        damp *= 10.0;
        continue;
      }
      within(na, nb);
      const double now = off(na, nb, tried);
      if (now < sum) {
        alpha = na;
        beta = nb;
        sum = now;
        r.swap(tried);
        damp = std::max(damp * 0.1, 1e-9);
        moved = true;
        break;
      }
      damp *= 10.0;
    }
    if (!moved) {
      break;
    }
  }
  const float miss = span_miss(span_segment(a, slope_a, b, slope_b, alpha, beta), samples, from, to);
  if (miss < best.miss) {
    best = {alpha, beta, miss};
  }
  return best;
}

/* A point a curve may keep: its place among the samples and the slopes its handles go at. */
struct Knot {
  size_t sample = 0;
  float slope_in = 0.0f, slope_out = 0.0f;
};

/* Points taken out one at a time, the one whose piece across its neighbours misses the samples least first (its
 * neighbours' handles fitted again), until the next would miss by more than `tolerance` or only `keep` are left between
 * the ends. Returns those kept (the ends always), and in `r_fits` the handle lengths of each piece between them. */
std::vector<size_t> knots_reduce(const std::vector<Knot> &knots, const std::vector<float2> &samples, float tolerance,
                                 size_t keep, std::vector<SpanFit> &r_fits)
{
  const size_t count = knots.size();
  std::vector<size_t> prev(count, 0), next(count, count - 1);
  std::vector<char> alive(count, 1);
  std::vector<SpanFit> cost(count);
  for (size_t i = 0, last = 0; i < count; i++) {
    if (alive[i] && i > 0) {
      prev[i] = last;
      next[last] = i;
      last = i;
    }
  }
  const auto fit = [&](size_t a, size_t b) {
    return span_fit(samples[knots[a].sample], knots[a].slope_out, samples[knots[b].sample], knots[b].slope_in, samples,
                    knots[a].sample, knots[b].sample);
  };
  size_t inside = 0;
  for (size_t i = 1; i + 1 < count; i++) {
    if (alive[i]) {
      cost[i] = fit(prev[i], next[i]);
      inside++;
    }
  }
  while (inside > keep) {
    size_t at = 0;
    for (size_t i = 1; i + 1 < count; i++) {
      if (alive[i] && (at == 0 || cost[i].miss < cost[at].miss)) {
        at = i;
      }
    }
    if (at == 0 || cost[at].miss > tolerance) {
      break;
    }
    alive[at] = 0;
    inside--;
    const size_t p = prev[at], q = next[at];
    next[p] = q;
    prev[q] = p;
    if (p > 0) {
      cost[p] = fit(prev[p], q);
    }
    if (q + 1 < count) {
      cost[q] = fit(p, next[q]);
    }
  }
  std::vector<size_t> kept;
  r_fits.clear();
  for (size_t i = 0; i < count; i++) {
    if (alive[i]) {
      if (!kept.empty()) {
        r_fits.push_back(fit(kept.back(), i));
      }
      kept.push_back(i);
    }
  }
  return kept;
}

}  // namespace

float timing_progress(const TimingCurve &curve, float t)
{
  if (curve.empty()) {
    return t;
  }
  t = std::clamp(t, curve.points.front().co.x, curve.points.back().co.x);
  const Segment s = segment(curve, segment_at(curve, t));
  return bezier(s, segment_param(s, t)).y;
}

std::vector<float> timing_frame_progress(const TimingCurve &curve, int n)
{
  n = std::max(n, 1);
  std::vector<float> u(size_t(n + 1));
  for (int k = 0; k <= n; k++) {
    u[size_t(k)] = timing_progress(curve, float(k) / float(n));
  }
  u[0] = 0.0f;
  u[size_t(n)] = 1.0f;
  return u;
}

std::vector<float> timing_speeds(const TimingCurve &curve, int n)
{
  const std::vector<float> u = timing_frame_progress(curve, n);
  n = int(u.size()) - 1;
  std::vector<float> s(static_cast<size_t>(n));
  for (int k = 0; k < n; k++) {
    s[size_t(k)] = (u[size_t(k + 1)] - u[size_t(k)]) * float(n);
  }
  return s;
}

TimingCurve timing_fit(const std::vector<float> &progress, float tolerance)
{
  const int n = int(progress.size()) - 1;
  if (n < 2) {
    return {};
  }
  std::vector<float> u = progress;
  u[0] = 0.0f;
  u[size_t(n)] = 1.0f;
  float off_even = 0.0f;
  for (int k = 0; k <= n; k++) {
    off_even = std::max(off_even, std::abs(u[size_t(k)] - float(k) / float(n)));
  }
  if (off_even <= tolerance) {
    return {};
  }
  const std::vector<float> m = frame_slopes(u);
  const float h = 1.0f / float(n);
  /* Every frame a point to start with, its slope the data's; then the points the curve keeps within the tolerance
   * without taken out, the one that costs least first, each piece's handles fitted to the frames it spans (so a smooth
   * bend is one piece, not many). */
  std::vector<float2> samples(size_t(n + 1));
  std::vector<Knot> knots(size_t(n + 1));
  for (int k = 0; k <= n; k++) {
    samples[size_t(k)] = {float(k) * h, u[size_t(k)]};
    knots[size_t(k)] = {size_t(k), m[size_t(k)], m[size_t(k)]};
  }
  std::vector<SpanFit> fits;
  const std::vector<size_t> kept = knots_reduce(knots, samples, tolerance, 0, fits);
  TimingCurve curve;
  for (size_t i = 0; i < kept.size(); i++) {
    const int k = int(kept[i]);
    TimingPoint p;
    p.co = samples[size_t(k)];
    const float slope = m[size_t(k)];
    if (i > 0) {
      p.in = {-fits[i - 1].beta, -slope * fits[i - 1].beta};
    }
    if (i + 1 < kept.size()) {
      p.out = {fits[i].alpha, slope * fits[i].alpha};
    }
    p.aligned = true;
    curve.points.push_back(p);
  }
  curve.points.front().co = {0.0f, 0.0f};
  curve.points.back().co = {1.0f, 1.0f};
  return curve;
}

int timing_decimate(TimingCurve &curve, int n, float ratio)
{
  if (curve.points.size() < 3) {
    return 0;
  }
  n = std::max(n, 1);
  const size_t count = curve.points.size();
  /* The curve sampled four times a frame, and at each of its points (which take the samples' places among them). */
  std::vector<float2> samples;
  std::vector<Knot> knots;
  const int per = 4 * n;
  int j = 0;
  for (size_t i = 0; i < count; i++) {
    const float x = curve.points[i].co.x;
    for (; j <= per && float(j) / float(per) < x - 1e-6f; j++) {
      const float sx = float(j) / float(per);
      samples.push_back({sx, timing_progress(curve, sx)});
    }
    while (j <= per && float(j) / float(per) <= x + 1e-6f) {
      j++;
    }
    float2 in, out;
    timing_point_handles(curve, i, in, out);
    /* The slope each handle goes at; a handle of no length goes the way the curve does there. */
    const auto slope = [&](float2 h, float side) {
      if (std::abs(h.x) > 1e-6f) {
        return h.y / h.x;
      }
      const float d = 1e-3f;
      const float x0 = std::clamp(x - d, 0.0f, 1.0f), x1 = std::clamp(x + d, 0.0f, 1.0f);
      const float a = side < 0.0f ? x0 : x, b = side < 0.0f ? x : x1;
      return b > a ? (timing_progress(curve, b) - timing_progress(curve, a)) / (b - a) : 0.0f;
    };
    knots.push_back({samples.size(), slope(in, -1.0f), slope(out, 1.0f)});
    samples.push_back(curve.points[i].co);
  }
  const size_t inside = count - 2;
  const size_t keep = size_t(std::lround(double(std::clamp(ratio, 0.0f, 1.0f)) * double(inside)));
  std::vector<SpanFit> fits;
  const std::vector<size_t> kept = knots_reduce(knots, samples, std::numeric_limits<float>::infinity(), keep, fits);
  if (kept.size() == count) {
    return 0;
  }
  TimingCurve out;
  for (size_t i = 0; i < kept.size(); i++) {
    TimingPoint p = curve.points[kept[i]];
    /* A piece between two points that were already neighbours keeps its handles; one across points taken out takes
     * the fitted ones. */
    if (i > 0 && kept[i - 1] + 1 != kept[i]) {
      p.in = {-fits[i - 1].beta, -knots[kept[i]].slope_in * fits[i - 1].beta};
    }
    if (i + 1 < kept.size() && kept[i] + 1 != kept[i + 1]) {
      p.out = {fits[i].alpha, knots[kept[i]].slope_out * fits[i].alpha};
    }
    out.points.push_back(p);
  }
  const int gone = int(count - kept.size());
  curve = std::move(out);
  return gone;
}

TimingCurve timing_from_speeds(const std::vector<float> &speeds)
{
  const int n = int(speeds.size());
  float total = 0.0f;
  for (const float s : speeds) {
    total += s;
  }
  if (n < 2 || total <= 1e-9f) {
    return {};
  }
  std::vector<float> u(size_t(n + 1), 0.0f);
  for (int k = 1; k <= n; k++) {
    u[size_t(k)] = u[size_t(k - 1)] + speeds[size_t(k - 1)] / total;
  }
  return timing_fit(u);
}

bool timing_valid(const TimingCurve &curve)
{
  if (curve.points.size() < 2) {
    return false;
  }
  for (size_t i = 0; i < curve.points.size(); i++) {
    const TimingPoint &p = curve.points[i];
    for (const float v : {p.co.x, p.co.y, p.in.x, p.in.y, p.out.x, p.out.y}) {
      if (!std::isfinite(v)) {
        return false;
      }
    }
    if (i > 0 && !(p.co.x > curve.points[i - 1].co.x)) {
      return false;
    }
  }
  const auto near = [](float2 a, float2 b) { return std::abs(a.x - b.x) < 1e-4f && std::abs(a.y - b.y) < 1e-4f; };
  return near(curve.points.front().co, {0.0f, 0.0f}) && near(curve.points.back().co, {1.0f, 1.0f});
}

TimingCurve timing_even()
{
  TimingCurve c;
  c.points.resize(2);
  c.points[0].co = {0.0f, 0.0f};
  c.points[0].out = {1.0f / 3.0f, 1.0f / 3.0f};
  c.points[1].co = {1.0f, 1.0f};
  c.points[1].in = {-1.0f / 3.0f, -1.0f / 3.0f};
  return c;
}

void timing_point_handles(const TimingCurve &curve, size_t i, float2 &r_in, float2 &r_out)
{
  r_in = r_out = {0.0f, 0.0f};
  if (curve.empty() || i >= curve.points.size()) {
    return;
  }
  if (i > 0) {
    const Segment s = segment(curve, i - 1);
    r_in = s.p2 - s.p3;
  }
  if (i + 1 < curve.points.size()) {
    const Segment s = segment(curve, i);
    r_out = s.p1 - s.p0;
  }
}

int timing_point_insert(TimingCurve &curve, float x)
{
  if (curve.empty() || x <= curve.points.front().co.x + GAP || x >= curve.points.back().co.x - GAP) {
    return -1;
  }
  const size_t i = segment_at(curve, x);
  if (x <= curve.points[i].co.x + GAP || x >= curve.points[i + 1].co.x - GAP) {
    return -1;
  }
  const Segment s = segment(curve, i);
  const float u = segment_param(s, x);
  /* de Casteljau: the segment split at u keeps its shape. */
  const float2 q0 = lerp(s.p0, s.p1, u), q1 = lerp(s.p1, s.p2, u), q2 = lerp(s.p2, s.p3, u);
  const float2 r0 = lerp(q0, q1, u), r1 = lerp(q1, q2, u), mid = lerp(r0, r1, u);
  curve.points[i].out = q0 - s.p0;
  curve.points[i + 1].in = q2 - s.p3;
  TimingPoint p;
  p.co = mid;
  p.in = r0 - mid;
  p.out = r1 - mid;
  p.aligned = true;
  curve.points.insert(curve.points.begin() + std::ptrdiff_t(i + 1), p);
  return int(i + 1);
}

void timing_point_move(TimingCurve &curve, size_t i, float2 co)
{
  if (curve.empty() || i == 0 || i + 1 >= curve.points.size()) {
    return;
  }
  co.x = std::clamp(co.x, curve.points[i - 1].co.x + GAP, curve.points[i + 1].co.x - GAP);
  curve.points[i].co = co;
}

void timing_handle_set(TimingCurve &curve, size_t i, bool out, float2 h, bool free)
{
  if (curve.empty() || i >= curve.points.size() || (out ? i + 1 >= curve.points.size() : i == 0)) {
    return;
  }
  TimingPoint &p = curve.points[i];
  const float dx = out ? curve.points[i + 1].co.x - p.co.x : p.co.x - curve.points[i - 1].co.x;
  h = handle_within(h, out, dx);
  (out ? p.out : p.in) = h;
  /* An end point has one handle: nothing to turn. */
  const bool has_other = out ? i > 0 : i + 1 < curve.points.size();
  if (p.aligned && !free && has_other && length(h) > 1e-9f) {
    float2 &other = out ? p.in : p.out;
    const float other_dx = out ? p.co.x - curve.points[i - 1].co.x : curve.points[i + 1].co.x - p.co.x;
    other = handle_within(-normalize(h) * length(other), !out, other_dx);
  }
}

bool timing_point_remove(TimingCurve &curve, size_t i)
{
  if (curve.empty() || i == 0 || i + 1 >= curve.points.size()) {
    return false;
  }
  curve.points.erase(curve.points.begin() + std::ptrdiff_t(i));
  return true;
}

/* Smooth's dabs along a stroke: a dab every so often as the graph shows the way (`dist`), at most so many that a huge
 * stroke or brush can't go on for ever (spaced further apart on a stroke longer than that). None on a stroke too long
 * to measure. */
template<typename Dist> static std::vector<float2> stroke_dabs(const std::vector<float2> &path, float radius, Dist dist)
{
  constexpr float MOST_DABS = 2000.0f;
  float total = 0.0f;
  for (size_t p = 1; p < path.size(); p++) {
    total += dist(path[p - 1], path[p]);
  }
  if (!std::isfinite(total)) {
    return {};
  }
  const float spacing = std::max({radius * 0.2f, 0.25f, total / MOST_DABS});
  std::vector<float2> dabs{path.front()};
  float since = 0.0f;
  for (size_t p = 1; p < path.size(); p++) {
    const float2 a = path[p - 1], b = path[p];
    const float len = dist(a, b);
    float at = spacing - since;
    while (at <= len) {
      dabs.push_back(lerp(a, b, len > 0.0f ? at / len : 1.0f));
      at += spacing;
    }
    since = len - (at - spacing);
  }
  return dabs;
}

/* Without Dyntopo: the brushes move the curve's own points and their handles' tips (none added or taken out), as a
 * drawing's sculpt does without Dyntopo. Grab moves each by the stroke, weighted by how near the brush's first place
 * it was; Smooth pulls each toward the curve's average around it. The points stay in time order (pushed along, not
 * stopped), aligned points' handles in line. */
static TimingCurve sculpt_points(const TimingCurve &curve, int n, TimingBrush brush, const std::vector<float2> &path,
                                 float radius, float strength, float aspect, bool lock_x)
{
  TimingCurve c = curve.empty() ? timing_even() : curve;
  const size_t count = c.points.size();
  const float fn = float(n);
  /* Places in the graph's terms: frames across, the way along up. */
  const auto graph = [fn](float2 co) { return float2(co.x * fn, co.y); };
  const auto dist = [aspect](float2 a, float2 b) { return length(float2(a.x - b.x, (a.y - b.y) * aspect)); };
  /* Each point and its two tips, as places in the curve's terms. */
  std::vector<float2> co(count), tip_in(count), tip_out(count);
  for (size_t i = 0; i < count; i++) {
    co[i] = c.points[i].co;
    tip_in[i] = co[i] + c.points[i].in;
    tip_out[i] = co[i] + c.points[i].out;
  }
  const auto each = [&](const std::function<void(float2 &)> &move) {
    for (size_t i = 0; i < count; i++) {
      /* The ends are the keys: they never move, though their handles do. */
      if (i > 0 && i + 1 < count) {
        move(co[i]);
      }
      if (i > 0) {
        move(tip_in[i]);
      }
      if (i + 1 < count) {
        move(tip_out[i]);
      }
    }
  };
  if (brush == TimingBrush::Grab) {
    const float2 from = path.front(), delta = path.back() - path.front();
    const float2 by{lock_x ? 0.0f : delta.x / fn, delta.y};
    each([&](float2 &p) { p += by * (brush_falloff(dist(graph(p), from), radius) * strength); });
  }
  else {
    const float reach = std::max(radius * 0.5f, 1.0f) / fn;
    for (const float2 dab : stroke_dabs(path, radius, dist)) {
      /* The curve as it is before this dab, and its average around a time `x` (0-1) within half the brush. */
      TimingCurve now = c;
      for (size_t i = 0; i < count; i++) {
        now.points[i].co = co[i];
        now.points[i].in = tip_in[i] - co[i];
        now.points[i].out = tip_out[i] - co[i];
      }
      const auto average = [&](float x) {
        constexpr int STEPS = 8;
        float sum = 0.0f, wsum = 0.0f;
        for (int k = -STEPS; k <= STEPS; k++) {
          const float t = 1.0f - std::abs(float(k)) / float(STEPS + 1);
          sum += timing_progress(now, std::clamp(x + reach * float(k) / float(STEPS), 0.0f, 1.0f)) * t;
          wsum += t;
        }
        return sum / wsum;
      };
      each([&](float2 &p) {
        const float w = brush_falloff(dist(graph(p), dab), radius) * strength;
        if (w > 0.0f) {
          p.y += (average(std::clamp(p.x, 0.0f, 1.0f)) - p.y) * w;
        }
      });
    }
  }
  /* Time only goes forward: each point after the one before, with room for those after it. */
  for (size_t i = 1; i + 1 < count; i++) {
    const float x = std::clamp(co[i].x, co[i - 1].x + GAP, 1.0f - float(count - 1 - i) * GAP);
    const float dx = x - co[i].x;
    co[i].x = x;
    tip_in[i].x += dx;
    tip_out[i].x += dx;
  }
  for (size_t i = 0; i < count; i++) {
    TimingPoint &p = c.points[i];
    p.co = co[i];
    p.in = i > 0 ? tip_in[i] - co[i] : float2(0.0f, 0.0f);
    p.out = i + 1 < count ? tip_out[i] - co[i] : float2(0.0f, 0.0f);
    /* An aligned point's handles back in line, each keeping its length. */
    if (p.aligned && i > 0 && i + 1 < count) {
      const float li = length(p.in), lo = length(p.out);
      const float2 way = p.out - p.in;
      const float lw = length(way);
      if (li > 1e-6f && lo > 1e-6f && lw > 1e-6f) {
        p.in = way * (-li / lw);
        p.out = way * (lo / lw);
      }
    }
  }
  return timing_valid(c) ? c : curve;
}

TimingCurve timing_sculpt(const TimingCurve &curve, int n, TimingBrush brush, const std::vector<float2> &path,
                          float radius, float strength, float aspect, bool lock_x, bool dyntopo)
{
  if (n < 2 || path.empty() || !(radius > 0.0f)) {
    return curve;
  }
  if (!std::isfinite(aspect) || aspect <= 0.0f) {
    aspect = 1.0f;
  }
  strength = std::clamp(strength, 0.0f, 1.0f);
  if (!dyntopo) {
    return sculpt_points(curve, n, brush, path, radius, strength, aspect, lock_x);
  }
  constexpr int PER = 4;
  const int m = n * PER;
  /* Distances as the graph shows them: a whole way up is `aspect` frames across, so the brush is a circle there. */
  auto dist = [aspect](float2 a, float2 b) { return length(float2(a.x - b.x, (a.y - b.y) * aspect)); };
  /* Samples a quarter frame apart, each with its frame (x) and how far along (y). */
  std::vector<float2> s(size_t(m + 1));
  for (int j = 0; j <= m; j++) {
    const float x = float(j) / float(m);
    s[size_t(j)] = {x * float(n), timing_progress(curve, x)};
  }
  std::vector<float> u(size_t(n + 1));
  if (brush == TimingBrush::Grab && !lock_x) {
    /* Grab in time too. Each sample goes up and down by the stroke as far as the brush reached it where the stroke
     * began (as Blender's Grab); in time, the part of the curve the brush was pressed on goes with the mouse and the
     * rest makes room, each side of it squeezed or stretched up to the key there, so no sample overtakes the one after
     * it (moved one by one within the brush alone, they piled up against those they would pass and the brush stuck).
     * Each frame then takes the progress where the moved samples cross it. */
    const float2 from = path.front(), delta = path.back() - path.front();
    const float move = std::abs(delta.x);
    /* How far each side reaches in time: the brush, or further the further the stroke goes (the falloff is steepest
     * at 1.5 / its reach), but never past the key on that side. */
    const float reach = std::max(radius, move * 1.6f);
    const auto in_time = [&](float x) {
      const float d = x - from.x;
      const float side = std::min(reach, d < 0.0f ? from.x : float(n) - from.x);
      if (!(side > 1e-6f) || std::abs(d) >= side) {
        return 0.0f;
      }
      const float q = std::abs(d) / side;
      const float smooth = 1.0f - q * q * (3.0f - 2.0f * q), straight = 1.0f - q;
      /* Straighter the further the stroke goes for the room it has, wholly so at half of it: a smooth falloff
       * squeezes the curve most at its steepest (1.5 / side), a straight one evenly up to the key. */
      const float t = std::clamp(2.0f * move / side, 0.0f, 1.0f);
      return smooth + (straight - smooth) * t;
    };
    /* Whether the brush was on the curve at all where it was pressed. */
    const float on = brush_falloff(std::abs(timing_progress(curve, std::clamp(from.x / float(n), 0.0f, 1.0f)) - from.y) *
                                       aspect,
                                   radius) *
                     strength;
    std::vector<float2> moved = s;
    for (int j = 1; j < m; j++) {
      moved[size_t(j)].x += delta.x * in_time(s[size_t(j)].x) * on;
      moved[size_t(j)].y += delta.y * brush_falloff(dist(s[size_t(j)], from), radius) * strength;
    }
    /* Time only goes forward (where the curve is steep, or near a key the reach runs into): each after the one before,
     * the ends where they were. */
    constexpr float gap = 1e-3f;
    for (int j = 1; j < m; j++) {
      moved[size_t(j)].x = std::clamp(moved[size_t(j)].x, moved[size_t(j - 1)].x + gap, float(n) - float(m - j) * gap);
    }
    size_t j = 0;
    for (int k = 0; k <= n; k++) {
      const float x = float(k);
      while (j + 2 < moved.size() && moved[j + 1].x < x) {
        j++;
      }
      const float2 p = moved[j], q = moved[j + 1];
      const float t = q.x - p.x > 1e-9f ? std::clamp((x - p.x) / (q.x - p.x), 0.0f, 1.0f) : 0.0f;
      u[size_t(k)] = k == 0 ? 0.0f : k == n ? 1.0f : p.y + (q.y - p.y) * t;
    }
  }
  else {
    if (brush == TimingBrush::Grab) {
      /* Up and down only (Lock X): each sample by the stroke, weighted by how near its first place it was. */
      const float2 from = path.front(), delta = path.back() - path.front();
      const std::vector<float2> start = s;
      for (int j = 1; j < m; j++) {
        s[size_t(j)].y += delta.y * brush_falloff(dist(start[size_t(j)], from), radius) * strength;
      }
    }
    else {
      /* Dabs along the stroke, each pulling the samples under it toward their neighbours' average (within half the
       * brush, at least a frame each way); up and down only, the samples staying a quarter frame apart. */
      const float reach = std::max(radius * 0.5f, 1.0f);
      /* Never wider than the whole stretch (a huge radius would overflow it). */
      const int span = int(std::min(std::ceil(reach * float(PER)), float(m)));
      for (const float2 c : stroke_dabs(path, radius, dist)) {
        const std::vector<float2> cur = s;
        for (int j = 1; j < m; j++) {
          const float w = brush_falloff(dist(cur[size_t(j)], c), radius) * strength;
          if (w <= 0.0f) {
            continue;
          }
          float sum = 0.0f, wsum = 0.0f;
          for (int o = std::max(0, j - span); o <= std::min(m, j + span); o++) {
            const float t = 1.0f - std::abs(float(o - j)) / float(span + 1);
            sum += cur[size_t(o)].y * t;
            wsum += t;
          }
          if (wsum > 0.0f) {
            s[size_t(j)].y += (sum / wsum - cur[size_t(j)].y) * w;
          }
        }
      }
    }
    for (int k = 0; k <= n; k++) {
      u[size_t(k)] = k == 0 ? 0.0f : k == n ? 1.0f : s[size_t(k * PER)].y;
    }
  }
  for (const float v : u) {
    if (!std::isfinite(v)) {
      return curve;
    }
  }
  return timing_fit(u, TIMING_SCULPT_TOLERANCE);
}

}  // namespace oanim

namespace oanim {

std::vector<float2> timing_stroke_parse(const std::string &text, float start)
{
  std::vector<float2> path;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find(';', at);
    end = end == std::string::npos ? text.size() : end;
    const std::string place = text.substr(at, end - at);
    const size_t comma = place.find(',');
    if (comma != std::string::npos) {
      char *stop_f = nullptr, *stop_u = nullptr;
      const float f = std::strtof(place.c_str(), &stop_f);
      const float u = std::strtof(place.c_str() + comma + 1, &stop_u);
      if (stop_f != place.c_str() && stop_u != place.c_str() + comma + 1 && std::isfinite(f) && std::isfinite(u)) {
        path.push_back({f - start, u});
      }
    }
    at = end + 1;
  }
  return path;
}

std::string timing_stroke_string(const std::vector<float2> &path)
{
  std::string text;
  char buf[64];
  for (const float2 p : path) {
    std::snprintf(buf, sizeof(buf), "%s%.4f,%.5f", text.empty() ? "" : ";", double(p.x), double(p.y));
    text += buf;
  }
  return text;
}

}  // namespace oanim
