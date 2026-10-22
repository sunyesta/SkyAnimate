#pragma once

/* Layers, following Moho: a tree of vector and group layers, drawn bottom of the list first.
 *
 * Masking works per group, as in Moho: a group's mask setting decides whether masking is on
 * and whether its contents start shown or hidden, and each child's masking mode adds to the
 * mask, subtracts from it, or is clipped by it.
 *
 * A reference layer is a live copy: it shows another layer's drawing (a group reference
 * holds references to the group's children) while keeping its own settings, so the same
 * artwork can be visible in one group and act as an invisible mask in another. */

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/math.hh"
#include "data/vector.hh"
#include "rna/rna.hh"

namespace oanim {

/* --- Reference layers ------------------------------------------------------------------------
 * A reference shows its original's drawing as it is now, with its own changes on top, each kept
 * apart so everything else goes on following the original (as Blender's library overrides, Unity's
 * prefab overrides and Figma's instances keep theirs). They find the original's points and shapes
 * by id, so they stay on the right ones as the original changes. */

/* A point moved on a reference: how far from the original's point, animatable. Its keys add to
 * the original's motion (or, with Follow Original's Animation off, to its setup place). */
struct ReferencePoint {
  float2 offset;
  Channel<float2> offset_keys;
};

/* A handle turned on a reference (R or S on its points): which one, by its point and the
 * neighbour it points toward along the curve (0: none, at an open end), and `in` for the handle
 * coming in from the previous point. Its offset is a ReferencePoint's. */
struct ReferenceHandleKey {
  uint64_t point = 0, toward = 0;
  bool in = false;
  auto operator<=>(const ReferenceHandleKey &) const = default;
};

/* A shape's look changed on a reference: each setting on its own (the rest follow the original). */
struct ReferenceLook {
  bool fill_set = false, line_set = false, width_set = false, brush_set = false;
  bool has_fill_set = false, has_outline_set = false;
  Color fill, line;
  float line_width = 0.0f;
  BrushSettings brush;
  bool has_fill = true, has_outline = true;
  /* The shape isn't shown on this reference. */
  bool hidden = false;
  bool empty() const
  {
    return !fill_set && !line_set && !width_set && !brush_set && !has_fill_set && !has_outline_set && !hidden;
  }
};

/* A colour swapped on a reference: every shape of the original filled (`fill`) or lined (`line`)
 * with `from` shows `to` instead, the shapes added later too, as recolouring a shared style. */
struct ReferenceSwap {
  Color from, to;
  bool fill = true, line = true;
};

struct ReferenceSettings {
  /* Its points can be moved (as offsets); off, only its shapes' looks can change. */
  bool edit_points = false;
  /* The original's animation comes through; off, only the original's setup shape (and this
   * reference's own keys). */
  bool follow_animation = true;
  /* Shows the original's final shape, posed by the original's bones, where the original is (its
   * own transform moving it from there); off, it binds to its own bone layer's bones. */
  bool match_original = true;
  /* By point id: moved points. By shape id: changed looks. */
  std::map<uint64_t, ReferencePoint> points;
  /* Turned or lengthened handles (as offsets from the original's). */
  std::map<ReferenceHandleKey, ReferencePoint> handles;
  /* The layer channels it has taken over ("position", "rotation", "scale", "opacity"): edited
   * or keyed on the reference. The others follow the original's, animation and all. */
  std::set<std::string> own_channels;
  std::map<uint64_t, ReferenceLook> looks;
  /* Edges shown or hidden on this reference unlike its original (Moho's Hide Edge), by the ids of
   * the points at either end, in curve order: true hides it. */
  std::map<std::pair<uint64_t, uint64_t>, bool> edges;
  std::vector<ReferenceSwap> swaps;
  /* With Match Original off: its points' own weights, by point id (for its own bone layer). */
  std::map<uint64_t, std::vector<BoneWeight>> weights;

