/* The Layers editor: Moho's layer panel with Blender outliner habits. A tree of layers (top of
 * the stack first) with visibility and lock toggles, label colours, type icons, rename on
 * double-click, drag to reorder or drop into a group, and a right-click menu. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "anim/switch.hh"
#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <cstring>

#include "anim/draw_order.hh"
#include "anim/reference.hh"
#include "assets/asset_library.hh"
#include "data/camera.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "data/media_layer.hh"
#include "editors/editors.hh"
#include "editors/ui.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"
#include "wm/keymap.hh"

namespace oanim {

static constexpr const char *LAYER_PAYLOAD = "OANIM_LAYER";
/* A drawing layer dragged in the draw-order view. */
static constexpr const char *ORDER_PAYLOAD = "OANIM_DRAW_ORDER";
/* Draw Order markers and the draw-order view's insertion line. */
static constexpr ImU32 COL_ORDER = IM_COL32(0xf0, 0xb0, 0x40, 255);

/* An arrow up (in front of a layer) or down (behind one). */
static void icon_order(ImDrawList *dl, ImVec2 c, float s, bool front, ImU32 col)
{
  const float d = front ? -1.0f : 1.0f;
  dl->AddTriangleFilled(ImVec2(c.x, c.y + d * s), ImVec2(c.x + s * 0.8f, c.y - d * s * 0.1f),
                        ImVec2(c.x - s * 0.8f, c.y - d * s * 0.1f), col);
  dl->AddRectFilled(ImVec2(c.x - s * 0.28f, c.y - d * s * 0.1f), ImVec2(c.x + s * 0.28f, c.y - d * s * 0.9f), col);
}

/* A layer drawn out of its Layers-editor place right now: its arrow, and on hover what it's in front
 * of or behind; clicking selects that layer. `x`: where it goes. */
static void order_marker(Context &C, const Layer &layer, float x, float y, float h)
{
  if (layer.draw_order == 0.0f) {
    return;
  }
  Document &doc = *C.document;
  const DrawOrderRule rule = draw_order_rule(layer.draw_order);
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  ImGui::PushID("##order_marker");
  const bool clicked = ImGui::InvisibleButton("##order", ImVec2(h * 0.8f, h));
  const bool over = ImGui::IsItemHovered();
  ImGui::PopID();
  const Layer *target = layer_find(doc, rule.target);
  icon_order(ImGui::GetWindowDrawList(), ImVec2(x + h * 0.4f, y + h * 0.5f), h * 0.22f,
             rule.mode == DrawOrderMode::Front, target ? (over ? IM_COL32(0xff, 0xd0, 0x70, 255) : COL_ORDER)
                                                       : IM_COL32(0xe0, 0x50, 0x50, 255));
  if (over) {
    std::string tip = "Drawn " + draw_order_label(doc, layer.draw_order) + " on this frame (Draw Order)";
    if (const Layer *faded = draw_order_faded_group(doc, layer.id); faded && target && !layer_inside(doc, target->id, faded->id)) {
      tip += "\nMoves \"" + faded->name + "\" as a whole: it's faded, so it's drawn as one image";
    }
    tip += target ? "\nClick to select " + target->name : "\nThat layer is gone, so it's drawn as Normal";
    ImGui::SetTooltip("%s", tip.c_str());
  }
  if (clicked && target) {
    doc.active_layer = target->id;
    C.undo->push("Active Layer", doc);
  }
}
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

/* Blender's render toggle: a small camera while the layer renders; off, it is a guide, shown as a
 * dashed frame in the guides' light blue (brighter while the viewport hides guides, saying why the
 * layer isn't there). */
static constexpr ImU32 COL_GUIDE = IM_COL32(0x86, 0xc4, 0xd6, 255);
static void icon_camera(ImDrawList *dl, ImVec2 c, float s, bool on, bool guides_hidden)
{
  if (!on) {
    const ImU32 col = guides_hidden ? IM_COL32(0xb8, 0xe6, 0xf2, 255) : COL_GUIDE;
    const ImVec2 a = c + ImVec2(-s * 0.85f, -s * 0.7f), b = c + ImVec2(s * 0.85f, s * 0.7f);
    /* Two dashes a side, corners left open. */
    for (const float u : {0.12f, 0.58f}) {
      const float v = u + 0.3f;
      dl->AddLine(ImVec2(a.x + (b.x - a.x) * u, a.y), ImVec2(a.x + (b.x - a.x) * v, a.y), col, 1.4f);
      dl->AddLine(ImVec2(a.x + (b.x - a.x) * u, b.y), ImVec2(a.x + (b.x - a.x) * v, b.y), col, 1.4f);
      dl->AddLine(ImVec2(a.x, a.y + (b.y - a.y) * u), ImVec2(a.x, a.y + (b.y - a.y) * v), col, 1.4f);
      dl->AddLine(ImVec2(b.x, a.y + (b.y - a.y) * u), ImVec2(b.x, a.y + (b.y - a.y) * v), col, 1.4f);
    }
    return;
  }
  const ImU32 col = COL_ICON;
  dl->AddRect(c + ImVec2(-s * 0.95f, -s * 0.5f), c + ImVec2(s * 0.35f, s * 0.6f), col, 1.5f, 0, 1.3f);
  dl->AddTriangleFilled(c + ImVec2(s * 0.35f, s * 0.05f), c + ImVec2(s * 0.95f, -s * 0.4f),
                        c + ImVec2(s * 0.95f, s * 0.5f), col);
  dl->AddRectFilled(c + ImVec2(-s * 0.75f, -s * 0.85f), c + ImVec2(-s * 0.2f, -s * 0.5f), col, 1.0f);
}

