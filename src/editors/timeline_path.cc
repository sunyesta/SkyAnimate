#include "editors/timeline_path.hh"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "anim/motion_path.hh"
#include "data/document.hh"
#include "editors/ui.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

/* At most this many rows, so a big rig traced doesn't push the keys out of the timeline. */
static constexpr int MAX_ROWS = 4;

static std::vector<MotionPathTarget> strip_targets(Context &C)
{
  if (!C.document || !C.wm || C.wm->tool_settings.tool != Tool::MotionPath) {
    return {};
  }
  std::vector<MotionPathTarget> targets = motion_path_targets(*C.document, editable_layers(C));
  if (targets.size() > size_t(MAX_ROWS)) {
    targets.resize(size_t(MAX_ROWS));
  }
  return targets;
}

float timeline_path_strip_height(Context &C, float row_h)
{
  const size_t n = strip_targets(C).size();
  return n == 0 ? 0.0f : float(n) * row_h * 1.25f + 2.0f;
}

/* A tick dragged: slid along its path from the document as it was pressed, one undo step. */
static struct {
  bool active = false;
  bool dragging = false;
  bool changed = false;
  MotionPathTarget target;
  int frame = 0, start = 0, end = 0;
  float press_x = 0.0f;
  std::optional<OpArgs> select_on_release;
  Document snapshot;
} g_tick;

static OpArgs target_args(const Document &doc, const MotionPathTarget &t, int frame)
{
  OpArgs args = {{"layer", std::to_string(t.layer)}, {"frame", std::to_string(frame)}};
  if (const Layer *l = layer_find(doc, t.layer)) {
    for (const Bone &b : l->bones) {
      if (b.id == t.bone) {
        args["bone"] = b.name;
      }
    }
  }
  if (t.point) {
    args["point"] = std::to_string(t.point);
  }
  return args;
}

