#include "anim/channel.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <type_traits>

namespace oanim {

static constexpr const char *INTERP_IDS[INTERP_COUNT] = {"LINEAR", "SMOOTH", "EASE_IN_OUT", "EASE_IN", "EASE_OUT",
                                                         "STEP",   "BEZIER", "BACK",        "BOUNCE",  "ELASTIC",
                                                         "NOISY",  "CYCLE",  "POSE",        "CUSTOM"};
static constexpr const char *INTERP_NAMES[INTERP_COUNT] = {
    "Linear", "Smooth", "Ease In/Out", "Ease In", "Ease Out", "Step", "Bezier",
    "Back",   "Bounce", "Elastic",     "Noisy",   "Cycle",    "Pose",     "Custom"};

const char *key_type_identifier(KeyType type)
{
  return type == KeyType::Breakdown ? "BREAKDOWN" : "KEYFRAME";
}

const char *key_type_name(KeyType type)
{
  return type == KeyType::Breakdown ? "Breakdown" : "Key";
}

bool key_type_from_identifier(const std::string &identifier, KeyType *r_type)
{
  if (identifier == "KEYFRAME" || identifier == "BREAKDOWN") {
    *r_type = identifier == "BREAKDOWN" ? KeyType::Breakdown : KeyType::Key;
    return true;
  }
  return false;
}

const char *interp_identifier(Interp interp)
{
  return INTERP_IDS[int(interp)];
}

const char *interp_name(Interp interp)
{
  return INTERP_NAMES[int(interp)];
}

bool interp_from_identifier(const std::string &identifier, Interp *r_interp)
{
  for (int i = 0; i < INTERP_COUNT; i++) {
    if (identifier == INTERP_IDS[i]) {
      *r_interp = Interp(i);
      return true;
    }
  }
  return false;
}

static constexpr const char *EASE_IDS[EASE_COUNT] = {"SINE", "QUAD", "CUBIC", "QUART", "QUINT", "EXPO", "CIRC"};
static constexpr const char *EASE_NAMES[EASE_COUNT] = {"Gentle (Sine)", "Quad",          "Cubic", "Quart",
                                                       "Quint",         "Strong (Expo)", "Circ"};

const char *ease_identifier(Ease ease)
{
  return EASE_IDS[int(ease)];
}

const char *ease_name(Ease ease)
{
  return EASE_NAMES[int(ease)];
}

bool ease_from_identifier(const std::string &identifier, Ease *r_ease)
{
  for (int i = 0; i < EASE_COUNT; i++) {
    if (identifier == EASE_IDS[i]) {
      *r_ease = Ease(i);
      return true;
    }
  }
  return false;
}

NewKeySettings &new_key_settings()
{
  static NewKeySettings settings;
  return settings;
}

static bool g_stepped_preview = false;

bool stepped_preview()
{
  return g_stepped_preview;
}

void stepped_preview_set(bool on)
{
  g_stepped_preview = on;
}

/* --- Easing curves, 0..1 -> 0..1 (Blender's BLI_easing ease-out forms). --------------- */

/* Slowing into the end (Penner's "out" form) at each strength. */
static float ease_slowing(Ease ease, float t)
{
  const float u = 1.0f - t;
  switch (ease) {
    case Ease::Sine:
      return std::sin(t * PI * 0.5f);
    case Ease::Quad:
      return 1.0f - u * u;
    case Ease::Cubic:
      return 1.0f - u * u * u;
    case Ease::Quart:
      return 1.0f - u * u * u * u;
    case Ease::Quint:
      return 1.0f - u * u * u * u * u;
    case Ease::Expo:
      return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
    case Ease::Circ:
      return std::sqrt(std::max(0.0f, 1.0f - u * u));
  }
  return t;
}

static float ease_back(float t, float overshoot)
{
  t -= 1.0f;
  return t * t * ((overshoot + 1.0f) * t + overshoot) + 1.0f;
}

static float ease_bounce(float t)
{
  if (t < 1.0f / 2.75f) {
    return 7.5625f * t * t;
  }
  if (t < 2.0f / 2.75f) {
    t -= 1.5f / 2.75f;
    return 7.5625f * t * t + 0.75f;
  }
  if (t < 2.5f / 2.75f) {
    t -= 2.25f / 2.75f;
    return 7.5625f * t * t + 0.9375f;
  }
  t -= 2.625f / 2.75f;
  return 7.5625f * t * t + 0.984375f;
}

static float ease_elastic(float t, float amplitude, float period)
{
  if (t <= 0.0f || t >= 1.0f) {
    return t <= 0.0f ? 0.0f : 1.0f;
  }
  period = std::max(period, 0.02f);
  /* Penner's elastic out: below 1 the swing can't reach past the move, so it is the move. */
  float s = period / 4.0f;
  if (amplitude < 1.0f) {
    amplitude = 1.0f;
  }
  else {
    s = period / (2.0f * PI) * std::asin(1.0f / amplitude);
  }
  return amplitude * std::pow(2.0f, -10.0f * t) * std::sin((t - s) * (2.0f * PI) / period) + 1.0f;
}

/* Moho's names: Ease In slows into this key, Ease Out slows away from the one before. */
static float ease_shaped(Interp interp, Ease ease, float t)
{
  switch (interp) {
    case Interp::EaseIn:
      return ease_slowing(ease, t);
    case Interp::EaseOut:
      return 1.0f - ease_slowing(ease, 1.0f - t);
    default:
      return t < 0.5f ? 0.5f - 0.5f * ease_slowing(ease, 1.0f - 2.0f * t)
                      : 0.5f + 0.5f * ease_slowing(ease, 2.0f * t - 1.0f);
  }
}

BezierHandles bezier_clamped(const BezierHandles &h)
{
  return {std::clamp(h[0], 0.0f, 1.0f), std::clamp(h[1], -2.0f, 3.0f), std::clamp(h[2], 0.0f, 1.0f),
          std::clamp(h[3], -2.0f, 3.0f)};
}

/* One coordinate of a cubic Bezier from 0 to 1 with control values c1, c2, at parameter s. */
static float bezier_1d(float c1, float c2, float s)
{
  const float u = 1.0f - s;
  return 3.0f * u * u * s * c1 + 3.0f * u * s * s * c2 + s * s * s;
}

/* The curve's parameter where its time is `t`: with both handle times in 0..1 time only goes forward, so halving
 * finds it. */
static float bezier_param(float x1, float x2, float t)
{
  float lo = 0.0f, hi = 1.0f;
  for (int i = 0; i < 24; i++) {
    const float mid = (lo + hi) * 0.5f;
    (bezier_1d(x1, x2, mid) < t ? lo : hi) = mid;
  }
  return (lo + hi) * 0.5f;
}

float bezier_ease(const BezierHandles &handles, float t)
{
  const BezierHandles h = bezier_clamped(handles);
  t = std::clamp(t, 0.0f, 1.0f);
  if (t <= 0.0f || t >= 1.0f) {
    return t;
  }
  return bezier_1d(h[1], h[3], bezier_param(h[0], h[2], t));
}

bool interp_bezier_fit(const KeySettings &s, BezierHandles &r_handles)
{
  switch (s.interp) {
    case Interp::Bezier:
      r_handles = bezier_clamped(s.bezier);
      return true;
    case Interp::Linear:
      r_handles = BEZIER_EVEN;
      return true;
    case Interp::Smooth:
      /* 3t^2 - 2t^3 exactly. */
      r_handles = {1.0f / 3.0f, 0.0f, 2.0f / 3.0f, 1.0f};
      return true;
    case Interp::EaseIn:
    case Interp::EaseOut:
    case Interp::EaseInOut:
    case Interp::Back:
      break;
    default:
      return false;
  }
  /* For each pair of handle times the progress is linear in the two handle heights, so those come from least
   * squares; the times are searched on a grid. */
  constexpr int SAMPLES = 32, GRID = 20;
  float best = 1e30f;
  for (int i = 0; i <= GRID; i++) {
    for (int j = 0; j <= GRID; j++) {
      const float x1 = float(i) / GRID, x2 = float(j) / GRID;
      float a11 = 0, a12 = 0, a22 = 0, b1 = 0, b2 = 0;
      float ts[SAMPLES], p1[SAMPLES], p2[SAMPLES], r[SAMPLES];
      for (int k = 0; k < SAMPLES; k++) {
        const float t = (float(k) + 0.5f) / SAMPLES;
        const float u = bezier_param(x1, x2, t), v = 1.0f - u;
        ts[k] = t;
        p1[k] = 3.0f * v * v * u;
        p2[k] = 3.0f * v * u * u;
        r[k] = interp_curve(s, t) - u * u * u;
        a11 += p1[k] * p1[k];
        a12 += p1[k] * p2[k];
        a22 += p2[k] * p2[k];
        b1 += p1[k] * r[k];
        b2 += p2[k] * r[k];
      }
      const float det = a11 * a22 - a12 * a12;
      if (std::fabs(det) < 1e-9f) {
        continue;
      }
      const float y1 = std::clamp((b1 * a22 - b2 * a12) / det, -2.0f, 3.0f);
      const float y2 = std::clamp((a11 * b2 - a12 * b1) / det, -2.0f, 3.0f);
      float err = 0.0f;
      for (int k = 0; k < SAMPLES; k++) {
        const float e = p1[k] * y1 + p2[k] * y2 - r[k];
        err += e * e;
      }
      if (err < best) {
        best = err;
        r_handles = {x1, y1, x2, y2};
      }
    }
  }
  return best < 1e29f;
}

float interp_curve(const KeySettings &s, float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  switch (s.interp) {
    case Interp::Step:
    case Interp::Pose:
      return t >= 1.0f ? 1.0f : 0.0f;
    case Interp::Linear:
    case Interp::Noisy:
    case Interp::Custom:
      return t;
    case Interp::EaseIn:
    case Interp::EaseOut:
    case Interp::EaseInOut:
      return ease_shaped(s.interp, s.ease, t);
    case Interp::Back:
      return ease_back(t, s.overshoot);
    case Interp::Bounce:
      return ease_bounce(t);
    case Interp::Elastic:
      return ease_elastic(t, s.elastic_amplitude, s.elastic_period);
    case Interp::Cycle:
      /* Back to the start and again: a saw. */
      return t < 0.5f ? t * 2.0f : (t - 0.5f) * 2.0f;
    case Interp::Bezier:
      return bezier_ease(s.bezier, t);
    case Interp::Smooth:
      break;
  }
  return t * t * (3.0f - 2.0f * t);
}

/* Smooth noise, -1..1, from a hash at whole numbers. */
static float noise1(uint32_t seed, float x)
{
  const auto hash = [](uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352dU;
    v ^= v >> 15;
    v *= 0x846ca68bU;
    v ^= v >> 16;
    return float(v & 0xffffff) / float(0x800000) - 1.0f;
  };
  const float f = std::floor(x);
  const float t = x - f;
  const uint32_t i = uint32_t(int32_t(f));
  const float a = hash(seed ^ (i * 2654435761U)), b = hash(seed ^ ((i + 1) * 2654435761U));
  const float s = t * t * (3.0f - 2.0f * t);
  return a + (b - a) * s;
}

static float noise_of(float /*value*/, const Key<float> &key, float frame)
{
  return key.noise_amplitude * noise1(0x51ed270bU, frame * key.noise_frequency);
}

static float2 noise_of(float2 /*value*/, const Key<float2> &key, float frame)
{
  return {key.noise_amplitude * noise1(0x51ed270bU, frame * key.noise_frequency),
          key.noise_amplitude * noise1(0x2f6b3c1dU, frame * key.noise_frequency)};
}

/* The slope at a key from its neighbours' values and frame gaps (Blender's auto-clamped
 * handle): flat at a peak, valley or plateau, else the neighbours' line, limited so the curve
 * can't overshoot either side (Fritsch-Carlson). */
static float clamped_slope(float vp, float v, float vn, float dp, float dn, bool has_prev, bool has_next)
{
  if (!has_prev && !has_next) {
    return 0.0f;
  }
  if (!has_prev) {
    return dn > 0.0f ? (vn - v) / dn : 0.0f;
  }
  if (!has_next) {
    return dp > 0.0f ? (v - vp) / dp : 0.0f;
  }
  const float d0 = dp > 0.0f ? (v - vp) / dp : 0.0f;
  const float d1 = dn > 0.0f ? (vn - v) / dn : 0.0f;
  if (d0 * d1 <= 0.0f) {
    return 0.0f;
  }
  const float m = (dp + dn) > 0.0f ? (vn - vp) / (dp + dn) : 0.0f;
  const float limit = 3.0f * std::min(std::fabs(d0), std::fabs(d1));
  return std::clamp(m, -limit, limit);
}

static void for_each_component(float &r, float v, float vp, float vn,
                               const std::function<void(float &, float, float, float)> &f)
{
  f(r, v, vp, vn);
}

static void for_each_component(float2 &r, float2 v, float2 vp, float2 vn,
                               const std::function<void(float &, float, float, float)> &f)
{
  f(r.x, v.x, vp.x, vn.x);
  f(r.y, v.y, vp.y, vn.y);
}

static void keys_changed();

/* Smooth's slope at key `i` (value per frame), from its neighbours. */
template<typename T> static T key_slope(const std::vector<Key<T>> &keys, int i)
{
  const int n = int(keys.size());
  const bool has_prev = i > 0, has_next = i + 1 < n;
  T m{};
  for_each_component(m, keys[i].value, has_prev ? keys[i - 1].value : keys[i].value,
                     has_next ? keys[i + 1].value : keys[i].value, [&](float &r, float v, float vp, float vn) {
                       const float fp = has_prev ? float(keys[i - 1].frame) : 0.0f;
                       const float fn = has_next ? float(keys[i + 1].frame) : 0.0f;
                       const float f = float(keys[i].frame);
                       r = clamped_slope(vp, v, vn, f - fp, fn - f, has_prev, has_next);
                     });
  return m;
}

/* The frames a stretch from key `a` to key `b` moves over (after a's hold). */
template<typename T> static float stretch_span(const Key<T> &a, const Key<T> &b)
{
  return float(b.frame - std::min(a.frame + std::max(a.hold, 0), b.frame));
}

static Interp stretch_interp(Interp arriving)
{
  return arriving == Interp::Cycle ? Interp::Smooth : arriving;
}

/* Timed by the curve's own parameter, as Smooth's Hermite is. */
static bool interp_smooth(Interp interp)
{
  return interp == Interp::Smooth;
}

/* Shaped as Smooth (Auto handles from the slopes): Custom and Bezier go along the path Smooth makes, at their own
 * timing. */
static bool interp_smooth_shape(Interp interp)
{
  return interp_smooth(interp) || interp == Interp::Custom || interp == Interp::Bezier;
}

bool interp_path_smooth(Interp interp)
{
  return interp_smooth_shape(stretch_interp(interp));
}

void key_path_handles_freeze(std::vector<Key<float2>> &keys, size_t i)
{
  if (i >= keys.size() || keys[i].path_handles != PathHandles::Auto) {
    return;
  }
  float2 in, out;
  key_path_handles(keys, i, in, out);
  keys[i].path_in = in;
  keys[i].path_out = out;
  keys[i].path_handles = PathHandles::Broken;
  keys_changed();
}

float custom_progress(const std::vector<float> &speeds, float t)
{
  const size_t n = speeds.size();
  if (n == 0) {
    return t;
  }
  /* Speeds are signed: a negative one goes back along the path, so the dot can pass its key and come back to it
   * (overshoot) or back up before setting off (anticipation); over the stretch they still add up to arriving. */
  float total = 0.0f;
  for (const float s : speeds) {
    total += s;
  }
  if (total <= 1e-9f) {
    return t;
  }
  const float x = std::clamp(t, 0.0f, 1.0f) * float(n);
  float done = 0.0f;
  size_t i = 0;
  for (; i < n && float(i + 1) <= x; i++) {
    done += speeds[i];
  }
  if (i < n) {
    done += speeds[i] * (x - float(i));
  }
  return done / total;
}

template<typename T> void key_speeds_set(std::vector<Key<T>> &keys, size_t i, std::vector<float> speeds)
{
  if (i >= keys.size()) {
    return;
  }
  keys[i].interp = Interp::Custom;
  keys[i].speeds = std::move(speeds);
  keys_changed();
}
template void key_speeds_set(std::vector<Key<float>> &keys, size_t i, std::vector<float> speeds);
template void key_speeds_set(std::vector<Key<float2>> &keys, size_t i, std::vector<float> speeds);

void key_path_handles(const std::vector<Key<float2>> &keys, size_t i, float2 &r_in, float2 &r_out)
{
  r_in = r_out = float2{0.0f, 0.0f};
  if (i >= keys.size()) {
    return;
  }
  const Key<float2> &k = keys[i];
  if (k.path_handles != PathHandles::Auto) {
    r_in = k.path_in;
    r_out = k.path_out;
    return;
  }
  if (i > 0) {
    const Key<float2> &a = keys[i - 1];
    r_in = interp_smooth_shape(stretch_interp(k.interp)) ? key_slope(keys, int(i)) * (-stretch_span(a, k) / 3.0f)
                                                    : (a.value - k.value) / 3.0f;
  }
  if (i + 1 < keys.size()) {
    const Key<float2> &b = keys[i + 1];
    r_out = interp_smooth_shape(stretch_interp(b.interp)) ? key_slope(keys, int(i)) * (stretch_span(k, b) / 3.0f)
                                                     : (b.value - k.value) / 3.0f;
  }
}

static float2 bezier(const float2 p[4], float t)
{
  const float s = 1.0f - t;
  return p[0] * (s * s * s) + p[1] * (3.0f * s * s * t) + p[2] * (3.0f * s * t * t) + p[3] * (t * t * t);
}

/* Where a stretch with handles is, `u` of the way along it by length (Linear moves at an even speed); past either end
 * (Back, Elastic) it carries on along the end's direction. */
static float2 bezier_along(const float2 p[4], float u)
{
  constexpr int PIECES = 32;
  float lengths[PIECES + 1] = {0.0f};
  float2 prev = p[0];
  for (int j = 1; j <= PIECES; j++) {
    const float2 q = bezier(p, float(j) / PIECES);
    lengths[j] = lengths[j - 1] + length(q - prev);
    prev = q;
  }
  const float total = lengths[PIECES];
  if (total < 1e-9f) {
    return p[0];
  }
  const auto direction = [](float2 from, float2 to, float2 fallback) {
    const float2 d = to - from;
    const float l = length(d);
    return l > 1e-9f ? d / l : fallback;
  };
  const float2 chord = direction(p[0], p[3], float2{1.0f, 0.0f});
  /* A handle next to nothing (a flat Auto one is float noise) gives no way: the curve leaves toward the other. */
  const float tiny = 1e-4f * total;
  if (u <= 0.0f) {
    const float2 d = direction(p[0], length(p[1] - p[0]) > tiny ? p[1] : p[2], chord);
    return p[0] + d * (u * total);
  }
  if (u >= 1.0f) {
    const float2 d = direction(length(p[2] - p[3]) > tiny ? p[2] : p[1], p[3], chord);
    return p[3] + d * ((u - 1.0f) * total);
  }
  const float want = u * total;
  int j = 1;
  while (j < PIECES && lengths[j] < want) {
    j++;
  }
  const float piece = lengths[j] - lengths[j - 1];
  const float f = piece > 1e-12f ? (want - lengths[j - 1]) / piece : 0.0f;
  return bezier(p, (float(j - 1) + f) / PIECES);
}

float2 path_control_value(float2 a, float2 b, float2 stored)
{
  const float2 d = b - a;
  if (length(d) < 1e-6f) {
    /* The keys on top of each other: kept as an offset from them. */
    return a + stored;
  }
  return a + d * stored.x + float2{-d.y, d.x} * stored.y;
}

float2 path_control_stored(float2 a, float2 b, float2 value)
{
  const float2 d = b - a, v = value - a;
  const float l2 = d.x * d.x + d.y * d.y;
  if (l2 < 1e-12f) {
    return v;
  }
  return float2{(v.x * d.x + v.y * d.y) / l2, (v.y * d.x - v.x * d.y) / l2};
}

float2 path_control_offset_value(float2 a, float2 b, float2 stored)
{
  return path_control_value(a, b, stored) - a;
}

float2 path_control_offset_stored(float2 a, float2 b, float2 offset)
{
  return path_control_stored(a, b, a + offset);
}

PathControlHandles path_control_handles_of(const Key<float2> &key, size_t j)
{
  return j < key.path_control_handles.size() ? key.path_control_handles[j] : PathControlHandles{};
}

/* Control point `j` of the stretch from `a` arriving at `b`: where it is and its handles as offsets. */
static std::pair<float2, PathControlHandles> path_control_placed(const Key<float2> &a, const Key<float2> &b, size_t j)
{
  PathControlHandles h = path_control_handles_of(b, j);
  h.in = path_control_offset_value(a.value, b.value, h.in);
  h.out = path_control_offset_value(a.value, b.value, h.out);
  return {path_control_value(a.value, b.value, b.path_controls[j]), h};
}

/* Stores placed control points on `b`, for the stretch from `a`. */
static void path_controls_store(const Key<float2> &a, Key<float2> &b,
                                const std::vector<std::pair<float2, PathControlHandles>> &placed)
{
  b.path_controls.clear();
  b.path_control_handles.clear();
  for (auto [at, h] : placed) {
    b.path_controls.push_back(path_control_stored(a.value, b.value, at));
    h.in = path_control_offset_stored(a.value, b.value, h.in);
    h.out = path_control_offset_stored(a.value, b.value, h.out);
    b.path_control_handles.push_back(h);
  }
}

void key_controls_split(std::vector<Key<float2>> &keys, size_t i)
{
  if (i == 0 || i + 1 >= keys.size() || keys[i + 1].path_controls.empty()) {
    return;
  }
  const Key<float2> &a = keys[i - 1];
  Key<float2> &k = keys[i], &b = keys[i + 1];
  std::vector<std::pair<float2, PathControlHandles>> placed;
  std::vector<float2> line = {a.value};
  for (size_t j = 0; j < b.path_controls.size(); j++) {
    placed.push_back(path_control_placed(a, b, j));
    line.push_back(placed.back().first);
  }
  line.push_back(b.value);
  /* The new key's place along the line through them: the nearest segment's. */
  size_t segment = 0;
  float nearest = std::numeric_limits<float>::max();
  for (size_t s = 0; s + 1 < line.size(); s++) {
    const float2 d = line[s + 1] - line[s];
    const float l2 = d.x * d.x + d.y * d.y;
    const float t = l2 > 1e-12f ? std::clamp(((k.value.x - line[s].x) * d.x + (k.value.y - line[s].y) * d.y) / l2, 0.0f, 1.0f)
                                : 0.0f;
    const float dist = length(line[s] + d * t - k.value);
    if (dist < nearest) {
      nearest = dist;
      segment = s;
    }
  }
  /* Control points before the segment (line[1..segment]) end the stretch into the new key. */
  const std::vector<std::pair<float2, PathControlHandles>> before(placed.begin(), placed.begin() + long(segment)),
      after(placed.begin() + long(segment), placed.end());
  path_controls_store(a, k, before);
  path_controls_store(k, b, after);
  keys_changed();
}

float stretch_path_length(const std::vector<Key<float2>> &keys, size_t i)
{
  if (i == 0 || i >= keys.size()) {
    return 0.0f;
  }
  const auto polyline = [](const std::vector<float2> &line) {
    float total = 0.0f;
    for (size_t j = 1; j < line.size(); j++) {
      total += length(line[j] - line[j - 1]);
    }
    return total;
  };
  if (!keys[i].path_controls.empty()) {
    return polyline(stretch_control_line(keys, i));
  }
  /* As bezier_along measures it. */
  float2 in, out, unused;
  key_path_handles(keys, i - 1, unused, out);
  key_path_handles(keys, i, in, unused);
  const float2 p[4] = {keys[i - 1].value, keys[i - 1].value + out, keys[i].value + in, keys[i].value};
  constexpr int PIECES = 32;
  std::vector<float2> line;
  for (int j = 0; j <= PIECES; j++) {
    line.push_back(bezier(p, float(j) / PIECES));
  }
  return polyline(line);
}

/* Where `v` is on the curve through `s`: the piece and its parameter there. False where it isn't on it (a dot past a
 * key, in an overshoot) or is at one of the curve's points. */
static bool curve_place(const StretchPoints &s, float2 v, size_t &r_piece, float &r_t)
{
  if (s.p.size() < 2 || s.in.size() != s.p.size() || s.out.size() != s.p.size()) {
    return false;
  }
  const size_t n = s.p.size() - 1;
  const auto piece = [&](size_t j, float t) {
    const float2 q[4] = {s.p[j], s.p[j] + s.out[j], s.p[j + 1] + s.in[j + 1], s.p[j + 1]};
    return bezier(q, t);
  };
  size_t at_piece = 0;
  float at_t = 0.0f, best = std::numeric_limits<float>::max(), size = 0.0f;
  constexpr int SAMPLES = 64;
  for (size_t j = 0; j < n; j++) {
    size += length(s.p[j + 1] - s.p[j]);
    for (int k = 0; k <= SAMPLES; k++) {
      const float d = length(piece(j, float(k) / SAMPLES) - v);
      if (d < best) {
        best = d;
        at_piece = j;
        at_t = float(k) / SAMPLES;
      }
    }
  }
  float lo = std::max(at_t - 1.0f / SAMPLES, 0.0f), hi = std::min(at_t + 1.0f / SAMPLES, 1.0f);
  for (int it = 0; it < 40; it++) {
    const float m1 = lo + (hi - lo) / 3.0f, m2 = hi - (hi - lo) / 3.0f;
    if (length(piece(at_piece, m1) - v) < length(piece(at_piece, m2) - v)) {
      hi = m2;
    }
    else {
      lo = m1;
    }
  }
  at_t = 0.5f * (lo + hi);
  best = length(piece(at_piece, at_t) - v);
  if (best > 1e-3f * std::max(size, 1e-6f) || at_t <= 1e-4f || at_t >= 1.0f - 1e-4f) {
    return false;
  }
  r_piece = at_piece;
  r_t = at_t;
  return true;
}

/* Piece `j` of `s` split at `t` (de Casteljau), as points: its start's new handle tip `a1`, the split `m` with its
 * handle tips `a2` and `b1`, its end's new handle tip `b2`. */
struct CurveSplit {
  float2 a1, a2, m, b1, b2;
};
static CurveSplit curve_split(const StretchPoints &s, size_t j, float t)
{
  const float2 P0 = s.p[j], P1 = s.p[j] + s.out[j], P2 = s.p[j + 1] + s.in[j + 1], P3 = s.p[j + 1];
  CurveSplit c;
  c.a1 = P0 + (P1 - P0) * t;
  const float2 mid = P1 + (P2 - P1) * t;
  c.b2 = P2 + (P3 - P2) * t;
  c.a2 = c.a1 + (mid - c.a1) * t;
  c.b1 = mid + (c.b2 - mid) * t;
  c.m = c.a2 + (c.b1 - c.a2) * t;
  return c;
}

/* A key's handle as stretch_points reads it: the Bezier's own with no control points, else a share of its piece. */
static float2 stretch_key_handle(const StretchPoints &s, bool start)
{
  const size_t m = s.p.size() - 1;
  if (m == 1) {
    return start ? s.out[0] : s.in[1];
  }
  float sum = 0.0f;
  for (size_t j = 0; j < m; j++) {
    sum += length(s.p[j + 1] - s.p[j]);
  }
  const float chord = start ? length(s.p[1] - s.p[0]) : length(s.p[m] - s.p[m - 1]);
  if (chord < 1e-9f) {
    return float2{0.0f, 0.0f};
  }
  return (start ? s.out[0] : s.in[m]) * (sum / chord);
}

/* A key frozen from Auto (Broken) is Aligned again where its handles are in line, as Auto's smooth ones are. */
static void key_aligned_again(Key<float2> &key, bool was_broken)
{
  const float li = length(key.path_in), lo = length(key.path_out);
  const float cross = key.path_in.x * key.path_out.y - key.path_in.y * key.path_out.x;
  if (!was_broken && li > 1e-9f && lo > 1e-9f && dot(key.path_in, key.path_out) < 0.0f &&
      std::abs(cross) <= 1e-4f * li * lo)
  {
    key.path_handles = PathHandles::Aligned;
  }
}

bool key_control_split(std::vector<Key<float2>> &keys, size_t i, float2 v, size_t *r_index)
{
  if (i == 0 || i >= keys.size()) {
    return false;
  }
  StretchPoints s = stretch_points(keys, i);
  size_t J = 0;
  float t = 0.0f;
  if (!curve_place(s, v, J, t)) {
    return false;
  }
  const CurveSplit c = curve_split(s, J, t);
  s.out[J] = c.a1 - s.p[J];
  s.in[J + 1] = c.b2 - s.p[J + 1];
  s.p.insert(s.p.begin() + std::ptrdiff_t(J + 1), c.m);
  s.in.insert(s.in.begin() + std::ptrdiff_t(J + 1), c.a2 - c.m);
  s.out.insert(s.out.begin() + std::ptrdiff_t(J + 1), c.b1 - c.m);
  const bool a_broken = keys[i - 1].path_handles == PathHandles::Broken;
  const bool b_broken = keys[i].path_handles == PathHandles::Broken;
  key_path_handles_freeze(keys, i - 1);
  key_path_handles_freeze(keys, i);
  Key<float2> &a = keys[i - 1], &b = keys[i];
  const Key<float2> b_was = b;
  /* Every control point's handles set as they ran: smooth ones would turn with a new neighbour. */
  b.path_controls.clear();
  b.path_control_handles.clear();
  for (size_t j = 1; j + 1 < s.p.size(); j++) {
    PathControlHandles h;
    h.set = true;
    h.aligned = true;
    if (j - 1 != J) {
      const PathControlHandles was = path_control_handles_of(b_was, j - 1 < J ? j - 1 : j - 2);
      h.aligned = was.set ? was.aligned : true;
    }
    h.in = path_control_offset_stored(a.value, b.value, s.in[j]);
    h.out = path_control_offset_stored(a.value, b.value, s.out[j]);
    b.path_controls.push_back(path_control_stored(a.value, b.value, s.p[j]));
    b.path_control_handles.push_back(h);
  }
  a.path_out = stretch_key_handle(s, true);
  key_aligned_again(a, a_broken);
  b.path_in = stretch_key_handle(s, false);
  key_aligned_again(b, b_broken);
  keys_changed();
  if (r_index) {
    *r_index = J;
  }
  return true;
}

bool key_curve_split(std::vector<Key<float2>> &keys, size_t i, const StretchPoints &old)
{
  if (i == 0 || i + 1 >= keys.size()) {
    return false;
  }
  Key<float2> &a = keys[i - 1], &k = keys[i], &b = keys[i + 1];
  const size_t n = old.p.size() - 1;
  /* Where on the curve the key is: the nearest piece and place on it. */
  size_t J = 0;
  float t = 0.0f;
  if (!curve_place(old, k.value, J, t)) {
    return false;
  }
  const CurveSplit c = curve_split(old, J, t);
  const float2 P0 = old.p[J], P3 = old.p[J + 1];
  const float2 A1 = c.a1, A2 = c.a2, B1 = c.b1, B2 = c.b2, M = c.m;
  /* The two stretches as points with the handles they run along. */
  StretchPoints s1, s2;
  for (size_t j = 0; j <= J; j++) {
    s1.p.push_back(old.p[j]);
    s1.in.push_back(old.in[j]);
    s1.out.push_back(old.out[j]);
  }
  s1.out[J] = A1 - P0;
  s1.p.push_back(M);
  s1.in.push_back(A2 - M);
  s1.out.push_back(float2{0.0f, 0.0f});
  s2.p.push_back(M);
  s2.in.push_back(float2{0.0f, 0.0f});
  s2.out.push_back(B1 - M);
  for (size_t j = J + 1; j <= n; j++) {
    s2.p.push_back(old.p[j]);
    s2.in.push_back(old.in[j]);
    s2.out.push_back(old.out[j]);
  }
  s2.in[1] = B2 - P3;
  const bool a_broken = a.path_handles == PathHandles::Broken, b_broken = b.path_handles == PathHandles::Broken;
  key_path_handles_freeze(keys, i - 1);
  key_path_handles_freeze(keys, i + 1);
  /* Control points: each side's, their handles set as they ran (so a new neighbour doesn't turn them). */
  const Key<float2> b_was = b;
  const auto controls_from_old = [&](const Key<float2> &from, Key<float2> &to, const StretchPoints &s,
                                     size_t first_old) {
    to.path_controls.clear();
    to.path_control_handles.clear();
    for (size_t j = 1; j + 1 < s.p.size(); j++) {
      const PathControlHandles was = path_control_handles_of(b_was, first_old + j - 1);
      PathControlHandles h;
      h.set = true;
      h.aligned = was.set ? was.aligned : true;
      h.in = path_control_offset_stored(from.value, to.value, s.in[j]);
      h.out = path_control_offset_stored(from.value, to.value, s.out[j]);
      to.path_controls.push_back(path_control_stored(from.value, to.value, s.p[j]));
      to.path_control_handles.push_back(h);
    }
  };
  controls_from_old(a, k, s1, 0);
  controls_from_old(k, b, s2, J);
  a.path_out = stretch_key_handle(s1, true);
  key_aligned_again(a, a_broken);
  k.path_in = stretch_key_handle(s1, false);
  k.path_out = stretch_key_handle(s2, true);
  k.path_handles = PathHandles::Aligned;
  b.path_in = stretch_key_handle(s2, false);
  key_aligned_again(b, b_broken);
  keys_changed();
  return true;
}

/* Control points `from` .. `to` - 1 of `s` (their handles as they ran) stored on `key` for the stretch from `start`;
 * `was_index(j)` is each one's index in `was`, or -1 for one that wasn't a control point. */
static void controls_store_from(const Key<float2> &start, Key<float2> &key, const StretchPoints &s, size_t from,
                                size_t to, const Key<float2> &was, const std::function<int(size_t)> &was_index)
{
  key.path_controls.clear();
  key.path_control_handles.clear();
  for (size_t j = from; j < to; j++) {
    PathControlHandles h;
    h.set = true;
    h.aligned = true;
    if (const int w = was_index(j); w >= 0) {
      const PathControlHandles old = path_control_handles_of(was, size_t(w));
      h.aligned = old.set ? old.aligned : true;
    }
    h.in = path_control_offset_stored(start.value, key.value, s.in[j]);
    h.out = path_control_offset_stored(start.value, key.value, s.out[j]);
    key.path_controls.push_back(path_control_stored(start.value, key.value, s.p[j]));
    key.path_control_handles.push_back(h);
  }
}

bool key_overshoot_split(std::vector<Key<float2>> &keys, size_t i, const StretchPoints &old, bool past_end)
{
  if (i == 0 || i + 1 >= keys.size() || old.p.size() < 2 || old.in.size() != old.p.size() ||
      old.out.size() != old.p.size())
  {
    return false;
  }
  const bool a_broken = keys[i - 1].path_handles == PathHandles::Broken;
  const bool b_broken = keys[i + 1].path_handles == PathHandles::Broken;
  key_path_handles_freeze(keys, i - 1);
  key_path_handles_freeze(keys, i + 1);
  Key<float2> &a = keys[i - 1], &k = keys[i], &b = keys[i + 1];
  const Key<float2> b_was = b;
  const float2 v = k.value;
  const size_t n = old.p.size() - 1;
  StretchPoints s = old;
  if (past_end) {
    /* Into the key: the old curve, its end now a control point, then on straight to where the dot went. */
    s.out[n] = (v - s.p[n]) / 3.0f;
    s.p.push_back(v);
    s.in.push_back((old.p[n] - v) / 3.0f);
    s.out.push_back(float2{0.0f, 0.0f});
    controls_store_from(a, k, s, 1, n + 1, b_was, [&](size_t j) { return j < n ? int(j) - 1 : -1; });
    a.path_out = stretch_key_handle(s, true);
    k.path_in = stretch_key_handle(s, false);
    /* And straight back. */
    k.path_out = (b.value - v) / 3.0f;
    b.path_in = (v - b.value) / 3.0f;
    b.path_controls.clear();
    b.path_control_handles.clear();
  }
  else {
    /* Out to where the dot went, straight; then back through the old start, now a control point, along the old
     * curve. */
    s.p.insert(s.p.begin(), v);
    s.in.insert(s.in.begin(), float2{0.0f, 0.0f});
    s.out.insert(s.out.begin(), (old.p[0] - v) / 3.0f);
    s.in[1] = (v - old.p[0]) / 3.0f;
    a.path_out = (v - a.value) / 3.0f;
    k.path_in = (a.value - v) / 3.0f;
    k.path_controls.clear();
    k.path_control_handles.clear();
    controls_store_from(k, b, s, 1, n + 1, b_was, [&](size_t j) { return j > 1 ? int(j) - 2 : -1; });
    k.path_out = stretch_key_handle(s, true);
    b.path_in = stretch_key_handle(s, false);
  }
  /* It turns back on itself there. */
  k.path_handles = PathHandles::Broken;
  key_aligned_again(a, a_broken);
  key_aligned_again(b, b_broken);
  keys_changed();
  return true;
}

void key_controls_join(std::vector<Key<float2>> &keys, size_t i)
{
  if (i == 0 || i + 1 >= keys.size() || (keys[i].path_controls.empty() && keys[i + 1].path_controls.empty())) {
    return;
  }
  const Key<float2> &a = keys[i - 1], &k = keys[i];
  Key<float2> &b = keys[i + 1];
  std::vector<std::pair<float2, PathControlHandles>> placed;
  for (size_t j = 0; j < k.path_controls.size(); j++) {
    placed.push_back(path_control_placed(a, k, j));
  }
  for (size_t j = 0; j < b.path_controls.size(); j++) {
    placed.push_back(path_control_placed(k, b, j));
  }
  path_controls_store(a, b, placed);
  keys_changed();
}

StretchPoints stretch_points(const std::vector<Key<float2>> &keys, size_t i)
{
  StretchPoints s;
  if (i == 0 || i >= keys.size()) {
    return s;
  }
  const Key<float2> &a = keys[i - 1], &b = keys[i];
  s.p.push_back(a.value);
  for (const float2 c : b.path_controls) {
    s.p.push_back(path_control_value(a.value, b.value, c));
  }
  s.p.push_back(b.value);
  const size_t n = s.p.size() - 1;
  std::vector<float> chord(n);
  float sum = 0.0f;
  for (size_t j = 0; j < n; j++) {
    chord[j] = length(s.p[j + 1] - s.p[j]);
    sum += chord[j];
  }
  sum = std::max(sum, 1e-9f);
  /* Through the points smoothly (Catmull-Rom between control points); at the keys along their handles, each a third of
   * its piece as a Bezier's, so the path leaves and reaches the keys as it would without them. */
  float2 in, out, unused;
  key_path_handles(keys, i - 1, unused, out);
  key_path_handles(keys, i, in, unused);
  std::vector<float2> tangent(n + 1);
  for (size_t j = 1; j < n; j++) {
    tangent[j] = (s.p[j + 1] - s.p[j - 1]) * 0.5f;
  }
  tangent[0] = length(out) > 1e-9f ? out * (3.0f * chord[0] / sum) : (s.p[1] - s.p[0]);
  tangent[n] = length(in) > 1e-9f ? in * (-3.0f * chord[n - 1] / sum) : (s.p[n] - s.p[n - 1]);
  if (n == 1) {
    /* No control points: the Bezier's own handles. */
    tangent[0] = out * 3.0f;
    tangent[1] = in * -3.0f;
  }
  for (size_t j = 0; j <= n; j++) {
    s.in.push_back(tangent[j] * (-1.0f / 3.0f));
    s.out.push_back(tangent[j] * (1.0f / 3.0f));
  }
  /* A control point's own handles, once set. */
  for (size_t j = 1; j < n; j++) {
    const PathControlHandles h = path_control_handles_of(b, j - 1);
    if (h.set) {
      s.in[j] = path_control_offset_value(a.value, b.value, h.in);
      s.out[j] = path_control_offset_value(a.value, b.value, h.out);
    }
  }
  return s;
}

std::vector<float2> stretch_control_line(const std::vector<Key<float2>> &keys, size_t i, int per)
{
  std::vector<float2> line;
  if (i == 0 || i >= keys.size()) {
    return line;
  }
  const StretchPoints sp = stretch_points(keys, i);
  const std::vector<float2> &p = sp.p;
  const size_t n = p.size() - 1;
  line.push_back(p[0]);
  for (size_t j = 0; j < n; j++) {
    const float2 q[4] = {p[j], p[j] + sp.out[j], p[j + 1] + sp.in[j + 1], p[j + 1]};
    for (int s = 1; s <= per; s++) {
      const float t = float(s) / float(per), m = 1.0f - t;
      line.push_back(q[0] * (m * m * m) + q[1] * (3.0f * m * m * t) + q[2] * (3.0f * m * t * t) + q[3] * (t * t * t));
    }
  }
  return line;
}

/* The point `u` of the way along `line` by length; past either end (overshoot) it carries on along the end's
 * direction, as a stretch without control points does. */
static float2 line_at(const std::vector<float2> &line, float u)
{
  float total = 0.0f;
  for (size_t j = 1; j < line.size(); j++) {
    total += length(line[j] - line[j - 1]);
  }
  const auto beyond = [&](size_t from, size_t to, float by) {
    const float2 d = line[to] - line[from];
    const float l = length(d);
    return l > 1e-12f ? line[to] + d * (by * total / l) : line[to];
  };
  if (line.size() >= 2 && u < 0.0f) {
    return beyond(1, 0, -u);
  }
  if (line.size() >= 2 && u > 1.0f) {
    return beyond(line.size() - 2, line.size() - 1, u - 1.0f);
  }
  float want = std::clamp(u, 0.0f, 1.0f) * total;
  for (size_t j = 1; j < line.size(); j++) {
    const float l = length(line[j] - line[j - 1]);
    if (want <= l && l > 1e-12f) {
      return line[j - 1] + (line[j] - line[j - 1]) * (want / l);
    }
    want -= l;
  }
  return line.back();
}

float stretch_smooth_progress(const std::vector<Key<float2>> &keys, size_t i, int frame)
{
  if (i == 0 || i >= keys.size()) {
    return 0.0f;
  }
  const Key<float2> &a = keys[i - 1], &b = keys[i];
  const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
  if (frame <= start) {
    return 0.0f;
  }
  const int interval = std::max(a.interval, 1);
  const int f = std::min(start + (frame - start) / interval * interval, b.frame);
  const float t = float(f - start) / float(std::max(b.frame - start, 1));
  /* Smooth runs along the Bezier its handles make by the curve's own timing: as far along it by length as that
   * Bezier has gone at the frame's place on it, measured finely (a straight piece per frame cuts its corners). */
  float2 in, out, unused;
  key_path_handles(keys, i - 1, unused, out);
  key_path_handles(keys, i, in, unused);
  const float2 p[4] = {a.value, a.value + out, b.value + in, b.value};
  constexpr int PIECES = 256;
  float total = 0.0f, upto = 0.0f;
  float2 prev = p[0];
  for (int j = 1; j <= PIECES; j++) {
    const float from = float(j - 1) / PIECES, to = float(j) / PIECES;
    const float2 q = bezier(p, to);
    const float l = length(q - prev);
    total += l;
    upto += l * std::clamp((t - from) * PIECES, 0.0f, 1.0f);
    prev = q;
  }
  return total > 1e-9f ? upto / total : t;
}

/* While set, Channel::evaluate leaves control points out (the stretch as it would go without them). */
static thread_local bool g_controls_off = false;

/* The stretch arriving at key `i` with control points, on `frame`: as far along its path through them, by length, as
 * the stretch without them has gone along its own by then, so its timing (eases, holds, Custom speeds) is kept. */
static float2 controls_evaluate(const Channel<float2> &ch, size_t i, int frame)
{
  const Key<float2> &a = ch.keys[i - 1], &b = ch.keys[i];
  /* A mode that runs along its path by length (all but Smooth and Noisy): its own progress, signed, so Custom speeds
   * and eases space the dots as they say and an overshoot goes past the key. Measured off the path without the
   * control points, a sharp turn in that path (its handles drawn long) would bunch the dots there. */
  const Interp interp = stretch_interp(b.interp);
  if (interp != Interp::Smooth && interp != Interp::Noisy && interp != Interp::Cycle && interp != Interp::Step &&
      interp != Interp::Pose)
  {
    const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
    if (frame <= start) {
      return a.value;
    }
    const int interval = std::max(a.interval, 1);
    const int f = std::min(start + (frame - start) / interval * interval, b.frame);
    const float t = float(f - start) / float(std::max(b.frame - start, 1));
    KeySettings settings = key_settings(b);
    settings.interp = interp;
    const float u = interp == Interp::Custom ? custom_progress(b.speeds, t) : interp_curve(settings, t);
    return line_at(stretch_control_line(ch.keys, i), u);
  }
  if (interp_smooth(interp)) {
    return line_at(stretch_control_line(ch.keys, i), stretch_smooth_progress(ch.keys, i, frame));
  }
  g_controls_off = true;
  std::vector<float2> plain;
  for (int f = a.frame; f <= b.frame; f++) {
    plain.push_back(ch.evaluate(f));
  }
  g_controls_off = false;
  float total = 0.0f, upto = 0.0f;
  for (size_t j = 1; j < plain.size(); j++) {
    const float l = length(plain[j] - plain[j - 1]);
    total += l;
    if (int(j) <= frame - a.frame) {
      upto += l;
    }
  }
  const float u = total > 1e-9f ? upto / total : float(frame - a.frame) / float(std::max(b.frame - a.frame, 1));
  return line_at(stretch_control_line(ch.keys, i), u);
}

float2 stretch_path_point(const std::vector<Key<float2>> &keys, size_t i, float u)
{
  if (i == 0 || i >= keys.size()) {
    return i < keys.size() ? keys[i].value : float2{0.0f, 0.0f};
  }
  if (!keys[i].path_controls.empty()) {
    /* Through its control points, as the stretch plays (controls_evaluate). */
    return line_at(stretch_control_line(keys, i), u);
  }
  float2 in, out, unused;
  key_path_handles(keys, i - 1, unused, out);
  key_path_handles(keys, i, in, unused);
  const float2 p[4] = {keys[i - 1].value, keys[i - 1].value + out, keys[i].value + in, keys[i].value};
  return bezier_along(p, u);
}

void key_path_handle_set(std::vector<Key<float2>> &keys, size_t i, bool out, float2 handle, bool broken)
{
  if (i >= keys.size()) {
    return;
  }
  float2 in_now, out_now;
  key_path_handles(keys, i, in_now, out_now);
  Key<float2> &k = keys[i];
  if (k.path_handles == PathHandles::Auto) {
    k.path_in = in_now;
    k.path_out = out_now;
    k.path_handles = PathHandles::Aligned;
  }
  if (broken) {
    k.path_handles = PathHandles::Broken;
  }
  (out ? k.path_out : k.path_in) = handle;
  if (k.path_handles == PathHandles::Aligned) {
    float2 &other = out ? k.path_in : k.path_out;
    const float l = length(handle);
    const float keep = length(other) > 1e-9f ? length(other) : l;
    if (l > 1e-9f) {
      other = handle * (-keep / l);
    }
  }
  keys_changed();
}

const char *path_handles_identifier(PathHandles mode)
{
  switch (mode) {
    case PathHandles::Auto:
      return "AUTO";
    case PathHandles::Aligned:
      return "ALIGNED";
    case PathHandles::Broken:
      return "BROKEN";
  }
  return "AUTO";
}

bool path_handles_from_identifier(const std::string &identifier, PathHandles *r_mode)
{
  for (const PathHandles mode : {PathHandles::Auto, PathHandles::Aligned, PathHandles::Broken}) {
    if (identifier == path_handles_identifier(mode)) {
      *r_mode = mode;
      return true;
    }
  }
  return false;
}

/* A filler key that shapes nothing (Key::filler_inside, or no key of the channel's own after it; frame 0, the setup,
 * always counts), given whether a key of its own comes after it. */
template<typename T> static bool filler_quiet(const Key<T> &k, bool own_after)
{
  return k.filler && k.frame != 0 && (k.filler_inside || !own_after);
}

template<typename T> T Channel<T>::evaluate(int frame) const
{
  if (keys.empty()) {
    return last;
  }
  {
    /* Worked out without the fillers that shape nothing: the curve through the rest is as it was. */
    bool own_after = false, quiet = false;
    for (size_t i = keys.size(); i-- > 0 && !quiet;) {
      quiet = filler_quiet(keys[i], own_after);
      own_after = own_after || !keys[i].filler;
    }
    if (quiet) {
      Channel<T> real;
      real.last = last;
      own_after = false;
      for (size_t i = keys.size(); i-- > 0;) {
        if (!filler_quiet(keys[i], own_after)) {
          real.keys.push_back(keys[i]);
        }
        own_after = own_after || !keys[i].filler;
      }
      std::reverse(real.keys.begin(), real.keys.end());
      return real.evaluate(frame);
    }
  }
  if (frame <= keys.front().frame) {
    return keys.front().value;
  }
  /* The key at or before `frame`. */
  const auto after = std::upper_bound(keys.begin(), keys.end(), frame,
                                      [](int f, const Key<T> &k) { return f < k.frame; });
  const int k = int(after - keys.begin()) - 1;
  const Key<T> &a = keys[k];

  if (g_stepped_preview) {
    /* Blocking: hold each key until the next. */
    return a.value;
  }
  if constexpr (std::is_same_v<T, float2>) {
    /* Inside a stretch with control points: along its path through them. */
    if (!g_controls_off && size_t(k) + 1 < keys.size() && !keys[size_t(k) + 1].path_controls.empty() &&
        a.interp != Interp::Cycle && frame > a.frame && frame < keys[size_t(k) + 1].frame)
    {
      return controls_evaluate(*this, size_t(k) + 1, frame);
    }
  }
  if (a.interp == Interp::Cycle) {
    /* Play again from an earlier frame, looping until the next key. */
    const int start = a.cycle_absolute ? a.cycle_frame : (a.cycle_back > 0 ? a.frame - a.cycle_back : keys.front().frame);
    const int length = a.frame - start;
    if (length > 0 && start < a.frame && (k + 1 >= int(keys.size()) || frame < keys[k + 1].frame)) {
      const int again = start + (frame - a.frame) % length;
      if (again < a.frame) {
        return evaluate(again);
      }
    }
    return a.value;
  }
  if (k + 1 >= int(keys.size())) {
    return a.value;
  }
  const Key<T> &b = keys[k + 1];

  /* Moho's hold, then the interval: on 2s the motion only moves every second frame. */
  const int start = std::min(a.frame + std::max(a.hold, 0), b.frame);
  if (frame <= start) {
    return a.value;
  }
  const int interval = std::max(a.interval, 1);
  int f = start + (frame - start) / interval * interval;
  f = std::min(f, b.frame);
  const float span = float(b.frame - start);
  const float t = span > 0.0f ? float(f - start) / span : 1.0f;

  /* The stretch arriving at a key takes that key's interpolation and its settings (as Moho's: how the
   * motion gets to this pose); a Cycle key's own interpolation is its loop after it, so the stretch
   * into one is Smooth. Hold, the interval and Cycle stay the earlier key's: what happens after it. */
  const Interp interp = stretch_interp(b.interp);
  if constexpr (std::is_same_v<T, float2>) {
    /* A dragged path handle at either end: the stretch runs along the Bezier the handles make, Smooth by the curve's
     * own timing (as without handles), any other mode by its progress along the curve's length. */
    if ((a.path_handles != PathHandles::Auto || b.path_handles != PathHandles::Auto || interp == Interp::Custom ||
         interp == Interp::Bezier) &&
        interp != Interp::Step && interp != Interp::Pose)
    {
      float2 in, out, unused;
      key_path_handles(keys, size_t(k), unused, out);
      key_path_handles(keys, size_t(k) + 1, in, unused);
      const float2 p[4] = {a.value, a.value + out, b.value + in, b.value};
      if (interp_smooth(interp)) {
        return bezier(p, t);
      }
      float u = t;
      switch (interp) {
        case Interp::EaseInOut:
        case Interp::EaseIn:
        case Interp::EaseOut:
          u = ease_shaped(interp, b.ease, t);
          break;
        case Interp::Back:
          u = ease_back(t, b.overshoot);
          break;
        case Interp::Bounce:
          u = ease_bounce(t);
          break;
        case Interp::Elastic:
          u = ease_elastic(t, b.elastic_amplitude, b.elastic_period);
          break;
        case Interp::Custom:
          u = custom_progress(b.speeds, t);
          break;
        case Interp::Bezier:
          u = bezier_ease(b.bezier, t);
          break;
        default:
          break;
      }
      const float2 along = bezier_along(p, u);
      return interp == Interp::Noisy ? along + noise_of(a.value, b, float(frame)) : along;
    }
  }
  switch (interp) {
    case Interp::Step:
    case Interp::Pose:
      return a.value;
    case Interp::Linear:
      return a.value + (b.value - a.value) * t;
    case Interp::EaseInOut:
    case Interp::EaseIn:
    case Interp::EaseOut:
      return a.value + (b.value - a.value) * ease_shaped(interp, b.ease, t);
    case Interp::Back:
      return a.value + (b.value - a.value) * ease_back(t, b.overshoot);
    case Interp::Bounce:
      return a.value + (b.value - a.value) * ease_bounce(t);
    case Interp::Elastic:
      return a.value + (b.value - a.value) * ease_elastic(t, b.elastic_amplitude, b.elastic_period);
    case Interp::Noisy:
      return a.value + (b.value - a.value) * t + noise_of(a.value, b, float(frame));
    case Interp::Custom:
      return a.value + (b.value - a.value) * custom_progress(b.speeds, t);
    case Interp::Bezier:
      return a.value + (b.value - a.value) * bezier_ease(b.bezier, t);
    case Interp::Smooth:
    case Interp::Cycle:
      break;
  }
  /* Smooth: a Hermite curve whose slope at each key comes from its neighbours (a key at either
   * end uses the straight line to its one neighbour), so it eases through keys as Moho does.
   * Like Blender's auto-clamped handles, a key that is a peak, a valley or equal to a neighbour
   * is flat, so equal keys hold still and the motion never overshoots its keys. */
  const T m0 = key_slope(keys, k) * span, m1 = key_slope(keys, k + 1) * span;
  const float t2 = t * t, t3 = t2 * t;
  return a.value * (2 * t3 - 3 * t2 + 1) + m0 * (t3 - 2 * t2 + t) + b.value * (-2 * t3 + 3 * t2) + m1 * (t3 - t2);
}

template<typename T> Key<T> *Channel<T>::key_at(int frame)
{
  for (Key<T> &k : keys) {
    if (k.frame == frame) {
      return &k;
    }
  }
  return nullptr;
}

template<typename T> const Key<T> *Channel<T>::key_at(int frame) const
{
  return const_cast<Channel<T> *>(this)->key_at(frame);
}

static std::atomic<uint64_t> g_keys_generation{0};
static thread_local int g_keys_quiet = 0;

uint64_t channel_keys_generation()
{
  return g_keys_generation.load(std::memory_order_relaxed);
}

ChannelKeysQuiet::ChannelKeysQuiet()
{
  g_keys_quiet++;
}

ChannelKeysQuiet::~ChannelKeysQuiet()
{
  g_keys_quiet--;
}

static void keys_changed()
{
  if (g_keys_quiet == 0) {
    g_keys_generation.fetch_add(1, std::memory_order_relaxed);
  }
}

void channel_keys_changed()
{
  keys_changed();
}

template<typename T> Key<T> &Channel<T>::set_key(int frame, const T &value)
{
  if (Key<T> *existing = key_at(frame)) {
    if (!(existing->value == value)) {
      keys_changed();
    }
    existing->value = value;
    return *existing;
  }
  keys_changed();
  const auto at = std::lower_bound(keys.begin(), keys.end(), frame,
                                   [](const Key<T> &k, int f) { return k.frame < f; });
  Key<T> key;
  key.frame = frame;
  key.value = value;
  const NewKeySettings &defaults = new_key_settings();
  const bool copy = defaults.copy_previous && at != keys.begin();
  key_settings_apply(key, copy ? key_settings(*std::prev(at)) : defaults.settings);
  /* Its type is the new-key one even when copying the key before's interpolation. */
  key.type = frame == 0 ? KeyType::Key : defaults.settings.type;
  if (copy && key.interp == Interp::Cycle) {
    /* A cycle key ends a loop; the keys after it start out ordinary. */
    key.interp = defaults.settings.interp == Interp::Cycle ? Interp::Smooth : defaults.settings.interp;
  }
  if (constant) {
    key.interp = Interp::Step;
  }
  return *keys.insert(at, key);
}

template<typename T> void keys_curves_to_arriving(std::vector<Key<T>> &keys)
{
  /* Before, a key's curve was for the stretch after it: each key takes the one before's. A Cycle
   * key keeps its loop (it was never a stretch's curve), and the stretch after one was its loop. */
  std::vector<KeySettings> before;
  for (const Key<T> &k : keys) {
    before.push_back(key_settings(k));
  }
  for (size_t i = 0; i < keys.size(); i++) {
    if (keys[i].interp == Interp::Cycle) {
      continue;
    }
    KeySettings s = key_settings(keys[i]);
    const KeySettings from = i > 0 && before[i - 1].interp != Interp::Cycle ? before[i - 1] : KeySettings();
    s.interp = from.interp;
    s.ease = from.ease;
    s.overshoot = from.overshoot;
    s.elastic_amplitude = from.elastic_amplitude;
    s.elastic_period = from.elastic_period;
    s.noise_amplitude = from.noise_amplitude;
    s.noise_frequency = from.noise_frequency;
    s.bezier = from.bezier;
    key_settings_apply(keys[i], s);
  }
}
template void keys_curves_to_arriving(std::vector<Key<float>> &keys);
template void keys_curves_to_arriving(std::vector<Key<float2>> &keys);

template<typename T> bool Channel<T>::remove_key(int frame)
{
  const size_t before = keys.size();
  std::erase_if(keys, [frame](const Key<T> &k) { return k.frame == frame; });
  if (keys.size() != before) {
    keys_changed();
  }
  return keys.size() != before;
}

template struct Channel<float>;
template struct Channel<float2>;

}  // namespace oanim
