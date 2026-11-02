#pragma once

/* Mirror Smart Actions (bone Symmetrize's option): an action's mirror image, as a new action, the original left as it
 * is. _L and _R bones and layers take each other's keys, reflected, since a turn's near and far sides differ; _X bones and
 * layers, and those with no side at all, take them inside themselves, each point its partner's across the axis. Keys
 * hold the change from the setup pose, so a setup that isn't exact on one side doesn't matter.
 *
 * - Points pair by where they are in the setup pose (2% of the layer's size, as mirror editing), handles by what they
 *   reach for, so a point welded across several curves is right.
 * - A point with no partner keeps its place along its line: the line is mirrored without it, and it is keyed at the
 *   same place along that, so the mirrored line looks as if it were a single segment.
 * - A drawing in a switch that can't pair up (a nose drawn side on, facing one way) has no keys that could turn it
 *   round: it gets a flipped copy beside it ("Front_Nose_X Flipped"), which the new action's switch keys pick.
 * - Draw Order and switch keys name their layers' partners.
 * - Bone layers inside go the same way: a sub bone group with no side mirrors its own bones, and one with a partner
 *   (Arm_L, Arm_R) takes its partner's bones' keys, bone for bone.
 *
 * Masks aren't keys and stay as they are, nor are references' offsets mirrored. Runs on the mainline, on frame 0. */

#include <cstdint>
#include <string>
#include <vector>

#include "data/document.hh"

namespace oanim {

/* Makes `to` of bone layer `owner` from `from`, replacing it if there is one. Returns what couldn't be mirrored, as
 * sentences for the status bar. */
std::vector<std::string> document_action_mirror(Document &doc, uint64_t owner, const std::string &from,
                                                const std::string &to);

}  // namespace oanim
