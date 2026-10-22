#define IMGUI_DEFINE_MATH_OPERATORS
/* Drivers in the UI (Blender's): a driven field is purple (red when its driver can't be worked out),
 * right-clicking a keyable field or a colour offers Add Driver..., Copy As New Driver, Copy Driver,
 * Edit Driver..., Delete Driver and Paste Driver, typing "#expression" into a number field drives it,
 * and the Driver window edits a field's drivers: type, expression, variables, remap. */

#include <algorithm>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"

#include "anim/animation.hh"
#include "anim/drivers.hh"
#include "data/camera.hh"
#include "data/document.hh"
#include "editors/ui.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

/* Blender's driven purple, and red for a driver that doesn't work. */
static const ImVec4 DRIVEN_BG(0.40f, 0.22f, 0.62f, 1.0f), DRIVEN_HOVER(0.47f, 0.27f, 0.71f, 1.0f);
static const ImVec4 BROKEN_BG(0.62f, 0.17f, 0.17f, 1.0f), BROKEN_HOVER(0.70f, 0.22f, 0.22f, 1.0f);

namespace {

struct Field {
  uint64_t layer = 0;
  std::string path;
  int component = -1;
  bool valid() const { return !path.empty(); }
};

/* The field a ui_driver_next asked for, for the next colour row. */
Field g_next;
/* The field each open field's menu belongs to (the menu is drawn by that field). */
int g_menu_frame = -1;
/* The Driver window: open on this field. */
struct EditorState {
  bool open = false;
  bool raise = false;
  Field field;
  /* Which of the field's numbers is shown (a colour's R G B A, a position's X Y). */
  int part = 0;
} g_editor;

std::vector<OpArgs> field_args(const Field &f)
{
  OpArgs args = {{"layer", std::to_string(f.layer)}, {"path", f.path}, {"component", std::to_string(f.component)}};
  /* A shape colour: every selected shape's, as the Material panel edits them. */
  const auto ends = [&](const char *end) {
    const size_t n = std::strlen(end);
    return f.path.size() >= n && f.path.compare(f.path.size() - n, n, end) == 0;
  };
  if (f.path.rfind("shapes[", 0) == 0 && (ends(".fill") || ends(".line"))) {
    args["selected_shapes"] = "1";
  }
  return {args};
}

/* 0: not driven; 1: driven; 2: a driver that doesn't work. */
int field_state(Document &doc, const Field &f)
{
  int state = 0;
  for (const DriverRef &r : driver_field_refs(doc, f.layer, f.path, f.component)) {
    if (const Driver *d = driver_find(doc, r)) {
      state = std::max(state, d->error.empty() ? 1 : 2);
    }
  }
  return state;
}

/* What hovering a driven field says: its drivers and how they work. */
std::string field_tooltip(Document &doc, const Field &f)
{
  std::string text;
  const char *parts = "RGBA";
  const std::vector<DriverRef> refs = driver_field_refs(doc, f.layer, f.path, f.component);
  for (const DriverRef &r : refs) {
    const Driver *d = driver_find(doc, r);
    if (!d) {
      continue;
    }
    std::string line;
    if (refs.size() > 1) {
      line += refs.size() == 4 ? std::string(1, parts[r.component]) + ": " : std::string(r.component == 0 ? "X: " : "Y: ");
    }
    static const char *const TYPES[] = {"", "Average of ", "Sum of ", "Smallest of ", "Largest of "};
    line += d->type == DriverType::Expression ? d->expression : std::string(TYPES[int(d->type)]) + "variables";
    for (const DriverVar &v : d->vars) {
      line += "\n    " + v.name + " = " + driver_ref_label(doc, v.source);
    }
    if (d->remap) {
      char remap[96];
      std::snprintf(remap, sizeof(remap), "\n    mapped %g..%g to %g..%g", d->from_min, d->from_max, d->to_min, d->to_max);
      line += remap;
    }
    if (!d->error.empty()) {
      line += "\n    Not working: " + d->error;
    }
    text += (text.empty() ? "" : "\n") + line;
  }
  return "Driven: " + text + "\n\nRight-click to edit, copy or delete the driver";
}

}  // namespace

