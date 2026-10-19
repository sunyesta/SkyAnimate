/* The Properties editor, like Blender's: tabs for the document, the active layer and its
 * drawing (shapes and their styles). A Modifiers tab arrives with the modifier stack. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <string>

#include "data/bones.hh"
#include "data/document.hh"
#include "editors/editors.hh"
#include "assets/brushes.hh"
#include "editors/brush_settings.hh"
#include "editors/ui.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace sky {

enum PropertiesTab { TAB_DOCUMENT = 0, TAB_LAYER = 1, TAB_DRAWING = 2, TAB_MODIFIERS = 3 };

static void document_tab(Context &C)
{
  const rna::PointerRNA ptr{C.document, &Document::rna()};
  const char *path = "sky.data.document";
  if (ui_panel_begin("Document")) {
    ui_prop(C, ptr, "name", path);
    ui_panel_end();
  }
  if (ui_panel_begin("Format")) {
    ui_prop_pair(C, ptr, "Resolution", "width", "W", "height", "H", path);
    ui_prop(C, ptr, "fps", path);
    float rgba[4] = {C.document->background.r, C.document->background.g, C.document->background.b, 1.0f};
    if (ui_color(C, "Background", rgba, "Background Color")) {
      C.document->background = {rgba[0], rgba[1], rgba[2], 1.0f};
    }
    ui_panel_end();
  }
  if (ui_panel_begin("Quality")) {
    ui_prop(C, ptr, "viewport_quality", path);
    ui_prop(C, ptr, "render_quality", path);
    ui_prop(C, ptr, "viewport_samples", path);
    ui_prop(C, ptr, "render_samples", path);
    ui_panel_end();
  }
  if (ui_panel_begin("Frame Range")) {
    ui_prop(C, ptr, "frame_start", path);
    ui_prop(C, ptr, "frame_end", path);
    ui_prop(C, ptr, "frame_current", path);
    ui_panel_end();
  }
}

/* A bone layer's skeleton (as a tree, active bone highlighted) and the active bone. */
static void bones_panel(Context &C, Layer &armature)
{
  Document &doc = *C.document;
  if (!ui_panel_begin("Bones")) {
    return;
  }
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TableSetColumnIndex(1);
  if (armature.bones.empty()) {
    ImGui::TextWrapped("No bones yet. Pick Add Bone in the tool bar and drag in the viewport (Setup frame).");
  }
  else {
    const float list_h = std::min(float(armature.bones.size()) + 0.5f, 8.0f) * ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("##bones", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
      const auto row = [&](auto &&self, uint64_t parent, int depth) -> void {
        for (Bone &b : armature.bones) {
          if (b.parent != parent || depth > 64) {
            continue;
          }
          ImGui::PushID(int(b.id));
          ImGui::SetCursorPosX(ImGui::GetCursorPosX() + float(depth) * ImGui::GetFontSize());
          if (ImGui::Selectable(b.name.c_str(), b.selected)) {
            if (!ImGui::GetIO().KeyShift) {
              armature_deselect_all(armature);
            }
            bone_select(b, true);
            armature.active_bone = b.id;
            C.undo->push("Select Bone", doc);
          }
          ImGui::PopID();
          self(self, b.id, depth + 1);
        }
      };
      row(row, 0, 0);
    }
    ImGui::EndChild();
  }
  Bone *active = bone_find(armature, armature.active_bone);
  if (active) {
    const auto label = [](const char *text) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(text);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-1);
    };
    label("Name");
    ImGui::InputText("##bone_name", &active->name);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Rename Bone", doc);
    }
    /* Reparenting keeps the bone where it is (Setup frame only, as other rig changes). */
    const Bone *parent = bone_find(armature, active->parent);
    label("Parent");
    ImGui::BeginDisabled(doc.frame_current != 0);
    if (ImGui::BeginCombo("##bone_parent", parent ? parent->name.c_str() : "None")) {
      if (ImGui::Selectable("None", parent == nullptr)) {
        operator_call(C, "armature.bone_parent_set", {{"bone", active->name}, {"parent", ""}});
      }
      for (const Bone &b : armature.bones) {
        /* Not itself, nor anything hanging from it. */
        if (bone_is_descendant(armature, b.id, active->id)) {
          continue;
        }
        if (ImGui::Selectable(b.name.c_str(), parent && parent->id == b.id)) {
          operator_call(C, "armature.bone_parent_set", {{"bone", active->name}, {"parent", b.name}});
        }
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(doc.frame_current != 0 ? "Switch to Setup (frame 0) to change the parent"
                                               : "The bone this one hangs from; it stays where it is");
    }
    active = bone_find(armature, armature.active_bone);
    if (!active) {
      ui_panel_end();
      return;
    }
    float degrees = active->angle * 180.0f / PI;
    label("Angle");
    if (ImGui::DragFloat("##bone_angle", &degrees, 0.5f, -36000.0f, 36000.0f, "%.1f\xC2\xB0")) {
      active->angle = degrees * PI / 180.0f;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Bone Angle", doc);
    }
    label("Scale");
    ImGui::DragFloat("##bone_scale", &active->scale, 0.01f, -100.0f, 100.0f, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Bone Scale", doc);
    }
    ImGui::BeginDisabled(doc.frame_current != 0);
    label("Length");
    ImGui::DragFloat("##bone_length", &active->length, 0.005f, 0.0f, 1000.0f, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Bone Length", doc);
    }
    label("Strength");
    ImGui::DragFloat("##bone_strength", &active->strength, 0.01f, 0.0f, 100.0f, "%.2f");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("How far the bone reaches when points get automatic weights (Setup frame only)");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Bone Strength", doc);
    }
    ImGui::EndDisabled();
  }
  ui_panel_end();
}

