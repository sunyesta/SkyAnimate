/* Operators for the asset library (Preferences), for giving shapes a brush and for brush presets. */

#include <cstdlib>
#include <string>

#include "anim/deform.hh"
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

Shape *shown_shape(Context &C)
{
  const Document &doc = *C.document;
  const std::vector<Layer *> layers = editable_layers(C);
  for (Layer *layer : layers) {
    if (layer->id != doc.active_shape_layer) {
      continue;
    }
    const int s = drawing_shape_index(layer->drawing, doc.active_shape);
    if (s >= 0 && drawing_selected_shapes(layer->drawing)[size_t(s)]) {
      return &layer->drawing.shapes[size_t(s)];
    }
  }
  for (Layer *layer : layers) {
    const std::vector<bool> selected = drawing_selected_shapes(layer->drawing);
    for (int s = int(selected.size()) - 1; s >= 0; s--) {
      if (selected[size_t(s)]) {
        return &layer->drawing.shapes[size_t(s)];
      }
    }
  }
  return nullptr;
}

bool shown_shape_brush(Context &C, BrushSettings *r_brush)
{
  if (const Shape *shape = shown_shape(C)) {
    *r_brush = shape->style.brush;
    return true;
  }
  return false;
}

int shapes_change_brush(Context &C, const BrushSettings &before, const BrushSettings &after)
{
  int changed = 0;
  for (Layer *layer : editable_layers(C)) {
    VectorDrawing &drawing = layer->drawing;
    const std::vector<bool> selected = drawing_selected_shapes(drawing);
    for (size_t s = 0; s < drawing.shapes.size(); s++) {
      if (selected[s]) {
        BrushSettings &b = drawing.shapes[s].style.brush;
        const BrushSettings was = b;
        brush_settings_apply_changes(before, after, b);
        changed += b == was ? 0 : 1;
      }
    }
  }
  return changed;
}

/* One setting from its text (a typed operator argument). */
static bool setting_parse(const std::string &text, std::string &r) { r = text; return true; }
static bool setting_parse(const std::string &text, bool &r)
{
  r = text == "1" || text == "True" || text == "true";
  return r || text == "0" || text == "False" || text == "false";
}
static bool setting_parse(const std::string &text, float &r)
{
  char *end = nullptr;
  r = std::strtof(text.c_str(), &end);
  return end && end != text.c_str();
}
static bool setting_parse(const std::string &text, int &r)
{
  char *end = nullptr;
  r = int(std::strtol(text.c_str(), &end, 10));
  return end && end != text.c_str();
}

/* The brush a preset operator works on: the Draw tool's (target TOOL) or the selected shapes'. */
static bool target_is_tool(Context &C)
{
  return op_arg(C, "target") == "TOOL";
}

/* mesh.style_copy_from: one look setting of a shape (`what`: FILL, LINE, WIDTH or BRUSH) onto the
 * selected shapes or the Draw tool. */