  /* Runtime: the drawing as last built from the original (before any edit since), to tell what
   * was edited: each point's place, radius and weights, and each shape's look. */
  bool built = false;
  std::vector<uint64_t> built_ids;
  std::vector<float2> built_pos;
  std::vector<float> built_radius;
  std::vector<std::vector<BoneWeight>> built_weights;
  std::vector<Shape> built_shapes;
  std::vector<Curve> built_curves;
  float2 built_position, built_scale;
  float built_rotation = 0.0f, built_opacity = 1.0f;
};

/* Bone: a group that owns a skeleton and moves its children with it (Moho's bone layer). Media: an
 * image, image sequence, video or sound (data/media_layer.hh). */
enum class LayerType { Vector, Group, Bone, Media };

/* --- Media layers ------------------------------------------------------------------------------
 * A Media layer shows clips along the timeline, as a video editor's track: each clip is a file shown
 * from its Start frame for its Length, starting In seconds into the file (After Effects' in point;
 * slipping changes it). Swapping the file part way through the animation makes a new clip there. A
 * new one is a guide (Show in Renders off): shown while you work, hidden with the viewport's overlays,
 * never rendered (After Effects' guide layers, Blender's reference images). Anything with sound plays
 * it with the animation. */

/* How a picture sits in the camera's frame (at zoom 1): all of it inside (Fit), covering it (Fill),
 * stretched to it, or one picture pixel per rendered pixel (Original Size). */
enum class MediaFit : uint8_t { Fit, Fill, Stretch, Original };
/* What a clip shows after its file runs out, until its Length ends: the last frame, the file again
 * from its In point, or nothing. */
enum class MediaEnd : uint8_t { Hold, Loop, Hide };
/* How a clip's frames map to the document's: at the file's own speed (a 30 fps video plays in real
 * time in a 24 fps scene), or one file frame per document frame (a reference made at 12 fps stepped
 * through a drawing at a time). Sequences are always frame by frame. */
enum class MediaTiming : uint8_t { RealTime, FrameByFrame };

struct MediaClip {
  uint64_t id = 0;
  /* The file, absolute (saved relative to the .oanim when it can be, and found from either). */
  std::string filepath;
  /* A numbered image sequence starting at `filepath`. */
  bool sequence = false;
  /* Packed into the .oanim: its content's id in the packed store (data/media_layer.hh); "" linked. */
  std::string packed;
  /* The first frame it shows on, and how many frames it covers. */
  int start = 1;
  int length = 1;
  /* Seconds into the file where it starts (its In point). */
  double in = 0.0;
  MediaTiming timing = MediaTiming::RealTime;
  MediaEnd end = MediaEnd::Hold;
  /* Picked in the timeline. Not saved. */
  bool selected = false;
};

struct MediaSettings {
  /* In time order; they never overlap. */
  std::vector<MediaClip> clips;
  uint64_t next_clip_id = 1;
  MediaFit fit = MediaFit::Fit;
  /* Shown in shades of grey, and/or tinted towards `tint` by `tint_amount` (a light table's look,
   * so drawing over it stands out). */
  bool greyscale = false;
  Color tint = {0.2f, 0.45f, 1.0f, 1.0f};
  float tint_amount = 0.0f;
  /* Drawn over everything (and unmasked), rather than at its place in the layer stack. */
  bool in_front = false;
  /* Sound: how loud (1: as recorded), and silenced. */
  float volume = 1.0f;
  bool muted = false;
};

/* A layer's masks are a list of rows read top to bottom, as After Effects' mask modes (and CSS's
 * mask-composite), each naming layers as its track mattes do: the shape of a row is all its layers
 * together, as drawn (before any masks of their own; their Shape and Fill Every Shape say which parts), and its mode says what it does to what the rows above
 * show. Add shows inside it too, Subtract hides inside it, Intersect keeps only where it overlaps
 * what is shown, Difference flips where it overlaps. With nothing above, a list that starts with
 * Subtract or Intersect starts from everything shown (so "outside the mouth" is one row), else from
 * nothing. A group's masks apply to everything in it, as well as each layer's own. Masks name layers
 * by id, so the order of the layers never matters, and a hidden layer still masks. Clip to Below
 * (Layer::clip_to_below) is a quick one more: Intersect with the layer below. */
enum class MaskMode : uint8_t { Add, Subtract, Intersect, Difference };
const char *mask_mode_identifier(MaskMode mode);
bool mask_mode_from_identifier(const std::string &identifier, MaskMode *r_mode);
/* What an Inside/Outside mask of files from before mask rows does at its place in the list (they
 * all had to hold): the first Inside adds, a later one intersects, Outside subtracts. */
MaskMode mask_mode_from_inside_outside(bool outside, bool first);
struct LayerMask;
/* Whether a row cuts its shape away (drawn as a cut: Subtract, or Intersect inverted). */
bool mask_cuts(const LayerMask &mask);
/* The rows above that end lines (End Lines at Edge), for mask_row_ends_lines. */
struct MaskLineEnding {
  bool active = false;
  bool flat = false;
};
/* Whether row `m` ends the masked layer's lines, and with which Ends (`r_flat`), given the rows above
 * it that are on (`above`, updated for the rows after): its own End Lines at Edge, or, for an Add row
 * after one that ends lines, that row's, so the Add row brings back the lines it ended. */
bool mask_row_ends_lines(const LayerMask &m, MaskLineEnding *above, bool *r_flat);
/* A mask's Extend: the layers' shapes stretched to the edge of everything in one direction (the mask
 * layer's own up, down, left or right, so it turns with the layer and its bone): "Outside the mouth
 * line, extended up" hides the mouth and everything above it. */
enum class MaskExtend : uint8_t { None, Up, Down, Left, Right };
const char *mask_extend_identifier(MaskExtend extend);
bool mask_extend_from_identifier(const std::string &identifier, MaskExtend *r_extend);
/* What of the masked layer a row cuts: all of it, only its fills, or only its lines. */
enum class MaskCuts : uint8_t { All, Fills, Lines };
const char *mask_cuts_identifier(MaskCuts cuts);
bool mask_cuts_from_identifier(const std::string &identifier, MaskCuts *r_cuts);

struct LayerMask {
  MaskMode mode = MaskMode::Add;
  /* The row's shape inside out: everything but its layers. */
  bool invert = false;
  std::vector<uint64_t> layers;
  /* Masking in the viewport; off, it's kept but doesn't mask there (to see what it cuts). */
  bool enabled = true;
  /* Masking in renders. */
  bool render = true;
  /* How the layers' shapes mask: grown by `grow` pixels (on screen; 1 hides thin seams along the
   * edges) or shrunk by a negative one. The Masking tab's Shape: As Drawn (fills and strokes), Fills
   * Only (`fills_only`) or Strokes Only (`strokes_only`); and its Fill Every Shape (`filled`): every
   * shape as if it were filled, an outline-only shape too (an open one closed by a straight line), as
   * Rive clips with a shape's path (nothing to do with Strokes Only). */
  float grow = 0.0f;
  bool fills_only = false;
  bool strokes_only = false;
  bool filled = false;
  /* With Fills Only: the fills cut at their lines' inner edge rather than down their middle (so a
   * layer cut by this one shows over the whole line), whatever the zoom, as covering it with a copy
   * of the fills would. */
  bool inside_stroke = false;
  /* End Lines at Edge: the masked layer's lines end where they cross this row's shape (worked out
   * along them, before a brush lays its dabs) instead of being sliced by it; with Ends Brush the
   * new ends stop short of the edge, whole dabs only (the brush's taper and fade on them), and the
   * row doesn't slice the lines by pixels too; with Flat (`end_lines_flat`) the row's pixel clip then cuts them straight.
   * Fills are masked as always. */
  bool end_lines = false;
  bool end_lines_flat = false;
  /* With Ends: Brush, how far the lines reach past the edge (+) or stop short of it (-), in line
   * widths at the end: 0 stops the line's round end just touching the edge, 0.5 puts its middle
   * there. Animatable (keyed by hand, as settings are). */
  float line_reach = 0.0f;
  Channel<float> line_reach_keys = Channel<float>::by_hand();
  /* Cuts: what of the masked layer the row masks. Lines: only its lines (ended or sliced), its fills
   * showing as if the row weren't there, so two layers can cut each other's lines where they join
   * without both fills going (a hole). Fills: only its fills (End Lines at Edge then does nothing),
   * as Toon Boom cuts colour art apart from line art. */
  MaskCuts cuts = MaskCuts::All;
  MaskExtend extend = MaskExtend::None;
  /* Shown open in the Masking tab. */
  bool show_expanded = true;
  /* Not saved: the row Clip to Below makes (layer_mask_rows), which masks with what its base shows. */
  bool clip = false;
  /* Not saved: a number of its own for drivers to follow it by when rows move (0 until one does). */
  uint64_t uid = 0;
  bool operator==(const LayerMask &o) const
  {
    return mode == o.mode && invert == o.invert && layers == o.layers && enabled == o.enabled && render == o.render &&
           grow == o.grow && fills_only == o.fills_only && strokes_only == o.strokes_only && filled == o.filled &&
           inside_stroke == o.inside_stroke && end_lines == o.end_lines && end_lines_flat == o.end_lines_flat &&
           line_reach == o.line_reach && cuts == o.cuts && extend == o.extend;
  }
};

/* Label colours for the Layers editor, as in Moho. */
enum class LabelColor { None, Red, Orange, Yellow, Green, Blue, Purple, Gray };

/* Bone constraints, Blender's stack with Moho's kinds: run top to bottom on posed frames (never
 * on the setup frame, frame 0), after keys and smart actions. Dynamics, a simulation, always
 * comes last. */
enum class BoneConstraintType {
  /* Moho's angle constraints: the angle (relative to the parent) stays between min and max. */
  LimitAngle,
  /* Moho's independent angle: keeps its angle in the bone layer when the parent turns. */
  IndependentAngle,
  /* Blender's Maintain Volume: the width follows the length however it got there (keys, IK,
   * Stretch To, Copy Scale, dynamics), so the bone keeps its area; it replaces Moho's squash and
   * stretch scaling. */
  MaintainVolume,
  /* Moho's control bones: follow another bone's change (from its setup) by `factor`, `delay`
   * frames behind. */
  CopyAngle,
  CopyPosition,
  CopyScale,
  /* Blender's: point at the target bone's base; also stretch to reach it. */
  TrackTo,
  StretchTo,
  /* Moho's IK target: the bone's tip reaches for the target bone's base, turning `chain` bones up
   * from it (0: up to a bone Ignored by IK, or the root). */
  IK,
  /* Moho's bone dynamics: springs per channel, simulated from frame to frame. */
  Dynamics,
};
inline constexpr int BONE_CONSTRAINT_TYPES = 10;
const char *bone_constraint_identifier(BoneConstraintType type);
const char *bone_constraint_name(BoneConstraintType type);
bool bone_constraint_from_identifier(const std::string &identifier, BoneConstraintType *r_type);

/* One channel of a bone's dynamics: Angle (the tip swings), Position (the head jiggles) or Scale
 * (Squash & Stretch: the bone's length stretches and squashes; Maintain Volume thins it). Movement throws it off where its keys put it and a spring settles it
 * back; see anim/dynamics.cc. */
struct BoneSpring {
  bool enabled = false;
  /* How quickly the swinging dies down once nothing moves it: at 1 in about half a second, at 2
   * twice as fast. A big swing still takes longer to calm than a small one. */
  float settle_speed = 1.0f;
  /* How far past where its keys put it it swings, as a fraction of the way (0 to 0.9): 0 eases
   * in without passing, more swings further and more times. */
  float bounce = 0.3f;
  /* Angle, Position: how much movement throws it off (what it hangs from moving or turning): 1
   * is natural, 0 moves rigidly with the body. */
  float inertia = 1.0f;
  /* Squash & Stretch: how stretchy it is: the head moving along the bone stretches or squashes
   * it, and swinging fast pulls it longer. */
  float stretch = 0.5f;
  /* Whether it has a furthest it can stray from its keys (`limit`, neared softly). Angle:
   * radians; Position: bone layer units; Scale: a fraction of its size. Kept while off. */
  bool use_limit = false;
  float limit = 0.5f;
  /* Keys for the numbers above: they are animatable (the on/off switches aren't). Settings are
   * keyed by hand: I and auto-keying leave them alone until one is keyed with its key button. */
  Channel<float> settle_speed_keys = Channel<float>::by_hand(), bounce_keys = Channel<float>::by_hand(),
                 inertia_keys = Channel<float>::by_hand(),
                 stretch_keys = Channel<float>::by_hand(), limit_keys = Channel<float>::by_hand();
};

struct BoneConstraint {
  BoneConstraintType type = BoneConstraintType::LimitAngle;
  /* Shown in its box's header; the type's name to start with. */
  std::string name;
  bool enabled = true;
  /* Shown open in the Bone Constraints tab. */
  bool expanded = true;
  /* How much of its effect it has, 0-1; animatable. */
  float influence = 1.0f;
  Channel<float> influence_keys;
  /* LimitAngle: radians, relative to the parent. */
  float min_angle = -1.5707964f;
  float max_angle = 1.5707964f;
  /* MaintainVolume: how much the width follows the length (1: keeps its area, in place of its
   * own X scale). */
  float amount = 1.0f;
  /* Copy*, TrackTo, StretchTo, IK: the other bone's id. */
  uint64_t target = 0;
  /* Copy*: how much of the other bone's change it takes. */
  float factor = 1.0f;
  /* Copy*: frames behind. */
  int delay = 0;
  /* IK: how many bones up it turns (0: up to a bone Ignored by IK, or the root). */
  int chain = 0;
  /* IK: how far the chain may stretch to reach (1: not at all, 1.5: half again). */
  float max_stretch = 1.0f;
  /* Dynamics, per channel. */
  BoneSpring angle_spring{.limit = 0.7853982f}, position_spring{.limit = 0.1f},
      scale_spring{.limit = 0.5f};
  /* Dynamics, for the whole bone: how heavy it is (how hard gravity pulls it, in every channel:
   * down for Angle and Position, along the bone for Squash & Stretch), and how much the wind
   * catches it (1 all of it, 0 none; a ribbon catches more than a braid). The wind doesn't follow
   * Weight: it pushes on a bone's surface, not its mass. */
  float weight = 1.0f;
  float wind_catch = 1.0f;
  /* Dynamics: this bone's own gravity, and its own wind, each in place of the document's (radians
   * for the wind's direction). */
  bool own_gravity = false;
  bool own_wind = false;
  /* Whether it has had its own (they start as the document's the first time, and are kept while
   * unticked, so ticking it again brings them back). */
  bool own_gravity_set = false;
  bool own_wind_set = false;
  float gravity = 0.0f;
  float wind = 0.0f;
  float wind_direction = 0.0f;
  /* As Document's. */
  float wind_gusts = 0.0f;
  float wind_gust_speed = 1.0f;
  float wind_turbulence = 0.0f;
  /* Not saved: a number of its own for drivers to follow it by when the stack changes (0 until one
   * does). */
  uint64_t uid = 0;
  /* Keys for Weight, Wind Catch and the six above, as the document's are keyed (by hand, as
   * settings are). */
  Channel<float> weight_keys = Channel<float>::by_hand(), wind_catch_keys = Channel<float>::by_hand();
  Channel<float> gravity_keys = Channel<float>::by_hand(), wind_keys = Channel<float>::by_hand(),
                 wind_direction_keys = Channel<float>::by_hand(), wind_gusts_keys = Channel<float>::by_hand(),
                 wind_gust_speed_keys = Channel<float>::by_hand(), wind_turbulence_keys = Channel<float>::by_hand();
};

/* Where dynamics put a bone right now, in the bone layer's space (runtime). */
struct BoneDynamicsState {
  float angle = 0.0f;
  float2 head;
  /* The length scale (Y). */
  float scale = 1.0f;
  bool angle_on = false, position_on = false, scale_on = false;
};

/* One bone of a bone layer's skeleton, as in Moho's M_Bone. Its frame is the parent bone's
 * (or, for a root, the bone layer's) space moved by `position`, turned by `angle` and scaled by
 * `scale`; the bone runs `length` from head to tail. As in Blender, the bone's own Y axis runs
 * along it (scale.y: its length) and X across (scale.x: its thickness); inside, the frame lays
 * the bone along x, so the solver swaps them. The values at frame 0 are the setup
 * pose; the difference from it is what moves bound artwork. */
/* Blender's bone collections: named sets of a bone layer's bones, and of the drawings inside it that
 * are Editable in Bone Group (whose points are then edited along with the bones). A bone or drawing
 * can be in several. Every bone layer also has All, a fixed collection holding every bone and every
 * editable drawing (nothing is assigned to it or removed from it). As in Blender, a bone shows while
 * any collection holding it shows, so while All shows, everything does. Hiding hides bones, and
 * drawings' points and wires; their artwork still renders. */
struct BoneCollection {
  uint64_t id = 0;
  std::string name;
  bool visible = true;
  /* Blender's star: while any collection is soloed, only soloed ones show. */
  bool solo = false;
};

/* What a bone is drawn as while the viewport shows custom bone shapes: a bone, a pin (Moho's pin
 * bones, which have no length) or another layer's drawing (Custom). */
enum class BoneShape { None, Pin, Custom };
/* Blender's bone colour sets: 0 its Default Colors, 1 to BONE_COLOR_THEME_SETS its theme sets,
 * BONE_COLOR_CUSTOM the bone's own three shades. */
inline constexpr int BONE_COLOR_THEME_SETS = 15;
inline constexpr int BONE_COLOR_CUSTOM = BONE_COLOR_THEME_SETS + 1;

struct Bone {
  uint64_t id = 0;
  std::string name;
  /* The parent bone's id, or 0 for a root. */
  uint64_t parent = 0;
  float2 position;
  /* Radians, counter-clockwise. */
  float angle = 0.0f;
  float length = 0.5f;
  /* Posing only (frame 0 changes sizes, not scale): x across the bone, y along it. */
  float2 scale = {1.0f, 1.0f};
  /* How far automatic weights reach, as a multiple of the length (Moho's bone strength). */
  float strength = 1.0f;
  /* Its colour set (see BONE_COLOR_CUSTOM) and, for Custom, its regular, selected and active shades
   * (display colours): what it is drawn in, whatever its shape. */
  int color_set = 0;
  std::array<Color, 3> custom_colors = {Color{0.6f, 0.6f, 0.6f, 1.0f}, Color{1.0f, 0.55f, 0.0f, 1.0f},
                                        Color{1.0f, 0.75f, 0.31f, 1.0f}};
  /* Connected, as in Blender: the head sits on the parent's tail and moves with it. */
  bool connected = false;
  /* Blender's edit-bone selection: the body, and the round joints at either end. A selected body
   * has both ends selected; a connected bone's head is its parent's tail, one joint. */
  bool selected = false;
  bool select_head = false;
  bool select_tail = false;
  /* The setup pose (frame 0): where the bone rests in its parent's space. Set by editing on frame 0
   * (taken when frame 0's edits are recorded); posing never moves it. */
  float2 setup_position;
  float setup_angle = 0.0f;
  /* Transform and Angle, as the Bone panel shows them away from frame 0: position and angle less
   * the setup's, the Transform measured along the bone's rest axes (x along the bone as it rests,
   * y across it), as Blender's pose location. These are what the position and angle keys hold (so moving the setup on frame 0
   * moves every pose with it, as in Blender); worked out from position and angle whenever the keys
   * are read or written (bone_changes_from_values). */
  float2 transform;
  float turn = 0.0f;
  Channel<float2> position_keys;
  Channel<float> angle_keys;
  Channel<float2> scale_keys;
  /* Top to bottom (see BoneConstraintType). */
  std::vector<BoneConstraint> constraints;
  /* Moho's Ignored by IK: IK chains stop below it and never turn it. */
  bool ik_ignore = false;
  /* Moho's Arc IK solver: a chain ending at this bone bends evenly, as an arc. */
  bool arc_ik = false;
  BoneShape shape = BoneShape::None;
  /* Pin: its size against the standard pin (it stays that size on screen, whatever the zoom). */
  float pin_size = 1.0f;
  /* Custom: the layer whose drawing it is drawn as (its id; 0 for none). */
  uint64_t shape_layer = 0;
  /* The bone collections it is in (their ids, in its bone layer). */
  std::vector<uint64_t> collections;
};

/* Blender-style modifiers, run on a layer's drawing in stack order. Armature is the bones
 * moving the drawing: every layer inside a bone layer has exactly one, added and removed with
 * that bone layer, never deleted by hand; where it sits in the stack decides what the bones
 * move (Mirror above it mirrors the setup drawing, so each half follows its own side's bones). */
enum class ModifierType { Mirror, Armature };

struct Modifier {
  ModifierType type = ModifierType::Mirror;
  std::string name = "Mirror";
  bool show_viewport = true;
  bool show_render = true;
  /* Shown open in the Modifiers tab. */
  bool show_expanded = true;
  /* Mirror: across the layer's vertical axis (x flips) and/or its horizontal one, through
   * `center` in the layer's space. */
  bool axis_x = true;
  bool axis_y = false;
  float2 center;
  /* Points within `merge_distance` of the axis are shared by both halves, so a curve ending
   * on the axis joins its mirror image into one line (and a half outline into a whole one). */
  bool merge = true;
  float merge_distance = 0.002f;
  /* Mirrored points bound to a bone named ..._L follow ..._R (and the other way), as
   * Blender's mirror modifier flips vertex groups. */
  bool mirror_bones = true;
  /* Not saved: a number of its own for drivers to follow it by when the stack changes (0 until one
   * does). */
  uint64_t uid = 0;
};

/* An animatable number known by its path, for things that aren't a layer's own (the document's
 * Simulation settings, on Document::simulation). */
struct CustomChannel {
  std::string path;
  float value = 0.0f;
  /* For a value mirrored elsewhere (a Document field): what both last agreed on, so whichever
   * changes since wins (an edit there, or keys here). */
  float synced = 0.0f;
  /* Settings are keyed by hand (see Channel::keyed_by_hand). */
  Channel<float> keys = Channel<float>::by_hand();
};

struct Layer {
  uint64_t id = 0;
  std::string name;
  /* Extra animatable numbers (see CustomChannel). */
  std::vector<CustomChannel> custom_channels;
  LayerType type = LayerType::Vector;
  bool visible = true;
  /* Drawn in renders (Blender's camera toggle). Off, the layer is a guide: shown in the viewport while
   * its overlays are (and Overlays > Guides is on), never rendered, so overlays off shows the render. */
  bool render_visible = true;
  bool locked = false;
  /* Shown open in the Layers editor (groups). */
  bool expanded = true;
  /* Picked in the Layers editor along with the active layer (Ctrl or Shift click), for
   * Group With Selection. Not saved. */
  bool selected = false;
  LabelColor label_color = LabelColor::None;