/* For a layer inside a bone layer: a bone it follows as a whole (Parent to Bone), or, for a
 * drawing, how its points are weighted to the bones. */
static void binding_panel(Context &C, Layer &layer, Layer &armature)
{
  Document &doc = *C.document;
  if (!ui_panel_begin("Bones")) {
    return;
  }
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Parent Bone");
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-1);
  const Bone *parent = bone_find(armature, layer.parent_bone);
  ImGui::BeginDisabled(doc.frame_current != 0);
  if (ImGui::BeginCombo("##parent_bone", parent ? parent->name.c_str() : "None")) {
    if (ImGui::Selectable("None", !parent)) {
      operator_call(C, "layer.parent_bone_set", {{"bone", ""}});
    }
    for (const Bone &b : armature.bones) {
      if (ImGui::Selectable(b.name.c_str(), parent && b.id == parent->id)) {
        operator_call(C, "layer.parent_bone_set", {{"bone", b.name}});
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(doc.frame_current != 0 ? "Switch to Setup (frame 0)"
                                             : "The whole layer follows this bone, as parenting to a bone in Blender");
  }
  if (layer.type == LayerType::Vector) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Automatic Weights", ImVec2(half, 0))) {
      operator_call(C, "weight.automatic");
    }
    ImGui::SameLine();
    if (ImGui::Button("Normalize", ImVec2(half, 0))) {
      operator_call(C, "weight.normalize");
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped(parent ? "The layer follows its parent bone. Giving its points weights sets Parent Bone to "
                                "None."
                              : "Points follow the bones by their weights. Select points with the Weight Select tool "
                                "to assign weights, or paint them with Weight Paint.");
  }
  ui_panel_end();
}

/* --- Modifiers tab, laid out like Blender's ------------------------------------------------ */

static constexpr ImU32 COL_MOD_BOX = IM_COL32(0x3d, 0x3d, 0x3d, 255);
static constexpr ImU32 COL_MOD_ICON = IM_COL32(0xd0, 0xd0, 0xd0, 255);
static constexpr const char *MODIFIER_PAYLOAD = "SKY_MODIFIER";

/* A small square toggle with a drawn icon, blue when on, as Blender's header toggles. */
static bool icon_toggle(const char *id, bool on, const char *tooltip, void (*icon)(ImDrawList *, ImVec2, float))
{
  const float h = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 back = on ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                        : ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
  dl->AddRectFilled(at, ImVec2(at.x + h, at.y + h), back, 3.0f);
  icon(dl, ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.32f);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return pressed;
}

static void icon_monitor(ImDrawList *dl, ImVec2 c, float s)
{
  dl->AddRect(ImVec2(c.x - s, c.y - s * 0.8f), ImVec2(c.x + s, c.y + s * 0.45f), COL_MOD_ICON, 1.0f, 0, 1.3f);
  dl->AddLine(ImVec2(c.x, c.y + s * 0.45f), ImVec2(c.x, c.y + s * 0.85f), COL_MOD_ICON, 1.3f);
  dl->AddLine(ImVec2(c.x - s * 0.5f, c.y + s * 0.9f), ImVec2(c.x + s * 0.5f, c.y + s * 0.9f), COL_MOD_ICON, 1.3f);
}

static void icon_camera(ImDrawList *dl, ImVec2 c, float s)
{
  dl->AddRect(ImVec2(c.x - s, c.y - s * 0.6f), ImVec2(c.x + s, c.y + s * 0.75f), COL_MOD_ICON, 2.0f, 0, 1.3f);
  dl->AddCircle(ImVec2(c.x, c.y + s * 0.07f), s * 0.42f, COL_MOD_ICON, 0, 1.3f);
  dl->AddRectFilled(ImVec2(c.x - s * 0.5f, c.y - s * 0.9f), ImVec2(c.x + s * 0.1f, c.y - s * 0.6f), COL_MOD_ICON);
}

