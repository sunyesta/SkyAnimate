#include "wm/pen_input.hh"

#include <SDL3/SDL.h>

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "wm/pen_wintab.hh"

namespace oanim {

static SDL_Window *g_window = nullptr;
static HWND g_hwnd = nullptr;
static bool g_seen = false;
static TabletApi g_api = TabletApi::Automatic;
/* When Windows Ink last sent a pen, and whether the last sample was WinTab's. */
static double g_ink_time = -1e9;
static bool g_from_wintab = false;
/* Automatic: WinTab's samples are left alone this long (seconds) after Windows Ink's, a driver sending both. */
static constexpr double INK_WINS_SECONDS = 1.0;
/* WinTab's place further than this (pixels) from the cursor is a driver's mapping gone wrong: several displays,
 * a part of the tablet mapped to a display. The cursor is always right. */
static constexpr double WINTAB_MISMATCH_PIXELS = 40.0;
static TabletState g_state;
static std::vector<PenSample> g_stroke;
static uint64_t g_stroke_id = 0;
static double g_began = 0.0, g_ended = 0.0;
/* The tablet's area in HIMETRIC and the screen's it maps onto, read again at each touch (the
 * displays may have changed). */
static HANDLE g_device = nullptr;
static RECT g_device_rect = {}, g_display_rect = {};
static bool g_rects = false;
/* A touch long enough for a whole drawing, and never more (nobody taking the samples). */
static constexpr size_t MAX_SAMPLES = 1 << 17;

static double seconds_per_tick()
{
  static const double s = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1.0 / double(f.QuadPart);
  }();
  return s;
}

double pen_now()
{
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return double(c.QuadPart) * seconds_per_tick();
}

/* A place on the screen in the window, as ImGui counts. */
static void screen_to_window(double x, double y, PenSample *s)
{
  POINT origin = {0, 0};
  ClientToScreen(g_hwnd, &origin);
  RECT client = {};
  GetClientRect(g_hwnd, &client);
  int w = 0, h = 0;
  SDL_GetWindowSize(g_window, &w, &h);
  /* The window's pixels to its size as SDL (and ImGui) count it. */
  const double sx = client.right > 0 ? double(w) / double(client.right) : 1.0;
  const double sy = client.bottom > 0 ? double(h) / double(client.bottom) : 1.0;
  s->x = float((x - origin.x) * sx);
  s->y = float((y - origin.y) * sy);
}

/* Where a pen sample is in the window, as ImGui counts. The pixel position is whole pixels; the
 * HIMETRIC one, mapped through the tablet's rectangles, keeps the tablet's finer steps (slow strokes
 * otherwise come out stepped), and is used when it lands on the same pixel (some drivers map it oddly). */
static void place(const POINTER_INFO &p, PenSample *s)
{
  double x = p.ptPixelLocation.x, y = p.ptPixelLocation.y;
  if (!g_rects || g_device != p.sourceDevice) {
    g_device = p.sourceDevice;
    g_rects = GetPointerDeviceRects(p.sourceDevice, &g_device_rect, &g_display_rect) &&
              g_device_rect.right > g_device_rect.left && g_device_rect.bottom > g_device_rect.top;
  }
  if (g_rects) {
    const double fx = g_display_rect.left + double(p.ptHimetricLocation.x - g_device_rect.left) *
                                                double(g_display_rect.right - g_display_rect.left) /
                                                double(g_device_rect.right - g_device_rect.left);
    const double fy = g_display_rect.top + double(p.ptHimetricLocation.y - g_device_rect.top) *
                                               double(g_display_rect.bottom - g_display_rect.top) /
                                               double(g_device_rect.bottom - g_device_rect.top);
    if (std::abs(fx - x) < 1.5 && std::abs(fy - y) < 1.5) {
      x = fx;
      y = fy;
    }
  }
  screen_to_window(x, y, s);
}

/* The next sample, whichever API it came through. */
static void take_sample(const PenSample &s, bool touching, bool in_range)
{
  g_state.in_range = in_range;
  g_state.eraser = s.eraser;
  g_state.pressure = touching ? s.pressure : 0.0f;
  g_state.tilt_x = s.tilt_x;
  g_state.tilt_y = s.tilt_y;
  if (touching && !g_state.touching) {
    g_stroke.clear();
    g_stroke_id++;
    g_began = s.time;
    g_ended = 0.0;
  }
  if (touching && g_stroke.size() < MAX_SAMPLES) {
    g_stroke.push_back(s);
  }
  if (!touching && g_state.touching) {
    g_ended = s.time;
  }
  g_state.touching = touching;
}

