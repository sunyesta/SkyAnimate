#pragma once

/* WinTab, the tablet makers' pen API, beside Windows Ink (wm/pen_input.hh). It isn't part of Windows: a tablet's
 * driver installs wintab32.dll, so it is loaded when wanted and there is nothing to link. Many artists use it with
 * "Use Windows Ink" off in their tablet's driver, where Windows Ink sends no pen at all.
 *
 * A context opened on the window posts WT_PACKET as samples queue up, and each message drains the queue, which
 * holds every sample since the last. The context gives raw tablet coordinates: the mapping to the screen is done
 * here, because drivers get high-DPI and several displays wrong when asked to. */

#include <cstdint>

#include "wm/pen_input.hh"

namespace oanim {

/* What a tablet's values are out of, from its context and device. */
struct WintabRange {
  /* The tablet's area, and the screen's it maps to (the y axes run opposite ways). */
  long in_x = 0, in_y = 0, in_width = 1, in_height = 1;
  int screen_x = 0, screen_y = 0, screen_width = 1, screen_height = 1;
  /* The most these reach. Azimuth and altitude 0: the pen has no tilt. */
  int pressure_max = 1, azimuth_max = 0, altitude_max = 0;
};

/* One packet's values. Azimuth runs clockwise from up, altitude from the tablet's plane up to the pen standing
 * straight, negative for some drivers when the pen is turned over. */
struct WintabPacket {
  long x = 0, y = 0;
  unsigned pressure = 0;
  int azimuth = 0, altitude = 0;
};

/* A packet's place on the screen (sub-pixel), and its pressure and tilt in `r_sample`. */
void wintab_convert(const WintabRange &range, const WintabPacket &packet, double *r_screen_x, double *r_screen_y,
                    PenSample *r_sample);

/* Whether a tablet's driver has installed WinTab. */
bool wintab_installed();
/* Opens a context on the window (`hwnd`). False: none to open. */
bool wintab_start(void *hwnd);
void wintab_stop();
bool wintab_started();
/* A message the window got: packets go to pen_input_wintab_sample. */
void wintab_message(unsigned message, uintptr_t wparam, intptr_t lparam);

/* A sample from WinTab (pen_input.cc). `touching`: the tip is down; `in_range`: the pen is near. */
void pen_input_wintab_sample(double screen_x, double screen_y, const PenSample &sample, bool touching, bool in_range);
/* The pen left the tablet. */
void pen_input_wintab_left();

}  // namespace oanim
