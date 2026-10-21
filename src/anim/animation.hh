#pragma once

/* How animation reaches the document's plain values, Moho style.
 *
 * Every animatable value (a layer's position, rotation, scale and opacity; a point's position
 * and radius; a curve point's curvature and stored handles) has a Channel beside it. The plain
 * value is the one at the evaluated frame, so tools and the renderer just read and write it.
 *
 * - Recording: a plain value that differs from what the channel last gave is an edit. At
 *   frame 0 (the setup frame) the edit sets the frame-0 key, so it changes every frame without
 *   its own key. At other frames, with auto-keying on, it keys that frame; the first key on a
 *   channel also keys frame 0 with the value from before the edit. With auto-keying off an
 *   unanimated value just changes, and an animated one snaps back at the next evaluation.
 * - Evaluating: each animated channel writes its value at the frame into the plain value.
 *
 * Recording runs before every undo step, before the frame changes, and before saving, so
 * edits always land on the frame they were made at. */

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "anim/channel.hh"

namespace oanim {

struct Bone;
struct Document;
struct Layer;

/* Turns edits since the last evaluation into keys at the evaluated frame. */
void animation_record(Document &doc);
/* Writes every animated channel's value at `frame` into its plain value. */
void animation_evaluate(Document &doc, int frame);
/* After the current frame changed: records edits at the old frame, then evaluates the new one.
 * Does nothing when the frame is already evaluated. */
void animation_sync(Document &doc);

/* --- Actions (Moho's): named alternative timelines of a bone layer and everything inside it.
 * Editing one swaps its keys into the channels, so keying, the timeline and evaluation all work
 * on it unchanged; layers outside keep playing the mainline. Frame 0 stays the shared setup pose:
 * editing it in an action changes the setup. ------------------------------------------------- */

/* The bone layer whose actions a layer can use: itself, or the one it sits in; null if neither. */
Layer *action_owner(Document &doc, uint64_t layer_id);
/* Edits action `name` of bone layer `owner` ("" for the mainline). Returns false when there is
 * no such action. */
bool document_set_action(Document &doc, uint64_t owner, const std::string &name);
/* By name alone: the bone layer is found from the active layer, else any bone layer that has it. */
bool document_set_action_by_name(Document &doc, const std::string &name);
/* Adds an action to `owner` (named uniquely from `name`) and starts editing it. Returns its name. */
std::string document_action_new(Document &doc, uint64_t owner, const std::string &name);
/* Deletes action `name` of `owner` and its keys (editing it: back to the mainline first). Returns
 * false when there is no such action. */
bool document_action_delete(Document &doc, uint64_t owner, const std::string &name);

/* Smart actions (Moho's smart bones): an action named after a bone of its bone layer (or the
 * name plus " 2", for turning the other way). In it the bone's angle is keyed from its setup
 * angle onwards; elsewhere the bone's angle picks the matching frame of the action, and every
 * other channel the action keys gets that frame's change from frame 0 added on top. Several smart
 * actions add together. Returns the driving bone, or null for an ordinary action. */
const Bone *smart_action_bone(const Layer &owner, const std::string &action);
/* Takes what smart actions add off every plain value, leaving the data (for saving). */
void animation_strip_smart(Document &doc);
/* Works smart actions out again for the bones as they are now (a bone turned without changing
 * frame). animation_sync does this when the frame hasn't moved. */
void animation_refresh_smart(Document &doc);

/* The document's Simulation settings (gravity, wind, wind_direction, wind_gusts, wind_gust_speed,
 * wind_turbulence) as channels of Document::simulation, made ready. */
Layer &document_simulation(Document &doc);
/* Whether a path names one of them. */
bool simulation_path(const std::string &path);
/* One of them on `frame`: its keys there when animated, else `plain` (the setting now). */
float simulation_value_at(const Document &doc, const char *path, float plain, int frame);

/* One named channel of a layer and the plain value it drives: "position", "rotation", "scale",
 * "opacity", "points[3].pos", "points[3].radius", "curves[0].points[2].curvature",
 * "curves[0].points[2].handle_in", "curves[0].points[2].handle_out". */
struct ChannelRef {
  std::string path;
  Channel<float> *scalar = nullptr;
  float *scalar_value = nullptr;
  Channel<float2> *vector = nullptr;
  float2 *vector_value = nullptr;
};
/* The layer's channels: its own, then (with `drawing`) those of its points and curves. */
std::vector<ChannelRef> layer_channels(Layer &layer, bool drawing = true);
bool layer_channel(Layer &layer, const std::string &path, ChannelRef *r_ref);

/* Keys every channel of the layer at `frame` with its current value (Blender's I). Returns
 * how many keys were set. */
int layer_insert_keys(Layer &layer, int frame);
/* Removes the layer's keys at `frame` (Alt I). Returns how many were removed. */
int layer_delete_keys(Layer &layer, int frame);
/* Blender's keyframe button for some channels of a layer (by path): keys them at `frame`, or, when
 * any is keyed there, takes those keys away (frame 0's only with the last key). Returns how many
 * were added, or minus how many went. */
int layer_toggle_keys(Layer &layer, const std::vector<std::string> &paths, int frame);
/* 0: none of them animated; 1: animated; 2: keyed at `frame`. */
int layer_key_state(Layer &layer, const std::vector<std::string> &paths, int frame);
/* Frames where the layer (or its drawing) has keys, sorted, each once. */
std::vector<int> layer_key_frames(const Layer &layer);

/* --- Editing keys in the timeline: the keys of several layers, by frame. ---------------- */

/* How a key looks in the timeline, by the family of its mode. */
enum KeyShape : int8_t {
  KEY_SHAPE_SMOOTH,  /* diamond */
  KEY_SHAPE_LINEAR,  /* thin diamond */
  KEY_SHAPE_STEP,    /* square */
  KEY_SHAPE_EASE,    /* circle */
  KEY_SHAPE_BOUNCY,  /* star: Back, Bounce, Elastic */
  KEY_SHAPE_SPECIAL, /* hexagon: Noisy, Cycle, Bezier, Pose */
  KEY_SHAPE_MIXED,   /* keys of different families on one frame: diamond */
};
KeyShape interp_key_shape(Interp interp);

struct KeyFrameSummary {
  int keys = 0;
  int selected = 0;
  /* The family of the keys' modes (yes/no channels, always Step, only count when there is
   * nothing else). */
  KeyShape shape = KEY_SHAPE_STEP;
  bool shape_set = false;
};
/* Which channels a timeline row covers: kinds of channel, and whether only those of selected
 * things (selected points, and bones picked by body or ball). */
enum KeyKind : uint32_t {
  KEY_LAYER_ANGLE = 1 << 0,     /* "rotation" */
  KEY_LAYER_TRANSFORM = 1 << 1, /* "position", "scale" */
  KEY_OPACITY = 1 << 2,
  KEY_VISIBILITY = 1 << 3,
  KEY_BONE_ANGLE = 1 << 4,
  KEY_BONE_TRANSFORM = 1 << 5, /* a bone's position and scale */
  KEY_POINTS = 1 << 6,         /* points' position and width, curves' shape and handles */
  KEY_SIMULATION = 1 << 7,     /* the document's gravity and wind (Document::simulation) */
  KEY_CONSTRAINTS = 1 << 8,    /* a bone's constraints' settings (influence, dynamics) */
  KEY_DRAW_ORDER = 1 << 9,     /* "draw_order" */
  KEY_LAYER = KEY_LAYER_ANGLE | KEY_LAYER_TRANSFORM | KEY_OPACITY | KEY_VISIBILITY | KEY_DRAW_ORDER,
  KEY_BONES = KEY_BONE_ANGLE | KEY_BONE_TRANSFORM | KEY_CONSTRAINTS,
  KEY_ALL = KEY_LAYER | KEY_BONES | KEY_POINTS | KEY_SIMULATION,
};
struct KeyFilter {
  uint32_t kinds = KEY_ALL;
  bool selected_only = false;
  /* Only this channel (a timeline row of one property), or, ending in '.', the channels under it
   * ('bones["arm"].': one bone's). */
  std::string path;
  /* Only pinned channels. */
  bool pinned_only = false;
  bool everything() const { return kinds == KEY_ALL && !selected_only && path.empty() && !pinned_only; }
};
/* The kind of channel a path names (see ChannelRef). */
KeyKind channel_kind(const std::string &path);
/* A layer's channels that pass the filter. */
std::vector<ChannelRef> layer_filtered_channels(Layer &layer, const KeyFilter &filter);

/* Whether a channel passes the filter's path ('bones["arm"].' covers the bone's channels). */
bool key_filter_path_matches(const KeyFilter &filter, const std::string &path);

/* --- Pinning channels to the timeline ------------------------------------------------------ */

/* Whether the layer's channel at `path` is pinned. */
bool layer_channel_pinned(Layer &layer, const std::string &path);
/* Pins (or unpins) the layer's channels at `paths`. Returns how many changed. */
int layer_pin_channels(Layer &layer, const std::vector<std::string> &paths, bool pinned);
/* Unpins every channel of the document (its layers and its Simulation settings). Returns how many. */
int document_clear_pins(Document &doc);
/* What a channel is called in the timeline: "Position", "Angle", "IK: Influence", "Dynamics: Angle
 * Bounce", "Gravity". With `owner`, whose it is first: "Layer 1: Opacity", "arm: Angle",
 * "Simulation: Wind". */
std::string channel_label(Layer &layer, const std::string &path, bool owner);

/* The channels of `layers` that pass `filter` and have a key at `frame` (the timeline's Keyed Here
 * list): each layer with the path of each such channel, in the layer's channel order. */
std::vector<std::pair<Layer *, std::string>> layers_keyed_at(const std::vector<Layer *> &layers,
                                                             const KeyFilter &filter, int frame);

/* For each keyed frame: how many keys there are, and how many of them are selected. */
std::map<int, KeyFrameSummary> layers_key_summary(const std::vector<Layer *> &layers, const KeyFilter &filter = {});
/* Selects (or deselects) every key on frames `first`..`last`. */
void layers_select_keys(const std::vector<Layer *> &layers, int first, int last, bool select,
                        const KeyFilter &filter = {});
void layers_deselect_keys(const std::vector<Layer *> &layers);
bool layers_any_key_selected(const std::vector<Layer *> &layers);

/* The selected keys' interpolation: how many there are, the first one's settings, and whether
 * their modes (or ease strengths) differ. */
struct SelectedKeys {
  int count = 0;
  KeySettings first;
  bool mixed = false;
  bool mixed_ease = false;
};
SelectedKeys layers_selected_keys(const std::vector<Layer *> &layers);
/* Changes the selected keys' interpolation settings with `change`; yes/no channels' keys stay
 * Step. Returns how many keys there were. */
int layers_change_selected_keys(const std::vector<Layer *> &layers, const std::function<void(KeySettings &)> &change);
/* Moves the selected keys to `to(frame)` (never below 1; frame-0 keys, the setup, stay put).
 * A moved key landing on an unselected one replaces it, as in Blender. */
void layers_move_selected_keys(const std::vector<Layer *> &layers, const std::function<int(int)> &to);
/* Deletes the selected keys, except frame 0's. Returns how many went. */
int layers_delete_selected_keys(const std::vector<Layer *> &layers);
/* Copies the selected keys `offset` frames later (never below 1); the copies become the
 * selection and replace keys where they land (Blender's Duplicate, then moving the copies). */
void layers_duplicate_selected_keys(const std::vector<Layer *> &layers, int offset);
/* Neighbouring keyed frames (a, b) across which nothing changes: every channel keyed at either
 * has a key at both with the same value. Blender draws a hold bar between such keys. */
std::vector<std::pair<int, int>> layers_hold_spans(const std::vector<Layer *> &layers, const KeyFilter &filter = {});

/* Blender's Copy and Paste Keyframes. Copying keeps the selected keys, by layer and channel, in a
 * clipboard that outlives switching actions (and documents). Pasting puts them at `frame` (the
 * first copied key lands there, the rest keep their spacing) onto the same layers' channels; a
 * layer that isn't there is matched by name, and keys copied from one layer go onto the first of
 * `layers` that has the channel. The pasted keys become the selection; one landing on frame 0
 * becomes the setup pose, and any before it are left out. Each returns how many keys it copied or
 * pasted. */
int layers_copy_selected_keys(const std::vector<Layer *> &layers);
int layers_paste_keys(const std::vector<Layer *> &layers, int frame);
bool key_clipboard_empty();

}  // namespace oanim
