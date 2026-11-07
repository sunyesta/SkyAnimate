#pragma once

/* Sculpting drawings, as Blender's Grease Pencil sculpt brushes do strokes: a round brush works on the
 * lines it covers, more at its middle by its falloff.
 *
 * - Grab picks up the line under the brush when the stroke starts and drags it with the mouse.
 * - Push nudges whatever of the line is under the brush along the way the mouse goes.
 * - Smooth evens the line out.
 * - Thickness widens the line (Invert: thins it).
 * - Pinch pulls the line toward the brush's middle (Invert: Inflate, away from it).
 * - Twist turns the line about the brush's middle (Invert: the other way).
 *
 * The brush works on the line itself, not just on its points: when a stroke starts, every curve being
 * edited is sampled densely (a few samples per brush width), each of its points anchored to its own
 * sample. Each dab moves the samples; then the curve is fitted back to them: every point goes where its
 * sample went, and the handles of each segment the dab touched are fitted to the samples between its
 * points (least squares; a smooth point's two handles stay in line). Segments it didn't touch keep their
 * points and handles exactly. The samples last the whole stroke, so fitting never builds on its own
 * error. Only the fitted curve shows.
 *
 * It works where the points show (posed by the bones; document space) and stores the result, so on any
 * frame but 0 it poses the drawing (auto-key keys it) and on frame 0 it edits the setup. With X mirror, a
 * layer without a side is sculpted on both sides of its own axis at once (each dab also made mirrored, as
 * Blender's symmetry), and a side's layer passes its change to its partner (Ear_L into Ear_R), widths too. Without
 * Dyntopo it never adds or removes points; it works on the curves as the detail level being edited shows them (finer detail rides
 * along), and with Selected Only leaves unselected points and their handles where they are.
 *
 * Dyntopo (Blender's dynamic topology, for lines): on a layer whose drawing has no animation, the stroke adds a point
 * wherever a segment can't follow the sculpted line to within the Detail Size (Subdivide), and once the stroke ends
 * takes out every point on the line it touched that the line keeps its shape without (Collapse), so it ends with
 * as few points as hold the shape. The stroke's change is the drawing's own (its setup pose) on any frame: no keys. */

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct Document;

/* Surface Smooth: takes a line's lumps out hard while its overall curve stays. */
enum class SculptBrush { Grab, Push, Smooth, Thickness, Pinch, Twist, SurfaceSmooth };
inline constexpr int SCULPT_BRUSH_COUNT = 7;
/* Blender's brush falloffs: Smooth eases out, Sphere stays strong almost to the edge, Constant is
 * the same all over. */
enum class SculptFalloff { Smooth, Sphere, Constant };

/* "GRAB", "PUSH"...: how Python and saved presets name them. */
const char *sculpt_brush_id(SculptBrush brush);
bool sculpt_brush_from_id(const std::string &id, SculptBrush *r_brush);
/* "Grab", "Push"...: how the interface names them. */
const char *sculpt_brush_name(SculptBrush brush);
const char *sculpt_falloff_id(SculptFalloff falloff);
bool sculpt_falloff_from_id(const std::string &id, SculptFalloff *r_falloff);

/* Dyntopo's Refine Method, as Blender's: add points, take them out, or both. */
enum class SculptRefine { Subdivide, Collapse, SubdivideCollapse };
const char *sculpt_refine_id(SculptRefine refine);
const char *sculpt_refine_name(SculptRefine refine);
bool sculpt_refine_from_id(const std::string &id, SculptRefine *r_refine);

/* How much of a brush's effect reaches `t` of the way out from its middle (0 to 1). */
float sculpt_falloff(SculptFalloff falloff, float t);

/* One dab of a stroke. */
struct SculptDab {
  SculptBrush brush = SculptBrush::Grab;
  /* Document space. */
  float2 center;
  /* Grab and Push: how far the mouse went since the last dab (document space). */
  float2 move;
  /* Document units. */
  float radius = 0.1f;
  float strength = 0.5f;
  /* Smooth, Thickness, Pinch and Twist work on as long as the brush is held: how much of a full
   * step this dab is (a frame's share of a tenth of a second while dragging; 1 for a scripted dab). */
  float amount = 1.0f;
  SculptFalloff falloff = SculptFalloff::Smooth;
  /* Ctrl: Thickness thins, Pinch inflates, Twist turns the other way. */
  bool invert = false;
  /* Only the selected points (Blender's selection mask): the line between an unselected point and
   * the next moves less toward it, and the point and its handles not at all. */
  bool selected_only = false;
  /* X mirror editing: a moved point's mirror image across the layer's vertical axis follows. */
  bool mirror_x = false;
  /* Surface Smooth's Lump Sensitivity: 0-1 (no top; 0 takes out nothing), how long a lump it takes out, the same
   * on any canvas whatever its resolution, the brush's size or the zoom (see sculpt_surface_tolerance). */
  float surface_sensitivity = 0.25f;
  /* Dyntopo: how far the line may stray from the shape sculpted (document units; the tool's Detail Size is in screen
   * pixels), 0 for off; and its Refine Method. Only on layers sculpt_dyntopo_blocked lets through. */
  float detail = 0.0f;
  SculptRefine refine = SculptRefine::SubdivideCollapse;
};

