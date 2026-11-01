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
#include "data/text.hh"
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
  bool operator==(const ReferencePoint &) const = default;
};

/* A handle turned on a reference (R or S on its points): which one, by its point and the
 * neighbour it points toward along the curve (0: none, at an open end), and `in` for the handle
 * coming in from the previous point. Its offset is a ReferencePoint's. */
struct ReferenceHandleKey {
  uint64_t point = 0, toward = 0;
  bool in = false;
  auto operator<=>(const ReferenceHandleKey &) const = default;
  bool operator==(const ReferenceHandleKey &) const = default;
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
  bool operator==(const ReferenceLook &) const = default;
};

/* A colour swapped on a reference: every shape of the original filled (`fill`) or lined (`line`)
 * with `from` shows `to` instead, the shapes added later too, as recolouring a shared style. */
struct ReferenceSwap {
  Color from, to;
  bool fill = true, line = true;
  bool operator==(const ReferenceSwap &) const = default;
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
  float2 built_position, built_scale, built_shear;
  float built_rotation = 0.0f, built_opacity = 1.0f;
  bool operator==(const ReferenceSettings &) const = default;
};

/* Bone: a group that owns a skeleton and moves its children with it (Moho's bone layer). Media: an
 * image, image sequence, video or sound (data/media_layer.hh). Text: text kept as typed, its letters'
 * outlines its drawing (data/text.hh). Switch: a group that shows one of its children, or none
 * (anim/switch.hh). */
enum class LayerType { Vector, Group, Bone, Media, Text, Switch };

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
  bool operator==(const MediaClip &) const = default;
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
  /* Don't Auto Key: placing, sizing or fading it never keys it (footage is usually set once for
   * the whole shot); keying by hand still does. */
  bool no_auto_key = true;
  /* Sound: how loud (1: as recorded), and silenced. */
  float volume = 1.0f;
  bool muted = false;
  bool operator==(const MediaSettings &) const = default;
};

/* A layer's masks are cards, each a mask of its own, painted as Photoshop's layer masks are: it
 * starts white (everything shown) or black (nothing shown), as its Start With says, and its steps,
 * read top to bottom, each paint their shape onto it: Add (+ Shape) paints white, shows inside it;
 * Subtract (- Shape) black, hides inside it; Invert paints the outside instead. The Masking tab calls a
 * card a Mask (the cut) and its steps Masking Areas, stacked on Show All or Hide All (the Start With). A
 * step's shape is all
 * its layers together, as drawn (before any masks of their own; its Shape and Fill Every Shape say
 * which parts), named as track mattes name theirs. Each card says what of the layer it masks
 * (Affects: all, its fills or its lines) and how it cuts lines (sliced, or ended at its shape's
 * edge), and the layer shows only where every card that masks that part shows it (they multiply).
 * A group's masks apply to everything in it, as well as each layer's own. Masks name layers by id,
 * so the order of the layers never matters, and a hidden layer still masks. Clip to Below
 * (Layer::clip_to_below) is a quick card more: everything but outside the layer below. */
enum class MaskMode : uint8_t { Add, Subtract };
const char *mask_mode_identifier(MaskMode mode);
bool mask_mode_from_identifier(const std::string &identifier, MaskMode *r_mode);
struct MaskStep;
struct LayerMask;
/* Rows saved before cards, with their modes' old names: INTERSECT (keep only inside) is Subtract of
 * the outside, DIFFERENCE is read as Add. `identifier` is the saved mode (or empty: an Inside/Outside
 * mask from before rows, which all had to hold: the first Inside adds, a later one keeps only inside,
 * Outside subtracts). Returns false for a name it doesn't know. */
bool mask_step_from_old_mode(const std::string &identifier, bool outside, bool first, MaskStep *s);
/* Whether a step cuts its shape away (Subtract of the inside). */
bool mask_step_cuts(const MaskStep &step);
/* Whether a card cuts away (its first step does, on a white start), for the outline drawn dashed. */
bool mask_cuts(const LayerMask &card);
/* Whether a card ends the masked layer's lines at its shape's edge (End Lines at Edge, on a card that
 * masks lines), and with which Ends (`r_flat`). */
bool mask_card_ends_lines(const LayerMask &card, bool *r_flat);
/* What a masking area does to what's below it: paints it white (Add; with Invert, its outside), paints it
 * black (Remove), or keeps only what's inside it (KeepOnly: black outside it). */