static void icon_lock(ImDrawList *dl, ImVec2 c, float s, bool locked)
{
  const ImU32 col = locked ? COL_ICON : COL_ICON_OFF;
  dl->AddRectFilled(c + ImVec2(-s * 0.6f, -s * 0.1f), c + ImVec2(s * 0.6f, s * 0.75f), col, 1.0f);
  const float shift = locked ? 0.0f : s * 0.45f;
  dl->PathArcTo(c + ImVec2(shift, -s * 0.15f), s * 0.38f, PI, 2.0f * PI, 8);
  dl->PathStroke(col, 0, 1.3f);
}

static void icon_gear(ImDrawList *dl, ImVec2 c, float s, ImU32 col)
{
  for (int i = 0; i < 8; i++) {
    const float a = float(i) * 0.785398f;
    const ImVec2 way(std::cos(a), std::sin(a));
    dl->AddLine(c + way * (s * 0.7f), c + way * (s * 1.05f), col, 2.0f);
  }
  dl->AddCircle(c, s * 0.55f, col, 16, 1.6f);
}

static void icon_layer_type(ImDrawList *dl, ImVec2 c, float s, const Layer &layer)
{
  const ImU32 col = layer.is_reference() ? COL_REFERENCE : COL_ICON;
  if (layer.is_media()) {
    /* What it holds: a picture (a frame with a hill), film (a frame with sprocket holes) or sound
     * (a speaker); a red mark when a file is missing. */
    const MediaKind kind = layer.media.clips.empty() ? MediaKind::Image : media_clip_info(layer.media.clips[0]).kind;
    if (kind == MediaKind::Audio) {
      dl->AddRectFilled(c + ImVec2(-s * 0.8f, -s * 0.3f), c + ImVec2(-s * 0.35f, s * 0.3f), col, 1.0f);
      dl->AddTriangleFilled(c + ImVec2(-s * 0.4f, -s * 0.3f), c + ImVec2(s * 0.15f, -s * 0.8f),
                            c + ImVec2(s * 0.15f, s * 0.8f), col);
      dl->AddTriangleFilled(c + ImVec2(-s * 0.4f, -s * 0.3f), c + ImVec2(s * 0.15f, s * 0.8f),
                            c + ImVec2(-s * 0.4f, s * 0.3f), col);
      dl->PathArcTo(c + ImVec2(s * 0.2f, 0.0f), s * 0.55f, -0.9f, 0.9f, 8);
      dl->PathStroke(col, 0, 1.3f);
    }
    else {
      dl->AddRect(c + ImVec2(-s * 0.85f, -s * 0.65f), c + ImVec2(s * 0.85f, s * 0.65f), col, 1.5f, 0, 1.3f);
      if (kind == MediaKind::Video || kind == MediaKind::Sequence) {
        for (int i = 0; i < 3; i++) {
          const float x = -s * 0.5f + float(i) * s * 0.5f;
          dl->AddRectFilled(c + ImVec2(x - s * 0.12f, -s * 0.5f), c + ImVec2(x + s * 0.12f, -s * 0.3f), col);
          dl->AddRectFilled(c + ImVec2(x - s * 0.12f, s * 0.3f), c + ImVec2(x + s * 0.12f, s * 0.5f), col);
        }
      }
      else {
        dl->AddTriangleFilled(c + ImVec2(-s * 0.7f, s * 0.5f), c + ImVec2(-s * 0.15f, -s * 0.15f),
                              c + ImVec2(s * 0.35f, s * 0.5f), col);
        dl->AddCircleFilled(c + ImVec2(s * 0.4f, -s * 0.25f), s * 0.16f, col);
      }
    }
    if (std::any_of(layer.media.clips.begin(), layer.media.clips.end(), media_clip_missing)) {
      dl->AddCircleFilled(c + ImVec2(s * 0.7f, s * 0.55f), s * 0.42f, IM_COL32(0xe0, 0x40, 0x40, 255), 12);
      dl->AddLine(c + ImVec2(s * 0.7f, s * 0.32f), c + ImVec2(s * 0.7f, s * 0.6f), IM_COL32_WHITE, 1.4f);
      dl->AddCircleFilled(c + ImVec2(s * 0.7f, s * 0.75f), 1.1f, IM_COL32_WHITE);
    }
    return;
  }
  if (layer.is_text()) {
    /* A serif T, as Blender's text object icon. */
    dl->AddLine(c + ImVec2(-s * 0.7f, -s * 0.65f), c + ImVec2(s * 0.7f, -s * 0.65f), col, 1.8f);
    dl->AddLine(c + ImVec2(-s * 0.7f, -s * 0.75f), c + ImVec2(-s * 0.7f, -s * 0.4f), col, 1.3f);
    dl->AddLine(c + ImVec2(s * 0.7f, -s * 0.75f), c + ImVec2(s * 0.7f, -s * 0.4f), col, 1.3f);
    dl->AddLine(c + ImVec2(0.0f, -s * 0.65f), c + ImVec2(0.0f, s * 0.7f), col, 1.8f);
    dl->AddLine(c + ImVec2(-s * 0.35f, s * 0.7f), c + ImVec2(s * 0.35f, s * 0.7f), col, 1.3f);
    return;
  }
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


static bool menu_op(Context &C, const char *label, const char *idname, const OpArgs &args = {})
{
  const OperatorType *ot = operators().find(idname);
  if (!ot) {
    return false;
  }
  const std::string shortcut = keymaps().shortcut_for(idname);
  if (ui_menu_item(label, shortcut.empty() ? nullptr : shortcut.c_str(), false, operator_poll(C, *ot))) {
    operator_call(C, idname, args);
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
  /* Alt held: a mask row of the dragged layer on the target instead of moving it (Add; Shift:
   * Subtract). */
  bool mask = false;
  bool subtract = false;
};

/* A small circle, its left half filled (inside a layer) or its right half (outside it). */
static void icon_mask(ImDrawList *dl, ImVec2 c, float r, bool outside, ImU32 col)
{
  dl->AddCircle(c, r, col, 16, 1.2f);
  const float a0 = outside ? -PI * 0.5f : PI * 0.5f;
  dl->PathArcTo(c, r, a0, a0 + PI, 12);
  dl->PathFillConvex(col);
}

/* A masked layer: one mask symbol after its name, no text (the Masking tab has the rest).
 * Hovering lists its masks and outlines their layers in the viewport; clicking opens the Masking
 * tab on it. Returns where it ends. */
static float mask_symbol(Context &C, Layer &layer, float x, float y, float h, float right)
{
  if (layer.masks.empty()) {
    return x;
  }
  Document &doc = *C.document;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float fs = ImGui::GetFontSize();
  const float w = fs * 1.1f;
  if (x + w > right) {
    return x;
  }
  bool any_on = false;
  for (const LayerMask &m : layer.masks) {
    any_on |= m.enabled;
  }
  const ImVec2 a(x, y), b(x + w, y + h);
  ImGui::SetCursorScreenPos(a);
  const bool clicked = ImGui::InvisibleButton("##mask_symbol", b - a);
  const bool over = ImGui::IsItemHovered();
  /* The mask orange (as the viewport's mask outlines); grey with every mask off. */
  const ImU32 col = !any_on ? IM_COL32(0x80, 0x80, 0x80, 255)
                    : over  ? IM_COL32(0xff, 0xc0, 0x80, 255)
                            : IM_COL32(0xff, 0x9a, 0x3c, 255);
  icon_mask(dl, ImVec2(x + w * 0.5f, y + h * 0.5f), fs * 0.3f, layer.masks.size() == 1 && mask_cuts(layer.masks[0]),
            col);
  if (over) {
    std::string tip;
    for (const LayerMask &m : layer.masks) {
      const std::vector<uint64_t> named = mask_card_layers(m);
      C.wm->mask_hover.insert(C.wm->mask_hover.end(), named.begin(), named.end());
      tip += mask_label(doc, m) + (m.enabled ? "" : " (off)") + "\n";
    }
    tip += mask_summary(doc, layer) + "\nClick: open the Masking tab";
    ImGui::SetTooltip("%s", tip.c_str());
  }
  if (clicked) {
    for (const Layer *l : document_layers_flat(doc)) {
      layer_find(doc, l->id)->selected = l->id == layer.id;
    }
    doc.active_layer = layer.id;
    C.wm->properties_tab_request = 6; /* The Masking tab. */
  }
  return x + w + 4.0f;
}

/* Moho's layer right-click menu: Group Mask on groups, Masking on layers inside a group, then
 * Group With Selection, Duplicate, Reference and Delete. Rename is a double-click or F2; label
 * colours and Break Reference are in the Properties editor's Layer tab. */
static void layer_context_menu(Context &C, Layer &layer)
{
  Document &doc = *C.document;
  doc.active_layer = layer.id;
  if (ui_submenu_begin("Mask")) {
    ui_mask_menu_items(C, 0);
    ui_submenu_end();
  }
  if (ui_submenu_begin("Draw Order")) {
    ui_draw_order_menu_items(C, 0);
    ui_submenu_end();
  }
  if (const Layer *sw = switch_of(doc, layer.id)) {
    if (ui_submenu_begin(("Switch: " + sw->name).c_str())) {
      ui_switch_menu_items(C, sw->id);
      ui_submenu_end();
    }
  }
  ImGui::Separator();
  menu_op(C, "Group With Selection", "layer.group");
  menu_op(C, "Convert to Switch", "layer.convert_to_switch");
  {
    int switches = 0;
    for (const Layer *l : document_layers_flat(doc)) {
      switches += l->is_switch() && (l->selected || l->id == doc.active_layer) && !l->switch_link ? 1 : 0;
    }
    if (switches >= 2) {
      menu_op(C, "Link Switches", "layer.switch_link");
    }
  }
  if (layer.has_drawing() && layer_armature(doc, layer.id)) {
    menu_op(C, "Select Bones This Drawing Uses", "armature.select_used_bones");
  }
  if (layer.type == LayerType::Group) {
    menu_op(C, "Convert to Bone Group", "layer.convert_to_bone");
  }
  else if (layer.is_bone_layer()) {
    menu_op(C, "Convert to Group", "layer.convert_to_group");
  }
  menu_op(C, "Duplicate Layer", "layer.duplicate");
  menu_op(C, "Reference Layer", "layer.make_reference");
  ImGui::Separator();
  menu_op(C, "Symmetrize + to -", "layer.symmetrize", {{"direction", "POSITIVE_TO_NEGATIVE"}});
  menu_op(C, "Symmetrize - to +", "layer.symmetrize", {{"direction", "NEGATIVE_TO_POSITIVE"}});
  ImGui::Separator();
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

/* The rows as the list shows them, top first: a group's layers while it's open. */
static void shown_rows(std::vector<Layer> &layers, std::vector<Layer *> &r_rows)
{
  for (Layer &layer : layers) {
    r_rows.push_back(&layer);
    if (layer.is_group() && layer.expanded) {
      shown_rows(layer.children, r_rows);
    }
  }
}

/* The camera, pinned above the layers (it isn't one of them: it can't be moved, hidden or locked).
 * A click picks it, as clicking its frame in the viewport does; Ctrl or Shift click toggles it. */
static void camera_row(Context &C)
{
  Document &doc = *C.document;
  Layer &camera = document_camera(doc);
  const float h = ImGui::GetFrameHeight();
  const ImVec2 row_min = ImGui::GetCursorScreenPos();
  const float row_w = ImGui::GetContentRegionAvail().x;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImGui::PushID("##camera_row");
  ImGui::InvisibleButton("##row", ImVec2(row_w, h));
  const bool hovered = ImGui::IsItemHovered();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    const ImGuiIO &io = ImGui::GetIO();
    camera.selected = io.KeyCtrl || io.KeyShift ? !camera.selected : true;
  }
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    /* Its settings: the Camera tab. */
    C.wm->properties_tab_request = 7;
  }
  if (hovered) {
    ImGui::SetTooltip("The camera: what renders show. Picked, the viewport looks through it");
  }
  if (camera.selected) {
    dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), COL_ACTIVE_ROW, 3.0f);
  }
  else if (hovered) {
    dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), IM_COL32(0xff, 0xff, 0xff, 14), 3.0f);
  }
  const float icon = h * 0.32f;
  const float cy = row_min.y + h * 0.5f;
  const float x = row_min.x + 8.0f + h * 0.7f;
  icon_camera(dl, ImVec2(x + h * 0.4f, cy), icon * 1.3f, true, false);
  dl->AddText(ImVec2(x + h * 0.9f, cy - ImGui::GetFontSize() * 0.5f), IM_COL32(0xe6, 0xe6, 0xe6, 255), "Camera");
  ImGui::PopID();
  /* A line between it and the layers. */
  const float y = row_min.y + h + 1.0f;
  dl->AddLine(ImVec2(row_min.x, y), ImVec2(row_min.x + row_w, y), IM_COL32(0xff, 0xff, 0xff, 24), 1.0f);
  ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + h + 3.0f));
}

