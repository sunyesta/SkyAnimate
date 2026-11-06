#include "editors/timeline_path.hh"

#include "imgui_internal.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "anim/motion_path.hh"
#include "data/document.hh"
#include "data/vector.hh"
#include "editors/ui.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

/* At most this many rows, so a big rig traced doesn't push the keys out of the timeline. */
static constexpr int MAX_ROWS = 4;

/* The proportional circle's band and outline, the dots it pulls, and Shift click's blend reach. */
static constexpr ImU32 REACH_FILL = IM_COL32(255, 255, 255, 16);
static constexpr ImU32 REACH_EDGE = IM_COL32(255, 255, 255, 90);
static constexpr ImU32 BLEND_FILL = IM_COL32(80, 150, 255, 30);
static constexpr ImU32 BLEND_EDGE = IM_COL32(90, 160, 255, 200);

static std::vector<MotionPathTarget> strip_targets(Context &C)
{
  if (!C.document || !C.wm || C.wm->tool_settings.tool != Tool::MotionPath) {
    return {};
  }
  std::vector<MotionPathTarget> targets = motion_path_timing_targets(C);
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

/* A Shift drag's blend, in the chart (0) or the graph (1): painted live from the document as it was pressed, a blend
 * each time the mouse reaches a new frame (every frame between too, so it doesn't matter how fast), then run as
 * anim.motion_path_timing_blend with the stroke's frames, one undo step the panel can run again. */
static struct {
  bool active = false;
  int owner = 0;
  MotionPathTarget target;
  int lo = 0, hi = 0, last = 0;
  std::vector<int> frames;
  Document snapshot;
} g_blend;

bool timeline_path_strip_busy()
{
  /* Shift over the chart: the wheel sizes the blend. */
  return g_tick.active || g_blend.active || ImGui::GetIO().KeyShift;
}

static OpArgs target_args(const Document &doc, const MotionPathTarget &t, int frame);

/* A key's diamond right-clicked (in the chart or the graph): its menu, the interpolation of the key behind it. */
static struct {
  MotionPathTarget target;
  int frame = 0;
} g_key_menu;

static void key_menu_open(const MotionPathTarget &t, int frame, const char *name)
{
  g_key_menu.target = t;
  g_key_menu.frame = frame;
  ImGui::OpenPopup(name);
}

static void key_menu(Context &C, const char *name)
{
  if (!ui_menu_begin(name)) {
    return;
  }
  Document &doc = *C.document;
  const MotionPathTarget t = g_key_menu.target;
  const int frame = g_key_menu.frame;
  ImGui::TextDisabled("Key on frame %d", frame);
  KeySettings current;
  const int count = motion_path_key_settings(doc, t, frame, nullptr, &current);
  if (count == 0) {
    ImGui::TextDisabled("No key here now");
    ui_menu_end();
    return;
  }
  bool mixed_interp = false, mixed_ease = false, mixed_type = false;
  motion_path_key_settings(doc, t, frame, [&](KeySettings &s) {
    mixed_interp |= s.interp != current.interp;
    mixed_ease |= s.ease != current.ease;
    mixed_type |= s.type != current.type;
  });
  if (ui_submenu_begin("Interpolation")) {
    timeline_interp_modes(current, mixed_interp, mixed_ease, false, [&](const OpArgs &set) {
      OpArgs args = target_args(doc, t, frame);
      for (const auto &[k, v] : set) {
        args[k] = v;
      }
      operator_call(C, "anim.motion_path_key_interp", args);
    });
    ui_submenu_end();
  }
  if (frame > 0 && ui_submenu_begin("Key Type")) {
    for (const KeyType type : {KeyType::Key, KeyType::Breakdown}) {
      if (ui_menu_item(key_type_name(type), nullptr, !mixed_type && current.type == type)) {
        OpArgs args = target_args(doc, t, frame);
        args["type"] = key_type_identifier(type);
        operator_call(C, "anim.motion_path_key_interp", args);
      }
    }
    ui_submenu_end();
  }
  ImGui::Separator();
  if (ui_menu_item("Delete Key")) {
    operator_call(C, "anim.motion_path_key_delete", target_args(doc, t, frame));
  }
  ui_menu_end();
}

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
  if (t.head) {
    args["head"] = "1";
  }
  if (t.drawing) {
    args["drawing"] = "1";
  }
  return args;
}

