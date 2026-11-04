#pragma once

/* Motion paths (Document::motion_paths): where things go over the frames around the shown one, a dot per frame,
 * as Blender's motion paths and Maya's motion trails. A bone traces its tip (or its head), so turning draws an arc
 * and moving a line; a point traces itself, through its bones; a layer with nothing picked in it traces its
 * origin. The range runs from `path_before` keys back to `path_after` keys ahead of the shown frame, by the keys
 * of what is traced and of everything moving it. Frames are worked out on a copy of the document from their keys
 * alone, as onion skins are; deforms are left out (bones and transforms only), so paths stay quick. */

#include <cstdint>
#include <string>
#include <vector>

#include "anim/animation.hh"
#include "anim/path_brush.hh"
#include "core/math.hh"
#include "data/document.hh"

namespace oanim {

struct Layer;

/* What the paths trace: for each of `editing` (the layers being edited), its selected bones or points, or with
 * none of those its origin; and every pinned target. */
std::vector<MotionPathTarget> motion_path_targets(const Document &doc, const std::vector<Layer *> &editing);
/* The kept style of `target` (pin, colour, end), or null. */
const MotionPathTarget *motion_path_style(const Document &doc, const MotionPathTarget &target);
MotionPathTarget &motion_path_style_ensure(Document &doc, const MotionPathTarget &target);
/* The frames the paths of `targets` cover. False with nothing to trace. */
bool motion_path_range(const Document &doc, const std::vector<MotionPathTarget> &targets, int &r_first,
                       int &r_last);
/* The keys of what moves `target` (its own and those of the bones and layers above it), in the animation. */
std::vector<int> motion_path_keys(const Document &doc, const MotionPathTarget &target);

struct MotionPath {
  MotionPathTarget target;
  Color color;
  int first = 0;
  /* Where it is on each frame from `first` on, in document space (drawn relative to Document::path_relative). */
  std::vector<float2> points;
  /* How fast it moves into each frame, 0 (still) to 1 (its fastest). */
  std::vector<float> speed;
  std::vector<int> keys;
};
/* The paths of `targets` over frames first..last. */
std::vector<MotionPath> motion_paths_make(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                          int first, int last);
/* A path's colour: its own, or one of a few told apart by the order it comes in. */
Color motion_path_color(const Document &doc, const MotionPathTarget &target, size_t index);
/* The drawings to show as a ghost from a path's dot: the target's layer, or the drawings a bone layer or group
 * carries. */
std::vector<uint64_t> motion_path_ghost_layers(const Document &doc, const std::vector<MotionPathTarget> &targets);
/* A label for a target: "arm_L tip", "Face point 12", "Face". */
std::string motion_path_label(const Document &doc, const MotionPathTarget &target);
/* Red where fast, blue where still. */
Color motion_path_speed_color(float speed);

/* --- Editing, the Motion Path tool's: every edit lands on keys (motion_path_record keys it whatever auto-key says). */

/* Where `target` shows now, and on `frame` (from its keys alone), in document space. */
bool motion_path_position(const Document &doc, const MotionPathTarget &target, float2 &r_at);
bool motion_path_position_at(const Document &doc, const MotionPathTarget &target, int frame, float2 &r_at);
/* Moves `target` so it shows at `at` (document space) on the frame shown, by its own values: a bone's tip by turning
 * the bone, its head by moving it, a point or a layer's origin by moving that. False where it can't (a connected
 * bone's head). */
bool motion_path_place(Document &doc, const MotionPathTarget &target, float2 at);
/* The keys of what moves `target` either side of `frame` (0 where there's none). */
void motion_path_neighbour_keys(const Document &doc, const MotionPathTarget &target, int frame, int &r_before,
                                int &r_after);
/* Ctrl while moving a dot: `at` onto the straight line between where the target is on the keys either side of the
 * frame shown (the nearest place on it). False without keys both sides. */
bool motion_path_straighten(const Document &doc, const MotionPathTarget &target, float2 &at);
/* A breakdown, as tweenMachine's: the target's own values on the frame shown become `factor` (0-1) of the way from
 * those on the key before to those on the key after, so a turning bone stays on its arc. */
bool motion_path_breakdown(Document &doc, const MotionPathTarget &target, float factor);
/* The channels of `target` itself, for picking its keys. */
KeyFilter motion_path_key_filter(const Document &doc, const MotionPathTarget &target);
/* Keys the edits on the frame shown, with auto-key on or off. */
void motion_path_record(Document &doc);
/* Editing a dot on another frame than the one shown (clicking a dot doesn't go to its frame): while it lives the
 * document is on `frame`, then back on the frame it showed, evaluated there. */
struct MotionPathOnFrame {
  Document &doc;
  int shown;
  MotionPathOnFrame(Document &doc, int frame);
  ~MotionPathOnFrame();
  MotionPathOnFrame(const MotionPathOnFrame &) = delete;
  MotionPathOnFrame &operator=(const MotionPathOnFrame &) = delete;
};
/* Smooth Arc: each of the target's keys strictly between `first` and `last` moves onto a smooth curve through the
 * keys either side of it (up to two each side). Returns how many keys moved. */
int motion_path_smooth_arc(Document &doc, const MotionPathTarget &target, int first, int last);

/* --- Path handles on position keys: a layer's origin, a bone's head (not a connected one), a point. A bone's tip goes
 * where its angle turns it, so its path has none. */

/* Whether `target` has position keys with path handles. */
bool motion_path_has_handles(const Document &doc, const MotionPathTarget &target);
/* Where `target`'s position key on `frame` is and where its handles end, in document space on that frame (a handle
 * shown a third of the way along its stretch, as a Bezier's). False with no position key there. */
bool motion_path_handles(const Document &doc, const MotionPathTarget &target, int frame, float2 &r_key, float2 &r_in,
                         float2 &r_out, PathHandles *r_mode = nullptr);
/* Which of the handles of `p`'s key on `frame` the tool shows: each shapes the stretch on its side, so it shows only
 * where some of that stretch is drawn (none past the path's ends, as when it ends on the frame shown). */
void motion_path_handle_sides(const MotionPath &p, int frame, bool &r_in, bool &r_out);
/* Moves the handle (`out`, else in) of `target`'s position key on `frame` so it ends at `at` (document space on that
 * frame); `broken` (Alt) moves it alone and breaks the pair. Only the keys change: call animation_evaluate after. */
bool motion_path_handle_set(Document &doc, const MotionPathTarget &target, int frame, bool out, float2 at,
                            bool broken);
/* Puts `target`'s selected position keys back to Auto handles. Returns how many changed. */
int motion_path_handles_auto(Document &doc, const MotionPathTarget &target);

/* --- Path brushes (anim/path_brush.hh), on the paths of `targets` over frames first..last, in document units. Every
 * brush keys what it changes whatever auto-key says. */

struct PathDab {
  float2 center;
  float radius = 0.1f;
  float strength = 0.5f;
  /* Paint Speed's speed (1: the stretch's even speed). */
  float speed = 2.0f;
  /* Ctrl: the opposite (Paint Speed 1 / speed, Faster as Slower and Slower as Faster, Blend Speed sharpening). */
  bool invert = false;
};
/* 1 at the centre, easing to 0 at `radius`. */
float path_brush_falloff(float distance, float radius);

/* Grab: the targets' own keys whose dots are inside the brush when it is pressed, where they were, and how much each
 * follows the mouse. */
struct PathGrabKey {
  MotionPathTarget target;
  int frame = 0;
  float2 start;
  float weight = 0.0f;
};
std::vector<PathGrabKey> motion_path_grab_keys(const Document &doc, const std::vector<MotionPathTarget> &targets,
                                               int first, int last, float2 center, float radius);
/* Moves each grabbed key from where it was by `offset` times its weight and `strength`. Returns how many moved. */
int motion_path_grab(Document &doc, const std::vector<PathGrabKey> &keys, float2 offset, float strength);
/* Smooth: each of the targets' own keys under the dab toward the curve through the keys either side (Smooth Arc's),
 * and its dragged handles toward Auto. Returns how many keys changed. */
int motion_path_smooth_dab(Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                           const PathDab &dab);
/* Paint Speed, Faster, Slower or Blend Speed: the speeds of the frames under the dab, on the channel that moves each
 * target's dot, its stretches becoming Custom (each still arriving on its key's frame). Returns how many stretches
 * changed. */
int motion_path_speed_dab(Document &doc, const std::vector<MotionPathTarget> &targets, int first, int last,
                          PathBrush brush, const PathDab &dab);

}  // namespace oanim
