#pragma once

/* The Stroke Pen's engine: turns what the pen or mouse sends into a line. Nothing here knows about the
 * window, so it runs the same in a background test.
 *
 * InkStroke takes samples as they come and keeps the line so far. Smart smoothing draws the line with a spring
 * pulled by the pen, loose while the pen is slow, which leaves a hand's wobble out, and tight while it is fast,
 * where it follows the pen as it is (Elasticurves, and Google's ink-stroke-modeler): pressure has a filter of
 * its own, since pressure noise, not position, is what makes a slow line's width wobble. tail() is the rest of
 * the line up to the pen as if it lifted now, and finish() adds exactly that, so what is drawn while the pen
 * is down is what stays: nothing jumps on release.
 *
 * ink_fit() then makes the line a few Bezier points: corners found where the pen slowed and turned, a fit of
 * position and width together between them (a stroke's width runs straight from point to point, so a point
 * goes wherever the width turns too), and points that turn out not to be needed taken away.
 *
 * Sizes that are about the hand (smoothing, tolerance) are in screen pixels, the same at any zoom. */

#include <vector>

#include "core/math.hh"

namespace oanim {

/* Off: the samples as they are. Smart: see above. Pulled String: the line stays put until the pen is
 * further than the string's length away, then is pulled along behind it (exact corners, a learned feel). */
enum class InkStabilizer { Off, Smart, PulledString };

struct InkSettings {
  InkStabilizer stabilizer = InkStabilizer::Smart;
  /* Smart: how much, 0-1. */
  float smoothing = 0.5f;
  /* Pulled String: its length in screen pixels; Catch Up ends the line at the pen rather than a string's
   * length short of it. */
  float string_length = 20.0f;
  bool catch_up = true;
  /* How closely the Bezier points follow the line, 0-1: from 2.5 pixels off and fewest points to 0.25
   * pixels off. */
  float fidelity = 0.5f;
  /* Pressure sets width. Its filter's strength 0-1 (0: as sent), and how pressure maps to size: a Bezier
   * from (0, 0) to (1, 1) through these two handles (Harmony's Pen Pressure Feel). */
  bool use_pressure = true;
  float pressure_smoothing = 0.5f;
  float2 pressure_curve_a = {1.0f / 3.0f, 1.0f / 3.0f};
  float2 pressure_curve_b = {2.0f / 3.0f, 2.0f / 3.0f};
  /* The thinnest the line gets from pressure, speed and tilt, as a fraction of its width. */
  float min_size = 0.15f;
  /* How much pen speed thins the line, and how much a pen held upright does (leaning it widens the line
   * as the side of a pencil), 0-1. */
  float speed_width = 0.0f;
  float tilt_width = 0.0f;
  /* Over how long the line narrows to a point from each end, in document pixels (as the width field). */
  float taper_start = 0.0f;
  float taper_end = 0.0f;
};

struct InkSample {
  /* Layer space. */
  float2 pos;
  float pressure = 1.0f;
  /* Degrees, across and up. */
  float2 tilt;
  /* Seconds. */
  double time = 0.0;
};

/* The sizes a stroke is drawn at: layer units per screen pixel; the line's half width, and a document
 * pixel, in layer units. */
struct InkView {
  float pixel = 1.0f;
  float half_width = 0.0f;
  float document_pixel = 1.0f;
};

/* A place on the smoothed line. */
struct InkPoint {
  /* Layer space. */
  float2 pos;
  /* As MeshPoint::radius: a fraction of the line's width. */
  float radius = 1.0f;
  /* Screen pixels a second. */
  float speed = 0.0f;
};

class InkStroke {
 public:
  void begin(const InkSettings &settings, const InkView &view);
  /* Takes the next sample. */
  void add(const InkSample &sample);
  bool empty() const { return recent_.empty(); }
  /* The last sample's place, in layer space. */
  float2 last_sample() const;
  /* The line so far: it only grows until finish(). */
  const std::vector<InkPoint> &points() const { return points_; }
  /* What follows points() to `pen`, the same way the line would end there. */
  std::vector<InkPoint> tail(float2 pen) const;
  /* The pen lifted: ends the line. */
  void finish();

  const InkSettings &settings() const { return settings_; }
  const InkView &view() const { return view_; }

 private:
  struct Raw {
    /* Screen pixels. */
    float2 pos;
    double time = 0.0;
    float pressure = 1.0f;
    float2 tilt;
  };
  /* A point on a critically damped spring pulled toward what it is given. */
  struct Spring {
    float2 pos, vel;
    void step(float2 target, float dt, float omega);
  };

  float radius(float pressure, float2 tilt, float speed) const;
  void emit(float2 pos, float pressure, float2 tilt, float speed);
  float filter_pressure(const Raw &raw);
  float recent_speed() const;
  void add_smart(const Raw &raw, float pressure);
  void add_pulled_string(const Raw &raw, float pressure);
  float2 lift_place() const;

  InkSettings settings_;
  InkView view_;
  std::vector<InkPoint> points_;
  /* The last moments' samples, oldest first. */
  std::vector<Raw> recent_;
  float pressure_ = 1.0f, pressure_rate_ = 0.0f;
  Spring spring_;
  /* The last place emitted (screen pixels). */
  float2 emitted_;
};

/* How far (screen pixels) the Bezier points may be off for a `fidelity`. */
float ink_tolerance(float fidelity);
/* Narrows the line's ends by the settings' tapers. */
void ink_taper(std::vector<InkPoint> &points, const InkSettings &settings, const InkView &view);

/* A Bezier point of a finished line: handles as offsets from it, in line unless it is a corner. */
struct InkKnot {
  float2 pos;
  float radius = 1.0f;
  float2 handle_in, handle_out;
  bool corner = false;
};

/* The line as Bezier points, tapered. Closed: `points` goes round to its start, and the last point joins
 * the first (the last knot's handle out leads to the first knot). A line too short to have any length (a
 * tap) is a dot: two points half a pixel apart, the width of its widest. */
std::vector<InkKnot> ink_fit(std::vector<InkPoint> points, const InkSettings &settings, const InkView &view,
                             bool closed);

}  // namespace oanim
