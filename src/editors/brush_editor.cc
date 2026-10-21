#define IMGUI_DEFINE_MATH_OPERATORS
#include "editors/brush_editor.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_stdlib.h"
#include <SDL3/SDL_opengl.h>

#include "assets/asset_library.hh"
#include "assets/brush_presets.hh"
#include "assets/brushes.hh"
#include "data/document.hh"
#include "editors/ui.hh"
#include "gpu/gl.hh"
#include "gpu/scene_renderer.hh"
#include "io/oanim_file.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

static constexpr float DEG = 3.14159265f / 180.0f;
/* A preset row's preview stroke. */
static constexpr float ROW_W = 260.0f, ROW_H = 40.0f;

/* ------------------------------------------------------------------------------------------------
 * Preview strokes: an S-shaped line painted with the settings, as Moho previews a brush. Each has a
 * renderer of its own (a preset menu shows many at once) and is drawn again only when what it
 * shows changes. */

namespace {

struct Preview {
  std::unique_ptr<SceneRenderer> renderer;
  std::string signature;
  unsigned int texture = 0;
  int used_frame = 0;
};

std::map<std::string, Preview> &previews()
{
  static std::map<std::string, Preview> p;
  return p;
}

}  // namespace

static unsigned int preview_texture(const std::string &key, const BrushSettings &settings, const Style &style,
                                    int width, int height)
{
  Preview &p = previews()[key];
  p.used_frame = ImGui::GetFrameCount();
  char extra[96];
  std::snprintf(extra, sizeof(extra), "|%dx%d|%.3f,%.3f,%.3f|%d", width, height, style.line.r, style.line.g,
                style.line.b, brush_find(settings.name) ? 1 : 0);
  const std::string signature = brush_settings_to_json(settings) + settings.name + extra;
  if (p.texture && p.signature == signature) {
    return p.texture;
  }
  Document doc;
  doc.width = width;
  doc.height = height;
  doc.frame_current = 0;
  Layer &layer = document_add_layer(doc, LayerType::Vector, "Preview");
  VectorDrawing &drawing = layer.drawing;
  /* Document units: the frame is 2 tall; the line spans most of the width. */
  const float half_w = float(width) / float(height) * 0.88f;
  Curve curve;
  for (const float2 q : {float2(-half_w, -0.5f), float2(-half_w * 0.35f, 0.15f), float2(half_w * 0.35f, -0.15f),
                         float2(half_w, 0.5f)}) {
    curve.points.push_back({drawing_add_point(drawing, q)});
  }
  drawing.curves.push_back(curve);
  Shape shape;
  shape.has_fill = false;
  shape.style = style;
  shape.style.brush = settings;
  shape.style.line.a = 1.0f;
  /* A quarter of the height wide, at most 22 pixels. */
  shape.style.line_width = std::min(float(height) * 0.25f, 22.0f) / (float(height) * 0.5f);
  for (int i = 0; i < curve.segment_count(); i++) {
    shape.edges.push_back({0, i});
  }
  drawing.shapes.push_back(shape);
  if (!p.renderer) {
    p.renderer = std::make_unique<SceneRenderer>();
  }
  const float scale = float(height) * 0.5f;
  const Transform2D to_pixels{scale, 0, 0, scale, float(width) * 0.5f, float(height) * 0.5f};
  const Color background{0.23f, 0.23f, 0.23f, 1.0f};
  p.texture = p.renderer->render(doc, width, height, to_pixels, 24, 8, &background);
  p.signature = signature;
  return p.texture;
}

/* Draws a preview at the cursor (as an item of `size`). */
static void preview_image(const std::string &key, const BrushSettings &settings, const Style &style, ImVec2 size,
                          float alpha = 1.0f)
{
  const ImVec2 a = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  const unsigned int texture = preview_texture(key, settings, style, std::max(int(size.x), 1),
                                               std::max(int(size.y), 1));
  if (texture) {
    /* The render is bottom-up. */
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(ImTextureID(intptr_t(texture))), a, a + size, ImVec2(0, 1),
                                         ImVec2(1, 0), IM_COL32(255, 255, 255, int(alpha * 255.0f)));
  }
}

/* Previews not drawn for a while give their renderer back. */
static void previews_trim()
{
  const int frame = ImGui::GetFrameCount();
  std::erase_if(previews(), [frame](const auto &item) { return frame - item.second.used_frame > 600; });
}

/* ------------------------------------------------------------------------------------------------
 * Stamps: a shape's first image, for the Shape grid. */

