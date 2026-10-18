/* The Layers editor: Moho's layer panel with Blender outliner habits. A tree of layers (top of
 * the stack first) with visibility and lock toggles, label colours, type icons, rename on
 * double-click, drag to reorder or drop into a group, and a right-click menu. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cstring>

#include "data/document.hh"
#include "editors/editors.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "wm/keymap.hh"

namespace sky {

static constexpr const char *LAYER_PAYLOAD = "SKY_LAYER";
static constexpr ImU32 COL_ICON = IM_COL32(0xc8, 0xc8, 0xc8, 255);
static constexpr ImU32 COL_ICON_OFF = IM_COL32(0x70, 0x70, 0x70, 255);
static constexpr ImU32 COL_ACTIVE_ROW = IM_COL32(0x33, 0x4d, 0x80, 255);
static constexpr ImU32 COL_REFERENCE = IM_COL32(0x6f, 0xb8, 0xff, 255);

/* --- Icons, drawn with lines. --- */

static void icon_eye(ImDrawList *dl, ImVec2 c, float s, bool open)
{
  const ImU32 col = open ? COL_ICON : COL_ICON_OFF;
  dl->PathArcTo(c + ImVec2(0, s * 0.9f), s * 1.2f, -PI * 0.78f, -PI * 0.22f, 10);
  dl->PathArcTo(c - ImVec2(0, s * 0.9f), s * 1.2f, PI * 0.22f, PI * 0.78f, 10);
  dl->PathStroke(col, ImDrawFlags_Closed, 1.3f);
  if (open) {
    dl->AddCircleFilled(c, s * 0.32f, col);
  }
  else {
    dl->AddLine(c + ImVec2(-s * 0.9f, s * 0.6f), c + ImVec2(s * 0.9f, -s * 0.6f), col, 1.3f);
  }
}

static void icon_lock(ImDrawList *dl, ImVec2 c, float s, bool locked)
{
  const ImU32 col = locked ? COL_ICON : COL_ICON_OFF;
  dl->AddRectFilled(c + ImVec2(-s * 0.6f, -s * 0.1f), c + ImVec2(s * 0.6f, s * 0.75f), col, 1.0f);
  const float shift = locked ? 0.0f : s * 0.45f;
  dl->PathArcTo(c + ImVec2(shift, -s * 0.15f), s * 0.38f, PI, 2.0f * PI, 8);
  dl->PathStroke(col, 0, 1.3f);
}

static void icon_layer_type(ImDrawList *dl, ImVec2 c, float s, const Layer &layer)
{
  const ImU32 col = layer.is_reference() ? COL_REFERENCE : COL_ICON;
  if (layer.is_group()) {
    /* A folder. */
    dl->AddRectFilled(c + ImVec2(-s * 0.8f, -s * 0.55f), c + ImVec2(-s * 0.1f, -s * 0.3f), col, 1.0f);
    dl->AddRectFilled(c + ImVec2(-s * 0.8f, -s * 0.35f), c + ImVec2(s * 0.8f, s * 0.6f), col, 1.5f);
  }
  else {
    /* A curve with a point: a vector layer. */
    dl->AddBezierCubic(c + ImVec2(-s * 0.8f, s * 0.5f), c + ImVec2(-s * 0.3f, -s * 0.9f),
                       c + ImVec2(s * 0.3f, s * 0.9f), c + ImVec2(s * 0.8f, -s * 0.5f), col, 1.4f, 12);
    dl->AddCircleFilled(c + ImVec2(s * 0.8f, -s * 0.5f), s * 0.18f, col);
  }
  if (layer.is_reference()) {
    /* Two chain links for a reference. */
    dl->AddCircle(c + ImVec2(s * 0.55f, s * 0.6f), s * 0.25f, COL_REFERENCE, 8, 1.2f);
    dl->AddCircle(c + ImVec2(s * 0.85f, s * 0.6f), s * 0.25f, COL_REFERENCE, 8, 1.2f);
  }
}

/* A short label for a layer's masking mode, shown when its group masks. */
static const char *mask_badge(MaskMode mode)
{
  switch (mode) {
    case MaskMode::Masked:
      return "";
    case MaskMode::NotMasked:
      return "not masked";
    case MaskMode::Add:
      return "+ mask";
    case MaskMode::Subtract:
      return "- mask";
    case MaskMode::AddInvisible:
      return "+ mask (hidden)";
    case MaskMode::SubtractInvisible:
      return "- mask (hidden)";
    case MaskMode::ClearAdd:
      return "clear + mask";
    case MaskMode::ClearAddInvisible:
      return "clear + mask (hidden)";
  }
  return "";
}