/* A breakdown key: a small circle, dark blue, light blue when selected. */
static void breakdown_dot(ImDrawList *dl, ImVec2 c, float r, bool selected)
{
  dl->AddCircleFilled(c, r, selected ? IM_COL32(0x8c, 0xc8, 0xff, 255) : IM_COL32(0x1f, 0x5f, 0xd6, 255));
  dl->AddCircle(c, r, IM_COL32(20, 20, 20, 220), 0, 1.0f);
}

static void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 fill, ImU32 edge)
{
  const ImVec2 d[4] = {{c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}};
  dl->AddConvexPolyFilled(d, 4, fill);
  dl->AddPolyline(d, 4, edge, ImDrawFlags_Closed, 1.0f);
}

/* Proportional editing is always on in the timing chart and the graph, each with its own reach in frames (0: the dot
 * alone), sized with the wheel while dragging. Returns whether it changed. */
static bool reach_wheel(float &size)
{
  const float wheel = ImGui::GetIO().MouseWheel;
  if (wheel == 0.0f) {
    return false;
  }
  const float was = size;
  size = std::clamp(std::round(size) + (wheel > 0.0f ? 1.0f : -1.0f), 0.0f, 100.0f);
  return size != was;
}

static PathSlide reach_slide(const ToolSettings &ts, float size)
{
  PathSlide slide;
  slide.proportional = size > 0.0f;
  slide.size = size;
  slide.falloff = ts.proportional_falloff;
  slide.keep_order = false;
  return slide;
}

/* How much a dot `frames` from the dragged one follows it. */
static float reach_weight(const ToolSettings &ts, int frames, float size)
{
  if (frames == 0) {
    return 1.0f;
  }
  return size > 0.0f ? proportional_weight(ts.proportional_falloff, float(std::abs(frames)), size, frames) : 0.0f;
}

/* The reach's number, small, by the circle. */
static void reach_label(ImDrawList *dl, ImVec2 at, float size, ImU32 col)
{
  char text[16];
  snprintf(text, sizeof(text), "%g", size);
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.8f, at, col, text);
}

static void blend_apply(Context &C)
{
  const ToolSettings &ts = C.wm->tool_settings;
  *C.document = g_blend.snapshot;
  motion_path_timing_blend_stroke(*C.document, g_blend.target, g_blend.frames, ts.path_blend_smoothing,
                                  ts.path_blend_repeat, ts.path_blend_radius);
  C.undo->mark_changed();
}

static void blend_begin(Context &C, int owner, const MotionPathTarget &t, int frame, int lo, int hi)
{
  auto &b = g_blend;
  animation_sync(*C.document);
  b.active = true;
  b.owner = owner;
  b.target = t;
  b.lo = lo;
  b.hi = hi;
  b.last = std::clamp(frame, lo, hi);
  b.frames = {b.last};
  b.snapshot = *C.document;
  blend_apply(C);
}

/* The stroke going on: the mouse at `frame`. */
static void blend_update(Context &C, float frame)
{
  auto &b = g_blend;
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    *C.document = std::move(b.snapshot);
    b = {};
    return;
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    *C.document = std::move(b.snapshot);
    OpArgs args = target_args(*C.document, b.target, b.frames.front());
    args["frames"] = path_frames_string(b.frames);
    b = {};
    operator_call(C, "anim.motion_path_timing_blend", args);
    return;
  }
  bool changed = reach_wheel(C.wm->tool_settings.path_blend_radius);
  const int to = std::clamp(int(std::lround(frame)), b.lo, b.hi);
  while (b.last != to) {
    b.last += to > b.last ? 1 : -1;
    b.frames.push_back(b.last);
    changed = true;
  }
  if (changed) {
    blend_apply(C);
  }
}