static void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 fill, ImU32 edge)
{
  const ImVec2 d[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
  dl->AddConvexPolyFilled(d, 4, fill);
  dl->AddPolyline(d, 4, edge, ImDrawFlags_Closed, 1.0f);
}

void timeline_path_strip(Context &C, ImVec2 min, ImVec2 max, float keys_x, float view_start, float span,
                         float row_h)
{
  const std::vector<MotionPathTarget> targets = strip_targets(C);
  if (targets.empty() || max.y - min.y < 4.0f) {
    g_tick = {};
    return;
  }
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  const ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  const bool edit = ts.path_brush == PathBrush::Edit;
  const float keys_w = std::max(max.x - keys_x, 8.0f);
  const auto x_of = [&](float frame) { return keys_x + (frame - view_start) / span * keys_w; };
  const auto frame_of = [&](float x) { return view_start + (x - keys_x) / keys_w * span; };
  const float lane_h = row_h * 1.25f;

  ImGui::SetCursorScreenPos(min);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##path_timing", ImVec2(max.x - min.x, max.y - min.y), ImGuiButtonFlags_MouseButtonLeft);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(min, max, true);
  dl->AddRectFilled(min, max, IM_COL32(0x24, 0x24, 0x24, 255));
  dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(0x18, 0x18, 0x18, 255), 2.0f);
  dl->AddRectFilled(min, ImVec2(keys_x, max.y), IM_COL32(0x26, 0x26, 0x26, 255));

  /* The tick under the mouse: its target, frame and stretch. */
  struct Hit {
    MotionPathTarget target;
    int frame = 0, start = 0, end = 0;
  };
  std::optional<Hit> hover;
  float best = 6.0f;
  const bool hover_fresh = ImGui::GetFrameCount() - wm.path_hover_tick <= 1;
  for (size_t r = 0; r < targets.size(); r++) {
    const MotionPathTarget &t = targets[r];
    const float top = min.y + 2.0f + float(r) * lane_h, cy = top + lane_h * 0.5f;
    const std::string label = "Timing: " + motion_path_label(doc, t);
    ImGui::RenderTextEllipsis(dl, ImVec2(min.x + 8.0f, top + (lane_h - ImGui::GetFontSize()) * 0.5f),
                              ImVec2(keys_x - 6.0f, top + lane_h), keys_x - 6.0f, label.c_str(), nullptr, nullptr);
    std::vector<int> dots;
    for (const auto &[dt, df] : wm.path_dots) {
      if (dt.same(t)) {
        dots.push_back(df);
      }
    }
    const bool all = doc.path_dot_colors == PathDotColors::TimingAll;
    for (const MotionPathStretch &s : motion_path_stretches(doc, t)) {
      const float xa = x_of(float(s.start)), xb = x_of(float(s.end));
      if (xb < keys_x - 10.0f || xa > max.x + 10.0f) {
        continue;
      }
      const bool lit = all || motion_path_stretch_lit(s.start, s.end, doc.frame_current, dots);
      dl->AddLine(ImVec2(xa, cy), ImVec2(xb, cy), IM_COL32(0x70, 0x70, 0x70, 255), 1.5f);
      const int n = s.end - s.start;
      for (int k = 1; k < n; k++) {
        const int frame = s.start + k;
        const float x = xa + (xb - xa) * s.progress[size_t(k)];
        const bool picked = std::find(dots.begin(), dots.end(), frame) != dots.end();
        const bool ringed = (g_tick.active && g_tick.target.same(t) && g_tick.frame == frame) ||
                            (hover_fresh && wm.path_hover_target.same(t) && wm.path_hover_frame == frame);
        const Color c = lit ? motion_path_time_color(float(k) / float(n)) : motion_path_unlit_color();
        const float half = lane_h * (picked ? 0.36f : 0.26f);
        if (picked || ringed) {
          dl->AddRectFilled(ImVec2(x - 3.0f, cy - half - 2.0f), ImVec2(x + 3.0f, cy + half + 2.0f),
                            picked ? IM_COL32(255, 160, 40, 255) : IM_COL32(255, 255, 255, 220));
        }
        dl->AddLine(ImVec2(x, cy - half), ImVec2(x, cy + half),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)), 2.5f);
        if (hovered && edit && !g_tick.active && std::fabs(io.MousePos.y - cy) < lane_h * 0.5f &&
            std::fabs(io.MousePos.x - x) < best)
        {
          best = std::fabs(io.MousePos.x - x);
          hover = Hit{t, frame, s.start, s.end};
        }
      }
    }
    for (const int k : motion_path_keys(doc, t)) {
      const float x = x_of(float(k));
      if (x >= keys_x - 8.0f && x <= max.x + 8.0f) {
        diamond(dl, ImVec2(x, cy), row_h * 0.28f, IM_COL32(240, 240, 240, 255), IM_COL32(20, 20, 20, 220));
      }
    }
  }
  /* The playhead carries on down. */
  const float px = x_of(float(doc.frame_current));
  dl->AddLine(ImVec2(px, min.y), ImVec2(px, max.y), IM_COL32(0x47, 0x72, 0xb3, 255), 2.0f);
  dl->PopClipRect();

  if (hover) {
    wm.path_hover_target = hover->target;
    wm.path_hover_frame = hover->frame;
    wm.path_hover_tick = ImGui::GetFrameCount();
    ImGui::SetTooltip("Frame %d", hover->frame);
  }
  auto &g = g_tick;
  if (!g.active) {
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || !edit) {
      return;
    }
    if (!hover) {
      if (!io.KeyShift && !io.KeyCtrl && !io.KeyAlt) {
        operator_call(C, "anim.motion_path_select", {{"none", "1"}});
      }
      return;
    }
    OpArgs args = target_args(doc, hover->target, hover->frame);
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      args["go"] = "1";
      operator_call(C, "anim.motion_path_select", args);
      return;
    }
    if (io.KeyShift) {
      args["extend"] = "1";
      operator_call(C, "anim.motion_path_select", args);
      return;
    }
    g.select_on_release.reset();
    const bool picked = std::any_of(wm.path_dots.begin(), wm.path_dots.end(), [&](const auto &d) {
      return d.first.same(hover->target) && d.second == hover->frame;
    });
    if (picked) {
      g.select_on_release = args;
    }
    else {
      operator_call(C, "anim.motion_path_select", args);
    }
    g.active = true;
    g.dragging = false;
    g.changed = false;
    g.target = hover->target;
    g.frame = hover->frame;
    g.start = hover->start;
    g.end = hover->end;
    g.press_x = io.MousePos.x;
    g.snapshot = doc;
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (g.dragging) {
      doc = std::move(g.snapshot);
    }
    g = {};
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (g.changed) {
      C.undo->push("Slide Path Dot", doc);
    }
    else if (!g.dragging && g.select_on_release) {
      operator_call(C, "anim.motion_path_select", *g.select_on_release);
    }
    g = {};
    return;
  }
  if (!g.dragging && std::fabs(io.MousePos.x - g.press_x) < 3.0f) {
    return;
  }
  if (!g.dragging || io.MouseDelta.x != 0.0f) {
    g.dragging = true;
    PathSlide slide;
    slide.proportional = ts.proportional;
    slide.size = ts.path_proportional_size;
    slide.falloff = ts.proportional_falloff;
    doc = g.snapshot;
    const float progress = (frame_of(io.MousePos.x) - float(g.start)) / float(std::max(g.end - g.start, 1));
    g.changed = motion_path_slide_progress(doc, g.target, g.frame, progress, slide);
    C.undo->mark_changed();
  }
  wm.path_hover_target = g.target;
  wm.path_hover_frame = g.frame;
  wm.path_hover_tick = ImGui::GetFrameCount();
}

