#pragma once

/* Animated channels, following Moho's AnimChannel: keys at whole frames, each with an
 * interpolation mode for the stretch arriving at it from the key before (how the motion gets to
 * this pose), plus Moho's per-key hold and interval (animating on 2s or 3s) for after it.
 *
 * A channel sits beside the plain value it animates (a layer's position, a point's
 * position). The plain value always holds the value at the frame last evaluated, so drawing
 * and editing code never looks at keys; see anim/animation.hh for how edits become keys. */

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/math.hh"

namespace oanim {

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
  /* Painted with the Motion Path tool's speed brushes: a speed for each frame of the stretch (Key::speeds), the
   * value going along the same way (a position along its path) at that speed, arriving on the key's frame. */
  Custom,
};
inline constexpr int INTERP_COUNT = 14;

const char *interp_identifier(Interp interp);
const char *interp_name(Interp interp);
bool interp_from_identifier(const std::string &identifier, Interp *r_interp);

/* How strongly Ease In, Ease Out and Ease In/Out slow down: Robert Penner's easing families, from
 * the gentlest (Sine) to the strongest (Expo). */
enum class Ease : uint8_t { Sine, Quad, Cubic, Quart, Quint, Expo, Circ };
inline constexpr int EASE_COUNT = 7;

const char *ease_identifier(Ease ease);
const char *ease_name(Ease ease);
bool ease_from_identifier(const std::string &identifier, Ease *r_ease);

/* A position key's path handles (the Motion Path tool's): Auto until one is dragged, then Aligned (the two in a line,
 * as a smooth point's) or Broken (Alt: each its own way). */
enum class PathHandles : uint8_t { Auto, Aligned, Broken };

template<typename T> struct Key {
  int frame = 0;
  T value{};
  Interp interp = Interp::Smooth;
  /* Frames the value waits at this key before moving on. */
  int hold = 0;
  /* 1 animates on 1s; 2 or 3 steps the motion every 2 or 3 frames. */
  int interval = 1;
  /* Ease In, Ease Out, Ease In/Out: how strongly. */
  Ease ease = Ease::Quad;
  /* Back: how far it overshoots. */
  float overshoot = 1.70158f;
  /* Elastic: how far the first swing goes past (1: as far as the move) and how long each wobble
   * is, as a share of the move. */
  float elastic_amplitude = 1.0f;
  float elastic_period = 0.3f;
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
  /* Position keys only (Channel<float2>): how the path leaves this key toward the next (path_out) and arrives from
   * the one before (path_in), as offsets from its value, as a Bezier's handles. Unused while Auto, when the path runs
   * as it always has (key_path_handles works out what they would be). */
  PathHandles path_handles = PathHandles::Auto;
  T path_in{}, path_out{};
  /* Custom: how fast the stretch arriving at this key goes on each of its frames (any scale: only how they compare
   * counts; one for each frame from the key before, after its hold). Empty: evenly. */
  std::vector<float> speeds;
  /* Picked in the timeline. Not saved. */
  bool selected = false;
  bool operator==(const Key &) const = default;
};

/* A std::map that holds nothing until something goes in. A rig has a channel for each point and
 * handle, hundreds of thousands, and nearly all have no action keys; an empty std::map still allocates
 * (MSVC's keeps a node), so copying a document for an undo step allocated once for every channel. */