/* The modifier's type icon: two mirrored halves, or a bone. */
static void modifier_type_icon(ImDrawList *dl, ImVec2 c, float s, ModifierType type)
{
  if (type == ModifierType::Mirror) {
    dl->AddTriangle(ImVec2(c.x - s * 0.15f, c.y - s), ImVec2(c.x - s * 0.15f, c.y + s), ImVec2(c.x - s, c.y + s),
                    COL_MOD_ICON, 1.3f);
    dl->AddTriangleFilled(ImVec2(c.x + s * 0.15f, c.y - s), ImVec2(c.x + s, c.y + s), ImVec2(c.x + s * 0.15f, c.y + s),
                          COL_MOD_ICON);
  }
  else {
    const ImVec2 head(c.x - s * 0.8f, c.y + s * 0.8f), tail(c.x + s * 0.8f, c.y - s * 0.8f);
    const ImVec2 mid(c.x - s * 0.45f, c.y + s * 0.45f), n(s * 0.3f, s * 0.3f);
    const ImVec2 quad[4] = {head, ImVec2(mid.x + n.x, mid.y + n.y), tail, ImVec2(mid.x - n.x, mid.y - n.y)};
    dl->AddPolyline(quad, 4, COL_MOD_ICON, ImDrawFlags_Closed, 1.3f);
  }
}

/* One row of a modifier's body: a right-aligned label, then the value from the middle. */
static void modifier_row_label(const char *label, float column)
{
  const float x = ImGui::GetCursorPosX();
  const float w = ImGui::CalcTextSize(label).x;
  ImGui::SetCursorPosX(x + std::max(0.0f, column - w - ImGui::GetStyle().ItemSpacing.x));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(x + column);
}

/* Blender's row of axis toggles (X, Y): joined buttons, blue when on. */
static bool axis_toggles(const char *id, bool *x, bool *y)
{
  bool changed = false;
  const float w = (ImGui::GetContentRegionAvail().x - 1.0f) * 0.5f;
  ImGui::PushID(id);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, ImGui::GetStyle().ItemSpacing.y));
  for (int i = 0; i < 2; i++) {
    bool *value = i == 0 ? x : y;
    if (i > 0) {
      ImGui::SameLine();
    }
    if (*value) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(i == 0 ? "X" : "Y", ImVec2(w, 0))) {
      *value = !*value;
      changed = true;
    }
    if (*value) {
      ImGui::PopStyleColor();
    }
  }
  ImGui::PopStyleVar();
  ImGui::PopID();
  return changed;
}

/* Blender's modifier stack: Add Modifier, then a box per modifier, top first. Drag a box by
 * its handle (the dots on the right) to change the order. */
