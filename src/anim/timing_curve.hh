#pragma once

/* A Custom stretch's timing as a curve (the Path Timing graph's): how far along (y: 0 at the stretch's start, 1 at its
 * key, past either for an overshoot or a backing up) against time through the stretch (x: 0 at its start, 1 at its key,
 * so the curve stretches with the stretch when its keys move). Bezier points with handles, as a drawing's: each handle
 * an offset from its point, the two in line while `aligned` (moving one turns the other). It is always a function of
 * time: no segment's handles reach past its other end, and the curve goes left to right.
 *
 * Kept with as few points as hold every frame's dot where it was: whatever made the timing (a brush, a dot slid, a
 * shape) gives a progress for each frame, and timing_fit makes the fewest points that put every frame within
 * TIMING_TOLERANCE of it (Dyntopo's Decimate, for timing): every frame a point to start with, those it keeps the
 * tolerance without taken out, cheapest first, each piece's handles fitted to its frames. Points placed by hand in the
 * graph stay as placed. */

#include <string>
#include <vector>

#include "core/math.hh"

namespace oanim {

struct TimingPoint {
  float2 co{};
  float2 in{}, out{};
  bool aligned = true;
  bool operator==(const TimingPoint &) const = default;
};

struct TimingCurve {
  /* In order left to right, the first at (0, 0) and the last at (1, 1). None: even (a straight line). */
  std::vector<TimingPoint> points;
  bool empty() const { return points.size() < 2; }
  bool operator==(const TimingCurve &) const = default;
};

/* How far a frame's dot may be from where the timing put it, as a share of the stretch's way: under half a pixel on a
 * path 2000 pixels long. */
inline constexpr float TIMING_TOLERANCE = 0.0002f;
/* A sculpt stroke's (with Dyntopo): freehand, so held to two pixels on a path 2000 pixels long, not chasing the brush's
 * own ripples with points. */
inline constexpr float TIMING_SCULPT_TOLERANCE = 0.001f;

/* How far along the curve is `t` (0-1) of the way through the stretch's time; an empty curve goes evenly. */
float timing_progress(const TimingCurve &curve, float t);
/* Its progress at each of a stretch's `n` + 1 frames (0 to n). */
std::vector<float> timing_frame_progress(const TimingCurve &curve, int n);
/* Its speed on each of `n` frames: each frame's step along, as a share of the even step (they average 1). */
std::vector<float> timing_speeds(const TimingCurve &curve, int n);

/* As few points as put each of the `progress` (one a frame, 0 to n; the first 0 and the last 1) within
 * `tolerance` of it, each with smooth handles a third of the way to its neighbours. Even progress: an empty curve. */
TimingCurve timing_fit(const std::vector<float> &progress, float tolerance = TIMING_TOLERANCE);
/* Decimate (the Path Timing graph's Edit mode): the curve's points taken out one at a time, the one whose piece across
 * its neighbours (their handles fitted again) moves the curve least first, until `ratio` (0-1) of those between the
 * ends is left; the ends stay, and pieces between points that stay neighbours keep their handles. How many went. `n`:
 * the stretch's frames (the curve is measured four times a frame). */
int timing_decimate(TimingCurve &curve, int n, float ratio);
/* timing_fit of per-frame speeds (any scale, signed: only how they compare counts). Speeds that add up to nothing go
 * evenly. */
TimingCurve timing_from_speeds(const std::vector<float> &speeds);
/* How many speeds timing_from_speeds has fitted a curve to (not kept from the last), for tests. */
uint64_t timing_fits_made();
/* A curve as a file or a script may give it: at least two points, all finite, going left to right from (0, 0) to
 * (1, 1). */
bool timing_valid(const TimingCurve &curve);
/* A straight curve with its two end points, for editing an even stretch. */
TimingCurve timing_even();

/* Point `i`'s handles as the curve uses them: each reaching at most across its segment, the two of a segment at most
 * that far together (both shortened alike, as Blender's F-curve handles), so the curve stays a function of time. */
void timing_point_handles(const TimingCurve &curve, size_t i, float2 &r_in, float2 &r_out);
/* A point put on the curve at time `x` (0-1, strictly inside), the segment split so its shape stays. Its index, or -1
 * on an empty curve or an `x` at a point already. */
int timing_point_insert(TimingCurve &curve, float x);
/* Moves point `i` to `co`, kept between its neighbours in time; its handles come with it. The ends never move (their
 * time and progress are the keys'). */
void timing_point_move(TimingCurve &curve, size_t i, float2 co);
/* Sets point `i`'s handle (`out` or in) to offset `h`, as a drawing's handle is dragged: an aligned point's other handle
 * turns to stay in line, keeping its length, unless `free` (Alt), which moves this one alone and makes the point
 * broken (Aligned in the Handles menu puts it back). */
void timing_handle_set(TimingCurve &curve, size_t i, bool out, float2 h, bool free);
/* Takes out point `i` (never an end); the curve fitted again over the frames it spanned would be another edit. */
bool timing_point_remove(TimingCurve &curve, size_t i);

/* A pick in the Path Timing graph's Edit mode: point `point` (`part` 0: its handles come with it) or one of its handles
 * on its own (-1 in, 1 out). */
struct TimingPick {
  int point = -1;
  int part = 0;
  bool operator==(const TimingPick &) const = default;
};
/* G R S on the picks (`kind`), as the graph shows the curve: `n` the stretch's frames, `aspect` frames a whole way
 * along. G moves the points picked by `offset` (x a share of the stretch's time, y the way along) together, kept between
 * the first not picked each side (the ends never move), and handles picked by it. R turns each picked point's handles
 * about it, and handles picked about their points, by `angle` radians (anticlockwise as shown). S scales by `factor`
 * (across, up): several points their places about their middle (kept in order) and their handles with them; one point
 * its handles; handles picked their lengths. An aligned point's handles stay in line; none reaches past its segment. */
void timing_transform(TimingCurve &curve, const std::vector<TimingPick> &picks, char kind, float2 offset, float angle,
                      float2 factor, int n, float aspect);
/* The handles of `points`: Aligned in line along the way between theirs (as shown, each keeping its length), Broken
 * free of each other, Flat level (each as long as it was on screen), so the move eases into and out of the point. */
enum class TimingHandles { Aligned, Broken, Flat };
void timing_handles_type(TimingCurve &curve, const std::vector<int> &points, TimingHandles type, int n, float aspect);

/* Grab and Smooth, the Path Timing graph's Sculpt mode, on a stretch of `n` frames along the stroke's places `path` (x
 * in frames from the stretch's start, y the way along), within `radius` frames of the brush (`aspect` frames to a whole
 * way along, as the graph shows them, so the brush is a circle there), `strength` how much, `lock_x` up and down only.
 * As a drawing's sculpt does, the curve is sampled finely (four places a frame and one on each point) and the brush
 * moves the samples: Grab moves them by how near each was to where it was pressed, up and down and in time (time
 * squeezed further ahead only as a long drag needs, smoothly, so nothing piles up or crosses); Smooth evens them out. The curve is then fitted back to them. Without `dyntopo`, each point goes where its sample
 * went and the pieces the stroke moved have their handles fitted again (a smooth point's in line), none added or taken
 * out. With it, those pieces get a point every frame, then the ones the curve keeps its shape without go (points it
 * neither made nor moved stay). Either way the pieces it didn't move stay exactly as they were. */
enum class TimingBrush { Grab, Smooth };
TimingCurve timing_sculpt(const TimingCurve &curve, int n, TimingBrush brush, const std::vector<float2> &path,
                          float radius, float strength, float aspect, bool lock_x, bool dyntopo);
/* A stroke's places as anim.timing_sculpt takes them, "frame,along;frame,along": parsed with `start` taken off each
 * frame (so from the stretch's start), and written. */
std::vector<float2> timing_stroke_parse(const std::string &text, float start);
std::string timing_stroke_string(const std::vector<float2> &path);

}  // namespace oanim
