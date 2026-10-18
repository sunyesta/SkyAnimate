/* Timeline editor, laid out like Blender's: a header with menus, playback buttons and the
 * frame fields, then a ruler with frame numbers, the playhead and the frame range. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "data/document.hh"
#include "editors/editors.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "wm/keymap.hh"

namespace sky {

/* Colours from Blender's default timeline (dope sheet) theme. */
static constexpr ImU32 COL_ICON = IM_COL32(0xe6, 0xe6, 0xe6, 255);
static constexpr ImU32 COL_BUTTON_HOVER = IM_COL32(0x54, 0x54, 0x54, 255);
static constexpr ImU32 COL_REGION = IM_COL32(0x2b, 0x2b, 0x2b, 255);
static constexpr ImU32 COL_OUTSIDE_RANGE = IM_COL32(0x00, 0x00, 0x00, 0x33);
static constexpr ImU32 COL_GRID = IM_COL32(0x3a, 0x3a, 0x3a, 255);
static constexpr ImU32 COL_GRID_MINOR = IM_COL32(0x31, 0x31, 0x31, 255);
static constexpr ImU32 COL_RULER_TEXT = IM_COL32(0x9a, 0x9a, 0x9a, 255);
static constexpr ImU32 COL_PLAYHEAD = IM_COL32(0x47, 0x72, 0xb3, 255);
static constexpr ImU32 COL_RECORD = IM_COL32(0xd6, 0x3c, 0x3c, 255);
/* Editing an action instead of the mainline: Blender's orange for "not the main data". */
static constexpr ImU32 COL_ACTION_ACTIVE = IM_COL32(0x8a, 0x5a, 0x1e, 255);
static constexpr ImU32 COL_ACTION_TINT = IM_COL32(0xd0, 0x8a, 0x2e, 0x14);

enum class Icon { Record, JumpStart, KeyPrev, PlayReverse, Play, Pause, KeyNext, JumpEnd };

/* Triangles are wound clockwise (screen y points down) so ImGui anti-aliases their edges. */
static void tri_right(ImDrawList *dl, float x0, float x1, float cy, float half_h, ImU32 col)
{
  dl->AddTriangleFilled(ImVec2(x0, cy - half_h), ImVec2(x1, cy), ImVec2(x0, cy + half_h), col);
}

static void tri_left(ImDrawList *dl, float x0, float x1, float cy, float half_h, ImU32 col)
{
  dl->AddTriangleFilled(ImVec2(x0, cy - half_h), ImVec2(x0, cy + half_h), ImVec2(x1, cy), col);
}

static void diamond(ImDrawList *dl, ImVec2 c, float r, ImU32 col)
{
  dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), col);
}

static void icon_draw(ImDrawList *dl, Icon icon, ImVec2 c, float s, bool on)
{
  const ImU32 col = COL_ICON;
  switch (icon) {
    case Icon::Record:
      if (on) {
        dl->AddCircleFilled(c, s * 0.8f, COL_RECORD);
      }
      else {
        dl->AddCircle(c, s * 0.75f, col, 0, 1.5f);
      }
      break;
    case Icon::Play:
      tri_right(dl, c.x - s * 0.7f, c.x + s * 0.9f, c.y, s, col);
      break;
    case Icon::PlayReverse:
      tri_left(dl, c.x + s * 0.7f, c.x - s * 0.9f, c.y, s, col);
      break;
    case Icon::Pause:
      dl->AddRectFilled(ImVec2(c.x - s * 0.7f, c.y - s), ImVec2(c.x - s * 0.2f, c.y + s), col);
      dl->AddRectFilled(ImVec2(c.x + s * 0.2f, c.y - s), ImVec2(c.x + s * 0.7f, c.y + s), col);
      break;
    case Icon::JumpStart:
      dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x - s * 0.65f, c.y + s), col);
      tri_left(dl, c.x + s * 0.9f, c.x - s * 0.55f, c.y, s, col);
      break;
    case Icon::JumpEnd:
      dl->AddRectFilled(ImVec2(c.x + s * 0.65f, c.y - s), ImVec2(c.x + s, c.y + s), col);
      tri_right(dl, c.x - s * 0.9f, c.x + s * 0.55f, c.y, s, col);
      break;
    case Icon::KeyPrev:
      tri_left(dl, c.x - s * 0.15f, c.x - s, c.y, s * 0.7f, col);
      diamond(dl, ImVec2(c.x + s * 0.45f, c.y), s * 0.6f, col);
      break;
    case Icon::KeyNext:
      tri_right(dl, c.x + s * 0.15f, c.x + s, c.y, s * 0.7f, col);
      diamond(dl, ImVec2(c.x - s * 0.45f, c.y), s * 0.6f, col);
      break;
  }
}