static void layer_row(Context &C, Area &area, Layer &layer, int depth, DropRequest &drop,
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

  /* Show Active (.): scroll so this row is in the middle, and flash it. */
  if (C.wm->layers_show_active == layer.id && area.show_active_seen != C.wm->layers_show_active_serial) {
    area.show_active_seen = C.wm->layers_show_active_serial;
    area.show_active_layer = layer.id;
    area.show_active_time = ImGui::GetTime();
    area.show_active_frames = 4;
    area.show_active_only_if_hidden = false;
  }
  if (area.show_active_layer == layer.id && area.show_active_frames > 0) {
    area.show_active_frames--;
    const float top = ImGui::GetWindowPos().y, bottom = top + ImGui::GetWindowHeight();
    if (row_min.y < top || row_min.y + h > bottom) {
      /* Out of sight: on to the middle like Show Active, the list's height settling over these frames. */
      area.show_active_only_if_hidden = false;
    }
    if (!area.show_active_only_if_hidden) {
      /* By its own place: SetScrollHereY goes by the last row drawn, and the rows out of sight above it aren't. */
      ImGui::SetScrollFromPosY(row_min.y + h * 0.5f - top, 0.5f);
    }
  }

  /* Scrolled out of sight: only its place is kept (a long list otherwise costs every frame), unless
   * it is being dragged, renamed or has its menu open. */
  const ImGuiPayload *carried = ImGui::GetDragDropPayload();
  const bool carrying_this = carried && carried->IsDataType(LAYER_PAYLOAD) &&
                             std::memcmp(carried->Data, &layer.id, sizeof(layer.id)) == 0;
  if (!ImGui::IsRectVisible(row_min, row_min + ImVec2(row_w, h)) && !carrying_this && area.renaming_layer != layer.id &&
      g_narrow_on_release != layer.id && !ImGui::IsPopupOpen("##layer_menu"))
  {
    ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + h));
    ImGui::PopID();
    if (layer.is_group() && layer.expanded) {
      for (Layer &child : layer.children) {
        layer_row(C, area, child, depth + 1, drop, r_context_open);
      }
    }
    return;
  }

  /* The row itself: click to make active, drag to move, double-click to rename. */
  ImGui::SetNextItemAllowOverlap();
  ImGui::InvisibleButton("##row", ImVec2(row_w, h));
  const bool row_hovered = ImGui::IsItemHovered();
  /* Alt over the line under a row (its lower part): clicking clips it to the layer below, or
   * releases it (Photoshop's Alt-click between layers). */
  const bool clip_zone = row_hovered && ImGui::GetIO().KeyAlt && !ImGui::GetDragDropPayload() &&
                         ImGui::GetIO().MousePos.y > row_min.y + h * 0.6f &&
                         (layer.clip_to_below || layer_clip_base(doc, layer));
  if (clip_zone) {
    const float line_y = row_min.y + h;
    dl->AddLine(ImVec2(row_min.x, line_y), ImVec2(row_min.x + row_w, line_y), IM_COL32(0xff, 0x9a, 0x3c, 255), 2.0f);
    ImGui::SetTooltip(layer.clip_to_below ? "Release %s's clip" : "Clip %s to the layer below", layer.name.c_str());
  }
  if (clip_zone && ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    operator_call(C, "mask.clip_to_below", {{"layer", std::to_string(layer.id)}});
  }
  else if (row_hovered && C.wm->draw_order_pick.mode != 0) {
    /* In Front Of... / Behind...: pointing at a row shows the result in the viewport. */
    C.wm->draw_order_pick.hover = layer.id;
  }
  if (clip_zone) {
    /* The click was the clip's. */
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ui_layer_eyedropper_active()) {
    /* The layer eyedropper takes the click: this layer becomes the bone's custom shape. */
    ui_layer_eyedropper_pick(C, layer.id);
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl &&
           layer_find(doc, doc.active_layer))
  {
    /* Shift click picks the rows from the active layer to this one, as Blender's Outliner and file
     * lists do; the active layer stays, so the next Shift click ranges from it too. */
    std::vector<Layer *> rows;
    shown_rows(doc.layers, rows);
    const auto at = [&](uint64_t id) {
      return int(std::find_if(rows.begin(), rows.end(), [&](const Layer *l) { return l->id == id; }) - rows.begin());
    };
    const int from = at(doc.active_layer), to = at(layer.id);
    layers_deselect(doc.layers);
    if (from < int(rows.size())) {
      for (int i = std::min(from, to); i <= std::max(from, to); i++) {
        rows[size_t(i)]->selected = true;
      }
    }
    else {
      /* The active layer is in a closed group: just this one joins it. */
      layer.selected = true;
    }
    doc.camera.selected = false;
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && ImGui::GetIO().KeyCtrl &&
           (layer.selected || doc.active_layer == layer.id))
  {
    /* Ctrl click on a picked layer unpicks just it; the active one hands that to another picked
     * layer, if there is one. */
    layer.selected = false;
    if (doc.active_layer == layer.id) {
      for (const Layer *l : document_layers_flat(doc)) {
        if (l->selected && l->id != layer.id) {
          doc.active_layer = l->id;
          layer_find(doc, l->id)->selected = false;
          break;
        }
      }
      if (doc.active_layer == layer.id) {
        /* The only one picked: it stays (a layer is always active). */
        layer.selected = false;
      }
    }
  }
  else if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    /* Ctrl click adds this layer to the picked ones (it becomes active); a plain click picks just
     * this one. */
    const ImGuiIO &io = ImGui::GetIO();
    if (io.KeyCtrl) {
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
    if (!io.KeyCtrl) {
      doc.camera.selected = false;
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
  if (ImGui::BeginDragDropTarget() && ImGui::GetIO().KeyAlt) {
    /* Alt: a mask row of the dragged layer on this one (Add; Shift: Subtract). */
    const ImGuiPayload *peek = ImGui::GetDragDropPayload();
    uint64_t moving = 0;
    if (peek && peek->IsDataType(LAYER_PAYLOAD)) {
      std::memcpy(&moving, peek->Data, sizeof(moving));
    }
    const Layer *source = layer_find(doc, moving);
    const bool subtract = ImGui::GetIO().KeyShift;
    dl->AddRect(row_min, row_min + ImVec2(row_w, h), IM_COL32(0xff, 0x9a, 0x3c, 255), 3.0f, 0, 2.0f);
    if (source && source->id != layer.id) {
      C.wm->mask_hover.push_back(source->id);
      ImGui::SetTooltip(subtract ? "Mask %s: Subtract %s (cut it out)" : "Mask %s: Add %s (show inside it)%s",
                        layer.name.c_str(), source->name.c_str(), subtract ? "" : "\nShift: Subtract");
    }
    if (ImGui::AcceptDragDropPayload(LAYER_PAYLOAD, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
      drop.moving = moving;
      drop.target = layer.id;
      drop.mask = true;
      drop.subtract = subtract;
    }
    ImGui::EndDragDropTarget();
  }
  else if (ImGui::BeginDragDropTarget()) {
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
  if (ui_menu_begin("##layer_menu")) {
    r_context_open = true;
    layer_context_menu(C, layer);
    ui_menu_end();
  }

  /* The active layer is a copy (reference layer): its original stands out, outlined in the
   * reference colour (or the folded group it is in). */
  const Layer *active_layer = document_active_layer(doc);
  const uint64_t original = active_layer && active_layer->is_reference() ? active_layer->reference_source : 0;
  const auto holds = [&](auto &&self, const Layer &l) -> bool {
    for (const Layer &child : l.children) {
      if (child.id == original || self(self, child)) {
        return true;
      }
    }
    return false;
  };
  if (original != 0 && (layer.id == original || (layer.is_group() && !layer.expanded && holds(holds, layer)))) {
    dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), IM_COL32(0x6f, 0xb8, 0xff, 40), 3.0f);
    dl->AddRect(row_min, row_min + ImVec2(row_w, h), COL_REFERENCE, 3.0f, 0, 1.5f);
    if (row_hovered) {
      ImGui::SetTooltip(layer.id == original ? "The original of the selected copy" : "Holds the original of the selected copy");
    }
  }
  /* A layer named by the mask field under the mouse in Properties (or the folded group it's in). */
  const std::vector<uint64_t> &hover = C.wm->layers_hover;
  if (C.wm->layers_hover_frame >= ImGui::GetFrameCount() - 1 && !hover.empty()) {
    const auto hovered = [&](uint64_t id) { return std::find(hover.begin(), hover.end(), id) != hover.end(); };
    const auto holds_hovered = [&](auto &&self, const Layer &l) -> bool {
      for (const Layer &child : l.children) {
        if (hovered(child.id) || self(self, child)) {
          return true;
        }
      }
      return false;
    };
    if (hovered(layer.id) || (layer.is_group() && !layer.expanded && holds_hovered(holds_hovered, layer))) {
      dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), IM_COL32(0xff, 0x9a, 0x3c, 40), 3.0f);
      dl->AddRect(row_min, row_min + ImVec2(row_w, h), IM_COL32(0xff, 0x9a, 0x3c, 255), 3.0f, 0, 1.5f);
    }
  }
  if (doc.active_layer == layer.id) {
    /* While the camera is picked the active layer waits, shown lighter. */
    dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), doc.camera.selected ? COL_SELECTED_ROW : COL_ACTIVE_ROW, 3.0f);
  }
  /* Just shown with Show Active: a white outline fading over a second. */
  if (area.show_active_layer == layer.id) {
    const float fade = 1.0f - float(ImGui::GetTime() - area.show_active_time);
    if (fade > 0.0f) {
      dl->AddRect(row_min, row_min + ImVec2(row_w, h), IM_COL32(0xff, 0xff, 0xff, int(220 * fade)), 3.0f, 0, 2.0f);
    }
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
  if (layer.clip_to_below && layer_clip_base(doc, layer)) {
    /* Clipped: indented, with a hook down to the layer it clips to (Photoshop's). */
    const ImU32 hook = IM_COL32(0xff, 0x9a, 0x3c, 255);
    const float hx = x + h * 0.2f;
    dl->AddLine(ImVec2(hx + icon * 0.9f, cy - icon * 0.5f), ImVec2(hx, cy - icon * 0.5f), hook, 1.5f);
    dl->AddLine(ImVec2(hx, cy - icon * 0.5f), ImVec2(hx, row_min.y + h - icon * 0.3f), hook, 1.5f);
    dl->AddTriangleFilled(ImVec2(hx - icon * 0.4f, row_min.y + h - icon * 0.6f),
                          ImVec2(hx + icon * 0.4f, row_min.y + h - icon * 0.6f), ImVec2(hx, row_min.y + h), hook);
    x += h * 0.6f;
  }
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
  if (const Layer *sw = layer_parent(doc, layer.id); sw && sw->is_switch()) {
    /* Rive's radio buttons: filled for the child the switch shows (as renders show it), a ring for the
     * others; one shown only for editing gets an orange ring. A click picks it, keyed at this frame
     * (Alt: for the whole hold); the eye stays the viewport's own. */
    const bool picked = sw->switch_shown == layer.id;
    const bool peeked = !picked && !layer.edit_out;
    const ImVec2 c(x + h * 0.3f, cy);
    ImGui::SetCursorScreenPos(ImVec2(x, row_min.y));
    const uint64_t sw_id = sw->id;
    const std::string name = layer.name;
    if (ImGui::InvisibleButton("##switch_pick", ImVec2(h * 0.6f, h))) {
      operator_call(C, "layer.switch_set",
                    {{"layer", std::to_string(sw_id)}, {"child", name}, {"hold", ImGui::GetIO().KeyAlt ? "1" : "0"}});
    }
    const bool dot_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(picked   ? "Shown by the switch now.\nClick another child's dot to show that one instead"
                        : peeked ? "Shown while you edit it, not keyed.\nClick to show it here (keyed at this frame; "
                                   "Alt: the whole hold)"
                                 : "Click to show this child here (keyed at this frame; Alt: the whole hold)");
    }
    const ImU32 ring = peeked ? IM_COL32(0xff, 0x9a, 0x3c, 255) : dot_hovered ? IM_COL32(0xf0, 0xf0, 0xf0, 255) : COL_ICON;
    if (picked) {
      dl->AddCircleFilled(c, icon * 0.75f, IM_COL32(0xe6, 0xe6, 0xe6, 255));
    }
    else {
      dl->AddCircle(c, icon * 0.7f, ring, 0, 1.5f);
    }
    x += h * 0.6f;
  }
  icon_layer_type(dl, ImVec2(x + h * 0.4f, cy), icon * 1.3f, layer);
  if (layer.is_reference() && reference_change_count(layer) > 0) {
    /* A copy with changes of its own (the Reference tab lists them). */
    dl->AddCircleFilled(ImVec2(x + h * 0.4f + icon * 1.1f, cy - icon * 0.9f), icon * 0.32f, IM_COL32(0x6b, 0xdb, 0xd1, 255));
  }
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
    /* Faded while hidden: by its eye, or as a guide while the viewport hides guides. */
    const bool guide_hidden = !layer.render_visible && !(doc.overlays.show && doc.overlays.guides);
    const ImU32 text = layer.shown() && !guide_hidden ? IM_COL32(0xe6, 0xe6, 0xe6, 255) : IM_COL32(0x80, 0x80, 0x80, 255);
    dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), text, layer.name.c_str());
    float after = x + ImGui::CalcTextSize(layer.name.c_str()).x + 8.0f;
    const float limit = row_min.x + row_w - h * 4.0f;
    if (layer.is_switch()) {
      const std::string pick = switch_value_label(layer, float(layer.switch_shown));
      if (after + ImGui::CalcTextSize(pick.c_str()).x < limit) {
        dl->AddText(ImVec2(after, cy - ImGui::GetFontSize() * 0.5f), IM_COL32(0x90, 0x90, 0x90, 255), pick.c_str());
        after += ImGui::CalcTextSize(pick.c_str()).x + 8.0f;
      }
      if (const SwitchLink *link = switch_link_of(doc, layer); link && after + h < limit) {
        /* Linked by name: a chain (teal); a click selects its partners. A warning beside it when its keys
         * pick names it has no drawing for. */
        const ImVec2 c(after + h * 0.4f, cy);
        const ImU32 teal = IM_COL32(0x4f, 0xc8, 0xc8, 255);
        dl->AddCircle(c - ImVec2(icon * 0.45f, 0), icon * 0.55f, teal, 0, 1.6f);
        dl->AddCircle(c + ImVec2(icon * 0.45f, 0), icon * 0.55f, teal, 0, 1.6f);
        ImGui::SetCursorScreenPos(ImVec2(after, row_min.y));
        const uint64_t link_id = link->id;
        if (ImGui::InvisibleButton("##link", ImVec2(h * 0.8f, h))) {
          for (Layer *m : switch_link_members(doc, link_id)) {
            m->selected = m->id != doc.active_layer;
          }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          std::string with;
          for (const Layer *m : switch_link_members(doc, link_id)) {
            const Layer *p = layer_parent(doc, m->id);
            with += "\n    " + (p ? p->name + " > " : std::string()) + m->name;
          }
          ImGui::SetTooltip("Linked by name with:%s\nPicking a drawing in one picks the same-named drawing in all.\n"
                            "Click to select them; the Switch menu has the Link Report and Unlink",
                            with.c_str());
        }
        after += h * 0.8f;
        const std::vector<std::string> lacks = switch_link_lacks(doc, layer);
        if (!lacks.empty() && after + h < limit) {
          const ImVec2 w(after + h * 0.35f, cy);
          const float r = icon * 0.75f;
          dl->AddTriangleFilled(w + ImVec2(0, -r), w + ImVec2(r, r * 0.8f), w + ImVec2(-r, r * 0.8f), IM_COL32(0xff, 0x9a, 0x3c, 255));
          ImGui::SetCursorScreenPos(ImVec2(after, row_min.y));
          ImGui::InvisibleButton("##lacks", ImVec2(h * 0.7f, h));
          if (ImGui::IsItemHovered()) {
            std::string names;
            for (const std::string &n : lacks) {
              names += (names.empty() ? "" : ", ") + n;
            }
            ImGui::SetTooltip("Its keys pick %s, which it has no drawing for: it keeps showing the last one it had.\n"
                              "Give a drawing that name as an alias (asset shelf: Aliases), or add one", names.c_str());
          }
          after += h * 0.7f;
        }
      }
    }
    mask_symbol(C, layer, after, row_min.y, h, limit);
  }

  /* Visibility, render and lock toggles on the right, as in Blender's outliner. */
  const float right = row_min.x + row_w - style.ItemSpacing.x;
  order_marker(C, layer, right - h * 3.85f, row_min.y, h);
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
    push_undo(C, layer.render_visible ? "Show in Renders" : "Make Guide");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(layer.render_visible
                          ? "Rendered.\nClick to make it a guide: shown while you work, hidden with the viewport's "
                            "overlays (Shift Z), never rendered"
                          : "Guide: shown while you work, hidden with the viewport's overlays (Shift Z), never "
                            "rendered.\nClick to render it");
  }
  icon_camera(dl, ImVec2(right - h * 1.5f, cy), icon, layer.render_visible,
              !(doc.overlays.show && doc.overlays.guides));
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
    for (Layer &child : layer.children) {
      layer_row(C, area, child, depth + 1, drop, r_context_open);
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
  if (drop.mask) {
    operator_call(C, "mask.add", {{"layer", std::to_string(drop.target)}, {"sources", std::to_string(drop.moving)},
                                  {"mode", drop.subtract ? "SUBTRACT" : "ADD"}});
    return;
  }
  if (move_layers(doc, dragged_layers(doc, drop.moving), drop.target, drop.where)) {
    doc.active_layer = drop.moving;
    C.undo->push("Move Layer", doc);
  }
}

