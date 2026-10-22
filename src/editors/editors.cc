#include "editors/editors.hh"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include <SDL3/SDL_opengl.h>
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include "anim/draw_order.hh"
#include "core/log.hh"
#include "editors/ui.hh"
#include "ui/theme.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "rna/rna.hh"
#include "wm/context.hh"
#include "wm/keymap.hh"

namespace oanim {

const char *editor_type_label(EditorType type)
{
  switch (type) {
    case EditorType::Viewport:
      return "Viewport";
    case EditorType::Properties:
      return "Properties";
    case EditorType::Layers:
      return "Layers";
    case EditorType::Timeline:
      return "Timeline";
    case EditorType::TextEditor:
      return "Text Editor";
    case EditorType::Console:
      return "Python Console";
    case EditorType::Info:
      return "Info";
  }
  return "Unknown";
}

const char *editor_keymap_name(EditorType type)
{
  return type == EditorType::Console ? "Console" : editor_type_label(type);
}

bool header_menu(const char *label)
{
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
  if (ImGui::Button(label)) {
    ImGui::OpenPopup(label);
  }
  ImGui::PopStyleColor();
  return ImGui::BeginPopup(label);
}

std::string Area::window_name() const
{
  return std::string(editor_type_label(type)) + "###area" + std::to_string(id);
}

/* ---------------------------------------------------------------------------------
 * Generic property widgets, like Blender's layout.prop(): one RNA description draws
 * any property. Edits made here push an undo step when the widget is released.
 */

/* The window manager whose properties_flash a Properties editor drawing now shows (ui_flash_bind). */
static WindowManager *g_flash_wm = nullptr;

static Document *g_fold_doc = nullptr;
static UndoStack *g_fold_undo = nullptr;
/* Where each open sub-panel's rows start (ui_subpanel_begin). */
static std::vector<float> g_subpanel_tops;

void ui_bind_project(Document *doc, UndoStack *undo)
{
  g_fold_doc = doc;
  g_fold_undo = undo;
}

std::string ui_fold_scope()
{
  const ImGuiWindow *window = ImGui::GetCurrentWindowRead();
  if (!window) {
    return "";
  }
  /* An area's window is "<editor>###area<id>"; a popup's its own name. */
  std::string name = window->RootWindow ? window->RootWindow->Name : window->Name;
  if (const size_t hash = name.find("##"); hash != std::string::npos) {
    name = name.substr(0, hash);
  }
  return name;
}

bool ui_fold_state(const std::string &key, bool default_open)
{
  if (!g_fold_doc) {
    return default_open;
  }
  const auto it = g_fold_doc->ui_open.find(key);
  return it == g_fold_doc->ui_open.end() ? default_open : it->second;
}

void ui_fold_record(const std::string &key, bool default_open, bool open)
{
  if (!g_fold_doc || ui_fold_state(key, default_open) == open) {
    return;
  }
  if (open == default_open) {
    g_fold_doc->ui_open.erase(key);
  }
  else {
    g_fold_doc->ui_open[key] = open;
  }
  if (g_fold_undo) {
    g_fold_undo->mark_changed();
  }
}

void ui_fold_next(const std::string &key, bool default_open)
{
  ImGui::SetNextItemOpen(ui_fold_state(key, default_open), ImGuiCond_Always);
}

/* ------------------------------------------------------------------------------------------------
 * Boxes: Blender's Properties panels. Between ui_boxes_begin and ui_boxes_end every panel and
 * collapsing header is a rounded box (its header and body one shade, apart from the editor's
 * background), with a grip at the header's right for dragging it to another place. A box runs from
 * its header to the next box (or the end).
 *
 * The order is kept per scope (Document::ui_panel_order once a box was dragged), without the editors
 * drawing their panels in it: each box is placed where the order puts it, from the heights the boxes
 * before it had (this frame's when drawn already, else the last frame's). */

/* Blender's theme: a panel's back and its outline, and the shade over a sub-panel's rows. */
static constexpr unsigned int BOX_BG = IM_COL32(0x3d, 0x3d, 0x3d, 255);
static constexpr unsigned int BOX_OUTLINE = IM_COL32(0xff, 0xff, 0xff, 0x12);
static constexpr unsigned int BOX_BAND = IM_COL32(0x00, 0x00, 0x00, 0x1f);
static constexpr unsigned int BOX_GRIP = IM_COL32(0x80, 0x80, 0x80, 255);
static constexpr const char *BOX_PAYLOAD = "OANIM_PANEL";

namespace {

struct BoxScope {
  bool active = false;
  std::string scope;
  bool started = false;
  float x0 = 0.0f, x1 = 0.0f, start_y = 0.0f, bottom = 0.0f;
  /* This frame's boxes, as met, and the one open now. */
  std::vector<std::string> seen;
  std::map<std::string, float> heights;
  bool open = false;
  std::string id;
  float top = 0.0f;
  ImRect header;
  ImDrawListSplitter splitter;
  ImDrawList *dl = nullptr;
};

BoxScope g_box;
/* Per scope: every box met, in order, and the last frame's boxes and heights. */
std::map<std::string, std::vector<std::string>> g_box_order;
std::map<std::string, std::map<std::string, float>> g_box_last_heights;

}  // namespace

static std::vector<std::string> &box_order()
{
  std::vector<std::string> &order = g_box_order[g_box.scope];
  /* A dragged order is the project's. */
  if (g_fold_doc) {
    if (const auto it = g_fold_doc->ui_panel_order.find(g_box.scope);
        it != g_fold_doc->ui_panel_order.end() && !it->second.empty())
    {
      for (const std::string &id : it->second) {
        if (std::find(order.begin(), order.end(), id) == order.end()) {
          order.push_back(id);
        }
      }
      std::vector<std::string> sorted = it->second;
      for (const std::string &id : order) {
        if (std::find(sorted.begin(), sorted.end(), id) == sorted.end()) {
          sorted.push_back(id);
        }
      }
      order = sorted;
    }
  }
  return order;
}

/* The room between a box's edges and what's in it. */
static float box_padding()
{
  return ImGui::GetStyle().FramePadding.x;
}

static void box_close()
{
  BoxScope &b = g_box;
  if (!b.open) {
    return;
  }
  b.open = false;
  ImGui::Unindent(box_padding());
  const float pad = ImGui::GetStyle().ItemSpacing.y;
  const bool body = ImGui::GetCursorScreenPos().y > b.header.Max.y + 1.0f;
  if (body) {
    ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));
  }
  const float y1 = body ? ImGui::GetCursorScreenPos().y : b.header.Max.y;
  const ImRect rect(ImVec2(b.x0, b.top), ImVec2(b.x1, y1));
  b.splitter.SetCurrentChannel(b.dl, 0);
  b.dl->AddRectFilled(rect.Min, rect.Max, BOX_BG, ImGui::GetStyle().FrameRounding);
  b.splitter.Merge(b.dl);
  /* The outline over the bands at the edges, a pixel wide. */
  b.dl->AddRect(rect.Min + ImVec2(0.5f, 0.5f), rect.Max - ImVec2(0.5f, 0.5f), BOX_OUTLINE,
                ImGui::GetStyle().FrameRounding, 0, 1.0f);
  b.dl = nullptr;
  const float height = rect.GetHeight() + pad;
  b.heights[b.id] = height;
  b.bottom = std::max(b.bottom, b.top + height);

  /* Dropping a dragged box on this one: above its middle puts it before, below after. */
  ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
  if (ImGui::BeginDragDropTargetCustom(rect, ImGui::GetID((b.id + "##box_drop").c_str()))) {
    const ImGuiPayload *peek = ImGui::GetDragDropPayload();
    const std::string dragged = peek && peek->IsDataType(BOX_PAYLOAD)
                                    ? std::string(static_cast<const char *>(peek->Data), size_t(peek->DataSize))
                                    : std::string();
    if (!dragged.empty() && dragged != b.id) {
      const bool before = ImGui::GetIO().MousePos.y < rect.GetCenter().y;
      const float line_y = before ? rect.Min.y - pad * 0.5f : rect.Max.y + pad * 0.5f;
      ImGui::GetWindowDrawList()->AddLine(ImVec2(rect.Min.x, line_y), ImVec2(rect.Max.x, line_y), UI_DROP_LINE_COLOR,
                                          2.0f);
      if (ImGui::AcceptDragDropPayload(BOX_PAYLOAD, flags)) {
        std::vector<std::string> &order = box_order();
        std::erase(order, dragged);
        auto at = std::find(order.begin(), order.end(), b.id);
        order.insert(before ? at : (at == order.end() ? at : at + 1), dragged);
        if (g_fold_doc) {
          g_fold_doc->ui_panel_order[b.scope] = order;
          if (g_fold_undo) {
            g_fold_undo->mark_changed();
          }
        }
      }
    }
    ImGui::EndDragDropTarget();
  }
  /* Whatever follows comes below every box so far (the next box places itself). */
  ImGui::SetCursorScreenPos(ImVec2(b.x0, b.bottom));
}