static bool menu_op(Context &C, const char *label, const char *idname)
{
  const OperatorType *ot = operators().find(idname);
  if (!ot) {
    return false;
  }
  const std::string shortcut = keymaps().shortcut_for(idname);
  if (ImGui::MenuItem(label, shortcut.empty() ? nullptr : shortcut.c_str(), false, operator_poll(C, *ot))) {
    operator_call(C, idname);
    return true;
  }
  return false;
}

/* Changes made directly by the editor (not through operators) get their undo step here. */
static void push_undo(Context &C, const char *name)
{
  C.undo->push(name, *C.document);
}

struct DropRequest {
  uint64_t moving = 0;
  uint64_t target = 0;
  /* -1 above the target, +1 below it, 0 into it (groups). */
  int where = 0;
};

static void layer_context_menu(Context &C, Area &area, Layer &layer, bool parent_masks)
{
  Document &doc = *C.document;
  doc.active_layer = layer.id;
  if (ImGui::MenuItem("Rename", "F2")) {
    area.renaming_layer = layer.id;
  }
  ImGui::Separator();
  menu_op(C, "Duplicate", "layer.duplicate");
  menu_op(C, "Duplicate as Reference", "layer.make_reference");
  menu_op(C, "Break Reference", "layer.break_reference");
  menu_op(C, "Group", "layer.group");
  ImGui::Separator();
  if (ImGui::BeginMenu("Label Color")) {
    const rna::PropertyRNA *prop = Layer::rna().find("label_color");
    for (int i = 0; i < int(prop->items.size()); i++) {
      if (ImGui::MenuItem(prop->items[i].name.c_str(), nullptr, int(layer.label_color) == i)) {
        layer.label_color = LabelColor(i);
        push_undo(C, "Label Color");
      }
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("Masking", parent_masks)) {
    const rna::PropertyRNA *prop = Layer::rna().find("mask_mode");
    for (int i = 0; i < int(prop->items.size()); i++) {
      if (ImGui::MenuItem(prop->items[i].name.c_str(), nullptr, int(layer.mask_mode) == i)) {
        layer.mask_mode = MaskMode(i);
        push_undo(C, "Masking");
      }
    }
    ImGui::EndMenu();
  }
  if (layer.is_group() && ImGui::BeginMenu("Group Mask")) {
    const rna::PropertyRNA *prop = Layer::rna().find("group_mask");
    for (int i = 0; i < int(prop->items.size()); i++) {
      if (ImGui::MenuItem(prop->items[i].name.c_str(), nullptr, int(layer.group_mask) == i)) {
        layer.group_mask = GroupMask(i);
        push_undo(C, "Group Mask");
      }
    }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  menu_op(C, "Delete", "layer.delete");
}

static void layer_row(Context &C, Area &area, Layer &layer, int depth, bool parent_masks, DropRequest &drop,
                      bool &r_context_open)
{
  Document &doc = *C.document;
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const float icon = h * 0.32f;
  const ImVec2 row_min = ImGui::GetCursorScreenPos();
  const float row_w = ImGui::GetContentRegionAvail().x;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::PushID(int(layer.id));

  /* The row itself: click to make active, drag to move, double-click to rename. */
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##row", ImVec2(row_w, h));
  const bool row_hovered = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    doc.active_layer = layer.id;
  }
  if (row_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    area.renaming_layer = layer.id;
  }
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload(LAYER_PAYLOAD, &layer.id, sizeof(layer.id));
    ImGui::TextUnformatted(layer.name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    /* Upper quarter: above. Lower quarter: below. Middle of a group: into it. */
    const float y = ImGui::GetIO().MousePos.y - row_min.y;
    int where = y < h * 0.5f ? -1 : 1;
    if (layer.is_group() && y > h * 0.25f && y < h * 0.75f) {
      where = 0;
    }
    if (where == 0) {
      dl->AddRect(row_min, row_min + ImVec2(row_w, h), COL_REFERENCE, 3.0f, 0, 2.0f);
    }
    else {
      const float line_y = where < 0 ? row_min.y : row_min.y + h;
      dl->AddLine(ImVec2(row_min.x, line_y), ImVec2(row_min.x + row_w, line_y), COL_REFERENCE, 2.0f);
    }
    if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(LAYER_PAYLOAD,
                                                                   ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
    {
      std::memcpy(&drop.moving, payload->Data, sizeof(drop.moving));
      drop.target = layer.id;
      drop.where = where;
    }
    ImGui::EndDragDropTarget();
  }
  if (row_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("##layer_menu");
  }
  if (ImGui::BeginPopup("##layer_menu")) {
    r_context_open = true;
    layer_context_menu(C, area, layer, parent_masks);
    ImGui::EndPopup();
  }

  if (doc.active_layer == layer.id) {
    dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), COL_ACTIVE_ROW, 3.0f);
  }
  else if (row_hovered) {
    dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), IM_COL32(0xff, 0xff, 0xff, 14), 3.0f);
  }
  const Color label = label_color_value(layer.label_color);
  if (label.a > 0.0f) {
    dl->AddRectFilled(row_min, row_min + ImVec2(4.0f, h),
                      ImGui::ColorConvertFloat4ToU32(ImVec4(label.r, label.g, label.b, 1.0f)));
  }

  /* Expand arrow, type icon and name. */
  float x = row_min.x + 8.0f + float(depth) * h * 0.8f;
  const float cy = row_min.y + h * 0.5f;
  if (layer.is_group()) {
    ImGui::SetCursorScreenPos(ImVec2(x, row_min.y));
    if (ImGui::InvisibleButton("##expand", ImVec2(h * 0.7f, h))) {
      layer.expanded = !layer.expanded;
    }
    const ImVec2 c(x + h * 0.35f, cy);
    if (layer.expanded) {
      dl->AddTriangleFilled(c + ImVec2(-icon * 0.8f, -icon * 0.4f), c + ImVec2(icon * 0.8f, -icon * 0.4f),
                            c + ImVec2(0, icon * 0.6f), COL_ICON);
    }
    else {
      dl->AddTriangleFilled(c + ImVec2(-icon * 0.4f, -icon * 0.8f), c + ImVec2(icon * 0.6f, 0),
                            c + ImVec2(-icon * 0.4f, icon * 0.8f), COL_ICON);
    }
  }
  x += h * 0.7f;
  icon_layer_type(dl, ImVec2(x + h * 0.4f, cy), icon * 1.3f, layer);
  x += h * 0.9f;

  const float toggles_w = h * 2.0f;
  if (area.renaming_layer == layer.id) {
    ImGui::SetCursorScreenPos(ImVec2(x, row_min.y));
    ImGui::SetNextItemWidth(row_min.x + row_w - toggles_w - x - style.ItemSpacing.x);
    if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("##rename", &layer.name, ImGuiInputTextFlags_AutoSelectAll);
    if (ImGui::IsItemDeactivated()) {
      if (layer.name.empty()) {
        layer.name = document_layer_unique_name(doc, "Layer");
      }
      push_undo(C, "Rename Layer");
      area.renaming_layer = 0;
    }
  }
  else {
    const ImU32 text = layer.visible ? IM_COL32(0xe6, 0xe6, 0xe6, 255) : IM_COL32(0x80, 0x80, 0x80, 255);
    dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), text, layer.name.c_str());
    if (parent_masks && layer.mask_mode != MaskMode::Masked) {
      const float name_w = ImGui::CalcTextSize(layer.name.c_str()).x;
      dl->AddText(ImVec2(x + name_w + 8.0f, cy - ImGui::GetFontSize() * 0.5f), IM_COL32(0x95, 0xd6, 0x00, 255),
                  mask_badge(layer.mask_mode));
    }
    if (layer.is_group() && layer.group_mask != GroupMask::None) {
      const float name_w = ImGui::CalcTextSize(layer.name.c_str()).x;
      dl->AddText(ImVec2(x + name_w + 8.0f, cy - ImGui::GetFontSize() * 0.5f), IM_COL32(0x95, 0xd6, 0x00, 255),
                  layer.group_mask == GroupMask::ShowAll ? "mask: reveal all" : "mask: hide all");
    }
  }

  /* Visibility and lock toggles on the right, as in Blender's outliner. */
  const float right = row_min.x + row_w - style.ItemSpacing.x;
  ImGui::SetCursorScreenPos(ImVec2(right - h * 2.0f, row_min.y));
  if (ImGui::InvisibleButton("##visible", ImVec2(h, h))) {
    layer.visible = !layer.visible;
    push_undo(C, layer.visible ? "Show Layer" : "Hide Layer");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Show or hide the layer");
  }
  icon_eye(dl, ImVec2(right - h * 1.5f, cy), icon, layer.visible);
  ImGui::SetCursorScreenPos(ImVec2(right - h, row_min.y));
  if (ImGui::InvisibleButton("##lock", ImVec2(h, h))) {
    layer.locked = !layer.locked;
    push_undo(C, layer.locked ? "Lock Layer" : "Unlock Layer");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Lock the layer's drawing against edits");
  }
  icon_lock(dl, ImVec2(right - h * 0.5f, cy), icon, layer.locked);

  ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + h));
  ImGui::PopID();

  if (layer.is_group() && layer.expanded) {
    const bool masks = layer.group_mask != GroupMask::None;
    for (Layer &child : layer.children) {
      layer_row(C, area, child, depth + 1, masks, drop, r_context_open);
    }
  }
}

