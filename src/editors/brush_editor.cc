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
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

static constexpr float DEG = 3.14159265f / 180.0f;
static constexpr const char *WINDOW = "Brush Settings###oanim_brush_settings";

/* Photoshop's panel colours, in the app's greys: the list and settings on the panel, the preview in a
 * darker well, the picked section lighter. */
static constexpr unsigned int WELL_BG = IM_COL32(0x26, 0x26, 0x26, 255);
static constexpr unsigned int CARD_BG = IM_COL32(0x2e, 0x2e, 0x2e, 255);
static constexpr unsigned int CARD_HOVER = IM_COL32(0x3a, 0x3a, 0x3a, 255);
static constexpr unsigned int CARD_PICKED = IM_COL32(0x3d, 0x5a, 0x80, 255);
static constexpr unsigned int MISSING_TEXT = IM_COL32(0xff, 0x99, 0x4d, 255);

/* ------------------------------------------------------------------------------------------------
 * Preview strokes: an S-shaped line painted with the settings, as Moho and Photoshop preview a
 * brush. Each has a renderer of its own (the Brushes tab shows many at once) and is drawn again
 * only when what it shows changes. */

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
                                    int width, int height, unsigned int background_rgb)
{
  Preview &p = previews()[key];
  p.used_frame = ImGui::GetFrameCount();
  char extra[128];
  std::snprintf(extra, sizeof(extra), "|%dx%d|%.3f,%.3f,%.3f|%d|%x", width, height, style.line.r, style.line.g,
                style.line.b, brush_find(settings.name) ? 1 : 0, background_rgb);
  const std::string signature = brush_settings_to_json(settings) + extra;
  if (p.texture && p.signature == signature) {
    return p.texture;
  }
  Document doc;
  doc.width = width;
  doc.height = height;
  doc.frame_current = 0;
  Layer &layer = document_add_layer(doc, LayerType::Vector, "Preview");
  VectorDrawing &drawing = layer.drawing;
  /* Document units: the frame is 2 tall; the line spans most of the width, thick in the middle and
   * thinner at the ends, as a pen stroke. */
  const float half_w = float(width) / float(height) * 0.86f;
  Curve curve;
  const float2 pts[4] = {float2(-half_w, -0.42f), float2(-half_w * 0.33f, 0.32f), float2(half_w * 0.33f, -0.32f),
                         float2(half_w, 0.42f)};
  const float radii[4] = {0.45f, 1.0f, 1.0f, 0.45f};
  for (int i = 0; i < 4; i++) {
    curve.points.push_back({drawing_add_point(drawing, pts[i])});
    drawing.points.back().radius = radii[i];
  }
  drawing.curves.push_back(curve);
  Shape shape;
  shape.has_fill = false;
  shape.style = style;
  shape.style.brush = settings;
  shape.style.line.a = 1.0f;
  /* A third of the height wide, at most 26 pixels. */
  shape.style.line_width = std::min(float(height) * 0.34f, 26.0f) / (float(height) * 0.5f);
  for (int i = 0; i < curve.segment_count(); i++) {
    shape.edges.push_back({0, i});
  }
  drawing.shapes.push_back(shape);
  if (!p.renderer) {
    p.renderer = std::make_unique<SceneRenderer>();
  }
  const float scale = float(height) * 0.5f;
  const Transform2D to_pixels{scale, 0, 0, scale, float(width) * 0.5f, float(height) * 0.5f};
  const Color background{float((background_rgb >> 16) & 0xff) / 255.0f, float((background_rgb >> 8) & 0xff) / 255.0f,
                         float(background_rgb & 0xff) / 255.0f, 1.0f};
  p.texture = p.renderer->render(doc, width, height, to_pixels, 24, 8, &background);
  p.signature = signature;
  return p.texture;
}

/* A preview drawn into a..b (no item). */
static void preview_draw(ImDrawList *dl, const std::string &key, const BrushSettings &settings, const Style &style,
                         ImVec2 a, ImVec2 b, unsigned int background_rgb, float alpha = 1.0f)
{
  const ImVec2 size = b - a;
  const unsigned int texture = preview_texture(key, settings, style, std::max(int(size.x), 1), std::max(int(size.y), 1),
                                               background_rgb);
  if (texture) {
    /* The render is bottom-up. */
    dl->AddImage(ImTextureRef(ImTextureID(intptr_t(texture))), a, b, ImVec2(0, 1), ImVec2(1, 0),
                 IM_COL32(255, 255, 255, int(alpha * 255.0f)));
  }
}

/* Previews not drawn for a while give their renderer back. */
static void previews_trim()
{
  const int frame = ImGui::GetFrameCount();
  std::erase_if(previews(), [frame](const auto &item) { return frame - item.second.used_frame > 600; });
}

/* ------------------------------------------------------------------------------------------------
 * Stamps: a shape's first image, for the Brush Tip Shape grid. */

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

/* A shape's stamp drawn in the box a..b, in `tint` (white on the dark grid, as Photoshop shows tips;
 * colour brushes in their colours). */
static void stamp_image(ImDrawList *dl, const Brush &brush, const BrushSettings &settings, ImVec2 a, ImVec2 b)
{
  const std::vector<BrushImage> &images = brush_tip_images(brush, settings);
  if (images.empty()) {
    return;
  }
  const BrushImage &image = images[0];
  const float pad = 4.0f, room_w = b.x - a.x - pad * 2.0f, room_h = b.y - a.y - pad * 2.0f;
  const float f = std::min(room_w, room_h) / float(std::max(image.width, image.height));
  const ImVec2 dim(image.width * f, image.height * f);
  const ImVec2 at(a.x + pad + (room_w - dim.x) * 0.5f, a.y + pad + (room_h - dim.y) * 0.5f);
  dl->AddImage(ImTextureRef(ImTextureID(intptr_t(thumbnail(image)))), at, at + dim, ImVec2(0, 0), ImVec2(1, 1),
               IM_COL32_WHITE);
}

/* One cell of the tip grid, Photoshop's: the tip on dark grey, outlined blue when picked. Returns
 * true when clicked. */
static bool stamp_cell(const char *id, const Brush *brush, bool picked, float size, const char *tip)
{
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const ImVec2 b(a.x + size, a.y + size);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, hovered ? CARD_HOVER : CARD_BG, 3.0f);
  if (!brush) {
    /* None: a solid line. */
    dl->AddLine(ImVec2(a.x + size * 0.22f, b.y - size * 0.3f), ImVec2(b.x - size * 0.22f, a.y + size * 0.3f),
                IM_COL32(0xe6, 0xe6, 0xe6, 255), std::max(2.0f, size * 0.07f));
  }
  else {
    BrushSettings shown = brush->defaults;
    stamp_image(dl, *brush, shown, a, b);
    if (brush->files.size() > 1 || brush->data.size() > 1) {
      /* Moho marks multi-brushes with dots in the corner. */
      for (int i = 0; i < 3; i++) {
        dl->AddCircleFilled(ImVec2(a.x + 5.0f + float(i) * 4.0f, a.y + 5.0f), 1.4f, IM_COL32(0xc0, 0xc0, 0xc0, 255));
      }
    }
  }
  if (picked) {
    dl->AddRect(a, b, IM_COL32(0x4f, 0x9a, 0xff, 255), 3.0f, 0, 2.0f);
  }
  if (hovered) {
    ImGui::SetTooltip("%s", tip);
  }
  return clicked;
}

