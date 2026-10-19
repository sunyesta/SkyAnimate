#include "anim/channel.hh"

#include <algorithm>
#include <cmath>
#include <functional>

namespace sky {

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

/* --- Easing curves, 0..1 -> 0..1 (Blender's BLI_easing ease-out forms). --------------- */

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

static float ease_elastic(float t)
{
  if (t <= 0.0f || t >= 1.0f) {
    return t <= 0.0f ? 0.0f : 1.0f;
  }
  const float period = 0.3f;
  return std::pow(2.0f, -10.0f * t) * std::sin((t - period / 4.0f) * (2.0f * PI) / period) + 1.0f;
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

  switch (a.interp) {
    case Interp::Step:
    case Interp::Pose:
      return a.value;
    case Interp::Linear:
      return a.value + (b.value - a.value) * t;
    case Interp::EaseInOut:
      return a.value + (b.value - a.value) * (t * t * (3.0f - 2.0f * t));
    case Interp::EaseIn:
      /* Slows only on arrival. */
      return a.value + (b.value - a.value) * (t * (2.0f - t));
    case Interp::EaseOut:
      /* Slows only on departure. */
      return a.value + (b.value - a.value) * (t * t);
    case Interp::Back:
      return a.value + (b.value - a.value) * ease_back(t, a.overshoot);
    case Interp::Bounce:
      return a.value + (b.value - a.value) * ease_bounce(t);
    case Interp::Elastic:
      return a.value + (b.value - a.value) * ease_elastic(t);
    case Interp::Noisy:
      return a.value + (b.value - a.value) * t + noise_of(a.value, a, float(frame));
    case Interp::Smooth:
    case Interp::Bezier:
    case Interp::Cycle:
      break;
  }
  /* Smooth: a Hermite curve whose slope at each key comes from its neighbours (a key at either
   * end uses the straight line to its one neighbour), so it eases through keys as Moho does.
   * Like Blender's auto-clamped handles, a key that is a peak, a valley or equal to a neighbour
   * is flat, so equal keys hold still and the motion never overshoots its keys. */
  const auto slope = [&](int i) {
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
  };
  const T m0 = slope(k) * span, m1 = slope(k + 1) * span;
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

template<typename T> Key<T> &Channel<T>::set_key(int frame, const T &value, Interp default_interp)
{
  if (Key<T> *existing = key_at(frame)) {
    existing->value = value;
    return *existing;
  }
  const auto at = std::lower_bound(keys.begin(), keys.end(), frame,
                                   [](const Key<T> &k, int f) { return k.frame < f; });
  Key<T> key;
  key.frame = frame;
  key.value = value;
  key.interp = at != keys.begin() ? std::prev(at)->interp : default_interp;
  if (key.interp == Interp::Cycle) {
    /* A cycle key ends a loop; the keys after it start out ordinary. */
    key.interp = default_interp;
  }
  return *keys.insert(at, key);
}

template<typename T> bool Channel<T>::remove_key(int frame)
{
  const size_t before = keys.size();
  std::erase_if(keys, [frame](const Key<T> &k) { return k.frame == frame; });
  return keys.size() != before;
}

template struct Channel<float>;
template struct Channel<float2>;

}  // namespace sky
