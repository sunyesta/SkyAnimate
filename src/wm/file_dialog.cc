#include "wm/file_dialog.hh"

#include <SDL3/SDL.h>

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace sky {

namespace {

struct Request {
  FileDialogDone done;
  /* SDL needs the filter to stay alive until the dialog closes. */
  std::string filter_name;
  std::string pattern;
  SDL_DialogFileFilter filter{};
};

std::mutex g_mutex;
/* Finished dialogs: SDL may call back on another thread, so results wait here. */
std::vector<std::pair<Request *, std::string>> g_finished;

void SDLCALL dialog_callback(void *userdata, const char *const *filelist, int /*filter*/)
{
  std::string path;
  if (filelist && filelist[0]) {
    path = filelist[0];
  }
  std::lock_guard lock(g_mutex);
  g_finished.emplace_back(static_cast<Request *>(userdata), std::move(path));
}

Request *request_new(const char *filter_name, const char *pattern, FileDialogDone done)
{
  Request *request = new Request{std::move(done), filter_name, pattern};
  request->filter = {request->filter_name.c_str(), request->pattern.c_str()};
  return request;
}

}  // namespace

void file_dialog_open(const char *filter_name, const char *pattern, FileDialogDone done)
{
  Request *request = request_new(filter_name, pattern, std::move(done));
  SDL_ShowOpenFileDialog(dialog_callback, request, SDL_GL_GetCurrentWindow(), &request->filter, 1, nullptr, false);
}

void file_dialog_save(const char *filter_name, const char *pattern, const std::string &default_path,
                      FileDialogDone done)
{
  Request *request = request_new(filter_name, pattern, std::move(done));
  SDL_ShowSaveFileDialog(dialog_callback, request, SDL_GL_GetCurrentWindow(), &request->filter, 1,
                         default_path.empty() ? nullptr : default_path.c_str());
}

void file_dialog_process()
{
  std::vector<std::pair<Request *, std::string>> finished;
  {
    std::lock_guard lock(g_mutex);
    finished.swap(g_finished);
  }
  for (auto &[request, path] : finished) {
    std::unique_ptr<Request> owned(request);
    if (!path.empty() && owned->done) {
      owned->done(path);
    }
  }
}

}  // namespace sky
