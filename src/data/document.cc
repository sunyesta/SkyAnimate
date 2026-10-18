#include "data/document.hh"

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
        def_int<Document>("frame_current", "Current Frame", "Frame shown in the viewport", &Document::frame_current,
                          0, 1048574),
        def_float<Document>("fps", "Frame Rate", "Frames per second", &Document::fps, 1.0f, 240.0f),
        def_int<Document>("width", "Width", "Output width in pixels", &Document::width, 4, 16384),
        def_int<Document>("height", "Height", "Output height in pixels", &Document::height, 4, 16384),
        def_string<Document>("action", "Action",
                             "Action being edited in the timeline; empty means the mainline", &Document::action),
        def_int<Document>("counter", "Counter", "Test value used by the milestone 1 operators", &Document::counter,
                          -1000000, 1000000),
    };
    return s;
  }();
  return srna;
}

std::vector<std::string> document_selectable_actions(const Document & /*doc*/)
{
  return {};
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
