#pragma once

/* Switch layers (Moho's switch layers, Spine's slots, Rive's Solos): a group that shows one of its
 * children, or none. Its Active channel (Layer::switch_keys) holds the child's layer id, as Draw
 * Order holds its target's, so renaming or reordering children never breaks a key; 0 is None. Keys
 * only hold. Frame 0 holds the default child.
 *
 * Choices never add (one child can't be added to another), so each frame has one winner, highest
 * first: a key made by hand after frame 0 (held until the next key; a Rig key hands the switch back
 * to what is below it, as Cartoon Animator's Release); an action bone action whose bone has turned past
 * one of its switch keys (the one listed first among its bone layer's actions when several have a
 * say); the default. Moho lets the action bone win over hand keys; here the hand wins. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace oanim {

struct Document;
struct Layer;
struct SwitchLink;

/* A key that hands the switch back to the rig (action bone actions, then the default). */
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

/* Who wins on every switch at `frame` (action bone actions have put their picks in switch_rig), which
 * child each shows, and which layers are switched out. Run after action bone actions. While the active
 * layer is inside a child a switch isn't showing, the viewport shows that child instead, for
 * editing, without keying it (Layer::edit_out; renders and exports keep the pick), on the frame it
 * became active: scrubbing or playing shows the picks again. */
void switches_resolve(Document &doc, int frame);

/* Picks `value` at the current frame: keyed there, or on frame 0 the default. */
void switch_set(Document &doc, Layer &sw, float value);
/* Picks `value` for the whole hold the current frame is in: the key it starts with gets it (the
 * default when that is frame 0, or the switch isn't keyed). */
void switch_set_hold(Document &doc, Layer &sw, float value);
/* Points every key holding `from` (frame 0, the timeline's and actions') at `to`; how many. */
int switch_replace_keys(Document &doc, Layer &sw, float from, float to, bool from_entry = false);

/* Who decides at a frame: a hand key, an action bone action, or the default (nothing else). */
enum class SwitchSource { Hand, Rig, Default };
/* Who decides now (action bone actions as they last played). */
SwitchSource switch_source(const Document &doc, const Layer &sw, int frame);
/* Whether any action bone action keys the switch. */
bool switch_keyed_by_actions(const Layer &sw);
/* The switch a layer belongs to: itself when it is one, else the nearest switch holding it; null. */
Layer *switch_of(Document &doc, uint64_t layer);
/* The child of `sw` that holds `layer` (or is it); null when it isn't inside `sw`. */
const Layer *switch_child_holding(const Document &doc, const Layer &sw, uint64_t layer);

/* A preview (the asset shelf's hover, a thumbnail being drawn): while set, the viewport shows `child` of
 * switch `sw` (0: none) in place of its pick and of a child being edited, nothing keyed and no undo
 * step. Renders and exports keep the pick. One switch at a time; cleared with switch_preview_clear. */
void switch_preview_set(uint64_t sw, uint64_t child);
void switch_preview_clear();
bool switch_preview_active(uint64_t *r_sw = nullptr, uint64_t *r_child = nullptr);

/* How many keys of `sw` (frame 0's, the timeline's and actions'; a linked switch's group keys) show `child`. */
int switch_keys_showing(const Document &doc, const Layer &sw, uint64_t child);

/* --- Switches linked by name (Document::switch_links) ---------------------------------------------
 * Same-named switches at the same place inside a switch's children (a Mouth in each view of Heads) link
 * by name (Layer::link_same_named, on by default), or any switches by hand (Link Switches): one set of
 * keys for the group, kept on its lead's Switch channel as entries of the group's names (ids, so a
 * rename keeps them). Each member shows its child answering to the name (its name, or an alias); one
 * lacking it keeps showing the last name it had (the latest key before that it has, then the default,
 * then its own setup child), and says so (Layer::switch_missing). A pick on any member keys the group.
 * Renaming a drawing renames its partners and the group's name; Unlink gives a member its own keys
 * showing what it showed. Functions above take and give a member's own child ids either way. */
SwitchLink *switch_link_of(Document &doc, const Layer &sw);
const SwitchLink *switch_link_of(const Document &doc, const Layer &sw);
std::vector<Layer *> switch_link_members(Document &doc, uint64_t link);
/* A value of `sw`'s Switch channel (a linked group's lead's: an entry) in words, whether `sw` lacks it,
 * and, when it does, the child it keeps showing at `frame` ("" when it has it). */
std::string switch_key_label(const Document &doc, const Layer &sw, float value);
bool switch_key_missing(const Document &doc, const Layer &sw, float value);
std::string switch_key_held(const Document &doc, const Layer &sw, float value, int frame);
/* The child `sw` shows by default (frame 0), and by its keys at `frame` (holds included; not action bone actions). */
uint64_t switch_default_child(const Document &doc, const Layer &sw);
uint64_t switch_child_at(const Document &doc, const Layer &sw, int frame);
/* Links `members` (keys from `keep_from`, else the first keyed, else the first; `r_dropped`: the others'
 * keys that went). The group's id, 0 for fewer than two. */
uint64_t switch_link_create(Document &doc, const std::vector<Layer *> &members, Layer *keep_from, int *r_dropped);
/* Unlinks `sw` by hand (not linked again by name), its keys what it showed. */
bool switch_unlink(Document &doc, Layer &sw);
/* Before a layer goes: a group's lead inside it hands the keys to another member. */
void switch_links_before_remove(Document &doc, uint64_t layer);
/* Links same-named switches, keeps groups whole and passes renames on to partners (unless
 * switch_rename_here_next was called just before). What it did, to report. */
std::vector<std::string> switch_links_update(Document &doc);
void switch_rename_here_next();
/* The names a linked switch's group keys (frame 0's and its timeline's) that `sw` has no drawing for. */
std::vector<std::string> switch_link_lacks(const Document &doc, const Layer &sw);
/* A name as a person reads it, for flagging near misses: lower case, without ".001", " copy", ".png". */
std::string switch_name_stem(const std::string &name);

/* A child put into an empty switch becomes its default. */
void switch_child_added(Layer &sw, uint64_t child);

/* Copied layers (old id to new): the switch's keys name the copied children. */
void switch_remap(Layer &layer, const std::map<uint64_t, uint64_t> &remap);

}  // namespace oanim