static void modifiers_panel(Context &C, Layer &layer)
{
  Document &doc = *C.document;
  const ImGuiStyle &style = ImGui::GetStyle();
  if (ImGui::Button("+  Add Modifier", ImVec2(-1, 0))) {
    ImGui::OpenPopup("##add_modifier");
  }
  if (ImGui::BeginPopup("##add_modifier")) {
    ImGui::TextDisabled("Generate");
    if (ImGui::MenuItem("Mirror")) {
      operator_call(C, "object.modifier_add", {{"type", "MIRROR"}});
    }
    ImGui::EndPopup();
  }
  ImGui::Spacing();

  int remove = -1, drag_from = -1, drag_to = -1;
  const float h = ImGui::GetFrameHeight();
  for (int i = 0; i < int(layer.modifiers.size()); i++) {
    Modifier &m = layer.modifiers[i];
    const bool armature = m.type == ModifierType::Armature;
    ImGui::PushID(i);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const ImVec2 box_min = ImGui::GetCursorScreenPos();
    const float box_w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorScreenPos(ImVec2(box_min.x + 4.0f, box_min.y + 4.0f));
    ImGui::BeginGroup();

    /* Header: collapse arrow, icon, name, toggles, X, drag handle. */
    const float inner_w = box_w - 8.0f;
    if (ImGui::ArrowButtonEx("##expand", m.show_expanded ? ImGuiDir_Down : ImGuiDir_Right, ImVec2(h * 0.8f, h),
                             ImGuiButtonFlags_None)) {
      m.show_expanded = !m.show_expanded;
    }
    ImGui::SameLine(0, 2);
    const ImVec2 icon_at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    modifier_type_icon(dl, ImVec2(icon_at.x + h * 0.5f, icon_at.y + h * 0.5f), h * 0.3f, m.type);
    ImGui::SameLine(0, 2);
    const float right_w = h * 3.0f + h * 0.8f + style.ItemSpacing.x * 3.0f + 6.0f;
    ImGui::SetNextItemWidth(std::max(40.0f, inner_w - (ImGui::GetCursorScreenPos().x - box_min.x) - right_w));
    ImGui::InputText("##name", &m.name);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Rename Modifier", doc);
    }
    ImGui::SameLine(0, 4);
    if (icon_toggle("##viewport", m.show_viewport, "Display modifier in viewport", icon_monitor)) {
      m.show_viewport = !m.show_viewport;
      C.undo->push("Modifier in Viewport", doc);
    }
    ImGui::SameLine(0, 1);
    if (icon_toggle("##render", m.show_render, "Use modifier during render", icon_camera)) {
      m.show_render = !m.show_render;
      C.undo->push("Modifier in Renders", doc);
    }
    ImGui::SameLine(0, 4);
    ImGui::BeginDisabled(armature);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    if (ImGui::Button("X", ImVec2(h, h))) {
      remove = i;
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(armature ? "The Armature modifier comes with the bone layer and can't be removed"
                                 : "Delete modifier");
    }
    ImGui::SameLine(0, 2);
    /* The drag handle. */
    const ImVec2 grip = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##grip", ImVec2(h * 0.8f, h));
    const bool grip_hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 2; c++) {
        dl->AddCircleFilled(ImVec2(grip.x + h * 0.25f + c * h * 0.3f, grip.y + h * 0.3f + r * h * 0.2f), 1.4f,
                            grip_hovered ? COL_MOD_ICON : IM_COL32(0x90, 0x90, 0x90, 255));
      }
    }
    if (grip_hovered) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(MODIFIER_PAYLOAD, &i, sizeof(int));
      ImGui::TextUnformatted(m.name.c_str());
      ImGui::EndDragDropSource();
    }

    /* Body. */
    if (m.show_expanded) {
      ImGui::Spacing();
      const float column = inner_w * 0.38f;
      ImGui::PushItemWidth(-4.0f);
      if (armature) {
        const Layer *bones = layer_armature(doc, layer.id);
        modifier_row_label("Object", column);
        ImGui::BeginDisabled();
        std::string name = bones ? bones->name : std::string("(none)");
        ImGui::InputText("##object", &name, ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
        modifier_row_label("", column);
        ImGui::TextDisabled("The bone layer's bones move this drawing.");
      }
      else {
        bool changed = false;
        modifier_row_label("Axis", column);
        changed |= axis_toggles("##axis", &m.axis_x, &m.axis_y);
        modifier_row_label("Mirror Center", column);
        ImGui::DragFloat2("##center", &m.center.x, 0.005f, -1000.0f, 1000.0f, "%.3f");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::Spacing();
        modifier_row_label("Merge", column);
        changed |= ImGui::Checkbox("##merge", &m.merge);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("Share points on the axis, so lines ending there join their reflection");
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!m.merge);
        ImGui::DragFloat("##merge_distance", &m.merge_distance, 0.0005f, 0.0f, 1.0f, "%.4f");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::EndDisabled();
        ImGui::Spacing();
        /* Blender keeps vertex group mirroring under Data. */
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
        if (ImGui::TreeNodeEx("Data", ImGuiTreeNodeFlags_SpanAvailWidth)) {
          modifier_row_label("", column);
          changed |= ImGui::Checkbox("Mirror Bones", &m.mirror_bones);
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Reflected points bound (or weighted) to ..._L bones follow ..._R ones, and the "
                              "other way (Blender's Vertex Groups)");
          }
          ImGui::TreePop();
        }
        ImGui::PopStyleColor();
        if (changed) {
          C.undo->push("Mirror Settings", doc);
        }
      }
      ImGui::PopItemWidth();
    }
    ImGui::EndGroup();
    const ImVec2 box_max(box_min.x + box_w, ImGui::GetItemRectMax().y + 6.0f);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(box_min, box_max, COL_MOD_BOX, 5.0f);
    dl->AddRect(box_min, box_max, IM_COL32(0x2a, 0x2a, 0x2a, 255), 5.0f, 0, 1.0f);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(box_min);
    ImGui::Dummy(ImVec2(box_w, box_max.y - box_min.y));
    /* Dropping a modifier on this box puts it here (above or below by the half). */
    if (ImGui::BeginDragDropTarget()) {
      const bool upper = ImGui::GetIO().MousePos.y < (box_min.y + box_max.y) * 0.5f;
      const float line_y = upper ? box_min.y - 3.0f : box_max.y + 3.0f;
      ImGui::GetWindowDrawList()->AddLine(ImVec2(box_min.x, line_y), ImVec2(box_max.x, line_y), UI_DROP_LINE_COLOR,
                                          2.0f);
      if (const ImGuiPayload *payload =
              ImGui::AcceptDragDropPayload(MODIFIER_PAYLOAD, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        drag_from = *static_cast<const int *>(payload->Data);
        drag_to = upper ? i : i + 1;
      }
      ImGui::EndDragDropTarget();
    }
    ImGui::Spacing();
    ImGui::PopID();
  }
  if (remove >= 0) {
    operator_call(C, "object.modifier_remove", {{"index", std::to_string(remove)}});
  }
  if (drag_from >= 0) {
    /* Index the modifier ends up at, after taking it out of its place. */
    const int to = drag_to > drag_from ? drag_to - 1 : drag_to;
    if (to != drag_from) {
      operator_call(C, "object.modifier_move_to", {{"index", std::to_string(drag_from)}, {"to", std::to_string(to)}});
    }
  }
}

