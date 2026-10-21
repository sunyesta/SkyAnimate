/* Text editor, laid out like Blender's: View, Text and Templates menus, a text selector with
 * New and Open, and the text with line numbers. Alt P runs the text as a Python script. */

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/log.hh"
#include "data/document.hh"
#include "editors/editors.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "python/py_runtime.hh"
#include "ui/theme.hh"
#include "wm/context.hh"
#include "wm/file_dialog.hh"
#include "wm/keymap.hh"

namespace oanim {

/* Colours from Blender's default text editor theme. */
static constexpr ImU32 COL_TEXT_BACK = IM_COL32(0x23, 0x23, 0x23, 255);
static constexpr ImU32 COL_GUTTER = IM_COL32(0x1d, 0x1d, 0x1d, 255);
static constexpr ImU32 COL_LINE_NUMBER = IM_COL32(0x77, 0x77, 0x77, 255);
static constexpr ImU32 COL_LINE_NUMBER_CURRENT = IM_COL32(0xe6, 0xe6, 0xe6, 255);

static std::filesystem::path path_from_utf8(const std::string &path)
{
  return std::filesystem::path(reinterpret_cast<const char8_t *>(path.c_str()));
}

static std::string path_filename_utf8(const std::string &path)
{
  const std::u8string name = path_from_utf8(path).filename().u8string();
  return std::string(name.begin(), name.end());
}

static bool file_read(const std::string &path, std::string &r_contents)
{
  std::ifstream file(path_from_utf8(path), std::ios::binary);
  if (!file) {
    return false;
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  r_contents = buffer.str();
  /* The editor works with \n line endings, like Blender's. */
  std::erase(r_contents, '\r');
  return true;
}

static void text_open_dialog(Context &C)
{
  Context *ctx = &C;
  file_dialog_open("Python scripts and text", "py;txt;md;json", [ctx](const std::string &path) {
    Document &doc = *ctx->document;
    for (const Text &text : doc.texts) {
      if (text.filepath == path) {
        ctx->text_name = text.name;
        return;
      }
    }
    std::string body;
    if (!file_read(path, body)) {
      report(ReportType::Error, "Could not open " + path);
      return;
    }
    Text &text = document_text_add(doc, path_filename_utf8(path), body);
    text.filepath = path;
    ctx->text_name = text.name;
  });
}

static void text_save_as_dialog(Context &C)
{
  const Text *text = document_text_find(*C.document, C.text_name);
  if (!text) {
    return;
  }
  Context *ctx = &C;
  const std::string name = text->name;
  const std::string default_path = text->filepath.empty() ? name : text->filepath;
  file_dialog_save("Python scripts", "py", default_path, [ctx, name](const std::string &path) {
    Text *text = document_text_find(*ctx->document, name);
    if (!text) {
      return;
    }
    text->filepath = path;
    /* As in Blender, a saved text takes its file's name. */
    const std::string file_name = path_filename_utf8(path);
    if (file_name != text->name) {
      text->name = document_text_unique_name(*ctx->document, file_name);
      ctx->text_name = text->name;
    }
    operator_call(*ctx, "text.save");
  });
}

/* Menu item that runs an operator, with its shortcut, greyed out when its poll fails. */
static bool menu_operator(Context &C, const char *label, const char *idname)
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

static void templates_menu(Context &C)
{
  const std::filesystem::path folder = python::scripts_dir() / "templates_py";
  std::vector<std::filesystem::path> files;
  if (std::filesystem::is_directory(folder)) {
    for (const auto &entry : std::filesystem::directory_iterator(folder)) {
      if (entry.path().extension() == ".py") {
        files.push_back(entry.path());
      }
    }
  }
  std::sort(files.begin(), files.end());
  if (files.empty()) {
    ImGui::TextDisabled("No templates in scripts/templates_py");
  }
  for (const std::filesystem::path &file : files) {
    const std::u8string stem_utf8 = file.stem().u8string();
    std::string label(stem_utf8.begin(), stem_utf8.end());
    std::replace(label.begin(), label.end(), '_', ' ');
    if (!label.empty()) {
      label[0] = char(std::toupper(static_cast<unsigned char>(label[0])));
    }
    if (ImGui::MenuItem(label.c_str())) {
      const std::u8string name_utf8 = file.filename().u8string();
      std::string body;
      const std::u8string file_utf8 = file.u8string();
      file_read(std::string(file_utf8.begin(), file_utf8.end()), body);
      C.text_name = document_text_add(*C.document, std::string(name_utf8.begin(), name_utf8.end()), body).name;
    }
  }
}

static void text_header(Area &area, Context &C)
{
  Document &doc = *C.document;
  area_editor_type_menu(area);

  ImGui::SameLine();
  if (header_menu("View")) {
    ImGui::MenuItem("Line Numbers", nullptr, &area.show_line_numbers);
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (header_menu("Text")) {
    menu_operator(C, "New", "text.new");
    if (ImGui::MenuItem("Open...")) {
      text_open_dialog(C);
    }
    ImGui::Separator();
    const Text *text = document_text_find(doc, C.text_name);
    if (text && !text->filepath.empty()) {
      menu_operator(C, "Save", "text.save");
    }
    if (ImGui::MenuItem("Save As...", nullptr, false, text != nullptr)) {
      text_save_as_dialog(C);
    }
    ImGui::Separator();
    menu_operator(C, "Run Script", "text.run_script");
    ImGui::Separator();
    menu_operator(C, "Delete", "text.unlink");
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (header_menu("Templates")) {
    if (ImGui::BeginMenu("Python")) {
      templates_menu(C);
      ImGui::EndMenu();
    }
    ImGui::EndPopup();
  }

  /* Text selector with New and Open, as in Blender's header. */
  ImGui::SameLine(0.0f, ImGui::GetFrameHeight());
  if (!doc.texts.empty()) {
    ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 7.0f);
    if (ImGui::BeginCombo("##text", C.text_name.c_str())) {
      for (const Text &text : doc.texts) {
        if (ImGui::Selectable(text.name.c_str(), text.name == C.text_name)) {
          C.text_name = text.name;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
  }
  if (ImGui::Button("+ New")) {
    operator_call(C, "text.new");
  }
  ImGui::SameLine();
  if (ImGui::Button("Open")) {
    text_open_dialog(C);
  }

  /* Run Script on the right, like Blender's play button. */
  const Text *text = document_text_find(doc, C.text_name);
  if (text) {
    const char *run_label = "Run Script";
    const float run_w = ImGui::CalcTextSize(run_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float right_x = cursor.x + ImGui::GetContentRegionAvail().x - run_w;
    ImGui::SetCursorScreenPos(ImVec2(std::max(cursor.x, right_x), cursor.y));
    if (ImGui::Button(run_label)) {
      operator_call(C, "text.run_script");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Run the text as a Python script\nShortcut: %s",
                        keymaps().shortcut_for("text.run_script").c_str());
    }
  }
}

/* Tracks the caret so the view can follow it, and types four spaces for Tab, as Blender does. */
struct CaretState {
  int line = 0;
};

static int text_callback(ImGuiInputTextCallbackData *data)
{
  if (data->CursorPos > 0 && data->Buf[data->CursorPos - 1] == '\t') {
    const int pos = data->CursorPos - 1;
    data->DeleteChars(pos, 1);
    data->InsertChars(pos, "    ");
  }
  CaretState *caret = static_cast<CaretState *>(data->UserData);
  caret->line = int(std::count(data->Buf, data->Buf + data->CursorPos, '\n'));
  return 0;
}

static void text_body(Area &area, Context &C, Text &text)
{
  const ImGuiStyle &style = ImGui::GetStyle();
  ImGui::PushFont(theme_font_mono(), 0.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_TEXT_BACK);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, COL_TEXT_BACK);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

  if (ImGui::BeginChild("text_body", ImVec2(0, 0))) {
    const float line_h = ImGui::GetTextLineHeight();
    const int line_count = 1 + int(std::count(text.body.begin(), text.body.end(), '\n'));
    const std::string widest_number = std::to_string(std::max(line_count, 999));
    const float gutter_w = area.show_line_numbers ? ImGui::CalcTextSize(widest_number.c_str()).x + 16.0f : 6.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float view_h = ImGui::GetWindowHeight();
    /* Tall enough for every line, so the child scrolls and the line numbers stay aligned. */
    const float body_h = std::max(view_h, float(line_count + 1) * line_h + style.FramePadding.y * 2.0f);

    ImGui::SetCursorScreenPos(origin + ImVec2(gutter_w, 0.0f));
    CaretState caret;
    if (ImGui::InputTextMultiline("##body", &text.body, ImVec2(-FLT_MIN, body_h),
                                  ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways,
                                  text_callback, &caret)) {
      C.undo->mark_changed();
    }
    const bool editing = ImGui::IsItemActive();

    /* Keep the caret in view while typing or moving with the arrow keys. */
    if (editing) {
      const float caret_y = style.FramePadding.y + float(caret.line) * line_h;
      const float scroll = ImGui::GetScrollY();
      if (caret_y < scroll) {
        ImGui::SetScrollY(caret_y);
      }
      else if (caret_y + line_h * 2.0f > scroll + view_h) {
        ImGui::SetScrollY(caret_y + line_h * 2.0f - view_h);
      }
    }

    if (area.show_line_numbers) {
      ImDrawList *dl = ImGui::GetWindowDrawList();
      const ImVec2 window_pos = ImGui::GetWindowPos();
      dl->AddRectFilled(window_pos, ImVec2(window_pos.x + gutter_w - 4.0f, window_pos.y + view_h), COL_GUTTER);
      const int first = std::max(0, int(ImGui::GetScrollY() / line_h) - 1);
      const int last = std::min(line_count, first + int(view_h / line_h) + 3);
      for (int i = first; i < last; i++) {
        const std::string number = std::to_string(i + 1);
        const float x = origin.x + gutter_w - 10.0f - ImGui::CalcTextSize(number.c_str()).x;
        const float y = origin.y + style.FramePadding.y + float(i) * line_h;
        dl->AddText(ImVec2(x, y), (editing && i == caret.line) ? COL_LINE_NUMBER_CURRENT : COL_LINE_NUMBER,
                    number.c_str());
      }
    }
  }
  ImGui::EndChild();

  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(2);
  ImGui::PopFont();
}

void text_editor_draw(Area &area, Context &C)
{
  Document &doc = *C.document;
  if (!C.text_name.empty() && !document_text_find(doc, C.text_name)) {
    C.text_name.clear();
  }
  if (C.text_name.empty() && !doc.texts.empty()) {
    C.text_name = doc.texts.front().name;
  }

  text_header(area, C);

  Text *text = document_text_find(doc, C.text_name);
  if (!text) {
    ImGui::Spacing();
    ImGui::TextDisabled("Create a text with + New, open a file with Open, or pick one from Templates.");
    return;
  }
  text_body(area, C, *text);
}

}  // namespace oanim
