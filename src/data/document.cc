#include "data/document.hh"

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
        def_int<Document>("counter", "Counter", "Test value used by the milestone 1 operators", &Document::counter,
                          -1000000, 1000000),
    };
    return s;
  }();
  return srna;
}

}  // namespace sky
