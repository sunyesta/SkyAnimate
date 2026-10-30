#pragma once

/* Drivers (Blender's): a value worked out every frame from other values, instead of keyed.
 *
 * A driver sets one number: a layer's channel (its path as in anim/animation.hh: "rotation",
 * "opacity", 'bones["arm"].angle', the camera's "zoom", the simulation's "gravity"), one component of
 * a two-number channel ("position" X or Y), or one component of a shape's colour
 * ("shapes[12].fill", R G B A). Its variables read other such numbers (or the frame), and its type
 * combines them: an expression (anim/expression.hh), or their average, sum, smallest or largest.
 * Remap then maps a range of the result onto another, as a smart bone maps an angle to an action. */

#include <cstdint>
#include <string>
#include <vector>

namespace oanim {

/* A number drivers set or read: a layer's (by id, the camera and the simulation included) channel or
 * colour at `path`, and its `component` (-1 for a one-number channel). */
struct DriverRef {
  uint64_t layer = 0;
  std::string path;
  int component = -1;
  /* Not saved: what the path meant when it was last worked out, so it can follow it (see
   * drivers_evaluate): the bone's id for 'bones["arm"]...', and the constraint's, modifier's or mask
   * row's uid for an index into them ("modifiers[1]..."). 0 until then. */
  uint64_t bone = 0;
  uint64_t item = 0;

  bool operator==(const DriverRef &o) const
  {
    return layer == o.layer && path == o.path && component == o.component;
  }
};

struct DriverVar {
  /* What the expression calls it. */
  std::string name = "var";
  DriverRef source;

  bool operator==(const DriverVar &) const = default;
};

enum class DriverType : uint8_t { Expression, Average, Sum, Min, Max };

struct Driver {
  DriverRef target;
  DriverType type = DriverType::Expression;
  std::string expression = "var";
  std::vector<DriverVar> vars;
  /* Remap: the result's range from_min..from_max goes to to_min..to_max (clamped to it with
   * `clamp`). */
  bool remap = false;
  float from_min = 0.0f, from_max = 1.0f;
  float to_min = 0.0f, to_max = 1.0f;
  bool clamp = true;
  /* 2-Way Linked (the Switch layers' two-way links): it and the property its one variable reads share one
   * set of keys, the variable's, and either can be edited (an edit to it goes back into the variable). It
   * shares the change from setup: target = target_rest + factor (minus with `flip`) x (source -
   * source_rest), so each side keeps its own rest. Only for a driver with one variable and an expression
   * like var, -var or var * 0.5 (see driver_two_way_factor). */
  bool two_way = false;
  float factor = 1.0f;
  bool flip = false;
  float source_rest = 0.0f, target_rest = 0.0f;
  /* target_rest holds what the driven value was before the driver was made (driving overwrites it). */
  bool rest_captured = false;
  /* Why it couldn't be worked out on the last evaluation ("" when it could): a missing layer, a bad
   * expression, a loop. Not saved. */
  std::string error;

  bool same_setup(const Driver &o) const
  {
    return type == o.type && expression == o.expression && vars == o.vars && remap == o.remap &&
           from_min == o.from_min && from_max == o.from_max && to_min == o.to_min && to_max == o.to_max &&
           clamp == o.clamp && two_way == o.two_way && factor == o.factor && flip == o.flip &&
           source_rest == o.source_rest && target_rest == o.target_rest && rest_captured == o.rest_captured;
  }
  bool operator==(const Driver &) const = default;
};

}  // namespace oanim
