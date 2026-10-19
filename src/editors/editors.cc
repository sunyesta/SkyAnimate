#include "editors/editors.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include <SDL3/SDL_opengl.h>
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include "core/log.hh"
#include "editors/ui.hh"
#include "data/document.hh"
#include "ops/undo.hh"
#include "rna/rna.hh"
#include "wm/context.hh"

namespace sky {

const char *editor_type_label(EditorType type)
{
  switch (type) {
    case EditorType::Viewport:
      return "Viewport";
    case EditorType::Properties:
      return "Properties";
    case EditorType::Layers:
      return "Layers";
    case EditorType::Timeline:
      return "Timeline";
    case EditorType::TextEditor:
      return "Text Editor";
    case EditorType::Console:
      return "Python Console";
    case EditorType::Info:
      return "Info";
  }
  return "Unknown";
}

const char *editor_keymap_name(EditorType type)
{
  return type == EditorType::Console ? "Console" : editor_type_label(type);
}

bool header_menu(const char *label)
{
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
  if (ImGui::Button(label)) {
    ImGui::OpenPopup(label);
  }
  ImGui::PopStyleColor();
  return ImGui::BeginPopup(label);
}

std::string Area::window_name() const
{
  return std::string(editor_type_label(type)) + "###area" + std::to_string(id);
}

/* ---------------------------------------------------------------------------------
 * Generic property widgets, like Blender's layout.prop(): one RNA description draws
 * any property. Edits made here push an undo step when the widget is released.
 */

bool ui_panel_begin(const char *title, bool default_open)
{
  if (!ImGui::CollapsingHeader(title, default_open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
    return false;
  }
  if (!ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp)) {
    return false;
  }
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
  return true;
}

void ui_panel_end()
{
  ImGui::EndTable();
}

/* Label in the left column, right-aligned against its field as in Blender. */
static void ui_label(const char *label)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  const float label_width = ImGui::CalcTextSize(label).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetColumnWidth() - label_width));
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-FLT_MIN);
}

