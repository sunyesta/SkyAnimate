#pragma once

/* Animated channels, following Moho's AnimChannel: keys at whole frames, each with an
 * interpolation mode for the stretch arriving at it from the key before (how the motion gets to
 * this pose), plus Moho's per-key hold and interval (animating on 2s or 3s) for after it.
 *
 * A channel sits beside the plain value it animates (a layer's position, a point's
 * position). The plain value always holds the value at the frame last evaluated, so drawing
 * and editing code never looks at keys; see anim/animation.hh for how edits become keys. */

#include <array>
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
  /* An ease shaped by two handles (Key::bezier), as a CSS cubic-bezier: the same curve fits a stretch of any length,
   * so one shape can be given to many keys. Position keys go along the path Smooth makes, at this timing. */
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

/* Bezier's handles: {x1, y1, x2, y2}, the handle leaving the key before and the one arriving at this key, in time (x,
 * 0..1 across the stretch) and progress (y, 0 at the key before and 1 at this key; past either end overshoots). */
using BezierHandles = std::array<float, 4>;
inline constexpr BezierHandles BEZIER_DEFAULT = {0.42f, 0.0f, 0.58f, 1.0f};
/* The presets: Even (steady), In (slows into this key), Out (starts slowly), In-Out. */
inline constexpr BezierHandles BEZIER_EVEN = {1.0f / 3.0f, 1.0f / 3.0f, 2.0f / 3.0f, 2.0f / 3.0f};
inline constexpr BezierHandles BEZIER_IN = {0.0f, 0.0f, 0.58f, 1.0f};
inline constexpr BezierHandles BEZIER_OUT = {0.42f, 0.0f, 1.0f, 1.0f};
/* Progress at `t` (0..1) along the ease the handles make. */
float bezier_ease(const BezierHandles &handles, float t);
/* Handles in range: times 0..1, progress -2..3. */
BezierHandles bezier_clamped(const BezierHandles &handles);

/* Blender's keyframe types, two of them: a Key, or a Breakdown, an in-between pose whose timing is the main keys'
 * either side: the stretches next to it are timed as one movement from main key to main key (Channel::evaluate), the
 * timing tools working on them as one (anim/motion_path.cc), and in the timeline a small blue circle. */
enum class KeyType : uint8_t { Key, Breakdown };

/* "KEYFRAME", "BREAKDOWN" (Blender's identifiers). */
const char *key_type_identifier(KeyType type);
const char *key_type_name(KeyType type);
bool key_type_from_identifier(const std::string &identifier, KeyType *r_type);

const char *ease_identifier(Ease ease);
const char *ease_name(Ease ease);
bool ease_from_identifier(const std::string &identifier, Ease *r_ease);

/* A position key's path handles (the Motion Path tool's): Auto until one is dragged, then Aligned (the two in a line,
 * as a smooth point's) or Broken (Alt: each its own way). */
enum class PathHandles : uint8_t { Auto, Aligned, Broken };

/* A control point's handles (Key::path_control_handles), as a vector point's: worked out smooth from the points either
 * side until one is moved (`set`), then kept where they are put. Aligned (as smooth ones come), moving one turns the
 * other to stay in line; Alt moves one alone and breaks the line. Stored as the control point is, relative to the
 * stretch's keys (x along the line from the key before to this one, y across it, in its length), so they turn and
 * grow with the keys. */
struct PathControlHandles {
  float2 in{}, out{};
  bool set = false;
  bool aligned = true;
  bool operator==(const PathControlHandles &) const = default;
};