/* The stroke's samples of the curves being sculpted (see above), made when it starts. */
struct SculptStroke {
  struct Curve {
    /* The curve (an index into the drawing's curves), and its points shown at the level being
     * edited (indices into its points). */
    int curve = -1;
    bool closed = false;
    std::vector<int> points;
    /* Each point's sample, and whether its handles were in line when the stroke started (a smooth point
     * whose handles were bent stays free, as a corner). */
    std::vector<int> anchor;
    std::vector<bool> in_line;
    /* Each point a corner Surface Smooth keeps: not set smooth (aligned), and peaked or its handles bent. */
    std::vector<bool> corner;
    /* Each point's handles as they show now (document space offsets): a handle the curve works out for
     * itself is worked out where the bones put the points, so it can't be found again from the stored
     * drawing. */
    std::vector<float2> handle_in, handle_out;
    /* How much shorter each handle shows than it is stored (detail: a handle facing finer detail). */
    std::vector<float> factor_in, factor_out;
    /* The samples, in order along the curve (document space), the line's width there, how much the
     * brush may move each (Selected Only), and where each was when the stroke started. */
    std::vector<float2> pos;
    /* Each sample's place in its segment's own parameter (0 at the segment's first point): the samples lie at even
     * steps along the line, and the fitting gives a segment back exactly from them. */
    std::vector<float> param;
    std::vector<float> width, width_start, select;
    /* Grab: how much of the move each sample takes, from where the stroke started (and, mirrored within
     * the layer, from where its mirror image started). */
    std::vector<float> grab, grab_mirror;
    /* Surface Smooth: where each point and its handles go, the line fitted when the stroke started
     * (sculpt_surface_target; document space, handles as offsets). */
    std::vector<float2> target_pos, target_in, target_out;
    /* Dyntopo: each point the stroke's fitting reached (one of its segments was fitted again), which Collapse may take
     * out when the stroke ends. */
    std::vector<bool> touched;
  };
  struct Layer {
    uint64_t id = 0;
    /* Each point's stored -> document map (its bones' move, then the layer's place). */
    std::vector<Transform2D> to_shown;
    std::vector<Curve> curves;
    /* X mirror within the layer (it has no side partner): every dab is also made mirrored across the
     * layer's own axis, so both sides are sculpted as one. `mirror` reflects across that axis as it shows
     * (document space). */
    bool mirror_self = false;
    Transform2D mirror;
    /* Dyntopo works on this layer, and how many points its drawing had when the stroke started (those after are the
     * stroke's own). */
    bool dyntopo = false;
    size_t points_before = 0;
  };
  std::vector<Layer> layers;
  bool empty() const { return layers.empty(); }
};

/* Samples the curves of `layers` (references only with their points unlocked) for a stroke whose first
 * dab is `dab`: spaced by its radius; Grab takes the line under it. Surface Smooth fits each curve, or takes the fit
 * from `fitted` (an earlier stroke with the same tolerance: the viewport's ghost) for each curve still just as it
 * was then, so only curves changed since are fitted again. With `fit_within` (a document-space box: what the view
 * shows), a curve wholly outside it that has no such fit is left unfitted (its targets empty) for later. */
struct SculptBox {
  float2 lo, hi;
};
SculptStroke sculpt_stroke_begin(const Document &doc, const std::vector<uint64_t> &layers, const SculptDab &dab,
                                 const SculptStroke *fitted = nullptr, const SculptBox *fit_within = nullptr);
/* Surface Smooth's target for a stroke's curve (`target_pos`, `target_in`, `target_out`): the line resampled at even
 * steps along it and noise-reduced (the Whittaker-Eilers smoother of the third order: wiggles shorter than about
 * `lump` along the line go, while an arc or a bend keeps its shape; corners and open ends stay), then each of its
 * own points put where its place along the line went and each segment's handles fitted to the smoothed line between
 * its two points, so the line with all its points traces the smoothed one. A lump of 0: the line as it is. */
void sculpt_surface_target(SculptStroke::Curve &c, float lump);
/* The lump length Surface Smooth takes out (document units) from its Lump Sensitivity: wiggles up to that long along
 * the line go; 1 is a quarter of the canvas's height (document space spans the canvas as 2 units tall, whatever its
 * resolution), so 0.25 is a sixteenth of it. */
inline float sculpt_surface_tolerance(const SculptDab &dab)
{
  return (dab.surface_sensitivity > 0.0f ? dab.surface_sensitivity : 0.0f) * 0.5f;
}
/* Applies one dab to the stroke's samples and fits the curves to them. Returns how many points changed
 * (moved, or their handles or width did). */
int sculpt_stroke_dab(Document &doc, SculptStroke &stroke, const SculptDab &dab);
/* The stroke is over: with Dyntopo, Collapse takes out the points the line keeps its shape without (to within
 * `dab.detail`), and the drawings it worked on take their new shape as their setup pose. Returns how many points went. */
int sculpt_stroke_end(Document &doc, SculptStroke &stroke, const SculptDab &dab);
/* Why Dyntopo can't work on layer `id` (empty: it can): not a drawing of its own, a reference or referenced, detail
 * levels, X Mirror on (`mirror_x`), or its points and handles keyed (in the main timeline or an action). */
std::string sculpt_dyntopo_blocked(const Document &doc, uint64_t id, bool mirror_x);
/* Simplify: on each of `layers` Dyntopo works on, every point the drawing keeps its shape without (to within `detail`,
 * document units) goes, as Collapse does after a stroke. Returns how many went. */
int sculpt_simplify(Document &doc, const std::vector<uint64_t> &layers, float detail);

}  // namespace oanim