static GLuint thumbnail(const BrushImage &image)
{
  static std::map<uint64_t, GLuint> thumbnails;
  if (auto it = thumbnails.find(image.id); it != thumbnails.end()) {
    return it->second;
  }
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               image.rgba.data());
  if (gl::GenerateMipmap) {
    gl::GenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  }
  else {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  thumbnails[image.id] = texture;
  return texture;
}

/* A shape's stamp drawn in the box a..b: dark ink on white (colour brushes in their colours). */
static void stamp_image(ImDrawList *dl, const Brush &brush, ImVec2 a, ImVec2 b)
{
  const std::vector<BrushImage> &images = brush_images(brush);
  if (images.empty()) {
    return;
  }
  const BrushImage &image = images[0];
  const float pad = 3.0f, room_w = b.x - a.x - pad * 2.0f, room_h = b.y - a.y - pad * 2.0f;
  const float f = std::min(room_w, room_h) / float(std::max(image.width, image.height));
  const ImVec2 dim(image.width * f, image.height * f);
  const ImVec2 at(a.x + pad + (room_w - dim.x) * 0.5f, a.y + pad + (room_h - dim.y) * 0.5f);
  dl->AddImage(ImTextureRef(ImTextureID(intptr_t(thumbnail(image)))), at, at + dim, ImVec2(0, 0), ImVec2(1, 1),
               brush.is_color ? IM_COL32_WHITE : IM_COL32_BLACK);
}

/* One cell of the Shape grid. Returns true when clicked. */
static bool stamp(const char *id, const Brush *brush, bool selected, float size)
{
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const ImVec2 b(a.x + size, a.y + size);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, selected ? IM_COL32(0x9a, 0xd0, 0xc8, 255) : IM_COL32(0xf4, 0xf4, 0xf4, 255));
  if (!brush) {
    /* None: a solid line. */
    dl->AddLine(ImVec2(a.x + 6, b.y - 10), ImVec2(b.x - 6, a.y + 10), IM_COL32(0x20, 0x20, 0x20, 255), 3.0f);
  }
  else {
    stamp_image(dl, *brush, a, b);
    if (brush->files.size() > 1 || brush->data.size() > 1) {
      /* Moho marks multi-brushes with dots in the corner. */
      dl->AddText(ImVec2(a.x + 2, a.y - 4), IM_COL32(0x20, 0x20, 0x20, 255), "***");
    }
  }
  if (hovered || selected) {
    dl->AddRect(a, b, selected ? IM_COL32(0x4f, 0x9a, 0xff, 255) : IM_COL32(0x90, 0x90, 0x90, 255), 0.0f, 0, 2.0f);
  }
  if (hovered) {
    ImGui::SetTooltip("%s", brush ? brush->name.c_str() : "None: a solid line");
  }
  return clicked;
}

/* ------------------------------------------------------------------------------------------------
 * The hovered preset, shown live on what the brush belongs to and taken back when the hover ends. */

namespace {

struct HoverWanted {
  bool set = false;
  BrushTarget target = BrushTarget::Shapes;
  std::string key;
  BrushSettings settings;
  BrushSettings original;
};

struct HoverShown {
  bool active = false;
  BrushTarget target = BrushTarget::Shapes;
  std::string key;
  /* What it replaced: the shapes' brushes (layer id, shape id), or the tool's. */
  struct Saved {
    uint64_t layer;
    uint64_t shape;
    BrushSettings brush;
  };
  std::vector<Saved> shapes;
  BrushSettings tool;
  /* The brush the editor showed before the hover, for marking its preset. */
  BrushSettings original;
};

HoverWanted g_wanted;
HoverShown g_shown;

}  // namespace

static void hover_restore(Context &C)
{
  if (!g_shown.active) {
    return;
  }
  if (g_shown.target == BrushTarget::Tool) {
    C.wm->tool_settings.style.brush = g_shown.tool;
  }
  else if (C.document) {
    for (const HoverShown::Saved &saved : g_shown.shapes) {
      if (Layer *layer = layer_find(*C.document, saved.layer)) {
        const int s = drawing_shape_index(layer->drawing, saved.shape);
        if (s >= 0) {
          layer->drawing.shapes[size_t(s)].style.brush = saved.brush;
        }
      }
    }
  }
  g_shown = {};
}

/* A preset's settings on what has `current`: a missing shape keeps the current one (as loading
 * does). */
