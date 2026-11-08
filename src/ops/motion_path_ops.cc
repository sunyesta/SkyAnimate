/* Motion path operators (anim/motion_path.hh): pinning a path, its colour and end, what paths are drawn relative to,
 * and a snapshot to compare an edit against. */

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cmath>
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

void motion_path_tool_sync(Context &C)
{
  motion_path_tool_on(C.wm && C.wm->tool_settings.tool == Tool::MotionPath);
}

/* Own Motion is for the Motion Path tool (motion_path_own_motion): told whether it is on as an operator here ends. */
struct PathToolSync {
  Context &C;
  ~PathToolSync()
  {
    motion_path_tool_sync(C);
  }
};

std::vector<MotionPathTarget> motion_path_targets(Context &C)
{
  return motion_path_targets(*C.document, editable_layers(C), C.wm && C.wm->tool_settings.path_edit_layer);
}

std::vector<MotionPathTarget> motion_path_timing_targets(Context &C)
{
  return motion_path_timing_targets(*C.document, editable_layers(C), C.wm && C.wm->tool_settings.path_edit_layer);
}

std::vector<MotionPathTarget> motion_path_key_pick_targets(Context &C)
{
  std::vector<MotionPathTarget> out;
  const bool edit_layer = C.wm && C.wm->tool_settings.path_edit_layer;
  for (const MotionPathTarget &t : motion_path_timing_targets(C)) {
    /* Edit Layer off: bones and points, never the layer's own position. */
    if (edit_layer == (!t.bone && !t.point && !t.drawing)) {
      out.push_back(t);
    }
  }
  return out;
}

MotionPathPicks motion_path_picks(Context &C)
{
  MotionPathPicks r;
  if (!C.document || !C.wm || C.wm->tool_settings.tool != Tool::MotionPath) {
    return r;
  }
  const WindowManager &wm = *C.wm;
  r.stretches = motion_path_picked_stretches(*C.document, motion_path_key_pick_targets(C), wm.path_dots,
                                             wm.path_handle_picks);
  r.spans = motion_path_picked_spans(r.stretches);
  r.shown = int(r.stretches.size()) - 1;
  for (size_t i = 0; i < r.stretches.size(); i++) {
    const MotionPathPickedStretch &p = r.stretches[i];
    if (p.target.same(wm.path_timing_target) && p.start < wm.path_timing_end && wm.path_timing_end <= p.end) {
      r.shown = int(i);
    }
  }
  return r;
}

/* The stretch of `t` holding the picked one (a span across breakdowns is one). */
static bool stretch_holding(const Document &doc, const MotionPathPickedStretch &p, MotionPathStretch &r_stretch)
{
  for (const MotionPathStretch &s : motion_path_stretches(doc, p.target)) {
    if (s.start <= p.start && p.end <= s.end) {
      r_stretch = s;
      return true;
    }
  }
  return false;
}

bool motion_path_timing_shown(Context &C, MotionPathTarget &r_target, MotionPathStretch &r_stretch,
                              MotionPathPicks *r_picks)
{
  const Document &doc = *C.document;
  const WindowManager &wm = *C.wm;
  const MotionPathPicks picks = motion_path_picks(C);
  if (r_picks) {
    *r_picks = picks;
  }
  if (picks.shown >= 0) {
    const MotionPathPickedStretch &p = picks.stretches[size_t(picks.shown)];
    if (stretch_holding(doc, p, r_stretch)) {
      r_target = p.target;
      return true;
    }
  }
  return motion_path_timing_stretch(doc, motion_path_timing_targets(C), wm.path_timing_target, wm.path_timing_end,
                                    wm.path_timing_frame, r_target, r_stretch);
}

bool motion_path_pick_at(Context &C, const std::vector<MotionPathTarget> &targets, int frame, bool extend)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  /* Edits not yet recorded belong to the frame they were made at. */
  animation_record(doc);
  PathToolSync sync{C};
  if (!extend) {
    std::vector<Layer *> all;
    for (const Layer *l : document_layers_flat(doc)) {
      all.push_back(layer_find(doc, l->id));
    }
    layers_deselect_keys(all);
    wm.path_dots.clear();
  }
  wm.path_handle_picks.clear();
  bool picked = false;
  for (const MotionPathTarget &t : targets) {
    Layer *layer = layer_find(doc, t.layer);
    int start = 0, end = 0;
    if (!layer) {
      continue;
    }
    if (motion_path_is_key(doc, t, frame)) {
      layers_select_keys({layer}, frame, frame, true, motion_path_key_filter(doc, t));
      wm.path_timing_target = t;
      wm.path_timing_end = frame;
      picked = true;
    }
    else if (frame > 0 && motion_path_stretch_at(doc, t, frame, start, end)) {
      if (std::none_of(wm.path_dots.begin(), wm.path_dots.end(),
                       [&](const auto &d) { return d.first.same(t) && d.second == frame; }))
      {
        wm.path_dots.push_back({t, frame});
      }
      wm.path_timing_target = t;
      wm.path_timing_end = end;
      picked = true;
    }
  }
  doc.frame_current = frame;
  wm.path_timing_frame = frame;
  animation_sync(doc);
  C.undo->mark_changed();
  return picked;
}