void ui_prop(Context &C, const rna::PointerRNA &ptr, const char *identifier, const char *python_path)
{
  const rna::PropertyRNA *prop = ptr.type->find(identifier);
  if (!prop) {
    return;
  }
  ui_label(prop->name.c_str());
  ImGui::PushID(identifier);
  ImGui::BeginDisabled(!prop->set);

  rna::Value value = prop->get(ptr.data);
  bool changed = false;
  switch (prop->type) {
    case rna::PropType::Bool:
      changed = ImGui::Checkbox("##value", &std::get<bool>(value));
      break;
    case rna::PropType::Int:
      changed = ImGui::DragInt("##value", &std::get<int>(value), 0.25f, int(prop->min), int(prop->max), "%d",
                               ImGuiSliderFlags_AlwaysClamp);
      break;
    case rna::PropType::Float:
      changed = ImGui::DragFloat("##value", &std::get<float>(value), 0.01f, float(prop->min), float(prop->max),
                                 "%.3f", ImGuiSliderFlags_AlwaysClamp);
      break;
    case rna::PropType::String:
      changed = ImGui::InputText("##value", &std::get<std::string>(value));
      break;
    case rna::PropType::Enum: {
      int &index = std::get<int>(value);
      const char *preview = (index >= 0 && index < int(prop->items.size())) ? prop->items[index].name.c_str() : "";
      if (ImGui::BeginCombo("##value", preview)) {
        for (int i = 0; i < int(prop->items.size()); i++) {
          if (ImGui::Selectable(prop->items[i].name.c_str(), i == index)) {
            index = i;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
      /* A combo edit is complete when picked, so it gets its undo step right away. */
      if (changed && prop->set) {
        prop->set(ptr.data, rna::clamp(*prop, value));
        if (C.undo && C.document) {
          C.undo->push("Change " + prop->name, *C.document);
        }
        changed = false;
      }
      break;
    }
  }
  if (changed && prop->set) {
    prop->set(ptr.data, rna::clamp(*prop, value));
  }
  if (ImGui::IsItemDeactivatedAfterEdit() && C.undo && C.document) {
    C.undo->push("Change " + prop->name, *C.document);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s\n\nPython: %s.%s", prop->description.c_str(), python_path, prop->identifier.c_str());
  }

  ImGui::EndDisabled();
  ImGui::PopID();
}

bool ui_color(Context &C, const char *label, float rgba[4], const char *undo_name)
{
  ui_label(label);
  ImGui::PushID(label);
  bool finished = false;
  const bool changed = ui_color_edit("##color", rgba, true, &finished);
  if (finished && C.undo && C.document && undo_name) {
    C.undo->push(undo_name, *C.document);
  }
  ImGui::PopID();
  return changed;
}

/* --- Colour picker and eyedropper ------------------------------------------------------------- */

namespace {

struct Eyedropper {
  bool active = false;
  /* The colour edit that started it; its result waits for that widget to draw again. */
  ImGuiID owner = 0;
  bool sample_pending = false;
  ImVec2 sample_pos;
  bool has_result = false;
  float rgb[3] = {0, 0, 0};
};
Eyedropper g_eyedropper;

/* A pipette, as on Blender's eyedropper button. */
void eyedropper_icon(ImDrawList *dl, ImVec2 c, float s, ImU32 col)
{
  const ImVec2 tip(c.x - s * 0.7f, c.y + s * 0.7f), bulb(c.x + s * 0.45f, c.y - s * 0.45f);
  dl->AddLine(tip, bulb, col, std::max(1.5f, s * 0.22f));
  dl->AddCircleFilled(ImVec2(bulb.x + s * 0.2f, bulb.y - s * 0.2f), s * 0.32f, col);
  dl->AddLine(ImVec2(bulb.x - s * 0.35f, bulb.y - s * 0.35f), ImVec2(bulb.x + s * 0.35f, bulb.y + s * 0.35f), col,
              std::max(1.5f, s * 0.2f));
}

}  // namespace

bool ui_color_edit(const char *id, float rgba[4], bool hex, bool *r_finished)
{
  ImGui::PushID(id);
  const ImGuiID owner = ImGui::GetID("##eyedropper");
  bool changed = false, finished = false;
  /* A colour the eyedropper picked for this widget: its RGB, keeping the alpha, as in Blender. */
  if (g_eyedropper.has_result && g_eyedropper.owner == owner) {
    rgba[0] = g_eyedropper.rgb[0];
    rgba[1] = g_eyedropper.rgb[1];
    rgba[2] = g_eyedropper.rgb[2];
    g_eyedropper.has_result = false;
    changed = finished = true;
  }

  const float h = ImGui::GetFrameHeight();
  const ImGuiColorEditFlags show = ImGuiColorEditFlags_AlphaPreviewHalf;
  if (ImGui::ColorButton("##swatch", ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]), show, ImVec2(h * 1.6f, h))) {
    ImGui::OpenPopup("##picker");
  }
  if (hex) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(-FLT_MIN);
    changed |= ImGui::ColorEdit4("##hex", rgba,
                                 ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoSmallPreview |
                                     ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoOptions |
                                     ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    finished |= ImGui::IsItemDeactivatedAfterEdit();
  }

  /* The picker remembers whether it changed the colour, to report the edit's end on close. */
  bool &picker_dirty = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("##picker_dirty"), false);
  if (ImGui::BeginPopup("##picker")) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##eyedropper", ImVec2(h, h))) {
      g_eyedropper = {};
      g_eyedropper.active = true;
      g_eyedropper.owner = owner;
      ImGui::CloseCurrentPopup();
    }
    eyedropper_icon(ImGui::GetWindowDrawList(), ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.32f,
                    IM_COL32(0xe6, 0xe6, 0xe6, 255));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Eyedropper: click anywhere to pick its colour (Esc or right-click cancels)");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Eyedropper");
    ImGui::SetNextItemWidth(h * 9.0f);
    if (ImGui::ColorPicker4("##pick", rgba,
                            ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaBar |
                                ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_PickerHueBar))
    {
      changed = true;
      picker_dirty = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  else if (picker_dirty) {
    picker_dirty = false;
    finished = true;
  }
  ImGui::PopID();
  if (r_finished) {
    *r_finished = finished;
  }
  return changed;
}