enum class MaskVerb : uint8_t { Add, Remove, KeepOnly };
const char *mask_verb_identifier(MaskVerb verb);
bool mask_verb_from_identifier(const std::string &identifier, MaskVerb *r_verb);
MaskVerb mask_step_verb(const MaskStep &step);
void mask_step_set_verb(MaskStep &step, MaskVerb verb);
/* Whether a step has nothing to paint: no layers, and not a group with something in it. */
bool mask_step_empty(const MaskStep &step);
/* What's wrong with group `index` of `card`, for the Masking tab to say; null when nothing is. */
const char *mask_group_problem(const LayerMask &card, size_t index);
/* `card` with its groups turned into ordinary steps where they can be: what the renderer, picking, End Lines and
 * export read (layer_mask_cards). One that can't (a mix of paints, above the base) stays a group step, and each of
 * those works out its shape on the side and paints it as a layer's. */
LayerMask mask_card_flatten(const LayerMask &card);
/* A card's name as shown: its own, or "Mask 2" by its place (`index`, from 0). */
std::string mask_card_name(const LayerMask &card, int index);
/* Every layer a card's steps name, once each. */
std::vector<uint64_t> mask_card_layers(const LayerMask &card);
/* A mask's Extend: the layers' shapes stretched to the edge of everything in one direction (the mask
 * layer's own up, down, left or right, so it turns with the layer and its bone): "Outside the mouth
 * line, extended up" hides the mouth and everything above it. */
enum class MaskExtend : uint8_t { None, Up, Down, Left, Right };
const char *mask_extend_identifier(MaskExtend extend);
bool mask_extend_from_identifier(const std::string &identifier, MaskExtend *r_extend);
/* What of the masked layer a card masks: all of it, only its fills, or only its lines. */
enum class MaskCuts : uint8_t { All, Fills, Lines };
const char *mask_cuts_identifier(MaskCuts cuts);
bool mask_cuts_from_identifier(const std::string &identifier, MaskCuts *r_cuts);
/* How a step's layers make its shape: all of them together (Union), the first with the others cut
 * out of it (Difference), or only where every one of them is (Intersection), as Blender's Boolean. */
enum class MaskCombine : uint8_t { Union, Difference, Intersection };
const char *mask_combine_identifier(MaskCombine combine);
const char *mask_combine_name(MaskCombine combine);
bool mask_combine_from_identifier(const std::string &identifier, MaskCombine *r_combine);

/* One step of a card's chain: its layers' shape painted on the card's mask. */
struct MaskStep {
  MaskMode mode = MaskMode::Add;
  /* The shape inside out: everything but its layers. */
  bool invert = false;
  std::vector<uint64_t> layers;
  /* How `layers` make the shape (in their order: Difference keeps the first). */
  MaskCombine combine = MaskCombine::Union;
  /* How the layers' shapes mask: grown by `grow` pixels (on screen; 1 hides thin seams along the
   * edges) or shrunk by a negative one. Shape: As Drawn (fills and strokes), Fills Only
   * (`fills_only`) or Strokes Only (`strokes_only`); and Fill Every Shape (`filled`): every shape as
   * if it were filled, an outline-only shape too (an open one closed by a straight line), as Rive
   * clips with a shape's path (nothing to do with Strokes Only). */
  float grow = 0.0f;
  bool fills_only = false;
  bool strokes_only = false;
  bool filled = false;
  /* With Fills Only: the fills cut at their lines' inner edge rather than down their middle (so a
   * layer cut by this one shows over the whole line), whatever the zoom, as covering it with a copy
   * of the fills would. */
  bool inside_stroke = false;
  MaskExtend extend = MaskExtend::None;
  /* Shown open (its shape settings) in the Masking tab. */
  bool show_expanded = false;
  /* Not saved: Clip to Below's (layer_mask_cards), which masks with what its base shows. */
  bool clip = false;
  /* A group: `children` build a shape of their own from nothing, as a layer's would be, for this step
   * to paint (mask_card_flatten). */
  bool group = false;
  std::vector<MaskStep> children;
  /* Show All (Add) or Hide All (Subtract): the whole canvas, not a layer's shape. */
  bool whole = false;
  /* A group's or a global area's name. */
  std::string name;
  /* A global masking area, as a shared material: every step with this number (0: none) is kept alike
   * but for its own paint, so nothing that reads steps has to know (mask_area_sync, mask_ops.cc). */
  uint64_t global = 0;
  /* Not saved: a flattened step's place in its card, (step << 8) | (child + 1). */
  int order = 0;
  bool operator==(const MaskStep &o) const
  {
    return mode == o.mode && invert == o.invert && layers == o.layers && combine == o.combine && grow == o.grow &&
           fills_only == o.fills_only &&
           strokes_only == o.strokes_only && filled == o.filled && inside_stroke == o.inside_stroke &&
           extend == o.extend && clip == o.clip && group == o.group && children == o.children && name == o.name &&
           global == o.global && whole == o.whole;
  }
};

