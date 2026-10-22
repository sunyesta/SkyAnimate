/* Keep Poses: a move on the setup frame (frame 0) that leaves the other frames' keyed poses where
 * they are. Keys hold changes from the setup pose, so moving the setup moves every pose with it; this
 * takes the setup's change back off every key on another frame, in the mainline and every action
 * (smart ones too): points' positions, handles' turn and length, bones' position, angle and length. */

#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct Document;

/* The setup pose of some layers, as it was before a move. */
struct SetupSnapshot {
  struct BoneSetup {
    float2 position;
    float angle = 0.0f;
    float length = 0.0f;
  };
  struct LayerSetup {
    uint64_t layer = 0;
    /* Points by id; handles by curve and curve point (a move doesn't change them). */
    std::map<uint64_t, float2> points;
    std::vector<std::vector<std::pair<float2, float2>>> handles;
    std::map<uint64_t, BoneSetup> bones;
  };
  std::vector<LayerSetup> layers;
  bool empty() const { return layers.empty(); }
};

SetupSnapshot setup_snapshot(const Document &doc, const std::vector<uint64_t> &layers);

/* After the move is recorded (animation_record): every key on another frame of what moved, worked out
 * again against the new setup so it shows the same pose. Returns how many keys changed. */
int keep_poses(Document &doc, const SetupSnapshot &before);

}  // namespace oanim