static void layer_tab(Context &C)
{
  Document &doc = *C.document;
  Layer *layer = document_active_layer(doc);
  if (!layer) {
    ImGui::TextDisabled("No active layer.");
    return;
  }
  const rna::PointerRNA ptr{layer, &Layer::rna()};
  const char *path = "sky.data.layers.active";
  if (ui_panel_begin("Layer")) {
    ui_prop(C, ptr, "name", path);
    ui_prop(C, ptr, "visible", path);
    ui_prop(C, ptr, "render_visible", path);
    ui_prop(C, ptr, "locked", path);
    ui_prop(C, ptr, "label_color", path);
    ui_panel_end();
  }
  if (ui_panel_begin("Transform")) {
    ui_prop_pair(C, ptr, "Location", "location_x", "X", "location_y", "Y", path);
    ui_prop(C, ptr, "rotation", path);
    ui_prop_pair(C, ptr, "Scale", "scale_x", "X", "scale_y", "Y", path);
    ui_panel_end();
  }
  if (ui_panel_begin("Compositing")) {
    ui_prop(C, ptr, "opacity", path);
    ui_panel_end();
  }
  /* Masking, as Moho sets it: a group turns masking on for its layers (and whether they start
   * shown or hidden); each layer inside then masks, adds to or cuts from the mask. */
  if (ui_panel_begin("Masking")) {
    const Layer *parent = layer_parent(doc, layer->id);
    if (layer->is_group()) {
      ui_prop(C, ptr, "group_mask", path);
    }
    if (parent) {
      ui_prop(C, ptr, "mask_mode", path);
      ui_prop(C, ptr, "mask_expand", path);
      /* Only vector layers have strokes to leave out, as in Moho. */
      if (!layer->is_group()) {
        ui_prop(C, ptr, "mask_exclude_strokes", path);
      }
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    if (parent && parent->group_mask == GroupMask::None) {
      ImGui::TextWrapped("Takes effect once the group \"%s\" has masking on.", parent->name.c_str());
    }
    else if (!parent && !layer->is_group()) {
      ImGui::TextWrapped("Masking works inside a group: turn it on for the group, then set how each layer "
                         "inside takes part.");
    }
    ui_panel_end();
  }
  if (const Layer *armature = layer->is_bone_layer() ? nullptr : layer_armature(doc, layer->id)) {
    binding_panel(C, *layer, *layer_find(doc, armature->id));
  }
  if (layer->is_reference()) {
    if (ImGui::CollapsingHeader("Reference", ImGuiTreeNodeFlags_DefaultOpen)) {
      const Layer *source = layer_find(doc, layer->reference_source);
      ImGui::TextWrapped("Shows the drawing of \"%s\". Settings here (masking, opacity, transform) are this "
                         "layer's own.",
                         source ? source->name.c_str() : "a deleted layer");
      if (ImGui::Button("Break Reference")) {
        operator_call(C, "layer.break_reference");
      }
    }
  }
}

static void shape_panel(Context &C, VectorDrawing &drawing, Shape &shape);
static void point_panel(Context &C, VectorDrawing &drawing);
static void edge_panel(Context &C, VectorDrawing &drawing);

static void drawing_tab(Context &C)
{
  Document &doc = *C.document;
  Layer *active = document_active_layer(doc);
  if (!active || active->is_group()) {
    ImGui::TextDisabled("Select a vector layer to see its shapes.");
    return;
  }
  Layer *layer = nullptr;
  VectorDrawing *drawing = editable_drawing(C, &layer);
  const VectorDrawing &shown = layer_drawing(doc, *active);
  ImGui::TextDisabled("%d points, %d curves, %d shapes", int(shown.points.size()), int(shown.curves.size()),
                      int(shown.shapes.size()));
  if (!drawing) {
    ImGui::TextDisabled(active->is_reference() ? "A reference shows its original's shapes; edit the original."
                                               : "The layer is locked.");
    return;
  }

  /* Shapes, top first like the layer list. The highlight follows the viewport: a shape is
   * selected when any of its points is, and clicking one here selects its points. Dragging a
   * shape onto another moves it to that place in the stacking order. */
  const std::vector<bool> selected = drawing_selected_shapes(*drawing);
  int selected_shape = -1;
  int move_from = -1, move_to = -1;
  if (ImGui::CollapsingHeader("Shapes", ImGuiTreeNodeFlags_DefaultOpen)) {
    const float list_h = std::min(float(drawing->shapes.size()) + 1.0f, 10.0f) * ImGui::GetFrameHeight();
    if (ImGui::BeginChild("##shapes", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
      for (int s = int(drawing->shapes.size()) - 1; s >= 0; s--) {
        Shape &shape = drawing->shapes[s];
        ImGui::PushID(s);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float sw = ImGui::GetTextLineHeight();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (ImGui::Selectable("##shape", selected[s], 0, ImVec2(0, ImGui::GetTextLineHeight()))) {
          /* Shift or Ctrl adds to the selection, as in the viewport. */
          if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
            for (MeshPoint &pt : drawing->points) {
              pt.selected = false;
            }
          }
          for (const ShapeEdge &e : shape.edges) {
            int a, b;
            curve_segment_points(drawing->curves[e.curve], e.segment, &a, &b);
            drawing->points[a].selected = drawing->points[b].selected = true;
          }
          C.undo->push("Select Shape", doc);
        }
        if (ImGui::BeginDragDropSource()) {
          ImGui::SetDragDropPayload("SKY_SHAPE", &s, sizeof(int));
          ImGui::TextUnformatted(shape.name.empty() ? "Shape" : shape.name.c_str());
          ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
          /* The list is top first: the upper half drops above this shape (stacked over it). */
          const ImVec2 row_min = ImGui::GetItemRectMin(), row_max = ImGui::GetItemRectMax();
          const bool upper = ImGui::GetIO().MousePos.y < (row_min.y + row_max.y) * 0.5f;
          const float line_y = upper ? row_min.y : row_max.y;
          ImGui::GetWindowDrawList()->AddLine(ImVec2(row_min.x, line_y), ImVec2(row_max.x, line_y),
                                              UI_DROP_LINE_COLOR, 2.0f);
          if (const ImGuiPayload *payload =
                  ImGui::AcceptDragDropPayload("SKY_SHAPE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
          {
            move_from = *static_cast<const int *>(payload->Data);
            /* Where it goes in the stacking order (bottom first), counted before it is taken out. */
            move_to = upper ? s + 1 : s;
          }
          ImGui::EndDragDropTarget();
        }
        /* Fill and line swatches, then the name. */
        const auto swatch = [&](float x, const Color &c, bool on) {
          const ImVec2 a = p + ImVec2(x, 1), b = a + ImVec2(sw - 2, sw - 2);
          if (on) {
            dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)), 2.0f);
          }
          dl->AddRect(a, b, IM_COL32(0x80, 0x80, 0x80, 255), 2.0f);
        };
        swatch(2.0f, shape.style.fill, shape.has_fill);
        swatch(sw + 4.0f, shape.style.line, shape.has_outline);
        dl->AddText(p + ImVec2(sw * 2.0f + 10.0f, 0), IM_COL32(0xe6, 0xe6, 0xe6, 255), shape.name.c_str());
        ImGui::PopID();
      }
      if (drawing->shapes.empty()) {
        ImGui::TextDisabled("No shapes yet. Draw, or select points and press F.");
      }
    }
    ImGui::EndChild();
  }
  /* After the list, so its rows weren't shifting while it drew. */
  if (move_from >= 0) {
    const int to = move_to > move_from ? move_to - 1 : move_to;
    if (to != move_from) {
      Shape moved = std::move(drawing->shapes[move_from]);
      drawing->shapes.erase(drawing->shapes.begin() + move_from);
      drawing->shapes.insert(drawing->shapes.begin() + to, std::move(moved));
      C.undo->push("Move Shape", doc);
      return;
    }
  }
  /* The panel shows the topmost selected shape. */
  for (int s = 0; s < int(drawing->shapes.size()); s++) {
    if (selected[s]) {
      selected_shape = s;
    }
  }
  if (selected_shape >= 0) {
    shape_panel(C, *drawing, drawing->shapes[selected_shape]);
  }
  edge_panel(C, *drawing);
  point_panel(C, *drawing);
}

