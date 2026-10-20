#pragma once

/* Bone dynamics: bones with a Dynamics constraint hang like weighted strands. When what they hang
 * from moves or turns, a strand's tip stays behind (Angle), a head lags behind (Position) and a
 * bone stretches or squashes along its length (Squash & Stretch; Maintain Volume thins it);
 * springs (Settle Speed, Bounce) bring them back to where their keys, smart actions and constraints put them, and the document's gravity
 * and wind push them (gravity by the bone's Weight, the wind by its Wind Catch). Each bone aims from where the dynamic bones above it have swung
 * to, so a wave runs down a strand. Max Swing, Max Distance and Max Stretch are soft limits.
 *
 * Unlike the rest of the pose, a frame depends on the frames before it, so the motion is
 * simulated forward from Document::simulation_start and cached; going back reads the cache, and
 * any change to the bones (keys, values, constraints) or to gravity and wind starts it again. The
 * setup frame (0) never moves. */

namespace sky {

struct BoneConstraint;
struct Document;
struct Layer;

/* Tick or untick a Dynamics constraint's own gravity, or its own wind. The first time, they start
 * as the document's, so nothing jumps; after that they are kept as they were, unticked or not. */
void dynamics_use_own_gravity(const Document &doc, BoneConstraint &c, bool on);
void dynamics_use_own_wind(const Document &doc, BoneConstraint &c, bool on);

/* Puts where dynamics have every dynamic bone on the shown frame into its bone layer
 * (Layer::dynamics_now), simulating as far as needed. Runs as part of evaluating a frame. */
void dynamics_update(Document &doc);
/* Forgets every simulated frame (they are worked out again when shown). */
void dynamics_clear_cache();
/* Live dynamics, for the app's main loop each frame (after the frame is worked out): while the
 * bones' aim changes on a frame (a bone dragged or turned), the dynamic bones simulate in real
 * time, `seconds` since the last call, so they trail, swing and settle as you work, and stay as
 * the live simulation has them until the frame changes (scrubbing or playing shows the frames'
 * own results again). Only what is shown: keys and the cache stay as they are, so playback and
 * baking don't change. Off (`allowed` false) while playing. */
void dynamics_live_tick(Document &doc, double seconds, bool allowed);
/* Whether a live simulation is running (the window keeps drawing for it). */
bool dynamics_live_running();
/* Bakes the simulation of `armature` (or every bone layer, when null) into keys, from the start
 * frame to the end of the frame range, and turns its Dynamics constraints off. Returns how many
 * keys it set. */
int dynamics_bake(Document &doc, Layer *armature);

}  // namespace sky
