/* The Properties editor's Text tab: a text layer's words, their font and look, and the layout. */

#include "editors/text_panel.hh"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_stdlib.h"

#include "data/document.hh"
#include "data/material.hh"
#include "data/text.hh"
#include "editors/text_tool.hh"
#include "editors/ui.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "rna/rna.hh"
#include "text/text_layer.hh"
#include "text/text_outline.hh"
#include "wm/context.hh"

namespace oanim {

/* The stretch the style rows change: the selection while it is being typed in, else all of it. */
static void style_range(const Layer &layer, int *r_from, int *r_to)
{
  uint64_t editing = 0;
  int from = 0, to = 0;
  if (text_tool_editing(&editing, &from, &to) && editing == layer.id && from != to) {
    *r_from = from;
    *r_to = to;
    return;
  }
  *r_from = 0;
  *r_to = text_length(layer.text);
}

static bool contains_nocase(const std::string &text, const char *part)
{
  if (!*part) {
    return true;
  }
  const auto it = std::search(text.begin(), text.end(), part, part + std::strlen(part), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  });
  return it != text.end();
}

static const char *weight_name(int weight)
{
  static const char *const NAMES[] = {"Thin", "Extra Light", "Light", "Regular", "Medium",
                                      "Semibold", "Bold", "Extra Bold", "Black"};
  return NAMES[std::clamp((weight + 50) / 100 - 1, 0, 8)];
}