namespace {

/* The targets an operator works on: layer=id (with bone=name, and head=1 for its head's path rather than its tip's,
 * or point=id) for one, else those traced now. */
bool op_targets(Context &C, std::vector<MotionPathTarget> &r_targets)
{
  Document &doc = *C.document;
  r_targets.clear();
  if (!op_has_arg(C, "layer")) {
    r_targets = motion_path_targets(C);
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
    t.head = op_arg_bool(C, "head");
  }
  t.point = uint64_t(std::stoull(op_arg(C, "point", "0")));
  /* A Key Whole Drawing layer's drawing as a whole (its timing). */
  t.drawing = op_arg_bool(C, "drawing");
  if (t.drawing && !(layer->has_drawing() ? layer->key_whole_drawing :
                                             key_together_allowed(*layer) && layer->key_together))
  {
    report(ReportType::Warning, layer->name + " isn't keyed as a whole drawing or together");
    return false;
  }
  /* With its style (colour, pinned). */
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
/* anim.motion_path_stroke's brush: brush=, else the tool's; shift=1 the one Shift swaps in. False for an unknown one. */
static bool stroke_brush(Context &C, PathBrush &r_brush)
{
  r_brush = C.wm ? C.wm->tool_settings.path_brush : PathBrush::Edit;
  if (op_has_arg(C, "brush") && !path_brush_from_id(op_arg(C, "brush"), &r_brush)) {
    return false;
  }
  if (op_arg_bool(C, "shift")) {
    r_brush = path_brush_shifted(r_brush);
  }
  return true;
}

static bool path_edit_poll(Context &C)
{
  return C.document && C.wm && C.wm->tool_settings.tool == Tool::MotionPath &&
         C.wm->tool_settings.path_brush == PathBrush::Edit;
}

/* The keys selected on the paths traced. */
static std::vector<std::pair<MotionPathTarget, int>> path_selected_keys(Context &C)
{
  return motion_path_selected_keys(*C.document, motion_path_targets(C));
}

/* The path handles picked whose keys are still there. */
static bool path_handles_picked(Context &C)
{
  if (!C.wm || !C.document) {
    return false;
  }
  for (const MotionPathHandlePick &p : C.wm->path_handle_picks) {
    if (motion_path_pick_valid(*C.document, p)) {
      return true;
    }
  }
  return false;
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

/* L and Shift L: the keys of the stretch of `target`'s path on `frame` (on a key, the stretches either side of it;
 * with `end`, the one ending on that key), breakdowns in it too, and its control points, picked with the keys and
 * control points picked already, or let go of (`deselect`). Path handles and dots picked go: they are picked on their
 * own. */
static OpStatus path_stretch_select(Context &C, const MotionPathTarget &target, int frame, int end, bool deselect)
{
  Document &doc = *C.document;
  Layer *layer = layer_find(doc, target.layer);
  if (!layer) {
    return OpStatus::Cancelled;
  }
  const bool on_key = !end && motion_path_is_key(doc, target, frame);
  std::vector<MotionPathStretch> stretches;
  for (const MotionPathStretch &s : motion_path_stretches(doc, target)) {
    if (end ? s.end == end : on_key ? s.start == frame || s.end == frame : frame > s.start && frame < s.end) {
      stretches.push_back(s);
    }
  }
  if (stretches.empty()) {
    report(ReportType::Info, "No stretch between two keys there");
    return OpStatus::Cancelled;
  }
  PathToolSync sync{C};
  std::vector<MotionPathHandlePick> &picks = C.wm->path_handle_picks;
  picks.erase(std::remove_if(picks.begin(), picks.end(),
                             [](const MotionPathHandlePick &p) { return p.control < 0 || p.control_handle != 0; }),
              picks.end());
  C.wm->path_dots.clear();
  for (const MotionPathStretch &s : stretches) {
    layers_select_keys({layer}, s.start, s.end, !deselect, motion_path_key_filter(doc, target));
    /* Each key in it keeps the control points of the way arriving at it. */
    for (int f = s.start + 1; f <= s.end; f++) {
      for (int i = 0;; i++) {
        const MotionPathHandlePick p{target, f, false, i};
        if (!motion_path_pick_valid(doc, p)) {
          break;
        }
        const auto it = std::find(picks.begin(), picks.end(), p);
        if (deselect && it != picks.end()) {
          picks.erase(it);
        }
        else if (!deselect && it == picks.end()) {
          picks.push_back(p);
        }
      }
    }
  }
  C.wm->path_timing_target = target;
  C.wm->path_timing_end = stretches.front().end;
  C.wm->path_timing_frame = doc.frame_current;
  C.undo->mark_changed();
  return OpStatus::Finished;
}

/* The Path Timing graph's Edit mode picks: the stretch they're on (its thing, a frame inside it, its curve and frames)
 * and those still on its curve. */
static bool timing_picked(Context &C, MotionPathTarget &r_target, int &r_frame, TimingCurve &r_curve, int &r_n,
                          std::vector<TimingPick> &r_picks)
{
  WindowManager &wm = *C.wm;
  Document &doc = *C.document;
  animation_sync(doc);
  r_target = wm.path_timing_point_target;
  r_frame = wm.path_timing_point_end - 1;
  int start = 0;
  r_picks.clear();
  if (!wm.path_timing_picks.empty() && motion_path_timing_curve(doc, r_target, r_frame, r_curve, &start, &r_n)) {
    if (r_curve.empty()) {
      r_curve = timing_even();
    }
    for (const TimingPick &k : wm.path_timing_picks) {
      if (k.point >= 0 && size_t(k.point) < r_curve.points.size()) {
        r_picks.push_back(k);
      }
    }
  }
  if (r_picks.empty()) {
    report(ReportType::Warning, "Pick points or handles of the timing curve first");
    return false;
  }
  return true;
}

/* Whether the graph under the mouse is the one its picks are on, and it is in Edit mode (G R S and V's poll). */
static bool timing_picks_here(Context &C)
{
  if (!C.document || !C.wm) {
    return false;
  }
  const WindowManager &wm = *C.wm;
  return !wm.tool_settings.path_graph_sculpt && wm.path_graph_hovered && !wm.path_timing_picks.empty() &&
         wm.path_timing_point_target.same(wm.path_graph_target) && wm.path_timing_point_end == wm.path_graph_end;
}

/* Picks a stretch's points and handles (the stretch's thing `t`, ending at `end`, its curve `curve`): action "ALL" or
 * "NONE"; else point `index` (part: 0 the point, -1 or 1 a handle; `extend` toggles it in or out); or a box (x 0-1 of
 * its time, y the way along) picking the points and handle tips inside, `mode` SET, ADD or SUB. */
static void timing_select_apply(WindowManager &wm, const MotionPathTarget &t, int end, const TimingCurve &curve,
                                const std::string &action, int index, int part, bool extend, const float *box,
                                const std::string &mode)
{
  if (!wm.path_timing_point_target.same(t) || wm.path_timing_point_end != end) {
    wm.path_timing_picks.clear();
    wm.path_timing_point = -1;
  }
  wm.path_timing_point_target = t;
  wm.path_timing_point_end = end;
  std::vector<TimingPick> &picks = wm.path_timing_picks;
  const size_t count = curve.points.size();
  const auto has = [&](TimingPick k) { return std::find(picks.begin(), picks.end(), k) != picks.end(); };
  const auto take = [&](TimingPick k) { picks.erase(std::remove(picks.begin(), picks.end(), k), picks.end()); };
  if (action == "NONE") {
    picks.clear();
    wm.path_timing_point = -1;
    return;
  }
  if (action == "ALL") {
    picks.clear();
    for (size_t i = 1; i + 1 < count; i++) {
      picks.push_back({int(i), 0});
    }
    if (count >= 2) {
      picks.push_back({0, 1});
      picks.push_back({int(count) - 1, -1});
    }
    return;
  }
  if (box) {
    const float x0 = std::min(box[0], box[2]), x1 = std::max(box[0], box[2]);
    const float y0 = std::min(box[1], box[3]), y1 = std::max(box[1], box[3]);
    const auto in_box = [&](float2 p) { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; };
    std::vector<TimingPick> found;
    for (size_t i = 0; i < count; i++) {
      const float2 co = curve.points[i].co;
      if (i > 0 && i + 1 < count && in_box(co)) {
        found.push_back({int(i), 0});
      }
      float2 in, out;
      timing_point_handles(curve, i, in, out);
      if (i > 0 && in_box(co + in)) {
        found.push_back({int(i), -1});
      }
      if (i + 1 < count && in_box(co + out)) {
        found.push_back({int(i), 1});
      }
    }
    if (mode == "SET") {
      picks.clear();
    }
    for (const TimingPick &k : found) {
      if (mode == "SUB") {
        take(k);
      }
      else if (!has(k)) {
        picks.push_back(k);
      }
    }
    if (std::none_of(picks.begin(), picks.end(), [&](const TimingPick &k) { return k.point == wm.path_timing_point; }))
    {
      wm.path_timing_point = -1;
      for (const TimingPick &k : picks) {
        if (k.part == 0) {
          wm.path_timing_point = k.point;
        }
      }
    }
    return;
  }
  if (index < 0 || size_t(index) >= count) {
    return;
  }
  const TimingPick k{index, part};
  if (extend && has(k)) {
    take(k);
    if (wm.path_timing_point == index &&
        std::none_of(picks.begin(), picks.end(), [&](const TimingPick &o) { return o.point == index; }))
    {
      wm.path_timing_point = -1;
    }
    return;
  }
  if (!extend) {
    picks.clear();
  }
  if (!has(k)) {
    picks.push_back(k);
  }
  wm.path_timing_point = index;
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
                     "colour (\"\" the usual one); head=1 with bone= names the bone's head path, else its tip's";
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
      const std::vector<MotionPathTarget> targets = motion_path_targets(C);
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
                     "extend=1 (Shift) adds it to the selection, or takes it out if it was in (a key keeping the control "
                     "points picked, a dot letting them go); go=1 (double click) "
                     "also goes to that frame; none=1 deselects every key and dot";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      /* Frame 0 only for its key, the setup's: picked for its out handle, never a dot to slide. */
      const int frame = std::max(0, op_arg_int(C, "frame", doc.frame_current));
      PathToolSync sync{C};
      std::vector<std::pair<MotionPathTarget, int>> &dots = C.wm->path_dots;
      const bool none = op_arg_bool(C, "none");
      /* Keys and dots are never picked with path handles. Shift on a key keeps the control points picked: keys and
       * control points go together (as L picks a stretch's); on a dot they go too. */
      std::vector<MotionPathHandlePick> &picks = C.wm->path_handle_picks;
      const bool on_key = std::any_of(targets.begin(), targets.end(), [&](const MotionPathTarget &t) {
        return layer_find(doc, t.layer) && motion_path_is_key(doc, t, frame);
      });
      if (op_arg_bool(C, "extend") && !none && on_key) {
        picks.erase(std::remove_if(picks.begin(), picks.end(),
                                   [](const MotionPathHandlePick &p) { return p.control < 0 || p.control_handle != 0; }),
                    picks.end());
      }
      else {
        picks.clear();
      }
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
        if (frame == 0) {
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
    ot.idname = "anim.motion_path_handle_select";
    ot.name = "Select Path Handle";
    ot.description = "Pick a path handle, as clicking one with the Motion Path tool's Edit does: of the key on "
                     "frame=N of what a path traces (layer=id with bone=name or point=id: that one; else the first "
                     "traced), out=1 the one toward the next key; or control=I, that control point of the stretch "
                     "ending on frame=N (control_handle=1 its in handle, 2 its out). On its own: every other path handle, control point, key and dot "
                     "is let go (the bones and points traced stay, as they make the paths show); extend=1 (Shift) "
                     "adds it to the handles picked or takes it out (a control point keeping the keys selected); none=1 "
                     "lets go of every path handle";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      PathToolSync picked{C};
      std::vector<MotionPathHandlePick> &picks = C.wm->path_handle_picks;
      if (op_arg_bool(C, "none")) {
        picks.clear();
        C.undo->mark_changed();
        return OpStatus::Finished;
      }
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      MotionPathHandlePick pick{targets.front(), op_arg_int(C, "frame", doc.frame_current), op_arg_bool(C, "out"),
                                op_arg_int(C, "control", -1)};
      if (pick.control >= 0) {
        pick.out = false;
        pick.control_handle = std::clamp(op_arg_int(C, "control_handle", 0), 0, 2);
      }
      if (!motion_path_pick_valid(doc, pick)) {
        report(ReportType::Warning, pick.control >= 0 ? "No such control point" : "No position key with path handles there");
        return OpStatus::Cancelled;
      }
      /* Whatever else is picked on the paths goes: a handle is picked alone or with other handles. A control point
       * with Shift keeps the keys selected (keys and control points go together, as L picks a stretch's), and lets go
       * of the handles picked with them. */
      const bool with_keys = op_arg_bool(C, "extend") && pick.control >= 0 && pick.control_handle == 0 &&
                             !path_selected_keys(C).empty();
      {
        PathToolSync sync{C};
        if (with_keys) {
          picks.erase(std::remove_if(picks.begin(), picks.end(),
                                     [](const MotionPathHandlePick &p) { return p.control < 0 || p.control_handle != 0; }),
                      picks.end());
        }
        else {
          std::vector<Layer *> all;
          for (const Layer *l : document_layers_flat(doc)) {
            all.push_back(layer_find(doc, l->id));
          }
          layers_deselect_keys(all);
        }
        C.wm->path_dots.clear();
      }
      const auto it = std::find(picks.begin(), picks.end(), pick);
      if (!op_arg_bool(C, "extend")) {
        picks = {pick};
      }
      else if (it != picks.end()) {
        picks.erase(it);
      }
      else {
        picks.push_back(pick);
      }
      C.wm->path_timing_target = pick.target;
      C.undo->mark_changed();
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_select_linked";
    ot.name = "Select Path Stretch";
    ot.description = "Pick the keys either end of the stretch of a path (layer=id with bone=name or point=id: what it "
                     "traces, else the first traced) on frame=N, and its control points, with what is picked already "
                     "(on a key: the stretches either side; end=N the one ending on that key); deselect=1 lets go of "
                     "them. Path handles and dots picked are let go";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      return path_stretch_select(C, targets.front(), op_arg_int(C, "frame", C.document->frame_current),
                                 op_arg_int(C, "end", 0), op_arg_bool(C, "deselect"));
    };
    operators().add(std::move(ot));
  }
  for (const bool deselect : {false, true}) {
    OperatorType ot;
    ot.idname = deselect ? "anim.motion_path_deselect_linked_pick" : "anim.motion_path_select_linked_pick";
    ot.name = deselect ? "Deselect Path Stretch" : "Select Path Stretch Under Mouse";
    ot.description = deselect ? "Shift L with the Motion Path tool's Edit: let go of the keys and control points of the "
                                "stretch of the path under the mouse"
                              : "L with the Motion Path tool's Edit: pick the keys either end of the stretch of the path "
                                "under the mouse and its control points, with what is picked already (on a key: the "
                                "stretches either side)";
    ot.flag = OPTYPE_REGISTER;
    /* Off the paths, L is the drawing's Select Linked. */
    ot.poll = [](Context &C) { return path_edit_poll(C) && C.wm->path_under; };
    ot.exec = [deselect](Context &C) {
      const WindowManager &wm = *C.wm;
      return path_stretch_select(C, wm.path_under_target, wm.path_under_frame, wm.path_under_end, deselect);
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_deselect_all";
    ot.name = "Deselect Path Picks";
    ot.description = "Alt A with the Motion Path tool's Edit and something picked on the paths: let go of the path "
                     "keys, dots, handles and control points picked, the bones and points traced staying (with "
                     "nothing on the paths picked, Alt A deselects those)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) {
      return path_edit_poll(C) && (!path_selected_keys(C).empty() || !C.wm->path_dots.empty() ||
                                   path_handles_picked(C));
    };
    ot.exec = [](Context &C) {
      PathToolSync sync{C};
      std::vector<Layer *> all;
      for (const Layer *l : document_layers_flat(*C.document)) {
        all.push_back(layer_find(*C.document, l->id));
      }
      layers_deselect_keys(all);
      C.wm->path_dots.clear();
      C.wm->path_handle_picks.clear();
      C.wm->path_timing_end = 0;
      C.undo->mark_changed();
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_handles_reset";
    ot.name = "Reset Path Handles";
    ot.description = "X with path handles or control points picked (the Motion Path tool's Edit): the handles' keys "
                     "back to Auto, the control points deleted (the path as it was without them; keys picked with them "
                     "deleted too), a control point's handles picked worked out smooth again";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return path_edit_poll(C) && path_handles_picked(C); };
    ot.exec = [](Context &C) {
      animation_sync(*C.document);
      std::vector<MotionPathHandlePick> &picks = C.wm->path_handle_picks;
      const int smooth = motion_path_control_handles_reset(*C.document, picks);
      const int controls = motion_path_controls_delete(*C.document, picks);
      const int handles = motion_path_handles_type_at(*C.document, picks, PathHandles::Auto);
      if (controls) {
        /* The control points after a deleted one are numbered again. */
        picks.erase(std::remove_if(picks.begin(), picks.end(), [](const auto &p) { return p.control >= 0; }),
                    picks.end());
      }
      /* Keys picked with the control points (L, Shift click): deleted with them. */
      int keys = 0;
      if (controls && smooth + handles == 0 && !path_selected_keys(C).empty()) {
        animation_record(*C.document);
        keys = motion_path_keys_delete(*C.document, motion_path_targets(C));
      }
      if (controls + handles + smooth + keys == 0) {
        report(ReportType::Info, "The handles are Auto already");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_control_add";
    ot.name = "Add Path Control Point";
    ot.description = "Alt click on a path with the Motion Path tool's Edit: a control point where the path is on "
                     "frame=N (between two of its position keys), shaping the path only: no key, the timing as it "
                     "was. It is picked on its own";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      int end = 0, index = 0;
      if (!motion_path_control_add(doc, targets.front(), op_arg_int(C, "frame", doc.frame_current), &end, &index)) {
        report(ReportType::Warning, "Control points go on a path with handles, between two of its position keys, where the "
                                               "path is (not where a dot has gone past its key)");
        return OpStatus::Cancelled;
      }
      PathToolSync sync{C};
      std::vector<Layer *> all;
      for (const Layer *l : document_layers_flat(doc)) {
        all.push_back(layer_find(doc, l->id));
      }
      layers_deselect_keys(all);
      C.wm->path_dots.clear();
      C.wm->path_handle_picks = {MotionPathHandlePick{targets.front(), end, false, index}};
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_control_move";
    ot.name = "Move Path Control Point";
    ot.description = "Put control point index=I of the stretch ending on frame=N at x, y (document space, as seen on "
                     "the frame of the dot nearest it), as dragging it does";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      const int end = op_arg_int(C, "frame", 0), index = op_arg_int(C, "index", 0);
      /* (Its handles' tips aren't wanted: each costs two more frames worked out.) */
      for (const MotionPathControl &c :
           motion_path_controls(doc, targets.front(), [](const MotionPathControl &) { return false; }))
      {
        if (c.end == end && c.index == index &&
            motion_path_control_set(doc, targets.front(), end, index, c.near_frame,
                                    float2{op_arg_float(C, "x", c.at.x), op_arg_float(C, "y", c.at.y)}))
        {
          return OpStatus::Finished;
        }
      }
      report(ReportType::Warning, "No such control point");
      return OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_control_handle_move";
    ot.name = "Move Path Control Point Handle";
    ot.description = "Put the tip of a handle of control point index=I of the stretch ending on frame=N (out=1 the one "
                     "toward that key) at x, y (document space, as seen on the frame of the dot nearest it), as "
                     "dragging it does: aligned, the other turns to stay in line; alone=1 (Alt) moves just this one "
                     "and unaligns the point";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      const int end = op_arg_int(C, "frame", 0), index = op_arg_int(C, "index", 0);
      const bool out = op_arg_bool(C, "out");
      for (const MotionPathControl &c : motion_path_controls(doc, targets.front(), [&](const MotionPathControl &c) {
             return c.end == end && c.index == index;
           }))
      {
        const float2 tip = out ? c.out_tip : c.in_tip;
        if (c.end == end && c.index == index &&
            motion_path_control_handle_set(doc, targets.front(), end, index, out, c.near_frame,
                                           float2{op_arg_float(C, "x", tip.x), op_arg_float(C, "y", tip.y)},
                                           op_arg_bool(C, "alone")))
        {
          return OpStatus::Finished;
        }
      }
      report(ReportType::Warning, "No such control point");
      return OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_controls_align";
    ot.name = "Align Control Point Handles";
    ot.description = "The control points picked (or whose handles are), as a vector point's Align Handles and Unalign "
                     "Handles: aligned=1 puts their handles in line (a handle picked alone turns to the other; the "
                     "point, both halfway) so moving one turns the other; aligned=0 lets each go its own way";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) {
      return C.document && C.wm &&
             std::any_of(C.wm->path_handle_picks.begin(), C.wm->path_handle_picks.end(),
                         [&](const MotionPathHandlePick &p) {
                           return p.control >= 0 && motion_path_pick_valid(*C.document, p);
                         });
    };
    ot.exec = [](Context &C) {
      animation_sync(*C.document);
      if (motion_path_controls_align(*C.document, C.wm->path_handle_picks, op_arg_bool(C, "aligned")) == 0) {
        report(ReportType::Info, "Nothing to change");
        return OpStatus::Cancelled;
      }
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
                     "before to the key after, staying on its arc; the key made is a breakdown";
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
      motion_path_key_type_set(doc, targets.front(), doc.frame_current, KeyType::Breakdown);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_stretch_reset";
    /* Its shape alone keeps the timing: nothing for the linked channels. */
    ot.timing = [](Context &C) { return !op_has_arg(C, "what") || op_arg(C, "what") != "SHAPE"; };
    ot.name = "Reset Stretch";
    ot.description = "Right-click a path dot or control point: its stretch (frame=N in it, after its first key and up to "
                     "its last: a control point's by its key's frame) "
                     "made a straight line at an even speed; what=SHAPE only straightens it (its timing kept), "
                     "what=TIMING only evens its speed (its shape kept). Its control points go; the keys stay on their "
                     "frames and where they are, the stretches beside them as they were. Of what a path traces "
                     "(layer=, bone=, point=, drawing=1, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      const std::string what = op_has_arg(C, "what") ? op_arg(C, "what") : "BOTH";
      if (what != "BOTH" && what != "SHAPE" && what != "TIMING") {
        report(ReportType::Error, "what= is one of BOTH, SHAPE, TIMING");
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      if (!motion_path_stretch_reset(doc, targets.front(), op_arg_int(C, "frame", doc.frame_current), what != "TIMING",
                                     what != "SHAPE"))
      {
        report(ReportType::Warning, "Nothing to reset there");
        return OpStatus::Cancelled;
      }
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
      if (!motion_path_range(doc, motion_path_targets(C), first, last)) {
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
    ot.description = "Pick the Motion Path tool's brush (brush=EDIT, GRAB, SMOOTH, BLEND_SPEED, PINCH or SLIDE), "
                     "Edit Layer (edit_layer=0/1: the layer itself, not its bones and points), and set that "
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
      if (path_brush_is_sculpt(sized) && op_has_arg(C, "brush_falloff") &&
          !sculpt_falloff_from_id(op_arg(C, "brush_falloff"), &bs.falloff))
      {
        report(ReportType::Error, "brush_falloff= is one of SMOOTH, SPHERE, CONSTANT");
        return OpStatus::Cancelled;
      }
      if (sized == PathBrush::SurfaceSmooth && op_has_arg(C, "sensitivity")) {
        bs.sensitivity = std::max(op_arg_float(C, "sensitivity", bs.sensitivity), 0.0f);
      }
      if (op_has_arg(C, "edit_layer")) {
        if (ts.path_edit_layer != op_arg_bool(C, "edit_layer")) {
          ts.path_edit_layer = !ts.path_edit_layer;
          /* Other paths now: the dots and handles picked on the old ones let go. */
          C.wm->path_dots.clear();
          C.wm->path_handle_picks.clear();
        }
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
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    /* Only the timing brushes retime: a shape brush keeps the timing (its Custom speeds are the old timing kept), so
     * the linked channels stay as they are. */
    ot.timing = [](Context &C) {
      PathBrush brush;
      return stroke_brush(C, brush) && path_brush_is_timing(brush);
    };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const ToolSettings &ts = C.wm->tool_settings;
      PathBrush brush;
      if (!stroke_brush(C, brush)) {
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
          if (end_y != rest + 1 && std::isfinite(x) && std::isfinite(y)) {
            points.push_back({x, y});
          }
        }
        at = end + 1;
      }
      std::vector<MotionPathTarget> targets;
      if (points.empty() || !op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      if (path_brush_is_sculpt(brush)) {
        /* A drawing's path is its points' timing, not a place of its own: there is nothing there to reshape. */
        std::erase_if(targets, [](const MotionPathTarget &t) { return t.drawing; });
        if (targets.empty()) {
          return OpStatus::Cancelled;
        }
      }
      int first = 0, last = 0;
      if (!motion_path_range(doc, motion_path_targets(C), first, last) &&
          !motion_path_range(doc, targets, first, last))
      {
        return OpStatus::Cancelled;
      }
      animation_sync(doc);
      PathDab dab;
      dab.radius = std::max(op_arg_float(C, "radius", 0.1f), 0.0f);
      /* As the brush's own setting (brush_set) is. */
      dab.strength = std::clamp(op_arg_float(C, "strength", ts.path_brush_settings(brush).strength), 0.0f, 1.0f);
      dab.invert = op_arg_bool(C, "invert");
      int changed = 0;
      if (path_brush_is_sculpt(brush)) {
        /* The Sculpt tool's stroke along the points on the paths that can take it; the others (a bone turning its
         * tip) by their keys, as before. */
        PathBrushSettings bs = ts.path_brush_settings(brush);
        bs.strength = dab.strength;
        if (op_has_arg(C, "brush_falloff") && !sculpt_falloff_from_id(op_arg(C, "brush_falloff"), &bs.falloff)) {
          report(ReportType::Error, "brush_falloff= is one of SMOOTH, SPHERE, CONSTANT");
          return OpStatus::Cancelled;
        }
        if (op_has_arg(C, "sensitivity")) {
          bs.sensitivity = op_arg_float(C, "sensitivity", bs.sensitivity);
        }
        std::vector<MotionPathTarget> own, other;
        for (const MotionPathTarget &t : targets) {
          (motion_path_sculptable(doc, t) ? own : other).push_back(t);
        }
        SculptDab sd = motion_path_sculpt_dab_make(brush, bs, points.front(), dab.radius);
        sd.amount = op_arg_float(C, "amount", 1.0f);
        PathSculptStroke stroke = motion_path_sculpt_begin(doc, own, first, last, sd);
        const std::vector<PathGrabKey> keys = brush == PathBrush::Grab ? motion_path_grab_keys(
                                                                             doc, other, first, last, points.front(), dab.radius)
                                                                       : std::vector<PathGrabKey>();
        for (size_t i = 0; i < points.size(); i++) {
          sd.move = i > 0 ? points[i] - points[i - 1] : float2();
          sd.center = points[i];
          if (brush == PathBrush::Grab && i == 0) {
            continue;
          }
          changed = std::max(changed, motion_path_sculpt_dab(doc, stroke, sd));
          if (brush == PathBrush::Smooth && !other.empty()) {
            dab.center = points[i];
            changed += motion_path_smooth_dab(doc, other, first, last, dab);
          }
        }
        if (!keys.empty()) {
          changed += motion_path_grab(doc, keys, points.back() - points.front(), dab.strength);
        }
      }
      else if (brush == PathBrush::Slide) {
        /* The dot nearest the first point slides to the last, the dots inside the circle going with it. */
        MotionPathTarget t;
        int frame = 0, reach = 1;
        float2 offset;
        if (motion_path_slide_pick(doc, targets, first, last, points.front(), dab.radius, t, frame, reach, offset)) {
          PathSlide slide;
          slide.proportional = true;
          slide.size = float(reach);
          changed = motion_path_slide(doc, t, frame, points.back() - offset, slide) ? 1 : 0;
        }
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
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
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
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
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
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
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
  /* The Path Timing graph's Edit and Sculpt modes: the curve of the stretch frame=N is strictly inside (of its first
   * traced thing, or layer=, bone=, point=, drawing=1), straight while even. */
  const auto timing_curve_of = [](Context &C, MotionPathTarget &r_target, int &r_frame, TimingCurve &r_curve,
                                  int *r_start, int *r_frames) {
    std::vector<MotionPathTarget> targets;
    if (!op_targets(C, targets)) {
      return false;
    }
    Document &doc = *C.document;
    animation_sync(doc);
    r_target = targets.front();
    r_frame = op_arg_int(C, "frame", doc.frame_current);
    if (!motion_path_timing_curve(doc, r_target, r_frame, r_curve, r_start, r_frames)) {
      report(ReportType::Warning, "Only between keys");
      return false;
    }
    if (r_curve.empty()) {
      r_curve = timing_even();
    }
    return true;
  };
  const auto timing_point_of = [](Context &C, const TimingCurve &curve, bool ends, size_t &r_index) {
    const int i = op_arg_int(C, "index", -1);
    if (i < 0 || size_t(i) >= curve.points.size()) {
      report(ReportType::Warning,
             "index= is one of the curve's points, 0 to " + std::to_string(curve.points.size() - 1));
      return false;
    }
    if (!ends && (i == 0 || size_t(i) + 1 == curve.points.size())) {
      report(ReportType::Warning, "The curve's ends are the keys");
      return false;
    }
    r_index = size_t(i);
    return true;
  };
  {
    OperatorType ot;
    ot.idname = "anim.timing_point_move";
    ot.name = "Move Timing Point";
    ot.description = "The Path Timing graph's Edit mode: point index=N of the timing curve of the stretch frame= is in "
                     "moved to x= (0-1 of the way through its time) and y= (how far along: 1 at its key), its handles "
                     "coming with it; kept between its neighbours in time. Of what a path traces (layer=, bone=, "
                     "point=, drawing=1, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [timing_curve_of, timing_point_of](Context &C) {
      MotionPathTarget t;
      int frame = 0;
      TimingCurve curve;
      size_t i = 0;
      if (!timing_curve_of(C, t, frame, curve, nullptr, nullptr) || !timing_point_of(C, curve, false, i)) {
        return OpStatus::Cancelled;
      }
      const float2 was = curve.points[i].co;
      timing_point_move(curve, i, {op_arg_float(C, "x", was.x), op_arg_float(C, "y", was.y)});
      if (!motion_path_timing_curve_set(*C.document, t, frame, curve)) {
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_handle_move";
    ot.name = "Move Timing Handle";
    ot.description = "The Path Timing graph's Edit mode: the side= IN or OUT handle of point index=N of the timing "
                     "curve of the stretch frame= is in set to the offset x=, y= from its point, as a drawing's handle: "
                     "an aligned point's other handle turns to stay in line unless free=1 (Alt drag), which breaks the point. Of what a path "
                     "traces (layer=, bone=, point=, drawing=1, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [timing_curve_of, timing_point_of](Context &C) {
      MotionPathTarget t;
      int frame = 0;
      TimingCurve curve;
      size_t i = 0;
      if (!timing_curve_of(C, t, frame, curve, nullptr, nullptr) || !timing_point_of(C, curve, true, i)) {
        return OpStatus::Cancelled;
      }
      const std::string side = op_arg(C, "side", "OUT");
      if (side != "IN" && side != "OUT") {
        report(ReportType::Warning, "side= is IN or OUT");
        return OpStatus::Cancelled;
      }
      const bool out = side == "OUT";
      if (out ? i + 1 == curve.points.size() : i == 0) {
        report(ReportType::Warning,
               out ? "The last point has only its in handle" : "The first point has only its out handle");
        return OpStatus::Cancelled;
      }
      const float2 was = out ? curve.points[i].out : curve.points[i].in;
      timing_handle_set(curve, i, out, {op_arg_float(C, "x", was.x), op_arg_float(C, "y", was.y)},
                        op_arg_bool(C, "free"));
      if (!motion_path_timing_curve_set(*C.document, t, frame, curve)) {
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_point_add";
    ot.name = "Add Timing Point";
    ot.description = "The Path Timing graph's Edit mode (Ctrl click): a point put on the timing curve of the stretch "
                     "frame= is in at x= (0-1 of the way through its time), the curve keeping its shape; with y= it "
                     "goes there, and with to_x= to that time (Alt drag on the curve: put there and dragged). Of what a path traces (layer=, bone=, point=, drawing=1, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [timing_curve_of](Context &C) {
      MotionPathTarget t;
      int frame = 0;
      TimingCurve curve;
      if (!timing_curve_of(C, t, frame, curve, nullptr, nullptr)) {
        return OpStatus::Cancelled;
      }
      const int i = timing_point_insert(curve, op_arg_float(C, "x", -1.0f));
      if (i < 0) {
        report(ReportType::Warning, "x= is between the curve's points, inside 0-1");
        return OpStatus::Cancelled;
      }
      if (op_has_arg(C, "y") || op_has_arg(C, "to_x")) {
        /* A y= or to_x= that isn't a number leaves the point on the curve there, as without one. */
        const float2 on = curve.points[size_t(i)].co;
        timing_point_move(curve, size_t(i), {op_arg_float(C, "to_x", on.x), op_arg_float(C, "y", on.y)});
      }
      if (!motion_path_timing_curve_set(*C.document, t, frame, curve)) {
        return OpStatus::Cancelled;
      }
      if (C.wm) {
        /* The new point picked, alone. */
        int start = 0, n = 0;
        TimingCurve now;
        motion_path_timing_curve(*C.document, t, frame, now, &start, &n);
        C.wm->path_timing_point_target = t;
        C.wm->path_timing_point_end = start + n;
        C.wm->path_timing_picks = {{i, 0}};
        C.wm->path_timing_point = i;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_point_delete";
    ot.name = "Delete Timing Point";
    ot.description = "The Path Timing graph's Edit mode (X): point index=N taken out of the timing curve of the stretch "
                     "frame= is in (never an end: those are the keys); without index=, the points picked in the graph. "
                     "Of what a path traces (layer=, bone=, point=, drawing=1, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [timing_curve_of, timing_point_of](Context &C) {
      MotionPathTarget t;
      int frame = 0;
      TimingCurve curve;
      size_t i = 0;
      WindowManager &wm = *C.wm;
      std::vector<size_t> gone;
      if (!op_has_arg(C, "index")) {
        /* X over the graph: the points picked there (the ends are the keys: never). */
        Document &doc = *C.document;
        animation_sync(doc);
        t = wm.path_timing_point_target;
        frame = wm.path_timing_point_end - 1;
        if (!motion_path_timing_curve(doc, t, frame, curve) || curve.empty()) {
          report(ReportType::Warning, "Pick a point of the timing curve first");
          return OpStatus::Cancelled;
        }
        for (const TimingPick &k : wm.path_timing_picks) {
          if (k.part == 0 && k.point > 0 && size_t(k.point) + 1 < curve.points.size()) {
            gone.push_back(size_t(k.point));
          }
        }
        if (gone.empty()) {
          report(ReportType::Warning, wm.path_timing_picks.empty() ? "Pick a point of the timing curve first"
                                                                    : "The curve's ends are the keys");
          return OpStatus::Cancelled;
        }
      }
      else if (!timing_curve_of(C, t, frame, curve, nullptr, nullptr) || !timing_point_of(C, curve, false, i)) {
        return OpStatus::Cancelled;
      }
      else {
        gone.push_back(i);
      }
      std::sort(gone.begin(), gone.end());
      gone.erase(std::unique(gone.begin(), gone.end()), gone.end());
      for (size_t g = gone.size(); g-- > 0;) {
        timing_point_remove(curve, gone[g]);
      }
      if (!motion_path_timing_curve_set(*C.document, t, frame, curve)) {
        return OpStatus::Cancelled;
      }
      wm.path_timing_point = -1;
      wm.path_timing_picks.clear();
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_decimate";
    ot.name = "Decimate";
    ot.description = "The Path Timing graph's Edit mode (its right click menu): take out the timing curve's points the "
                     "curve changes least without, one at a time (the handles either side fitted again), until ratio= "
                     "(0-1; default: the one last used) of those between its ends is left, on the stretch frame= is in. "
                     "Of what a path traces (layer=, bone=, point=, drawing=1, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [timing_curve_of](Context &C) {
      ToolSettings &ts = C.wm->tool_settings;
      const float ratio = op_arg_float(C, "ratio", ts.timing_decimate_ratio);
      if (!(ratio >= 0.0f && ratio <= 1.0f)) {
        report(ReportType::Error, "ratio= is the share of the curve's points to keep, 0 to 1");
        return OpStatus::Cancelled;
      }
      char used[32];
      std::snprintf(used, sizeof(used), "%g", double(ratio));
      op_arg_used(C, "ratio", used);
      MotionPathTarget t;
      int frame = 0, start = 0, n = 0;
      TimingCurve curve;
      if (!timing_curve_of(C, t, frame, curve, &start, &n)) {
        return OpStatus::Cancelled;
      }
      const int inside = curve.empty() ? 0 : int(curve.points.size()) - 2;
      if (inside <= 0) {
        report(ReportType::Warning, "The curve has no points between its ends to take out");
        return OpStatus::Cancelled;
      }
      ts.timing_decimate_ratio = ratio;
      /* Finished even when nothing goes (a Ratio of 1), so raising it in the Adjust Last Operation panel puts back the
       * points a lower one took out. */
      const int gone = timing_decimate(curve, n, ratio);
      if (gone > 0 && !motion_path_timing_curve_set(*C.document, t, frame, curve)) {
        return OpStatus::Cancelled;
      }
      C.wm->path_timing_point = -1;
      C.wm->path_timing_picks.clear();
      const std::string count = std::to_string(gone) + " of " + std::to_string(inside) +
                                (inside == 1 ? " point" : " points") + " removed";
      op_note(count);
      report(ReportType::Info, "Decimate: " + count);
      return OpStatus::Finished;
    };
    ot.props = {op_float_property("ratio", "Ratio",
                                  "The share of the curve's points to keep: 1 keeps them all. The ones the curve "
                                  "changes least without go first",
                                  "0.5", 0.0f, 1.0f, 0.01f)};
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_select";
    ot.name = "Select Timing Points";
    ot.description = "The Path Timing graph's Edit mode: pick point index=N of the timing curve of the stretch frame= is "
                     "in (part= POINT, or IN or OUT for one of its handles alone; extend=1 toggles it, as Shift click); "
                     "action=ALL picks every point and the ends' handles, NONE none; box=\"x0,y0,x1,y1\" (0-1 of its "
                     "time, the way along) the points and handle tips inside it, mode= SET, ADD or SUB. Of what a path "
                     "traces (layer=, bone=, point=, drawing=1, or the first traced); with none of those nor frame=, "
                     "the graph under the mouse";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [timing_curve_of](Context &C) {
      WindowManager &wm = *C.wm;
      MotionPathTarget t;
      int frame = 0, start = 0, n = 0;
      TimingCurve curve;
      const bool given = op_has_arg(C, "frame") || op_has_arg(C, "layer") || op_has_arg(C, "bone") ||
                         op_has_arg(C, "point") || op_has_arg(C, "drawing");
      if (given) {
        if (!timing_curve_of(C, t, frame, curve, &start, &n)) {
          return OpStatus::Cancelled;
        }
      }
      else {
        animation_sync(*C.document);
        t = wm.path_graph_target;
        frame = wm.path_graph_end - 1;
        if (!motion_path_timing_curve(*C.document, t, frame, curve, &start, &n)) {
          report(ReportType::Warning, "Over the Path Timing graph only");
          return OpStatus::Cancelled;
        }
        if (curve.empty()) {
          curve = timing_even();
        }
      }
      const std::string action = op_arg(C, "action", "");
      if (!action.empty() && action != "ALL" && action != "NONE") {
        report(ReportType::Warning, "action= is ALL or NONE");
        return OpStatus::Cancelled;
      }
      const std::string part_id = op_arg(C, "part", "POINT");
      if (part_id != "POINT" && part_id != "IN" && part_id != "OUT") {
        report(ReportType::Warning, "part= is POINT, IN or OUT");
        return OpStatus::Cancelled;
      }
      const std::string mode = op_arg(C, "mode", "SET");
      if (mode != "SET" && mode != "ADD" && mode != "SUB") {
        report(ReportType::Warning, "mode= is SET, ADD or SUB");
        return OpStatus::Cancelled;
      }
      float box[4];
      bool has_box = false;
      if (op_has_arg(C, "box")) {
        const std::string text = op_arg(C, "box");
        const char *at = text.data(), *end = text.data() + text.size();
        int read = 0;
        for (; read < 4; read++) {
          const std::from_chars_result r = std::from_chars(at, end, box[read]);
          if (r.ec != std::errc()) {
            break;
          }
          at = r.ptr;
          if (read < 3) {
            if (at == end || *at != ',') {
              break;
            }
            at++;
          }
        }
        has_box = read == 4 && at == end;
        if (!has_box) {
          report(ReportType::Warning, "box= is \"x0,y0,x1,y1\"");
          return OpStatus::Cancelled;
        }
      }
      const int index = op_arg_int(C, "index", -1);
      if (action.empty() && !has_box && (index < 0 || size_t(index) >= curve.points.size())) {
        report(ReportType::Warning,
               "index= is one of the curve's points, 0 to " + std::to_string(curve.points.size() - 1));
        return OpStatus::Cancelled;
      }
      timing_select_apply(wm, t, start + n, curve, action, index, part_id == "IN" ? -1 : part_id == "OUT" ? 1 : 0,
                          op_arg_bool(C, "extend"), has_box ? box : nullptr, mode);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  for (const bool all : {true, false}) {
    OperatorType ot;
    ot.idname = all ? "anim.timing_select_all" : "anim.timing_deselect_all";
    ot.name = all ? "Select All Timing Points" : "Deselect All Timing Points";
    ot.description = all ? "A over the Path Timing graph in Edit mode: pick every point of its curve and the ends' handles"
                         : "Alt A over the Path Timing graph in Edit mode: pick none of its curve's points and handles";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) {
      return C.document && C.wm && C.wm->path_graph_hovered && !C.wm->tool_settings.path_graph_sculpt;
    };
    ot.exec = [all](Context &C) {
      WindowManager &wm = *C.wm;
      animation_sync(*C.document);
      TimingCurve curve;
      int start = 0, n = 0;
      if (!motion_path_timing_curve(*C.document, wm.path_graph_target, wm.path_graph_end - 1, curve, &start, &n)) {
        return OpStatus::Cancelled;
      }
      if (curve.empty()) {
        curve = timing_even();
      }
      timing_select_apply(wm, wm.path_graph_target, start + n, curve, all ? "ALL" : "NONE", -1, 0, false, nullptr,
                          "SET");
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_transform";
    ot.name = "Transform Timing Points";
    ot.description = "The Path Timing graph's Edit mode (G R S): the points and handles picked, mode=MOVE by x= (frames) "
                     "and y= (the way along: 1 is the whole way), ROTATE (each point's handles about it, handles picked "
                     "alone about their points) by angle= (degrees, anticlockwise as shown), SCALE by factor= (several "
                     "points: their places about their middle and their handles; one point: its handles; handles "
                     "picked: their lengths); axis=X (time only) or Y (the way along only) for SCALE; aspect= frames "
                     "a whole way along as the graph shows it (turns and lengths as seen there; default the stretch's "
                     "frames). The points stay in order, the ends never move";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      const std::string mode = op_arg(C, "mode", "MOVE");
      if (mode != "MOVE" && mode != "ROTATE" && mode != "SCALE") {
        report(ReportType::Warning, "mode= is MOVE, ROTATE or SCALE");
        return OpStatus::Cancelled;
      }
      const std::string axis = op_arg(C, "axis", "");
      if (!axis.empty() && axis != "X" && axis != "Y") {
        report(ReportType::Warning, "axis= is X or Y");
        return OpStatus::Cancelled;
      }
      MotionPathTarget t;
      int frame = 0, n = 0;
      TimingCurve curve;
      std::vector<TimingPick> picks;
      if (!timing_picked(C, t, frame, curve, n, picks)) {
        return OpStatus::Cancelled;
      }
      const float aspect = std::max(op_arg_float(C, "aspect", float(n)), 1e-3f);
      const float f = op_arg_float(C, "factor", 1.0f);
      const float2 factor{axis == "Y" ? 1.0f : f, axis == "X" ? 1.0f : f};
      const float2 offset{op_arg_float(C, "x", 0.0f) / float(std::max(n, 1)), op_arg_float(C, "y", 0.0f)};
      timing_transform(curve, picks, mode == "MOVE" ? 'G' : mode == "ROTATE" ? 'R' : 'S', offset,
                       op_arg_float(C, "angle", 0.0f) * 3.14159265f / 180.0f, factor, n, aspect);
      if (!motion_path_timing_curve_set(*C.document, t, frame, curve)) {
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_handles_type";
    ot.name = "Timing Handle Type";
    ot.description = "The Path Timing graph's Edit mode (V, or its right click menu): the handles of the points picked "
                     "(and of the points whose handles are picked), type=ALIGNED (in line, along the way between theirs "
                     "as shown), BROKEN (free of each other) or FLAT (level: the move eases into and out of the "
                     "point); aspect= frames a whole way along as the graph shows it (default the stretch's frames)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      const std::string type = op_arg(C, "type", "ALIGNED");
      if (type != "ALIGNED" && type != "BROKEN" && type != "FLAT") {
        report(ReportType::Warning, "type= is ALIGNED, BROKEN or FLAT");
        return OpStatus::Cancelled;
      }
      MotionPathTarget t;
      int frame = 0, n = 0;
      TimingCurve curve;
      std::vector<TimingPick> picks;
      if (!timing_picked(C, t, frame, curve, n, picks)) {
        return OpStatus::Cancelled;
      }
      std::vector<int> points;
      for (const TimingPick &k : picks) {
        if (std::find(points.begin(), points.end(), k.point) == points.end()) {
          points.push_back(k.point);
        }
      }
      timing_handles_type(curve, points,
                          type == "ALIGNED"  ? TimingHandles::Aligned
                          : type == "BROKEN" ? TimingHandles::Broken
                                             : TimingHandles::Flat,
                          n, std::max(op_arg_float(C, "aspect", float(n)), 1e-3f));
      if (!motion_path_timing_curve_set(*C.document, t, frame, curve)) {
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  for (const char kind : {'G', 'R', 'S'}) {
    OperatorType ot;
    ot.idname = kind == 'G' ? "anim.timing_grab" : kind == 'R' ? "anim.timing_rotate" : "anim.timing_scale";
    ot.name = kind == 'G' ? "Move Timing Points" : kind == 'R' ? "Rotate Timing Handles" : "Scale Timing Points";
    ot.description = kind == 'G' ? "G over the Path Timing graph in Edit mode: move the points and handles picked with "
                                   "the mouse (X or Y: in time or the way along only; numbers typed in frames and %; "
                                   "Ctrl snaps to whole frames and 5%)"
                     : kind == 'R' ? "R over the Path Timing graph in Edit mode: turn each picked point's handles "
                                     "about it, and handles picked about their points (Ctrl snaps to 5 degrees)"
                                   : "S over the Path Timing graph in Edit mode: scale the points picked about their "
                                     "middle, or one point's handles (X or Y: in time or the way along only; Ctrl "
                                     "snaps to 0.1)";
    ot.flag = 0;
    ot.poll = timing_picks_here;
    ot.exec = [kind](Context &C) {
      C.wm->path_graph_transform_request = kind;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_handles_menu";
    ot.name = "Timing Handle Type Menu";
    ot.description = "V over the Path Timing graph in Edit mode: the picked points' handle types";
    ot.flag = 0;
    ot.poll = timing_picks_here;
    ot.exec = [](Context &C) {
      C.wm->menu_request = "timing_handles";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_sculpt";
    ot.name = "Sculpt Timing";
    ot.description = "The Path Timing graph's Sculpt mode: a stroke of brush= GRAB (moves the curve within the radius of "
                     "where it starts, by the stroke) or SMOOTH on the timing curve of the stretch frame= is in, along "
                     "path=\"frame,along;...\" (frames, the way along: 1 at the key), radius= (frames), strength= (0-1), "
                     "aspect= (frames a whole way along is, as the graph shows it, so the brush is a circle there), "
                     "lock_x=1 to move it up and down only, dyntopo=1 to work on the curve finely sampled and make it "
                     "again with as few points as hold every frame (else its own points and handles move). Of what a path traces (layer=, bone=, point=, drawing=1, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [timing_curve_of](Context &C) {
      const std::string brush_id = op_arg(C, "brush", "GRAB");
      if (brush_id != "GRAB" && brush_id != "SMOOTH") {
        report(ReportType::Warning, "brush= is GRAB or SMOOTH");
        return OpStatus::Cancelled;
      }
      const TimingBrush brush = brush_id == "GRAB" ? TimingBrush::Grab : TimingBrush::Smooth;
      MotionPathTarget t;
      int frame = 0, start = 0, n = 0;
      TimingCurve curve;
      if (!timing_curve_of(C, t, frame, curve, &start, &n)) {
        return OpStatus::Cancelled;
      }
      const std::vector<float2> path = timing_stroke_parse(op_arg(C, "path"), float(start));
      if (path.empty()) {
        report(ReportType::Warning, "path= is the stroke's places, \"frame,along;frame,along\"");
        return OpStatus::Cancelled;
      }
      const ToolSettings &ts = C.wm->tool_settings;
      const int b = int(brush);
      const TimingCurve sculpted = timing_sculpt(
          curve, n, brush, path, std::max(op_arg_float(C, "radius", ts.path_timing_radius[b]), 0.01f),
          std::clamp(op_arg_float(C, "strength", ts.path_timing_strength[b]), 0.0f, 1.0f),
          std::max(op_arg_float(C, "aspect", float(n)), 1e-3f),
          op_has_arg(C, "lock_x") ? op_arg_bool(C, "lock_x") : ts.path_timing_lock_x,
          op_has_arg(C, "dyntopo") ? op_arg_bool(C, "dyntopo") : ts.path_timing_dyntopo);
      if (!motion_path_timing_curve_set(*C.document, t, frame, sculpted)) {
        return OpStatus::Cancelled;
      }
      if (sculpted.points.size() != curve.points.size()) {
        C.wm->path_timing_picks.clear();
        C.wm->path_timing_point = -1;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_graph_set";
    ot.name = "Set Path Timing Graph";
    ot.description = "The Path Timing graph's mode= EDIT (its curve's points and handles) or SCULPT (brushes on the "
                     "curve); its brush= GRAB or SMOOTH, that brush's radius= (frames) and strength= (0-1), and "
                     "lock_x=0/1 (the brushes move the curve up and down only) and dyntopo=0/1";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.wm != nullptr; };
    ot.exec = [](Context &C) {
      ToolSettings &ts = C.wm->tool_settings;
      if (op_has_arg(C, "mode")) {
        const std::string mode = op_arg(C, "mode");
        if (mode != "EDIT" && mode != "SCULPT") {
          report(ReportType::Warning, "mode= is EDIT or SCULPT");
          return OpStatus::Cancelled;
        }
        ts.path_graph_sculpt = mode == "SCULPT";
      }
      if (op_has_arg(C, "brush")) {
        const std::string brush = op_arg(C, "brush");
        if (brush != "GRAB" && brush != "SMOOTH") {
          report(ReportType::Warning, "brush= is GRAB or SMOOTH");
          return OpStatus::Cancelled;
        }
        ts.path_timing_brush = brush == "GRAB" ? TimingBrush::Grab : TimingBrush::Smooth;
      }
      const int b = int(ts.path_timing_brush);
      if (op_has_arg(C, "radius")) {
        ts.path_timing_radius[b] = std::clamp(op_arg_float(C, "radius", ts.path_timing_radius[b]), 0.1f, 500.0f);
      }
      if (op_has_arg(C, "strength")) {
        ts.path_timing_strength[b] = std::clamp(op_arg_float(C, "strength", ts.path_timing_strength[b]), 0.0f, 1.0f);
      }
      if (op_has_arg(C, "lock_x")) {
        ts.path_timing_lock_x = op_arg_bool(C, "lock_x");
      }
      if (op_has_arg(C, "dyntopo")) {
        ts.path_timing_dyntopo = op_arg_bool(C, "dyntopo");
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.timing_radius_adjust";
    ot.name = "Adjust Timing Brush Radius";
    ot.description = "Size the Path Timing graph's Sculpt brush with the mouse (F over the graph); click to keep it";
    ot.flag = 0;
    ot.poll = [](Context &C) { return C.wm && C.wm->tool_settings.path_graph_sculpt; };
    ot.exec = [](Context &C) {
      C.wm->path_graph_resize_request = true;
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
                     "there. interp=, ease=, hold=, type= (KEYFRAME or BREAKDOWN) and the rest as anim.keys_interpolation_set "
                     "takes them";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
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
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
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
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
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
    ot.description = "The Path Timing panel shows the stretch step= (-1 or 1) before or after the one it shows. "
                     "With picks in different stretches, the next or previous of those (the picks kept); with one "
                     "stretch picked, the one beside it is picked instead (the dot in its middle, or its end key) "
                     "and the frame goes there";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      WindowManager &wm = *C.wm;
      MotionPathTarget t;
      MotionPathStretch shown;
      MotionPathPicks picks;
      if (!motion_path_timing_shown(C, t, shown, &picks)) {
        return OpStatus::Cancelled;
      }
      const int step = op_arg_int(C, "step", 1) < 0 ? -1 : 1;
      if (picks.spans > 1) {
        /* Shown one by one, round and round; what is picked stays. */
        const int n = int(picks.stretches.size());
        const MotionPathPickedStretch &p = picks.stretches[size_t(((picks.shown + step) % n + n) % n)];
        wm.path_timing_target = p.target;
        wm.path_timing_end = p.end;
        wm.path_timing_frame = doc.frame_current;
        return OpStatus::Finished;
      }
      const std::vector<MotionPathStretch> stretches = motion_path_stretches(doc, t);
      for (size_t i = 0; i < stretches.size(); i++) {
        const int j = int(i) + step;
        if (stretches[i].end != shown.end || j < 0 || j >= int(stretches.size())) {
          continue;
        }
        const MotionPathStretch &next = stretches[size_t(j)];
        if (picks.spans == 1) {
          /* Kept in step with the rest: the stretch beside is picked, at its middle (a dot), else its end key. */
          const int frame = next.end - next.start >= 2 ? (next.start + next.end) / 2 : next.end;
          motion_path_pick_at(C, {t}, frame, false);
          return OpStatus::Finished;
        }
        wm.path_timing_target = t;
        wm.path_timing_end = next.end;
        wm.path_timing_frame = doc.frame_current;
        return OpStatus::Finished;
      }
      return OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.motion_path_scale_spacing";
    ot.name = "Scale Path Spacing";
    ot.description = "Alt S: the dots around frame=N (else around each dot and key selected) brought factor= as far "
                     "from it along the path (under 1 closer: slower there; on a key, easing into and out of it), the "
                     "keys staying on their frames; the dots within size= frames, by falloff= (proportional=0: all "
                     "alike; the tool's size and falloff when left out, always soft whatever O says). Of what a path traces (layer=, bone=, "
                     "point=, or the first traced)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_TIMING;
    ot.poll = [](Context &C) { return C.document != nullptr && C.wm != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const ToolSettings &ts = C.wm->tool_settings;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      PathSlide slide;
      /* Soft unless told otherwise, whatever O says (as Alt S is). */
      slide.proportional = op_has_arg(C, "proportional") ? op_arg_bool(C, "proportional") : true;
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
    ot.poll = [](Context &C) {
      return path_edit_poll(C) && (path_handles_picked(C) || !path_selected_keys(C).empty());
    };
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
                     "and scaling with them; a key on its own stays, its handles turning or scaling about it. Control "
                     "points picked with them go too, all about their middle; path handles picked go on their own";
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
      const float2 offset{op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f)};
      /* Path handles picked: they alone, each about its key; control points picked with keys: together, about the
       * middle of them all. */
      std::vector<MotionPathHandlePick> picks;
      for (const MotionPathHandlePick &p : C.wm->path_handle_picks) {
        if (motion_path_pick_valid(doc, p)) {
          picks.push_back(p);
        }
      }
      const int changed = motion_path_picks_transform(doc, path_selected_keys(C), picks, kind[0], offset,
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
      /* Path handles picked: their keys; only control points picked (with keys): the keys selected. */
      const bool handles = path_handles_picked(C) && !motion_path_picks_with_keys(C.wm->path_handle_picks);
      if ((handles ? motion_path_handles_type_at(*C.document, C.wm->path_handle_picks, mode)
                   : motion_path_handles_type(*C.document, motion_path_targets(C), mode)) == 0)
      {
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
    ot.description = "V with the Motion Path tool's Edit: a menu of handle types for the selected keys (or the keys of "
                     "the path handles picked)";
    ot.flag = 0;
    ot.poll = [](Context &C) { return path_edit_poll(C) && (path_handles_picked(C) || !path_selected_keys(C).empty()); };
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
      if (motion_path_keys_delete(*C.document, motion_path_targets(C)) == 0) {
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
                     "now, so the path stays as it is (Alt click with the Motion Path tool's Edit); it is selected. On "
                     "2s or 3s a held frame keys the frame whose pose it holds. A breakdown, unless type=KEYFRAME";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::vector<MotionPathTarget> targets;
      if (!op_targets(C, targets)) {
        return OpStatus::Cancelled;
      }
      int frame = op_arg_int(C, "frame", doc.frame_current);
      animation_sync(doc);
      KeyType type = KeyType::Breakdown;
      if (op_has_arg(C, "type") && !key_type_from_identifier(op_arg(C, "type"), &type)) {
        report(ReportType::Error, "type is KEYFRAME or BREAKDOWN");
        return OpStatus::Cancelled;
      }
      if (!motion_path_key_add(doc, targets.front(), frame, type, &frame)) {
        report(ReportType::Info, "A key is there already");
        return OpStatus::Cancelled;
      }
      motion_path_key_type_set(doc, targets.front(), frame, type);
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