static BrushSettings preset_applied(const BrushSettings &preset, const BrushSettings &current)
{
  BrushSettings b = preset;
  if (!b.name.empty() && !brush_find(b.name)) {
    b.name = current.name;
  }
  return b;
}

static void hover_show(Context &C, const HoverWanted &wanted)
{
  HoverShown shown;
  shown.active = true;
  shown.target = wanted.target;
  shown.key = wanted.key;
  shown.original = wanted.original;
  if (wanted.target == BrushTarget::Tool) {
    BrushSettings &brush = C.wm->tool_settings.style.brush;
    shown.tool = brush;
    brush = preset_applied(wanted.settings, brush);
  }
  else {
    for (Layer *layer : editable_layers(C)) {
      const std::vector<bool> selected = drawing_selected_shapes(layer->drawing);
      for (size_t s = 0; s < selected.size(); s++) {
        if (selected[s]) {
          Shape &shape = layer->drawing.shapes[s];
          shown.shapes.push_back({layer->id, shape.id, shape.style.brush});
          shape.style.brush = preset_applied(wanted.settings, shape.style.brush);
        }
      }
    }
  }
  g_shown = std::move(shown);
}

void brush_editor_frame_end(Context &C)
{
  if (g_wanted.set) {
    if (!g_shown.active || g_shown.key != g_wanted.key || g_shown.target != g_wanted.target) {
      hover_restore(C);
      hover_show(C, g_wanted);
    }
  }
  else {
    hover_restore(C);
  }
  g_wanted = {};
  previews_trim();
}

/* ------------------------------------------------------------------------------------------------
 * The preset menu. */

static const char *target_arg(BrushTarget target)
{
  return target == BrushTarget::Tool ? "TOOL" : "SHAPES";
}

/* One preset's row: its preview and name, the buttons at the right. A click loads it; hovering shows
 * it live. */
static void preset_row(Context &C, const BrushPreset &preset, bool library, const BrushSettings &current,
                       const Style &style, BrushTarget target, std::string *r_delete_library)
{
  const bool missing = !preset.settings.name.empty() && !brush_find(preset.settings.name);
  const bool matches = preset.settings == current;
  const std::string key = std::string(library ? "lib:" : "file:") + preset.name;
  ImGui::PushID(key.c_str());
  const float buttons_w = library ? ImGui::GetFrameHeight() : ImGui::GetFrameHeight() + 56.0f;
  const float row_h = ROW_H + 4.0f;
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float row_w = std::max(ImGui::GetContentRegionAvail().x, ROW_W + 160.0f + buttons_w);
  ImGui::SetNextItemAllowOverlap();
  const bool clicked = ImGui::Selectable("##row", matches, ImGuiSelectableFlags_None, ImVec2(row_w - buttons_w, row_h));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::SetCursorScreenPos(a + ImVec2(2, 2));
  preview_image(key, preset_applied(preset.settings, current), style, ImVec2(ROW_W, ROW_H), missing ? 0.45f : 1.0f);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const ImVec2 text_at = a + ImVec2(ROW_W + 10.0f, (row_h - ImGui::GetTextLineHeight()) * 0.5f);
  dl->AddText(text_at, missing ? IM_COL32(0x90, 0x90, 0x90, 255) : IM_COL32(0xe6, 0xe6, 0xe6, 255),
              preset.name.c_str());
  if (missing) {
    dl->AddText(text_at + ImVec2(ImGui::CalcTextSize(preset.name.c_str()).x + 8.0f, 0),
                IM_COL32(0xff, 0x99, 0x4d, 255), "(shape missing)");
  }
  if (hovered) {
    g_wanted.set = true;
    g_wanted.target = target;
    g_wanted.key = key;
    g_wanted.settings = preset.settings;
    g_wanted.original = current;
    /* Bigger, beside the menu (left of it, so the menu stays in view). */
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetWindowPos().x - 6.0f, a.y), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::BeginTooltip();
    preview_image(key + ":big", preset_applied(preset.settings, current), style, ImVec2(ROW_W * 2.0f, ROW_H * 2.5f));
    ImGui::TextUnformatted(preset.name.c_str());
    if (missing) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                         "Its shape \"%s\" isn't in the asset library:\nloading keeps your shape and takes the rest",
                         preset.settings.name.c_str());
    }
    else {
      ImGui::TextDisabled("Click to load it (a copy: nothing stays linked)");
    }
    ImGui::EndTooltip();
  }
  /* The buttons, centred on the row. */
  ImGui::SetCursorScreenPos(ImVec2(a.x + row_w - buttons_w, a.y + (row_h - ImGui::GetFrameHeight()) * 0.5f));
  if (!library) {
    if (ImGui::Button("Save", ImVec2(52.0f, 0))) {
      operator_call(C, "brush.preset_save_to_library", {{"preset", preset.name}});
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Save to the asset library, for every project (its shape too)");
    }
    ImGui::SameLine(0, 4.0f);
  }
  const bool remove = ImGui::Button("-", ImVec2(ImGui::GetFrameHeight(), 0));
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(library ? "Delete from the asset library" : "Remove from this file");
  }
  ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + row_h + ImGui::GetStyle().ItemSpacing.y));
  ImGui::Dummy(ImVec2(0, 0));
  if (clicked) {
    /* What the hover showed goes back first, so the undo step holds the brush from before. */
    g_wanted = {};
    hover_restore(C);
    operator_call(C, "brush.preset_load",
                  {{"preset", preset.name}, {"library", library ? "1" : "0"}, {"target", target_arg(target)}});
  }
  if (remove) {
    if (library) {
      *r_delete_library = preset.name;
    }
    else {
      g_wanted = {};
      hover_restore(C);
      operator_call(C, "brush.preset_remove", {{"preset", preset.name}, {"library", "0"}});
    }
  }
  ImGui::PopID();
}

