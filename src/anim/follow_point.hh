#pragma once

/* Follow Point (BoneConstraintType::FollowPoint), Harmony's Point Kinematic Output: a bone stays on a bending shape
 * without bending, by a point of its own that a Curve, Cage or Mesh Deform carries. The point rests in the space of the
 * layer or group the deform is on, and only its change from there reaches the bone, on top of what the bone's parent
 * gives it, so nothing jumps and keys on the bone still work. Layers follow by Parent to Bone.
 *
 * A pose can't read another layer, so every frame this works out each constraint's move and stores it, as
 * shape_limit.hh does. A bone that moves the shape it follows can't work: the constraint is refused and says so. Bones
 * that only sit in the same rig as the shape can: a face feature on a head its own bones move. When a bone above it moves
 * the shape too, the point goes by the shape's change of shape alone, as those bones move it already.
 *
 * Instead of a place of its own, a point can be one the shape already has: the bone takes that point's change. */

#include <cstdint>
#include <string>

#include "core/math.hh"

namespace oanim {

struct Bone;
struct BoneConstraint;
struct Document;
struct Layer;
struct Modifier;

void follow_points_update(Document &doc);

/* The deform `c` names, null when it's gone; `r_owner` the layer it's on. */
Modifier *follow_point_modifier(Document &doc, const BoneConstraint &c, Layer **r_owner);
/* The deform named `modifier` on the layer `layer` names, by its id or (a group and a drawing can share one) by a name:
 * the first layer of that name with such a deform. Null when there's none. */
Modifier *follow_point_find(Document &doc, const std::string &layer, const std::string &modifier, Layer **r_owner);
/* Aims `c` at `m`, a deform on `owner` (null for neither), its point starting at the bone's head. */
void follow_point_aim(Document &doc, Layer &rig, const Bone &bone, BoneConstraint &c, Layer *owner, Modifier *m);
/* Puts `c`'s point `which` (0 the first, 1 the second, 2 the third) at `point`, in `owner`'s space. */
void follow_point_place(const Document &doc, const Layer &rig, BoneConstraint &c, const Layer &owner, float2 point,
                        int which = 0);
/* What `c`'s point `which` has to be shifted by to show at `to` (document space) on this frame, through the bend. */
float2 follow_point_shift_to(Document &doc, Layer &rig, Bone &bone, BoneConstraint &c, int which, float2 to);
/* `c`'s point `which` goes as point `id` of the deform's shape does (0: its own place, where the shape's point
 * rested). */
void follow_point_pick(Document &doc, const Layer &rig, BoneConstraint &c, int which, uint64_t id);
/* `c` on a bone just symmetrized: its points across the bone layer's vertical axis, each its own place, and on the other
 * side's deform when the layer it's on has a partner (Brow_L, Brow_R) with a deform of that name. */
void follow_point_mirror(Document &doc, const Layer &rig, BoneConstraint &c);
/* How many points `c` goes by (1-3). A point used for the first time starts along the bone (the second) or square to it
 * (the third), far enough off for a steady angle. Those left out are kept. */
void follow_point_count(const Document &doc, const Layer &rig, const Bone &bone, BoneConstraint &c, int count);

}  // namespace oanim
