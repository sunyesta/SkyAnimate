#include "anim/timing_curve.hh"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
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
  /* From the two keys inward: each span between kept frames split at the frame it misses most, until it misses none
   * by more than the tolerance (each kept frame's slope is the data's, so a span's curve is those slopes' Hermite). */
  std::vector<char> keep(size_t(n + 1), 0);
  keep[0] = keep[size_t(n)] = 1;
  std::vector<std::pair<int, int>> spans{{0, n}};
  while (!spans.empty()) {
    const auto [a, b] = spans.back();
    spans.pop_back();
    const float len = float(b - a) * h;
    float worst = tolerance;
    int at = -1;
    for (int k = a + 1; k < b; k++) {
      const float s = float(k - a) / float(b - a), s2 = s * s, s3 = s2 * s;
      const float y = (2.0f * s3 - 3.0f * s2 + 1.0f) * u[size_t(a)] + (s3 - 2.0f * s2 + s) * len * m[size_t(a)] +
                      (-2.0f * s3 + 3.0f * s2) * u[size_t(b)] + (s3 - s2) * len * m[size_t(b)];
      const float err = std::abs(y - u[size_t(k)]);
      if (err > worst) {
        worst = err;
        at = k;
      }
    }
    if (at >= 0) {
      keep[size_t(at)] = 1;
      spans.emplace_back(a, at);
      spans.emplace_back(at, b);
    }
  }
  std::vector<int> knots;
  for (int k = 0; k <= n; k++) {
    if (keep[size_t(k)]) {
      knots.push_back(k);
    }
  }
  TimingCurve curve;
  for (size_t i = 0; i < knots.size(); i++) {
    const int k = knots[i];
    TimingPoint p;
    p.co = {float(k) * h, u[size_t(k)]};
    const float slope = m[size_t(k)];
    if (i > 0) {
      const float dx = float(k - knots[i - 1]) * h / 3.0f;
      p.in = {-dx, -slope * dx};
    }
    if (i + 1 < knots.size()) {
      const float dx = float(knots[i + 1] - k) * h / 3.0f;
      p.out = {dx, slope * dx};
    }
    p.aligned = true;
    curve.points.push_back(p);
  }
  curve.points.front().co = {0.0f, 0.0f};
  curve.points.back().co = {1.0f, 1.0f};
  return curve;
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

TimingCurve timing_sculpt(const TimingCurve &curve, int n, TimingBrush brush, const std::vector<float2> &path,
                          float radius, float strength, float aspect, bool lock_x)
{
  if (n < 2 || path.empty() || !(radius > 0.0f)) {
    return curve;
  }
  if (!std::isfinite(aspect) || aspect <= 0.0f) {
    aspect = 1.0f;
  }
  constexpr int PER = 4;
  const int m = n * PER;
  std::vector<float2> s(size_t(m + 1));
  for (int j = 0; j <= m; j++) {
    const float x = float(j) / float(m);
    s[size_t(j)] = {x * float(n), timing_progress(curve, x)};
  }
  /* Distances as the graph shows them: a whole way up is `aspect` frames across, so the brush is a circle there. */
  auto dist = [aspect](float2 a, float2 b) { return length(float2(a.x - b.x, (a.y - b.y) * aspect)); };
  strength = std::clamp(strength, 0.0f, 1.0f);
  if (brush == TimingBrush::Grab) {
    const float2 from = path.front(), delta = path.back() - path.front();
    const std::vector<float2> start = s;
    for (int j = 1; j < m; j++) {
      const float w = brush_falloff(dist(start[size_t(j)], from), radius) * strength;
      s[size_t(j)].y += delta.y * w;
      if (!lock_x) {
        s[size_t(j)].x += delta.x * w;
      }
    }
  }
  else {
    /* Dabs along the stroke, each pulling the samples under it toward their neighbours' average (within half the
     * brush, at least a frame each way). */
    /* At most so many dabs, spaced further apart on a stroke longer than that (and none on one too long to measure):
     * a huge stroke or brush could otherwise go on for ever. */
    constexpr float MOST_DABS = 2000.0f;
    float total = 0.0f;
    for (size_t p = 1; p < path.size(); p++) {
      total += dist(path[p - 1], path[p]);
    }
    if (!std::isfinite(total)) {
      return curve;
    }
    const float spacing = std::max({radius * 0.2f, 0.25f, total / MOST_DABS});
    const float reach = std::max(radius * 0.5f, 1.0f);
    /* Never wider than the whole stretch (a huge radius would overflow it). */
    const int span = int(std::min(std::ceil(reach * float(PER)), float(m)));
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
    for (const float2 c : dabs) {
      const std::vector<float2> cur = s;
      for (int j = 1; j < m; j++) {
        const float w = brush_falloff(dist(cur[size_t(j)], c), radius) * strength;
        if (w <= 0.0f) {
          continue;
        }
        float2 sum{0.0f, 0.0f};
        float wsum = 0.0f;
        for (int o = std::max(0, j - span); o <= std::min(m, j + span); o++) {
          const float t = 1.0f - std::abs(float(o - j)) / float(span + 1);
          sum += cur[size_t(o)] * t;
          wsum += t;
        }
        if (!(wsum > 0.0f)) {
          continue;
        }
        const float2 avg = sum / wsum;
        s[size_t(j)].y += (avg.y - cur[size_t(j)].y) * w;
        if (!lock_x) {
          s[size_t(j)].x += (avg.x - cur[size_t(j)].x) * w;
        }
      }
    }
  }
  /* Time only goes forward: each sample after the one before, the ends where they were. */
  s.front() = {0.0f, 0.0f};
  s.back() = {float(n), 1.0f};
  constexpr float gap = 1e-3f;
  for (int j = 1; j < m; j++) {
    s[size_t(j)].x = std::clamp(s[size_t(j)].x, s[size_t(j - 1)].x + gap, float(n) - float(m - j) * gap);
  }
  /* Each frame's progress where the moved samples cross it. */
  std::vector<float> u(size_t(n + 1));
  size_t j = 0;
  for (int k = 0; k <= n; k++) {
    const float x = float(k);
    while (j + 2 < s.size() && s[j + 1].x < x) {
      j++;
    }
    const float2 a = s[j], b = s[j + 1];
    const float t = b.x - a.x > 1e-9f ? std::clamp((x - a.x) / (b.x - a.x), 0.0f, 1.0f) : 0.0f;
    u[size_t(k)] = a.y + (b.y - a.y) * t;
    if (!std::isfinite(u[size_t(k)])) {
      return curve;
    }
  }
  return timing_fit(u);
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
