#include "editors/timeline_path.hh"

#include "imgui_internal.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "anim/motion_path.hh"
#include "anim/timing_curve.hh"
#include "data/document.hh"
#include "data/vector.hh"
#include "editors/editors.hh"
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
    int focus_start = 0, focus_end = 0;
    if (timeline_key_focus_span(t, focus_start, focus_end)) {
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
        if (s.held(k)) {
          /* On 2s or 3s: it shows the frame before's pose, so it has no tick (nothing to drag) of its own. */
          continue;
        }
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

/* Joined buttons, Blender's expanded enum: the chosen one blue. Returns the one clicked (not the chosen), or -1. */
static int segmented_buttons(const char *id, int value, int count, const char *const *names, const char *const *tips,
                             float total, float h)
{
  const ImGuiStyle &style = ImGui::GetStyle();
  const float each = std::floor((total - float(count - 1)) / float(count));
  int clicked = -1;
  ImGui::PushID(id);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  for (int k = 0; k < count; k++) {
    if (k > 0) {
      ImGui::SameLine(0, 1);
    }
    const float seg_w = k == count - 1 ? total - (each + 1.0f) * float(count - 1) : each;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(k);
    if (ImGui::InvisibleButton("##segment", ImVec2(seg_w, h)) && k != value) {
      clicked = k;
    }
    const bool on = k == value, hovered = ImGui::IsItemHovered();
    const ImU32 back = ImGui::GetColorU32(on ? ImGuiCol_ButtonActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    const ImDrawFlags corners = k == 0 ? ImDrawFlags_RoundCornersLeft
                                : k == count - 1 ? ImDrawFlags_RoundCornersRight
                                                 : ImDrawFlags_RoundCornersNone;
    dl->AddRectFilled(at, ImVec2(at.x + seg_w, at.y + h), back, style.FrameRounding, corners);
    const ImVec2 text = ImGui::CalcTextSize(names[k]);
    dl->AddText(ImVec2(at.x + std::floor((seg_w - text.x) * 0.5f), at.y + std::floor((h - text.y) * 0.5f)),
                ImGui::GetColorU32(on ? ImVec4(1, 1, 1, 1) : style.Colors[ImGuiCol_Text]), names[k]);
    if (tips && tips[k] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", tips[k]);
    }
    ImGui::PopID();
  }
  ImGui::PopID();
  return clicked;
}

/* The Path Timing graph's Edit mode: a curve point (side 0) or one of its handles (-1 in, 1 out) dragged, the curve
 * worked out from the one pressed on, live on the document as it was pressed; run as its operator once let go (one
 * undo step). */
static struct {
  bool active = false;
  bool dragging = false;
  /* The graph (one Path Timing editor's) it goes on in: another editor showing the stretch only draws it. */
  ImGuiID owner = 0;
  MotionPathTarget target;
  int frame = 0, n = 1;
  int point = -1, side = 0;
  ImVec2 press;
  float2 was{};
  float2 now{};
  bool free = false;
  TimingCurve curve;
  Document snapshot;
} g_edit;

/* Its Sculpt mode: a stroke going on (its places in frames and the way along), shown live, run as anim.timing_sculpt
 * once let go; and F sizing the brush (Blender's: the circle stays where it was, the mouse's distance its radius). */
static struct {
  bool active = false;
  ImGuiID owner = 0;
  MotionPathTarget target;
  int frame = 0, start = 0, n = 1;
  TimingBrush brush = TimingBrush::Grab;
  float aspect = 1.0f;
  std::vector<float2> path;
  ImVec2 last;
  TimingCurve curve;
  Document snapshot;
} g_stroke;

static struct {
  bool active = false;
  ImGuiID owner = 0;
  /* The brush being sized, as it was when F was pressed (Shift let go or pressed after doesn't change which). */
  int brush = 0;
  ImVec2 center;
  float was = 0.0f;
} g_resize;

ImVec2 timing_resize_center(ImVec2 mouse, float radius, float per_frame)
{
  return ImVec2(mouse.x - radius * std::max(per_frame, 1e-3f), mouse.y);
}

float timing_resize_radius(ImVec2 center, ImVec2 mouse, float per_frame)
{
  return std::clamp(std::hypot(mouse.x - center.x, mouse.y - center.y) / std::max(per_frame, 1e-3f), 0.1f, 500.0f);
}

/* The graph's view: frames across, the way along up. It fits the stretch whenever a different one is shown, and is
 * otherwise the user's (wheel zoom, middle drag pan, Ctrl middle drag stretch, Home to fit again), so edits on the
 * stretch never move it. */
struct GraphView {
  bool valid = false;
  MotionPathTarget target;
  int start = 0, end = 0;
  float f0 = 0.0f, f1 = 1.0f, y0 = 0.0f, y1 = 1.0f;
  /* A middle drag: panning, or (Ctrl) stretching the axes around where it was pressed. */
  bool nav = false, stretch = false;
  ImVec2 press;
  float pf0 = 0.0f, pf1 = 1.0f, py0 = 0.0f, py1 = 1.0f;
};
/* Each Path Timing editor's own (by its graph's ImGui id). */
static std::unordered_map<ImGuiID, GraphView> g_views;

static void graph_fit(GraphView &v, const MotionPathTarget &t, const MotionPathStretch &s)
{
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

/* Whether the graph's picked point is one of the stretch shown, and its index. */
static int picked_point(const WindowManager &wm, const MotionPathTarget &t, const MotionPathStretch &s,
                        const TimingCurve &curve)
{
  if (wm.path_timing_point < 0 || !wm.path_timing_point_target.same(t) || wm.path_timing_point_end != s.end ||
      size_t(wm.path_timing_point) >= curve.points.size())
  {
    return -1;
  }
  return wm.path_timing_point;
}

static void point_pick(WindowManager &wm, const MotionPathTarget &t, const MotionPathStretch &s, int point)
{
  wm.path_timing_point_target = t;
  wm.path_timing_point_end = s.end;
  wm.path_timing_point = point;
}

static void path_timing_graph(Context &C, const MotionPathTarget &t, const MotionPathStretch &s, ImVec2 min,
                              ImVec2 max)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  ToolSettings &ts = wm.tool_settings;
  const ImGuiIO &io = ImGui::GetIO();
  const int n = s.end - s.start;
  const ImGuiID graph_id = ImGui::GetID("##path_graph");
  GraphView &v = g_views[graph_id];
  if (!v.valid || !v.target.same(t) || v.start != s.start || v.end != s.end) {
    graph_fit(v, t, s);
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
  /* What this graph has going on, and whether another editor's graph has something going on. */
  const bool mine = (g_edit.active && g_edit.owner == graph_id) || (g_stroke.active && g_stroke.owner == graph_id) ||
                    (g_resize.active && g_resize.owner == graph_id);
  if (hovered || mine) {
    wm.path_graph_hovered = true;
    wm.path_graph_hovered_frame = ImGui::GetFrameCount();
  }

  /* The curve: a Custom stretch's own, else the one its timing makes (even: a straight line). A span across
   * breakdowns is timed by each stretch's own curve, so only shown there. */
  TimingCurve curve;
  int start = s.start, frames = n;
  const bool editable = s.breakdowns.empty() && n >= 2 &&
                        motion_path_timing_curve(doc, t, s.start + 1, curve, &start, &frames) && frames == n;
  if (editable && curve.empty()) {
    curve = timing_even();
  }
  const auto curve_screen = [&](float2 p) { return to_screen(float(s.start) + p.x * float(n), p.y); };
  const bool sculpt = ts.path_graph_sculpt;

  /* Navigation, as the viewport's: not while something is dragged. */
  const bool busy = g_edit.active || g_stroke.active || g_resize.active;
  if (!busy) {
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
      graph_fit(v, t, s);
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
  const float per_frame = w / (v.f1 - v.f0);
  /* Frames a whole way along is, as shown: the brush's circle is round on screen. */
  const float aspect = (h / (v.y1 - v.y0)) / per_frame;

  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(min, max, true);
  dl->AddRectFilled(min, max, IM_COL32(0x1d, 0x1d, 0x1d, 255), 3.0f);
  /* Frame lines, thinned out when zoomed far out. */
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
                ImVec2(da.x + (db.x - da.x) * b, da.y + (db.y - da.y) * b), IM_COL32(150, 150, 150, 120), 1.0f);
  }
  /* The stretches either side, grey and faint: the one before ending at 0, the one after starting at 1. */
  const Color grey = motion_path_unlit_color();
  const ImU32 grey_col = ImGui::ColorConvertFloat4ToU32(ImVec4(grey.r, grey.g, grey.b, grey.a * 0.4f));
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
      dl->AddCircleFilled(to_screen(f, before ? o.progress[k] - 1.0f : 1.0f + o.progress[k]), 2.0f, grey_col);
    }
  }
  /* The curve, then each frame's dot on it, small and faint (where the frames land). */
  if (editable) {
    const int samples = std::clamp(int(w / 2.0f), 32, 600);
    ImVec2 prev = curve_screen({0.0f, 0.0f});
    for (int j = 1; j <= samples; j++) {
      const float x = float(j) / float(samples);
      const ImVec2 p = curve_screen({x, timing_progress(curve, x)});
      dl->AddLine(prev, p, IM_COL32(200, 200, 200, 255), 1.6f);
      prev = p;
    }
  }
  else {
    for (int k = 0; k < n; k++) {
      dl->AddLine(to_screen(float(s.start + k), s.progress[size_t(k)]),
                  to_screen(float(s.start + k + 1), s.progress[size_t(k + 1)]), IM_COL32(200, 200, 200, 255), 1.6f);
    }
  }
  for (int k = 1; k < n; k++) {
    if (s.held(k)) {
      continue;
    }
    const Color c = motion_path_time_color(float(k) / float(n));
    dl->AddCircleFilled(to_screen(float(s.start + k), s.progress[size_t(k)]), 2.5f,
                        ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 0.55f)));
  }

  /* Edit mode: the curve's points (its ends are the keys' diamonds) and their handles, as a drawing's. */
  const int picked = editable ? picked_point(wm, t, s, curve) : -1;
  struct Hit {
    int point = -1, side = 0;
  };
  std::optional<Hit> hit;
  float best = 8.0f;
  const bool can_edit = editable && !sculpt && !g_stroke.active;
  if (can_edit) {
    for (size_t i = 0; i < curve.points.size(); i++) {
      const TimingPoint &p = curve.points[i];
      const ImVec2 at = curve_screen(p.co);
      float2 in, out;
      timing_point_handles(curve, i, in, out);
      for (const int side : {-1, 1}) {
        if ((side < 0 && i == 0) || (side > 0 && i + 1 == curve.points.size())) {
          continue;
        }
        const ImVec2 tip = curve_screen(p.co + (side < 0 ? in : out));
        const bool on = int(i) == picked || (g_edit.active && g_edit.point == int(i));
        dl->AddLine(at, tip, on ? IM_COL32(255, 175, 90, 230) : IM_COL32(160, 160, 160, 170), 1.0f);
        dl->AddCircleFilled(tip, 3.0f, on ? IM_COL32(255, 175, 90, 255) : IM_COL32(190, 190, 190, 220));
        const float d = std::hypot(io.MousePos.x - tip.x, io.MousePos.y - tip.y);
        if (hovered && !g_edit.active && d < best) {
          best = d;
          hit = Hit{int(i), side};
        }
      }
      if (i == 0 || i + 1 == curve.points.size()) {
        continue;
      }
      const bool on = int(i) == picked;
      dl->AddCircleFilled(at, 4.5f, on ? IM_COL32(255, 160, 40, 255) : IM_COL32(235, 235, 235, 255));
      dl->AddCircle(at, 4.5f, IM_COL32(20, 20, 20, 220), 0, 1.2f);
      const float d = std::hypot(io.MousePos.x - at.x, io.MousePos.y - at.y);
      /* A point under the mouse wins over a handle tip just as near. */
      if (hovered && !g_edit.active && d < best + 1.0f) {
        best = d;
        hit = Hit{int(i), 0};
      }
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
    if (hovered && k > 0 && !hit && std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y) < 8.0f) {
      key_hit = k;
    }
  }

  /* Sculpt mode: the brush (Shift swaps it while held), its circle and the falloff's half-way ring. */
  const TimingBrush brush = g_stroke.active ? g_stroke.brush
                            : g_resize.active ? TimingBrush(g_resize.brush)
                            : io.KeyShift   ? (ts.path_timing_brush == TimingBrush::Grab ? TimingBrush::Smooth
                                                                                        : TimingBrush::Grab)
                                            : ts.path_timing_brush;
  const int bi = int(brush);
  if (editable && sculpt && (hovered || mine)) {
    const ImVec2 c = g_resize.active ? g_resize.center : io.MousePos;
    const float r = std::max(ts.path_timing_radius[bi] * per_frame, 3.0f);
    const ImU32 edge = brush == TimingBrush::Grab ? IM_COL32(255, 255, 255, 200) : IM_COL32(140, 190, 255, 220);
    dl->AddCircle(c, r, edge, 64, 1.4f);
    dl->AddCircle(c, r * 0.5f, (edge & 0x00ffffffu) | (90u << 24), 48, 1.0f);
  }
  dl->PopClipRect();

  if (hit && !busy) {
    const TimingPoint &p = curve.points[size_t(hit->point)];
    if (hit->side == 0) {
      ImGui::SetTooltip("Frame %.2f: %.1f%% along", double(float(s.start) + p.co.x * float(n)), double(p.co.y * 100.0f));
    }
  }

  /* The right click's menu: the stretch's shapes, Even Spacing for the dots picked; on a key, the key's. */
  if (hovered && !busy && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
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

  /* F: the brush sized by the mouse's distance from where the circle was. */
  auto &rs = g_resize;
  /* F goes to the graph under the mouse. */
  if (wm.path_graph_resize_request && hovered) {
    wm.path_graph_resize_request = false;
    if (editable && sculpt && !busy) {
      rs.active = true;
      rs.owner = graph_id;
      rs.brush = bi;
      rs.center = timing_resize_center(io.MousePos, ts.path_timing_radius[bi], per_frame);
      rs.was = ts.path_timing_radius[bi];
    }
  }
  if (rs.active && rs.owner == graph_id) {
    ts.path_timing_radius[rs.brush] = timing_resize_radius(rs.center, io.MousePos, per_frame);
    ImGui::SetTooltip("Radius: %.1f frames", double(ts.path_timing_radius[rs.brush]));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      ts.path_timing_radius[rs.brush] = rs.was;
      rs = {};
    }
    else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      rs = {};
    }
    return;
  }

  /* A sculpt stroke. */
  auto &st = g_stroke;
  if (st.active && st.owner == graph_id) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      doc = std::move(st.snapshot);
      st = {};
      return;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      doc = std::move(st.snapshot);
      std::vector<float2> path = st.path;
      for (float2 &p : path) {
        p.x += float(st.start);
      }
      OpArgs args = target_args(doc, st.target, st.frame);
      args["brush"] = st.brush == TimingBrush::Grab ? "GRAB" : "SMOOTH";
      args["path"] = timing_stroke_string(path);
      args["radius"] = std::to_string(ts.path_timing_radius[int(st.brush)]);
      args["strength"] = std::to_string(ts.path_timing_strength[int(st.brush)]);
      args["aspect"] = std::to_string(st.aspect);
      args["lock_x"] = ts.path_timing_lock_x ? "1" : "0";
      st = {};
      operator_call(C, "anim.timing_sculpt", args);
      return;
    }
    if (std::hypot(io.MousePos.x - st.last.x, io.MousePos.y - st.last.y) >= 1.0f) {
      st.last = io.MousePos;
      st.path.push_back({frame_at(io.MousePos.x) - float(st.start), u_at(io.MousePos.y)});
      doc = st.snapshot;
      motion_path_timing_curve_set(doc, st.target, st.frame,
                                   timing_sculpt(st.curve, st.n, st.brush, st.path, ts.path_timing_radius[int(st.brush)],
                                                 ts.path_timing_strength[int(st.brush)], st.aspect,
                                                 ts.path_timing_lock_x));
      C.undo->mark_changed();
    }
    return;
  }

  /* An Edit drag. */
  auto &g = g_edit;
  if (g.active && g.owner == graph_id) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      if (g.dragging) {
        doc = std::move(g.snapshot);
      }
      g = {};
      return;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (g.dragging) {
        doc = std::move(g.snapshot);
        OpArgs args = target_args(doc, g.target, g.frame);
        args["index"] = std::to_string(g.point);
        args["x"] = std::to_string(g.now.x);
        args["y"] = std::to_string(g.now.y);
        if (g.side != 0) {
          args["side"] = g.side < 0 ? "IN" : "OUT";
          if (g.free) {
            args["free"] = "1";
          }
        }
        const char *op = g.side == 0 ? "anim.timing_point_move" : "anim.timing_handle_move";
        g = {};
        operator_call(C, op, args);
      }
      else {
        g = {};
      }
      return;
    }
    if (!g.dragging && std::hypot(io.MousePos.x - g.press.x, io.MousePos.y - g.press.y) < 3.0f) {
      return;
    }
    g.dragging = true;
    /* The mouse's move in the curve's terms: a share of the stretch's time across, the way along up. */
    const float2 move{(frame_at(io.MousePos.x) - frame_at(g.press.x)) / float(g.n), u_at(io.MousePos.y) - u_at(g.press.y)};
    TimingCurve moved = g.curve;
    g.free = io.KeyAlt;
    if (g.side == 0) {
      timing_point_move(moved, size_t(g.point), g.was + move);
      g.now = moved.points[size_t(g.point)].co;
    }
    else {
      timing_handle_set(moved, size_t(g.point), g.side > 0, g.was + move, g.free);
      g.now = g.side > 0 ? moved.points[size_t(g.point)].out : moved.points[size_t(g.point)].in;
    }
    doc = g.snapshot;
    motion_path_timing_curve_set(doc, g.target, g.frame, moved);
    C.undo->mark_changed();
    return;
  }

  /* Nothing new starts here while another editor's graph has a drag, stroke or sizing going on. */
  if (busy || !hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    return;
  }
  if (key_hit && motion_path_is_key(doc, t, *key_hit)) {
    OpArgs args = target_args(doc, t, *key_hit);
    if (io.KeyCtrl) {
      args["extend"] = "1";
    }
    operator_call(C, "anim.motion_path_select", args);
    return;
  }
  if (!editable) {
    return;
  }
  if (sculpt) {
    animation_sync(doc);
    st.active = true;
    st.owner = graph_id;
    st.target = t;
    st.frame = s.start + 1;
    st.start = s.start;
    st.n = n;
    st.brush = brush;
    st.aspect = aspect;
    st.curve = curve;
    st.snapshot = doc;
    st.last = io.MousePos;
    st.path = {{frame_at(io.MousePos.x) - float(s.start), u_at(io.MousePos.y)}};
    return;
  }
  if (!hit) {
    if (io.KeyCtrl) {
      /* Ctrl click: a point there, on the curve's time under the mouse. */
      const float x = (frame_at(io.MousePos.x) - float(s.start)) / float(n);
      OpArgs args = target_args(doc, t, s.start + 1);
      args["x"] = std::to_string(x);
      args["y"] = std::to_string(u_at(io.MousePos.y));
      if (operator_call(C, "anim.timing_point_add", args) == OpStatus::Finished) {
        TimingCurve now;
        if (motion_path_timing_curve(doc, t, s.start + 1, now)) {
          int nearest = -1;
          float d = 1e9f;
          for (size_t i = 1; i + 1 < now.points.size(); i++) {
            if (std::abs(now.points[i].co.x - x) < d) {
              d = std::abs(now.points[i].co.x - x);
              nearest = int(i);
            }
          }
          point_pick(wm, t, s, nearest);
        }
      }
      return;
    }
    point_pick(wm, t, s, -1);
    return;
  }
  point_pick(wm, t, s, hit->point);
  animation_sync(doc);
  g.active = true;
  g.dragging = false;
  g.owner = graph_id;
  g.target = t;
  g.frame = s.start + 1;
  g.n = n;
  g.point = hit->point;
  g.side = hit->side;
  g.press = io.MousePos;
  g.curve = curve;
  const TimingPoint &p = curve.points[size_t(hit->point)];
  if (hit->side == 0) {
    g.was = p.co;
  }
  else {
    /* From the handle as the curve shows it (kept within its segment). */
    float2 in, out;
    timing_point_handles(curve, size_t(hit->point), in, out);
    g.was = hit->side > 0 ? out : in;
  }
  g.now = g.was;
  g.snapshot = doc;
}