/* The topmost selected shape's name, fill, outline and line width. */
/* "Label  [X 0.000] [Y 0.000]": the average position of some points; dragging moves them all
 * (layer space). */
static void mean_position_row(Context &C, const char *label, VectorDrawing &drawing, const std::vector<int> &points,
                              const char *tooltip, const char *undo_name)
{
  float2 mean;
  if (!drawing_points_mean(drawing, points, &mean)) {
    return;
  }
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  const float label_w = ImGui::CalcTextSize(label).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetColumnWidth() - label_w));
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::PushID(label);
  const float spacing = 2.0f;
  const float w = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
  bool done = false;
  for (int axis = 0; axis < 2; axis++) {
    if (axis > 0) {
      ImGui::SameLine(0.0f, spacing);
    }
    ImGui::SetNextItemWidth(axis == 0 ? w : -FLT_MIN);
    float value = axis == 0 ? mean.x : mean.y;
    if (ImGui::DragFloat(axis == 0 ? "##x" : "##y", &value, 0.005f, -100000.0f, 100000.0f,
                         axis == 0 ? "X  %.3f" : "Y  %.3f")) {
      drawing_points_move(drawing, points, axis == 0 ? float2(value - mean.x, 0.0f) : float2(0.0f, value - mean.y));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", tooltip);
    }
    done |= ImGui::IsItemDeactivatedAfterEdit();
  }
  ImGui::PopID();
  if (done) {
    C.undo->push(undo_name, *C.document);
  }
}

