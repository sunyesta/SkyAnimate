#include "wm/clipboard_image.hh"

#include <windows.h>

#include <SDL3/SDL.h>

#include <cstring>
#include <vector>

namespace oanim {

/* The image as a PNG file's bytes. */
static bool png_bytes(const uint8_t *rgba, int width, int height, std::vector<uint8_t> &r_png)
{
  SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, const_cast<uint8_t *>(rgba),
                                               width * 4);
  SDL_IOStream *io = surface ? SDL_IOFromDynamicMem() : nullptr;
  bool ok = io && SDL_SavePNG_IO(surface, io, false);
  if (ok) {
    const Sint64 size = SDL_GetIOSize(io);
    const void *data = SDL_GetPointerProperty(SDL_GetIOProperties(io), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr);
    ok = data && size > 0;
    if (ok) {
      r_png.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
    }
  }
  if (io) {
    SDL_CloseIO(io);
  }
  SDL_DestroySurface(surface);
  return ok;
}

static HGLOBAL global_copy(const void *data, size_t size)
{
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, size);
  if (!handle) {
    return nullptr;
  }
  void *to = GlobalLock(handle);
  std::memcpy(to, data, size);
  GlobalUnlock(handle);
  return handle;
}

bool clipboard_set_image(const uint8_t *rgba, int width, int height, std::string *r_error)
{
  const auto fail = [&](const char *what) {
    if (r_error) {
      *r_error = what;
    }
    return false;
  };
  if (!rgba || width <= 0 || height <= 0) {
    return fail("there is no image");
  }
  std::vector<uint8_t> png;
  if (!png_bytes(rgba, width, height, png)) {
    return fail("it could not be made into a PNG");
  }
  /* The bitmap: rows from the bottom, BGRA, alpha straight (as most apps read CF_DIBV5's). */
  std::vector<uint8_t> dib(sizeof(BITMAPV5HEADER) + size_t(width) * height * 4);
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = width;
  header.bV5Height = height;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5SizeImage = DWORD(size_t(width) * height * 4);
  header.bV5RedMask = 0x00ff0000;
  header.bV5GreenMask = 0x0000ff00;
  header.bV5BlueMask = 0x000000ff;
  header.bV5AlphaMask = 0xff000000;
  header.bV5CSType = LCS_sRGB;
  header.bV5Intent = LCS_GM_IMAGES;
  std::memcpy(dib.data(), &header, sizeof(header));
  uint8_t *pixels = dib.data() + sizeof(header);
  for (int y = 0; y < height; y++) {
    const uint8_t *from = rgba + size_t(height - 1 - y) * width * 4;
    uint8_t *to = pixels + size_t(y) * width * 4;
    for (int x = 0; x < width; x++) {
      to[x * 4 + 0] = from[x * 4 + 2];
      to[x * 4 + 1] = from[x * 4 + 1];
      to[x * 4 + 2] = from[x * 4 + 0];
      to[x * 4 + 3] = from[x * 4 + 3];
    }
  }
  if (!OpenClipboard(nullptr)) {
    return fail("the clipboard is in use by another app");
  }
  EmptyClipboard();
  bool any = false;
  if (HGLOBAL h = global_copy(png.data(), png.size())) {
    if (SetClipboardData(RegisterClipboardFormatW(L"PNG"), h)) {
      any = true;
    }
    else {
      GlobalFree(h);
    }
  }
  if (HGLOBAL h = global_copy(dib.data(), dib.size())) {
    if (SetClipboardData(CF_DIBV5, h)) {
      any = true;
    }
    else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
  return any || fail("the clipboard didn't take it");
}

}  // namespace oanim
