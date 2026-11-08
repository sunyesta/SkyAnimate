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
                                 size_t keep, std::vector<SpanFit> &r_fits, const std::vector<char> *can_go = nullptr)
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
  /* Only those `can_go` lets (all, without it) are taken out, or counted against `keep`. */
  const auto goes = [&](size_t i) { return !can_go || (*can_go)[i]; };
  size_t inside = 0;
  for (size_t i = 1; i + 1 < count; i++) {
    if (alive[i] && goes(i)) {
      cost[i] = fit(prev[i], next[i]);
      inside++;
    }
  }
  while (inside > keep) {
    size_t at = 0;
    for (size_t i = 1; i + 1 < count; i++) {
      if (alive[i] && goes(i) && (at == 0 || cost[i].miss < cost[at].miss)) {
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
    if (p > 0 && goes(p)) {
      cost[p] = fit(prev[p], q);
    }
    if (q + 1 < count && goes(q)) {
      cost[q] = fit(p, next[q]);
    }
  }
  std::vector<size_t> kept;
  r_fits.clear();
  for (size_t i = 0; i < count; i++) {
    if (alive[i]) {
      if (!kept.empty()) {
        /* Two neighbours neither of which could go: a piece as it was (left unfitted). */
        const bool as_was = kept.back() + 1 == i && !goes(kept.back()) && !goes(i);
        r_fits.push_back(as_was ? SpanFit{} : fit(kept.back(), i));
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
  /* Moved alone, the two are no longer in line: the point is broken from then on, as a motion path's control point
   * is, so the next plain drag doesn't snap the other back. */
  if (free && has_other) {
    p.aligned = false;
  }
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

/* The Edit mode's G R S and handle types work as the graph shows the curve: x in frames, y `aspect` frames a whole way
 * along, so a turn or a level handle is one on screen. */
namespace {

struct ViewScale {
  float x, y;
  float2 to(float2 h) const { return {h.x * x, h.y * y}; }
  float2 from(float2 v) const { return {v.x / x, v.y / y}; }
};

ViewScale view_scale(int n, float aspect)
{
  return {float(std::max(n, 1)), std::max(aspect, 1e-6f)};
}

/* Point `i`'s room in time on each side: to its neighbours. */
float gap_before(const TimingCurve &c, size_t i)
{
  return i > 0 ? std::max(c.points[i].co.x - c.points[i - 1].co.x, 0.0f) : 0.0f;
}
float gap_after(const TimingCurve &c, size_t i)
{
  return i + 1 < c.points.size() ? std::max(c.points[i + 1].co.x - c.points[i].co.x, 0.0f) : 0.0f;
}

/* Both of point `i`'s handles set from what the curve shows (`in`, `out`), each kept pointing its way and within its
 * segment; the aligned flag as it was. The two segments it touches are then stored as the curve shows them (a handle
 * and the neighbour's facing it shortened together where they'd reach past each other), so reading the handles back
 * gives what was put and pressing Aligned or Flat again changes nothing. */
void handles_put(TimingCurve &c, size_t i, float2 in, float2 out)
{
  TimingPoint &p = c.points[i];
  if (i > 0) {
    p.in = handle_within(in, false, gap_before(c, i));
  }
  if (i + 1 < c.points.size()) {
    p.out = handle_within(out, true, gap_after(c, i));
  }
  for (size_t k = i > 0 ? i - 1 : 0; k <= i && k + 1 < c.points.size(); k++) {
    const Segment sg = segment(c, k);
    c.points[k].out = sg.p1 - sg.p0;
    c.points[k + 1].in = sg.p2 - sg.p3;
  }
}

/* Point `i`'s handles in line along the way between theirs as the graph shows them (`vs`), each keeping its length; one
 * of no length takes the other's way, two pointing back on each other lie level. */
void handles_in_line(TimingCurve &c, size_t i, const ViewScale &vs)
{
  float2 in, out;
  timing_point_handles(c, i, in, out);
  const float2 vin = vs.to(in), vout = vs.to(out);
  const float lin = length(vin), lout = length(vout);
  float2 way{0.0f, 0.0f};
  if (lout > 1e-9f) {
    way += vout / lout;
  }
  if (lin > 1e-9f) {
    way -= vin / lin;
  }
  if (length(way) < 1e-9f) {
    way = {1.0f, 0.0f};
  }
  way = normalize(way);
  if (way.x < 0.0f) {
    way = -way;
  }
  handles_put(c, i, vs.from(-way * lin), vs.from(way * lout));
}

float2 turned(float2 v, float angle)
{
  const float cs = std::cos(angle), sn = std::sin(angle);
  return {v.x * cs - v.y * sn, v.x * sn + v.y * cs};
}

}  // namespace

void timing_transform(TimingCurve &curve, const std::vector<TimingPick> &picks, char kind, float2 offset, float angle,
                      float2 factor, int n, float aspect)
{
  if (curve.empty()) {
    return;
  }
  const size_t count = curve.points.size();
  const ViewScale vs = view_scale(n, aspect);
  std::vector<char> point(count, 0);
  std::vector<int> handles;
  for (const TimingPick &k : picks) {
    if (k.point < 0 || size_t(k.point) >= count) {
      continue;
    }
    if (k.part == 0) {
      point[size_t(k.point)] = 1;
    }
  }
  /* Handles picked on their own (not with their point), as point * 2 + (0 in, 1 out); a point with both picked moves
   * them as a pair (`both`), not one after the other (where the second would turn the first back into line). */
  for (const TimingPick &k : picks) {
    if (k.point >= 0 && size_t(k.point) < count && k.part != 0 && !point[size_t(k.point)]) {
      const size_t i = size_t(k.point);
      if ((k.part < 0 && i == 0) || (k.part > 0 && i + 1 == count)) {
        continue;
      }
      const int code = k.point * 2 + (k.part > 0 ? 1 : 0);
      if (std::find(handles.begin(), handles.end(), code) == handles.end()) {
        handles.push_back(code);
      }
    }
  }
  const auto shown = [&](const TimingCurve &c, size_t i, bool out) {
    float2 in, o;
    timing_point_handles(c, i, in, o);
    return out ? o : in;
  };
  const TimingCurve was = curve;
  std::vector<char> both(count, 0);
  for (const int code : handles) {
    if (code % 2 == 0 && std::find(handles.begin(), handles.end(), code + 1) != handles.end()) {
      both[size_t(code / 2)] = 1;
    }
  }
  std::erase_if(handles, [&](int code) { return both[size_t(code / 2)] != 0; });
  /* Each pair's handles from what the curve showed (`f` gives a handle's new value); an aligned point's put back in line
   * along the way between them. */
  const auto pairs_set = [&](auto f) {
    for (size_t i = 0; i < count; i++) {
      if (both[i]) {
        handles_put(curve, i, f(shown(was, i, false)), f(shown(was, i, true)));
        if (was.points[i].aligned) {
          handles_in_line(curve, i, vs);
        }
      }
    }
  };
  size_t points = 0;
  for (size_t i = 0; i < count; i++) {
    points += point[i] ? 1 : 0;
  }

  if (kind == 'G') {
    /* The points picked go together, as far as the first not picked on each side lets them (the ends never move). */
    float dx = offset.x;
    for (size_t i = 1; i + 1 < count; i++) {
      if (!point[i]) {
        continue;
      }
      size_t next = i + 1, prev = i - 1;
      while (next + 1 < count && point[next]) {
        next++;
      }
      while (prev > 0 && point[prev]) {
        prev--;
      }
      dx = std::min(dx, was.points[next].co.x - GAP - was.points[i].co.x);
      dx = std::max(dx, was.points[prev].co.x + GAP - was.points[i].co.x);
    }
    for (size_t i = 1; i + 1 < count; i++) {
      if (point[i]) {
        curve.points[i].co = was.points[i].co + float2(dx, offset.y);
      }
    }
    pairs_set([&](float2 h) { return h + offset; });
    for (const int code : handles) {
      const size_t i = size_t(code / 2);
      const bool out = code % 2 == 1;
      timing_handle_set(curve, i, out, shown(was, i, out) + offset, false);
    }
    return;
  }

  if (kind == 'R') {
    /* Each picked point's handles about it; handles picked alone about their points. As shown, so a turn looks right. */
    for (size_t i = 0; i < count; i++) {
      if (point[i]) {
        float2 in, out;
        timing_point_handles(was, i, in, out);
        handles_put(curve, i, vs.from(turned(vs.to(in), angle)), vs.from(turned(vs.to(out), angle)));
      }
    }
    pairs_set([&](float2 h) { return vs.from(turned(vs.to(h), angle)); });
    for (const int code : handles) {
      const size_t i = size_t(code / 2);
      const bool out = code % 2 == 1;
      timing_handle_set(curve, i, out, vs.from(turned(vs.to(shown(was, i, out)), angle)), false);
    }
    return;
  }

  /* S. */
  factor = {std::max(factor.x, 0.0f), std::max(factor.y, 0.0f)};
  if (points >= 2) {
    /* Their places about their middle, in order between the points not picked (which never move); their handles with
     * them. */
    float2 mid{0.0f, 0.0f};
    for (size_t i = 0; i < count; i++) {
      if (point[i]) {
        mid += was.points[i].co;
      }
    }
    mid *= 1.0f / float(points);
    for (size_t i = 1; i + 1 < count; i++) {
      if (point[i]) {
        const float2 d = was.points[i].co - mid;
        curve.points[i].co = mid + float2(d.x * factor.x, d.y * factor.y);
      }
    }
    for (size_t i = 1; i + 1 < count; i++) {
      if (point[i]) {
        curve.points[i].co.x = std::max(curve.points[i].co.x, curve.points[i - 1].co.x + GAP);
      }
    }
    for (size_t i = count - 1; i-- > 1;) {
      if (point[i]) {
        curve.points[i].co.x = std::min(curve.points[i].co.x, curve.points[i + 1].co.x - GAP);
      }
    }
  }
  for (size_t i = 0; i < count; i++) {
    if (point[i]) {
      float2 in, out;
      timing_point_handles(was, i, in, out);
      handles_put(curve, i, {in.x * factor.x, in.y * factor.y}, {out.x * factor.x, out.y * factor.y});
    }
  }
  pairs_set([&](float2 h) { return float2{h.x * factor.x, h.y * factor.y}; });
  for (const int code : handles) {
    const size_t i = size_t(code / 2);
    const bool out = code % 2 == 1;
    const float2 h = shown(was, i, out);
    timing_handle_set(curve, i, out, {h.x * factor.x, h.y * factor.y}, false);
  }
}

void timing_handles_type(TimingCurve &curve, const std::vector<int> &points, TimingHandles type, int n, float aspect)
{
  if (curve.empty()) {
    return;
  }
  const size_t count = curve.points.size();
  const ViewScale vs = view_scale(n, aspect);
  for (const int pi : points) {
    if (pi < 0 || size_t(pi) >= count) {
      continue;
    }
    const size_t i = size_t(pi);
    TimingPoint &p = curve.points[i];
    if (type == TimingHandles::Broken) {
      p.aligned = false;
      continue;
    }
    float2 in, out;
    timing_point_handles(curve, i, in, out);
    const float2 vin = vs.to(in), vout = vs.to(out);
    const float lin = length(vin), lout = length(vout);
    if (type == TimingHandles::Flat) {
      /* Level, each as long as it was on screen: the move eases into and out of the point. */
      handles_put(curve, i, vs.from({-lin, 0.0f}), vs.from({lout, 0.0f}));
      continue;
    }
    p.aligned = true;
    if (i == 0 || i + 1 == count) {
      continue;
    }
    handles_in_line(curve, i, vs);
  }
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

/* A sculpt stroke's samples of the curve, in the graph's terms (x in frames from the stretch's start, y the way along):
 * a quarter frame apart and one on each of the curve's points (`anchor`: each point's sample), where each was when the
 * stroke started (`at`) and where the stroke takes it (`moved`). As a drawing's sculpt does, the brushes move the
 * samples and the curve is fitted back to them; every update of a stroke starts again from the curve it started on, so
 * the samples last the whole stroke and the fitting never builds on its own error. */
struct StrokeSamples {
  std::vector<float2> at, moved;
  /* Each sample on a whole frame (where Dyntopo may put a point). */
  std::vector<char> frame;
  std::vector<size_t> anchor;
};

static StrokeSamples stroke_samples(const TimingCurve &c, int n)
{
  constexpr int PER = 4;
  const int m = n * PER;
  const float fn = float(n);
  StrokeSamples ss;
  int j = 0;
  for (size_t i = 0; i < c.points.size(); i++) {
    const float x = c.points[i].co.x;
    for (; j <= m && float(j) / float(m) < x - 1e-6f; j++) {
      const float sx = float(j) / float(m);
      ss.at.push_back({sx * fn, timing_progress(c, sx)});
      ss.frame.push_back(j % PER == 0);
    }
    while (j <= m && float(j) / float(m) <= x + 1e-6f) {
      j++;
    }
    ss.anchor.push_back(ss.at.size());
    ss.at.push_back({x * fn, c.points[i].co.y});
    ss.frame.push_back(0);
  }
  ss.moved = ss.at;
  return ss;
}

/* How far along a squeeze that reaches `u` (0 to 1) of its way has gone: the falloff of time ahead of a Grab, as a
 * share of the move. Its rate is a plateau with rounded shoulders, each `e` (at most half) of the way: at a half, the
 * smoothstep (rate 6u(1 - u), at most 1.5); narrower shoulders squeeze more evenly (at most 1 / (1 - 2e / 3)), so a
 * drag nearly to the key still needs no corner. */
static float squeeze_gone(float u, float e)
{
  u = std::clamp(u, 0.0f, 1.0f);
  const float h = 1.0f / (1.0f - 2.0f * e / 3.0f);
  const auto rise = [&](float v) {
    const float t = v / e;
    return h * e * (t * t - t * t * t / 3.0f);
  };
  if (u > 0.5f) {
    return 1.0f - squeeze_gone(1.0f - u, e);
  }
  return u <= e ? rise(u) : h * (2.0f * e / 3.0f + (u - e));
}

/* The brush's move of the samples. Grab, as Blender's and Maya's: each sample is weighed by how near it was to where the
 * brush was pressed (the circle, as the graph shows it) and goes up and down by the stroke that much. In time (without
 * Lock X) the curve where the brush was nearest goes with the mouse; behind it time stretches within the circle, and
 * ahead it squeezes within the circle while that can hold the move, reaching further ahead (never behind, never past
 * the key) only as the move needs, smoothly, so time never runs backward and the curve has no corners (a reach both
 * ways that grew with the move and a straight falloff moved what the circle wasn't on and left kinks). Smooth: dabs
 * along the stroke, each pulling the samples under it up or down toward their neighbours' average (within half the
 * brush, at least a frame each way, and near a key only as far on each side as the key is: an average of the
 * neighbours on one side alone dragged the samples away from the key, which held, so the curve shot to it). */
static void stroke_move(StrokeSamples &ss, const TimingCurve & /*c*/, int n, TimingBrush brush,
                        const std::vector<float2> &path, float radius, float strength, float aspect, bool lock_x)
{
  /* Distances as the graph shows them: a whole way up is `aspect` frames across, so the brush is a circle there. */
  const auto dist = [aspect](float2 a, float2 b) { return length(float2(a.x - b.x, (a.y - b.y) * aspect)); };
  const size_t count = ss.at.size();
  std::vector<float2> &s = ss.moved;
  if (brush == TimingBrush::Grab) {
    const float2 from = path.front(), delta = path.back() - path.front();
    std::vector<float> w(count, 0.0f);
    size_t c = 0;
    for (size_t j = 1; j + 1 < count; j++) {
      w[j] = brush_falloff(dist(ss.at[j], from), radius) * strength;
      s[j].y += delta.y * w[j];
      if (w[j] > w[c]) {
        c = j;
      }
    }
    const float d = std::abs(delta.x);
    if (lock_x || c == 0 || !(d > 0.0f)) {
      return;
    }
    /* Each side of the nearest sample: how far the circle reaches along the curve (to the first sample it misses),
     * never past the key. */
    const float xc = ss.at[c].x, fn = float(n);
    size_t lo = c, hi = c;
    while (lo > 0 && w[lo] > 0.0f) {
      lo--;
    }
    while (hi + 1 < count && w[hi] > 0.0f) {
      hi++;
    }
    const float forward = delta.x > 0.0f ? 1.0f : -1.0f;
    const float reach_up = std::clamp(ss.at[hi].x - xc, 0.25f, std::max(fn - xc, 0.25f));
    const float reach_down = std::clamp(xc - ss.at[lo].x, 0.25f, std::max(xc, 0.25f));
    const float behind = forward > 0.0f ? reach_down : reach_up;
    /* Ahead: the circle, or as far as squeezes time to at most 2.5 times as fast with the smoothstep, up to the key.
     * Where the key cuts that short, the shoulders narrow so the squeeze stays as even as it can. */
    constexpr float MOST_SQUEEZE = 0.6f; /* the most a frame ahead may shrink to (1 - 1 / 2.5) */
    const float move = d * w[c];
    const float room = forward > 0.0f ? fn - xc : xc;
    const float ahead = std::min(std::max(forward > 0.0f ? reach_up : reach_down, move * 1.5f / MOST_SQUEEZE), room);
    const float q = ahead > 0.0f ? move / ahead : 1.0f;
    const float e = std::clamp(1.5f * (1.0f - q / MOST_SQUEEZE), 0.05f, 0.5f);
    for (size_t j = 1; j + 1 < count; j++) {
      const float off = (ss.at[j].x - xc) * forward;
      const float left = off >= 0.0f ? 1.0f - squeeze_gone(off / ahead, e) : 1.0f - squeeze_gone(-off / behind, 0.5f);
      s[j].x += delta.x * w[c] * left;
    }
    /* Time only goes forward (a drag right to the key): each after the one before, the ends where they were. */
    const auto gap = [&](size_t j) { return std::min(1e-3f, ss.at[j].x - ss.at[j - 1].x); };
    for (size_t j = 1; j + 1 < count; j++) {
      s[j].x = std::max(s[j].x, s[j - 1].x + gap(j));
    }
    for (size_t j = count - 1; j-- > 1;) {
      s[j].x = std::min(s[j].x, s[j + 1].x - gap(j + 1));
    }
    return;
  }
  const float reach = std::max(radius * 0.5f, 1.0f);
  for (const float2 dab : stroke_dabs(path, radius, dist)) {
    const std::vector<float2> cur = s;
    for (size_t j = 1; j + 1 < count; j++) {
      const float w = brush_falloff(dist(cur[j], dab), radius) * strength;
      if (w <= 0.0f) {
        continue;
      }
      const float r = std::min({reach, cur[j].x, float(n) - cur[j].x});
      float sum = 0.0f, wsum = 0.0f;
      const auto add = [&](size_t o) {
        const float t = 1.0f - std::abs(cur[o].x - cur[j].x) / (r + 0.25f);
        sum += cur[o].y * t;
        wsum += t;
      };
      for (size_t o = j; o-- > 0 && cur[j].x - cur[o].x <= r;) {
        add(o);
      }
      for (size_t o = j; o < count && cur[o].x - cur[j].x <= r; o++) {
        add(o);
      }
      if (wsum > 0.0f) {
        s[j].y += (sum / wsum - cur[j].y) * w;
      }
    }
  }
}

/* Which of the curve's pieces (point i to i + 1) the stroke moved: any of their samples. */
static std::vector<char> pieces_touched(const StrokeSamples &ss)
{
  std::vector<char> touched(ss.anchor.size() - 1, 0);
  for (size_t p = 0; p < touched.size(); p++) {
    for (size_t j = ss.anchor[p]; j <= ss.anchor[p + 1] && !touched[p]; j++) {
      touched[p] = std::abs(ss.moved[j].x - ss.at[j].x) > 1e-6f || std::abs(ss.moved[j].y - ss.at[j].y) > 1e-6f;
    }
  }
  return touched;
}

/* The samples where the stroke took them, in the curve's terms (time 0-1). */
static std::vector<float2> samples_moved(const StrokeSamples &ss, int n)
{
  std::vector<float2> out(ss.moved.size());
  for (size_t j = 0; j < out.size(); j++) {
    out[j] = {ss.moved[j].x / float(n), ss.moved[j].y};
  }
  return out;
}

/* The slopes point `i`'s handles go at as the curve uses them; a handle of no length goes the way the curve does. */
static void point_slopes(const TimingCurve &c, size_t i, float &r_in, float &r_out)
{
  float2 in, out;
  timing_point_handles(c, i, in, out);
  const float x = c.points[i].co.x;
  const auto slope = [&](float2 h, bool left) {
    if (std::abs(h.x) > 1e-6f) {
      return h.y / h.x;
    }
    const float a = left ? std::max(x - 1e-3f, 0.0f) : x, b = left ? x : std::min(x + 1e-3f, 1.0f);
    return b > a ? (timing_progress(c, b) - timing_progress(c, a)) / (b - a) : 0.0f;
  };
  r_in = slope(in, true);
  r_out = slope(out, false);
}

/* Least squares by Levenberg and Marquardt's steps: moves the few numbers `p` (at most four) to make `residuals`
 * small, each try put back within bounds by `within`; `step` is how far each is nudged to see how the residuals change
 * (0 keeps it as it is). */
template<typename Residuals, typename Within>
static void least_squares(std::vector<double> &p, const std::vector<double> &step, Residuals residuals, Within within,
                          int iterations)
{
  const size_t k = p.size();
  std::vector<double> r, tried;
  std::vector<std::vector<double>> jac(k);
  double sum = residuals(p, r);
  double damp = 1e-3;
  for (int it = 0; it < iterations && sum > 1e-14; it++) {
    for (size_t a = 0; a < k; a++) {
      jac[a].assign(r.size(), 0.0);
      if (step[a] == 0.0) {
        continue;
      }
      std::vector<double> q = p;
      q[a] += step[a];
      within(q);
      const double d = q[a] - p[a];
      if (d == 0.0) {
        q[a] = p[a] - step[a];
        within(q);
      }
      const double dd = q[a] - p[a];
      if (dd == 0.0) {
        continue;
      }
      residuals(q, jac[a]);
      for (size_t i = 0; i < r.size(); i++) {
        jac[a][i] = (jac[a][i] - r[i]) / dd;
      }
    }
    double A[4][4] = {}, g[4] = {};
    for (size_t a = 0; a < k; a++) {
      for (size_t i = 0; i < r.size(); i++) {
        g[a] += jac[a][i] * r[i];
      }
      for (size_t b = 0; b < k; b++) {
        for (size_t i = 0; i < r.size(); i++) {
          A[a][b] += jac[a][i] * jac[b][i];
        }
      }
    }
    bool moved = false;
    while (damp < 1e8) {
      /* (A + damp diag A) x = g, by elimination. */
      double M[4][5];
      for (size_t a = 0; a < k; a++) {
        for (size_t b = 0; b < k; b++) {
          M[a][b] = A[a][b] + (a == b ? A[a][a] * damp + 1e-18 : 0.0);
        }
        M[a][k] = g[a];
      }
      bool solved = true;
      for (size_t c = 0; c < k && solved; c++) {
        size_t piv = c;
        for (size_t a = c + 1; a < k; a++) {
          if (std::abs(M[a][c]) > std::abs(M[piv][c])) {
            piv = a;
          }
        }
        if (!(std::abs(M[piv][c]) > 0.0)) {
          solved = false;
          break;
        }
        for (size_t b = 0; b <= k; b++) {
          std::swap(M[c][b], M[piv][b]);
        }
        for (size_t a = 0; a < k; a++) {
          if (a != c) {
            const double f = M[a][c] / M[c][c];
            for (size_t b = c; b <= k; b++) {
              M[a][b] -= f * M[c][b];
            }
          }
        }
      }
      std::vector<double> q = p;
      for (size_t a = 0; a < k && solved; a++) {
        q[a] -= M[a][k] / M[a][a];
        solved = std::isfinite(q[a]);
      }
      if (!solved) {
        damp *= 10.0;
        continue;
      }
      within(q);
      const double now = residuals(q, tried);
      if (now < sum) {
        p = q;
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
}

/* A piece's handles: `alpha` and `beta` long in time, going at `slope_a` (out of its first point) and `slope_b` (into
 * its last). */
struct PieceFit {
  float alpha = 0.0f, beta = 0.0f, slope_a = 0.0f, slope_b = 0.0f;
};

/* The piece from `a` to `b` fitted to the samples between (`from` to `to`, both left out; the curve's terms), from
 * `start`: its handles' lengths, and the slopes of those `free_a` and `free_b` (else kept). Kept within the piece in
 * time (each handle at most across it, the two together too, so time only goes forward). */
static PieceFit piece_fit(float2 a, float2 b, PieceFit start, bool free_a, bool free_b,
                          const std::vector<float2> &samples, size_t from, size_t to)
{
  constexpr double SLOPE_MOST = 100.0;
  const float len = b.x - a.x;
  /* Samples that only go one way (`dir`, 0 where they turn back) keep the piece going that way: its handles' heights
   * never past each other, so its dots never run backwards where the stroke didn't ask for it. The free slopes give way
   * first, then the lengths. */
  int dir = 0;
  {
    bool up = b.y >= a.y - 1e-6f, down = b.y <= a.y + 1e-6f;
    float prev = a.y;
    for (size_t i = from + 1; i < to && i < samples.size(); i++) {
      up &= samples[i].y >= prev - 1e-6f;
      down &= samples[i].y <= prev + 1e-6f;
      prev = samples[i].y;
    }
    up &= b.y >= prev - 1e-6f;
    down &= b.y <= prev + 1e-6f;
    dir = up && !down ? 1 : down && !up ? -1 : 0;
  }
  const double rise = double(dir) * double(b.y - a.y);
  const auto within = [len, SLOPE_MOST, dir, rise, free_a, free_b](std::vector<double> &p) {
    p[0] = std::clamp(p[0], double(len) * 0.02, double(len));
    p[1] = std::clamp(p[1], double(len) * 0.02, double(len));
    if (p[0] + p[1] > double(len)) {
      const double k = double(len) / (p[0] + p[1]);
      p[0] *= k;
      p[1] *= k;
    }
    p[2] = std::clamp(p[2], -SLOPE_MOST, SLOPE_MOST);
    p[3] = std::clamp(p[3], -SLOPE_MOST, SLOPE_MOST);
    if (dir == 0) {
      return;
    }
    if (free_a) {
      p[2] = dir > 0 ? std::max(p[2], 0.0) : std::min(p[2], 0.0);
    }
    if (free_b) {
      p[3] = dir > 0 ? std::max(p[3], 0.0) : std::min(p[3], 0.0);
    }
    const double kept = dir * ((free_a ? 0.0 : p[2] * p[0]) + (free_b ? 0.0 : p[3] * p[1]));
    const double loose = dir * ((free_a ? p[2] * p[0] : 0.0) + (free_b ? p[3] * p[1] : 0.0));
    if (kept < 0.0 || (!free_a && dir * p[2] < 0.0) || (!free_b && dir * p[3] < 0.0)) {
      return; /* A kept slope already turns back: nothing to hold to. */
    }
    if (kept + loose > rise) {
      if (kept <= rise) {
        const double k = loose > 0.0 ? (rise - kept) / loose : 0.0;
        p[2] *= free_a ? k : 1.0;
        p[3] *= free_b ? k : 1.0;
      }
      else {
        p[2] *= free_a ? 0.0 : 1.0;
        p[3] *= free_b ? 0.0 : 1.0;
        const double k = rise / kept;
        p[0] *= k;
        p[1] *= k;
      }
    }
  };
  std::vector<double> p{start.alpha, start.beta, start.slope_a, start.slope_b};
  if (!(len > 1e-7f)) {
    return start;
  }
  within(p);
  if (to > from + 1) {
    const auto residuals = [&](const std::vector<double> &q, std::vector<double> &r) {
      const Segment s = span_segment(a, float(q[2]), b, float(q[3]), float(q[0]), float(q[1]));
      r.resize(to - from - 1);
      double sum = 0.0;
      for (size_t i = from + 1; i < to; i++) {
        const double d = double(bezier(s, segment_param(s, samples[i].x)).y - samples[i].y);
        r[i - from - 1] = d;
        sum += d * d;
      }
      return sum;
    };
    const double h = double(len) * 1e-3;
    least_squares(p,
                  {h, h, free_a ? 1e-3 * std::max(1.0, std::abs(p[2])) : 0.0,
                   free_b ? 1e-3 * std::max(1.0, std::abs(p[3])) : 0.0},
                  residuals, within, 16);
  }
  return {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
}

/* An aligned point fitted on one side only: its other handle (`kept_in`: the out one) turned exactly opposite the
 * one that stays, keeping its length. */
static void handle_opposite_kept(TimingPoint &p, bool kept_in)
{
  const float2 kept = kept_in ? p.in : p.out;
  float2 &fitted = kept_in ? p.out : p.in;
  const float lk = length(kept), lf = length(fitted);
  if (lk > 1e-9f && lf > 1e-9f) {
    fitted = -kept * (lf / lk);
  }
}

/* Without Dyntopo, as a drawing's sculpt does: each point goes where its sample went, the handles of each piece the
 * stroke moved are fitted to its samples, slopes too (a smooth point's two kept in line: along the way of both where
 * both were fitted, else the one on the piece not moved turned opposite the fitted one, keeping its length, so that
 * piece bends a little, as a drawing's does, and a piece between two smooth points still follows the brush), and the
 * pieces it didn't move are otherwise as they were. None added or taken out. */
static TimingCurve sculpt_fit_points(const TimingCurve &c, int n, const StrokeSamples &ss)
{
  const size_t count = c.points.size();
  const std::vector<char> touched = pieces_touched(ss);
  if (std::find(touched.begin(), touched.end(), 1) == touched.end()) {
    return c;
  }
  const std::vector<float2> S = samples_moved(ss, n);
  TimingCurve out = c;
  for (size_t i = 1; i + 1 < count; i++) {
    out.points[i].co = S[ss.anchor[i]];
    out.points[i].co.x = std::clamp(out.points[i].co.x, out.points[i - 1].co.x + GAP, 1.0f - float(count - 1 - i) * GAP);
  }
  std::vector<float> was_in(count), was_out(count);
  std::vector<float2> h_in(count), h_out(count);
  for (size_t i = 0; i < count; i++) {
    point_slopes(c, i, was_in[i], was_out[i]);
    timing_point_handles(c, i, h_in[i], h_out[i]);
  }
  const auto smooth = [&](size_t i) { return i > 0 && i + 1 < count && c.points[i].aligned; };
  std::vector<PieceFit> fit(count - 1);
  const auto fit_piece = [&](size_t p, PieceFit start, bool free_a, bool free_b) {
    fit[p] = piece_fit(out.points[p].co, out.points[p + 1].co, start, free_a, free_b, S, ss.anchor[p],
                       ss.anchor[p + 1]);
  };
  for (size_t p = 0; p + 1 < count; p++) {
    if (!touched[p]) {
      continue;
    }
    const size_t a = p, b = p + 1;
    /* Every slope fitted, a smooth point's too: its handle on the side the stroke left turns to stay in line. */
    PieceFit start;
    start.alpha = std::max(h_out[a].x, 0.0f);
    start.beta = std::max(-h_in[b].x, 0.0f);
    start.slope_a = was_out[a];
    start.slope_b = was_in[b];
    fit_piece(p, start, true, true);
  }
  /* A smooth point fitted on both sides: its handles along the way between the two, each piece's lengths fitted to it. */
  std::vector<char> again(count - 1, 0);
  for (size_t i = 1; i + 1 < count; i++) {
    if (smooth(i) && touched[i - 1] && touched[i]) {
      const float way = std::tan(0.5f * (std::atan(fit[i - 1].slope_b) + std::atan(fit[i].slope_a)));
      fit[i - 1].slope_b = fit[i].slope_a = way;
      again[i - 1] = again[i] = 1;
    }
  }
  for (size_t p = 0; p + 1 < count; p++) {
    if (again[p]) {
      fit_piece(p, fit[p], false, false);
    }
  }
  for (size_t p = 0; p + 1 < count; p++) {
    if (touched[p]) {
      out.points[p].out = {fit[p].alpha, fit[p].slope_a * fit[p].alpha};
      out.points[p + 1].in = {-fit[p].beta, -fit[p].slope_b * fit[p].beta};
    }
  }
  for (size_t i = 1; i + 1 < count; i++) {
    if (smooth(i) && touched[i - 1] != touched[i]) {
      handle_opposite_kept(out.points[i], touched[i - 1] != 0);
    }
  }
  return timing_valid(out) ? out : c;
}

/* The slope of samples `S` (the curve's terms) around sample `j`, over at least half a frame (`side` < 0: before it
 * only, > 0: after). */
static float samples_slope(const std::vector<float2> &S, size_t j, int n, int side)
{
  const float h = 0.5f / float(n);
  size_t lo = j, hi = j;
  while (side <= 0 && lo > 0 && S[j].x - S[lo].x < h) {
    lo--;
  }
  while (side >= 0 && hi + 1 < S.size() && S[hi].x - S[j].x < h) {
    hi++;
  }
  return S[hi].x > S[lo].x ? (S[hi].y - S[lo].y) / (S[hi].x - S[lo].x) : 0.0f;
}

/* With Dyntopo, as a drawing's does: where the stroke moved the curve it has a point every frame as well as its own,
 * then those it made or moved that the curve keeps its shape without (TIMING_SCULPT_TOLERANCE) go, the cheapest first,
 * each piece's handles fitted to its samples; the points and pieces it didn't move stay exactly as they were. */
static TimingCurve sculpt_fit_dyntopo(const TimingCurve &c, int n, const StrokeSamples &ss)
{
  const size_t count = c.points.size();
  const std::vector<char> touched = pieces_touched(ss);
  if (std::find(touched.begin(), touched.end(), 1) == touched.end()) {
    return c;
  }
  const std::vector<float2> S = samples_moved(ss, n);
  /* What the fit holds to: the moved curve at each frame (as a dot shows it) and at each of its points, which the knots
   * sit on (`on`: each point's). Every frame inside a piece the stroke moved is a knot too. */
  std::vector<float2> F;
  std::vector<size_t> on(count);
  std::vector<int> frame_piece, frame_of;
  /* The moved curve at each frame alone, for the slopes there (as timing_fit takes them). */
  std::vector<float> u(size_t(n + 1));
  size_t j = 0, next = 0;
  for (int k = 0; k <= n || next < count;) {
    const float fx = float(k) / float(n);
    const bool point_first = next < count && (k > n || S[ss.anchor[next]].x <= fx + 1e-6f);
    if (point_first) {
      const float2 p = S[ss.anchor[next]];
      if (k <= n && std::abs(p.x - fx) <= 1e-6f) {
        k++;
      }
      on[next] = F.size();
      F.push_back(p);
      frame_piece.push_back(-1);
      frame_of.push_back(-1);
      next++;
      continue;
    }
    while (j + 2 < S.size() && S[j + 1].x < fx) {
      j++;
    }
    const float2 a = S[j], b = S[j + 1];
    const float t = b.x - a.x > 1e-9f ? std::clamp((fx - a.x) / (b.x - a.x), 0.0f, 1.0f) : 0.0f;
    F.push_back({fx, a.y + (b.y - a.y) * t});
    u[size_t(k)] = F.back().y;
    frame_of.push_back(k);
    /* The piece it falls in: between the last point put and the next. */
    frame_piece.push_back(next > 0 && next < count && touched[next - 1] ? int(next - 1) : -1);
    k++;
  }
  for (size_t i = 0; i < count; i++) {
    /* A point on a frame: the frame's progress is the point's. */
    const float x = F[on[i]].x * float(n);
    if (std::abs(x - std::round(x)) <= 1e-6f * float(n)) {
      u[size_t(std::lround(x))] = F[on[i]].y;
    }
  }
  const std::vector<float> m = frame_slopes(u);
  std::vector<Knot> knots;
  /* Each knot's point of the curve (-1: one the stroke made), and whether it may go. */
  std::vector<int> of_point;
  std::vector<char> can_go;
  for (size_t f = 0, i = 0; f < F.size(); f++) {
    if (i < count && on[i] == f) {
      const bool left = i > 0 && touched[i - 1], right = i + 1 < count && touched[i];
      float was_in, was_out;
      point_slopes(c, i, was_in, was_out);
      Knot k{f, was_in, was_out};
      if (c.points[i].aligned && i > 0 && i + 1 < count && (left || right)) {
        k.slope_in = k.slope_out = !left ? was_in : !right ? was_out : samples_slope(F, f, n, 0);
      }
      else {
        k.slope_in = left ? samples_slope(F, f, n, -1) : was_in;
        k.slope_out = right ? samples_slope(F, f, n, 1) : was_out;
      }
      knots.push_back(k);
      of_point.push_back(int(i));
      can_go.push_back(i > 0 && i + 1 < count && (left || right));
      i++;
    }
    else if (frame_piece[f] >= 0) {
      const float slope = m[size_t(frame_of[f])];
      knots.push_back({f, slope, slope});
      of_point.push_back(-1);
      can_go.push_back(1);
    }
  }
  std::vector<SpanFit> fits;
  const std::vector<size_t> kept = knots_reduce(knots, F, TIMING_SCULPT_TOLERANCE, 0, fits, &can_go);
  TimingCurve out;
  for (size_t q = 0; q < kept.size(); q++) {
    const Knot &k = knots[kept[q]];
    const int point = of_point[kept[q]];
    TimingPoint p;
    if (point >= 0) {
      p = c.points[size_t(point)];
    }
    else {
      p.aligned = true;
    }
    p.co = F[k.sample];
    out.points.push_back(p);
  }
  for (size_t q = 0; q + 1 < kept.size(); q++) {
    const int a = of_point[kept[q]], b = of_point[kept[q + 1]];
    /* A piece the stroke didn't move, between the same two points: as it was. */
    const bool as_was = a >= 0 && b == a + 1 && !touched[size_t(a)];
    if (as_was) {
      continue;
    }
    out.points[q].out = {fits[q].alpha, knots[kept[q]].slope_out * fits[q].alpha};
    out.points[q + 1].in = {-fits[q].beta, -knots[kept[q + 1]].slope_in * fits[q].beta};
  }
  for (size_t q = 1; q + 1 < kept.size(); q++) {
    const int a = of_point[kept[q]];
    if (a < 0 || !out.points[q].aligned) {
      continue;
    }
    const bool left_was = of_point[kept[q - 1]] == a - 1 && !touched[size_t(a - 1)];
    const bool right_was = of_point[kept[q + 1]] == a + 1 && !touched[size_t(a)];
    if (left_was != right_was) {
      handle_opposite_kept(out.points[q], left_was);
    }
  }
  out.points.front().co = {0.0f, 0.0f};
  out.points.back().co = {1.0f, 1.0f};
  return timing_valid(out) ? out : c;
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
  const TimingCurve c = curve.empty() ? timing_even() : curve;
  StrokeSamples ss = stroke_samples(c, n);
  stroke_move(ss, c, n, brush, path, radius, strength, aspect, lock_x);
  for (const float2 v : ss.moved) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y)) {
      return curve;
    }
  }
  return dyntopo ? sculpt_fit_dyntopo(c, n, ss) : sculpt_fit_points(c, n, ss);
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
