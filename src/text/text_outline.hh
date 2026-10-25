#pragma once

/* Text laid out and turned into outlines with DirectWrite (Windows' own text engine: kerning,
 * ligatures, right-to-left scripts, and letters a font lacks taken from another). */

#include <string>
#include <vector>

#include "data/text.hh"
#include "data/vector.hh"

namespace oanim {

/* Where things are in a text's layout, in the layer's space: x across, y up, the layer's origin at the
 * top of the first line (for point text, on its left end, middle or right end as it is aligned; for a
 * text box, its left edge). */
struct TextCaret {
  float2 top, bottom;
};
struct TextBox {
  float2 min, max;
};

/* The text's letters as a drawing: a shape for each letter (its outlines, filling Nonzero, as fonts
 * are made), named after it, with its run's look. Points and shapes are numbered from 1. */
VectorDrawing text_outline_drawing(const TextSettings &text, std::vector<int> *r_chars = nullptr);
/* `r_chars` (optional): for each shape, the text position (UTF-16) of the letter it shows. */

/* The caret before position `pos` (UTF-16, as data/text.hh counts). */
TextCaret text_caret(const TextSettings &text, int pos);
/* The position nearest `p` (layer space). */
int text_hit(const TextSettings &text, float2 p);
/* Boxes covering [from, to), a line each. */
std::vector<TextBox> text_selection(const TextSettings &text, int from, int to);
/* The position at the start or end of `pos`'s line, or on the line above or below nearest across. */
int text_line_start(const TextSettings &text, int pos);
int text_line_end(const TextSettings &text, int pos);
int text_line_step(const TextSettings &text, int pos, int lines);
/* The whole text's bounds (its lines' boxes), for picking and the box's outline. */
TextBox text_bounds(const TextSettings &text);

/* The installed font families, by name (the system's, sorted). */
const std::vector<std::string> &text_font_families();
bool text_font_installed(const std::string &family);
/* The fonts the text asks for that aren't installed: what DirectWrite drew instead is shown. */
std::vector<std::string> text_missing_fonts(const TextSettings &text);
/* A font by its PostScript name ("Arial-Black", as Flash names them): its family, weight and italic.
 * False when no installed font has it. */
bool text_font_from_postscript(const std::string &postscript, std::string *r_family, int *r_weight,
                               bool *r_italic);

}  // namespace oanim