/* A card: one mask, its steps painted on its start, with how it cuts. */
struct LayerMask {
  /* Its name in the Masking tab ("Ear join"); empty, it's "Mask 1", "Mask 2"... by its place. */
  std::string name;
  /* Start With: white, everything shown, for the steps to paint on; false, black, nothing shown. */
  bool start_shown = true;
  std::vector<MaskStep> steps;
  /* Masking in the viewport; off, it's kept but doesn't mask there (to see what it cuts). */
  bool enabled = true;
  /* Masking in renders. */
  bool render = true;
  /* Affects: what of the masked layer the card masks. Lines: only its lines (ended or sliced), its
   * fills showing as if the card weren't there, so two layers can cut each other's lines where they
   * join without both fills going (a hole). Fills: only its fills (End Lines at Edge then does
   * nothing), as Toon Boom cuts colour art apart from line art. */
  MaskCuts cuts = MaskCuts::All;
  /* End Lines at Edge: the masked layer's lines end where they cross the edge of the card's shape
   * (worked out along them, before a brush lays its dabs) instead of being sliced by it; with Ends
   * Brush the new ends stop short of the edge, whole dabs only (the brush's taper and fade on them),
   * and the card doesn't slice the lines by pixels too; with Flat (`end_lines_flat`) the card's pixel
   * clip then cuts them straight. Fills are masked as always. */
  bool end_lines = false;
  bool end_lines_flat = false;
  /* With Ends: Brush, how far the lines reach past the edge (+) or stop short of it (-), in line
   * widths at the end: 0 stops the line's round end just touching the edge, 0.5 puts its middle
   * there. Animatable (keyed by hand, as settings are). */
  float line_reach = 0.0f;
  Channel<float> line_reach_keys = Channel<float>::by_hand();
  /* Shown open in the Masking tab. */
  bool show_expanded = true;
  /* Not saved: the card Clip to Below makes (layer_mask_cards). */
  bool clip = false;
  /* Not saved: a number of its own for drivers to follow it by when cards move (0 until one does). */
  uint64_t uid = 0;
  bool operator==(const LayerMask &o) const
  {
    return name == o.name && start_shown == o.start_shown && steps == o.steps && enabled == o.enabled &&
           render == o.render &&
           cuts == o.cuts && end_lines == o.end_lines && end_lines_flat == o.end_lines_flat &&
           line_reach == o.line_reach;
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
  /* Keeps the bone's head (or the drawings it carries) inside a closed shape of another layer, as
   * that shape is drawn on the frame: a pupil inside its eye white, at any angle of the head. */
  LimitShape,
  /* Blender's Child Of: moves (turns, scales) with the target bone's change from its setup pose, as if
   * parented to it, each part switchable; keyed Influence lets it pick up or drop the target. */
  ChildOf,
  /* Eyes: the bone (a pupil) shifts toward the target bone (a look target), which stands in front of
   * the face at its Depth: each eye a ball, turning to look at it, the pupil where that turn puts it. */
  LookAt,
};
inline constexpr int BONE_CONSTRAINT_TYPES = 13;

/* LimitShape: what has to stay inside the shape, the bone's head or the whole of the drawings it
 * carries. */
enum class ShapeFit { Center, Whole };
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
  /* Squash & Stretch only: `limit` is Max Stretch, and this the furthest it can squash, as a part of
   * its length (0.3: down to 0.7 of it), neared softly the same way. Kept while off. Squashing never
   * goes below a tenth of its length, limit or not. */
  bool use_squash_limit = false;
  float squash_limit = 0.3f;
  /* Keys for the numbers above: they are animatable (the on/off switches aren't). Settings are
   * keyed by hand: I and auto-keying leave them alone until one is keyed with its key button. */
  Channel<float> settle_speed_keys = Channel<float>::by_hand(), bounce_keys = Channel<float>::by_hand(),
                 inertia_keys = Channel<float>::by_hand(),
                 stretch_keys = Channel<float>::by_hand(), limit_keys = Channel<float>::by_hand(),
                 squash_limit_keys = Channel<float>::by_hand();
  bool operator==(const BoneSpring &) const = default;
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
  /* LimitAngle: radians, how far the bone may turn each way from its setup angle (as the Bone panel's
   * Angle shows its turn), so the limits move with the rest. */
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
  /* CopyPosition: off, the bone goes to where the target's base is (Blender's Copy Location), Factor
   * the way there; on, it copies the target's move from its setup pose instead (Moho's control bones;
   * files saved before this keep it on). */
  bool follow_move = false;
  /* IK: how many bones up it turns (0: up to a bone Ignored by IK, or the root). */
  int chain = 0;
  /* IK: how far the chain may stretch to reach (1: not at all, 1.5: half again). */
  float max_stretch = 1.0f;
  /* LimitShape: the layer and its shape (ids; shape 0: the layer's first closed shape) to stay
   * inside; what stays inside, and with Whole the layer whose drawing it is (0: the layers that
   * follow this bone); how far that may go past the edge (0-1 of its size); and how soft the edge is
   * (bone layer units: it slows over this much before stopping). */
  uint64_t shape_layer = 0;
  uint64_t shape_id = 0;
  ShapeFit shape_fit = ShapeFit::Whole;
  uint64_t fit_layer = 0;
  float overlap = 0.0f;
  float softness = 0.0f;
  /* Blender's Affect Transform: moving the bone past the edge stops its own position there too (what
   * its keys get), not only where it shows. */
  bool affect_transform = true;
  /* ChildOf: which of the target's change it takes. */
  bool child_position = true;
  bool child_rotation = true;
  bool child_scale = true;
  /* LookAt: how far the pupil moves looking fully sideways and up or down (bone layer units: the eye's
   * radius), worked out from the Limit to Shape below it when `range_auto`; `squash`, shown as Warp: how
   * much the layers following the bone wrap round the eyeball (0-1, data/bones.hh bone_look_warp); and how far each eye turns outward
   * from the target (radians), as real eyes don't quite meet on it. */
  bool range_auto = true;
  float2 look_range = {0.05f, 0.05f};
  float squash = 1.0f;
  float divergence = 0.0f;
  /* LookAt, the eyeball: its size (half its width and height, bone layer units: a cartoon eye is a
   * stretched ball), from its Eye Shape at frame 0 when `eyeball_auto`; which way its front faces (yaw
   * and pitch, radians: 0 at the camera, a quarter turn in profile), keyed in a head turn so the pupil
   * sits and foreshortens as the turned eye shows it; how far the eye turns from there to look, sideways,
   * up and down (a real eye turns least upward); and the layers following the bone left unwarped (a shine). */
  bool eyeball_auto = true;
  float2 eyeball_size = {0.05f, 0.05f};
  float2 facing;
  Channel<float2> facing_keys = Channel<float2>::by_hand();
  float max_side = 0.7853982f;
  float max_up = 0.5235988f;
  float max_down = 0.7853982f;
  std::vector<uint64_t> keep_round;
  /* LookAt: a bone whose head (as rigged) is the middle of the eyeball (0: the ball's front is the
   * pupil as rigged). The pupil drawn off its front already points that way at rest. */
  uint64_t eyeball_center = 0;
  /* LookAt with Eyeball Auto, worked out every frame and never saved: from the Eye Shape at frame 0. */
  float2 eyeball_auto_size;
  /* LookAt with Range Auto, worked out every frame and never saved (anim/shape_limit.hh): the reach
   * inside the bone's Limit to Shape (0 when it has none). */
  float2 look_auto_range;
  /* LimitShape, worked out every frame and never saved (anim/shape_limit.hh): the shape's outline
   * and the carried drawing's outline around the head, both in the bone layer's space; empty when
   * there is nothing to stay inside. */
  std::vector<float2> shape_region;
  std::vector<float2> shape_footprint;
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
  bool operator==(const BoneConstraint &) const = default;
};

