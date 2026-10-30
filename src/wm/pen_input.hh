#pragma once

/* Pen tablets through Windows Ink, every sample the pen sends. SDL passes on one pen position per event
 * and the window gets one mouse position a frame, so a quick stroke loses its detail; Windows coalesces
 * the pen's samples (a tablet sends 133-240 a second) into its pointer messages and keeps the rest as
 * the message's history. A Windows message hook (SDL_SetWindowsMessageHook) reads that history, oldest
 * first, with each sample's pressure, tilt, time and whether the eraser end is down, at the sub-pixel
 * place the tablet gives (its HIMETRIC position mapped onto the screen). Nothing is taken from SDL: it
 * still makes the pen's mouse events and pressure.
 *
 * The samples of one touch of the pen make a stroke. A tool takes the stroke's samples as they come in
 * (pen_stroke and pen_stroke_id), from the first, so a press that ImGui hands a tool a frame late still
 * starts where the pen touched down. */

#include <cstdint>
#include <vector>

struct SDL_Window;

namespace oanim {

struct PenSample {
  /* Where, in the window as ImGui counts (sub-pixel). */
  float x = 0.0f, y = 0.0f;
  /* 0-1. */
  float pressure = 1.0f;
  /* Degrees, -90 to 90: leaning right (x) and toward the user (y). */
  float tilt_x = 0.0f, tilt_y = 0.0f;
  /* The pen's eraser end (or its eraser button). */
  bool eraser = false;
  /* Seconds, on the pen's own clock. */
  double time = 0.0;
};

/* The pen as last heard from: in range of the tablet, touching it, which end, its pressure and tilt. */
struct TabletState {
  bool in_range = false;
  bool touching = false;
  bool eraser = false;
  float pressure = 1.0f;
  float tilt_x = 0.0f, tilt_y = 0.0f;
};

/* Hooks the window's messages (after SDL made the window). */
void pen_input_install(SDL_Window *window);
/* Whether any pen sample has come through the hook (so the tablet works with Windows Ink). */
bool pen_input_seen();

const TabletState &tablet_state();
/* The samples of the pen's current touch, or its last one after it lifts, oldest first; and which touch
 * that is, counting up from 1 (0: none yet). */
const std::vector<PenSample> &pen_stroke();
uint64_t pen_stroke_id();
/* When the last touch began, and ended (0 while touching), in pen_now() seconds. */
double pen_stroke_began();
double pen_stroke_ended();
/* Now, on the pen's clock. */
double pen_now();

/* A stroke from Python (oanim.wm.mouse_simulate(pen=...)): starts a touch, adds samples to it, lifts it. */
void pen_simulate_down(bool eraser);
void pen_simulate_sample(const PenSample &sample);
void pen_simulate_up();

/* Where the pen will be `ahead` seconds after its last sample, going on as it has over the last few
 * samples: for drawing the line a moment ahead of the pen, so it keeps up with the hand. Nothing (the last
 * sample itself) while the pen moves slowly, where a guess would wobble, and never further than
 * `max_pixels`. */
PenSample pen_predict(const std::vector<PenSample> &samples, double ahead, float max_pixels);

}  // namespace oanim
