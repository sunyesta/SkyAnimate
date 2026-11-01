#pragma once

/* Follow Point (BoneConstraintType::FollowPoint), Harmony's Point Kinematic Output: a bone stays on a bending shape
 * without bending, by a point of its own that a Curve, Cage or Mesh Deform carries. The point rests in the space of the
 * layer or group the deform is on, and only its change from there reaches the bone, on top of what the bone's parent
 * gives it, so nothing jumps and keys on the bone still work. Layers follow by Parent to Bone.
 *
 * A pose can't read another layer, so every frame this works out each constraint's move and stores it, as
 * shape_limit.hh does. A bone that moves the shape it follows can't work: the constraint is refused and says so. Bones
 * that only sit in the same rig as the shape can: a face feature on a head its own bones move. When a bone above it moves
 * the shape too, the point goes by the shape's change of shape alone, as those bones move it already. */

#include <cstdint>

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
/* Aims `c` at `m`, a deform on `owner` (null for neither), its point starting at the bone's head. */
void follow_point_aim(Document &doc, Layer &rig, const Bone &bone, BoneConstraint &c, Layer *owner, Modifier *m);
/* Puts `c`'s point at `point`, in `owner`'s space. */
void follow_point_place(const Document &doc, const Layer &rig, BoneConstraint &c, const Layer &owner, float2 point);

}  // namespace oanim
