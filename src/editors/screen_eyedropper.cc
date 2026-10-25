#include "editors/screen_eyedropper.hh"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#  define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace oanim {

namespace {

struct State {
  bool active = false;
  HWND cover = nullptr;
  HWND swatch = nullptr;
  HCURSOR cursor = nullptr;
  bool own_cursor = false;
  /* The window that was in front: it gets the keyboard back after. */
  HWND before = nullptr;
  /* Set by the cover's window procedure. */
  bool done = false;
  bool picked = false;
  POINT click{};
  float rgb[3] = {0, 0, 0};
};
State g;

const wchar_t *const COVER_CLASS = L"OpenAnimateEyedropperCover";
const wchar_t *const SWATCH_CLASS = L"OpenAnimateEyedropperSwatch";

float dpi_scale()
{
  const UINT dpi = GetDpiForSystem();
  return dpi > 0 ? float(dpi) / 96.0f : 1.0f;
}

/* The colour on screen at `p` (as composed, our windows left out). */
bool screen_color(POINT p, float r_rgb[3])
{
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return false;
  }
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = 1;
  bi.bmiHeader.biHeight = 1;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void *bits = nullptr;
  HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bmp && bits) {
    HGDIOBJ old = SelectObject(mem, bmp);
    ok = BitBlt(mem, 0, 0, 1, 1, screen, p.x, p.y, SRCCOPY) != 0;
    SelectObject(mem, old);
    if (ok) {
      const uint8_t *px = static_cast<const uint8_t *>(bits);
      r_rgb[0] = float(px[2]) / 255.0f;
      r_rgb[1] = float(px[1]) / 255.0f;
      r_rgb[2] = float(px[0]) / 255.0f;
    }
    DeleteObject(bmp);
  }
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok;
}

/* Premultiplied BGRA pixels, painted shape by shape with soft edges. */
struct Canvas {
  int w, h;
  std::vector<uint32_t> px;
  Canvas(int width, int height) : w(width), h(height), px(size_t(width) * height, 0) {}

  /* Lays colour (r, g, b, 0-1) over a pixel with `cover` (0-1) of it. */
  void blend(int x, int y, float r, float g_, float b, float cover)
  {
    if (x < 0 || y < 0 || x >= w || y >= h || cover <= 0.0f) {
      return;
    }
    cover = std::min(cover, 1.0f);
    uint32_t &d = px[size_t(y) * w + x];
    const float da = float((d >> 24) & 255) / 255.0f, dr = float((d >> 16) & 255) / 255.0f,
                dg = float((d >> 8) & 255) / 255.0f, db = float(d & 255) / 255.0f;
    const float a = cover + da * (1.0f - cover);
    const float nr = r * cover + dr * (1.0f - cover), ng = g_ * cover + dg * (1.0f - cover),
                nb = b * cover + db * (1.0f - cover);
    const auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    d = (byte(a) << 24) | (byte(nr) << 16) | (byte(ng) << 8) | byte(nb);
  }
  /* A disc of radius `radius` about (cx, cy). */
  void disc(float cx, float cy, float radius, float r, float g_, float b)
  {
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const float d = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
        blend(x, y, r, g_, b, radius + 0.5f - d);
      }
    }
  }
  /* A line from a to b, `width` wide with round ends. */
  void line(float ax, float ay, float bx, float by, float width, float r, float g_, float b)
  {
    const float ex = bx - ax, ey = by - ay, l2 = std::max(ex * ex + ey * ey, 1e-6f);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const float px_ = float(x) + 0.5f, py_ = float(y) + 0.5f;
        const float t = std::clamp(((px_ - ax) * ex + (py_ - ay) * ey) / l2, 0.0f, 1.0f);
        const float d = std::hypot(px_ - (ax + ex * t), py_ - (ay + ey * t));
        blend(x, y, r, g_, b, width * 0.5f + 0.5f - d);
      }
    }
  }
};

/* The pipette, its tip at the bottom left (the hotspot), as Substance Painter's: a dark outline,
 * then the white glass, collar and bulb. */
