#pragma once

/* Switch layers (Moho's switch layers, Spine's slots, Rive's Solos): a group that shows one of its
 * children, or none. Its Active channel (Layer::switch_keys) holds the child's layer id, as Draw
 * Order holds its target's, so renaming or reordering children never breaks a key; 0 is None. Keys
 * only hold. Frame 0 holds the default child.
 *
 * Choices never add (one child can't be added to another), so each frame has one winner, highest
 * first: a key made by hand after frame 0 (held until the next key; a Rig key hands the switch back
 * to what is below it, as Cartoon Animator's Release); a smart action whose bone has turned past
 * one of its switch keys (the one listed first among its bone layer's actions when several have a
 * say); the default. Moho lets the smart bone win over hand keys; here the hand wins. */

#include <cstdint>
#include <map>
#include <string>

namespace oanim {

struct Document;
struct Layer;

/* A key that hands the switch back to the rig (smart actions, then the default). */
constexpr float SWITCH_RIG = -1.0f;

/* The child of `sw` a value names, or null (None, Rig, or a child that is gone). */
const Layer *switch_child(const Layer &sw, float value);
/* The default: the value on frame 0. */
float switch_default(const Layer &sw);
/* "Mouth O", "None", "Rig", or the id with " (missing)" for a child that is gone. */
std::string switch_value_label(const Layer &sw, float value);
/* The value a name gives: a child's id, 0 for "" (None), SWITCH_RIG for "RIG"; false when no child
 * has that name. */
bool switch_value_from_name(const Layer &sw, const std::string &name, float *r_value);

/* Who wins on every switch at `frame` (smart actions have put their picks in switch_rig), which
 * child each shows, and which layers are switched out. Run after smart actions. */
void switches_resolve(Document &doc, int frame);

/* Picks `value` at the current frame: keyed there, or on frame 0 the default. */
void switch_set(Document &doc, Layer &sw, float value);

/* A child put into an empty switch becomes its default. */
void switch_child_added(Layer &sw, uint64_t child);

/* Copied layers (old id to new): the switch's keys name the copied children. */
void switch_remap(Layer &layer, const std::map<uint64_t, uint64_t> &remap);

}  // namespace oanim