/* Starts box `id` (closing the one before) where the order puts it. False outside ui_boxes_begin. */
static bool box_open(const std::string &id)
{
  BoxScope &b = g_box;
  if (!b.active) {
    return false;
  }
  box_close();
  if (!b.started) {
    b.started = true;
    b.x0 = ImGui::GetCursorScreenPos().x;
    b.x1 = b.x0 + ImGui::GetContentRegionAvail().x;
    b.start_y = b.bottom = ImGui::GetCursorScreenPos().y;
  }
  std::vector<std::string> &order = box_order();
  if (std::find(order.begin(), order.end(), id) == order.end()) {
    /* New: after the box met before it, so the editor's order is the default. */
    auto at = b.seen.empty() ? order.begin() : std::find(order.begin(), order.end(), b.seen.back());
    order.insert(at == order.end() ? at : (b.seen.empty() ? at : at + 1), id);
  }
  b.seen.push_back(id);
  const std::map<std::string, float> &last = g_box_last_heights[b.scope];
  float y = b.start_y;
  for (const std::string &other : order) {
    if (other == id) {
      break;
    }
    if (const auto now = b.heights.find(other); now != b.heights.end()) {
      y += now->second;
    }
    else if (const auto was = last.find(other); was != last.end()) {
      y += was->second;
    }
  }
  ImGui::SetCursorScreenPos(ImVec2(b.x0, y));
  /* Its content keeps off the box's edges. */
  ImGui::Indent(box_padding());
  b.open = true;
  b.id = id;
  b.top = y;
  b.dl = ImGui::GetWindowDrawList();
  b.splitter.Split(b.dl, 3);
  b.splitter.SetCurrentChannel(b.dl, 2);
  return true;
}

/* The header of a box: ImGui's collapsing header on the box's shade, and the grip at its right. */
static bool box_header(const char *label, const std::string &id)
{
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0xff, 0xff, 0xff, 0x0c));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0xff, 0xff, 0xff, 0x14));
  ImGui::SetNextItemAllowOverlap();
  const bool open = ImGui::CollapsingHeader(label);
  ImGui::PopStyleColor(3);
  g_box.header = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  const ImVec2 after = ImGui::GetCursorScreenPos();
  /* The grip: 4 x 2 dots, as Blender's. */
  const ImRect &h = g_box.header;
  const float s = ImGui::GetFontSize() * 0.12f, step = s * 3.0f;
  const ImVec2 grip_min(h.Max.x - step * 4.0f - s * 4.0f, h.Min.y);
  ImGui::SetCursorScreenPos(grip_min);
  ImGui::InvisibleButton((id + "##grip").c_str(), ImVec2(h.Max.x - grip_min.x, h.GetHeight()));
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const float cy = h.GetCenter().y;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 2; row++) {
      dl->AddCircleFilled(ImVec2(grip_min.x + s * 2.0f + step * float(col), cy + (row == 0 ? -step : step) * 0.5f), s,
                          hot ? ImGui::GetColorU32(ImGuiCol_Text) : BOX_GRIP);
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive()) {
    ImGui::SetTooltip("Drag to move the panel");
  }
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
    ImGui::SetDragDropPayload(BOX_PAYLOAD, id.data(), id.size());
    std::string shown = label;
    if (const size_t hash = shown.find("###"); hash != std::string::npos) {
      shown = shown.substr(0, hash);
    }
    ImGui::TextUnformatted(shown.c_str());
    ImGui::EndDragDropSource();
  }
  ImGui::SetCursorScreenPos(after);
  return open;
}

void ui_boxes_begin(const std::string &scope)
{
  g_box = {};
  g_box.active = true;
  g_box.scope = scope;
}

void ui_boxes_end()
{
  BoxScope &b = g_box;
  if (!b.active) {
    return;
  }
  box_close();
  if (b.started) {
    ImGui::SetCursorScreenPos(ImVec2(b.x0, std::max(b.bottom, ImGui::GetCursorScreenPos().y)));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
  }
  g_box_last_heights[b.scope] = b.heights;
  g_box.active = false;
}

/* The room a box's right edge keeps from its content. */
static float box_inset()
{
  return g_box.open ? box_padding() : 0.0f;
}

bool ui_subpanel_begin(const std::string &key, const std::string &label, bool default_open)
{
  const std::string fold = ui_fold_scope() + "/" + key;
  ui_fold_next(fold, default_open);
  const bool in_table = ImGui::GetCurrentTable() != nullptr;
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0xff, 0xff, 0xff, 0x0c));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0xff, 0xff, 0xff, 0x14));
  bool open;
  if (in_table) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                   ImGuiTreeNodeFlags_FramePadding);
  }
  else {
    open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                   ImGuiTreeNodeFlags_FramePadding);
  }
  ImGui::PopStyleColor(3);
  ui_fold_record(fold, default_open, open);
  if (open) {
    g_subpanel_tops.push_back(ImGui::GetItemRectMax().y);
  }
  return open;
}

void ui_subpanel_end()
{
  if (g_subpanel_tops.empty()) {
    return;
  }
  const float top = g_subpanel_tops.back();
  g_subpanel_tops.pop_back();
  /* The rows under an open sub-panel sit on a darker band, the box's width (Blender's). */
  const float pad = ImGui::GetStyle().ItemSpacing.y * 0.5f;
  if (ImGuiTable *table = ImGui::GetCurrentTable()) {
    /* The last row ends where the next one starts. */
    ImGui::TableNextRow();
    /* A row with a column, which the table's channels go back to after the background. */
    ImGui::TableSetColumnIndex(0);
    const float bottom = table->RowPosY1;
    ImGui::TablePushBackgroundChannel();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float x0 = g_box.open ? g_box.x0 : table->OuterRect.Min.x, x1 = g_box.open ? g_box.x1 : table->OuterRect.Max.x;
    dl->PushClipRect(ImVec2(x0, top), ImVec2(x1, bottom), false);
    dl->AddRectFilled(ImVec2(x0, top + 1.0f), ImVec2(x1, bottom), BOX_BAND);
    dl->PopClipRect();
    ImGui::TablePopBackgroundChannel();
  }
  else if (g_box.open && g_box.dl) {
    ImGui::Dummy(ImVec2(0.0f, pad));
    const float bottom = ImGui::GetCursorScreenPos().y;
    g_box.splitter.SetCurrentChannel(g_box.dl, 1);
    g_box.dl->AddRectFilled(ImVec2(g_box.x0, top + 1.0f), ImVec2(g_box.x1, bottom), BOX_BAND);
    g_box.splitter.SetCurrentChannel(g_box.dl, 2);
  }
}

bool ui_collapsing_header(const char *label, bool default_open)
{
  /* "Changes (3)###changes": the part after ### is what it's kept by. */
  std::string id = label;
  if (const size_t hash = id.find("###"); hash != std::string::npos) {
    id = id.substr(hash + 3);
  }
  const std::string key = ui_fold_scope() + "/" + id;
  /* The box first: closing the one before submits items, which would use up the fold. */
  const bool boxed = box_open(id);
  ui_fold_next(key, default_open);
  const bool open = boxed ? box_header(label, id) : ImGui::CollapsingHeader(label);
  ui_fold_record(key, default_open, open);
  return open;
}

bool ui_panel_begin(const char *title, const std::string &header, unsigned int color, bool default_open);

bool ui_panel_begin(const char *title, bool default_open)
{
  return ui_panel_begin(title, std::string(), 0u, default_open);
}

bool ui_panel_begin(const char *title, const std::string &header, unsigned int color)
{
  return ui_panel_begin(title, header, color, true);
}

bool ui_panel_begin(const char *title, const std::string &header, unsigned int color, bool default_open)
{
  const std::string key = ui_fold_scope() + "/" + title;
  /* A property being shown (ui_flash_bind) opens its panel. */
  if (g_flash_wm && g_flash_wm->properties_flash.layer != 0 && g_flash_wm->properties_flash.start < 0.0 &&
      g_flash_wm->properties_flash.panel == title)
  {
    ui_fold_record(key, default_open, true);
  }
  const bool boxed = box_open(title);
  ui_fold_next(key, default_open);
  if (color != 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
  }
  const std::string label = header.empty() ? std::string(title) : header + "###" + title;
  const bool open = boxed ? box_header(label.c_str(), title) : ImGui::CollapsingHeader(label.c_str());
  if (color != 0) {
    ImGui::PopStyleColor();
  }
  ui_fold_record(key, default_open, open);
  if (!open) {
    return false;
  }
  if (!ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp, ImVec2(-box_inset(), 0.0f))) {
    return false;
  }
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
  return true;
}

void ui_panel_end()
{
  ImGui::EndTable();
}

/* A key button waiting for the next row (ui_key_next). */
static struct {
  bool pending = false;
  int state = 0;
  std::function<void()> on_click;
  int pin = -1;
  std::function<void()> on_pin;
  uint64_t flash_layer = 0;
  std::string flash_path;
} g_key_next;


void ui_flash_bind(WindowManager *wm)
{
  g_flash_wm = wm;
  if (wm && wm->properties_flash.layer != 0) {
    wm->properties_flash.seen_properties = true;
  }
}