float timeline_sidebar_width(Context &C, float avail_w)
{
  if (!C.wm || !C.wm->timeline_sidebar) {
    return 0.0f;
  }
  return std::min(ImGui::GetFontSize() * 17.0f, avail_w * 0.5f);
}

/* A graph dot dragged up or down: slid from the document as it was pressed, one undo step. */
static struct {
  bool active = false;
  bool dragging = false;
  bool changed = false;
  MotionPathTarget target;
  int frame = 0;
  float press_y = 0.0f;
  std::optional<OpArgs> select_on_release;
  Document snapshot;
} g_graph;

/* Alt S over the graph: the spacing around the selected dots scaled by the mouse's left and right. */
static struct {
  bool active = false;
  float press_x = 0.0f;
  Document snapshot;
} g_graph_scale;

static void path_timing_graph(Context &C, const MotionPathTarget &t, const MotionPathStretch &s, ImVec2 min,
                              ImVec2 max)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  const ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  const int n = s.end - s.start;
  /* A sliver of the stretches either side, in grey, at the edges. */
  const float pad = std::max(0.5f, float(n) * 0.12f);
  const float f0 = float(s.start) - pad, f1 = float(s.end) + pad;
  const float y0 = -0.06f, y1 = 1.06f;
  const auto to_screen = [&](float frame, float u) {
    return ImVec2(min.x + (frame - f0) / (f1 - f0) * (max.x - min.x), max.y - (u - y0) / (y1 - y0) * (max.y - min.y));
  };
  ImGui::SetCursorScreenPos(min);
  ImGui::InvisibleButton("##path_graph", ImVec2(max.x - min.x, max.y - min.y), ImGuiButtonFlags_MouseButtonLeft);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(min, max, true);
  dl->AddRectFilled(min, max, IM_COL32(0x1d, 0x1d, 0x1d, 255), 3.0f);
  /* Frame lines and the 0 and 1 lines. */
  for (int f = s.start; f <= s.end; f++) {
    const float x = to_screen(float(f), 0.0f).x;
    dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(0x2a, 0x2a, 0x2a, 255));
  }
  for (const float u : {0.0f, 1.0f}) {
    const float y = to_screen(0.0f, u).y;
    dl->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), IM_COL32(0x3a, 0x3a, 0x3a, 255));
  }
  /* Even speed: the dashed diagonal. */
  const ImVec2 da = to_screen(float(s.start), 0.0f), db = to_screen(float(s.end), 1.0f);
  for (int k = 0; k < 24; k += 2) {
    const float a = float(k) / 24.0f, b = float(k + 1) / 24.0f;
    dl->AddLine(ImVec2(da.x + (db.x - da.x) * a, da.y + (db.y - da.y) * a),
                ImVec2(da.x + (db.x - da.x) * b, da.y + (db.y - da.y) * b), IM_COL32(150, 150, 150, 160), 1.0f);
  }
  /* The stretches either side, grey: the one before ending at 0, the one after starting at 1. */
  const Color grey = motion_path_unlit_color();
  const ImU32 grey_col = ImGui::ColorConvertFloat4ToU32(ImVec4(grey.r, grey.g, grey.b, grey.a * 0.6f));
  for (const MotionPathStretch &o : motion_path_stretches(doc, t)) {
    if (o.end == s.end) {
      continue;
    }
    const bool before = o.end <= s.start;
    for (size_t k = 0; k < o.progress.size(); k++) {
      const float f = float(o.start + int(k));
      if (f < f0 || f > f1) {
        continue;
      }
      dl->AddCircleFilled(to_screen(f, before ? o.progress[k] - 1.0f : 1.0f + o.progress[k]), 3.0f, grey_col);
    }
  }
  /* The curve through the dots, then the dots. */
  for (int k = 0; k < n; k++) {
    dl->AddLine(to_screen(float(s.start + k), s.progress[size_t(k)]),
                to_screen(float(s.start + k + 1), s.progress[size_t(k + 1)]), IM_COL32(110, 110, 110, 255), 1.5f);
  }
  std::optional<int> hover;
  float best = 9.0f;
  for (int k = 1; k < n; k++) {
    const int frame = s.start + k;
    const ImVec2 p = to_screen(float(frame), s.progress[size_t(k)]);
    const bool picked = std::any_of(wm.path_dots.begin(), wm.path_dots.end(),
                                    [&](const auto &d) { return d.first.same(t) && d.second == frame; });
    const bool ringed = (g_graph.active && g_graph.frame == frame) ||
                        (ImGui::GetFrameCount() - wm.path_hover_tick <= 1 && wm.path_hover_target.same(t) &&
                         wm.path_hover_frame == frame);
    const Color c = motion_path_time_color(float(k) / float(n));
    dl->AddCircleFilled(p, 5.0f, ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)));
    dl->AddCircle(p, 5.0f, IM_COL32(20, 20, 20, 220), 0, 1.5f);
    if (picked || ringed) {
      dl->AddCircle(p, 7.5f, picked ? IM_COL32(255, 160, 40, 255) : IM_COL32(255, 255, 255, 220), 0, 1.6f);
    }
    const float d = std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y);
    if (hovered && !g_graph.active && d < best) {
      best = d;
      hover = frame;
    }
  }
  for (const int k : {s.start, s.end}) {
    const ImVec2 p = to_screen(float(k), k == s.start ? 0.0f : 1.0f);
    diamond(dl, p, 5.5f, IM_COL32(240, 240, 240, 255), IM_COL32(20, 20, 20, 220));
  }
  dl->PopClipRect();

  if (hover) {
    wm.path_hover_target = t;
    wm.path_hover_frame = *hover;
    wm.path_hover_tick = ImGui::GetFrameCount();
    const float u = s.progress[size_t(*hover - s.start)];
    ImGui::SetTooltip("Frame %d: %.0f%% along", *hover, u * 100.0f);
  }

  /* Alt S: scale the spacing around the selected dots, as in the viewport. */
  auto &sc = g_graph_scale;
  if (!sc.active && hovered && !g_graph.active && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_S, false) &&
      !wm.path_dots.empty())
  {
    sc.active = true;
    sc.press_x = io.MousePos.x;
    sc.snapshot = doc;
  }
  if (sc.active) {
    const float factor = std::max(0.0f, 1.0f + (io.MousePos.x - sc.press_x) / 150.0f);
    PathSlide slide;
    slide.proportional = ts.proportional;
    slide.size = ts.path_proportional_size;
    slide.falloff = ts.proportional_falloff;
    doc = sc.snapshot;
    for (const auto &[dt, df] : wm.path_dots) {
      motion_path_scale_spacing(doc, dt, df, factor, slide);
    }
    C.undo->mark_changed();
    ImGui::SetTooltip("Scale Spacing: %.2f", factor);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      doc = std::move(sc.snapshot);
      sc = {};
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      C.undo->push("Scale Path Spacing", doc);
      sc = {};
    }
    return;
  }

  /* The right click's menu: the stretch's shapes, Even Spacing for the dots picked. */
  if (hovered && !g_graph.active && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right))
  {
    ImGui::OpenPopup("##path_timing_menu");
  }
  if (ui_menu_begin("##path_timing_menu")) {
    ImGui::TextDisabled("Path Timing");
    ImGui::Separator();
    OpArgs args = target_args(doc, t, s.start + 1);
    for (const auto &[label, id] : {std::pair{"Even", "EVEN"}, std::pair{"Ease In", "EASE_IN"},
                                    std::pair{"Ease Out", "EASE_OUT"}, std::pair{"Ease In-Out", "EASE_IN_OUT"}})
    {
      if (ui_menu_item(label)) {
        args["shape"] = id;
        operator_call(C, "anim.motion_path_stretch_shape", args);
      }
    }
    if (!wm.path_dots.empty()) {
      ImGui::Separator();
      if (ui_menu_item("Even Spacing (Selected Dots)")) {
        operator_call(C, "anim.motion_path_even_spacing");
      }
    }
    ui_menu_end();
  }

  auto &g = g_graph;
  if (!g.active) {
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      return;
    }
    if (!hover) {
      if (!io.KeyShift && !io.KeyCtrl) {
        operator_call(C, "anim.motion_path_select", {{"none", "1"}});
      }
      return;
    }
    OpArgs args = target_args(doc, t, *hover);
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      args["go"] = "1";
      operator_call(C, "anim.motion_path_select", args);
      return;
    }
    if (io.KeyShift) {
      args["extend"] = "1";
      operator_call(C, "anim.motion_path_select", args);
      return;
    }
    g.select_on_release.reset();
    const bool picked = std::any_of(wm.path_dots.begin(), wm.path_dots.end(),
                                    [&](const auto &d) { return d.first.same(t) && d.second == *hover; });
    if (picked) {
      g.select_on_release = args;
    }
    else {
      operator_call(C, "anim.motion_path_select", args);
    }
    g.active = true;
    g.dragging = false;
    g.changed = false;
    g.target = t;
    g.frame = *hover;
    g.press_y = io.MousePos.y;
    g.snapshot = doc;
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    if (g.dragging) {
      doc = std::move(g.snapshot);
    }
    g = {};
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    if (g.changed) {
      C.undo->push("Slide Path Dot", doc);
    }
    else if (!g.dragging && g.select_on_release) {
      operator_call(C, "anim.motion_path_select", *g.select_on_release);
    }
    g = {};
    return;
  }
  if (!g.dragging && std::fabs(io.MousePos.y - g.press_y) < 3.0f) {
    return;
  }
  g.dragging = true;
  PathSlide slide;
  slide.proportional = ts.proportional;
  slide.size = ts.path_proportional_size;
  slide.falloff = ts.proportional_falloff;
  doc = g.snapshot;
  /* Up is further along; Ctrl puts it on the even line. */
  float progress = y0 + (max.y - io.MousePos.y) / (max.y - min.y) * (y1 - y0);
  if (io.KeyCtrl) {
    progress = float(g.frame - s.start) / float(std::max(n, 1));
  }
  g.changed = motion_path_slide_progress(doc, g.target, g.frame, progress, slide);
  C.undo->mark_changed();
}