void ui_driver_next(uint64_t layer, const std::string &path)
{
  g_next = {layer, path, -1};
}

bool ui_driver_next_take(uint64_t *r_layer, std::string *r_path)
{
  if (!g_next.valid()) {
    return false;
  }
  *r_layer = g_next.layer;
  *r_path = g_next.path;
  g_next = {};
  return true;
}

bool ui_driver_menu_opened_this_frame()
{
  return g_menu_frame == ImGui::GetFrameCount();
}

int ui_driver_field_begin(Context &C, uint64_t layer, const std::string &path, int component)
{
  ImGui::BeginGroup();
  if (!C.document || path.empty()) {
    return 0;
  }
  const int state = field_state(*C.document, {layer, path, component});
  if (state == 0) {
    return 0;
  }
  const ImVec4 bg = state == 2 ? BROKEN_BG : DRIVEN_BG, hover = state == 2 ? BROKEN_HOVER : DRIVEN_HOVER;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, bg);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, hover);
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, hover);
  ImGui::PushStyleColor(ImGuiCol_CheckboxSelectedBg, hover);
  return 4;
}

void ui_driver_field_end(Context &C, uint64_t layer, const std::string &path, int component, int pushed)
{
  ImGui::EndGroup();
  ImGui::PopStyleColor(pushed);
  if (!C.document || path.empty() || driver_ref_size(*C.document, layer, path) == 0) {
    return;
  }
  Document &doc = *C.document;
  const Field field{layer, path, component};
  const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                       ImGui::IsMouseHoveringRect(lo, hi);
  ImGui::PushID(path.c_str());
  ImGui::PushID(component);
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    ImGui::OpenPopup("##driver_menu");
    g_menu_frame = ImGui::GetFrameCount();
  }
  if (pushed > 0 && !ImGui::IsAnyItemActive() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", field_tooltip(doc, field).c_str());
  }
  if (ui_menu_begin("##driver_menu")) {
    const bool driven = field_state(doc, field) > 0;
    const std::vector<OpArgs> args = field_args(field);
    if (driven) {
      if (ui_menu_item("Edit Driver...")) {
        g_editor = {true, true, field, 0};
      }
      if (ui_menu_item("Copy Driver")) {
        operator_call(C, "anim.driver_copy", args[0]);
      }
    }
    else {
      if (ui_menu_item("Add Driver...")) {
        OpArgs add = args[0];
        operator_call(C, "anim.driver_add", add);
        g_editor = {true, true, field, 0};
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Drive it with an expression or other values (tip: type #frame/10 into a number field)");
      }
      if (ui_menu_item("Copy As New Driver")) {
        operator_call(C, "anim.driver_copy_new", args[0]);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Copy a driver that reads this: Paste Driver on another field makes it follow this one");
      }
    }
    const size_t copied = driver_clipboard().size();
    const size_t size = driver_field_refs(doc, layer, path, component).size();
    ImGui::BeginDisabled(copied == 0 || (copied != 1 && copied != size));
    if (ui_menu_item("Paste Driver")) {
      operator_call(C, "anim.driver_paste", args[0]);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(copied == 0 ? "Nothing copied yet: right-click a field > Copy As New Driver"
                                    : "Drive this with the copied driver");
    }
    if (driven) {
      ImGui::Separator();
      if (ui_menu_item("Delete Driver")) {
        operator_call(C, "anim.driver_remove", args[0]);
      }
    }
    ui_menu_end();
  }
  ImGui::PopID();
  ImGui::PopID();
}