void paint_pipette(Canvas &c, float s)
{
  const float tip_x = 2.0f * s, tip_y = float(c.h) - 2.0f * s;
  const auto at = [&](float u, float v) { return std::pair<float, float>(tip_x + u * s, tip_y - v * s); };
  const auto [t0x, t0y] = at(0.5f, 0.5f);
  const auto [t1x, t1y] = at(11.0f, 11.0f);
  const auto [b0x, b0y] = at(10.0f, 10.0f);
  const auto [b1x, b1y] = at(17.0f, 17.0f);
  const auto [c0x, c0y] = at(13.5f, 19.5f);
  const auto [c1x, c1y] = at(19.5f, 13.5f);
  const auto [ux, uy] = at(21.0f, 21.0f);
  for (const bool outline : {true, false}) {
    const float grow = outline ? 2.0f * s : 0.0f;
    const float v = outline ? 0.08f : 0.96f;
    c.line(t0x, t0y, t1x, t1y, 2.2f * s + grow, v, v, v);
    c.line(b0x, b0y, b1x, b1y, 4.6f * s + grow, v, v, v);
    c.line(c0x, c0y, c1x, c1y, 2.4f * s + grow, v, v, v);
    c.disc(ux, uy, 3.6f * s + grow * 0.5f, v, v, v);
  }
}

HCURSOR make_cursor()
{
  const float s = dpi_scale();
  const int size = int(std::round(32.0f * s));
  Canvas c(size, size);
  paint_pipette(c, s);
  BITMAPV5HEADER bi{};
  bi.bV5Size = sizeof(bi);
  bi.bV5Width = size;
  bi.bV5Height = -size;
  bi.bV5Planes = 1;
  bi.bV5BitCount = 32;
  bi.bV5Compression = BI_BITFIELDS;
  bi.bV5RedMask = 0x00ff0000;
  bi.bV5GreenMask = 0x0000ff00;
  bi.bV5BlueMask = 0x000000ff;
  bi.bV5AlphaMask = 0xff000000;
  HDC dc = GetDC(nullptr);
  void *bits = nullptr;
  HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO *>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, dc);
  if (!color || !bits) {
    return LoadCursor(nullptr, IDC_CROSS);
  }
  std::copy(c.px.begin(), c.px.end(), static_cast<uint32_t *>(bits));
  /* The mask is unused where the colour bitmap has alpha, but must be defined: all zero. */
  const std::vector<uint8_t> zeros(size_t((size + 15) / 16 * 2) * size, 0);
  HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
  ICONINFO info{};
  info.fIcon = FALSE;
  info.xHotspot = DWORD(2.0f * s);
  info.yHotspot = DWORD(size - 2.0f * s);
  info.hbmMask = mask;
  info.hbmColor = color;
  HCURSOR cursor = CreateIconIndirect(&info);
  DeleteObject(color);
  DeleteObject(mask);
  g.own_cursor = cursor != nullptr;
  return cursor ? cursor : LoadCursor(nullptr, IDC_CROSS);
}

void finish(bool picked, POINT at)
{
  if (g.done) {
    return;
  }
  g.done = true;
  g.picked = picked && screen_color(at, g.rgb);
}