static void take(const POINTER_PEN_INFO &info)
{
  const POINTER_INFO &p = info.pointerInfo;
  const bool touching = (p.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
  if (touching && !g_state.touching) {
    g_rects = false;
  }
  PenSample s;
  place(p, &s);
  s.pressure = (info.penMask & PEN_MASK_PRESSURE) ? std::clamp(float(info.pressure) / 1024.0f, 0.0f, 1.0f) : 1.0f;
  s.tilt_x = (info.penMask & PEN_MASK_TILT_X) ? float(info.tiltX) : 0.0f;
  s.tilt_y = (info.penMask & PEN_MASK_TILT_Y) ? float(info.tiltY) : 0.0f;
  s.eraser = (info.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
  s.time = p.PerformanceCount ? double(p.PerformanceCount) * seconds_per_tick() : pen_now();
  take_sample(s, touching, (p.pointerFlags & POINTER_FLAG_INRANGE) != 0 && !(p.pointerFlags & POINTER_FLAG_CANCELED));
}

void pen_input_wintab_sample(double screen_x, double screen_y, const PenSample &sample, bool touching, bool in_range)
{
  if (g_api == TabletApi::Automatic && pen_now() - g_ink_time < INK_WINS_SECONDS) {
    return;
  }
  g_from_wintab = true;
  PenSample s = sample;
  POINT cursor;
  if (GetCursorPos(&cursor) && (std::abs(screen_x - cursor.x) > WINTAB_MISMATCH_PIXELS ||
                                std::abs(screen_y - cursor.y) > WINTAB_MISMATCH_PIXELS))
  {
    screen_x = cursor.x;
    screen_y = cursor.y;
  }
  screen_to_window(screen_x, screen_y, &s);
  take_sample(s, touching, in_range);
}

void pen_input_wintab_left()
{
  if (g_from_wintab) {
    PenSample s;
    s.pressure = 0.0f;
    s.time = pen_now();
    take_sample(s, false, false);
  }
}

/* Every message the window gets, before SDL handles it (which it then does as usual). A pen's pointer
 * message carries the samples coalesced into it as its history, newest first. */
static bool SDLCALL pen_hook(void * /*userdata*/, MSG *msg)
{
  const UINT m = msg->message;
  if (msg->hwnd == g_hwnd) {
    wintab_message(m, msg->wParam, msg->lParam);
  }
  if ((m != WM_POINTERDOWN && m != WM_POINTERUPDATE && m != WM_POINTERUP) || msg->hwnd != g_hwnd) {
    return true;
  }
  /* WinTab picked: the same pen comes through there. */
  if (g_api == TabletApi::WinTab && wintab_started()) {
    return true;
  }
  const UINT32 id = GET_POINTERID_WPARAM(msg->wParam);
  POINTER_INPUT_TYPE type = PT_POINTER;
  POINTER_PEN_INFO info = {};
  if (!GetPointerType(id, &type) || type != PT_PEN || !GetPointerPenInfo(id, &info)) {
    return true;
  }
  g_seen = true;
  g_ink_time = pen_now();
  g_from_wintab = false;
  UINT32 count = std::max<UINT32>(info.pointerInfo.historyCount, 1);
  std::vector<POINTER_PEN_INFO> history(count);
  if (count > 1 && GetPointerPenInfoHistory(id, &count, history.data()) && count > 0) {
    for (UINT32 i = count; i-- > 0;) {
      take(history[i]);
    }
  }
  else {
    take(info);
  }
  return true;
}

void pen_input_install(SDL_Window *window)
{
  g_window = window;
  g_hwnd = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (g_hwnd) {
    SDL_SetWindowsMessageHook(pen_hook, nullptr);
  }
}

bool pen_input_seen()
{
  return g_seen;
}

void pen_input_set_api(TabletApi api)
{
  g_api = api;
  if (api == TabletApi::WindowsInk) {
    wintab_stop();
  }
  else if (g_hwnd) {
    wintab_start(g_hwnd);
  }
}

TabletApi pen_input_api()
{
  return g_api;
}

bool pen_input_from_wintab()
{
  return g_from_wintab;
}

const TabletState &tablet_state()
{
  return g_state;
}

const std::vector<PenSample> &pen_stroke()
{
  return g_stroke;
}

uint64_t pen_stroke_id()
{
  return g_stroke_id;
}

double pen_stroke_began()
{
  return g_began;
}

double pen_stroke_ended()
{
  return g_ended;
}

void pen_simulate_down(bool eraser)
{
  g_stroke.clear();
  g_stroke_id++;
  g_began = pen_now();
  g_ended = 0.0;
  g_state.in_range = g_state.touching = true;
  g_state.eraser = eraser;
}

void pen_simulate_sample(const PenSample &sample)
{
  PenSample s = sample;
  s.eraser = g_state.eraser;
  if (s.time <= 0.0) {
    s.time = pen_now();
  }
  g_state.pressure = s.pressure;
  g_state.tilt_x = s.tilt_x;
  g_state.tilt_y = s.tilt_y;
  if (g_stroke.size() < MAX_SAMPLES) {
    g_stroke.push_back(s);
  }
}

void pen_simulate_up()
{
  g_ended = pen_now();
  g_state.touching = false;
  g_state.pressure = 0.0f;
}

PenSample pen_predict(const std::vector<PenSample> &samples, double ahead, float max_pixels)
{
  if (samples.empty()) {
    return {};
  }
  PenSample last = samples.back();
  if (samples.size() < 3 || ahead <= 0.0) {
    return last;
  }
  /* The pen's speed over its last 30 ms (at least its last two steps). */
  size_t from = samples.size() - 1;
  while (from > 0 && last.time - samples[from - 1].time <= 0.03) {
    from--;
  }
  from = std::min(from, samples.size() - 3);
  const double dt = last.time - samples[from].time;
  if (dt <= 1e-4) {
    return last;
  }
  const double vx = (last.x - samples[from].x) / dt, vy = (last.y - samples[from].y) / dt;
  /* Slower than this (pixels a second), a guess shows as wobble rather than keeping up. */
  if (vx * vx + vy * vy < 150.0 * 150.0) {
    return last;
  }
  double dx = vx * ahead, dy = vy * ahead;
  const double len = std::sqrt(dx * dx + dy * dy);
  if (len > max_pixels) {
    dx *= max_pixels / len;
    dy *= max_pixels / len;
  }
  last.x += float(dx);
  last.y += float(dy);
  last.time += ahead;
  return last;
}

}  // namespace oanim
