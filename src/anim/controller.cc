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

float controller_side_reach(const Bone &knob, int side, float frame)
{
  const std::vector<float2> &timing = knob.controller_timing[side];
  if (timing.empty() || frame <= 0.0f) {
    return std::max(frame, 0.0f);
  }
  float2 prev(0.0f, 0.0f);
  for (const float2 &k : timing) {
    if (frame <= k.x) {
      const float span = k.x - prev.x;
      return span > 1e-6f ? prev.y + (k.y - prev.y) * (frame - prev.x) / span : k.y;
    }
    prev = k;
  }
  return prev.y + (frame - prev.x);
}

float controller_side_frame(const Bone &knob, int side, float along)
{
  if (along <= 0.0f) {
    return 0.0f;
  }
  float t = along;
  const std::vector<float2> &timing = knob.controller_timing[side];
  if (!timing.empty()) {
    float2 prev(0.0f, 0.0f);
    bool found = false;
    for (const float2 &k : timing) {
      if (along <= k.y) {
        const float rise = k.y - prev.y;
        t = rise > 1e-6f ? prev.x + (k.x - prev.x) * (along - prev.y) / rise : prev.x;
        found = true;
        break;
      }
      prev = k;
    }
    if (!found) {
      t = prev.x + (along - prev.y);
    }
  }
  return std::min(t, controller_side_frames(knob, side));
}

float2 controller_frames_place(const Bone &knob, float2 frames)
{
  const auto one = [&](float v, int plus, int minus) {
    return v >= 0.0f ? controller_side_reach(knob, plus, v) : -controller_side_reach(knob, minus, -v);
  };
  return {one(frames.x, CONTROLLER_RIGHT, CONTROLLER_LEFT), one(frames.y, CONTROLLER_UP, CONTROLLER_DOWN)};
}

float2 controller_side_only(int side, float2 at)
{
  const float along = std::max((controller_side_axis(side) == 0 ? at.x : at.y) * controller_side_sign(side), 0.0f);
  return controller_side_axis(side) == 0 ? float2(along * controller_side_sign(side), 0.0f) :
                                           float2(0.0f, along * controller_side_sign(side));
}

void controller_box(const Bone &knob, float2 &lo, float2 &hi)
{
  /* Each side reaches as far as the knob plays its action's end. */
  const auto reach = [&](int side) { return controller_side_reach(knob, side, controller_side_frames(knob, side)); };
  lo = {-reach(CONTROLLER_LEFT), -reach(CONTROLLER_DOWN)};
  hi = {reach(CONTROLLER_RIGHT), reach(CONTROLLER_UP)};
}

float2 controller_knob(const Bone &knob)
{
  return bone_transform_from_position(knob, knob.position);
}

float2 controller_knob_clamped(const Bone &knob)
{
  float2 lo, hi;
  controller_box(knob, lo, hi);
  const float2 at = controller_knob(knob);
  return {std::clamp(at.x, lo.x, hi.x), std::clamp(at.y, lo.y, hi.y)};
}

float2 controller_knob_place(const Bone &knob, const BonePose &pose)
{
  /* Where its constraints move it on top. */
  if (!knob.controller_knob_independent) {
    return controller_knob(knob) + pose.knob_pull;
  }
  const float size = std::max(knob.controller_frame_size, 1e-6f);
  const float2 place = pose.knob_parent_skin.apply(pose.setup_head +
                                                   rotate(controller_knob(knob) * size, knob.controller_box_angle));
  return rotate(pose.box_skin.inverted().apply_vector(place - pose.head), -knob.controller_box_angle) / size +
         pose.knob_pull;
}

float2 controller_knob_held(const Bone &knob, const BonePose &pose)
{
  float2 lo, hi;
  controller_box(knob, lo, hi);
  const float2 at = controller_knob_place(knob, pose);
  return {std::clamp(at.x, lo.x, hi.x), std::clamp(at.y, lo.y, hi.y)};
}

float2 controller_knob_played(const Layer &owner, const Bone &knob)
{
  if (!knob.controller_knob_independent && !controller_constrained(knob)) {
    /* Past the box (Contain Knob in Box off), its actions play the nearest point inside. */
    return controller_knob_clamped(knob);
  }
  const std::vector<BonePose> poses = armature_pose(owner, false);
  const BonePose *pose = bone_pose_find(poses, knob.id);
  return pose ? controller_knob_held(knob, *pose) : controller_knob(knob);
}

