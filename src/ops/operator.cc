#include "ops/operator.hh"

#include <cmath>
#include <cstdlib>

#include "anim/animation.hh"
#include "data/bones.hh"
#include "text/text_layer.hh"

#include <algorithm>

#include "core/log.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "core/profile.hh"

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
  /* "nan" and "inf" parse, but no setting means them: they'd spread through whatever they're mixed into. */
  return (!value.empty() && end && *end == 0 && std::isfinite(v)) ? v : fallback;
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
  const auto step = [&](bool back) {
    const int frame = C.document->frame_current;
    auto texts = C.document->texts;
    const ViewportOverlays overlays = C.document->overlays;
    auto ui_open = C.document->ui_open;
    const ViewportView view = C.document->view;
    if (back) {
      C.undo->undo(*C.document);
    }
    else {
      C.undo->redo(*C.document);
    }
    C.document->view = view;
    C.document->frame_current = frame;
    C.document->texts = std::move(texts);
    C.document->overlays = overlays;
    C.document->ui_open = std::move(ui_open);
  };
  step(true);
  /* As it was first run: from the same editor. */
  const std::string saved_area = C.area_type;
  C.area_type = area;
  const OpStatus status = operator_call(C, idname, args);
  C.area_type = saved_area;
  if (status == OpStatus::Cancelled) {
    /* The last result stands, and the panel stays on what was asked, for another option to make it work. */
    step(false);
    g_last_redo.args = args;
    operator_last_redo_keep(C);
  }
  return status;
}

/* How many operators that make an undo step are running: one called inside another's run is part of
 * that step (Blender's macros, a Python operator calling others), so only the outermost pushes. */
static int g_undo_depth = 0;

static std::string g_note;

void op_note(const std::string &note)
{
  g_note = note;
}

void op_arg_used(Context &C, const std::string &name, const std::string &value)
{
  if (C.op_args_used) {
    (*C.op_args_used)[name] = value;
  }
}

OpProperty op_float_property(const char *name, const char *label, const char *description, const char *fallback,
                             float min, float max, float step)
{
  OpProperty p;
  p.name = name;
  p.label = label;
  p.description = description;
  p.kind = OpProperty::Kind::Float;
  p.fallback = fallback;
  p.min = min;
  p.max = max;
  p.step = step;
  return p;
}

OpProperty op_bool_property(const char *name, const char *label, const char *description, const char *fallback)
{
  OpProperty p;
  p.name = name;
  p.label = label;
  p.description = description;
  p.kind = OpProperty::Kind::Bool;
  p.fallback = fallback;
  return p;
}

static uint64_t g_operator_runs = 0;

uint64_t operator_runs()
{
  return g_operator_runs;
}

OpStatus operator_call(Context &C, std::string_view idname, const OpArgs &args)
{
  const OperatorType *found = operators().find(idname);
  if (!found) {
    report(ReportType::Error, "Operator not found: " + std::string(idname));
    return OpStatus::Cancelled;
  }
  /* Copy: a Python operator may re-register operators while it runs. */
  const OperatorType ot = *found;
  ProfileScope profile(ot.idname.c_str());

  /* The poll sees the arguments too: an operator told which layer to work on (layer=) can run whatever
   * layer is active. */
  {
    const OpArgs *outer = C.op_args;
    C.op_args = &args;
    const bool can_run = operator_poll(C, ot);
    C.op_args = outer;
    if (!can_run) {
      return OpStatus::Cancelled;
    }
  }

  /* Arguments are visible to the operator while it runs; restore for nested calls. */
  const OpArgs *outer_args = C.op_args;
  C.op_args = &args;
  OpArgs *outer_used = C.op_args_used;
  OpArgs used;
  C.op_args_used = &used;
  const std::string outer_undo = C.undo_name;
  C.undo_name.clear();
  const bool undo_step = (ot.flag & OPTYPE_UNDO) && g_undo_depth == 0;
  struct UndoDepth {
    bool counts;
    explicit UndoDepth(bool c) : counts(c) { g_undo_depth += counts ? 1 : 0; }
    ~UndoDepth() { g_undo_depth -= counts ? 1 : 0; }
  };
  OpStatus status;
  if (!outer_args) {
    g_note.clear();
  }
  {
    const UndoDepth depth((ot.flag & OPTYPE_UNDO) != 0);
    /* Mirror editing: the mirror images join the selection for the run (an outer run did it already). */
    const bool mirror = (ot.flag & OPTYPE_MIRROR) && !outer_args;
    const MirrorSelection mirrored = mirror ? mirror_selection_begin(C) : MirrorSelection();
    g_operator_runs++;
    /* Time Together: the timing edits reach the linked channels (an outer run's does it once). */
    const auto together_arg = args.find("together");
    const bool together = (ot.flag & OPTYPE_TIMING) && !outer_args && C.document && C.document->time_together &&
                          (together_arg == args.end() || together_arg->second != "0") && (!ot.timing || ot.timing(C));
    TimeTogetherEdit timing_edit;
    if (together) {
      timing_edit.start(*C.document);
    }
    status = ot.exec(C);
    if (together && status == OpStatus::Finished) {
      keys_moved_report(timing_edit.finish(*C.document));
    }
    if (mirror) {
      mirror_selection_end(C, mirrored);
    }
  }
  const std::string undo_name = C.undo_name.empty() ? ot.name : C.undo_name;
  C.undo_name = outer_undo;
  C.op_args = outer_args;
  C.op_args_used = outer_used;
  /* An operator that changed the frame (a frame jump, undo) shows the new frame's values. */
  if (C.document) {
    animation_sync(*C.document);
    /* Layers moved into or out of a bone layer gain or lose their Armature modifier. */
    document_sync_armature_modifiers(*C.document);
    /* A text changed has its letters made again. */
    document_sync_text(*C.document);
  }
  if (status != OpStatus::Finished) {
    return status;
  }

  if (ot.flag & OPTYPE_REGISTER) {
    report(ReportType::Operator, operator_call_repr(ot.idname, args));
  }
  if (undo_step && C.undo && C.document) {
    C.undo->push(undo_name, *C.document);
    /* Recording may have keyed a bone that drives an action bone action. */
    animation_refresh_action_bones(*C.document);
    /* Run by the user (not inside another operator) with options: it can be adjusted now. */
    if (!ot.props.empty() && !outer_args) {
      g_last_redo.idname = ot.idname;
      g_last_redo.args = args;
      for (const auto &[name, value] : used) {
        g_last_redo.args.try_emplace(name, value);
      }
      for (const OpProperty &prop : ot.props) {
        g_last_redo.args.try_emplace(prop.name, prop.fallback);
      }
      /* The editor it was done in: the one drawing its menu, else the one under the mouse. */
      g_last_redo.area = C.drawing_area.empty() ? C.area_type : C.drawing_area;
      g_last_redo.note = g_note;
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
