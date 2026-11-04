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
  /* A Key Whole Drawing layer's drawing as a whole (its timing). */
  t.drawing = op_arg_bool(C, "drawing");
  if (t.drawing && !layer->key_whole_drawing) {
    report(ReportType::Warning, layer->name + " isn't keyed as a whole drawing");
    return false;
  }
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


/* The Motion Path tool with Edit, its shortcuts' poll. */
static bool path_edit_poll(Context &C)
{
  return C.document && C.wm && C.wm->tool_settings.tool == Tool::MotionPath &&
         C.wm->tool_settings.path_brush == PathBrush::Edit;
}

/* The keys selected on the paths traced. */
static std::vector<std::pair<MotionPathTarget, int>> path_selected_keys(Context &C)
{
  return motion_path_selected_keys(*C.document, motion_path_targets(*C.document, editable_layers(C)));
}

/* The dots selected, then the keys selected: what Alt S scales the timing around. */
static std::vector<std::pair<MotionPathTarget, int>> path_selection(Context &C)
{
  std::vector<std::pair<MotionPathTarget, int>> all = C.wm ? C.wm->path_dots : std::vector<std::pair<MotionPathTarget, int>>();
  for (const auto &k : path_selected_keys(C)) {
    all.push_back(k);
  }
  return all;
}

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
    ot.description = "Select what's on frame=N of the paths traced, as clicking a path's dot with the Motion Path "
                     "tool's Edit does: a key (showing its path handles), or a dot between keys (for its timing); "
                     "extend=1 (Shift) adds it to the selection, or takes it out if it was in; go=1 (double click) "
                     "also goes to that frame; none=1 deselects every key and dot";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      const int frame = std::max(1, op_arg_int(C, "frame", doc.frame_current));
      std::vector<std::pair<MotionPathTarget, int>> &dots = C.wm->path_dots;
      const bool none = op_arg_bool(C, "none");
      if (op_arg_bool(C, "go") && !none) {
        /* On something selected already (a double click's second press, Go to Frame): only the frame changes. */
        for (const MotionPathTarget &t : targets) {
          const bool dot = std::any_of(dots.begin(), dots.end(),
                                       [&](const auto &d) { return d.first.same(t) && d.second == frame; });
          if (dot || motion_path_key_selected(doc, t, frame)) {
            doc.frame_current = frame;
            animation_sync(doc);
            C.undo->mark_changed();
            return OpStatus::Finished;
          }
        }
      }
      if (!op_arg_bool(C, "extend") || none) {
        std::vector<Layer *> all;
        for (const Layer *l : document_layers_flat(doc)) {
          all.push_back(layer_find(doc, l->id));
        }
        layers_deselect_keys(all);
        dots.clear();
      }
      if (none) {
        /* Nothing picked: the Path Timing panel follows the frame shown again. */
        C.wm->path_timing_end = 0;
        C.undo->mark_changed();
        return OpStatus::Finished;
      }
      if (op_arg_bool(C, "go")) {
        doc.frame_current = frame;
        animation_sync(doc);
      }
      for (const MotionPathTarget &t : targets) {
        Layer *layer = layer_find(doc, t.layer);
        if (!layer) {
          continue;
        }
        if (motion_path_is_key(doc, t, frame)) {
          /* The key, on every channel the path's keys are on (as the timeline shows them). */
          const bool was = motion_path_key_selected(doc, t, frame);
          C.wm->path_timing_target = t;
          C.wm->path_timing_end = frame;
          C.wm->path_timing_frame = doc.frame_current;
          layers_select_keys({layer}, frame, frame, !(was && op_arg_bool(C, "extend")), motion_path_key_filter(doc, t));
          continue;
        }
        /* The Path Timing panel shows the stretch picked on. */
        C.wm->path_timing_target = t;
        C.wm->path_timing_frame = doc.frame_current;
        for (const MotionPathStretch &s : motion_path_stretches(doc, t)) {
          if (frame > s.start && frame < s.end) {
            C.wm->path_timing_end = s.end;
          }
        }
        const auto it = std::find_if(dots.begin(), dots.end(),
                                     [&](const auto &d) { return d.first.same(t) && d.second == frame; });
        if (it != dots.end()) {
          dots.erase(it);
        }
        else {
          dots.push_back({t, frame});
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
    ot.description = "Pick the Motion Path tool's brush (brush=EDIT, GRAB, SMOOTH or BLEND_SPEED), and set that "
                     "brush's own radius= (screen pixels) and strength= (Edit's are its Shift brush's, Blend Speed's), "
                     "the timing chart's and Path Timing graph's reach (chart_size=, graph_size=, in frames), "
                     "and Edit's proportional editing of dots: proportional=0/1, proportional_size= (frames), falloff= "
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
      /* Edit has no circle of its own: its settings are Shift's brush's. */
      const PathBrush sized = path_brush_has_radius(ts.path_brush) ? ts.path_brush : path_brush_shifted(ts.path_brush);
      PathBrushSettings &bs = ts.path_brush_settings(sized);
      if (op_has_arg(C, "radius")) {
        bs.radius = std::clamp(op_arg_float(C, "radius", bs.radius), 2.0f, 1000.0f);
      }
      if (op_has_arg(C, "strength")) {
        bs.strength = std::clamp(op_arg_float(C, "strength", bs.strength), 0.0f, 1.0f);
      }
      if (op_has_arg(C, "proportional")) {
        ts.proportional = op_arg_bool(C, "proportional");
      }
      if (op_has_arg(C, "chart_size")) {
        ts.path_chart_size = std::clamp(op_arg_float(C, "chart_size", ts.path_chart_size), 0.0f, 100.0f);
      }
      if (op_has_arg(C, "graph_size")) {
        ts.path_graph_size = std::clamp(op_arg_float(C, "graph_size", ts.path_graph_size), 0.0f, 100.0f);
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
    /* With Edit: Shift's brush's (Blend Speed). */
    ot.poll = [](Context &C) { return C.wm && C.wm->tool_settings.tool == Tool::MotionPath; };
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
    ot.description = "A stroke of a path brush (brush=GRAB, SMOOTH or BLEND_SPEED; else the tool's, or with shift=1 "
                     "the brush Shift swaps in: Grab's Smooth, Edit's Blend Speed) through points=\"x,y;x,y\" "
                     "(document space), with radius= in document units, strength= (else the brush's own) and "
                     "invert=1 (Ctrl), on the paths traced (or layer=, bone=, point=): Grab "
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
      if (op_arg_bool(C, "shift")) {
        brush = path_brush_shifted(brush);
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
      dab.strength = op_arg_float(C, "strength", ts.path_brush_settings(brush).strength);
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
    ot.description = "Dragging a dot with Edit: slide the dot on frame=N (between keys) along its path to the place nearest x, y "
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
      const int frame = op_arg_int(C, "frame", doc.frame_current);
      /* progress= (the timing chart's): how far along its stretch, instead of a place to be nearest. */
      const bool slid = op_has_arg(C, "progress")
                            ? motion_path_slide_progress(doc, targets.front(), frame, op_arg_float(C, "progress", 0.5f),
                                                         slide)
                            : motion_path_slide(doc, targets.front(), frame, at, slide);
      if (!slid) {
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
    ot.description = "The stretch frame=N is in (else each selected dot's) goes along its path at an even speed, the "
                     "keys staying on their frames; of what a path traces (layer=, bone=, point=, or the first "
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
      int changed = 0;
      if (op_has_arg(C, "frame")) {
        changed += motion_path_even_spacing(doc, targets.front(), op_arg_int(C, "frame", doc.frame_current)) ? 1 : 0;
      }
      else {
        for (const auto &[t, f] : C.wm->path_dots) {
          changed += motion_path_even_spacing(doc, t, f) ? 1 : 0;
        }
      }
      if (changed == 0) {
        report(ReportType::Warning, "Only between keys");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_stretch_shape";
    ot.name = "Path Timing Shape";
    ot.description = "The stretch frame=N is strictly inside goes along its path in shape= EVEN, EASE_IN, EASE_OUT or "
                     "EASE_IN_OUT, the keys staying on their frames; of what a path traces (layer=, bone=, point=, or "
                     "the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      PathShape shape = PathShape::Even;
      if (!path_shape_from_identifier(op_arg(C, "shape", "EVEN"), shape)) {
        report(ReportType::Warning, "shape= is EVEN, EASE_IN, EASE_OUT or EASE_IN_OUT");
        return OpStatus::Cancelled;
      }
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      if (!motion_path_stretch_shape(doc, targets.front(), op_arg_int(C, "frame", doc.frame_current), shape)) {
        report(ReportType::Warning, "Only between keys");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_key_interp";
    ot.name = "Path Key Interpolation";
    ot.description = "Right-click a key's diamond in the timing chart or Path Timing graph: the interpolation of the "
                     "key on frame=N of what a path traces (layer=, bone=, point=, drawing=1, or the first traced): its "
                     "own key there (a drawing's: all its channels'), else its layer's or a group's that moves it "
                     "there. interp=, ease=, hold= and the rest as anim.keys_interpolation_set takes them";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_record(doc);
      bool ok = true;
      std::string what;
      const int frame = op_arg_int(C, "frame", doc.frame_current);
      const int count = motion_path_key_settings(doc, targets.front(), frame,
                                                 [&](KeySettings &s) { ok &= interp_args_apply(C, s, &what); });
      if (!ok) {
        return OpStatus::Cancelled;
      }
      if (count == 0) {
        report(ReportType::Warning, "No key on frame " + std::to_string(frame));
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_key_delete";
    ot.name = "Delete Path Key";
    ot.description = "Delete the key on frame=N of what a path traces (layer=, bone=, point=, drawing=1), the one "
                     "anim.motion_path_key_interp sets";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_record(doc);
      const int frame = op_arg_int(C, "frame", doc.frame_current);
      if (motion_path_key_delete(doc, targets.front(), frame) == 0) {
        report(ReportType::Warning, "No key on frame " + std::to_string(frame));
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      animation_evaluate(doc, doc.frame_current);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_timing_blend";
    ot.name = "Blend Timing";
    ot.description = "Shift click or drag in the timing chart or the Path Timing graph: even out the spacing around "
                     "frame=N (a drag's stroke: frames=\"15,16,17\", a blend on each in turn), each dot within radius= frames moving smoothing= (0-1) of the way to the midpoint of its "
                     "neighbours, repeat= times, the keys staying; of what a path traces (layer=, bone=, point=, or "
                     "the first traced). Left out, the settings of the last blend";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      ToolSettings &ts = C.wm->tool_settings;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      /* Each setting given is kept for the next click; each left out is the last one's (and shown as used). */
      ts.path_blend_smoothing = std::clamp(op_arg_float(C, "smoothing", ts.path_blend_smoothing), 0.0f, 1.0f);
      ts.path_blend_repeat = std::clamp(op_arg_int(C, "repeat", ts.path_blend_repeat), 1, 100);
      ts.path_blend_radius = std::clamp(op_arg_float(C, "radius", ts.path_blend_radius), 0.0f, 100.0f);
      op_arg_used(C, "smoothing", std::to_string(ts.path_blend_smoothing));
      op_arg_used(C, "repeat", std::to_string(ts.path_blend_repeat));
      op_arg_used(C, "radius", std::to_string(ts.path_blend_radius));
      animation_sync(doc);
      /* A Shift drag's stroke gives its frames, a blend on each in turn; a click one frame. */
      std::vector<int> frames = path_frames_parse(op_arg(C, "frames"));
      if (frames.empty()) {
        frames.push_back(op_arg_int(C, "frame", doc.frame_current));
      }
      if (motion_path_timing_blend_stroke(doc, targets.front(), frames, ts.path_blend_smoothing,
                                          ts.path_blend_repeat, ts.path_blend_radius) == 0)
      {
        report(ReportType::Warning, "No dots between keys there");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    ot.props = {op_float_property("smoothing", "Smoothing", "How far each dot moves toward the midpoint of its "
                                  "neighbours each time", "0.5", 0.0f, 1.0f, 0.01f),
                op_float_property("repeat", "Repeat", "How many times", "1", 1.0f, 100.0f, 1.0f),
                op_float_property("radius", "Radius", "How many frames either side of the click it reaches, "
                                  "fading to the edge", "3", 0.0f, 100.0f, 0.1f)};
    ot.props[1].kind = OpProperty::Kind::Int;
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_dot_speed";
    ot.name = "Path Dot Speed";
    ot.description = "The dot on frame=N goes speed= (1: the stretch's even speed), the rest of its stretch making up "
                     "the time; of what a path traces (layer=, bone=, point=, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      if (!motion_path_dot_speed_set(doc, targets.front(), op_arg_int(C, "frame", doc.frame_current),
                                     op_arg_float(C, "speed", 1.0f)))
      {
        report(ReportType::Warning, "Only between keys");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_timing_step";
    ot.name = "Previous/Next Stretch";
    ot.description = "The Path Timing panel shows the stretch step= (-1 or 1) before or after the one it shows";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      const Document &doc = *C.document;
      WindowManager &wm = *C.wm;
      MotionPathTarget t;
      MotionPathStretch shown;
      if (!motion_path_timing_stretch(doc, motion_path_timing_targets(doc, editable_layers(C)), wm.path_timing_target,
                                      wm.path_timing_end, wm.path_timing_frame, t, shown))
      {
        return OpStatus::Cancelled;
      }
      const std::vector<MotionPathStretch> stretches = motion_path_stretches(doc, t);
      const int step = op_arg_int(C, "step", 1) < 0 ? -1 : 1;
      for (size_t i = 0; i < stretches.size(); i++) {
        const int j = int(i) + step;
        if (stretches[i].end == shown.end && j >= 0 && j < int(stretches.size())) {
          wm.path_timing_target = t;
          wm.path_timing_end = stretches[size_t(j)].end;
          wm.path_timing_frame = doc.frame_current;
          return OpStatus::Finished;
        }
      }
      return OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timeline_sidebar_toggle";
    ot.name = "Sidebar";
    ot.description = "N: show or hide the timeline's sidebar (its Path Timing tab)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.wm != nullptr; };
    ot.exec = [](Context &C) {
      C.wm->timeline_sidebar = !C.wm->timeline_sidebar;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_scale_spacing";
    ot.name = "Scale Path Spacing";
    ot.description = "Alt S: the dots around frame=N (else around each dot and key selected) brought factor= as far "
                     "from it along the path (under 1 closer: slower there; on a key, easing into and out of it), the "
                     "keys staying on their frames; the dots within size= frames, by falloff= with proportional=1, "
                     "else all alike (the tool's settings when left out). Of what a path traces (layer=, bone=, "
                     "point=, or the first traced)";
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
      /* Around frame=, or else around each dot and key selected. */
      std::vector<std::pair<MotionPathTarget, int>> pivots;
      if (op_has_arg(C, "frame")) {
        pivots.push_back({targets.front(), op_arg_int(C, "frame", doc.frame_current)});
      }
      else {
        pivots = path_selection(C);
      }
      int changed = 0;
      for (const auto &[t, f] : pivots) {
        changed += motion_path_scale_spacing(doc, t, f, op_arg_float(C, "factor", 1.0f), slide);
      }
      if (changed == 0)
      {
        report(ReportType::Warning, "No moving stretch there to scale");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_scale_spacing_modal";
    ot.name = "Scale Path Spacing";
    ot.description = "Alt S with the Motion Path tool's Edit: scale the timing around the dots and keys selected with "
                     "the mouse (left closer, right further; numbers typed; Ctrl snaps to 0.1, Shift is finer, the "
                     "wheel sizes the reach; click or Enter keeps it, Esc or a right click cancels)";
    ot.flag = 0;
    ot.poll = [](Context &C) { return path_edit_poll(C) && !path_selection(C).empty(); };
    ot.exec = [](Context &C) {
      C.wm->path_scale_request = true;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  for (const char kind : {'G', 'R', 'S'}) {
    OperatorType ot;
    ot.idname = kind == 'G' ? "anim.motion_path_keys_grab" : kind == 'R' ? "anim.motion_path_keys_rotate"
                                                                         : "anim.motion_path_keys_scale";
    ot.name = kind == 'G' ? "Move Path Keys" : kind == 'R' ? "Rotate Path Keys" : "Scale Path Keys";
    ot.description = kind == 'G' ? "G with the Motion Path tool's Edit: move the selected keys with the mouse"
                     : kind == 'R'
                         ? "R with the Motion Path tool's Edit: turn the selected key's handles about it (several "
                           "keys: the keys about their centre, handles turning with them)"
                         : "S with the Motion Path tool's Edit: scale the selected key's handles (several keys: the "
                           "keys about their centre, handles scaling with them)";
    ot.flag = 0;
    ot.poll = [](Context &C) { return path_edit_poll(C) && !path_selected_keys(C).empty(); };
    ot.exec = [kind](Context &C) {
      C.wm->path_keys_request = kind;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_keys_transform";
    ot.name = "Transform Path Keys";
    ot.description = "The selected keys of the paths traced: kind=G moves them by x=, y= (document space), R turns "
                     "them by angle= (radians) and S scales them by factor= about their centre, their handles turning "
                     "and scaling with them; a key on its own stays, its handles turning or scaling about it";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::string kind = op_arg(C, "kind");
      if (kind != "G" && kind != "R" && kind != "S") {
        report(ReportType::Error, "kind= is G, R or S");
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      const int changed = motion_path_keys_transform(
          doc, path_selected_keys(C), kind[0], float2{op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f)},
          op_arg_float(C, "angle", 0.0f), op_arg_float(C, "factor", 1.0f));
      if (changed == 0) {
        report(ReportType::Info, "No keys selected on the paths");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_handles_type";
    ot.name = "Set Path Handle Type";
    ot.description = "The selected keys' path handles: mode=AUTO (as before they were dragged), ALIGNED (in line, each "
                     "keeping its length) or BROKEN (free of each other)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      PathHandles mode;
      if (!path_handles_from_identifier(op_arg(C, "mode"), &mode)) {
        report(ReportType::Error, "mode= is AUTO, ALIGNED or BROKEN");
        return OpStatus::Cancelled;
      }
      animation_sync(*C.document);
      if (motion_path_handles_type(*C.document, motion_path_targets(*C.document, editable_layers(C)), mode) == 0) {
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_handles_menu";
    ot.name = "Path Handle Type";
    ot.description = "V with the Motion Path tool's Edit: a menu of handle types for the selected keys";
    ot.flag = 0;
    ot.poll = [](Context &C) { return path_edit_poll(C) && !path_selected_keys(C).empty(); };
    ot.exec = [](Context &C) {
      C.wm->menu_request = "motion_path_handles";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_keys_delete";
    ot.name = "Delete Path Keys";
    ot.description = "Delete the selected keys of the paths traced (X with the Motion Path tool's Edit; the setup "
                     "frame's stay)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return path_edit_poll(C) && !path_selected_keys(C).empty(); };
    ot.exec = [](Context &C) {
      animation_record(*C.document);
      if (motion_path_keys_delete(*C.document, motion_path_targets(*C.document, editable_layers(C))) == 0) {
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_key_add";
    ot.name = "Add Motion Path Key";
    ot.description = "A key on frame=N of what a path traces (layer=, bone=, point=, or the first traced), where it is "
                     "now, so the path stays as it is (Alt click with the Motion Path tool's Edit); it is selected";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      const int frame = op_arg_int(C, "frame", doc.frame_current);
      animation_sync(doc);
      if (!motion_path_key_add(doc, targets.front(), frame)) {
        report(ReportType::Info, "A key is there already");
        return OpStatus::Cancelled;
      }
      std::vector<Layer *> all;
      for (const Layer *l : document_layers_flat(doc)) {
        all.push_back(layer_find(doc, l->id));
      }
      layers_deselect_keys(all);
      C.wm->path_dots.clear();
      if (Layer *layer = layer_find(doc, targets.front().layer)) {
        layers_select_keys({layer}, frame, frame, true, motion_path_key_filter(doc, targets.front()));
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace oanim