template<typename T> struct Key {
  int frame = 0;
  T value{};
  Interp interp = Interp::Smooth;
  /* Never a Breakdown on frame 0, the setup. */
  KeyType type = KeyType::Key;
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
  /* Bezier: its two handles. */
  BezierHandles bezier = BEZIER_DEFAULT;
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
  /* Position keys only: the control points of the stretch arriving at this key (the Motion Path tool's Alt click),
   * in order along it. They shape its path only (no frame, no timing: its dots go the same share of the way each
   * frame as without them). Each is where it sits relative to the keys either side: x along the line from the key
   * before to this one, y across it (both in that line's length), so they ride with the keys. */
  std::vector<T> path_controls;
  /* Their handles, one for each (any missing: not set yet). */
  std::vector<PathControlHandles> path_control_handles;
  /* Picked in the timeline. Not saved. */
  bool selected = false;
  /* Key Together (a group's): added only to hold this channel where it was while another layer of the group was
   * keyed here. Taken away when Key Together is turned off, unless it was edited since: its value or timing
   * (filler_self; 0 until taken, as when opened), or the timing of the stretch after it while the key after is the one it had (filler_next: its
   * frame, filler_out: its timing). */
  bool filler = false;
  /* Added inside motion the channel already had (before its last key of its own): it holds nothing the curve
   * doesn't, so the curve is worked out without it, as is one after its last key of its own (Channel::evaluate).
   * Turning Key Together on moves nothing. */
  bool filler_inside = false;
  int filler_next = 0;
  uint32_t filler_self = 0, filler_out = 0;
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
  /* Frame 0's key in an action not being edited (by name): its value is the setup's, shared, but its path handles
   * are the action's own (the way its path leaves the setup pose). Only kept when they aren't Auto. */
  LazyMap<std::string, Key<T>> action_setup;
  /* What action bone actions add to the value right now (it is in the plain value too). Edits and saving
   * take it off, so it never becomes part of the data. */
  T from_action_bones{};
  /* An action bone action is playing into it now (its bone turned into it, the offset maybe 0 at this turn):
   * its field shows pink. Worked out with `from_action_bones`, never saved. */
  bool action_bone_live = false;
  /* Key Whole Drawing and Key Together: it was there when its keys were last made to agree with the rest's; one of
   * that time's frames it misses now had its key deleted (a channel new since, like an eraser's cut end, says
   * nothing). Not saved. Each its own: a drawing's channel is its drawing's (`synced`) and its Key Together group's
   * (`together_synced`), and one synced by its drawing is still new to a group it has just joined. */
  bool synced = false;
  bool together_synced = false;
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
/* Where the stretch arriving at key `i` is `u` (0-1) of the way along its path by length, as Custom goes along it (in
 * the keys' own space). */
float2 stretch_path_point(const std::vector<Key<float2>> &keys, size_t i, float u);

/* A control point stored relative to the stretch from `a` to `b` (Key::path_controls) as a value, and back. */
float2 path_control_value(float2 a, float2 b, float2 stored);
float2 path_control_stored(float2 a, float2 b, float2 value);
/* A control point's handle (an offset from it) stored the same way, and back. */
float2 path_control_offset_value(float2 a, float2 b, float2 stored);
float2 path_control_offset_stored(float2 a, float2 b, float2 offset);
/* Control point `j`'s handles as stored (not set when it has none yet). */
PathControlHandles path_control_handles_of(const Key<float2> &key, size_t j);
/* The stretch arriving at key `i` as points with the handles the path runs along (offsets from them), in the keys' own
 * space: its two keys (along their path handles, each a share of its piece) and its control points between (their own
 * handles once set, else smooth through the points either side). */
struct StretchPoints {
  std::vector<float2> p, in, out;
};
StretchPoints stretch_points(const std::vector<Key<float2>> &keys, size_t i);
/* The path of the stretch arriving at key `i` through its control points, as a line of points (`per` a piece), in the
 * keys' own space. */
std::vector<float2> stretch_control_line(const std::vector<Key<float2>> &keys, size_t i, int per = 24);

/* "AUTO", "ALIGNED", "BROKEN". */
const char *path_handles_identifier(PathHandles mode);
bool path_handles_from_identifier(const std::string &identifier, PathHandles *r_mode);

/* Grows each time a key is added, changed or removed (set_key, remove_key), anywhere: for what is kept until
 * keys change. */
uint64_t channel_keys_generation();
/* For keys changed in place without set_key or remove_key (an action's put-away keys). */
void channel_keys_changed();
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
  KeyType type = KeyType::Key;
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
  BezierHandles bezier = BEZIER_DEFAULT;

  bool operator==(const KeySettings &) const = default;
};

/* Keys from files before format 4, whose curves were for the stretch after each key: moved onto the
 * key after, as they are now for the stretch arriving at a key, so the motion stays the same. */
template<typename T> void keys_curves_to_arriving(std::vector<Key<T>> &keys);

template<typename T> KeySettings key_settings(const Key<T> &k)
{
  KeySettings s;
  s.interp = k.interp;
  s.type = k.type;
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
  s.bezier = k.bezier;
  return s;
}

template<typename T> void key_settings_apply(Key<T> &k, const KeySettings &s)
{
  k.interp = s.interp;
  k.type = k.frame == 0 ? KeyType::Key : s.type;
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
  k.bezier = bezier_clamped(s.bezier);
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
/* Bezier handles that draw `s`'s curve, or as near as two handles get (least squares): exact for Linear, Smooth and
 * Bezier, close for the Ease modes and Back. False for modes two handles can't shape (Step, Bounce, Elastic, Noisy,
 * Cycle, Pose, Custom). */
bool interp_bezier_fit(const KeySettings &s, BezierHandles &r_handles);

}  // namespace oanim