void ui_eyedropper_overlay()
{
  if (!g_eyedropper.active) {
    return;
  }
  /* A clear window over everything catches the click, so nothing underneath reacts to it. */
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::SetNextWindowFocus();
  ImGui::Begin("##eyedropper_overlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::InvisibleButton("##catch", viewport->Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const ImGuiIO &io = ImGui::GetIO();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    g_eyedropper.sample_pending = true;
    g_eyedropper.sample_pos = io.MousePos;
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    g_eyedropper.active = false;
  }
  /* A ring around the pixel that will be picked, leaving the pixel itself clear. */
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const ImVec2 m = io.MousePos;
  dl->AddCircle(m, 9.0f, IM_COL32(0, 0, 0, 220), 24, 3.0f);
  dl->AddCircle(m, 9.0f, IM_COL32(255, 255, 255, 230), 24, 1.5f);
  eyedropper_icon(dl, ImVec2(m.x + 18.0f, m.y - 18.0f), 7.0f, IM_COL32(255, 255, 255, 230));
  dl->AddText(ImVec2(m.x + 30.0f, m.y - 26.0f), IM_COL32(255, 255, 255, 230), "Click to pick  |  Esc cancels");
  ImGui::End();
}

void ui_eyedropper_sample(int framebuffer_height)
{
  if (!g_eyedropper.sample_pending) {
    return;
  }
  g_eyedropper.sample_pending = false;
  g_eyedropper.active = false;
  const ImGuiIO &io = ImGui::GetIO();
  const int x = int(g_eyedropper.sample_pos.x * io.DisplayFramebufferScale.x);
  const int y = framebuffer_height - 1 - int(g_eyedropper.sample_pos.y * io.DisplayFramebufferScale.y);
  unsigned char pixel[4] = {0, 0, 0, 0};
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  for (int i = 0; i < 3; i++) {
    g_eyedropper.rgb[i] = float(pixel[i]) / 255.0f;
  }
  g_eyedropper.has_result = true;
}

/* ---------------------------------------------------------------------------------
 * Editors
 */

static void info_draw()
{
  ImGui::BeginChild("reports", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  for (const Report &r : reports()) {
    ImVec4 color(0.9f, 0.9f, 0.9f, 1.0f);
    switch (r.type) {
      case ReportType::Operator:
        color = ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
        break;
      case ReportType::Warning:
        color = ImVec4(1.0f, 0.75f, 0.3f, 1.0f);
        break;
      case ReportType::Error:
        color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
        break;
      case ReportType::Info:
        break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(r.message.c_str());
    ImGui::PopStyleColor();
  }
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
}

/* Dragging the grip moves the area, the way dragging a tab does in ImGui: the window is
 * undocked and follows the mouse, and dropping it on a docking target docks it there. */
static void area_grip_drag_start(const ImRect &grip_rect)
{
  ImGuiContext &g = *ImGui::GetCurrentContext();
  ImGuiWindow *window = ImGui::GetCurrentWindow()->RootWindow;
  if (window->DockNode) {
    ImGui::DockContextQueueUndockWindow(&g, window);
    g.MovingWindow = window;
    ImGui::SetActiveID(window->MoveId, window);
    g.ActiveIdClickOffset -= window->Pos - grip_rect.Min;
    g.ActiveIdNoClearOnFocusLoss = true;
    ImGui::SetActiveIdUsingAllKeyboardKeys();
  }
  else {
    ImGui::StartMouseMovingWindow(window);
  }
  /* ImGui only offers docking targets when the window was grabbed within its top strip, where
   * a title bar would be. Areas have no title bar, so keep the grab point inside that strip. */
  g.ActiveIdClickOffset.y = std::min(g.ActiveIdClickOffset.y, ImGui::GetFrameHeight() - 1.0f);
}

/* Two horizontal lines to grab the area by, with Blender's area options on right-click.
 * The grip starts at the area's left edge and holds equal margins on both sides of the lines,
 * so they sit centred between the edge and the dropdown that follows, and centred vertically
 * on the header row. Sizes are whole pixels so both lines stay crisp. */
static void area_grip(Area &area)
{
  const float h = ImGui::GetFrameHeight();
  const float margin = std::round(ImGui::GetStyle().ItemSpacing.x);
  const float line_w = std::round(h * 0.45f);
  const float thickness = std::max(1.0f, std::round(h * 0.07f));
  const float line_gap = std::max(2.0f, std::round(h * 0.16f));
  const ImVec2 size(margin * 2.0f + line_w, h);

  const ImVec2 p(std::round(ImGui::GetWindowPos().x), std::round(ImGui::GetCursorScreenPos().y));
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##area_grip", size);
  const bool hovered = ImGui::IsItemHovered();
  const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, ImGui::GetFontSize() * 0.3f)) {
    area_grip_drag_start(rect);
  }
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      ImGui::OpenPopup("##area_menu");
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Drag to move this area\nRight-click for area options");
  }

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 col = hovered || ImGui::IsItemActive() ? IM_COL32(0xff, 0xff, 0xff, 255)
                                                     : IM_COL32(0x8c, 0x8c, 0x8c, 255);
  const float block_h = thickness * 2.0f + line_gap;
  const float x0 = p.x + margin;
  const float y0 = p.y + std::round((h - block_h) * 0.5f);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + line_w, y0 + thickness), col);
  dl->AddRectFilled(ImVec2(x0, y0 + thickness + line_gap), ImVec2(x0 + line_w, y0 + block_h), col);

  if (ImGui::BeginPopup("##area_menu")) {
    ImGui::TextDisabled("Area");
    ImGui::Separator();
    if (ImGui::MenuItem("Vertical Split")) {
      area.split_request = AreaSplit::Vertical;
    }
    if (ImGui::MenuItem("Horizontal Split")) {
      area.split_request = AreaSplit::Horizontal;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Close Area")) {
      area.open = false;
    }
    ImGui::EndPopup();
  }
}