void ui_flash_row(uint64_t layer, const std::string &path)
{
  if (!g_flash_wm) {
    return;
  }
  WindowManager::PropertiesFlash &f = g_flash_wm->properties_flash;
  if (f.layer == 0 || f.layer != layer || f.path != path) {
    return;
  }
  const double now = ImGui::GetTime();
  const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (f.start < 0.0) {
    f.start = now;
    ImGui::SetScrollFromPosY(lo.y - window->Pos.y, 0.5f);
  }
  const float fade = 1.0f - std::clamp(float(now - f.start), 0.0f, 1.0f);
  if (fade <= 0.0f) {
    return;
  }
  /* The whole row, across the window (not just the cell it is drawn in). */
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 a(window->InnerClipRect.Min.x + 2.0f, lo.y - 2.0f), b(window->InnerClipRect.Max.x - 2.0f, hi.y + 2.0f);
  dl->PushClipRect(window->InnerClipRect.Min, window->InnerClipRect.Max, false);
  dl->AddRectFilled(a, b, IM_COL32(0x47, 0x72, 0xb3, int(110.0f * fade)), 3.0f);
  dl->AddRect(a, b, IM_COL32(0x6f, 0x9c, 0xe8, int(230.0f * fade)), 3.0f, 0, 1.5f);
  dl->PopClipRect();
}

void ui_flash_update(WindowManager &wm)
{
  WindowManager::PropertiesFlash &f = wm.properties_flash;
  if (f.layer == 0) {
    return;
  }
  f.frames++;
  if (f.start >= 0.0 && ImGui::GetTime() - f.start > 1.0) {
    f = {};
  }
  else if (f.start < 0.0 && f.frames > 4) {
    if (!f.seen_properties) {
      report_banner(ReportType::Info, "Open a Properties editor to see this property", "",
                      "No Properties editor open");
    }
    f = {};
  }
}

float ui_key_button_width()
{
  /* The diamond, and the pin's place after it (kept even when empty, so fields line up). */
  return (ImGui::GetFrameHeight() + 2.0f) * 2.0f;
}

void ui_pin_draw(ImDrawList *dl, ImVec2 min, float size, unsigned int color)
{
  /* A push pin, leaning a little: a round head, a collar, and the needle. */
  const ImVec2 c(min.x + size * 0.5f, min.y + size * 0.5f);
  const float r = size * 0.36f;
  const auto at = [&](float x, float y) {
    /* Turned 30 degrees clockwise about the middle. */
    const float s = 0.5f, k = 0.866f;
    return ImVec2(c.x + (x * k - y * s) * r, c.y + (x * s + y * k) * r);
  };
  const ImVec2 head[4] = {at(-0.42f, -1.0f), at(0.42f, -1.0f), at(0.42f, -0.72f), at(-0.42f, -0.72f)};
  const ImVec2 body[4] = {at(-0.24f, -0.72f), at(0.24f, -0.72f), at(0.24f, -0.12f), at(-0.24f, -0.12f)};
  const ImVec2 collar[4] = {at(-0.58f, -0.12f), at(0.58f, -0.12f), at(0.58f, 0.08f), at(-0.58f, 0.08f)};
  dl->AddConvexPolyFilled(head, 4, color);
  dl->AddConvexPolyFilled(body, 4, color);
  dl->AddConvexPolyFilled(collar, 4, color);
  dl->AddLine(at(0.0f, 0.08f), at(0.0f, 1.0f), color, std::max(1.0f, size * 0.07f));
}

bool ui_pin_button(const char *id, int pin)
{
  const float h = ImGui::GetFrameHeight();
  ImGui::SameLine(0.0f, 2.0f);
  if (pin < 0) {
    ImGui::Dummy(ImVec2(h, h));
    return false;
  }
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
  const bool hovered = ImGui::IsItemHovered();
  const ImU32 col = pin == 1 ? (hovered ? IM_COL32(0xff, 0x70, 0x70, 255) : IM_COL32(0xe0, 0x40, 0x40, 255))
                             : (hovered ? IM_COL32(0xe0, 0xe0, 0xe0, 255) : IM_COL32(0x80, 0x80, 0x80, 255));
  ui_pin_draw(ImGui::GetWindowDrawList(), at, h, col);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(pin == 1 ? "Unpin from the Timeline" : "Pin to the Timeline: listed under Pinned, whatever is selected");
  }
  return pressed;
}

void ui_pinned_row()
{
  const ImU32 tint = IM_COL32(0xc0, 0x30, 0x30, 40);
  if (ImGui::GetCurrentTable()) {
    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, tint);
    return;
  }
  /* Outside a table: across the row just drawn. */
  const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
  const float left = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMin().x;
  const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(left, lo.y - 1.0f), ImVec2(right, hi.y + 1.0f), tint);
}

int ui_key_tint_push(int state)
{
  if (state <= 0) {
    return 0;
  }
  /* Blender's colours: green for animated, yellow for keyed on this frame. */
  const ImVec4 c = state == 2 ? ImVec4(0.56f, 0.50f, 0.13f, 1.0f) : ImVec4(0.28f, 0.45f, 0.20f, 1.0f);
  const ImVec4 hover(c.x * 1.15f, c.y * 1.15f, c.z * 1.15f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, c);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, hover);
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, hover);
  /* A ticked checkbox fills with this instead of the selection blue. */
  ImGui::PushStyleColor(ImGuiCol_CheckboxSelectedBg, state == 2 ? ImVec4(0.80f, 0.70f, 0.16f, 1.0f)
                                                                : ImVec4(0.36f, 0.62f, 0.26f, 1.0f));
  return 4;
}

bool ui_key_button(const char *id, int state)
{
  const float h = ImGui::GetFrameHeight();
  ImGui::SameLine(0.0f, 2.0f);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(h, h));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 c(at.x + h * 0.5f, at.y + h * 0.5f);
  const bool hovered = ImGui::IsItemHovered();
  const ImU32 col = hovered ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xc8, 0xc8, 0xc8, 255);
  if (state == 0) {
    dl->AddCircleFilled(c, h * 0.09f, col);
  }
  else {
    const float r = h * 0.28f;
    const ImVec2 d[4] = {ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y)};
    if (state == 2) {
      dl->AddConvexPolyFilled(d, 4, col);
    }
    else {
      dl->AddPolyline(d, 4, col, ImDrawFlags_Closed, 1.4f);
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip(state == 2 ? "Delete Keyframe" : "Insert Keyframe");
  }
  return pressed;
}

void ui_key_next(int state, std::function<void()> on_click, int pin, std::function<void()> on_pin,
                 uint64_t flash_layer, const std::string &flash_path)
{
  g_key_next = {true, state, std::move(on_click), pin, std::move(on_pin), flash_layer, flash_path};
}

/* After a row's fields: its key button, if one was asked for. */
static void key_button_finish(int pushed)
{
  if (!g_key_next.pending) {
    return;
  }
  ImGui::PopStyleColor(pushed);
  auto on_click = std::move(g_key_next.on_click);
  auto on_pin = std::move(g_key_next.on_pin);
  const int state = g_key_next.state, pin = g_key_next.pin;
  const uint64_t flash_layer = g_key_next.flash_layer;
  const std::string flash_path = g_key_next.flash_path;
  g_key_next = {};
  if (ui_key_button("##key", state) && on_click) {
    on_click();
  }
  ui_flash_row(flash_layer, flash_path);
  if (ui_pin_button("##pin", pin) && on_pin) {
    on_pin();
  }
  if (pin == 1) {
    ui_pinned_row();
  }
}

/* Label in the left column, right-aligned against its field as in Blender. */
static void ui_label(const char *label)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  const float label_width = ImGui::CalcTextSize(label).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetColumnWidth() - label_width));
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(g_key_next.pending ? -ui_key_button_width() : -FLT_MIN);
}

void ui_row(const char *label, const std::function<void(float width)> &body)
{
  ui_label(label);
  ImGui::PushID(label);
  const int pushed = g_key_next.pending ? ui_key_tint_push(g_key_next.state) : 0;
  body(ImGui::GetContentRegionAvail().x - (g_key_next.pending ? ui_key_button_width() : 0.0f));
  key_button_finish(pushed);
  ImGui::PopID();
}