void properties_text_tab(Context &C)
{
  Document &doc = *C.document;
  Layer *found = document_active_layer(doc);
  if (!found || !found->is_text()) {
    ImGui::TextDisabled("Select a text layer.");
    return;
  }
  Layer &layer = *found;
  const rna::PointerRNA ptr{&layer, &Layer::rna()};
  const char *path = "oanim.data.layers.active";

  if (ui_panel_begin("Text")) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TableSetColumnIndex(1);
    std::string body = text_string(layer.text);
    const float h = ImGui::GetTextLineHeight() * std::clamp(float(std::count(body.begin(), body.end(), '\n') + 2), 3.0f, 12.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextMultiline("##body", &body, ImVec2(-FLT_MIN, h))) {
      text_set_string(layer.text, body);
      document_sync_text(doc);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Edit Text", doc);
    }
    for (const std::string &missing : text_missing_fonts(layer.text)) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f), "%s isn't installed: another font is drawn",
                         missing.c_str());
    }
    ui_panel_end();
  }

  int from = 0, to = 0;
  style_range(layer, &from, &to);
  /* What it shows: the first selected letter's run (the run after a join, not the one before it). */
  const int run_index = text_run_at(layer.text, from < to ? from + 1 : from);
  TextRun shown = layer.text.runs.empty() ? TextRun() : layer.text.runs[size_t(run_index)];
  /* Changes every run of the range, one undo step when the change is done. */
  const auto restyle = [&](const char *undo_name, bool finished, auto &&change) {
    for (TextRun *run : text_runs_between(layer.text, from, to)) {
      change(*run);
    }
    text_tidy(layer.text);
    document_sync_text(doc);
    if (finished) {
      C.undo->push(undo_name, doc);
    }
  };
  const bool part = from != 0 || to != text_length(layer.text);
  if (ui_panel_begin("Font", part ? "Font: the selected letters" : "Font", 0u)) {
    ui_row("Font", [&](float width) {
      ImGui::SetNextItemWidth(width);
      if (ImGui::BeginCombo("##font", shown.font.c_str(), ImGuiComboFlags_HeightLarge)) {
        static char filter[64] = {};
        if (ImGui::IsWindowAppearing()) {
          filter[0] = '\0';
          ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "Search fonts", filter, sizeof(filter));
        for (const std::string &family : text_font_families()) {
          if (!contains_nocase(family, filter)) {
            continue;
          }
          if (ImGui::Selectable(family.c_str(), family == shown.font)) {
            restyle("Text Font", true, [&](TextRun &r) { r.font = family; });
          }
        }
        ImGui::EndCombo();
      }
    });
    ui_row("Weight", [&](float width) {
      ImGui::SetNextItemWidth(width);
      if (ImGui::BeginCombo("##weight", weight_name(shown.weight))) {
        for (int w = 100; w <= 900; w += 100) {
          if (ImGui::Selectable(weight_name(w), (shown.weight + 50) / 100 * 100 == w)) {
            restyle("Text Weight", true, [&](TextRun &r) { r.weight = w; });
          }
        }
        ImGui::EndCombo();
      }
    });
    ui_row("Italic", [&](float) {
      bool italic = shown.italic;
      if (ImGui::Checkbox("##italic", &italic)) {
        restyle("Text Italic", true, [&](TextRun &r) { r.italic = italic; });
      }
    });
    ui_row("Size", [&](float width) {
      float size = shown.size;
      ImGui::SetNextItemWidth(width);
      if (ui_drag_float("##size", &size, 0.001f, 0.001f, 100.0f, "%.3f")) {
        restyle("Text Size", false, [&](TextRun &r) { r.size = std::max(size, 1e-4f); });
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        C.undo->push("Text Size", doc);
      }
    });
    ui_panel_end();
  }

  /* The look, as a shape's: fill and outline, its own colours and width, or a shared material's. */
  const Material *material = material_find(doc, shown.material);
  if (ui_panel_begin("Look", material ? "Look: " + material->name + " (shared)" : std::string("Look: Own look"), 0u)) {
    ui_row("Show", [&](float) {
      bool fill = shown.has_fill, outline = shown.has_outline;
      if (ImGui::Checkbox("Fill", &fill)) {
        restyle("Text Fill", true, [&](TextRun &r) { r.has_fill = fill; });
      }
      ImGui::SameLine();
      if (ImGui::Checkbox("Outline", &outline)) {
        restyle("Text Outline", true, [&](TextRun &r) { r.has_outline = outline; });
      }
    });
    ui_row("Material", [&](float width) {
      ImGui::SetNextItemWidth(width);
      if (ImGui::BeginCombo("##material", material ? material->name.c_str() : "Own look")) {
        if (ImGui::Selectable("Own look", !material)) {
          restyle("Unlink Material", true, [&](TextRun &r) {
            if (r.material) {
              r.material_last = material ? material->name : r.material_last;
            }
            r.material = 0;
          });
        }
        for (const Material &m : doc.materials) {
          if (ImGui::Selectable(m.name.c_str(), material && material->id == m.id)) {
            const uint64_t id = m.id;
            const Style style = m.style;
            restyle("Link Material", true, [&](TextRun &r) {
              r.material = id;
              r.style = style;
            });
          }
        }
        ImGui::EndCombo();
      }
    });
    /* A shared look changes its material, so every shape and text using it changes too. */
    const auto set_look = [&](const char *undo_name, bool finished, auto &&change) {
      if (Material *m = shown.material ? material_find(doc, shown.material) : nullptr) {
        Style style = m->style;
        change(style);
        material_set_style(doc, *m, style);
        restyle(undo_name, finished, [&](TextRun &r) {
          if (r.material == m->id) {
            r.style = style;
          }
        });
      }
      else {
        restyle(undo_name, finished, [&](TextRun &r) { change(r.style); });
      }
    };
    float fill[4] = {shown.style.fill.r, shown.style.fill.g, shown.style.fill.b, shown.style.fill.a};
    if (ui_color(C, "Fill Color", fill, nullptr)) {
      set_look("Text Fill Color", false, [&](Style &s) { s.fill = {fill[0], fill[1], fill[2], fill[3]}; });
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Text Fill Color", doc);
    }
    float line[4] = {shown.style.line.r, shown.style.line.g, shown.style.line.b, shown.style.line.a};
    if (ui_color(C, "Line Color", line, nullptr)) {
      set_look("Text Line Color", false, [&](Style &s) { s.line = {line[0], line[1], line[2], line[3]}; });
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      C.undo->push("Text Line Color", doc);
    }
    ui_row("Line Width", [&](float width) {
      float w = shown.style.line_width;
      ImGui::SetNextItemWidth(width);
      if (ui_drag_float("##line_width", &w, 0.0002f, 0.0f, 1.0f, "%.4f")) {
        set_look("Text Line Width", false, [&](Style &s) { s.line_width = std::max(w, 0.0f); });
      }
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        C.undo->push("Text Line Width", doc);
      }
    });
    ui_panel_end();
  }

  if (ui_panel_begin("Layout")) {
    ui_prop(C, ptr, "text_align", path);
    ui_prop(C, ptr, "text_line_spacing", path);
    ui_prop(C, ptr, "text_letter_spacing", path);
    ui_prop(C, ptr, "text_word_spacing", path);
    ui_prop(C, ptr, "text_shear", path);
    ui_prop(C, ptr, "text_box_width", path);
    ui_prop(C, ptr, "text_dont_auto_key", path);
    ui_panel_end();
  }

  if (ui_panel_begin("Convert")) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (ImGui::Button("To Vectors", ImVec2(w, 0))) {
      operator_call(C, "text.convert_to_vectors");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("An ordinary vector layer of its letters, to edit their points (one way)");
    }
    ImGui::SameLine();
    if (ImGui::Button("To Letters", ImVec2(w, 0))) {
      operator_call(C, "text.convert_to_vectors", {{"split", "LETTERS"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("A vector layer for each letter, in a group");
    }
    ImGui::SameLine();
    if (ImGui::Button("To Words", ImVec2(w, 0))) {
      operator_call(C, "text.convert_to_vectors", {{"split", "WORDS"}});
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("A vector layer for each word, in a group");
    }
    ui_panel_end();
  }
}

}  // namespace oanim
