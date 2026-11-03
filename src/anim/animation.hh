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
#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "anim/channel.hh"

namespace oanim {

struct Bone;
struct ActionPlay;
struct FixAction;
struct FixPose;
struct Document;
struct Layer;

/* Turns edits since the last evaluation into keys at the evaluated frame. */
void animation_record(Document &doc);
/* Writes every animated channel's value at `frame` into its plain value. */
void animation_evaluate(Document &doc, int frame);
/* After the current frame changed: records edits at the old frame, then evaluates the new one.
 * Does nothing when the frame is already evaluated. */
void animation_sync(Document &doc);
/* How many times the current frame has changed this session (counted by animation_sync; not part of
 * the document, so undo leaves it alone): what lasts "until the frame changes" checks it. */
uint64_t animation_frame_changes();

/* Playing while something is dragged, as Blender records with auto-key: animation_sync has keyed the
 * frame just left with what the drag gave it. A drag puts back how things were when it began on every
 * update, so `start` (that state) takes the keys, and whatever the drag isn't holding takes the new
 * frame's value: the rest plays on while the dragged parts stay where the mouse has them. `dragged` is
 * the state as the drag left it on the frame just left, `now` as the new frame has it. `held` is kept
 * by the caller for the whole drag: what the drag has held once stays held (on a frame the drag didn't
 * update, the mouse outside the window, say, it would look unheld and the move be added again). Returns
 * false (changing nothing) when they don't hold the same channels. */
bool animation_follow_playback(Document &start, Document &dragged, Document &now, std::vector<char> &held);
/* The same for a bone layer's bones (a drag that keeps only those). */
bool animation_follow_playback(std::vector<Bone> &start, std::vector<Bone> &dragged, std::vector<Bone> &now,
                               std::vector<char> &held);

/* --- Actions (Moho's): named alternative timelines of a bone layer and everything inside it.
 * Editing one swaps its keys into the channels, so keying, the timeline and evaluation all work
 * on it unchanged; layers outside keep playing the mainline. Frame 0 stays the shared setup pose:
 * editing it in an action changes the setup. ------------------------------------------------- */

/* The bone layer whose actions a layer can use: itself, or the one it sits in; null if neither. */
Layer *action_owner(Document &doc, uint64_t layer_id);
/* Every bone layer whose actions reach a layer, nearest first: action_owner, then those around it, since an action
 * animates everything inside its bone layer. */
std::vector<Layer *> action_owners(Document &doc, uint64_t layer_id);
struct VectorDrawing;
/* Puts `drawing`'s keys being edited away under timeline `from` and brings out `to` ("": the mainline), for a drawing
 * not in the document, as document_set_action does for those that are. */
void drawing_timeline_swap(VectorDrawing &drawing, const std::string &from, const std::string &to);
/* Edits action `name` of bone layer `owner` ("" for the mainline). Returns false when there is
 * no such action. */
bool document_set_action(Document &doc, uint64_t owner, const std::string &name);
/* By name alone: the bone layer is found from the active layer (the nearest around it that has it), else any bone layer
 * that has it. */
bool document_set_action_by_name(Document &doc, const std::string &name);
/* Adds an action to `owner` (named uniquely from `name`) and starts editing it. Returns its name. */
std::string document_action_new(Document &doc, uint64_t owner, const std::string &name);
/* Deletes action `name` of `owner` and its keys (editing it: back to the mainline first). Returns
 * false when there is no such action. */
bool document_action_delete(Document &doc, uint64_t owner, const std::string &name);

/* Action bone actions (Moho's smart bones): an action named after a bone of its bone layer (or the
 * name plus " 2", for turning the other way). In it the bone's angle is keyed from its setup
 * angle onwards; elsewhere the bone's angle picks the matching frame of the action, and every
 * other channel the action keys gets that frame's change from frame 0 added on top. Several action bone
 * actions add together. Returns the driving bone, or null for an ordinary action. A 2 Axis Controller's
 * ordinary actions are played the same way, by its knob (anim/controller.hh), and it is what this returns. */
const Bone *action_bone_of(const Layer &owner, const std::string &action);

/* --- 2 Axis Controllers (anim/controller.hh) ------------------------------------------------------- */

/* Where a controller's knob shows, in frames: where it is, or inside one of its own actions the frame shown on that
 * side, and inside a fix the pose held. */
float2 controller_knob_shown(const Document &doc, const Layer &owner, const Bone &knob);
/* Where its actions are really playing, in frames: not the knob where something else turns its dials. */
float2 controller_knob_playing(const Layer &owner, const Bone &knob);
/* The poses of the fixes of its actions, in frames, as the box marks them. */
std::vector<float2> controller_fix_poses(const Layer &owner, const Bone &knob);
/* Whether a dial is held at rest because the knob is on the other side's. */
bool controller_dial_held(const Layer &owner, const Bone &dial);
/* What the sides' actions key, found again after keys change (Bone::controller_keys). */
void controllers_update(Document &doc);
/* Makes the bone a controller, or regular again, on the mainline. To a controller: its own action bone actions go into
 * Right and Left, its angle keys becoming knob keys; any other position keys are read as frames. Back: they come out
 * the same way, and linked dials get keys of their own. Returns anything the user should hear of. */
std::string bone_convert_to_controller(Document &doc, uint64_t owner, uint64_t bone);
std::string bone_convert_to_regular(Document &doc, uint64_t owner, uint64_t bone);
/* Puts `action` in a side ("" empties it), from the mainline. An action bone action's dial is linked: its keys move to
 * the knob. Keys there too on that axis are one too many: `keep` says whose ("THIS" or "OTHER"). False with why,
 * nothing changed. `r_note`: anything the user should hear of. */
bool controller_side_set(Document &doc, uint64_t owner, uint64_t knob, int side, const std::string &action,
                         const std::string &keep, std::string *r_why, std::string *r_note);
/* Whether `action` going in `side` needs `keep`. */
bool controller_side_keys_clash(const Layer &owner, const Bone &knob, int side, const std::string &action);
/* Before a controller goes: its dials get keys of their own that move them as the knob did. */
void controller_release(Document &doc, uint64_t owner, uint64_t knob);
/* What the action bone actions of bone layer `owner` add to its bones on `frame`, as apply_action_bone_actions plays
 * them on the frame shown: `bones` (owner's, in its order) hold the keyed values there, without action bone
 * actions, and get the action bone actions' amounts added. For working out other frames than the one shown
 * (dynamics simulate every frame up to it). */
void action_bone_actions_add_at(const Layer &owner, int frame, std::vector<Bone> &bones);
/* --- Fix actions (correctives): see FixAction (data/layer.hh). A fix plays after the action bone actions, never
 * moving an action bone itself. Fixes add up: one for three action bone actions is keyed on top of those for two of
 * them (they play while it is edited), and fades out whenever any of its own is at rest. --------------- */

/* Fix action `name` of `owner`, or null (an ordinary or action bone action). */
const FixAction *fix_action_find(const Layer &owner, const std::string &name);
/* Why it can't play (a driver deleted, or not an action bone action any more), or "" when it can. */
std::string fix_action_broken(const Layer &owner, const FixAction &fix);
/* The action bone actions of `owner` playing now, its bones as they are: each with the frame its bone's turn
 * picks (more than 0). */
std::vector<std::pair<std::string, float>> action_bone_actions_playing(const Layer &owner);
/* An action bone action's length: its driving bone's last key there (0 with none). */
float action_bone_action_length(const Layer &owner, const std::string &name);
/* A 360 action bone action (Moho's 360 action bone): its bone's last key is exactly one whole turn from the setup,
 * so the bone's turn counts round and round (modulo a turn). */
bool action_bone_action_full_turn(const Layer &owner, const std::string &name);

/* --- The Actions panel (ActionPlay, data/layer.hh): every action's Frame, Mute and Loop. ---------------------- */

/* "actions[\"wave\"].frame": the channel of an action's own Frame. */
std::string action_frame_path(const std::string &name);
const ActionPlay *action_play_find(const Layer &owner, const std::string &name);
ActionPlay *action_play_find(Layer &owner, const std::string &name);
/* Made the first time any of it is set. */
ActionPlay &action_play_ensure(Layer &owner, const std::string &name);
/* What says an action's frame: its own Frame, a bone's turn (an action bone action), a knob (a controller's side,
 * linked dials too: the knob holds the keys) or, for a fix, its drivers. `r_holder`: the bone or knob. */
enum class ActionFrameBy { Own, Bone, Knob, Fix };
ActionFrameBy action_frame_by(const Layer &owner, const std::string &name, const Bone **r_holder = nullptr);
bool action_muted(const Layer &owner, const std::string &name);
/* Whether it starts over past its last frame (ActionPlay::loop, Auto worked out). */
bool action_loops(const Layer &owner, const std::string &name);
/* Its last frame: its bone's or knob's for those, else the last key in it anywhere in the bone layer. `edited`: its
 * keys are out in the channels. */
float action_length(const Layer &owner, const std::string &name, bool edited = false);
/* The frame it is at, as the panel shows it: its own Frame, or where its bone or knob has it (0: not playing). A fix
 * shows how much of it plays (0 to 1). */
float action_frame_shown(const Layer &owner, const std::string &name);
/* Puts it at `frame` (0: not playing): its own Frame, or its bone turned or knob moved to there (keyed as edits are).
 * False for a fix, or a bone with no keys yet to say how far. */
bool action_frame_set(Layer &owner, const std::string &name, float frame);
/* Turned less than this share of its length, an action bone action doesn't count toward a fix being made or
 * offered (a bone nudged by accident); fixes still play from any amount. */
inline constexpr float FIX_COUNTS_FROM = 0.05f;
/* The action bone actions playing now that count for fixes (FIX_COUNTS_FROM of their length or more). */
std::vector<std::pair<std::string, float>> action_bone_actions_counting(const Layer &owner);
/* Where pose `pose` of a fix has each driver, as a share of that action bone action's length (0 to 1). */
std::vector<float> fix_pose_shares(const Layer &owner, const FixAction &fix, const FixPose &pose);
/* "head_turn 52% · HeadTurnUp 30%": a pose of a fix, for labels. */
std::string fix_pose_label(const Layer &owner, const FixAction &fix, const FixPose &pose);
/* How a fix plays with `playing` (action bone action frames, as action_bone_actions_playing gives): each pose with how
 * much of its correction is added (its keys on its frame, as offsets from the setup), `strength` how much
 * of the fix plays (0 to 1: 1 at or past one of its poses, 0 while any of its drivers rests), `nearest` the
 * pose it is closest to (-1 with none). Exact at each pose; between poses they blend by inverse distance in
 * the drivers' shares; toward rest each pose fades with the product of its drivers' shares of it. */
struct FixMix {
  std::vector<std::pair<size_t, float>> poses;
  float strength = 0.0f;
  int nearest = -1;
};
FixMix fix_action_mix(const Layer &owner, const FixAction &fix, const std::vector<std::pair<std::string, float>> &playing);
float fix_action_strength(const Layer &owner, const FixAction &fix, const std::vector<std::pair<std::string, float>> &playing);
/* The pose of `fix` within FIX_COUNTS_FROM of `playing` on each of its drivers (the closest), or -1. */
int fix_pose_near(const Layer &owner, const FixAction &fix, const std::vector<std::pair<std::string, float>> &playing);
/* The pose a fix's frame `frame` holds its drivers at while it is edited: the last pose on or before it (the
 * first, before that); -1 with no poses. */
int fix_pose_held(const FixAction &fix, int frame);
/* The pose on frame `frame` exactly, or -1 (a frame between poses: keys there don't play). */
int fix_pose_on_frame(const FixAction &fix, int frame);
/* Whether action `name` adds onto the pose rather than being a timeline of its own (an action bone or fix action):
 * its keys are offsets that pose changes elsewhere leave as they are. */
bool action_adds(const Layer &owner, const std::string &name);
/* Opens the fix for this pose: for `drivers` (action bone action names; empty: every one counting now), the fix of
 * exactly those, made if there is none ("head_turn × head_up"), at its pose here (one within FIX_COUNTS_FROM
 * of each driver's share), added if there is none, keyed to start from what the fix played here before.
 * Edited at once on that pose's frame. */
struct FixOpened {
  std::string name;
  int pose = -1;
  bool new_fix = false, new_pose = false;
  /* Why nothing opened, for the report. */
  std::string error;
};
FixOpened document_fix_pose_here(Document &doc, uint64_t owner, const std::vector<std::string> &drivers = {});
/* Adds a pose to fix `name` of `owner` with its drivers at `shares` (0 to 1 of each action bone action), keyed to
 * start from what the fix played there before, and opens it. Returns its index, or -1. */
int document_fix_pose_add(Document &doc, uint64_t owner, const std::string &name, const std::vector<float> &shares);
/* Removes pose `pose` of fix `name` (its keys on its frame with it); never the last one. */
bool document_fix_pose_remove(Document &doc, uint64_t owner, const std::string &name, size_t pose);
/* Whether `bone` of `armature` is held by the fix being edited (one of its action bones: they show the pose,
 * so they can't be picked or changed until it's left). */
bool bone_held_by_fix(const Document &doc, const Layer &armature, const Bone &bone);
/* After every change (each undo step): a fix's action bones picked while it is edited are let go (saying
 * why), and a pose whose keys have all been deleted goes, the fix with its last one. */
void fix_actions_tidy(Document &doc);
/* Renames action `from` of `owner`: its keys, the fixes it drives and, edited, the timeline's name. */
bool document_action_rename(Document &doc, uint64_t owner, const std::string &from, const std::string &to);
/* Bone `from` of `owner` renamed `to`: its action bone actions take the new name, and the fixes they drive with
 * them. */
void armature_bone_renamed(Document &doc, uint64_t owner, const std::string &from, const std::string &to);

/* Takes what action bone actions add off every plain value, leaving the data (for saving). */
void animation_strip_action_bones(Document &doc);

/* A layer and everything inside it without timeline keys (Duplicate in a switch): each channel keeps
 * its setup value (frame 0), or with `from_now` the value it has now, its drawings then taking that as
 * their setup pose. With `keep_actions` its actions' keys (action bone actions) stay. */
void layer_strip_keys(Layer &layer, bool from_now, bool keep_actions);
/* Works action bone actions out again for the bones as they are now (a bone turned without changing
 * frame). animation_sync does this when the frame hasn't moved. */
/* As animation_sync, for the main loop's own frame changes (playing): `edits` stamps every edit the app can see
 * coming (Python runs, undo steps, operators run, edits in the UI). While it and the keys stay as they were, a
 * drawing a switch hides that comes out the same on every frame (no keys, no action data, nothing action bone actions
 * added) is neither recorded nor evaluated again: on a rig with many views, most of the document. */
void animation_sync_playing(Document &doc, uint64_t edits);
void animation_refresh_action_bones(Document &doc);
/* A bone drag on `doc` begins: until it ends, the layers action bone actions play into are the ones found now (it
 * changes no layers, only bones), and the drag works action bone actions out again itself every update, so
 * animation_sync leaves them. End it before the document is put back or replaced. */
void animation_action_bone_drag_begin(Document &doc);
void animation_action_bone_drag_end();
/* The layers action bone actions play into during the bone drag going on (their ids). */
std::unordered_set<uint64_t> animation_action_bone_drag_layers();

/* The document's Simulation settings (gravity, wind, wind_direction, wind_gusts, wind_gust_speed,
 * wind_turbulence) as channels of Document::simulation, made ready. */
Layer &document_simulation(Document &doc);
/* Whether a path names one of them. */
bool simulation_path(const std::string &path);
/* Writes the Simulation channels' values into the document's settings (after drivers set them). */
void simulation_values_out(Document &doc);
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
/* Deletes the channel's key on `frame`. Frame 0's key is the setup (the rest pose, in the mainline and
 * every action): it goes only with the last of the keys, since without it frame 0 would show the
 * nearest key's pose, and recording there would take that pose as the rest. A setup key left alone
 * goes too, the channel keeping its value. Returns whether a key went. */
bool channel_delete_key(Channel<float> &ch, float &value, int frame);
bool channel_delete_key(Channel<float2> &ch, float2 &value, int frame);
/* Blender's keyframe button for some channels of a layer (by path): keys them at `frame`, or, when
 * any is keyed there, takes those keys away (frame 0's only with the last key). `mode` 1 only keys
 * and -1 only takes away, whatever is keyed (a group's button for the layers inside it). Returns how
 * many were added or went. */
int layer_toggle_keys(Layer &layer, const std::vector<std::string> &paths, int frame, int mode = 0);
/* 0: none of them animated; 1: animated; 2: keyed at `frame`. */
int layer_key_state(Layer &layer, const std::vector<std::string> &paths, int frame);
/* Added to a key state: an action bone action plays into one of them now (its field shows the value with it,
 * pink; ui_key_tint_push). */
inline constexpr int KEY_STATE_ACTION_BONE = 4;
/* KEY_STATE_ACTION_BONE when an action bone action is playing into any of them now, else 0. */
int layer_action_bone_state(Layer &layer, const std::vector<std::string> &paths);
/* Frames where the layer (or its drawing) has keys, sorted, each once. */
std::vector<int> layer_key_frames(const Layer &layer);
/* Whether any of the layer's channels has a key (cheaper than layer_key_frames). */
bool layer_has_keys(const Layer &layer);

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
  /* With selected_only: the layers whose selected points count, those being edited (a selection
   * left in a layer no longer edited stays stored, but isn't the selection now); null: every layer's. */
  std::shared_ptr<const std::set<uint64_t>> point_layers;
  bool everything() const { return kinds == KEY_ALL && !selected_only && path.empty() && !pinned_only; }
};
/* The kind of channel a path names (see ChannelRef). */
KeyKind channel_kind(const std::string &path);
/* A layer's channels that pass the filter. */
std::vector<ChannelRef> layer_filtered_channels(Layer &layer, const KeyFilter &filter);
/* Keys the layer's channels that pass `filter` at `frame` with their current values (a timeline row's
 * channels, double-clicked). Returns how many keys were set. */
