#include "editors/editors.hh"

#include <algorithm>
#include <cfloat>

#include "imgui.h"
#include "imgui_stdlib.h"

#include "core/log.hh"
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
    case EditorType::Timeline:
      return "Timeline";
    case EditorType::Info:
      return "Info";
  }
  return "Unknown";
}

const char *editor_keymap_name(EditorType type)
{
  return editor_type_label(type);
}

std::string Area::window_name() const
{
  return std::string(editor_type_label(type)) + "###area" + std::to_string(id);
}

/* ---------------------------------------------------------------------------------
 * Generic property widgets, like Blender's layout.prop(): one RNA description draws
 * any property. Edits made here push an undo step when the widget is released.
 */

static void ui_prop(Context &C, const rna::PointerRNA &ptr, const char *identifier)
{
  const rna::PropertyRNA *prop = ptr.type->find(identifier);
  if (!prop) {
    return;
  }

  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  /* Blender right-aligns labels against their field. */
  const float label_width = ImGui::CalcTextSize(prop->name.c_str()).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetColumnWidth() - label_width));
  ImGui::TextUnformatted(prop->name.c_str());

  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-FLT_MIN);
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
      changed = ImGui::DragFloat("##value", &std::get<float>(value), 0.05f, float(prop->min), float(prop->max),
                                 "%.3f", ImGuiSliderFlags_AlwaysClamp);
      break;
    case rna::PropType::String:
      changed = ImGui::InputText("##value", &std::get<std::string>(value));
      break;
  }
  if (changed && prop->set) {
    prop->set(ptr.data, rna::clamp(*prop, value));
  }
  if (ImGui::IsItemDeactivatedAfterEdit() && C.undo && C.document) {
    C.undo->push("Change " + prop->name, *C.document);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s\n\nPython: sky.data.%s.%s", prop->description.c_str(), "document",
                      prop->identifier.c_str());
  }

  ImGui::EndDisabled();
  ImGui::PopID();
}

/* A collapsible panel holding a two-column table of properties. */
static void ui_panel(Context &C, const rna::PointerRNA &ptr, const char *title,
                     std::initializer_list<const char *> identifiers)
{
  if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }
  if (ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    for (const char *identifier : identifiers) {
      ui_prop(C, ptr, identifier);
    }
    ImGui::EndTable();
  }
}

/* ---------------------------------------------------------------------------------
 * Editors
 */

static void viewport_draw(Context &C)
{
  const Document &doc = *C.document;
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 size = ImGui::GetContentRegionAvail();
  if (size.x <= 1.0f || size.y <= 1.0f) {
    return;
  }

  /* Background, then the camera frame at the document's aspect ratio, like Blender's
   * camera view with its passepartout. */
  draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(0x3d, 0x3d, 0x3d, 255));
  const float aspect = float(doc.width) / float(std::max(doc.height, 1));
  float frame_w = size.x * 0.8f;
  float frame_h = frame_w / aspect;
  if (frame_h > size.y * 0.8f) {
    frame_h = size.y * 0.8f;
    frame_w = frame_h * aspect;
  }
  const ImVec2 frame_min(origin.x + (size.x - frame_w) * 0.5f, origin.y + (size.y - frame_h) * 0.5f);
  const ImVec2 frame_max(frame_min.x + frame_w, frame_min.y + frame_h);
  draw->AddRectFilled(frame_min, frame_max, IM_COL32(0x47, 0x47, 0x47, 255));
  draw->AddRect(frame_min, frame_max, IM_COL32(0x1d, 0x1d, 0x1d, 255), 0.0f, 0, 1.5f);

  /* Overlay text below the floating header. */
  const float text_y = origin.y + ImGui::GetFrameHeight() + 12;
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 10, text_y));
  ImGui::Text("Camera  |  Frame %d  |  %d x %d", doc.frame_current, doc.width, doc.height);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + 10, text_y + ImGui::GetTextLineHeightWithSpacing()));
  ImGui::Text("Test counter: %d", doc.counter);

  const char *hint = "Drawing arrives in milestone 2.  T adds to the test counter, Ctrl Z undoes, Ctrl Shift Z redoes.";
  const ImVec2 hint_size = ImGui::CalcTextSize(hint);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + (size.x - hint_size.x) * 0.5f, frame_max.y + 8));
  ImGui::TextDisabled("%s", hint);

  /* Claim the whole region so the window doesn't scroll. */
  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(size);
}

static void properties_draw(Context &C)
{
  const rna::PointerRNA ptr{C.document, &Document::rna()};
  ui_panel(C, ptr, "Document", {"name"});
  ui_panel(C, ptr, "Format", {"width", "height", "fps"});
  ui_panel(C, ptr, "Frame Range", {"frame_start", "frame_end", "frame_current"});
  ui_panel(C, ptr, "Test", {"counter"});
}

static void timeline_draw(Context &C)
{
  Document &doc = *C.document;
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::SliderInt("##frame", &doc.frame_current, doc.frame_start, std::max(doc.frame_start, doc.frame_end),
                   "Frame %d", ImGuiSliderFlags_AlwaysClamp);
  ImGui::TextDisabled("Keyframes, playback and the dope sheet arrive in milestone 3.");
}

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

/* Blender puts an editor-type menu at the start of every area header. */
static void area_header_draw(Area &area, bool separator)
{
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("Properties").x + ImGui::GetFrameHeight() * 2.0f);
  if (ImGui::BeginCombo("##editor_type", editor_type_label(area.type))) {
    for (EditorType type : EDITOR_TYPES) {
      if (ImGui::Selectable(editor_type_label(type), type == area.type)) {
        area.type = type;
      }
    }
    ImGui::EndCombo();
  }
  if (separator) {
    ImGui::Separator();
  }
}

bool area_draw(Area &area, Context &C)
{
  const bool viewport = area.type == EditorType::Viewport;
  if (viewport) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  }
  const bool visible = ImGui::Begin(area.window_name().c_str(), &area.open,
                                    viewport ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
  if (viewport) {
    ImGui::PopStyleVar();
  }
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

  if (visible) {
    if (viewport) {
      /* The viewport fills the area edge to edge; its header floats on top, as in Blender.
       * Screen coordinates: for a docked window, SetCursorPos() is measured from above the tab bar. */
      const ImVec2 content_start = ImGui::GetCursorScreenPos();
      viewport_draw(C);
      ImGui::SetCursorScreenPos(ImVec2(content_start.x + 6, content_start.y + 4));
    }
    area_header_draw(area, !viewport);
    switch (area.type) {
      case EditorType::Viewport:
        break;
      case EditorType::Properties:
        properties_draw(C);
        break;
      case EditorType::Timeline:
        timeline_draw(C);
        break;
      case EditorType::Info:
        info_draw();
        break;
    }
  }
  ImGui::End();
  return hovered;
}

}  // namespace sky
