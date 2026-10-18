/* The Properties editor, like Blender's: tabs for the document, the active layer and its
 * drawing (shapes and their styles). A Modifiers tab arrives with the modifier stack. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <string>

#include "data/document.hh"
#include "editors/editors.hh"
#include "editors/ui.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace sky {

enum PropertiesTab { TAB_DOCUMENT = 0, TAB_LAYER = 1, TAB_DRAWING = 2 };

static void document_tab(Context &C)
{
  const rna::PointerRNA ptr{C.document, &Document::rna()};
  const char *path = "sky.data.document";
  if (ui_panel_begin("Document")) {
    ui_prop(C, ptr, "name", path);
    ui_panel_end();
  }
  if (ui_panel_begin("Format")) {
    ui_prop(C, ptr, "width", path);
    ui_prop(C, ptr, "height", path);
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
    ui_panel_end();
  }
  if (ui_panel_begin("Frame Range")) {
    ui_prop(C, ptr, "frame_start", path);
    ui_prop(C, ptr, "frame_end", path);
    ui_prop(C, ptr, "frame_current", path);
    ui_panel_end();
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
    ui_prop(C, ptr, "locked", path);
    ui_prop(C, ptr, "label_color", path);
    ui_panel_end();
  }
  if (ui_panel_begin("Transform")) {
    ui_prop(C, ptr, "location_x", path);
    ui_prop(C, ptr, "location_y", path);
    ui_prop(C, ptr, "rotation", path);
    ui_prop(C, ptr, "scale_x", path);
    ui_prop(C, ptr, "scale_y", path);
    ui_panel_end();
  }
  if (ui_panel_begin("Compositing")) {
    ui_prop(C, ptr, "opacity", path);
    const Layer *parent = layer_parent(doc, layer->id);
    if (parent && parent->group_mask != GroupMask::None) {
      ui_prop(C, ptr, "mask_mode", path);
    }
    if (layer->is_group()) {
      ui_prop(C, ptr, "group_mask", path);
    }
    ui_panel_end();
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

static void shape_panel(Context &C, Shape &shape);
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
          if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("SKY_SHAPE")) {
            move_from = *static_cast<const int *>(payload->Data);
            move_to = s;
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
  if (move_from >= 0 && move_from != move_to) {
    Shape moved = std::move(drawing->shapes[move_from]);
    drawing->shapes.erase(drawing->shapes.begin() + move_from);
    drawing->shapes.insert(drawing->shapes.begin() + move_to, std::move(moved));
    C.undo->push("Move Shape", doc);
    return;
  }
  /* The panel shows the topmost selected shape. */
  for (int s = 0; s < int(drawing->shapes.size()); s++) {
    if (selected[s]) {
      selected_shape = s;
    }
  }
  if (selected_shape >= 0) {
    shape_panel(C, drawing->shapes[selected_shape]);
  }
  edge_panel(C, *drawing);
  point_panel(C, *drawing);
}

/* The topmost selected shape's name, fill, outline and line width. */
static void shape_panel(Context &C, Shape &shape)
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
  ui_panel_end();
}

void properties_draw(Area &area, Context &C)
{
  area_editor_type_menu(area);
  ImGui::SameLine();
  static const char *tabs[] = {"Document", "Layer", "Drawing"};
  for (int i = 0; i < int(std::size(tabs)); i++) {
    if (i > 0) {
      ImGui::SameLine(0, 2);
    }
    const bool active = area.properties_tab == i;
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::Button(tabs[i])) {
      area.properties_tab = i;
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
      case TAB_DRAWING:
        drawing_tab(C);
        break;
      default:
        document_tab(C);
        break;
    }
  }
  ImGui::EndChild();
}

}  // namespace sky
