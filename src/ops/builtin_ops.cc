#include "ops/builtin_ops.hh"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>

#include "core/log.hh"
#include "data/document.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "python/py_runtime.hh"
#include "wm/context.hh"

namespace sky {

/* What undo leaves alone: the playhead (as in Blender and Moho) and text blocks, so undoing a
 * drawing change never loses typed code. The action being edited is undone with the rest: its
 * keys are swapped into the channels, so the two always go together. */
struct ViewState {
  int frame_current;
  std::vector<Text> texts;
  ViewportOverlays overlays;
  std::map<std::string, bool> ui_open;
  ViewportView view;
};

static ViewState view_state_get(const Document &doc)
{
  return {doc.frame_current, doc.texts, doc.overlays, doc.ui_open, doc.view};
}

static void view_state_restore(Document &doc, ViewState &view)
{
  doc.frame_current = view.frame_current;
  doc.texts = std::move(view.texts);
  doc.overlays = view.overlays;
  doc.ui_open = std::move(view.ui_open);
  doc.view = view.view;
}

static void ed_undo_register()
{
  OperatorType ot;
  ot.idname = "ed.undo";
  ot.name = "Undo";
  ot.description = "Undo the last action";
  ot.poll = [](Context &C) { return C.undo && C.undo->can_undo(); };
  ot.exec = [](Context &C) {
    const std::string name = C.undo->step_name(C.undo->active());
    ViewState view = view_state_get(*C.document);
    C.undo->undo(*C.document);
    view_state_restore(*C.document, view);
    report(ReportType::Info, "Undo: " + name, false);
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
    ViewState view = view_state_get(*C.document);
    C.undo->redo(*C.document);
    view_state_restore(*C.document, view);
    report(ReportType::Info, "Redo: " + C.undo->step_name(C.undo->active()), false);
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

  /* Tab, as Blender's edit mode toggle: to the setup frame (0), where the rig and drawing are
   * edited, and back to the frame you were posing on. */
  frame_operator_register("screen.frame_setup_toggle", "Toggle Setup Frame",
                          "Go to the setup frame (0), or back to the frame you came from (Tab)", [](Context &C) {
                            Document &doc = *C.document;
                            if (doc.frame_current != 0) {
                              C.wm->frame_before_setup = doc.frame_current;
                              doc.frame_current = 0;
                            }
                            else {
                              doc.frame_current = C.wm->frame_before_setup > 0 ? C.wm->frame_before_setup
                                                                               : doc.frame_start;
                            }
                            return OpStatus::Finished;
                          });
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

  /* Keyframe jumps live with the other keyframe operators (ops/anim_ops.cc). */
}

/* ---------------------------------------------------------------------------------
 * Text editor, named after Blender's text.* operators.
 */

static Text *active_text(Context &C)
{
  return C.document ? document_text_find(*C.document, C.text_name) : nullptr;
}

static void text_operators_register()
{
  OperatorType new_text;
  new_text.idname = "text.new";
  new_text.name = "New Text";
  new_text.description = "Create a new text block";
  new_text.exec = [](Context &C) {
    C.text_name = document_text_add(*C.document, "Text").name;
    C.undo->mark_changed();
    return OpStatus::Finished;
  };
  operators().add(std::move(new_text));

  OperatorType run;
  run.idname = "text.run_script";
  run.name = "Run Script";
  run.description = "Run the text in the text editor as a Python script";
  run.poll = [](Context &C) { return active_text(C) != nullptr; };
  run.exec = [](Context &C) {
    /* Copy: the script may add or remove texts while it runs. */
    const Text text = *active_text(C);
    return python::text_run(text.name, text.body) ? OpStatus::Finished : OpStatus::Cancelled;
  };
  operators().add(std::move(run));

  OperatorType save;
  save.idname = "text.save";
  save.name = "Save Text";
  save.description = "Save the text to the file it was opened from or last saved to";
  save.poll = [](Context &C) {
    const Text *text = active_text(C);
    return text && !text->filepath.empty();
  };
  save.exec = [](Context &C) {
    const Text &text = *active_text(C);
    std::ofstream file(std::filesystem::path(reinterpret_cast<const char8_t *>(text.filepath.c_str())),
                       std::ios::binary);
    file << text.body;
    if (!file) {
      report(ReportType::Error, "Could not save " + text.filepath);
      return OpStatus::Cancelled;
    }
    report(ReportType::Info, "Saved text \"" + text.filepath + "\"");
    return OpStatus::Finished;
  };
  operators().add(std::move(save));

  OperatorType remove;
  remove.idname = "text.unlink";
  remove.name = "Delete Text";
  remove.description = "Delete the text block from the document";
  remove.poll = [](Context &C) { return active_text(C) != nullptr; };
  remove.exec = [](Context &C) {
    std::erase_if(C.document->texts, [&](const Text &text) { return text.name == C.text_name; });
    C.undo->mark_changed();
    C.text_name = C.document->texts.empty() ? "" : C.document->texts.back().name;
    return OpStatus::Finished;
  };
  operators().add(std::move(remove));
}

static void workspace_operators_register()
{
  for (const int step : {1, -1}) {
    OperatorType ot;
    ot.idname = step > 0 ? "screen.workspace_cycle_next" : "screen.workspace_cycle_prev";
    ot.name = step > 0 ? "Next Workspace" : "Previous Workspace";
    ot.description = "Switch to the next or previous workspace tab";
    ot.poll = [](Context &C) { return C.wm && C.wm->workspace_count > 1; };
    ot.exec = [step](Context &C) {
      WindowManager &wm = *C.wm;
      wm.workspace_index = (wm.workspace_index + step + wm.workspace_count) % wm.workspace_count;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

void builtin_operators_register()
{
  playback_operators_register();
  text_operators_register();
  workspace_operators_register();
  ed_undo_register();
  ed_redo_register();
  {
    OperatorType ot;
    ot.idname = "view.overlays_toggle";
    ot.name = "Toggle Overlays";
    ot.description = "Show or hide the viewport's overlays (Shift Z), as the Overlays button does";
    ot.poll = [](Context &C) { return C.wm != nullptr; };
    ot.exec = [](Context &C) {
      C.wm->overlays_toggle_request = true;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* F9: Blender's Adjust Last Operation, opening (or folding) the ViewportActionPanel. */
    OperatorType ot;
    ot.idname = "screen.redo_last";
    ot.name = "Adjust Last Operation";
    ot.description = "Open the last action's options in the ViewportActionPanel (F9), or fold them";
    ot.poll = [](Context &C) { return C.wm != nullptr && operator_last_redo(C) != nullptr; };
    ot.exec = [](Context &C) {
      C.wm->action_panel_toggle_request = true;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  wm_ui_scale_register("wm.ui_scale_increase", "Increase Interface Scale", 0.25f);
  wm_ui_scale_register("wm.ui_scale_decrease", "Decrease Interface Scale", -0.25f);
  wm_ui_scale_register("wm.ui_scale_reset", "Reset Interface Scale", 0.0f);
  {
    /* Blender's Render Image: the current frame through the camera, at the document's size,
     * render quality and render samples, shown in the Render Result window. */
    OperatorType ot;
    ot.idname = "render.render";
    ot.name = "Render Frame";
    ot.description = "Render the current frame at the document's size and show it";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.wm && !C.wm->background; };
    ot.exec = [](Context &C) {
      C.wm->render_requested = true;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace sky