/* Where dynamics put a bone right now, in the bone layer's space (runtime). */
struct BoneDynamicsState {
  float angle = 0.0f;
  float2 head;
  /* The length scale (Y). */
  float scale = 1.0f;
  bool angle_on = false, position_on = false, scale_on = false;
  bool operator==(const BoneDynamicsState &) const = default;
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
  bool operator==(const BoneCollection &) const = default;
};

/* What a bone is drawn as while the viewport shows custom bone shapes: a bone, a pin (Moho's pin
 * bones, which have no length) or another layer's drawing (Custom). */
enum class BoneShape { None, Pin, Custom };
/* Blender's bone colour sets: 0 its Default Colors, 1 to BONE_COLOR_THEME_SETS its theme sets,
 * BONE_COLOR_CUSTOM the bone's own three shades. */
inline constexpr int BONE_COLOR_THEME_SETS = 15;
inline constexpr int BONE_COLOR_CUSTOM = BONE_COLOR_THEME_SETS + 1;

/* Blender's Inherit Scale, cut to what a 2D rig needs: the parent's change in size as it is (its squash
 * and stretch too), only its overall size (Uniform), or none of it. */
enum class InheritScale { Full, Uniform, None };

struct Bone {
  uint64_t id = 0;
  std::string name;
  /* The parent bone's id, or 0 for a root. */
  uint64_t parent = 0;
  /* A bone swap (anim/bone_family.hh): this bone stands in for bone `swap_bone` of bone layer
   * `swap_layer`, its counterpart in another child of a switch (a bent leg's shin for the straight
   * leg's). 0: none. Swaps don't share poses; they say which bone is which across children. */
  uint64_t swap_layer = 0;
  uint64_t swap_bone = 0;
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
  /* Relations, as Blender's. Posing (the setup pose always hangs from the parent as it rests): whether
   * the bone turns as its parent has turned from its setup pose, and how much of its parent's change in
   * size it takes. Its head goes where the parent takes it either way. Local Location: its Transform
   * reads along the bone as it rests (X down it), else along its parent; the keys are the same. */
  bool inherit_rotation = true;
  InheritScale inherit_scale = InheritScale::Full;
  bool local_location = true;
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
  /* A look target's Depth: how far in front of the face it stands (bone layer units), for the Look At
   * constraints aiming at it: close, the eyes cross; far, they look the same way. Keyable. */
  float depth = 1.0f;
  Channel<float> depth_keys;
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
  bool operator==(const Bone &) const = default;
};