/* A property's value field, at the next item width. `prefix` goes inside number fields ("X"). */
static void prop_field(Context &C, const rna::PointerRNA &ptr, const rna::PropertyRNA *prop, const char *python_path,
                       const char *prefix)
{
  ImGui::PushID(prop->identifier.c_str());
  const std::string int_format = prefix ? std::string(prefix) + "  %d" : std::string("%d");
  const std::string float_format = prefix ? std::string(prefix) + "  %.3f" : std::string("%.3f");
  ImGui::BeginDisabled(!prop->set);

  rna::Value value = prop->get(ptr.data);
  bool changed = false;
  switch (prop->type) {
    case rna::PropType::Bool:
      changed = ImGui::Checkbox("##value", &std::get<bool>(value));
      break;
    case rna::PropType::Int:
      changed = ImGui::DragInt("##value", &std::get<int>(value), 0.25f, int(prop->min), int(prop->max),
                               int_format.c_str(), ImGuiSliderFlags_AlwaysClamp);
      break;
    case rna::PropType::Float:
      changed = ImGui::DragFloat("##value", &std::get<float>(value), 0.01f, float(prop->min), float(prop->max),
                                 float_format.c_str(), ImGuiSliderFlags_AlwaysClamp);
      break;
    case rna::PropType::String:
      changed = ImGui::InputText("##value", &std::get<std::string>(value));
      break;
    case rna::PropType::Enum: {
      int &index = std::get<int>(value);
      const char *preview = (index >= 0 && index < int(prop->items.size())) ? prop->items[index].name.c_str() : "";
      if (ImGui::BeginCombo("##value", preview)) {
        for (int i = 0; i < int(prop->items.size()); i++) {
          if (ImGui::Selectable(prop->items[i].name.c_str(), i == index)) {
            index = i;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
      /* A combo edit is complete when picked, so it gets its undo step right away. */
      if (changed && prop->set) {
        prop->set(ptr.data, rna::clamp(*prop, value));
        if (C.undo && C.document) {
          C.undo->push("Change " + prop->name, *C.document);
        }
        changed = false;
      }
      break;
    }
  }
  if (changed && prop->set) {
    prop->set(ptr.data, rna::clamp(*prop, value));
  }
  if (ImGui::IsItemDeactivatedAfterEdit() && C.undo && C.document) {
    C.undo->push("Change " + prop->name, *C.document);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s\n\nPython: %s.%s", prop->description.c_str(), python_path, prop->identifier.c_str());
  }

  ImGui::EndDisabled();
  ImGui::PopID();
}

void ui_prop(Context &C, const rna::PointerRNA &ptr, const char *identifier, const char *python_path)
{
  const rna::PropertyRNA *prop = ptr.type->find(identifier);
  if (!prop) {
    return;
  }
  ui_label(prop->name.c_str());
  ImGui::PushID(identifier);
  const int pushed = g_key_next.pending ? ui_key_tint_push(g_key_next.state) : 0;
  uint64_t dl = g_key_next.flash_layer;
  std::string dp = g_key_next.flash_path;
  /* Not keyed: a layer's own setting may still be driven, as Blender drives any setting
   * (anim/drivers.cc says which). */
  if (!g_key_next.pending && ptr.type == &Layer::rna()) {
    dl = static_cast<const Layer *>(ptr.data)->id;
    dp = std::string("props.") + identifier;
  }
  const int driven = ui_driver_field_begin(C, dl, dp, -1);
  prop_field(C, ptr, prop, python_path, nullptr);
  ui_driver_typed(C, dl, dp, -1);
  ui_driver_field_end(C, dl, dp, -1, driven);
  key_button_finish(pushed);
  ImGui::PopID();
}

void ui_prop_check(Context &C, const rna::PointerRNA &ptr, const char *identifier, const char *python_path,
                   const char *heading)
{
  const rna::PropertyRNA *prop = ptr.type->find(identifier);
  if (!prop || prop->type != rna::PropType::Bool) {
    return;
  }
  ui_label(heading);
  ImGui::PushID(identifier);
  ImGui::BeginDisabled(!prop->set);
  bool value = std::get<bool>(prop->get(ptr.data));
  if (ImGui::Checkbox(prop->name.c_str(), &value) && prop->set) {
    prop->set(ptr.data, value);
    if (C.undo && C.document) {
      C.undo->push("Change " + prop->name, *C.document);
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s\n\nPython: %s.%s", prop->description.c_str(), python_path, prop->identifier.c_str());
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}

void ui_prop_pair(Context &C, const rna::PointerRNA &ptr, const char *label, const char *first,
                  const char *first_prefix, const char *second, const char *second_prefix, const char *python_path)
{
  const rna::PropertyRNA *a = ptr.type->find(first);
  const rna::PropertyRNA *b = ptr.type->find(second);
  if (!a || !b) {
    return;
  }
  ui_label(label);
  ImGui::PushID(label);
  const bool keyed = g_key_next.pending;
  const int pushed = keyed ? ui_key_tint_push(g_key_next.state) : 0;
  const float spacing = 2.0f;
  const float avail = ImGui::GetContentRegionAvail().x - (keyed ? ui_key_button_width() : 0.0f);
  const float w = (avail - spacing) * 0.5f;
  const uint64_t dl = g_key_next.flash_layer;
  const std::string dp = g_key_next.flash_path;
  /* Each of the pair is one part of the channel (a position's X and Y), driven on its own. */
  int driven = ui_driver_field_begin(C, dl, dp, 0);
  ImGui::SetNextItemWidth(w);
  prop_field(C, ptr, a, python_path, first_prefix);
  ui_driver_typed(C, dl, dp, 0);
  ui_driver_field_end(C, dl, dp, 0, driven);
  ImGui::SameLine(0.0f, spacing);
  driven = ui_driver_field_begin(C, dl, dp, 1);
  ImGui::SetNextItemWidth(keyed ? w : -FLT_MIN);
  prop_field(C, ptr, b, python_path, second_prefix);
  ui_driver_typed(C, dl, dp, 1);
  ui_driver_field_end(C, dl, dp, 1, driven);
  key_button_finish(pushed);
  ImGui::PopID();
}

bool ui_color(Context &C, const char *label, float rgba[4], const char *undo_name, bool *r_revert, bool *r_pick,
              bool pick_active)
{
  ui_label(label);
  ImGui::PushID(label);
  bool finished = false;
  const float revert_w = (r_revert ? ui_revert_button_space() : 0.0f) + (r_pick ? ui_revert_button_space() : 0.0f);
  uint64_t dl = 0;
  std::string dp;
  ui_driver_next_take(&dl, &dp);
  const int driven = ui_driver_field_begin(C, dl, dp, -1);
  const bool changed = ui_color_edit("##color", rgba, true, &finished, revert_w);
  ui_driver_field_end(C, dl, dp, -1, driven);
  if (finished && C.undo && C.document && undo_name) {
    C.undo->push(undo_name, *C.document);
  }
  if (r_revert) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    *r_revert = ui_revert_button("##revert", "Revert to the original's colour");
  }
  if (r_pick) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    const std::string tip = std::string("Eyedropper: click a shape in the viewport to take its ") + label;
    *r_pick = ui_pick_button("##pick", tip.c_str(), pick_active);
  }
  ImGui::PopID();
  return changed;
}

/* A report's icon, as Blender's: solid `ink` with its mark cut out in `cut` (the box's colour): a
 * rounded square with "i" (info), a triangle with "!" (warning, ICON_ERROR), a disc with an X (error,
 * ICON_CANCEL). */
static void report_icon(ImDrawList *dl, ImVec2 c, float size, ReportType type, ImU32 ink, ImU32 cut)
{
  const float r = size * 0.42f;
  if (type == ReportType::Warning) {
    dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 1.05f), ImVec2(c.x + r * 1.15f, c.y + r * 0.9f),
                          ImVec2(c.x - r * 1.15f, c.y + r * 0.9f), ink);
    dl->AddLine(ImVec2(c.x, c.y - r * 0.4f), ImVec2(c.x, c.y + r * 0.25f), cut, 1.8f);
    dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.58f), 1.2f, cut);
    return;
  }
  if (type == ReportType::Error) {
    dl->AddCircleFilled(c, r, ink, 24);
    const float x = r * 0.45f;
    dl->AddLine(ImVec2(c.x - x, c.y - x), ImVec2(c.x + x, c.y + x), cut, 1.8f);
    dl->AddLine(ImVec2(c.x - x, c.y + x), ImVec2(c.x + x, c.y - x), cut, 1.8f);
    return;
  }
  dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), ink, r * 0.45f);
  dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.5f), 1.3f, cut);
  dl->AddLine(ImVec2(c.x, c.y - r * 0.15f), ImVec2(c.x, c.y + r * 0.6f), cut, 1.8f);
}