void brush_preset_icon(ImDrawList *dl, ImVec2 a, ImVec2 b, unsigned int color)
{
  const float s = std::min(b.x - a.x, b.y - a.y);
  const ImVec2 c = (a + b) * 0.5f;
  const float knobs[3] = {0.18f, -0.2f, 0.06f};
  for (int i = 0; i < 3; i++) {
    const float x = std::floor(c.x + s * 0.24f * float(i - 1)) + 0.5f;
    dl->AddLine(ImVec2(x, c.y - s * 0.3f), ImVec2(x, c.y + s * 0.3f), color, 1.0f);
    const float ky = std::floor(c.y + s * knobs[i]);
    dl->AddRectFilled(ImVec2(x - s * 0.09f, ky - s * 0.06f), ImVec2(x + s * 0.09f, ky + s * 0.06f), color, 1.0f);
  }
}

/* ------------------------------------------------------------------------------------------------
 * The hovered preset, shown live on what the brush belongs to and taken back when the hover ends. */

namespace {

struct HoverWanted {
  bool set = false;
  BrushTarget target = BrushTarget::Shapes;
  std::string key;
  BrushPreset preset;
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
  /* The brush the window showed before the hover, for marking its preset. */
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
static BrushSettings preset_applied(const BrushPreset &preset, const BrushSettings &current)
{
  BrushSettings b = preset.settings;
  b.preset = preset.name;
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
    brush = preset_applied(wanted.preset, brush);
  }
  else {
    for (Layer *layer : editable_layers(C)) {
      const std::vector<bool> selected = drawing_selected_shapes(layer->drawing);
      for (size_t s = 0; s < selected.size(); s++) {
        if (selected[s]) {
          Shape &shape = layer->drawing.shapes[s];
          shown.shapes.push_back({layer->id, shape.id, shape.style.brush});
          shape.style.brush = preset_applied(wanted.preset, shape.style.brush);
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
 * Names. */

static const BrushPreset *preset_named(const Document &doc, const std::string &name)
{
  for (const BrushPreset &p : doc.brush_presets) {
    if (p.name == name) {
      return &p;
    }
  }
  return library_brush_preset_find(name);
}

/* The preset the brush matches (file presets first), or nullptr. */
static const BrushPreset *matching_preset(const Document &doc, const BrushSettings &brush)
{
  const BrushSettings bare = brush_settings_unnamed(brush);
  if (const BrushPreset *named = preset_named(doc, brush.preset); named && named->settings == bare) {
    return named;
  }
  for (const BrushPreset &p : doc.brush_presets) {
    if (p.settings == bare) {
      return &p;
    }
  }
  for (const BrushPreset &p : library_brush_presets()) {
    if (p.settings == bare) {
      return &p;
    }
  }
  return nullptr;
}

std::string brush_shown_name(const Document &doc, const BrushSettings &brush)
{
  if (const BrushPreset *matched = matching_preset(doc, brush)) {
    return matched->name;
  }
  /* A brush matching no preset (one edited since it was loaded, or never one) is Custom. */
  return brush.name.empty() ? std::string("Solid line") : std::string("Custom");
}

/* ------------------------------------------------------------------------------------------------
 * The window's state. */

namespace {

enum Section { TIP, SHAPE_DYNAMICS, SCATTERING, COLOR_DYNAMICS, TRANSFER, LINE_BOIL, SECTIONS };

struct WindowState {
  bool open = false;
  bool raise = false;
  BrushTarget target = BrushTarget::Shapes;
  int section = TIP;
  /* A tab to switch to on the next frame (-1: stay). */
  int tab_request = -1;
  std::string search;
  std::string new_name;
  std::string delete_library;
  /* Where + saves (Substance's dropdown, remembered): the project, or the library in `category`;
   * `typing_category` while a new category's name is being typed. */
  bool to_library = false;
  std::string category = BRUSH_CATEGORY_DEFAULT;
  bool typing_category = false;
  bool focus_category = false;
  std::string typed_category;
  /* A project preset being saved to a new category from its right-click menu. */
  std::string save_preset;
  std::string save_category;
  /* A card to bring into view (just saved), by its key. */
  std::string reveal;
};

WindowState g_window;

}  // namespace

void brush_window_open(BrushTarget target)
{
  g_window.open = true;
  g_window.raise = true;
  g_window.target = target;
}

static const char *target_arg(BrushTarget target)
{
  return target == BrushTarget::Tool ? "TOOL" : "SHAPES";
}

/* ------------------------------------------------------------------------------------------------
 * Fields: a label and a value, Blender's way (label right-aligned in its column). */

struct Edit {
  bool changed = false;
  bool done = false;
};

static void label_cell(const char *label)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  const float w = ImGui::CalcTextSize(label).x;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-1);
}

static void tooltip(const char *tip)
{
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("%s", tip);
  }
}

/* A field shown in other units (degrees, percent). */
static void drag_scaled(Edit &r, const char *label, const char *id, float &value, float factor, float speed, float min,
                        float max, const char *format, const char *tip)
{
  label_cell(label);
  float shown = value * factor;
  if (ImGui::DragFloat(id, &shown, speed, min, max, format, ImGuiSliderFlags_AlwaysClamp)) {
    value = shown / factor;
    r.changed = true;
  }
  r.done |= ImGui::IsItemDeactivatedAfterEdit();
  tooltip(tip);
}

static void drag_int(Edit &r, const char *label, const char *id, int &value, int min, int max, const char *format,
                     const char *tip)
{
  label_cell(label);
  if (ImGui::DragInt(id, &value, 0.2f, min, max, format, ImGuiSliderFlags_AlwaysClamp)) {
    r.changed = true;
  }
  r.done |= ImGui::IsItemDeactivatedAfterEdit();
  tooltip(tip);
}

static void check(Edit &r, const char *label, bool &value, const char *tip)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(1);
  if (ImGui::Checkbox(label, &value)) {
    r.changed = r.done = true;
  }
  tooltip(tip);
}

/* A heading between groups of fields. */
static void heading(const char *text)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::TextDisabled("%s", text);
}

static bool fields_begin()
{
  if (!ImGui::BeginTable("##fields", 2, ImGuiTableFlags_SizingStretchProp)) {
    return false;
  }
  ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
  return true;
}

/* ------------------------------------------------------------------------------------------------
 * Brush Tip Shape. */

/* Photoshop's angle and roundness widget: the tip's outline, an ellipse turned to the angle; dragging
 * turns it. */
static void tip_widget(Edit &r, BrushSettings &b, float size)
{
  const ImVec2 a = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##tip_widget", ImVec2(size, size));
  const ImVec2 c = a + ImVec2(size, size) * 0.5f;
  const float radius = size * 0.42f;
  if (ImGui::IsItemActive()) {
    const ImVec2 m = ImGui::GetIO().MousePos - c;
    if (m.x * m.x + m.y * m.y > 4.0f) {
      /* Screen y runs down; the angle runs anticlockwise, as Photoshop's. */
      float angle = std::atan2(-m.y, m.x);
      if (ImGui::GetIO().KeyCtrl) {
        angle = std::round(angle / (15.0f * DEG)) * 15.0f * DEG;
      }
      if (std::abs(angle - b.tip_angle) > 1e-5f) {
        b.tip_angle = angle;
        r.changed = true;
      }
    }
  }
  r.done |= ImGui::IsItemDeactivated();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  dl->AddRectFilled(a, a + ImVec2(size, size), WELL_BG, 3.0f);
  const unsigned int line = hot ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xc8, 0xc8, 0xc8, 255);
  dl->AddCircle(c, radius, IM_COL32(0x60, 0x60, 0x60, 255), 48, 1.0f);
  const float ca = std::cos(b.tip_angle), sa = -std::sin(b.tip_angle);
  const float minor = radius * std::clamp(b.roundness, 0.01f, 1.0f);
  ImVec2 pts[48];
  for (int i = 0; i < 48; i++) {
    const float t = float(i) / 48.0f * 2.0f * 3.14159265f;
    const float x = std::cos(t) * radius, y = std::sin(t) * minor;
    pts[i] = c + ImVec2(x * ca - y * sa, x * sa + y * ca);
  }
  dl->AddPolyline(pts, 48, line, ImDrawFlags_Closed, 1.5f);
  /* The arrow along the tip's angle, and the two roundness handles. */
  const ImVec2 tip = c + ImVec2(ca, sa) * (radius + 3.0f);
  dl->AddLine(c - ImVec2(ca, sa) * radius, tip, line, 1.0f);
  dl->AddTriangleFilled(tip + ImVec2(ca, sa) * 5.0f, tip + ImVec2(-sa, ca) * 3.5f, tip - ImVec2(-sa, ca) * 3.5f, line);
  for (const float side : {1.0f, -1.0f}) {
    dl->AddCircleFilled(c + ImVec2(-sa, ca) * minor * side, 3.0f, line);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive()) {
    ImGui::SetTooltip("Drag to turn the tip (Ctrl: 15 degree steps)");
  }
}