LRESULT CALLBACK cover_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg) {
    case WM_LBUTTONDOWN: {
      POINT p;
      GetCursorPos(&p);
      finish(true, p);
      return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
      finish(false, {});
      return 0;
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) {
        finish(false, {});
      }
      return 0;
    case WM_ACTIVATE:
      /* Switched away (Alt Tab): nothing picked. */
      if (LOWORD(wp) == WA_INACTIVE && g.active) {
        finish(false, {});
      }
      break;
    case WM_SETCURSOR:
      SetCursor(g.cursor);
      return TRUE;
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool register_classes()
{
  static bool done = false;
  if (done) {
    return true;
  }
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = cover_proc;
  wc.hInstance = inst;
  wc.lpszClassName = COVER_CLASS;
  if (!RegisterClassExW(&wc)) {
    return false;
  }
  WNDCLASSEXW sw{};
  sw.cbSize = sizeof(sw);
  sw.lpfnWndProc = DefWindowProcW;
  sw.hInstance = inst;
  sw.lpszClassName = SWATCH_CLASS;
  if (!RegisterClassExW(&sw)) {
    return false;
  }
  done = true;
  return true;
}

/* The swatch: a disc of the colour, ringed white and dark, at the bottom right of the cursor. */
void update_swatch(POINT cursor, const float rgb[3])
{
  const float s = dpi_scale();
  const int size = int(std::round(72.0f * s));
  Canvas c(size, size);
  const float centre = float(size) * 0.5f, radius = float(size) * 0.5f - 2.0f * s;
  c.disc(centre, centre, radius + 1.0f * s, 0.08f, 0.08f, 0.08f);
  c.disc(centre, centre, radius - 0.5f * s, 0.92f, 0.92f, 0.92f);
  c.disc(centre, centre, radius - 2.5f * s, rgb[0], rgb[1], rgb[2]);

  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = size;
  bi.bmiHeader.biHeight = -size;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  void *bits = nullptr;
  HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp && bits) {
    std::copy(c.px.begin(), c.px.end(), static_cast<uint32_t *>(bits));
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT to{cursor.x + LONG(14.0f * s), cursor.y + LONG(14.0f * s)};
    SIZE wh{size, size};
    POINT from{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g.swatch, screen, &to, &wh, mem, &from, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bmp);
  }
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

}  // namespace

bool screen_eyedropper_begin()
{
  screen_eyedropper_end();
  if (!register_classes()) {
    return false;
  }
  g = State();
  g.before = GetForegroundWindow();
  g.cursor = make_cursor();
  HINSTANCE inst = GetModuleHandleW(nullptr);
  const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  /* All but invisible (alpha 1 of 255), yet it takes the clicks; the swatch is click-through. */
  g.cover = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, COVER_CLASS, L"", WS_POPUP, x, y, w, h,
                            nullptr, nullptr, inst, nullptr);
  g.swatch = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             SWATCH_CLASS, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
  if (!g.cover || !g.swatch) {
    screen_eyedropper_end();
    return false;
  }
  SetLayeredWindowAttributes(g.cover, 0, 1, LWA_ALPHA);
  SetWindowDisplayAffinity(g.cover, WDA_EXCLUDEFROMCAPTURE);
  SetWindowDisplayAffinity(g.swatch, WDA_EXCLUDEFROMCAPTURE);
  g.active = true;
  ShowWindow(g.cover, SW_SHOW);
  SetForegroundWindow(g.cover);
  SetFocus(g.cover);
  POINT p;
  GetCursorPos(&p);
  float rgb[3] = {0, 0, 0};
  screen_color(p, rgb);
  update_swatch(p, rgb);
  ShowWindow(g.swatch, SW_SHOWNOACTIVATE);
  SetCursor(g.cursor);
  return true;
}

bool screen_eyedropper_active()
{
  return g.active;
}

bool screen_eyedropper_update(bool *r_picked, float r_rgb[3])
{
  if (!g.active) {
    return false;
  }
  if (g.done) {
    *r_picked = g.picked;
    std::copy(g.rgb, g.rgb + 3, r_rgb);
    screen_eyedropper_end();
    return true;
  }
  POINT p;
  if (GetCursorPos(&p)) {
    float rgb[3];
    if (screen_color(p, rgb)) {
      update_swatch(p, rgb);
    }
  }
  return false;
}

void screen_eyedropper_end()
{
  if (g.swatch) {
    DestroyWindow(g.swatch);
  }
  if (g.cover) {
    DestroyWindow(g.cover);
  }
  if (g.cursor && g.own_cursor) {
    DestroyCursor(g.cursor);
  }
  if (g.active && g.before && IsWindow(g.before)) {
    SetForegroundWindow(g.before);
  }
  g.swatch = g.cover = nullptr;
  g.cursor = nullptr;
  g.own_cursor = false;
  g.active = false;
}

}  // namespace oanim
