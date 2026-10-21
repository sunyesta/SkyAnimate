#include "wm/file_drop.hh"

#include <SDL3/SDL.h>

#include <windows.h>

#include <oleidl.h>
#include <shellapi.h>

#include <deque>
#include <memory>

namespace oanim {

static FileDrag g_drag;

const FileDrag &file_drag()
{
  return g_drag;
}

void file_drag_set(const FileDrag &drag)
{
  g_drag = drag;
}

static std::string utf8(const wchar_t *w)
{
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(size_t(std::max(n - 1, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

/* The files in a drag (Explorer gives them as CF_HDROP). */
static std::vector<std::string> files_of(IDataObject *data)
{
  std::vector<std::string> files;
  FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium = {};
  if (!data || FAILED(data->GetData(&format, &medium))) {
    return files;
  }
  if (HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; i++) {
      std::wstring name(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
      DragQueryFileW(drop, i, name.data(), UINT(name.size()));
      files.push_back(utf8(name.c_str()));
    }
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);
  return files;
}

class DropTarget final : public IDropTarget {
 public:
  explicit DropTarget(HWND hwnd, SDL_WindowID window) : hwnd_(hwnd), window_(window) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *ppv = static_cast<IDropTarget *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(++refs_); }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const long left = --refs_;
    if (left == 0) {
      delete this;
    }
    return ULONG(left);
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL pt, DWORD *effect) override
  {
    g_drag.files = files_of(data);
    g_drag.active = !g_drag.files.empty();
    place(pt);
    *effect = g_drag.active ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD *effect) override
  {
    place(pt);
    *effect = g_drag.active ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override
  {
    g_drag = {};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL pt, DWORD *effect) override
  {
    place(pt);
    const std::vector<std::string> files = files_of(data);
    /* As SDL's own drops come: one event per file, at the position dropped. SDL keeps the pointer
     * until the event is handled. */
    static std::deque<std::string> held;
    for (const std::string &file : files) {
      held.push_back(file);
      if (held.size() > 64) {
        held.pop_front();
      }
      SDL_Event e{};
      e.type = SDL_EVENT_DROP_FILE;
      e.drop.windowID = window_;
      e.drop.x = g_drag.x;
      e.drop.y = g_drag.y;
      e.drop.data = held.back().c_str();
      SDL_PushEvent(&e);
    }
    g_drag = {};
    *effect = files.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
    return S_OK;
  }

 private:
  void place(POINTL pt)
  {
    POINT p = {pt.x, pt.y};
    ScreenToClient(hwnd_, &p);
    g_drag.x = float(p.x);
    g_drag.y = float(p.y);
  }

  HWND hwnd_;
  SDL_WindowID window_;
  long refs_ = 1;
};

void file_drop_install(SDL_Window *window)
{
  HWND hwnd = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (!hwnd) {
    return;
  }
  /* SDL's goes (it stays SDL's to free); ours takes its place. Without OLE (it failed to start) the
   * window keeps SDL's older drops, which still work, unpreviewed. */
  RevokeDragDrop(hwnd);
  DropTarget *target = new DropTarget(hwnd, SDL_GetWindowID(window));
  if (FAILED(RegisterDragDrop(hwnd, target))) {
    DragAcceptFiles(hwnd, TRUE);
  }
  target->Release();
}

}  // namespace oanim