/* The tip grid: None (a solid line), Round, the library's shapes, then those only this project's
 * presets carry. Picking one changes the shape alone; the settings stay. */
static void tip_grid(Context &C, Edit &r, BrushSettings &brush, float max_height)
{
  {
    /* As tall as its rows, up to max_height (then it scrolls). */
    const float gap = 4.0f, room = ImGui::GetContentRegionAvail().x - 12.0f;
    const int columns = std::max(4, int((room + gap) / (52.0f + gap)));
    const float cell = std::floor((room - gap * float(columns - 1)) / float(columns));
    int cells = 2 + int(brushes().size());
    for (const BrushPreset &p : C.document->brush_presets) {
      cells += !p.settings.name.empty() && p.settings.name != BRUSH_ROUND_TIP && !brush_find_in_library(p.settings.name);
    }
    const int rows = (cells + columns - 1) / columns;
    max_height = std::min(max_height, float(rows) * (cell + gap) - gap + 12.0f);
  }
  const float height = max_height;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, WELL_BG);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
  if (ImGui::BeginChild("##tips", ImVec2(0, height), ImGuiChildFlags_AlwaysUseWindowPadding)) {
    const float gap = 4.0f;
    const float room = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(4, int((room + gap) / (52.0f + gap)));
    const float cell = std::floor((room - gap * float(columns - 1)) / float(columns));
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
    /* A shape is only its images: picking one keeps every setting. */
    const auto pick = [&](const Brush &b) {
      brush.name = b.name;
      r.changed = r.done = true;
    };
    if (stamp_cell("##none", nullptr, brush.name.empty(), cell, "None: a solid line")) {
      brush.name.clear();
      r.changed = r.done = true;
    }
    next();
    if (const Brush *round = brush_find(BRUSH_ROUND_TIP)) {
      if (stamp_cell("##round", round, brush.name == round->name, cell,
                     "Round (built in): hard or soft by its Hardness, Photoshop's General Brushes")) {
        pick(*round);
      }
      next();
    }
    const std::vector<Brush> &all = brushes();
    for (size_t i = 0; i < all.size(); i++) {
      ImGui::PushID(int(i));
      if (stamp_cell("##stamp", &all[i], brush.name == all[i].name, cell, all[i].name.c_str())) {
        pick(all[i]);
      }
      ImGui::PopID();
      next();
    }
    std::vector<std::string> carried;
    for (const BrushPreset &p : C.document->brush_presets) {
      const std::string &name = p.settings.name;
      if (!name.empty() && name != BRUSH_ROUND_TIP && !brush_find_in_library(name) &&
          std::find(carried.begin(), carried.end(), name) == carried.end())
      {
        carried.push_back(name);
      }
    }
    for (size_t i = 0; i < carried.size(); i++) {
      if (const Brush *b = brush_find(carried[i])) {
        ImGui::PushID(int(all.size() + i));
        const std::string tip = b->name + " (carried by this project's presets)";
        if (stamp_cell("##carried", b, brush.name == b->name, cell, tip.c_str())) {
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
      ImGui::TextDisabled(asset_library_ready() ? "More shapes: put PNGs (or folders of them) in the library's Brush Shapes."
                                                : "Set up an asset library for more shapes: Edit > Preferences.");
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
}

static void tip_section(Context &C, Edit &r, BrushSettings &b)
{
  tip_grid(C, r, b, 170.0f);
  ImGui::Spacing();
  if (!fields_begin()) {
    return;
  }
  ImGui::BeginDisabled(b.name.empty());
  /* Angle, Roundness and the flips beside Photoshop's widget. */
  label_cell("Tip");
  {
    const float widget = 84.0f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float fields_w = ImGui::GetContentRegionAvail().x - widget - ImGui::GetStyle().ItemSpacing.x;
    ImGui::BeginGroup();
    float deg = b.tip_angle / DEG;
    ImGui::SetNextItemWidth(fields_w);
    if (ImGui::DragFloat("##angle", &deg, 1.0f, -180.0f, 180.0f, "Angle  %.0f deg", ImGuiSliderFlags_AlwaysClamp)) {
      b.tip_angle = deg * DEG;
      r.changed = true;
    }
    r.done |= ImGui::IsItemDeactivatedAfterEdit();
    tooltip("The tip turned this much");
    float round_pct = b.roundness * 100.0f;
    ImGui::SetNextItemWidth(fields_w);
    if (ImGui::DragFloat("##roundness", &round_pct, 0.5f, 1.0f, 100.0f, "Roundness  %.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
      b.roundness = round_pct / 100.0f;
      r.changed = true;
    }
    r.done |= ImGui::IsItemDeactivatedAfterEdit();
    tooltip("100%: the tip as drawn; less squashes it across, for calligraphic lines");
    if (ImGui::Checkbox("Flip X", &b.flip_x)) {
      r.changed = r.done = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Flip Y", &b.flip_y)) {
      r.changed = r.done = true;
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(start.x + fields_w + ImGui::GetStyle().ItemSpacing.x, start.y));
    tip_widget(r, b, widget);
  }
  const bool round = b.name == BRUSH_ROUND_TIP;
  ImGui::BeginDisabled(!round);
  drag_scaled(r, "Hardness", "##hardness", b.hardness, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              round ? "How hard the Round tip's edge is: 100% crisp, less fades it out" : "For the Round tip");
  ImGui::EndDisabled();
  drag_scaled(r, "Spacing", "##spacing", b.spacing, 100.0f, 0.5f, 1.0f, 1000.0f, "%.0f%%",
              "Distance between stamps, as a share of the line's width");
  {
    /* What a stretched line's new stamps do: two buttons, as the Point panel's Corner. */
    label_cell("When Stretched");
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const char *tips[2] = {
        "As the line stretches, the stamps filling its gaps fade in (and fade out as it squashes)",
        "As the line stretches, the stamps filling its gaps appear whole (and vanish whole as it\n"
        "squashes): no see-through stamps, for brushes where a faint stamp looks wrong"};
    for (int i = 0; i < 2; i++) {
      const bool picked = b.pop_in == (i == 1);
      if (i == 1) {
        ImGui::SameLine();
      }
      if (picked) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      }
      if (ImGui::Button(i == 0 ? "Fade In##stretch" : "Pop In##stretch", ImVec2(half, 0)) && !picked) {
        b.pop_in = i == 1;
        r.changed = r.done = true;
      }
      if (picked) {
        ImGui::PopStyleColor();
      }
      tooltip(tips[i]);
    }
  }
  check(r, "Merge alpha", b.merged_alpha, "Overlapping stamps don't darken each other");
  check(r, "Tint with line colour", b.tint, "Colour shapes take the line's colour");
  check(r, "Random stamp order", b.random_order, "A multi-image shape's images in random order");
  ImGui::EndDisabled();
  ImGui::EndTable();
}

static void shape_dynamics_section(Edit &r, BrushSettings &b)
{
  if (!fields_begin()) {
    return;
  }
  heading("Size");
  drag_scaled(r, "Size Jitter", "##size_jitter", b.size_jitter, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "Each stamp smaller at random, by up to this much");
  ImGui::BeginDisabled(b.size_jitter <= 0.0f);
  drag_scaled(r, "Minimum Size", "##size_min", b.size_min, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "No stamp smaller than this (of the line's width)");
  ImGui::EndDisabled();
  drag_int(r, "Taper Start", "##size_fade_in", b.size_fade_in, 0, 200, b.size_fade_in == 1 ? "%d stamp" : "%d stamps",
           "Thin the start of the line to a point: the stamps grow from nothing to full size over\n"
           "this many stamps (0: no taper; Photoshop's Size Jitter Control: Fade)");
  drag_int(r, "Taper End", "##size_fade_out", b.size_fade_out, 0, 200, b.size_fade_out == 1 ? "%d stamp" : "%d stamps",
           "Thin the end of the line to a point: the stamps shrink to nothing over the last\n"
           "this many stamps (0: no taper)");
  drag_scaled(r, "Size Wave", "##amp", b.size_variation_amp, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "The width wanders smoothly by up to this much (Moho's size variation)");
  ImGui::BeginDisabled(b.size_variation_amp <= 0.0f);
  drag_scaled(r, "Wave Length", "##scale", b.size_variation_scale, 100.0f, 0.5f, 1.0f, 1000.0f, "%.0f%%",
              "How long the width's waves are (of the frame's height)");
  ImGui::EndDisabled();
  heading("Angle");
  drag_scaled(r, "Angle Jitter", "##jitter", b.jitter, 1.0f / DEG, 1.0f, 0.0f, 360.0f, "%.0f deg",
              "Each stamp turned at random by up to this much");
  drag_scaled(r, "Angle Drift", "##drift", b.angle_drift, 1.0f / DEG, 1.0f, 0.0f, 720.0f, "%.0f deg",
              "The stamps' turn wandering smoothly along the line");
  check(r, "Follow the line's direction", b.align,
        "Photoshop's Angle Control: Direction. Each stamp turns with the line");
  heading("Roundness");
  drag_scaled(r, "Roundness Jitter", "##round_jitter", b.roundness_jitter, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "Each stamp squashed at random by up to this much");
  ImGui::EndTable();
}

static void scattering_section(Edit &r, BrushSettings &b)
{
  if (!fields_begin()) {
    return;
  }
  drag_scaled(r, "Scatter", "##scatter", b.scatter, 100.0f, 1.0f, 0.0f, 1000.0f, "%.0f%%",
              "Stamps pushed off the line by up to this much (of the line's width)");
  check(r, "Both Axes", b.scatter_both, "Pushed along the line too, not just across it");
  drag_int(r, "Count", "##count", b.count, 1, 16, "%d", "Stamps at each place");
  ImGui::BeginDisabled(b.count <= 1);
  drag_scaled(r, "Count Jitter", "##count_jitter", b.count_jitter, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "Fewer stamps at random places, by up to this much");
  ImGui::EndDisabled();
  ImGui::EndTable();
}

static void color_section(Edit &r, BrushSettings &b)
{
  if (!fields_begin()) {
    return;
  }
  drag_scaled(r, "Hue Jitter", "##hue", b.hue_drift, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "Each stamp's hue changed at random by up to this much");
  drag_scaled(r, "Saturation Jitter", "##sat", b.sat_drift, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "Each stamp's saturation changed at random");
  drag_scaled(r, "Brightness Jitter", "##val", b.val_drift, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "Each stamp's brightness changed at random");
  ImGui::EndTable();
}

static void transfer_section(Edit &r, BrushSettings &b)
{
  if (!fields_begin()) {
    return;
  }
  drag_scaled(r, "Opacity Jitter", "##opacity_jitter", b.opacity_jitter, 100.0f, 0.5f, 0.0f, 100.0f, "%.0f%%",
              "Each stamp more see-through at random, by up to this much");
  drag_int(r, "Fade In Start", "##opacity_fade_in", b.opacity_fade_in, 0, 200,
           b.opacity_fade_in == 1 ? "%d stamp" : "%d stamps",
           "Make the start of the line see-through, turning solid over this many stamps\n"
           "(0: no fade; Photoshop's Opacity Jitter Control: Fade)");
  drag_int(r, "Fade Out End", "##opacity_fade_out", b.opacity_fade_out, 0, 200,
           b.opacity_fade_out == 1 ? "%d stamp" : "%d stamps",
           "Make the end of the line see-through, fading out over the last this many stamps\n"
           "(0: no fade)");
  ImGui::EndTable();
}

static void boil_section(Edit &r, BrushSettings &b)
{
  if (!fields_begin()) {
    return;
  }
  drag_int(r, "Every", "##interval", b.random_interval, 1, 24, b.random_interval == 1 ? "%d frame" : "%d frames",
           "The stamps change this often: the hand-drawn wobble of a line redrawn each frame");
  ImGui::EndTable();
  ImGui::Spacing();
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("Moho's line boil: every random setting picks new values this often, so a still line "
                      "shimmers as if traced again.");
  ImGui::PopTextWrapPos();
}

/* ------------------------------------------------------------------------------------------------
 * The Brushes tab: the presets as preview cards. */

/* One preset's card: its preview and name. A click loads it; hovering shows it live; right-click
 * offers Save to Library and Delete. */
static void preset_card(Context &C, const BrushPreset &preset, bool library, const BrushSettings &current,
                        const Style &style, BrushTarget target, ImVec2 size)
{
  const bool missing = !preset.settings.name.empty() && !brush_find(preset.settings.name);
  const bool matches = preset.settings == brush_settings_unnamed(current);
  const std::string key = std::string(library ? "lib:" : "file:") + preset.group + "/" + preset.name;
  ImGui::PushID(key.c_str());
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##card", size);
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, a + size, matches ? CARD_PICKED : hovered ? CARD_HOVER : CARD_BG, 4.0f);
  if (g_window.reveal == key) {
    /* Just saved: scrolled to, outlined. */
    ImGui::SetScrollHereY(0.5f);
    g_window.reveal.clear();
  }
  const float pad = 5.0f, text_h = ImGui::GetTextLineHeight();
  const ImVec2 pa = a + ImVec2(pad, pad), pb = a + ImVec2(size.x - pad, size.y - text_h - pad * 1.6f);
  preview_draw(dl, key, preset_applied(preset, current), style, pa, pb, matches ? 0x2a3d57 : 0x262626,
               missing ? 0.4f : 1.0f);
  /* The name under it, cut short to fit. */
  const ImVec2 text_at(a.x + pad + 2.0f, pb.y + pad * 0.6f);
  dl->PushClipRect(text_at, ImVec2(a.x + size.x - pad, a.y + size.y), true);
  dl->AddText(text_at, missing ? IM_COL32(0x90, 0x90, 0x90, 255) : IM_COL32(0xe6, 0xe6, 0xe6, 255), preset.name.c_str());
  dl->PopClipRect();
  if (missing) {
    /* A warning mark in the corner. */
    const ImVec2 w(a.x + size.x - pad - 6.0f, a.y + pad + 6.0f);
    dl->AddCircleFilled(w, 6.0f, MISSING_TEXT);
    dl->AddText(w - ImGui::CalcTextSize("!") * 0.5f, IM_COL32(0x20, 0x20, 0x20, 255), "!");
  }
  if (hovered) {
    g_wanted.set = true;
    g_wanted.target = target;
    g_wanted.key = key;
    g_wanted.preset = preset;
    g_wanted.original = current;
    if (missing) {
      ImGui::SetTooltip("Its shape \"%s\" isn't in the asset library:\nloading keeps your shape and takes the rest",
                        brush_display_name(preset.settings.name).c_str());
    }
    else if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("%s\nClick to load it (a copy: nothing stays linked). Right-click for more.", preset.name.c_str());
    }
  }
  if (clicked) {
    /* What the hover showed goes back first, so the undo step holds the brush from before. */
    g_wanted = {};
    hover_restore(C);
    operator_call(C, "brush.preset_load",
                  {{"preset", preset.name}, {"library", library ? "1" : "0"}, {"target", target_arg(target)}});
  }
  if (ImGui::BeginPopupContextItem("##card_menu")) {
    if (!library) {
      /* Save to Library, in a category: one of the library's, or a new one. */
      ImGui::BeginDisabled(!asset_library_ready());
      if (ImGui::BeginMenu("Save to Library")) {
        for (const std::string &category : library_brush_categories()) {
          if (ImGui::MenuItem(category.c_str())) {
            operator_call(C, "brush.preset_save_to_library", {{"preset", preset.name}, {"category", category}});
            g_window.category = category;
          }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("New Category...")) {
          g_window.save_preset = preset.name;
          g_window.save_category.clear();
        }
        ImGui::EndMenu();
      }
      ImGui::EndDisabled();
    }
    if (!preset.builtin && ImGui::MenuItem(library ? "Delete from Library..." : "Remove from Project")) {
      g_wanted = {};
      hover_restore(C);
      if (library) {
        g_window.delete_library = preset.name;
      }
      else {
        operator_call(C, "brush.preset_remove", {{"preset", preset.name}, {"library", "0"}});
      }
    }
    ImGui::EndPopup();
  }
  ImGui::PopID();
}

/* A group of cards, folding (open to start with when `default_open`), in a grid as wide as the
 * window allows. While searching every group shows what it found, folded or not. */
static void preset_group(Context &C, const char *key, const std::string &title, const std::vector<const BrushPreset *> &list,
                         bool library, const BrushSettings &current, const Style &style, BrushTarget target,
                         const char *empty_text, bool default_open, bool searching)
{
  if (list.empty() && !empty_text) {
    return;
  }
  const std::string label = title + "  (" + std::to_string(list.size()) + ")";
  if (searching) {
    ImGui::TextDisabled("%s", label.c_str());
  }
  else {
    const bool open = ui_subpanel_begin(std::string("Brushes/") + key, label + "###" + key, default_open);
    if (library && !title.empty() && ImGui::IsItemClicked()) {
      /* The group being browsed is where a new library brush goes. */
      const size_t slash = title.find(" / ");
      if (slash != std::string::npos) {
        g_window.category = title.substr(slash + 3);
      }
    }
    if (!open) {
      return;
    }
  }
  ImGui::Indent(6.0f);
  if (list.empty()) {
    ImGui::TextDisabled("%s", empty_text);
  }
  const float gap = 6.0f;
  const float room = ImGui::GetContentRegionAvail().x - 6.0f;
  const int columns = std::max(1, int((room + gap) / (200.0f + gap)));
  const float w = std::floor((room - gap * float(columns - 1)) / float(columns));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
  for (size_t i = 0; i < list.size(); i++) {
    if (i % size_t(columns) != 0) {
      ImGui::SameLine();
    }
    preset_card(C, *list[i], library, current, style, target, ImVec2(w, 74.0f));
  }
  ImGui::PopStyleVar();
  ImGui::Unindent(6.0f);
  ImGui::Spacing();
  if (!searching) {
    ui_subpanel_end();
  }
}

static bool search_matches(const std::string &search, const std::string &name)
{
  if (search.empty()) {
    return true;
  }
  const auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return s;
  };
  return lower(name).find(lower(search)) != std::string::npos;
}