static bool icon_button(const char *id, Icon icon, bool on, const char *tooltip, const std::string &shortcut)
{
  const float h = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (ImGui::IsItemHovered()) {
    dl->AddRectFilled(p, p + ImVec2(h, h), COL_BUTTON_HOVER, ImGui::GetStyle().FrameRounding);
  }
  icon_draw(dl, icon, p + ImVec2(h * 0.5f, h * 0.5f), h * 0.3f, on);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    if (shortcut.empty()) {
      ImGui::SetTooltip("%s", tooltip);
    }
    else {
      ImGui::SetTooltip("%s\nShortcut: %s", tooltip, shortcut.c_str());
    }
  }
  return pressed;
}

/* A button that runs an operator, with its name and shortcut as the tooltip. */
static void operator_icon_button(Context &C, const char *id, Icon icon, const char *idname)
{
  const OperatorType *ot = operators().find(idname);
  if (!ot) {
    return;
  }
  if (icon_button(id, icon, false, ot->name.c_str(), keymaps().shortcut_for(idname))) {
    operator_call(C, idname);
  }
}

static void view_frame_all(Area &area, const Document &doc)
{
  const float span = float(std::max(doc.frame_end - doc.frame_start, 10));
  const float margin = span * 0.04f + 1.0f;
  area.view_start = float(doc.frame_start) - margin;
  area.view_end = float(doc.frame_end) + margin;
}

static void view_center_current(Area &area, const Document &doc)
{
  const float half = (area.view_end - area.view_start) * 0.5f;
  area.view_start = float(doc.frame_current) - half;
  area.view_end = float(doc.frame_current) + half;
}

/* An integer field that pushes an undo step when the user lets go of it. */
static void frame_field(Context &C, const char *id, int *value, const char *format, float width,
                        const char *undo_name)
{
  ImGui::SetNextItemWidth(width);
  ImGui::DragInt(id, value, 0.25f, 0, 1048574, format, ImGuiSliderFlags_AlwaysClamp);
  if (undo_name && ImGui::IsItemDeactivatedAfterEdit() && C.undo) {
    C.undo->push(undo_name, *C.document);
  }
}

/* Which action the timeline edits: the mainline, or one of the selected armature's smart
 * actions, like Moho's switch between the mainline and a smart bone's action. */
static void action_menu(Context &C, float width)
{
  Document &doc = *C.document;
  const bool in_action = !doc.action.empty();
  const std::string preview = in_action ? doc.action : "Mainline";

  if (in_action) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, COL_ACTION_ACTIVE);
  }
  ImGui::SetNextItemWidth(width);
  const bool open = ImGui::BeginCombo("##action", preview.c_str());
  if (in_action) {
    ImGui::PopStyleColor();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Action being edited: the mainline, or a smart action of the selected armature\n\n"
                      "Python: sky.data.document.action");
  }
  if (!open) {
    return;
  }

  if (ImGui::Selectable("Mainline", !in_action)) {
    doc.action.clear();
  }
  const std::vector<std::string> actions = document_selectable_actions(doc);
  if (!actions.empty()) {
    ImGui::SeparatorText("Smart Actions");
  }
  for (const std::string &name : actions) {
    if (ImGui::Selectable(name.c_str(), doc.action == name)) {
      doc.action = name;
    }
  }
  if (actions.empty()) {
    ImGui::Separator();
    ImGui::TextDisabled("Select an armature with smart bones\nto edit its smart actions here.");
  }
  /* An action set from Python that the menu doesn't list still shows, so it can be left. */
  if (in_action && std::find(actions.begin(), actions.end(), doc.action) == actions.end()) {
    ImGui::Selectable(doc.action.c_str(), true, ImGuiSelectableFlags_Disabled);
  }
  ImGui::EndCombo();
}

