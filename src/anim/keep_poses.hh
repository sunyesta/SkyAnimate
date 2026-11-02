/* Keep Poses: a move on the setup frame (frame 0) that leaves the other frames' keyed poses where
 * they are. Keys hold changes from the setup pose, so moving the setup moves every pose with it; this
 * takes the setup's change back off every key on another frame, in the mainline and every action
 * (an action bone action's keys, offsets added as its bone turns, stay as they are): points' positions,
 * handles' turn and length, bones' position, angle and length. */

#pragma once

#include <cstdint>
#include <map>
#include <string>
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

/* Keep Animation for a change on the setup frame that changes how the bones bend drawings (weights):
 * where each point of `layers` shows on every keyed frame, before the change. */
struct DrawingsShown {
  struct Layer {
    uint64_t id = 0;
    std::vector<Transform2D> skins;
    std::vector<float2> points;
    /* Each curve point's handles as shown, curve by curve. */
    std::vector<std::pair<float2, float2>> handles;
  };
  std::vector<int> frames;
  /* Per frame, per layer. */
  std::vector<std::vector<Layer>> shown;
  bool empty() const { return frames.empty(); }
};
/* On the setup frame, before the change: the frames that matter (any keyed on the layers' points, their
 * bone layers' bones, or the transforms placing them) and where the points show on each. */
DrawingsShown drawings_shown_capture(Document &doc, const std::vector<uint64_t> &layers);
/* The same on frames picked by the caller, in the timeline being edited. */
DrawingsShown drawings_shown_capture_frames(Document &doc, const std::vector<uint64_t> &layers,
                                            const std::vector<int> &frames);
/* After it: on each of those frames, the points the change bends differently keyed back where they
 * showed (their handles too). Returns how many keys it set. */
int drawings_shown_keep(Document &doc, const DrawingsShown &before);

/* The same for every action the layers play in (the mainline and their bone layers' actions), from
 * whatever frame and action it is done on. */
struct DrawingsShownAll {
  struct Action {
    uint64_t owner = 0;
    std::string name;
    DrawingsShown shown;
  };
  std::vector<Action> actions;
  bool empty() const;
};
DrawingsShownAll drawings_shown_capture_all(Document &doc, const std::vector<uint64_t> &layers);

/* Keep Animation for a change in how bones hang from their parents (Relations: Inherit Rotation and
 * Scale): how each of `bones` shows (its angle, and its size along and across it) on every frame keyed
 * on it or a bone above it, in the mainline and every action of its bone layer. */
struct BonesShownAll {
  struct Shown {
    float angle = 0.0f;
    float2 size;
  };
  struct Frame {
    int frame = 0;
    std::vector<Shown> bones;
  };
  struct Action {
    std::string name;
    std::vector<Frame> frames;
  };
  uint64_t armature = 0;
  std::vector<uint64_t> bones;
  std::vector<Action> actions;
};
BonesShownAll bones_shown_capture_all(Document &doc, uint64_t armature, const std::vector<uint64_t> &bones);
/* After the change: on each of those frames each bone's angle and scale keyed back so it shows as it did
 * (parents first, actions before the mainline). Returns how many keys it set. */
int bones_shown_keep_all(Document &doc, const BonesShownAll &before);
/* The actions first, the mainline last: an action bone action's keys add to the mainline, so the mainline is put
 * right with them already in place (nothing is put right twice). Returns how many keys it set. */
int drawings_shown_keep_all(Document &doc, const DrawingsShownAll &before);

}  // namespace oanim