/* The draw-order view: every drawing layer as drawn on this frame, top first, with its group. Drag
 * one onto another to draw it in front of (upper half) or behind (lower half) it: keyed on frames
 * other than 0, the setup order on frame 0. */
static void draw_order_view(Context &C)
{
  Document &doc = *C.document;
  const float h = ImGui::GetFrameHeight();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const std::vector<const Layer *> order = draw_order_layers(doc, [](const Layer &) { return true; });
  uint64_t moving = 0, target = 0;
  int where = 0;
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const Layer &layer = **it;
    const ImVec2 row_min = ImGui::GetCursorScreenPos();
    const float row_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(int(layer.id));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##order_row", ImVec2(row_w, h));
    const bool row_hovered = ImGui::IsItemHovered();
    if (row_hovered && C.wm->draw_order_pick.mode != 0) {
      C.wm->draw_order_pick.hover = layer.id;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (ui_layer_eyedropper_active()) {
        ui_layer_eyedropper_pick(C, layer.id);
      }
      else {
        doc.active_layer = layer.id;
        C.undo->push("Active Layer", doc);
      }
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(ORDER_PAYLOAD, &layer.id, sizeof(layer.id));
      ImGui::Text("%s", layer.name.c_str());
      ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
      const bool upper = ImGui::GetIO().MousePos.y < row_min.y + h * 0.5f;
      const float line_y = upper ? row_min.y : row_min.y + h;
      dl->AddLine(ImVec2(row_min.x, line_y), ImVec2(row_min.x + row_w, line_y), COL_ORDER, 2.0f);
      if (const ImGuiPayload *payload =
              ImGui::AcceptDragDropPayload(ORDER_PAYLOAD, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        std::memcpy(&moving, payload->Data, sizeof(moving));
        target = layer.id;
        where = upper ? 1 : -1;
      }
      ImGui::EndDragDropTarget();
    }
    if (row_hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      doc.active_layer = layer.id;
      ImGui::OpenPopup("##order_menu");
    }
    if (ui_menu_begin("##order_menu")) {
      ImGui::TextDisabled("Draw Order: %s", layer.name.c_str());
      ui_draw_order_menu_items(C, 0);
      ui_menu_end();
    }
    if (doc.active_layer == layer.id) {
      dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), COL_ACTIVE_ROW, 3.0f);
    }
    else if (row_hovered) {
      dl->AddRectFilled(row_min, row_min + ImVec2(row_w, h), IM_COL32(0xff, 0xff, 0xff, 14), 3.0f);
    }
    const float cy = row_min.y + h * 0.5f;
    float x = row_min.x + 8.0f;
    icon_layer_type(dl, ImVec2(x + h * 0.4f, cy), h * 0.32f * 1.3f, layer);
    x += h * 0.9f;
    const ImU32 text = layer.shown() ? IM_COL32(0xe6, 0xe6, 0xe6, 255) : IM_COL32(0x80, 0x80, 0x80, 255);
    dl->AddText(ImVec2(x, cy - ImGui::GetFontSize() * 0.5f), text, layer.name.c_str());
    if (const Layer *parent = layer_parent(doc, layer.id)) {
      const float name_w = ImGui::CalcTextSize(layer.name.c_str()).x;
      const std::string in = "in " + parent->name;
      dl->AddText(ImVec2(x + name_w + 8.0f, cy - ImGui::GetFontSize() * 0.5f), IM_COL32(0x80, 0x80, 0x80, 255), in.c_str());
    }
    order_marker(C, layer, row_min.x + row_w - h * 1.2f, row_min.y, h);
    ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + h));
    ImGui::PopID();
  }
  if (order.empty()) {
    ImGui::TextDisabled("No drawing layers.");
  }
  ImGui::Dummy(ImVec2(1, 4));
  ImGui::TextDisabled("Drag a layer onto another to draw it in front of\nor behind it (keyed on frames other than 0).");
  if (moving != 0 && target != 0 && moving != target) {
    operator_call(C, "layer.draw_order_set",
                  {{"layer", std::to_string(moving)}, {"mode", where > 0 ? "FRONT" : "BEHIND"},
                   {"target", std::to_string(target)}});
  }
}