static void brushes_tab(Context &C, const BrushSettings &current, const Style &style, BrushTarget target, float footer)
{
  WindowState &w = g_window;
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##search", "Search brushes", &w.search);
  ImGui::Spacing();
  if (ImGui::BeginChild("##presets", ImVec2(0, -footer))) {
    std::vector<const BrushPreset *> file;
    for (const BrushPreset &p : C.document->brush_presets) {
      if (search_matches(w.search, p.name)) {
        file.push_back(&p);
      }
    }
    preset_group(C, "file", "Project", file, false, current, style, target,
                 w.search.empty() ? "None yet: name the brush below, pick Project and press +" : nullptr, true,
                 !w.search.empty());
    /* The library's, by folder. */
    std::vector<std::string> groups;
    for (const BrushPreset &p : library_brush_presets()) {
      if (std::find(groups.begin(), groups.end(), p.group) == groups.end()) {
        groups.push_back(p.group);
      }
    }
    if (groups.empty()) {
      groups.push_back("");
    }
    for (const std::string &group : groups) {
      std::vector<const BrushPreset *> list;
      for (const BrushPreset &p : library_brush_presets()) {
        if (p.group == group && search_matches(w.search, p.name)) {
          list.push_back(&p);
        }
      }
      const char *empty = nullptr;
      if (group.empty() && w.search.empty()) {
        empty = !asset_library_ready() ? "Set up an asset library first: Edit > Preferences"
                                       : "None yet: save one below with Library picked";
      }
      preset_group(C, ("lib/" + group).c_str(), group.empty() ? "Library" : "Library / " + group, list, true, current,
                   style, target, empty,
                   /* The library's groups start folded, Basic aside. */
                   group == "Basic", !w.search.empty());
    }
  }
  ImGui::EndChild();
}

