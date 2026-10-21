/* Operators for the asset library (Preferences), for giving shapes a brush and for brush presets. */

#include <string>

#include "assets/asset_library.hh"
#include "assets/brush_presets.hh"
#include "assets/brushes.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "wm/file_dialog.hh"

namespace oanim {

int shapes_set_brush(Context &C, const BrushSettings &brush)
{
  int changed = 0;
  for (Layer *layer : editable_layers(C)) {
    VectorDrawing &drawing = layer->drawing;
    const std::vector<bool> selected = drawing_selected_shapes(drawing);
    for (size_t s = 0; s < drawing.shapes.size(); s++) {
      if (selected[s] && drawing.shapes[s].style.brush != brush) {
        drawing.shapes[s].style.brush = brush;
        changed++;
      }
    }
  }
  return changed;
}

bool shown_shape_brush(Context &C, BrushSettings *r_brush)
{
  const Document &doc = *C.document;
  const std::vector<Layer *> layers = editable_layers(C);
  for (const Layer *layer : layers) {
    if (layer->id != doc.active_shape_layer) {
      continue;
    }
    const int s = drawing_shape_index(layer->drawing, doc.active_shape);
    if (s >= 0 && drawing_selected_shapes(layer->drawing)[size_t(s)]) {
      *r_brush = layer->drawing.shapes[size_t(s)].style.brush;
      return true;
    }
  }
  for (const Layer *layer : layers) {
    const std::vector<bool> selected = drawing_selected_shapes(layer->drawing);
    for (int s = int(selected.size()) - 1; s >= 0; s--) {
      if (selected[size_t(s)]) {
        *r_brush = layer->drawing.shapes[size_t(s)].style.brush;
        return true;
      }
    }
  }
  return false;
}

/* The brush a preset operator works on: the Draw tool's (target TOOL) or the selected shapes'. */
static bool target_is_tool(Context &C)
{
  return op_arg(C, "target") == "TOOL";
}

static void asset_operator_add(const char *idname, const char *name, const char *description, unsigned flag,
                               std::function<OpStatus(Context &C)> exec)
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = flag;
  ot.exec = std::move(exec);
  operators().add(std::move(ot));
}

