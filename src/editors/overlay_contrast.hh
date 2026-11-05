/* Overlays that pick their colour by what is drawn under them: a vector wire, a point, a motion path's line is its
 * usual colour, and only where that is hard to see against the drawing under it does that piece take a lighter
 * colour (over something darker) or a darker one (over something lighter). The colours are the Preferences'. */

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/math.hh"

namespace oanim {

/* The canvas as the viewport last drew it, read back for the overlays to look under themselves. */
struct CanvasSample {
  /* Top row first, RGBA 8 bits each, premultiplied (as SceneRenderer::read_pixels). */
  std::vector<uint8_t> rgba;
  int width = 0, height = 0;
  /* Document space to the image's pixels (y up from its bottom row). */
  Transform2D document_to_pixels;
  /* What shows where nothing is drawn. */
  Color background;
  /* Changes whenever the pixels do. */
  uint64_t serial = 0;
  /* The colour shown at `at` (document space): the canvas over the background. False off the image. */
  bool at(float2 at, Color &r) const;
};

enum class ContrastPick : uint8_t { Base, Lighter, Darker };

/* Which colour reads over `under`: `base`, unless the two are hard to tell apart; then the lighter one over something
 * darker than `base`, the darker one over something lighter. `previous` (the piece before it along a line) keeps an
 * alternative until the contrast is clearly enough again, so a borderline stretch doesn't speckle. */
ContrastPick contrast_pick(const Color &under, const Color &base, ContrastPick previous = ContrastPick::Base);

/* A piece of a line and the colour it takes. */
struct ContrastPiece {
  float2 a, b;
  ContrastPick pick = ContrastPick::Base;
};
/* The line through `points` in pieces about `piece` long (in the points' units), each picked by what `under` says is
 * at its middle (none: `base`). Appended to `out`. */
void contrast_pieces(const std::vector<float2> &points, float piece,
                     const std::function<bool(float2, Color &)> &under, const Color &base,
                     std::vector<ContrastPiece> &out);

/* 0xRRGGBB as a colour. */
Color color_from_rgb(uint32_t rgb);
/* The colours overlays take (the Preferences'). */
struct ContrastColors {
  Color base, lighter, darker;
  const Color &of(ContrastPick pick) const
  {
    return pick == ContrastPick::Lighter ? lighter : pick == ContrastPick::Darker ? darker : base;
  }
};
ContrastColors wire_colors();
ContrastColors path_colors();
/* For a colour of its own (a path's set colour): itself, a lighter and a darker one. */
ContrastColors contrast_colors_of(const Color &base);

struct Document;
struct Layer;
struct MotionPath;

/* A piece of a motion path's line (document space) and its colour (ImU32); dashed where it overshoots. */
struct PathLinePiece {
  float2 a, b;
  uint32_t color = 0;
  bool dashed = false;
};
/* `p`'s line in pieces about `piece` long, each coloured by what `under` shows under it (viewport.cc). */
std::vector<PathLinePiece> motion_path_line_pieces(const Document &doc, const MotionPath &p, const CanvasSample *under,
                                                   float piece);

/* A motion path handle's line, key (0) to tip (1), in runs of colour: its own white, or with its tip picked (being
 * dragged) orange there fading to white at the key. */
struct PathHandleRun {
  float from = 0.0f, to = 1.0f;
  uint32_t color = 0;
};
std::vector<PathHandleRun> path_handle_runs(bool tip_picked);

/* For tests: the wires and points `layer`'s overlay draws (document space, ImU32 colours) over `under`, at `pixel`
 * document units a pixel. */
struct OverlayColorsTest {
  struct Wire {
    float2 a, b;
    uint32_t color = 0;
    float width = 1.0f;
    /* Its colour at `b` (blended from `color`); 0: one colour. */
    uint32_t color_b = 0;
  };
  struct Point {
    float2 at;
    uint32_t color = 0;
    uint32_t outline = 0;
    /* 1 a round dot, 0 a square. */
    float round = 0.0f;
  };
  std::vector<Wire> wires;
  std::vector<Point> points;
};
OverlayColorsTest overlay_colors_test(const Document &doc, const Layer &layer, const CanvasSample &under, float pixel,
                                      bool handles = false);

}  // namespace oanim
