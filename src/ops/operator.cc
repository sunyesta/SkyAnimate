#include "ops/operator.hh"

#include <cstdlib>

#include "anim/animation.hh"
#include "data/bones.hh"

#include <algorithm>

#include "core/log.hh"
#include "data/document.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

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

std::string op_arg(const Context &C, const std::string &name, const std::string &fallback)
{
  if (C.op_args) {
    if (auto it = C.op_args->find(name); it != C.op_args->end()) {
      return it->second;
    }
  }
  return fallback;
}

bool op_has_arg(const Context &C, const std::string &name)
{
  return C.op_args && C.op_args->count(name) > 0;
}

bool op_arg_bool(const Context &C, const std::string &name)
{
  const std::string value = op_arg(C, name);
  return value == "1" || value == "True" || value == "true";
}

int op_arg_int(const Context &C, const std::string &name, int fallback)
{
  const std::string value = op_arg(C, name);
  char *end = nullptr;
  const long v = std::strtol(value.c_str(), &end, 10);
  return (!value.empty() && end && *end == 0) ? int(v) : fallback;
}

float op_arg_float(const Context &C, const std::string &name, float fallback)
{
  const std::string value = op_arg(C, name);
  char *end = nullptr;
  const float v = std::strtof(value.c_str(), &end);
  return (!value.empty() && end && *end == 0) ? v : fallback;
}

/* oanim.ops.wm.open_mainfile(filepath="walk.oanim"), for the Info log. */
static std::string operator_call_repr(const std::string &idname, const OpArgs &args)
{
  std::string text = "oanim.ops." + idname + "(";
  bool first = true;
  for (const auto &[name, value] : args) {
    text += (first ? "" : ", ") + name + "=\"";
    for (const char c : value) {
      if (c == '\\' || c == '"') {
        text += '\\';
      }
      text += c;
    }
    text += "\"";
    first = false;
  }
  return text + ")";
}

static LastOperation g_last_redo;
static bool g_has_last_redo = false;

const LastOperation *operator_last_redo(const Context &C)
{
  if (!g_has_last_redo || !C.undo || C.undo->change_count() != g_last_redo.change_count ||
      !operators().find(g_last_redo.idname))
  {
    return nullptr;
  }
  return &g_last_redo;
}

void operator_last_redo_keep(const Context &C)
{
  if (g_has_last_redo && C.undo) {
    g_last_redo.change_count = C.undo->change_count();
  }
}

OpStatus operator_redo_last(Context &C, const OpArgs &args)
{
  const LastOperation *last = operator_last_redo(C);
  if (!last || !C.document) {
    return OpStatus::Cancelled;
  }
  const std::string idname = last->idname, area = last->area;
  /* As Undo does: the frame, the texts and the overlays aren't part of the step. */
  const int frame = C.document->frame_current;
  auto texts = C.document->texts;
  const ViewportOverlays overlays = C.document->overlays;
  auto ui_open = C.document->ui_open;
  const ViewportView view = C.document->view;
  C.undo->undo(*C.document);
  C.document->view = view;
  C.document->frame_current = frame;
  C.document->texts = std::move(texts);
  C.document->overlays = overlays;
  C.document->ui_open = std::move(ui_open);
  /* As it was first run: from the same editor. */
  const std::string saved_area = C.area_type;
  C.area_type = area;
  const OpStatus status = operator_call(C, idname, args);
  C.area_type = saved_area;
  return status;
}

/* How many operators that make an undo step are running: one called inside another's run is part of
 * that step (Blender's macros, a Python operator calling others), so only the outermost pushes. */
static int g_undo_depth = 0;

OpStatus operator_call(Context &C, std::string_view idname, const OpArgs &args)
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

  /* Arguments are visible to the operator while it runs; restore for nested calls. */
  const OpArgs *outer_args = C.op_args;
  C.op_args = &args;
  const std::string outer_undo = C.undo_name;
  C.undo_name.clear();
  const bool undo_step = (ot.flag & OPTYPE_UNDO) && g_undo_depth == 0;
  struct UndoDepth {
    bool counts;
    explicit UndoDepth(bool c) : counts(c) { g_undo_depth += counts ? 1 : 0; }
    ~UndoDepth() { g_undo_depth -= counts ? 1 : 0; }
  };
  OpStatus status;
  {
    const UndoDepth depth((ot.flag & OPTYPE_UNDO) != 0);
    status = ot.exec(C);
  }
  const std::string undo_name = C.undo_name.empty() ? ot.name : C.undo_name;
  C.undo_name = outer_undo;
  C.op_args = outer_args;
  /* An operator that changed the frame (a frame jump, undo) shows the new frame's values. */
  if (C.document) {
    animation_sync(*C.document);
    /* Layers moved into or out of a bone layer gain or lose their Armature modifier. */
    document_sync_armature_modifiers(*C.document);
  }
  if (status != OpStatus::Finished) {
    return status;
  }

  if (ot.flag & OPTYPE_REGISTER) {
    report(ReportType::Operator, operator_call_repr(ot.idname, args));
  }
  if (undo_step && C.undo && C.document) {
    C.undo->push(undo_name, *C.document);
    /* Recording may have keyed a bone that drives a smart action. */
    animation_refresh_smart(*C.document);
    /* Run by the user (not inside another operator) with options: it can be adjusted now. */
    if (!ot.props.empty() && !outer_args) {
      g_last_redo.idname = ot.idname;
      g_last_redo.args = args;
      for (const OpProperty &prop : ot.props) {
        g_last_redo.args.try_emplace(prop.name, prop.fallback);
      }
      /* The editor it was done in: the one drawing its menu, else the one under the mouse. */
      g_last_redo.area = C.drawing_area.empty() ? C.area_type : C.drawing_area;
      g_last_redo.change_count = C.undo->change_count();
      g_has_last_redo = true;
    }
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

}  // namespace oanim
