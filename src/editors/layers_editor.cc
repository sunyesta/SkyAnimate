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
static constexpr ImU32 COL_SELECTED_ROW = IM_COL32(0x33, 0x4d, 0x80, 130);
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

/* Blender's render toggle: a small camera, struck through when the layer is left out. */
static void icon_camera(ImDrawList *dl, ImVec2 c, float s, bool on)
{
  const ImU32 col = on ? COL_ICON : COL_ICON_OFF;
  dl->AddRect(c + ImVec2(-s * 0.95f, -s * 0.5f), c + ImVec2(s * 0.35f, s * 0.6f), col, 1.5f, 0, 1.3f);
  dl->AddTriangleFilled(c + ImVec2(s * 0.35f, s * 0.05f), c + ImVec2(s * 0.95f, -s * 0.4f),
                        c + ImVec2(s * 0.95f, s * 0.5f), col);
  dl->AddRectFilled(c + ImVec2(-s * 0.75f, -s * 0.85f), c + ImVec2(-s * 0.2f, -s * 0.5f), col, 1.0f);
  if (!on) {
    dl->AddLine(c + ImVec2(-s * 0.95f, s * 0.75f), c + ImVec2(s * 0.95f, -s * 0.85f), col, 1.3f);
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

/* Moho's layer right-click menu: Group Mask on groups, Masking on layers inside a group, then
 * Group With Selection, Duplicate, Reference and Delete. Rename is a double-click or F2; label
 * colours and Break Reference are in the Properties editor's Layer tab. */
static void layer_context_menu(Context &C, Layer &layer, bool in_group)
{
  Document &doc = *C.document;
  doc.active_layer = layer.id;
  bool any = false;
  if (layer.is_group() && ImGui::BeginMenu("Group Mask")) {
    /* Moho's shortcuts; Layer Properties > Masking has the full settings. */
    const Layer *bottom = layer.children.empty() ? nullptr : &layer.children.back();
    const bool inside = layer.group_mask == GroupMask::HideAll && bottom && bottom->mask_mode == MaskMode::Add;
    const bool outside = layer.group_mask == GroupMask::ShowAll && bottom &&
                         bottom->mask_mode == MaskMode::SubtractInvisible;
    const struct {
      const char *label, *mode;
      bool checked;
    } items[] = {{"None", "NONE", layer.group_mask == GroupMask::None},
                 {"Mask - Inside Bottom Layer", "INSIDE", inside},
                 {"Mask - Outside Bottom Layer", "OUTSIDE", outside}};
    for (const auto &item : items) {
      if (ImGui::MenuItem(item.label, nullptr, item.checked, bottom || std::string(item.mode) == "NONE")) {
        operator_call(C, "layer.group_mask_set", {{"mode", item.mode}});
      }
    }
    ImGui::EndMenu();
    any = true;
  }
  if (in_group && ImGui::BeginMenu("Masking")) {
    const rna::PropertyRNA *prop = Layer::rna().find("mask_mode");
    for (int i = 0; i < int(prop->items.size()); i++) {
      if (ImGui::MenuItem(prop->items[i].name.c_str(), nullptr, int(layer.mask_mode) == i)) {
        layer.mask_mode = MaskMode(i);
        push_undo(C, "Masking");
      }
    }
    ImGui::EndMenu();
    any = true;
  }
  if (any) {
    ImGui::Separator();
  }
  menu_op(C, "Group With Selection", "layer.group");
  if (layer.type == LayerType::Group) {
    menu_op(C, "Convert to Bone Group", "layer.convert_to_bone");
  }
  else if (layer.is_bone_layer()) {
    menu_op(C, "Convert to Group", "layer.convert_to_group");
  }
  menu_op(C, "Duplicate Layer", "layer.duplicate");
  menu_op(C, "Reference Layer", "layer.make_reference");
  menu_op(C, "Delete Layer", "layer.delete");
}

/* Clears the extra selection, so only the active layer is picked. */
/* A plain press on an already picked layer: the layer whose release (without a drag) narrows
 * the selection to it. */
static uint64_t g_narrow_on_release = 0;

static void layers_deselect(std::vector<Layer> &layers)
{
  for (Layer &layer : layers) {
    layer.selected = false;
    layers_deselect(layer.children);
  }
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
    /* Ctrl or Shift click picks more layers, as in Moho; a plain click picks just this one. */
    const ImGuiIO &io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyShift) {
      layer.selected = !(layer.selected || doc.active_layer == layer.id);
      if (Layer *previous = layer_find(doc, doc.active_layer); previous && previous != &layer) {
        previous->selected = true;
      }
    }
    else if (layer.selected || doc.active_layer == layer.id) {
      /* Pressing on a picked layer may start dragging the whole selection: keep it, and only
       * narrow it to this layer if the button comes up without a drag. */
      g_narrow_on_release = layer.id;
    }
    else {
      layers_deselect(doc.layers);
    }
    doc.active_layer = layer.id;
  }
  if (g_narrow_on_release == layer.id && ImGui::IsItemDeactivated()) {
    const ImGuiIO &io = ImGui::GetIO();
    const bool dragged = io.MouseDragMaxDistanceSqr[0] > io.MouseDragThreshold * io.MouseDragThreshold;
    if (!dragged && !ImGui::GetDragDropPayload()) {
      layers_deselect(doc.layers);
    }
    g_narrow_on_release = 0;
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
    layer_context_menu(C, layer, depth > 0);
    ImGui::EndPopup();
  }

  if (doc.active_layer == layer.id) {
    dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), COL_ACTIVE_ROW, 3.0f);
  }
  else if (layer.selected) {
    dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), COL_SELECTED_ROW, 3.0f);
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

  const float toggles_w = h * 3.0f;
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

  /* Visibility, render and lock toggles on the right, as in Blender's outliner. */
  const float right = row_min.x + row_w - style.ItemSpacing.x;
  ImGui::SetCursorScreenPos(ImVec2(right - h * 3.0f, row_min.y));
  if (ImGui::InvisibleButton("##visible", ImVec2(h, h))) {
    layer.visible = !layer.visible;
    push_undo(C, layer.visible ? "Show Layer" : "Hide Layer");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Show or hide the layer");
  }
  icon_eye(dl, ImVec2(right - h * 2.5f, cy), icon, layer.visible);
  ImGui::SetCursorScreenPos(ImVec2(right - h * 2.0f, row_min.y));
  if (ImGui::InvisibleButton("##render", ImVec2(h, h))) {
    layer.render_visible = !layer.render_visible;
    push_undo(C, layer.render_visible ? "Show in Renders" : "Hide in Renders");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Show or hide the layer in renders");
  }
  icon_camera(dl, ImVec2(right - h * 1.5f, cy), icon, layer.render_visible);
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