static void shape_panel(Context &C, VectorDrawing &drawing, Shape &shape)
{
  Document &doc = *C.document;
  if (ui_panel_begin("Shape")) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##name", &shape.name);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Rename Shape", doc);
    }
    mean_position_row(C, "Position", drawing, shape_points(drawing, shape),
                      "The middle of the shape's points; changing it moves the whole shape", "Move Shape");

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Checkbox("Fill", &shape.has_fill)) {
      C.undo->push("Shape Fill", doc);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Outline", &shape.has_outline)) {
      C.undo->push("Shape Outline", doc);
    }

    float fill[4] = {shape.style.fill.r, shape.style.fill.g, shape.style.fill.b, shape.style.fill.a};
    if (ui_color(C, "Fill Color", fill, "Fill Color")) {
      shape.style.fill = {fill[0], fill[1], fill[2], fill[3]};
    }
    float line[4] = {shape.style.line.r, shape.style.line.g, shape.style.line.b, shape.style.line.a};
    if (ui_color(C, "Line Color", line, "Line Color")) {
      shape.style.line = {line[0], line[1], line[2], line[3]};
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Line Width");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    const float units_to_pixels = float(doc.height) * 0.5f;
    float pixels = shape.style.line_width * units_to_pixels;
    if (ImGui::DragFloat("##width", &pixels, 0.1f, 0.0f, 500.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp)) {
      shape.style.line_width = pixels / units_to_pixels;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Line Width", doc);
    }

    /* Moho's brush: the button opens Brush Settings; OK gives every selected shape the result. */
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Brush");
    ImGui::TableSetColumnIndex(1);
    const std::string &brush = shape.style.brush.name;
    const bool missing = !brush.empty() && !brush_find(brush);
    if (ImGui::Button(((brush.empty() ? std::string("None") : brush) + "##brush").c_str(), ImVec2(-1, 0))) {
      Context *ctx = &C;
      brush_settings_open(shape.style.brush, shape.style, [ctx](const BrushSettings &settings) {
        if (shapes_set_brush(*ctx, settings) > 0) {
          ctx->undo->push("Brush Settings", *ctx->document);
        }
      });
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(missing ? "This brush isn't in the asset library: the line is drawn plain"
                                : "Paint the line with a brush from the asset library");
    }
    if (missing) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Not in the asset library");
    }
    ui_panel_end();
  }
}

/* The selected edges (segments with both points selected). Visible is Moho's Hide Edge: off,
 * the outline isn't drawn along them, though fills still use them as a boundary. */
