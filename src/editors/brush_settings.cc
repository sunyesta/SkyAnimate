#include "editors/brush_settings.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>

#include "imgui.h"
#include "imgui_stdlib.h"
#include <SDL3/SDL_opengl.h>

#include "assets/asset_library.hh"
#include "assets/brushes.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "gpu/gl.hh"
#include "gpu/scene_renderer.hh"
#include "wm/context.hh"

namespace sky {

static constexpr const char *POPUP = "Brush Settings";
static constexpr float DEG = 3.14159265f / 180.0f;

namespace {

struct State {
  bool open_requested = false;
  BrushSettings settings;
  Style style;
  BrushSettingsDone done;
  std::string save_name;
  std::string save_error;
  std::unique_ptr<SceneRenderer> preview;
  /* Stamp thumbnails, by BrushImage::id. */
  std::map<uint64_t, GLuint> thumbnails;
};

State &state()
{
  static State s;
  return s;
}

}  // namespace

void brush_settings_open(const BrushSettings &current, const Style &style, BrushSettingsDone done)
{
  State &s = state();
  s.settings = current;
  s.style = style;
  s.done = std::move(done);
  s.open_requested = true;
}

static GLuint thumbnail(const BrushImage &image)
{
  State &s = state();
  if (auto it = s.thumbnails.find(image.id); it != s.thumbnails.end()) {
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
  s.thumbnails[image.id] = texture;
  return texture;
}

/* One stamp in the grid: the brush's first image on white, black for ink brushes. Returns true
 * when clicked. */
static bool stamp(const char *id, const Brush *brush, bool selected, float size)
{
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const ImVec2 b(a.x + size, a.y + size);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (!brush) {
    dl->AddRectFilled(a, b, selected ? IM_COL32(0x9a, 0xd0, 0xc8, 255) : IM_COL32(0xf0, 0xf0, 0xf0, 255));
    const ImVec2 text = ImGui::CalcTextSize("None");
    dl->AddText(ImVec2(a.x + (size - text.x) * 0.5f, a.y + (size - text.y) * 0.5f), IM_COL32(0x20, 0x20, 0x20, 255),
                "None");
  }
  else {
    dl->AddRectFilled(a, b, selected ? IM_COL32(0x9a, 0xd0, 0xc8, 255) : IM_COL32(0xff, 0xff, 0xff, 255));
    const std::vector<BrushImage> &images = brush_images(*brush);
    if (!images.empty()) {
      const BrushImage &image = images[0];
      const float pad = 3.0f, room = size - pad * 2.0f;
      const float f = room / float(std::max(image.width, image.height));
      const ImVec2 dim(image.width * f, image.height * f);
      const ImVec2 at(a.x + pad + (room - dim.x) * 0.5f, a.y + pad + (room - dim.y) * 0.5f);
      dl->AddImage(ImTextureRef(ImTextureID(intptr_t(thumbnail(image)))), at, ImVec2(at.x + dim.x, at.y + dim.y),
                   ImVec2(0, 0), ImVec2(1, 1), brush->is_color ? IM_COL32_WHITE : IM_COL32_BLACK);
    }
    if (brush->files.size() > 1) {
      /* Moho marks multi-brushes with dots in the corner. */
      dl->AddText(ImVec2(a.x + 2, a.y - 4), IM_COL32(0x20, 0x20, 0x20, 255), "***");
    }
  }
  if (hovered || selected) {
    dl->AddRect(a, b, selected ? IM_COL32(0x4f, 0x9a, 0xff, 255) : IM_COL32(0x90, 0x90, 0x90, 255), 0.0f, 0, 2.0f);
  }
  if (hovered && brush) {
    ImGui::SetTooltip("%s", brush->name.c_str());
  }
  return clicked;
}

/* A label and a value field in the settings table. */
static void field_label(const char *label)
{
  ImGui::TableNextColumn();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::TableNextColumn();
  ImGui::SetNextItemWidth(-1);
}

/* An S-shaped line painted with the settings, as Moho previews a brush. */
static void preview(const BrushSettings &settings, const Style &style, ImVec2 size)
{
  State &s = state();
  const int width = std::max(int(size.x), 1), height = std::max(int(size.y), 1);
  Document doc;
  doc.width = width;
  doc.height = height;
  doc.frame_current = 0;
  Layer &layer = document_add_layer(doc, LayerType::Vector, "Preview");
  VectorDrawing &drawing = layer.drawing;
  /* Document units: the frame is 2 tall; the line spans most of the width. */
  const float half_w = float(width) / float(height) * 0.88f;
  Curve curve;
  for (const float2 p : {float2(-half_w, -0.55f), float2(-half_w * 0.35f, 0.15f), float2(half_w * 0.35f, 0.15f),
                         float2(half_w, 0.6f)}) {
    curve.points.push_back({drawing_add_point(drawing, p)});
  }
  drawing.curves.push_back(curve);
  Shape shape;
  shape.has_fill = false;
  shape.style = style;
  shape.style.brush = settings;
  shape.style.line.a = 1.0f;
  shape.style.line_width = 22.0f / (float(height) * 0.5f);
  for (int i = 0; i < curve.segment_count(); i++) {
    shape.edges.push_back({0, i});
  }
  drawing.shapes.push_back(shape);

  if (!s.preview) {
    s.preview = std::make_unique<SceneRenderer>();
  }
  const float scale = float(height) * 0.5f;
  const Transform2D to_pixels{scale, 0, 0, scale, float(width) * 0.5f, float(height) * 0.5f};
  const Color background{0.23f, 0.23f, 0.23f, 1.0f};
  const unsigned int texture = s.preview->render(doc, width, height, to_pixels, 24, 8, &background);
  const ImVec2 a = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  if (texture) {
    /* The render is bottom-up. */
    ImGui::GetWindowDrawList()->AddImage(ImTextureRef(ImTextureID(intptr_t(texture))), a,
                                         ImVec2(a.x + size.x, a.y + size.y), ImVec2(0, 1), ImVec2(1, 0));
  }
}

void brush_settings_draw(Context & /*C*/)
{
  State &s = state();
  if (s.open_requested) {
    ImGui::OpenPopup(POPUP);
    s.open_requested = false;
  }
  const ImGuiViewport *main = ImGui::GetMainViewport();
  ImGui::SetNextWindowSize(ImVec2(680, 700), ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(main->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(POPUP, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
    return;
  }
  BrushSettings &b = s.settings;
  const bool alt = ImGui::GetIO().KeyAlt;

  /* The stamps. A click takes the brush with its own settings; Alt-click keeps the settings. */
  const float cell = 44.0f, gap = 3.0f;
  if (ImGui::BeginChild("##stamps", ImVec2(0, 290), ImGuiChildFlags_Borders)) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const int columns = std::max(1, int((ImGui::GetContentRegionAvail().x + gap) / (cell + gap)));
    int column = 0;
    const auto next = [&] {
      if (++column < columns) {
        ImGui::SameLine();
      }
      else {
        column = 0;
      }
    };
    if (stamp("##none", nullptr, b.name.empty(), cell)) {
      b.name.clear();
    }
    next();
    const std::vector<Brush> &all = brushes();
    for (size_t i = 0; i < all.size(); i++) {
      ImGui::PushID(int(i));
      if (stamp("##stamp", &all[i], b.name == all[i].name, cell)) {
        if (!alt) {
          b = all[i].defaults;
        }
        b.name = all[i].name;
      }
      ImGui::PopID();
      next();
    }
    ImGui::PopStyleVar();
    if (all.empty()) {
      ImGui::NewLine();
      ImGui::TextDisabled(asset_library_ready() ? "No brushes in the asset library yet: Edit > Preferences > Import Moho Brushes."
                                                : "Set up an asset library first: Edit > Preferences.");
    }
  }
  ImGui::EndChild();

  ImGui::TextUnformatted(b.name.empty() ? "No Brush" : b.name.c_str());
  ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
  ImGui::TextDisabled("Alt-click to carry values to new stamp");

  ImGui::BeginDisabled(b.name.empty());
  if (ImGui::BeginTable("##settings", 5, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("##checks", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthStretch, 1.2f);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::TableSetupColumn("##drift_label", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("##drift_value", ImGuiTableColumnFlags_WidthStretch, 0.6f);

    float jitter = b.jitter / DEG, spacing = b.spacing * 100.0f, amp = b.size_variation_amp * 100.0f,
          scale = b.size_variation_scale * 100.0f, drift = b.angle_drift / DEG, hue = b.hue_drift * 100.0f,
          sat = b.sat_drift * 100.0f, val = b.val_drift * 100.0f;
    for (int row = 0; row < 6; row++) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      switch (row) {
        case 0: ImGui::Checkbox("Align brush with curve", &b.align); break;
        case 1: ImGui::Checkbox("Random stamp order", &b.random_order); break;
        case 2: ImGui::Checkbox("Merge alpha", &b.merged_alpha); break;
        case 3: ImGui::Checkbox("Tint brush using stroke color", &b.tint); break;
        case 4: ImGui::Checkbox("Line boil", &b.randomize); break;
        default: break;
      }
      ImGui::PushID(row);
      switch (row) {
        case 0:
          field_label("Jitter angle");
          if (ImGui::DragFloat("##v", &jitter, 1.0f, 0.0f, 360.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) {
            b.jitter = jitter * DEG;
          }
          break;
        case 1:
          field_label("Spacing");
          if (ImGui::DragFloat("##v", &spacing, 0.5f, 1.0f, 1000.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            b.spacing = spacing / 100.0f;
          }
          break;
        case 2:
          field_label("Size variation amplitude");
          if (ImGui::DragFloat("##v", &amp, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            b.size_variation_amp = amp / 100.0f;
          }
          break;
        case 3:
          field_label("Size variation scale");
          if (ImGui::DragFloat("##v", &scale, 0.5f, 1.0f, 1000.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            b.size_variation_scale = scale / 100.0f;
          }
          break;
        case 4:
          field_label("Randomness interval");
          ImGui::DragInt("##v", &b.random_interval, 0.1f, 1, 1000, "%d fr", ImGuiSliderFlags_AlwaysClamp);
          break;
        case 5:
          field_label("Angle drift");
          if (ImGui::DragFloat("##v", &drift, 1.0f, 0.0f, 3600.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) {
            b.angle_drift = drift * DEG;
          }
          break;
        default: break;
      }
      /* Colour drift: random hue, saturation and value per stamp. */
      switch (row) {
        case 0:
          ImGui::TableNextColumn();
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted("Color drift");
          ImGui::TableNextColumn();
          break;
        case 1:
          field_label("Hue");
          if (ImGui::DragFloat("##hue", &hue, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            b.hue_drift = hue / 100.0f;
          }
          break;
        case 2:
          field_label("Saturation");
          if (ImGui::DragFloat("##sat", &sat, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            b.sat_drift = sat / 100.0f;
          }
          break;
        case 3:
          field_label("Value");
          if (ImGui::DragFloat("##val", &val, 0.5f, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
            b.val_drift = val / 100.0f;
          }
          break;
        default: break;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  const float buttons_h = ImGui::GetFrameHeightWithSpacing() + 6.0f;
  const ImVec2 room = ImGui::GetContentRegionAvail();
  preview(b, s.style, ImVec2(room.x, std::max(60.0f, room.y - buttons_h)));

  /* Save Brush... / OK / Cancel along the bottom right. */
  const float w = 90.0f;
  ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - w * 3 - ImGui::GetStyle().ItemSpacing.x * 2);
  ImGui::BeginDisabled(b.name.empty());
  if (ImGui::Button("Save Brush...", ImVec2(w, 0))) {
    s.save_name = b.name + " copy";
    s.save_error.clear();
    ImGui::OpenPopup("Save Brush");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  bool close = false;
  const bool keys = !ImGui::IsPopupOpen("Save Brush");
  if (ImGui::Button("OK", ImVec2(w, 0)) || (keys && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
    if (s.done) {
      s.done(b);
    }
    close = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(w, 0)) || (keys && ImGui::IsKeyPressed(ImGuiKey_Escape))) {
    close = true;
  }

  if (ImGui::BeginPopupModal("Save Brush", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Save these settings as a new brush in the asset library.");
    ImGui::SetNextItemWidth(320);
    if (ImGui::IsWindowAppearing()) {
      ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputText("Name", &s.save_name);
    if (!s.save_error.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", s.save_error.c_str());
    }
    if (ImGui::Button("Save", ImVec2(w, 0))) {
      if (brush_save(s.save_name, b, &s.save_error)) {
        report(ReportType::Info, "Saved brush " + s.save_name);
        b.name = s.save_name;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(w, 0))) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (close) {
    s.done = nullptr;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

}  // namespace sky
