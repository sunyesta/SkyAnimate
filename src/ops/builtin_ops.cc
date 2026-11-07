#include "ops/builtin_ops.hh"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

#include "core/log.hh"
#include "anim/motion_path.hh"
#include "data/document.hh"
#include "io/pdf_export.hh"
#include "io/svg_export.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "python/py_runtime.hh"
#include "wm/context.hh"
#include "wm/file_dialog.hh"

namespace oanim {

/* An export's file: the "filepath" argument (its extension added when missing), or, with a window, asked for in a
 * save dialog that runs `idname` again with it (false then: nothing yet). */
static bool export_path(Context &C, const char *idname, const char *filter_name, const char *extension,
                        const std::string &default_name, std::string *r_path)
{
  std::string path = op_arg(C, "filepath");
  if (path.empty()) {
    if (C.wm->background) {
      report(ReportType::Error, std::string(idname) + " needs a filepath without a window");
      return false;
    }
    Context *ctx = &C;
    OpArgs args = C.op_args ? *C.op_args : OpArgs{};
    const std::string op = idname;
    file_dialog_save(filter_name, extension, default_name, [ctx, args, op](const std::string &chosen) mutable {
      args["filepath"] = chosen;
      operator_call(*ctx, op, args);
    });
    return false;
  }
  if (std::filesystem::path(std::u8string(path.begin(), path.end())).extension() != std::string(".") + extension) {
    path += std::string(".") + extension;
  }
  *r_path = path;
  return true;
}


/* What undo leaves alone: the playhead (as in Blender and Moho), text blocks, so undoing a
 * drawing change never loses typed code, and what is shown (overlays, Motion Paths: the Motion Path tool turns
 * them on without an undo step, so undoing a stroke would hide the path it was made on). The action being edited is undone with the rest: its
 * keys are swapped into the channels, so the two always go together. */
struct ViewState {
  int frame_current;
  std::vector<Text> texts;
  ViewportOverlays overlays;
  std::map<std::string, bool> ui_open;
  ViewportView view;
  bool motion_paths;
  /* The timeline header's switches are how to edit, not what was made (as Blender's Auto Keying). */
  bool auto_key;
  AutoKeyMode auto_key_mode;
  bool time_together;
};

static ViewState view_state_get(const Document &doc)
{
  return {doc.frame_current, doc.texts,     doc.overlays,      doc.ui_open,
          doc.view,          doc.motion_paths, doc.auto_key, doc.auto_key_mode, doc.time_together};
}

static void view_state_restore(Document &doc, ViewState &view)
{
  doc.frame_current = view.frame_current;
  doc.texts = std::move(view.texts);
  doc.overlays = view.overlays;
  doc.ui_open = std::move(view.ui_open);
  doc.view = view.view;
  doc.motion_paths = view.motion_paths;
  doc.auto_key = view.auto_key;
  doc.auto_key_mode = view.auto_key_mode;
  doc.time_together = view.time_together;
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
    wm.play_range_first = wm.play_range_last = 0;
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

  /* Alt Space: loop over just the motion paths' range, taken as playing starts so it holds still. */
  frame_operator_register(
      "screen.animation_play_path_range", "Play Path Range",
      "Play over and over just the frames the motion paths cover (the keys before and after this frame), or pause",
      [](Context &C) {
        WindowManager &wm = *C.wm;
        Document &doc = *C.document;
        if (wm.playing) {
          wm.playing = false;
          return OpStatus::Finished;
        }
        int first = 0, last = 0;
        if (!motion_path_range(doc, motion_path_targets(C), first, last) || last <= first) {
          report(ReportType::Warning, "No range to play: select something with keys around this frame");
          return OpStatus::Cancelled;
        }
        wm.play_start_frame = doc.frame_current;
        wm.play_range_first = first;
        wm.play_range_last = last;
        doc.frame_current = first;
        wm.playing = true;
        wm.play_reverse = false;
        wm.play_last_ns = 0;
        wm.play_frames_due = 0.0;
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
    OperatorType ot;
    ot.idname = "view.local_view";
    ot.name = "Local View";
    ot.description = "Show only the picked layers (and what's inside them) in the viewport, as Blender's Local "
                     "View, leaving the view where it is; again to show every layer (/ or Numpad /)";
    ot.poll = [](Context &C) { return C.wm != nullptr && C.document != nullptr; };
    ot.exec = [](Context &C) {
      WindowManager &wm = *C.wm;
      Document &doc = *C.document;
      if (!wm.local_view.empty()) {
        wm.local_view.clear();
        return OpStatus::Finished;
      }
      /* The picked layers; a layer inside a picked group comes with it anyway. */
      for (const Layer *l : document_layers_flat(doc)) {
        if (l->selected || l->id == doc.active_layer) {
          wm.local_view.push_back(l->id);
        }
      }
      if (wm.local_view.empty()) {
        report(ReportType::Info, "Pick the layers to show on their own");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "view.media_view";
    ot.name = "Media View";
    ot.description = "Show every media layer at full opacity (guides too) with the viewport's overlays hidden, "
                     "to see the footage clearly; again to go back (`). Changes no layer and keys nothing";
    ot.poll = [](Context &C) { return C.wm != nullptr && C.document != nullptr; };
    ot.exec = [](Context &C) {
      C.wm->media_view = !C.wm->media_view;
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
  {
    /* The current frame through the camera as vectors (io/svg_export.hh). */
    OperatorType ot;
    ot.idname = "export.svg";
    ot.name = "Export SVG";
    ot.description = "Save the current frame through the camera as an SVG file, at the document's size: fills, lines "
                     "and masks as vectors, media and brushed lines as pictures";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.wm != nullptr && C.document != nullptr; };
    ot.exec = [](Context &C) {
      char name[32];
      std::snprintf(name, sizeof(name), "_%04d.svg", C.document->frame_current);
      std::string path;
      if (!export_path(C, "export.svg", "SVG images", "svg", C.document->name + name, &path)) {
        return OpStatus::Cancelled;
      }
      SvgExport options;
      options.width = op_arg_int(C, "width", 0);
      options.height = op_arg_int(C, "height", 0);
      options.transparent = op_arg_bool(C, "transparent");
      std::string error;
      SvgExportReport made;
      if (!svg_export_frame(*C.document, path, options, &error, &made)) {
        report(ReportType::Error, error);
        return OpStatus::Cancelled;
      }
      std::string text = "Exported frame " + std::to_string(C.document->frame_current) + " as SVG to " + path + " (" +
                         std::to_string(made.fills) + " fills, " + std::to_string(made.lines) + " lines, " +
                         std::to_string(made.masks) + " masks";
      if (made.brushed > 0) {
        text += ", " + std::to_string(made.brushed) + " brushed lines as pictures";
      }
      if (made.pictures > 0) {
        text += ", " + std::to_string(made.pictures) + " media pictures";
      }
      report(ReportType::Info, text + ")");
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* Frames through the camera as vector pages, every layer a PDF layer (io/pdf_export.hh). */
    OperatorType ot;
    ot.idname = "export.pdf";
    ot.name = "Export PDF";
    ot.description = "Save frames through the camera as a PDF, a page a frame (frames: CURRENT, or RANGE for the "
                     "scene's frame range), every layer a PDF layer that can be switched off and on (hidden layers "
                     "and guides in it too, switched off)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.wm != nullptr && C.document != nullptr; };
    ot.exec = [](Context &C) {
      const Document &doc = *C.document;
      const bool range = op_arg(C, "frames", "CURRENT") == "RANGE";
      const int first = op_arg_int(C, "frame_start", doc.frame_start);
      const int last = std::max(op_arg_int(C, "frame_end", doc.frame_end), first);
      char name[48];
      if (range) {
        std::snprintf(name, sizeof(name), "_%04d-%04d.pdf", first, last);
      }
      else {
        std::snprintf(name, sizeof(name), "_%04d.pdf", doc.frame_current);
      }
      std::string path;
      if (!export_path(C, "export.pdf", "PDF documents", "pdf", doc.name + name, &path)) {
        return OpStatus::Cancelled;
      }
      PdfExport options;
      options.width = op_arg_int(C, "width", 0);
      options.height = op_arg_int(C, "height", 0);
      options.transparent = op_arg_bool(C, "transparent");
      if (range) {
        options.frame_first = first;
        options.frame_last = last;
      }
      std::string error;
      PdfExportReport made;
      if (!pdf_export(doc, path, options, &error, &made)) {
        report(ReportType::Error, error);
        return OpStatus::Cancelled;
      }
      std::string text = "Exported " + std::to_string(made.pages) + (made.pages == 1 ? " page" : " pages") +
                         " as PDF to " + path + " (" + std::to_string(made.layers) + " layers, " +
                         std::to_string(made.masks) + " masks";
      if (made.brushed > 0) {
        text += ", " + std::to_string(made.brushed) + " brushed lines as pictures";
      }
      if (made.pictures > 0) {
        text += ", " + std::to_string(made.pictures) + " media pictures";
      }
      char size[32];
      std::snprintf(size, sizeof(size), "; %.1f MB", double(made.bytes) / 1e6);
      report(ReportType::Info, text + ")" + size);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace oanim
