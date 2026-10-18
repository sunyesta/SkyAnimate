#include "ops/builtin_ops.hh"

#include <algorithm>
#include <functional>

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
    /* As in Blender, undo doesn't move the playhead. */
    const int frame = C.document->frame_current;
    C.undo->undo(*C.document);
    C.document->frame_current = frame;
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
    const int frame = C.document->frame_current;
    C.undo->redo(*C.document);
    C.document->frame_current = frame;
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

/* ---------------------------------------------------------------------------------
 * Frames and playback, named after Blender's screen.* operators. Changing frames is not
 * an undo step, as in Blender.
 */

static void frame_operator_register(const char *idname, const char *name, const char *description,
                                    std::function<OpStatus(Context &C)> exec)
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.exec = std::move(exec);
  operators().add(std::move(ot));
}

static void animation_play_toggle(Context &C, bool reverse)
{
  WindowManager &wm = *C.wm;
  if (wm.playing && wm.play_reverse == reverse) {
    wm.playing = false;
    return;
  }
  if (!wm.playing) {
    wm.play_start_frame = C.document->frame_current;
  }
  wm.playing = true;
  wm.play_reverse = reverse;
  wm.play_last_ns = 0;
  wm.play_frames_due = 0.0;
}

static void playback_operators_register()
{
  frame_operator_register("screen.animation_play", "Play Animation", "Play the animation, or pause it",
                          [](Context &C) {
                            animation_play_toggle(C, false);
                            return OpStatus::Finished;
                          });
  frame_operator_register("screen.animation_play_reverse", "Play Animation in Reverse",
                          "Play the animation backwards, or pause it", [](Context &C) {
                            animation_play_toggle(C, true);
                            return OpStatus::Finished;
                          });

  OperatorType cancel;
  cancel.idname = "screen.animation_cancel";
  cancel.name = "Cancel Animation";
  cancel.description = "Stop playback and return to the frame it started from";
  cancel.poll = [](Context &C) { return C.wm && C.wm->playing; };
  cancel.exec = [](Context &C) {
    C.wm->playing = false;
    C.document->frame_current = C.wm->play_start_frame;
    return OpStatus::Finished;
  };
  operators().add(std::move(cancel));

  frame_operator_register("screen.frame_jump_start", "Jump to Start", "Jump to the first frame of the range",
                          [](Context &C) {
                            C.document->frame_current = C.document->frame_start;
                            return OpStatus::Finished;
                          });
  frame_operator_register("screen.frame_jump_end", "Jump to End", "Jump to the last frame of the range",
                          [](Context &C) {
                            C.document->frame_current = C.document->frame_end;
                            return OpStatus::Finished;
                          });
  frame_operator_register("screen.frame_offset_forward", "Next Frame", "Go forward one frame", [](Context &C) {
    C.document->frame_current++;
    return OpStatus::Finished;
  });
  frame_operator_register("screen.frame_offset_backward", "Previous Frame", "Go back one frame", [](Context &C) {
    C.document->frame_current = std::max(0, C.document->frame_current - 1);
    return OpStatus::Finished;
  });

  /* Keyframes arrive in milestone 3; until then there is nothing to jump to. */
  const auto no_keyframes = [](Context &) {
    report(ReportType::Info, "No more keyframes to jump to in this direction");
    return OpStatus::Cancelled;
  };
  frame_operator_register("screen.keyframe_jump_next", "Jump to Next Keyframe", "Jump to the next keyframe",
                          no_keyframes);
  frame_operator_register("screen.keyframe_jump_prev", "Jump to Previous Keyframe",
                          "Jump to the previous keyframe", no_keyframes);
}

void builtin_operators_register()
{
  playback_operators_register();
  ed_undo_register();
  ed_redo_register();
  wm_quit_register();
  wm_ui_scale_register("wm.ui_scale_increase", "Increase Interface Scale", 0.25f);
  wm_ui_scale_register("wm.ui_scale_decrease", "Decrease Interface Scale", -0.25f);
  wm_ui_scale_register("wm.ui_scale_reset", "Reset Interface Scale", 0.0f);
}

}  // namespace sky
