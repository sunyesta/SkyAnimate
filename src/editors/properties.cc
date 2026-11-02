/* The Properties editor, like Blender's: tabs for the document, the active layer and its
 * drawing (shapes and their styles). A Modifiers tab arrives with the modifier stack. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <cmath>
#include <set>
#include <string>

#include "anim/animation.hh"
#include "anim/follow_point.hh"
#include "anim/deform.hh"
#include "anim/vector_deform.hh"
#include "anim/drivers.hh"
#include "anim/draw_order.hh"
#include "anim/switch.hh"
#include "anim/bone_family.hh"
#include "anim/merge.hh"
#include "anim/dynamics.hh"
#include "anim/reference.hh"
#include "core/log.hh"
#include "data/camera.hh"
#include "data/detail.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "data/media_layer.hh"
#include "editors/editors.hh"
#include "assets/brushes.hh"
#include "editors/brush_editor.hh"
#include "editors/ui.hh"
#include "editors/text_panel.hh"
#include "ops/edit_ops.hh"
#include "ops/media_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

enum PropertiesTab {
  TAB_DOCUMENT = 0,
  TAB_LAYER = 1,
  TAB_DRAWING = 2,
  TAB_MODIFIERS = 3,
  TAB_CONSTRAINTS = 4,
  TAB_REFERENCE = 5,
  TAB_MASKING = 6,
  TAB_CAMERA = 7,
  TAB_MEDIA = 8,
  TAB_TEXT = 9,
  TAB_SHAPE = 10,
  TAB_BONE = 11
};

static void key_next(Context &C, Layer &layer, std::vector<std::string> paths);
static int pin_state(Layer &layer, const std::vector<std::string> &paths, int key_state);
static void pin_toggle(Context &C, uint64_t layer, const std::string &paths, int pin);

static void document_tab(Context &C)
{
  const rna::PointerRNA ptr{C.document, &Document::rna()};
  const char *path = "oanim.data.document";
  if (ui_panel_begin("Document")) {
    ui_prop(C, ptr, "name", path);
    ui_panel_end();
  }
  if (ui_panel_begin("Format")) {
    /* The link between them: on, changing one side changes the other, keeping the shape. */
    ui_prop_pair(C, ptr, "Resolution", "width", "W", "height", "H", path, [&C] {
      Document &doc = *C.document;
      const float h = ImGui::GetFrameHeight();
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const bool on = doc.resolution_linked;
      if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      const bool clicked = ImGui::Button("##link", ImVec2(h, h));
      if (on) {
        ImGui::PopStyleColor();
      }
      /* A chain: two links joined, or apart when off. */
      ImDrawList *dl = ImGui::GetWindowDrawList();
      const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
      const ImVec2 c(at.x + h * 0.5f, at.y + h * 0.5f);
      const float lw = h * 0.22f, lh = h * 0.13f, gap = on ? h * 0.06f : h * 0.16f;
      dl->AddRect(ImVec2(c.x - gap - lw * 1.6f, c.y - lh), ImVec2(c.x - gap + lw * 0.4f, c.y + lh), col, lh, 0, 1.4f);
      dl->AddRect(ImVec2(c.x + gap - lw * 0.4f, c.y - lh), ImVec2(c.x + gap + lw * 1.6f, c.y + lh), col, lh, 0, 1.4f);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip(on ? "Linked: changing the width changes the height too, keeping the shape (click to unlink)"
                             : "Link width and height: changing one changes the other, keeping the shape");
      }
      if (clicked) {
        const rna::PointerRNA doc_ptr{C.document, &Document::rna()};
        doc_ptr.type->find("resolution_linked")->set(C.document, !on);
        C.undo->push(on ? "Unlink Resolution" : "Link Resolution", doc);
      }
    });
    ui_prop(C, ptr, "fps", path);
    float rgba[4] = {C.document->background.r, C.document->background.g, C.document->background.b, 1.0f};
    ui_driver_next(0, "background");
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
  /* Bone dynamics for the whole document, as Moho's. */
  if (ui_panel_begin("Simulation")) {
    /* Each animatable, with its key button. */
    for (const char *setting : {"gravity", "wind", "wind_direction", "wind_gusts", "wind_gust_speed", "wind_turbulence"}) {
      key_next(C, document_simulation(*C.document), {setting});
      ui_prop(C, ptr, setting, path);
    }
    ui_prop(C, ptr, "simulation_start", path);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Bake All Dynamics", ImVec2(half, 0))) {
      operator_call(C, "armature.dynamics_bake", {{"all", "1"}});
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Cache", ImVec2(half, 0))) {
      operator_call(C, "armature.dynamics_clear_cache");
    }
    ui_panel_end();
  }
}

/* An eye (open, or shut: a line with lashes), as Blender's visibility toggle. */
static void eye_icon(ImDrawList *dl, ImVec2 c, float s, bool open, ImU32 col)
{
  if (open) {
    const int n = 16;
    ImVec2 pts[2 * n];
    for (int i = 0; i < n; i++) {
      const float t = float(i) / float(n - 1);
      const float x = c.x - s + 2.0f * s * t, y = s * 0.62f * std::sin(t * PI);
      pts[i] = ImVec2(x, c.y - y);
      pts[2 * n - 1 - i] = ImVec2(x, c.y + y);
    }
    dl->AddPolyline(pts, 2 * n, col, ImDrawFlags_Closed, 1.3f);
    dl->AddCircleFilled(c, s * 0.34f, col);
  }
  else {
    dl->AddBezierQuadratic(ImVec2(c.x - s, c.y - s * 0.1f), ImVec2(c.x, c.y + s * 0.7f), ImVec2(c.x + s, c.y - s * 0.1f),
                           col, 1.3f);
    for (const float k : {-0.55f, 0.0f, 0.55f}) {
      dl->AddLine(ImVec2(c.x + s * k, c.y + s * 0.3f), ImVec2(c.x + s * k * 1.25f, c.y + s * 0.7f), col, 1.2f);
    }
  }
}

/* A five-pointed star, filled when on: Blender's solo toggle. */
static void star_icon(ImDrawList *dl, ImVec2 c, float s, bool on, ImU32 col)
{
  ImVec2 pts[10];
  for (int i = 0; i < 10; i++) {
    const float a = -PI * 0.5f + float(i) * PI / 5.0f;
    const float r = (i % 2 == 0) ? s : s * 0.45f;
    pts[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
  }
  if (on) {
    dl->AddConcavePolyFilled(pts, 10, col);
  }
  else {
    dl->AddPolyline(pts, 10, col, ImDrawFlags_Closed, 1.2f);
  }
}

/* Blender's Bone Collections panel: the bone layer's collections, each with a dot (filled: something
 * of it is selected; hollow: it has bones or drawings), its eye and its star; + and - beside, and a
 * menu; Assign, Remove, Select and Deselect under it. */
static void bone_collections_panel(Context &C, Layer &armature)
{
  Document &doc = *C.document;
  if (!ui_collapsing_header("Bone Collections", true)) {
    return;
  }
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const float row_h = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
  /* Per bone layer: the list's height in rows, its name filter and whether the filter shows. */
  struct ListState {
    float rows = 5.0f;
    bool filter_open = false;
    std::string filter;
    uint64_t renaming = 0;
    std::string rename_text;
  };
  static std::map<uint64_t, ListState> states;
  ListState &st = states[armature.id];
  const auto picked = [](const Bone &b) { return b.selected || b.select_head || b.select_tail; };
  const auto has = [](const std::vector<uint64_t> &ids, uint64_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };
  std::vector<const Layer *> drawings;
  for (const Layer *l : document_layers_flat(doc)) {
    const Layer *owner = l->type == LayerType::Vector ? layer_armature(doc, l->id) : nullptr;
    if (owner && owner->id == armature.id) {
      drawings.push_back(l);
    }
  }

  const float side = h + style.ItemSpacing.x;
  const float list_w = ImGui::GetContentRegionAvail().x - side;
  const ImVec2 list_min = ImGui::GetCursorScreenPos();
  ImGui::BeginChild("##collections", ImVec2(list_w, st.rows * row_h + style.WindowPadding.y), ImGuiChildFlags_Borders);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 icon = IM_COL32(0xe6, 0xe6, 0xe6, 255), dim = IM_COL32(0xe6, 0xe6, 0xe6, 110);
  /* All first: every bone and editable drawing, nothing assigned to it; it can't be renamed or removed. */
  BoneCollection all_row;
  all_row.id = 0;
  all_row.name = BONE_COLLECTION_ALL;
  all_row.visible = armature.all_bones_visible;
  all_row.solo = armature.all_bones_solo;
  std::vector<const BoneCollection *> rows = {&all_row};
  for (const BoneCollection &c : armature.bone_collections) {
    rows.push_back(&c);
  }
  const bool all_active = !bone_collection_find(armature, armature.active_collection);
  /* A collection deleted from its row's right-click menu, after the list. */
  std::string delete_collection;
  for (const BoneCollection *row_c : rows) {
    const BoneCollection &c = *row_c;
    const bool is_all = &c == &all_row;
    if (!st.filter.empty() && c.name.find(st.filter) == std::string::npos) {
      continue;
    }
    ImGui::PushID(int(c.id) + (is_all ? 0 : 1));
    const ImVec2 row = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float icons_w = row_h * 3.4f;
    if (!is_all && st.renaming == c.id) {
      ImGui::SetNextItemWidth(w - icons_w);
      if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) {
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::InputText("##rename", &st.rename_text, ImGuiInputTextFlags_AutoSelectAll);
      if (ImGui::IsItemDeactivated()) {
        if (!st.rename_text.empty() && st.rename_text != c.name) {
          operator_call(C, "armature.collection_set", {{"collection", c.name}, {"name", st.rename_text}});
        }
        st.renaming = 0;
      }
    }
    else {
      ImGui::SetNextItemAllowOverlap();
      const bool is_active = is_all ? all_active : c.id == armature.active_collection;
      if (ImGui::Selectable("##row", is_active, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w - icons_w, row_h - 2.0f))) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !is_all) {
          st.renaming = c.id;
          st.rename_text = c.name;
        }
        else if (!is_active) {
          operator_call(C, "armature.collection_set", {{"collection", c.name}, {"active", "1"}});
        }
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip(is_all ? "All: every bone, and every drawing Editable in Bone Group. Its eye shows or hides "
                                   "them all; nothing is assigned to it"
                                 : "Click to make it active; double-click to rename; right-click to delete");
      }
      /* Right-click: rename or delete it (its bones and drawings just leave it). All has neither. */
      if (!is_all && ImGui::BeginPopupContextItem("##collection_menu")) {
        ImGui::TextDisabled("%s", c.name.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Rename")) {
          st.renaming = c.id;
          st.rename_text = c.name;
        }
        if (ImGui::MenuItem("Delete")) {
          delete_collection = c.name;
        }
        ImGui::EndPopup();
      }
      const bool shown = is_all ? bone_collection_all_shown(armature) : bone_collection_shown(armature, c);
      dl->AddText(ImVec2(row.x + style.FramePadding.x, row.y + 1.0f),
                  shown ? ImGui::GetColorU32(ImGuiCol_Text) : ImGui::GetColorU32(ImGuiCol_TextDisabled),
                  c.name.c_str());
    }
    /* The dot: filled while something of it is selected, hollow while it has members. */
    bool members = false, selected = false;
    for (const Bone &b : armature.bones) {
      if (is_all || has(b.collections, c.id)) {
        members = true;
        selected |= picked(b);
      }
    }
    for (const Layer *d : drawings) {
      if (d->edit_with_bones && (is_all || has(d->collections, c.id))) {
        members = true;
        selected |= std::any_of(d->drawing.points.begin(), d->drawing.points.end(),
                                [](const MeshPoint &p) { return p.selected; });
      }
    }
    const float cy = row.y + (row_h - 2.0f) * 0.5f;
    const ImVec2 dot(row.x + w - row_h * 2.65f, cy);
    if (selected) {
      dl->AddCircleFilled(dot, row_h * 0.16f, icon);
    }
    else if (members) {
      dl->AddCircle(dot, row_h * 0.16f, dim, 0, 1.2f);
    }
    /* The eye and the star. */
    ImGui::SameLine(w - row_h * 2.0f);
    const ImVec2 eye_at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##eye", ImVec2(row_h, row_h - 2.0f))) {
      operator_call(C, "armature.collection_set", {{"collection", c.name}, {"visible", c.visible ? "0" : "1"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show or hide its bones, and its drawings' points and wires (their artwork still renders)");
    }
    eye_icon(dl, ImVec2(eye_at.x + row_h * 0.5f, cy), row_h * 0.32f, c.visible, ImGui::IsItemHovered() ? icon : dim);
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 star_at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##solo", ImVec2(row_h, row_h - 2.0f))) {
      operator_call(C, "armature.collection_set", {{"collection", c.name}, {"solo", c.solo ? "0" : "1"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Solo: while any collection is soloed, only soloed ones show");
    }
    star_icon(dl, ImVec2(star_at.x + row_h * 0.5f, cy), row_h * 0.32f, c.solo,
              c.solo || ImGui::IsItemHovered() ? icon : dim);
    ImGui::PopID();
  }
  if (armature.bone_collections.empty()) {
    ImGui::TextDisabled("No collections yet: + adds one");
  }
  ImGui::EndChild();
  if (!delete_collection.empty()) {
    operator_call(C, "armature.collection_remove", {{"collection", delete_collection}});
    return;
  }
  const ImVec2 list_max = ImGui::GetItemRectMax();

  /* Beside the list: +, - and the menu. */
  ImGui::SetCursorScreenPos(ImVec2(list_min.x + list_w + style.ItemSpacing.x, list_min.y));
  ImGui::BeginGroup();
  if (ImGui::Button("+", ImVec2(h, h))) {
    operator_call(C, "armature.collection_add");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Add a bone collection after the active one");
  }
  ImGui::BeginDisabled(all_active);
  if (ImGui::Button("-", ImVec2(h, h))) {
    operator_call(C, "armature.collection_remove");
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(all_active ? "All can't be removed" : "Remove the active bone collection (its bones and drawings just leave it)");
  }
  ImGui::Dummy(ImVec2(h, 4.0f));
  if (ImGui::ArrowButton("##specials", ImGuiDir_Down)) {
    ImGui::OpenPopup("##collection_specials");
  }
  if (ui_menu_begin("##collection_specials")) {
    if (ui_menu_item("Move Up")) {
      operator_call(C, "armature.collection_move", {{"direction", "UP"}});
    }
    if (ui_menu_item("Move Down")) {
      operator_call(C, "armature.collection_move", {{"direction", "DOWN"}});
    }
    ImGui::Separator();
    if (ui_menu_item("Show All")) {
      operator_call(C, "armature.collection_show_all");
    }
    if (ui_menu_item("Unsolo All")) {
      operator_call(C, "armature.collection_unsolo_all");
    }
    ImGui::Separator();
    if (ui_menu_item("Select All in Collection")) {
      operator_call(C, "armature.collection_select");
    }
    if (ui_menu_item("Remove Unused Collections")) {
      operator_call(C, "armature.collection_remove_unused");
    }
    ui_menu_end();
  }
  ImGui::EndGroup();

  /* Under the list: the filter toggle (left) and the grip that sizes it (middle). */
  ImGui::SetCursorScreenPos(ImVec2(list_min.x, list_max.y + 1.0f));
  if (ImGui::ArrowButton("##filter", st.filter_open ? ImGuiDir_Down : ImGuiDir_Right)) {
    st.filter_open = !st.filter_open;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Filter the list by name");
  }
  ImGui::SameLine();
  const ImVec2 grip(list_min.x + list_w * 0.5f - h, ImGui::GetCursorScreenPos().y);
  ImGui::SetCursorScreenPos(grip);
  ImGui::InvisibleButton("##grip", ImVec2(h * 2.0f, h * 0.6f));
  if (ImGui::IsItemActive()) {
    st.rows = std::clamp(st.rows + ImGui::GetIO().MouseDelta.y / row_h, 2.0f, 30.0f);
  }
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  }
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 2; j++) {
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(grip.x + h * 0.55f + float(i) * h * 0.3f, grip.y + h * 0.15f + float(j) * h * 0.25f),
                                                  1.2f, IM_COL32(0xa0, 0xa0, 0xa0, 200));
    }
  }
  if (st.filter_open) {
    ImGui::SetNextItemWidth(list_w);
    ImGui::InputTextWithHint("##filter_text", "Filter by name", &st.filter);
  }

  /* Assign, Remove, Select, Deselect. */
  const bool any_bone = std::any_of(armature.bones.begin(), armature.bones.end(), picked);
  /* Drawings Assign and Remove take: picked in the Layers editor, or with points selected in the
   * viewport (as M and Shift M). */
  const bool any_drawing = !collection_drawings(doc, armature).empty();
  const BoneCollection *active = bone_collection_find(armature, armature.active_collection);
  bool active_members = all_active && !armature.bones.empty();
  if (active) {
    for (const Bone &b : armature.bones) {
      active_members |= has(b.collections, active->id);
    }
    for (const Layer *d : drawings) {
      active_members |= d->edit_with_bones && has(d->collections, active->id);
    }
  }
  const float bw = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 3.0f) / 4.0f;
  struct Action {
    const char *label, *op, *tip;
    bool enabled;
  };
  const Action actions[] = {
      {"Assign", "armature.collection_assign", "Put the selected bones and drawings (points selected, or picked in the Layers editor) in the active collection",
       active && (any_bone || any_drawing)},
      {"Remove", "armature.collection_unassign", "Take the selected bones and drawings out of the active collection",
       active && (any_bone || any_drawing)},
      {"Select", "armature.collection_select", "Select the active collection's bones, and its drawings' points",
       active_members},
      {"Deselect", "armature.collection_deselect", "Deselect the active collection's bones, and its drawings' points",
       active_members}};
  for (int i = 0; i < 4; i++) {
    if (i > 0) {
      ImGui::SameLine();
    }
    ImGui::BeginDisabled(!actions[i].enabled);
    if (ImGui::Button(actions[i].label, ImVec2(bw, 0.0f))) {
      operator_call(C, actions[i].op);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", actions[i].tip);
    }
  }
  ImGui::Spacing();
}

/* Relations' Keep Animation: off by default, kept while Open Animate is open (as Parent Bone's). */
static bool *relations_set_keep_flag()
{
  static bool keep = false;
  return &keep;
}

/* Blender's bone Relations, for the active bone: its parent (a searchable list and an eyedropper; on the
 * setup frame), Connected, Local Location, Inherit Rotation and Inherit Scale (with Keep Animation), and
 * the bone collections it is in. */
static void bone_relations_panel(Context &C, Layer &armature)
{
  Document &doc = *C.document;
  Bone *active = bone_find(armature, armature.active_bone);
  if (!active || !ui_panel_begin("Relations")) {
    return;
  }
  const auto label = [](const char *text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
  };
  const bool setup = doc.frame_current == 0;
  const std::string bone_name = active->name;
  const auto relations_set = [&](const char *key, const std::string &value) {
    OpArgs args{{"bone", bone_name}, {key, value}};
    if (std::string(key) == "inherit_rotation" || std::string(key) == "inherit_scale") {
      args["keep_animation"] = *relations_set_keep_flag() ? "1" : "0";
    }
    operator_call(C, "armature.bone_relations_set", args);
    return relations_set_keep_flag();
  };

  /* Parent: hanging it from another bone keeps it where it is. */
  const Bone *parent = bone_find(armature, active->parent);
  label("Parent");
  ImGui::BeginDisabled(!setup);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 2.0f);
  if (ImGui::BeginCombo("##bone_parent", parent ? parent->name.c_str() : "None", ImGuiComboFlags_HeightLarge)) {
    static std::string search;
    if (ImGui::IsWindowAppearing()) {
      search.clear();
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputTextWithHint("##search", "Search bones", &search, ImGuiInputTextFlags_EnterReturnsTrue);
    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    const auto matches = [&](const std::string &name) {
      std::string lower = name;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
      return lower.find(needle) != std::string::npos;
    };
    if (needle.empty() && ImGui::Selectable("None", parent == nullptr)) {
      operator_call(C, "armature.bone_parent_set", {{"bone", bone_name}, {"parent", ""}});
    }
    const Bone *first = nullptr;
    for (const Bone &b : armature.bones) {
      /* Not itself, nor anything hanging from it. */
      if (bone_is_descendant(armature, b.id, active->id) || !matches(b.name)) {
        continue;
      }
      first = first ? first : &b;
      if (ImGui::Selectable(b.name.c_str(), parent && parent->id == b.id)) {
        operator_call(C, "armature.bone_parent_set", {{"bone", bone_name}, {"parent", b.name}});
      }
    }
    if (!first && !needle.empty()) {
      ImGui::TextDisabled("No bone named like that");
    }
    if (entered && first) {
      operator_call(C, "armature.bone_parent_set", {{"bone", bone_name}, {"parent", first->name}});
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(setup ? "The bone this one hangs from; it stays where it is"
                            : "Switch to Setup (frame 0) to change the parent");
  }
  ImGui::SameLine(0, 2);
  const uint64_t rig_id = armature.id, own = active->id;
  if (ui_pick_button("##pick_parent", "Eyedropper: click the parent bone in the viewport",
                     ui_bone_eyedropper_armature() == rig_id))
  {
    ui_bone_eyedropper_start(
        rig_id,
        [rig_id, own](Context &C, uint64_t picked) {
          Layer *rig = layer_find(*C.document, rig_id);
          const Bone *b = rig ? bone_find(*rig, own) : nullptr;
          const Bone *up = rig ? bone_find(*rig, picked) : nullptr;
          if (!b || !up) {
            return true;
          }
          if (bone_is_descendant(*rig, up->id, b->id)) {
            return false;
          }
          operator_call(C, "armature.bone_parent_set", {{"bone", b->name}, {"parent", up->name}});
          return true;
        },
        [] { return std::string("Click the parent bone  |  Esc cancels"); });
  }
  ImGui::EndDisabled();
  active = bone_find(armature, own);
  if (!active) {
    ui_panel_end();
    return;
  }
  parent = bone_find(armature, active->parent);

  label("");
  ImGui::BeginDisabled(!setup || !parent);
  bool connected = active->connected;
  if (ImGui::Checkbox("Connected", &connected)) {
    relations_set("connected", connected ? "1" : "0");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(!parent  ? "Connected needs a parent"
                      : !setup ? "Switch to Setup (frame 0) to connect it"
                               : "Its head sits on the parent's tail and moves with it");
  }
  ImGui::EndDisabled();

  active = bone_find(armature, own);
  label("");
  bool local_location = active->local_location;
  if (ImGui::Checkbox("Local Location", &local_location)) {
    relations_set("local_location", local_location ? "1" : "0");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Its Transform reads along the bone as it rests (X down it); off, along its parent's axes.\n"
                      "Only the numbers shown change: the keys and the pose stay");
  }

  active = bone_find(armature, own);
  label("");
  ImGui::BeginDisabled(!parent);
  bool inherit_rotation = active->inherit_rotation;
  bool *keep = nullptr;
  if (ImGui::Checkbox("Inherit Rotation", &inherit_rotation)) {
    keep = relations_set("inherit_rotation", inherit_rotation ? "1" : "0");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Posing, it turns as its parent turns; off, it keeps its own angle in the picture (Moho's\n"
                      "Independent Angle). Its head still goes where the parent takes it");
  }
  active = bone_find(armature, own);
  label("Inherit Scale");
  const char *scales[] = {"Full", "Uniform", "None"};
  const char *scale_ids[] = {"FULL", "UNIFORM", "NONE"};
  const int scale = int(active->inherit_scale);
  if (ImGui::BeginCombo("##inherit_scale", scales[scale])) {
    for (int k = 0; k < 3; k++) {
      if (ImGui::Selectable(scales[k], k == scale) && k != scale) {
        keep = relations_set("inherit_scale", scale_ids[k]);
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("How much of its parent's change in size it takes, posing: Full (its squash and stretch\n"
                      "too), Uniform (only its overall size) or None (it keeps its own size)");
  }
  ImGui::EndDisabled();
  (void)keep;
  /* Keep Animation for the two above: off by default, kept while Open Animate is open. */
  bool *keep_animation = relations_set_keep_flag();
  label("");
  ImGui::BeginDisabled(!parent);
  ImGui::Checkbox("Keep Animation##relations", keep_animation);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Changing Inherit Rotation or Scale keeps every keyed frame of every action looking as it did:\n"
                      "the bone's angle and scale are keyed back there");
  }
  ImGui::EndDisabled();

  /* In a child of a switch: the bone it stands in for in another child (a bone swap,
   * anim/bone_family.hh), which carries the selection across a pick and guides Match Pose. */
  if (Layer *sw = switch_of(doc, armature.id); sw && switch_child_holding(doc, *sw, armature.id)) {
    active = bone_find(armature, own);
    const Layer *own_child = switch_child_holding(doc, *sw, armature.id);
    const Layer *swap_layer = active->swap_layer ? layer_find(doc, active->swap_layer) : nullptr;
    const Bone *swap_bone = swap_layer ? bone_find(*swap_layer, active->swap_bone) : nullptr;
    const std::string now = swap_bone ? swap_layer->name + ": " + swap_bone->name : "None";
    label("Stands In For");
    std::string pick_layer, pick_bone;
    bool picked = false;
    if (ImGui::BeginCombo("##stands_in_for", now.c_str(), ImGuiComboFlags_HeightLarge)) {
      if (ImGui::Selectable("None", !swap_bone)) {
        picked = true;
      }
      for (Layer &child : sw->children) {
        Layer *other = &child == own_child ? nullptr : switch_child_armature(child);
        if (!other || other->id == armature.id) {
          continue;
        }
        for (const Bone &b : other->bones) {
          const std::string item = other->name + ": " + b.name;
          if (ImGui::Selectable(item.c_str(), swap_bone && swap_layer->id == other->id && swap_bone->id == b.id)) {
            picked = true;
            pick_layer = std::to_string(other->id);
            pick_bone = b.name;
          }
        }
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Its counterpart in another of the switch's drawings (the straight leg's shin for the bent\n"
                        "leg's). Poses aren't shared: a pick carries the selection across, and Match Pose from\n"
                        "Previous Drawing lines it up. The switch's menu has Auto-link Bones by Name");
    }
    if (picked) {
      operator_call(C, "armature.bone_swap_set", {{"bone", bone_name}, {"layer", pick_layer}, {"target", pick_bone}});
    }
    if (swap_bone) {
      label("");
      if (ImGui::Button("Link All Channels")) {
        operator_call(C, "armature.bone_link_all", {{"bone", bone_name}});
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Its angle, position and scale share the keys of the bone it stands in for, both ways\n"
                          "(2-Way Linked drivers at factor 1): for the same bone across drawings");
      }
    }
  }

  /* The bone collections it is in, with their eye and star (as the Bone Collections list). */
  active = bone_find(armature, own);
  label("Collections");
  bool any = false;
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float h = ImGui::GetFrameHeight();
  const ImU32 icon = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  for (const BoneCollection &c : armature.bone_collections) {
    if (std::find(active->collections.begin(), active->collections.end(), c.id) == active->collections.end()) {
      continue;
    }
    if (any) {
      label("");
    }
    any = true;
    ImGui::PushID(int(c.id));
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(c.name.c_str());
    ImGui::SameLine(std::max(0.0f, w - h * 2.0f));
    const ImVec2 eye_at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##eye", ImVec2(h, h))) {
      operator_call(C, "armature.collection_set", {{"collection", c.name}, {"visible", c.visible ? "0" : "1"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show or hide its bones, and its drawings' points and wires");
    }
    eye_icon(dl, ImVec2(eye_at.x + h * 0.5f, eye_at.y + h * 0.5f), h * 0.32f, c.visible, ImGui::IsItemHovered() ? icon : dim);
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 star_at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##solo", ImVec2(h, h))) {
      operator_call(C, "armature.collection_set", {{"collection", c.name}, {"solo", c.solo ? "0" : "1"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Solo: while any collection is soloed, only soloed ones show");
    }
    star_icon(dl, ImVec2(star_at.x + h * 0.5f, star_at.y + h * 0.5f), h * 0.32f, c.solo,
              c.solo || ImGui::IsItemHovered() ? icon : dim);
    ImGui::PopID();
    active = bone_find(armature, own);
    if (!active) {
      break;
    }
  }
  if (!any) {
    ImGui::TextDisabled("None (Assign in Bone Collections, or M)");
  }
  ui_panel_end();
}

/* A two-driver fix's poses as a map (Live2D's linked parameter grid): its first action bone action across, its
 * second up, each from rest (0%) to all of it (100%); a dot per pose (ringed while it's edited), a cross where
 * the pose is now. Click a dot to edit that pose, right-click it to remove it, click elsewhere to add a pose
 * there. */
static void fix_pose_map(Context &C, const Layer &owner, const FixAction &fix, const std::vector<float> &now, int edited)
{
  const float font = ImGui::GetFontSize();
  const float side = std::min(ImGui::GetContentRegionAvail().x, font * 11.0f);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float pad = font * 0.6f;
  ImGui::InvisibleButton("##pose_map", ImVec2(side, side));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a(p0.x + pad, p0.y + pad), b(p0.x + side - pad, p0.y + side - pad);
  const auto to_screen = [&](float x, float y) { return ImVec2(a.x + (b.x - a.x) * x, b.y - (b.y - a.y) * y); };
  const auto to_share = [&](ImVec2 m) {
    return std::pair<float, float>(std::clamp((m.x - a.x) / (b.x - a.x), 0.0f, 1.0f),
                                   std::clamp((b.y - m.y) / (b.y - a.y), 0.0f, 1.0f));
  };
  dl->AddRectFilled(p0, ImVec2(p0.x + side, p0.y + side), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
  for (int g = 1; g < 4; g++) {
    const float t = float(g) * 0.25f;
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_Border, g == 2 ? 0.9f : 0.5f);
    dl->AddLine(to_screen(t, 0.0f), to_screen(t, 1.0f), grid);
    dl->AddLine(to_screen(0.0f, t), to_screen(1.0f, t), grid);
  }
  dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border));
  /* Its two action bone actions along the edges; rest is the bottom-left corner. */
  const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  const ImVec2 x_size = ImGui::CalcTextSize(fix.drivers[0].c_str());
  dl->AddText(ImVec2(b.x - x_size.x - 2.0f, b.y - font - 2.0f), dim, fix.drivers[0].c_str());
  dl->AddText(ImVec2(a.x + 3.0f, a.y + 2.0f), dim, fix.drivers[1].c_str());
  dl->AddText(ImVec2(a.x + 3.0f, b.y - font - 2.0f), dim, "rest");
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  int under = -1;
  float under_d = font * 0.7f;
  for (size_t k = 0; k < fix.poses.size(); k++) {
    const std::vector<float> sh = fix_pose_shares(owner, fix, fix.poses[k]);
    const ImVec2 at = to_screen(sh[0], sh[1]);
    const float d = std::hypot(at.x - mouse.x, at.y - mouse.y);
    if (hovered && d < under_d) {
      under = int(k);
      under_d = d;
    }
  }
  /* Where it is now. */
  if (now.size() >= 2) {
    const ImVec2 at = to_screen(std::clamp(now[0], 0.0f, 1.0f), std::clamp(now[1], 0.0f, 1.0f));
    const ImU32 col = IM_COL32(0xf0, 0xf0, 0xf0, 230);
    dl->AddLine(ImVec2(at.x - 6.0f, at.y), ImVec2(at.x + 6.0f, at.y), col, 1.5f);
    dl->AddLine(ImVec2(at.x, at.y - 6.0f), ImVec2(at.x, at.y + 6.0f), col, 1.5f);
  }
  for (size_t k = 0; k < fix.poses.size(); k++) {
    const std::vector<float> sh = fix_pose_shares(owner, fix, fix.poses[k]);
    const ImVec2 at = to_screen(sh[0], sh[1]);
    const float r = font * 0.32f;
    dl->AddCircleFilled(at, r, int(k) == under ? UI_FIX_COLOR : UI_FIX_ACTIVE_COLOR);
    dl->AddCircle(at, r, UI_FIX_COLOR, 0, 1.5f);
    if (int(k) == edited) {
      dl->AddCircle(at, r + 3.5f, IM_COL32(0xff, 0xff, 0xff, 230), 0, 2.0f);
    }
    /* Its number beside it, on the inside near the right edge. */
    const std::string num = std::to_string(k + 1);
    const float num_w = ImGui::CalcTextSize(num.c_str()).x;
    const float num_x = at.x + r + 3.0f + num_w > p0.x + side - 2.0f ? at.x - r - 3.0f - num_w : at.x + r + 3.0f;
    dl->AddText(ImVec2(num_x, at.y - font * 0.5f), UI_FIX_COLOR, num.c_str());
  }
  /* The pose under the mouse, or where a click would add one. */
  if (hovered) {
    if (under >= 0) {
      ImGui::SetTooltip("Pose %d: %s (frame %d)\nClick to edit it  |  right-click to remove it", under + 1,
                        fix_pose_label(owner, fix, fix.poses[size_t(under)]).c_str(), fix.poses[size_t(under)].frame);
    }
    else {
      const auto [x, y] = to_share(mouse);
      ImGui::SetTooltip("%s %d%%  \xC2\xB7  %s %d%%\nClick to add a pose here", fix.drivers[0].c_str(),
                        int(std::lround(std::max(x, FIX_COUNTS_FROM) * 100.0f)), fix.drivers[1].c_str(),
                        int(std::lround(std::max(y, FIX_COUNTS_FROM) * 100.0f)));
    }
  }
  static int removing = -1;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    if (under >= 0) {
      operator_call(C, "anim.action_set", {{"name", fix.name}, {"frame", std::to_string(fix.poses[size_t(under)].frame)}});
    }
    else {
      const auto [x, y] = to_share(mouse);
      char shares[64];
      std::snprintf(shares, sizeof(shares), "%.4f,%.4f", std::max(x, FIX_COUNTS_FROM), std::max(y, FIX_COUNTS_FROM));
      operator_call(C, "anim.fix_pose_add", {{"name", fix.name}, {"shares", shares}});
    }
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && under >= 0) {
    removing = under;
    ImGui::OpenPopup("##pose_map_menu");
  }
  if (ui_menu_begin("##pose_map_menu")) {
    const bool can = fix.poses.size() > 1;
    if (ui_menu_item(("Remove Pose " + std::to_string(removing + 1)).c_str(), nullptr, false, can)) {
      operator_call(C, "anim.fix_pose_remove", {{"name", fix.name}, {"pose", std::to_string(removing + 1)}});
    }
    if (!can && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("It's the fix's only pose: delete the fix instead");
    }
    ui_menu_end();
  }
}

/* Fix actions (correctives: Maya's combination shapes, Blender's corrective shape keys), for the bone layer
 * that owns them. While one is edited: what it fixes, its poses (a map for two action bone actions), the pose
 * shown, and how to key it. Otherwise each fix with how strongly it plays now, and fixing the pose now. */
static void fix_actions_panel(Context &C, Layer &owner)
{
  Document &doc = *C.document;
  const FixAction *editing = doc.action_layer == owner.id && !doc.action.empty() ? fix_action_find(owner, doc.action)
                                                                                 : nullptr;
  if (!editing && owner.fix_actions.empty() && action_bone_actions_counting(owner).size() < 2 &&
      std::none_of(owner.actions.begin(), owner.actions.end(),
                   [&](const std::string &n) { return action_bone_of(owner, n); }))
  {
    return; /* No action bone actions to fix: nothing to show. */
  }
  const std::string header = editing ? "Fix Action: editing " + editing->name : "Fix Actions";
  if (!ui_panel_begin("Fix Actions", header, editing ? UI_FIX_COLOR : 0u)) {
    return;
  }
  const auto label = [](const char *text) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
  };
  const auto percent = [](float share) { return std::to_string(int(std::lround(share * 100.0f))) + "%"; };
  const auto tip = [](const char *text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", text);
    }
  };
  const ImVec4 fix_col = ImGui::ColorConvertU32ToFloat4(UI_FIX_COLOR);
  const ImVec4 broken_col = ImGui::ColorConvertU32ToFloat4(UI_BROKEN_COLOR);
  /* Where each of a fix's drivers is now, as shares (0 with one not playing). */
  const auto shares_now = [&](const FixAction &f, const std::vector<std::pair<std::string, float>> &playing) {
    FixPose probe;
    for (const std::string &d : f.drivers) {
      const auto it = std::find_if(playing.begin(), playing.end(), [&](const auto &pl) { return pl.first == d; });
      probe.at.push_back(it == playing.end() ? 0.0f : it->second);
    }
    return fix_pose_shares(owner, f, probe);
  };
  /* Its poses: a map for two action bone actions, a list for more. */
  const auto poses_ui = [&](const FixAction &f, const std::vector<float> &now, int edited) {
    if (f.drivers.size() == 2) {
      label("Poses");
      fix_pose_map(C, owner, f, now, edited);
      return;
    }
    for (size_t k = 0; k < f.poses.size(); k++) {
      ImGui::PushID(int(k));
      label(k == 0 ? "Poses" : "");
      const std::string text = std::to_string(k + 1) + ": " + fix_pose_label(owner, f, f.poses[k]);
      if (ImGui::Selectable(text.c_str(), int(k) == edited)) {
        operator_call(C, "anim.action_set", {{"name", f.name}, {"frame", std::to_string(f.poses[k].frame)}});
      }
      tip("Edit this pose (its frame in the fix)");
      ImGui::PopID();
    }
  };

  if (editing) {
    const std::string name = editing->name;
    const int pose = fix_pose_on_frame(*editing, doc.frame_current);
    /* How to work in it, first: the one thing to know. */
    label("");
    ImGui::PushTextWrapPos(0.0f);
    if (pose >= 0) {
      ImGui::TextColored(fix_col, "Pose %d of %d. Its action bones are held at it: key what should change. Exactly here "
                         "it plays as keyed; between poses they blend.", pose + 1, int(editing->poses.size()));
    }
    else {
      ImGui::TextColored(broken_col, "Frame %d isn't one of its poses: keys here don't play. Pick a pose below.",
                         doc.frame_current);
    }
    ImGui::PopTextWrapPos();

    label("Fixes");
    std::string drivers;
    for (const std::string &d : editing->drivers) {
      drivers += (drivers.empty() ? "" : " \xC3\x97 ") + d;
    }
    ImGui::TextUnformatted(drivers.c_str());
    tip("The action bones this fixes playing together. Fixes for fewer of them play under it while you key it");

    const int held = fix_pose_held(*editing, doc.frame_current);
    const std::vector<float> now = held >= 0 ? fix_pose_shares(owner, *editing, editing->poses[size_t(held)]) : std::vector<float>();
    poses_ui(*editing, now, pose);

    if (pose >= 0) {
      label("This Pose");
      ImGui::TextUnformatted(fix_pose_label(owner, *editing, editing->poses[size_t(pose)]).c_str());
      label("");
      ImGui::BeginDisabled(editing->poses.size() < 2);
      if (ImGui::Button("Remove This Pose", ImVec2(-1, 0))) {
        operator_call(C, "anim.fix_pose_remove", {{"name", name}, {"pose", std::to_string(pose + 1)}});
      }
      ImGui::EndDisabled();
      tip(editing->poses.size() < 2 ? "It's the fix's only pose: delete the fix instead"
                                    : "Remove this pose and its keys; the fix blends between the others");
    }
    label("");
    if (ImGui::Button("Back to Mainline", ImVec2(-1, 0))) {
      operator_call(C, "anim.action_set", {{"name", ""}});
    }
    tip("Stop editing the fix. On the mainline it plays by itself whenever its action bones turn together");
    ui_panel_end();
    return;
  }

  /* Not editing one: each fix, how strongly it plays now, and what to do with it. */
  const std::vector<std::pair<std::string, float>> playing = action_bone_actions_playing(owner);
  const std::vector<std::pair<std::string, float>> counting = action_bone_actions_counting(owner);
  const bool in_action_bone_action = !doc.action.empty() && doc.action_layer == owner.id &&
                                     action_adds(owner, doc.action);
  std::string doomed;
  for (const FixAction &f : owner.fix_actions) {
    ImGui::PushID(f.name.c_str());
    const std::string broken = fix_action_broken(owner, f);
    const float strength = broken.empty() ? fix_action_strength(owner, f, playing) : 0.0f;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    if (!broken.empty()) {
      ImGui::TextColored(broken_col, "%s", f.name.c_str());
    }
    else if (strength > 0.0f) {
      ImGui::TextColored(fix_col, "%s", f.name.c_str());
    }
    else {
      ImGui::TextUnformatted(f.name.c_str());
    }
    std::string drivers;
    for (const std::string &d : f.drivers) {
      drivers += (drivers.empty() ? "" : " and ") + d;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Fixes %s turning together: %d pose%s", drivers.c_str(), int(f.poses.size()),
                        f.poses.size() == 1 ? "" : "s");
    }
    ImGui::TableSetColumnIndex(1);
    const float h = ImGui::GetFrameHeight(), spacing = ImGui::GetStyle().ItemSpacing.x;
    const float edit_w = ImGui::CalcTextSize("Edit").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float bar_w = ImGui::GetContentRegionAvail().x - edit_w - h - spacing * 2.0f;
    if (!broken.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, UI_BROKEN_COLOR);
      ImGui::ProgressBar(0.0f, ImVec2(bar_w, 0), "can't play");
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Can't play: %s. Its keys are kept; delete it, or bring the action bone action back", broken.c_str());
      }
    }
    else {
      const std::string text = strength > 0.0f ? percent(strength) + " now" : "not playing";
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, UI_FIX_ACTIVE_COLOR);
      ImGui::ProgressBar(strength, ImVec2(bar_w, 0), text.c_str());
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("How much of it plays on this frame: all of it at or past one of its poses, none while any of "
                          "%s rests", drivers.c_str());
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Edit")) {
      operator_call(C, "anim.action_set", {{"name", f.name}});
    }
    tip("Open it in the timeline on its pose nearest the pose now, its action bones held there");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##more", ImGuiDir_Down)) {
      ImGui::OpenPopup("##fix_more");
    }
    tip("More: Delete");
    if (ui_menu_begin("##fix_more")) {
      if (ui_menu_item("Delete")) {
        doomed = f.name;
      }
      ui_menu_end();
    }
    ImGui::PopID();
  }

  /* Fixing the pose now: here as in the timeline's Actions menu, under what plays. */
  const bool can_fix = counting.size() >= 2 && !in_action_bone_action && doc.frame_current != 0;
  const FixAction *for_all = nullptr;
  if (counting.size() >= 2 && !in_action_bone_action) {
    label("Playing Now");
    std::string line;
    std::vector<std::string> names;
    for (const auto &[n, t] : counting) {
      const float l = action_bone_action_length(owner, n);
      line += (line.empty() ? "" : "   \xC2\xB7   ") + n + " " + percent(l > 0.0f ? std::min(t / l, 1.0f) : 0.0f);
      names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    for (const FixAction &f : owner.fix_actions) {
      std::vector<std::string> d = f.drivers;
      std::sort(d.begin(), d.end());
      for_all = d == names ? &f : for_all;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", line.c_str());
    ImGui::PopTextWrapPos();
    tip("The action bones playing on this frame (turned at least 5%), each as far as it has turned");
  }
  const int near_pose = for_all ? fix_pose_near(owner, *for_all, counting) : -1;
  label("");
  ImGui::BeginDisabled(!can_fix);
  const std::string button = !for_all     ? std::string("New Fix Action for This Pose")
                             : near_pose >= 0 ? "Edit Pose " + std::to_string(near_pose + 1) + " of " + for_all->name
                                         : "Add This Pose to " + for_all->name;
  if (ImGui::Button(button.c_str(), ImVec2(-1, 0))) {
    operator_call(C, "anim.fix_action_new");
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    if (can_fix) {
      ImGui::SetTooltip(!for_all ? "A fix plays by itself whenever these play together. It opens with them held right "
                                   "here, to key the correction on top"
                        : near_pose >= 0 ? "The fix has this pose already: open it to change the correction"
                                    : "Add this pose to the fix, starting from what it plays here now, and key the "
                                      "correction. Each pose is exact; between them they blend");
    }
    else {
      ImGui::SetTooltip(in_action_bone_action ? "Fix a pose from the mainline, not inside an action bone action"
                        : doc.frame_current == 0 ? "Frame 0 is the rest pose: go to another frame"
                                                 : "Turn two or more action bones at once (each at least 5%, on the "
                                                   "mainline) where the result doesn't look right, then fix that pose");
    }
  }
  /* The fix for what plays now: where this pose is among its poses. */
  if (for_all && can_fix) {
    poses_ui(*for_all, shares_now(*for_all, counting), near_pose);
  }
  if (owner.fix_actions.empty() && !can_fix) {
    label("");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Turn two action bones together where they don't look right, then fix that pose.");
    ImGui::PopTextWrapPos();
  }
  ui_panel_end();
  if (!doomed.empty()) {
    operator_call(C, "anim.action_delete", {{"name", doomed}});
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
  ui_panel_end();
}

/* Blender's Bone tab: the active bone's own settings (its name, where it is on the setup frame or how
 * it is posed on others, its IK). */
static void bone_panel(Context &C, Layer &armature)
{
  Document &doc = *C.document;
  Bone *active = bone_find(armature, armature.active_bone);
  if (!active || !ui_panel_begin("Bone")) {
    return;
  }
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
    /* Its action bone actions are named after it: they (and the fixes they drive) follow a rename. */
    static std::string renaming_from;
    const std::string name_before = active->name;
    ImGui::InputText("##bone_name", &active->name);
    if (ImGui::IsItemActivated()) {
      renaming_from = name_before;
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      armature_bone_renamed(doc, armature.id, renaming_from, active->name);
      C.undo->push("Rename Bone", doc);
    }
    active = bone_find(armature, armature.active_bone);
    if (!active) {
      ui_panel_end();
      return;
    }
    float degrees = active->angle * 180.0f / PI;
    const std::string bone_path = "bones[\"" + active->name + "\"].";
    /* A bone field with its keyframe button. */
    const auto keyed_field = [&](const char *name, const std::string &channel, auto &&field) {
      const int state = layer_key_state(armature, {bone_path + channel}, doc.frame_current);
      label(name);
      ImGui::SetNextItemWidth(-ui_key_button_width());
      const int pushed = ui_key_tint_push(state | layer_action_bone_state(armature, {bone_path + channel}));
      const int driven = ui_driver_field_begin(C, armature.id, bone_path + channel, -1);
      field();
      ui_driver_typed(C, armature.id, bone_path + channel, -1);
      ui_driver_field_end(C, armature.id, bone_path + channel, -1, driven);
      ImGui::PopStyleColor(pushed);
      ImGui::PushID(name);
      if (ui_key_button("##key", state)) {
        operator_call(C, "anim.keyframe_toggle",
                      {{"layer", std::to_string(armature.id)}, {"paths", bone_path + channel}});
      }
      const int pin = pin_state(armature, {bone_path + channel}, state);
      if (ui_pin_button("##pin", pin)) {
        pin_toggle(C, armature.id, bone_path + channel, pin);
      }
      if (pin == 1) {
        ui_pinned_row();
      }
      ImGui::PopID();
    };
    if (doc.frame_current == 0) {
      /* The setup frame edits the rig, as Blender's edit mode: where the bone's ends are (in the
       * bone layer's space). A connected bone's head is its parent's tail, so it moves that too. */
      const std::vector<BonePose> poses = armature_pose(armature, true);
      const BonePose *pose = bone_pose_find(poses, active->id);
      float2 head = pose ? pose->head : active->position, tail = pose ? pose->tail : active->position;
      const Bone *above = active->connected ? bone_find(armature, active->parent) : nullptr;
      const BonePose *above_pose = above ? bone_pose_find(poses, above->id) : nullptr;
      label("Head");
      if (ui_drag_float2("##bone_head", &head.x, 0.005f, -10000.0f, 10000.0f, "%.3f")) {
        std::map<uint64_t, std::pair<float2, float2>> targets{{active->id, {head, tail}}};
        if (above_pose) {
          targets[above->id] = {above_pose->head, head};
        }
        armature_set_joints(armature, targets);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip(above ? "Where the bone starts. Connected: it is its parent's tail, which moves with it"
                                : "Where the bone starts (the bone layer's space)");
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        C.undo->push("Bone Head", doc);
      }
      label("Tail");
      if (ui_drag_float2("##bone_tail", &tail.x, 0.005f, -10000.0f, 10000.0f, "%.3f")) {
        armature_set_joints(armature, {{active->id, {head, tail}}});
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Where the bone ends (the bone layer's space); connected children's heads follow it");
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        C.undo->push("Bone Tail", doc);
      }
      active = bone_find(armature, armature.active_bone);
      label("Length");
      ui_drag_float("##bone_length", &active->length, 0.005f, 0.0f, 1000.0f, "%.3f");
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        C.undo->push("Bone Length", doc);
      }
    }
    else {
      /* Any other frame poses it, as Blender's pose mode: Transform and Angle are the change from
       * the setup pose (0 at rest), Scale its scale; the keys keep the bone's own values, as Moho's. */
      const float setup_angle = bone_setup_angle(*active);
      /* An action bone of the fix being edited is held at the fix's pose: shown, not changed. */
      const bool held = bone_held_by_fix(doc, armature, *active);
      if (held) {
        label("");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(UI_FIX_COLOR),
                           "Held at the fix's pose: pick another pose, or go back to the mainline to turn it.");
        ImGui::PopTextWrapPos();
        ImGui::BeginDisabled();
      }
      keyed_field("Transform", "position", [&] {
        /* Local Location (Relations): along the bone as it rests; off, along its parent. */
        const bool local = active->local_location;
        float2 moved = local ? bone_transform_from_position(*active, active->position)
                             : active->position - active->setup_position;
        if (ui_drag_float2("##bone_position", &moved.x, 0.005f, -10000.0f, 10000.0f, "%.3f")) {
          active->position = local ? bone_position_from_transform(*active, moved) : active->setup_position + moved;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip(local ? "How far its base has moved from the setup pose, along the bone as it rests (X) and "
                                    "across it (Y)"
                                  : "How far its base has moved from the setup pose, along its parent's axes (Local "
                                    "Location is off)");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          C.undo->push("Bone Transform", doc);
        }
      });
      keyed_field("Angle", "angle", [&] {
        degrees = (active->angle - setup_angle) * 180.0f / PI;
        if (ui_drag_float("##bone_angle", &degrees, 0.5f, -36000.0f, 36000.0f, "%.1f\xC2\xB0")) {
          active->angle = setup_angle + degrees * PI / 180.0f;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("How far it has turned from the setup pose");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          C.undo->push("Bone Angle", doc);
        }
      });
      keyed_field("Scale", "scale", [&] {
        /* X across the bone, Y along it, as Blender's bone axes. */
        ui_drag_float2("##bone_scale", &active->scale.x, 0.01f, -100.0f, 100.0f, "%.3f");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("X: across the bone (its thickness). Y: along it (its length)");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          C.undo->push("Bone Scale", doc);
        }
      });
      if (held) {
        ImGui::EndDisabled();
      }
    }
    /* A look target's Depth (Look At constraints aim at it): how far in front of the face it stands. */
    bool look_target = false;
    for (const Bone &b : armature.bones) {
      for (const BoneConstraint &bc : b.constraints) {
        look_target = look_target || (bc.type == BoneConstraintType::LookAt && bc.target == active->id);
      }
    }
    if (look_target) {
      keyed_field("Depth", "depth", [&] {
        if (ui_drag_float("##bone_depth", &active->depth, 0.005f, 0.001f, 10000.0f, "%.3f",
                             ImGuiSliderFlags_AlwaysClamp))
        {
          active->depth = std::max(active->depth, 0.001f);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("How far in front of the face this look target stands: close, the eyes looking at it\n"
                            "cross; far, they look the same way. Keyable");
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          C.undo->push("Look Depth", doc);
        }
      });
    }
    /* Moho's IK settings of the bone itself. */
    label("IK");
    const std::string driven_ik_path = std::string("bones[\"" + active->name + "\"].") + "ik_ignore";
    const int driven_ik = ui_driver_field_begin(C, armature.id, driven_ik_path, -1);
    if (ImGui::Checkbox("Ignored by IK", &active->ik_ignore)) {
      C.undo->push("Ignored by IK", doc);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("IK chains stop below this bone and never turn it");
    }
    ui_driver_typed(C, armature.id, driven_ik_path, -1);
    ui_driver_field_end(C, armature.id, driven_ik_path, -1, driven_ik);
    ImGui::SameLine();
    const std::string driven_arc_path = std::string("bones[\"" + active->name + "\"].") + "arc_ik";
    const int driven_arc = ui_driver_field_begin(C, armature.id, driven_arc_path, -1);
    if (ImGui::Checkbox("Arc Solver", &active->arc_ik)) {
      C.undo->push("Arc IK Solver", doc);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Moho's Arc IK solver: a chain ending at this bone bends evenly, as an arc");
    }
    ui_driver_typed(C, armature.id, driven_arc_path, -1);
    ui_driver_field_end(C, armature.id, driven_arc_path, -1, driven_arc);
    ImGui::BeginDisabled(doc.frame_current != 0);
    label("Strength");
    ui_drag_float("##bone_strength", &active->strength, 0.01f, 0.0f, 100.0f, "%.2f");
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

static void props_label(const char *text)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(text);
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-1);
}

/* Three small swatches of a bone colour set's shades, from `at`, each `w` wide. */
static void color_set_swatches(ImDrawList *dl, ImVec2 at, float w, float h, const BoneColors &c)
{
  const Color shades[3] = {c.regular, c.select, c.active};
  for (int k = 0; k < 3; k++) {
    const Color &s = shades[k];
    dl->AddRectFilled(ImVec2(at.x + w * float(k), at.y), ImVec2(at.x + w * float(k + 1), at.y + h),
                      IM_COL32(int(s.r * 255.0f), int(s.g * 255.0f), int(s.b * 255.0f), 255));
  }
}

/* Blender's Bone > Viewport Display: what the bone is drawn as (Bone, Pin or another layer's
 * drawing) and its colour set. Only how it looks, so any frame. */
static void bone_display_panel(Context &C, Layer &armature)
{
  Document &doc = *C.document;
  Bone *active = bone_find(armature, armature.active_bone);
  if (!active || !ui_panel_begin("Viewport Display")) {
    return;
  }
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();

  /* Shape: Bone | Pin | Custom, as Blender's tabs. */
  props_label("Shape");
  {
    static const char *const names[] = {"Bone", "Pin", "Custom"};
    static const char *const tips[] = {"Drawn as a bone", "A round pin at its base, as Moho draws pin bones",
                                       "Drawn as a vector layer's drawing, where that layer sits at rest"};
    const float gap = 1.0f;
    const float w = (ImGui::GetContentRegionAvail().x - 2.0f * gap) / 3.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, style.ItemSpacing.y));
    for (int k = 0; k < 3; k++) {
      if (k > 0) {
        ImGui::SameLine();
      }
      const bool on = int(active->shape) == k;
      if (on) {
        const ImVec4 blue = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, blue);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(blue.x * 1.15f, blue.y * 1.15f, blue.z * 1.15f, blue.w));
      }
      if (ImGui::Button(names[k], ImVec2(w, 0.0f)) && !on) {
        active->shape = BoneShape(k);
        C.undo->push("Bone Shape", doc);
      }
      if (on) {
        ImGui::PopStyleColor(2);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s\n(Shown while Viewport Overlays > Custom Bone Shapes is on)", tips[k]);
      }
    }
    ImGui::PopStyleVar();
  }
  if (active->shape == BoneShape::Pin) {
    props_label("Size");
    ui_drag_float("##pin_size", &active->pin_size, 0.01f, 0.1f, 20.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Against the standard pin; it keeps its size on screen, whatever the zoom");
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Pin Size", doc);
    }
  }
  if (active->shape == BoneShape::Custom) {
    /* The layer: a list to pick from (or drop a layer from the Layers editor), the eyedropper,
     * and a button that clears it. */
    props_label("Layer");
    const Layer *shape_layer = active->shape_layer ? layer_find(doc, active->shape_layer) : nullptr;
    const bool missing = active->shape_layer != 0 && !shape_layer;
    const float buttons = h * (active->shape_layer ? 2.0f : 1.0f) + style.ItemInnerSpacing.x * (active->shape_layer ? 2.0f : 1.0f);
    ImGui::SetNextItemWidth(-buttons);
    if (missing) {
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0xff, 0x80, 0x70, 255));
    }
    const std::string preview = shape_layer ? shape_layer->name : missing ? "(missing layer)" : "";
    const bool open = ImGui::BeginCombo("##shape_layer", preview.c_str());
    if (missing) {
      ImGui::PopStyleColor();
    }
    if (preview.empty() && !open) {
      /* A placeholder, as Blender's empty object field. */
      const ImVec2 min = ImGui::GetItemRectMin();
      ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + style.FramePadding.x, min.y + style.FramePadding.y),
                                          ImGui::GetColorU32(ImGuiCol_TextDisabled), "Layer");
    }
    if (open) {
      for (const Layer *l : document_layers_flat(doc)) {
        if (l->type != LayerType::Vector) {
          continue;
        }
        if (ImGui::Selectable(l->name.c_str(), l->id == active->shape_layer)) {
          active->shape_layer = l->id;
          C.undo->push("Bone Shape Layer", doc);
        }
      }
      ImGui::EndCombo();
    }
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("OANIM_LAYER")) {
        uint64_t id = 0;
        std::memcpy(&id, payload->Data, sizeof(id));
        const Layer *dropped = layer_find(doc, id);
        if (dropped && dropped->type == LayerType::Vector) {
          active->shape_layer = id;
          C.undo->push("Bone Shape Layer", doc);
        }
      }
      ImGui::EndDragDropTarget();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The vector layer the bone is drawn as: its drawing where it sits at rest, moving with\n"
                        "the bone. Pick it here, drop one from the Layers editor, or use the eyedropper");
    }
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##shape_eyedropper", ImVec2(h, h))) {
      ui_layer_eyedropper_start(armature.id, active->id);
    }
    ui_eyedropper_icon(ImGui::GetWindowDrawList(), ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.32f,
                       IM_COL32(0xe6, 0xe6, 0xe6, 255));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Eyedropper: click a layer in the viewport or the Layers editor (Esc cancels)");
    }
    if (active->shape_layer) {
      ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
      if (ImGui::Button("X##clear_shape_layer", ImVec2(h, h))) {
        active->shape_layer = 0;
        C.undo->push("Bone Shape Layer", doc);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Clear: drawn as a bone until a layer is picked");
      }
    }
  }

  /* Bone Color: Blender's sets, each with its regular, selected and active shades. */
  props_label("Bone Color");
  {
    const float copy_w = h + style.ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(-copy_w);
    const float sw = h * 0.42f;
    const char *current = bone_color_set_name(active->color_set);
    if (ImGui::BeginCombo("##bone_color", "", ImGuiComboFlags_CustomPreview)) {
      for (int set = 0; set <= BONE_COLOR_CUSTOM; set++) {
        ImGui::PushID(set);
        const ImVec2 row = ImGui::GetCursorScreenPos();
        const bool chosen = ImGui::Selectable("##set", set == active->color_set, 0, ImVec2(0, ImGui::GetTextLineHeight()));
        const float text_x = row.x + (set > 0 && set < BONE_COLOR_CUSTOM ? sw * 3.0f + style.ItemInnerSpacing.x : 0.0f);
        if (set > 0 && set < BONE_COLOR_CUSTOM) {
          color_set_swatches(ImGui::GetWindowDrawList(), row, sw, ImGui::GetTextLineHeight(), bone_color_set(set));
        }
        ImGui::GetWindowDrawList()->AddText(ImVec2(text_x, row.y), ImGui::GetColorU32(ImGuiCol_Text),
                                            bone_color_set_name(set));
        if (chosen && set != active->color_set) {
          bone_set_color_set(*active, set);
          C.undo->push("Bone Color", doc);
        }
        ImGui::PopID();
      }
      ImGui::EndCombo();
    }
    if (ImGui::BeginComboPreview()) {
      float x = ImGui::GetCursorScreenPos().x;
      if (active->color_set > 0) {
        color_set_swatches(ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(), sw, ImGui::GetTextLineHeight(),
                           bone_colors(*active));
        x += sw * 3.0f + style.ItemInnerSpacing.x;
      }
      ImGui::GetWindowDrawList()->AddText(ImVec2(x, ImGui::GetCursorScreenPos().y), ImGui::GetColorU32(ImGuiCol_Text),
                                          current);
      ImGui::EndComboPreview();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The colours the bone is drawn in, as Blender's bone colours: its regular shade, and its\n"
                        "selected and active shades when picked. Default Colors: grey, orange when selected");
    }
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##copy_bone_color", ImVec2(h, h))) {
      operator_call(C, "armature.copy_bone_color_to_selected");
    }
    /* Two overlapping squares: copy. */
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 icon = IM_COL32(0xe6, 0xe6, 0xe6, 255);
    const float s = h * 0.24f;
    const ImVec2 c(at.x + h * 0.5f, at.y + h * 0.5f);
    dl->AddRect(ImVec2(c.x - s, c.y - s * 0.4f), ImVec2(c.x + s * 0.6f, c.y + s * 1.2f), icon, 1.0f, 0, 1.2f);
    dl->AddRect(ImVec2(c.x - s * 0.4f, c.y - s * 1.2f), ImVec2(c.x + s * 1.2f, c.y + s * 0.4f), icon, 1.0f, 0, 1.2f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Copy Bone Color to Selected: give the other selected bones this bone's colours");
    }
  }
  if (active->color_set == BONE_COLOR_CUSTOM) {
    props_label("Custom Colors");
    static const char *const shade_names[] = {"Regular", "Select", "Active"};
    const float gap = 1.0f;
    const float w = (ImGui::GetContentRegionAvail().x - 2.0f * gap) / 3.0f;
    for (int k = 0; k < 3; k++) {
      if (k > 0) {
        ImGui::SameLine(0.0f, gap);
      }
      Color &c = active->custom_colors[size_t(k)];
      float rgba[4] = {c.r, c.g, c.b, 1.0f};
      bool finished = false;
      if (ui_color_edit(shade_names[k], rgba, false, &finished, 0.0f, w)) {
        c = {rgba[0], rgba[1], rgba[2], 1.0f};
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", k == 0 ? "Regular: the bone as it is drawn" : k == 1 ? "Select: when it is selected"
                                                                                    : "Active: when it is the active bone");
      }
      if (finished) {
        C.undo->push("Bone Custom Color", doc);
      }
    }
  }
  ui_panel_end();
}

/* Blender's keyframe button for some of a layer's channels, on the next row. */
/* The pin button's state for some channels: -1 not shown (not animated and not pinned), 0 not
 * pinned, 1 pinned. */
static int pin_state(Layer &layer, const std::vector<std::string> &paths, int key_state)
{
  bool pinned = false;
  for (const std::string &p : paths) {
    pinned = pinned || layer_channel_pinned(layer, p);
  }
  return pinned ? 1 : key_state > 0 ? 0 : -1;
}

/* Pins or unpins channels (from their pin button). */
static void pin_toggle(Context &C, uint64_t layer, const std::string &paths, int pin)
{
  operator_call(C, "anim.channel_pin", {{"layer", std::to_string(layer)}, {"paths", paths}, {"pinned", pin == 1 ? "0" : "1"}});
}

static void key_next(Context &C, Layer &layer, std::vector<std::string> paths)
{
  const int state = layer_key_state(layer, paths, C.document->frame_current);
  const int pin = pin_state(layer, paths, state);
  const int action_bone_state = layer_action_bone_state(layer, paths);
  std::string joined;
  for (const std::string &p : paths) {
    joined += (joined.empty() ? "" : "|") + p;
  }
  const uint64_t id = layer.id;
  ui_key_next(
      state | action_bone_state,
      [&C, id, joined]() {
        operator_call(C, "anim.keyframe_toggle", {{"layer", std::to_string(id)}, {"paths", joined}});
      },
      pin, [&C, id, joined, pin]() { pin_toggle(C, id, joined, pin); }, id, paths.empty() ? std::string() : paths.front());
}

/* For a layer inside a bone layer: a bone it follows as a whole (Parent to Bone), or, for a
 * drawing, how its points are weighted to the bones. */
/* --- Changed on a reference ----------------------------------------------------------------------
 * A value a reference has changed from its original shows teal, as Blender shows overridden
 * properties, and right-clicking its row offers Reset to Original and Push to Original. The same
 * two words are used everywhere: Reset (back to the original) and Push (written into it). */

static const ImVec4 CHANGED_BG(0.09f, 0.31f, 0.32f, 1.0f), CHANGED_HOVER(0.12f, 0.40f, 0.41f, 1.0f),
    CHANGED_ACTIVE(0.15f, 0.48f, 0.49f, 1.0f);
static const ImVec4 CHANGED_TEXT(0.42f, 0.86f, 0.82f, 1.0f);

/* Tints the widgets after it teal while `on`. */
static bool changed_begin(bool on)
{
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, CHANGED_BG);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, CHANGED_HOVER);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, CHANGED_ACTIVE);
    ImGui::PushStyleColor(ImGuiCol_Button, CHANGED_BG);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CHANGED_HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, CHANGED_ACTIVE);
  }
  return on;
}

/* Ends the tint, and offers Reset and Push on a right-click anywhere on the row (in a panel's
 * table). `reset` and `push` are the operator calls for this change (`push` empty: it can't go
 * to the original). */
static void changed_end(Context &C, bool on, const char *id, const std::vector<OpArgs> &reset,
                        const std::vector<OpArgs> &push = {})
{
  if (!on) {
    return;
  }
  ImGui::PopStyleColor(6);
  const bool row_hovered = ImGui::GetCurrentTable() && ImGui::TableGetHoveredRow() == ImGui::TableGetRowIndex();
  if (row_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !ui_driver_menu_opened_this_frame()) {
    ImGui::OpenPopup(id);
  }
  if (row_hovered && !ImGui::IsPopupOpen(id) && ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
    ImGui::SetItemTooltip("Changed on this copy (right-click to reset or push it)");
  }
  if (ui_menu_begin(id)) {
    ImGui::TextDisabled("Changed on this copy");
    if (ui_menu_item("Reset to Original")) {
      for (const OpArgs &args : reset) {
        operator_call(C, "layer.reference_revert", args);
      }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Drop this change: it follows the original again");
    }
    ImGui::BeginDisabled(push.empty());
    if (ui_menu_item("Push to Original")) {
      for (const OpArgs &args : push) {
        operator_call(C, "layer.reference_apply", args);
      }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(push.empty() ? "This stays the copy's own" : "Write it into the original, so it and every copy get it");
    }
    ui_menu_end();
  }
}

/* The settings a look changes, as a list ("Fill Color, Line Width"). */
static std::string look_settings(const ReferenceLook &look)
{
  std::string s;
  const auto add = [&](bool on, const char *name) {
    if (on) {
      s += (s.empty() ? "" : ", ") + std::string(name);
    }
  };
  add(look.fill_set, "Fill Color");
  add(look.line_set, "Line Color");
  add(look.width_set, "Line Width");
  add(look.brush_set, "Brush");
  add(look.has_fill_set, "Fill");
  add(look.has_outline_set, "Outline");
  add(look.hidden, "Hidden");
  return s;
}

static bool same_color(Color a, Color b)
{
  return std::fabs(a.r - b.r) < 1e-4f && std::fabs(a.g - b.g) < 1e-4f && std::fabs(a.b - b.b) < 1e-4f &&
         std::fabs(a.a - b.a) < 1e-4f;
}

/* Two buttons, one lit: a choice between two ways (Pose, Points). Returns the one clicked, or -1. */
static int choice_buttons(const char *a, const char *b, int current, const char *tip_a, const char *tip_b)
{
  int clicked = -1;
  const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  for (int i = 0; i < 2; i++) {
    if (i > 0) {
      ImGui::SameLine();
    }
    if (i == current) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(i == 0 ? a : b, ImVec2(half, 0)) && i != current) {
      clicked = i;
    }
    if (i == current) {
      ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", i == 0 ? tip_a : tip_b);
    }
  }
  return clicked;
}

/* How wide a Reset / Push pair is. */
static float reset_push_width()
{
  return ImGui::CalcTextSize("Reset").x + ImGui::CalcTextSize("Push").x + ImGui::GetStyle().FramePadding.x * 4.0f +
         ImGui::GetStyle().ItemSpacing.x;
}

/* A small Reset / Push pair, at the right end of the line (`right`), or where the cursor is. */
static void reset_push(Context &C, const char *id, const OpArgs &args, bool can_push, bool right = true)
{
  ImGui::PushID(id);
  if (right) {
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - reset_push_width()));
  }
  if (ImGui::SmallButton("Reset")) {
    operator_call(C, "layer.reference_revert", args);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Reset to Original: drop this, it follows the original again");
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!can_push);
  if (ImGui::SmallButton("Push")) {
    operator_call(C, "layer.reference_apply", args);
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(can_push ? "Push to Original: write it into the original, so it and every copy get it"
                               : "This stays the copy's own");
  }
  ImGui::PopID();
}

/* Picks points of a reference (making it the active layer), so a change in the list shows where it is. */
static void reference_select(Context &C, Layer &ref, const std::vector<int> &points)
{
  for (MeshPoint &p : ref.drawing.points) {
    p.selected = false;
  }
  for (const int i : points) {
    if (i >= 0 && i < int(ref.drawing.points.size())) {
      ref.drawing.points[i].selected = true;
    }
  }
  C.document->active_layer = ref.id;
  C.undo->push("Select", *C.document);
}

static const char *channel_title(const std::string &name)
{
  return name == "position" ? "Location" : name == "rotation" ? "Rotation" : name == "scale" ? "Scale" : "Opacity";
}

/* Recolor: every style change at once (colours, line widths, brushes, fill and outline), back to
 * the original's or pushed into it. Each colour changed in the Shape tab has its own revert beside
 * it. */
static void reference_recolor(Context &C, Layer &ref)
{
  int styles = int(ref.reference.swaps.size());
  for (const auto &[id, look] : ref.reference.looks) {
    ReferenceLook rest = look;
    rest.hidden = false;
    styles += !rest.empty();
  }
  ImGui::BeginDisabled(styles == 0);
  const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  if (ImGui::Button("Reset All Shape Styles", ImVec2(half, 0))) {
    operator_call(C, "layer.reference_revert", {{"kind", "STYLES"}});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Every colour, line width, brush and fill or outline changed on this copy goes back to the\n"
                      "original's (hidden shapes stay hidden)");
  }
  ImGui::SameLine();
  if (ImGui::Button("Push All Shape Styles", ImVec2(half, 0))) {
    operator_call(C, "layer.reference_apply", {{"kind", "STYLES"}});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Write every style changed on this copy into the original, so it and every copy get them");
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled(styles ? "%d style change%s on this copy." : "Styles match the original.", styles, styles == 1 ? "" : "s");
}

/* The changes a copy has, grouped, each group folding open to its items: clicking an item picks it
 * in the viewport; each has Reset and Push, and so does each group. */
static void reference_changes(Context &C, Layer &ref, const Layer *source)
{
  ReferenceSettings &rs = ref.reference;
  const auto group = [&](const char *title, int count, const OpArgs &args, bool can_push) {
    if (count == 0) {
      return false;
    }
    ImGui::AlignTextToFramePadding();
    const bool open = ui_subpanel_begin(std::string("Changes/") + title,
                                        std::string(title) + " (" + std::to_string(count) + ")###" + title, true);
    reset_push(C, title, args, can_push);
    if (open) {
      /* Its rows are indented under it, as a tree node's. */
      ImGui::Indent();
    }
    return open;
  };
  if (group("Colors", int(rs.swaps.size()), {{"kind", "SWAPS"}}, true)) {
    for (size_t i = 0; i < rs.swaps.size(); i++) {
      const ReferenceSwap &sw = rs.swaps[i];
      ImGui::PushID(int(i));
      const float h = ImGui::GetTextLineHeight();
      ImGui::ColorButton("##from", ImVec4(sw.from.r, sw.from.g, sw.from.b, sw.from.a), ImGuiColorEditFlags_NoTooltip, ImVec2(h * 1.6f, h));
      ImGui::SameLine();
      ImGui::TextUnformatted("->");
      ImGui::SameLine();
      ImGui::ColorButton("##to", ImVec4(sw.to.r, sw.to.g, sw.to.b, sw.to.a), ImGuiColorEditFlags_NoTooltip, ImVec2(h * 1.6f, h));
      reset_push(C, "##rp", {{"kind", "SWAP"}, {"id", std::to_string(i)}}, true);
      ImGui::PopID();
    }
    ImGui::Unindent();
    ui_subpanel_end();
  }
  if (group("Shapes", int(rs.looks.size()), {{"kind", "LOOKS"}}, true)) {
    for (const auto &[id, look] : rs.looks) {
      const int si = drawing_shape_index(ref.drawing, id);
      const std::string name = si >= 0 ? ref.drawing.shapes[si].name : std::string("(gone from the original)");
      ImGui::PushID(int(id));
      if (ImGui::Selectable((name + ": " + look_settings(look)).c_str(), false, ImGuiSelectableFlags_AllowOverlap) && si >= 0) {
        reference_select(C, ref, shape_points(ref.drawing, ref.drawing.shapes[si]));
      }
      ReferenceLook rest = look;
      rest.hidden = false;
      reset_push(C, "##rp", {{"kind", "LOOK"}, {"id", std::to_string(id)}}, si >= 0 && !rest.empty());
      ImGui::PopID();
    }
    ImGui::Unindent();
    ui_subpanel_end();
  }
  /* Points, with their handles. */
  std::set<uint64_t> moved;
  for (const auto &[id, o] : rs.points) {
    moved.insert(id);
  }
  for (const auto &[key, o] : rs.handles) {
    moved.insert(key.point);
  }
  if (group("Points", int(moved.size()), {{"kind", "POINTS"}}, true)) {
    int shown = 0;
    for (const uint64_t id : moved) {
      if (++shown > 30) {
        ImGui::TextDisabled("and %d more", int(moved.size()) - 30);
        break;
      }
      const int pi = drawing_point_index(ref.drawing, id);
      const bool point = rs.points.count(id) > 0;
      const bool handles = std::any_of(rs.handles.begin(), rs.handles.end(), [&](const auto &e) { return e.first.point == id; });
      bool keyed = point && rs.points.at(id).offset_keys.animated();
      for (const auto &[key, o] : rs.handles) {
        keyed |= key.point == id && o.offset_keys.animated();
      }
      const std::string label = (pi >= 0 ? "Point " + std::to_string(pi + 1) : std::string("(gone from the original)")) +
                                (point && handles ? ": moved, handles" : point ? ": moved" : ": handles") +
                                (keyed ? " (keyed)" : "");
      ImGui::PushID(int(id));
      if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowOverlap) && pi >= 0) {
        reference_select(C, ref, {pi});
      }
      reset_push(C, "##rp", {{"kind", "POINT"}, {"id", std::to_string(id)}}, pi >= 0);
      ImGui::PopID();
    }
    ImGui::Unindent();
    ui_subpanel_end();
  }
  if (group("Layer Animation", int(rs.own_channels.size()), {{"kind", "CHANNELS"}}, source != nullptr)) {
    for (const std::string &name : rs.own_channels) {
      ImGui::PushID(name.c_str());
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(channel_title(name));
      reset_push(C, "##rp", {{"kind", "CHANNEL"}, {"path", name}}, source != nullptr);
      ImGui::PopID();
    }
    ImGui::Unindent();
    ui_subpanel_end();
  }
  if (group("Bone Weights", rs.weights.empty() ? 0 : 1, {{"kind", "WEIGHTS"}}, false)) {
    ImGui::TextDisabled("%d points move by this copy's own weights", int(rs.weights.size()));
    ImGui::Unindent();
    ui_subpanel_end();
  }
}

/* The Reference tab: everything about a copy's link to its original in one place. */
static void reference_tab(Context &C)
{
  Document &doc = *C.document;
  Layer *ref = document_active_layer(doc);
  if (!ref || !ref->is_reference()) {
    ImGui::TextDisabled("Select a reference layer.");
    return;
  }
  const Layer *source = layer_find(doc, ref->reference_source);
  /* Whose copy, and how much it differs. */
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("Copy of");
  ImGui::SameLine();
  ImGui::TextUnformatted(source ? source->name.c_str() : "(a deleted layer)");
  if (source) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Go to Original")) {
      doc.active_layer = source->id;
      C.undo->push("Active Layer", doc);
      return;
    }
  }
  const int changes = reference_change_count(*ref);
  if (changes == 0) {
    ImGui::TextDisabled("No changes: it matches the original.");
  }
  else {
    ImGui::TextColored(CHANGED_TEXT, "%d change%s on this copy", changes, changes == 1 ? "" : "s");
  }
  if (ref->is_group()) {
    ImGui::TextWrapped("Each layer inside is a copy of its own: select one to change it.");
  }
  ReferenceSettings &rs = ref->reference;
  if (!ref->is_group()) {
    if (ui_collapsing_header("Pose", true)) {
      const int pose = choice_buttons("Same as Original", "Own Bones", rs.match_original ? 0 : 1,
                                      "Posed by the original's bones (action bones and dynamics too), wherever this copy sits",
                                      "Posed by this copy's own bone layer, with its own weights (starting as the "
                                      "original's, by bone name)");
      if (pose >= 0) {
        operator_call(C, "layer.reference_set", {{"match_original", pose == 0 ? "1" : "0"}});
      }
      ImGui::TextDisabled(rs.match_original ? "It moves exactly like the original, wherever it is."
                                            : "It moves with the bones of the bone layer it's in.");
    }
    if (ui_collapsing_header("Points", true)) {
      const int points = choice_buttons("Locked", "Editable", rs.edit_points ? 1 : 0,
                                        "Its points stay where the original has them; only looks can change here",
                                        "Its points can move here (kept as changes on top of the original, its motion "
                                        "still coming through)");
      if (points >= 0) {
        operator_call(C, "layer.reference_set", {{"edit_points", points == 1 ? "1" : "0"}});
      }
      ImGui::TextDisabled(rs.edit_points ? "Moves made here are kept on top of the original's."
                                         : "Unlock to move its points (also the lock in the viewport header).");
    }
    if (source) {
      if (ui_collapsing_header("Recolor", true)) {
        reference_recolor(C, *ref);
      }
    }
  }

  const std::string changes_title = changes ? "Changes on This Copy (" + std::to_string(changes) + ")###changes"
                                              : std::string("Changes on This Copy (none)###changes");
  if (ui_collapsing_header(changes_title.c_str(), false)) {
    /* Each group has its own Reset and Push (no all-at-once pair below them). */
    reference_changes(C, *ref, source);
    if (!changes) {
      ImGui::TextDisabled("Everything matches the original.");
    }
  }
  if (ui_collapsing_header("More", false)) {
    if (!ref->is_group()) {
      bool follow = rs.follow_animation;
      if (ImGui::Checkbox("Follow Original's Animation", &follow)) {
        operator_call(C, "layer.reference_set", {{"follow_animation", follow ? "1" : "0"}});
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("On: the original's animation comes through. Off: only its setup shape, with this\n"
                          "copy's own point keys.");
      }
    }
    if (ImGui::Button("Remove Unused Changes")) {
      operator_call(C, "layer.reference_remove_unused");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Drop changes to points and shapes the original no longer has");
    }
  }
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.35f, 1.0f), "Break Reference");
  ImGui::TextWrapped("Makes it an ordinary layer showing what it shows now; it stops following the original.");
  if (ImGui::Button("Break Reference")) {
    operator_call(C, "layer.break_reference");
  }
}

/* What Custom Weights comes to for a layer, or for a group the drawings in it: the bones its points are weighted to
 * and those that move what's inside as a whole. Bone layers inside follow their own bones. */
static void custom_weights_in(const Layer &layer, std::set<uint64_t> &r_bones)
{
  for (const MeshPoint &p : layer.drawing.points) {
    for (const BoneWeight &w : p.weights) {
      if (w.bone != 0 && w.weight > 0.0f) {
        r_bones.insert(w.bone);
      }
    }
  }
  for (const Layer &child : layer.children) {
    if (child.parent_bone != 0) {
      r_bones.insert(child.parent_bone);
    }
    if (!child.is_bone_layer()) {
      custom_weights_in(child, r_bones);
    }
  }
}

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
  /* A group above with a Parent Bone moves this layer as a whole: its bone shows here, greyed. */
  const Layer *binding = layer_binding_group(doc, layer.id);
  const Bone *parent = bone_find(armature, binding ? binding->parent_bone : layer.parent_bone);
  /* A copy posed Same as Original moves with its original's bones: no bone of its own until it is
   * given Own Bones (the Reference tab). */
  const bool same_as_original = layer.is_reference() && layer.reference.match_original;
  ImGui::BeginDisabled(doc.frame_current != 0 || binding || same_as_original);
  /* Keep Animation (the checkbox under it): a change of bone keys the layer where it showed. */
  static bool keep_animation = false;
  const std::string keep_arg = keep_animation ? "1" : "0";
  /* The eyedropper beside it: click the bone in the viewport. */
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 2.0f);
  /* Anything with points follows their weights unless it has a bone, and the box says which: a layer never follows a
   * bone nothing shows. Images, video and text have no points to weigh. */
  const bool weighable = layer.type == LayerType::Vector || layer.is_group();
  const char *const no_bone = weighable ? "Custom Weights" : "None";
  std::set<uint64_t> weighted;
  custom_weights_in(layer, weighted);
  std::string shown = no_bone;
  if (parent) {
    shown = parent->name;
  }
  else if (weighable && weighted.empty()) {
    shown += " (no weights)";
  }
  else if (const Bone *only = weighted.size() == 1 ? bone_find(armature, *weighted.begin()) : nullptr) {
    shown += ": " + only->name;
  }
  else if (weighable) {
    shown += ": " + std::to_string(weighted.size()) + " bones";
  }
  if (ImGui::BeginCombo("##parent_bone", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
    /* A search field on top, focused as it opens: typing narrows the list (any part of the name, any
     * case); Enter takes the first bone left. */
    static std::string search;
    if (ImGui::IsWindowAppearing()) {
      search.clear();
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputTextWithHint("##search", "Search bones", &search,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    const auto matches = [&](const std::string &name) {
      std::string lower = name;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
      return lower.find(needle) != std::string::npos;
    };
    if (needle.empty()) {
      if (ImGui::Selectable(no_bone, !parent)) {
        operator_call(C, "layer.parent_bone_set", {{"bone", ""}, {"keep_animation", keep_arg}});
      }
      ImGui::Separator();
    }
    const Bone *first = nullptr;
    for (const Bone &b : armature.bones) {
      if (!matches(b.name)) {
        continue;
      }
      first = first ? first : &b;
      if (ImGui::Selectable(b.name.c_str(), parent && b.id == parent->id)) {
        operator_call(C, "layer.parent_bone_set", {{"bone", b.name}, {"keep_animation", keep_arg}});
      }
    }
    if (!first && !needle.empty()) {
      ImGui::TextDisabled("No bone named like that");
    }
    if (entered && first) {
      operator_call(C, "layer.parent_bone_set", {{"bone", first->name}, {"keep_animation", keep_arg}});
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndCombo();
  }
  const bool combo_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled);
  ImGui::SameLine(0, 2);
  const bool picking = ui_bone_eyedropper_armature() == armature.id;
  if (ui_pick_button("##pick_parent_bone", "Eyedropper: click the bone in the viewport", picking)) {
    const uint64_t layer_id = layer.id;
    const std::string layer_name = layer.name;
    ui_bone_eyedropper_start(
        armature.id,
        [layer_id, keep_arg](Context &C, uint64_t bone_id) {
          const Layer *rig = layer_armature(*C.document, layer_id);
          const Bone *bone = rig ? bone_find(*rig, bone_id) : nullptr;
          if (bone) {
            /* As the dropdown: the active and selected layers. */
            operator_call(C, "layer.parent_bone_set", {{"bone", bone->name}, {"keep_animation", keep_arg}});
          }
          return true;
        },
        [layer_name] { return "Click the bone " + layer_name + " follows  |  Esc cancels"; });
  }
  ImGui::EndDisabled();
  if (combo_hovered) {
    if (binding) {
      ImGui::SetTooltip("Set by the group \"%s\": its Parent Bone moves everything inside it", binding->name.c_str());
    }
    else if (same_as_original) {
      ImGui::SetTooltip("Its pose is Same as Original: it moves with the original's bones.\n"
                        "Choose Own Bones in the Reference tab to give it a parent bone");
    }
    else {
      ImGui::SetTooltip(doc.frame_current != 0 ? "Switch to Setup (frame 0)"
                                               : "Pick a bone and the whole layer follows it, as parenting to a bone in "
                                                 "Blender. Custom Weights: its points follow their own weights");
    }
  }
  /* Keep Animation: changing the bone leaves the layer where it showed on every keyed frame (Keep
   * Poses' reverse: the setup frame is left alone, the poses are kept). */
  if (parent && !binding) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginDisabled(doc.frame_current != 0 || same_as_original || layer.points_follow_bone());
    ImGui::Checkbox("Keep Animation##parent_keep", &keep_animation);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Changing the parent bone keeps the layer where it shows on every keyed frame: its position, "
                        "rotation and scale are keyed again there against the new bone.\nOff, it follows the new "
                        "bone's motion from its setup pose (Deform: Layer only)");
    }
  }
  /* Deform: what the bone moves, the layer as a whole or its points. A group moves as a whole. */
  if (parent && !binding) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Deform");
    ImGui::TableSetColumnIndex(1);
    const bool usable = !same_as_original && !layer.is_group() && doc.frame_current == 0;
    ImGui::BeginDisabled(!usable);
    const float w = (ImGui::GetContentRegionAvail().x - 1.0f) * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, ImGui::GetStyle().ItemSpacing.y));
    static const char *const NAMES[] = {"Layer", "Points"};
    static const char *const TIPS[] = {
        "The bone moves the layer as a whole: its squash and stretch squashes the lines and brush stamps too",
        "The bone moves the points, as if each were weighted all to it: lines keep their width and brush "
        "stamps their shape"};
    const int now = layer.parent_bone_points && !layer.is_group() ? 1 : 0;
    for (int k = 0; k < 2; k++) {
      if (k > 0) {
        ImGui::SameLine();
      }
      if (k == now) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      if (ImGui::Button(NAMES[k], ImVec2(w, 0)) && k != now) {
        operator_call(C, "layer.parent_bone_set", {{"deform", k == 1 ? "POINTS" : "LAYER"}});
      }
      if (k == now) {
        ImGui::PopStyleColor();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", layer.is_group() ? "A group moves as a whole" : TIPS[k]);
      }
    }
    ImGui::PopStyleVar();
    ImGui::EndDisabled();
  }
  /* Bone Follows Selection: the parent bone keyed onto the selection on each of its keyed frames. */
  if (parent && (layer.has_drawing() || layer.is_group())) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginDisabled(same_as_original || (doc.frame_current == 0 && doc.action.empty()));
    if (ImGui::Button("Bone Follows Selection", ImVec2(-1, 0))) {
      operator_call(C, "layer.bone_follow_selection");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("On every frame the selected points (all, when none are) are keyed in this timeline,\n"
                        "key the parent bone where they show, keeping the drawings where they are (Move Only\n"
                        "Bones). For a bone that follows a feature through an action bone action, as an eye through a\n"
                        "head turn. Keep Offset and Fit (Move & Turn, Move, Turn & Scale) in Adjust Last Operation");
    }
  }
  if (layer.type == LayerType::Vector) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped(parent ? "The layer follows its parent bone. Giving its points weights switches it to Custom "
                                "Weights."
                              : "Points follow the bones by their weights, and a new point takes the average of the "
                                "points next to it. Make and change them with the Weight Select and Weight Paint "
                                "tools.");
  }
  ui_panel_end();
}

/* --- Modifiers tab, laid out like Blender's ------------------------------------------------ */

static constexpr ImU32 COL_MOD_BOX = IM_COL32(0x3d, 0x3d, 0x3d, 255);
static constexpr ImU32 COL_MOD_ICON = IM_COL32(0xd0, 0xd0, 0xd0, 255);
static constexpr const char *MODIFIER_PAYLOAD = "OANIM_MODIFIER";

/* A small square toggle with a drawn icon, blue when on, as Blender's header toggles. */
/* `driven`: wrapped in ui_driver_field_begin / _end and driven, so it takes the driven field's colour
 * (on dimmed when off) and leaves its tooltip to the driver's, and a click changes nothing: the
 * driver would put it back. */
static bool icon_toggle(const char *id, bool on, const char *tooltip, void (*icon)(ImDrawList *, ImVec2, float),
                        bool driven = false)
{
  const float h = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImU32 back = on ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                  : ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
  if (driven) {
    back = ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg, on ? 1.0f : 0.45f);
  }
  dl->AddRectFilled(at, ImVec2(at.x + h, at.y + h), back, 3.0f);
  icon(dl, ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.32f);
  if (!driven && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  return pressed && !driven;
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

/* Show Points: a line with hollow points on it, in the teal the viewport marks them with. */
static void icon_points(ImDrawList *dl, ImVec2 c, float s)
{
  const ImU32 teal = IM_COL32(0x2e, 0xc4, 0xb6, 255);
  dl->AddBezierCubic(ImVec2(c.x - s, c.y + s * 0.6f), ImVec2(c.x - s * 0.4f, c.y - s * 0.9f),
                     ImVec2(c.x + s * 0.4f, c.y + s * 0.9f), ImVec2(c.x + s, c.y - s * 0.6f), COL_MOD_ICON, 1.0f);
  for (const float t : {0.0f, 0.5f, 1.0f}) {
    const float u = 1.0f - t;
    const ImVec2 p(c.x - s * u * u * u - s * 0.4f * 3.0f * u * u * t + s * 0.4f * 3.0f * u * t * t + s * t * t * t,
                   c.y + s * 0.6f * u * u * u - s * 0.9f * 3.0f * u * u * t + s * 0.9f * 3.0f * u * t * t -
                       s * 0.6f * t * t * t);
    dl->AddCircle(p, s * 0.32f, teal, 0, 1.4f);
  }
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
  else if (type == ModifierType::CurveDeform) {
    /* A line bent along a curve, its points on it. */
    ImVec2 pts[9];
    for (int k = 0; k < 9; k++) {
      const float t = float(k) / 8.0f;
      pts[k] = ImVec2(c.x - s + 2.0f * s * t, c.y + s * 0.7f - std::sin(t * 3.1415927f) * s * 1.2f);
    }
    dl->AddPolyline(pts, 9, COL_MOD_ICON, 0, 1.3f);
    for (const int k : {0, 4, 8}) {
      dl->AddCircleFilled(pts[k], s * 0.2f, COL_MOD_ICON);
    }
  }
  else if (type == ModifierType::FollowDeform) {
    /* A curve, and a point riding it. */
    dl->AddBezierCubic(ImVec2(c.x - s, c.y + s * 0.6f), ImVec2(c.x - s * 0.4f, c.y - s * 0.9f), ImVec2(c.x + s * 0.4f, c.y + s * 0.9f),
                       ImVec2(c.x + s, c.y - s * 0.6f), COL_MOD_ICON, 1.3f);
    dl->AddCircleFilled(c, s * 0.38f, COL_MOD_ICON);
  }
  else if (type == ModifierType::CageDeform) {
    /* A closed, pulled outline, its points on it. */
    ImVec2 pts[16];
    for (int k = 0; k < 16; k++) {
      const float a = float(k) / 16.0f * 6.2831853f;
      const float r = s * (0.8f + 0.2f * std::cos(a * 2.0f));
      pts[k] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r * 0.8f);
    }
    dl->AddPolyline(pts, 16, COL_MOD_ICON, ImDrawFlags_Closed, 1.3f);
    for (const int k : {0, 4, 8, 12}) {
      dl->AddCircleFilled(pts[k], s * 0.2f, COL_MOD_ICON);
    }
  }
  else if (modifier_is_deform(type)) {
    /* A grid bowed upward in the middle, as a pull bends it. */
    const auto at = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s - (1.0f - x * x) * 0.3f * s); };
    for (int k = 0; k < 3; k++) {
      const float y = float(k - 1) * 0.75f;
      ImVec2 row[7];
      for (int q = 0; q < 7; q++) {
        row[q] = at(float(q - 3) / 3.0f, y);
      }
      dl->AddPolyline(row, 7, COL_MOD_ICON, 0, 1.2f);
    }
    for (int k = 0; k < 3; k++) {
      const float x = float(k - 1) * 0.75f;
      dl->AddLine(at(x, -0.75f), at(x, 0.75f), COL_MOD_ICON, 1.2f);
    }
  }
  else if (type == ModifierType::Resample) {
    /* A curve with points along it. */
    ImVec2 pts[7];
    for (int k = 0; k < 7; k++) {
      const float t = float(k) / 6.0f;
      pts[k] = ImVec2(c.x - s + 2.0f * s * t, c.y + s * 0.6f * std::sin(t * 6.2831853f));
    }
    dl->AddPolyline(pts, 7, COL_MOD_ICON, 0, 1.3f);
    for (int k = 0; k < 7; k += 2) {
      dl->AddCircleFilled(pts[k], s * 0.18f, COL_MOD_ICON);
    }
  }
  else if (type == ModifierType::Bevel) {
    /* A square with its corner cut off. */
    const ImVec2 pts[5] = {ImVec2(c.x - s, c.y + s), ImVec2(c.x - s, c.y - s), ImVec2(c.x + s * 0.2f, c.y - s),
                           ImVec2(c.x + s, c.y - s * 0.2f), ImVec2(c.x + s, c.y + s)};
    dl->AddPolyline(pts, 5, COL_MOD_ICON, ImDrawFlags_Closed, 1.3f);
  }
  else if (type == ModifierType::Boolean) {
    dl->AddCircle(ImVec2(c.x - s * 0.35f, c.y), s * 0.65f, COL_MOD_ICON, 0, 1.3f);
    dl->AddCircleFilled(ImVec2(c.x + s * 0.35f, c.y), s * 0.65f, COL_MOD_ICON);
  }
  else {
    const ImVec2 head(c.x - s * 0.8f, c.y + s * 0.8f), tail(c.x + s * 0.8f, c.y - s * 0.8f);
    const ImVec2 mid(c.x - s * 0.45f, c.y + s * 0.45f), n(s * 0.3f, s * 0.3f);
    const ImVec2 quad[4] = {head, ImVec2(mid.x + n.x, mid.y + n.y), tail, ImVec2(mid.x - n.x, mid.y - n.y)};
    dl->AddPolyline(quad, 4, COL_MOD_ICON, ImDrawFlags_Closed, 1.3f);
  }
}

/* One row of a modifier's body: a right-aligned label, then the value from the middle. */
static void modifier_row_label(const char *label, float column, const char *tip = nullptr)
{
  const float x = ImGui::GetCursorPosX();
  const float w = ImGui::CalcTextSize(label).x;
  ImGui::SetCursorPosX(x + std::max(0.0f, column - w - ImGui::GetStyle().ItemSpacing.x));
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  /* Hovering the name explains the setting. */
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tip);
  }
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

using SegmentIcon = std::function<void(ImDrawList *, ImVec2, float, int, ImU32)>;
static int segmented(const char *id, int value, int count, const char *const *names, const char *const *tips,
                     const SegmentIcon &icon = {});

/* "|" between ids, as the Boolean operator takes shapes and layers. */
static std::string id_list(const std::vector<uint64_t> &ids)
{
  std::string out;
  for (const uint64_t id : ids) {
    out += (out.empty() ? "" : "|") + std::to_string(id);
  }
  return out;
}

static bool search_matches(const std::string &name, const std::string &needle);

/* Which shapes of `drawing` a Boolean takes, picked in a popup of checkboxes ("All shapes" for every
 * one going all the way round). Returns true, with the new picks, when they change. */
static bool merge_shape_picker(const char *id, const VectorDrawing &drawing, const std::vector<uint64_t> &picked,
                               std::vector<uint64_t> *r_picked)
{
  std::string label;
  if (picked.empty()) {
    label = "All shapes";
  }
  else {
    for (const Shape &s : drawing.shapes) {
      if (std::find(picked.begin(), picked.end(), s.id) != picked.end()) {
        label += (label.empty() ? "" : ", ") + s.name;
      }
    }
  }
  bool changed = false;
  ImGui::PushID(id);
  const float width = ImGui::CalcItemWidth();
  if (ImGui::Button((label + "##pick").c_str(), ImVec2(width, 0))) {
    ImGui::OpenPopup("##shapes");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Which shapes merge: all that go all the way round, or the ones ticked");
  }
  if (ImGui::BeginPopup("##shapes")) {
    static std::string search;
    if (ImGui::IsWindowAppearing()) {
      search.clear();
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(std::max(ImGui::CalcItemWidth(), 160.0f));
    ImGui::InputTextWithHint("##search", "Search shapes", &search);
    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    bool all = picked.empty();
    if (ImGui::Checkbox("All shapes", &all) && all) {
      r_picked->clear();
      changed = true;
    }
    ImGui::Separator();
    for (const Shape &s : drawing.shapes) {
      if (!merge_takes_shape(drawing, s, {}) || !search_matches(s.name, needle)) {
        continue;
      }
      bool on = std::find(picked.begin(), picked.end(), s.id) != picked.end();
      ImGui::PushID(int(s.id));
      if (ImGui::Checkbox(s.name.c_str(), &on)) {
        *r_picked = picked;
        if (on) {
          r_picked->push_back(s.id);
        }
        else {
          std::erase(*r_picked, s.id);
        }
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return changed;
}

/* Whether `name` holds `needle` (already lower case), any case: the dropdowns' search. */
static bool search_matches(const std::string &name, const std::string &needle)
{
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
  return lower.find(needle) != std::string::npos;
}

/* A dropdown of layers as Parent Bone's is of bones: a search field on top, focused as it opens
 * (any part of the name, any case); Enter takes the first layer left. Returns the layer picked, or 0. */
static uint64_t layer_search_combo(const char *id, const char *preview, const std::vector<const Layer *> &choices,
                                   uint64_t current)
{
  uint64_t picked = 0;
  if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge)) {
    static std::string search;
    if (ImGui::IsWindowAppearing()) {
      search.clear();
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputTextWithHint("##search", "Search layers", &search,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    const Layer *first = nullptr;
    for (const Layer *l : choices) {
      if (!search_matches(l->name, needle)) {
        continue;
      }
      first = first ? first : l;
      ImGui::PushID(int(l->id));
      if (ImGui::Selectable(l->name.c_str(), l->id == current) && l->id != current) {
        picked = l->id;
      }
      ImGui::PopID();
    }
    if (!first) {
      ImGui::TextDisabled(needle.empty() ? "No other drawing layers" : "No layer named like that");
    }
    if (entered && first) {
      picked = first->id != current ? first->id : 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndCombo();
  }
  return picked;
}

/* The Boolean eyedropper: a shape clicked in the viewport, and its layer, go into Boolean `index` of
 * layer `owner`, in place of layer `replace` (0: as a new one). It looks past the owner, which may
 * be drawn over what it merges; a shape not going all the way round can't merge, so it waits for
 * another click. */
static void merge_eyedropper_start(uint64_t owner, int index, uint64_t replace, const std::string &tag)
{
  ui_shape_eyedropper_start(
      [owner, index, replace](Context &C, uint64_t layer_id, int shape_index) {
        Document &doc = *C.document;
        Layer *own = layer_find(doc, owner);
        const Layer *clicked = layer_find(doc, layer_id);
        if (!own || index >= int(own->modifiers.size()) || own->modifiers[size_t(index)].type != ModifierType::Boolean) {
          return true;
        }
        if (!clicked || clicked->id == owner || clicked->type != LayerType::Vector) {
          return false;
        }
        const VectorDrawing shown = layer_evaluated_drawing(doc, *clicked, false);
        if (shape_index < 0 || shape_index >= int(shown.shapes.size())) {
          return false;
        }
        const Shape &shape = shown.shapes[size_t(shape_index)];
        /* The Boolean's layer is the one the operator changes. */
        doc.active_layer = owner;
        OpArgs args{{"index", std::to_string(index)},
                    {"pick", std::to_string(clicked->id)},
                    {"pick_shape", std::to_string(shape.id)}};
        if (replace != 0) {
          args["replace"] = std::to_string(replace);
        }
        /* A shape that can't merge (not going all the way round) waits for another click. */
        return operator_call(C, "object.modifier_boolean_set", args) == OpStatus::Finished;
      },
      [owner] { return std::string("Click a shape to merge it in  |  Esc cancels"); }, tag.c_str(), owner);
}

static void merge_drawing_layers(const std::vector<Layer> &list, std::vector<const Layer *> &r)
{
  for (const Layer &l : list) {
    if (l.type == LayerType::Vector) {
      r.push_back(&l);
    }
    merge_drawing_layers(l.children, r);
  }
}

/* A Boolean modifier's body: the layer's own shapes, the layers merged in (each with its shapes), the
 * operation, Smoothing (keyable) and Corners Only. Apply is the header menu's (Ctrl A). */
static void merge_modifier_body(Context &C, Layer &layer, Modifier &m, int index, float column)
{
  Document &doc = *C.document;
  const std::string idx = std::to_string(index);
  const auto set = [&](std::initializer_list<std::pair<const std::string, std::string>> args) {
    OpArgs a(args);
    a["index"] = idx;
    operator_call(C, "object.modifier_boolean_set", a);
  };
  std::vector<uint64_t> picks;
  modifier_row_label("Shapes", column,
                     "This layer's shapes the others merge into; the result takes the first one's place and look");
  if (merge_shape_picker("##own", layer.drawing, m.merge_shapes, &picks)) {
    set({{"shapes", id_list(picks)}});
  }

  /* The layers merged in: a row each (the layer, as Parent Bone's dropdown with its search; its
   * shapes; the eyedropper; x), then one to add a layer. */
  modifier_row_label("Layers", column,
                     "The layers whose shapes join this layer's, as they're posed. Shapes merged in stop drawing on "
                     "their own layer, so nothing is drawn twice. The eyedropper takes a shape you click, and its layer");
  std::vector<uint64_t> ids;
  for (const MergeTarget &t : m.merge_targets) {
    ids.push_back(t.layer);
  }
  std::vector<const Layer *> drawings;
  merge_drawing_layers(doc.layers, drawings);
  const float h = ImGui::GetFrameHeight();
  int remove = -1;
  for (int t = 0; t <= int(m.merge_targets.size()); t++) {
    const bool adding = t == int(m.merge_targets.size());
    const MergeTarget target = adding ? MergeTarget{} : m.merge_targets[t];
    const Layer *l = adding ? nullptr : layer_find(doc, target.layer);
    ImGui::PushID(t);
    if (t > 0) {
      modifier_row_label("", column);
    }
    /* Layers it can take: drawings other than this one and the ones already in. */
    std::vector<const Layer *> choices;
    for (const Layer *d : drawings) {
      if (d->id != layer.id && (d->id == target.layer || std::find(ids.begin(), ids.end(), d->id) == ids.end())) {
        choices.push_back(d);
      }
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    const float layer_w = adding ? avail - h - 2.0f : (avail - h * 2.0f - 8.0f) * 0.5f;
    /* Hidden, a layer isn't joined; it still cuts (Subtract, Intersect), as a mask shape does. */
    const bool shown = adding || (l && (layer_shown_in_document(doc, target.layer) ||
                                        m.merge_operation != MergeOperation::Union));
    ImGui::SetNextItemWidth(layer_w);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(shown ? ImGuiCol_Text : ImGuiCol_TextDisabled));
    const uint64_t picked = layer_search_combo("##layer", adding ? "Add Layer..." : l ? l->name.c_str() : "(gone)",
                                               choices, target.layer);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(adding ? "Add another layer: pick it, or click one of its shapes with the eyedropper"
                        : !shown ? "Hidden: it isn't joined while it's hidden (with Subtract or Intersect it still "
                                   "cuts, as a mask shape does)"
                                 : "A layer merged in: pick another to merge it instead");
    }
    if (picked != 0) {
      std::vector<uint64_t> next = ids;
      if (adding) {
        next.push_back(picked);
      }
      else {
        next[size_t(t)] = picked;
      }
      set({{"targets", id_list(next)}});
    }
    if (!adding) {
      ImGui::SameLine(0, 2);
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - h * 2.0f - 6.0f);
      std::vector<uint64_t> target_picks;
      if (l && merge_shape_picker("##target_shapes", l->drawing, target.shapes, &target_picks)) {
        set({{"target", std::to_string(target.layer)}, {"target_shapes", id_list(target_picks)}});
      }
    }
    ImGui::SameLine(0, 2);
    const std::string tag = "BOOLEAN:" + std::to_string(layer.id) + ":" + idx + ":" + std::to_string(target.layer);
    if (ui_pick_button("##pick", adding ? "Eyedropper: click a shape to merge in (its layer comes with it)"
                                        : "Eyedropper: click a shape to merge in its place (its layer comes with it)",
                       ui_shape_eyedropper_active(tag.c_str())))
    {
      merge_eyedropper_start(layer.id, index, target.layer, tag);
    }
    if (!adding) {
      ImGui::SameLine(0, 2);
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
      if (ImGui::Button("x", ImVec2(h, 0))) {
        remove = t;
      }
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Stop merging this layer");
      }
    }
    ImGui::PopID();
  }
  if (remove >= 0) {
    std::vector<uint64_t> rest = ids;
    rest.erase(rest.begin() + remove);
    set({{"targets", id_list(rest)}});
  }
  ImGui::Spacing();

  static const char *const OP_NAMES[] = {"Union", "Subtract", "Intersect"};
  static const char *const OP_TIPS[] = {
      "One outline round them all: the corners where they meet can be rounded",
      "This layer's shapes with the others cut away, the outline running on along the cut",
      "Only where this layer's shapes and the others overlap"};
  static const char *const OP_IDS[] = {"UNION", "SUBTRACT", "INTERSECT"};
  modifier_row_label("Operation", column, "How the other layers' shapes go with this layer's");
  const int op = segmented("##operation", int(m.merge_operation), 3, OP_NAMES, OP_TIPS, {});
  if (op >= 0 && op != int(m.merge_operation)) {
    set({{"operation", OP_IDS[op]}});
  }

  /* Smoothing: keyable, with its key button, as a mask's Reach. */
  const std::string path = "modifiers[" + idx + "].smoothing";
  const int key_state = layer_key_state(layer, {path}, doc.frame_current);
  const int action_bone_smoothing = layer_action_bone_state(layer, {path});
  modifier_row_label("Smoothing", column,
                     "Rounds the corners where the outlines meet, as a radius: 0 keeps them sharp. Only there: the "
                     "shapes' own corners stay as drawn. It can be keyed");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
  const int tint = ui_key_tint_push(key_state | action_bone_smoothing);
  const int driven = ui_driver_field_begin(C, layer.id, path, -1);
  float smoothing = m.merge_smoothing;
  if (ui_drag_float("##smoothing", &smoothing, 0.001f, 0.0f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
    /* Shown as it changes; one undo step when let go. */
    m.merge_smoothing = smoothing;
  }
  ui_driver_typed(C, layer.id, path, -1);
  ui_driver_field_end(C, layer.id, path, -1, driven);
  ImGui::PopStyleColor(tint);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    char value[32];
    std::snprintf(value, sizeof(value), "%g", double(m.merge_smoothing));
    set({{"smoothing", value}});
  }
  ImGui::SameLine(0, 4);
  if (ui_key_button("##smoothing_key", key_state)) {
    operator_call(C, "anim.keyframe_toggle", {{"layer", std::to_string(layer.id)}, {"paths", path}});
  }
  ui_flash_row(layer.id, path);
  modifier_row_label("", column);
  bool corners = m.merge_corners_only;
  ImGui::BeginDisabled(m.merge_operation != MergeOperation::Union);
  if (ImGui::Checkbox("Corners Only", &corners)) {
    set({{"corners_only", corners ? "1" : "0"}});
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Only grow the rounded corners where the shapes meet, taking nothing from the other layers: "
                      "for a fill layer whose outline layer has the Boolean, so the rounded corners are filled too "
                      "(give both the same Smoothing)");
  }
}

static bool factor_slider(const char *id, float *value);

/* Blender's Bevel modifier panel, the parts that fit curves: Width Type, Amount (keyable), Limit Method
 * with its Angle or Point Group, then Profile and Geometry folded. */
static void bevel_modifier_body(Context &C, Layer &layer, Modifier &m, int index, float column)
{
  Document &doc = *C.document;
  const std::string idx = std::to_string(index);
  bool changed = false, done = false;
  const auto combo = [&](const char *id, int value, const char *const *names, int count, const char *const *tips) {
    int picked = value;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
    if (ImGui::BeginCombo(id, names[value])) {
      for (int k = 0; k < count; k++) {
        if (ImGui::Selectable(names[k], k == value)) {
          picked = k;
        }
        if (tips && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("%s", tips[k]);
        }
      }
      ImGui::EndCombo();
    }
    return picked;
  };

  static const char *const WIDTH_NAMES[] = {"Offset", "Width", "Depth", "Percent"};
  static const char *const WIDTH_TIPS[] = {
      "Amount is how far along each side from the corner the cut starts",
      "Amount is how long the cut is, from one side to the other",
      "Amount is how far the cut is from the corner",
      "Amount is how much of each side the cut takes, in percent"};
  modifier_row_label("Width Type", column, "What Amount measures");
  if (const int w = combo("##width_type", int(m.bevel_width), WIDTH_NAMES, 4, WIDTH_TIPS); w != int(m.bevel_width)) {
    m.bevel_width = BevelWidth(w);
    done = true;
  }

  /* Amount: keyable, with its key button, as Blender's. */
  const std::string path = "modifiers[" + idx + "].amount";
  const int key_state = layer_key_state(layer, {path}, doc.frame_current);
  modifier_row_label("Amount", column, "How big the cuts are (Width Type says how it's measured). It can be keyed");
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
  const int tint = ui_key_tint_push(key_state | layer_action_bone_state(layer, {path}));
  const int driven = ui_driver_field_begin(C, layer.id, path, -1);
  const bool percent = m.bevel_width == BevelWidth::Percent;
  float amount = m.bevel_amount;
  if (ui_drag_float("##amount", &amount, percent ? 0.2f : 0.001f, 0.0f, percent ? 100.0f : 100.0f,
                       percent ? "%.1f%%" : "%.3f", ImGuiSliderFlags_AlwaysClamp))
  {
    m.bevel_amount = amount;
  }
  done |= ImGui::IsItemDeactivatedAfterEdit();
  ui_driver_typed(C, layer.id, path, -1);
  ui_driver_field_end(C, layer.id, path, -1, driven);
  ImGui::PopStyleColor(tint);
  ImGui::SameLine(0, 4);
  if (ui_key_button("##amount_key", key_state)) {
    operator_call(C, "anim.keyframe_toggle", {{"layer", std::to_string(layer.id)}, {"paths", path}});
  }
  ui_flash_row(layer.id, path);
  ImGui::Spacing();

  static const char *const LIMIT_NAMES[] = {"None", "Angle", "Point Group"};
  static const char *const LIMIT_TIPS[] = {
      "Every corner: each point between two segments (not a line's ends, nor points where curves weld)",
      "Only the corners where the outline turns more than the Angle",
      "Only the points of a point group, each cut by its weight"};
  modifier_row_label("Limit Method", column, "Which corners it cuts");
  if (const int l = combo("##limit", int(m.bevel_limit), LIMIT_NAMES, 3, LIMIT_TIPS); l != int(m.bevel_limit)) {
    m.bevel_limit = BevelLimit(l);
    done = true;
  }
  if (m.bevel_limit == BevelLimit::Angle) {
    modifier_row_label("Angle", column, "Corners where the outline turns more than this are cut; smooth points never are");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
    float degrees = m.bevel_angle * 180.0f / PI;
    if (ui_drag_float("##angle", &degrees, 0.5f, 0.0f, 180.0f, "%.0f\xC2\xB0", ImGuiSliderFlags_AlwaysClamp)) {
      m.bevel_angle = degrees * PI / 180.0f;
    }
    done |= ImGui::IsItemDeactivatedAfterEdit();
  }
  else if (m.bevel_limit == BevelLimit::Group) {
    modifier_row_label("Point Group", column, "The point group whose points are cut (Shape tab > Point Groups)");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
    const bool known = std::any_of(layer.drawing.groups.begin(), layer.drawing.groups.end(),
                                   [&](const PointGroup &g) { return g.name == m.bevel_group; });
    if (ImGui::BeginCombo("##group", m.bevel_group.empty() ? "" : m.bevel_group.c_str(), ImGuiComboFlags_HeightLarge)) {
      /* A search field on top, as Parent Bone's. */
      static std::string search;
      if (ImGui::IsWindowAppearing()) {
        search.clear();
        ImGui::SetKeyboardFocusHere();
      }
      ImGui::SetNextItemWidth(-1);
      const bool entered = ImGui::InputTextWithHint("##search", "Search point groups", &search,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
      std::string needle = search;
      std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
      const PointGroup *first = nullptr;
      for (const PointGroup &g : layer.drawing.groups) {
        if (!search_matches(g.name, needle)) {
          continue;
        }
        first = first ? first : &g;
        if (ImGui::Selectable(g.name.c_str(), g.name == m.bevel_group)) {
          m.bevel_group = g.name;
          done = true;
        }
      }
      if (!first) {
        ImGui::TextDisabled(layer.drawing.groups.empty() ? "No point groups: add one in the Shape tab"
                                                         : "No point group named like that");
      }
      if (entered && first) {
        m.bevel_group = first->name;
        done = true;
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndCombo();
    }
    if (!m.bevel_group.empty() && !known && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("This layer has no point group \"%s\"", m.bevel_group.c_str());
    }
  }
  ImGui::Spacing();

  /* Profile and Geometry, folded, as Blender's subpanels. */
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
  const std::string profile_key = ui_fold_scope() + "/Bevel/Profile";
  ui_fold_next(profile_key, false);
  const bool profile_open = ImGui::TreeNodeEx("Profile", ImGuiTreeNodeFlags_SpanAvailWidth);
  ui_fold_record(profile_key, false, profile_open);
  if (profile_open) {
    modifier_row_label("Shape", column,
                       "The cut's shape: 0.5 round (a circle's arc), 0.25 straight, 1 square, below 0.25 scooped inward");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
    if (factor_slider("##profile", &m.bevel_profile)) {
      changed = true;
    }
    done |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::TreePop();
  }
  const std::string geometry_key = ui_fold_scope() + "/Bevel/Geometry";
  ui_fold_next(geometry_key, false);
  const bool geometry_open = ImGui::TreeNodeEx("Geometry", ImGuiTreeNodeFlags_SpanAvailWidth);
  ui_fold_record(geometry_key, false, geometry_open);
  if (geometry_open) {
    modifier_row_label("", column);
    if (ImGui::Checkbox("Clamp Overlap", &m.bevel_clamp)) {
      done = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Keep two cuts on one segment from crossing: each takes at most half of it");
    }
    ImGui::TreePop();
  }
  ImGui::PopStyleColor();
  (void)changed;
  if (done) {
    C.undo->push("Bevel Settings", doc);
  }
}

/* Two or three joined buttons, the picked one blue, as the bone Shape selector (Bone | Pin | Custom).
 * Returns the one clicked, or -1. */
static int segmented_choice(const char *id, int picked, const char *const *names, const char *const *tips, int count,
                            float width)
{
  ImGui::PushID(id);
  const float gap = 1.0f;
  const float w = (width - float(count - 1) * gap) / float(count);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, ImGui::GetStyle().ItemSpacing.y));
  int clicked = -1;
  for (int k = 0; k < count; k++) {
    if (k > 0) {
      ImGui::SameLine();
    }
    const bool on = picked == k;
    if (on) {
      const ImVec4 blue = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
      ImGui::PushStyleColor(ImGuiCol_Button, blue);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(blue.x * 1.15f, blue.y * 1.15f, blue.z * 1.15f, blue.w));
    }
    if (ImGui::Button(names[k], ImVec2(w, 0.0f)) && !on) {
      clicked = k;
    }
    if (on) {
      ImGui::PopStyleColor(2);
    }
    if (tips && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", tips[k]);
    }
  }
  ImGui::PopStyleVar();
  ImGui::PopID();
  return clicked;
}

/* How many points modifier `index` of `layer` adds: counted in the drawing as the whole stack leaves it, worked out
 * once a frame per layer. -1 while it is hidden in the viewport. */
static int modifier_added_points(const Document &doc, const Layer &layer, size_t index)
{
  static int frame = -1;
  static std::map<uint64_t, std::vector<int>> counts;
  if (frame != ImGui::GetFrameCount()) {
    frame = ImGui::GetFrameCount();
    counts.clear();
  }
  auto it = counts.find(layer.id);
  if (it == counts.end()) {
    std::vector<int> made(layer.modifiers.size(), 0);
    const VectorDrawing shown = layer_evaluated_drawing(doc, layer, false);
    for (const MeshPoint &p : shown.points) {
      if (p.made_by >= 0 && size_t(p.made_by) < made.size()) {
        made[size_t(p.made_by)]++;
      }
    }
    it = counts.emplace(layer.id, std::move(made)).first;
  }
  if (index >= it->second.size() || index >= layer.modifiers.size()) {
    return 0;
  }
  return layer.modifiers[index].show_viewport ? it->second[index] : -1;
}

/* Resample: how many points go between yours (Per Segment or By Length), how many that makes, and the rest folded
 * under More. */
static void resample_modifier_body(Context &C, Layer &layer, Modifier &m, int index, float column)
{
  Document &doc = *C.document;
  bool done = false;
  /* The value column's width, from where the row's label left the cursor. */
  const auto width = []() { return ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f; };

  static const char *const MODE_NAMES[] = {"Per Segment", "By Length"};
  static const char *const MODE_TIPS[] = {
      "Each segment between your points split into the same number of pieces: it always adds points, and how many "
      "is easy to tell",
      "A point at least every Spacing along each segment (measured in the setup pose): long segments get more, short "
      "ones none, so bends are equally smooth everywhere"};
  modifier_row_label("Mode", column, "How the points it adds between yours are spaced (yours stay as they are)");
  if (const int k = segmented_choice("##mode", m.subdiv_by_length ? 1 : 0, MODE_NAMES, MODE_TIPS, 2, width()); k >= 0)
  {
    m.subdiv_by_length = k == 1;
    done = true;
  }
  if (m.subdiv_by_length) {
    modifier_row_label("Spacing", column, "The longest a piece may be (measured in the setup pose, so the pieces "
                                          "stay the same on every frame)");
    ImGui::SetNextItemWidth(width());
    ui_drag_float("##spacing", &m.subdiv_spacing, 0.001f, 0.001f, 100.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
    done |= ImGui::IsItemDeactivatedAfterEdit();
  }
  else {
    modifier_row_label("Pieces", column, "How many pieces each segment becomes (2: one point added in the middle)");
    ImGui::SetNextItemWidth(width());
    ui_drag_int("##pieces", &m.subdiv_pieces, 0.1f, 1, 256, "%d", ImGuiSliderFlags_AlwaysClamp);
    done |= ImGui::IsItemDeactivatedAfterEdit();
  }

  /* What it adds, and why when it adds none. */
  modifier_row_label("", column);
  const int added = modifier_added_points(doc, layer, size_t(index));
  const ImVec4 warn(1.0f, 0.6f, 0.3f, 1.0f);
  if (added < 0) {
    ImGui::TextDisabled("Hidden in the viewport");
  }
  else if (added > 0) {
    ImGui::TextDisabled("Adds %d point%s", added, added == 1 ? "" : "s");
  }
  else if (!m.subdiv_by_length && m.subdiv_pieces <= 1) {
    ImGui::TextColored(warn, "Adds no points: Pieces is 1");
  }
  else if (m.subdiv_by_length) {
    ImGui::TextColored(warn, "Adds no points: every segment is shorter than Spacing");
  }
  else if (!m.subdiv_group.empty()) {
    ImGui::TextColored(warn, "Adds no points: no segment touches %s", m.subdiv_group.c_str());
  }
  else {
    ImGui::TextColored(warn, "Adds no points");
  }

  /* The rest, folded as Blender's subpanels. */
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
  const std::string more_key = ui_fold_scope() + "/Resample/More";
  ui_fold_next(more_key, false);
  const bool more_open = ImGui::TreeNodeEx("More", ImGuiTreeNodeFlags_SpanAvailWidth);
  ui_fold_record(more_key, false, more_open);
  if (more_open) {
    static const char *const WEIGHT_NAMES[] = {"Blend", "From Bones"};
    static const char *const WEIGHT_TIPS[] = {
        "Each new point mixes its two end points' weights by how far along it is: a smooth, rubber-hose bend",
        "Each new point weighted from the nearest bones, so a segment can bend at a joint between its points"};
    modifier_row_label("Weights", column, "How the new points follow the bones");
    if (const int k = segmented_choice("##weights", m.subdiv_from_bones ? 1 : 0, WEIGHT_NAMES, WEIGHT_TIPS, 2,
                                       width());
        k >= 0)
    {
      m.subdiv_from_bones = k == 1;
      done = true;
    }

    modifier_row_label("", column);
    if (ImGui::Checkbox("Preserve Volume", &m.subdiv_preserve_volume)) {
      done = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The new points blend the bones' turns as rigid motions, so the line doesn't pinch or curl "
                        "where bones turn far apart (Blender's Preserve Volume). Your own points move as before");
    }

    if (m.subdiv_by_length) {
      modifier_row_label("Render", column, "Spacing for the final render (0: the same as Spacing): finer for "
                                           "smoother bends there while the viewport stays quick");
      ImGui::SetNextItemWidth(width());
      ui_drag_float("##render_spacing", &m.subdiv_render_spacing, 0.001f, 0.0f, 100.0f,
                    m.subdiv_render_spacing > 0.0f ? "%.3f" : "Same", ImGuiSliderFlags_AlwaysClamp);
      done |= ImGui::IsItemDeactivatedAfterEdit();
    }

    modifier_row_label("Max Pieces", column, "No segment becomes more pieces than this, however long");
    ImGui::SetNextItemWidth(width());
    ui_drag_int("##max_pieces", &m.subdiv_max_pieces, 0.2f, 1, 256, "%d", ImGuiSliderFlags_AlwaysClamp);
    done |= ImGui::IsItemDeactivatedAfterEdit();

    modifier_row_label("Point Group", column, "Only the segments touching this point group's points (none: all)");
    ImGui::SetNextItemWidth(width());
    if (ImGui::BeginCombo("##subdiv_group", m.subdiv_group.empty() ? "All" : m.subdiv_group.c_str())) {
      if (ImGui::Selectable("All", m.subdiv_group.empty()) && !m.subdiv_group.empty()) {
        m.subdiv_group.clear();
        done = true;
      }
      for (const PointGroup &g : layer.drawing.groups) {
        if (ImGui::Selectable(g.name.c_str(), g.name == m.subdiv_group) && g.name != m.subdiv_group) {
          m.subdiv_group = g.name;
          done = true;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::TreePop();
  }
  ImGui::PopStyleColor();

  /* Below the Armature modifier it only adds points to the bent drawing: say so. */
  bool below = false, armature_seen = false;
  for (int k = 0; k < int(layer.modifiers.size()); k++) {
    armature_seen = armature_seen || layer.modifiers[size_t(k)].type == ModifierType::Armature;
    below = below || (k == index && armature_seen);
  }
  if (below) {
    modifier_row_label("", column);
    ImGui::TextDisabled("Drag it above Armature to bend through the bones");
  }
  if (done) {
    C.undo->push("Resample Settings", doc);
  }
}

/* A deform modifier's body (Curve, Cage or Mesh Deform): the shape that bends the layer (a layer, then one of its shapes), what kind
 * of deform that makes, the weights (Automatic: every point; or Custom, painted in Weight Paint), Strength
 * (keyable) and Precision. */
/* The deform eyedropper: a shape clicked in the viewport, and its layer, become what deform modifier `index` of
 * layer `owner` bends it by. It looks past the owner (drawn over the shape it follows); a click on no drawing's
 * shape waits for another. A shape this deform doesn't take still goes in, and the modifier says which does. A
 * layer clicked in the Layers editor goes in with the first of its shapes this deform takes (else its only one). */
static void deform_eyedropper_start(uint64_t owner, int index, const std::string &tag)
{
  ui_shape_eyedropper_start(
      [owner, index](Context &C, uint64_t layer_id, int shape_index) {
        Document &doc = *C.document;
        Layer *own = layer_find(doc, owner);
        if (!own || index >= int(own->modifiers.size()) || !modifier_is_deform(own->modifiers[size_t(index)].type)) {
          return true;
        }
        const Layer *clicked = layer_find(doc, layer_id);
        if (!clicked || clicked->id == owner || clicked->type != LayerType::Vector) {
          return false;
        }
        const VectorDrawing shown = layer_evaluated_drawing(doc, *clicked, false);
        if (shape_index < 0 || shape_index >= int(shown.shapes.size()) ||
            drawing_shape_index(clicked->drawing, shown.shapes[size_t(shape_index)].id) < 0)
        {
          return false;
        }
        Modifier &m = own->modifiers[size_t(index)];
        if (m.deform_layer != clicked->id || m.deform_shape != shown.shapes[size_t(shape_index)].id) {
          m.deform_point_id = m.deform_point2_id = 0;
        }
        m.deform_layer = clicked->id;
        m.deform_shape = shown.shapes[size_t(shape_index)].id;
        C.undo->push(std::string(deform_type_name(m.type)) + " Shape", doc);
        return true;
      },
      [] { return std::string("Click the shape that bends this layer, or its layer  |  Esc cancels"); }, tag.c_str(),
      owner,
      [owner, index](Context &C, uint64_t layer_id) {
        Document &doc = *C.document;
        Layer *own = layer_find(doc, owner);
        if (!own || index >= int(own->modifiers.size()) || !modifier_is_deform(own->modifiers[size_t(index)].type)) {
          return true;
        }
        const Layer *clicked = layer_find(doc, layer_id);
        if (!clicked || clicked->id == owner || clicked->type != LayerType::Vector) {
          return false;
        }
        Modifier &m = own->modifiers[size_t(index)];
        Modifier test = m;
        test.deform_layer = clicked->id;
        uint64_t shape = clicked->drawing.shapes.size() == 1 ? clicked->drawing.shapes[0].id : 0;
        for (const Shape &s : clicked->drawing.shapes) {
          test.deform_shape = s.id;
          if (vector_deform_kind(doc, *own, test) != DeformKind::None ||
              (m.type == ModifierType::CurveDeform && vector_deform_takes_part(doc, test)))
          {
            shape = s.id;
            break;
          }
        }
        if (m.deform_layer != clicked->id || m.deform_shape != shape) {
          m.deform_part_from = m.deform_part_to = 0;
          m.deform_point_id = m.deform_point2_id = 0;
        }
        m.deform_layer = clicked->id;
        m.deform_shape = shape;
        C.undo->push(std::string(deform_type_name(m.type)) + " Shape", doc);
        return true;
      });
}

/* A Follow Deform's or Follow Point's point, past its label: Own Place or a point of `shape` on `shape_layer`, from the list
 * or with the eyedropper. `pick` takes the point's id (0: its own place) and finds what it's for by itself, as the
 * eyedropper calls it later. `id`, what it is now; `tag` says who's asking. */
static void follow_point_row(Context &C, Layer *shape_layer, uint64_t shape, uint64_t id, float width, const std::string &tag,
                             const std::function<bool(Context &, uint64_t)> &pick)
{
  const int on_shape = shape_layer ? drawing_shape_index(shape_layer->drawing, shape) : -1;
  const int picked = shape_layer && id != 0 ? drawing_point_index(shape_layer->drawing, id) : -1;
  const std::string name = id == 0 ? "Own Place" : picked >= 0 ? "Point " + std::to_string(picked) : "Point gone";
  ImGui::PushID(tag.c_str());
  ImGui::SetNextItemWidth(width - ImGui::GetFrameHeight() - 2.0f);
  ImGui::BeginDisabled(on_shape < 0);
  if (ImGui::BeginCombo("##follow_point", name.c_str())) {
    if (ImGui::Selectable("Own Place", id == 0) && id != 0) {
      pick(C, 0);
    }
    drawing_ensure_ids(shape_layer->drawing);
    for (const int p : shape_points(shape_layer->drawing, shape_layer->drawing.shapes[size_t(on_shape)])) {
      const uint64_t point = shape_layer->drawing.points[size_t(p)].id;
      if (ImGui::Selectable(("Point " + std::to_string(p)).c_str(), point == id) && point != id) {
        pick(C, point);
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine(0, 2);
  if (ui_pick_button("##follow_pick", "Eyedropper: click a point of the shape in the viewport",
                     ui_point_eyedropper_active(tag.c_str())))
  {
    drawing_ensure_ids(shape_layer->drawing);
    ui_point_eyedropper_start(shape_layer->id, shape, pick, [] { return std::string("Click a point of the shape  |  Esc cancels"); },
                              tag.c_str());
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}

static void vector_deform_modifier_body(Context &C, Layer &layer, Modifier &m, int index, float column)
{
  Document &doc = *C.document;
  bool done = false;
  const auto width = []() { return ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f; };

  /* The layer: any other drawing layer. */
  std::vector<const Layer *> drawings;
  merge_drawing_layers(doc.layers, drawings);
  std::erase_if(drawings, [&](const Layer *l) { return l->id == layer.id; });
  const Layer *target = layer_find(doc, m.deform_layer);
  const bool follows = m.type == ModifierType::FollowDeform;
  modifier_row_label("Layer", column, follows ? "The layer whose shape carries this one" : "The layer whose shape bends this one");
  ImGui::SetNextItemWidth(width());
  if (const uint64_t picked = layer_search_combo("##deform_layer", target ? target->name.c_str() : "Pick a layer",
                                                 drawings, m.deform_layer))
  {
    m.deform_layer = picked;
    target = layer_find(doc, picked);
    /* Its only shape, or its first grid's, picked for you. */
    m.deform_shape = target && target->drawing.shapes.size() == 1 ? target->drawing.shapes[0].id : 0;
    m.deform_part_from = m.deform_part_to = 0;
    m.deform_point_id = m.deform_point2_id = 0;
    done = true;
  }

  /* The shape: one of that layer's. */
  modifier_row_label("Shape", column, follows ? "The line, closed shape, grid or mesh that carries it"
                                       : m.type == ModifierType::CurveDeform ? "The line that bends it, or the shape with "
                                                                              "the Part of it that does"
                                       : m.type == ModifierType::CageDeform ? "The closed shape (a cage) that bends it"
                                                                             : "The grid, mesh of cells drawn by hand, or "
                                                                               "line (each point pinned to it, as a bendy "
                                                                               "bone holds what it carries) that bends it");
  const float pick_w = ImGui::GetFrameHeight();
  ImGui::SetNextItemWidth(width() - pick_w - 2.0f);
  const int shape_index = target ? drawing_shape_index(target->drawing, m.deform_shape) : -1;
  ImGui::BeginDisabled(!target);
  if (ImGui::BeginCombo("##deform_shape",
                        shape_index >= 0 ? target->drawing.shapes[size_t(shape_index)].name.c_str() : "Pick a shape"))
  {
    for (const Shape &s : target->drawing.shapes) {
      /* Only the shapes this deform takes; the others say which one does. */
      Modifier test = m;
      test.deform_shape = s.id;
      const bool fits = vector_deform_kind(doc, layer, test) != DeformKind::None ||
                        (m.type == ModifierType::CurveDeform && vector_deform_takes_part(doc, test));
      const ModifierType other = fits ? m.type : vector_deform_fitting_type(doc, layer, test);
      const std::string label = fits || other == m.type ? s.name
                                                        : s.name + "  (" + deform_type_name(other) + ")";
      ImGui::PushID(int(s.id));
      ImGui::BeginDisabled(!fits);
      if (ImGui::Selectable(label.c_str(), s.id == m.deform_shape) && s.id != m.deform_shape) {
        m.deform_shape = s.id;
        m.deform_part_from = m.deform_part_to = 0;
        m.deform_point_id = m.deform_point2_id = 0;
        done = true;
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  /* Or click it in the viewport (its layer comes with it), as Boolean's eyedropper. */
  ImGui::SameLine(0, 2);
  const std::string pick_tag = "DEFORM:" + std::to_string(layer.id) + ":" + std::to_string(index);
  if (ui_pick_button("##deform_pick", "Eyedropper: click the shape that bends this layer (its layer comes with it)",
                     ui_shape_eyedropper_active(pick_tag.c_str())))
  {
    deform_eyedropper_start(layer.id, index, pick_tag);
  }

  /* A Curve Deform's Part: one stretch of its shape, as the points selected on it say. */
  const auto part_row = [&](bool closed) {
    modifier_row_label("Part", column, "Follow one stretch of the shape, not all of it. On the shape's layer, select the "
                                       "points along that stretch, end to end, and press this. It stays live, so two "
                                       "lips can follow the top and bottom of one mouth line");
    if (ImGui::Button("Use Selected Points")) {
      if (vector_deform_part_from_selection(doc, m)) {
        done = true;
      }
      else {
        report(ReportType::Warning, "Select the points along the part you want on the shape's layer, end to end: two at "
                                    "least, and not all the way round");
      }
    }
    if (m.deform_part_from != 0) {
      modifier_row_label("", column);
      if (closed && ImGui::Button("Other Side")) {
        std::swap(m.deform_part_from, m.deform_part_to);
        done = true;
      }
      if (closed) {
        ImGui::SameLine();
      }
      if (ImGui::Button("Clear")) {
        m.deform_part_from = m.deform_part_to = 0;
        done = true;
      }
    }
  };

  /* What it makes of the shape. */
  const DeformKind kind = vector_deform_kind(doc, layer, m);
  modifier_row_label("", column);
  if (vector_deform_loops(doc, layer, m)) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "A loop: this layer bends that shape, so neither bends");
  }
  else if (kind == DeformKind::Grid) {
    ImGui::TextDisabled(follows ? "Grid: the point goes as its cell does" : "Grid: each cell bends what is in it");
    static const char *const METHOD_NAMES[] = {"Cells", "Envelope"};
    static const char *const METHOD_TIPS[] = {
        "Each cell bends what is in it, so the drawing fits the mesh exactly, and a line along the edge stretches with "
        "its cell",
        "As Cage Deform's Envelope: the outer cells keep what lies near the outline at its distance from it, so lines "
        "along the edge keep their width. This eases off toward the next cells in, whose points and lines are still "
        "exactly where the mesh puts them (Blend says how evenly the sides share a corner)"};
    modifier_row_label("Method", column, "How what is in the mesh follows it");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
    const int method = int(m.deform_mesh_method);
    if (ImGui::BeginCombo("##mesh_method", METHOD_NAMES[method])) {
      for (int k = 0; k < 2; k++) {
        if (ImGui::Selectable(METHOD_NAMES[k], k == method) && k != method) {
          m.deform_mesh_method = MeshMethod(k);
          done = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("%s", METHOD_TIPS[k]);
        }
      }
      ImGui::EndCombo();
    }
    if (m.deform_mesh_method == MeshMethod::Envelope) {
      modifier_row_label("Blend", column, "How evenly the sides share what lies near more than one of them, as at a "
                                          "corner: low, the nearest side leads sharply (edges hug their own side "
                                          "most); high, the sides blend gently");
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
      factor_slider("##mesh_blend", &m.deform_blend);
      done |= ImGui::IsItemDeactivatedAfterEdit();
    }
    if (vector_deform_drawn_grid(doc, layer, m)) {
      modifier_row_label("", column);
      if (ImGui::Checkbox("Smooth", &m.deform_smooth)) {
        done = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Bends by smooth lines through the grid's nodes, in place of its sides' own handles, so what "
                          "it bends has no corners at the nodes. You still key only the nodes, and it goes through "
                          "every one. The grid itself draws as it is");
      }
    }
  }
  else if (kind == DeformKind::Cage) {
    ImGui::TextDisabled(follows ? "Cage: the point goes as the shape does" : "Cage: what is in the shape bends with it");
    static const char *const METHOD_NAMES[] = {"Mean Value", "Rigid", "Envelope"};
    static const char *const METHOD_TIPS[] = {
        "Smooth, and your drawing's outline stays exactly on the cage, but the inside squashes where a side bulges "
        "out or stretches (mean value coordinates)",
        "The inside keeps its shape as far as it can (as rigid as possible, as Photoshop's Puppet Warp) and the "
        "outline still stays exactly on the cage: for art drawn along the cage's edge. A little slower",
        "Each side of the cage carries what lies near it, at the same distance from it, as Toon Boom Harmony's "
        "envelopes do: lines along the outline keep their width and edges hug the cage, and the middle follows the "
        "sides around it (Blend says how evenly)"};
    modifier_row_label("Method", column, "How what is in the cage follows it");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
    const int method = int(m.deform_cage_method);
    if (ImGui::BeginCombo("##cage_method", METHOD_NAMES[method])) {
      for (int k = 0; k < 3; k++) {
        if (ImGui::Selectable(METHOD_NAMES[k], k == method) && k != method) {
          m.deform_cage_method = CageMethod(k);
          done = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("%s", METHOD_TIPS[k]);
        }
      }
      ImGui::EndCombo();
    }
    if (m.deform_cage_method == CageMethod::Envelope) {
      modifier_row_label("Blend", column, "How evenly the sides share what lies near more than one of them, as at a "
                                          "corner: low, the nearest side leads sharply (edges hug their own side "
                                          "most); high, the sides blend gently");
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
      factor_slider("##deform_blend", &m.deform_blend);
      done |= ImGui::IsItemDeactivatedAfterEdit();
    }
  }
  else if (kind == DeformKind::Mesh) {
    ImGui::TextDisabled(follows ? "Mesh: the point goes as its cell does" : "Mesh: each cell bends what is in it");
  }
  else if (kind == DeformKind::Wire) {
    ImGui::TextDisabled(follows ? "Wire: the point is pinned to the line" : "Wire: everything moves with the line, pinned to it (a bendy bone)");
  }
  else if (kind == DeformKind::Line) {
    ImGui::TextDisabled(follows ? "Line: the point goes along it" : "Line: what lies along it bends with it");
    modifier_row_label("", column);
    if (ImGui::Checkbox("Stretch", &m.deform_stretch)) {
      done = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("As the line grows or shrinks, what lies along it spreads or bunches to fit, end to end "
                        "(Illustrator's Art Brush). Off, it keeps its length and slides along the line, as "
                        "Blender's Curve modifier does");
    }
    if (!follows && vector_deform_takes_part(doc, m)) {
      Modifier whole = m;
      whole.deform_part_from = whole.deform_part_to = 0;
      part_row(vector_deform_kind(doc, layer, whole) != DeformKind::Line);
    }
  }
  else if (m.type == ModifierType::CurveDeform && vector_deform_takes_part(doc, m)) {
    /* A closed shape with no Part, or one whose points have gone. */
    ImGui::TextDisabled("Closed shape: pick the Part of it to follow");
    part_row(true);
  }
  else if (target && drawing_shape_index(target->drawing, m.deform_shape) >= 0 &&
           vector_deform_fitting_type(doc, layer, m) != m.type)
  {
    /* The shape no longer fits (joined into cells, closed, opened): say which deform takes it, and switch. */
    const ModifierType fit = vector_deform_fitting_type(doc, layer, m);
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "This shape needs %s", deform_type_name(fit));
    modifier_row_label("", column);
    if (ImGui::Button((std::string("Switch to ") + deform_type_name(fit)).c_str())) {
      if (m.name.rfind(deform_type_name(m.type), 0) == 0) {
        m.name = deform_type_name(fit) + m.name.substr(std::string(deform_type_name(m.type)).size());
      }
      m.type = fit;
      done = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Make this modifier a %s, keeping its settings and painted weights", deform_type_name(fit));
    }
  }
  else {
    ImGui::TextDisabled(!target ? "Pick a layer, then its shape" : follows ? "Pick the shape it follows" : "Pick the shape that bends it");
  }

  /* Weights: a group's deform has the automatic ones, every drawing inside following it fully. */
  if (!layer.is_group() && !follows) {
    static const char *const WEIGHT_NAMES[] = {"Automatic", "Custom"};
    static const char *const WEIGHT_TIPS[] = {
        "Every point of the drawing follows it fully, past the shape too (as Blender's deform modifiers bend the "
        "whole object). Paint Custom weights to hold parts back",
        "A point group of their own, painted in Weight Paint (pick this modifier at the top of the viewport). It "
        "starts from the automatic weights; points added later take theirs"};
    modifier_row_label("Weights", column, "How much each point follows the deform (and so, inside a bone layer, "
                                          "how much less it follows the bones)");
    if (const int k = segmented_choice("##deform_weights", m.deform_custom ? 1 : 0, WEIGHT_NAMES, WEIGHT_TIPS, 2, width());
        k >= 0 && (k == 1) != m.deform_custom)
    {
      m.deform_custom = k == 1;
      const bool has_group = !m.deform_group.empty() &&
                             std::any_of(layer.drawing.groups.begin(), layer.drawing.groups.end(),
                                         [&](const PointGroup &g) { return g.name == m.deform_group; });
      if (m.deform_custom && !has_group) {
        vector_deform_fill_group(doc, layer, m);
      }
      done = true;
    }
    if (m.deform_custom) {
      modifier_row_label("Point Group", column, "Where the weights are kept");
      ImGui::TextUnformatted(m.deform_group.c_str());
      modifier_row_label("", column);
      if (ImGui::Button("Reset to Automatic")) {
        vector_deform_fill_group(doc, layer, m);
        done = true;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Fill the point group with the automatic weights again, as a fresh start");
      }
      ImGui::SameLine();
      ImGui::TextDisabled("Paint them in Weight Paint");
    }
  }

  /* Strength: keyable, with its key button. */
  const std::string path = "modifiers[" + std::to_string(index) + "].strength";
  const int key_state = layer_key_state(layer, {path}, doc.frame_current);
  modifier_row_label("Strength", column, follows ? "How much of the shape's move it takes: 0 none, 1 all of it. It can be keyed"
                                                  : "How much of the deform shows: 0 none, 1 all of it. It can be keyed, to fade "
                                                    "a deform in or out");
  ImGui::SetNextItemWidth(width());
  const int tint = ui_key_tint_push(key_state | layer_action_bone_state(layer, {path}));
  const int driven = ui_driver_field_begin(C, layer.id, path, -1);
  float strength = m.deform_strength;
  if (ui_slider_float("##deform_strength", &strength, 0.0f, 1.0f, "%.3f")) {
    m.deform_strength = strength;
  }
  done |= ImGui::IsItemDeactivatedAfterEdit();
  ui_driver_typed(C, layer.id, path, -1);
  ui_driver_field_end(C, layer.id, path, -1, driven);
  ImGui::PopStyleColor(tint);
  ImGui::SameLine(0, 4);
  if (ui_key_button("##deform_strength_key", key_state)) {
    operator_call(C, "anim.keyframe_toggle", {{"layer", std::to_string(layer.id)}, {"paths", path}});
  }
  ui_flash_row(layer.id, path);

  if (follows) {
    /* Its own place, or a point the shape already has. */
    const auto point_row = [&](bool second) {
      modifier_row_label(second ? "Second" : "Point", column,
                         "Own Place is a point of its own, which you can put anywhere on the shape, drag and key.\n"
                         "Or pick a point the shape already has: the drawing goes exactly as that point does");
      const uint64_t owner = layer.id;
      follow_point_row(C, layer_find(doc, m.deform_layer), m.deform_shape, second ? m.deform_point2_id : m.deform_point_id,
                       width(), "FOLLOW:" + std::to_string(owner) + ":" + std::to_string(index) + (second ? ":2" : ":1"),
                       [owner, index, second](Context &C, uint64_t point) {
                         Document &doc = *C.document;
                         Layer *own = layer_find(doc, owner);
                         if (own && index < int(own->modifiers.size()) &&
                             own->modifiers[size_t(index)].type == ModifierType::FollowDeform)
                         {
                           vector_deform_follow_pick(doc, *own, own->modifiers[size_t(index)], second, point);
                           C.undo->push("Follow Deform", doc);
                         }
                         return true;
                       });
    };
    point_row(false);
    modifier_row_label("", column);
    if (ImGui::Checkbox("Show Point", &m.deform_show_point)) {
      done = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Shows the point in the viewport, light blue. Drag it on frame 0 to choose the place on the shape\n"
                        "that carries the drawing; drag it on any other frame to key it along the shape. Hiding it changes nothing");
    }
    modifier_row_label("", column);
    bool two = m.deform_two;
    if (ImGui::Checkbox("Second Point", &two)) {
      vector_deform_follow_two(m, two);
      done = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("A second place on the shape: the way from the first to it can turn the drawing, and how far\n"
                        "apart they are can scale it. Further apart gives a steadier angle");
    }
    if (m.deform_two) {
      point_row(true);
    }
    ImGui::BeginDisabled(doc.frame_current == 0);
    for (const bool second : {false, true}) {
      /* A point of the shape goes as it does: there's nothing to shift. */
      if ((second && !m.deform_two) || (second ? m.deform_point2_id : m.deform_point_id) != 0) {
        continue;
      }
      const std::string shift_path = "modifiers[" + std::to_string(index) + (second ? "].shift2" : "].shift");
      const int shift_state = layer_key_state(layer, {shift_path}, doc.frame_current);
      modifier_row_label(second ? "Second Shift" : "Shift", column,
                         "How far the point is moved from where it rests, on this frame: key it for a drawing that slides\n"
                         "along the shape. Dragging the light blue point does the same");
      ImGui::SetNextItemWidth(width());
      const int shift_tint = ui_key_tint_push(shift_state | layer_action_bone_state(layer, {shift_path}));
      float2 &shift = second ? m.deform_shift2 : m.deform_shift;
      float values[2] = {shift.x, shift.y};
      if (ui_drag_float2(second ? "##deform_shift2" : "##deform_shift", values, 0.001f, -100.0f, 100.0f, "%.3f")) {
        shift = {values[0], values[1]};
      }
      done |= ImGui::IsItemDeactivatedAfterEdit();
      ImGui::PopStyleColor(shift_tint);
      ImGui::SameLine(0, 4);
      if (ui_key_button(second ? "##deform_shift2_key" : "##deform_shift_key", shift_state)) {
        operator_call(C, "anim.keyframe_toggle", {{"layer", std::to_string(layer.id)}, {"paths", shift_path}});
      }
      ui_flash_row(layer.id, shift_path);
    }
    ImGui::EndDisabled();
    if (m.deform_two) {
      modifier_row_label("", column);
      if (ImGui::Checkbox("Rotation", &m.deform_rotation)) {
        done = true;
      }
      ImGui::SameLine();
      if (ImGui::Checkbox("Scale", &m.deform_scale)) {
        done = true;
      }
    }
    if (done) {
      C.undo->push("Follow Deform", doc);
    }
    return;
  }

  modifier_row_label("Precision", column, "How far a bent line may stray from the true bend: where the deform bends "
                                          "a segment more than its points and handles can follow, points are added "
                                          "there (Illustrator's Fidelity). Smaller is truer. Off: only the points "
                                          "you drew move");
  ImGui::SetNextItemWidth(width());
  /* Resample Self adds no points, so Precision has nothing to do with it on. */
  ImGui::BeginDisabled(m.deform_resample_self);
  ui_drag_float("##deform_precision", &m.deform_precision, 0.0002f, 0.0f, 1.0f,
                m.deform_precision > 0.0f && !m.deform_resample_self ? "%.4f" : "Off", ImGuiSliderFlags_AlwaysClamp);
  done |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::EndDisabled();
  modifier_row_label("", column);
  if (ImGui::Checkbox("Resample Self", &m.deform_resample_self)) {
    done = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Bends a finely resampled copy of this drawing, then snaps your own points to where theirs went "
                      "and fits each segment's handles to the bent copy (as Sculpt mode does). Truer bends with only "
                      "your points: none are added");
  }

  if (done) {
    C.undo->push(std::string(deform_type_name(m.type)) + " Settings", doc);
  }
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
    if (!layer.is_group()) {
      ImGui::TextDisabled("Generate");
      if (ImGui::MenuItem("Mirror")) {
        operator_call(C, "object.modifier_add", {{"type", "MIRROR"}});
      }
      if (ImGui::MenuItem("Bevel")) {
        operator_call(C, "object.modifier_add", {{"type", "BEVEL"}});
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Cut the drawing's corners off and round them: the sharp ones, or a point group's");
      }
      if (ImGui::MenuItem("Resample")) {
        operator_call(C, "object.modifier_add", {{"type", "RESAMPLE"}});
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Split the segments into smaller pieces before the bones bend them, for bendier "
                          "animation without more points to animate. At rest the drawing looks the same");
      }
      if (ImGui::MenuItem("Boolean")) {
        operator_call(C, "object.modifier_add", {{"type", "BOOLEAN"}});
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Join this layer's shapes with other layers' into one outline, the corners where they "
                          "meet rounded as you like");
      }
      ImGui::Separator();
    }
    ImGui::TextDisabled(layer.is_group() ? "Deform every drawing inside" : "Deform");
    if (ImGui::MenuItem("Curve Deform")) {
      operator_call(C, "object.modifier_add", {{"type", "CURVE_DEFORM"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Bend the drawing along an open line on another layer (a tail, a strand of hair, a sleeve), "
                        "as Blender's Curve modifier. Animate the line and the drawing follows");
    }
    if (ImGui::MenuItem("Cage Deform")) {
      operator_call(C, "object.modifier_add", {{"type", "CAGE_DEFORM"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Bend what is in a closed shape on another layer with it (a cheek, a belly), as Blender's "
                        "Mesh Deform. Animate the shape and the drawing follows");
    }
    if (ImGui::MenuItem("Mesh Deform")) {
      operator_call(C, "object.modifier_add", {{"type", "MESH_DEFORM"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Bend the drawing with a grid, or a mesh of cells you drew, on another layer: each cell "
                        "bends what is in it, as Blender's Lattice and Moho's Smart Warp. Animate it and the "
                        "drawing follows");
    }
    if (!layer.is_group()) {
      if (ImGui::MenuItem("Follow Deform")) {
        operator_call(C, "object.modifier_add", {{"type", "FOLLOW_DEFORM"}});
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Keep the drawing on a shape on another layer without bending it (an eye, a button, a stud), as "
                          "Toon Boom's Point Kinematic Output: a point on the shape carries it");
      }
    }
    ImGui::EndPopup();
  }
  ImGui::Spacing();

  int remove = -1, drag_from = -1, drag_to = -1, apply = -1, duplicate = -1;
  const float h = ImGui::GetFrameHeight();
  /* Apply needs the setup frame, as the rig's changes do. */
  const bool setup = doc.frame_current == 0;
  for (int i = 0; i < int(layer.modifiers.size()); i++) {
    Modifier &m = layer.modifiers[i];
    const bool armature = m.type == ModifierType::Armature;
    const bool can_apply = modifier_can_apply(m) && setup;
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
    const float right_w = h * 5.0f + h * 0.8f + style.ItemSpacing.x * 3.0f + 9.0f;
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
    /* Show Points, on the modifiers that add points; the others keep its place, so the buttons line up. */
    ImGui::SameLine(0, 1);
    if (modifier_makes_points(m) && !layer.is_group()) {
      ImGui::BeginDisabled(!m.show_viewport);
      const int added = modifier_added_points(doc, layer, size_t(i));
      const std::string tip = "Show the points this modifier adds, as hollow teal dots where they end up (they "
                              "can't be picked): " +
                              (added > 0 ? std::to_string(added) + (added == 1 ? " point" : " points") + " now"
                                         : std::string("it adds none now"));
      if (icon_toggle("##points", m.show_points && m.show_viewport, tip.c_str(), icon_points))
      {
        m.show_points = !m.show_points;
        C.undo->push("Modifier Show Points", doc);
      }
      ImGui::EndDisabled();
    }
    else {
      ImGui::Dummy(ImVec2(h, h));
    }
    /* Blender's header menu: Apply, Duplicate, Move to First / Last. */
    ImGui::SameLine(0, 4);
    const ImVec2 menu_at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##menu", ImVec2(h, h))) {
      ImGui::OpenPopup("##modifier_menu");
    }
    if (ImGui::IsItemHovered()) {
      dl->AddRectFilled(menu_at, menu_at + ImVec2(h, h), ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    ImGui::RenderArrow(dl, ImVec2(menu_at.x + h * 0.22f, menu_at.y + h * 0.22f), COL_MOD_ICON, ImGuiDir_Down, 0.75f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Apply, duplicate or move the modifier");
    }
    if (ImGui::BeginPopup("##modifier_menu")) {
      if (ImGui::MenuItem("Apply", "Ctrl A", false, can_apply)) {
        apply = i;
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(armature ? "The Armature modifier can't be applied: it comes with the bone layer"
                          : !setup ? "On Setup (frame 0): make it part of the drawing"
                                   : "Make it part of the drawing, and remove it");
      }
      if (ImGui::MenuItem("Duplicate", "Shift D", false, !armature)) {
        duplicate = i;
      }
      if (modifier_bends(m.type) && ImGui::BeginMenu("Change Type")) {
        /* Another deform, keeping its settings and painted weights. */
        for (const ModifierType type :
             {ModifierType::CurveDeform, ModifierType::CageDeform, ModifierType::MeshDeform})
        {
          if (ImGui::MenuItem(deform_type_name(type), nullptr, type == m.type) && type != m.type) {
            if (m.name.rfind(deform_type_name(m.type), 0) == 0) {
              m.name = deform_type_name(type) + m.name.substr(std::string(deform_type_name(m.type)).size());
            }
            m.type = type;
            C.undo->push("Change Modifier Type", doc);
          }
        }
        ImGui::EndMenu();
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Move to First", nullptr, false, i > 0)) {
        drag_from = i;
        drag_to = 0;
      }
      if (ImGui::MenuItem("Move to Last", nullptr, false, i + 1 < int(layer.modifiers.size()))) {
        drag_from = i;
        drag_to = int(layer.modifiers.size());
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 2);
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
        modifier_row_label("Object", column, "The bone layer whose bones move this drawing");
        ImGui::BeginDisabled();
        std::string name = bones ? bones->name : std::string("(none)");
        ImGui::InputText("##object", &name, ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
        modifier_row_label("", column);
        ImGui::TextDisabled("The bone layer's bones move this drawing.");
      }
      else if (m.type == ModifierType::Boolean) {
        merge_modifier_body(C, layer, m, i, column);
      }
      else if (m.type == ModifierType::Bevel) {
        bevel_modifier_body(C, layer, m, i, column);
      }
      else if (m.type == ModifierType::Resample) {
        resample_modifier_body(C, layer, m, i, column);
      }
      else if (modifier_is_deform(m.type)) {
        vector_deform_modifier_body(C, layer, m, i, column);
      }
      else {
        bool changed = false;
        modifier_row_label("Axis", column, "Which way to reflect the drawing: X across a vertical line, Y across a horizontal one");
        const std::string driven_axis_path = std::string("modifiers[" + std::to_string(i) + "].") + "axis_x";
        const int driven_axis = ui_driver_field_begin(C, layer.id, driven_axis_path, -1);
        changed |= axis_toggles("##axis", &m.axis_x, &m.axis_y);
        ui_driver_typed(C, layer.id, driven_axis_path, -1);
        ui_driver_field_end(C, layer.id, driven_axis_path, -1, driven_axis);
        modifier_row_label("Mirror Center", column, "Where the mirror line runs through");
        const std::string driven_center_path = std::string("modifiers[" + std::to_string(i) + "].") + "center";
        const int driven_center = ui_driver_field_begin(C, layer.id, driven_center_path, -1);
        ui_drag_float2("##center", &m.center.x, 0.005f, -1000.0f, 1000.0f, "%.3f");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ui_driver_typed(C, layer.id, driven_center_path, -1);
        ui_driver_field_end(C, layer.id, driven_center_path, -1, driven_center);
        ImGui::Spacing();
        modifier_row_label("Merge", column,
                           "Share points on the axis, so lines ending there join their reflection.\n"
                           "The number is how close to the axis a point must be to be shared");
        const std::string driven_merge_path = std::string("modifiers[" + std::to_string(i) + "].") + "merge";
        const int driven_merge = ui_driver_field_begin(C, layer.id, driven_merge_path, -1);
        changed |= ImGui::Checkbox("##merge", &m.merge);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("Share points on the axis, so lines ending there join their reflection");
        }
        ui_driver_typed(C, layer.id, driven_merge_path, -1);
        ui_driver_field_end(C, layer.id, driven_merge_path, -1, driven_merge);
        ImGui::SameLine();
        ImGui::BeginDisabled(!m.merge);
        const std::string driven_merge_distance_path = std::string("modifiers[" + std::to_string(i) + "].") + "merge_distance";
        const int driven_merge_distance = ui_driver_field_begin(C, layer.id, driven_merge_distance_path, -1);
        ui_drag_float("##merge_distance", &m.merge_distance, 0.0005f, 0.0f, 1.0f, "%.4f");
        changed |= ImGui::IsItemDeactivatedAfterEdit();
        ui_driver_typed(C, layer.id, driven_merge_distance_path, -1);
        ui_driver_field_end(C, layer.id, driven_merge_distance_path, -1, driven_merge_distance);
        ImGui::EndDisabled();
        ImGui::Spacing();
        /* Blender keeps vertex group mirroring under Data. */
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
        const std::string data_key = ui_fold_scope() + "/Mirror/Data";
        ui_fold_next(data_key, false);
        const bool data_open = ImGui::TreeNodeEx("Data", ImGuiTreeNodeFlags_SpanAvailWidth);
        ui_fold_record(data_key, false, data_open);
        if (data_open) {
          modifier_row_label("", column);
          const std::string driven_mirror_bones_path = std::string("modifiers[" + std::to_string(i) + "].") + "mirror_bones";
          const int driven_mirror_bones = ui_driver_field_begin(C, layer.id, driven_mirror_bones_path, -1);
          changed |= ImGui::Checkbox("Mirror Bones", &m.mirror_bones);
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Reflected points bound (or weighted) to ..._L bones follow ..._R ones, and the "
                              "other way (Blender's Vertex Groups)");
          }
          ui_driver_typed(C, layer.id, driven_mirror_bones_path, -1);
          ui_driver_field_end(C, layer.id, driven_mirror_bones_path, -1, driven_mirror_bones);
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
    /* Over its box, as in Blender: Ctrl A applies it (not the Armature modifier, and only on Setup),
     * Shift D duplicates it. */
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(box_min, box_max) &&
        !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive())
    {
      if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_None, ImGuiKeyOwner_Any)) {
        if (can_apply) {
          apply = i;
        }
        else {
          report(ReportType::Warning, armature ? "The Armature modifier can't be applied"
                                               : "Apply works on Setup (frame 0)");
        }
      }
      if (!armature && ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_D, ImGuiInputFlags_None, ImGuiKeyOwner_Any)) {
        duplicate = i;
      }
    }
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
  if (apply >= 0) {
    operator_call(C, "object.modifier_apply", {{"index", std::to_string(apply)}});
  }
  if (duplicate >= 0) {
    operator_call(C, "object.modifier_duplicate", {{"index", std::to_string(duplicate)}});
  }
  if (drag_from >= 0) {
    /* Index the modifier ends up at, after taking it out of its place. */
    const int to = drag_to > drag_from ? drag_to - 1 : drag_to;
    if (to != drag_from) {
      operator_call(C, "object.modifier_move_to", {{"index", std::to_string(drag_from)}, {"to", std::to_string(to)}});
    }
  }
}

/* --- Bone Constraints tab: the Modifiers tab's boxes, for the active bone's constraints ------- */

static constexpr const char *CONSTRAINT_PAYLOAD = "OANIM_BONE_CONSTRAINT";

/* A constraint's icon: a spring for Dynamics, a chain for IK, two linked rings for the rest (as
 * Blender marks constraints). */
static void constraint_type_icon(ImDrawList *dl, ImVec2 c, float s, BoneConstraintType type)
{
  if (type == BoneConstraintType::Dynamics) {
    ImVec2 pts[7];
    for (int i = 0; i < 7; i++) {
      pts[i] = ImVec2(c.x - s + float(i) * s / 3.0f, c.y + (i % 2 == 0 ? -s * 0.5f : s * 0.5f));
    }
    dl->AddPolyline(pts, 7, COL_MOD_ICON, 0, 1.3f);
  }
  else if (type == BoneConstraintType::IK) {
    dl->AddRect(ImVec2(c.x - s, c.y - s * 0.35f), ImVec2(c.x - s * 0.1f, c.y + s * 0.35f), COL_MOD_ICON, s * 0.3f, 0, 1.3f);
    dl->AddRect(ImVec2(c.x + s * 0.1f, c.y - s * 0.35f), ImVec2(c.x + s, c.y + s * 0.35f), COL_MOD_ICON, s * 0.3f, 0, 1.3f);
    dl->AddLine(ImVec2(c.x - s * 0.4f, c.y), ImVec2(c.x + s * 0.4f, c.y), COL_MOD_ICON, 1.3f);
  }
  else {
    dl->AddCircle(ImVec2(c.x - s * 0.35f, c.y), s * 0.55f, COL_MOD_ICON, 0, 1.3f);
    dl->AddCircle(ImVec2(c.x + s * 0.35f, c.y), s * 0.55f, COL_MOD_ICON, 0, 1.3f);
  }
}

/* Blender's Bone Constraints tab: Add Bone Constraint, then a box per constraint, top first, laid
 * out as the Modifiers tab. Drag a box by its handle to change the order; Dynamics stays last. */
static void constraints_panel(Context &C, Layer &armature)
{
  Document &doc = *C.document;
  const ImGuiStyle &style = ImGui::GetStyle();
  /* Only the active bone's, and only while it is selected: with nothing selected there is no bone
   * to give constraints to (the active bone stays remembered after deselecting). */
  Bone *bone = bone_find(armature, armature.active_bone);
  if (!bone || !(bone->selected || bone->select_head || bone->select_tail)) {
    ImGui::TextWrapped("Select a bone to give it constraints.");
    return;
  }
  const std::string bone_name = bone->name;
  if (ImGui::Button("+  Add Bone Constraint", ImVec2(-1, 0))) {
    ImGui::OpenPopup("##add_constraint");
  }
  if (ImGui::BeginPopup("##add_constraint")) {
    /* Grouped as Blender's menu. */
    static const std::pair<const char *, std::vector<BoneConstraintType>> groups[] = {
        {"Transform",
         {BoneConstraintType::CopyAngle, BoneConstraintType::CopyPosition, BoneConstraintType::CopyScale,
          BoneConstraintType::LimitAngle}},
        {"Tracking", {BoneConstraintType::TrackTo, BoneConstraintType::StretchTo, BoneConstraintType::IK,
                      BoneConstraintType::LookAt}},
        {"Limit", {BoneConstraintType::LimitShape}},
        {"Relationship", {BoneConstraintType::ChildOf, BoneConstraintType::FollowPoint,
                          BoneConstraintType::IndependentAngle, BoneConstraintType::MaintainVolume}},
        {"Physics", {BoneConstraintType::Dynamics}},
    };
    for (const auto &[group, types] : groups) {
      ImGui::TextDisabled("%s", group);
      for (const BoneConstraintType type : types) {
        if (ImGui::MenuItem(bone_constraint_name(type))) {
          operator_call(C, "armature.constraint_add",
                        {{"type", bone_constraint_identifier(type)}, {"bone", bone_name}});
        }
      }
    }
    ImGui::EndPopup();
  }
  ImGui::Spacing();
  if (bone->constraints.empty()) {
    ImGui::TextDisabled("%s has no constraints. They act on posed frames, never on the setup frame (0).",
                        bone_name.c_str());
    return;
  }

  int remove = -1, drag_from = -1, drag_to = -1;
  const float h = ImGui::GetFrameHeight();
  for (int i = 0; i < int(bone->constraints.size()); i++) {
    BoneConstraint &c = bone->constraints[i];
    const bool dynamics = c.type == BoneConstraintType::Dynamics;
    ImGui::PushID(i);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const ImVec2 box_min = ImGui::GetCursorScreenPos();
    const float box_w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorScreenPos(ImVec2(box_min.x + 4.0f, box_min.y + 4.0f));
    ImGui::BeginGroup();

    /* Header: collapse arrow, icon, name, on/off, X, drag handle. */
    const float inner_w = box_w - 8.0f;
    if (ImGui::ArrowButtonEx("##expand", c.expanded ? ImGuiDir_Down : ImGuiDir_Right, ImVec2(h * 0.8f, h),
                             ImGuiButtonFlags_None)) {
      c.expanded = !c.expanded;
    }
    ImGui::SameLine(0, 2);
    const ImVec2 icon_at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    constraint_type_icon(dl, ImVec2(icon_at.x + h * 0.5f, icon_at.y + h * 0.5f), h * 0.3f, c.type);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", bone_constraint_name(c.type));
    }
    ImGui::SameLine(0, 2);
    const float right_w = h * 2.0f + h * 0.8f + style.ItemSpacing.x * 2.0f + 6.0f;
    ImGui::SetNextItemWidth(std::max(40.0f, inner_w - (ImGui::GetCursorScreenPos().x - box_min.x) - right_w));
    if (c.name.empty()) {
      c.name = bone_constraint_name(c.type);
    }
    ImGui::InputText("##name", &c.name);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Rename Constraint", doc);
    }
    ImGui::SameLine(0, 4);
    {
      /* Drivable, as Blender's constraint toggle: purple and naming its driver while driven. */
      const std::string enabled_path = "bones[\"" + bone_name + "\"].constraints[" + std::to_string(i) + "].enabled";
      const int driven = ui_driver_field_begin(C, armature.id, enabled_path, -1);
      if (icon_toggle("##enabled", c.enabled, "Enable or disable the constraint", icon_monitor, driven > 0)) {
        c.enabled = !c.enabled;
        C.undo->push("Constraint On/Off", doc);
      }
      ui_driver_field_end(C, armature.id, enabled_path, -1, driven);
    }
    ImGui::SameLine(0, 4);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    if (ImGui::Button("X", ImVec2(h, h))) {
      remove = i;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Delete constraint");
    }
    ImGui::SameLine(0, 2);
    /* The drag handle (Dynamics stays last: it has none). */
    const ImVec2 grip = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(dynamics);
    ImGui::InvisibleButton("##grip", ImVec2(h * 0.8f, h));
    const bool grip_hovered = !dynamics && (ImGui::IsItemHovered() || ImGui::IsItemActive());
    ImGui::EndDisabled();
    for (int r = 0; r < 3 && !dynamics; r++) {
      for (int k = 0; k < 2; k++) {
        dl->AddCircleFilled(ImVec2(grip.x + h * 0.25f + k * h * 0.3f, grip.y + h * 0.3f + r * h * 0.2f), 1.4f,
                            grip_hovered ? COL_MOD_ICON : IM_COL32(0x90, 0x90, 0x90, 255));
      }
    }
    if (grip_hovered) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (!dynamics && ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(CONSTRAINT_PAYLOAD, &i, sizeof(int));
      ImGui::TextUnformatted(c.name.c_str());
      ImGui::EndDragDropSource();
    }

    /* Body. */
    if (c.expanded) {
      ImGui::Spacing();
      const float column = inner_w * 0.38f;
      ImGui::PushItemWidth(-4.0f);
      const auto done = [&](const char *what) {
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          C.undo->push(what, doc);
        }
      };
      /* As Parent Bone's: a search field on top of the list, and an eyedropper to click the bone. */
      /* A bone the constraint uses (its Target, Look At's Pupil Center): a list with a search field and
       * an eyedropper, as Parent Bone's. */
      const auto bone_row = [&](const char *label, const char *tip, uint64_t BoneConstraint::*field, const char *id,
                                const char *prompt) {
        modifier_row_label(label, column, tip);
        ImGui::PushID(id);
        const Bone *target = bone_find(armature, c.*field);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f - ImGui::GetFrameHeight() - 2.0f);
        if (ImGui::BeginCombo("##target", target ? target->name.c_str() : "None", ImGuiComboFlags_HeightLarge)) {
          static std::string search;
          if (ImGui::IsWindowAppearing()) {
            search.clear();
            ImGui::SetKeyboardFocusHere();
          }
          ImGui::SetNextItemWidth(-1);
          const bool entered = ImGui::InputTextWithHint("##search", "Search bones", &search,
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
          std::string needle = search;
          std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
          const auto matches = [&](const std::string &name) {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            return lower.find(needle) != std::string::npos;
          };
          if (needle.empty() && ImGui::Selectable("None", !target)) {
            c.*field = 0;
            C.undo->push("Constraint Target", doc);
          }
          const Bone *first = nullptr;
          for (const Bone &b : armature.bones) {
            if (b.id == bone->id || !matches(b.name)) {
              continue;
            }
            first = first ? first : &b;
            if (ImGui::Selectable(b.name.c_str(), target && b.id == target->id)) {
              c.*field = b.id;
              C.undo->push("Constraint Target", doc);
            }
          }
          if (!first && !needle.empty()) {
            ImGui::TextDisabled("No bone named like that");
          }
          if (entered && first) {
            c.*field = first->id;
            C.undo->push("Constraint Target", doc);
            ImGui::CloseCurrentPopup();
          }
          ImGui::EndCombo();
        }
        ImGui::SameLine(0, 2);
        const uint64_t rig_id = armature.id, own = bone->id;
        const size_t at = size_t(i);
        if (ui_pick_button("##pick_target", "Eyedropper: click the bone in the viewport",
                           ui_bone_eyedropper_armature() == rig_id))
        {
          ui_bone_eyedropper_start(
              rig_id,
              [rig_id, own, at, field](Context &C, uint64_t picked) {
                Layer *rig = layer_find(*C.document, rig_id);
                Bone *b = rig ? bone_find(*rig, own) : nullptr;
                if (!b || at >= b->constraints.size()) {
                  return true;
                }
                if (picked == own) {
                  return false;
                }
                b->constraints[at].*field = picked;
                C.undo->push("Constraint Target", *C.document);
                return true;
              },
              [prompt] { return std::string(prompt); });
        }
        ImGui::PopID();
      };
      const auto target_row = [&] {
        bone_row("Target", "The bone this constraint follows, copies or reaches for", &BoneConstraint::target,
                 "target", "Click the target bone  |  Esc cancels");
      };
      /* A keyable number of this constraint: its field leaves room for the key button after it. */
      const std::string constraint_path = "bones[\"" + bone_name + "\"].constraints[" + std::to_string(i) + "].";
      const auto keyed_begin = [&](const std::string &p) {
        const int state = layer_key_state(armature, {p}, doc.frame_current);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
        const int tint = ui_key_tint_push(state | layer_action_bone_state(armature, {p}));
        /* The driver tint over the key's: pushed last, popped first. */
        return std::pair<int, int>(state, tint | (ui_driver_field_begin(C, armature.id, p, -1) << 8));
      };
      const auto keyed_end = [&](const std::string &p, std::pair<int, int> s) {
        ui_driver_typed(C, armature.id, p, -1);
        ui_driver_field_end(C, armature.id, p, -1, s.second >> 8);
        ImGui::PopStyleColor(s.second & 0xff);
        ImGui::PushID(p.c_str());
        if (ui_key_button("##key", s.first)) {
          operator_call(C, "anim.keyframe_toggle", {{"layer", std::to_string(armature.id)}, {"paths", p}});
        }
        ui_flash_row(armature.id, p);
        const int pin = pin_state(armature, {p}, s.first);
        if (ui_pin_button("##pin", pin)) {
          pin_toggle(C, armature.id, p, pin);
        }
        if (pin == 1) {
          ui_pinned_row();
        }
        ImGui::PopID();
      };
      /* A channel: a checkbox, and its settings under it, one labelled row each, while it is on. */
      const auto spring_rows = [&](const char *label, BoneSpring &sp) {
        ImGui::PushID(label);
        modifier_row_label("", column);
        const std::string driven_spring_path = constraint_path + (&sp == &c.angle_spring ? "angle_spring." : &sp == &c.position_spring ? "position_spring." : "scale_spring.") + "enabled";
        const int driven_spring = ui_driver_field_begin(C, armature.id, driven_spring_path, -1);
        if (ImGui::Checkbox(label, &sp.enabled)) {
          C.undo->push("Dynamics", doc);
        }
        ui_driver_typed(C, armature.id, driven_spring_path, -1);
        ui_driver_field_end(C, armature.id, driven_spring_path, -1, driven_spring);
        const bool is_angle = &sp == &c.angle_spring, is_position = &sp == &c.position_spring;
        const std::string spring_path =
            constraint_path + (is_angle ? "angle_spring." : is_position ? "position_spring." : "scale_spring.");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("%s", is_angle    ? "Swing: when the bone moves or turns, its tip lags behind and springs back"
                                  : is_position ? "Jiggle: when the bone above moves, the head lags behind and springs back"
                                                : "Squash & Stretch: when the bone moves along its length, it stretches or squashes "
                                                  "and springs back");
        }
        if (sp.enabled) {
          /* `shown`: what one of the value shows as (100 for a percentage, degrees for radians). */
          const auto field = [&](const char *name, const char *id, float *value, float shown, float speed, float lo,
                                 float hi, const char *fmt, const char *tip) {
            modifier_row_label(name, column, tip);
            /* The setting's path: its id without the ##. */
            const std::string p = spring_path + (std::string(id) == "##settle" ? "settle_speed" : std::string(id + 2));
            const auto k = keyed_begin(p);
            float v = *value * shown;
            if (ui_drag_float(id, &v, speed, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) {
              *value = v / shown;
            }
            done("Dynamics");
            keyed_end(p, k);
          };
          field("Settle Speed", "##settle", &sp.settle_speed, 1.0f, 0.01f, 0.05f, 20.0f, "%.2f",
                "How quickly the swinging dies down once the motion stops: about half a second\n"
                "at 1, twice as fast at 2. A big swing takes longer to calm.");
          field("Bounce", "##bounce", &sp.bounce, 100.0f, 0.5f, 0.0f, 90.0f, "%.0f%%",
                "How far past its keys it swings: 0% eases in without passing, about 30% swings\n"
                "forward, back and settles, more swings further and more times.");
          if (is_angle || is_position) {
            field("Inertia", "##inertia", &sp.inertia, 100.0f, 0.5f, 0.0f, 200.0f, "%.0f%%",
                  is_angle ? "How much movement swings it: as what it hangs from moves or turns, the\n"
                             "strand's tip stays behind. 100% is natural; 0% moves rigidly with the body."
                           : "How much movement jiggles it: as the bone above moves, its head stays\n"
                             "behind and springs back. 100% is natural; 0% moves rigidly with it.");
          }
          else {
            field("Stretch", "##stretch", &sp.stretch, 100.0f, 0.5f, 0.0f, 200.0f, "%.0f%%",
                  "How stretchy it is: moving along the bone stretches or squashes it (its tip\n"
                  "stays behind as its head moves), and swinging fast pulls it longer, most at the\n"
                  "fastest point of a whip. 0% only follows its keyed scale (and Weight).");
          }
          /* The limit: a checkbox, then its value, greyed out while it is off. */
          const auto limit_row = [&](const char *name, float shown, float speed, float hi, const char *fmt,
                                     const char *tip) {
            modifier_row_label(name, column, tip);
            const std::string driven_use_limit_path = spring_path + "use_limit";
            const int driven_use_limit = ui_driver_field_begin(C, armature.id, driven_use_limit_path, -1);
            if (ImGui::Checkbox("##use_limit", &sp.use_limit)) {
              C.undo->push("Dynamics", doc);
            }
            ui_driver_typed(C, armature.id, driven_use_limit_path, -1);
            ui_driver_field_end(C, armature.id, driven_use_limit_path, -1, driven_use_limit);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
              ImGui::SetTooltip("%s", tip);
            }
            ImGui::SameLine(0, 4);
            ImGui::BeginDisabled(!sp.use_limit);
            const auto k = keyed_begin(spring_path + "limit");
            float v = sp.limit * shown;
            if (ui_drag_float("##limit", &v, speed, speed, hi, fmt, ImGuiSliderFlags_AlwaysClamp)) {
              sp.limit = v / shown;
            }
            done("Dynamics");
            ImGui::EndDisabled();
            keyed_end(spring_path + "limit", k);
          };
          if (is_angle) {
            limit_row("Max Swing", 180.0f / PI, 0.5f, 180.0f, "%.0f\xC2\xB0",
                      "The furthest it can swing from where its keys put it, slowing softly as it\n"
                      "nears. Stops a fast move flinging it too far. Unticked: no limit.");
          }
          else if (is_position) {
            limit_row("Max Distance", 1.0f, 0.005f, 1000.0f, "%.3f",
                      "The furthest the head can drift from where it should be, slowing softly as it\n"
                      "nears. Unticked: no limit.");
          }
          else {
            /* Shown as times its length: Max Stretch 1 + limit, Max Squash 1 - squash_limit. */
            const auto factor_row = [&](const char *name, const char *field_name, bool *use, float *value, float sign,
                                        float lo, float hi, const char *tip) {
              const std::string use_name = std::string("use_") + field_name;
              const std::string value_path = spring_path + field_name;
              modifier_row_label(name, column, tip);
              const std::string driven_path = spring_path + use_name;
              const int driven = ui_driver_field_begin(C, armature.id, driven_path, -1);
              if (ImGui::Checkbox(("##" + use_name).c_str(), use)) {
                C.undo->push("Dynamics", doc);
              }
              ui_driver_typed(C, armature.id, driven_path, -1);
              ui_driver_field_end(C, armature.id, driven_path, -1, driven);
              if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s", tip);
              }
              ImGui::SameLine(0, 4);
              ImGui::BeginDisabled(!*use);
              const auto k = keyed_begin(value_path);
              float v = 1.0f + sign * *value;
              if (ui_drag_float(("##" + std::string(field_name)).c_str(), &v, 0.005f, lo, hi, "%.2f\xC3\x97",
                                   ImGuiSliderFlags_AlwaysClamp))
              {
                *value = (v - 1.0f) * sign;
              }
              done("Dynamics");
              ImGui::EndDisabled();
              keyed_end(value_path, k);
            };
            factor_row("Max Stretch", "limit", &sp.use_limit, &sp.limit, 1.0f, 1.01f, 6.0f,
                       "The longest it can stretch, as times its length (1.5: half again), slowing\n"
                       "softly as it nears. Unticked: no limit.");
            factor_row("Max Squash", "squash_limit", &sp.use_squash_limit, &sp.squash_limit, -1.0f, 0.1f, 0.99f,
                       "The shortest it can squash, as times its length (0.7: down to 70%), slowing\n"
                       "softly as it nears. Unticked: no limit, though it never goes below 0.1.");
          }
        }
        ImGui::PopID();
      };
      /* Limit to Shape's rows, which Look At has too (its eye shape): the layer (searchable, with an
       * eyedropper), which of its shapes, what stays inside, Overlap and Softness. */
      const auto shape_rows = [&] {
            /* The layer, as Parent Bone's list (a search field on top), with an eyedropper that takes a shape
             * clicked in the viewport; then which of its closed shapes. */
            modifier_row_label("Layer", column, "The layer whose closed shape the bone stays inside, as it is drawn on each\n"
                                                "frame: a pupil inside its eye white, however the head turns");
            const Layer *target = layer_find(doc, c.shape_layer);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f - ImGui::GetFrameHeight() - 2.0f);
            if (ImGui::BeginCombo("##shape_layer", target ? target->name.c_str() : "None", ImGuiComboFlags_HeightLarge)) {
              static std::string search;
              if (ImGui::IsWindowAppearing()) {
                search.clear();
                ImGui::SetKeyboardFocusHere();
              }
              ImGui::SetNextItemWidth(-1);
              const bool entered = ImGui::InputTextWithHint("##search", "Search layers", &search,
                                                            ImGuiInputTextFlags_EnterReturnsTrue);
              std::string needle = search;
              std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
              const auto matches = [&](const std::string &name) {
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                return lower.find(needle) != std::string::npos;
              };
              if (needle.empty() && ImGui::Selectable("None", !target)) {
                c.shape_layer = 0;
                c.shape_id = 0;
                C.undo->push("Limit to Shape", doc);
              }
              const Layer *first = nullptr;
              for (const Layer *l : document_layers_flat(doc)) {
                if (!l->has_drawing() || l->drawing.shapes.empty() || !matches(l->name)) {
                  continue;
                }
                first = first ? first : l;
                ImGui::PushID(int(l->id));
                if (ImGui::Selectable(l->name.c_str(), target && target->id == l->id)) {
                  c.shape_layer = l->id;
                  c.shape_id = 0;
                  C.undo->push("Limit to Shape", doc);
                }
                ImGui::PopID();
              }
              if (!first && !needle.empty()) {
                ImGui::TextDisabled("No layer named like that");
              }
              if (entered && first) {
                c.shape_layer = first->id;
                c.shape_id = 0;
                C.undo->push("Limit to Shape", doc);
                ImGui::CloseCurrentPopup();
              }
              ImGui::EndCombo();
            }
            ImGui::SameLine(0, 2);
            {
              const uint64_t rig_id = armature.id, bone_id = bone->id, uid_index = uint64_t(i);
              const char *tag = "LIMIT_SHAPE";
              if (ui_pick_button("##pick_limit_shape", "Eyedropper: click the shape in the viewport",
                                 ui_shape_eyedropper_active(tag)))
              {
                ui_shape_eyedropper_start(
                    [rig_id, bone_id, uid_index](Context &C, uint64_t layer_id, int shape_index) {
                      Document &d = *C.document;
                      Layer *rig = layer_find(d, rig_id);
                      Bone *b = rig ? bone_find(*rig, bone_id) : nullptr;
                      if (!b || uid_index >= b->constraints.size() ||
                          (b->constraints[uid_index].type != BoneConstraintType::LimitShape &&
                           b->constraints[uid_index].type != BoneConstraintType::LookAt))
                      {
                        return true;
                      }
                      const Layer *clicked = layer_find(d, layer_id);
                      if (!clicked || !clicked->has_drawing()) {
                        return false;
                      }
                      const VectorDrawing shown = layer_evaluated_drawing(d, *clicked, false);
                      if (shape_index < 0 || shape_index >= int(shown.shapes.size())) {
                        return false;
                      }
                      BoneConstraint &lc = b->constraints[uid_index];
                      lc.shape_layer = clicked->id;
                      lc.shape_id = shown.shapes[size_t(shape_index)].id;
                      C.undo->push("Limit to Shape", d);
                      return true;
                    },
                    [] { return std::string("Click the shape to stay inside  |  Esc cancels"); }, tag);
              }
            }
            modifier_row_label("Shape", column, "Which of the layer's closed shapes (First Closed: the first one that goes all the way round)");
            ImGui::BeginDisabled(!target);
            const Shape *picked = nullptr;
            if (target) {
              for (const Shape &s : target->drawing.shapes) {
                if (s.id == c.shape_id) {
                  picked = &s;
                }
              }
            }
            if (ImGui::BeginCombo("##shape_id", picked ? (picked->name.empty() ? "Shape" : picked->name.c_str()) : "First Closed")) {
              if (ImGui::Selectable("First Closed", !picked)) {
                c.shape_id = 0;
                C.undo->push("Limit to Shape", doc);
              }
              for (size_t k = 0; target && k < target->drawing.shapes.size(); k++) {
                const Shape &s = target->drawing.shapes[k];
                const std::string label = (s.name.empty() ? "Shape " + std::to_string(k + 1) : s.name) + "##" + std::to_string(s.id);
                if (ImGui::Selectable(label.c_str(), picked == &s)) {
                  c.shape_id = s.id;
                  C.undo->push("Limit to Shape", doc);
                }
              }
              ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            if (target && c.shape_region.empty() && c.enabled) {
              modifier_row_label("", column);
              ImGui::TextDisabled("No closed shape there");
            }
            /* What stays inside: the head, or the drawings the bone carries. */
            modifier_row_label("Keep Inside", column, "Center: the bone's head stays inside.\n"
                                                      "Whole Drawing: all of what the bone carries (its pupil) fits inside.");
            {
              const float w = (ImGui::GetContentRegionAvail().x - 4.0f - 1.0f) * 0.5f;
              ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, style.ItemSpacing.y));
              static const char *const FITS[] = {"Center", "Whole Drawing"};
              for (int k = 0; k < 2; k++) {
                if (k > 0) {
                  ImGui::SameLine();
                }
                const bool on = int(c.shape_fit) == k;
                if (on) {
                  ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button(FITS[k], ImVec2(w, 0)) && !on) {
                  c.shape_fit = ShapeFit(k);
                  C.undo->push("Limit to Shape", doc);
                }
                if (on) {
                  ImGui::PopStyleColor();
                }
              }
              ImGui::PopStyleVar();
            }
            if (c.shape_fit == ShapeFit::Whole) {
              modifier_row_label("Drawing", column, "What has to fit inside: a layer, or (Auto) the layers that follow this bone");
              const Layer *fit = layer_find(doc, c.fit_layer);
              if (ImGui::BeginCombo("##fit_layer", fit ? fit->name.c_str() : "Auto", ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("Auto", !fit)) {
                  c.fit_layer = 0;
                  C.undo->push("Limit to Shape", doc);
                }
                for (const Layer *l : document_layers_flat(doc)) {
                  if (!l->has_drawing()) {
                    continue;
                  }
                  ImGui::PushID(int(l->id));
                  if (ImGui::Selectable(l->name.c_str(), fit && fit->id == l->id)) {
                    c.fit_layer = l->id;
                    C.undo->push("Limit to Shape", doc);
                  }
                  ImGui::PopID();
                }
                ImGui::EndCombo();
              }
              modifier_row_label("Overlap", column, "How far it may go past the edge, as a part of its size: an eyelid or a\n"
                                                    "mask can cover that much. 0% keeps all of it inside.");
              float percent = c.overlap * 100.0f;
              const std::string overlap_path = constraint_path + "overlap";
              const int driven_overlap = ui_driver_field_begin(C, armature.id, overlap_path, -1);
              if (ui_drag_float("##overlap", &percent, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
                c.overlap = percent / 100.0f;
              }
              done("Limit to Shape");
              ui_driver_typed(C, armature.id, overlap_path, -1);
              ui_driver_field_end(C, armature.id, overlap_path, -1, driven_overlap);
            }
            modifier_row_label("Softness", column, "How far from the edge it starts slowing down, so it eases to a stop\n"
                                                   "instead of hitting the edge (0: a hard stop)");
            const std::string softness_path = constraint_path + "softness";
            const int driven_softness = ui_driver_field_begin(C, armature.id, softness_path, -1);
            ui_drag_float("##softness", &c.softness, 0.0005f, 0.0f, 1000.0f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
            done("Limit to Shape");
            ui_driver_typed(C, armature.id, softness_path, -1);
            ui_driver_field_end(C, armature.id, softness_path, -1, driven_softness);
      };
      switch (c.type) {
        case BoneConstraintType::LimitAngle: {
          /* Min and Max, each with a button that takes the bone's angle now: pose it to the end you want,
           * then click. */
          for (const bool max : {false, true}) {
            modifier_row_label(max ? "Max" : "Min", column,
                               max ? "The furthest the bone can turn counter-clockwise from its setup angle, as its Angle "
                                     "in the Bone panel shows it"
                                   : "The furthest the bone can turn clockwise from its setup angle, as its Angle in the "
                                     "Bone panel shows it");
            const float bh = ImGui::GetFrameHeight();
            float &value = max ? c.max_angle : c.min_angle;
            float shown = value * 180.0f / PI;
            ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - bh - style.ItemInnerSpacing.x));
            if (ui_drag_float(max ? "##max_angle" : "##min_angle", &shown, 0.5f, -360.0f, 360.0f, "%.1f\xC2\xB0")) {
              value = shown * PI / 180.0f;
            }
            done("Limit Angle");
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Button(max ? "##max_current" : "##min_current", ImVec2(bh, bh))) {
              operator_call(C, "armature.limit_angle_set_current",
                            {{"bone", bone_name}, {"index", std::to_string(i)}, {"end", max ? "MAX" : "MIN"}});
            }
            /* An angle: two arms from a corner, the moving one with a dot at its tip (the bone now). */
            ImDrawList *idl = ImGui::GetWindowDrawList();
            const ImU32 icon = IM_COL32(0xe6, 0xe6, 0xe6, 255);
            const float s = bh * 0.3f;
            const ImVec2 o(at.x + bh * 0.5f - s * 0.8f, at.y + bh * 0.5f + s * 0.7f);
            const ImVec2 tip(o.x + s * 1.3f, o.y - s * 1.3f);
            idl->AddLine(o, ImVec2(o.x + s * 1.8f, o.y), icon, 1.3f);
            idl->AddLine(o, tip, icon, 1.3f);
            idl->AddCircleFilled(tip, 1.8f, icon);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
              ImGui::SetTooltip(max ? "Set to Current Angle: the bone's Angle now becomes its Max"
                                    : "Set to Current Angle: the bone's Angle now becomes its Min");
            }
          }
          break;
        }
        case BoneConstraintType::IndependentAngle:
          modifier_row_label("", column);
          ImGui::TextDisabled("Keeps its angle when its parent turns.");
          break;
        case BoneConstraintType::LimitShape: {
          shape_rows();
          modifier_row_label("", column);
          if (ImGui::Checkbox("Affect Transform", &c.affect_transform)) {
            C.undo->push("Limit to Shape", doc);
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Moving the bone past the edge stops its own position there too, so its keys hold where\n"
                              "it shows (Blender's Affect Transform). Off, only where it shows is held: its position can\n"
                              "go on past the edge, and has to come all the way back before it moves again");
          }
          break;
        }
        case BoneConstraintType::FollowPoint: {
          modifier_row_label("Deform", column, "The Curve, Cage or Mesh Deform to follow. The bone takes the move it gives a\n"
                                               "point of its own, which starts at the bone's head, and doesn't bend");
          Layer *owner = nullptr;
          const Modifier *followed = follow_point_modifier(doc, c, &owner);
          const std::string shown = followed ? owner->name + ": " + followed->name : std::string("None");
          if (ImGui::BeginCombo("##follow_deform", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("None", !followed)) {
              follow_point_aim(doc, armature, *bone, c, nullptr, nullptr);
              C.undo->push("Follow Point", doc);
            }
            for (const Layer *l : document_layers_flat(doc)) {
              Layer &candidate = *layer_find(doc, l->id);
              for (Modifier &m : candidate.modifiers) {
                if (!modifier_is_deform(m.type)) {
                  continue;
                }
                const std::string label = candidate.name + ": " + m.name;
                ImGui::PushID(&m);
                if (ImGui::Selectable(label.c_str(), &m == followed)) {
                  follow_point_aim(doc, armature, *bone, c, &candidate, &m);
                  C.undo->push("Follow Point", doc);
                }
                ImGui::PopID();
              }
            }
            ImGui::EndCombo();
          }
          /* Its own place, or a point the shape already has. */
          const auto point_row = [&](bool second) {
            modifier_row_label(second ? "Second" : "Point", column,
                               "Own Place is a point of its own, which you can put anywhere on the shape, drag and key.\n"
                               "Or pick a point the shape already has: the bone takes that point's move");
            const uint64_t rig_id = armature.id, bone_id = bone->id;
            const int index = i;
            follow_point_row(C, followed ? layer_find(doc, followed->deform_layer) : nullptr, followed ? followed->deform_shape : 0,
                             second ? c.follow_point2_id : c.follow_point_id, ImGui::GetContentRegionAvail().x - 4.0f,
                             "FOLLOWBONE:" + std::to_string(rig_id) + ":" + std::to_string(bone_id) + ":" + std::to_string(i) +
                                 (second ? ":2" : ":1"),
                             [rig_id, bone_id, index, second](Context &C, uint64_t point) {
                               Document &doc = *C.document;
                               Layer *rig = layer_find(doc, rig_id);
                               Bone *b = rig ? bone_find(*rig, bone_id) : nullptr;
                               if (b && index < int(b->constraints.size()) &&
                                   b->constraints[size_t(index)].type == BoneConstraintType::FollowPoint)
                               {
                                 follow_point_pick(doc, *rig, b->constraints[size_t(index)], second, point);
                                 C.undo->push("Follow Point", doc);
                               }
                               return true;
                             });
          };
          point_row(false);
          modifier_row_label("", column);
          if (ImGui::Checkbox("Show Follow Point", &c.follow_show)) {
            C.undo->push("Follow Point", doc);
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Shows the point in the viewport, light blue. Drag it on frame 0 to choose the place this\n"
                              "bone follows. Hiding it changes nothing");
          }
          modifier_row_label("", column);
          {
            bool two = c.follow_two;
            if (ImGui::Checkbox("Second Point", &two)) {
              follow_point_second(doc, armature, *bone, c, two);
              C.undo->push("Follow Point", doc);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
              ImGui::SetTooltip("A second place on the shape: the way from the first to it can turn the bone, and how far\n"
                                "apart they are can scale it. Further apart gives a steadier angle");
            }
          }
          if (c.follow_two) {
            point_row(true);
          }
          ImGui::BeginDisabled(doc.frame_current == 0);
          for (const bool second : {false, true}) {
            /* A point of the shape goes as it does: there's nothing to shift. */
            if ((second && !c.follow_two) || (second ? c.follow_point2_id : c.follow_point_id) != 0) {
              continue;
            }
            modifier_row_label(second ? "Second Shift" : "Shift", column,
                               "How far the point is moved from where it rests, on this frame: key it for a follower that\n"
                               "slides along the shape. Dragging the light blue point does the same");
            const std::string shift_path = constraint_path + (second ? "follow_shift2" : "follow_shift");
            const auto k = keyed_begin(shift_path);
            float2 &shift = second ? c.follow_shift2 : c.follow_shift;
            float values[2] = {shift.x, shift.y};
            if (ui_drag_float2(second ? "##follow_shift2" : "##follow_shift", values, 0.001f, -100.0f, 100.0f, "%.3f")) {
              shift = {values[0], values[1]};
            }
            done("Follow Point");
            keyed_end(shift_path, k);
          }
          ImGui::EndDisabled();
          if (c.follow_two) {
            modifier_row_label("", column);
            if (ImGui::Checkbox("Rotation", &c.follow_rotation)) {
              C.undo->push("Follow Point", doc);
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Scale", &c.follow_scale)) {
              C.undo->push("Follow Point", doc);
            }
          }
          modifier_row_label("", column);
          ImGui::PushTextWrapPos(0.0f);
          if (!c.follow_error.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", c.follow_error.c_str());
          }
          else {
            ImGui::TextDisabled("Action bone actions, drivers, Dynamics and Delay don't see this move.");
          }
          ImGui::PopTextWrapPos();
          break;
        }
        case BoneConstraintType::ChildOf: {
          target_row();
          modifier_row_label("Follow", column, "Which of the target's change it takes: its move, its turn, its size.\n"
                                               "Key Influence to pick the target up or drop it (a hat lifted off, a look\n"
                                               "target leaving the head).");
          bool changed = ImGui::Checkbox("Position", &c.child_position);
          ImGui::SameLine();
          changed |= ImGui::Checkbox("Rotation", &c.child_rotation);
          ImGui::SameLine();
          changed |= ImGui::Checkbox("Scale", &c.child_scale);
          if (changed) {
            C.undo->push("Child Of", doc);
          }
          break;
        }
        case BoneConstraintType::LookAt: {
          target_row();
          if (const Bone *target = bone_find(armature, c.target)) {
            modifier_row_label("", column);
            ImGui::TextDisabled("Depth: %.3f (the target's, in its Bone panel)", target->depth);
          }
          /* The eye: the shape the pupil stays inside, which also says how big the eye is. */
          ImGui::Spacing();
          modifier_row_label("", column);
          ImGui::TextDisabled("Eye Shape");
          shape_rows();
          if (c.enabled && c.shape_layer == 0) {
            modifier_row_label("", column);
            ImGui::TextDisabled("Pick the eye's shape: Look At needs it");
          }
          break;
        }
        case BoneConstraintType::MaintainVolume: {
          modifier_row_label("Volume", column,
                             "How much the width follows the length, however the bone got longer or\n"
                             "shorter (keys, IK, Stretch To, Copy Scale, dynamics): at 100% it keeps its\n"
                             "area, in place of its own X scale; 0% leaves the width alone.");
          float percent = c.amount * 100.0f;
          const std::string driven_amount_path = constraint_path + "amount";
          const int driven_amount = ui_driver_field_begin(C, armature.id, driven_amount_path, -1);
          if (ui_drag_float("##amount", &percent, 0.5f, 0.0f, 400.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            c.amount = percent / 100.0f;
          }
          done("Maintain Volume");
          ui_driver_typed(C, armature.id, driven_amount_path, -1);
          ui_driver_field_end(C, armature.id, driven_amount_path, -1, driven_amount);
          break;
        }
        case BoneConstraintType::CopyAngle:
        case BoneConstraintType::CopyPosition:
        case BoneConstraintType::CopyScale: {
          target_row();
          const bool go_to = c.type == BoneConstraintType::CopyPosition && !c.follow_move;
          if (c.type == BoneConstraintType::CopyPosition) {
            modifier_row_label("Copy", column, "To Target: the bone goes to where the target's base is.\n"
                                               "Its Move: it copies the target's move from its setup pose (Moho's control bones).");
            const float w2 = (ImGui::GetContentRegionAvail().x - 4.0f - 1.0f) * 0.5f;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, style.ItemSpacing.y));
            static const char *const MODES[] = {"To Target", "Its Move"};
            for (int k = 0; k < 2; k++) {
              if (k > 0) {
                ImGui::SameLine();
              }
              const bool on = int(c.follow_move) == k;
              if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
              }
              if (ImGui::Button(MODES[k], ImVec2(w2, 0)) && !on) {
                c.follow_move = k == 1;
                C.undo->push("Copy Position", doc);
              }
              if (on) {
                ImGui::PopStyleColor();
              }
            }
            ImGui::PopStyleVar();
          }
          modifier_row_label("Factor", column,
                             go_to ? "How far it goes to the target: 1 all the way, 0.5 halfway"
                                   : "How much of the target's motion to copy: 0.5 is half, -1 copies it reversed");
          const std::string driven_factor_path = constraint_path + "factor";
          const int driven_factor = ui_driver_field_begin(C, armature.id, driven_factor_path, -1);
          ui_drag_float("##factor", &c.factor, 0.01f, -10.0f, 10.0f, "%.2f");
          done("Copy Factor");
          ui_driver_typed(C, armature.id, driven_factor_path, -1);
          ui_driver_field_end(C, armature.id, driven_factor_path, -1, driven_factor);
          modifier_row_label("Delay", column, "How many frames behind the target it copies, for follow-through");
          const std::string driven_delay_path = constraint_path + "delay";
          const int driven_delay = ui_driver_field_begin(C, armature.id, driven_delay_path, -1);
          ui_drag_int("##delay", &c.delay, 0.2f, 0, 1000, "%d fr");
          done("Copy Delay");
          ui_driver_typed(C, armature.id, driven_delay_path, -1);
          ui_driver_field_end(C, armature.id, driven_delay_path, -1, driven_delay);
          break;
        }
        case BoneConstraintType::TrackTo:
        case BoneConstraintType::StretchTo:
          target_row();
          break;
        case BoneConstraintType::IK: {
          target_row();
          modifier_row_label("Chain Length", column,
                             "How many bones up from this one the IK may turn.\n"
                             "0 goes up to a bone Ignored by IK, or the root");
          const std::string driven_chain_path = constraint_path + "chain";
          const int driven_chain = ui_driver_field_begin(C, armature.id, driven_chain_path, -1);
          ui_drag_int("##chain", &c.chain, 0.1f, 0, 64, c.chain == 0 ? "To Ignored / Root" : "%d");
          done("IK Chain");
          ui_driver_typed(C, armature.id, driven_chain_path, -1);
          ui_driver_field_end(C, armature.id, driven_chain_path, -1, driven_chain);
          modifier_row_label("Max Stretch", column,
                             "How far the chain may stretch to reach a target out of reach: 1 never stretches, 2 up to twice as long");
          const std::string driven_stretch_path = constraint_path + "max_stretch";
          const int driven_stretch = ui_driver_field_begin(C, armature.id, driven_stretch_path, -1);
          ui_drag_float("##stretch", &c.max_stretch, 0.01f, 1.0f, 10.0f, "%.2f");
          done("IK Stretch");
          ui_driver_typed(C, armature.id, driven_stretch_path, -1);
          ui_driver_field_end(C, armature.id, driven_stretch_path, -1, driven_stretch);
          break;
        }
        case BoneConstraintType::Dynamics: {
          /* Between its sections (each channel, the bone's Weight and Wind Catch, its own gravity,
           * its own wind, baking): a gap, a faint line across the box, and a gap. */
          const auto divider = [] {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddLine(a, ImVec2(a.x + w, a.y), ImGui::GetColorU32(ImGuiCol_Separator, 0.6f));
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
          };
          spring_rows("Angle", c.angle_spring);
          divider();
          spring_rows("Position", c.position_spring);
          divider();
          spring_rows("Squash & Stretch", c.scale_spring);
          divider();
          {
            /* For the whole bone, each keyable, with its key button. */
            const auto setting = [&](const char *name, const char *id, float *value, float shown, float speed,
                                     float lo, float hi, const char *fmt, const char *tip) {
              modifier_row_label(name, column, tip);
              const std::string p = constraint_path + id;
              const auto k = keyed_begin(p);
              float v = *value * shown;
              if (ui_drag_float((std::string("##") + id).c_str(), &v, speed, lo, hi, fmt,
                                   ImGuiSliderFlags_AlwaysClamp))
              {
                *value = v / shown;
              }
              done(name);
              keyed_end(p, k);
            };
            /* An Own checkbox: the first time, its settings start as the document's. */
            const auto own_box = [&](const char *label, bool own, bool gravity, const char *tip) {
              modifier_row_label("", column);
              if (ImGui::Checkbox(label, &own)) {
                if (gravity) {
                  dynamics_use_own_gravity(doc, c, own);
                }
                else {
                  dynamics_use_own_wind(doc, c, own);
                }
                C.undo->push(label, doc);
              }
              if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s", tip);
              }
            };
            setting("Weight", "weight", &c.weight, 1.0f, 0.01f, 0.0f, 10.0f, "%.2f",
                    "How heavy the bone is: how hard gravity pulls it, down for Angle and Position,\n"
                    "along the bone for Squash & Stretch. 0 floats.");
            setting("Wind Catch", "wind_catch", &c.wind_catch, 100.0f, 0.5f, 0.0f, 1000.0f, "%.0f%%",
                    "How much the wind catches this bone: 100% all of it, 0% none. A ribbon catches\n"
                    "more than a braid. It doesn't follow Weight: the wind pushes on a surface, not a mass.");
            divider();
            own_box("Own Gravity", c.own_gravity, true,
                    "Give this bone its own gravity in place of the document's (the Document tab's\n"
                    "Simulation panel). Unticked, it keeps its own for when it's ticked again.");
            if (c.own_gravity) {
              setting("Gravity", "gravity", &c.gravity, 1.0f, 0.05f, -100.0f, 100.0f, "%.2f",
                      "How hard gravity pulls down here (by the bone's Weight)");
            }
            divider();
            own_box("Own Wind", c.own_wind, false,
                    "Give this bone its own wind in place of the document's (the Document tab's\n"
                    "Simulation panel), e.g. a ribbon caught by a breeze. Unticked, it keeps its own\n"
                    "for when it's ticked again.");
            if (c.own_wind) {
              setting("Wind", "wind", &c.wind, 1.0f, 0.05f, -100.0f, 100.0f, "%.2f",
                      "How hard the wind blows here (by the bone's Wind Catch)");
              setting("Wind Direction", "wind_direction", &c.wind_direction, 180.0f / PI, 0.5f, -360.0f, 360.0f,
                      "%.0f\xC2\xB0", "The way the wind blows (0\xC2\xB0: to the right)");
              setting("Gusts", "wind_gusts", &c.wind_gusts, 1.0f, 0.01f, 0.0f, 2.0f, "%.2f",
                      "How much gusts make the wind stronger and weaker (and waver its direction):\n"
                      "0 is steady, 0.5 between half and one and a half times as strong");
              setting("Gust Speed", "wind_gust_speed", &c.wind_gust_speed, 1.0f, 0.01f, 0.05f, 10.0f, "%.2f",
                      "How often gusts come: about every two seconds at 1; more often, they are shorter");
              setting("Turbulence", "wind_turbulence", &c.wind_turbulence, 1.0f, 0.01f, 0.0f, 2.0f, "%.2f",
                      "A small, fast flutter on top of the wind, along it and across it");
            }
          }
          divider();
          modifier_row_label("", column);
          if (ImGui::Button("Bake to Keys")) {
            operator_call(C, "armature.dynamics_bake");
          }
          break;
        }
      }
      ImGui::Spacing();
      /* Influence, animatable, with its keyframe button (as Blender's constraint boxes end). */
      const std::string path = "bones[\"" + bone_name + "\"].constraints[" + std::to_string(i) + "].influence";
      const int state = layer_key_state(armature, {path}, doc.frame_current);
      modifier_row_label("Influence", column, "How much of the constraint's effect is used: 0 none, 1 all. It can be keyed");
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
      const int tint = ui_key_tint_push(state | layer_action_bone_state(armature, {path}));
      const int driven = ui_driver_field_begin(C, armature.id, path, -1);
      ui_slider_float("##influence", &c.influence, 0.0f, 1.0f, "%.3f");
      ui_driver_typed(C, armature.id, path, -1);
      ui_driver_field_end(C, armature.id, path, -1, driven);
      ImGui::PopStyleColor(tint);
      done("Influence");
      if (ui_key_button("##key", state)) {
        operator_call(C, "anim.keyframe_toggle", {{"layer", std::to_string(armature.id)}, {"paths", path}});
      }
      ui_flash_row(armature.id, path);
      const int pin = pin_state(armature, {path}, state);
      if (ui_pin_button("##pin", pin)) {
        pin_toggle(C, armature.id, path, pin);
      }
      if (pin == 1) {
        ui_pinned_row();
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
    /* Dropping a constraint on this box puts it here (above or below by the half). */
    if (ImGui::BeginDragDropTarget()) {
      const bool upper = dynamics || ImGui::GetIO().MousePos.y < (box_min.y + box_max.y) * 0.5f;
      const float line_y = upper ? box_min.y - 3.0f : box_max.y + 3.0f;
      ImGui::GetWindowDrawList()->AddLine(ImVec2(box_min.x, line_y), ImVec2(box_max.x, line_y), UI_DROP_LINE_COLOR,
                                          2.0f);
      if (const ImGuiPayload *payload =
              ImGui::AcceptDragDropPayload(CONSTRAINT_PAYLOAD, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        drag_from = *static_cast<const int *>(payload->Data);
        drag_to = upper ? i : i + 1;
      }
      ImGui::EndDragDropTarget();
    }
    ImGui::Spacing();
    ImGui::PopID();
  }
  if (remove >= 0) {
    operator_call(C, "armature.constraint_remove", {{"index", std::to_string(remove)}, {"bone", bone_name}});
  }
  if (drag_from >= 0) {
    /* Index the constraint ends up at, after taking it out of its place. */
    const int to = drag_to > drag_from ? drag_to - 1 : drag_to;
    if (to != drag_from) {
      operator_call(C, "armature.constraint_move_to",
                    {{"index", std::to_string(drag_from)}, {"to", std::to_string(to)}, {"bone", bone_name}});
    }
  }
}

/* Draw Order: Normal, In Front Of or Behind a layer (picked from a list or with the eyedropper),
 * with its key button; keyed on frames other than 0. */
static void draw_order_row(Context &C, Layer &layer)
{
  Document &doc = *C.document;
  const DrawOrderRule rule = draw_order_rule(layer.draw_order);
  const Layer *target = rule.mode == DrawOrderMode::Normal ? nullptr : layer_find(doc, rule.target);
  key_next(C, layer, {"draw_order"});
  ui_row("Draw Order", [&](float width) {
    const float h = ImGui::GetFrameHeight();
    const float spacing = 2.0f;
    const float mode_w = std::max(ImGui::CalcTextSize("In Front Of").x + h * 1.3f, (width - h - spacing * 2.0f) * 0.5f);
    const float target_w = std::max(h, width - mode_w - h - spacing * 2.0f);
    const char *modes[] = {"Normal", "In Front Of", "Behind"};
    const char *mode_ids[] = {"NORMAL", "FRONT", "BEHIND"};
    const int current = int(rule.mode);
    ImGui::SetNextItemWidth(mode_w);
    if (ImGui::BeginCombo("##draw_order_mode", modes[current])) {
      for (int i = 0; i < 3; i++) {
        if (ImGui::Selectable(modes[i], i == current) && i != current) {
          if (i == 0 || target) {
            OpArgs args{{"layer", std::to_string(layer.id)}, {"mode", mode_ids[i]}};
            if (target) {
              args["target"] = std::to_string(target->id);
            }
            operator_call(C, "layer.draw_order_set", args);
          }
          else {
            /* No layer to go in front of or behind yet: pick one. */
            operator_call(C, "layer.draw_order_pick", {{"layer", std::to_string(layer.id)}, {"mode", mode_ids[i]}});
          }
        }
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Where it's drawn: where the Layers editor has it (Normal), or in front of or behind another "
                        "layer anywhere in the document. Masks keep the Layers editor's order. Ctrl ] and Ctrl [ "
                        "step it in the viewport");
    }
    ImGui::SameLine(0.0f, spacing);
    const std::string name = rule.mode == DrawOrderMode::Normal ? std::string("-")
                              : target                          ? target->name
                                                                : std::string("(missing)");
    ImGui::SetNextItemWidth(target_w);
    if (ImGui::BeginCombo("##draw_order_target", name.c_str())) {
      const std::string mode = rule.mode == DrawOrderMode::Behind ? "BEHIND" : "FRONT";
      for (const Layer *l : document_layers_flat(doc)) {
        if (l->id == layer.id || layer_inside(doc, l->id, layer.id) || layer_inside(doc, layer.id, l->id)) {
          continue;
        }
        if (ImGui::Selectable(l->name.c_str(), target && target->id == l->id)) {
          operator_call(C, "layer.draw_order_set",
                        {{"layer", std::to_string(layer.id)}, {"mode", mode}, {"target", std::to_string(l->id)}});
        }
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The layer it's drawn in front of or behind");
    }
    ImGui::SameLine(0.0f, spacing);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##draw_order_pick", ImVec2(h, h))) {
      operator_call(C, "layer.draw_order_pick",
                    {{"layer", std::to_string(layer.id)}, {"mode", rule.mode == DrawOrderMode::Behind ? "BEHIND" : "FRONT"}});
    }
    ui_eyedropper_icon(ImGui::GetWindowDrawList(), at + ImVec2(h * 0.5f, h * 0.5f), h * 0.28f, IM_COL32(0xe6, 0xe6, 0xe6, 255));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Click the layer to go in front of or behind, in the viewport or the Layers editor (it shows as "
                        "you point)");
    }
  });
  if (target) {
    if (const Layer *faded = draw_order_faded_group(doc, layer.id); faded && !layer_inside(doc, target->id, faded->id)) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.4f, 1.0f), "Moves \"%s\" as a whole: it's faded,\nso it's drawn as one image",
                         faded->name.c_str());
    }
  }
}

/* Mask: each card builds a shape from nothing, step by step, and shows or hides the layer by it (however it
 * is saved: see MaskVerb), and Clip to Below; then which layers use this one as a mask. */
static constexpr const char *MASK_PAYLOAD = "OANIM_MASK";

/* What a step does to the card's shape, in MaskVerb's order. */
static const char *const MASK_VERB_TIPS[] = {"+ Add: paint this area white, so it shows",
                                             "- Subtract: paint this area black, so it's hidden",
                                             "Keep Only: hide everything outside this area"};

static const char *const MASK_COMBINE_TIPS[] = {
    "Union: the layers' shapes together make the shape",
    "Difference: the first layer's shape, with the other layers' shapes cut out of it",
    "Intersection: only where all the layers' shapes overlap"};

/* Two overlapping rings, the part a Union (0), Difference (1) or Intersection (2) keeps filled. */
static void mask_combine_icon(ImDrawList *dl, ImVec2 c, float s, int combine, ImU32 col)
{
  const float r = s * 0.62f, d = s * 0.38f;
  const ImVec2 a(c.x - d, c.y), b(c.x + d, c.y);
  const ImU32 fill = IM_COL32(0xd8, 0xd8, 0xd8, 255);
  if (combine == 0) {
    dl->AddCircleFilled(a, r, fill);
    dl->AddCircleFilled(b, r, fill);
  }
  else if (combine == 1) {
    /* The left ring with the right one bitten out of it. */
    const float t = std::acos(d / r);
    dl->PathArcTo(a, r, t, 2.0f * IM_PI - t, 24);
    dl->PathArcTo(b, r, IM_PI + t, IM_PI - t, 12);
    dl->PathFillConcave(fill);
  }
  else {
    const float t = std::acos(d / r);
    dl->PathArcTo(a, r, -t, t, 12);
    dl->PathArcTo(b, r, IM_PI - t, IM_PI + t, 12);
    dl->PathFillConvex(fill);
  }
  dl->AddCircle(a, r, col, 0, 1.2f);
  dl->AddCircle(b, r, col, 0, 1.2f);
}

/* What a masking area paints, on its button: white with a plus, black with a minus, or grey with the overlap of two
 * rings for Keep Only. */
static void mask_verb_icon(ImDrawList *dl, ImVec2 c, float s, int verb, ImU32 /*col*/)
{
  const bool white = verb == int(MaskVerb::Add), black = verb == int(MaskVerb::Remove);
  const ImU32 back = white ? IM_COL32(0xf2, 0xf2, 0xf2, 255) : black ? IM_COL32(0x10, 0x10, 0x10, 255) : IM_COL32(0x5a, 0x5a, 0x5a, 255);
  const ImU32 mark = white ? IM_COL32(0x10, 0x10, 0x10, 255) : IM_COL32(0xf2, 0xf2, 0xf2, 255);
  dl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), back, 2.0f);
  dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(0x90, 0x90, 0x90, 255), 2.0f);
  if (verb == int(MaskVerb::KeepOnly)) {
    mask_combine_icon(dl, c, s * 0.62f, int(MaskCombine::Intersection), mark);
    return;
  }
  const float r = s * 0.6f;
  dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), mark, 1.6f);
  if (white) {
    dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), mark, 1.6f);
  }
}

/* The layer a step takes only an outline from without meaning to (none: nullptr): closed shapes, none filled, so
 * As Drawn masks a thin ring along the line. An extended outline is used for its edge, on purpose. */
static const Layer *mask_step_outline_layer(const Document &doc, const MaskStep &step)
{
  if (step.filled || step.strokes_only || step.extend != MaskExtend::None) {
    return nullptr;
  }
  for (const uint64_t id : step.layers) {
    const Layer *l = layer_find(doc, id);
    if (!l || l->type != LayerType::Vector) {
      continue;
    }
    const VectorDrawing &drawing = layer_drawing(doc, *l);
    bool closed_outline = false, filled = false;
    for (const Shape &s : drawing.shapes) {
      std::vector<OrientedEdge> chain;
      bool closed = false;
      shape_chain(drawing, s, chain, &closed);
      closed_outline = closed_outline || (closed && !s.hidden);
      filled = filled || (s.has_fill && !s.hidden);
    }
    if (closed_outline && !filled) {
      return l;
    }
  }
  return nullptr;
}

static constexpr ImU32 COL_MOD_SUBPANEL = IM_COL32(0x33, 0x33, 0x33, 255);

/* A subpanel in a mask row's box, as Blender's modifier panels have: a bar with its fold arrow and its
 * name (the whole bar opens it). It starts folded, the bar showing `note` (what's set inside) after
 * the name. Returns whether it's open (kept per row while Open Animate runs). */
static bool mask_subpanel(const char *id, const char *label, const std::string &note, float width)
{
  ImGui::PushID(id);
  ImGuiStorage *storage = ImGui::GetStateStorage();
  const ImGuiID key = ImGui::GetID("##open");
  bool open = storage->GetBool(key, false);
  const float h = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (ImGui::InvisibleButton("##fold", ImVec2(width, h))) {
    open = !open;
    storage->SetBool(key, open);
  }
  dl->AddRectFilled(at, ImVec2(at.x + width, at.y + h),
                    ImGui::IsItemHovered() ? IM_COL32(0x3a, 0x3a, 0x3a, 255) : COL_MOD_SUBPANEL, 3.0f);
  ImGui::RenderArrow(dl, ImVec2(at.x + h * 0.25f, at.y + h * 0.22f), COL_MOD_ICON,
                     open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
  const float pad = ImGui::GetStyle().FramePadding.y;
  dl->AddText(ImVec2(at.x + h, at.y + pad), ImGui::GetColorU32(ImGuiCol_Text), label);
  if (!open && !note.empty()) {
    const float x = at.x + h + ImGui::CalcTextSize(label).x + 8.0f;
    ImGui::PushClipRect(ImVec2(x, at.y), ImVec2(at.x + width - 4.0f, at.y + h), true);
    dl->AddText(ImVec2(x, at.y + pad), ImGui::GetColorU32(ImGuiCol_TextDisabled), note.c_str());
    ImGui::PopClipRect();
  }
  ImGui::PopID();
  return open;
}

/* The paint a mode lays on the mask, as a swatch: white (Add, and Start With Everything) or black
 * (Subtract, Nothing), outlined in `col`. */
static void mask_paint_icon(ImDrawList *dl, ImVec2 c, float s, bool white, ImU32 col)
{
  const ImVec2 lo(c.x - s, c.y - s), hi(c.x + s, c.y + s);
  dl->AddRectFilled(lo, hi, white ? IM_COL32(0xf2, 0xf2, 0xf2, 255) : IM_COL32(0x10, 0x10, 0x10, 255), s * 0.25f);
  dl->AddRect(lo, hi, col, s * 0.25f, 0, 1.2f);
}

/* Which part of a shape: 0 fill and line, 1 the fill, 2 the line (Cuts and Use draw these). */
static void shape_part_icon(ImDrawList *dl, ImVec2 c, float s, int part, ImU32 col)
{
  if (part != 2) {
    dl->AddCircleFilled(c, part == 0 ? s * 0.62f : s * 0.85f, part == 0 ? (col & 0x00ffffff) | 0x99000000 : col, 20);
  }
  if (part != 1) {
    dl->AddCircle(c, s * 0.8f, col, 20, part == 0 ? 2.0f : 1.6f);
  }
}

/* Extend's directions: 0 none, then up, down, left and right. */
static void extend_icon(ImDrawList *dl, ImVec2 c, float s, int dir, ImU32 col)
{
  if (dir == 0) {
    dl->AddCircle(c, s * 0.55f, col, 16, 1.3f);
    dl->AddLine(ImVec2(c.x - s * 0.4f, c.y + s * 0.4f), ImVec2(c.x + s * 0.4f, c.y - s * 0.4f), col, 1.3f);
    return;
  }
  static const ImVec2 DIRS[] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
  const ImVec2 v = DIRS[dir - 1], n(-v.y, v.x);
  const ImVec2 tip(c.x + v.x * s, c.y + v.y * s), tail(c.x - v.x * s, c.y - v.y * s);
  dl->AddLine(tail, tip, col, 1.5f);
  dl->AddTriangleFilled(tip, ImVec2(tip.x - v.x * s * 0.6f + n.x * s * 0.5f, tip.y - v.y * s * 0.6f + n.y * s * 0.5f),
                        ImVec2(tip.x - v.x * s * 0.6f - n.x * s * 0.5f, tip.y - v.y * s * 0.6f - n.y * s * 0.5f), col);
}

/* Blender's expanded enum: joined buttons, the chosen one blue. The others take the frame colour, so a
 * driven setting's tint shows. A segment too narrow for its name shows just its icon (the tooltip names
 * it). Returns the segment clicked (not the chosen one), or -1. */
static int segmented(const char *id, int value, int count, const char *const *names, const char *const *tips,
                     const SegmentIcon &icon)
{
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const float total = ImGui::CalcItemWidth();
  const float w = std::floor((total - float(count - 1)) / float(count));
  int clicked = -1;
  ImGui::PushID(id);
  ImGui::BeginGroup();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  for (int k = 0; k < count; k++) {
    if (k > 0) {
      ImGui::SameLine(0, 1);
    }
    const float seg_w = k == count - 1 ? total - (w + 1.0f) * float(count - 1) : w;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::PushID(k);
    if (ImGui::InvisibleButton("##segment", ImVec2(seg_w, h)) && k != value) {
      clicked = k;
    }
    const bool on = k == value, hovered = ImGui::IsItemHovered();
    const ImU32 back = ImGui::GetColorU32(on        ? ImGuiCol_ButtonActive
                                          : hovered ? ImGuiCol_FrameBgHovered
                                                    : ImGuiCol_FrameBg);
    const ImDrawFlags corners = count == 1       ? ImDrawFlags_RoundCornersAll
                                : k == 0         ? ImDrawFlags_RoundCornersLeft
                                : k == count - 1 ? ImDrawFlags_RoundCornersRight
                                                 : ImDrawFlags_RoundCornersNone;
    dl->AddRectFilled(at, ImVec2(at.x + seg_w, at.y + h), back, style.FrameRounding, corners);
    const ImU32 col = ImGui::GetColorU32(on ? ImVec4(1, 1, 1, 1) : style.Colors[ImGuiCol_Text]);
    const char *name = names ? names[k] : nullptr;
    const float text_w = name && *name ? ImGui::CalcTextSize(name).x : 0.0f;
    const float icon_w = icon ? h * 0.75f : 0.0f;
    const bool show_text = text_w > 0.0f && text_w + icon_w + 8.0f <= seg_w;
    const float content = (show_text ? text_w : 0.0f) + icon_w + (show_text && icon ? 2.0f : 0.0f);
    float x = at.x + std::floor((seg_w - content) * 0.5f);
    if (icon) {
      icon(dl, ImVec2(x + icon_w * 0.5f, at.y + h * 0.5f), h * 0.32f, k, col);
      x += icon_w + 2.0f;
    }
    if (show_text) {
      dl->AddText(ImVec2(x, at.y + style.FramePadding.y), col, name);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (tips && tips[k]) {
        ImGui::SetTooltip("%s", tips[k]);
      }
      else if (name) {
        ImGui::SetTooltip("%s", name);
      }
    }
    ImGui::PopID();
  }
  ImGui::EndGroup();
  ImGui::PopID();
  return clicked;
}

/* Current Masking Area: the settings of the picked one, so the stack stays a short list. */
static void mask_area_panel(Context &C, Layer &layer, const WindowManager::MaskAreaPick &pick)
{
  Document &doc = *C.document;
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const float column = ImGui::GetContentRegionAvail().x * 0.3f;
  const LayerMask &card = layer.masks[size_t(pick.card)];
  ImGui::Separator();
  ImGui::Spacing();
  if (pick.step < 0) {
    ImGui::Text("Current Masking Area:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.24f, 1.0f), "%s", card.start_shown ? "Show All" : "Hide All");
    ImGui::TextDisabled("The whole canvas, so there's nothing to set here.");
    return;
  }
  const MaskStep &top = card.steps[size_t(pick.step)];
  const MaskStep st = pick.child >= 0 ? top.children[size_t(pick.child)] : top;
  if (st.whole) {
    ImGui::Text("Current Masking Area:");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.24f, 1.0f), "%s", st.mode == MaskMode::Add ? "Show All" : "Hide All");
    ImGui::TextDisabled("The whole canvas, so there's nothing to set here.");
    return;
  }
  const std::string id = std::to_string(layer.id);
  OpArgs at{{"layer", id}, {"index", std::to_string(pick.card)}, {"step", std::to_string(pick.step)}};
  if (pick.child >= 0) {
    at["child"] = std::to_string(pick.child);
  }
  const auto with_step = [&](const char *key, const std::string &value) {
    OpArgs args = at;
    args[key] = value;
    return args;
  };
  const std::string step_path = "masks[" + std::to_string(pick.card) + "].steps[" + std::to_string(pick.step) + "].";
  /* Drivers name a card's own steps; what's inside a group has none. */
  const auto driven_begin = [&](const std::string &path) {
    return pick.child >= 0 ? 0 : ui_driver_field_begin(C, layer.id, path, -1);
  };
  const auto driven_end = [&](const std::string &path, int driven) {
    if (pick.child < 0) {
      ui_driver_field_end(C, layer.id, path, -1, driven);
    }
  };
  std::string name = st.name;
  if (!st.group && name.empty()) {
    for (const uint64_t l : st.layers) {
      const Layer *source = layer_find(doc, l);
      name += (name.empty() ? "" : ", ") + (source ? source->name : std::string("(missing)"));
    }
  }
  ImGui::Text("Current Masking Area:");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.24f, 1.0f), "%s", name.c_str());
  /* Held: the mask as far as this area. */
  if (!st.group || !st.children.empty()) {
    const float view_w = ImGui::CalcTextSize("View").x + style.FramePadding.x * 2.0f + h * 0.8f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - view_w));
    const ImVec2 view_at = ImGui::GetCursorScreenPos();
    ImGui::Button("##area_view", ImVec2(view_w, 0));
    if (ImGui::IsItemActive()) {
      const int order = (pick.step << 8) | (st.group ? 0xff : pick.child + 1);
      C.wm->mask_preview = {layer.id, card.cuts == MaskCuts::Lines, pick.card, ImGui::GetFrameCount(), order};
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Hold to see this mask as far as this area: white shows, black is hidden");
    }
    ImDrawList *vdl = ImGui::GetWindowDrawList();
    const ImVec2 c(view_at.x + style.FramePadding.x + h * 0.3f, view_at.y + h * 0.5f);
    const float s = h * 0.26f;
    vdl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x, c.y + s), IM_COL32(0xf2, 0xf2, 0xf2, 255));
    vdl->AddRectFilled(ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(0x10, 0x10, 0x10, 255));
    vdl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(0x90, 0x90, 0x90, 255));
    vdl->AddText(ImVec2(c.x + s + 4.0f, view_at.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), "View");
  }
  ImGui::PushItemWidth(-4.0f);

  /* Global: as a material's row, the browse button, then its name and user count, or Make Global. */
  if (pick.child < 0) {
    modifier_row_label("Global Area", column, "Share this area between masks, as a material is shared between shapes: "
                                              "each keeps its own +, - or Keep Only");
    OpArgs global_at = at;
    if (ImGui::Button("v##global_browse", ImVec2(h, 0))) {
      ImGui::OpenPopup("##global_list");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Link this area to a global area");
    }
    if (ImGui::BeginPopup("##global_list")) {
      const std::vector<MaskAreaName> areas = mask_areas(doc);
      if (areas.empty()) {
        ImGui::TextDisabled("No global areas yet");
      }
      for (const MaskAreaName &a : areas) {
        if (ImGui::Selectable(a.name.c_str(), a.id == st.global) && a.id != st.global) {
          global_at["action"] = "LINK";
          global_at["global"] = std::to_string(a.id);
          operator_call(C, "mask.area_global", global_at);
        }
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 2);
    if (st.global != 0) {
      const int users = mask_area_users(doc, st.global);
      const std::string count = std::to_string(users);
      const float count_w = ImGui::CalcTextSize(count.c_str()).x + style.FramePadding.x * 2.0f + 4.0f;
      ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - count_w - 6.0f);
      std::string edited = st.name;
      ImGui::InputText("##global_name", &edited);
      if (ImGui::IsItemDeactivatedAfterEdit() && !edited.empty()) {
        operator_call(C, "mask.set", with_step("area_name", edited));
      }
      ImGui::SameLine(0, 2);
      if (ImGui::Button((count + "##global_count").c_str(), ImVec2(count_w, 0))) {
        global_at["action"] = "UNLINK";
        operator_call(C, "mask.area_global", global_at);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Used by %d mask%s. Click to make this one its own, looking the same", users,
                          users == 1 ? "" : "s");
      }
      if (users > 1) {
        modifier_row_label("", column);
        ImGui::TextColored(ImVec4(0.44f, 0.83f, 0.77f, 1.0f), "Changing this changes all %d masks using it", users);
      }
    }
    else if (ImGui::Button("Make Global", ImVec2(ImGui::GetContentRegionAvail().x - 4.0f, 0))) {
      global_at["action"] = "MAKE";
      operator_call(C, "mask.area_global", global_at);
    }
  }
  /* Inverse: the outside of the shape. On a - area that's Keep Only, which the row then is. */
  const auto inverse_row = [&] {
    modifier_row_label("", column);
    const std::string driven_invert_path = step_path + "invert";
    const int driven_invert = driven_begin(driven_invert_path);
    bool invert = st.invert;
    if (ImGui::Checkbox("Inverse (use the outside)", &invert)) {
      operator_call(C, "mask.set", with_step("invert", invert ? "1" : "0"));
    }
    driven_end(driven_invert_path, driven_invert);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Use what's outside the shape instead of what's inside. A - area then hides what's outside "
                        "it, which is Keep Only");
    }
  };
  if (st.group) {
    if (st.global == 0) {
      modifier_row_label("Name", column, "The group's name");
      std::string edited = st.name;
      ImGui::InputText("##group_name", &edited);
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        operator_call(C, "mask.set", with_step("area_name", edited));
      }
    }
    inverse_row();
    modifier_row_label("", column);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The areas in a group build a shape of their own, starting from nothing.");
    ImGui::PopTextWrapPos();
    ImGui::PopItemWidth();
    return;
  }

  /* Its layers: a chip each, + for the list, the eyedropper. */
  modifier_row_label("Layers", column, "The layers that make this area");
  ImGui::BeginGroup();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float chips_right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - (h + 2.0f) * 2.0f;
  for (size_t k = 0; k < st.layers.size(); k++) {
    const Layer *source = layer_find(doc, st.layers[k]);
    const std::string chip_name = source ? source->name : std::string("(missing)");
    const float chip_w = ImGui::CalcTextSize(chip_name.c_str()).x + style.FramePadding.x * 2.0f + h * 0.7f;
    if (k > 0 && ImGui::GetItemRectMax().x + 3.0f + chip_w <= chips_right) {
      ImGui::SameLine(0, 3);
    }
    ImGui::PushID(int(k));
    const ImVec2 chip_at = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##chip", ImVec2(chip_w, h));
    const bool chip_hovered = ImGui::IsItemHovered();
    const bool on_x = chip_hovered && ImGui::GetIO().MousePos.x > chip_at.x + chip_w - h * 0.75f;
    dl->AddRectFilled(chip_at, chip_at + ImVec2(chip_w, h),
                      ImGui::GetColorU32(chip_hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), h * 0.5f);
    dl->AddText(ImVec2(chip_at.x + style.FramePadding.x, chip_at.y + style.FramePadding.y),
                source ? ImGui::GetColorU32(ImGuiCol_Text) : IM_COL32(0xe0, 0x60, 0x60, 255), chip_name.c_str());
    const ImVec2 xc(chip_at.x + chip_w - h * 0.45f, chip_at.y + h * 0.5f);
    const float xs = h * 0.15f;
    const ImU32 x_col = on_x ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0x9a, 0x9a, 0x9a, 255);
    dl->AddLine(ImVec2(xc.x - xs, xc.y - xs), ImVec2(xc.x + xs, xc.y + xs), x_col, 1.3f);
    dl->AddLine(ImVec2(xc.x - xs, xc.y + xs), ImVec2(xc.x + xs, xc.y - xs), x_col, 1.3f);
    if (chip_hovered) {
      C.wm->layers_hover = {st.layers[k]};
      C.wm->layers_hover_frame = ImGui::GetFrameCount();
      C.wm->mask_hover = {st.layers[k]};
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip(on_x ? "Take %s out of the area" : "%s (x takes it out)", chip_name.c_str());
      }
    }
    if (ImGui::IsItemClicked() && on_x) {
      std::string sources;
      for (size_t n = 0; n < st.layers.size(); n++) {
        if (n != k) {
          sources += (sources.empty() ? "" : "|") + std::to_string(st.layers[n]);
        }
      }
      operator_call(C, "mask.set", with_step("sources", sources));
    }
    ImGui::PopID();
  }
  if (!st.layers.empty()) {
    ImGui::SameLine(0, 3);
  }
  const ImVec2 plus_at = ImGui::GetCursorScreenPos();
  if (ImGui::Button("##more", ImVec2(st.layers.empty() ? std::max(h, chips_right - plus_at.x) : h, h))) {
    ImGui::OpenPopup("##layers");
  }
  if (st.layers.empty()) {
    dl->AddText(ImVec2(plus_at.x + style.FramePadding.x, plus_at.y + style.FramePadding.y),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), "Pick a layer");
  }
  else {
    const ImVec2 pc(plus_at.x + h * 0.5f, plus_at.y + h * 0.5f);
    dl->AddLine(ImVec2(pc.x - h * 0.22f, pc.y), ImVec2(pc.x + h * 0.22f, pc.y), COL_MOD_ICON, 1.4f);
    dl->AddLine(ImVec2(pc.x, pc.y - h * 0.22f), ImVec2(pc.x, pc.y + h * 0.22f), COL_MOD_ICON, 1.4f);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Choose the layers that make this area");
  }
  if (pick.child < 0) {
    ImGui::SameLine(0, 2);
    const ImVec2 pick_at = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##pick", ImVec2(h, h))) {
      operator_call(C, "mask.pick", {{"layer", id}, {"index", std::to_string(pick.card)}, {"step", std::to_string(pick.step)}});
    }
    ui_eyedropper_icon(dl, pick_at + ImVec2(h * 0.5f, h * 0.5f), h * 0.28f, IM_COL32(0xe6, 0xe6, 0xe6, 255));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Click a layer to add it to this area");
    }
  }
  ImGui::EndGroup();
  if (ImGui::BeginPopup("##layers")) {
    ImGui::TextDisabled("The ticked layers together make the area");
    for (const Layer *l : document_layers_flat(doc)) {
      if (l->id == layer.id) {
        continue;
      }
      bool ticked = std::find(st.layers.begin(), st.layers.end(), l->id) != st.layers.end();
      const bool changed = ImGui::Checkbox(l->name.c_str(), &ticked);
      if (ImGui::IsItemHovered()) {
        C.wm->layers_hover = {l->id};
        C.wm->layers_hover_frame = ImGui::GetFrameCount();
        C.wm->mask_hover = {l->id};
      }
      if (changed) {
        std::vector<uint64_t> next = st.layers;
        std::erase(next, l->id);
        if (ticked) {
          next.push_back(l->id);
        }
        std::string sources;
        for (const uint64_t n : next) {
          sources += (sources.empty() ? "" : "|") + std::to_string(n);
        }
        operator_call(C, "mask.set", with_step("sources", sources));
      }
    }
    ImGui::EndPopup();
  }
  /* How several layers make one shape: Union, Difference, Intersection (Blender Boolean's). */
  if (st.layers.size() > 1) {
    modifier_row_label("Combine", column, "How these layers make one shape");
    const std::string driven_combine_path = step_path + "combine";
    const int driven_combine = driven_begin(driven_combine_path);
    const int picked_combine = segmented("##combine", int(st.combine), 3, nullptr, MASK_COMBINE_TIPS,
                                         [](ImDrawList *d, ImVec2 c, float sz, int k, ImU32) {
                                           mask_combine_icon(d, c, sz * 0.85f, k, IM_COL32(0x90, 0x90, 0x90, 255));
                                         });
    if (picked_combine >= 0 && picked_combine != int(st.combine)) {
      operator_call(C, "mask.set", with_step("combine", mask_combine_identifier(MaskCombine(picked_combine))));
    }
    driven_end(driven_combine_path, driven_combine);
  }
  inverse_row();
  /* An outline-only layer masks with just its line, which is seldom what's wanted. */
  if (const Layer *outline = mask_step_outline_layer(doc, st)) {
    modifier_row_label("", column);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.45f, 1.0f), "%s is only an outline, so just its line masks",
                       outline->name.c_str());
    ImGui::PopTextWrapPos();
    modifier_row_label("", column);
    if (ImGui::SmallButton("Fill It")) {
      operator_call(C, "mask.set", with_step("filled", "1"));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Turn on Fill Every Shape, so the inside of the outline masks too. If you do want only "
                        "the line, set Mask With to Strokes");
    }
  }
  /* Which parts of the layers' shapes count, and how far they reach. */
  static const char *const SHAPE_IDS[] = {"AS_DRAWN", "FILLS_ONLY", "STROKES_ONLY"};
  static const char *const SHAPE_NAMES[] = {"As Drawn", "Fills", "Strokes"};
  static const char *const SHAPE_TIPS[] = {"As Drawn: their fills and strokes, as they're drawn",
                                           "Fills Only: their fills, not their strokes",
                                           "Strokes Only: their strokes, not their fills"};
  const int shape = st.fills_only ? 1 : st.strokes_only ? 2 : 0;
  modifier_row_label("Mask With", column, "Which parts of the layers' shapes make this area");
  const std::string driven_shape_path = step_path + "shape";
  const int driven_shape = driven_begin(driven_shape_path);
  const int shape_clicked = segmented("##shape", shape, 3, SHAPE_NAMES, SHAPE_TIPS, shape_part_icon);
  if (shape_clicked >= 0) {
    operator_call(C, "mask.set", with_step("shape", SHAPE_IDS[shape_clicked]));
  }
  driven_end(driven_shape_path, driven_shape);
  modifier_row_label("", column);
  ImGui::BeginDisabled(st.strokes_only);
  const std::string driven_filled_path = step_path + "filled";
  const int driven_filled = driven_begin(driven_filled_path);
  bool filled = st.filled && !st.strokes_only;
  if (ImGui::Checkbox("Fill Every Shape", &filled)) {
    operator_call(C, "mask.set", with_step("filled", filled ? "1" : "0"));
  }
  driven_end(driven_filled_path, driven_filled);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(st.strokes_only ? "Strokes Only has no insides to fill"
                                     : "Mask with the inside of every shape, as if it were filled: an "
                                       "outline-only shape too (an open one closed by a straight line)");
  }
  modifier_row_label("", column);
  ImGui::BeginDisabled(!st.fills_only);
  const std::string driven_inside_path = step_path + "inside_stroke";
  const int driven_inside = driven_begin(driven_inside_path);
  bool inside_stroke = st.inside_stroke && st.fills_only;
  if (ImGui::Checkbox("Inside the Stroke", &inside_stroke)) {
    operator_call(C, "mask.set", with_step("inside_stroke", inside_stroke ? "1" : "0"));
  }
  driven_end(driven_inside_path, driven_inside);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(!st.fills_only ? "With Fills Only: the fills end at their lines' inner edge"
                                    : "The fills end at their lines' inner edge, not down the middle of the "
                                      "lines, at any zoom: a layer cut by this one shows over the whole line");
  }
  modifier_row_label("Grow", column, "Grow the shape by pixels (negative shrinks it); a pixel hides thin seams along the edge");
  float grow = st.grow;
  const std::string driven_grow_path = step_path + "grow";
  const int driven_grow = driven_begin(driven_grow_path);
  if (ui_drag_float("##grow", &grow, 0.1f, -64.0f, 64.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp)) {
    /* Shown as it changes; one undo step when let go. */
    MaskStep &live = pick.child >= 0 ? layer.masks[size_t(pick.card)].steps[size_t(pick.step)].children[size_t(pick.child)]
                                     : layer.masks[size_t(pick.card)].steps[size_t(pick.step)];
    live.grow = grow;
  }
  if (pick.child < 0) {
    ui_driver_typed(C, layer.id, driven_grow_path, -1);
  }
  driven_end(driven_grow_path, driven_grow);
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    char value[32];
    std::snprintf(value, sizeof(value), "%g", double(grow));
    operator_call(C, "mask.set", with_step("grow", value));
  }
  /* Extend: the shapes stretched to the edge one way, in the mask layer's own directions. */
  static const char *const EXTEND_IDS[] = {"NONE", "UP", "DOWN", "LEFT", "RIGHT"};
  static const char *const EXTEND_TIPS[] = {"None: the shape as it is", "Up: stretched to the top edge",
                                            "Down: stretched to the bottom edge", "Left: stretched to the left edge",
                                            "Right: stretched to the right edge"};
  modifier_row_label("Extend", column,
                     "Stretch the shape to the edge one way (the mask layer's own up, down, left or right, turning with "
                     "it): subtracting the mouth line, extended Up, hides the mouth and everything above it");
  const std::string driven_extend_path = step_path + "extend";
  const int driven_extend = driven_begin(driven_extend_path);
  /* Icons only: five names don't fit beside each other. */
  const int extend_clicked = segmented("##extend", int(st.extend), 5, nullptr, EXTEND_TIPS, extend_icon);
  if (extend_clicked >= 0) {
    operator_call(C, "mask.set", with_step("extend", EXTEND_IDS[extend_clicked]));
  }
  driven_end(driven_extend_path, driven_extend);
  ImGui::PopItemWidth();
}

/* The Masking tab's cards, as the Modifiers tab's boxes: each a mask of its own, and the layer shows
 * only where all of them show it. The result in words comes first, a card to a line (hovering one
 * points at its card). Each card's header has the open arrow, its name (folded: the card in words),
 * viewport and render switches, a menu, an x and a grip to reorder; inside, how it masks (Affects with View,
 * Line Ends), then its Masking Areas, a stack as the Layers editor's: the base at the bottom, the top painted
 * last, with + for the menu of what to add. The picked area's settings are below all the masks. Hovering a
 * card outlines its layers in the viewport. */
static void mask_cards(Context &C, Layer &layer)
{
  Document &doc = *C.document;
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const std::string id = std::to_string(layer.id);
  /* The picked Masking Area: what it was if that's still there, else the top of the first mask. */
  WindowManager::MaskAreaPick &area = C.wm->mask_area_pick;
  std::vector<int> &picked_rows = C.wm->mask_area_rows;
  C.wm->mask_tab_shown = true;
  {
    const bool card_ok = area.layer == layer.id && area.card >= 0 && area.card < int(layer.masks.size());
    const std::vector<MaskStep> *steps = card_ok ? &layer.masks[size_t(area.card)].steps : nullptr;
    const bool step_ok = steps && (area.step == -1 || (area.step >= 0 && area.step < int(steps->size())));
    const bool child_ok = step_ok && (area.child < 0 || (area.step >= 0 && area.child < int((*steps)[size_t(area.step)].children.size())));
    if (!child_ok) {
      area = {layer.id, 0, layer.masks.empty() ? -1 : int(layer.masks[0].steps.size()) - 1, -1};
      picked_rows.clear();
    }
    /* What's picked is what's still there, the active one among it. */
    if (!layer.masks.empty()) {
      const std::vector<MaskStep> &now = layer.masks[size_t(area.card)].steps;
      std::erase_if(picked_rows, [&](int row) {
        const int at = row >> 8, inside = (row & 0xff) - 1;
        return at >= int(now.size()) || inside >= int(now[size_t(at)].children.size());
      });
      const int active = (area.step << 8) | (area.child + 1);
      if (area.step >= 0 && std::find(picked_rows.begin(), picked_rows.end(), active) == picked_rows.end()) {
        picked_rows.push_back(active);
      }
    }
  }
  /* A plain press on a picked row keeps them all for a drag, and narrows to it if the button comes up without one. */
  static int narrow_row = -1;
  /* A row dragged onto another, moved once the stacks are drawn. */
  OpArgs area_drop;

  /* The result in words, a card to a line ("Shows:", "everything but head", "lines: ..."). */
  const std::vector<std::string> phrases = mask_row_phrases(doc, layer);
  int summary_hover = -1;
  /* The card a summary line was clicked for (its uid, or index + 1), scrolled to when drawn. */
  static uint64_t scroll_to = 0;
  const ImVec4 summary_color(1.0f, 0.75f, 0.45f, 1.0f);
  ImGui::PushTextWrapPos(0.0f);
  if (std::all_of(phrases.begin(), phrases.end(), [](const std::string &p) { return p.empty(); })) {
    ImGui::TextColored(summary_color, "Shows everything");
  }
  else {
    ImGui::TextColored(summary_color, "Shows:");
    ImGui::Indent(h * 0.7f);
    for (size_t k = 0; k < phrases.size(); k++) {
      if (phrases[k].empty()) {
        continue;
      }
      ImGui::TextColored(summary_color, "%s", phrases[k].c_str());
      /* Hovering a line points at its card (and its layers in the viewport); clicking opens the card. */
      if (k < layer.masks.size() && ImGui::IsItemHovered()) {
        summary_hover = int(k);
        C.wm->mask_hover = mask_card_layers(layer.masks[k]);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          layer.masks[k].show_expanded = true;
          scroll_to = layer.masks[k].uid ? layer.masks[k].uid : k + 1;
        }
      }
    }
    ImGui::Unindent(h * 0.7f);
  }
  ImGui::PopTextWrapPos();
  ImGui::Spacing();

  const float half = (ImGui::GetContentRegionAvail().x - 2.0f) * 0.5f;
  if (ImGui::Button("+  Add Mask", ImVec2(half, 0))) {
    ImGui::OpenPopup("##add_mask");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("A new mask card, then click its first layer in the viewport or the Layers editor\n"
                      "(or Alt-drag a layer onto this one in the Layers editor; Shift to hide it)");
  }
  if (ImGui::BeginPopup("##add_mask")) {
    ImGui::TextDisabled("Then click the layer");
    static const char *const ADD_LABELS[] = {"      Show Inside a Layer...", "      Hide Inside a Layer..."};
    static const char *const ADD_TIPS[] = {
        "Starts with nothing and shows the layer only inside the shape (+ Shape on black)",
        "Starts with everything and hides the layer inside the shape (- Shape on white)"};
    for (int k = 0; k < 2; k++) {
      if (ImGui::Selectable(ADD_LABELS[k])) {
        operator_call(C, "mask.pick", {{"layer", id}, {"mode", mask_mode_identifier(MaskMode(k))}});
      }
      const ImVec2 lo = ImGui::GetItemRectMin();
      mask_paint_icon(ImGui::GetWindowDrawList(), ImVec2(lo.x + h * 0.4f, lo.y + ImGui::GetItemRectSize().y * 0.5f),
                      h * 0.22f, k == 0, IM_COL32(0x90, 0x90, 0x90, 255));
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", ADD_TIPS[k]);
      }
    }
    ImGui::EndPopup();
  }
  /* Join Lines: two picked layers that meet cut each other's lines there. */
  ImGui::SameLine(0, 2);
  int picked = 0;
  for (const Layer *l : document_layers_flat(doc)) {
    picked += l->id == doc.active_layer || l->selected ? 1 : 0;
  }
  ImGui::BeginDisabled(picked != 2);
  if (ImGui::Button("Join Lines", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
    operator_call(C, "mask.join_lines");
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(picked != 2 ? "Pick two layers that meet (a head and an ear) to join their lines"
                                  : "Each of the two picked layers cuts the other's lines where they meet, so they "
                                    "read as one shape: their lines end at the join and the lower one's fill "
                                    "covers the overlap");
  }
  const Layer *base = layer_clip_base(doc, layer);
  bool clipped = layer.clip_to_below && base;
  const std::string driven_clip_path = std::string("clip_to_below");
  const int driven_clip = ui_driver_field_begin(C, layer.id, driven_clip_path, -1);
  if (ImGui::Checkbox("Clip to Below", &clipped)) {
    operator_call(C, "mask.clip_to_below", {{"layer", id}, {"set", clipped ? "1" : "0"}});
  }
  ui_driver_typed(C, layer.id, driven_clip_path, -1);
  ui_driver_field_end(C, layer.id, driven_clip_path, -1, driven_clip);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Show it only where the layer below shows (Ctrl Alt G, or Alt-click the line under it in "
                      "the Layers editor)");
  }
  if (layer.clip_to_below && base) {
    ImGui::Indent(ImGui::GetFrameHeight());
    bool exclude = layer.clip_exclude_strokes;
    const std::string driven_exclude_path = std::string("clip_exclude_strokes");
    const int driven_exclude = ui_driver_field_begin(C, layer.id, driven_exclude_path, -1);
    if (ImGui::Checkbox("Exclude Strokes", &exclude)) {
      operator_call(C, "mask.clip_to_below", {{"layer", id}, {"exclude_strokes", exclude ? "1" : "0"}});
    }
    ui_driver_typed(C, layer.id, driven_exclude_path, -1);
    ui_driver_field_end(C, layer.id, driven_exclude_path, -1, driven_exclude);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Clip only to %s's fills, not its lines (shading stops inside the outline)",
                        base->name.c_str());
    }
    ImGui::TextDisabled("Clipped to %s (with the cards)", base->name.c_str());
    if (ImGui::IsItemHovered()) {
      C.wm->mask_hover = {base->id};
    }
    ImGui::Unindent(ImGui::GetFrameHeight());
  }
  ImGui::Spacing();

  enum class RowAction { None, Duplicate, First, Last, Delete };
  struct {
    int index = -1;
    RowAction action = RowAction::None;
  } row_action;
  int drag_from = -1, drag_to = -1;
  for (int i = 0; i < int(layer.masks.size()); i++) {
    const LayerMask m = layer.masks[size_t(i)];
    const OpArgs at{{"layer", id}, {"index", std::to_string(i)}};
    const auto with = [&](const char *key, const std::string &value) {
      OpArgs args = at;
      args[key] = value;
      return args;
    };
    ImGui::PushID(i);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const ImVec2 box_min = ImGui::GetCursorScreenPos();
    const float box_w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorScreenPos(ImVec2(box_min.x + 4.0f, box_min.y + 4.0f));
    ImGui::BeginGroup();
    const float inner_w = box_w - 8.0f;

    /* Header: open arrow, its name (folded: the card in words), switches, menu, x, grip. */
    if (ImGui::ArrowButtonEx("##expand", m.show_expanded ? ImGuiDir_Down : ImGuiDir_Right, ImVec2(h * 0.8f, h),
                             ImGuiButtonFlags_None)) {
      layer.masks[size_t(i)].show_expanded = !m.show_expanded;
    }
    ImGui::SameLine(0, 2);
    const float right_w = h * 4.0f + h * 0.8f + style.ItemSpacing.x * 3.0f + 8.0f;
    const float title_w = std::max(40.0f, inner_w - (ImGui::GetCursorScreenPos().x - box_min.x) - right_w);
    /* Its name, edited in place as a modifier's is; empty, it reads "Mask 2" by its place. */
    const std::string phrase = size_t(i) < phrases.size() ? phrases[size_t(i)] : std::string();
    const std::string hint = "Mask " + std::to_string(i + 1);
    ImGui::SetNextItemWidth(title_w);
    ImGui::InputTextWithHint("##name", hint.c_str(), &layer.masks[size_t(i)].name);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Rename Mask", doc);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !phrase.empty() && !ImGui::IsItemActive()) {
      ImGui::SetTooltip("%s", phrase.c_str());
    }
    ImGui::SameLine(0, 4);
    if (icon_toggle("##viewport", m.enabled, "Mask in the viewport (off: kept, to see what it does)", icon_monitor)) {
      operator_call(C, "mask.set", with("enabled", m.enabled ? "0" : "1"));
    }
    ImGui::SameLine(0, 1);
    if (icon_toggle("##render", m.render, "Mask in renders", icon_camera)) {
      operator_call(C, "mask.set", with("render", m.render ? "0" : "1"));
    }
    /* The card's menu, as Blender's modifier headers have: Duplicate, Move to First / Last, Delete. */
    ImGui::SameLine(0, 4);
    const ImVec2 menu_at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##menu", ImVec2(h, h))) {
      ImGui::OpenPopup("##row_menu");
    }
    if (ImGui::IsItemHovered()) {
      dl->AddRectFilled(menu_at, menu_at + ImVec2(h, h), ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    ImGui::RenderArrow(dl, ImVec2(menu_at.x + h * 0.22f, menu_at.y + h * 0.22f), COL_MOD_ICON, ImGuiDir_Down, 0.75f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Duplicate, move or delete the mask");
    }
    if (ImGui::BeginPopup("##row_menu")) {
      if (ImGui::MenuItem("Duplicate")) {
        row_action = {i, RowAction::Duplicate};
      }
      if (ImGui::MenuItem("Move to First", nullptr, false, i > 0)) {
        row_action = {i, RowAction::First};
      }
      if (ImGui::MenuItem("Move to Last", nullptr, false, i + 1 < int(layer.masks.size()))) {
        row_action = {i, RowAction::Last};
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Delete")) {
        row_action = {i, RowAction::Delete};
      }
      ImGui::EndPopup();
    }
    /* Delete, one click away (as Blender's modifier headers have it). */
    ImGui::SameLine(0, 2);
    const ImVec2 x_at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##delete", ImVec2(h, h))) {
      row_action = {i, RowAction::Delete};
    }
    if (ImGui::IsItemHovered()) {
      dl->AddRectFilled(x_at, x_at + ImVec2(h, h), ImGui::GetColorU32(ImGuiCol_ButtonHovered), 3.0f);
    }
    {
      const ImVec2 xc(x_at.x + h * 0.5f, x_at.y + h * 0.5f);
      const float xs = h * 0.2f;
      dl->AddLine(ImVec2(xc.x - xs, xc.y - xs), ImVec2(xc.x + xs, xc.y + xs), COL_MOD_ICON, 1.5f);
      dl->AddLine(ImVec2(xc.x - xs, xc.y + xs), ImVec2(xc.x + xs, xc.y - xs), COL_MOD_ICON, 1.5f);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Delete the mask");
    }
    ImGui::SameLine(0, 2);
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Drag to reorder");
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload(MASK_PAYLOAD, &i, sizeof(int));
      ImGui::Text("%s", mask_card_name(m, i).c_str());
      ImGui::EndDragDropSource();
    }

    /* Body: Start With, the steps (each + or - Shape, its layers, Invert and Shape), + Shape and
     * - Shape, Affects with View, then Line Ends, as Blender's modifier panels group theirs. */
    if (scroll_to != 0 && (scroll_to == m.uid || scroll_to == uint64_t(i) + 1)) {
      ImGui::SetScrollHereY(0.0f);
      scroll_to = 0;
    }
    if (m.show_expanded) {
      ImGui::Spacing();
      /* A row that's off in the viewport is kept, faded, until it's switched back on. */
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, style.Alpha * (m.enabled ? 1.0f : 0.5f));
      const float column = inner_w * 0.38f;
      ImGui::PushItemWidth(-4.0f);
      /* Affects: what of this layer the card masks. */
      static const char *const CUTS_NAMES[] = {"All", "Fills", "Lines"};
      static const char *const CUTS_TIPS[] = {
          "All of this layer: its fills and its lines",
          "Only this layer's fills; its lines show as if the mask weren't there",
          "Only this layer's lines (sliced, or ended with Line Ends); its fills show as if the mask weren't there, "
          "so two layers that cut each other's lines keep both fills where they overlap"};
      const int cuts = int(m.cuts);
      modifier_row_label("Affects", column, "What of this layer the mask masks: all of it, only its fills, or only its lines");
      const std::string driven_cuts_path = "masks[" + std::to_string(i) + "].cuts";
      const int driven_cuts = ui_driver_field_begin(C, layer.id, driven_cuts_path, -1);
      const float view_w = ImGui::CalcTextSize("View").x + style.FramePadding.x * 2.0f + h * 0.8f;
      ImGui::SetNextItemWidth(std::max(h * 3.0f, ImGui::GetContentRegionAvail().x - view_w - 2.0f));
      const int cuts_clicked = segmented("##cuts", cuts, 3, CUTS_NAMES, CUTS_TIPS, shape_part_icon);
      if (cuts_clicked >= 0) {
        operator_call(C, "mask.set", with("cuts", mask_cuts_identifier(MaskCuts(cuts_clicked))));
      }
      ui_driver_field_end(C, layer.id, driven_cuts_path, -1, driven_cuts);
      /* View: held, the viewport shows this mask in black and white over the drawing (the lines' when it
       * affects only lines, else the fills'). */
      ImGui::SameLine(0, 2);
      const ImVec2 view_at = ImGui::GetCursorScreenPos();
      ImGui::Button("##view", ImVec2(ImGui::GetContentRegionAvail().x, 0));
      const bool viewing = ImGui::IsItemActive();
      if (viewing) {
        C.wm->mask_preview = {layer.id, m.cuts == MaskCuts::Lines, i, ImGui::GetFrameCount()};
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip(m.cuts == MaskCuts::Lines
                              ? "Hold to see this mask in the viewport:\nwhite where it shows the lines, "
                                "black where it hides them"
                              : "Hold to see this mask in the viewport:\nwhite where it shows the fills, "
                                "black where it hides them");
      }
      {
        /* A half white, half black square, then the label. */
        const ImVec2 c(view_at.x + style.FramePadding.x + h * 0.3f, view_at.y + h * 0.5f);
        const float s = h * 0.26f;
        ImDrawList *vdl = ImGui::GetWindowDrawList();
        vdl->AddRectFilled(ImVec2(c.x - s, c.y - s), ImVec2(c.x, c.y + s), IM_COL32(0xf2, 0xf2, 0xf2, 255));
        vdl->AddRectFilled(ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(0x10, 0x10, 0x10, 255));
        vdl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(0x90, 0x90, 0x90, 255));
        vdl->AddText(ImVec2(c.x + s + 4.0f, view_at.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text),
                     "View");
      }
      /* Line Ends: whether this layer's lines end where they cross the edge rather than being sliced
       * by it, and how. A mask that affects only fills leaves the lines alone. */
      ImGui::Spacing();
      const bool lines_cut = m.cuts != MaskCuts::Fills;
      char ends_note[64] = "off";
      if (!lines_cut) {
        std::snprintf(ends_note, sizeof(ends_note), "not used (Affects: Fills)");
      }
      else if (m.end_lines) {
        std::snprintf(ends_note, sizeof(ends_note), m.end_lines_flat ? "Flat" : "Brush, reach %.2f",
                      double(m.line_reach));
      }
      if (mask_subpanel("line_ends", "Line Ends", ends_note, inner_w)) {
        ImGui::BeginDisabled(!lines_cut);
        modifier_row_label("", column);
        const std::string driven_end_path = "masks[" + std::to_string(i) + "].end_lines";
        const int driven_end = ui_driver_field_begin(C, layer.id, driven_end_path, -1);
        bool end_lines = m.end_lines;
        if (ImGui::Checkbox("End Lines at Edge", &end_lines)) {
          operator_call(C, "mask.set", with("end_lines", end_lines ? "1" : "0"));
        }
        ui_driver_field_end(C, layer.id, driven_end_path, -1, driven_end);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
          ImGui::SetTooltip(!lines_cut ? "The mask affects only fills: it leaves the lines alone"
                                       : "This layer's lines end where they cross the edge of this mask's shape, as "
                                         "if drawn to there, instead of being sliced by it (a brushed line's dabs "
                                         "aren't cut in half)");
        }
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!lines_cut || !m.end_lines);
        static const char *const ENDS_IDS[] = {"BRUSH", "FLAT"};
        static const char *const ENDS_NAMES[] = {"Brush", "Flat"};
        static const char *const ENDS_TIPS[] = {
            "The new ends taper and fade as the brush's own ends do (its Taper and Fade settings); a solid "
            "line's are round",
            "The new ends stop as the line is, without tapering; a solid line's are cut square"};
        const int ends = m.end_lines_flat ? 1 : 0;
        modifier_row_label("Ends", column, "How a line ends where the edge cuts it");
        const std::string driven_ends_path = "masks[" + std::to_string(i) + "].ends";
        const int driven_ends = ui_driver_field_begin(C, layer.id, driven_ends_path, -1);
        const int ends_clicked = segmented("##ends", ends, 2, ENDS_NAMES, ENDS_TIPS);
        if (ends_clicked >= 0) {
          operator_call(C, "mask.set", with("ends", ENDS_IDS[ends_clicked]));
        }
        ui_driver_field_end(C, layer.id, driven_ends_path, -1, driven_ends);
        /* Reach: how far Brush ends go past the edge, in line widths; animatable, with its key button. */
        const bool reach_used = !m.end_lines_flat;
        const std::string reach_path = "masks[" + std::to_string(i) + "].reach";
        const int reach_state = layer_key_state(layer, {reach_path}, doc.frame_current);
        modifier_row_label("Reach", column,
                           "How far the lines go past the edge, in line widths: 0 ends them just touching it, 0.5 "
                           "puts their last dab's middle on it, below 0 leaves a gap. It can be keyed");
        ImGui::BeginDisabled(!reach_used);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ui_key_button_width() - 4.0f);
        const int reach_tint = ui_key_tint_push(reach_state | layer_action_bone_state(layer, {reach_path}));
        const int driven_reach = ui_driver_field_begin(C, layer.id, reach_path, -1);
        float reach = m.line_reach;
        if (ui_drag_float("##reach", &reach, 0.01f, -2.0f, 2.0f, "%.2f line widths",
                             ImGuiSliderFlags_AlwaysClamp))
        {
          /* Shown as it changes; one undo step when let go. */
          layer.masks[size_t(i)].line_reach = reach;
        }
        ui_driver_typed(C, layer.id, reach_path, -1);
        ui_driver_field_end(C, layer.id, reach_path, -1, driven_reach);
        ImGui::PopStyleColor(reach_tint);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          char value[32];
          std::snprintf(value, sizeof(value), "%g", double(layer.masks[size_t(i)].line_reach));
          operator_call(C, "mask.set", with("reach", value));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled) &&
            !reach_used)
        {
          ImGui::SetTooltip("With Ends: Brush: how far the lines go past the edge");
        }
        if (ui_key_button("##reach_key", reach_state)) {
          operator_call(C, "anim.keyframe_toggle", {{"layer", id}, {"paths", reach_path}});
        }
        ImGui::EndDisabled();
        ui_flash_row(layer.id, reach_path);
        const int reach_pin = pin_state(layer, {reach_path}, reach_state);
        if (ui_pin_button("##reach_pin", reach_pin)) {
          pin_toggle(C, layer.id, reach_path, reach_pin);
        }
        if (reach_pin == 1) {
          ui_pinned_row();
        }
        ImGui::EndDisabled();
      }
      ImGui::Spacing();
      /* Masking Areas: a stack, as the Layers editor's, painted from the bottom up on Show All or Hide All. */
      ImGui::Spacing();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("Masking Areas");
      ImGui::SameLine();
      if (ImGui::Button("+##area_add", ImVec2(h * 1.2f, h))) {
        ImGui::OpenPopup("##area_menu");
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Add a masking area, on top of the stack (or in the group that's picked)");
      }
      if (ImGui::BeginPopup("##area_menu")) {
        const bool here = area.card == i && area.step >= 0;
        const int into = here && m.steps[size_t(area.step)].group ? area.step : -1;
        static const char *const ADD_AREA[] = {"      Add Area...", "      Subtract Area...", "      Keep Only Area..."};
        static const char *const ADD_AREA_TIPS[] = {"Paint another layer's shape white, then click it",
                                                    "Paint another layer's shape black, then click it",
                                                    "Keep only what's inside another layer, then click it"};
        for (int k = 0; k < 3; k++) {
          if (ImGui::Selectable(ADD_AREA[k])) {
            OpArgs pick = at;
            pick["verb"] = mask_verb_identifier(MaskVerb(k));
            if (into >= 0) {
              pick["group"] = std::to_string(into);
            }
            operator_call(C, "mask.pick", pick);
          }
          const ImVec2 lo = ImGui::GetItemRectMin();
          mask_verb_icon(ImGui::GetWindowDrawList(), ImVec2(lo.x + h * 0.4f, lo.y + ImGui::GetItemRectSize().y * 0.5f),
                         h * 0.24f, k, 0);
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", ADD_AREA_TIPS[k]);
          }
        }
        if (ImGui::Selectable("      Group")) {
          operator_call(C, "mask.group", at);
          area = {layer.id, i, int(m.steps.size()), -1};
        }
        ImGui::Separator();
        for (int k = 0; k < 2; k++) {
          if (ImGui::Selectable(k == 0 ? "      Show All" : "      Hide All")) {
            OpArgs add_all{{"layer", id}, {"card", std::to_string(i)}, {"whole", k == 0 ? "SHOW" : "HIDE"}};
            if (into >= 0) {
              add_all["group"] = std::to_string(into);
            }
            operator_call(C, "mask.add", add_all);
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(k == 0 ? "Paint the whole canvas white, at the bottom of the stack or group"
                                     : "Paint the whole canvas black, at the bottom of the stack or group");
          }
          const ImVec2 lo = ImGui::GetItemRectMin();
          mask_paint_icon(ImGui::GetWindowDrawList(), ImVec2(lo.x + h * 0.4f, lo.y + ImGui::GetItemRectSize().y * 0.5f),
                          h * 0.22f, k == 0, IM_COL32(0x90, 0x90, 0x90, 255));
        }
        ImGui::EndPopup();
      }
      /* What each row does here, from the base up: a row that changes nothing is dimmed and says why. */
      std::vector<const char *> notes(m.steps.size(), nullptr);
      std::vector<bool> dims(m.steps.size(), false);
      {
        bool nothing_yet = true;
        for (size_t k = 0; k < m.steps.size(); k++) {
          const MaskStep &s = m.steps[k];
          const MaskVerb verb = mask_step_verb(s);
          if (const char *problem = mask_group_problem(m, k)) {
            notes[k] = problem;
          }
          else if (verb == MaskVerb::KeepOnly && !s.group) {
            notes[k] = "Hides everything outside this layer. To keep part of a shape, put this in a group";
          }
          else if (nothing_yet && verb == MaskVerb::Add && !s.invert && m.start_shown) {
            notes[k] = "Adds nothing: Show All already shows everything";
            dims[k] = true;
          }
          else if (nothing_yet && verb == MaskVerb::Remove && !m.start_shown) {
            notes[k] = "Removes nothing: Hide All already hides everything";
            dims[k] = true;
          }
          nothing_yet = nothing_yet && dims[k];
        }
        /* Show All or Hide All paints over everything below it. */
        for (size_t k = m.steps.size(); k-- > 1;) {
          if (m.steps[k].whole) {
            for (size_t below = 0; below < k; below++) {
              notes[below] = m.steps[k].mode == MaskMode::Add ? "Covered by the Show All above it" : "Covered by the Hide All above it";
              dims[below] = true;
            }
            break;
          }
        }
      }
      const float list_w = ImGui::GetContentRegionAvail().x - 4.0f;
      /* The rows as they're listed, for Shift's range. */
      std::vector<int> row_order;
      for (int si = int(m.steps.size()) - 1; si >= 0; si--) {
        row_order.push_back(si << 8);
        if (m.steps[size_t(si)].group && m.steps[size_t(si)].show_expanded) {
          for (int ci = int(m.steps[size_t(si)].children.size()) - 1; ci >= 0; ci--) {
            row_order.push_back((si << 8) | (ci + 1));
          }
        }
      }
      /* A row dragged that's one of several picked, in the stack, takes the others with it. */
      const auto moved_rows = [&](int step, int child) {
        const int row = (step << 8) | (child + 1);
        std::string rows;
        if (picked_rows.size() > 1 && child < 0 && std::find(picked_rows.begin(), picked_rows.end(), row) != picked_rows.end() &&
            std::all_of(picked_rows.begin(), picked_rows.end(), [](int r) { return (r & 0xff) == 0; }))
        {
          for (const int r : picked_rows) {
            rows += (rows.empty() ? "" : "|") + std::to_string(r);
          }
        }
        return rows;
      };
      const auto area_row = [&](const MaskStep &s, int si, int ci) {
        ImGui::PushID(si * 1024 + ci + 1);
        const float indent = ci >= 0 ? h * 1.2f : 0.0f;
        const ImVec2 row_at = ImGui::GetCursorScreenPos();
        const int this_row = (si << 8) | (ci + 1);
        const bool active_here = area.card == i && area.step == si && area.child == ci;
        const bool picked_here = area.card == i && std::find(picked_rows.begin(), picked_rows.end(), this_row) != picked_rows.end();
        ImGui::SetNextItemAllowOverlap();
        const bool released = ImGui::Selectable("##row", picked_here, ImGuiSelectableFlags_AllowOverlap, ImVec2(list_w, h));
        /* As the Layers editor's rows: Shift picks from the active one to here and it stays active, Ctrl adds or takes
         * out this one, and a plain press picks just this one. */
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
          const ImGuiIO &io = ImGui::GetIO();
          if (io.KeyShift && !io.KeyCtrl && area.card == i && area.step >= 0) {
            const int active = (area.step << 8) | (area.child + 1);
            const size_t a = size_t(std::find(row_order.begin(), row_order.end(), active) - row_order.begin());
            const size_t b = size_t(std::find(row_order.begin(), row_order.end(), this_row) - row_order.begin());
            if (a < row_order.size() && b < row_order.size()) {
              picked_rows.assign(row_order.begin() + std::ptrdiff_t(std::min(a, b)), row_order.begin() + std::ptrdiff_t(std::max(a, b) + 1));
            }
          }
          else if (io.KeyCtrl && area.card == i && picked_here) {
            std::erase(picked_rows, this_row);
            if (active_here && !picked_rows.empty()) {
              area = {layer.id, i, picked_rows.back() >> 8, (picked_rows.back() & 0xff) - 1};
            }
            else if (active_here) {
              /* The only one picked: it stays. */
              picked_rows.push_back(this_row);
            }
          }
          else if (io.KeyCtrl && area.card == i && area.step >= 0) {
            picked_rows.push_back(this_row);
            area = {layer.id, i, si, ci};
          }
          else if (picked_here) {
            narrow_row = this_row;
            area = {layer.id, i, si, ci};
          }
          else {
            picked_rows = {this_row};
            area = {layer.id, i, si, ci};
          }
        }
        if (released && narrow_row == this_row && area.card == i) {
          const ImGuiIO &io = ImGui::GetIO();
          if (io.MouseDragMaxDistanceSqr[0] <= io.MouseDragThreshold * io.MouseDragThreshold) {
            picked_rows = {this_row};
          }
          narrow_row = -1;
        }
        if (ImGui::BeginDragDropSource()) {
          const int moved[3] = {i, si, ci};
          ImGui::SetDragDropPayload("OANIM_MASK_AREA", moved, sizeof(moved));
          ImGui::TextUnformatted(picked_here && picked_rows.size() > 1 ? "Masking areas"
                                 : s.group                             ? s.name.c_str()
                                                                       : "Masking area");
          ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
          const float top_y = ImGui::GetItemRectMin().y, bottom_y = ImGui::GetItemRectMax().y;
          const float y = ImGui::GetIO().MousePos.y;
          const bool onto = s.group && ci < 0 && y > top_y + h * 0.25f && y < bottom_y - h * 0.25f;
          const bool upper = y < (top_y + bottom_y) * 0.5f;
          ImDrawList *wdl = ImGui::GetWindowDrawList();
          if (onto) {
            wdl->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), UI_DROP_LINE_COLOR, 3.0f, 0, 2.0f);
          }
          else {
            const float line_y = upper ? top_y : bottom_y;
            wdl->AddLine(ImVec2(row_at.x + indent, line_y), ImVec2(row_at.x + list_w, line_y), UI_DROP_LINE_COLOR, 2.0f);
          }
          if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("OANIM_MASK_AREA", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const int *moved = static_cast<const int *>(payload->Data);
            if (moved[0] == i) {
              /* The list runs top to bottom, the steps bottom to top. */
              area_drop = {{"layer", id}, {"index", std::to_string(i)}, {"step", std::to_string(moved[1])}};
              if (moved[2] >= 0) {
                area_drop["child"] = std::to_string(moved[2]);
              }
              if (const std::string rows = moved_rows(moved[1], moved[2]); !rows.empty()) {
                area_drop["rows"] = rows;
              }
              if (onto) {
                area_drop["group"] = std::to_string(si);
                area_drop["to"] = std::to_string(s.children.size());
              }
              else if (ci >= 0) {
                area_drop["group"] = std::to_string(si);
                area_drop["to"] = std::to_string(upper ? ci + 1 : ci);
              }
              else {
                area_drop["to"] = std::to_string(upper ? si + 1 : si);
              }
            }
          }
          ImGui::EndDragDropTarget();
        }
        if (picked_here) {
          dl->AddRectFilled(row_at, row_at + ImVec2(list_w, h),
                            active_here ? IM_COL32(0x3a, 0x5a, 0x8c, 255) : IM_COL32(0x2e, 0x42, 0x60, 255), 3.0f);
        }
        const bool dimmed = ci < 0 && dims[size_t(si)];
        const ImU32 text_col = ImGui::GetColorU32(dimmed ? ImGuiCol_TextDisabled : ImGuiCol_Text);
        float x = row_at.x + indent + 2.0f;
        if (s.group) {
          ImGui::SetCursorScreenPos(ImVec2(x, row_at.y));
          if (ImGui::ArrowButtonEx("##fold", s.show_expanded ? ImGuiDir_Down : ImGuiDir_Right, ImVec2(h * 0.8f, h),
                                   ImGuiButtonFlags_None))
          {
            layer.masks[size_t(i)].steps[size_t(si)].show_expanded = !s.show_expanded;
          }
        }
        x += h * 0.85f;
        /* Its +, - or Keep Only: clicking goes on to the next. */
        ImGui::SetCursorScreenPos(ImVec2(x, row_at.y));
        const MaskVerb verb = mask_step_verb(s);
        if (ImGui::InvisibleButton("##verb", ImVec2(h, h))) {
          OpArgs args = at;
          args["step"] = std::to_string(si);
          if (ci >= 0) {
            args["child"] = std::to_string(ci);
          }
          args["verb"] = mask_verb_identifier(MaskVerb((int(verb) + 1) % (s.whole ? 2 : 3)));
          operator_call(C, "mask.set", args);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          if (s.whole) {
            ImGui::SetTooltip("%s\nClick to swap Show All and Hide All", s.mode == MaskMode::Add ? "Paints the whole canvas white" : "Paints the whole canvas black");
          }
          else {
            ImGui::SetTooltip("%s\nClick for the next one", MASK_VERB_TIPS[int(verb)]);
          }
        }
        if (s.whole) {
          mask_paint_icon(dl, ImVec2(x + h * 0.5f, row_at.y + h * 0.5f), h * 0.24f, s.mode == MaskMode::Add, IM_COL32(0x90, 0x90, 0x90, 255));
        }
        else {
          mask_verb_icon(dl, ImVec2(x + h * 0.5f, row_at.y + h * 0.5f), h * 0.3f, int(verb), 0);
        }
        x += h + 4.0f;
        std::string label = s.whole ? std::string(s.mode == MaskMode::Add ? "Show All" : "Hide All") : s.name;
        if (!s.group && label.empty()) {
          for (const uint64_t l : s.layers) {
            const Layer *source = layer_find(doc, l);
            label += (label.empty() ? "" : ", ") + (source ? source->name : std::string("(missing)"));
          }
        }
        label += s.invert && verb == MaskVerb::Add ? " (outside)" : "";
        if (s.group) {
          /* A folder, as the Layers editor's. */
          const ImU32 folder = IM_COL32(0xc8, 0xc8, 0xc8, 255);
          const float fy = row_at.y + h * 0.5f, fw = h * 0.36f, fh = h * 0.24f;
          dl->AddRectFilled(ImVec2(x, fy - fh), ImVec2(x + fw * 0.9f, fy), folder, 1.0f);
          dl->AddRectFilled(ImVec2(x, fy - fh * 0.6f), ImVec2(x + fw * 2.0f, fy + fh), folder, 1.5f);
          x += fw * 2.0f + 5.0f;
        }
        dl->AddText(ImVec2(x, row_at.y + style.FramePadding.y), text_col, label.c_str());
        float right = row_at.x + list_w - 6.0f;
        if (s.global != 0) {
          const std::string count = std::to_string(mask_area_users(doc, s.global));
          right -= ImGui::CalcTextSize(count.c_str()).x;
          dl->AddText(ImVec2(right, row_at.y + style.FramePadding.y), IM_COL32(0x6f, 0xd3, 0xc4, 255), count.c_str());
          right -= h * 0.8f;
          dl->AddCircle(ImVec2(right + h * 0.2f, row_at.y + h * 0.5f), h * 0.14f, IM_COL32(0x6f, 0xd3, 0xc4, 255), 0, 1.3f);
          dl->AddCircle(ImVec2(right + h * 0.42f, row_at.y + h * 0.5f), h * 0.14f, IM_COL32(0x6f, 0xd3, 0xc4, 255), 0, 1.3f);
        }
        if (!s.group && mask_step_outline_layer(doc, s)) {
          right -= h * 0.7f;
          dl->AddText(ImVec2(right, row_at.y + style.FramePadding.y), IM_COL32(0xff, 0xbf, 0x73, 255), "!");
        }
        /* X or Delete over it deletes it (mask.area_delete_hovered). */
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseHoveringRect(row_at, row_at + ImVec2(list_w, h)))
        {
          C.wm->mask_area_hover = {layer.id, i, si, ci};
        }
        /* Right-click, anywhere on the row: what to turn it into, and Delete. */
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseHoveringRect(row_at, row_at + ImVec2(list_w, h)) && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
          /* On one of the picked, for all of them; else just this one. */
          if (!picked_here || area.card != i) {
            picked_rows = {this_row};
            area = {layer.id, i, si, ci};
          }
          ImGui::OpenPopup("##area_row_menu");
        }
        if (ImGui::BeginPopup("##area_row_menu")) {
          std::string row_list;
          bool in_stack = true;
          for (const int row : picked_rows) {
            row_list += (row_list.empty() ? "" : "|") + std::to_string(row);
            in_stack = in_stack && (row & 0xff) == 0 && (row >> 8) < int(m.steps.size()) && !m.steps[size_t(row >> 8)].group;
          }
          OpArgs args = at;
          args["rows"] = row_list;
          if (ImGui::BeginMenu("Convert To")) {
            static const char *const VERB_NAMES[] = {"Add", "Subtract", "Keep Only"};
            static const char *const WHOLE_NAMES[] = {"Show All", "Hide All"};
            for (int k = 0; k < (s.whole ? 2 : 3); k++) {
              if (ImGui::MenuItem((s.whole ? WHOLE_NAMES : VERB_NAMES)[k], nullptr, int(verb) == k) &&
                  (int(verb) != k || picked_rows.size() > 1))
              {
                OpArgs convert = args;
                convert["verb"] = mask_verb_identifier(MaskVerb(k));
                operator_call(C, "mask.areas_set", convert);
              }
            }
            ImGui::EndMenu();
          }
          if (ImGui::MenuItem("Join as Group", "Ctrl G", false, in_stack)) {
            operator_call(C, "mask.areas_group", args);
          }
          if (!in_stack && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Groups don't go inside groups: pick areas that are in the stack, not in a group");
          }
          ImGui::Separator();
          if (ImGui::MenuItem("Delete", "X")) {
            operator_call(C, "mask.areas_delete", args);
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(ci < 0 && m.steps.size() == 1 ? "Delete this area, and the mask with it: it's the last one"
                                                            : s.group ? "Delete this group and what's in it"
                                                                      : "Delete this area");
          }
          ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(ImVec2(row_at.x, row_at.y + h + 1.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        if (ci < 0 && notes[size_t(si)]) {
          ImGui::SetCursorPosX(ImGui::GetCursorPosX() + h * 2.1f);
          ImGui::PushTextWrapPos(0.0f);
          ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.45f, 1.0f), "%s", notes[size_t(si)]);
          ImGui::PopTextWrapPos();
        }
        ImGui::PopID();
      };
      for (int si = int(m.steps.size()) - 1; si >= 0; si--) {
        const MaskStep &s = m.steps[size_t(si)];
        area_row(s, si, -1);
        if (s.group && s.show_expanded) {
          for (int ci = int(s.children.size()) - 1; ci >= 0; ci--) {
            area_row(s.children[size_t(ci)], si, ci);
          }
        }
      }
      /* The base: Show All or Hide All, always at the bottom. */
      {
        const ImVec2 base_at = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##base", area.card == i && area.step == -1, 0, ImVec2(list_w, h))) {
          area = {layer.id, i, -1, -1};
          picked_rows.clear();
        }
        if (area.card == i && area.step == -1) {
          dl->AddRectFilled(base_at, base_at + ImVec2(list_w, h), IM_COL32(0x3a, 0x5a, 0x8c, 255), 3.0f);
        }
        if (ImGui::BeginDragDropTarget()) {
          ImGui::GetWindowDrawList()->AddLine(ImVec2(base_at.x, base_at.y), ImVec2(base_at.x + list_w, base_at.y),
                                              UI_DROP_LINE_COLOR, 2.0f);
          if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("OANIM_MASK_AREA", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
            const int *moved = static_cast<const int *>(payload->Data);
            if (moved[0] == i) {
              area_drop = {{"layer", id}, {"index", std::to_string(i)}, {"step", std::to_string(moved[1])}, {"to", "0"}};
              if (moved[2] >= 0) {
                area_drop["child"] = std::to_string(moved[2]);
              }
              if (const std::string rows = moved_rows(moved[1], moved[2]); !rows.empty()) {
                area_drop["rows"] = rows;
              }
            }
          }
          ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("What the areas above paint on. Right-click to change it");
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          ImGui::OpenPopup("##base_menu");
        }
        if (ImGui::BeginPopup("##base_menu")) {
          if (ImGui::BeginMenu("Convert To")) {
            if (ImGui::MenuItem("Show All", nullptr, m.start_shown) && !m.start_shown) {
              operator_call(C, "mask.set", with("start", "EVERYTHING"));
            }
            if (ImGui::MenuItem("Hide All", nullptr, !m.start_shown) && m.start_shown) {
              operator_call(C, "mask.set", with("start", "NOTHING"));
            }
            ImGui::EndMenu();
          }
          ImGui::EndPopup();
        }
        mask_paint_icon(dl, ImVec2(base_at.x + h * 1.35f, base_at.y + h * 0.5f), h * 0.24f, m.start_shown,
                        IM_COL32(0x90, 0x90, 0x90, 255));
        dl->AddText(ImVec2(base_at.x + h * 1.9f, base_at.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                    m.start_shown ? "Show All" : "Hide All");
      }
      ImGui::Spacing();
      ImGui::PopItemWidth();
      ImGui::PopStyleVar();
    }
    ImGui::EndGroup();
    const ImVec2 box_max(box_min.x + box_w, ImGui::GetItemRectMax().y + 6.0f);
    dl->ChannelsSetCurrent(0);
    /* Hovering the card outlines its layers in the viewport. */
    const bool box_hovered = ImGui::IsMouseHoveringRect(box_min, box_max) && ImGui::IsWindowHovered(
                                                                              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (box_hovered && C.wm->mask_hover.empty()) {
      C.wm->mask_hover = mask_card_layers(m);
    }
    dl->AddRectFilled(box_min, box_max, COL_MOD_BOX, 5.0f);
    const bool pointed_at = box_hovered || summary_hover == i;
    dl->AddRect(box_min, box_max, pointed_at ? IM_COL32(0xff, 0x9a, 0x3c, 160) : IM_COL32(0x2a, 0x2a, 0x2a, 255),
                5.0f, 0, summary_hover == i ? 2.0f : 1.0f);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(box_min);
    ImGui::Dummy(ImVec2(box_w, box_max.y - box_min.y));
    /* A whole mask dragged, not one of its areas. */
    const ImGuiPayload *dragged = ImGui::GetDragDropPayload();
    if (dragged && dragged->IsDataType(MASK_PAYLOAD) && ImGui::BeginDragDropTarget()) {
      const bool upper = ImGui::GetIO().MousePos.y < (box_min.y + box_max.y) * 0.5f;
      const float line_y = upper ? box_min.y - 3.0f : box_max.y + 3.0f;
      ImGui::GetWindowDrawList()->AddLine(ImVec2(box_min.x, line_y), ImVec2(box_max.x, line_y), UI_DROP_LINE_COLOR,
                                          2.0f);
      if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(MASK_PAYLOAD, ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        drag_from = *static_cast<const int *>(payload->Data);
        drag_to = upper ? i : i + 1;
      }
      ImGui::EndDragDropTarget();
    }
    ImGui::Spacing();
    ImGui::PopID();
  }
  if (!area_drop.empty()) {
    operator_call(C, "mask.area_move", area_drop);
  }
  if (area.card < int(layer.masks.size()) && area.layer == layer.id && !layer.masks.empty()) {
    const std::vector<MaskStep> &steps = layer.masks[size_t(area.card)].steps;
    if (area.step < int(steps.size()) && (area.child < 0 || area.child < int(steps[size_t(area.step)].children.size()))) {
      mask_area_panel(C, layer, area);
    }
  }
  if (layer.masks.empty() && !layer.clip_to_below) {
    ImGui::TextDisabled("No masks: the layer shows everywhere.");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Add Mask, or Alt-drag a layer onto this one in the Layers editor (Shift to hide it). "
                        "Join Lines takes two picked layers that meet.");
    ImGui::PopTextWrapPos();
  }
  if (row_action.index >= 0) {
    const std::string index = std::to_string(row_action.index);
    switch (row_action.action) {
      case RowAction::Duplicate:
        operator_call(C, "mask.duplicate", {{"layer", id}, {"index", index}});
        break;
      case RowAction::First:
        operator_call(C, "mask.move", {{"layer", id}, {"index", index}, {"to", "0"}});
        break;
      case RowAction::Last:
        operator_call(C, "mask.move",
                      {{"layer", id}, {"index", index}, {"to", std::to_string(int(layer.masks.size()) - 1)}});
        break;
      case RowAction::Delete:
        operator_call(C, "mask.remove", {{"layer", id}, {"index", index}});
        break;
      case RowAction::None:
        break;
    }
  }
  if (drag_from >= 0) {
    const int to = drag_to > drag_from ? drag_to - 1 : drag_to;
    if (to != drag_from) {
      operator_call(C, "mask.move", {{"layer", id}, {"index", std::to_string(drag_from)}, {"to", std::to_string(to)}});
    }
  }
}

/* Used as a Mask: which layers use this one as a mask (clicking one selects it). Only on layers
 * something masks with. */
static void used_as_mask_panel(Context &C, Layer &layer)
{
  Document &doc = *C.document;
  std::vector<const Layer *> users;
  for (const Layer *l : document_layers_flat(doc)) {
    for (const LayerMask &m : l->masks) {
      const std::vector<uint64_t> named = mask_card_layers(m);
      if (std::find(named.begin(), named.end(), layer.id) != named.end()) {
        users.push_back(l);
        break;
      }
    }
  }
  if (users.empty() || !ui_panel_begin("Used as a Mask")) {
    return;
  }
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  const char *label = "Masking";
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetColumnWidth() - ImGui::CalcTextSize(label).x));
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  for (size_t i = 0; i < users.size(); i++) {
    if (i > 0) {
      ImGui::SameLine(0, 4);
      if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(users[i]->name.c_str()).x + 16.0f) {
        ImGui::NewLine();
      }
    }
    ImGui::PushID(int(i));
    if (ImGui::SmallButton(users[i]->name.c_str())) {
      doc.active_layer = users[i]->id;
      C.undo->push("Active Layer", doc);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Uses this layer as a mask; click to select it");
    }
    ImGui::PopID();
  }
  ui_panel_end();
}

/* The Camera tab: where the camera is, its zoom and roll (each keyable), and how the viewport shows it. */
static void camera_tab(Context &C)
{
  Document &doc = *C.document;
  Layer &camera = document_camera(doc);
  const rna::PointerRNA ptr{&camera, &Layer::rna()};
  const rna::PointerRNA doc_ptr{&doc, &Document::rna()};
  const char *path = "oanim.data.camera";
  if (ui_panel_begin("Camera")) {
    key_next(C, camera, {"position"});
    ui_prop_pair(C, ptr, "Position", "location_x", "X", "location_y", "Y", path);
    key_next(C, camera, {"zoom"});
    ui_prop(C, doc_ptr, "camera_zoom", "oanim.data.document");
    key_next(C, camera, {"rotation"});
    ui_prop(C, doc_ptr, "camera_roll", "oanim.data.document");
    ui_panel_end();
  }
  if (ui_panel_begin("Viewport Display")) {
    ui_prop(C, doc_ptr, "camera_passepartout", "oanim.data.document");
    ui_panel_end();
  }
}

/* The Masking tab: the active layer's masks, and how it masks other layers. */
static void masking_tab(Context &C)
{
  Layer *layer = document_active_layer(*C.document);
  if (!layer) {
    ImGui::TextDisabled("No active layer.");
    return;
  }
  mask_cards(C, *layer);
  ImGui::Spacing();
  used_as_mask_panel(C, *layer);
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
  const char *path = "oanim.data.layers.active";
  if (ui_panel_begin("Layer")) {
    ui_prop(C, ptr, "name", path);
    key_next(C, *layer, {"visible"});
    ui_prop(C, ptr, "visible", path);
    draw_order_row(C, *layer);
    ui_prop(C, ptr, "render_visible", path);
    ui_prop(C, ptr, "locked", path);
    if (layer->type == LayerType::Vector && layer_armature(doc, layer->id)) {
      /* Edited along with its bone layer's bones (and so in its All collection; Assign or M put it in
       * others). */
      ui_prop(C, ptr, "edit_with_bones", path);
    }
    if (layer->type == LayerType::Vector && (layer->ignore_group_deforms || layer_has_group_deforms(doc, *layer, false))) {
      ui_prop(C, ptr, "ignore_group_deforms", path);
    }
    if (layer->is_group()) {
      /* Picking the group edits what's inside it. */
      ui_prop(C, ptr, "edit_descendants", path);
    }
    if (layer->is_bone_layer() && layer_parent(doc, layer->id) && layer_armature(doc, layer_parent(doc, layer->id)->id)) {
      /* A bone group inside another: its bones show with the outer one's. */
      ui_prop(C, ptr, "sub_bone_group", path);
    }
    ui_prop(C, ptr, "label_color", path);
    ui_panel_end();
  }
  /* On a reference, a channel it took over from its original shows teal. */
  const auto own = [&](const char *name) { return layer->is_reference() && layer->reference.own_channels.count(name) > 0; };
  const auto channel_args = [](const char *name) { return std::vector<OpArgs>{{{"kind", "CHANNEL"}, {"path", name}}}; };
  if (ui_panel_begin("Transform")) {
    bool on = changed_begin(own("position"));
    key_next(C, *layer, {"position"});
    ui_prop_pair(C, ptr, "Location", "location_x", "X", "location_y", "Y", path);
    changed_end(C, on, "##changed_position", channel_args("position"), channel_args("position"));
    on = changed_begin(own("rotation"));
    key_next(C, *layer, {"rotation"});
    ui_prop(C, ptr, "rotation", path);
    changed_end(C, on, "##changed_rotation", channel_args("rotation"), channel_args("rotation"));
    on = changed_begin(own("scale"));
    key_next(C, *layer, {"scale"});
    ui_prop_pair(C, ptr, "Scale", "scale_x", "X", "scale_y", "Y", path);
    changed_end(C, on, "##changed_scale", channel_args("scale"), channel_args("scale"));
    on = changed_begin(own("shear"));
    key_next(C, *layer, {"shear"});
    ui_prop_pair(C, ptr, "Shear", "shear_x", "X", "shear_y", "Y", path);
    changed_end(C, on, "##changed_shear", channel_args("shear"), channel_args("shear"));
    ui_panel_end();
  }
  if (ui_panel_begin("Compositing")) {
    const bool on = changed_begin(own("opacity"));
    key_next(C, *layer, {"opacity"});
    ui_prop(C, ptr, "opacity", path);
    changed_end(C, on, "##changed_opacity", channel_args("opacity"), channel_args("opacity"));
    ui_panel_end();
  }
  if (const Layer *armature = layer->is_bone_layer() ? nullptr : layer_armature(doc, layer->id)) {
    binding_panel(C, *layer, *layer_find(doc, armature->id));
  }
}

/* The Media tab's picture of the current frame: a small texture per Media layer, refilled when the
 * frame it shows changes. */
static unsigned int media_thumbnail(const Layer &layer, const MediaFrame &frame)
{
  struct Thumb {
    unsigned int texture = 0;
    uint64_t serial = 0;
  };
  static std::map<uint64_t, Thumb> thumbs;
  Thumb &t = thumbs[layer.id];
  if (!t.texture) {
    glGenTextures(1, &t.texture);
  }
  if (t.serial != frame.serial) {
    glBindTexture(GL_TEXTURE_2D, t.texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    t.serial = frame.serial;
  }
  return t.texture;
}

/* How a clip's timing reads in words: "30 fps video in a 24 fps scene: plays in real time". */
static std::string media_timing_text(const Document &doc, const MediaClip &clip, const MediaInfo &info)
{
  char text[200];
  if (info.kind == MediaKind::Sequence) {
    std::snprintf(text, sizeof(text), "One image per frame (%d images)", info.frame_count);
  }
  else if (info.kind == MediaKind::Video && clip.timing == MediaTiming::FrameByFrame) {
    std::snprintf(text, sizeof(text), "%.3g fps video in a %.3g fps scene: one video frame per frame (%.2gx speed)",
                  info.fps, double(doc.fps), double(doc.fps) / std::max(info.fps, 1.0));
  }
  else if (info.kind == MediaKind::Video) {
    std::snprintf(text, sizeof(text), "%.3g fps video in a %.3g fps scene: plays in real time", info.fps,
                  double(doc.fps));
  }
  else {
    return {};
  }
  return text;
}

/* A clip setting changed in the tab: overwrite what it now covers, and the undo step. */
static void media_clip_changed(Context &C, Layer &layer, const MediaClip &clip, const char *undo_name)
{
  media_clips_tidy(*C.document, layer, clip.id);
  C.undo->push(undo_name, *C.document);
}

/* The Media tab: the file (what it is, Replace, Reload, Pack; Locate when it is missing), the clip at
 * the current frame (Start, Length, In, After End, Timing), how it looks, and its sound. */
static void media_tab(Context &C)
{
  Document &doc = *C.document;
  Layer *layer = document_active_layer(doc);
  if (!layer || !layer->is_media()) {
    ImGui::TextDisabled("Select a Media layer.");
    return;
  }
  const rna::PointerRNA ptr{layer, &Layer::rna()};
  const char *path = "oanim.data.layers.active";
  /* The clip on this frame; else the one picked in the timeline, else the nearest. */
  MediaClip *clip = media_clip_at(*layer, doc.frame_current);
  const bool on_frame = clip != nullptr;
  for (MediaClip &c : layer->media.clips) {
    if (!clip && c.selected) {
      clip = &c;
    }
  }
  int nearest = INT32_MAX;
  for (MediaClip &c : layer->media.clips) {
    const int away = std::min(std::abs(c.start - doc.frame_current), std::abs(c.start + c.length - 1 - doc.frame_current));
    if (!on_frame && !std::any_of(layer->media.clips.begin(), layer->media.clips.end(),
                                  [](const MediaClip &k) { return k.selected; }) && away < nearest)
    {
      nearest = away;
      clip = &c;
    }
  }
  const float h = ImGui::GetFrameHeight();
  const float spacing = ImGui::GetStyle().ItemSpacing.x;
  const std::string id = std::to_string(layer->id);

  if (ui_panel_begin("Media")) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TableSetColumnIndex(1);
    if (!clip) {
      ImGui::TextDisabled("No clips: Replace from Current Frame adds one");
    }
    else {
      if (!on_frame) {
        ImGui::TextDisabled("Not on this frame: shows on frames %d to %d", clip->start, clip->start + clip->length - 1);
      }
      const MediaInfo info = media_clip_info(*clip);
      const bool missing = media_clip_missing(*clip);
      /* The frame showing, small (the clip's first, when it isn't on this frame). */
      const MediaShown shown = media_shown_at(doc, *layer, doc.frame_current);
      const double seconds = on_frame ? (shown.clip ? shown.seconds : -1.0)
                                      : media_clip_seconds(doc, *clip, double(clip->start));
      if (MediaFramePtr frame = seconds >= 0.0 ? media_frame(media_clip_file(*clip), clip->sequence, seconds, false)
                                               : nullptr)
      {
        const float w = std::min(ImGui::GetContentRegionAvail().x, h * 9.0f);
        const float th = w * float(frame->height) / float(std::max(frame->width, 1));
        ImGui::Image(ImTextureRef(ImTextureID(intptr_t(media_thumbnail(*layer, *frame)))), ImVec2(w, std::min(th, w)));
      }
      const std::u8string name = std::filesystem::path(std::u8string(clip->filepath.begin(), clip->filepath.end()))
                                     .filename()
                                     .u8string();
      ImGui::TextUnformatted(std::string(name.begin(), name.end()).c_str());
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("%s", clip->filepath.c_str());
      }
      if (missing) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0xff, 0x70, 0x70, 255));
        ImGui::TextWrapped("Missing: the file isn't where it was");
        ImGui::PopStyleColor();
        if (ImGui::Button("Locate...", ImVec2(-FLT_MIN, 0))) {
          operator_call(C, "media.locate", {{"layer", id}, {"clip", std::to_string(clip->id)}});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("Point it at where the file is now; other missing files in that folder are found too");
        }
      }
      else {
        ImGui::TextDisabled("%s%s", media_info_text(info).c_str(), clip->packed.empty() ? "" : " (packed)");
      }
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    const float third = (ImGui::GetContentRegionAvail().x - spacing * 2.0f) / 3.0f;
    if (ImGui::Button("Replace...", ImVec2(third, 0))) {
      operator_call(C, "media.replace", {{"layer", id}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show another file in this clip, keeping its timing");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(third, 0))) {
      operator_call(C, "media.reload", {{"layer", id}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Read the files again, after changing them in another program");
    }
    ImGui::SameLine();
    const bool packed = clip && !clip->packed.empty();
    if (ImGui::Button(packed ? "Unpack" : "Pack", ImVec2(-FLT_MIN, 0))) {
      operator_call(C, packed ? "media.unpack" : "media.pack", {{"layer", id}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(packed ? "Use the files on disk again instead of the copies inside the .oanim"
                               : "Keep a copy of the files inside the .oanim, so it opens anywhere (a bigger file)");
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Button("Replace from Current Frame...", ImVec2(-FLT_MIN, 0))) {
      operator_call(C, "media.replace_from_frame", {{"layer", id}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show another file from this frame on: this clip ends here and a new one starts");
    }
    ui_panel_end();
  }

  if (clip && ui_panel_begin("Clip")) {
    const MediaInfo info = media_clip_info(*clip);
    ui_row("Start", [&](float w) {
      ImGui::SetNextItemWidth(w);
      ui_drag_int("##start", &clip->start, 0.25f, 1, 1048574, "%d", ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        media_clip_changed(C, *layer, *clip, "Move Clip");
      }
    });
    ui_row("Length", [&](float w) {
      ImGui::SetNextItemWidth(w);
      ui_drag_int("##length", &clip->length, 0.25f, 1, 1048574, "%d frames", ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        media_clip_changed(C, *layer, *clip, "Trim Clip");
      }
    });
    if (info.kind != MediaKind::Image) {
      ui_row("In", [&](float w) {
        ImGui::SetNextItemWidth(w);
        float in = float(clip->in);
        const double limit = std::max(info.duration, 0.0);
        if (ui_drag_float("##in", &in, 0.01f, 0.0f, float(limit), "%.2f s into the file",
                             ImGuiSliderFlags_AlwaysClamp))
        {
          clip->in = double(in);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          media_clip_changed(C, *layer, *clip, "Slip Clip");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("Where in the file the clip starts (Alt-drag the clip in the timeline to slip it)");
        }
      });
      ui_row("After End", [&](float w) {
        static const char *const ends[] = {"Hold Last Frame", "Loop", "Hide"};
        int end = int(clip->end);
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##end", &end, ends, 3)) {
          clip->end = MediaEnd(end);
          media_clip_changed(C, *layer, *clip, "Change Clip");
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("What the clip shows once its file runs out, until its Length ends");
        }
      });
    }
    if (info.kind == MediaKind::Video) {
      ui_row("Timing", [&](float w) {
        static const char *const timings[] = {"Real Time", "Frame by Frame"};
        int timing = int(clip->timing);
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo("##timing", &timing, timings, 2)) {
          /* Keep it covering the same part of the file: its length follows the new speed. */
          const double before = media_clip_file_frames(doc, *clip);
          clip->timing = MediaTiming(timing);
          const double after = media_clip_file_frames(doc, *clip);
          if (before > 0.0) {
            clip->length = std::max(1, int(std::lround(double(clip->length) * after / before)));
          }
          media_clip_changed(C, *layer, *clip, "Change Clip Timing");
        }
      });
    }
    const std::string timing = media_timing_text(doc, *clip, info);
    if (!timing.empty()) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextDisabled("%s", timing.c_str());
    }
    ui_panel_end();
  }

  if (ui_panel_begin("Look")) {
    ui_prop(C, ptr, "media_guide", path);
    ui_prop(C, ptr, "media_fit", path);
    key_next(C, *layer, {"opacity"});
    ui_prop(C, ptr, "opacity", path);
    ui_prop(C, ptr, "media_greyscale", path);
    float rgba[4] = {layer->media.tint.r, layer->media.tint.g, layer->media.tint.b, 1.0f};
    ui_driver_next(layer->id, "media.tint");
    if (ui_color(C, "Tint Color", rgba, "Change Tint Color")) {
      layer->media.tint = {rgba[0], rgba[1], rgba[2], 1.0f};
    }
    ui_prop(C, ptr, "media_tint_amount", path);
    ui_prop(C, ptr, "media_in_front", path);
    ui_prop(C, ptr, "media_dont_auto_key", path);
    ui_panel_end();
  }

  const bool sound = std::any_of(layer->media.clips.begin(), layer->media.clips.end(),
                                 [](const MediaClip &c) { return media_clip_info(c).has_audio; });
  if (sound && ui_panel_begin("Sound")) {
    ui_prop(C, ptr, "media_volume", path);
    ui_prop(C, ptr, "media_muted", path);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    ImGui::TextDisabled("Playback > Play Audio and Scrubbing (timeline)");
    ui_panel_end();
  }
}

static void shape_panel(Context &C, Layer &owner_layer, VectorDrawing &drawing, Shape &shape);
static void shared_mark(ImDrawList *dl, ImVec2 corner, float s);
static void point_panel(Context &C, VectorDrawing &drawing);
static void edge_panel(Context &C, VectorDrawing &drawing);

/* `driver_layer`, `driver_path`: what drives the row's X and Y (a shape's "shapes[<id>].position"), if
 * it can be driven. */
static void mean_position_row(Context &C, const char *label, VectorDrawing &drawing, const std::vector<int> &points,
                              const char *tooltip, const char *undo_name, bool posed = false,
                              uint64_t driver_layer = 0, const std::string &driver_path = std::string());

/* Blender's factor slider: a bar filled as far as its value (0-1), the number in its middle; click or
 * drag to set it. True while it changes. */
static bool factor_slider(const char *id, float *value)
{
  return ui_slider_float(id, value, 0.0f, 1.0f, "%.3f");
}

/* Blender's vertex group icon: a little grid of points, one corner lit. */
static void icon_point_group(ImDrawList *dl, ImVec2 c, float s)
{
  for (int y = -1; y <= 1; y++) {
    for (int x = -1; x <= 1; x++) {
      const bool lit = x == -1 && y == -1;
      dl->AddRectFilled(ImVec2(c.x + x * s * 0.62f - s * 0.2f, c.y + y * s * 0.62f - s * 0.2f),
                        ImVec2(c.x + x * s * 0.62f + s * 0.2f, c.y + y * s * 0.62f + s * 0.2f),
                        lit ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0x9a, 0x9a, 0x9a, 255));
    }
  }
}

/* A padlock, shut when `locked` (the Layers editor's). */
static void icon_padlock(ImDrawList *dl, ImVec2 c, float s, bool locked)
{
  const ImU32 col = locked ? COL_MOD_ICON : IM_COL32(0x8a, 0x8a, 0x8a, 255);
  dl->AddRectFilled(c + ImVec2(-s * 0.6f, -s * 0.1f), c + ImVec2(s * 0.6f, s * 0.75f), col, 1.0f);
  const float shift = locked ? 0.0f : s * 0.45f;
  dl->PathArcTo(c + ImVec2(shift, -s * 0.15f), s * 0.38f, PI, 2.0f * PI, 8);
  dl->PathStroke(col, 0, 1.3f);
}

/* Point Groups, laid out as Blender's Vertex Groups: the list (each row the group's icon, its name and
 * its lock; the active one lit; double-click a name to rename it; under it, the filter's arrow and the
 * grip that resizes it), beside it + and -, the specials menu and the up and down arrows; then Assign |
 * Remove and Select | Deselect, and the Weight Assign gives. */
static void point_groups_panel(Context &C, VectorDrawing &drawing)
{
  if (!ui_collapsing_header("Point Groups", true)) {
    return;
  }
  const ImGuiStyle &style = ImGui::GetStyle();
  const float h = ImGui::GetFrameHeight();
  const int n = int(drawing.groups.size());
  const int active = n ? std::clamp(drawing.active_group, 0, n - 1) : -1;
  /* The group being renamed and its name as typed; the list's height (rows) and its filter, kept
   * while Open Animate runs. */
  static int renaming = -1;
  static std::string typed;
  static float rows = 5.0f;
  static bool filter_open = false, sort_by_name = false;
  static std::string filter;

  const float row_h = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
  const float bar_h = h * 0.75f;
  const float list_w = ImGui::GetContentRegionAvail().x - h - 6.0f;
  const ImVec2 box_min = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->ChannelsSplit(2);
  dl->ChannelsSetCurrent(1);
  ImGui::BeginGroup();
  /* The rows, filtered (any part of the name, any case) and, with the filter's sort, by name. */
  std::vector<int> order;
  std::string needle = filter;
  std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
  for (int i = 0; i < n; i++) {
    if (needle.empty() || search_matches(drawing.groups[size_t(i)].name, needle)) {
      order.push_back(i);
    }
  }
  if (sort_by_name) {
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return drawing.groups[size_t(a)].name < drawing.groups[size_t(b)].name; });
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  if (ImGui::BeginChild("##point_groups", ImVec2(list_w, rows * row_h + 4.0f), ImGuiChildFlags_None)) {
    /* The rows' icons and names go on the list's own drawing, over its highlight. */
    ImDrawList *list_dl = ImGui::GetWindowDrawList();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 2.0f));
    ImGui::SetCursorPos(ImVec2(4.0f, 4.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    for (const int i : order) {
      const PointGroup &g = drawing.groups[size_t(i)];
      ImGui::PushID(i);
      ImGui::SetCursorPosX(4.0f);
      const ImVec2 r0 = ImGui::GetCursorScreenPos();
      const float row_w = ImGui::GetContentRegionAvail().x - 4.0f;
      const float icon = row_h * 0.32f;
      if (renaming == i) {
        ImGui::SetCursorScreenPos(ImVec2(r0.x + row_h, r0.y));
        ImGui::SetNextItemWidth(row_w - row_h * 2.0f);
        if (!ImGui::IsAnyItemActive()) {
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("##rename", &typed, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivated()) {
          if (!ImGui::IsKeyPressed(ImGuiKey_Escape) && typed != g.name) {
            operator_call(C, "mesh.point_group_set", {{"index", std::to_string(i)}, {"name", typed}});
          }
          renaming = -1;
        }
        icon_point_group(list_dl, ImVec2(r0.x + row_h * 0.5f, r0.y + row_h * 0.5f), icon);
      }
      else {
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0x47, 0x72, 0xb3, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0x3c, 0x3c, 0x3c, 255));
        const bool clicked = ImGui::Selectable("##group", i == active,
                                               ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap,
                                               ImVec2(row_w, row_h - 2.0f));
        ImGui::PopStyleColor(2);
        if (clicked) {
          if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            renaming = i;
            typed = g.name;
          }
          else if (i != active) {
            operator_call(C, "mesh.point_group_set", {{"index", std::to_string(i)}, {"active", "1"}});
          }
        }
        const bool row_hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
        icon_point_group(list_dl, ImVec2(r0.x + row_h * 0.5f, r0.y + (row_h - 2.0f) * 0.5f), icon);
        list_dl->AddText(ImVec2(r0.x + row_h + 2.0f, r0.y + (row_h - 2.0f - ImGui::GetTextLineHeight()) * 0.5f),
                         ImGui::GetColorU32(ImGuiCol_Text), g.name.c_str());
        /* The lock, at the row's right: click it to lock or unlock the group. */
        const ImVec2 lock_at(r0.x + row_w - row_h, r0.y);
        ImGui::SetCursorScreenPos(lock_at);
        if (ImGui::InvisibleButton("##lock", ImVec2(row_h, row_h - 2.0f))) {
          operator_call(C, "mesh.point_group_set", {{"index", std::to_string(i)}, {"lock", g.locked ? "0" : "1"}});
        }
        const bool lock_hovered = ImGui::IsItemHovered();
        icon_padlock(list_dl, ImVec2(lock_at.x + row_h * 0.5f, lock_at.y + (row_h - 2.0f) * 0.5f), row_h * 0.3f,
                     g.locked);
        if (lock_hovered) {
          ImGui::SetTooltip(g.locked ? "Locked: Assign and Remove leave it alone. Click to unlock it"
                                     : "Lock it, so Assign and Remove leave it alone");
        }
        else if (row_hovered) {
          ImGui::SetTooltip("%s: %d points. Click to make it the active group, double-click to rename it",
                            g.name.c_str(), int(g.weights.size()));
        }
      }
      ImGui::PopID();
    }
    ImGui::PopStyleVar();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
  /* Under the list: the filter's arrow at the left, the grip that resizes the list in the middle. */
  const ImVec2 bar = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("##filter", ImVec2(bar_h, bar_h))) {
    filter_open = !filter_open;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Filter the list");
  }
  ImGui::RenderArrow(dl, ImVec2(bar.x + bar_h * 0.25f, bar.y + bar_h * 0.15f), IM_COL32(0xb0, 0xb0, 0xb0, 255),
                     filter_open ? ImGuiDir_Down : ImGuiDir_Right, 0.6f);
  ImGui::SameLine(0, 0);
  ImGui::SetCursorScreenPos(ImVec2(bar.x + list_w * 0.5f - h * 0.5f, bar.y));
  ImGui::InvisibleButton("##grip", ImVec2(h, bar_h));
  const bool grip = ImGui::IsItemHovered() || ImGui::IsItemActive();
  if (ImGui::IsItemActive()) {
    rows = std::clamp(rows + ImGui::GetIO().MouseDelta.y / row_h, 2.0f, 30.0f);
  }
  if (grip) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
  }
  for (int k = 0; k < 3; k++) {
    for (int r = 0; r < 2; r++) {
      dl->AddCircleFilled(ImVec2(bar.x + list_w * 0.5f - h * 0.18f + k * h * 0.18f, bar.y + bar_h * 0.38f + r * h * 0.16f),
                          1.1f, grip ? COL_MOD_ICON : IM_COL32(0x80, 0x80, 0x80, 255));
    }
  }
  if (filter_open) {
    ImGui::SetCursorScreenPos(ImVec2(bar.x + 4.0f, bar.y + bar_h));
    ImGui::SetNextItemWidth(list_w - h * 2.0f - 12.0f);
    ImGui::InputTextWithHint("##filter_text", "Filter by name", &filter);
    ImGui::SameLine(0, 2);
    if (sort_by_name) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button("A-Z", ImVec2(h * 2.0f, 0))) {
      sort_by_name = !sort_by_name;
    }
    if (sort_by_name) {
      ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show the list in order of names (Sort by Name in the menu puts them in that order)");
    }
  }
  ImGui::EndGroup();
  const ImVec2 box_max(box_min.x + list_w, ImGui::GetItemRectMax().y + 3.0f);
  dl->ChannelsSetCurrent(0);
  dl->AddRectFilled(box_min, box_max, IM_COL32(0x28, 0x28, 0x28, 255), 4.0f);
  dl->AddRect(box_min, box_max, IM_COL32(0x20, 0x20, 0x20, 255), 4.0f);
  dl->ChannelsMerge();

  /* Beside it: +, -, the menu, then up and down, as Blender's. */
  ImGui::SetCursorScreenPos(ImVec2(box_min.x + list_w + 6.0f, box_min.y));
  ImGui::BeginGroup();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 1.0f));
  if (ImGui::Button("+##add_group", ImVec2(h, h))) {
    operator_call(C, "mesh.point_group_add");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Add a point group");
  }
  ImGui::BeginDisabled(n == 0);
  if (ImGui::Button("-##remove_group", ImVec2(h, h))) {
    operator_call(C, "mesh.point_group_remove");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Remove the active point group (its points stay)");
  }
  ImGui::Dummy(ImVec2(h, h * 0.3f));
  const ImVec2 menu_at = ImGui::GetCursorScreenPos();
  if (ImGui::Button("##group_menu", ImVec2(h, h))) {
    ImGui::OpenPopup("##point_group_menu");
  }
  ImGui::RenderArrow(dl, ImVec2(menu_at.x + h * 0.25f, menu_at.y + h * 0.25f), COL_MOD_ICON, ImGuiDir_Down, 0.75f);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Point group specials");
  }
  ImGui::Dummy(ImVec2(h, h * 0.3f));
  const ImVec2 up_at = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(active <= 0);
  if (ImGui::Button("##group_up", ImVec2(h, h))) {
    operator_call(C, "mesh.point_group_move", {{"direction", "-1"}});
  }
  ImGui::EndDisabled();
  ImGui::RenderArrow(dl, ImVec2(up_at.x + h * 0.25f, up_at.y + h * 0.22f), COL_MOD_ICON, ImGuiDir_Up, 0.75f);
  const ImVec2 down_at = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(active < 0 || active >= n - 1);
  if (ImGui::Button("##group_down", ImVec2(h, h))) {
    operator_call(C, "mesh.point_group_move", {{"direction", "1"}});
  }
  ImGui::EndDisabled();
  ImGui::RenderArrow(dl, ImVec2(down_at.x + h * 0.25f, down_at.y + h * 0.28f), COL_MOD_ICON, ImGuiDir_Down, 0.75f);
  ImGui::EndDisabled();
  ImGui::PopStyleVar();
  ImGui::EndGroup();
  if (ImGui::BeginPopup("##point_group_menu")) {
    const bool any_selected = std::any_of(drawing.points.begin(), drawing.points.end(),
                                          [](const MeshPoint &p) { return p.selected; });
    if (ImGui::MenuItem("Sort by Name")) {
      operator_call(C, "mesh.point_group_sort");
    }
    if (ImGui::MenuItem("Copy Point Group")) {
      operator_call(C, "mesh.point_group_copy");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Remove from All Groups", nullptr, false, any_selected)) {
      operator_call(C, "mesh.point_group_unassign_all");
    }
    if (ImGui::MenuItem("Delete All Unlocked Groups")) {
      operator_call(C, "mesh.point_group_remove_all", {{"unlocked", "1"}});
    }
    if (ImGui::MenuItem("Delete All Groups")) {
      operator_call(C, "mesh.point_group_remove_all");
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Lock All")) {
      operator_call(C, "mesh.point_group_lock_all", {{"lock", "1"}});
    }
    if (ImGui::MenuItem("Unlock All")) {
      operator_call(C, "mesh.point_group_lock_all", {{"lock", "0"}});
    }
    ImGui::EndPopup();
  }
  ImGui::SetCursorScreenPos(ImVec2(box_min.x, std::max(box_max.y, ImGui::GetItemRectMax().y) + style.ItemSpacing.y));
  ImGui::Dummy(ImVec2(0.0f, 0.0f));
  if (n == 0) {
    return;
  }

  /* Assign | Remove, then Select | Deselect: two joined pairs on one row, as Blender's. */
  static float weight = 1.0f;
  const bool any_selected = std::any_of(drawing.points.begin(), drawing.points.end(),
                                        [](const MeshPoint &p) { return p.selected; });
  const bool locked = drawing.groups[size_t(active)].locked;
  const float gap = 8.0f;
  const float w = (ImGui::GetContentRegionAvail().x - gap - 2.0f) * 0.25f;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, style.ItemSpacing.y));
  ImGui::BeginDisabled(!any_selected || locked);
  if (ImGui::Button("Assign", ImVec2(w, 0))) {
    char text[32];
    std::snprintf(text, sizeof(text), "%g", double(weight));
    operator_call(C, "mesh.point_group_assign", {{"weight", text}});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(locked ? "The active group is locked" : any_selected ? "Put the selected points in the active group, with the Weight below"
                                                                          : "Select points to put them in the group");
  }
  ImGui::SameLine();
  if (ImGui::Button("Remove", ImVec2(w, 0))) {
    operator_call(C, "mesh.point_group_unassign");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(locked ? "The active group is locked" : "Take the selected points out of the active group");
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0, gap);
  if (ImGui::Button("Select", ImVec2(w, 0))) {
    operator_call(C, "mesh.point_group_select");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Select the active group's points (adding to the selection)");
  }
  ImGui::SameLine();
  if (ImGui::Button("Deselect", ImVec2(w, 0))) {
    operator_call(C, "mesh.point_group_deselect");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Deselect the active group's points");
  }
  ImGui::PopStyleVar();
  /* Weight: a label on the left, Blender's filled bar on the right. */
  ImGui::Spacing();
  const float column = ImGui::GetContentRegionAvail().x * 0.4f;
  const float label_w = ImGui::CalcTextSize("Weight").x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + column - label_w - style.ItemSpacing.x);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Weight");
  ImGui::SameLine(0, style.ItemSpacing.x);
  ImGui::SetNextItemWidth(-1);
  factor_slider("##weight", &weight);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("The weight Assign gives the points (0-1): how much a modifier limited to the group does to "
                      "them. Shift drags finely");
  }
}

static void drawing_tab(Context &C, bool shapes_tab)
{
  Document &doc = *C.document;
  Layer *active = document_active_layer(doc);
  if (!active || !active->has_drawing()) {
    ImGui::TextDisabled("Select a vector layer to see its shapes.");
    return;
  }
  Layer *layer = nullptr;
  VectorDrawing *drawing = editable_drawing(C, &layer);
  const VectorDrawing &shown = layer_drawing(doc, *active);
  if (!shapes_tab) {
    ImGui::TextDisabled("%d points, %d curves, %d shapes", int(shown.points.size()), int(shown.curves.size()),
                        int(shown.shapes.size()));
  }
  if (!drawing) {
    ImGui::TextDisabled("The layer is locked.");
    return;
  }
  /* A reference (a copy): its looks change here as on any layer; changes show teal. */
  const bool reference = active->is_reference();
  if (!shapes_tab) {
    if (reference) {
      /* Only where its points are (with Points: Editable); widths, corners and handle types follow the
       * original. */
      std::vector<int> picked;
      bool moved = false;
      for (int i = 0; i < int(drawing->points.size()); i++) {
        if (drawing->points[i].selected) {
          picked.push_back(i);
          const uint64_t id = drawing->points[i].id;
          moved |= active->reference.points.count(id) > 0 ||
                   std::any_of(active->reference.handles.begin(), active->reference.handles.end(),
                               [&](const auto &e) { return e.first.point == id; });
        }
      }
      if (active->reference.edit_points && !picked.empty() && ui_panel_begin("Selected Points")) {
        const bool on = changed_begin(moved);
        mean_position_row(C, "Mean Position", *drawing, picked, "The middle of the selected points; changing it moves them all",
                          "Move Points");
        changed_end(C, on, "##changed_points", {{{"kind", "SELECTED"}}}, {{{"kind", "SELECTED"}}});
        ui_panel_end();
      }
      /* Showing or hiding edges is the reference's own, whatever its points' lock. */
      edge_panel(C, *drawing);
      return;
    }
    point_groups_panel(C, *drawing);
    point_panel(C, *drawing);
    edge_panel(C, *drawing);
    return;
  }

  /* Shapes of every picked layer being edited (multi-layer editing), each layer's top first like the
   * layer list, under its name when there are several. The highlight follows the viewport: a shape
   * is selected when any of its points is, and clicking one here selects its points. Dragging a
   * shape onto another of its layer moves it to that place in the stacking order. */
  std::vector<Layer *> listed = editable_layers(C);
  std::erase_if(listed, [](const Layer *l) { return !l->has_drawing(); });
  struct ShapeRef {
    int layer = -1;
    int shape = -1;
  };
  ShapeRef move_from, move_to;
  /* A shape clicked in the list, picked after the list is drawn: the undo step it makes rebuilds a
   * reference's drawing, which would pull the shapes out from under the rows still being drawn. */
  ShapeRef clicked;
  bool clicked_adds = false;
  /* A shape deleted from its row's right-click menu (its layer and id), after the list. */
  uint64_t delete_layer = 0, delete_shape = 0;
  uint64_t make_active = 0;
  /* The shape the panels below show: the active shape (the last clicked) while it's selected, else the
   * topmost selected one, in the first listed layer that has one (the active layer comes first). Its
   * row is marked as the active one. */
  ShapeRef shown_shape;
  for (int li = 0; li < int(listed.size()) && shown_shape.layer < 0; li++) {
    if (listed[size_t(li)]->id != doc.active_shape_layer) {
      continue;
    }
    const VectorDrawing &d = listed[size_t(li)]->drawing;
    const int s = drawing_shape_index(d, doc.active_shape);
    if (s >= 0 && drawing_selected_shapes(d)[size_t(s)]) {
      shown_shape = {li, s};
    }
  }
  for (int li = 0; li < int(listed.size()) && shown_shape.layer < 0; li++) {
    const std::vector<bool> picked_shapes = drawing_selected_shapes(listed[size_t(li)]->drawing);
    for (int s = 0; s < int(picked_shapes.size()); s++) {
      if (picked_shapes[s]) {
        shown_shape = {li, s};
      }
    }
  }
  if (ui_collapsing_header("Shapes", true)) {
    const bool several = listed.size() > 1;
    int rows = 0;
    for (const Layer *l : listed) {
      rows += int(l->drawing.shapes.size()) + (several ? 1 : 0);
    }
    const float list_h = std::min(float(rows) + 1.0f, several ? 14.0f : 10.0f) * ImGui::GetFrameHeight();
    if (ImGui::BeginChild("##shapes", ImVec2(0, list_h), ImGuiChildFlags_Borders)) {
      for (int li = 0; li < int(listed.size()); li++) {
        Layer &owner = *listed[size_t(li)];
        VectorDrawing &shapes_of = owner.drawing;
        const std::vector<bool> picked_shapes = drawing_selected_shapes(shapes_of);
        ImGui::PushID(int(owner.id));
        if (several) {
          /* The layer's name: clicking it makes it the active layer. */
          const bool is_active = owner.id == doc.active_layer;
          ImGui::PushStyleColor(ImGuiCol_Text, is_active ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xa0, 0xa0, 0xa0, 255));
          if (ImGui::Selectable((owner.name + (is_active ? "  (active)" : "") + "##layer").c_str(), false,
                                ImGuiSelectableFlags_None, ImVec2(0, ImGui::GetTextLineHeight())))
          {
            make_active = owner.id;
          }
          ImGui::PopStyleColor();
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("Click: make %s the active layer", owner.name.c_str());
          }
          ImGui::Indent(ImGui::GetTextLineHeight());
        }
        for (int s = int(shapes_of.shapes.size()) - 1; s >= 0; s--) {
          Shape &shape = shapes_of.shapes[s];
          ImGui::PushID(s);
          const ImVec2 p = ImGui::GetCursorScreenPos();
          const float sw = ImGui::GetTextLineHeight();
          ImDrawList *dl = ImGui::GetWindowDrawList();
          if (ImGui::Selectable("##shape", picked_shapes[s], 0, ImVec2(0, ImGui::GetTextLineHeight()))) {
            clicked = {li, s};
            /* Shift or Ctrl adds to the selection, as in the viewport. */
            clicked_adds = ImGui::GetIO().KeyShift || ImGui::GetIO().KeyCtrl;
          }
          const bool is_shown = li == shown_shape.layer && s == shown_shape.shape;
          if (is_shown) {
            /* The one the panels below show: lighter, with a bar on its left (as the active layer's
             * row stands out from the other picked ones). */
            const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            dl->AddRectFilled(r0, r1, IM_COL32(0x47, 0x72, 0xb3, 255), 2.0f);
            dl->AddRectFilled(r0, ImVec2(r0.x + 3.0f, r1.y), IM_COL32(0xff, 0xff, 0xff, 255));
          }
          if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip(is_shown ? "Shown in the Shape and Material panels below (right-click: Delete)"
                                       : "Click: select it (Shift adds); right-click: Delete");
          }
          /* Right-click: delete just this shape (its curve and points stay for the others on them). */
          if (ImGui::BeginPopupContextItem("##shape_menu")) {
            ImGui::TextDisabled("%s", shape.name.empty() ? "Shape" : shape.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Delete Shape", nullptr, false, !owner.is_reference())) {
              delete_layer = owner.id;
              delete_shape = shape.id;
            }
            if (owner.is_reference() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
              ImGui::SetTooltip("A copy keeps its original's shapes: hide them in the Reference tab");
            }
            ImGui::EndPopup();
          }
          if (ImGui::BeginDragDropSource()) {
            const int payload[2] = {li, s};
            ImGui::SetDragDropPayload("OANIM_SHAPE", payload, sizeof(payload));
            ImGui::TextUnformatted(shape.name.empty() ? "Shape" : shape.name.c_str());
            ImGui::EndDragDropSource();
          }
          if (ImGui::BeginDragDropTarget()) {
            /* The list is top first: the upper half drops above this shape (stacked over it). Only
             * within its own layer. */
            const ImGuiPayload *peek = ImGui::GetDragDropPayload();
            const bool same_layer = peek && peek->IsDataType("OANIM_SHAPE") && static_cast<const int *>(peek->Data)[0] == li;
            const ImVec2 row_min = ImGui::GetItemRectMin(), row_max = ImGui::GetItemRectMax();
            const bool upper = ImGui::GetIO().MousePos.y < (row_min.y + row_max.y) * 0.5f;
            if (same_layer) {
              const float line_y = upper ? row_min.y : row_max.y;
              ImGui::GetWindowDrawList()->AddLine(ImVec2(row_min.x, line_y), ImVec2(row_max.x, line_y),
                                                  UI_DROP_LINE_COLOR, 2.0f);
              if (const ImGuiPayload *payload =
                      ImGui::AcceptDragDropPayload("OANIM_SHAPE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect))
              {
                move_from = {li, static_cast<const int *>(payload->Data)[1]};
                /* Where it goes in the stacking order (bottom first), counted before it is taken out. */
                move_to = {li, upper ? s + 1 : s};
              }
            }
            ImGui::EndDragDropTarget();
          }
          /* Fill and line swatches, then the name. */
          const auto swatch = [&](float x, const Color &c, bool on) {
            const ImVec2 sa = p + ImVec2(x, 1), sb = sa + ImVec2(sw - 2, sw - 2);
            if (on) {
              dl->AddRectFilled(sa, sb, ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)), 2.0f);
            }
            dl->AddRect(sa, sb, IM_COL32(0x80, 0x80, 0x80, 255), 2.0f);
          };
          swatch(2.0f, shape.style.fill, shape.has_fill);
          if (!owner.is_reference() && material_find(doc, shape.material)) {
            /* Linked to a shared material: its mark. */
            shared_mark(dl, p + ImVec2(sw, sw), sw * 0.5f);
          }
          swatch(sw + 4.0f, shape.style.line, shape.has_outline);
          dl->AddText(p + ImVec2(sw * 2.0f + 10.0f, 0), is_shown ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xe6, 0xe6, 0xe6, 255),
                      shape.name.c_str());
          ImGui::PopID();
        }
        if (shapes_of.shapes.empty()) {
          ImGui::TextDisabled(several ? "No shapes" : "No shapes yet. Draw, or select points and press F.");
        }
        if (several) {
          ImGui::Unindent(ImGui::GetTextLineHeight());
        }
        ImGui::PopID();
      }
      if (listed.empty()) {
        ImGui::TextDisabled("The layer is locked.");
      }
    }
    ImGui::EndChild();
  }
  if (delete_shape != 0) {
    operator_call(C, "mesh.delete_shapes",
                  {{"layer", std::to_string(delete_layer)}, {"shapes", std::to_string(delete_shape)}});
    return;
  }
  if (make_active != 0) {
    /* The layer becomes active; the others stay picked. */
    const uint64_t previous = doc.active_layer;
    if (Layer *was = layer_find(doc, previous); was && previous != make_active) {
      was->selected = true;
    }
    doc.active_layer = make_active;
    C.undo->push("Select", doc);
    return;
  }
  if (clicked.layer >= 0 && clicked.layer < int(listed.size())) {
    VectorDrawing &d = listed[size_t(clicked.layer)]->drawing;
    if (clicked.shape < int(d.shapes.size())) {
      if (!clicked_adds) {
        for (Layer *l : listed) {
          for (MeshPoint &pt : l->drawing.points) {
            pt.selected = false;
          }
        }
      }
      for (const ShapeEdge &e : d.shapes[clicked.shape].edges) {
        int pa, pb;
        curve_segment_points(d.curves[e.curve], e.segment, &pa, &pb);
        d.points[pa].selected = d.points[pb].selected = true;
      }
      doc.active_shape_layer = listed[size_t(clicked.layer)]->id;
      doc.active_shape = d.shapes[clicked.shape].id;
      C.undo->push("Select Shape", doc);
    }
  }
  /* After the list, so its rows weren't shifting while it drew. A reference keeps its original's
   * stacking order. */
  if (move_from.layer >= 0 && move_from.layer == move_to.layer && move_from.layer < int(listed.size()) &&
      !listed[size_t(move_from.layer)]->is_reference())
  {
    VectorDrawing &d = listed[size_t(move_from.layer)]->drawing;
    const int from = move_from.shape;
    const int to = move_to.shape > from ? move_to.shape - 1 : move_to.shape;
    if (to != from && from >= 0 && from < int(d.shapes.size())) {
      Shape moved = std::move(d.shapes[from]);
      d.shapes.erase(d.shapes.begin() + from);
      d.shapes.insert(d.shapes.begin() + std::clamp(to, 0, int(d.shapes.size())), std::move(moved));
      C.undo->push("Move Shape", doc);
      return;
    }
  }
  if (shown_shape.layer >= 0) {
    Layer &l = *listed[size_t(shown_shape.layer)];
    if (shown_shape.shape < int(l.drawing.shapes.size())) {
      shape_panel(C, l, l.drawing, l.drawing.shapes[size_t(shown_shape.shape)]);
    }
  }
}

/* The topmost selected shape's name, fill, outline and line width. */
/* One handle shown in the Point panel. */
struct HandleRow {
  CurvePoint *cp;
  bool in;
};

/* The handles the Point panel shows: one picked on its own, or else the selected point's (one point,
 * on each curve it is on; not the In of an open curve's first point nor the Out of its last). */
static std::vector<HandleRow> point_panel_handles(VectorDrawing &drawing)
{
  std::vector<HandleRow> lone;
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      for (const bool in : {true, false}) {
        if (cp.handles_set && (in ? cp.select_in : cp.select_out)) {
          lone.push_back({&cp, in});
        }
      }
    }
  }
  if (!lone.empty()) {
    return lone.size() == 1 ? lone : std::vector<HandleRow>{};
  }
  const std::vector<int> picked = drawing_selected_points(drawing);
  if (picked.size() != 1) {
    return {};
  }
  std::vector<HandleRow> rows;
  for (Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    for (int i = 0; i < n; i++) {
      CurvePoint &cp = curve.points[i];
      if (cp.point != picked[0] || !cp.handles_set) {
        continue;
      }
      if (curve.closed || i > 0) {
        rows.push_back({&cp, true});
      }
      if (curve.closed || i < n - 1) {
        rows.push_back({&cp, false});
      }
    }
  }
  return rows;
}

/* "Handle In  [Angle 135.0°] [Length 0.120]": a handle's direction (degrees, counter-clockwise from
 * the layer's x axis) and length, as Moho and Illustrator give them. Away from frame 0, how far it has
 * turned and grown from the setup pose instead (what its keys hold), 0 at rest. An aligned partner
 * turns with it. `suffix` tells the curves apart when the point is on several. */
static void handle_row(Context &C, const HandleRow &row, const char *suffix)
{
  CurvePoint &cp = *row.cp;
  float2 &handle = row.in ? cp.handle_in : cp.handle_out;
  const bool posed = C.document->frame_current != 0;
  const float2 setup = handle_setup(cp, row.in);
  float2 value = posed ? handle_change(setup, handle, (row.in ? cp.change_in : cp.change_out).x) :
                         float2(std::atan2(handle.y, handle.x), length(handle));
  const std::string label = std::string(row.in ? "Handle In" : "Handle Out") + suffix;
  ImGui::PushID(label.c_str());
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label.c_str());
  ImGui::TableSetColumnIndex(1);
  const float w = (ImGui::GetContentRegionAvail().x - 2.0f) * 0.5f;
  float degrees = value.x * 180.0f / PI;
  bool edited = false, done = false;
  ImGui::SetNextItemWidth(w);
  if (ui_drag_float("##angle", &degrees, 0.5f, -36000.0f, 36000.0f, "Angle  %.1f\xC2\xB0")) {
    value.x = degrees * PI / 180.0f;
    edited = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(posed ? "How far the handle has turned from the setup pose (frame 0)" :
                              "Which way the handle points: degrees, counter-clockwise from the layer's x axis");
  }
  done |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SameLine(0.0f, 2.0f);
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (posed ? ui_drag_float("##length", &value.y, 0.002f, -10000.0f, 10000.0f, "Length  %+.3f") :
              ui_drag_float("##length", &value.y, 0.002f, 0.0f, 10000.0f, "Length  %.3f",
                               ImGuiSliderFlags_AlwaysClamp))
  {
    edited = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(posed ? "How much longer the handle is than in the setup pose (frame 0)" :
                              "How far the handle reaches from its point: longer bends the curve more fully");
  }
  done |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::PopID();
  if (edited) {
    const float2 before = handle;
    handle = posed ? handle_from_change(setup, value) : float2(std::cos(value.x), std::sin(value.x)) * value.y;
    if (cp.aligned && length(before) > 1e-9f && length(handle) > 1e-9f) {
      /* Its partner stays in line: it turns by as much. */
      float2 &other = row.in ? cp.handle_out : cp.handle_in;
      other = rotate(other, std::atan2(handle.y, handle.x) - std::atan2(before.y, before.x));
    }
  }
  if (done) {
    C.undo->push("Move Handle", *C.document);
  }
}

static void handle_rows(Context &C, VectorDrawing &drawing)
{
  const std::vector<HandleRow> rows = point_panel_handles(drawing);
  /* The point on two curves (welded): the second curve's handles are told apart. */
  const CurvePoint *first = rows.empty() ? nullptr : rows[0].cp;
  int curve = 1;
  for (size_t i = 0; i < rows.size(); i++) {
    if (i > 0 && rows[i].cp != rows[i - 1].cp) {
      curve++;
    }
    const std::string suffix = (rows.back().cp == first) ? "" : " (" + std::to_string(curve) + ")";
    handle_row(C, rows[i], suffix.c_str());
  }
}

/* "Label  [X 0.000] [Y 0.000]": the average position of some points; dragging moves them all
 * (layer space). `posed`: away from frame 0 it is their Transform, the average change from the setup
 * pose (what their keys hold), (0, 0) at rest. */
static void mean_position_row(Context &C, const char *label, VectorDrawing &drawing, const std::vector<int> &points,
                              const char *tooltip, const char *undo_name, bool posed, uint64_t driver_layer,
                              const std::string &driver_path)
{
  float2 mean;
  if (!drawing_points_mean(drawing, points, &mean)) {
    return;
  }
  if (posed && C.document->frame_current != 0) {
    float2 setup;
    for (const int i : points) {
      setup += point_setup_pos(drawing.points[i]);
    }
    mean -= setup / float(points.size());
    label = "Transform";
    tooltip = "How far they have moved from the setup pose (frame 0), on average: what their keys hold. Changing it "
              "moves them all";
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
    const int driven = ui_driver_field_begin(C, driver_layer, driver_path, axis);
    ImGui::SetNextItemWidth(axis == 0 ? w : -FLT_MIN);
    float value = axis == 0 ? mean.x : mean.y;
    if (ui_drag_float(axis == 0 ? "##x" : "##y", &value, 0.005f, -100000.0f, 100000.0f,
                         axis == 0 ? "X  %.3f" : "Y  %.3f")) {
      drawing_points_move(drawing, points, axis == 0 ? float2(value - mean.x, 0.0f) : float2(0.0f, value - mean.y));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s", tooltip);
    }
    done |= ImGui::IsItemDeactivatedAfterEdit();
    ui_driver_typed(C, driver_layer, driver_path, axis);
    ui_driver_field_end(C, driver_layer, driver_path, axis, driven);
  }
  ImGui::PopID();
  if (done) {
    C.undo->push(undo_name, *C.document);
  }
}

/* Detail levels (data/detail.hh): the shape's level, what it has, and Delete Higher, Apply Detail and
 * Rebuild Levels (on the selected shapes). */
static void detail_panel(Context &C, VectorDrawing &drawing, Shape &shape)
{
  const Layer *owner = document_active_layer(*C.document);
  if (!ui_panel_begin("Detail")) {
    return;
  }
  ImGui::BeginDisabled(owner && owner->is_reference());
  const int top = shape_detail_max_level(drawing, shape);
  const auto row = [](const char *label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
  };
  row("Level");
  if (ImGui::ArrowButton("##detail_down", ImGuiDir_Left)) {
    operator_call(C, "mesh.detail_level_set", {{"delta", "-1"}});
  }
  ImGui::SameLine(0, 2);
  int value = shape.detail_level;
  ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 2.0f);
  if (ui_drag_int("##detail_level", &value, 0.05f, 0, DETAIL_LEVEL_MAX, "%d", ImGuiSliderFlags_AlwaysClamp) &&
      value != shape.detail_level)
  {
    operator_call(C, "mesh.detail_level_set", {{"level", std::to_string(value)}});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("The level the shape is edited at: its curves show their points up to it, and the knife (K) "
                      "adds detail points at it (Ctrl 0 to Ctrl 4, Alt Page Up / Page Down)");
  }
  ImGui::SameLine(0, 4);
  ImGui::Text("/ %d", std::max(top, shape.detail_level));
  ImGui::SameLine(0, 4);
  if (ImGui::ArrowButton("##detail_up", ImGuiDir_Right)) {
    operator_call(C, "mesh.detail_level_set", {{"delta", "1"}});
  }
  row("Has");
  const int points = shape_detail_point_count(drawing, shape);
  if (top == 0) {
    ImGui::TextDisabled(shape.detail_level > 0 ? "No detail yet: the knife (K) adds some" : "No detail");
  }
  else {
    ImGui::Text("Levels %d  \xc2\xb7  %d detail point%s", top, points, points == 1 ? "" : "s");
  }
  row("");
  const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  ImGui::BeginDisabled(top <= shape.detail_level);
  if (ImGui::Button("Delete Higher", ImVec2(half, 0))) {
    operator_call(C, "mesh.detail_delete_higher");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Remove the detail above the level the selected shapes are edited at");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(top == 0);
  if (ImGui::Button("Apply Detail", ImVec2(half, 0))) {
    operator_call(C, "mesh.detail_apply");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Make the selected shapes' detail points their own points, as they show (setup frame)");
  }
  ImGui::EndDisabled();
  row("");
  static float tolerance = 0.02f;
  ImGui::SetNextItemWidth(half);
  ui_drag_float("##rebuild_tolerance", &tolerance, 0.001f, 0.0f, 1.0f, "Tolerance %.3f",
                   ImGuiSliderFlags_AlwaysClamp);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("How far a point can stray from the line through the kept ones and still become detail");
  }
  ImGui::SameLine();
  char tol[32];
  std::snprintf(tol, sizeof(tol), "%g", tolerance);
  if (ImGui::Button("Rebuild Levels", ImVec2(half, 0))) {
    operator_call(C, "mesh.detail_rebuild", {{"tolerance", tol}});
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Keep the points that make the selected shapes' outline as their own and make the rest level 1 "
                      "detail, as they show (setup frame): a dense drawing can then be edited with few points");
  }
  ImGui::EndDisabled();
  ui_panel_end();
}


/* The shared mark (Illustrator's global swatch triangle): a small triangle in a swatch's lower right. */
static void shared_mark(ImDrawList *dl, ImVec2 corner, float s)
{
  dl->AddTriangleFilled(corner, corner - ImVec2(s, 0), corner - ImVec2(0, s), IM_COL32(0xff, 0xff, 0xff, 255));
  dl->AddTriangle(corner, corner - ImVec2(s, 0), corner - ImVec2(0, s), IM_COL32(0x30, 0x30, 0x30, 255), 1.0f);
}

/* A material's preview: a disc in its fill colour ringed in its line colour; `shared` adds the mark. */
static void material_swatch(ImDrawList *dl, ImVec2 c, float r, const Style &style, bool shared = false)
{
  dl->AddCircleFilled(c, r, ImGui::ColorConvertFloat4ToU32(ImVec4(style.fill.r, style.fill.g, style.fill.b, 1.0f)), 20);
  dl->AddCircle(c, r, ImGui::ColorConvertFloat4ToU32(ImVec4(style.line.r, style.line.g, style.line.b, 1.0f)), 20,
                std::max(1.5f, r * 0.28f));
  if (shared) {
    shared_mark(dl, c + ImVec2(r * 1.05f, r * 1.05f), r * 0.95f);
  }
}

/* While a shared material's field is under the mouse or being changed: its shapes outlined in the
 * viewport, and the header says the edit reaches them (next frame). */
static int g_material_field_active_frame = -10;
static void material_field_reach(Context &C, const Material *shared)
{
  if (!shared) {
    return;
  }
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    C.wm->material_hover = shared->id;
    C.wm->material_hover_frame = ImGui::GetFrameCount();
  }
  if (ImGui::IsItemActive()) {
    g_material_field_active_frame = ImGui::GetFrameCount();
  }
}

/* Blender's data-block selector for the shape's material: browse (the shared materials, with a search),
 * the material's name (rename it here) and how many shapes use it (click: this shape keeps the look as
 * its own). With the shape's own look: browse and New (make it a shared material). */
static void material_row(Context &C, Shape &shape, bool reference)
{
  Document &doc = *C.document;
  const float h = ImGui::GetFrameHeight();
  Material *material = material_find(doc, shape.material);
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Material");
  ImGui::TableSetColumnIndex(1);
  /* A copy's looks are its own changes on its original's: materials are set on the original. */
  ImGui::BeginDisabled(reference);

  /* Browse. */
  const ImVec2 at = ImGui::GetCursorScreenPos();
  if (ImGui::Button("##browse", ImVec2(h * 1.5f, h))) {
    ImGui::OpenPopup("##materials");
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip(reference ? "A copy's shapes take their material from the original"
                                : "Link the shape to a shared material");
  }
  ImDrawList *dl = ImGui::GetWindowDrawList();
  material_swatch(dl, at + ImVec2(h * 0.55f, h * 0.5f), h * 0.28f, shape.style, material && !reference);
  const ImVec2 arrow = at + ImVec2(h * 1.15f, h * 0.45f);
  dl->AddTriangleFilled(arrow + ImVec2(-h * 0.12f, -h * 0.06f), arrow + ImVec2(h * 0.12f, -h * 0.06f),
                        arrow + ImVec2(0, h * 0.1f), IM_COL32(0xd0, 0xd0, 0xd0, 255));
  ImGui::SameLine(0, 2);

  const float rest = ImGui::GetContentRegionAvail().x;
  if (!material) {
    if (ImGui::Button("Make Shared", ImVec2(rest, 0))) {
      operator_call(C, "material.new");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Make this look a shared material (named after the shape, or the material it last came "
                        "from; rename it beside the swatch): other shapes can then use it, and changing it "
                        "changes them all");
    }
  }
  else {
    const int users = material_users(doc, material->id);
    char count[16];
    std::snprintf(count, sizeof(count), "%d", users);
    const float count_w = ImGui::CalcTextSize(count).x + ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
    std::string name = material->name;
    ImGui::SetNextItemWidth(rest - count_w - 2.0f);
    ImGui::InputText("##material_name", &name);
    if (ImGui::IsItemDeactivatedAfterEdit() && !name.empty() && name != material->name) {
      operator_call(C, "material.rename", {{"material", std::to_string(material->id)}, {"name", name}});
    }
    ImGui::SameLine(0, 2);
    if (ImGui::Button(count, ImVec2(count_w, 0))) {
      operator_call(C, "material.unlink");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%d shape%s use%s %s. Click: give this shape its own look (it looks the same)", users,
                        users == 1 ? "" : "s", users == 1 ? "s" : "", material->name.c_str());
    }
  }
  ImGui::EndDisabled();

  /* The list: every material with its preview and users, searched by name. Hovering one outlines its
   * shapes in the viewport; right-click deletes it. */
  if (ImGui::BeginPopup("##materials")) {
    static std::string search;
    if (ImGui::IsWindowAppearing()) {
      search.clear();
    }
    std::string lower_search = search;
    for (char &c : lower_search) {
      c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    ImGui::BeginChild("##material_list", ImVec2(h * 10.0f, std::min(float(doc.materials.size()) + 1.0f, 12.0f) * h));
    if (doc.materials.empty()) {
      ImGui::TextDisabled("No materials yet: New makes one from a shape's look");
    }
    uint64_t remove = 0;
    for (const Material &m : doc.materials) {
      std::string lower = m.name;
      for (char &c : lower) {
        c = char(std::tolower(static_cast<unsigned char>(c)));
      }
      if (!lower_search.empty() && lower.find(lower_search) == std::string::npos) {
        continue;
      }
      ImGui::PushID(int(m.id));
      const int users = material_users(doc, m.id);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const bool picked = ImGui::Selectable("##m", m.id == shape.material, 0, ImVec2(0, h));
      if (ImGui::IsItemHovered()) {
        C.wm->material_hover = m.id;
        C.wm->material_hover_frame = ImGui::GetFrameCount();
      }
      ImDrawList *ldl = ImGui::GetWindowDrawList();
      material_swatch(ldl, p + ImVec2(h * 0.5f, h * 0.5f), h * 0.3f, m.style, true);
      const ImU32 text = users > 0 ? IM_COL32(0xe6, 0xe6, 0xe6, 255) : IM_COL32(0x80, 0x80, 0x80, 255);
      ldl->AddText(p + ImVec2(h * 1.1f, (h - ImGui::GetFontSize()) * 0.5f), text, m.name.c_str());
      char count[16];
      std::snprintf(count, sizeof(count), "%d", users);
      const float cw = ImGui::CalcTextSize(count).x;
      ldl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - cw - 4.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f),
                   IM_COL32(0x90, 0x90, 0x90, 255), count);
      if (picked && m.id != shape.material) {
        operator_call(C, "material.assign", {{"material", std::to_string(m.id)}});
      }
      if (ImGui::BeginPopupContextItem("##material_menu")) {
        if (ImGui::MenuItem("Delete Material")) {
          remove = m.id;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::SetTooltip("The shapes using it keep its look as their own");
        }
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    ImGui::EndChild();
    if (remove != 0) {
      operator_call(C, "material.remove", {{"material", std::to_string(remove)}});
    }
    /* The search, at the bottom as in Blender, typed into as soon as the list opens. */
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(h * 10.0f - h * 4.6f);
    ImGui::InputTextWithHint("##search", "Search", &search);
    ImGui::SameLine(0, 2);
    bool unused = false;
    for (const Material &m : doc.materials) {
      unused |= material_users(doc, m.id) == 0;
    }
    ImGui::BeginDisabled(!unused);
    if (ImGui::Button("Remove Unused", ImVec2(-FLT_MIN, 0))) {
      operator_call(C, "material.remove_unused");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Delete the materials no shape uses (greyed out in the list)");
    }
    ImGui::EndPopup();
  }

}

static void shape_panel(Context &C, Layer &owner_layer, VectorDrawing &drawing, Shape &shape)
{
  Document &doc = *C.document;
  Layer *owner = &owner_layer;
  const bool reference = owner->is_reference();
  const std::string shape_header = "Shape: " + (shape.name.empty() ? std::string("(unnamed)") : shape.name) + "  in " + owner->name;
  /* On a reference: what this copy changed of this shape (teal), and what a recolour changed. */
  const ReferenceLook *look = nullptr;
  const Shape *original = nullptr;
  if (reference) {
    if (const auto it = owner->reference.looks.find(shape.id); it != owner->reference.looks.end()) {
      look = &it->second;
    }
    if (const Layer *source = layer_find(doc, owner->reference_source)) {
      const int si = drawing_shape_index(source->drawing, shape.id);
      original = si >= 0 ? &source->drawing.shapes[si] : nullptr;
    }
  }
  const std::string sid = std::to_string(shape.id);
  const auto setting = [&](const char *name) {
    return std::vector<OpArgs>{{{"kind", "LOOK"}, {"id", sid}, {"setting", name}}};
  };
  /* A colour changed by Recolor: its swap's index, or -1. */
  const auto swapped = [&](bool fill) {
    if (!reference || !original) {
      return -1;
    }
    const Color from = fill ? original->style.fill : original->style.line;
    for (size_t i = 0; i < owner->reference.swaps.size(); i++) {
      const ReferenceSwap &sw = owner->reference.swaps[i];
      if ((fill ? sw.fill : sw.line) && same_color(sw.from, from)) {
        return int(i);
      }
    }
    return -1;
  };
  const auto color_change = [&](bool fill, bool set) {
    if (set) {
      return setting(fill ? "fill" : "line");
    }
    const int s = swapped(fill);
    return s >= 0 ? std::vector<OpArgs>{{{"kind", "SWAP"}, {"id", std::to_string(s)}}} : std::vector<OpArgs>{};
  };

  if (ui_panel_begin("Shape", shape_header, 0u)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    /* A reference's shapes are named by its original. */
    ImGui::BeginDisabled(reference);
    ImGui::InputText("##name", &shape.name);
    ImGui::EndDisabled();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Rename Shape", doc);
    }
    /* On a copy whose points are locked, the shape can't move. */
    const bool points_locked = reference && !owner->reference.edit_points;
    ImGui::BeginDisabled(points_locked);
    static const char *const LOCKED_TIP =
        "Point editing is locked on this copy: unlock it (the lock in the viewport header, or\n"
        "the Reference tab's Points) to move the shape";
    mean_position_row(C, "Position", drawing, shape_points(drawing, shape),
                      points_locked ? LOCKED_TIP : "The middle of the shape's points; changing it moves the whole shape",
                      "Move Shape", !reference, owner_layer.id, driver_shape_position_path(shape.id));
    ImGui::EndDisabled();
    if (points_locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", LOCKED_TIP);
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    bool on = changed_begin(look && (look->has_fill_set || look->has_outline_set));
    const std::string shape_path = "shapes[" + std::to_string(shape.id) + "].";
    int driven = ui_driver_field_begin(C, owner_layer.id, shape_path + "has_fill", -1);
    if (ImGui::Checkbox("Fill", &shape.has_fill)) {
      C.undo->push("Shape Fill", doc);
    }
    ui_driver_field_end(C, owner_layer.id, shape_path + "has_fill", -1, driven);
    ImGui::SameLine();
    driven = ui_driver_field_begin(C, owner_layer.id, shape_path + "has_outline", -1);
    if (ImGui::Checkbox("Outline", &shape.has_outline)) {
      C.undo->push("Shape Outline", doc);
    }
    ui_driver_field_end(C, owner_layer.id, shape_path + "has_outline", -1, driven);
    {
      std::vector<OpArgs> both;
      if (look && look->has_fill_set) {
        both.push_back(setting("has_fill")[0]);
      }
      if (look && look->has_outline_set) {
        both.push_back(setting("has_outline")[0]);
      }
      changed_end(C, on, "##changed_flags", both, both);
    }
    /* A shape of several loops: how they fill together (Join Fills sets Even-Odd). */
    if (shape_runs(drawing, shape).size() > 1 || shape.fill_rule != FillRule::Loops) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted("Fill Rule");
      ImGui::TableSetColumnIndex(1);
      static const char *const RULES[] = {"Each Loop", "Even-Odd", "Nonzero"};
      static const char *const TIPS[] = {"Each loop filled on its own, holes filled over",
                                         "A loop inside another is a hole, whichever way it was drawn",
                                         "A loop drawn the other way round is a hole; the same way, it adds to the fill"};
      int rule = int(shape.fill_rule);
      ImGui::BeginDisabled(reference);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo("##fill_rule", RULES[rule])) {
        for (int k = 0; k < 3; k++) {
          if (ImGui::Selectable(RULES[k], k == rule)) {
            shape.fill_rule = FillRule(k);
            C.undo->push("Fill Rule", doc);
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TIPS[k]);
          }
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();
    }

    ui_panel_end();
  }
  /* The look, in a panel of its own: the material (shared, or the shape's own) and what it sets. A
   * shared one's header says so even folded. */
  const Material *shared = reference ? nullptr : material_find(doc, shape.material);
  std::string header = "Material: Own look";
  if (shared) {
    const int users = material_users(doc, shared->id);
    char text[256];
    if (ImGui::GetFrameCount() - g_material_field_active_frame <= 1) {
      std::snprintf(text, sizeof(text), "Material: editing %s changes %d shape%s", shared->name.c_str(), users,
                    users == 1 ? "" : "s");
    }
    else {
      std::snprintf(text, sizeof(text), "Material: %s (shared by %d shape%s)", shared->name.c_str(), users,
                    users == 1 ? "" : "s");
    }
    header = text;
  }
  if (ui_panel_begin("Material", header, 0u)) {
    material_row(C, shape, reference);

    /* A colour changed on a copy has a revert button beside it. */
    bool revert = false;
    float fill[4] = {shape.style.fill.r, shape.style.fill.g, shape.style.fill.b, shape.style.fill.a};
    bool on = changed_begin((look && look->fill_set) || swapped(true) >= 0);
    bool pick = false;
    /* A linked shape's look is its material's: drivers go on the material, so every shape sharing it
     * shows them (drivers_onto_materials moves ones made on a shape there). */
    const uint64_t look_layer = shape.material ? 0 : owner_layer.id;
    const auto look_path = [&](const char *what) {
      return shape.material ? "materials[" + std::to_string(shape.material) + "]." + what
                            : "shapes[" + std::to_string(shape.id) + "]." + what;
    };
    ui_driver_next(look_layer, look_path("fill"));
    if (ui_color(C, "Fill Color", fill, "Fill Color", on ? &revert : nullptr, &pick, ui_shape_eyedropper_active("FILL"))) {
      shape.style.fill = {fill[0], fill[1], fill[2], fill[3]};
    }
    if (pick) {
      ui_style_eyedropper_start("FILL", "fill colour");
    }
    material_field_reach(C, shared);
    const std::vector<OpArgs> fill_change = color_change(true, look && look->fill_set);
    changed_end(C, on, "##changed_fill", fill_change, fill_change);
    if (revert) {
      for (const OpArgs &args : fill_change) {
        operator_call(C, "layer.reference_revert", args);
      }
    }
    revert = false;
    float line[4] = {shape.style.line.r, shape.style.line.g, shape.style.line.b, shape.style.line.a};
    on = changed_begin((look && look->line_set) || swapped(false) >= 0);
    pick = false;
    ui_driver_next(look_layer, look_path("line"));
    if (ui_color(C, "Line Color", line, "Line Color", on ? &revert : nullptr, &pick, ui_shape_eyedropper_active("LINE"))) {
      shape.style.line = {line[0], line[1], line[2], line[3]};
    }
    if (pick) {
      ui_style_eyedropper_start("LINE", "line colour");
    }
    material_field_reach(C, shared);
    const std::vector<OpArgs> line_change = color_change(false, look && look->line_set);
    changed_end(C, on, "##changed_line", line_change, line_change);
    if (revert) {
      for (const OpArgs &args : line_change) {
        operator_call(C, "layer.reference_revert", args);
      }
    }
    on = changed_begin(look && look->width_set);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Line Width");
    ImGui::TableSetColumnIndex(1);
    /* Changed on a copy: a revert button beside it, as beside a changed colour; the eyedropper last. */
    ImGui::SetNextItemWidth(-(ui_revert_button_space() * (on ? 2.0f : 1.0f)));
    const float units_to_pixels = float(doc.height) * 0.5f;
    float pixels = shape.style.line_width * units_to_pixels;
    /* Driven in pixels, as shown. */
    const std::string width_path = look_path("line_width");
    const int driven_width = ui_driver_field_begin(C, look_layer, width_path, -1);
    if (ui_drag_float("##width", &pixels, 0.1f, 0.0f, 500.0f, "%.1f px", ImGuiSliderFlags_AlwaysClamp)) {
      shape.style.line_width = pixels / units_to_pixels;
    }
    material_field_reach(C, shared);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Line Width", doc);
    }
    ui_driver_typed(C, look_layer, width_path, -1);
    ui_driver_field_end(C, look_layer, width_path, -1, driven_width);
    if (on) {
      ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
      if (ui_revert_button("##revert_width", "Revert to the original's line width")) {
        operator_call(C, "layer.reference_revert", setting("line_width")[0]);
      }
    }
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ui_pick_button("##pick_width", "Eyedropper: click a shape in the viewport to take its line width",
                       ui_shape_eyedropper_active("WIDTH"))) {
      ui_style_eyedropper_start("WIDTH", "line width");
    }
    changed_end(C, on, "##changed_width", setting("line_width"), setting("line_width"));

    /* The brush (editors/brush_editor.hh): its preset or shape, and the preset button; both open the
     * Brush Settings window. */
    on = changed_begin(look && look->brush_set);
    const bool brush_revert = brush_row(C, shape.style.brush, shape.style, on ? "Revert to the original's brush" : nullptr);
    material_field_reach(C, shared);
    if (brush_revert) {
      operator_call(C, "layer.reference_revert", setting("brush")[0]);
    }
    changed_end(C, on, "##changed_brush", setting("brush"), setting("brush"));

    ui_panel_end();
  }
  detail_panel(C, drawing, shape);
}

/* The selected edges (segments with both points selected). Visible is Moho's Hide Edge: off,
 * the outline isn't drawn along them, though fills still use them as a boundary. */
static void edge_panel(Context &C, VectorDrawing &drawing)
{
  const std::vector<ShapeEdge> edges = drawing_selected_edges(drawing);
  if (edges.empty() || !ui_panel_begin("Selected Edges")) {
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
  if (mean < 0.0f) {
    /* Only a handle picked on its own: just its row. */
    if (!point_panel_handles(drawing).empty() && ui_panel_begin("Selected Points")) {
      handle_rows(C, drawing);
      ui_panel_end();
    }
    return;
  }
  if (!ui_panel_begin("Selected Points")) {
    return;
  }
  mean_position_row(C, "Mean Position", drawing, drawing_selected_points(drawing),
                    "The average position of the selected points; changing it moves them all", "Move Points", true);
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Mean Radius");
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-1);
  float value = mean;
  if (ui_drag_float("##mean_radius", &value, 0.005f, 0.0f, POINT_RADIUS_MAX, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
    drawing_set_selected_mean_radius(drawing, value);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Stroke radius at the selected points, as a multiple of the shape's line width (Alt S in the "
                      "viewport scales it)");
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    C.undo->push("Point Radius", *C.document);
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
  handle_rows(C, drawing);
  ui_panel_end();
}

void properties_draw(Area &area, Context &C)
{
  C.wm->mask_area_hover = {};
  C.wm->mask_tab_shown = false;
  /* A property shown from the timeline: its panel opens and its row flashes (see ui_flash_bind). */
  struct FlashBinding {
    explicit FlashBinding(WindowManager *wm) { ui_flash_bind(wm); }
    ~FlashBinding() { ui_flash_bind(nullptr); }
  } flash_binding(C.wm);
  area_editor_type_menu(area);
  ImGui::SameLine();
  if (C.wm->properties_tab_request >= 0) {
    area.properties_tab = C.wm->properties_tab_request;
    C.wm->properties_tab_request = -1;
  }
  const Layer *active_layer = document_active_layer(*C.document);
  const bool bones = active_layer && active_layer->is_bone_layer();
  /* Drawings have a Modifiers tab, left of their data tab, as Blender's modifier tab sits beside the
   * object data tab. Groups have one for the deforms that bend what is in them. */
  const bool modifiers = active_layer && (active_layer->has_drawing() || active_layer->is_group());
  struct TabEntry {
    const char *label;
    int id;
  };
  /* One order always, as Blender's (Object, Modifiers, Data, Bone, Bone Constraints, Material): the
   * layer's own settings, then its kind's data tab (Vector, Armature, Media, Text), then a drawing's
   * Shape or a bone layer's Bone and Bone Constraints, then a reference's link. */
  std::vector<TabEntry> tabs = {{"Document", TAB_DOCUMENT}};
  const bool camera = C.document->camera.selected;
  const bool vector = active_layer && active_layer->has_drawing();
  const bool reference = active_layer && active_layer->is_reference();
  const bool media = active_layer && active_layer->is_media();
  const bool text = active_layer && active_layer->is_text();
  if (camera) {
    /* The picked camera is what Properties shows, beside Document, as Blender shows the active
     * object's tabs: it has no layer, masking, modifier or drawing settings. */
    tabs.push_back({"Camera", TAB_CAMERA});
  }
  else {
    tabs.push_back({"Layer", TAB_LAYER});
    if (active_layer) {
      tabs.push_back({"Masking", TAB_MASKING});
    }
    if (modifiers) {
      tabs.push_back({"Modifiers", TAB_MODIFIERS});
    }
    if (vector || bones) {
      tabs.push_back({bones ? "Armature" : "Vector", TAB_DRAWING});
    }
    if (media) {
      tabs.push_back({"Media", TAB_MEDIA});
    }
    if (text) {
      tabs.push_back({"Text", TAB_TEXT});
    }
    if (vector) {
      tabs.push_back({"Shape", TAB_SHAPE});
    }
    if (bones) {
      tabs.push_back({"Bone", TAB_BONE});
      tabs.push_back({"Bone Constraints", TAB_CONSTRAINTS});
    }
    if (reference) {
      tabs.push_back({"Reference", TAB_REFERENCE});
    }
  }
  /* The tab picked shows when it applies; otherwise Layer (the camera's own with the camera picked),
   * the pick kept for when it applies again. */
  int shown = area.properties_tab;
  if (camera && shown != TAB_DOCUMENT) {
    shown = TAB_CAMERA;
  }
  if (std::none_of(tabs.begin(), tabs.end(), [&](const TabEntry &t) { return t.id == shown; })) {
    shown = TAB_LAYER;
  }
  for (size_t i = 0; i < tabs.size(); i++) {
    if (i > 0) {
      ImGui::SameLine(0, 2);
    }
    const bool active = shown == tabs[i].id;
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(tabs[i].label)) {
      area.properties_tab = shown = tabs[i].id;
    }
    if (active) {
      ImGui::PopStyleColor();
    }
  }
  ImGui::Separator();
  if (ImGui::BeginChild("##properties", ImVec2(0, 0))) {
    /* Each tab's panels in boxes, in the order kept for that tab. */
    std::string tab_name = "Document";
    for (const TabEntry &t : tabs) {
      if (t.id == shown) {
        tab_name = t.label;
      }
    }
    ui_boxes_begin("Properties/" + tab_name);
    switch (shown) {
      case TAB_LAYER:
        layer_tab(C);
        break;
      case TAB_CAMERA:
        camera_tab(C);
        break;
      case TAB_MASKING:
        masking_tab(C);
        break;
      case TAB_MODIFIERS:
        modifiers_panel(C, *document_active_layer(*C.document));
        break;
      case TAB_REFERENCE:
        reference_tab(C);
        break;
      case TAB_MEDIA:
        media_tab(C);
        break;
      case TAB_TEXT:
        properties_text_tab(C);
        break;
      case TAB_CONSTRAINTS:
        constraints_panel(C, *armature_worked_on(*C.document, C.document->active_layer));
        break;
      case TAB_DRAWING:
        if (bones) {
          /* The skeleton as a whole: its collections, every bone, its fixes. */
          bone_collections_panel(C, *document_active_layer(*C.document));
          bones_panel(C, *document_active_layer(*C.document));
          fix_actions_panel(C, *document_active_layer(*C.document));
        }
        else {
          /* Keying a fix on a drawing: what is being edited, at the top. */
          if (Layer *fix_owner = C.document->action.empty() ? nullptr : layer_find(*C.document, C.document->action_layer);
              fix_owner && fix_action_find(*fix_owner, C.document->action))
          {
            fix_actions_panel(C, *fix_owner);
          }
          drawing_tab(C, false);
        }
        break;
      case TAB_SHAPE:
        drawing_tab(C, true);
        break;
      case TAB_BONE: {
        /* The active bone alone; with none, the tab stays and says how to get one. */
        Layer &armature = *armature_worked_on(*C.document, C.document->active_layer);
        if (!bone_find(armature, armature.active_bone)) {
          ImGui::TextDisabled("No active bone: click a bone to edit it.");
          break;
        }
        bone_panel(C, armature);
        bone_relations_panel(C, armature);
        bone_display_panel(C, armature);
        break;
      }
      default:
        document_tab(C);
        break;
    }
    ui_boxes_end();
  }
  ImGui::EndChild();
}

}  // namespace oanim