/* ------------------------------------------------------------------------------------------------
 * The window. */

/* Opens the group a saved brush went into and brings its card into view. */
static void reveal_saved(Context &C, BrushTarget target, bool library)
{
  WindowState &w = g_window;
  const Shape *shape = target == BrushTarget::Tool ? nullptr : shown_shape(C);
  const std::string &name = target == BrushTarget::Tool ? C.wm->tool_settings.style.brush.preset
                                                         : (shape ? shape->style.brush.preset : w.new_name);
  if (library) {
    const BrushPreset *saved = library_brush_preset_find(name);
    if (!saved) {
      return;
    }
    ui_fold_record(ui_fold_scope() + "/Brushes/lib/" + saved->group, saved->group == "Basic", true);
    w.reveal = "lib:" + saved->group + "/" + saved->name;
  }
  else {
    ui_fold_record(ui_fold_scope() + "/Brushes/file", true, true);
    w.reveal = "file:/" + name;
  }
  w.tab_request = 1;
}

/* The footer, Substance's way: a name, where it goes (Project, or Library and its category), and +. */
static void footer_row(Context &C, BrushTarget target, bool enabled)
{
  WindowState &w = g_window;
  ImGui::Separator();
  ImGui::BeginDisabled(!enabled);
  const ImGuiStyle &st = ImGui::GetStyle();
  const float plus = ImGui::GetFrameHeight(), where_w = 96.0f, category_w = 140.0f;
  const bool library_ready = asset_library_ready();
  const bool to_library = w.to_library && library_ready;
  const float right = plus + where_w + st.ItemSpacing.x * 2.0f + (to_library ? category_w + st.ItemSpacing.x : 0.0f);
  ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - right, 80.0f));
  bool save = ImGui::InputTextWithHint("##new_name", "New preset name", &w.new_name, ImGuiInputTextFlags_EnterReturnsTrue);

  /* Where it goes. */
  ImGui::SameLine();
  ImGui::SetNextItemWidth(where_w);
  if (ImGui::BeginCombo("##where", to_library ? "Library" : "Project")) {
    if (ImGui::Selectable("Project", !to_library)) {
      w.to_library = false;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Saved in this project (.oanim), with a copy of its shape");
    }
    ImGui::BeginDisabled(!library_ready);
    if (ImGui::Selectable("Library", to_library)) {
      w.to_library = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(library_ready ? "Saved in the asset library, for every project, in a category"
                                      : "Set up an asset library first: Edit > Preferences");
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    ImGui::SetTooltip("Where the new preset is saved");
  }

  /* The library's category: one of its folders, or a new one typed in. */
  if (to_library) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(category_w);
    if (w.typing_category) {
      if (w.focus_category) {
        /* Typed into straight away, once. */
        ImGui::SetKeyboardFocusHere();
        w.focus_category = false;
      }
      const bool entered = ImGui::InputTextWithHint("##new_category", "New category", &w.typed_category,
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("A new category: a new folder of Brushes (Enter keeps it, Esc goes back)");
      }
      if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        w.typing_category = false;
      }
      else if (entered || (ImGui::IsItemDeactivated() && !w.typed_category.empty())) {
        w.category = w.typed_category;
        w.typing_category = false;
        save = save || (entered && !w.new_name.empty());
      }
      else if (ImGui::IsItemDeactivated() && w.typed_category.empty()) {
        w.typing_category = false;
      }
    }
    else {
      if (ImGui::BeginCombo("##category", w.category.c_str())) {
        std::vector<std::string> categories = library_brush_categories();
        if (std::find(categories.begin(), categories.end(), w.category) == categories.end()) {
          /* Picked earlier but not made yet: it's made with the first brush saved in it. */
          categories.push_back(w.category);
        }
        for (const std::string &category : categories) {
          if (ImGui::Selectable(category.c_str(), category == w.category)) {
            w.category = category;
          }
        }
        ImGui::Separator();
        if (ImGui::Selectable("New Category...")) {
          w.typing_category = true;
          w.focus_category = true;
          w.typed_category.clear();
        }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::SetTooltip("The library category it goes in (a folder of Brushes)");
      }
    }
  }

  ImGui::SameLine();
  save = ImGui::Button("+##new_preset", ImVec2(plus, 0)) || save;
  tooltip(to_library ? "Save this brush to the library, in the category beside" : "Keep this brush as a preset in the project");
  if (save && !(to_library && w.typing_category)) {
    if (to_library) {
      operator_call(C, "brush.preset_add",
                    {{"name", w.new_name}, {"target", target_arg(target)}, {"library", "1"}, {"category", w.category}});
    }
    else {
      operator_call(C, "brush.preset_add", {{"name", w.new_name}, {"target", target_arg(target)}});
    }
    reveal_saved(C, target, to_library);
    w.new_name.clear();
  }
  ImGui::EndDisabled();

  /* A project preset saved from its menu into a new category. */
  if (!w.save_preset.empty() && !ImGui::IsPopupOpen("New Category")) {
    ImGui::OpenPopup("New Category");
  }
  if (ImGui::BeginPopupModal("New Category", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Save \"%s\" to the library in a new category:", w.save_preset.c_str());
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    const bool entered = ImGui::InputTextWithHint("##name", "Category name", &w.save_category,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    ImGui::BeginDisabled(w.save_category.empty());
    if (ImGui::Button("Save", ImVec2(100, 0)) || (entered && !w.save_category.empty())) {
      operator_call(C, "brush.preset_save_to_library", {{"preset", w.save_preset}, {"category", w.save_category}});
      w.category = w.save_category;
      w.save_preset.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      w.save_preset.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

static void delete_confirm(Context &C)
{
  WindowState &w = g_window;
  if (!w.delete_library.empty() && !ImGui::IsPopupOpen("Delete Brush Preset")) {
    ImGui::OpenPopup("Delete Brush Preset");
  }
  if (ImGui::BeginPopupModal("Delete Brush Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Delete \"%s\" from the asset library?", w.delete_library.c_str());
    ImGui::TextDisabled("Every project loses it; this can't be undone.");
    ImGui::Spacing();
    if (ImGui::Button("Delete", ImVec2(100, 0))) {
      operator_call(C, "brush.preset_remove", {{"preset", w.delete_library}, {"library", "1"}});
      w.delete_library.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      w.delete_library.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

/* The section list's row: its switch (if it has one) and name. */
static bool section_row(int index, const char *name, bool *on, Edit &r)
{
  WindowState &w = g_window;
  ImGui::PushID(index);
  const bool picked = w.section == index;
  const float h = ImGui::GetFrameHeight() + 2.0f;
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  ImGui::SetNextItemAllowOverlap();
  if (ImGui::Selectable("##section", picked, ImGuiSelectableFlags_None, ImVec2(width, h))) {
    w.section = index;
  }
  if (picked) {
    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + 3.0f, a.y + h), IM_COL32(0x4f, 0x9a, 0xff, 255));
  }
  /* A small box, Photoshop's: its own click switches the section, the rest of the row picks it. */
  const float box = std::floor(ImGui::GetTextLineHeight() * 0.85f);
  const ImVec2 box_at(a.x + 12.0f, std::floor(a.y + (h - box) * 0.5f));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (on) {
    ImGui::SetCursorScreenPos(box_at - ImVec2(3, 3));
    if (ImGui::InvisibleButton("##on", ImVec2(box + 6.0f, box + 6.0f))) {
      *on = !*on;
      r.changed = r.done = true;
      w.section = index;
    }
    const bool hot = ImGui::IsItemHovered();
    dl->AddRectFilled(box_at, box_at + ImVec2(box, box),
                      *on ? IM_COL32(0x47, 0x72, 0xb3, 255) : hot ? IM_COL32(0x60, 0x60, 0x60, 255) : IM_COL32(0x54, 0x54, 0x54, 255),
                      2.0f);
    if (*on) {
      const ImVec2 p0 = box_at + ImVec2(box * 0.22f, box * 0.52f), p1 = box_at + ImVec2(box * 0.42f, box * 0.72f),
                   p2 = box_at + ImVec2(box * 0.78f, box * 0.28f);
      dl->AddLine(p0, p1, IM_COL32_WHITE, 1.6f);
      dl->AddLine(p1, p2, IM_COL32_WHITE, 1.6f);
    }
  }
  dl->AddText(ImVec2(box_at.x + box + 8.0f, a.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
              picked ? IM_COL32(0xff, 0xff, 0xff, 255) : IM_COL32(0xd0, 0xd0, 0xd0, 255), name);
  ImGui::SetCursorScreenPos(ImVec2(a.x, a.y + h));
  ImGui::Dummy(ImVec2(0, 0));
  ImGui::PopID();
  return picked;
}

static void settings_tab(Context &C, Edit &r, BrushSettings &b, const Style &style, bool tool,
                         float footer)
{
  WindowState &w = g_window;
  const float preview_h = 92.0f;
  const float body_h = ImGui::GetContentRegionAvail().y - preview_h - footer - ImGui::GetStyle().ItemSpacing.y * 2.0f;
  /* The sections down the left, Photoshop's. */
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0x2a, 0x2a, 0x2a, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
  if (ImGui::BeginChild("##sections", ImVec2(176.0f, body_h), ImGuiChildFlags_AlwaysUseWindowPadding)) {
    struct Entry {
      const char *name;
      bool *on;
    };
    const Entry entries[SECTIONS] = {{"Brush Tip Shape", nullptr},       {"Shape Dynamics", &b.shape_dynamics},
                                     {"Scattering", &b.scattering},      {"Color Dynamics", &b.color_dynamics},
                                     {"Transfer", &b.transfer},          {"Line Boil", &b.randomize}};
    ImGui::BeginDisabled(b.name.empty());
    for (int i = 0; i < SECTIONS; i++) {
      if (i == 1) {
        ImGui::Dummy(ImVec2(0, 4));
      }
      section_row(i, entries[i].name, entries[i].on, r);
    }
    ImGui::EndDisabled();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  /* The section's settings. */
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
  if (ImGui::BeginChild("##settings", ImVec2(0, body_h), ImGuiChildFlags_AlwaysUseWindowPadding)) {
    const char *titles[SECTIONS] = {"Brush Tip Shape", "Shape Dynamics", "Scattering", "Color Dynamics", "Transfer",
                                    "Line Boil"};
    const int section = b.name.empty() ? TIP : w.section;
    ImGui::TextUnformatted(titles[section]);
    ImGui::Separator();
    ImGui::Spacing();
    const bool *on[SECTIONS] = {nullptr, &b.shape_dynamics, &b.scattering, &b.color_dynamics, &b.transfer, &b.randomize};
    ImGui::BeginDisabled(on[section] && !*on[section]);
    switch (section) {
      case TIP: tip_section(C, r, b); break;
      case SHAPE_DYNAMICS: shape_dynamics_section(r, b); break;
      case SCATTERING: scattering_section(r, b); break;
      case COLOR_DYNAMICS: color_section(r, b); break;
      case TRANSFER: transfer_section(r, b); break;
      case LINE_BOIL: boil_section(r, b); break;
      default: break;
    }
    ImGui::EndDisabled();
    if (on[section] && !*on[section]) {
      ImGui::Spacing();
      ImGui::TextDisabled("Off: tick it on the left to use these.");
    }
    if (b.name.empty()) {
      ImGui::Spacing();
      ImGui::TextDisabled("A solid line: pick a tip above to paint it with stamps.");
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
  /* The preview stroke, in a well along the bottom. */
  ImGui::Spacing();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const ImVec2 size(ImGui::GetContentRegionAvail().x, preview_h);
  ImGui::Dummy(size);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, a + size, WELL_BG, 4.0f);
  preview_draw(dl, tool ? "window:tool" : "window:shapes", b, style, a + ImVec2(4, 4), a + size - ImVec2(4, 4), 0x262626);
}

void brush_window_draw(Context &C)
{
  WindowState &w = g_window;
  if (!w.open) {
    return;
  }
  const ImGuiViewport *main = ImGui::GetMainViewport();
  ImGui::SetNextWindowSize(ImVec2(680, 620), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(520, 440), ImVec2(FLT_MAX, FLT_MAX));
  if (w.raise) {
    ImGui::SetNextWindowFocus();
    w.raise = false;
  }
  if (!ImGui::Begin(WINDOW, &w.open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  const bool tool = w.target == BrushTarget::Tool;
  ToolSettings &ts = C.wm->tool_settings;
  Shape *shape = tool ? nullptr : shown_shape(C);
  const bool editable = tool || shape;
  BrushSettings brush = tool ? ts.style.brush : shape ? shape->style.brush : BrushSettings{};
  const BrushSettings before = brush;
  const Style style = tool ? ts.style : shape ? shape->style : ts.style;
  /* While a preset is hovered the brush shows it: cards mark the one from before. */
  const BrushSettings current = g_shown.active && g_shown.target == w.target ? g_shown.original : brush;

  /* What it edits, and the brush's name. */
  int selected = 0;
  if (!tool) {
    for (const Layer *layer : editable_layers(C)) {
      for (const bool s : drawing_selected_shapes(layer->drawing)) {
        selected += s ? 1 : 0;
      }
    }
  }
  {
    std::string what = tool ? std::string("The Draw tool's brush (new strokes)")
                            : selected == 0 ? std::string("No shape selected")
                            : selected == 1 ? std::string("1 selected shape")
                                            : std::to_string(selected) + " selected shapes";
    ImGui::TextUnformatted(editable ? brush_shown_name(*C.document, current).c_str() : "");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", what.c_str());
    if (!tool && selected > 1 && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("Showing the active shape's brush; a change goes to every selected shape,\nwhich keep their other settings");
    }
  }
  ImGui::Spacing();

  Edit r;
  const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 2.0f;
  ImGui::BeginDisabled(!editable);
  if (ImGui::BeginTabBar("##brush_tabs")) {
    const ImGuiTabItemFlags settings_flags = w.tab_request == 0 ? ImGuiTabItemFlags_SetSelected : 0;
    const ImGuiTabItemFlags brushes_flags = w.tab_request == 1 ? ImGuiTabItemFlags_SetSelected : 0;
    w.tab_request = -1;
    if (ImGui::BeginTabItem("Brush Settings", nullptr, settings_flags)) {
      settings_tab(C, r, brush, style, tool, footer);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Brushes", nullptr, brushes_flags)) {
      brushes_tab(C, current, style, w.target, footer);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::EndDisabled();
  footer_row(C, w.target, editable);
  delete_confirm(C);
  if (!editable) {
    const char *text = "Select a shape to edit its brush";
    const ImVec2 at = ImGui::GetWindowPos() + ImGui::GetWindowSize() * 0.5f - ImGui::CalcTextSize(text) * 0.5f;
    ImGui::GetWindowDrawList()->AddText(at, IM_COL32(0xd0, 0xd0, 0xd0, 255), text);
  }
  ImGui::End();

  if (r.changed) {
    if (tool) {
      brush_settings_apply_changes(before, brush, ts.style.brush);
    }
    else {
      shapes_change_brush(C, before, brush);
    }
  }
  if (r.done && !tool) {
    C.undo->push("Brush", *C.document);
  }
}

/* ------------------------------------------------------------------------------------------------
 * The Material panel's row. */

bool brush_row(Context &C, const BrushSettings &brush, const Style &style, const char *revert_tip)
{
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Brush");
  ImGui::TableSetColumnIndex(1);
  const float revert_w = revert_tip ? ui_revert_button_space() : 0.0f;
  const float preview_h = 48.0f, button_w = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemInnerSpacing.x;
  /* Blender's preset button (three sliders), as tall as the preview: it opens Brush Settings. */
  const ImVec2 at = ImGui::GetCursorScreenPos();
  if (ImGui::Button("##brush_settings", ImVec2(button_w, preview_h))) {
    g_window.tab_request = 0;
    brush_window_open(BrushTarget::Shapes);
  }
  ImDrawList *dl = ImGui::GetWindowDrawList();
  brush_preset_icon(dl, at, at + ImVec2(button_w, preview_h), ImGui::GetColorU32(ImGuiCol_Text));
  tooltip("Brush Settings: the brush's tip, dynamics and presets");
  ImGui::SameLine(0.0f, gap);
  /* The preview stroke, with the brush's name in its corner; clicking it opens the presets (the
   * window's Brushes tab). */
  const ImVec2 pa = ImGui::GetCursorScreenPos();
  const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x - revert_w, 40.0f), preview_h);
  ImGui::SetNextItemAllowOverlap();
  if (ImGui::InvisibleButton("##brush_preview", size)) {
    g_window.tab_request = 1;
    brush_window_open(BrushTarget::Shapes);
  }
  const bool hot = ImGui::IsItemHovered();
  dl->AddRectFilled(pa, pa + size, WELL_BG, ImGui::GetStyle().FrameRounding);
  preview_draw(dl, "panel:shapes", brush, style, pa + ImVec2(2, 2), pa + size - ImVec2(2, 2), 0x262626);
  const bool missing = !brush.name.empty() && !brush_find(brush.name);
  const std::string name = brush_shown_name(*C.document, brush) + (missing ? "  (shape missing)" : "");
  dl->AddText(pa + ImVec2(6.0f, 3.0f), missing ? MISSING_TEXT : IM_COL32(0xc8, 0xc8, 0xc8, 220), name.c_str());
  if (hot) {
    dl->AddRect(pa, pa + size, IM_COL32(0x80, 0x80, 0x80, 255), ImGui::GetStyle().FrameRounding);
  }
  tooltip(missing ? "This brush's shape isn't in the asset library: the line is drawn plain. Click for the brush presets"
                  : "Click for the brush presets");
  {
    /* The brush eyedropper, in the preview's bottom right: click a stroke in the viewport to take its
     * brush. */
    const float s = ImGui::GetFrameHeight();
    const ImVec2 after = ImGui::GetCursorScreenPos();
    const ImVec2 b0(pa.x + size.x - s - 3.0f, pa.y + size.y - s - 3.0f);
    ImGui::SetCursorScreenPos(b0);
    const bool picking = ui_shape_eyedropper_active("BRUSH");
    /* Clicking it again while picking stops it (as any click off a stroke does). */
    if (ImGui::InvisibleButton("##brush_eyedropper", ImVec2(s, s))) {
      if (!picking) {
        ui_style_eyedropper_start("BRUSH", "brush");
      }
    }
    const bool dropper_hot = ImGui::IsItemHovered();
    dl->AddRectFilled(b0, b0 + ImVec2(s, s),
                      picking ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                              : dropper_hot ? IM_COL32(0x60, 0x60, 0x60, 235) : IM_COL32(0x33, 0x33, 0x33, 215),
                      ImGui::GetStyle().FrameRounding);
    ui_eyedropper_icon(dl, b0 + ImVec2(s, s) * 0.5f, s * 0.32f, ImGui::GetColorU32(ImGuiCol_Text));
    tooltip(picking ? "Picking: click a stroke in the viewport (click here or Esc to stop)"
                    : "Brush eyedropper: click a stroke in the viewport to give the selected shapes its brush");
    bool revert = false;
    if (revert_tip) {
      ImGui::SetCursorScreenPos(ImVec2(pa.x + size.x + gap, pa.y));
      revert = ui_revert_button("##revert_brush", revert_tip);
    }
    /* Back below the row (an item there, as ImGui wants after moving the cursor). */
    ImGui::SetCursorScreenPos(after);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    return revert;
  }
}

}  // namespace oanim
