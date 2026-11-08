/* The Sculpt tool's operators (anim/sculpt.hh, assets/sculpt_presets.hh): a scripted stroke, picking
 * a brush or preset, saving and deleting presets, and F / Shift F to size the brush. */

#include <sstream>
#include <string>
#include <vector>

#include "anim/animation.hh"
#include "anim/keep_poses.hh"
#include "anim/sculpt.hh"
#include "assets/sculpt_presets.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

/* Picks a preset, as clicking it on the shelf does: the brush going keeps what it was changed to (for coming
 * back to), and the one coming takes what it was last changed to, else its saved settings. */
static void preset_apply(ToolSettings &ts, const SculptPreset &p)
{
  if (!ts.sculpt_preset.empty()) {
    ts.sculpt_changes[ts.sculpt_preset] = {ts.sculpt_radius, ts.sculpt_strength, ts.sculpt_falloff};
  }
  ts.sculpt_preset = p.name;
  ts.sculpt_brush = p.brush;
  ts.sculpt_radius = p.radius;
  ts.sculpt_strength = p.strength;
  ts.sculpt_falloff = p.falloff;
  if (const auto it = ts.sculpt_changes.find(p.name); it != ts.sculpt_changes.end()) {
    ts.sculpt_radius = it->second.radius;
    ts.sculpt_strength = it->second.strength;
    ts.sculpt_falloff = it->second.falloff;
  }
}

/* "x,y;x,y;...": the stroke's path in document space. */
static std::vector<float2> parse_path(const std::string &text)
{
  std::vector<float2> path;
  std::stringstream points(text);
  std::string item;
  while (std::getline(points, item, ';')) {
    float2 p;
    char comma = 0;
    std::stringstream xy(item);
    if (xy >> p.x >> comma >> p.y && comma == ',') {
      path.push_back(p);
    }
  }
  return path;
}

static OpStatus stroke_exec(Context &C)
{
  Document &doc = *C.document;
  const ToolSettings &ts = C.wm->tool_settings;
  SculptDab dab;
  dab.brush = ts.sculpt_brush;
  if (op_has_arg(C, "brush") && !sculpt_brush_from_id(op_arg(C, "brush"), &dab.brush)) {
    report(ReportType::Error, "brush= is one of GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST, SURFACE_SMOOTH");
    return OpStatus::Cancelled;
  }
  dab.falloff = ts.sculpt_falloff;
  if (op_has_arg(C, "falloff") && !sculpt_falloff_from_id(op_arg(C, "falloff"), &dab.falloff)) {
    report(ReportType::Error, "falloff= is one of SMOOTH, SPHERE, CONSTANT");
    return OpStatus::Cancelled;
  }
  dab.radius = std::max(op_arg_float(C, "radius", 0.1f), 1e-6f);
  dab.strength = op_arg_float(C, "strength", ts.sculpt_strength);
  dab.amount = op_arg_float(C, "amount", 1.0f);
  dab.invert = op_arg_bool(C, "invert");
  dab.selected_only = op_has_arg(C, "selected_only") ? op_arg_bool(C, "selected_only") : ts.sculpt_selected_only;
  dab.mirror_x = op_has_arg(C, "mirror") ? op_arg_bool(C, "mirror") : ts.mirror_x;
  dab.surface_sensitivity = op_has_arg(C, "sensitivity") ? op_arg_float(C, "sensitivity", 0.25f)
                                                         : ts.sculpt_surface_sensitivity;
  /* Dyntopo: resolution= points per unit of each layer's own space (0 or none: off), decimate= taking out the points
   * the line doesn't need when the stroke ends. */
  dab.resolution = std::max(op_arg_float(C, "resolution", 0.0f), 0.0f);
  dab.decimate = op_has_arg(C, "decimate") ? op_arg_bool(C, "decimate") : ts.sculpt_decimate;
  const std::vector<float2> path = parse_path(op_arg(C, "path"));
  if (path.empty()) {
    report(ReportType::Error, "path= gives the stroke's points: \"x,y;x,y;...\" (document space)");
    return OpStatus::Cancelled;
  }
  std::vector<uint64_t> ids;
  for (const Layer *l : editable_layers(C)) {
    ids.push_back(l->id);
  }
  const SetupSnapshot kept = doc.frame_current == 0 && op_arg_bool(C, "keep_poses") ? setup_snapshot(doc, ids)
                                                                                     : SetupSnapshot();
  dab.center = path.front();
  SculptStroke stroke = sculpt_stroke_begin(doc, ids, dab);
  int changed = 0;
  for (size_t i = 0; i < path.size(); i++) {
    dab.move = i > 0 ? path[i] - path[i - 1] : float2();
    /* Grab keeps to the line it took; the others work where the brush is now. */
    dab.center = path[i];
    if ((dab.brush == SculptBrush::Grab || dab.brush == SculptBrush::Push) && i == 0) {
      continue;
    }
    changed += sculpt_stroke_dab(doc, stroke, dab);
  }
  if (changed > 0) {
    changed += sculpt_stroke_end(doc, stroke, dab);
  }
  if (!kept.empty()) {
    animation_record(doc);
    keep_poses(doc, kept);
  }
  return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
}

