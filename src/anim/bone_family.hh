#pragma once

/* Sub bone groups and bone swaps, for switches of drawings that each have their own bones (a switch of
 * legs: straight, bent, kicking).
 *
 * A sub bone group is a bone layer inside another (Layer::sub_bone_group, on by default), hung from one
 * of the outer bones with Parent Bone. Its bones show, and a click on one picks it, while the bone layer
 * around it is worked in, and the other way round: the bone layers worked in together are a family. Worked
 * in from the outside, those inside draw as its own bones do, a click on one of their bones leaves it active, A,
 * Alt A, Ctrl I and a box take in their bones as well, and G, R and S move them. The Transform Bones tool's box
 * still moves the active bone layer's alone. A
 * sub bone group inside a switch belongs only while the switch shows it, so its bones come and go with
 * its drawing (as Spine's skin bones). Drawings still follow only their nearest bone layer (Moho's
 * rule), so a drawing can never be bound to another child's bones.
 *
 * A bone swap says one bone stands in for another across a switch's children (Bone::swap_layer,
 * swap_bone): the bent leg's shin for the straight leg's. Swaps don't share poses (a front view's head
 * tilt can be an action bone while the side view's is the head bone's angle); they say which bone is
 * which, for carrying the selection across a pick and for Match Pose from Previous Drawing. Bones that
 * stand in for the same bone, directly or along a chain of swaps, are counterparts. */

#include <cstdint>
#include <utility>
#include <vector>

namespace oanim {

struct Document;
struct Layer;

/* The bone layers worked in together with `armature`: out through sub bone groups to the outermost,
 * then it and every sub bone group inside it the viewport shows; `armature` itself always. */
std::vector<Layer *> armature_family(Document &doc, uint64_t armature);
/* `armature`, then the bone layers of its family inside it: what picking all or a box takes in from it, since a character's
 * bones are worked from the bone layer around them. */
std::vector<Layer *> armature_and_inside(Document &doc, uint64_t armature);
/* The bone layer holding the bones being worked on from `armature`: itself while it has bones picked, else the first
 * inside it that has (a bone clicked there leaves `armature` active), else itself. What the Bone tabs show and the
 * commands on the active bone act on. */
Layer *armature_worked_on(Document &doc, uint64_t armature);

/* What a bone's swaps lead to: the (layer, bone) at the end of its chain of swaps, itself for none. */
std::pair<uint64_t, uint64_t> bone_swap_root(const Document &doc, uint64_t layer, uint64_t bone);
/* The bone of `to` that is the counterpart of bone `bone` of bone layer `layer`, or 0. */
uint64_t bone_counterpart(const Document &doc, uint64_t layer, uint64_t bone, const Layer &to);

/* The bone layer a switch's child works with: the child when it is one, else the first inside it;
 * null for none. */
Layer *switch_child_armature(Layer &child);
/* Auto-link by Name: in every child of switch `sw` but the first one with bones, each bone without a
 * swap stands in for the first one's bone of the same name. How many swaps it made. */
int switch_swaps_auto_link(Document &doc, Layer &sw);

/* Match Pose: turns (and, for roots, moves) the bones of `to` that have counterparts in `from` so
 * they lie as those do now, in the picture. The values change; recording keys them. How many. */
int armature_match_pose(Document &doc, Layer &to, const Layer &from);

}  // namespace oanim
