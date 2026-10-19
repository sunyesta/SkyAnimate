#pragma once

/* Skeletons: where each bone of a bone layer is, in the setup pose (frame 0) and now.
 *
 * A bone's frame is its parent's frame (a root's is the bone layer's space) moved by its
 * position, turned by its angle and scaled by its scale. What moves bound artwork is each
 * bone's change from setup to now: skin = pose * setup^-1. */

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/math.hh"

namespace sky {

struct Bone;
struct Document;
struct Layer;

struct BonePose {
  uint64_t id = 0;
  /* Bone frame -> bone layer space, in the setup pose and now. */
  Transform2D setup;
  Transform2D pose;
  /* Setup to now, in bone layer space. */
  Transform2D skin;
  /* Where the bone runs in the setup pose and now (bone layer space). */
  float2 setup_head, setup_tail;
  float2 head, tail;
};

/* Every bone of a bone layer, in the order of `layer.bones`. `setup_frame`: the document is on
 * frame 0, where moving a bone changes the setup pose itself. Elsewhere an edit not yet keyed
 * is a pose: the setup stays where the bone was before it. */
std::vector<BonePose> armature_pose(const Layer &armature, bool setup_frame = true);
const BonePose *bone_pose_find(const std::vector<BonePose> &poses, uint64_t id);
Bone *bone_find(Layer &armature, uint64_t id);
const Bone *bone_find(const Layer &armature, uint64_t id);
const Bone *bone_find_by_name(const Layer &armature, const std::string &name);
/* A bone and everything below it. */
bool bone_is_descendant(const Layer &armature, uint64_t id, uint64_t ancestor);

/* Selects or deselects a whole bone: its body and both ends. */
void bone_select(Bone &bone, bool select);
void armature_deselect_all(Layer &armature);
/* Whether an end is selected, counting the shared joint of connected bones. */
bool bone_head_selected(const Layer &armature, const Bone &bone);
bool bone_tail_selected(const Layer &armature, const Bone &bone);
/* After ends were picked one by one: a bone whose two ends are both selected is selected, as in
 * Blender, and one missing an end is not. */
void armature_flush_selection(Layer &armature);

/* G, R and S on bones, from the bones as they were (`original`) so a drag can be redone every
 * update. `delta` is in the bone layer's space. G moves a bone picked by its body or base, and
 * aims a bone picked by its tip alone (on the setup frame the tip also sets its length); R turns
 * picked bones about their bases; S scales them. Children always come along, as in Moho. */
bool armature_any_picked(const Layer &armature);
/* Alt G, Alt R and Alt S while posing, as Blender's Clear Location / Rotation / Scale: the picked
 * bones' position, angle or scale go back to the setup pose (frame 0). Returns false when no bone
 * is picked. */
bool armature_clear_pose(Layer &armature, bool position, bool angle, bool scale);
/* The bases of the picked bones (bone layer space), for the pivot shown while turning. */
std::vector<float2> armature_picked_heads(const Layer &armature, bool setup_frame);
void armature_grab_selected(Layer &armature, const std::vector<Bone> &original, float2 delta, bool setup_frame);
void armature_rotate_selected(Layer &armature, const std::vector<Bone> &original, float angle);
void armature_scale_selected(Layer &armature, const std::vector<Bone> &original, float factor);
/* X mirror editing: every bone that changed since `original` puts its other side (by _L / _R name)
 * in the mirror image across the bone layer's vertical axis, on any frame. */
void armature_mirror_edits(Layer &armature, const std::vector<Bone> &original, bool setup_frame);

/* The Transform Bones gizmo (Moho's sz_transform_bones_gizmo). The bounds of the picked bones'
 * ends (bone layer space), at least 0.01 across; false when nothing is picked. */
bool armature_picked_bounds(const Layer &armature, bool setup_frame, float2 &lo, float2 &hi);
/* Sends each picked bone's base and tip through `map` (bone layer space), from `original`. Unpicked
 * children of picked bones stay where they are. The setup pose takes new lengths; a pose takes
 * scales instead. */
void armature_map_selected(Layer &armature, const std::vector<Bone> &original, const Transform2D &map,
                           bool setup_frame);
/* Box selection. `inside` tests a point in bone layer space. Joints: the ends inside are picked, as
 * Blender's box select does (a bone with both ends inside is selected). Bones: a bone any part of
 * which is inside is picked whole, as Moho's gizmo does. `extend` keeps the selection; `deselect`
 * removes instead. */
void armature_select_joints_in(Layer &armature, bool setup_frame, const std::function<bool(float2)> &inside,
                               bool extend, bool deselect);
void armature_select_bones_in(Layer &armature, bool setup_frame, const std::function<bool(float2)> &inside,
                              bool extend, bool deselect);
/* Puts bones' ends (setup pose, bone layer space) where `targets` says, by bone id, keeping every
 * other bone where it is (connected heads follow their parent's tail). */
void armature_set_joints(Layer &armature, const std::map<uint64_t, std::pair<float2, float2>> &targets);

/* Blender's symmetry names: "arm_L" <-> "arm_R", "arm.L" <-> "arm.R", "Left arm" <-> "Right arm".
 * Names without a side come back unchanged. */
std::string bone_mirror_name(const std::string &name);
/* The id of the bone on the other side (by name), or `id` itself when there is none. */
uint64_t bone_mirror_id(const Layer &armature, uint64_t id);

/* The nearest bone layer the layer sits in, or null. */
const Layer *layer_armature(const Document &doc, uint64_t layer_id);
/* Gives every vector layer inside a bone layer its Armature modifier, and takes it from those
 * that left one. Returns true when anything changed. */
bool document_sync_armature_modifiers(Document &doc);

}  // namespace sky
