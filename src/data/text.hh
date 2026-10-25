#pragma once

/* Text layers: text kept as typed (runs of it, each in one font, size and look), whose letters'
 * outlines are the layer's drawing, made again whenever the text changes (text/text_outline.hh). The
 * outlines are saved with it, so a file shows its text on a computer without the font. Convert to
 * Vectors turns one into an ordinary vector layer. */

#include <cstdint>
#include <string>
#include <vector>

#include "data/vector.hh"

namespace oanim {

enum class TextAlign : uint8_t { Left, Center, Right, Justify };

/* A stretch of the text in one style. */
struct TextRun {
  /* UTF-8; "\n" starts a new line. */
  std::string text;
  /* The font family ("Segoe UI"), its weight (100-900: 400 regular, 700 bold) and italic. */
  std::string font = "Segoe UI";
  int weight = 400;
  bool italic = false;
  /* How big, in document units (the frame is 2 units tall): the font's em. */
  float size = 0.12f;
  /* Its look, as a shape's: fill and outline on or off, its own colours, width and brush, or a
   * shared material's (data/material.hh). Every letter of the run is a shape with this look. */
  bool has_fill = true;
  bool has_outline = false;
  Style style = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 0.004f, {}};
  uint64_t material = 0;
  std::string material_last;
  bool operator==(const TextRun &) const = default;
};

struct TextSettings {
  std::vector<TextRun> runs;
  TextAlign align = TextAlign::Left;
  /* Line spacing, a multiple of the font's own. */
  float line_spacing = 1.0f;
  /* Added after every letter, and after every space too, in ems of its run. */
  float letter_spacing = 0.0f;
  float word_spacing = 0.0f;
  /* A slant: each point moves across by this much of its height above its line's baseline. */
  float shear = 0.0f;
  /* A text box: lines wrap at this width (document units). 0: point text, lines breaking only at new
   * lines. */
  float box_width = 0.0f;
  /* Moving, turning, scaling or fading it never sets keys, whatever the record button says (as a
   * Media layer's): keying it by hand still does. On for new text. */
  bool no_auto_key = true;
  bool operator==(const TextSettings &) const = default;
};

/* The whole text, every run's joined. */
std::string text_string(const TextSettings &text);
/* Positions in the text count UTF-16 code units, as the layout's do. */
int text_length(const TextSettings &text);
/* Replaces positions [from, to) with `with` (UTF-8), in the style of the run it lands in (where two
 * meet, the one before, as typing goes on in the style of what came before). */
void text_replace(TextSettings &text, int from, int to, const std::string &with);
/* Whole text replaced, keeping the runs where it is unchanged: the edit is worked out as the shortest
 * change between the old text and the new (what typing in the Properties box does). */
void text_set_string(TextSettings &text, const std::string &with);
/* The runs over [from, to), split there so they cover just it, for changing their style; then
 * text_tidy joins neighbours that came out alike. With from == to, the run at that position. */
std::vector<TextRun *> text_runs_between(TextSettings &text, int from, int to);
void text_tidy(TextSettings &text);
/* The run a position is in (the one before, at a join). */
int text_run_at(const TextSettings &text, int pos);

/* UTF-8 and UTF-16, both ways (invalid bytes become U+FFFD). */
std::u16string utf8_to_utf16(const std::string &s);
std::string utf16_to_utf8(const std::u16string &s);

}  // namespace oanim