/* Blender-style modifiers, run on a layer's drawing in stack order. Armature is the bones
 * moving the drawing: every layer inside a bone layer has exactly one, added and removed with
 * that bone layer, never deleted by hand; where it sits in the stack decides what the bones
 * move (Mirror above it mirrors the setup drawing, so each half follows its own side's bones). */
enum class ModifierType { Mirror, Armature, Boolean, Bevel, Resample, CurveDeform, CageDeform, MeshDeform };

/* The deform modifiers (anim/vector_deform.hh): a shape on another layer bends this one. Curve Deform follows an
 * open line, Cage Deform one closed shape, Mesh Deform a grid or a mesh of cells drawn by hand. They share their
 * settings (the deform_* fields). */
inline bool modifier_is_deform(ModifierType type)
{
  return type == ModifierType::CurveDeform || type == ModifierType::CageDeform || type == ModifierType::MeshDeform;
}
/* "Curve Deform", "Cage Deform", "Mesh Deform"; and as files and Python name them, "CURVE_DEFORM"... */
inline const char *deform_type_name(ModifierType type)
{
  return type == ModifierType::CurveDeform ? "Curve Deform" : type == ModifierType::CageDeform ? "Cage Deform" : "Mesh Deform";
}
inline const char *deform_type_id(ModifierType type)
{
  return type == ModifierType::CurveDeform ? "CURVE_DEFORM" : type == ModifierType::CageDeform ? "CAGE_DEFORM" : "MESH_DEFORM";
}

/* Bevel: which corners it cuts (Blender's Limit Method): every one, those the outline turns sharply
 * at, or a point group's; and what its Amount measures (Width Type): how far along each side the cut
 * starts (Offset), how long the cut is (Width), how far it is from the corner (Depth), or how much of
 * each side it takes (Percent). */
enum class BevelLimit { None, Angle, Group };
enum class BevelWidth { Offset, Width, Depth, Percent };

/* Boolean: how its targets' shapes go with the layer's own. */
enum class MergeOperation { Union, Subtract, Intersect };

/* A layer whose shapes a Boolean modifier takes in, and which of them (their ids; empty: every
 * shape that goes all the way round). */
struct MergeTarget {
  uint64_t layer = 0;
  std::vector<uint64_t> shapes;
  bool operator==(const MergeTarget &) const = default;
};

struct Modifier;
/* Whether Apply can make it part of the drawing (all but Armature, which comes with the bone layer). */
bool modifier_can_apply(const Modifier &m);
/* Whether it can add points to the drawing (Mirror, Bevel, Resample, Boolean, and the deforms' Precision): the
 * ones with a Show Points button. */
bool modifier_makes_points(const Modifier &m);

/* How Cage Deform bends what is in its cage: Mean Value (mean value coordinates: smooth, the outline exact, but the
 * inside squashes where a side bulges or stretches), Rigid (as rigid as possible: the inside keeps its shape as far as
 * it can, the outline still exact) or Envelope (Toon Boom Harmony's: each side of the cage carries what lies near it,
 * at its distance from it, so lines along the outline keep their width; the sides blended by nearness, as much as
 * Blend says). */
enum class CageMethod { MeanValue, Rigid, Envelope };

/* A Cage Method's name in files and Python ("MEAN_VALUE", "RIGID", "ENVELOPE"), and back (false for none: a file's
 * "GREEN", a method since removed, opens as Mean Value). */
const char *cage_method_id(CageMethod method);
bool cage_method_from_id(const std::string &id, CageMethod &r_method);

