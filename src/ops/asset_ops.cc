/* Operators for the asset library (Preferences) and for giving shapes a brush. */

#include <string>

#include "assets/asset_library.hh"
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
}

}  // namespace oanim