/* Auto Focus: fold down to the selected layers when the selection changes, so a group opened by hand stays open till
 * then. It waits while the mouse is over the list or held, or a row is being renamed: rows moving under the mouse
 * would turn a second click or a drag into another layer's. */
static void auto_focus_update(Area &area, Context &C)
{
  const Document &doc = *C.document;
  std::vector<uint64_t> picked;
  if (doc.active_layer) {
    picked.push_back(doc.active_layer);
  }
  for (const Layer *layer : document_layers_flat(doc)) {
    if (layer->selected && layer->id != doc.active_layer) {
      picked.push_back(layer->id);
    }
  }
  if (picked == area.auto_focus_picked || picked.empty() || area.renaming_layer != 0 ||
      ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) || ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown() ||
      ImGui::GetDragDropPayload())
  {
    return;
  }
  area.auto_focus_picked = std::move(picked);
  operator_call(C, "layer.auto_focus");
  area.show_active_layer = doc.active_layer;
  area.show_active_frames = 4;
  area.show_active_only_if_hidden = true;
}

/* The settings button at the right of the header, and its popup. */
static void layers_settings_button(Area &area)
{
  const float size = ImGui::GetFrameHeight();
  ImGui::SameLine(0, 8);
  const float room = ImGui::GetContentRegionAvail().x - size;
  if (room > 0.0f) {
    ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos() + ImVec2(room, 0.0f));
  }
  Preferences &prefs = preferences();
  if (ImGui::Button("##layers_settings", ImVec2(size, size))) {
    ImGui::OpenPopup("##layers_settings_popup");
  }
  icon_gear(ImGui::GetWindowDrawList(), (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f, size * 0.3f,
            prefs.layers_auto_focus ? COL_REFERENCE : COL_ICON);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Layers editor settings");
  }
  if (ImGui::BeginPopup("##layers_settings_popup")) {
    if (ImGui::Checkbox("Auto Focus", &prefs.layers_auto_focus)) {
      preferences_save();
      /* Turned on: at once, whatever is selected already. */
      area.auto_focus_picked.clear();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Keep the groups folded except those holding the selected layers, and scroll to the active "
                        "layer. A group opened by hand stays open until the selection changes");
    }
    ImGui::EndPopup();
  }
}