enum class MeshMethod { Cells, Envelope };

/* A Mesh Method's name in files and Python ("CELLS", "ENVELOPE"), and back. */
const char *mesh_method_id(MeshMethod method);
bool mesh_method_from_id(const std::string &id, MeshMethod &r_method);

struct Modifier {
  ModifierType type = ModifierType::Mirror;
  std::string name = "Mirror";
  bool show_viewport = true;
  bool show_render = true;
  /* Show Points: the points it adds marked in the viewport (hollow teal dots, where they end up; never picked). */
  bool show_points = false;
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
  /* Boolean: the layer's own shapes (ids; empty: every one that goes all the way round) and the
   * other layers' shapes made one outline with them, as posed; the result takes the first own
   * shape's place and look, and the target shapes it took in stop drawing on their own layers.
   * Smoothing rounds the corners where the outlines meet (a radius in the layer's units); it can
   * be keyed. */
  std::vector<uint64_t> merge_shapes;
  std::vector<MergeTarget> merge_targets;
  MergeOperation merge_operation = MergeOperation::Union;
  float merge_smoothing = 0.0f;
  /* Corners Only: the layer's shapes just grow the rounded corners where they meet the others (a
   * fill layer under an outline layer's merge), taking nothing from them. */
  bool merge_corners_only = false;
  /* Bevel (anim/bevel.hh): the corners turning more than `bevel_angle` (radians), or the points of
   * point group `bevel_group` (by name; their weights scale the amount); cut `bevel_amount` from the
   * corner each way (keyable), the cut's Profile 0.25 straight, 0.5 round, 1 square, below 0.25
   * inward; Clamp Overlap keeps two cuts on one segment from crossing. */
  BevelLimit bevel_limit = BevelLimit::Angle;
  BevelWidth bevel_width = BevelWidth::Offset;
  float bevel_angle = 0.5235988f;
  std::string bevel_group;
  float bevel_amount = 0.02f;
  Channel<float> bevel_amount_keys = Channel<float>::by_hand();
  float bevel_profile = 0.5f;
  bool bevel_clamp = true;
  Channel<float> merge_smoothing_keys = Channel<float>::by_hand();
  /* Resample (anim/subdivide.hh; it was called Subdivide): By Length (a point at least every `subdiv_spacing` along each
   * segment, in the setup pose; `subdiv_render_spacing` for renders, 0: the same) or By Count
   * (`subdiv_pieces` per segment), never more than `subdiv_max_pieces`; the new points' weights Blend
   * their end points' or come From Bones; only the segments touching point group `subdiv_group` when
   * it names one. */
  bool subdiv_by_length = true;
  float subdiv_spacing = 0.05f;
  float subdiv_render_spacing = 0.0f;
  int subdiv_pieces = 4;
  int subdiv_max_pieces = 64;
  bool subdiv_from_bones = false;
  /* The new points blend the bones' turns as rigid motions, so they don't curl when bones turn far apart
   * (Blender's Preserve Volume); off, they blend the bones' matrices as other points do. */
  bool subdiv_preserve_volume = true;
  std::string subdiv_group;
  /* Curve, Cage and Mesh Deform (anim/vector_deform.hh): shape `deform_shape` (its id) of layer `deform_layer` bends this
   * layer as it moves from where it is on the setup frame: a grid by its cells, a closed shape as a cage
   * round what is in it, an open line by what lies along it. How much each point follows it: Automatic
   * (every point of the layer fully, past the shape too), or Custom, point group `deform_group`'s weights
   * (points it has no weight for take the automatic one). A point's weight is also how much it follows the deform rather than the
   * bones. `deform_strength` (keyable) scales every weight. `deform_precision` > 0 first adds points where
   * the deform would bend a segment further than that from what its points and handles can follow
   * (Illustrator's Fidelity), so straight lines bend too. A line: what lies along it keeps its distance along
   * it, spread as the line grows when `deform_stretch`, and a Curve Deform follows only the stretch of its shape from
   * point `deform_part_from` to `deform_part_to` (their ids, 0: all of it; a closed shape needs one); */
  uint64_t deform_layer = 0;
  uint64_t deform_shape = 0;
  bool deform_custom = false;
  std::string deform_group;
  float deform_strength = 1.0f;
  Channel<float> deform_strength_keys = Channel<float>::by_hand();
  float deform_precision = 0.002f;
  /* Resample Self: each segment of the bent drawing is sampled evenly along it, the samples are bent, and the
   * drawing's own points go where theirs went, each segment's handles fitted to its bent samples (as a sculpt
   * stroke fits a line back): the drawing bends truer with only its own points (Precision adds none). */
  bool deform_resample_self = false;
  /* Not saved: read from a file of the single Vector Deform modifier, its type still to be told by its shape. */
  bool deform_legacy = false;
  bool deform_stretch = true;
  uint64_t deform_part_from = 0;
  uint64_t deform_part_to = 0;
  /* Cage Deform's Method: how what is in the cage follows it. */
  CageMethod deform_cage_method = CageMethod::MeanValue;
  /* Mesh Deform's Method: each cell bending what is in it, or with Envelope the outline's sides also keeping what lies
   * near them at its distance from them, in the outer cells. */
  MeshMethod deform_mesh_method = MeshMethod::Cells;
  /* Mesh Deform on a drawn grid: its rows and columns as smooth lines through their nodes (GridSurface::smooth). */
  bool deform_smooth = false;
  /* Envelope's Blend: how evenly the sides near a place share it: 0, the nearest leads sharply (as the cube of how
   * near it is); 1, gently (as how near it is). */
  float deform_blend = 0.5f;
  /* Not saved: a number of its own for drivers to follow it by when the stack changes (0 until one
   * does). */
  uint64_t uid = 0;
  /* Not saved, set on the copy a group's deform runs as on a drawing inside it (anim/deform.cc): the group, measured
   * from as it stood at setup, so what moves the group itself moves what is in it on top of the deform. */
  uint64_t deform_from_group = 0;
  /* With it: the drawing is moved by the bones that move the shape, so the shape's rest is posed by them too and only
   * its change of shape bends the drawing (their movement would count twice). */
  bool deform_rest_posed = false;
  bool operator==(const Modifier &) const = default;
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
  bool operator==(const CustomChannel &) const = default;
};

