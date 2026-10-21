#include "data/document.hh"

#include "anim/animation.hh"
#include "data/camera.hh"

#include <cstdio>

namespace sky {

const rna::StructRNA &Document::rna()
{
  static const rna::StructRNA srna = [] {
    using namespace rna;
    StructRNA s;
    s.identifier = "Document";
    s.name = "Document";
    s.properties = {
        def_string<Document>("name", "Name", "Name of the document", &Document::name),
        def_int<Document>("frame_start", "Start", "First frame of the animation", &Document::frame_start, 0, 1048574),
        def_int<Document>("frame_end", "End", "Last frame of the animation", &Document::frame_end, 0, 1048574),
        [] {
          /* Setting the frame shows its animation at once (for scripts as much as the UI). */
          rna::PropertyRNA prop = def_int<Document>("frame_current", "Current Frame", "Frame shown in the viewport",
                                                    &Document::frame_current, 0, 1048574);
          prop.set = [](void *data, const rna::Value &v) {
            Document &doc = *static_cast<Document *>(data);
            doc.frame_current = std::get<int>(v);
            animation_sync(doc);
          };
          return prop;
        }(),
        def_bool<Document>("auto_key", "Auto Keying",
                           "Editing at a frame other than 0 keys what changes there (the record button)",
                           &Document::auto_key),
        def_float<Document>("fps", "Frame Rate", "Frames per second", &Document::fps, 1.0f, 240.0f),
        def_float<Document>("gravity", "Gravity", "How hard gravity pulls dynamic bones down (by their weight)",
                            &Document::gravity, -100.0f, 100.0f),
        def_float<Document>("wind", "Wind", "How hard the wind pushes dynamic bones (all the same)",
                            &Document::wind, -100.0f, 100.0f),
        def_float_fn<Document>(
            "wind_direction", "Wind Direction", "The way the wind blows, in degrees (0: to the right)", -360.0f,
            360.0f, [](const Document &d) { return d.wind_direction * 180.0f / PI; },
            [](Document &d, float v) { d.wind_direction = v * PI / 180.0f; }),
        def_float<Document>("wind_gusts", "Gusts",
                            "How much gusts make the wind stronger and weaker (and waver its direction): "
                            "0 is steady, 0.5 between half and one and a half times as strong",
                            &Document::wind_gusts, 0.0f, 2.0f),
        def_float<Document>("wind_gust_speed", "Gust Speed",
                            "How often gusts come: about every two seconds at 1; more often, they are shorter",
                            &Document::wind_gust_speed, 0.05f, 10.0f),
        def_float<Document>("wind_turbulence", "Turbulence",
                            "A small, fast flutter on top of the wind, along it and across it",
                            &Document::wind_turbulence, 0.0f, 2.0f),
        def_int<Document>("simulation_start", "Simulation Start", "The frame bone dynamics start simulating from",
                          &Document::simulation_start, 1, 1048574),
        def_float_fn<Document>(
            "camera_zoom", "Zoom", "The camera's zoom: 2 shows half as much (its keys change it at a steady rate)",
            0.001f, 10000.0f, [](const Document &d) { return camera_zoom(d); },
            [](Document &d, float v) { camera_set_zoom(d, v); }),
        def_float_fn<Document>(
            "camera_roll", "Roll", "The camera's turn about its centre, in degrees, counter-clockwise", -100000.0f,
            100000.0f, [](const Document &d) { return d.camera.rotation; },
            [](Document &d, float v) { document_camera(d).rotation = v; }),
        def_float<Document>("camera_passepartout", "Passepartout",
                            "How dark the canvas outside the camera's frame is in the viewport",
                            &Document::camera_passepartout, 0.0f, 1.0f),
        def_int<Document>("width", "Width", "Output width in pixels", &Document::width, 4, 16384),
        def_int<Document>("height", "Height", "Output height in pixels", &Document::height, 4, 16384),
        def_int<Document>("viewport_quality", "Viewport Quality",
                          "Stroke resolution in the viewport: pieces drawn per curve segment",
                          &Document::viewport_quality, 1, 64),
        def_int<Document>("render_quality", "Render Quality",
                          "Stroke resolution in renders: pieces drawn per curve segment", &Document::render_quality,
                          1, 64),
        def_int<Document>("viewport_samples", "Viewport Samples",
                          "Anti-aliasing samples per pixel in the viewport (1, 2, 4, 8 or 16)",
                          &Document::viewport_samples, 1, 16),
        def_int<Document>("render_samples", "Render Samples",
                          "Anti-aliasing samples per pixel in renders (1, 2, 4, 8 or 16)", &Document::render_samples,
                          1, 16),
        [] {
          rna::PropertyRNA p = def_string<Document>(
              "action", "Action", "Action being edited in the timeline; empty means the mainline",
              &Document::action);
          /* Switching swaps the action's keys in. */
          p.set = [](void *data, const rna::Value &v) {
            document_set_action_by_name(*static_cast<Document *>(data), std::get<std::string>(v));
          };
          return p;
        }(),
        def_int_readonly<Document>("layer_count", "Layers", "Number of layers, including those inside groups",
                                   [](const Document &doc) { return int(document_layers_flat(doc).size()); }),
    };
    return s;
  }();
  return srna;
}

Text *document_text_find(Document &doc, const std::string &name)
{
  for (Text &text : doc.texts) {
    if (text.name == name) {
      return &text;
    }
  }
  return nullptr;
}

std::string document_text_unique_name(const Document &doc, const std::string &base)
{
  const auto taken = [&](const std::string &name) {
    for (const Text &text : doc.texts) {
      if (text.name == name) {
        return true;
      }
    }
    return false;
  };
  if (!base.empty() && !taken(base)) {
    return base;
  }
  const std::string stem = base.empty() ? "Text" : base;
  for (int i = 1;; i++) {
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), ".%03d", i);
    if (!taken(stem + suffix)) {
      return stem + suffix;
    }
  }
}

Text &document_text_add(Document &doc, const std::string &name, const std::string &body)
{
  doc.texts.push_back({document_text_unique_name(doc, name), body, ""});
  return doc.texts.back();
}

static void hash_mix(uint64_t &h, uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
}

static void selection_hash_layers(const std::vector<Layer> &layers, uint64_t &h)
{
  for (const Layer &layer : layers) {
    hash_mix(h, layer.id);
    hash_mix(h, layer.selected ? 1 : 0);
    hash_mix(h, layer.active_bone);
    for (const Bone &b : layer.bones) {
      hash_mix(h, (b.selected ? 1 : 0) | (b.select_head ? 2 : 0) | (b.select_tail ? 4 : 0));
    }
    for (const MeshPoint &p : layer.drawing.points) {
      hash_mix(h, p.selected ? 1 : 0);
    }
    for (const Curve &curve : layer.drawing.curves) {
      for (const CurvePoint &cp : curve.points) {
        hash_mix(h, (cp.select_in ? 1 : 0) | (cp.select_out ? 2 : 0));
      }
    }
    selection_hash_layers(layer.children, h);
  }
}

uint64_t document_selection_hash(const Document &doc)
{
  uint64_t h = doc.active_layer;
  selection_hash_layers(doc.layers, h);
  hash_mix(h, doc.camera.selected ? 1 : 0);
  return h;
}

}  // namespace sky