static void timeline_header(Area &area, Context &C)
{
  Document &doc = *C.document;
  WindowManager &wm = *C.wm;
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const float pad = style.ItemSpacing.x;
  const float top = 4.0f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;

  /* Left: editor type and the View, Marker and Playback menus. */
  ImGui::SetCursorScreenPos(origin + ImVec2(pad, top));
  area_editor_type_menu(area);
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
  for (const char *menu : {"View", "Marker", "Playback"}) {
    ImGui::SameLine();
    if (ImGui::Button(menu)) {
      ImGui::OpenPopup(menu);
    }
  }
  ImGui::PopStyleColor();
  ImGui::SameLine(0.0f, pad * 2.0f);
  action_menu(C, h * 6.0f);
  const float menus_end = ImGui::GetItemRectMax().x;

  if (ImGui::BeginPopup("View")) {
    if (ImGui::MenuItem("Frame All", "Home")) {
      view_frame_all(area, doc);
    }
    if (ImGui::MenuItem("Go to Current Frame")) {
      view_center_current(area, doc);
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("Marker")) {
    ImGui::MenuItem("Add Marker", "M", false, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Arrives in milestone 3");
    }
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopup("Playback")) {
    ImGui::MenuItem("Loop Playback", nullptr, &wm.play_loop);
    ImGui::MenuItem("Auto Keying", nullptr, &wm.auto_key);
    ImGui::EndPopup();
  }

  /* Centre: auto-keying, then the playback buttons. */
  const float spacing = std::max(1.0f, style.ItemSpacing.x * 0.35f);
  const float record_gap = h * 0.6f;
  const float transport_w = 7.0f * h + 5.0f * spacing + record_gap;
  const float transport_x = std::max(menus_end + pad * 2.0f, origin.x + (width - transport_w) * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(transport_x, origin.y + top));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, style.ItemSpacing.y));
  if (icon_button("##auto_key", Icon::Record, wm.auto_key, "Auto Keying (takes effect in milestone 3)", "")) {
    wm.auto_key = !wm.auto_key;
  }
  ImGui::SameLine(0.0f, record_gap);
  operator_icon_button(C, "##jump_start", Icon::JumpStart, "screen.frame_jump_start");
  ImGui::SameLine();
  operator_icon_button(C, "##key_prev", Icon::KeyPrev, "screen.keyframe_jump_prev");
  ImGui::SameLine();
  operator_icon_button(C, "##play_reverse", (wm.playing && wm.play_reverse) ? Icon::Pause : Icon::PlayReverse,
                       "screen.animation_play_reverse");
  ImGui::SameLine();
  operator_icon_button(C, "##play", (wm.playing && !wm.play_reverse) ? Icon::Pause : Icon::Play,
                       "screen.animation_play");
  ImGui::SameLine();
  operator_icon_button(C, "##key_next", Icon::KeyNext, "screen.keyframe_jump_next");
  ImGui::SameLine();
  operator_icon_button(C, "##jump_end", Icon::JumpEnd, "screen.frame_jump_end");
  ImGui::PopStyleVar();
  const float transport_end = ImGui::GetItemRectMax().x;

  /* Right: current frame, then the Start and End of the range. */
  const float current_w = h * 2.6f;
  const float range_w = h * 4.0f;
  const float group_gap = h * 0.6f;
  const float fields_w = current_w + group_gap + range_w * 2.0f + style.ItemSpacing.x;
  const float fields_x = std::max(transport_end + pad * 2.0f, origin.x + width - fields_w - pad);
  ImGui::SetCursorScreenPos(ImVec2(fields_x, origin.y + top));
  frame_field(C, "##frame_current", &doc.frame_current, "%d", current_w, nullptr);
  ImGui::SameLine(0.0f, group_gap);
  frame_field(C, "##frame_start", &doc.frame_start, "Start %d", range_w, "Change Start");
  ImGui::SameLine();
  frame_field(C, "##frame_end", &doc.frame_end, "End %d", range_w, "Change End");
  doc.frame_end = std::max(doc.frame_end, doc.frame_start);

  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + top + h + top));
}

/* Frames between labelled ruler ticks, chosen so labels are at least min_px apart.
 * Like Blender, steps follow the frame rate: at 24 fps that is 6, 12, 24, 48, ... */
static float ruler_step(float span, float width, float min_px, float fps)
{
  const float second = std::max(1.0f, std::round(fps));
  const float candidates[] = {1.0f, 2.0f, second / 4.0f, second / 2.0f, second};
  for (float step : candidates) {
    if (step >= 1.0f && std::floor(step) == step && width * step / span >= min_px) {
      return step;
    }
  }
  float step = second;
  while (width * step / span < min_px) {
    step *= 2.0f;
  }
  return step;
}

