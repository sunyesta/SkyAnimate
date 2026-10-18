#include "ops/operator.hh"

#include <algorithm>

#include "core/log.hh"
#include "data/document.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace sky {

OperatorRegistry &operators()
{
  static OperatorRegistry registry;
  return registry;
}

void OperatorRegistry::add(OperatorType ot)
{
  remove(ot.idname);
  types_.push_back(std::move(ot));
}

bool OperatorRegistry::remove(std::string_view idname)
{
  auto it = std::find_if(types_.begin(), types_.end(), [&](const OperatorType &ot) { return ot.idname == idname; });
  if (it == types_.end()) {
    return false;
  }
  types_.erase(it);
  return true;
}

void OperatorRegistry::remove_python()
{
  std::erase_if(types_, [](const OperatorType &ot) { return ot.is_python; });
}

const OperatorType *OperatorRegistry::find(std::string_view idname) const
{
  for (const OperatorType &ot : types_) {
    if (ot.idname == idname) {
      return &ot;
    }
  }
  return nullptr;
}

bool operator_idname_valid(std::string_view idname)
{
  const size_t dot = idname.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot == idname.size() - 1 ||
      idname.find('.', dot + 1) != std::string_view::npos)
  {
    return false;
  }
  return std::all_of(idname.begin(), idname.end(), [](char c) {
    return c == '.' || c == '_' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  });
}

bool operator_poll(Context &C, const OperatorType &ot)
{
  return !ot.poll || ot.poll(C);
}

OpStatus operator_call(Context &C, std::string_view idname)
{
  const OperatorType *found = operators().find(idname);
  if (!found) {
    report(ReportType::Error, "Operator not found: " + std::string(idname));
    return OpStatus::Cancelled;
  }
  /* Copy: a Python operator may re-register operators while it runs. */
  const OperatorType ot = *found;

  if (!operator_poll(C, ot)) {
    return OpStatus::Cancelled;
  }

  const OpStatus status = ot.exec(C);
  if (status != OpStatus::Finished) {
    return status;
  }

  if (ot.flag & OPTYPE_REGISTER) {
    report(ReportType::Operator, "sky.ops." + ot.idname + "()");
  }
  if ((ot.flag & OPTYPE_UNDO) && C.undo && C.document) {
    C.undo->push(ot.name, *C.document);
  }
  return status;
}

const char *op_status_name(OpStatus status)
{
  switch (status) {
    case OpStatus::Finished:
      return "FINISHED";
    case OpStatus::Cancelled:
      return "CANCELLED";
    case OpStatus::PassThrough:
      return "PASS_THROUGH";
  }
  return "CANCELLED";
}

}  // namespace sky
