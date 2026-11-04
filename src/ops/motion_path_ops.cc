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
    ot.description = "Go to frame=N, as clicking a path's dot with the Motion Path tool does, and select the keys "
                     "there of what the paths trace (on an in-between dot, the keys ending its stretch, whose "
                     "interpolation it moves by). extend=1 keeps the keys selected already";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      const int frame = std::max(1, op_arg_int(C, "frame", doc.frame_current));
      doc.frame_current = frame;
      animation_sync(doc);
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
                     "at x, y on frame=N, keyed there whatever auto-key says: a key moves, an in-between gets a key. "
                     "A bone's tip turns the bone. straight=1 puts it on the line between the keys either side, "
                     "nearest x, y";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      const MotionPathTarget t = targets.front();
      const int frame = std::max(1, op_arg_int(C, "frame", doc.frame_current));
      doc.frame_current = frame;
      animation_sync(doc);
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
      doc.frame_current = std::max(1, op_arg_int(C, "frame", doc.frame_current));
      animation_sync(doc);
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
}

}  // namespace oanim