static void apply_drop(Context &C, const DropRequest &drop)
{
  Document &doc = *C.document;
  if (!drop.moving || drop.moving == drop.target) {
    return;
  }
  bool moved = false;
  if (drop.where == 0) {
    moved = document_reparent_layer(doc, drop.moving, drop.target, 0);
  }
  else {
    Layer *parent = layer_parent(doc, drop.target);
    const uint64_t parent_id = parent ? parent->id : 0;
    /* Take the layer out first so the target's index is measured without it. */
    int moving_index = 0;
    std::vector<Layer> *moving_container = layer_container(doc, drop.moving, &moving_index);
    if (!moving_container) {
      return;
    }
    Layer moving = (*moving_container)[moving_index];
    if (parent_id == drop.moving) {
      return;
    }
    for (Layer *p = parent; p; p = layer_parent(doc, p->id)) {
      if (p->id == drop.moving) {
        return; /* A group can't go inside itself. */
      }
    }
    moving_container->erase(moving_container->begin() + moving_index);
    int target_index = 0;
    std::vector<Layer> *target_container = layer_container(doc, drop.target, &target_index);
    if (!target_container) {
      moving_container->insert(moving_container->begin() + moving_index, std::move(moving));
      return;
    }
    const int index = drop.where < 0 ? target_index : target_index + 1;
    target_container->insert(target_container->begin() + index, std::move(moving));
    moved = true;
  }
  if (moved) {
    doc.active_layer = drop.moving;
    C.undo->push("Move Layer", doc);
  }
}

