/* Overlays that pick their colour by what is drawn under them: a vector wire, a point, a motion path's line is its
 * usual colour, and only where that is hard to see against the drawing under it does that piece take a lighter
 * colour (over something darker) or a darker one (over something lighter). The colours are the Preferences'. */

#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "core/math.hh"

namespace oanim {

/* The canvas as the viewport last drew it, read back for the overlays to look under themselves. */
struct CanvasSample {
  /* Bottom row first (as the GPU reads them: no flipping), RGBA 8 bits each, premultiplied. */
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

/* A fingerprint of a canvas read back: its pixels, size, place and background. The same picture drawn again (a click
 * that only picked something) has the same one, so the overlays' colours aren't worked out again. */
uint64_t canvas_hash(const std::vector<uint8_t> &rgba, int width, int height, const Transform2D &document_to_pixels,
                     const Color &background);

/* What the last canvas read back shows at `at` (document space), as the overlays see it; false with none read yet. */
bool canvas_now_at(float2 at, Color &r);

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
struct Context;

/* Test hook: what the overlays key a layer's own drawing by (its fingerprint, kept while its revision is, and what
 * is picked in it): a pick that changes it, with or without a new revision, makes the overlays again. */
uint64_t overlay_drawing_identity_test(const Layer &layer);

/* Test hook: what a Select-tool press at document point `at` grabs (zoom: pixels per unit, the view centred on 0), as
 * (layer id, points); with `drag`, picked as dragging it picks, every edited layer's selected points then. */
/* What a left press with the Motion Path tool's Edit brush does, from what is under it: a path handle; the path (a
 * key or dot); on the dot of the frame shown over the bone or point it traces, a drag moves the path and a click picks
 * that; off the paths, a bone or point is picked; else the path selection clears. */
enum class PathPress { Handle, Path, PathOrPick, Pick, Deselect };
PathPress motion_path_press_kind(bool on_handle, bool on_path, bool on_frame_shown, bool thing_under, bool alt);
/* Test hook: the Motion Path tool's pick at document point `at` (zoom pixels a unit): whether a bone or point is
 * there, and with `apply` picks it (`extend` adds). */
bool motion_path_pick_test(Context &C, float2 at, float zoom, bool extend, bool apply);
/* Test hook: whether a point, shown handle or (outside Points mode) line of the drawings being edited is at `at`. */
bool motion_path_vector_under_test(Context &C, float2 at, float zoom);
/* The motion path dots' hover while things move: `moving` now (returned); after a move the ghost of the hovered
 * dot's frame waits (path_hover_quiet) until the mouse leaves where it ended. */
bool path_hover_quiet_update(bool moving, float2 mouse);
bool path_hover_quiet();
std::vector<std::pair<uint64_t, std::vector<int>>> select_press_grab_test(Context &C, float2 at, float zoom,
                                                                          bool shift_or_ctrl, bool drag);

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
/* A motion path handle's line from `key` to `tip` (document space) in runs of colour (0 at the key, 1 at the tip):
 * the Preferences' violet, a lighter or darker shade only where what `under` says is there makes it hard to see (no
 * outline); a picked tip orange, fading to the violet at the key. Pieces about `piece` long. */
/* A click reach given in screen points, in pixels: times the display's scale and Preferences' Click Reach. */
float click_reach_pixels(float points);
/* Whether a path dot at `dot` lies under one of the handle lines (screen space), within `reach`: it is left out, so
 * a handle along the path reads as one bar. */
bool path_dot_under_handles(float2 dot, const std::vector<std::pair<float2, float2>> &handles, float reach);
std::vector<PathHandleRun> path_handle_pieces(float2 key, float2 tip, bool tip_picked, float piece,
                                              const std::function<bool(float2, Color &)> &under);

/* Outside the Motion Path tool a path is only looked at: its dots and keys all in its colour (lighter or darker where
 * hard to see, as its line), keys still diamonds (breakdowns hollow) and a touch lighter; no outlines (0); faded
 * before the scene's Start. */
struct PathQuietMark {
  enum Kind : uint8_t { Dot, Key, Breakdown };
  int frame = 0;
  float2 at;
  Kind kind = Dot;
  uint32_t fill = 0, outline = 0;
  /* Frames held still here (one mark for them all). */
  int held = 1;
};
std::vector<PathQuietMark> motion_path_quiet_marks(const Document &doc, const MotionPath &p,
                                                   const CanvasSample *under);

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
