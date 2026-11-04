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

}  // namespace oanim
