#include "data/document.hh"

#include "anim/animation.hh"

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

}  // namespace sky