void ui_driver_typed(Context &C, uint64_t layer, const std::string &path, int component)
{
  /* "#expression" typed into a number field (Blender's). A number field typed into is a text field
   * for a while (ImGui's temp input) whose text it reads as a number and then lets go of, so the
   * text is kept here as it is typed, and used when the edit ends (not when it's cancelled). */
  static ImGuiID typing = 0;
  static std::string typed;
  if (!C.document || path.empty()) {
    return;
  }
  ImGuiContext &g = *GImGui;
  const ImGuiID id = ImGui::GetItemID();
  if (id == 0) {
    return;
  }
  if (g.InputTextState.ID == id && (g.ActiveId == id || ImGui::IsItemDeactivated())) {
    typing = id;
    typed.assign(g.InputTextState.TextA.Data ? g.InputTextState.TextA.Data : "", size_t(std::max(g.InputTextState.TextLen, 0)));
  }
  if (!ImGui::IsItemDeactivated() || typing != id) {
    return;
  }
  typing = 0;
  const std::string text = std::move(typed);
  typed.clear();
  if (ImGui::IsKeyDown(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    return;
  }
  /* "#" and spaces before it allowed; the rest is the expression ("#frame / 10"). */
  const size_t hash = text.find_first_not_of(" 	");
  if (hash == std::string::npos || text[hash] != '#' || text.find_first_not_of(" 	", hash + 1) == std::string::npos) {
    return;
  }
  OpArgs args = field_args({layer, path, component})[0];
  args["expression"] = text.substr(hash + 1);
  operator_call(C, "anim.driver_add", args);
}

/* --- The Driver window --------------------------------------------------------------------------- */

/* The layers a variable can read: every layer, then the camera and the simulation. */
static std::vector<Layer *> source_layers(Document &doc)
{
  std::vector<Layer *> layers;
  for (const Layer *l : document_layers_flat(doc)) {
    layers.push_back(layer_find(doc, l->id));
  }
  layers.push_back(&document_camera(doc));
  layers.push_back(&document_simulation(doc));
  return layers;
}

/* A layer's channels a variable can read: its own and its bones' (not every point's). */
static std::vector<std::string> source_paths(Layer &layer)
{
  std::vector<std::string> paths;
  for (const ChannelRef &ref : layer_channels(layer, true)) {
    if (ref.path.rfind("points[", 0) != 0 && ref.path.rfind("curves[", 0) != 0) {
      paths.push_back(ref.path);
    }
  }
  return paths;
}

/* A variable's source, as three dropdowns: layer, value, and part (X / Y). True when changed. */
static bool source_editor(Document &doc, DriverRef &source)
{
  bool changed = false;
  Layer *current = layer_find(doc, source.layer);
  const float w = ImGui::GetContentRegionAvail().x;
  ImGui::SetNextItemWidth(w * 0.42f);
  /* Layer 0: the document's own (its background, a material), from Copy As New Driver. */
  const char *layer_name = current ? current->name.c_str() : source.layer == 0 && !source.path.empty() ? "Document"
                                                                                                        : "(pick a layer)";
  if (ImGui::BeginCombo("##layer", layer_name)) {
    for (Layer *l : source_layers(doc)) {
      ImGui::PushID(int(l->id & 0x7fffffff));
      if (ImGui::Selectable(l->name.c_str(), current == l)) {
        source.layer = l->id;
        const std::vector<std::string> paths = source_paths(*l);
        source.path = paths.empty() ? std::string() : paths.front();
        source.component = driver_ref_size(doc, l->id, source.path) == 2 ? 0 : -1;
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  const int size = driver_ref_size(doc, source.layer, source.path);
  ImGui::SetNextItemWidth(size == 2 ? w * 0.36f : -FLT_MIN);
  ChannelRef channel;
  std::string shown = "(a value)";
  if (current && !source.path.empty() && layer_channel(*current, source.path, &channel)) {
    shown = channel_label(*current, source.path, false);
  }
  else if (!source.path.empty()) {
    /* A setting (Line Width, a mask row's Grow...): its label after the layer's name. */
    DriverRef whole = source;
    whole.component = -1;
    shown = driver_ref_label(doc, whole);
    const size_t colon = shown.rfind(": ");
    if (colon != std::string::npos && current && shown.rfind(current->name, 0) == 0) {
      shown = shown.substr(current->name.size() + 2);
    }
  }
  if (ImGui::BeginCombo("##path", shown.c_str())) {
    if (current) {
      for (const std::string &p : source_paths(*current)) {
        if (ImGui::Selectable(channel_label(*current, p, false).c_str(), p == source.path)) {
          source.path = p;
          source.component = driver_ref_size(doc, source.layer, p) == 2 ? 0 : -1;
          changed = true;
        }
      }
    }
    ImGui::EndCombo();
  }
  if (size == 2) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##part", source.component == 1 ? "Y" : "X")) {
      for (int c = 0; c < 2; c++) {
        if (ImGui::Selectable(c == 0 ? "X" : "Y", source.component == c)) {
          source.component = c;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
  }
  if (changed) {
    /* A new source: worked out afresh, not followed to where the old one went. */
    source.bone = 0;
    source.item = 0;
  }
  return changed;
}

void ui_driver_window_draw(Context &C)
{
  EditorState &e = g_editor;
  if (!e.open || !C.document) {
    return;
  }
  Document &doc = *C.document;
  const std::vector<DriverRef> refs = driver_field_refs(doc, e.field.layer, e.field.path, e.field.component);
  /* A fixed width, the height of what it shows. */
  const float width = ImGui::GetFontSize() * 26.0f;
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, FLT_MAX));
  if (e.raise) {
    /* Beside the field it was opened from, on the side with room, inside the window. */
    const ImGuiViewport *view = ImGui::GetMainViewport();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    float x = mouse.x + 24.0f;
    if (x + width > view->WorkPos.x + view->WorkSize.x) {
      x = mouse.x - 24.0f - width;
    }
    const float y = std::clamp(mouse.y - 40.0f, view->WorkPos.y, view->WorkPos.y + view->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(ImVec2(std::max(x, view->WorkPos.x), y), ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    e.raise = false;
  }
  if (!ImGui::Begin("Driver###oanim_driver", &e.open,
                    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::End();
    return;
  }
  if (refs.empty()) {
    ImGui::TextDisabled("What this drove is gone.");
    ImGui::End();
    return;
  }
  /* One of the field's numbers at a time: a colour's R G B A, a position's X Y. */
  e.part = std::clamp(e.part, 0, int(refs.size()) - 1);
  if (refs.size() > 1) {
    const char *names4[] = {"R", "G", "B", "A"}, *names2[] = {"X", "Y"};
    for (int i = 0; i < int(refs.size()); i++) {
      if (i > 0) {
        ImGui::SameLine(0, 2);
      }
      const bool on = e.part == i;
      const bool has = driver_find(doc, refs[size_t(i)]) != nullptr;
      if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      if (ImGui::Button((std::string(refs.size() == 4 ? names4[i] : names2[i]) + (has ? "" : " (none)")).c_str())) {
        e.part = i;
      }
      if (on) {
        ImGui::PopStyleColor();
      }
    }
  }
  const DriverRef target = refs[size_t(e.part)];
  ImGui::TextUnformatted(driver_ref_label(doc, target).c_str());
  Driver *d = driver_find(doc, target);
  if (!d) {
    ImGui::TextDisabled("Not driven.");
    if (ImGui::Button("Add Driver")) {
      operator_call(C, "anim.driver_add",
                    {{"layer", std::to_string(target.layer)}, {"path", target.path}, {"component", std::to_string(target.component)}});
    }
    ImGui::End();
    return;
  }
  bool done = false;
  ImGui::Separator();
  if (ImGui::BeginTable("##driver", 2, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    const auto row = [](const char *label) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
    };
    row("Type");
    static const char *const TYPES[] = {"Expression", "Average", "Sum", "Min", "Max"};
    int type = int(d->type);
    if (ImGui::Combo("##type", &type, TYPES, 5)) {
      d->type = DriverType(type);
      done = true;
    }
    if (d->type == DriverType::Expression) {
      row("Expression");
      ImGui::InputText("##expression", &d->expression);
      done = done || ImGui::IsItemDeactivatedAfterEdit();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("Variables by name, frame, pi; + - * / %% ** ; comparisons, and/or/not, a if test else b;\n"
                          "sin cos tan asin acos atan atan2 sqrt abs sign floor ceil round fmod pow exp log\n"
                          "min max clamp(x, lo, hi) lerp(a, b, t) smoothstep(lo, hi, x) radians degrees");
      }
    }
    row("Value");
    float value = 0.0f;
    driver_ref_read(doc, target, &value);
    if (d->error.empty()) {
      ImGui::Text("%.4g", value);
    }
    else {
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Not working: %s", d->error.c_str());
    }
    ImGui::EndTable();
  }

  /* The variables. */
  ImGui::Spacing();
  ImGui::TextDisabled("Variables");
  int remove = -1;
  for (size_t i = 0; i < d->vars.size(); i++) {
    DriverVar &v = d->vars[i];
    ImGui::PushID(int(i));
    ImGui::SetNextItemWidth(70.0f);
    ImGui::InputText("##name", &v.name);
    done = done || ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    /* The source's dropdowns fill a strip that leaves room for the remove button. */
    ImGui::BeginChild("##source", ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - 4.0f,
                                         ImGui::GetFrameHeight()),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    done = source_editor(doc, v.source) || done;
    ImGui::EndChild();
    ImGui::SameLine(0, 4.0f);
    if (ImGui::Button("-", ImVec2(ImGui::GetFrameHeight(), 0))) {
      remove = int(i);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Remove this variable");
    }
    ImGui::PopID();
  }
  if (remove >= 0) {
    d->vars.erase(d->vars.begin() + remove);
    done = true;
  }
  if (ImGui::Button("+ Add Variable")) {
    DriverVar v;
    v.name = d->vars.empty() ? "var" : "var" + std::to_string(d->vars.size() + 1);
    v.source = {CAMERA_LAYER_ID, "zoom", -1};
    if (Layer *l = layer_find(doc, doc.active_layer)) {
      v.source = {l->id, "rotation", -1};
    }
    d->vars.push_back(v);
    done = true;
  }
  ImGui::SameLine();
  const std::vector<Driver> &clip = driver_clipboard();
  const bool can_paste = clip.size() == 1 && clip[0].vars.size() == 1;
  ImGui::BeginDisabled(!can_paste);
  if (ImGui::Button("Paste as Variable")) {
    DriverVar v = clip[0].vars[0];
    v.name = d->vars.empty() ? "var" : "var" + std::to_string(d->vars.size() + 1);
    d->vars.push_back(v);
    done = true;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("A variable reading the field you last used Copy As New Driver on");
  }

  /* Remap: a range of the result onto another. */
  ImGui::Spacing();
  if (ImGui::Checkbox("Remap", &d->remap)) {
    done = true;
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Map a range of the result onto another: an angle of 0..90 onto an opacity of 1..0");
  }
  if (d->remap) {
    ImGui::SameLine();
    if (ImGui::Checkbox("Clamp", &d->clamp)) {
      done = true;
    }
    const float half = (ImGui::GetContentRegionAvail().x - 60.0f - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const auto pair = [&](const char *label, float *a, float *b) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      ImGui::SameLine(60.0f);
      ImGui::PushID(label);
      ImGui::SetNextItemWidth(half);
      ImGui::DragFloat("##a", a, 0.01f, 0.0f, 0.0f, "%.3g");
      done = done || ImGui::IsItemDeactivatedAfterEdit();
      ImGui::SameLine();
      ImGui::SetNextItemWidth(half);
      ImGui::DragFloat("##b", b, 0.01f, 0.0f, 0.0f, "%.3g");
      done = done || ImGui::IsItemDeactivatedAfterEdit();
      ImGui::PopID();
    };
    pair("From", &d->from_min, &d->from_max);
    pair("To", &d->to_min, &d->to_max);
  }
  ImGui::Separator();
  if (ImGui::Button("Delete Driver")) {
    operator_call(C, "anim.driver_remove",
                  {{"layer", std::to_string(target.layer)}, {"path", target.path}, {"component", std::to_string(target.component)}});
    done = false;
  }
  ImGui::End();
  if (done) {
    drivers_evaluate(doc);
    C.undo->push("Change Driver", doc);
  }
}

}  // namespace oanim