void ui_status_bar_draw()
{
  /* As tall as Blender's (a button and a margin), so the banner's boxes fit with room round them. */
  const float h = ImGui::GetFrameHeight() + 6.0f;
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0x18, 0x18, 0x18, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 0.0f));
  const bool open = ImGui::BeginViewportSideBar("##status_bar", ImGui::GetMainViewport(), ImGuiDir_Down, h,
                                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  if (!open) {
    ImGui::End();
    return;
  }
  /* The reports banner, as Blender's: a new report flashes in, stays a while, then fades. */
  static uint64_t seen = 0;
  static double since = -100.0;
  const BannerReport &r = banner_report();
  const double now = ImGui::GetTime();
  if (r.serial != seen) {
    seen = r.serial;
    since = now;
  }
  const double age = now - since;
  const double stay = r.type == ReportType::Info ? 4.0 : 8.0, fade = 1.0;
  const float alpha = age < stay ? 1.0f : age < stay + fade ? float(1.0 - (age - stay) / fade) : 0.0f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (r.serial != 0 && alpha > 0.0f) {
    std::string text = r.message;
    if (!r.title.empty()) {
      text = r.hint.empty() ? r.message : r.hint;
    }
    ImFont *bold = theme_font_bold();
    const float fs = ImGui::GetFontSize();
    const float title_w = r.title.empty() ? 0.0f : bold->CalcTextSizeA(fs, FLT_MAX, 0.0f, (r.title + "  ").c_str()).x;
    /* As Blender's banner: a box in the report's colour holding its icon, joined to a box of the same
     * colour, faint, holding the message; in the middle of the status bar, at most 800 wide. */
    const float unit = h, icon_w = unit + 6.0f;
    const float text_w = std::min(title_w + ImGui::CalcTextSize(text.c_str()).x, 800.0f);
    const float box_w = icon_w + text_w + unit * 0.75f;
    const float left = at.x + std::max(0.0f, (ImGui::GetContentRegionAvail().x - box_w) * 0.5f);
    const ImVec2 min(left, at.y + 2.0f), max(left + box_w, at.y + h - 2.0f);
    const auto a = [&](int v) { return int(float(v) * alpha); };
    /* Blender's colours: the icon's box in the report's colour, the message's the same at a quarter
     * over the bar, a grey outline round both; a new report brightens the icon's box briefly. */
    int c[3];
    if (r.type == ReportType::Error) {
      c[0] = 0x9b, c[1] = 0x1c, c[2] = 0x1c;
    }
    else if (r.type == ReportType::Warning) {
      c[0] = 0xac, c[1] = 0x87, c[2] = 0x37;
    }
    else {
      c[0] = 0x28, c[1] = 0x48, c[2] = 0x7d;
    }
    const int brighten = age < 0.6 ? int(32.0 * (1.0 - age / 0.6)) : 0;
    const auto lit = [&](int v) { return std::min(255, v + brighten); };
    const auto quarter = [&](int v) { return 0x18 + (v - 0x18) / 4; };
    const ImU32 icon_bg = IM_COL32(lit(c[0]), lit(c[1]), lit(c[2]), a(255));
    const ImU32 text_bg = IM_COL32(quarter(c[0]), quarter(c[1]), quarter(c[2]), a(255));
    const ImU32 ink = IM_COL32(0xff, 0xff, 0xff, a(255));
    const ImU32 text_col = IM_COL32(0xcc, 0xcc, 0xcc, a(255));
    const float round = (max.y - min.y) * 0.2f;
    const float split = min.x + icon_w;
    dl->AddRectFilled(min, ImVec2(split, max.y), icon_bg, round, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(split, min.y), max, text_bg, round, ImDrawFlags_RoundCornersRight);
    dl->AddRect(min, max, IM_COL32(0x3d, 0x3d, 0x3d, a(255)), round, 0, 1.0f);
    report_icon(dl, ImVec2(min.x + icon_w * 0.5f, (min.y + max.y) * 0.5f), fs, r.type, ink, icon_bg);
    float x = split + unit * 0.35f;
    const float y = (min.y + max.y - fs) * 0.5f;
    if (!r.title.empty()) {
      dl->AddText(bold, fs, ImVec2(x, y), text_col, r.title.c_str());
      x += title_w;
    }
    dl->PushClipRect(ImVec2(x, min.y), ImVec2(max.x - unit * 0.3f, max.y), true);
    dl->AddText(ImVec2(x, y), text_col, text.c_str());
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(min);
    if (ImGui::InvisibleButton("##report_banner", max - min)) {
      ImGui::OpenPopup("##recent_reports");
    }
    if (ImGui::IsItemHovered()) {
      /* All of it, and it stays while looked at. */
      since = std::max(since, now - stay + 0.5);
      ImGui::SetTooltip("%s%s%s\n\nClick: recent reports", r.title.empty() ? "" : (r.title + "\n").c_str(),
                        r.message.c_str(), r.hint.empty() ? "" : ("\n" + r.hint).c_str());
    }
  }
  /* The recent reports, newest first, as the Info editor lists them. */
  if (ImGui::BeginPopup("##recent_reports")) {
    int shown = 0;
    const std::vector<Report> &all = reports();
    for (auto it = all.rbegin(); it != all.rend() && shown < 15; ++it) {
      if (it->type == ReportType::Operator) {
        continue;
      }
      const ImVec4 col = it->type == ReportType::Error   ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
                         : it->type == ReportType::Warning ? ImVec4(0.95f, 0.75f, 0.3f, 1.0f)
                                                           : ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
      ImGui::TextColored(col, "%s", it->message.c_str());
      shown++;
    }
    if (shown == 0) {
      ImGui::TextDisabled("No reports yet");
    }
    ImGui::EndPopup();
  }
  ImGui::End();
}

float ui_revert_button_space()
{
  return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
}

bool ui_revert_button(const char *id, const char *tooltip)
{
  const float h = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::Button(id, ImVec2(h, h));
  if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tooltip);
  }
  /* A curved arrow turning back. */
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 c(at.x + h * 0.5f, at.y + h * 0.5f);
  const float r = h * 0.26f;
  const ImU32 col = IM_COL32(0xe6, 0xe6, 0xe6, 255);
  dl->PathArcTo(c, r, -0.4f * 3.14159265f, 1.2f * 3.14159265f, 14);
  dl->PathStroke(col, 0, 1.5f);
  /* The arrowhead at the arc's start, pointing back. */
  const ImVec2 tip(c.x + r * 0.809f, c.y - r * 0.588f);
  dl->AddTriangleFilled(ImVec2(tip.x - r * 0.55f, tip.y - r * 0.15f), ImVec2(tip.x + r * 0.35f, tip.y - r * 0.55f),
                        ImVec2(tip.x + r * 0.2f, tip.y + r * 0.45f), col);
  return clicked;
}

/* --- Colour picker and eyedropper ------------------------------------------------------------- */

namespace {

struct Eyedropper {
  bool active = false;
  /* The colour edit that started it; its result waits for that widget to draw again. */
  ImGuiID owner = 0;
  bool sample_pending = false;
  ImVec2 sample_pos;
  bool has_result = false;
  float rgb[3] = {0, 0, 0};
};
Eyedropper g_eyedropper;

/* A pipette, as on Blender's eyedropper button. */
void eyedropper_icon(ImDrawList *dl, ImVec2 c, float s, ImU32 col)
{
  const ImVec2 tip(c.x - s * 0.7f, c.y + s * 0.7f), bulb(c.x + s * 0.45f, c.y - s * 0.45f);
  dl->AddLine(tip, bulb, col, std::max(1.5f, s * 0.22f));
  dl->AddCircleFilled(ImVec2(bulb.x + s * 0.2f, bulb.y - s * 0.2f), s * 0.32f, col);
  dl->AddLine(ImVec2(bulb.x - s * 0.35f, bulb.y - s * 0.35f), ImVec2(bulb.x + s * 0.35f, bulb.y + s * 0.35f), col,
              std::max(1.5f, s * 0.2f));
}

}  // namespace

bool ui_color_edit(const char *id, float rgba[4], bool hex, bool *r_finished, float right_space, float swatch_width)
{
  ImGui::PushID(id);
  const ImGuiID owner = ImGui::GetID("##eyedropper");
  bool changed = false, finished = false;
  /* A colour the eyedropper picked for this widget: its RGB, keeping the alpha, as in Blender. */
  if (g_eyedropper.has_result && g_eyedropper.owner == owner) {
    rgba[0] = g_eyedropper.rgb[0];
    rgba[1] = g_eyedropper.rgb[1];
    rgba[2] = g_eyedropper.rgb[2];
    g_eyedropper.has_result = false;
    changed = finished = true;
  }

  const float h = ImGui::GetFrameHeight();
  const ImGuiColorEditFlags show = ImGuiColorEditFlags_AlphaPreviewHalf;
  if (ImGui::ColorButton("##swatch", ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]), show,
                         ImVec2(swatch_width > 0.0f ? swatch_width : h * 1.6f, h))) {
    ImGui::OpenPopup("##picker");
  }
  if (hex) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(right_space > 0.0f ? -right_space : -FLT_MIN);
    changed |= ImGui::ColorEdit4("##hex", rgba,
                                 ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_NoSmallPreview |
                                     ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoOptions |
                                     ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar);
    finished |= ImGui::IsItemDeactivatedAfterEdit();
  }

  /* The picker remembers whether it changed the colour, to report the edit's end on close. */
  bool &picker_dirty = *ImGui::GetStateStorage()->GetBoolRef(ImGui::GetID("##picker_dirty"), false);
  if (ImGui::BeginPopup("##picker")) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##eyedropper", ImVec2(h, h))) {
      g_eyedropper = {};
      g_eyedropper.active = true;
      g_eyedropper.owner = owner;
      ImGui::CloseCurrentPopup();
    }
    eyedropper_icon(ImGui::GetWindowDrawList(), ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.32f,
                    IM_COL32(0xe6, 0xe6, 0xe6, 255));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Eyedropper: click anywhere to pick its colour (Esc or right-click cancels)");
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Eyedropper");
    ImGui::SetNextItemWidth(h * 9.0f);
    if (ImGui::ColorPicker4("##pick", rgba,
                            ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_AlphaBar |
                                ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_PickerHueBar))
    {
      changed = true;
      picker_dirty = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  else if (picker_dirty) {
    picker_dirty = false;
    finished = true;
  }
  ImGui::PopID();
  if (r_finished) {
    *r_finished = finished;
  }
  return changed;
}

void ui_eyedropper_icon(ImDrawList *dl, ImVec2 center, float size, ImU32 color)
{
  eyedropper_icon(dl, center, size, color);
}

namespace {

struct LayerEyedropper {
  bool active = false;
  uint64_t armature = 0, bone = 0;
  /* Picking for something other than a bone's shape: what to do with the layer clicked (true when
   * done), and what to say at the mouse. */
  std::function<bool(Context &, uint64_t)> pick;
  /* Picking a shape (ui_shape_eyedropper_start) rather than a layer. */
  std::function<bool(Context &, uint64_t, int)> pick_shape;
  std::function<std::string()> cue;
  /* What started a shape eyedropper (ui_shape_eyedropper_active(tag)). */
  std::string tag;
  /* A click this frame went to it (a layer, or nothing under the mouse): not a click elsewhere. */
  bool took_click = false;
};
LayerEyedropper g_layer_eyedropper;
/* The document the eyedropper's cue names layers from. */
const Document *g_doc_for_cue = nullptr;

}  // namespace