void timeline_sidebar(Context &C, ImVec2 min, ImVec2 max)
{
  if (max.x - min.x < 20.0f || max.y - min.y < 20.0f || !C.document || !C.wm) {
    return;
  }
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(min, max, IM_COL32(0x30, 0x30, 0x30, 255));
  dl->AddLine(min, ImVec2(min.x, max.y), IM_COL32(0x18, 0x18, 0x18, 255), 2.0f);
  ImGui::SetCursorScreenPos(ImVec2(min.x + 2.0f, min.y));
  if (!ImGui::BeginChild("##timeline_sidebar", ImVec2(max.x - min.x - 2.0f, max.y - min.y),
                         ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground))
  {
    ImGui::EndChild();
    return;
  }
  /* The one tab, as Blender's sidebar tabs. */
  if (ImGui::BeginTabBar("##sidebar_tabs")) {
    if (ImGui::BeginTabItem("Path Timing")) {
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  const float w = ImGui::GetContentRegionAvail().x;
  if (wm.tool_settings.tool != Tool::MotionPath) {
    ImGui::TextWrapped("Pick the Motion Path tool to edit the timing of what its paths trace here.");
    ImGui::EndChild();
    return;
  }
  MotionPathTarget t;
  MotionPathStretch s;
  if (!motion_path_timing_stretch(doc, motion_path_targets(doc, editable_layers(C)), wm.path_timing_target,
                                  wm.path_timing_end, t, s))
  {
    ImGui::TextWrapped("Trace something with keys a few frames apart: its stretches show here.");
    ImGui::EndChild();
    return;
  }
  /* Which thing and stretch, with arrows to the ones either side. */
  if (ImGui::ArrowButton("##stretch_prev", ImGuiDir_Left)) {
    wm.path_timing_target = t;
    wm.path_timing_end = s.end;
    operator_call(C, "anim.motion_path_timing_step", {{"step", "-1"}});
  }
  ImGui::SameLine();
  const std::string title = motion_path_label(doc, t) + "  " + std::to_string(s.start) + "-" + std::to_string(s.end);
  const float arrow_w = ImGui::GetFrameHeight();
  const float title_w = std::max(w - arrow_w * 2.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f, 10.0f);
  const ImVec2 tp = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(title_w, arrow_w));
  ImGui::RenderTextEllipsis(dl, ImVec2(tp.x, tp.y + (arrow_w - ImGui::GetFontSize()) * 0.5f),
                            ImVec2(tp.x + title_w, tp.y + arrow_w), tp.x + title_w, title.c_str(), nullptr, nullptr);
  ImGui::SameLine();
  if (ImGui::ArrowButton("##stretch_next", ImGuiDir_Right)) {
    wm.path_timing_target = t;
    wm.path_timing_end = s.end;
    operator_call(C, "anim.motion_path_timing_step", {{"step", "1"}});
  }
  /* The graph: as tall as there is room for, under the fields and buttons. */
  const float below = ImGui::GetFrameHeightWithSpacing() * 5.0f;
  const float graph_h = std::clamp(ImGui::GetContentRegionAvail().y - below, ImGui::GetFontSize() * 4.0f,
                                   ImGui::GetFontSize() * 16.0f);
  const ImVec2 g0 = ImGui::GetCursorScreenPos();
  path_timing_graph(C, t, s, g0, ImVec2(g0.x + w, g0.y + graph_h));
  ImGui::SetCursorScreenPos(ImVec2(g0.x, g0.y + graph_h + ImGui::GetStyle().ItemSpacing.y));

  /* The selected dot of this stretch (the last picked): its % along and speed. A drag shows its value and changes
   * the dot once let go, one undo step. */
  int dot = 0;
  for (const auto &[dt, df] : wm.path_dots) {
    if (dt.same(t) && df > s.start && df < s.end) {
      dot = df;
    }
  }
  if (dot) {
    static float along_edit = 0.0f, speed_edit = 1.0f;
    static bool along_live = false, speed_live = false;
    if (!along_live) {
      along_edit = s.progress[size_t(dot - s.start)] * 100.0f;
    }
    if (!speed_live) {
      speed_edit = motion_path_dot_speed(doc, t, dot);
    }
    ImGui::Text("Frame %d", dot);
    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##along", &along_edit, 0.25f, 0.0f, 100.0f, "Along %.1f%%");
    along_live = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      OpArgs args = target_args(doc, t, dot);
      args["progress"] = std::to_string(along_edit / 100.0f);
      operator_call(C, "anim.motion_path_slide", args);
    }
    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##speed", &speed_edit, 0.01f, 0.02f, float(s.end - s.start) * 0.5f, "Speed %.2f");
    speed_live = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      OpArgs args = target_args(doc, t, dot);
      args["speed"] = std::to_string(speed_edit);
      operator_call(C, "anim.motion_path_dot_speed", args);
    }
  }
  else {
    ImGui::TextDisabled("Click a dot for its %% along and speed");
  }
  /* The stretch's shapes. */
  const float bw = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  int i = 0;
  for (const auto &[label, id] : {std::pair{"Even", "EVEN"}, std::pair{"Ease In", "EASE_IN"},
                                  std::pair{"Ease Out", "EASE_OUT"}, std::pair{"Ease In-Out", "EASE_IN_OUT"}})
  {
    if (i++ % 2) {
      ImGui::SameLine();
    }
    if (ImGui::Button(label, ImVec2(bw, 0.0f))) {
      OpArgs args = target_args(doc, t, s.start + 1);
      args["shape"] = id;
      operator_call(C, "anim.motion_path_stretch_shape", args);
    }
  }
  ImGui::EndChild();
}

}  // namespace oanim
