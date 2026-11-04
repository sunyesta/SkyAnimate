#include "anim/channel.hh"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <type_traits>

namespace oanim {

static constexpr const char *INTERP_IDS[INTERP_COUNT] = {"LINEAR", "SMOOTH", "EASE_IN_OUT", "EASE_IN", "EASE_OUT",
                                                         "STEP",   "BEZIER", "BACK",        "BOUNCE",  "ELASTIC",
                                                         "NOISY",  "CYCLE",  "POSE"};
static constexpr const char *INTERP_NAMES[INTERP_COUNT] = {
    "Linear", "Smooth", "Ease In/Out", "Ease In", "Ease Out", "Step", "Bezier",
    "Back",   "Bounce", "Elastic",     "Noisy",   "Cycle",    "Pose"};

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

float interp_curve(const KeySettings &s, float t)
{
  t = std::clamp(t, 0.0f, 1.0f);
  switch (s.interp) {
    case Interp::Step:
    case Interp::Pose:
      return t >= 1.0f ? 1.0f : 0.0f;
    case Interp::Linear:
    case Interp::Noisy:
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
    case Interp::Smooth:
    case Interp::Bezier:
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

static bool interp_smooth(Interp interp)
{
  return interp == Interp::Smooth || interp == Interp::Bezier;
}

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
    r_in = interp_smooth(stretch_interp(k.interp)) ? key_slope(keys, int(i)) * (-stretch_span(a, k) / 3.0f)
                                                    : (a.value - k.value) / 3.0f;
  }
  if (i + 1 < keys.size()) {
    const Key<float2> &b = keys[i + 1];
    r_out = interp_smooth(stretch_interp(b.interp)) ? key_slope(keys, int(i)) * (stretch_span(k, b) / 3.0f)
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
  if (u <= 0.0f) {
    const float2 d = direction(p[0], length(p[1] - p[0]) > 1e-9f ? p[1] : p[2], chord);
    return p[0] + d * (u * total);
  }
  if (u >= 1.0f) {
    const float2 d = direction(length(p[2] - p[3]) > 1e-9f ? p[2] : p[1], p[3], chord);
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

template<typename T> T Channel<T>::evaluate(int frame) const
{
  if (keys.empty()) {
    return last;
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
    if ((a.path_handles != PathHandles::Auto || b.path_handles != PathHandles::Auto) && interp != Interp::Step &&
        interp != Interp::Pose)
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
    case Interp::Smooth:
    case Interp::Bezier:
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
