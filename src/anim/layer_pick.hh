#pragma once

/* Picking layers by where they show, for the Select Layer tool and Alt-click (layer.select_at): the
 * drawing layers drawn at a point, top first (Draw Order included), counting only where a layer
 * really shows: its fills and lines, and only where its masks (and its groups') let it show.
 * Media layers are never picked (references are traced over, not edited); hidden layers, guides
 * while guides are hidden, and layers outside Local View aren't either. */

#include <cstdint>
#include <functional>
#include <vector>

#include "core/math.hh"
#include "data/layer.hh"

namespace oanim {

struct Document;
struct Layer;

struct LayerPickOptions {
  /* How near a line counts as on it, in document units (a few pixels at the view's zoom). */
  float tolerance = 0.0f;
  /* Guides (layers not rendered) show: the viewport's overlays and their Guides are on. */
  bool show_guides = true;
  /* Local View's layers, or empty for every layer. */
  std::vector<uint64_t> local_view;
};

/* The drawing layers shown at `at` (document space), top first. */
std::vector<uint64_t> layers_at(const Document &doc, float2 at, const LayerPickOptions &options);

/* Whether `layer`'s masks (its own rows, its Clip to Below, and its groups') let it show at `at`.
 * Grow and Extend aren't counted. */
bool layer_masks_show_at(const Document &doc, const Layer &layer, float2 at);

/* End Lines at Edge (LayerMask::end_lines): where a layer's lines show, as its rows with it on say
 * (its own and its groups'), worked out once per frame with every mask shape in document space,
 * then asked point by point along the lines. Grow and Extend aren't counted. */
struct LineCutter {
  struct Piece {
    /* A shape run's outline in document space; whether it is an area (filled and closed, or Fill
     * Every Shape) and has a line, and the line's half width (document units). */
    std::vector<float2> outline;
    bool closed = false;
    bool area = false;
    bool line = false;
    float half = 0.0f;
    /* A shape whose loops fill together: all its closed loops (document space) and its fill rule, which
     * then say what is inside rather than the outline alone. */
    std::vector<std::vector<float2>> fill_loops;
    FillRule fill_rule = FillRule::Loops;
    /* Which of the row's layers it is of (their order in the step). */
    int source = 0;
  };
  struct Row {
    int mode = 0;
    bool invert = false;
    bool fills_only = false;
    bool strokes_only = false;
    bool inside_stroke = false;
    MaskCombine combine = MaskCombine::Union;
    /* How many layers the pieces are of. */
    int sources = 0;
    std::vector<Piece> pieces;
    /* On a level's first row: its card's Start With (the level starts with everything shown). */
    bool start_shown = false;
  };
  /* The steps of each card that ends lines, the layer's, then its groups' (each level must show). */
  std::vector<std::vector<Row>> levels;
  /* Ends: Flat (cut straight) rather than Brush (the brush's own taper and fade). */
  bool flat = false;
  /* Brush ends' Reach (LayerMask::line_reach, the first card with Brush ends): line widths past
   * the edge. */
  float reach = 0.0f;
  bool empty() const { return levels.empty(); }
};
/* The cutter for `layer` (empty when no card of it or its groups has End Lines at Edge). `render`:
 * cards on in renders rather than in the viewport. */
LineCutter line_cutter(const Document &doc, const Layer &layer, bool render);
/* Whether a line shows at `at` (document space) by those rows. */
bool line_cutter_shows(const LineCutter &cutter, float2 at);

/* A line's samples (shape_outline's: layer space, each point's half width, whether the outline runs on from it,
 * its join) as the runs that draw, each handed to `stroke` (points, half widths, closed, joins): the whole line,
 * or open runs where hidden edges break it and, with a `cutter` (`to_document` taking the line to its space),
 * where it crosses its shapes' edges, a round end stopped short of the edge as its Reach says or a flat one
 * there. */
using LineRunStroke = std::function<void(const std::vector<float2> &points, const std::vector<float> &half_widths,
                                         bool closed, const std::vector<StrokeJoin> &joins)>;
void line_runs(const std::vector<float2> &outline, const std::vector<float> &radii, const std::vector<bool> &visible,
               const std::vector<StrokeJoin> &joins, bool closed, const LineCutter *cutter,
               const Transform2D &to_document, const LineRunStroke &stroke);

/* The group a pick with Pick: Group takes instead of `id`: its outermost group (the layer itself at
 * the top level). */
uint64_t layer_pick_group(const Document &doc, uint64_t id);

/* How a pick changes the layer selection: the picked layer alone, added to it or taken out if
 * picked already (Shift), or taken out (Ctrl). */
enum class LayerPickMode { Set, Toggle, Remove };
/* Applies a pick; returns whether the selection changed. */
bool layer_pick_apply(Document &doc, uint64_t id, LayerPickMode mode);

}  // namespace oanim
