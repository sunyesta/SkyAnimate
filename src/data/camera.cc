#include "data/camera.hh"

#include <algorithm>
#include <cmath>

#include "data/document.hh"

namespace sky {

static constexpr float ZOOM_MIN = 1e-3f, ZOOM_MAX = 1e4f;

Layer &document_camera(Document &doc)
{
  Layer &camera = doc.camera;
  camera.id = CAMERA_LAYER_ID;
  camera.name = "Camera";
  camera.type = LayerType::Group;
  camera.scale = {1.0f, 1.0f};
  camera.opacity = 1.0f;
  camera.visible = true;
  const auto it = std::find_if(camera.custom_channels.begin(), camera.custom_channels.end(),
                               [](const CustomChannel &c) { return c.path == "zoom"; });
  if (it == camera.custom_channels.end()) {
    /* A pose, not a setting: auto-keying keys it, as it does the position and roll. */
    CustomChannel zoom;
    zoom.path = "zoom";
    zoom.keys = Channel<float>();
    camera.custom_channels.push_back(std::move(zoom));
  }
  return camera;
}

static const CustomChannel *zoom_channel(const Document &doc)
{
  for (const CustomChannel &c : doc.camera.custom_channels) {
    if (c.path == "zoom") {
      return &c;
    }
  }
  return nullptr;
}

float camera_zoom(const Document &doc)
{
  const CustomChannel *c = zoom_channel(doc);
  return c ? std::clamp(std::exp2(c->value), ZOOM_MIN, ZOOM_MAX) : 1.0f;
}

void camera_set_zoom(Document &doc, float zoom)
{
  Layer &camera = document_camera(doc);
  for (CustomChannel &c : camera.custom_channels) {
    if (c.path == "zoom") {
      c.value = std::log2(std::clamp(zoom, ZOOM_MIN, ZOOM_MAX));
    }
  }
}

float2 camera_half_size(const Document &doc)
{
  const float aspect = float(doc.width) / float(std::max(doc.height, 1));
  return float2(aspect, 1.0f) / camera_zoom(doc);
}

Transform2D camera_to_frame(const Document &doc)
{
  const float zoom = camera_zoom(doc);
  return Transform2D::scaling({zoom, zoom}) * Transform2D::rotate(-doc.camera.rotation * PI / 180.0f) *
         Transform2D::translate(-doc.camera.position);
}

void camera_frame_corners(const Document &doc, float2 r_corners[4])
{
  const Transform2D to_document = camera_to_frame(doc).inverted();
  const float aspect = float(doc.width) / float(std::max(doc.height, 1));
  const float2 corners[4] = {{-aspect, -1.0f}, {aspect, -1.0f}, {aspect, 1.0f}, {-aspect, 1.0f}};
  for (int i = 0; i < 4; i++) {
    r_corners[i] = to_document.apply(corners[i]);
  }
}

}  // namespace sky