static void timeline_body(Area &area, Context &C)
{
  Document &doc = *C.document;
  ImGuiIO &io = ImGui::GetIO();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  if (size.x < 8.0f || size.y < 8.0f) {
    return;
  }
  const ImVec2 p1 = p0 + size;

  if (!area.view_initialized) {
    view_frame_all(area, doc);
    area.view_initialized = true;
  }

  ImGui::InvisibleButton("##timeline_region", size,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();

  /* Navigation: wheel zooms around the mouse, Ctrl+wheel or middle-drag pans, Home frames all. */
  float span = area.view_end - area.view_start;
  if (hovered) {
    const float mouse_frame = area.view_start + (io.MousePos.x - p0.x) / size.x * span;
    if (io.MouseWheel != 0.0f && !io.KeyCtrl) {
      const float factor = std::pow(0.85f, io.MouseWheel);
      const float new_span = std::clamp(span * factor, 10.0f, 200000.0f);
      const float t = (mouse_frame - area.view_start) / span;
      area.view_start = mouse_frame - t * new_span;
      area.view_end = area.view_start + new_span;
    }
    const float pan_wheel = io.KeyCtrl ? io.MouseWheel : io.MouseWheelH;
    if (pan_wheel != 0.0f) {
      const float delta = -pan_wheel * span * 0.1f;
      area.view_start += delta;
      area.view_end += delta;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
      view_frame_all(area, doc);
    }
  }
  span = area.view_end - area.view_start;
  if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
    const float delta = -io.MouseDelta.x / size.x * span;
    area.view_start += delta;
    area.view_end += delta;
  }

  /* Scrubbing: click or drag with the left button sets the frame and pauses playback. */
  if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float frame = area.view_start + (io.MousePos.x - p0.x) / size.x * span;
    doc.frame_current = std::max(0, int(std::lround(frame)));
    C.wm->playing = false;
  }

  const auto frame_x = [&](float frame) { return p0.x + (frame - area.view_start) / span * size.x; };

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float ruler_h = ImGui::GetFrameHeight() + 2.0f;
  dl->AddRectFilled(p0, p1, COL_REGION);
  if (!doc.action.empty()) {
    /* A warm tint is a constant reminder that keys go into the action, not the mainline. */
    dl->AddRectFilled(p0, p1, COL_ACTION_TINT);
  }

  /* Darken everything outside the frame range. */
  const float x_start = frame_x(float(doc.frame_start));
  const float x_end = frame_x(float(doc.frame_end));
  if (x_start > p0.x) {
    dl->AddRectFilled(p0, ImVec2(std::min(x_start, p1.x), p1.y), COL_OUTSIDE_RANGE);
  }
  if (x_end < p1.x) {
    dl->AddRectFilled(ImVec2(std::max(x_end, p0.x), p0.y), p1, COL_OUTSIDE_RANGE);
  }

  /* Grid lines and ruler numbers. */
  const float step = ruler_step(span, size.x, ImGui::GetFontSize() * 4.0f, doc.fps);
  const float font_size = ImGui::GetFontSize();
  for (float f = std::floor(area.view_start / step) * step; f <= area.view_end; f += step) {
    const float x = frame_x(f);
    if (step >= 2.0f) {
      const float xm = frame_x(f + step * 0.5f);
      dl->AddLine(ImVec2(xm, p0.y + ruler_h), ImVec2(xm, p1.y), COL_GRID_MINOR);
    }
    if (x < p0.x) {
      continue;
    }
    dl->AddLine(ImVec2(x, p0.y + ruler_h), ImVec2(x, p1.y), COL_GRID);
    const std::string label = std::to_string(int(f));
    const float tw = ImGui::CalcTextSize(label.c_str()).x;
    dl->AddText(ImVec2(x - tw * 0.5f, p0.y + (ruler_h - font_size) * 0.5f), COL_RULER_TEXT, label.c_str());
  }

  /* Playhead: a blue line with the frame number in a box on the ruler. */
  const float x = frame_x(float(doc.frame_current));
  if (x >= p0.x - 40.0f && x <= p1.x + 40.0f) {
    dl->AddLine(ImVec2(x, p0.y + ruler_h * 0.5f), ImVec2(x, p1.y), COL_PLAYHEAD, 2.0f);
    const std::string label = std::to_string(doc.frame_current);
    const float box_w = std::max(ImGui::CalcTextSize(label.c_str()).x + font_size * 0.8f, ruler_h);
    const ImVec2 box_min(x - box_w * 0.5f, p0.y + 2.0f);
    const ImVec2 box_max(x + box_w * 0.5f, p0.y + ruler_h - 2.0f);
    dl->AddRectFilled(box_min, box_max, COL_PLAYHEAD, ImGui::GetStyle().FrameRounding);
    const float tw = ImGui::CalcTextSize(label.c_str()).x;
    dl->AddText(ImVec2(x - tw * 0.5f, p0.y + (ruler_h - font_size) * 0.5f), IM_COL32_WHITE, label.c_str());
  }
}

void timeline_draw(Area &area, Context &C)
{
  timeline_header(area, C);
  timeline_body(area, C);
}

}  // namespace sky