  float2 position;
  /* Degrees, counter-clockwise. */
  float rotation = 0.0f;
  float2 scale = {1.0f, 1.0f};
  float opacity = 1.0f;
  /* Keys for the transform and opacity (see anim/channel.hh). */
  Channel<float2> position_keys;
  Channel<float> rotation_keys;
  Channel<float2> scale_keys;
  Channel<float> opacity_keys;
  /* Visibility as an animated channel (stepped): 1 shown, 0 hidden, mirrored into `visible`. */
  Channel<float> visible_keys = [] {
    Channel<float> c;
    c.keyed_by_hand = true;
    c.constant = true;
    return c;
  }();
  float visible_value = 1.0f;
  /* Draw Order (anim/draw_order.hh): 0 Normal, a layer's id for In Front Of it, minus it for
   * Behind it. Keyed like visibility: each key holds until the next. */
  float draw_order = 0.0f;
  Channel<float> draw_order_keys = [] {
    Channel<float> c;
    c.keyed_by_hand = true;
    c.constant = true;
    return c;
  }();

  /* Its masks (see LayerMask). */
  std::vector<LayerMask> masks;
  /* Photoshop's and Procreate's clipping: shown only where the layer below it (in its group, as the
   * Layers editor lists them; the first one down that isn't clipped itself, so several can clip to
   * one) has its shapes, as drawn. Alt-click between two rows in the Layers editor. */
  bool clip_to_below = false;
  /* Clip to Below's Exclude Strokes: clipped only to the base's fills, not its lines (so a
   * shading layer stops inside the outline instead of covering it). */
  bool clip_exclude_strokes = false;