/* One pose a fix action corrects: where each of its drivers is (that smart action's frame), and the fix's
 * frame holding the correction for it (its keys there). */
struct FixPose {
  std::vector<float> at;
  int frame = 10;
  /* It has had keys: once they are all deleted, the pose goes (fix_actions_tidy). */
  bool keyed = false;
  bool operator==(const FixPose &) const = default;
};
/* A fix action (a corrective, Maya's combination shapes): an action of a bone layer that plays by itself
 * when the smart actions it names (its drivers) play together. It corrects one or more poses of theirs, each
 * on a frame of its own (10, 20, ...): exactly at a pose its keys there play, between poses they blend by
 * how close the pose is to each, and it fades out toward rest as its drivers do (anim/animation.hh). */
struct FixAction {
  std::string name;
  std::vector<std::string> drivers;
  std::vector<FixPose> poses;
  bool operator==(const FixAction &) const = default;
};

/* A number that changes whenever what it is on may have changed (see document_touch_all): caches of what
 * is drawn of a layer (the renderer's, the overlays') keep it while the number stays. Not saved, and never
 * part of comparing (two layers alike are alike whatever their numbers). */
struct Revision {
  uint64_t value = 0;
  bool operator==(const Revision &) const { return true; }
};

struct Layer {
  uint64_t id = 0;
  Revision revision;
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
  /* Shear, degrees (as Moho's layer shear, Flash's skew, Spine's bone shear): how far its X axis (x) and its
   * Y axis (y) tilt from square, counter-clockwise, between turning and scaling (see local_transform). */
  float2 shear;
  float opacity = 1.0f;
  /* Keys for the transform and opacity (see anim/channel.hh). */
  Channel<float2> position_keys;
  Channel<float> rotation_keys;
  Channel<float2> scale_keys;
  Channel<float2> shear_keys;
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
  /* Switch layers (anim/switch.hh): the child shown, as keyed: its id, 0 for none, SWITCH_RIG to hand
   * it back to the rig. Keyed like visibility: each key holds until the next; frame 0 is the default. */
  float switch_value = 0.0f;
  Channel<float> switch_keys = [] {
    Channel<float> c;
    c.constant = true;
    return c;
  }();
  /* Runtime, worked out every frame (switches_resolve): the child a switch shows (0: none), what its
   * smart actions pick (switch_rig, when switch_rig_set), and for every layer whether a switch above
   * it shows another child, which hides it without touching its own visibility. */
  /* Switch layers linked by name (Document::switch_links): the group, 0 for none; `switch_link_off`:
   * unlinked by hand, so same-named switches don't link it again. `link_same_named`: on a switch, the
   * same-named switches at the same place inside its different children link (a Mouth in each view of
   * Heads). A switch's child: the other names it answers to (an alias: the profile "Oh" for O). */
  uint64_t switch_link = 0;
  bool switch_link_off = false;
  bool link_same_named = true;
  std::vector<std::string> switch_aliases;
  /* Runtime: a child's name when links last looked (a rename shows as a change); a linked switch's name
   * the keys want that it has no child for, now (it keeps its last match). */
  std::string link_name_seen;
  std::string switch_missing;
  uint64_t switch_shown = 0;
  float switch_rig = 0.0f;
  bool switch_rig_set = false;
  bool switched_out = false;
  /* Runtime: switched out as the viewport shows it, where the child being edited is shown in place of
   * the pick (see switches_resolve). */
  bool edit_out = false;

