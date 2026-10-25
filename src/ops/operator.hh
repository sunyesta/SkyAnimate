#pragma once

/* Operators: every editing action is a named command, like Blender's wmOperatorType.
 * Keymaps, menus, Python and (later) the MCP server all run operators, so undo and the
 * Info log behave the same whichever way an action is started. */

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wm/context.hh"

namespace oanim {

enum class OpStatus { Finished, Cancelled, PassThrough };

enum OperatorFlag : unsigned {
  /* Log the call in the Info editor. */
  OPTYPE_REGISTER = 1 << 0,
  /* Push an undo step after the operator finishes. */
  OPTYPE_UNDO = 1 << 1,
  /* With X mirror editing on, it acts on the selected points' mirror images too (in the same layer,
   * or a sided layer's partner): see mirror_selection_begin. */
  OPTYPE_MIRROR = 1 << 2,
};

/* An option an operator shows in the ViewportActionPanel after it runs (Blender's operator
 * properties in Adjust Last Operation): changing it there runs the operator again with it. Values
 * travel as OpArgs strings, as every argument does. */
struct OpProperty {
  enum class Kind { Enum, Bool, Float, Int };
  std::string name;
  std::string label;
  std::string description;
  Kind kind = Kind::Enum;
  /* Enum: (identifier, label) pairs. */
  std::vector<std::pair<std::string, std::string>> items;
  std::string fallback;
  float min = 0.0f, max = 1.0f, step = 0.01f;
  /* Shown in the panel only when this says so of the run's args (null: always). */
  std::function<bool(const OpArgs &)> visible;
  /* Greyed out in the panel unless this says it can do something here (null: always on), with
   * `disabled_why` added to its tooltip. */
  std::function<bool(Context &)> enabled;
  std::string disabled_why;
};

struct OperatorType {
  /* "category.name", e.g. "ed.undo". */
  std::string idname;
  std::string name;
  std::string description;
  unsigned flag = 0;
  /* Defined in Python; removed before the interpreter shuts down. */
  bool is_python = false;
  /* Optional: returns false when the operator can't run in this context. */
  std::function<bool(Context &C)> poll;
  std::function<OpStatus(Context &C)> exec;
  /* Options for the ViewportActionPanel; with some, an undoable run can be redone with others. */
  std::vector<OpProperty> props;
};

class OperatorRegistry {
 public:
  /* Adds an operator type, replacing one with the same idname. */
  void add(OperatorType ot);
  bool remove(std::string_view idname);
  void remove_python();
  const OperatorType *find(std::string_view idname) const;
  const std::vector<OperatorType> &all() const { return types_; }

 private:
  std::vector<OperatorType> types_;
};

OperatorRegistry &operators();

/* True if "category.name" with lowercase letters, digits and underscores. */
bool operator_idname_valid(std::string_view idname);

bool operator_poll(Context &C, const OperatorType &ot);

/* Runs an operator: poll, exec, then the Info log and an undo step when flagged. */
OpStatus operator_call(Context &C, std::string_view idname, const OpArgs &args = {});

const char *op_status_name(OpStatus status);

/* The last operator run, if it can be adjusted (Blender's last redo): an undoable operator with
 * options, run from the editor `area`, that nothing has changed the document after. Its args have
 * every option, the ones not given at their defaults. */
struct LastOperation {
  std::string idname;
  OpArgs args;
  std::string area;
  uint64_t change_count = 0;
};
const LastOperation *operator_last_redo(const Context &C);
/* A change that leaves the last operation as it was (folding or opening the ViewportActionPanel,
 * which the project keeps): it stays adjustable. Call right after making it, while it still was. */
void operator_last_redo_keep(const Context &C);
/* Runs it again with `args` instead: undoes its step, then runs it with them. */
OpStatus operator_redo_last(Context &C, const OpArgs &args);
/* The running operator says what it used for option `name` when it wasn't given one (a choice kept
 * from the last move, say): the ViewportActionPanel shows that instead of the option's default. */
void op_arg_used(Context &C, const std::string &name, const std::string &value);
/* An option for the ViewportActionPanel: a number from min to max, or a checkbox. */
OpProperty op_float_property(const char *name, const char *label, const char *description, const char *fallback,
                             float min, float max, float step);
OpProperty op_bool_property(const char *name, const char *label, const char *description, const char *fallback);

}  // namespace oanim
