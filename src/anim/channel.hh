#pragma once

/* Animated channels, following Moho's AnimChannel: keys at whole frames, each with an
 * interpolation mode for the stretch from it to the next key, plus Moho's per-key hold and
 * interval (animating on 2s or 3s).
 *
 * A channel sits beside the plain value it animates (a layer's position, a point's
 * position). The plain value always holds the value at the frame last evaluated, so drawing
 * and editing code never looks at keys; see anim/animation.hh for how edits become keys. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/math.hh"

namespace sky {

/* Moho's interpolation modes (MOHO.INTERP_*), plus Blender's Back easing. */
enum class Interp : uint8_t {
  Linear,
  Smooth,
  EaseInOut,
  EaseIn,
  EaseOut,
  Step,
  /* A custom curve; until the graph editor edits its handles it moves like Smooth. */
  Bezier,
  Back,
  Bounce,
  Elastic,
  Noisy,
  Cycle,
  /* Holds a pose from an action; until actions arrive (milestone 4) it holds like Step. */
  Pose,
};
inline constexpr int INTERP_COUNT = 13;

const char *interp_identifier(Interp interp);
const char *interp_name(Interp interp);
bool interp_from_identifier(const std::string &identifier, Interp *r_interp);

template<typename T> struct Key {
  int frame = 0;
  T value{};
  Interp interp = Interp::Smooth;
  /* Frames the value waits at this key before moving on. */
  int hold = 0;
  /* 1 animates on 1s; 2 or 3 steps the motion every 2 or 3 frames. */
  int interval = 1;
  /* Back: how far it overshoots. */
  float overshoot = 1.70158f;
  /* Noisy: jitter size (in the channel's units) and how many wiggles per frame. */
  float noise_amplitude = 0.1f;
  float noise_frequency = 0.5f;
  /* Cycle: after this key, play again from `cycle_back` frames earlier (0: the first key), or
   * from frame `cycle_frame` when `cycle_absolute`. */
  int cycle_back = 0;
  bool cycle_absolute = false;
  int cycle_frame = 0;
  /* Moho's key colour tag (0 = none). */
  int color = 0;
  /* Picked in the timeline. Not saved. */
  bool selected = false;
};

template<typename T> struct Channel {
  /* Sorted by frame. Empty: not animated, the plain value is all there is. */
  std::vector<Key<T>> keys;
  /* The value as last evaluated or recorded: an edit is a plain value that differs from it. */
  T last{};
  bool has_last = false;
  /* Keys of the timelines not being edited, by action name, as Moho keeps actions inside each
   * channel. The timeline being edited is always `keys`; while an action is edited the mainline's
   * keys wait here under "". Action keys start after frame 0, which is the shared setup pose. */
  std::map<std::string, std::vector<Key<T>>> actions;
  /* What smart actions add to the value right now (it is in the plain value too). Edits and saving
   * take it off, so it never becomes part of the data. */
  T smart{};
  /* Auto-keying leaves it alone until it is animated (keyed by hand first): a layer's visibility,
   * so hiding a layer while working doesn't key it. */
  bool keyed_by_hand = false;
  /* A yes/no property (visibility): its keys only hold, as Blender's Constant interpolation. */
  bool constant = false;

  bool animated() const { return !keys.empty(); }
  T evaluate(int frame) const;
  Key<T> *key_at(int frame);
  const Key<T> *key_at(int frame) const;
  /* Sets the value at `frame`, adding a key there if needed. A new key copies the mode of
   * the key before it, or uses `default_interp`. */
  Key<T> &set_key(int frame, const T &value, Interp default_interp = Interp::Smooth);
  bool remove_key(int frame);
};

extern template struct Channel<float>;
extern template struct Channel<float2>;

}  // namespace sky
