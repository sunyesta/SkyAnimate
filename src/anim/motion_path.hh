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
/* Smooth Arc: each of the target's keys strictly between `first` and `last` moves onto a smooth curve through the
 * keys either side of it (up to two each side). Returns how many keys moved. */
int motion_path_smooth_arc(Document &doc, const MotionPathTarget &target, int first, int last);

}  // namespace oanim