void sculpt_operators_register()
{
  const auto add = [](const char *idname, const char *name, const char *description, unsigned flag,
                      std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = description;
    ot.flag = flag;
    ot.poll = std::move(poll);
    ot.exec = std::move(exec);
    operators().add(std::move(ot));
  };
  const auto has_drawing = [](Context &C) { return C.document && !editable_layers(C).empty(); };
  const auto sculpt_tool = [](Context &C) { return C.wm->tool_settings.tool == Tool::Sculpt; };

  add("sculpt.stroke", "Sculpt",
      "A sculpt stroke along path=\"x,y;x,y;...\" (document space) over the drawings being edited, as the Sculpt "
      "tool makes one: brush= GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST or SURFACE_SMOOTH (default: the tool's), radius= in "
      "document units, strength=, falloff= SMOOTH, SPHERE or CONSTANT, invert=1 (Ctrl), selected_only=1, "
      "mirror=1 (X mirror), amount= how much of a full step each point of the path is for the brushes that work "
      "on while held (1), sensitivity= Surface Smooth's Lump Sensitivity, 0-1 (default: the tool's), "
      "keep_poses=1 on frame 0, resolution= turns Dyntopo on (the line resampled at the stroke's own "
      "samples where it moves it, on drawings without animation; none: off) and is its Resolution, points per unit of "
      "each layer's own space, decimate=1 taking out the points the line doesn't need when the stroke ends, to within a "
      "tenth of a Resolution step (default: the tool's)",
      OPTYPE_REGISTER | OPTYPE_UNDO, has_drawing, stroke_exec);

  add("sculpt.simplify", "Simplify",
      "Take out every point the drawings being edited keep their shape without, to within a tenth of a step of "
      "resolution= (points per unit of each layer's own space; default: Dyntopo's), as Dyntopo's Decimate does after a "
      "stroke: never an end, a corner, a point two curves share or one with weights of its own. Only on drawings "
      "without animation", OPTYPE_REGISTER | OPTYPE_UNDO, has_drawing,
      [](Context &C) {
        const float resolution = op_arg_float(C, "resolution", C.wm->tool_settings.sculpt_resolution);
        if (!(resolution > 0.0f)) {
          report(ReportType::Error, "resolution= is points per unit of the layer's own space (more than 0)");
          return OpStatus::Cancelled;
        }
        char used[32];
        std::snprintf(used, sizeof(used), "%g", double(resolution));
        op_arg_used(C, "resolution", used);
        std::vector<uint64_t> ids;
        std::string blocked;
        for (const Layer *l : editable_layers(C)) {
          const std::string why = sculpt_dyntopo_blocked(*C.document, l->id, false);
          if (why.empty()) {
            ids.push_back(l->id);
          }
          else if (blocked.empty()) {
            blocked = l->name + ": " + why;
          }
        }
        if (ids.empty()) {
          report(ReportType::Warning, !blocked.empty() ? "Nothing simplified (" + blocked + ")"
                                                       : std::string("Nothing to simplify"));
          return OpStatus::Cancelled;
        }
        /* Finished even when nothing goes at this Resolution, so dragging it finer in the Adjust Last Operation panel
         * puts back the points a coarser one took out. */
        const int gone = sculpt_simplify(*C.document, ids, resolution);
        const std::string count = std::to_string(gone) + (gone == 1 ? " point" : " points") + " removed";
        op_note(count);
        report(ReportType::Info, "Simplify: " + count);
        return OpStatus::Finished;
      });
  {
    OperatorType &ot = const_cast<OperatorType &>(*operators().find("sculpt.simplify"));
    ot.props = {op_float_property("resolution", "Resolution",
                                  "Points per unit of the layer's own space: how closely the line is kept, to a tenth "
                                  "of a step of 1/Resolution. Lower takes out more",
                                  "50", 1.0f, 500.0f, 0.5f)};
  }

  add("mesh.decimate", "Decimate Curve",
      "Take out the selected points the line changes least without, one at a time, until ratio= (0-1; default: the "
      "one last used) of each curve's selected points is left (Blender's Decimate Curve): never an end, a corner, a "
      "point two curves share or one with weights of its own, so fewer may go. Only on drawings without animation",
      OPTYPE_REGISTER | OPTYPE_UNDO, has_drawing, [](Context &C) {
        ToolSettings &ts = C.wm->tool_settings;
        const float ratio = op_arg_float(C, "ratio", ts.decimate_ratio);
        if (!(ratio >= 0.0f && ratio <= 1.0f)) {
          report(ReportType::Error, "ratio= is the share of the selected points to keep, 0 to 1");
          return OpStatus::Cancelled;
        }
        char used[32];
        std::snprintf(used, sizeof(used), "%g", double(ratio));
        op_arg_used(C, "ratio", used);
        std::vector<uint64_t> ids;
        std::string blocked;
        int selected = 0;
        for (const Layer *l : editable_layers(C)) {
          int here = 0;
          for (const MeshPoint &p : l->drawing.points) {
            here += p.selected ? 1 : 0;
          }
          if (here == 0) {
            continue;
          }
          const std::string why = sculpt_dyntopo_blocked(*C.document, l->id, false);
          if (why.empty()) {
            ids.push_back(l->id);
            selected += here;
          }
          else if (blocked.empty()) {
            blocked = l->name + ": " + why;
          }
        }
        if (ids.empty()) {
          report(ReportType::Warning, blocked.empty() ? std::string("No points selected")
                                                      : "Nothing decimated (" + blocked + ")");
          return OpStatus::Cancelled;
        }
        ts.decimate_ratio = ratio;
        /* Finished even when nothing goes (a Ratio of 1), so raising it in the Adjust Last Operation panel puts back the
         * points a lower one took out. */
        const int gone = sculpt_decimate_curve(*C.document, ids, ratio);
        const std::string count = std::to_string(gone) + " of " + std::to_string(selected) +
                                  (selected == 1 ? " point" : " points") + " removed";
        op_note(count);
        report(ReportType::Info, "Decimate Curve: " + count);
        return OpStatus::Finished;
      });
  {
    OperatorType &ot = const_cast<OperatorType &>(*operators().find("mesh.decimate"));
    ot.props = {op_float_property("ratio", "Ratio",
                                  "The share of the selected points to keep: 1 keeps them all. The ones the line "
                                  "changes least without go first",
                                  "0.5", 0.0f, 1.0f, 0.01f)};
  }

  add("sculpt.brush_set", "Set Sculpt Brush",
      "Pick brush= (GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST, SURFACE_SMOOTH): its built-in preset", 0, nullptr, [](Context &C) {
        SculptBrush brush;
        if (!sculpt_brush_from_id(op_arg(C, "brush"), &brush)) {
          report(ReportType::Error, "brush= is one of GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST, SURFACE_SMOOTH");
          return OpStatus::Cancelled;
        }
        preset_apply(C.wm->tool_settings, sculpt_preset_default(brush));
        return OpStatus::Finished;
      });
  /* One per brush, for the Sculpt keymap (its keys take no arguments). */
  for (int i = 0; i < SCULPT_BRUSH_COUNT; i++) {
    const SculptBrush brush = SculptBrush(i);
    static std::vector<std::string> ids, names;
    ids.push_back(std::string("sculpt.brush_") + sculpt_brush_id(brush));
    for (char &c : ids.back()) {
      c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    names.push_back(std::string("Sculpt Brush: ") + sculpt_brush_name(brush));
    OperatorType ot;
    ot.idname = ids.back();
    ot.name = names.back();
    ot.description = "Pick this sculpt brush (its built-in preset)";
    ot.poll = sculpt_tool;
    ot.exec = [brush](Context &C) {
      preset_apply(C.wm->tool_settings, sculpt_preset_default(brush));
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  add("sculpt.preset_load", "Load Sculpt Preset", "Pick the sculpt preset named name= (as clicking it on the shelf)", 0,
      nullptr, [](Context &C) {
        const SculptPreset *p = sculpt_preset_find(op_arg(C, "name"));
        if (!p) {
          report(ReportType::Error, "No sculpt brush named \"" + op_arg(C, "name") + "\"");
          return OpStatus::Cancelled;
        }
        preset_apply(C.wm->tool_settings, *p);
        return OpStatus::Finished;
      });
  add("sculpt.preset_save", "Save Sculpt Preset",
      "Save the Sculpt tool's brush and settings to the asset library as name= (made unique), shown under Mine",
      OPTYPE_REGISTER, nullptr, [](Context &C) {
        ToolSettings &ts = C.wm->tool_settings;
        SculptPreset p;
        p.name = op_arg(C, "name", sculpt_brush_name(ts.sculpt_brush));
        p.brush = ts.sculpt_brush;
        p.radius = ts.sculpt_radius;
        p.strength = ts.sculpt_strength;
        p.falloff = ts.sculpt_falloff;
        std::string error;
        const std::string saved = sculpt_preset_save(p, &error);
        if (saved.empty()) {
          report(ReportType::Error, error);
          return OpStatus::Cancelled;
        }
        ts.sculpt_preset = saved;
        /* Saved as it is: nothing changed from that. */
        ts.sculpt_changes.erase(saved);
        report(ReportType::Info, "Saved sculpt brush \"" + saved + "\"");
        return OpStatus::Finished;
      });
  add("sculpt.preset_remove", "Delete Sculpt Preset", "Delete the saved sculpt preset named name= from the asset library",
      OPTYPE_REGISTER, nullptr, [](Context &C) {
        std::string error;
        if (!sculpt_preset_remove(op_arg(C, "name"), &error)) {
          report(ReportType::Error, error);
          return OpStatus::Cancelled;
        }
        C.wm->tool_settings.sculpt_changes.erase(op_arg(C, "name"));
        return OpStatus::Finished;
      });
  add("sculpt.radius_adjust", "Adjust Sculpt Radius", "Size the sculpt brush with the mouse (F); click to keep it",
      0, sculpt_tool, [](Context &C) {
        C.wm->sculpt_adjust_request = 'R';
        return OpStatus::Finished;
      });
  add("sculpt.resolution_adjust", "Adjust Dyntopo Resolution",
      "Set Dyntopo's Resolution with the mouse (R, as Blender's detail size): right is finer; a ghost grid shows a step "
      "of it on the active layer; click to keep it", 0, sculpt_tool, [](Context &C) {
        C.wm->sculpt_adjust_request = 'D';
        return OpStatus::Finished;
      });
  add("sculpt.strength_adjust", "Adjust Sculpt Strength",
      "Set the sculpt brush's strength with the mouse (Shift F); click to keep it", 0, sculpt_tool, [](Context &C) {
        C.wm->sculpt_adjust_request = 'S';
        return OpStatus::Finished;
      });
}

}  // namespace oanim