  /* Its masks, as cards (see LayerMask). */
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
  /* Text layers: the text; and what it was when `drawing` was last made from it (runtime: a text no
   * longer matching has its letters made again, see text/text_layer.hh). */
  TextSettings text;
  TextSettings text_built;
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
   * all to that bone (Points: lines keep their width and stamps their shape). Points for new
   * drawings; groups move as a whole. */
  bool parent_bone_points = true;
  /* Bone layers: their bone collections, in list order, and the active one (its id; 0: All). */
  std::vector<BoneCollection> bone_collections;
  uint64_t next_collection_id = 1;
  uint64_t active_collection = 0;
  /* Bone layers: the All collection's eye and star. */
  bool all_bones_visible = true;
  bool all_bones_solo = false;
  /* Bone layers inside a bone layer: a sub bone group, whose bones show and can be picked with the bone
   * layer around it while either is worked in (a leg's own bones in a switch of legs, hung from the
   * hip with Parent Bone); only while it is shown. */
  bool sub_bone_group = true;
  /* Inside a bone layer: its points can be picked and moved along with the bones while the bone
   * layer is the active one (it is in All, and can be put in other collections too). */
  bool edit_with_bones = false;
  /* The deform modifiers of the groups around this drawing leave it alone. */
  bool ignore_group_deforms = false;
  /* A group (or bone group) picked or active: every drawing inside it, however deep, is edited along
   * with it, as if each were picked too. On for groups, off for bone groups (whose drawings come
   * with their bone collections instead). */
  bool edit_descendants = true;
  /* A drawing that is: the bone collections it is in besides All (their ids). */
  std::vector<uint64_t> collections;
  /* Run on the drawing in order (see ModifierType). */
  std::vector<Modifier> modifiers;
  /* Bone layers: the names of their actions, alternative timelines for the layer and everything
   * inside it (see anim/animation.hh). */
  std::vector<std::string> actions;
  /* Bone layers: which of those actions are fix actions, and what drives them. */
  std::vector<FixAction> fix_actions;
  /* Bone layers, runtime: the frame the bones show (for constraints' delays) and where dynamics put
   * them on it, by bone id. */
  int pose_frame = 0;
  std::map<uint64_t, BoneDynamicsState> dynamics_now;

  bool is_group() const { return type == LayerType::Group || type == LayerType::Bone || type == LayerType::Switch; }
  bool is_switch() const { return type == LayerType::Switch; }
  /* Visible, and not a child a switch above it isn't showing now: what the viewport draws, picks and
   * edits (a hidden child being edited shows there). */
  bool shown() const { return visible && !edit_out; }
  /* As renders and exports show it: the switches' own picks. */
  bool shown_in_render() const { return visible && !switched_out; }
  bool is_bone_layer() const { return type == LayerType::Bone; }
  bool is_media() const { return type == LayerType::Media; }
  bool is_text() const { return type == LayerType::Text; }
  /* Field by field, children too (undo steps compare a layer with its last copy). */
  bool operator==(const Layer &) const = default;
  /* Has a drawing of its own to show and edit (vector layers and references of them). */
  bool has_drawing() const { return type == LayerType::Vector; }
  /* Has shapes to draw, pick and mask with: a drawing of its own, or a text's letters (which only
   * change with its text). */
  bool has_shapes() const { return type == LayerType::Vector || type == LayerType::Text; }
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
  /* Translate * Rotate * Shear * Scale: the X axis turned by its rotation plus shear.x, the Y axis by its
   * rotation plus shear.y, each then scaled. */
  Transform2D local_transform() const;
  /* Sets position, rotation, shear and scale to make `t` exactly (shear.x 0: the X axis by the rotation
   * alone), its scale's x negative when `negative_x` (a flip kept as one). */
  void set_local_transform(const Transform2D &t, bool negative_x = false);

  static const rna::StructRNA &rna();
};

const char *layer_type_name(LayerType type);
/* Start With from a card's first step: Add starts from nothing, Subtract from everything. */
void mask_card_start_from_steps(LayerMask &card);
/* Colour of a label in the Layers editor, 0 alpha for None. */
Color label_color_value(LabelColor label);

}  // namespace oanim