static void preset_menu(Context &C, const BrushSettings &current, const Style &style, BrushTarget target)
{
  static std::string new_name;
  static std::string delete_library;
  const Document &doc = *C.document;

  ImGui::TextDisabled("In This File");
  if (doc.brush_presets.empty()) {
    ImGui::TextDisabled("  None yet: name one below and press +");
  }
  for (const BrushPreset &preset : doc.brush_presets) {
    preset_row(C, preset, false, current, style, target, &delete_library);
  }
  ImGui::Separator();
  ImGui::TextDisabled("Library");
  if (!asset_library_ready()) {
    ImGui::TextDisabled("  Set up an asset library first: Edit > Preferences");
  }
  else if (library_brush_presets().empty()) {
    ImGui::TextDisabled("  None yet: Save a file preset to put it here");
  }
  for (const BrushPreset &preset : library_brush_presets()) {
    preset_row(C, preset, true, current, style, target, &delete_library);
  }
  ImGui::Separator();
  /* A new file preset from the brush as it is. */
  ImGui::SetNextItemWidth(ROW_W);
  const bool enter = ImGui::InputTextWithHint("##new_name", "New preset name", &new_name,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), 0)) || enter) {
    operator_call(C, "brush.preset_add", {{"name", new_name}, {"target", target_arg(target)}});
    new_name.clear();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Keep this brush as a preset in this file");
  }

  if (!delete_library.empty()) {
    ImGui::OpenPopup("Delete Brush Preset");
  }
  if (ImGui::BeginPopupModal("Delete Brush Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Delete \"%s\" from the asset library?", delete_library.c_str());
    ImGui::TextDisabled("Every project loses it; this can't be undone.");
    if (ImGui::Button("Delete", ImVec2(90, 0))) {
      operator_call(C, "brush.preset_remove", {{"preset", delete_library}, {"library", "1"}});
      delete_library.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      delete_library.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  else {
    delete_library.clear();
  }
}

/* The preset the brush matches (file presets first), or nullptr. */
static const BrushPreset *matching_preset(const Document &doc, const BrushSettings &brush)
{
  for (const BrushPreset &p : doc.brush_presets) {
    if (p.settings == brush) {
      return &p;
    }
  }
  for (const BrushPreset &p : library_brush_presets()) {
    if (p.settings == brush) {
      return &p;
    }
  }
  return nullptr;
}

/* ------------------------------------------------------------------------------------------------
 * The section. */

static void label_cell(const char *label)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-1);
}

/* A drag field shown in other units (degrees, percent). */
static void drag_scaled(BrushEditResult &r, const char *label, const char *id, float &value, float factor, float speed,
                        float min, float max, const char *format, const char *tip)
{
  label_cell(label);
  float shown = value * factor;
  if (ImGui::DragFloat(id, &shown, speed, min, max, format, ImGuiSliderFlags_AlwaysClamp)) {
    value = shown / factor;
    r.changed = true;
  }
  r.done |= ImGui::IsItemDeactivatedAfterEdit();
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tip);
  }
}

static void check(BrushEditResult &r, const char *label, bool &value, const char *tip)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(1);
  if (ImGui::Checkbox(label, &value)) {
    r.changed = r.done = true;
  }
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tip);
  }
}

