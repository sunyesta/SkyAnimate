#pragma once

/* 2 Axis Controllers (Toon Boom's Master Controller, on Moho's action bones): a bone drawn as a box with a knob, its
 * four sides each playing an action. The knob's position is the action frame: its position keys hold frames, right
 * and up positive, so an action's length is how far that side reaches and a key stays on its pose whatever happens to
 * the box's size (Bone::controller_frame_size) or to the action's end. Past the end it shows the last frame. A side
 * with no action isn't there, so a controller with only Left and Right is a slider.
 *
 * The knob never bends a drawing: posed, the bone stays where it rests, riding on its parent, and its children and
 * points with it. Only the knob moves, inside the box.
 *
 * An ordinary action in a side is played by the controller. An action bone action stays its bone's (a dial), which
 * still plays it, so what else turns that bone goes on working: the knob and the dial are linked instead, one set of
 * keys, the knob's, the dial's row in the timeline showing them. Moving either moves the other. The knob has one value
 * per axis, so while it is on one side the other side's dial is held at rest. The link is the mainline's: inside an
 * action the dial is keyed as ever. */

#include <string>
#include <vector>

#include "data/layer.hh"

namespace oanim {

struct BonePose;
struct Document;

/* How long a side's action counts as while it has no keys, so there is somewhere to pull the knob. */
inline constexpr int CONTROLLER_EMPTY_FRAMES = 24;

inline bool bone_is_controller(const Bone &bone)
{
  return bone.type == BoneType::Controller2D;
}
/* 0 X, 1 Y. */
inline int controller_side_axis(int side)
{
  return side / 2;
}
/* +1 Right and Up, -1 Left and Down. */
inline float controller_side_sign(int side)
{
  return side % 2 == 0 ? 1.0f : -1.0f;
}
inline int controller_side_opposite(int side)
{
  return side ^ 1;
}
/* "Right", ... */
const char *controller_side_name(int side);
/* "RIGHT", ... as files and Python name them; -1 for none. */
const char *controller_side_id(int side);
int controller_side_from_id(const std::string &id);

/* The controller with `action` in a side (and which side), or null. */
const Bone *controller_of_action(const Layer &owner, const std::string &action, int *r_side = nullptr);
/* The bone an action bone action belongs to by its name, a controller never: the dial a side would be linked to. */
const Bone *controller_dial_of_action(const Layer &owner, const std::string &action);
Bone *controller_dial_of_action(Layer &owner, const std::string &action);
/* The controller a dial is linked to (one of its actions is in a side of it, on `r_axis`), or null. */
const Bone *controller_of_dial(const Layer &owner, const Bone &dial, int *r_axis = nullptr);
/* The dial linked to on `side`, if it has anything to link by: its angle keyed in that action. */
Bone *controller_side_dial(Layer &owner, const Bone &knob, int side);
const Bone *controller_side_dial(const Layer &owner, const Bone &knob, int side);

/* A side's action's length in frames: 0 for a side with no action. */
float controller_side_frames(const Bone &knob, int side);
/* The box in frames from the rest point: `lo` left and down (0 or less), `hi` right and up. */
void controller_box(const Bone &knob, float2 &lo, float2 &hi);
/* Where the knob is, in frames. */
float2 controller_knob(const Bone &knob);
/* Puts the knob there, in the box. */
void controller_knob_set(Bone &knob, float2 frames);
/* Frames from the rest point to the bone layer's space, the box riding on the bone's parent. */
float2 controller_box_point(const Bone &knob, const BonePose &pose, float2 frames);
/* A move of the knob by `delta` (bone layer space) from where it is. */
void controller_knob_move(Bone &knob, const BonePose &pose, float2 delta);
/* The knob onto the nearest key of its sides' actions (Ctrl), or the rest point. */
void controller_knob_snap(Bone &knob);
/* The frame size that gives the box a sensible size for its actions (the longest side half a unit). */
void controller_fit_size(Bone &knob);

}  // namespace oanim