  /* Id of the layer this one shows, or 0. */
  uint64_t reference_source = 0;
  /* A reference's own changes on top of its original (see anim/reference.hh). */
  ReferenceSettings reference;

  VectorDrawing drawing;
  /* Media layers: their clips and how they show. */
  MediaSettings media;
  /* Group children, top of the stack first, as the Layers editor lists them. */
  std::vector<Layer> children;

  /* Bone layers: the skeleton, roots before their children. */
  std::vector<Bone> bones;
  uint64_t next_bone_id = 1;
  uint64_t active_bone = 0;
  /* Moho's layer binding: a layer inside a bone layer can follow one bone as a whole. */
  uint64_t parent_bone = 0;
  /* What Parent Bone moves (Deform): false, the layer as a whole (Layer: a bone's squash and
   * stretch squashes its lines and brush stamps too); true, its points, as if each were weighted
   * all to that bone (Points: lines keep their width and stamps their shape). Groups move as a
   * whole. */
  bool parent_bone_points = false;
  /* Bone layers: their bone collections, in list order, and the active one (its id; 0: All). */
  std::vector<BoneCollection> bone_collections;
  uint64_t next_collection_id = 1;
  uint64_t active_collection = 0;
  /* Bone layers: the All collection's eye and star. */
  bool all_bones_visible = true;
  bool all_bones_solo = false;
  /* Inside a bone layer: its points can be picked and moved along with the bones while the bone
   * layer is the active one (it is in All, and can be put in other collections too). */
  bool edit_with_bones = false;
  /* A drawing that is: the bone collections it is in besides All (their ids). */
  std::vector<uint64_t> collections;
  /* Run on the drawing in order (see ModifierType). */
  std::vector<Modifier> modifiers;
  /* Bone layers: the names of their actions, alternative timelines for the layer and everything
   * inside it (see anim/animation.hh). */
  std::vector<std::string> actions;
  /* Bone layers, runtime: the frame the bones show (for constraints' delays) and where dynamics put
   * them on it, by bone id. */
  int pose_frame = 0;
  std::map<uint64_t, BoneDynamicsState> dynamics_now;

  bool is_group() const { return type == LayerType::Group || type == LayerType::Bone; }
  bool is_bone_layer() const { return type == LayerType::Bone; }
  bool is_media() const { return type == LayerType::Media; }
  /* Has a drawing of its own to show and edit (vector layers and references of them). */
  bool has_drawing() const { return type == LayerType::Vector; }
  bool is_reference() const { return reference_source != 0; }
  /* The bone it follows as a whole (0 for none): a copy posed Same as Original moves with its
   * original's bones, so its own Parent Bone waits until it is given Own Bones. */
  uint64_t followed_bone() const
  {
    return (is_reference() && reference.match_original) || points_follow_bone() ? 0 : parent_bone;
  }
  /* Parent Bone moving its points rather than the layer (parent_bone_points). */
  bool points_follow_bone() const
  {
    return parent_bone != 0 && parent_bone_points && !is_group() && !(is_reference() && reference.match_original);
  }
  Transform2D local_transform() const;

  static const rna::StructRNA &rna();
};

const char *layer_type_name(LayerType type);
/* Colour of a label in the Layers editor, 0 alpha for None. */
Color label_color_value(LabelColor label);

}  // namespace oanim
