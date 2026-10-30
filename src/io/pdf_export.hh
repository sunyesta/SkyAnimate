#pragma once

/* Export PDF: frames through the camera as vector pages (io/vector_export.hh), one page a frame, each looking as
 * Render Frame's picture does. Every layer is a PDF layer (Optional Content) named and nested as in the Layers
 * editor, so a PDF viewer or Illustrator can switch layers off and on; hidden layers and guides are in the file
 * too, switched off. Masks are soft masks, faded groups transparency groups, pictures images. */

#include <string>

namespace oanim {

struct Document;

struct PdfExport {
  /* The page's size in pixels (0: the document's), a pixel three quarters of a point (96 to the inch). */
  int width = 0, height = 0;
  /* No background: clear where nothing is drawn. */
  bool transparent = false;
  /* The frames, a page each (first > last: the current frame alone). */
  int frame_first = 0, frame_last = -1;
};

/* What a file holds, for the report. */
struct PdfExportReport {
  int pages = 0;
  int layers = 0;
  int masks = 0;
  int pictures = 0;
  /* Brushed lines, as pictures of their stamps. */
  int brushed = 0;
  size_t bytes = 0;
};

/* Writes the PDF to `path` (UTF-8). The document is left as it was (other frames are worked out on a copy). */
bool pdf_export(const Document &doc, const std::string &path, const PdfExport &options, std::string *r_error,
                PdfExportReport *r_report = nullptr);

}  // namespace oanim
