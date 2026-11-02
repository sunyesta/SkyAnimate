#include "anim/controller.hh"

#include <algorithm>
#include <cmath>

#include "data/bones.hh"

namespace oanim {

const char *controller_side_name(int side)
{
  static const char *const names[] = {"Right", "Left", "Up", "Down"};
  return names[std::clamp(side, 0, CONTROLLER_SIDES - 1)];
}

const char *controller_side_id(int side)
{
  static const char *const ids[] = {"RIGHT", "LEFT", "UP", "DOWN"};
  return ids[std::clamp(side, 0, CONTROLLER_SIDES - 1)];
}

int controller_side_from_id(const std::string &id)
{
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    if (id == controller_side_id(side)) {
      return side;
    }
  }
  return -1;
}

const Bone *controller_of_action(const Layer &owner, const std::string &action, int *r_side)
{
  if (action.empty()) {
    return nullptr;
  }
  for (const Bone &bone : owner.bones) {
    if (!bone_is_controller(bone)) {
      continue;
    }
    for (int side = 0; side < CONTROLLER_SIDES; side++) {
      if (bone.controller_actions[side] == action) {
        if (r_side) {
          *r_side = side;
        }
        return &bone;
      }
    }
  }
  return nullptr;
}

const Bone *controller_dial_of_action(const Layer &owner, const std::string &action)
{
  const Bone *bone = bone_find_by_name(owner, action);
  if (!bone && action.size() > 2 && action.compare(action.size() - 2, 2, " 2") == 0) {
    bone = bone_find_by_name(owner, action.substr(0, action.size() - 2));
  }
  return bone && !bone_is_controller(*bone) ? bone : nullptr;
}

Bone *controller_dial_of_action(Layer &owner, const std::string &action)
{
  return const_cast<Bone *>(controller_dial_of_action(const_cast<const Layer &>(owner), action));
}

const Bone *controller_side_dial(const Layer &owner, const Bone &knob, int side)
{
  const std::string &action = knob.controller_actions[side];
  const Bone *dial = action.empty() ? nullptr : controller_dial_of_action(owner, action);
  if (!dial) {
    return nullptr;
  }
  const auto keys = dial->angle_keys.actions.find(action);
  return keys != dial->angle_keys.actions.end() && !keys->second.empty() ? dial : nullptr;
}

Bone *controller_side_dial(Layer &owner, const Bone &knob, int side)
{
  return const_cast<Bone *>(controller_side_dial(const_cast<const Layer &>(owner), knob, side));
}

const Bone *controller_of_dial(const Layer &owner, const Bone &dial, int *r_axis)
{
  for (const Bone &knob : owner.bones) {
    if (!bone_is_controller(knob)) {
      continue;
    }
    for (int side = 0; side < CONTROLLER_SIDES; side++) {
      if (controller_side_dial(owner, knob, side) == &dial) {
        if (r_axis) {
          *r_axis = controller_side_axis(side);
        }
        return &knob;
      }
    }
  }
  return nullptr;
}

float controller_side_frames(const Bone &knob, int side)
{
  if (knob.controller_actions[side].empty()) {
    return 0.0f;
  }
  const std::vector<int> &keys = knob.controller_keys[side];
  return float(keys.empty() ? CONTROLLER_EMPTY_FRAMES : keys.back());
}

void controller_box(const Bone &knob, float2 &lo, float2 &hi)
{
  lo = {-controller_side_frames(knob, CONTROLLER_LEFT), -controller_side_frames(knob, CONTROLLER_DOWN)};
  hi = {controller_side_frames(knob, CONTROLLER_RIGHT), controller_side_frames(knob, CONTROLLER_UP)};
}

float2 controller_knob(const Bone &knob)
{
  return bone_transform_from_position(knob, knob.position);
}

void controller_knob_set(Bone &knob, float2 frames)
{
  float2 lo, hi;
  controller_box(knob, lo, hi);
  knob.position = bone_position_from_transform(knob, {std::clamp(frames.x, lo.x, hi.x), std::clamp(frames.y, lo.y, hi.y)});
}

float2 controller_box_point(const Bone &knob, const BonePose &pose, float2 frames)
{
  const float2 size = knob.controller_frame_size;
  return pose.head + pose.skin.apply_vector({frames.x * size.x, frames.y * size.y});
}

void controller_knob_move(Bone &knob, const BonePose &pose, float2 delta)
{
  const float2 local = pose.skin.inverted().apply_vector(delta);
  const float2 size = knob.controller_frame_size;
  controller_knob_set(knob, controller_knob(knob) + float2(local.x / std::max(size.x, 1e-6f), local.y / std::max(size.y, 1e-6f)));
}

void controller_knob_snap(Bone &knob)
{
  const auto nearest = [&](float at, int plus, int minus) {
    float best = 0.0f;
    for (const int side : {plus, minus}) {
      const float sign = controller_side_sign(side);
      for (const int frame : knob.controller_keys[side]) {
        if (std::fabs(float(frame) * sign - at) < std::fabs(best - at)) {
          best = float(frame) * sign;
        }
      }
      /* A side with no keys yet still has its end. */
      if (const float end = controller_side_frames(knob, side) * sign; std::fabs(end - at) < std::fabs(best - at)) {
        best = end;
      }
    }
    return best;
  };
  const float2 at = controller_knob(knob);
  controller_knob_set(knob, {nearest(at.x, CONTROLLER_RIGHT, CONTROLLER_LEFT), nearest(at.y, CONTROLLER_UP, CONTROLLER_DOWN)});
}

void controller_fit_size(Bone &knob)
{
  float longest = float(CONTROLLER_EMPTY_FRAMES);
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    longest = std::max(longest, controller_side_frames(knob, side));
  }
  const float size = 0.5f / longest;
  knob.controller_frame_size = {size, size};
}

}  // namespace oanim
