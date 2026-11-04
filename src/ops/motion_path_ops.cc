/* Motion path operators (anim/motion_path.hh): pinning a path, its colour and end, what paths are drawn relative to,
 * and a snapshot to compare an edit against. */

#include <algorithm>
#include <cstdlib>
#include <string>

#include "anim/animation.hh"
#include "anim/motion_path.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

namespace {

/* The targets an operator works on: layer=id (with bone=name or point=id) for one, else those traced now. */
bool op_targets(Context &C, std::vector<MotionPathTarget> &r_targets)
{
  Document &doc = *C.document;
  r_targets.clear();
  if (!op_has_arg(C, "layer")) {
    r_targets = motion_path_targets(doc, editable_layers(C));
    return !r_targets.empty();
  }
  const Layer *layer = layer_find(doc, uint64_t(std::stoull(op_arg(C, "layer", "0"))));
  if (!layer) {
    report(ReportType::Warning, "No such layer");
    return false;
  }
  MotionPathTarget t;
  t.layer = layer->id;
  if (const std::string bone = op_arg(C, "bone"); !bone.empty()) {
    for (const Bone &b : layer->bones) {
      if (b.name == bone) {
        t.bone = b.id;
      }
    }
    if (!t.bone) {
      report(ReportType::Warning, "No bone named " + bone);
      return false;
    }
  }
  t.point = uint64_t(std::stoull(op_arg(C, "point", "0")));
  /* As it is traced: by its head, say. */
  if (const MotionPathTarget *style = motion_path_style(doc, t)) {
    t = *style;
  }
  r_targets.push_back(t);
  return true;
}

bool parse_color(const std::string &text, Color &r_color)
{
  float c[3];
  const char *at = text.c_str();
  for (int i = 0; i < 3; i++) {
    char *end = nullptr;
    c[i] = std::strtof(at, &end);
    if (end == at || (i < 2 && *end != ',')) {
      return false;
    }
    at = end + (i < 2 ? 1 : 0);
  }
  r_color = {c[0], c[1], c[2], 1.0f};
  return true;
}

}  // namespace