void ui_bone_collection_menus(Context &C, bool hovered)
{
  WindowManager &wm = *C.wm;
  if (hovered && (wm.menu_request == "bone_collection_move" || wm.menu_request == "bone_collection_assign")) {
    ImGui::OpenPopup(wm.menu_request == "bone_collection_move" ? "##bone_collection_move" : "##bone_collection_assign");
    wm.menu_request.clear();
  }
  Document &doc = *C.document;
  Layer *armature = collection_armature(doc);
  for (const bool move : {true, false}) {
    if (!ui_menu_begin(move ? "##bone_collection_move" : "##bone_collection_assign")) {
      continue;
    }
    ImGui::TextDisabled(move ? "Move to Collection" : "Assign to Collection");
    ImGui::Separator();
    const char *op = move ? "armature.move_to_collection" : "armature.assign_to_collection";
    if (armature) {
      const bool bones = doc.active_layer == armature->id;
      const std::vector<Layer *> drawings = collection_drawings(doc, *armature);
      for (const BoneCollection &c : armature->bone_collections) {
        /* Ticked: something it would move is in it already. */
        const auto in = [&](const std::vector<uint64_t> &ids) {
          return std::find(ids.begin(), ids.end(), c.id) != ids.end();
        };
        bool has = std::any_of(drawings.begin(), drawings.end(), [&](const Layer *d) { return in(d->collections); });
        for (const Bone &b : armature->bones) {
          has |= bones && (b.selected || b.select_head || b.select_tail) && in(b.collections);
        }
        if (ui_menu_item(c.name.c_str(), nullptr, has)) {
          operator_call(C, op, {{"collection", c.name}});
        }
      }
      if (!armature->bone_collections.empty()) {
        ImGui::Separator();
      }
    }
    if (ui_menu_item("New Collection...")) {
      operator_call(C, op, {{"new", "1"}});
    }
    ui_menu_end();
  }
}

void ui_layer_eyedropper_start(uint64_t armature, uint64_t bone)
{
  g_layer_eyedropper = {true, armature, bone, {}, {}, {}, {}, false};
}

void ui_layer_eyedropper_start_with(std::function<bool(Context &, uint64_t)> pick, std::function<std::string()> cue)
{
  g_layer_eyedropper = {true, 0, 0, std::move(pick), {}, std::move(cue), {}, false};
}

void ui_shape_eyedropper_start(std::function<bool(Context &, uint64_t, int)> pick, std::function<std::string()> cue,
                               const char *tag)
{
  g_layer_eyedropper = {true, 0, 0, {}, std::move(pick), std::move(cue), tag ? tag : "", false};
}

bool ui_shape_eyedropper_active()
{
  return g_layer_eyedropper.active && g_layer_eyedropper.pick_shape != nullptr;
}

bool ui_shape_eyedropper_active(const char *tag)
{
  return ui_shape_eyedropper_active() && g_layer_eyedropper.tag == tag;
}

void ui_style_eyedropper_start(const char *what, const char *what_label)
{
  const std::string setting = what, label = what_label;
  ui_shape_eyedropper_start(
      [setting](Context &C, uint64_t layer, int shape) {
        operator_call(C, "mesh.style_copy_from",
                      {{"layer", std::to_string(layer)}, {"shape", std::to_string(shape)}, {"what", setting}});
        return true;
      },
      [label] { return "Click a shape to take its " + label + "  |  Esc cancels"; }, what);
}

bool ui_pick_button(const char *id, const char *tooltip, bool active)
{
  const float h = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  /* A click while it's on stops it, as any click off a shape does. */
  const bool clicked = ImGui::Button(id, ImVec2(h, h)) && !active;
  if (active) {
    ImGui::PopStyleColor();
  }
  if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", active ? "Picking: click a shape in the viewport (Esc stops)" : tooltip);
  }
  ui_eyedropper_icon(ImGui::GetWindowDrawList(), ImVec2(at.x + h * 0.5f, at.y + h * 0.5f), h * 0.32f,
                     ImGui::GetColorU32(ImGuiCol_Text));
  return clicked;
}

void ui_shape_eyedropper_pick(Context &C, uint64_t layer, int shape)
{
  LayerEyedropper &e = g_layer_eyedropper;
  if (!e.active || !e.pick_shape) {
    return;
  }
  e.took_click = true;
  if (layer != 0 && shape >= 0 && e.pick_shape(C, layer, shape)) {
    e.active = false;
  }
}

void ui_draw_order_pick_update(Context &C)
{
  WindowManager::DrawOrderPick &pick = C.wm->draw_order_pick;
  if (pick.mode == 0) {
    return;
  }
  if (pick.started) {
    if (!ui_layer_eyedropper_active()) {
      /* Done, or cancelled (Esc, right-click, a click elsewhere). */
      pick = {};
    }
    return;
  }
  pick.started = true;
  WindowManager *wm = C.wm;
  ui_layer_eyedropper_start_with(
      [wm](Context &C, uint64_t target) {
        WindowManager::DrawOrderPick &p = wm->draw_order_pick;
        Document &doc = *C.document;
        for (const uint64_t id : p.layers) {
          if (layer_inside(doc, target, id) || layer_inside(doc, id, target)) {
            report_banner(ReportType::Warning, "Pick a layer outside the one you're moving",
                            "A layer goes in front of or behind a layer outside it", "That's the layer being moved");
            return false;
          }
        }
        const std::string mode = p.mode > 0 ? "FRONT" : "BEHIND";
        for (const uint64_t id : std::vector<uint64_t>(p.layers)) {
          operator_call(C, "layer.draw_order_set",
                        {{"layer", std::to_string(id)}, {"mode", mode}, {"target", std::to_string(target)}});
        }
        p.hover = 0;
        return true;
      },
      [wm]() {
        const WindowManager::DrawOrderPick &p = wm->draw_order_pick;
        std::string text = p.mode > 0 ? "Click the layer to go in front of" : "Click the layer to go behind";
        if (p.hover && g_doc_for_cue) {
          if (const Layer *l = layer_find(*g_doc_for_cue, p.hover)) {
            text = std::string(p.mode > 0 ? "In front of " : "Behind ") + l->name + " (click)";
          }
        }
        return text + "  |  Esc cancels";
      });
}

bool ui_draw_order_preview_begin(Document &doc, std::vector<std::pair<Layer *, float>> &r_saved, uint64_t target,
                                 int mode, const std::vector<uint64_t> &layers)
{
  g_doc_for_cue = &doc;
  if (target == 0 || mode == 0) {
    return false;
  }
  for (const uint64_t id : layers) {
    if (layer_inside(doc, target, id) || layer_inside(doc, id, target)) {
      return false;
    }
  }
  for (const uint64_t id : layers) {
    if (Layer *l = layer_find(doc, id)) {
      r_saved.push_back({l, l->draw_order});
      l->draw_order = mode > 0 ? float(target) : -float(target);
    }
  }
  return !r_saved.empty();
}

void ui_draw_order_preview_end(std::vector<std::pair<Layer *, float>> &saved)
{
  for (const auto &[l, v] : saved) {
    l->draw_order = v;
  }
  saved.clear();
}

void ui_mask_pick_update(Context &C)
{
  WindowManager::MaskPick &pick = C.wm->mask_pick;
  if (!pick.active) {
    return;
  }
  if (pick.started) {
    if (!ui_layer_eyedropper_active()) {
      pick = {};
    }
    return;
  }
  pick.started = true;
  WindowManager *wm = C.wm;
  ui_layer_eyedropper_start_with(
      [wm](Context &C, uint64_t source) {
        WindowManager::MaskPick &p = wm->mask_pick;
        Document &doc = *C.document;
        if (std::find(p.layers.begin(), p.layers.end(), source) != p.layers.end()) {
          report_banner(ReportType::Warning, "Pick another layer: a layer can't be its own mask",
                          "Click the layer whose shape it should show inside or outside", "That's the layer being masked");
          return false;
        }
        for (const uint64_t id : std::vector<uint64_t>(p.layers)) {
          const Layer *layer = layer_find(doc, id);
          if (!layer) {
            continue;
          }
          if (p.index >= 0 && p.index < int(layer->masks.size())) {
            /* Joins that mask: inside or outside any of its layers. */
            std::string sources;
            for (const uint64_t l : layer->masks[size_t(p.index)].layers) {
              sources += std::to_string(l) + "|";
            }
            sources += std::to_string(source);
            operator_call(C, "mask.set",
                          {{"layer", std::to_string(id)}, {"index", std::to_string(p.index)}, {"sources", sources}});
          }
          else {
            OpArgs add = {{"layer", std::to_string(id)}, {"sources", std::to_string(source)}};
            if (!p.mode.empty()) {
              add["mode"] = p.mode;
            }
            operator_call(C, "mask.add", add);
          }
        }
        return true;
      },
      [wm]() {
        const WindowManager::MaskPick &p = wm->mask_pick;
        const char *what = p.index >= 0              ? "Click the layer to join the mask row"
                           : p.mode == "SUBTRACT"   ? "Click the layer to cut out (Subtract)"
                           : p.mode == "INTERSECT"  ? "Click the layer to keep only what's inside (Intersect)"
                           : p.mode == "DIFFERENCE" ? "Click the layer to flip (Difference)"
                                                    : "Click the layer to show it inside of (Add)";
        return std::string(what) + "  |  Esc cancels";
      });
}