bool timeline_path_strip_ticks_editable(const ToolSettings &ts)
{
  /* The brush is what a drag on the path does in the viewport; the chart is a place of its own, as the timeline is,
   * so its ticks are picked and slid whichever brush is chosen. */
  (void)ts;
  return true;
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
  ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  const bool edit = timeline_path_strip_ticks_editable(ts);
  const float keys_w = std::max(max.x - keys_x, 8.0f);
  const auto x_of = [&](float frame) { return keys_x + (frame - view_start) / span * keys_w; };
  const auto frame_of = [&](float x) { return view_start + (x - keys_x) / keys_w * span; };
  const float lane_h = row_h * 1.25f;
  const auto lane_top = [&](size_t r) { return min.y + 2.0f + float(r) * lane_h; };

  ImGui::SetCursorScreenPos(min);
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##path_timing", ImVec2(max.x - min.x, max.y - min.y), ImGuiButtonFlags_MouseButtonLeft);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(min, max, true);
  dl->AddRectFilled(min, max, IM_COL32(0x24, 0x24, 0x24, 255));
  dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(0x18, 0x18, 0x18, 255), 2.0f);
  dl->AddRectFilled(min, ImVec2(keys_x, max.y), IM_COL32(0x26, 0x26, 0x26, 255));

  /* The lane under the mouse, right of the names. */
  std::optional<size_t> lane;
  if (hovered && io.MousePos.x >= keys_x) {
    for (size_t r = 0; r < targets.size(); r++) {
      if (io.MousePos.y >= lane_top(r) && io.MousePos.y < lane_top(r) + lane_h) {
        lane = r;
      }
    }
  }
  /* Shift: a click or drag blends the timing there instead of picking a dot. */
  const bool stroke = g_blend.active && g_blend.owner == 0;
  if (stroke) {
    lane.reset();
    for (size_t r = 0; r < targets.size(); r++) {
      if (targets[r].same(g_blend.target)) {
        lane = r;
      }
    }
  }
  const bool blending = stroke || (!g_tick.active && !g_blend.active && io.KeyShift && !io.KeyCtrl && !io.KeyAlt &&
                                   lane.has_value());

  /* The key diamond under the mouse, for its right-click menu. */
  std::optional<std::pair<MotionPathTarget, int>> key_hit;
  /* The tick under the mouse: its target, frame and stretch. */
  struct Hit {
    MotionPathTarget target;
    int frame = 0, start = 0, end = 0;
    size_t lane = 0;
  };
  std::optional<Hit> hover;
  float best = 6.0f;
  const bool hover_fresh = ImGui::GetFrameCount() - wm.path_hover_tick <= 1;
  const float size = ts.path_chart_size;
  for (size_t r = 0; r < targets.size(); r++) {
    const MotionPathTarget &t = targets[r];
    const float top = lane_top(r), cy = top + lane_h * 0.5f;
    const std::string label = "Timing: " + motion_path_label(doc, t);
    ImGui::RenderTextEllipsis(dl, ImVec2(min.x + 8.0f, top + (lane_h - ImGui::GetFontSize()) * 0.5f),
                              ImVec2(keys_x - 6.0f, top + lane_h), keys_x - 6.0f, label.c_str(), nullptr, nullptr);
    std::vector<int> dots;
    for (const auto &[dt, df] : wm.path_dots) {
      if (dt.same(t)) {
        dots.push_back(df);
      }
    }
    MotionPathTarget focus_target;
    int focus_start = 0, focus_end = 0;
    if (timeline_key_focus_span(focus_target, focus_start, focus_end) && focus_target.same(t)) {
      dl->AddRectFilled(ImVec2(x_of(float(focus_start)), top + 1.0f), ImVec2(x_of(float(focus_end)), top + lane_h - 1.0f),
                        KEY_FOCUS_FILL);
    }
    const bool all = doc.path_dot_colors == PathDotColors::TimingAll;
    const bool pulling = g_tick.active && g_tick.dragging && g_tick.target.same(t);
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
        /* Past its key or back before its start: on a raised lane, short, so it doesn't sit on the next stretch's. */
        const float u = s.progress[size_t(k)];
        const bool over = u > 1.0f + 1e-4f || u < -1e-4f;
        const float half = lane_h * (over ? 0.14f : picked ? 0.36f : 0.26f);
        const float ty = over ? cy - lane_h * 0.3f : cy;
        if (over) {
          const float kx = u > 1.0f ? xb : xa;
          dl->AddLine(ImVec2(kx, ty), ImVec2(x, ty), IM_COL32(0x90, 0x90, 0x90, 160), 1.0f);
        }
        if (picked || ringed) {
          dl->AddRectFilled(ImVec2(x - 3.0f, ty - half - 2.0f), ImVec2(x + 3.0f, ty + half + 2.0f),
                            picked ? IM_COL32(255, 160, 40, 255) : IM_COL32(255, 255, 255, 220));
        }
        /* The dots the dragged one pulls along: a faint ring, fainter as they follow less. */
        else if (pulling && g_tick.start == s.start) {
          const float w = reach_weight(ts, frame - g_tick.frame, size);
          if (w > 0.0f) {
            dl->AddRect(ImVec2(x - 3.0f, ty - half - 2.0f), ImVec2(x + 3.0f, ty + half + 2.0f),
                        IM_COL32(255, 255, 255, int(40.0f + 110.0f * w)));
          }
        }
        dl->AddLine(ImVec2(x, ty - half), ImVec2(x, ty + half),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)), 2.5f);
        if (hovered && edit && !blending && !g_tick.active && std::fabs(io.MousePos.y - ty) < lane_h * 0.5f &&
            std::fabs(io.MousePos.x - x) < best)
        {
          best = std::fabs(io.MousePos.x - x);
          hover = Hit{t, frame, s.start, s.end, r};
        }
      }
    }
    for (const int k : motion_path_keys(doc, t)) {
      const float x = x_of(float(k));
      if (x >= keys_x - 8.0f && x <= max.x + 8.0f) {
        const bool sel = motion_path_key_selected(doc, t, k);
        if (motion_path_key_is_breakdown(doc, t, k)) {
          breakdown_dot(dl, ImVec2(x, cy), row_h * 0.2f, sel);
        }
        else {
          diamond(dl, ImVec2(x, cy), row_h * 0.28f, sel ? IM_COL32(255, 160, 40, 255) : IM_COL32(240, 240, 240, 255),
                  IM_COL32(20, 20, 20, 220));
        }
        if (hovered && std::fabs(io.MousePos.x - x) < 6.0f && std::fabs(io.MousePos.y - cy) < lane_h * 0.5f) {
          key_hit = std::pair{t, k};
        }
      }
    }
  }
  /* The reach, as a band of frames either side: Shift's blend in blue around the mouse; otherwise the proportional
   * circle around the dot dragged or under the mouse. */
  const auto band = [&](size_t r, float frame, float reach, ImU32 fill, ImU32 edge) {
    const float top = lane_top(r) + 1.0f, bottom = lane_top(r) + lane_h - 1.0f;
    const float xa = x_of(frame - reach), xb = x_of(frame + reach);
    dl->AddRectFilled(ImVec2(xa, top), ImVec2(std::max(xb, xa + 1.0f), bottom), fill, 2.0f);
    dl->AddLine(ImVec2(xa, top), ImVec2(xa, bottom), edge);
    dl->AddLine(ImVec2(xb, top), ImVec2(xb, bottom), edge);
    reach_label(dl, ImVec2(xb + 3.0f, top), reach, edge);
  };
  if (blending && lane) {
    band(*lane, std::round(frame_of(io.MousePos.x)), ts.path_blend_radius, BLEND_FILL, BLEND_EDGE);
  }
  else if (g_tick.active && g_tick.dragging) {
    for (size_t r = 0; r < targets.size(); r++) {
      if (targets[r].same(g_tick.target)) {
        band(r, float(g_tick.frame), size, REACH_FILL, REACH_EDGE);
      }
    }
  }
  else if (hover) {
    band(hover->lane, float(hover->frame), size, REACH_FILL, REACH_EDGE);
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
  if (key_hit && !g_tick.active && !stroke && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Right))
  {
    key_menu_open(key_hit->first, key_hit->second, "##path_key_menu_chart");
  }
  key_menu(C, "##path_key_menu_chart");
  if (stroke) {
    blend_update(C, frame_of(io.MousePos.x));
    return;
  }
  /* Shift held: the wheel sizes the blend. */
  if (blending) {
    reach_wheel(ts.path_blend_radius);
  }
  auto &g = g_tick;
  if (!g.active) {
    if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      return;
    }
    if (blending) {
      blend_begin(C, 0, targets[*lane], int(std::lround(frame_of(io.MousePos.x))), INT32_MIN / 2, INT32_MAX / 2);
      return;
    }
    if (key_hit && !hover && motion_path_is_key(doc, key_hit->first, key_hit->second)) {
      /* A key: selected as on the path (Ctrl adds or takes out), so X deletes it and the menus act on it. */
      OpArgs args = target_args(doc, key_hit->first, key_hit->second);
      if (io.KeyCtrl) {
        args["extend"] = "1";
      }
      operator_call(C, "anim.motion_path_select", args);
      return;
    }
    if (!edit) {
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
    if (io.KeyCtrl) {
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
  /* The wheel while dragging sizes the reach, the dots around following at once. */
  const bool resized = g.dragging && reach_wheel(ts.path_chart_size);
  if (!g.dragging || io.MouseDelta.x != 0.0f || resized) {
    g.dragging = true;
    doc = g.snapshot;
    const float progress = (frame_of(io.MousePos.x) - float(g.start)) / float(std::max(g.end - g.start, 1));
    g.changed = motion_path_slide_progress(doc, g.target, g.frame, progress, reach_slide(ts, ts.path_chart_size));
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

/* The graph's view: frames across, the way along up. It fits the stretch whenever a different one is shown, and is
 * otherwise the user's (wheel zoom, middle drag pan, Ctrl middle drag stretch, Home to fit again), so edits on the
 * stretch never move it. */
static struct {
  bool valid = false;
  MotionPathTarget target;
  int start = 0, end = 0;
  float f0 = 0.0f, f1 = 1.0f, y0 = 0.0f, y1 = 1.0f;
  /* A middle drag: panning, or (Ctrl) stretching the axes around where it was pressed. */
  bool nav = false, stretch = false;
  ImVec2 press;
  float pf0 = 0.0f, pf1 = 1.0f, py0 = 0.0f, py1 = 1.0f;
} g_view;

static void graph_fit(const MotionPathTarget &t, const MotionPathStretch &s)
{
  auto &v = g_view;
  const int n = s.end - s.start;
  /* A sliver of the stretches either side, and all of the stretch's overshoot, with a margin. */
  const float pad = std::max(0.5f, float(n) * 0.12f);
  float lo = 0.0f, hi = 1.0f;
  for (const float u : s.progress) {
    lo = std::min(lo, u);
    hi = std::max(hi, u);
  }
  const float margin = (hi - lo) * 0.12f;
  v.valid = true;
  v.target = t;
  v.start = s.start;
  v.end = s.end;
  v.f0 = float(s.start) - pad;
  v.f1 = float(s.end) + pad;
  v.y0 = lo - margin;
  v.y1 = hi + margin;
  v.nav = false;
}

static void path_timing_graph(Context &C, const MotionPathTarget &t, const MotionPathStretch &s, ImVec2 min,
                              ImVec2 max)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  const int n = s.end - s.start;
  auto &v = g_view;
  if (!v.valid || !v.target.same(t) || v.start != s.start || v.end != s.end) {
    graph_fit(t, s);
  }
  const float w = max.x - min.x, h = max.y - min.y;
  const auto to_screen = [&](float frame, float u) {
    return ImVec2(min.x + (frame - v.f0) / (v.f1 - v.f0) * w, max.y - (u - v.y0) / (v.y1 - v.y0) * h);
  };
  const auto frame_at = [&](float x) { return v.f0 + (x - min.x) / w * (v.f1 - v.f0); };
  const auto u_at = [&](float y) { return v.y0 + (max.y - y) / h * (v.y1 - v.y0); };
  ImGui::SetCursorScreenPos(min);
  ImGui::InvisibleButton("##path_graph", ImVec2(w, h),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  /* The wheel is the graph's here, not the sidebar's scroll. */
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
  const bool hovered = ImGui::IsItemHovered();

  /* Navigation, as the viewport's: not while a dot is dragged, when the wheel sizes its reach instead, nor with
   * Shift held, when it sizes the blend. */
  const bool stroke = g_blend.active && g_blend.owner == 1;
  if (!g_graph.active && !g_graph_scale.active && !stroke && !io.KeyShift) {
    if (hovered && io.MouseWheel != 0.0f) {
      const float factor = std::pow(0.85f, io.MouseWheel);
      const float mf = frame_at(io.MousePos.x), mu = u_at(io.MousePos.y);
      v.f0 = mf - (mf - v.f0) * factor;
      v.f1 = mf + (v.f1 - mf) * factor;
      v.y0 = mu - (mu - v.y0) * factor;
      v.y1 = mu + (v.y1 - mu) * factor;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
      v.nav = true;
      v.stretch = io.KeyCtrl;
      v.press = io.MousePos;
      v.pf0 = v.f0;
      v.pf1 = v.f1;
      v.py0 = v.y0;
      v.py1 = v.y1;
    }
    if (v.nav && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      v.nav = false;
    }
    if (v.nav) {
      const float dx = io.MousePos.x - v.press.x, dy = io.MousePos.y - v.press.y;
      if (v.stretch) {
        /* Right and up stretch it out (zoom in on that axis), around where it was pressed. */
        const float sx = std::exp(-dx * 0.01f), sy = std::exp(dy * 0.01f);
        const float pf = v.pf0 + (v.press.x - min.x) / w * (v.pf1 - v.pf0);
        const float pu = v.py0 + (max.y - v.press.y) / h * (v.py1 - v.py0);
        v.f0 = pf - (pf - v.pf0) * sx;
        v.f1 = pf + (v.pf1 - pf) * sx;
        v.y0 = pu - (pu - v.py0) * sy;
        v.y1 = pu + (v.py1 - pu) * sy;
      }
      else {
        const float df = dx / w * (v.pf1 - v.pf0), du = dy / h * (v.py1 - v.py0);
        v.f0 = v.pf0 - df;
        v.f1 = v.pf1 - df;
        v.y0 = v.py0 + du;
        v.y1 = v.py1 + du;
      }
    }
    if (hovered && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
      graph_fit(t, s);
    }
    /* Not so far in or out that it stops making sense. */
    const auto keep = [](float &a, float &b, float least, float most) {
      const float mid = (a + b) * 0.5f, half = std::clamp(b - a, least, most) * 0.5f;
      a = mid - half;
      b = mid + half;
    };
    keep(v.f0, v.f1, 0.5f, 100000.0f);
    keep(v.y0, v.y1, 0.01f, 100.0f);
  }

  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(min, max, true);
  dl->AddRectFilled(min, max, IM_COL32(0x1d, 0x1d, 0x1d, 255), 3.0f);
  /* Frame lines, thinned out when zoomed far out. */
  const float per_frame = w / (v.f1 - v.f0);
  const int step = std::max(1, int(std::ceil(5.0f / per_frame)));
  for (int f = int(std::ceil(v.f0 / float(step))) * step; float(f) <= v.f1; f += step) {
    const float x = to_screen(float(f), 0.0f).x;
    dl->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(0x2a, 0x2a, 0x2a, 255));
  }
  /* Past the key and before the start: shaded, the 0 and 100% lines bright. */
  dl->AddRectFilled(min, ImVec2(max.x, std::max(to_screen(0.0f, 1.0f).y, min.y)), IM_COL32(0x2a, 0x24, 0x18, 255));
  dl->AddRectFilled(ImVec2(min.x, std::min(to_screen(0.0f, 0.0f).y, max.y)), max, IM_COL32(0x2a, 0x24, 0x18, 255));
  for (const float u : {0.0f, 1.0f}) {
    const float y = to_screen(0.0f, u).y;
    dl->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), IM_COL32(0x60, 0x60, 0x60, 255));
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
      if (f < v.f0 - 1.0f || f > v.f1 + 1.0f) {
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
  const bool blending = stroke || (hovered && !g_graph.active && !g_graph_scale.active && !g_blend.active && !v.nav &&
                                   io.KeyShift && !io.KeyCtrl && !io.KeyAlt);
  const float size = ts.path_graph_size;
  std::optional<int> hover;
  float best = 9.0f;
  /* Dots out of view: an arrow at the edge toward each; clicking one fits the view again. */
  bool on_arrow = false;
  for (int k = 1; k < n; k++) {
    const int frame = s.start + k;
    const ImVec2 p = to_screen(float(frame), s.progress[size_t(k)]);
    if (p.x < min.x || p.x > max.x || p.y < min.y || p.y > max.y) {
      const ImVec2 e(std::clamp(p.x, min.x + 7.0f, max.x - 7.0f), std::clamp(p.y, min.y + 7.0f, max.y - 7.0f));
      float ax = p.x - e.x, ay = p.y - e.y;
      const float len = std::max(std::hypot(ax, ay), 1e-3f);
      ax /= len;
      ay /= len;
      const Color c = motion_path_time_color(float(k) / float(n));
      dl->AddTriangleFilled(ImVec2(e.x + ax * 6.0f, e.y + ay * 6.0f),
                            ImVec2(e.x - ax * 3.0f - ay * 4.5f, e.y - ay * 3.0f + ax * 4.5f),
                            ImVec2(e.x - ax * 3.0f + ay * 4.5f, e.y - ay * 3.0f - ax * 4.5f),
                            ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 0.9f)));
      on_arrow |= hovered && std::hypot(io.MousePos.x - e.x, io.MousePos.y - e.y) < 8.0f;
      continue;
    }
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
    /* The dots the dragged one pulls along: a faint ring, fainter as they follow less. */
    else if (g_graph.active && g_graph.dragging) {
      const float wt = reach_weight(ts, frame - g_graph.frame, size);
      if (wt > 0.0f) {
        dl->AddCircle(p, 7.5f, IM_COL32(255, 255, 255, int(40.0f + 110.0f * wt)), 0, 1.0f);
      }
    }
    const float d = std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y);
    if (hovered && !g_graph.active && !blending && d < best) {
      best = d;
      hover = frame;
    }
  }
  std::optional<int> key_hit;
  /* The span's breakdowns, where its timing reaches them. */
  for (const int k : s.breakdowns) {
    const int j = k - s.start;
    if (j < 0 || j >= int(s.progress.size())) {
      continue;
    }
    const ImVec2 p = to_screen(float(k), s.progress[size_t(j)]);
    breakdown_dot(dl, p, 4.0f, motion_path_key_selected(doc, t, k));
    if (hovered && std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y) < 8.0f) {
      key_hit = k;
    }
  }
  for (const int k : {s.start, s.end}) {
    const ImVec2 p = to_screen(float(k), k == s.start ? 0.0f : 1.0f);
    const bool sel = k > 0 && motion_path_key_selected(doc, t, k);
    diamond(dl, p, 5.5f, sel ? IM_COL32(255, 160, 40, 255) : IM_COL32(240, 240, 240, 255),
            IM_COL32(20, 20, 20, 220));
    if (hovered && k > 0 && std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y) < 8.0f) {
      key_hit = k;
    }
  }
  /* The reach as a circle: Shift's blend in blue around the mouse; else the proportional circle around the dot
   * dragged or under the mouse. */
  const auto circle = [&](ImVec2 c, float reach, ImU32 fill, ImU32 edge) {
    const float r = std::max(reach * per_frame, 3.0f);
    dl->AddCircleFilled(c, r, fill, 48);
    dl->AddCircle(c, r, edge, 48, 1.0f);
    reach_label(dl, ImVec2(c.x + r * 0.72f + 2.0f, c.y - r * 0.72f - ImGui::GetFontSize() * 0.8f), reach, edge);
  };
  if (blending) {
    circle(io.MousePos, ts.path_blend_radius, BLEND_FILL, BLEND_EDGE);
  }
  else if (g_graph.active && g_graph.dragging) {
    circle(to_screen(float(g_graph.frame), s.progress[size_t(g_graph.frame - s.start)]), size, REACH_FILL,
           REACH_EDGE);
  }
  else if (hover) {
    circle(to_screen(float(*hover), s.progress[size_t(*hover - s.start)]), size, REACH_FILL, REACH_EDGE);
  }
  dl->PopClipRect();

  if (hover) {
    wm.path_hover_target = t;
    wm.path_hover_frame = *hover;
    wm.path_hover_tick = ImGui::GetFrameCount();
    const float u = s.progress[size_t(*hover - s.start)];
    ImGui::SetTooltip("Frame %d: %.0f%% along", *hover, u * 100.0f);
  }

  if (stroke) {
    blend_update(C, frame_at(io.MousePos.x));
    return;
  }
  /* Shift held: the wheel sizes the blend. */
  if (blending) {
    reach_wheel(ts.path_blend_radius);
  }
  /* Alt S: scale the spacing around the selected dots, as in the viewport, with the graph's reach. */
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
    PathSlide slide = reach_slide(ts, size);
    slide.keep_order = true;
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
    if (key_hit) {
      key_menu_open(t, *key_hit, "##path_key_menu_graph");
    }
    else {
      ImGui::OpenPopup("##path_timing_menu");
    }
  }
  key_menu(C, "##path_key_menu_graph");
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
    if (key_hit && !blending && motion_path_is_key(doc, t, *key_hit)) {
      OpArgs args = target_args(doc, t, *key_hit);
      if (io.KeyCtrl) {
        args["extend"] = "1";
      }
      operator_call(C, "anim.motion_path_select", args);
      return;
    }
    if (on_arrow) {
      graph_fit(t, s);
      return;
    }
    if (blending) {
      blend_begin(C, 1, t, int(std::lround(frame_at(io.MousePos.x))), s.start, s.end);
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
    if (io.KeyCtrl) {
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
  /* The wheel while dragging sizes the reach, the dots around following at once. */
  if (g.dragging) {
    reach_wheel(ts.path_graph_size);
  }
  g.dragging = true;
  doc = g.snapshot;
  /* Up is further along; Ctrl puts it on the even line. */
  float progress = u_at(io.MousePos.y);
  if (io.KeyCtrl) {
    progress = float(g.frame - s.start) / float(std::max(n, 1));
  }
  g.changed = motion_path_slide_progress(doc, g.target, g.frame, progress, reach_slide(ts, ts.path_graph_size));
  C.undo->mark_changed();
}

bool timeline_sidebar_tab(ImVec2 min, ImVec2 max, bool open, ImVec2 &r_min, ImVec2 &r_max)
{
  constexpr float W = 12.0f, H = 24.0f;
  if (open || max.x - min.x < W * 4.0f || max.y - min.y < H * 2.0f) {
    return false;
  }
  const float y = min.y + (max.y - min.y) / 3.0f;
  r_min = ImVec2(max.x - W, y - H * 0.5f);
  r_max = ImVec2(max.x, y + H * 0.5f);
  return true;
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
  if (!motion_path_timing_stretch(doc, motion_path_timing_targets(C), wm.path_timing_target,
                                  wm.path_timing_end, wm.path_timing_frame, t, s))
  {
    ImGui::TextWrapped("Trace something with keys a few frames apart: its stretches show here.");
    ImGui::EndChild();
    return;
  }
  /* Which thing and stretch, with arrows to the ones either side. */
  if (ImGui::ArrowButton("##stretch_prev", ImGuiDir_Left)) {
    wm.path_timing_target = t;
    wm.path_timing_end = s.end;
    wm.path_timing_frame = doc.frame_current;
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
    wm.path_timing_frame = doc.frame_current;
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
    ImGui::DragFloat("##along", &along_edit, 0.25f, -100.0f, 200.0f, "Along %.1f%%");
    along_live = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      OpArgs args = target_args(doc, t, dot);
      args["progress"] = std::to_string(along_edit / 100.0f);
      operator_call(C, "anim.motion_path_slide", args);
    }
    ImGui::SetNextItemWidth(w);
    ImGui::DragFloat("##speed", &speed_edit, 0.01f, -float(s.end - s.start), float(s.end - s.start) * 0.5f,
                     "Speed %.2f");
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