/* The layers a drag moves: the dragged one, or, when it is picked (active or Ctrl/Shift
 * selected), every picked layer, top first; a layer inside a picked group goes with the group. */
static std::vector<uint64_t> dragged_layers(Document &doc, uint64_t dragged)
{
  const Layer *dragged_layer = layer_find(doc, dragged);
  if (!dragged_layer || !(dragged_layer->selected || dragged == doc.active_layer)) {
    return {dragged};
  }
  const auto picked = [&](const Layer &l) { return l.selected || l.id == doc.active_layer; };
  std::vector<uint64_t> ids;
  for (const Layer *layer : document_layers_flat(doc)) {
    if (!picked(*layer)) {
      continue;
    }
    bool inside_picked = false;
    for (const Layer *p = layer_parent(doc, layer->id); p; p = layer_parent(doc, p->id)) {
      inside_picked = inside_picked || picked(*p);
    }
    if (!inside_picked) {
      ids.push_back(layer->id);
    }
  }
  return ids;
}

/* Moves layers (top first) above (-1) or below (+1) `target`, into it (0, a group), or to the
 * bottom of the top level (target 0), keeping their order. */
static bool move_layers(Document &doc, const std::vector<uint64_t> &ids, uint64_t target, int where)
{
  for (const uint64_t id : ids) {
    if (id == target) {
      return false;
    }
    for (const Layer *p = target ? layer_parent(doc, target) : nullptr; p; p = layer_parent(doc, p->id)) {
      if (p->id == id) {
        return false; /* A group can't go inside itself. */
      }
    }
  }
  if (where == 0 && (!layer_find(doc, target) || !layer_find(doc, target)->is_group())) {
    return false;
  }
  std::vector<Layer> taken;
  for (const uint64_t id : ids) {
    int index = 0;
    if (std::vector<Layer> *container = layer_container(doc, id, &index)) {
      taken.push_back(std::move((*container)[index]));
      container->erase(container->begin() + index);
    }
  }
  std::vector<Layer> *destination = &doc.layers;
  int index = int(doc.layers.size());
  if (where == 0) {
    destination = &layer_find(doc, target)->children;
    index = 0;
  }
  else if (target) {
    int target_index = 0;
    destination = layer_container(doc, target, &target_index);
    index = where < 0 ? target_index : target_index + 1;
  }
  for (Layer &layer : taken) {
    destination->insert(destination->begin() + index++, std::move(layer));
  }
  return !taken.empty();
}

static void apply_drop(Context &C, const DropRequest &drop)
{
  Document &doc = *C.document;
  if (!drop.moving || drop.moving == drop.target) {
    return;
  }
  if (move_layers(doc, dragged_layers(doc, drop.moving), drop.target, drop.where)) {
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
    menu_op(C, "Bone Layer", "layer.new_bone");
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
      /* Empty space below the list: to the bottom, shown by a line under the last layer. */
      const ImVec2 area_min = ImGui::GetItemRectMin(), area_max = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddLine(ImVec2(area_min.x, area_min.y + 1.0f), ImVec2(area_max.x, area_min.y + 1.0f),
                                          COL_REFERENCE, 2.0f);
      if (const ImGuiPayload *payload =
              ImGui::AcceptDragDropPayload(LAYER_PAYLOAD, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        uint64_t moving;
        std::memcpy(&moving, payload->Data, sizeof(moving));
        if (move_layers(doc, dragged_layers(doc, moving), 0, 1)) {
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