/* The Shape grid: the asset library's shapes (None first), then those only this file's presets
 * carry. A click takes the shape with its own settings; Alt-click keeps the settings. */
static void shape_grid(Context &C, BrushEditResult &r, BrushSettings &brush)
{
  const bool alt = ImGui::GetIO().KeyAlt;
  const float cell = 44.0f, gap = 3.0f;
  const int columns = 8;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
  int column = 0;
  const auto next = [&] {
    if (++column < columns) {
      ImGui::SameLine();
    }
    else {
      column = 0;
    }
  };
  const auto pick = [&](const Brush &b) {
    if (!alt) {
      brush = b.defaults;
    }
    brush.name = b.name;
    r.changed = r.done = true;
    ImGui::CloseCurrentPopup();
  };
  if (stamp("##none", nullptr, brush.name.empty(), cell)) {
    brush.name.clear();
    r.changed = r.done = true;
    ImGui::CloseCurrentPopup();
  }
  next();
  const std::vector<Brush> &all = brushes();
  for (size_t i = 0; i < all.size(); i++) {
    ImGui::PushID(int(i));
    if (stamp("##stamp", &all[i], brush.name == all[i].name, cell)) {
      pick(all[i]);
    }
    ImGui::PopID();
    next();
  }
  std::vector<std::string> carried;
  for (const BrushPreset &p : C.document->brush_presets) {
    const std::string &name = p.settings.name;
    if (!name.empty() && !brush_find_in_library(name) && std::find(carried.begin(), carried.end(), name) == carried.end()) {
      carried.push_back(name);
    }
  }
  for (size_t i = 0; i < carried.size(); i++) {
    if (const Brush *b = brush_find(carried[i])) {
      ImGui::PushID(int(all.size() + i));
      if (stamp("##carried", b, brush.name == b->name, cell)) {
        pick(*b);
      }
      ImGui::PopID();
      next();
    }
  }
  ImGui::PopStyleVar();
  if (column != 0) {
    ImGui::NewLine();
  }
  if (all.empty()) {
    ImGui::TextDisabled(asset_library_ready() ? "No shapes in the asset library yet: Edit > Preferences > Import Moho Brushes."
                                              : "Set up an asset library first: Edit > Preferences.");
  }
  ImGui::TextDisabled("Click: the shape with its settings  |  Alt-click: keep your settings");
}

