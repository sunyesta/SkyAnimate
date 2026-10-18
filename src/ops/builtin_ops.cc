#include "ops/builtin_ops.hh"

#include <algorithm>

#include "core/log.hh"
#include "data/document.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace sky {

static void ed_undo_register()
{
  OperatorType ot;
  ot.idname = "ed.undo";
  ot.name = "Undo";
  ot.description = "Undo the last action";
  ot.poll = [](Context &C) { return C.undo && C.undo->can_undo(); };
  ot.exec = [](Context &C) {
    const std::string name = C.undo->step_name(C.undo->active());
    C.undo->undo(*C.document);
    report(ReportType::Info, "Undo: " + name);
    return OpStatus::Finished;
  };
  operators().add(std::move(ot));
}

static void ed_redo_register()
{
  OperatorType ot;
  ot.idname = "ed.redo";
  ot.name = "Redo";
  ot.description = "Redo the last undone action";
  ot.poll = [](Context &C) { return C.undo && C.undo->can_redo(); };
  ot.exec = [](Context &C) {
    C.undo->redo(*C.document);
    report(ReportType::Info, "Redo: " + C.undo->step_name(C.undo->active()));
    return OpStatus::Finished;
  };
  operators().add(std::move(ot));
}

static void wm_quit_register()
{
  OperatorType ot;
  ot.idname = "wm.quit";
  ot.name = "Quit";
  ot.description = "Quit SkyAnimate";
  ot.exec = [](Context &C) {
    C.wm->quit_requested = true;
    return OpStatus::Finished;
  };
  operators().add(std::move(ot));
}

static void wm_ui_scale_register(const char *idname, const char *name, float step)
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = "Change the interface scale";
  ot.poll = [](Context &C) { return C.wm && !C.wm->background; };
  ot.exec = [step](Context &C) {
    const float scale = (step == 0.0f) ? 1.0f : C.wm->ui_scale + step;
    C.wm->ui_scale = std::clamp(scale, 0.5f, 3.0f);
    C.wm->ui_scale_changed = true;
    return OpStatus::Finished;
  };
  operators().add(std::move(ot));
}

void builtin_operators_register()
{
  ed_undo_register();
  ed_redo_register();
  wm_quit_register();
  wm_ui_scale_register("wm.ui_scale_increase", "Increase Interface Scale", 0.25f);
  wm_ui_scale_register("wm.ui_scale_decrease", "Decrease Interface Scale", -0.25f);
  wm_ui_scale_register("wm.ui_scale_reset", "Reset Interface Scale", 0.0f);
}

}  // namespace sky
