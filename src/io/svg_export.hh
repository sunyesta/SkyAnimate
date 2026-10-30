#pragma once

/* Render as SVG: the current frame through the camera, as Render Frame draws it, written as vectors. Fills are
 * their Bezier outlines; a line is the outline of what the renderer draws for it (its width at every point, its
 * joins and ends), so it can be filled as one path; masks are SVG masks painted as the cards paint theirs (Start
 * With, Add and Subtract, Invert, Union, Difference and Intersection, Grow and Shrink, Extend, Fills or Strokes
 * Only, Clip to Below); faded groups and see-through layers are groups with an opacity; media pictures and brushed
 * lines' stamps go in as PNGs (io/vector_export.hh). */

#include <string>

namespace oanim {

struct Document;

struct SvgExport {
  /* The picture's size in pixels (0: the document's). */
  int width = 0, height = 0;
  /* No background: clear where nothing is drawn. */
  bool transparent = false;
};

/* What a file holds, for the report. */
struct SvgExportReport {
  int fills = 0;
  int lines = 0;
  int masks = 0;
  /* Brushed lines, as pictures of their stamps. */
  int brushed = 0;
  int pictures = 0;
};

/* The frame's SVG text. */
std::string svg_frame(const Document &doc, const SvgExport &options, SvgExportReport *r_report = nullptr);
/* Writes it to `path` (UTF-8). */
bool svg_export_frame(const Document &doc, const std::string &path, const SvgExport &options, std::string *r_error,
                      SvgExportReport *r_report = nullptr);

}  // namespace oanim
