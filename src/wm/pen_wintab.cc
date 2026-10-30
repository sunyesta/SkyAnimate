#include "wm/pen_wintab.hh"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace oanim {

/* What the WinTab specification lays down (no SDK is needed: wintab32.dll comes with the driver). */
namespace {

using HCTX = void *;
using FIX32 = DWORD;
constexpr UINT WT_DEFBASE = 0x7FF0, WT_PACKET = WT_DEFBASE, WT_PROXIMITY = WT_DEFBASE + 5;
constexpr UINT WTI_DEFSYSCTX = 4, WTI_DEVICES = 100, DVC_NPRESSURE = 15, DVC_ORIENTATION = 17;
constexpr UINT CXO_MESSAGES = 0x0004;
constexpr DWORD PK_STATUS = 0x0002, PK_TIME = 0x0004, PK_CURSOR = 0x0020, PK_BUTTONS = 0x0040, PK_X = 0x0080,
                PK_Y = 0x0100, PK_NORMAL_PRESSURE = 0x0400, PK_ORIENTATION = 0x1000;
/* Packet status: out of the context, and the pen turned over to its eraser end. */
constexpr UINT TPS_PROXIMITY = 0x0001, TPS_INVERT = 0x0010;

struct LogContext {
  char lcName[40];
  UINT lcOptions, lcStatus, lcLocks, lcMsgBase, lcDevice, lcPktRate;
  DWORD lcPktData, lcPktMode, lcMoveMask, lcBtnDnMask, lcBtnUpMask;
  LONG lcInOrgX, lcInOrgY, lcInOrgZ, lcInExtX, lcInExtY, lcInExtZ;
  LONG lcOutOrgX, lcOutOrgY, lcOutOrgZ, lcOutExtX, lcOutExtY, lcOutExtZ;
  FIX32 lcSensX, lcSensY, lcSensZ;
  BOOL lcSysMode;
  int lcSysOrgX, lcSysOrgY, lcSysExtX, lcSysExtY;
  FIX32 lcSysSensX, lcSysSensY;
};

struct Axis {
  LONG axMin, axMax;
  UINT axUnits;
  FIX32 axResolution;
};

/* The fields asked for, in the order the specification gives them. */
constexpr DWORD PACKET_DATA = PK_STATUS | PK_TIME | PK_CURSOR | PK_BUTTONS | PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION;
struct Packet {
  UINT status;
  DWORD time;
  UINT cursor;
  DWORD buttons;
  LONG x, y;
  UINT pressure;
  int azimuth, altitude, twist;
};

using WTInfoFn = UINT(WINAPI *)(UINT, UINT, LPVOID);
using WTOpenFn = HCTX(WINAPI *)(HWND, LogContext *, BOOL);
using WTCloseFn = BOOL(WINAPI *)(HCTX);
using WTPacketsGetFn = int(WINAPI *)(HCTX, int, LPVOID);
using WTQueueSizeSetFn = BOOL(WINAPI *)(HCTX, int);
using WTEnableFn = BOOL(WINAPI *)(HCTX, BOOL);
using WTOverlapFn = BOOL(WINAPI *)(HCTX, BOOL);

}  // namespace

/* As many samples as a message can have waiting: a slow frame's worth at a tablet's fastest. */
static constexpr int QUEUE = 256;

static HMODULE g_library = nullptr;
static HCTX g_context = nullptr;
static WintabRange g_range;
static WTInfoFn g_info = nullptr;
static WTOpenFn g_open = nullptr;
static WTCloseFn g_close = nullptr;
static WTPacketsGetFn g_packets_get = nullptr;
static WTQueueSizeSetFn g_queue_size_set = nullptr;
static WTEnableFn g_enable = nullptr;
static WTOverlapFn g_overlap = nullptr;

void wintab_convert(const WintabRange &range, const WintabPacket &packet, double *r_screen_x, double *r_screen_y,
                    PenSample *r_sample)
{
  const double u = double(packet.x - range.in_x) / double(std::max(range.in_width, 1L));
  /* The tablet counts up from its bottom edge, the screen down from its top. */
  const double v = 1.0 - double(packet.y - range.in_y) / double(std::max(range.in_height, 1L));
  *r_screen_x = range.screen_x + u * range.screen_width;
  *r_screen_y = range.screen_y + v * range.screen_height;
  r_sample->pressure = std::clamp(float(packet.pressure) / float(std::max(range.pressure_max, 1)), 0.0f, 1.0f);
  r_sample->tilt_x = r_sample->tilt_y = 0.0f;
  if (range.azimuth_max > 0 && range.altitude_max > 0) {
    /* The pen as a line up from the tablet: tilt is how far that leans each way. */
    const double up = std::abs(double(packet.altitude)) / range.altitude_max * 1.5707963267949;
    const double turn = double(packet.azimuth) / range.azimuth_max * 6.28318530717959;
    const double height = std::sin(up), lean = std::cos(up);
    r_sample->tilt_x = float(std::atan2(lean * std::sin(turn), height) * 57.2957795130823);
    /* Azimuth 0 leans up the tablet, away from the user. */
    r_sample->tilt_y = float(std::atan2(-lean * std::cos(turn), height) * 57.2957795130823);
  }
}