BrushEditResult brush_editor(Context &C, BrushSettings &brush, const Style &style, BrushTarget target,
                             const char *revert_tip)
{
  BrushEditResult r;
  ImGui::PushID(target == BrushTarget::Tool ? "brush_tool" : "brush_shapes");
  /* While a preset is hovered the brush shows it: the menu marks the one from before. */
  const BrushSettings current = g_shown.active && g_shown.target == target ? g_shown.original : brush;

  /* Brush: Blender's preset button (three sliders), as tall as the preview stroke beside it, which
   * shows the matching preset's name in its corner. */
  label_cell("Brush");
  const float revert_w = revert_tip ? ui_revert_button_space() : 0.0f;
  const float preview_h = 48.0f, button_w = ImGui::GetFrameHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImDrawList *pdl = ImGui::GetWindowDrawList();
  if (ImGui::Button("##presets", ImVec2(button_w, preview_h))) {
    ImGui::OpenPopup("##brush_presets");
  }
  {
    const float s = button_w;
    const ImVec2 c(at.x + s * 0.5f, at.y + preview_h * 0.5f);
    /* Three sliders, their knobs at different heights. */
    const unsigned int col = ImGui::GetColorU32(ImGuiCol_Text);
    const float knobs[3] = {0.24f, -0.28f, 0.08f};
    for (int i = 0; i < 3; i++) {
      const float x = c.x + s * 0.22f * float(i - 1);
      pdl->AddLine(ImVec2(x, c.y - s * 0.32f), ImVec2(x, c.y + s * 0.32f), col, 1.0f);
      const float ky = c.y + s * knobs[i];
      pdl->AddRectFilled(ImVec2(x - s * 0.08f, ky - s * 0.05f), ImVec2(x + s * 0.08f + 1.0f, ky + s * 0.05f + 1.0f), col);
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Brush presets: load one, or keep this brush as one");
  }
  ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
  const ImVec2 preview_at = ImGui::GetCursorScreenPos();
  preview_image(target == BrushTarget::Tool ? "main:tool" : "main:shapes", brush, style,
                ImVec2(std::max(ImGui::GetContentRegionAvail().x - revert_w, 40.0f), preview_h));
  if (const BrushPreset *matched = matching_preset(*C.document, current)) {
    pdl->AddText(preview_at + ImVec2(5.0f, 3.0f), IM_COL32(0xb0, 0xb0, 0xb0, 200), matched->name.c_str());
  }
  if (revert_tip) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    r.revert = ui_revert_button("##revert_brush", revert_tip);
  }
  if (ImGui::BeginPopup("##brush_presets")) {
    preset_menu(C, current, style, target);
    ImGui::EndPopup();
  }

  /* Shape: the stamp images, from a grid. */
  label_cell("Shape");
  const Brush *shape = brush.name.empty() ? nullptr : brush_find(brush.name);
  {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const std::string text = brush.name.empty() ? std::string("None (solid line)")
                             : shape            ? brush.name
                                                : brush.name + " (missing)";
    if (ImGui::Button(("##shape_button"), ImVec2(-1.0f, 0))) {
      ImGui::OpenPopup("##brush_shapes");
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (shape) {
      dl->AddRectFilled(a + ImVec2(2, 2), a + ImVec2(h - 2, h - 2), IM_COL32(0xf4, 0xf4, 0xf4, 255));
      stamp_image(dl, *shape, a + ImVec2(2, 2), a + ImVec2(h - 2, h - 2));
    }
    dl->AddText(a + ImVec2(h + 4.0f, ImGui::GetStyle().FramePadding.y),
                shape || brush.name.empty() ? IM_COL32(0xe6, 0xe6, 0xe6, 255) : IM_COL32(0xff, 0x99, 0x4d, 255),
                text.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip(shape || brush.name.empty() ? "The stamp the line is painted with"
                                                    : "This shape isn't in the asset library: the line is drawn plain");
    }
  }
  if (ImGui::BeginPopup("##brush_shapes")) {
    shape_grid(C, r, brush);
    ImGui::EndPopup();
  }

  ImGui::BeginDisabled(brush.name.empty());
  drag_scaled(r, "Spacing", "##spacing", brush.spacing, 100.0f, 0.5f, 1.0f, 1000.0f, "%.0f%%",
              "Distance between stamps, as a share of the line's width");

  /* Advanced: the rest, folded. */
  const bool open = ui_subpanel_begin(target == BrushTarget::Tool ? "Brush/Advanced Tool" : "Brush/Advanced",
                                      "Advanced", false);
  if (open) {
    check(r, "Align with curve", brush.align, "Turn each stamp along the line");
    drag_scaled(r, "Jitter Angle", "##jitter", brush.jitter, 1.0f / DEG, 1.0f, 0.0f, 360.0f, "%.0f deg",
                "Each stamp turned at random by up to this much");
    drag_scaled(r, "Angle Drift", "##drift", brush.angle_drift, 1.0f / DEG, 1.0f, 0.0f, 3600.0f, "%.0f deg",
                "The stamps' turn wandering along the line");
    check(r, "Random stamp order", brush.random_order, "A multi-brush's images in random order");
    drag_scaled(r, "Size Variation", "##amp", brush.size_variation_amp, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
                "How much the line's width wanders");
    drag_scaled(r, "Variation Scale", "##scale", brush.size_variation_scale, 100.0f, 0.5f, 1.0f, 1000.0f, "%.0f%%",
                "How long the width's waves are");
    check(r, "Merge alpha", brush.merged_alpha, "Overlapping stamps don't darken each other");
    check(r, "Tint with line colour", brush.tint, "Colour brushes take the line's colour");
    check(r, "Line boil", brush.randomize, "The stamps change every few frames (hand-drawn wobble)");
    label_cell("Boil Interval");
    if (ImGui::DragInt("##interval", &brush.random_interval, 0.1f, 1, 1000, "%d fr", ImGuiSliderFlags_AlwaysClamp)) {
      r.changed = true;
    }
    r.done |= ImGui::IsItemDeactivatedAfterEdit();
    drag_scaled(r, "Hue Drift", "##hue", brush.hue_drift, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%", "Random hue per stamp");
    drag_scaled(r, "Saturation Drift", "##sat", brush.sat_drift, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
                "Random saturation per stamp");
    drag_scaled(r, "Value Drift", "##val", brush.val_drift, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
                "Random brightness per stamp");
    ui_subpanel_end();
  }
  ImGui::EndDisabled();
  ImGui::PopID();
  return r;
}

}  // namespace oanim