static void edge_panel(Context &C, VectorDrawing &drawing)
{
  const std::vector<ShapeEdge> edges = drawing_selected_edges(drawing);
  if (edges.empty() || !ui_panel_begin("Edge")) {
    return;
  }
  bool visible = true;
  for (const ShapeEdge &e : edges) {
    visible = visible && !drawing.curves[e.curve].points[e.segment].edge_hidden;
  }
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(edges.size() == 1 ? "1 edge" : (std::to_string(edges.size()) + " edges").c_str());
  ImGui::TableSetColumnIndex(1);
  if (ImGui::Checkbox("Visible", &visible)) {
    drawing_set_edges_hidden(drawing, edges, !visible);
    C.undo->push(visible ? "Show Edge" : "Hide Edge", *C.document);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Draw the outline along the selected edges (Moho's Hide Edge when off)");
  }
  ui_panel_end();
}

/* The selected points. Mean Radius shows their average radius; changing it moves every
 * selected point's radius by the same amount, as Blender's Mean Radius field does. */
static void point_panel(Context &C, VectorDrawing &drawing)
{
  const float mean = drawing_selected_mean_radius(drawing);
  if (mean < 0.0f || !ui_panel_begin("Point")) {
    return;
  }
  mean_position_row(C, "Mean Position", drawing, drawing_selected_points(drawing),
                    "The average position of the selected points; changing it moves them all", "Move Points");
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Mean Radius");
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-1);
  float value = mean;
  if (ImGui::SliderFloat("##mean_radius", &value, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
    drawing_set_selected_mean_radius(drawing, value);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Stroke radius at the selected points, as a fraction of the shape's line width");
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    C.undo->push("Point Radius", *C.document);
  }

  /* Corner: how strokes turn here and end here. Neither button lights up for a mix. */
  bool all_round = true, all_sharp = true;
  for (const MeshPoint &p : drawing.points) {
    if (p.selected) {
      all_round = all_round && p.corner == CornerType::Round;
      all_sharp = all_sharp && p.corner == CornerType::Sharp;
    }
  }
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Corner");
  ImGui::TableSetColumnIndex(1);
  const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  const struct {
    const char *label;
    CornerType type;
    bool on;
    const char *tooltip;
  } choices[] = {{"Round", CornerType::Round, all_round, "Round joins at corners and round caps at stroke ends"},
                 {"Sharp", CornerType::Sharp, all_sharp, "Mitred corners (bevelled when very tight) and flat stroke ends"}};
  for (int i = 0; i < 2; i++) {
    if (i > 0) {
      ImGui::SameLine();
    }
    if (choices[i].on) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool pressed = ImGui::Button(choices[i].label, ImVec2(half, 0));
    if (choices[i].on) {
      ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", choices[i].tooltip);
    }
    if (pressed && !choices[i].on) {
      drawing_set_selected_corner(drawing, choices[i].type);
      C.undo->push(i == 0 ? "Round Corner" : "Sharp Corner", *C.document);
    }
  }

  /* Align: the handles stay in line (moving one turns the other); a mix shows a dash. */
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Handles");
  ImGui::TableSetColumnIndex(1);
  const int alignment = drawing_selected_alignment(drawing);
  bool aligned = alignment == 1;
  if (alignment == -2) {
    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
  }
  if (ImGui::Checkbox("Aligned", &aligned)) {
    operator_call(C, aligned ? "mesh.handles_align" : "mesh.handles_unalign");
  }
  if (alignment == -2) {
    ImGui::PopItemFlag();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Keep each point's two handles in line: moving one turns the other (Alt moves one alone)");
  }
  ui_panel_end();
}

void properties_draw(Area &area, Context &C)
{
  area_editor_type_menu(area);
  ImGui::SameLine();
  /* A bone layer has an Armature tab where a drawing has its Drawing tab. */
  const Layer *active_layer = document_active_layer(*C.document);
  const bool bones = active_layer && active_layer->is_bone_layer();
  /* Drawings have a Modifiers tab, left of their Drawing tab, as Blender's modifier tab sits
   * beside the object data tab. */
  const bool modifiers = active_layer && !active_layer->is_group();
  struct TabEntry {
    const char *label;
    int id;
  };
  std::vector<TabEntry> tabs = {{"Document", TAB_DOCUMENT}, {"Layer", TAB_LAYER}};
  if (modifiers) {
    tabs.push_back({"Modifiers", TAB_MODIFIERS});
  }
  tabs.push_back({bones ? "Armature" : "Drawing", TAB_DRAWING});
  if (area.properties_tab == TAB_MODIFIERS && !modifiers) {
    area.properties_tab = TAB_LAYER;
  }
  for (size_t i = 0; i < tabs.size(); i++) {
    if (i > 0) {
      ImGui::SameLine(0, 2);
    }
    const bool active = area.properties_tab == tabs[i].id;
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(tabs[i].label)) {
      area.properties_tab = tabs[i].id;
    }
    if (active) {
      ImGui::PopStyleColor();
    }
  }
  ImGui::Separator();
  if (ImGui::BeginChild("##properties", ImVec2(0, 0))) {
    switch (area.properties_tab) {
      case TAB_LAYER:
        layer_tab(C);
        break;
      case TAB_MODIFIERS:
        modifiers_panel(C, *document_active_layer(*C.document));
        break;
      case TAB_DRAWING:
        if (bones) {
          bones_panel(C, *document_active_layer(*C.document));
        }
        else {
          drawing_tab(C);
        }
        break;
      default:
        document_tab(C);
        break;
    }
  }
  ImGui::EndChild();
}

}  // namespace sky
