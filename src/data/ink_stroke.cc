#include "data/ink_stroke.hh"

#include <algorithm>
#include <cmath>

#include "data/fill_area.hh"

namespace oanim {

/* The spring's clock: it moves in steps this long (seconds) however seldom samples come, so a mouse's line
 * is as smooth as a tablet's. */
static constexpr float STEP = 1.0f / 240.0f;
/* At most this many steps between two samples: a pen resting has settled long before. */
static constexpr int MAX_STEPS = 40;
/* The most steps a line takes to reach the pen at its end, and how tightly it is drawn there. */
static constexpr int CATCH_UP_STEPS = 32;
static constexpr float CATCH_UP_OMEGA = 300.0f;
/* Places closer than this (screen pixels) make one point of the line. */
static constexpr float MIN_SPACING = 0.35f;
/* How long (seconds) the samples are kept for: the pen's speed, and its lift. */
static constexpr double RECENT_SECONDS = 0.16;

static float smooth_range(float low, float high, float v)
{
  const float t = std::clamp((v - low) / (high - low), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/* How tightly Smart's spring holds to the pen (radians a second) at a speed (screen pixels a second): loose when
 * slow, a few Hz, under the 8-12 Hz of a hand's tremor, and at speed tight enough to only take out the steps
 * between samples. A spring only ever heads for the pen, so when the pen stops dead the line comes to rest
 * there, where blending toward an average of where the pen has been would send it backward. */
static float smart_omega(const InkSettings &s, float speed)
{
  const float slow = 2.0f * PI * 12.0f * std::pow(1.0f / 6.0f, s.smoothing);
  const float fast = 2.0f * PI * (60.0f - 40.0f * s.smoothing);
  return slow * std::pow(fast / slow, smooth_range(150.0f, 1500.0f, speed));
}

static bool smart(const InkSettings &s)
{
  return s.stabilizer == InkStabilizer::Smart && s.smoothing > 0.0f;
}

void InkStroke::Spring::step(float2 target, float dt, float omega)
{
  /* The spring's exact solution for a target held still over the step, so no step is too long for it. */
  const float2 off = pos - target;
  const float2 temp = (vel + off * omega) * dt;
  const float e = std::exp(-omega * dt);
  vel = (vel - temp * omega) * e;
  pos = target + (off + temp) * e;
}

/* How a pressure of 0-1 maps to size: y where the curve's x is `pressure`. */
static float pressure_curve(const InkSettings &s, float pressure)
{
  const float2 a = s.pressure_curve_a, b = s.pressure_curve_b;
  const auto at = [](float p1, float p2, float t) {
    const float u = 1.0f - t;
    return 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t;
  };
  float lo = 0.0f, hi = 1.0f;
  for (int i = 0; i < 12; i++) {
    const float mid = 0.5f * (lo + hi);
    (at(a.x, b.x, mid) < pressure ? lo : hi) = mid;
  }
  return std::clamp(at(a.y, b.y, 0.5f * (lo + hi)), 0.0f, 1.0f);
}

void InkStroke::begin(const InkSettings &settings, const InkView &view)
{
  *this = {};
  settings_ = settings;
  view_ = view;
}

float2 InkStroke::last_sample() const
{
  return recent_.empty() ? float2() : recent_.back().pos * view_.pixel;
}

float InkStroke::radius(float pressure, float2 tilt, float speed) const
{
  const InkSettings &s = settings_;
  float v = s.use_pressure ? pressure_curve(s, pressure) : 1.0f;
  v *= 1.0f - s.speed_width * smooth_range(150.0f, 2500.0f, speed);
  if (s.tilt_width > 0.0f) {
    /* A pen leaning 60 degrees draws its full width. */
    const float lean = std::min(length(tilt) / 60.0f, 1.0f);
    v *= 1.0f + (lean - 1.0f) * s.tilt_width;
  }
  return s.min_size + (1.0f - s.min_size) * v;
}

void InkStroke::emit(float2 pos, float pressure, float2 tilt, float speed)
{
  const float r = radius(pressure, tilt, speed);
  if (!points_.empty() && distance(pos, emitted_) < MIN_SPACING) {
    /* A pen resting in place: pressing harder there still shows. */
    points_.back().radius = std::max(points_.back().radius, r);
    return;
  }
  points_.push_back({pos * view_.pixel, r, speed});
  emitted_ = pos;
}

/* The 1-euro filter: a low-pass whose cutoff rises with how fast the value changes, so a steady pressure
 * loses its jitter while a real press or lift still comes through at once. */
float InkStroke::filter_pressure(const Raw &raw)
{
  const float strength = settings_.pressure_smoothing;
  if (recent_.empty() || strength <= 0.0f) {
    pressure_ = raw.pressure;
    return pressure_;
  }
  const float dt = float(raw.time - recent_.back().time);
  const auto alpha = [dt](float cutoff) {
    const float tau = 1.0f / (2.0f * PI * cutoff);
    return 1.0f / (1.0f + tau / dt);
  };
  pressure_rate_ += ((raw.pressure - pressure_) / dt - pressure_rate_) * alpha(1.0f);
  /* From 16 Hz at none down to 1 Hz at full strength (a pen's jitter is well above that). */
  const float cutoff = 16.0f * std::pow(1.0f / 16.0f, strength) + 5.0f * std::abs(pressure_rate_);
  pressure_ += (raw.pressure - pressure_) * alpha(cutoff);
  return pressure_;
}

/* The pen's speed over the last moment (screen pixels a second). Going by where it got to, not the way
 * it went: a wobble in place is not speed. */
float InkStroke::recent_speed() const
{
  const Raw &last = recent_.back();
  size_t from = recent_.size() - 1;
  while (from > 0 && last.time - recent_[from].time < 0.04) {
    from--;
  }
  const double dt = last.time - recent_[from].time;
  return dt > 1e-5 ? float(distance(last.pos, recent_[from].pos) / dt) : 0.0f;
}

void InkStroke::add_smart(const Raw &raw, float pressure)
{
  const Raw &before = recent_[recent_.size() - 2];
  const float dt = float(raw.time - before.time);
  const int steps = std::clamp(int(std::ceil(dt / STEP)), 1, MAX_STEPS);
  const float omega = smart_omega(settings_, recent_speed());
  for (int i = 1; i <= steps; i++) {
    const float f = float(i) / float(steps);
    spring_.step(lerp(before.pos, raw.pos, f), dt / float(steps), omega);
    emit(spring_.pos, pressure, lerp(before.tilt, raw.tilt, f), length(spring_.vel));
  }
}

void InkStroke::add_pulled_string(const Raw &raw, float pressure)
{
  const float2 d = raw.pos - spring_.pos;
  const float len = length(d);
  const float over = len - settings_.string_length;
  if (over <= 0.0f) {
    return;
  }
  const float2 from = spring_.pos;
  spring_.pos = from + d * (over / len);
  /* A long pull in steps, for the pressure and the fit. */
  const int steps = std::max(int(over), 1);
  for (int i = 1; i <= steps; i++) {
    emit(lerp(from, spring_.pos, float(i) / float(steps)), pressure, raw.tilt, recent_speed());
  }
}

void InkStroke::add(const InkSample &sample)
{
  Raw raw{sample.pos / view_.pixel, sample.time, sample.pressure, sample.tilt};
  const bool first = recent_.empty();
  if (!first) {
    /* Samples can share a moment (a pen with no clock, a frame's mouse). */
    raw.time = std::max(raw.time, recent_.back().time + 1e-3);
  }
  const float pressure = filter_pressure(raw);
  recent_.push_back(raw);
  while (recent_.size() > 2 && raw.time - recent_[1].time > RECENT_SECONDS) {
    recent_.erase(recent_.begin());
  }
  if (first) {
    spring_.pos = raw.pos;
    emit(raw.pos, pressure, raw.tilt, 0.0f);
    return;
  }
  if (smart(settings_)) {
    add_smart(raw, pressure);
  }
  else if (settings_.stabilizer == InkStabilizer::PulledString) {
    add_pulled_string(raw, pressure);
  }
  else {
    spring_.pos = raw.pos;
    emit(raw.pos, pressure, raw.tilt, recent_speed());
  }
}

std::vector<InkPoint> InkStroke::tail(float2 pen) const
{
  std::vector<InkPoint> out;
  if (recent_.empty()) {
    return out;
  }
  const bool has_string = settings_.stabilizer == InkStabilizer::PulledString;
  if (has_string && !settings_.catch_up) {
    return out;
  }
  const float2 end = pen / view_.pixel;
  const float speed = has_string ? 0.0f : length(spring_.vel);
  const float r = radius(pressure_, recent_.back().tilt, speed);
  float2 last = emitted_;
  if (smart(settings_)) {
    Spring spring = spring_;
    for (int i = 1; i <= CATCH_UP_STEPS && distance(spring.pos, end) > MIN_SPACING; i++) {
      spring.step(end, STEP, CATCH_UP_OMEGA);
      if (distance(spring.pos, last) >= MIN_SPACING) {
        out.push_back({spring.pos * view_.pixel, r, speed});
        last = spring.pos;
      }
    }
  }
  /* However near, the line ends at the pen itself. */
  if (distance(end, last) > 1e-3f) {
    out.push_back({end * view_.pixel, r, speed});
  }
  return out;
}

/* Where the line should end: a pen lifting usually flicks off as it leaves the tablet, a hook of a few
 * pixels off the way it was going with the pressure falling away, and the line ends where that began. */
float2 InkStroke::lift_place() const
{
  static constexpr float HOOK_PIXELS = 6.0f;
  const size_t count = recent_.size();
  const Raw &last = recent_.back();
  size_t start = count - 1;
  for (float along = 0.0f; start > 0 && along < HOOK_PIXELS; start--) {
    along += distance(recent_[start].pos, recent_[start - 1].pos);
  }
  size_t before = start;
  for (float along = 0.0f; before > 0 && along < HOOK_PIXELS * 2.0f; before--) {
    along += distance(recent_[before].pos, recent_[before - 1].pos);
  }
  const float2 hook = last.pos - recent_[start].pos, way = recent_[start].pos - recent_[before].pos;
  if (length(hook) < 1.0f || length(way) < HOOK_PIXELS || !settings_.use_pressure) {
    return last.pos;
  }
  const bool lifting = last.pressure < recent_[start].pressure * 0.7f;
  const bool turned = dot(normalize(hook), normalize(way)) < 0.7f;
  return lifting && turned ? recent_[start].pos : last.pos;
}

void InkStroke::finish()
{
  if (recent_.empty()) {
    return;
  }
  const float2 end = lift_place();
  if (end != recent_.back().pos) {
    /* The spring may be into the hook already. */
    size_t nearest = points_.size() - 1;
    float along = 0.0f;
    for (size_t i = points_.size() - 1; i > 0 && along < 12.0f; i--) {
      along += distance(points_[i].pos, points_[i - 1].pos) / view_.pixel;
      if (distance(points_[i - 1].pos / view_.pixel, end) < distance(points_[nearest].pos / view_.pixel, end)) {
        nearest = i - 1;
      }
    }
    points_.resize(nearest + 1);
    emitted_ = points_.back().pos / view_.pixel;
  }
  const std::vector<InkPoint> rest = tail(end * view_.pixel);
  points_.insert(points_.end(), rest.begin(), rest.end());
}

float ink_tolerance(float fidelity)
{
  return 2.5f * std::pow(0.1f, std::clamp(fidelity, 0.0f, 1.0f));
}

void ink_taper(std::vector<InkPoint> &points, const InkSettings &settings, const InkView &view)
{
  float start = settings.taper_start * view.document_pixel, end = settings.taper_end * view.document_pixel;
  if ((start <= 0.0f && end <= 0.0f) || points.size() < 2) {
    return;
  }
  std::vector<float> along(points.size(), 0.0f);
  for (size_t i = 1; i < points.size(); i++) {
    along[i] = along[i - 1] + distance(points[i - 1].pos, points[i].pos);
  }
  const float total = along.back();
  /* A line shorter than its tapers narrows the whole way. */
  if (start + end > total && total > 0.0f) {
    const float scale = total / (start + end);
    start *= scale;
    end *= scale;
  }
  /* Quickly at the tip, easing into the line's width. */
  const auto ease = [](float x) { return x * (2.0f - x); };
  for (size_t i = 0; i < points.size(); i++) {
    if (start > 0.0f && along[i] < start) {
      points[i].radius *= ease(along[i] / start);
    }
    if (end > 0.0f && total - along[i] < end) {
      points[i].radius *= ease((total - along[i]) / end);
    }
  }
}

namespace {

/* One Bezier segment, from sample `first` to sample `last`. */
struct FitSegment {
  int first = 0, last = 0;
  float2 ctrl[4];
};

float2 bezier_at(const float2 c[4], float t)
{
  const float u = 1.0f - t;
  return c[0] * (u * u * u) + c[1] * (3.0f * u * u * t) + c[2] * (3.0f * u * t * t) + c[3] * (t * t * t);
}

/* Schneider's "An Algorithm for Automatically Fitting Digitized Curves" (Graphics Gems I), with the width
 * counted in the error: a segment's width runs straight along it, so a stretch the width doesn't is split
 * like one the line doesn't follow. All in screen pixels. */
struct Fitter {
  const std::vector<float2> &p;
  /* How far along the line each sample is, and its half width. */
  const std::vector<float> &along;
  const std::vector<float> &half;
  float tolerance;
  /* Each sample's place along the segment being fitted. */
  std::vector<float> u;
  std::vector<FitSegment> segments;

  /* Which way the line goes from sample i, forward (step 1) or back (step -1), over `reach` pixels: closer
   * than that a sample's step says more about the pen than the line. */
  float2 direction(int i, int step, int limit, float reach = 4.0f) const
  {
    int j = i;
    while (j != limit && std::abs(along[j] - along[i]) < reach) {
      j += step;
    }
    return normalize(p[j] - p[i]);
  }

  /* How far to look for the way a stretch of the line goes: further the longer it is, so what wobble is left
   * doesn't set a long segment's direction. */
  float reach_for(int first, int last) const
  {
    return std::clamp((along[last] - along[first]) * 0.25f, 4.0f, 40.0f);
  }

  void chord_places(int first, int last)
  {
    const float span = along[last] - along[first];
    for (int i = first; i <= last; i++) {
      u[i] = span > 0.0f ? (along[i] - along[first]) / span : 0.0f;
    }
  }

  /* The handles' lengths that fit the samples best, for handles going `t1` and `t2`. */
  void solve(FitSegment &seg, float2 t1, float2 t2) const
  {
    const float2 a = p[seg.first], b = p[seg.last];
    double c00 = 0.0, c01 = 0.0, c11 = 0.0, x0 = 0.0, x1 = 0.0;
    for (int i = seg.first; i <= seg.last; i++) {
      const double t = u[i], v = 1.0 - t;
      const double b0 = v * v * v, b1 = 3.0 * v * v * t, b2 = 3.0 * v * t * t, b3 = t * t * t;
      const double a0x = t1.x * b1, a0y = t1.y * b1, a1x = t2.x * b2, a1y = t2.y * b2;
      c00 += a0x * a0x + a0y * a0y;
      c01 += a0x * a1x + a0y * a1y;
      c11 += a1x * a1x + a1y * a1y;
      const double rx = p[i].x - a.x * (b0 + b1) - b.x * (b2 + b3);
      const double ry = p[i].y - a.y * (b0 + b1) - b.y * (b2 + b3);
      x0 += a0x * rx + a0y * ry;
      x1 += a1x * rx + a1y * ry;
    }
    const double det = c00 * c11 - c01 * c01;
    double left = 0.0, right = 0.0;
    if (std::abs(det) > 1e-12) {
      left = (x0 * c11 - x1 * c01) / det;
      right = (c00 * x1 - c01 * x0) / det;
    }
    const double span = along[seg.last] - along[seg.first];
    /* No good answer (too few samples, or an S the tangents can't make): a third of the way. */
    const auto usable = [span](double v) { return v > 1e-3 * span && v < 1.5 * span; };
    if (!usable(left) || !usable(right)) {
      left = right = span / 3.0;
    }
    seg.ctrl[0] = a;
    seg.ctrl[1] = a + t1 * float(left);
    seg.ctrl[2] = b + t2 * float(right);
    seg.ctrl[3] = b;
  }

  /* The worst sample's error, and which it is. */
  float error(const FitSegment &seg, int *r_worst) const
  {
    float worst = 0.0f;
    *r_worst = (seg.first + seg.last) / 2;
    const float span = along[seg.last] - along[seg.first];
    for (int i = seg.first + 1; i < seg.last; i++) {
      const float blend = span > 0.0f ? (along[i] - along[seg.first]) / span : 0.0f;
      const float width_off = std::abs(half[seg.first] + (half[seg.last] - half[seg.first]) * blend - half[i]);
      /* Counted for more: the eye sees a bend in a line's edge well before a place a pixel off. */
      const float e = std::max(distance(bezier_at(seg.ctrl, u[i]), p[i]), width_off * 3.0f);
      if (e > worst) {
        worst = e;
        *r_worst = i;
      }
    }
    return worst;
  }

  /* Newton's step: each sample's place on the segment moved to the nearest to it. */
  void refine_places(const FitSegment &seg)
  {
    const float2 *c = seg.ctrl;
    const float2 d1[3] = {(c[1] - c[0]) * 3.0f, (c[2] - c[1]) * 3.0f, (c[3] - c[2]) * 3.0f};
    const float2 d2[2] = {(d1[1] - d1[0]) * 2.0f, (d1[2] - d1[1]) * 2.0f};
    for (int i = seg.first + 1; i < seg.last; i++) {
      const float t = u[i], v = 1.0f - t;
      const float2 off = bezier_at(c, t) - p[i];
      const float2 q1 = d1[0] * (v * v) + d1[1] * (2.0f * v * t) + d1[2] * (t * t);
      const float2 q2 = d2[0] * v + d2[1] * t;
      const float den = dot(q1, q1) + dot(off, q2);
      if (std::abs(den) > 1e-9f) {
        u[i] = std::clamp(t - dot(off, q1) / den, 0.0f, 1.0f);
      }
    }
  }

  /* One segment over the samples, if one will do. */
  bool fit_one(int first, int last, float2 t1, float2 t2, FitSegment *r_seg, int *r_worst)
  {
    FitSegment &seg = *r_seg;
    seg.first = first;
    seg.last = last;
    chord_places(first, last);
    solve(seg, t1, t2);
    float e = error(seg, r_worst);
    if (e <= tolerance) {
      return true;
    }
    /* Near enough for better places along it to help. */
    if (e <= tolerance * 4.0f) {
      for (int i = 0; i < 4; i++) {
        refine_places(seg);
        solve(seg, t1, t2);
        e = error(seg, r_worst);
        if (e <= tolerance) {
          return true;
        }
      }
    }
    return false;
  }

  /* Where to split near the worst sample: a sample as near the fit as there is, close by. Splitting on what is
   * left of a wobble's crest would pin the line there, and the next crest would then be twice as far off, so
   * every wobble would get a point. */
  int settled(const FitSegment &seg, int worst) const
  {
    int best = worst;
    float best_off = distance(bezier_at(seg.ctrl, u[worst]), p[worst]);
    for (const int step : {-1, 1}) {
      for (int i = worst + step; i > seg.first && i < seg.last && std::abs(along[i] - along[worst]) < 12.0f; i += step) {
        const float off = distance(bezier_at(seg.ctrl, u[i]), p[i]);
        if (off < best_off) {
          best_off = off;
          best = i;
        }
      }
    }
    return best_off < tolerance * 0.35f ? best : worst;
  }

  void fit(int first, int last, float2 t1, float2 t2)
  {
    FitSegment seg;
    int worst = 0;
    if (fit_one(first, last, t1, t2, &seg, &worst) || last - first < 2) {
      segments.push_back(seg);
      return;
    }
    worst = settled(seg, std::clamp(worst, first + 1, last - 1));
    const float reach = reach_for(first, last);
    const float2 middle = normalize(direction(worst, 1, last, reach) - direction(worst, -1, first, reach));
    fit(first, worst, t1, middle * -1.0f);
    fit(worst, last, middle, t2);
  }

  /* Takes away the points the split left that aren't needed: a split falls at the worst place, not the best. */
  void merge(size_t from)
  {
    for (int pass = 0; pass < 3; pass++) {
      bool merged = false;
      for (size_t k = from; k + 1 < segments.size();) {
        const FitSegment &a = segments[k], &b = segments[k + 1];
        FitSegment seg;
        int worst = 0;
        if (fit_one(a.first, b.last, normalize(a.ctrl[1] - a.ctrl[0]), normalize(b.ctrl[2] - b.ctrl[3]), &seg, &worst)) {
          segments[k] = seg;
          segments.erase(segments.begin() + k + 1);
          merged = true;
        }
        else {
          k++;
        }
      }
      if (!merged) {
        break;
      }
    }
  }
};

}  // namespace

/* Corners: where the line turns sharply over a few pixels and the pen slowed for it (a tight curve drawn at
 * an even speed turns as much and is no corner), or doubles back. */
static std::vector<int> find_corners(const std::vector<float2> &p, const std::vector<float> &along,
                                     const std::vector<InkPoint> &points)
{
  static constexpr float REACH = 6.0f;
  const int n = int(p.size());
  std::vector<float> turn(n, 0.0f);
  int back = 0, ahead = 0;
  for (int i = 0; i < n; i++) {
    while (along[i] - along[back + 1] >= REACH) {
      back++;
    }
    while (ahead < n - 1 && along[ahead] - along[i] < REACH) {
      ahead++;
    }
    if (along[i] < REACH || along[n - 1] - along[i] < REACH) {
      continue;
    }
    const float c = dot(normalize(p[i] - p[back]), normalize(p[ahead] - p[i]));
    turn[i] = std::acos(std::clamp(c, -1.0f, 1.0f));
  }
  std::vector<int> corners;
  const float MIN_TURN = 50.0f * PI / 180.0f, DOUBLES_BACK = 110.0f * PI / 180.0f;
  for (int i = 0; i < n; i++) {
    if (turn[i] < MIN_TURN) {
      continue;
    }
    bool peak = true;
    float fastest = 0.0f;
    for (int j = i - 1; j >= 0 && along[i] - along[j] < REACH * 3.0f; j--) {
      peak &= along[i] - along[j] > REACH || turn[j] <= turn[i];
      fastest = std::max(fastest, points[j].speed);
    }
    for (int j = i + 1; j < n && along[j] - along[i] < REACH * 3.0f; j++) {
      peak &= along[j] - along[i] > REACH || turn[j] < turn[i];
      fastest = std::max(fastest, points[j].speed);
    }
    if (peak && (turn[i] >= DOUBLES_BACK || points[i].speed < fastest * 0.75f)) {
      corners.push_back(i);
    }
  }
  return corners;
}

/* How much of each end of a closed line (screen pixels) the fit goes across. */
static constexpr float SEAM_PIXELS = 12.0f;

std::vector<InkKnot> ink_fit(std::vector<InkPoint> points, const InkSettings &settings, const InkView &view,
                             bool closed)
{
  std::vector<InkKnot> knots;
  if (points.empty()) {
    return knots;
  }
  float length_total = 0.0f, widest = 0.0f;
  for (size_t i = 0; i < points.size(); i++) {
    length_total += i > 0 ? distance(points[i - 1].pos, points[i].pos) : 0.0f;
    widest = std::max(widest, points[i].radius);
  }
  if (points.size() < 2 || length_total < view.pixel) {
    const float2 at = points.front().pos;
    knots.push_back({at, widest});
    knots.push_back({at + float2(view.pixel * 0.5f, 0.0f), widest});
    return knots;
  }
  if (!closed) {
    ink_taper(points, settings, view);
  }
  else {
    /* The line starts behind a moving pen and catches up with it at the end, each end cutting inside the curve,
     * so the fit goes across that stretch instead of making a point where they meet. */
    const auto trimmed = [&](size_t from, int step) {
      float along_end = 0.0f;
      size_t i = from;
      while (along_end < SEAM_PIXELS * view.pixel) {
        along_end += distance(points[i].pos, points[i + step].pos);
        i += step;
      }
      return i;
    };
    if (length_total > SEAM_PIXELS * 8.0f * view.pixel) {
      const size_t first = trimmed(0, 1), last_kept = trimmed(points.size() - 1, -1);
      points = std::vector<InkPoint>(points.begin() + first, points.begin() + last_kept + 1);
    }
    points.push_back(points.front());
  }

  /* In screen pixels, skipping any samples on top of each other. */
  std::vector<float2> p;
  std::vector<float> along, half;
  std::vector<InkPoint> kept;
  const float half_pixels = view.half_width / view.pixel;
  for (const InkPoint &point : points) {
    const float2 at = point.pos / view.pixel;
    if (!p.empty() && distance(at, p.back()) < 1e-4f) {
      continue;
    }
    along.push_back(p.empty() ? 0.0f : along.back() + distance(at, p.back()));
    p.push_back(at);
    half.push_back(point.radius * half_pixels);
    kept.push_back(point);
  }
  const int last = int(p.size()) - 1;

  Fitter fitter{p, along, half, ink_tolerance(settings.fidelity), std::vector<float>(p.size(), 0.0f), {}};
  std::vector<int> ends = {0};
  for (const int corner : find_corners(p, along, kept)) {
    ends.push_back(corner);
  }
  ends.push_back(last);
  /* A closed line joins its ends smoothly unless they meet at a corner. */
  const float2 from_start = fitter.direction(0, 1, last), into_end = fitter.direction(last, -1, 0);
  const bool smooth_seam = closed && dot(from_start, into_end) < -0.6f;
  const float2 seam = normalize(from_start - into_end);
  for (size_t i = 0; i + 1 < ends.size(); i++) {
    /* Into a corner the line's own last pixels say how it arrives; at the line's ends, what wobble is left. */
    const float reach = fitter.reach_for(ends[i], ends[i + 1]);
    const bool first = i == 0, final = i + 2 == ends.size();
    const float2 t1 = first && smooth_seam ? seam : fitter.direction(ends[i], 1, ends[i + 1], first ? reach : 6.0f);
    const float2 t2 = final && smooth_seam ? seam * -1.0f :
                                             fitter.direction(ends[i + 1], -1, ends[i], final ? reach : 6.0f);
    const size_t from = fitter.segments.size();
    fitter.fit(ends[i], ends[i + 1], t1, t2);
    fitter.merge(from);
  }

  const std::vector<FitSegment> &segments = fitter.segments;
  knots.resize(segments.size() + 1);
  for (size_t i = 0; i <= segments.size(); i++) {
    InkKnot &knot = knots[i];
    const int sample = i < segments.size() ? segments[i].first : segments.back().last;
    knot.pos = kept[sample].pos;
    knot.radius = kept[sample].radius;
    /* The line's own ends aren't corners: E carries on from them smoothly. */
    knot.corner = i > 0 && i < segments.size() && std::find(ends.begin(), ends.end(), sample) != ends.end();
    if (i > 0) {
      knot.handle_in = (segments[i - 1].ctrl[2] - segments[i - 1].ctrl[3]) * view.pixel;
    }
    if (i < segments.size()) {
      knot.handle_out = (segments[i].ctrl[1] - segments[i].ctrl[0]) * view.pixel;
    }
  }
  if (closed && knots.size() >= 3) {
    knots.front().handle_in = knots.back().handle_in;
    knots.front().corner = !smooth_seam;
    knots.pop_back();
  }
  return knots;
}

std::vector<InkKnot> ink_fit_ring(const std::vector<float2> &ring, float tolerance, float pixel)
{
  /* Its corners, in screen pixels, with no sides too short to say which way they go. */
  std::vector<float2> sides;
  for (const float2 &point : ring) {
    const float2 at = point / pixel;
    if (sides.empty() || distance(at, sides.back()) >= 0.2f) {
      sides.push_back(at);
    }
  }
  while (sides.size() > 3 && distance(sides.back(), sides.front()) < 0.2f) {
    sides.pop_back();
  }
  const int count = int(sides.size());
  std::vector<InkKnot> knots;
  if (count < 3) {
    return knots;
  }
  std::vector<int> sharp;
  for (int i = 0; i < count; i++) {
    const float2 in = normalize(sides[i] - sides[(i + count - 1) % count]);
    const float2 out = normalize(sides[(i + 1) % count] - sides[i]);
    if (dot(in, out) < std::cos(40.0f * PI / 180.0f)) {
      sharp.push_back(i);
    }
  }
  /* Going round from a corner if it has one, every few pixels: a fit only looks at its samples, and a long
   * straight side has none between its ends. */
  const int first = sharp.empty() ? 0 : sharp.front();
  std::vector<float2> p;
  std::vector<float> along;
  std::vector<int> ends;
  const auto add = [&](float2 at) {
    along.push_back(p.empty() ? 0.0f : along.back() + distance(at, p.back()));
    p.push_back(at);
  };
  for (int k = 0; k < count; k++) {
    const int i = (first + k) % count;
    if (std::find(sharp.begin(), sharp.end(), i) != sharp.end()) {
      ends.push_back(int(p.size()));
    }
    const float2 a = sides[i], b = sides[(i + 1) % count];
    const int steps = std::max(int(distance(a, b) / 3.0f), 1);
    for (int s = 0; s < steps; s++) {
      add(lerp(a, b, float(s) / float(steps)));
    }
  }
  add(sides[first]);
  const int last = int(p.size()) - 1;
  const bool cornered = !ends.empty();
  if (!cornered) {
    ends.push_back(0);
  }
  ends.push_back(last);
  /* A loop needs at least three points: runs split at their middles, smoothly, until it has them. */
  std::vector<bool> corner(p.size(), false);
  for (const int end : ends) {
    corner[end] = cornered;
  }
  while (ends.size() < 4) {
    size_t longest = 0;
    for (size_t i = 0; i + 1 < ends.size(); i++) {
      if (ends[i + 1] - ends[i] > ends[longest + 1] - ends[longest]) {
        longest = i;
      }
    }
    if (ends[longest + 1] - ends[longest] < 2) {
      return knots;
    }
    ends.insert(ends.begin() + longest + 1, (ends[longest] + ends[longest + 1]) / 2);
  }

  const std::vector<float> no_width(p.size(), 0.0f);
  Fitter fitter{p, along, no_width, tolerance, std::vector<float>(p.size(), 0.0f), {}};
  /* The way the line goes through a smooth end. */
  const auto through = [&](int i) {
    const int before = i == 0 ? last : i, after = i == last ? 0 : i;
    return normalize(fitter.direction(after, 1, last) - fitter.direction(before, -1, 0));
  };
  for (size_t i = 0; i + 1 < ends.size(); i++) {
    const int a = ends[i], b = ends[i + 1];
    const float2 t1 = corner[a] ? fitter.direction(a, 1, b, 6.0f) : through(a);
    const float2 t2 = corner[b] ? fitter.direction(b, -1, a, 6.0f) : through(b) * -1.0f;
    const size_t from = fitter.segments.size();
    fitter.fit(a, b, t1, t2);
    fitter.merge(from);
  }

  const std::vector<FitSegment> &segments = fitter.segments;
  knots.resize(segments.size());
  for (size_t i = 0; i < segments.size(); i++) {
    const FitSegment &segment = segments[i], &before = segments[(i + segments.size() - 1) % segments.size()];
    knots[i].pos = p[segment.first] * pixel;
    knots[i].corner = corner[segment.first];
    knots[i].handle_in = (before.ctrl[2] - before.ctrl[3]) * pixel;
    knots[i].handle_out = (segment.ctrl[1] - segment.ctrl[0]) * pixel;
  }
  return knots;
}

InkSweep ink_sweep(std::vector<InkPoint> points, const InkSettings &settings, const InkView &view)
{
  /* A tap: its widest, as a dot's. */
  float widest = 0.0f;
  for (const InkPoint &point : points) {
    widest = std::max(widest, point.radius);
  }
  ink_taper(points, settings, view);
  std::vector<float2> centers;
  std::vector<float> radii;
  for (const InkPoint &point : points) {
    const float radius = point.radius * view.half_width;
    /* Fewer than the line has: every sample's disc would only cost time, this close together. */
    if (centers.empty() || distance(point.pos, centers.back()) >= std::max(view.pixel, radius * 0.2f)) {
      centers.push_back(point.pos);
      radii.push_back(radius);
    }
  }
  if (centers.size() == 1) {
    radii[0] = widest * view.half_width;
  }
  else if (!centers.empty() && distance(points.back().pos, centers.back()) > 0.0f) {
    centers.push_back(points.back().pos);
    radii.push_back(points.back().radius * view.half_width);
  }
  return {std::move(centers), std::move(radii)};
}

std::vector<FillPiece> ink_area(const std::vector<InkPoint> &points, const InkSettings &settings, const InkView &view)
{
  const InkSweep sweep = ink_sweep(points, settings, view);
  return sweep.centers.empty() ? std::vector<FillPiece>() : fill_sweep(sweep.centers, sweep.radii, view.pixel * 0.1f);
}

}  // namespace oanim