static OpStatus style_copy_from(Context &C, const std::string &what)
{
  const Layer *layer = layer_find(*C.document, std::strtoull(op_arg(C, "layer").c_str(), nullptr, 10));
  const int index = std::atoi(op_arg(C, "shape").c_str());
  if (what != "FILL" && what != "LINE" && what != "WIDTH" && what != "BRUSH") {
    report(ReportType::Error, "`what` is FILL, LINE, WIDTH or BRUSH, not '" + what + "'");
    return OpStatus::Cancelled;
  }
  if (!layer || !layer->has_drawing()) {
    report(ReportType::Error, "No drawing layer to take it from");
    return OpStatus::Cancelled;
  }
  const bool modified = layer_has_modifier_effect(*C.document, *layer, false);
  const VectorDrawing shown = modified ? layer_evaluated_drawing(*C.document, *layer, false)
                                       : layer_drawing(*C.document, *layer);
  if (index < 0 || index >= int(shown.shapes.size())) {
    report(ReportType::Error, "No shape " + op_arg(C, "shape") + " in " + layer->name);
    return OpStatus::Cancelled;
  }
  const Style &from = shown.shapes[size_t(index)].style;
  const auto take = [&](Style &to) {
    if (what == "FILL") {
      to.fill = from.fill;
    }
    else if (what == "LINE") {
      to.line = from.line;
    }
    else if (what == "WIDTH") {
      to.line_width = from.line_width;
    }
    else {
      to.brush = from.brush;
    }
  };
  if (target_is_tool(C)) {
    take(C.wm->tool_settings.style);
  }
  else {
    int shapes = 0;
    for (Layer *l : editable_layers(C)) {
      const std::vector<bool> selected = drawing_selected_shapes(l->drawing);
      for (size_t s = 0; s < selected.size(); s++) {
        if (selected[s]) {
          take(l->drawing.shapes[s].style);
          shapes++;
        }
      }
    }
    if (shapes == 0) {
      report(ReportType::Warning, "Select a shape to give it that");
      return OpStatus::Cancelled;
    }
  }
  const Shape &source = shown.shapes[size_t(index)];
  const char *names[] = {"fill colour", "line colour", "line width", "brush"};
  const int n = what == "FILL" ? 0 : what == "LINE" ? 1 : what == "WIDTH" ? 2 : 3;
  report(ReportType::Info, std::string("Took the ") + names[n] + " of " +
                               (source.name.empty() ? std::string("a shape") : source.name) + " in " + layer->name);
  return OpStatus::Finished;
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

  asset_operator_add("preferences.brushes_reload", "Reload Brushes",
                     "Read the asset library's Brush Shapes and Brush Presets folders again", OPTYPE_REGISTER,
                     [](Context &) {
                       brushes_reload();
                       return OpStatus::Finished;
                     });

  asset_operator_add("mesh.brush_set", "Set Brush Shape",
                     "Paint the selected shapes' lines with a brush shape (`brush`; an empty name gives plain "
                     "lines); their other brush settings stay as they are",
                     OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_MIRROR, [](Context &C) {
                       const std::string name = op_arg(C, "brush");
                       if (!name.empty() && !brush_find(name)) {
                         report(ReportType::Error, "No brush shape named '" + name + "' in the asset library");
                         return OpStatus::Cancelled;
                       }
                       int shapes = 0;
                       for (Layer *layer : editable_layers(C)) {
                         const std::vector<bool> selected = drawing_selected_shapes(layer->drawing);
                         for (size_t s = 0; s < selected.size(); s++) {
                           if (selected[s]) {
                             layer->drawing.shapes[s].style.brush.name = name;
                             shapes++;
                           }
                         }
                       }
                       if (shapes == 0) {
                         report(ReportType::Warning, "Select a point of a shape to give it a brush");
                         return OpStatus::Cancelled;
                       }
                       return OpStatus::Finished;
                     });

  /* Brush presets (assets/brush_presets.hh). */
  asset_operator_add("brush.preset_add", "Add Brush Preset",
                     "Keep the brush as a preset (`name`; a taken name gets a number): in the project, with a copy "
                     "of its shape, or with `library` 1 in the asset library, in `category` (a folder of Brushes, "
                     "made when new; My Brushes when none is named)",
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
                       std::string added_name;
                       if (op_arg(C, "library") == "1") {
                         BrushPreset preset;
                         preset.name = name;
                         preset.settings = brush_settings_unnamed(brush);
                         if (!brush.name.empty()) {
                           brush_shape_bytes(brush.name, preset.shape_files, preset.shape_data);
                         }
                         std::string error;
                         bool copied = false;
                         added_name = library_brush_preset_save(preset, op_arg(C, "category"), &error, &copied);
                         if (added_name.empty()) {
                           report(ReportType::Error, error);
                           return OpStatus::Cancelled;
                         }
                         const BrushPreset *saved = library_brush_preset_find(added_name);
                         report(ReportType::Info, "Saved brush " + added_name + " to Library / " +
                                                      (saved ? saved->group : std::string()) +
                                                      (copied ? " (its shape copied into Brush Shapes)" : ""));
                       }
                       else {
                         added_name = document_brush_preset_add(*C.document, name, brush_settings_unnamed(brush)).name;
                         report(ReportType::Info, "Added brush preset " + added_name + " to the project");
                       }
                       const std::string added = added_name;
                       /* The brush is now that preset's. */
                       BrushSettings named = brush;
                       named.preset = added;
                       if (target_is_tool(C)) {
                         C.wm->tool_settings.style.brush.preset = added;
                       }
                       else {
                         shapes_change_brush(C, brush, named);
                       }
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
                       settings.preset = preset->name;
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

  asset_operator_add("brush.setting_set", "Set Brush Setting",
                     "Change one brush setting (`setting`, as oanim's brush dicts name it, e.g. spacing or "
                     "sizeJitter, to `value`) on the selected shapes, or the Draw tool's (target TOOL); their "
                     "other settings stay as they are",
                     OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                       const std::string key = op_arg(C, "setting"), value = op_arg(C, "value");
                       /* The setting read into `b`: 0 no such setting, 1 unreadable, 2 set. */
                       const auto set_on = [&](BrushSettings &b) {
                         int result = 0;
#define OANIM_BRUSH_SET(type, field, name)   if (key == name || key == #field) {     result = setting_parse(value, b.field) ? 2 : 1;   }
                         OANIM_BRUSH_SETTINGS(OANIM_BRUSH_SET)
#undef OANIM_BRUSH_SET
                         return result;
                       };
                       BrushSettings probe;
                       const int known = set_on(probe);
                       if (known != 2) {
                         report(ReportType::Error, known == 1 ? "Can't read '" + value + "' for " + key
                                                              : "No brush setting called '" + key + "'");
                         return OpStatus::Cancelled;
                       }
                       if (target_is_tool(C)) {
                         set_on(C.wm->tool_settings.style.brush);
                         return OpStatus::Finished;
                       }
                       int shapes = 0;
                       for (Layer *layer : editable_layers(C)) {
                         const std::vector<bool> selected = drawing_selected_shapes(layer->drawing);
                         for (size_t s = 0; s < selected.size(); s++) {
                           if (selected[s]) {
                             set_on(layer->drawing.shapes[s].style.brush);
                             shapes++;
                           }
                         }
                       }
                       if (shapes == 0) {
                         report(ReportType::Warning, "Select a shape to change its brush");
                         return OpStatus::Cancelled;
                       }
                       return OpStatus::Finished;
                     });

  asset_operator_add("mesh.style_copy_from", "Copy Look From Shape",
                     "Give the selected shapes (or the Draw tool, target TOOL) one look setting of shape `shape` of "
                     "layer `layer` (its index in the drawing as shown): `what` FILL (fill colour), LINE (line "
                     "colour), WIDTH (line width) or BRUSH (the whole brush). The Material panel's eyedroppers",
                     OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) { return style_copy_from(C, op_arg(C, "what")); });
  asset_operator_add("brush.copy_from", "Copy Brush From Shape",
                     "Give the selected shapes (or the Draw tool, target TOOL) the whole brush of shape `shape` of "
                     "layer `layer` (its index in the drawing as shown): the brush eyedropper",
                     OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) { return style_copy_from(C, "BRUSH"); });

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
                     "Copy a project preset into the asset library, for every project, in `category` (a folder of "
                     "Brushes, made when new; My Brushes when none is named). Its shape goes too when the library "
                     "lacks it; taken names get a number",
                     OPTYPE_REGISTER, [](Context &C) {
                       const std::string name = op_arg(C, "preset");
                       const BrushPreset *preset = document_brush_preset_find(*C.document, name);
                       if (!preset) {
                         report(ReportType::Error, "No brush preset named '" + name + "' in the project");
                         return OpStatus::Cancelled;
                       }
                       std::string error;
                       bool copied = false;
                       const std::string saved = library_brush_preset_save(*preset, op_arg(C, "category"), &error, &copied);
                       if (saved.empty()) {
                         report(ReportType::Error, error);
                         return OpStatus::Cancelled;
                       }
                       const BrushPreset *in_library = library_brush_preset_find(saved);
                       report(ReportType::Info, "Saved brush " + saved + " to Library / " +
                                                    (in_library ? in_library->group : std::string()) +
                                                    (copied ? " (its shape copied into Brush Shapes)" : ""));
                       return OpStatus::Finished;
                     });
}

}  // namespace oanim