template<typename K, typename V> class LazyMap {
 public:
  using Map = std::map<K, V>;
  using iterator = typename Map::iterator;
  using const_iterator = typename Map::const_iterator;
  using value_type = typename Map::value_type;

  LazyMap() = default;
  LazyMap(const LazyMap &other) : map_(other.map_ && !other.map_->empty() ? std::make_unique<Map>(*other.map_) : nullptr) {}
  LazyMap(LazyMap &&) noexcept = default;
  LazyMap &operator=(const LazyMap &other)
  {
    if (this != &other) {
      map_ = other.map_ && !other.map_->empty() ? std::make_unique<Map>(*other.map_) : nullptr;
    }
    return *this;
  }
  LazyMap &operator=(LazyMap &&) noexcept = default;

  bool empty() const { return !map_ || map_->empty(); }
  size_t size() const { return map_ ? map_->size() : 0; }
  size_t count(const K &key) const { return map_ ? map_->count(key) : 0; }
  bool contains(const K &key) const { return map_ && map_->count(key) > 0; }
  void clear() { map_.reset(); }

  /* With nothing held, iterators come from one shared empty map, so find() == end() holds. */
  iterator begin() { return map_ ? map_->begin() : none().begin(); }
  iterator end() { return map_ ? map_->end() : none().end(); }
  const_iterator begin() const { return map_ ? map_->cbegin() : none().cbegin(); }
  const_iterator end() const { return map_ ? map_->cend() : none().cend(); }
  iterator find(const K &key) { return map_ ? map_->find(key) : none().end(); }
  const_iterator find(const K &key) const { return map_ ? map_->find(key) : none().cend(); }

  V &operator[](const K &key) { return own()[key]; }
  V &at(const K &key) { return own().at(key); }
  const V &at(const K &key) const { return map_ ? map_->at(key) : none().at(key); }
  template<typename... A> std::pair<iterator, bool> emplace(A &&...args) { return own().emplace(std::forward<A>(args)...); }
  template<typename... A> std::pair<iterator, bool> try_emplace(const K &key, A &&...args)
  {
    return own().try_emplace(key, std::forward<A>(args)...);
  }
  std::pair<iterator, bool> insert(const value_type &v) { return own().insert(v); }
  size_t erase(const K &key) { return map_ ? map_->erase(key) : 0; }
  iterator erase(iterator it) { return map_->erase(it); }
  iterator erase(const_iterator it) { return map_->erase(it); }

  bool operator==(const LazyMap &other) const
  {
    return empty() ? other.empty() : (!other.empty() && *map_ == *other.map_);
  }

 private:
  static Map &none()
  {
    static Map empty_map;
    return empty_map;
  }
  Map &own()
  {
    if (!map_) {
      map_ = std::make_unique<Map>();
    }
    return *map_;
  }
  std::unique_ptr<Map> map_;
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
  LazyMap<std::string, std::vector<Key<T>>> actions;
  /* What action bone actions add to the value right now (it is in the plain value too). Edits and saving
   * take it off, so it never becomes part of the data. */
  T from_action_bones{};
  /* An action bone action is playing into it now (its bone turned into it, the offset maybe 0 at this turn):
   * its field shows pink. Worked out with `from_action_bones`, never saved. */
  bool action_bone_live = false;
  /* Auto-keying leaves it alone until it is animated (keyed by hand first): a layer's visibility,
   * so hiding a layer while working doesn't key it. */
  bool keyed_by_hand = false;
  /* A yes/no property (visibility): its keys only hold, as Blender's Constant interpolation. */
  bool constant = false;
  /* Pinned to the timeline: listed under Pinned whatever is selected, even with no keys left
   * (the pin button beside its key button in Properties). */
  bool pinned = false;

  /* A setting rather than a pose (visibility, simulation settings): keyed by hand. */
  static Channel by_hand()
  {
    Channel c;
    c.keyed_by_hand = true;
    return c;
  }

  bool animated() const { return !keys.empty(); }
  T evaluate(int frame) const;
  Key<T> *key_at(int frame);
  const Key<T> *key_at(int frame) const;
  /* Sets the value at `frame`, adding a key there if needed. A new key gets the new-key settings
   * (new_key_settings()): the settings of the key before it with Copy Previous Key, else the
   * defaults. */
  Key<T> &set_key(int frame, const T &value);
  bool remove_key(int frame);
  bool operator==(const Channel &) const = default;
};

extern template struct Channel<float>;
extern template struct Channel<float2>;

/* Key `i`'s path handles as they shape its two stretches: its own once dragged, else the Auto ones (on a Smooth
 * stretch the slope Smooth gives it, on any other pointing a third of the way to the key at the other end, so the
 * stretch stays straight). Zero where there is no stretch. */
void key_path_handles(const std::vector<Key<float2>> &keys, size_t i, float2 &r_in, float2 &r_out);
/* Sets one of key `i`'s handles (`out`, else in) to `handle`: an Auto key takes its Auto handles first; Aligned turns
 * the other one to stay in line, keeping its length; `broken` breaks them for good. */
void key_path_handle_set(std::vector<Key<float2>> &keys, size_t i, bool out, float2 handle, bool broken);
/* Whether a stretch arriving at a key with `interp` runs along a curve through the keys (Smooth, Bezier, Custom, a
 * Cycle's lead-in) rather than straight, while its handles are Auto. */