void asset_operators_register()
{
  asset_operator_add("preferences.asset_library_set", "Set Asset Library",
                     "Choose the folder that holds Open Animate's brushes, and set it up there", OPTYPE_REGISTER,
                     [](Context &C) {
                       const std::string directory = op_arg(C, "directory");
                       if (directory.empty()) {
                         if (C.wm->background) {
                           report(ReportType::Error, "preferences.asset_library_set needs a directory");
                           return OpStatus::Cancelled;
                         }
                         Context *ctx = &C;
                         file_dialog_folder([ctx](const std::string &path) {
                           operator_call(*ctx, "preferences.asset_library_set", {{"directory", path}});
                         });
                         return OpStatus::Cancelled;
                       }
                       return asset_library_set(directory) ? OpStatus::Finished : OpStatus::Cancelled;
                     });

  asset_operator_add("preferences.import_moho_brushes", "Import Moho Brushes",
                     "Copy Moho's brushes into the asset library (from Moho's install and custom content, or "
                     "from `directory`)",
                     OPTYPE_REGISTER, [](Context &C) {
                       const std::string directory = op_arg(C, "directory");
                       const std::vector<std::string> dirs = directory.empty() ? moho_brush_dirs()
                                                                               : std::vector<std::string>{directory};
                       if (dirs.empty()) {
                         report(ReportType::Warning, "No Moho brushes found on this computer");
                         return OpStatus::Cancelled;
                       }
                       return asset_library_import_moho_brushes(dirs) >= 0 ? OpStatus::Finished
                                                                           : OpStatus::Cancelled;
                     });

  asset_operator_add("preferences.brushes_reload", "Reload Brushes",
                     "Read the asset library's Brushes folder again", OPTYPE_REGISTER, [](Context &) {
                       brushes_reload();
                       return OpStatus::Finished;
                     });

  asset_operator_add("mesh.brush_set", "Set Brush",
                     "Paint the selected shapes' lines with a brush from the asset library, using its settings "
                     "(an empty name gives plain lines)",
                     OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                       const std::string name = op_arg(C, "brush");
                       BrushSettings settings;
                       if (!name.empty()) {
                         const Brush *brush = brush_find(name);
                         if (!brush) {
                           report(ReportType::Error, "No brush named '" + name + "' in the asset library");
                           return OpStatus::Cancelled;
                         }
                         settings = brush->defaults;
                         settings.name = name;
                       }
                       if (shapes_set_brush(C, settings) == 0) {
                         report(ReportType::Warning, "Select a point of a shape to give it a brush");
                         return OpStatus::Cancelled;
                       }
                       return OpStatus::Finished;
                     });

  /* Brush presets (assets/brush_presets.hh). */
  asset_operator_add("brush.preset_add", "Add Brush Preset",
                     "Keep the brush as a preset in this file (`name`; a taken name gets a number), with a copy "
                     "of its shape",
                     OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                       BrushSettings brush;
                       if (target_is_tool(C)) {
                         brush = C.wm->tool_settings.style.brush;
                       }
                       else if (!shown_shape_brush(C, &brush)) {
                         report(ReportType::Warning, "Select a shape to keep its brush as a preset");
                         return OpStatus::Cancelled;
                       }
                       std::string name = op_arg(C, "name");
                       if (name.empty()) {
                         name = brush.name.empty() ? std::string("Solid") : brush.name;
                       }
                       const BrushPreset &added = document_brush_preset_add(*C.document, name, brush);
                       report(ReportType::Info, "Added brush preset " + added.name);
                       brush_presets_register_shapes(*C.document);
                       return OpStatus::Finished;
                     });

  asset_operator_add("brush.preset_load", "Load Brush Preset",
                     "Give the selected shapes (or the Draw tool, target TOOL) a preset's brush: a copy of its "
                     "settings, nothing stays linked. `library` 1 takes it from the asset library, else from "
                     "this file",
                     OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                       const std::string name = op_arg(C, "preset");
                       const bool library = op_arg(C, "library") == "1";
                       const BrushPreset *preset = library ? library_brush_preset_find(name)
                                                           : document_brush_preset_find(*C.document, name);
                       if (!preset) {
                         report(ReportType::Error, std::string("No brush preset named '") + name + "' in " +
                                                       (library ? "the asset library" : "this file"));
                         return OpStatus::Cancelled;
                       }
                       const bool tool = target_is_tool(C);
                       BrushSettings current;
                       if (tool) {
                         current = C.wm->tool_settings.style.brush;
                       }
                       else if (!shown_shape_brush(C, &current)) {
                         report(ReportType::Warning, "Select a shape to give it the preset's brush");
                         return OpStatus::Cancelled;
                       }
                       BrushSettings settings = preset->settings;
                       const bool missing = !settings.name.empty() && !brush_find(settings.name);
                       if (missing) {
                         /* The rest of the preset still applies; the shape stays what it was. */
                         settings.name = current.name;
                         report(ReportType::Warning, "Preset " + preset->name + ": its shape \"" +
                                                         preset->settings.name +
                                                         "\" isn't in the asset library, so the shape stayed");
                       }
                       if (tool) {
                         C.wm->tool_settings.style.brush = settings;
                       }
                       else {
                         shapes_set_brush(C, settings);
                       }
                       return OpStatus::Finished;
                     });

  asset_operator_add("brush.preset_remove", "Remove Brush Preset",
                     "Delete a preset (`library` 1: its file in the asset library, which can't be undone). What "
                     "used it keeps its brush",
                     OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                       const std::string name = op_arg(C, "preset");
                       if (op_arg(C, "library") == "1") {
                         std::string error;
                         if (!library_brush_preset_remove(name, &error)) {
                           report(ReportType::Error, error);
                           return OpStatus::Cancelled;
                         }
                         report(ReportType::Info, "Deleted brush preset " + name + " from the asset library");
                         /* Nothing in the file changed: no undo step. */
                         return OpStatus::Cancelled;
                       }
                       std::vector<BrushPreset> &presets = C.document->brush_presets;
                       const size_t before = presets.size();
                       std::erase_if(presets, [&](const BrushPreset &p) { return p.name == name; });
                       if (presets.size() == before) {
                         report(ReportType::Error, "No brush preset named '" + name + "' in this file");
                         return OpStatus::Cancelled;
                       }
                       brush_presets_register_shapes(*C.document);
                       return OpStatus::Finished;
                     });

  asset_operator_add("brush.preset_save_to_library", "Save Brush Preset to Library",
                     "Copy a file preset into the asset library, for every project (its shape too, when the "
                     "library lacks it; taken names get a number)",
                     OPTYPE_REGISTER, [](Context &C) {
                       const std::string name = op_arg(C, "preset");
                       const BrushPreset *preset = document_brush_preset_find(*C.document, name);
                       if (!preset) {
                         report(ReportType::Error, "No brush preset named '" + name + "' in this file");
                         return OpStatus::Cancelled;
                       }
                       std::string error;
                       const std::string saved = library_brush_preset_save(*preset, &error);
                       if (saved.empty()) {
                         report(ReportType::Error, error);
                         return OpStatus::Cancelled;
                       }
                       report(ReportType::Info, "Saved brush preset " + saved + " to the asset library");
                       return OpStatus::Finished;
                     });
}

}  // namespace oanim