void ui_mask_menu_items(Context &C, uint64_t layer_id)
{
  Document &doc = *C.document;
  OpArgs args;
  if (layer_id != 0) {
    args["layer"] = std::to_string(layer_id);
  }
  const Layer *layer = layer_id ? layer_find(doc, layer_id) : document_active_layer(doc);
  if (layer && !layer->masks.empty()) {
    for (const LayerMask &m : layer->masks) {
      const std::string text = mask_label(doc, m) + (m.enabled ? "" : " (off)");
      ImGui::TextDisabled("%s", text.c_str());
    }
    ImGui::Separator();
  }
  const auto item = [&](const char *label, const char *idname, OpArgs extra = {}) {
    const std::string shortcut = keymaps().shortcut_for(idname);
    if (ui_menu_item(label, shortcut.empty() ? nullptr : shortcut.c_str())) {
      for (const auto &[k, v] : args) {
        extra[k] = v;
      }
      operator_call(C, idname, extra);
    }
  };
  {
    const std::string shortcut = keymaps().shortcut_for("mask.clip_to_below");
    if (ImGui::MenuItem("Clip to Below", shortcut.empty() ? nullptr : shortcut.c_str(), layer && layer->clip_to_below)) {
      operator_call(C, "mask.clip_to_below", args);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Show it only where the layer below shows (Alt-click the line between them in the Layers editor)");
    }
  }
  item("Add Layer...", "mask.pick", {{"mode", "ADD"}});
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Then click the layer to show it inside of (viewport or Layers editor)");
  }
  item("Subtract Layer...", "mask.pick", {{"mode", "SUBTRACT"}});
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Then click the layer to cut out of it (viewport or Layers editor)");
  }
  ImGui::Separator();
  if (ui_menu_item("Remove Masks", nullptr, false, layer && !layer->masks.empty())) {
    operator_call(C, "mask.remove", args);
  }
}

void ui_draw_order_menu_items(Context &C, uint64_t layer_id)
{
  Document &doc = *C.document;
  OpArgs args;
  if (layer_id != 0) {
    args["layer"] = std::to_string(layer_id);
  }
  const Layer *layer = layer_id ? layer_find(doc, layer_id) : document_active_layer(doc);
  if (layer) {
    const std::string now = "Now: " + draw_order_label(doc, layer->draw_order);
    ImGui::TextDisabled("%s", now.c_str());
    ImGui::Separator();
  }
  const auto item = [&](const char *label, const char *idname, OpArgs extra = {}) {
    const std::string shortcut = keymaps().shortcut_for(idname);
    if (ui_menu_item(label, shortcut.empty() ? nullptr : shortcut.c_str())) {
      for (const auto &[k, v] : args) {
        extra[k] = v;
      }
      operator_call(C, idname, extra);
    }
  };
  item("Bring to Front", "layer.draw_order_front");
  item("Bring Forward", "layer.draw_order_forward");
  item("Send Backward", "layer.draw_order_backward");
  item("Send to Back", "layer.draw_order_back");
  ImGui::Separator();
  item("In Front Of...", "layer.draw_order_pick", {{"mode", "FRONT"}});
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Then click the layer to go in front of, in the viewport or the Layers editor");
  }
  item("Behind...", "layer.draw_order_pick", {{"mode", "BEHIND"}});
  ImGui::Separator();
  const bool ruled = layer && layer->draw_order != 0.0f;
  if (ui_menu_item("Normal", nullptr, false, ruled)) {
    OpArgs extra = args;
    extra["mode"] = "NORMAL";
    operator_call(C, "layer.draw_order_set", extra);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Back where the Layers editor has it");
  }
}

bool ui_layer_eyedropper_active()
{
  return g_layer_eyedropper.active;
}

void ui_layer_eyedropper_pick(Context &C, uint64_t layer_id)
{
  if (!g_layer_eyedropper.active) {
    return;
  }
  g_layer_eyedropper.took_click = true;
  /* A shape is picked in the viewport: a layer row says nothing about which. */
  if (layer_id == 0 || g_layer_eyedropper.pick_shape) {
    return;
  }
  if (g_layer_eyedropper.pick) {
    if (g_layer_eyedropper.pick(C, layer_id)) {
      g_layer_eyedropper.active = false;
    }
    return;
  }
  Document &doc = *C.document;
  const Layer *layer = layer_find(doc, layer_id);
  Layer *armature = layer_find(doc, g_layer_eyedropper.armature);
  Bone *bone = armature ? bone_find(*armature, g_layer_eyedropper.bone) : nullptr;
  if (!bone) {
    g_layer_eyedropper.active = false;
    return;
  }
  if (!layer || layer->type != LayerType::Vector) {
    report_banner(ReportType::Warning, "A custom bone shape is a vector layer's drawing", "Pick a vector layer",
                    "Not a vector layer");
    return;
  }
  bone->shape = BoneShape::Custom;
  bone->shape_layer = layer->id;
  g_layer_eyedropper.active = false;
  C.undo->push("Bone Shape Layer", doc);
}

void ui_layer_eyedropper_overlay()
{
  LayerEyedropper &e = g_layer_eyedropper;
  if (!e.active) {
    return;
  }
  const ImGuiIO &io = ImGui::GetIO();
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
      (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !e.took_click))
  {
    e.active = false;
    return;
  }
  e.took_click = false;
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const ImVec2 m = io.MousePos;
  eyedropper_icon(dl, ImVec2(m.x + 14.0f, m.y - 14.0f), 7.0f, IM_COL32(255, 255, 255, 230));
  const std::string cue = e.cue ? e.cue() : std::string("Click a layer (viewport or Layers editor)  |  Esc cancels");
  dl->AddText(ImVec2(m.x + 26.0f, m.y - 22.0f), IM_COL32(255, 255, 255, 230), cue.c_str());
}

void ui_eyedropper_overlay()
{
  if (!g_eyedropper.active) {
    return;
  }
  /* A clear window over everything catches the click, so nothing underneath reacts to it. */
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::SetNextWindowFocus();
  ImGui::Begin("##eyedropper_overlay", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
  ImGui::InvisibleButton("##catch", viewport->Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const ImGuiIO &io = ImGui::GetIO();
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    g_eyedropper.sample_pending = true;
    g_eyedropper.sample_pos = io.MousePos;
  }
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    g_eyedropper.active = false;
  }
  /* A ring around the pixel that will be picked, leaving the pixel itself clear. */
  ImDrawList *dl = ImGui::GetForegroundDrawList();
  const ImVec2 m = io.MousePos;
  dl->AddCircle(m, 9.0f, IM_COL32(0, 0, 0, 220), 24, 3.0f);
  dl->AddCircle(m, 9.0f, IM_COL32(255, 255, 255, 230), 24, 1.5f);
  eyedropper_icon(dl, ImVec2(m.x + 18.0f, m.y - 18.0f), 7.0f, IM_COL32(255, 255, 255, 230));
  dl->AddText(ImVec2(m.x + 30.0f, m.y - 26.0f), IM_COL32(255, 255, 255, 230), "Click to pick  |  Esc cancels");
  ImGui::End();
}

void ui_eyedropper_sample(int framebuffer_height)
{
  if (!g_eyedropper.sample_pending) {
    return;
  }
  g_eyedropper.sample_pending = false;
  g_eyedropper.active = false;
  const ImGuiIO &io = ImGui::GetIO();
  const int x = int(g_eyedropper.sample_pos.x * io.DisplayFramebufferScale.x);
  const int y = framebuffer_height - 1 - int(g_eyedropper.sample_pos.y * io.DisplayFramebufferScale.y);
  unsigned char pixel[4] = {0, 0, 0, 0};
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
  for (int i = 0; i < 3; i++) {
    g_eyedropper.rgb[i] = float(pixel[i]) / 255.0f;
  }
  g_eyedropper.has_result = true;
}

/* ---------------------------------------------------------------------------------
 * Editors
 */

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

/* Dragging the grip moves the area, the way dragging a tab does in ImGui: the window is
 * undocked and follows the mouse, and dropping it on a docking target docks it there. */
static void area_grip_drag_start(const ImRect &grip_rect)
{
  ImGuiContext &g = *ImGui::GetCurrentContext();
  ImGuiWindow *window = ImGui::GetCurrentWindow()->RootWindow;
  if (window->DockNode) {
    ImGui::DockContextQueueUndockWindow(&g, window);
    g.MovingWindow = window;
    ImGui::SetActiveID(window->MoveId, window);
    g.ActiveIdClickOffset -= window->Pos - grip_rect.Min;
    g.ActiveIdNoClearOnFocusLoss = true;
    ImGui::SetActiveIdUsingAllKeyboardKeys();
  }
  else {
    ImGui::StartMouseMovingWindow(window);
  }
  /* ImGui only offers docking targets when the window was grabbed within its top strip, where
   * a title bar would be. Areas have no title bar, so keep the grab point inside that strip. */
  g.ActiveIdClickOffset.y = std::min(g.ActiveIdClickOffset.y, ImGui::GetFrameHeight() - 1.0f);
}

