/* Python console, like Blender's: scrollback coloured by kind of line, a prompt with history
 * (Up/Down) and completion (Tab). The interpreter itself lives in scripts/modules/sky/_console.py. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_stdlib.h"

#include <cfloat>

#include "editors/editors.hh"
#include "python/py_runtime.hh"
#include "ui/theme.hh"
#include "wm/context.hh"

namespace sky {

/* Colours from Blender's default console theme. */
static constexpr ImU32 COL_CONSOLE_BACK = IM_COL32(0x1d, 0x1d, 0x1d, 255);
static constexpr ImU32 COL_INPUT = IM_COL32(0xf2, 0xf2, 0xf2, 255);
static constexpr ImU32 COL_OUTPUT = IM_COL32(0x71, 0xa8, 0xff, 255);
static constexpr ImU32 COL_INFO = IM_COL32(0x95, 0xd6, 0x00, 255);
static constexpr ImU32 COL_ERROR = IM_COL32(0xff, 0x4d, 0x84, 255);

/* All console areas show the same interpreter, so they share one copy of its scrollback. */
static std::vector<python::ConsoleLine> g_lines;
static uint64_t g_lines_version = UINT64_MAX;

static ImU32 line_color(const std::string &kind)
{
  if (kind == "INPUT") {
    return COL_INPUT;
  }
  if (kind == "ERROR") {
    return COL_ERROR;
  }
  if (kind == "INFO") {
    return COL_INFO;
  }
  return COL_OUTPUT;
}

static void replace_input(ImGuiInputTextCallbackData *data, const std::string &text)
{
  data->DeleteChars(0, data->BufTextLen);
  data->InsertChars(0, text.c_str());
}

static int input_callback(ImGuiInputTextCallbackData *data)
{
  Area &area = *static_cast<Area *>(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    const std::vector<std::string> history = python::console_history();
    if (history.empty()) {
      return 0;
    }
    int &pos = area.console_history_pos;
    if (data->EventKey == ImGuiKey_UpArrow) {
      pos = (pos == -1) ? int(history.size()) - 1 : std::max(0, pos - 1);
    }
    else if (data->EventKey == ImGuiKey_DownArrow && pos != -1) {
      pos = (pos + 1 >= int(history.size())) ? -1 : pos + 1;
    }
    replace_input(data, pos == -1 ? std::string() : history[pos]);
  }
  else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
    const std::string line(data->Buf, data->BufTextLen);
    const std::string completed = python::console_complete(line);
    if (completed != line) {
      replace_input(data, completed);
    }
  }
  return 0;
}

void console_draw(Area &area, Context & /*C*/)
{
  area_editor_type_menu(area);
  ImGui::SameLine();
  if (header_menu("Console")) {
    if (ImGui::MenuItem("Clear")) {
      python::console_clear();
    }
    ImGui::EndPopup();
  }

  const uint64_t version = python::console_version();
  if (version != g_lines_version) {
    g_lines = python::console_lines();
    g_lines_version = version;
  }
  const bool scroll_to_bottom = area.console_seen_version != version;
  area.console_seen_version = version;

  ImGui::PushFont(theme_font_mono(), 0.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_CONSOLE_BACK);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, COL_CONSOLE_BACK);

  bool clicked = false;
  const float input_h = ImGui::GetFrameHeightWithSpacing();
  if (ImGui::BeginChild("console_body", ImVec2(0, 0), ImGuiChildFlags_None)) {
    if (ImGui::BeginChild("scrollback", ImVec2(0, -input_h), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
      ImGuiListClipper clipper;
      clipper.Begin(int(g_lines.size()));
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
          ImGui::PushStyleColor(ImGuiCol_Text, line_color(g_lines[i].kind));
          ImGui::TextUnformatted(g_lines[i].text.c_str());
          ImGui::PopStyleColor();
        }
      }
      if (scroll_to_bottom) {
        ImGui::SetScrollHereY(1.0f);
      }
      clicked = ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    }
    ImGui::EndChild();

    /* Prompt and input line. Clicking anywhere in the console types into it, as in Blender. */
    const std::string prompt = python::console_prompt();
    ImGui::PushStyleColor(ImGuiCol_Text, COL_INPUT);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(prompt.c_str());
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (clicked || ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    /* No inner padding, so typed text starts right after the prompt as in Blender. */
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
    const bool entered = ImGui::InputText(
        "##console_input", &area.console_input,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
            ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_EscapeClearsAll,
        input_callback, &area);
    ImGui::PopStyleVar();
    if (entered) {
      python::console_push(area.console_input);
      area.console_input.clear();
      area.console_history_pos = -1;
      ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::PopStyleColor();
  }
  ImGui::EndChild();

  ImGui::PopStyleColor(2);
  ImGui::PopFont();
}

}  // namespace sky
