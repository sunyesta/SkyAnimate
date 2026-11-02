#pragma once

/* Working drivers out (data/driver.hh): every frame, after keys and action bone actions and before the
 * bones bend the drawings, in the order they depend on each other (a driver reading a driven value
 * goes after its driver). Each sets its number and the channel's recorded value alike, so recording
 * never takes a driven value for an edit. A driver that can't be worked out (a missing layer, a bad
 * expression, a loop) keeps its error and leaves its number alone. */

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "data/driver.hh"

namespace oanim {

struct Document;

/* Works out every driver at the document's current frame. True when one posed a bone (action bone
 * actions then follow it; see animation_evaluate). On the setup frame (0) drivers leave bones and
 * shapes at rest, as constraints do. */
bool drivers_evaluate(Document &doc);

/* How many numbers `path` of `layer` holds (1 a number, 2 a position or scale, 4 a colour), 0 when
 * there is no such thing to drive. */
int driver_ref_size(Document &doc, uint64_t layer, const std::string &path);
/* The number a reference names, or false when it names nothing. */
bool driver_ref_read(Document &doc, const DriverRef &ref, float *r_value);
/* What a reference is called: "Layer 1: Angle", "Clay: Opacity X", "Box 1: Fill Color R". */
std::string driver_ref_label(Document &doc, const DriverRef &ref);

/* The driver of a number, or null. */
Driver *driver_find(Document &doc, const DriverRef &target);
/* The references a field shows: one per component (`component` -1 on a two-number channel or a
 * colour is all of them). Empty when it can't be driven (points and curves, a component past its
 * end). */
std::vector<DriverRef> driver_field_refs(Document &doc, uint64_t layer, const std::string &path, int component);
/* A shape's colour as a driver path: "shapes[<id>].fill" or ".line". */
std::string driver_shape_color_path(uint64_t shape_id, bool fill);
/* A shape's row of X and Y in the Shape tab as a driver path: "shapes[<id>].position". It is what the
 * row shows: on frame 0 the middle of its points (Position), on other frames how far that has moved
 * from the setup pose (Transform, what their keys hold). Driving it moves the points together,
 * keeping their own motion. */
std::string driver_shape_position_path(uint64_t shape_id);

/* Layers copied (old id to new): the drivers of the old ones copied onto the new ones, their
 * variables reading the copies where they read a copied layer. */
void drivers_copy_for_layers(Document &doc, const std::map<uint64_t, uint64_t> &remap);

/* Two-way links (data/driver.hh, Driver::two_way). The factor a driver's expression scales its one
 * variable by (var 1, -var -1, var * 0.5, 2 * var, var / 4), or false with why it can't be 2-Way Linked. */
bool driver_two_way_factor(const Driver &d, float *r_factor, std::string *r_why);
/* Makes `d` 2-Way Linked: its factor from its expression (a Flip for a minus), each side's rest, and the
 * keys stored once: the variable's, or with `keep_target` the driven property's turned into the
 * variable's. `r_dropped`: how many keys went. False with why when it can't be: not one variable times a
 * number, a side that is linked already in another way, a loop through an action bone action. */
bool driver_two_way_link(Document &doc, Driver &d, bool keep_target, std::string *r_why, int *r_dropped);
/* A field's rest: a bone's pose part at rest (no change from the setup pose, a scale of 1), else its value
 * on frame 0 (its key there, or its value now). */
float driver_field_rest(Document &doc, const DriverRef &ref);
/* How many keys linking would drop: the driven property's (or with `keep_target` the variable's). */
int driver_two_way_drop_count(Document &doc, const Driver &d, bool keep_target);
/* Before a 2-Way Linked driver is taken off (Unlink): the driven property gets keys of its own that move it
 * as the link did, so nothing changes on screen. */
void driver_two_way_bake(Document &doc, const Driver &d);
/* What a field is in a two-way link: 0 none, 1 the driven side, 2 the side read (the keys' owner). */
int driver_two_way_role(Document &doc, const DriverRef &ref);

/* A driver that copies `source` (Copy As New Driver): its variable reads it, the expression is "var". */
Driver driver_copying(const DriverRef &source);

/* The drivers copied (Copy As New Driver, Copy Driver): one per number of the field copied, for
 * Paste Driver. The app's own, not the system's. */
std::vector<Driver> &driver_clipboard();

}  // namespace oanim