void layers_draw(Area &area, Context &C)
{
  Document &doc = *C.document;
  area_editor_type_menu(area);

  /* Header buttons: add, duplicate as reference, delete, and move up/down. */
  ImGui::SameLine();
  if (ImGui::Button("+")) {
    ImGui::OpenPopup("##add_layer");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Add a layer");
  }
  if (ImGui::BeginPopup("##add_layer")) {
    menu_op(C, "Vector Layer", "layer.new");
    menu_op(C, "Group", "layer.new_group");
    ImGui::Separator();
    menu_op(C, "Reference of Active Layer", "layer.make_reference");
    ImGui::EndPopup();
  }
  const auto op_button = [&](const char *label, const char *idname, const char *tooltip) {
    ImGui::SameLine(0, 2);
    const OperatorType *ot = operators().find(idname);
    ImGui::BeginDisabled(!ot || !operator_poll(C, *ot));
    if (ImGui::Button(label)) {
      operator_call(C, idname);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", tooltip);
    }
  };
  op_button("Dup", "layer.duplicate", "Duplicate the active layer");
  op_button("Ref", "layer.make_reference", "Duplicate the active layer as a reference");
  op_button("Del", "layer.delete", "Delete the active layer");
  op_button("Up", "layer.move_up", "Move the active layer up");
  op_button("Down", "layer.move_down", "Move the active layer down");
  ImGui::Separator();

  DropRequest drop;
  bool context_open = false;
  if (ImGui::BeginChild("##layers", ImVec2(0, 0))) {
    for (Layer &layer : doc.layers) {
      layer_row(C, area, layer, 0, false, drop, context_open);
    }
    if (doc.layers.empty()) {
      ImGui::TextDisabled("No layers. Click + to add one.");
    }
    /* Dropping below the last row moves the layer to the bottom of the top level. */
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, std::max(ImGui::GetContentRegionAvail().y, 8.0f)));
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(LAYER_PAYLOAD)) {
        uint64_t moving;
        std::memcpy(&moving, payload->Data, sizeof(moving));
        if (document_reparent_layer(doc, moving, 0, int(doc.layers.size()))) {
          C.undo->push("Move Layer", doc);
        }
      }
      ImGui::EndDragDropTarget();
    }
  }
  ImGui::EndChild();
  apply_drop(C, drop);

  if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_F2, false) &&
      doc.active_layer)
  {
    area.renaming_layer = doc.active_layer;
  }
}

}  // namespace sky