void layers_draw(Area &area, Context &C)
{
  Document &doc = *C.document;
  ui_draw_order_pick_update(C);
  ui_mask_pick_update(C);
  area_editor_type_menu(area);
  /* M and Shift M over the Layers editor: the picked drawings' bone collections. */
  ui_bone_collection_menus(C, ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));

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
    menu_op(C, "Switch", "layer.new_switch");
    menu_op(C, "Bone Layer", "layer.new_bone");
    menu_op(C, "Text", "layer.new_text");
    menu_op(C, "Media...", "media.import");
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
  /* The draw-order view: this frame's draw order instead of the tree. */
  ImGui::SameLine(0, 8);
  if (area.layers_draw_order_view) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  const bool order_clicked = ImGui::Button("Order");
  if (area.layers_draw_order_view) {
    ImGui::PopStyleColor();
  }
  if (order_clicked) {
    area.layers_draw_order_view = !area.layers_draw_order_view;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Show the layers in this frame's draw order (Draw Order rules applied), top first: drag one "
                      "onto another to draw it in front of or behind it");
  }
  layers_settings_button(area);
  ImGui::Separator();

  if (area.layers_draw_order_view) {
    if (ImGui::BeginChild("##layers_order", ImVec2(0, 0))) {
      draw_order_view(C);
    }
    ImGui::EndChild();
    return;
  }

  if (preferences().layers_auto_focus) {
    auto_focus_update(area, C);
  }

  DropRequest drop;
  bool context_open = false;
  if (ImGui::BeginChild("##layers", ImVec2(0, 0))) {
    camera_row(C);
    for (Layer &layer : doc.layers) {
      layer_row(C, area, layer, 0, drop, context_open);
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

}  // namespace oanim