void motion_path_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_pin";
    ot.name = "Pin Motion Path";
    ot.description = "Keep the paths traced now (layer=id with bone=name or point=id: that one) showing when they "
                     "aren't selected; pinned=0 lets them go, without it each one is toggled";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      for (const MotionPathTarget &t : targets) {
        MotionPathTarget &style = motion_path_style_ensure(*C.document, t);
        style.pinned = op_has_arg(C, "pinned") ? op_arg_bool(C, "pinned") : !style.pinned;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_style";
    ot.name = "Motion Path Style";
    ot.description = "For the paths traced now (layer=id with bone=name or point=id: that one): color=\"r,g,b\" its "
                     "colour (\"\" the usual one), head=1 a bone traced by its head, head=0 by its tip";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      Color color;
      const bool recolor = op_has_arg(C, "color");
      if (recolor && !op_arg(C, "color").empty() && !parse_color(op_arg(C, "color"), color)) {
        report(ReportType::Warning, "A colour is three numbers 0-1: \"1,0.5,0\"");
        return OpStatus::Cancelled;
      }
      for (const MotionPathTarget &t : targets) {
        MotionPathTarget &style = motion_path_style_ensure(*C.document, t);
        if (recolor) {
          style.has_color = !op_arg(C, "color").empty();
          style.color = style.has_color ? color : Color();
        }
        if (op_has_arg(C, "head")) {
          style.head = op_arg_bool(C, "head");
        }
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_relative";
    ot.name = "Motion Paths Relative To";
    ot.description = "Draw motion paths relative to=WORLD, CAMERA or PARENT (layer=id, and bone=name for one of its "
                     "bones; without them the active layer and its active bone), so that stands still";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::string to = op_arg(C, "to", "WORLD");
      if (to == "WORLD" || to == "CAMERA") {
        doc.path_relative = to == "WORLD" ? PathRelative::World : PathRelative::Camera;
        return OpStatus::Finished;
      }
      if (to != "PARENT") {
        report(ReportType::Warning, "Relative to WORLD, CAMERA or PARENT");
        return OpStatus::Cancelled;
      }
      const Layer *layer = op_has_arg(C, "layer") ? layer_find(doc, uint64_t(std::stoull(op_arg(C, "layer", "0"))))
                                                  : layer_find(doc, doc.active_layer);
      if (!layer) {
        report(ReportType::Warning, "Pick the layer the paths are relative to");
        return OpStatus::Cancelled;
      }
      uint64_t bone = 0;
      if (op_has_arg(C, "bone")) {
        for (const Bone &b : layer->bones) {
          if (b.name == op_arg(C, "bone")) {
            bone = b.id;
          }
        }
        if (!bone && !op_arg(C, "bone").empty()) {
          report(ReportType::Warning, "No bone named " + op_arg(C, "bone"));
          return OpStatus::Cancelled;
        }
      }
      else if (layer->is_bone_layer()) {
        bone = layer->active_bone;
      }
      doc.path_relative = PathRelative::Parent;
      doc.path_relative_layer = layer->id;
      doc.path_relative_bone = bone;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_snapshot";
    ot.name = "Motion Path Snapshot";
    ot.description = "Keep a faint copy of the paths as they are now, to compare an edit against (clear=1: take it "
                     "away)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      WindowManager &wm = *C.wm;
      wm.path_snapshot.clear();
      if (op_arg_bool(C, "clear")) {
        return OpStatus::Finished;
      }
      const Document &doc = *C.document;
      const std::vector<MotionPathTarget> targets = motion_path_targets(doc, editable_layers(C));
      int first = 0, last = 0;
      if (!motion_path_range(doc, targets, first, last)) {
        report(ReportType::Warning, "No motion paths to keep: select what to trace");
        return OpStatus::Cancelled;
      }
      for (const MotionPath &p : motion_paths_make(doc, targets, first, last)) {
        wm.path_snapshot.push_back(p.points);
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_select";
    ot.name = "Select Motion Path Dot";
    ot.description = "Select the keys on frame=N of what the paths trace, as clicking a path's dot with the Motion "
                     "Path tool does, showing their path handles (on an in-between dot, the keys ending its stretch, "
                     "whose interpolation it moves by). go=1 (Alt click) also goes to that frame. extend=1 keeps the "
                     "keys selected already";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      const int frame = std::max(1, op_arg_int(C, "frame", doc.frame_current));
      if (op_arg_bool(C, "go")) {
        doc.frame_current = frame;
        animation_sync(doc);
      }
      if (!op_arg_bool(C, "extend")) {
        std::vector<Layer *> all;
        for (const Layer *l : document_layers_flat(doc)) {
          all.push_back(layer_find(doc, l->id));
        }
        layers_deselect_keys(all);
      }
      for (const MotionPathTarget &t : targets) {
        Layer *layer = layer_find(doc, t.layer);
        const std::vector<int> keys = motion_path_keys(doc, t);
        const auto key = std::lower_bound(keys.begin(), keys.end(), frame);
        if (layer && key != keys.end()) {
          layers_select_keys({layer}, *key, *key, true, motion_path_key_filter(doc, t));
        }
      }
      C.undo->mark_changed();
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_move";
    ot.name = "Move Motion Path Dot";
    ot.description = "Put what a path traces (layer=id with bone=name or point=id: that one; else the first traced) "
                     "at x, y on frame=N (x, y in document space on that frame), keyed there whatever auto-key says: "
                     "a key moves, an in-between gets a key; the frame shown stays. A bone's tip turns the bone. "
                     "straight=1 puts it on the line between the keys either side, nearest x, y";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      const MotionPathTarget t = targets.front();
      const MotionPathOnFrame on(doc, std::max(1, op_arg_int(C, "frame", doc.frame_current)));
      float2 at = {op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f)};
      if (op_arg_bool(C, "straight") && !motion_path_straighten(doc, t, at)) {
        report(ReportType::Warning, "Straight needs keys either side");
        return OpStatus::Cancelled;
      }
      if (!motion_path_place(doc, t, at)) {
        report(ReportType::Warning, "That can't be moved there (a connected bone's head goes with its parent)");
        return OpStatus::Cancelled;
      }
      motion_path_record(doc);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_breakdown";
    ot.name = "Motion Path Breakdown";
    ot.description = "A breakdown on frame=N, as Shift-dragging an in-between dot does: what a path traces (layer=, "
                     "bone=, point= or the first traced) keyed factor=0-1 of the way from its pose on the key "
                     "before to the key after, staying on its arc";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      const MotionPathOnFrame on(doc, std::max(1, op_arg_int(C, "frame", doc.frame_current)));
      if (!motion_path_breakdown(doc, targets.front(), op_arg_float(C, "factor", 0.5f))) {
        report(ReportType::Warning, "A breakdown goes between two keys");
        return OpStatus::Cancelled;
      }
      motion_path_record(doc);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_smooth_arc";
    ot.name = "Smooth Arc";
    ot.description = "Even out a lumpy path: each key inside the path's range moves onto a smooth curve through the "
                     "keys either side of it (layer=, bone=, point= for one path; else every path traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      int first = 0, last = 0;
      if (!motion_path_range(doc, motion_path_targets(doc, editable_layers(C)), first, last)) {
        motion_path_range(doc, targets, first, last);
      }
      int moved = 0;
      for (const MotionPathTarget &t : targets) {
        moved += motion_path_smooth_arc(doc, t, first, last);
      }
      if (moved == 0) {
        report(ReportType::Info, "No keys inside the path's range to smooth");
        return OpStatus::Cancelled;
      }
      report(ReportType::Info, "Smoothed " + std::to_string(moved) + (moved == 1 ? " key" : " keys"));
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_handle";
    ot.name = "Move Motion Path Handle";
    ot.description = "Move a path handle of the position key on frame=N of what a path traces (layer=, bone= (its "
                     "head), point=, or the first traced) so it ends at x, y: side=OUT shapes the path toward the next "
                     "key, IN from the one before. The other handle stays in line, unless broken=1 (Alt), which "
                     "breaks the pair";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      const int frame = op_arg_int(C, "frame", doc.frame_current);
      const float2 at = {op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f)};
      if (!motion_path_handle_set(doc, targets.front(), frame, op_arg(C, "side") != "IN", at, op_arg_bool(C, "broken")))
      {
        report(ReportType::Warning, "No position key there with path handles (a bone's tip turns, so has none)");
        return OpStatus::Cancelled;
      }
      animation_evaluate(doc, doc.frame_current);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_handles_auto";
    ot.name = "Auto Handles";
    ot.description = "Put the selected keys' path handles back to Auto, the path running through them as it did "
                     "before they were dragged (layer=, bone=, point= for one path; else every path traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      int changed = 0;
      for (const MotionPathTarget &t : targets) {
        changed += motion_path_handles_auto(doc, t);
      }
      if (changed == 0) {
        report(ReportType::Info, "No selected key has dragged handles");
        return OpStatus::Cancelled;
      }
      animation_evaluate(doc, doc.frame_current);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_brush_set";
    ot.name = "Set Motion Path Brush";
    ot.description = "Pick the Motion Path tool's brush (brush=EDIT, GRAB, SMOOTH, PAINT_SPEED, FASTER, SLOWER or "
                     "BLEND_SPEED or EDIT_SPEED), and set radius= (screen pixels), strength=, speed= (Paint Speed's), "
                     "and Edit Speed's proportional editing: proportional=0/1, proportional_size= (frames), falloff= "
                     "(SMOOTH, SPHERE, ROOT, INVERSE_SQUARE, SHARP, LINEAR, CONSTANT, RANDOM)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.wm != nullptr; };
    ot.exec = [](Context &C) {
      ToolSettings &ts = C.wm->tool_settings;
      if (op_has_arg(C, "brush")) {
        PathBrush brush;
        if (!path_brush_from_id(op_arg(C, "brush"), &brush)) {
          report(ReportType::Error, "Unknown path brush '" + op_arg(C, "brush") + "'");
          return OpStatus::Cancelled;
        }
        ts.path_brush = brush;
      }
      if (op_has_arg(C, "radius")) {
        ts.path_radius = std::clamp(op_arg_float(C, "radius", ts.path_radius), 2.0f, 1000.0f);
      }
      if (op_has_arg(C, "strength")) {
        ts.path_strength = std::clamp(op_arg_float(C, "strength", ts.path_strength), 0.0f, 1.0f);
      }
      if (op_has_arg(C, "speed")) {
        ts.path_speed = std::clamp(op_arg_float(C, "speed", ts.path_speed), 0.05f, 20.0f);
      }
      if (op_has_arg(C, "proportional")) {
        ts.proportional = op_arg_bool(C, "proportional");
      }
      if (op_has_arg(C, "proportional_size")) {
        ts.path_proportional_size =
            std::clamp(op_arg_float(C, "proportional_size", ts.path_proportional_size), 1.0f, 500.0f);
      }
      if (op_has_arg(C, "falloff") && !proportional_falloff_from_identifier(op_arg(C, "falloff"), &ts.proportional_falloff)) {
        report(ReportType::Error, "Unknown falloff '" + op_arg(C, "falloff") + "'");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  for (const char *which : {"radius", "strength"}) {
    OperatorType ot;
    const bool radius = std::string(which) == "radius";
    ot.idname = radius ? "anim.motion_path_radius_adjust" : "anim.motion_path_strength_adjust";
    ot.name = radius ? "Adjust Path Brush Radius" : "Adjust Path Brush Strength";
    ot.description = radius ? "Size the Motion Path tool's brush with the mouse (F); click to keep it"
                            : "Set the Motion Path tool's brush strength with the mouse (Shift F); click to keep it";
    ot.flag = 0;
    ot.poll = [](Context &C) {
      return C.wm && C.wm->tool_settings.tool == Tool::MotionPath && path_brush_has_radius(C.wm->tool_settings.path_brush);
    };
    ot.exec = [radius](Context &C) {
      C.wm->path_adjust_request = radius ? 'R' : 'S';
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_stroke";
    ot.name = "Motion Path Brush Stroke";
    ot.description = "A stroke of a path brush (brush=GRAB, SMOOTH, PAINT_SPEED, FASTER, SLOWER or BLEND_SPEED; "
                     "else the tool's) through points=\"x,y;x,y\" (document space), with radius= in document units, "
                     "strength=, speed= and invert=1 (Ctrl), on the paths traced (or layer=, bone=, point=): Grab "
                     "moves what was under the first point by the move to the last; the others dab at every point";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const ToolSettings &ts = C.wm->tool_settings;
      PathBrush brush = ts.path_brush;
      if (op_has_arg(C, "brush") && !path_brush_from_id(op_arg(C, "brush"), &brush)) {
        report(ReportType::Error, "Unknown path brush '" + op_arg(C, "brush") + "'");
        return OpStatus::Cancelled;
      }
      if (!path_brush_has_radius(brush)) {
        report(ReportType::Warning, std::string(path_brush_name(brush)) + " isn't a brush: drag the path's dots");
        return OpStatus::Cancelled;
      }
      std::vector<float2> points;
      const std::string text = op_arg(C, "points");
      for (size_t at = 0; at < text.size();) {
        const size_t end = std::min(text.find(';', at), text.size());
        const std::string pair = text.substr(at, end - at);
        char *rest = nullptr;
        const float x = std::strtof(pair.c_str(), &rest);
        if (rest != pair.c_str() && *rest == ',') {
          char *end_y = nullptr;
          const float y = std::strtof(rest + 1, &end_y);
          if (end_y != rest + 1) {
            points.push_back({x, y});
          }
        }
        at = end + 1;
      }
      std::vector<MotionPathTarget> targets;
      if (points.empty() || !op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      int first = 0, last = 0;
      if (!motion_path_range(doc, motion_path_targets(doc, editable_layers(C)), first, last) &&
          !motion_path_range(doc, targets, first, last))
      {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      PathDab dab;
      dab.radius = op_arg_float(C, "radius", 0.1f);
      dab.strength = op_arg_float(C, "strength", ts.path_strength);
      dab.speed = op_arg_float(C, "speed", ts.path_speed);
      dab.invert = op_arg_bool(C, "invert");
      int changed = 0;
      if (brush == PathBrush::Grab) {
        const std::vector<PathGrabKey> keys = motion_path_grab_keys(doc, targets, first, last, points.front(), dab.radius);
        changed = motion_path_grab(doc, keys, points.back() - points.front(), dab.strength);
      }
      else {
        for (const float2 p : points) {
          dab.center = p;
          changed += brush == PathBrush::Smooth ? motion_path_smooth_dab(doc, targets, first, last, dab)
                                                : motion_path_speed_dab(doc, targets, first, last, brush, dab);
        }
      }
      if (changed == 0) {
        report(ReportType::Info, "Nothing under the brush");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_slide";
    ot.name = "Slide Motion Path Dot";
    ot.description = "Edit Speed: slide the dot on frame=N (between keys) along its path to the place nearest x, y "
                     "(document space on that frame), the keys staying on their frames (its stretch made Custom); "
                     "proportional=1 slides the dots within size= frames too, by falloff= (the tool's settings when "
                     "left out). Of what a path traces (layer=, bone=, point=, or the first traced); the frame shown "
                     "stays";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const ToolSettings &ts = C.wm->tool_settings;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      PathSlide slide;
      slide.proportional = op_has_arg(C, "proportional") ? op_arg_bool(C, "proportional") : ts.proportional;
      slide.size = std::max(op_arg_float(C, "size", ts.path_proportional_size), 1.0f);
      slide.falloff = ts.proportional_falloff;
      if (op_has_arg(C, "falloff") && !proportional_falloff_from_identifier(op_arg(C, "falloff"), &slide.falloff)) {
        report(ReportType::Error, "Unknown falloff '" + op_arg(C, "falloff") + "'");
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      const float2 at = {op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f)};
      if (!motion_path_slide(doc, targets.front(), op_arg_int(C, "frame", doc.frame_current), at, slide)) {
        report(ReportType::Warning, "Only a dot between keys slides (not a key, a held frame or a stepped stretch)");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_even_spacing";
    ot.name = "Even Spacing";
    ot.description = "The stretch frame=N is in goes along its path at an even speed (Ctrl click with Edit Speed), "
                     "the keys staying on their frames; of what a path traces (layer=, bone=, point=, or the first "
                     "traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      if (!motion_path_even_spacing(doc, targets.front(), op_arg_int(C, "frame", doc.frame_current))) {
        report(ReportType::Warning, "Only between keys");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace oanim