void controller_knob_set(Bone &knob, float2 frames)
{
  if (knob.controller_knob_edit_side >= 0) {
    /* In a side's own action: along that side, keying how far it goes on this frame. */
    knob.position = bone_position_from_transform(knob, controller_side_only(knob.controller_knob_edit_side, frames));
    return;
  }
  if (knob.controller_knob_independent) {
    /* A place in the picture: the box may have moved from under it, so it is kept in the box as it plays. */
    knob.position = bone_position_from_transform(knob, frames);
    return;
  }
  if (!knob.controller_contain_knob) {
    knob.position = bone_position_from_transform(knob, frames);
    return;
  }
  float2 lo, hi;
  controller_box(knob, lo, hi);
  knob.position = bone_position_from_transform(knob, {std::clamp(frames.x, lo.x, hi.x), std::clamp(frames.y, lo.y, hi.y)});
}

float2 controller_box_point(const Bone &knob, const BonePose &pose, float2 frames)
{
  return pose.head + pose.box_skin.apply_vector(rotate(frames * knob.controller_frame_size, knob.controller_box_angle));
}

void controller_knob_move(Bone &knob, const BonePose &pose, float2 delta)
{
  const float size = std::max(knob.controller_frame_size, 1e-6f);
  const float2 local = rotate(pose.box_skin.inverted().apply_vector(delta), -knob.controller_box_angle) / size;
  if (knob.controller_knob_edit_side >= 0) {
    /* In a side's own action: from where it is shown, along that side only. */
    controller_knob_set(knob, knob.controller_knob_edit_at + local);
    return;
  }
  if (!knob.controller_knob_independent) {
    controller_knob_set(knob, controller_knob(knob) + local);
    return;
  }
  /* From where it is seen (held at the edge, or past it), the way the mouse goes; contained, it stops at the edge, so
   * dragging back moves it at once. Its keys are a place against its Knob Parent as that rests. */
  float2 to = (knob.controller_contain_knob ? controller_knob_held(knob, pose) : controller_knob_place(knob, pose)) + local;
  if (knob.controller_contain_knob) {
    float2 lo, hi;
    controller_box(knob, lo, hi);
    to = {std::clamp(to.x, lo.x, hi.x), std::clamp(to.y, lo.y, hi.y)};
  }
  /* Its keys without what its constraints add. */
  const float2 place = controller_box_point(knob, pose, to - pose.knob_pull);
  const float2 keyed = rotate(pose.knob_parent_skin.inverted().apply(place) - pose.setup_head, -knob.controller_box_angle);
  knob.position = bone_position_from_transform(knob, keyed / size);
}

void controller_knob_snap(Bone &knob, const std::vector<float2> &also)
{
  const auto nearest = [&](float at, int plus, int minus) {
    float best = 0.0f;
    for (const int side : {plus, minus}) {
      const float sign = controller_side_sign(side);
      for (const int frame : knob.controller_keys[side]) {
        const float there = controller_side_reach(knob, side, float(frame)) * sign;
        if (std::fabs(there - at) < std::fabs(best - at)) {
          best = there;
        }
      }
      /* A side with no keys yet still has its end. */
      if (const float end = controller_side_reach(knob, side, controller_side_frames(knob, side)) * sign;
          std::fabs(end - at) < std::fabs(best - at))
      {
        best = end;
      }
    }
    return best;
  };
  const float2 at = controller_knob(knob);
  /* The nearest crossing of keys is the nearest along each axis; a spot or fix pose nearer than that wins. */
  float2 best(nearest(at.x, CONTROLLER_RIGHT, CONTROLLER_LEFT), nearest(at.y, CONTROLLER_UP, CONTROLLER_DOWN));
  const auto dist = [&](float2 p) { return std::hypot(p.x - at.x, p.y - at.y); };
  for (const ControllerSpot &spot : knob.controller_spots) {
    best = dist(spot.at) < dist(best) ? spot.at : best;
  }
  for (const float2 &p : also) {
    best = dist(p) < dist(best) ? p : best;
  }
  controller_knob_set(knob, best);
}

ControllerSpot *controller_spot_find(Bone &knob, const std::string &name)
{
  for (ControllerSpot &spot : knob.controller_spots) {
    if (spot.name == name) {
      return &spot;
    }
  }
  return nullptr;
}

const ControllerSpot *controller_spot_find(const Bone &knob, const std::string &name)
{
  return controller_spot_find(const_cast<Bone &>(knob), name);
}

std::string controller_spot_unique_name(const Bone &knob, const std::string &name)
{
  std::string unique = name;
  for (int n = 2; controller_spot_find(knob, unique); n++) {
    unique = name + " " + std::to_string(n);
  }
  return unique;
}

void controller_fit_size(Bone &knob)
{
  float longest = float(CONTROLLER_EMPTY_FRAMES);
  for (int side = 0; side < CONTROLLER_SIDES; side++) {
    /* By the actions' lengths, not the timing: keying the knob never rescales the box under it. */
    longest = std::max(longest, controller_side_frames(knob, side));
  }
  knob.controller_frame_size = 0.5f / longest;
}

}  // namespace oanim
