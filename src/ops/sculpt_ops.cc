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

/* Copies a preset into the tool, as picking it on the shelf does. */
static void preset_apply(ToolSettings &ts, const SculptPreset &p)
{
  ts.sculpt_preset = p.name;
  ts.sculpt_brush = p.brush;
  ts.sculpt_radius = p.radius;
  ts.sculpt_strength = p.strength;
  ts.sculpt_falloff = p.falloff;
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
    report(ReportType::Error, "brush= is one of GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST");
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
      "tool makes one: brush= GRAB, PUSH, SMOOTH, THICKNESS, PINCH or TWIST (default: the tool's), radius= in "
      "document units, strength=, falloff= SMOOTH, SPHERE or CONSTANT, invert=1 (Ctrl), selected_only=1, "
      "mirror=1 (X mirror), amount= how much of a full step each point of the path is for the brushes that work "
      "on while held (1), keep_poses=1 on frame 0",
      OPTYPE_REGISTER | OPTYPE_UNDO, has_drawing, stroke_exec);

  add("sculpt.brush_set", "Set Sculpt Brush",
      "Pick brush= (GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST): its built-in preset", 0, nullptr, [](Context &C) {
        SculptBrush brush;
        if (!sculpt_brush_from_id(op_arg(C, "brush"), &brush)) {
          report(ReportType::Error, "brush= is one of GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST");
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
        return OpStatus::Finished;
      });
  add("sculpt.radius_adjust", "Adjust Sculpt Radius", "Size the sculpt brush with the mouse (F); click to keep it",
      0, sculpt_tool, [](Context &C) {
        C.wm->sculpt_adjust_request = 'R';
        return OpStatus::Finished;
      });
  add("sculpt.strength_adjust", "Adjust Sculpt Strength",
      "Set the sculpt brush's strength with the mouse (Shift F); click to keep it", 0, sculpt_tool, [](Context &C) {
        C.wm->sculpt_adjust_request = 'S';
        return OpStatus::Finished;
      });
}

}  // namespace oanim