/* Two horizontal lines to grab the area by, with Blender's area options on right-click.
 * The grip starts at the area's left edge and holds equal margins on both sides of the lines,
 * so they sit centred between the edge and the dropdown that follows, and centred vertically
 * on the header row. Sizes are whole pixels so both lines stay crisp. */
static void area_grip(Area &area)
{
  const float h = ImGui::GetFrameHeight();
  const float margin = std::round(ImGui::GetStyle().ItemSpacing.x);
  const float line_w = std::round(h * 0.45f);
  const float thickness = std::max(1.0f, std::round(h * 0.07f));
  const float line_gap = std::max(2.0f, std::round(h * 0.16f));
  const ImVec2 size(margin * 2.0f + line_w, h);

  const ImVec2 p(std::round(ImGui::GetWindowPos().x), std::round(ImGui::GetCursorScreenPos().y));
  ImGui::SetCursorScreenPos(p);
  ImGui::InvisibleButton("##area_grip", size);
  const bool hovered = ImGui::IsItemHovered();
  const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, ImGui::GetFontSize() * 0.3f)) {
    area_grip_drag_start(rect);
  }
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
      ImGui::OpenPopup("##area_menu");
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Drag to move this area\nRight-click for area options");
  }

  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImU32 col = hovered || ImGui::IsItemActive() ? IM_COL32(0xff, 0xff, 0xff, 255)
                                                     : IM_COL32(0x8c, 0x8c, 0x8c, 255);
  const float block_h = thickness * 2.0f + line_gap;
  const float x0 = p.x + margin;
  const float y0 = p.y + std::round((h - block_h) * 0.5f);
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + line_w, y0 + thickness), col);
  dl->AddRectFilled(ImVec2(x0, y0 + thickness + line_gap), ImVec2(x0 + line_w, y0 + block_h), col);

  if (ui_menu_begin("##area_menu")) {
    ImGui::TextDisabled("Area");
    ImGui::Separator();
    if (ui_menu_item("Vertical Split")) {
      area.split_request = AreaSplit::Vertical;
    }
    if (ui_menu_item("Horizontal Split")) {
      area.split_request = AreaSplit::Horizontal;
    }
    ImGui::Separator();
    if (ui_menu_item("Close Area")) {
      area.open = false;
    }
    ui_menu_end();
  }
}

/* Blender puts an editor-type menu at the start of every area header. */
void area_editor_type_menu(Area &area)
{
  area_grip(area);
  ImGui::SameLine(0.0f, 0.0f);
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("Python Console").x + ImGui::GetFrameHeight() * 1.5f);
  if (ImGui::BeginCombo("##editor_type", editor_type_label(area.type))) {
    for (EditorType type : EDITOR_TYPES) {
      if (ImGui::Selectable(editor_type_label(type), type == area.type)) {
        area.type = type;
      }
    }
    ImGui::EndCombo();
  }
}

bool area_draw(Area &area, Context &C)
{
  /* The viewport and timeline draw edge to edge with their own header layout. */
  const bool full_bleed = area.type == EditorType::Viewport || area.type == EditorType::Timeline;
  if (full_bleed) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  }
  /* No tab above the area: the header's grip and editor dropdown do that job, as in Blender.
   * Areas can split one another but never stack in the same spot, since without tabs a
   * stacked area would be hidden. */
  ImGuiWindowClass window_class;
  window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoWindowMenuButton |
                                          ImGuiDockNodeFlags_NoCloseButton | ImGuiDockNodeFlags_NoDockingOverMe |
                                          ImGuiDockNodeFlags_NoDockingOverOther;
  ImGui::SetNextWindowClass(&window_class);
  /* No title bar even while floating: the area is moved by its grip, like a Blender area. */
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
  if (full_bleed) {
    flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  }
  const bool visible = ImGui::Begin(area.window_name().c_str(), &area.open, flags);
  if (full_bleed) {
    ImGui::PopStyleVar();
  }
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

  C.drawing_area = editor_keymap_name(area.type);
  if (visible) {
    switch (area.type) {
      case EditorType::Viewport:
        viewport_draw(area, C);
        break;
      case EditorType::Timeline:
        timeline_draw(area, C);
        break;
      case EditorType::TextEditor:
        text_editor_draw(area, C);
        break;
      case EditorType::Console:
        console_draw(area, C);
        break;
      case EditorType::Layers:
        layers_draw(area, C);
        break;
      case EditorType::Properties:
        properties_draw(area, C);
        break;
      case EditorType::Info:
        area_editor_type_menu(area);
        ImGui::Separator();
        info_draw();
        break;
    }
  }
  C.drawing_area.clear();
  ImGui::End();
  return hovered;
}

}  // namespace oanim

namespace oanim {

/* --- Menus at the mouse, with memory (see ui_menu_begin) -------------------------------------- */

namespace {

struct MenuPlace {
  ImVec2 mouse;
  int frame = 0;
  bool has_pos = false;
  ImVec2 pos;
};

struct OpenMenu {
  std::string key;
  ImGuiID popup = 0;
  ImGuiWindow *window = nullptr;
  /* Where its first item and the remembered one sit, down from the menu's top (-1: not seen). */
  float first_y = -1.0f, last_y = -1.0f;
  /* The submenu open inside it, whose choices count as choosing it. */
  std::string sub;
};

std::map<std::string, std::string> g_menu_last;
std::map<ImGuiID, MenuPlace> g_menu_place;
std::vector<OpenMenu> g_open_menus;

void menu_note(const std::string &label, float center_y)
{
  if (g_open_menus.empty() || ImGui::GetCurrentWindow() != g_open_menus.back().window) {
    return;
  }
  OpenMenu &m = g_open_menus.back();
  const float y = center_y - m.window->Pos.y;
  if (m.first_y < 0.0f) {
    m.first_y = y;
  }
  if (g_menu_last[m.key] == label) {
    m.last_y = y;
  }
}

void menu_choose(const std::string &label)
{
  if (g_open_menus.empty()) {
    return;
  }
  OpenMenu &m = g_open_menus.back();
  g_menu_last[m.key] = ImGui::GetCurrentWindow() == m.window || m.sub.empty() ? label : m.sub;
}

}  // namespace

bool ui_menu_begin(const char *id)
{
  const ImGuiID popup = ImGui::GetID(id);
  /* Once placed, it stays put (ImGui would otherwise place it again as it first shows). */
  if (const auto it = g_menu_place.find(popup); it != g_menu_place.end() && it->second.has_pos && ImGui::IsPopupOpen(id)) {
    ImGui::SetNextWindowPos(it->second.pos);
  }
  if (!ImGui::BeginPopup(id)) {
    return false;
  }
  if (ImGui::IsWindowAppearing()) {
    g_menu_place[popup] = {ImGui::GetIO().MousePos, ImGui::GetFrameCount(), false, {}};
  }
  OpenMenu m;
  m.key = id;
  m.popup = popup;
  m.window = ImGui::GetCurrentWindow();
  g_open_menus.push_back(std::move(m));
  return true;
}

bool ui_menu_item(const char *label, const char *shortcut, bool selected, bool enabled)
{
  const bool chosen = ImGui::MenuItem(label, shortcut, selected, enabled);
  menu_note(label, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
  if (chosen) {
    menu_choose(label);
  }
  return chosen;
}

bool ui_submenu_begin(const char *label, bool enabled)
{
  /* Its row, measured before it opens (once open, the submenu's window is the current one). */
  menu_note(label, ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f);
  const bool open = ImGui::BeginMenu(label, enabled);
  if (open && !g_open_menus.empty()) {
    g_open_menus.back().sub = label;
  }
  return open;
}

void ui_submenu_end()
{
  ImGui::EndMenu();
  if (!g_open_menus.empty()) {
    g_open_menus.back().sub.clear();
  }
}

void ui_menu_end()
{
  if (!g_open_menus.empty()) {
    const OpenMenu m = g_open_menus.back();
    g_open_menus.pop_back();
    MenuPlace &place = g_menu_place[m.popup];
    /* Placed over its first frames (the first is hidden while ImGui sizes it): the remembered item,
     * else the first, under where the mouse was when it opened, a little in from the left. */
    const float y = m.last_y >= 0.0f ? m.last_y : m.first_y;
    if (y >= 0.0f && ImGui::GetFrameCount() <= place.frame + 1) {
      const ImGuiViewport *vp = ImGui::GetMainViewport();
      const ImVec2 size = m.window->Size;
      ImVec2 pos(place.mouse.x - 24.0f, place.mouse.y - y);
      pos.x = std::clamp(pos.x, vp->WorkPos.x, std::max(vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - size.x));
      pos.y = std::clamp(pos.y, vp->WorkPos.y, std::max(vp->WorkPos.y, vp->WorkPos.y + vp->WorkSize.y - size.y));
      place.pos = pos;
      place.has_pos = true;
      ImGui::SetWindowPos(pos);
    }
  }
  ImGui::EndPopup();
}

}  // namespace oanim