/* Blender puts an editor-type menu at the start of every area header. */
void area_editor_type_menu(Area &area)
{
  area_grip(area);
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("Python Console").x + ImGui::GetFrameHeight() * 1.5f);
  if (ImGui::BeginCombo("##editor_type", editor_type_label(area.type))) {
    for (EditorType type : EDITOR_TYPES) {
      if (ImGui::Selectable(editor_type_label(type), type == area.type)) {
        area.type = type;
      }
    }
    ImGui::EndCombo();
  }
}

bool area_draw(Area &area, Context &C)
{
  /* The viewport and timeline draw edge to edge with their own header layout. */
  const bool full_bleed = area.type == EditorType::Viewport || area.type == EditorType::Timeline;
  if (full_bleed) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  }
  /* No tab above the area: the header's grip and editor dropdown do that job, as in Blender.
   * Areas can split one another but never stack in the same spot, since without tabs a
   * stacked area would be hidden. */
  ImGuiWindowClass window_class;
  window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoWindowMenuButton |
                                          ImGuiDockNodeFlags_NoCloseButton | ImGuiDockNodeFlags_NoDockingOverMe |
                                          ImGuiDockNodeFlags_NoDockingOverOther;
  ImGui::SetNextWindowClass(&window_class);
  /* No title bar even while floating: the area is moved by its grip, like a Blender area. */
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
  if (full_bleed) {
    flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  }
  const bool visible = ImGui::Begin(area.window_name().c_str(), &area.open, flags);
  if (full_bleed) {
    ImGui::PopStyleVar();
  }
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

  if (visible) {
    switch (area.type) {
      case EditorType::Viewport:
        viewport_draw(area, C);
        break;
      case EditorType::Timeline:
        timeline_draw(area, C);
        break;
      case EditorType::TextEditor:
        text_editor_draw(area, C);
        break;
      case EditorType::Console:
        console_draw(area, C);
        break;
      case EditorType::Layers:
        layers_draw(area, C);
        break;
      case EditorType::Properties:
        properties_draw(area, C);
        break;
      case EditorType::Info:
        area_editor_type_menu(area);
        ImGui::Separator();
        info_draw();
        break;
    }
  }
  ImGui::End();
  return hovered;
}

}  // namespace sky
