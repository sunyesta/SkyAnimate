#pragma once

/* Operators: every editing action is a named command, like Blender's wmOperatorType.
 * Keymaps, menus, Python and (later) the MCP server all run operators, so undo and the
 * Info log behave the same whichever way an action is started. */

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "wm/context.hh"

namespace sky {

enum class OpStatus { Finished, Cancelled, PassThrough };

enum OperatorFlag : unsigned {
  /* Log the call in the Info editor. */
  OPTYPE_REGISTER = 1 << 0,
  /* Push an undo step after the operator finishes. */
  OPTYPE_UNDO = 1 << 1,
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

}  // namespace sky