void path_timing_draw(Area &area, Context &C)
{
  area_editor_type_menu(area);
  ImGui::Separator();
  /* A graph says it is under the mouse as it draws; until one does this frame, none is. */
  if (C.wm && C.wm->path_graph_hovered_frame != ImGui::GetFrameCount()) {
    C.wm->path_graph_hovered = false;
  }
  if (!C.document || !C.wm) {
    return;
  }
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float w = ImGui::GetContentRegionAvail().x;
  if (wm.tool_settings.tool != Tool::MotionPath) {
    ImGui::TextWrapped("Pick the Motion Path tool to edit the timing of what its paths trace here.");
    return;
  }
  MotionPathTarget t;
  MotionPathStretch s;
  MotionPathPicks picks;
  if (!motion_path_timing_shown(C, t, s, &picks)) {
    ImGui::TextWrapped("Trace something with keys a few frames apart: its stretches show here.");
    return;
  }
  /* Which thing and stretch, with arrows to the ones either side. */
  if (ImGui::ArrowButton("##stretch_prev", ImGuiDir_Left)) {
    if (picks.stretches.empty()) {
      wm.path_timing_target = t;
      wm.path_timing_end = s.end;
      wm.path_timing_frame = doc.frame_current;
    }
    operator_call(C, "anim.motion_path_timing_step", {{"step", "-1"}});
  }
  ImGui::SameLine();
  /* Picks in different stretches: which of them this is; the arrows go through them. */
  std::string title = motion_path_label(doc, t) + "  " + std::to_string(s.start) + "-" + std::to_string(s.end);
  if (picks.spans > 1) {
    title = std::to_string(picks.shown + 1) + " of " + std::to_string(picks.stretches.size()) + ":  " + title;
  }
  const float arrow_w = ImGui::GetFrameHeight();
  const float title_w = std::max(w - arrow_w * 2.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f, 10.0f);
  const ImVec2 tp = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(title_w, arrow_w));
  ImGui::RenderTextEllipsis(dl, ImVec2(tp.x, tp.y + (arrow_w - ImGui::GetFontSize()) * 0.5f),
                            ImVec2(tp.x + title_w, tp.y + arrow_w), tp.x + title_w, title.c_str(), nullptr, nullptr);
  ImGui::SameLine();
  if (ImGui::ArrowButton("##stretch_next", ImGuiDir_Right)) {
    if (picks.stretches.empty()) {
      wm.path_timing_target = t;
      wm.path_timing_end = s.end;
      wm.path_timing_frame = doc.frame_current;
    }
    operator_call(C, "anim.motion_path_timing_step", {{"step", "1"}});
  }
  const ImGuiStyle &style = ImGui::GetStyle();
  ToolSettings &ts = wm.tool_settings;
  const bool sculpt = ts.path_graph_sculpt;
  /* The graph: as tall as the area has room for, over the mode buttons and the mode's settings. */
  const float row = ImGui::GetFrameHeightWithSpacing();
  const float below = row * (sculpt ? 5.0f : 2.0f) + style.ItemSpacing.y * 3.0f + 1.0f;
  const float graph_h = std::max(ImGui::GetContentRegionAvail().y - below, ImGui::GetFontSize() * 4.0f);
  const ImVec2 g0 = ImGui::GetCursorScreenPos();
  path_timing_graph(C, t, s, g0, ImVec2(g0.x + w, g0.y + graph_h));
  ImGui::SetCursorScreenPos(ImVec2(g0.x, g0.y + graph_h + style.ItemSpacing.y));

  /* Edit and Sculpt, right under the graph. */
  const char *modes[] = {"Edit", "Sculpt"};
  const char *mode_tips[] = {"Move the curve's points and handles, as a drawing's: Alt drag frees a handle, Ctrl click "
                             "adds a point, X deletes the one picked",
                             "Brush the curve: Grab moves it, Smooth evens it out (Shift swaps them while held, F "
                             "sizes the brush); it keeps as few points as hold every frame after each stroke"};
  const int mode = segmented_buttons("##graph_mode", sculpt ? 1 : 0, 2, modes, mode_tips, w, ImGui::GetFrameHeight());
  if (mode >= 0) {
    operator_call(C, "anim.timing_graph_set", {{"mode", mode == 1 ? "SCULPT" : "EDIT"}});
  }
  /* A thin line, then the mode's settings. */
  const ImVec2 line = ImGui::GetCursorScreenPos();
  dl->AddLine(ImVec2(line.x, line.y + 1.0f), ImVec2(line.x + w, line.y + 1.0f), IM_COL32(0x48, 0x48, 0x48, 255));
  ImGui::Dummy(ImVec2(w, 3.0f));

  if (!ts.path_graph_sculpt) {
    /* The picked point's frame and how far along it is; a drag shows its value and moves the point once let go. */
    TimingCurve curve;
    const bool has = s.breakdowns.empty() && motion_path_timing_curve(doc, t, s.start + 1, curve) && !curve.empty();
    const int picked = has ? picked_point(wm, t, s, curve) : -1;
    const bool movable = picked > 0 && size_t(picked) + 1 < curve.points.size();
    static float frame_edit = 0.0f, along_edit = 0.0f;
    static bool frame_live = false, along_live = false;
    const int n = s.end - s.start;
    if (movable && !frame_live) {
      frame_edit = float(s.start) + curve.points[size_t(picked)].co.x * float(n);
    }
    if (movable && !along_live) {
      along_edit = curve.points[size_t(picked)].co.y * 100.0f;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Point");
    ImGui::SameLine();
    const float field_w = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(!movable);
    const auto apply = [&](float x, float y) {
      OpArgs args = target_args(doc, t, s.start + 1);
      args["index"] = std::to_string(picked);
      args["x"] = std::to_string(x);
      args["y"] = std::to_string(y);
      operator_call(C, "anim.timing_point_move", args);
    };
    ImGui::SetNextItemWidth(field_w);
    ImGui::DragFloat("##point_frame", &frame_edit, 0.02f, float(s.start), float(s.end), movable ? "%.2f" : "Frame");
    frame_live = ImGui::IsItemActive();
    if (movable && ImGui::IsItemDeactivatedAfterEdit()) {
      apply((frame_edit - float(s.start)) / float(std::max(n, 1)), curve.points[size_t(picked)].co.y);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("The picked point's frame (it stays between its neighbours)");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field_w);
    ImGui::DragFloat("##point_along", &along_edit, 0.25f, -100.0f, 200.0f, movable ? "%.1f%%" : "%%");
    along_live = ImGui::IsItemActive();
    if (movable && ImGui::IsItemDeactivatedAfterEdit()) {
      apply(curve.points[size_t(picked)].co.x, along_edit / 100.0f);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("How far along the picked point is (100%% at the key)");
    }
    ImGui::EndDisabled();
  }
  else {
    /* The brushes: a small pair of their own, under their label, apart from the modes. */
    ImGui::TextUnformatted("Brush");
    const char *brushes[] = {"Grab", "Smooth"};
    const char *brush_tips[] = {"Moves the curve under the brush with the mouse, in time too unless Lock X",
                                "Evens out the curve under the brush"};
    const float pair_w = std::min(w, ImGui::GetFontSize() * 8.0f);
    const int b = segmented_buttons("##timing_brush", int(ts.path_timing_brush), 2, brushes, brush_tips, pair_w,
                                    std::floor(ImGui::GetFrameHeight() * 0.85f));
    if (b >= 0) {
      operator_call(C, "anim.timing_graph_set", {{"brush", b == 0 ? "GRAB" : "SMOOTH"}});
    }
    const int bi = int(ts.path_timing_brush);
    ImGui::SetNextItemWidth(w);
    ui_slider_float("##timing_radius", &ts.path_timing_radius[bi], 0.5f, 20.0f, "Radius %.1f frames", 0, 0.1f,
                    500.0f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("How many frames either side the brush reaches (F over the graph sizes it)");
    }
    ImGui::SetNextItemWidth(w);
    ui_slider_float("##timing_strength", &ts.path_timing_strength[bi], 0.0f, 1.0f, "Strength %.2f");
    ImGui::Checkbox("Lock X", &ts.path_timing_lock_x);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The brushes move the curve up and down only, never in time");
    }
  }
}

}  // namespace oanim