static HMODULE library_load()
{
  /* Not the working directory: where Windows keeps things, and beside the app. */
  return LoadLibraryExW(L"wintab32.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

bool wintab_installed()
{
  if (g_library) {
    return true;
  }
  const HMODULE library = library_load();
  if (library) {
    FreeLibrary(library);
  }
  return library != nullptr;
}

template<typename T> static bool find_function(T *r_function, const char *name)
{
  *r_function = reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(g_library, name)));
  return *r_function != nullptr;
}

static void release()
{
  if (g_library) {
    FreeLibrary(g_library);
  }
  g_library = nullptr;
  g_context = nullptr;
}

bool wintab_start(void *hwnd)
{
  if (g_context) {
    return true;
  }
  g_library = library_load();
  if (!g_library) {
    return false;
  }
  if (!find_function(&g_info, "WTInfoA") || !find_function(&g_open, "WTOpenA") || !find_function(&g_close, "WTClose") ||
      !find_function(&g_packets_get, "WTPacketsGet") || !find_function(&g_queue_size_set, "WTQueueSizeSet") ||
      !find_function(&g_enable, "WTEnable") || !find_function(&g_overlap, "WTOverlap"))
  {
    release();
    return false;
  }
  LogContext context = {};
  if (!g_info(WTI_DEFSYSCTX, 0, &context)) {
    release();
    return false;
  }
  g_range.in_x = context.lcInOrgX;
  g_range.in_y = context.lcInOrgY;
  g_range.in_width = std::abs(context.lcInExtX);
  g_range.in_height = std::abs(context.lcInExtY);
  g_range.screen_x = context.lcSysOrgX;
  g_range.screen_y = context.lcSysOrgY;
  g_range.screen_width = context.lcSysExtX;
  g_range.screen_height = std::abs(context.lcSysExtY);
  Axis pressure = {};
  g_info(WTI_DEVICES, DVC_NPRESSURE, &pressure);
  g_range.pressure_max = std::max<int>(pressure.axMax, 1);
  /* A tablet without tilt reports no resolution for these. */
  Axis orientation[3] = {};
  g_range.azimuth_max = g_range.altitude_max = 0;
  if (g_info(WTI_DEVICES, DVC_ORIENTATION, orientation) && orientation[0].axResolution && orientation[1].axResolution) {
    g_range.azimuth_max = orientation[0].axMax;
    g_range.altitude_max = orientation[1].axMax;
  }
  context.lcOptions |= CXO_MESSAGES;
  context.lcMsgBase = WT_DEFBASE;
  context.lcPktData = PACKET_DATA;
  context.lcPktMode = 0;
  context.lcMoveMask = PACKET_DATA;
  /* The tablet's own coordinates, unscaled: the finest there are. */
  context.lcOutOrgX = context.lcInOrgX;
  context.lcOutOrgY = context.lcInOrgY;
  context.lcOutExtX = context.lcInExtX;
  context.lcOutExtY = context.lcInExtY;
  g_context = g_open(static_cast<HWND>(hwnd), &context, TRUE);
  if (!g_context) {
    release();
    return false;
  }
  /* Drivers differ in the largest queue they allow, and a refused size leaves none. */
  for (int size = QUEUE; size >= 8 && !g_queue_size_set(g_context, size); size /= 2) {
  }
  g_overlap(g_context, TRUE);
  return true;
}

void wintab_stop()
{
  if (g_context) {
    g_close(g_context);
  }
  release();
}

bool wintab_started()
{
  return g_context != nullptr;
}

void wintab_message(unsigned message, uintptr_t wparam, intptr_t lparam)
{
  if (!g_context) {
    return;
  }
  if (message == WM_ACTIVATE) {
    /* Pens go to the app in front, as the mouse does. */
    const BOOL active = LOWORD(wparam) != WA_INACTIVE;
    g_enable(g_context, active);
    g_overlap(g_context, active);
  }
  else if (message == WT_PROXIMITY && LOWORD(lparam) == 0) {
    pen_input_wintab_left();
  }
  else if (message == WT_PACKET) {
    Packet packets[QUEUE];
    const int count = g_packets_get(g_context, QUEUE, packets);
    /* The moment now, and how long ago each packet was by the tablet's clock. */
    const double now = pen_now();
    const DWORD ticks = GetTickCount();
    for (int i = 0; i < count; i++) {
      const Packet &p = packets[i];
      PenSample sample;
      double x = 0.0, y = 0.0;
      wintab_convert(g_range, {p.x, p.y, p.pressure, p.azimuth, p.altitude}, &x, &y, &sample);
      sample.eraser = (p.status & TPS_INVERT) != 0 || p.altitude < 0;
      sample.time = now - double(ticks - p.time) * 0.001;
      pen_input_wintab_sample(x, y, sample, p.pressure > 0, (p.status & TPS_PROXIMITY) == 0);
    }
  }
}

}  // namespace oanim