int layer_insert_filtered_keys(Layer &layer, const KeyFilter &filter, int frame);

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
/* With `r_layers` and `r_frames`: each layer pasted onto with the layer its keys were copied from, and
 * each copied key's frame with the frame it was pasted on (for Ignore Bones). */
int layers_paste_keys(const std::vector<Layer *> &layers, int frame,
                      std::vector<std::pair<uint64_t, uint64_t>> *r_layers = nullptr,
                      std::vector<std::pair<int, int>> *r_frames = nullptr);
bool key_clipboard_empty();
/* The document the copied keys came from, as it was when they were copied (for Ignore Bones), or null. */
void key_clipboard_set_source(const Document &doc);
const Document *key_clipboard_source();

/* Ignore Bones (Duplicate and Paste Keys): the point and handle keys just placed on each `frames`
 * (source frame, new frame) of `layers` (the layer in `doc`, the one in `source` its keys came from) are
 * worked out again so each point shows where it did on the source frame in `source`, whatever the
 * bones do on the new frame. Only selected keys (the new ones) change; detail points follow their
 * parents, and a point a bone flattens to nothing keeps its key. Returns how many keys changed. */
int keys_ignore_bones(Document &doc, const Document &source, const std::vector<std::pair<uint64_t, uint64_t>> &layers,
                      const std::vector<std::pair<int, int>> &frames);
/* Keep Place (Duplicate and Paste Keys): the copied shape without its place. The points whose
 * position keys were just placed on each `frames` (source frame, new frame) of `layers` (selected keys,
 * the new ones) move together, so their centre shows on the new frame where the same points' centre
 * did there before the copy (`before`: the document as it was). Their shape, and how the copied pieces
 * sit together, stay as copied. Detail points follow their parents. Returns how many keys changed. */
int keys_keep_place(Document &doc, const Document &before, const std::vector<uint64_t> &layers,
                    const std::vector<std::pair<int, int>> &frames);
/* Whether bones move any point of `layer` (its Armature modifier, or Parent Bone moving its points). */
bool layer_bones_move_points(const Document &doc, const Layer &layer);

}  // namespace oanim