bool interp_path_smooth(Interp interp);
/* Gives key `i` its Auto handles as its own (Broken), so its path stays as it is whatever the stretches either side
 * become. */
void key_path_handles_freeze(std::vector<Key<float2>> &keys, size_t i);

/* Custom's progress (0..1) `t` of the way through a stretch of `speeds`, each frame's taking its share. */
float custom_progress(const std::vector<float> &speeds, float t);
/* Makes the stretch arriving at key `i` Custom with `speeds`. */
template<typename T> void key_speeds_set(std::vector<Key<T>> &keys, size_t i, std::vector<float> speeds);

/* "AUTO", "ALIGNED", "BROKEN". */
const char *path_handles_identifier(PathHandles mode);
bool path_handles_from_identifier(const std::string &identifier, PathHandles *r_mode);

/* Grows each time a key is added, changed or removed (set_key, remove_key), anywhere: for what is kept until
 * keys change. */
uint64_t channel_keys_generation();
/* While one is alive (on this thread), keys set on scratch channels (a copy sampled and dropped) don't count. */
struct ChannelKeysQuiet {
  ChannelKeysQuiet();
  ~ChannelKeysQuiet();
  ChannelKeysQuiet(const ChannelKeysQuiet &) = delete;
  ChannelKeysQuiet &operator=(const ChannelKeysQuiet &) = delete;
};

/* A key's interpolation and its settings: everything but its frame, value, colour and selection. */
struct KeySettings {
  Interp interp = Interp::Smooth;
  Ease ease = Ease::Quad;
  int hold = 0;
  int interval = 1;
  float overshoot = 1.70158f;
  float elastic_amplitude = 1.0f;
  float elastic_period = 0.3f;
  float noise_amplitude = 0.1f;
  float noise_frequency = 0.5f;
  int cycle_back = 0;
  bool cycle_absolute = false;
  int cycle_frame = 0;

  bool operator==(const KeySettings &) const = default;
};

/* Keys from files before format 4, whose curves were for the stretch after each key: moved onto the
 * key after, as they are now for the stretch arriving at a key, so the motion stays the same. */
template<typename T> void keys_curves_to_arriving(std::vector<Key<T>> &keys);

template<typename T> KeySettings key_settings(const Key<T> &k)
{
  KeySettings s;
  s.interp = k.interp;
  s.ease = k.ease;
  s.hold = k.hold;
  s.interval = k.interval;
  s.overshoot = k.overshoot;
  s.elastic_amplitude = k.elastic_amplitude;
  s.elastic_period = k.elastic_period;
  s.noise_amplitude = k.noise_amplitude;
  s.noise_frequency = k.noise_frequency;
  s.cycle_back = k.cycle_back;
  s.cycle_absolute = k.cycle_absolute;
  s.cycle_frame = k.cycle_frame;
  return s;
}

template<typename T> void key_settings_apply(Key<T> &k, const KeySettings &s)
{
  k.interp = s.interp;
  k.ease = s.ease;
  k.hold = s.hold;
  k.interval = s.interval;
  k.overshoot = s.overshoot;
  k.elastic_amplitude = s.elastic_amplitude;
  k.elastic_period = s.elastic_period;
  k.noise_amplitude = s.noise_amplitude;
  k.noise_frequency = s.noise_frequency;
  k.cycle_back = s.cycle_back;
  k.cycle_absolute = s.cycle_absolute;
  k.cycle_frame = s.cycle_frame;
}

/* What new keys get (the timeline's Interpolation menu with no keys selected sets it). With
 * `copy_previous`, a new key copies the key before it instead, when there is one. Saved with the
 * preferences. */
struct NewKeySettings {
  KeySettings settings;
  bool copy_previous = false;
  bool operator==(const NewKeySettings &) const = default;
};
NewKeySettings &new_key_settings();

/* Stepped Preview: every channel plays as Step (holding each key), for blocking, without changing
 * any key. Not saved. */
bool stepped_preview();
void stepped_preview_set(bool on);

/* The shape of a mode on its own, 0..1 progress for t in 0..1, as the menus and the settings graph
 * draw it (Smooth as between two flat keys; Noisy without its jitter). */
float interp_curve(const KeySettings &s, float t);

}  // namespace oanim
