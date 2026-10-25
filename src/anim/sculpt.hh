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
 * Blender's symmetry), and a side's layer passes its change to its partner (Ear_L into Ear_R), widths too. It never
 * adds or removes points, works on the curves as the detail level being edited shows them (finer detail rides
 * along), and with Selected Only leaves unselected points and their handles where they are. */

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct Document;

enum class SculptBrush { Grab, Push, Smooth, Thickness, Pinch, Twist };
inline constexpr int SCULPT_BRUSH_COUNT = 6;
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
    /* Each point's handles as they show now (document space offsets): a handle the curve works out for
     * itself is worked out where the bones put the points, so it can't be found again from the stored
     * drawing. */
    std::vector<float2> handle_in, handle_out;
    /* How much shorter each handle shows than it is stored (detail: a handle facing finer detail). */
    std::vector<float> factor_in, factor_out;
    /* The samples, in order along the curve (document space), the line's width there, how much the
     * brush may move each (Selected Only), and where each was when the stroke started. */
    std::vector<float2> pos;
    std::vector<float> width, width_start, select;
    /* Grab: how much of the move each sample takes, from where the stroke started (and, mirrored within
     * the layer, from where its mirror image started). */
    std::vector<float> grab, grab_mirror;
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
  };
  std::vector<Layer> layers;
  bool empty() const { return layers.empty(); }
};

/* Samples the curves of `layers` (references only with their points unlocked) for a stroke whose first
 * dab is `dab`: spaced by its radius; Grab takes the line under it. */
SculptStroke sculpt_stroke_begin(const Document &doc, const std::vector<uint64_t> &layers, const SculptDab &dab);
/* Applies one dab to the stroke's samples and fits the curves to them. Returns how many points changed
 * (moved, or their handles or width did). */
int sculpt_stroke_dab(Document &doc, SculptStroke &stroke, const SculptDab &dab);

}  // namespace oanim
