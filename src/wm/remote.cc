/* Winsock first, without the min/max (and near/far) baggage of windows.h leaking further. */
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "wm/remote.hh"

#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_internal.h"

#include "core/log.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "ops/file_ops.hh"
#include "python/py_runtime.hh"
#include "wm/context.hh"
#include "wm/event_system.hh"
#include "wm/keymap.hh"

namespace oanim {

using json = nlohmann::json;

namespace {

/* A request from a connection, answered by the main loop. */
struct Request {
  json message;
  std::promise<std::string> reply;
};

struct Server {
  std::atomic<bool> running{false};
  SOCKET listener = INVALID_SOCKET;
  int port = 0;
  /* Set before listening starts, read by the connections. */
  std::string token;
  /* remote.json, written for the bridge ("" in tests). */
  std::filesystem::path published;
  std::thread accept_thread;
  /* Connections being served, each by its own thread (detached; counted to wait for them). */
  std::atomic<int> serving{0};
  std::mutex mutex;
  std::vector<SOCKET> clients;
  std::deque<std::shared_ptr<Request>> queue;
  /* Main thread only: screenshots waiting for the frame to be drawn. */
  std::vector<std::shared_ptr<Request>> screenshots;
};
Server g_server;

std::string answer(const json &id, const json &result)
{
  return json{{"id", id}, {"result", result}}.dump();
}

std::string failure(const json &id, const std::string &message)
{
  return json{{"id", id}, {"error", message}}.dump();
}

bool send_all(SOCKET s, const std::string &data)
{
  size_t sent = 0;
  while (sent < data.size()) {
    const int n = send(s, data.data() + sent, int(std::min<size_t>(data.size() - sent, 1 << 20)), 0);
    if (n <= 0) {
      return false;
    }
    sent += size_t(n);
  }
  return true;
}

/* Takes the connection out of the list and closes it, unless remote_stop already has. */
void drop_client(SOCKET s)
{
  std::lock_guard lock(g_server.mutex);
  auto &clients = g_server.clients;
  if (const auto it = std::find(clients.begin(), clients.end(), s); it != clients.end()) {
    clients.erase(it);
    closesocket(s);
  }
}

/* One connection: a request per line, each answered (by the main loop) before the next. One
 * that isn't a request (a browser's HTTP request line) or lacks the token ends it. */
void serve(SOCKET s)
{
  std::string buffer;
  char chunk[4096];
  bool trusted = false, hang_up = false;
  while (g_server.running && !hang_up) {
    const int n = recv(s, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      break;
    }
    buffer.append(chunk, size_t(n));
    for (size_t nl = buffer.find('\n'); nl != std::string::npos; nl = buffer.find('\n')) {
      const std::string line = buffer.substr(0, nl);
      buffer.erase(0, nl + 1);
      if (line.find_first_not_of(" \r\t") == std::string::npos) {
        continue;
      }
      std::string reply;
      json message = json::parse(line, nullptr, false);
      const bool is_request = !message.is_discarded() && message.is_object();
      if (!trusted && (!is_request || message.value("token", "") != g_server.token)) {
        /* Not ours: say nothing more and hang up. */
        if (is_request) {
          send_all(s, failure(message.value("id", json()), "Wrong or missing token") + "\n");
        }
        hang_up = true;
        break;
      }
      trusted = true;
      if (!is_request) {
        reply = failure(nullptr, "Not a JSON object");
      }
      else if (message.value("token", "") != g_server.token) {
        reply = failure(message.value("id", json()), "Wrong or missing token");
      }
      else {
        auto request = std::make_shared<Request>();
        request->message = std::move(message);
        std::future<std::string> future = request->reply.get_future();
        const json id = request->message.value("id", json());
        {
          std::lock_guard lock(g_server.mutex);
          g_server.queue.push_back(request);
        }
        reply = future.wait_for(std::chrono::seconds(30)) == std::future_status::ready
                    ? future.get()
                    : failure(id, "Open Animate didn't answer in time (is it busy, or its window minimized?)");
      }
      if (!send_all(s, reply + "\n")) {
        hang_up = true;
        break;
      }
    }
  }
  drop_client(s);
  g_server.serving--;
}

void accept_loop()
{
  while (g_server.running) {
    const SOCKET client = accept(g_server.listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      break;
    }
    std::lock_guard lock(g_server.mutex);
    if (!g_server.running) {
      closesocket(client);
      break;
    }
    g_server.clients.push_back(client);
    g_server.serving++;
    std::thread(serve, client).detach();
  }
}

std::string upper(std::string text)
{
  for (char &c : text) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  return text;
}

json state_of(Context &C)
{
  Document &doc = *C.document;
  json s;
  s["file"] = C.wm->filepath;
  s["unsaved_changes"] = document_is_dirty(C);
  s["frame"] = doc.frame_current;
  s["frame_range"] = {doc.frame_start, doc.frame_end};
  s["fps"] = doc.fps;
  s["playing"] = C.wm->playing;
  s["auto_key"] = doc.auto_key;
  s["transforming"] = C.wm->modal_running;
  s["mouse_over"] = C.area_type;
  json layers = json::array(), selected = json::array();
  for (const Layer *l : document_layers_flat(doc)) {
    layers.push_back(l->name);
    if (l->selected || l->id == doc.active_layer) {
      selected.push_back(l->name);
    }
  }
  s["layers"] = layers;
  s["selected_layers"] = selected;
  if (const Layer *active = layer_find(doc, doc.active_layer)) {
    json a = {{"name", active->name}, {"type", layer_type_name(active->type)}};
    if (active->is_bone_layer()) {
      const Bone *bone = bone_find(*active, active->active_bone);
      a["active_bone"] = bone ? bone->name : "";
      json bones = json::array();
      for (const Bone &b : active->bones) {
        if (b.selected || b.select_head || b.select_tail) {
          bones.push_back(b.name);
        }
      }
      a["selected_bones"] = bones;
    }
    else {
      int points = 0;
      for (const MeshPoint &p : active->drawing.points) {
        points += p.selected ? 1 : 0;
      }
      a["selected_points"] = points;
    }
    s["active_layer"] = a;
  }
  json reports = json::array();
  const std::vector<Report> &all = oanim::reports();
  static const char *const TYPES[] = {"INFO", "OPERATOR", "WARNING", "ERROR"};
  for (size_t i = all.size() > 15 ? all.size() - 15 : 0; i < all.size(); i++) {
    reports.push_back({{"type", TYPES[int(all[i].type)]}, {"message", all[i].message}});
  }
  s["recent_reports"] = reports;
  return s;
}

/* A key as if pressed with the mouse over `area`; during G/R/S it goes to the transform, as a
 * real key would. `text` is typed (numbers into a transform, words into a field) next frame. */
json press(Context &C, const json &params)
{
  const std::string text = params.value("text", "");
  if (!text.empty() && ImGui::GetCurrentContext()) {
    ImGui::GetIO().AddInputCharactersUTF8(text.c_str());
  }
  const std::string key = upper(params.value("key", ""));
  if (key.empty()) {
    return {{"handled", !text.empty()}};
  }
  const bool ctrl = params.value("ctrl", false), shift = params.value("shift", false),
             alt = params.value("alt", false);
  if (C.wm->modal_running) {
    C.wm->modal_keys.push_back({key, ctrl, shift, alt});
    return {{"handled", true}, {"to", "transform"}};
  }
  const std::string area = params.value("area", "");
  const std::string saved_area = C.area_type;
  if (!area.empty()) {
    C.area_type = area;
  }
  const bool saved_mouse = C.wm->mouse_in_viewport;
  C.wm->mouse_in_viewport = saved_mouse || area == "Viewport";
  const bool handled = wm_handle_key_event(C, {key, ctrl, shift, alt}, params.value("typing", false));
  C.wm->mouse_in_viewport = saved_mouse;
  C.area_type = saved_area;
  return {{"handled", handled}};
}

std::string dispatch(Context &C, const json &message)
{
  const json id = message.value("id", json());
  const std::string method = message.value("method", "");
  const json params = message.contains("params") && message["params"].is_object() ? message["params"] : json::object();
  try {
    if (method == "ping") {
      return answer(id, {{"pong", true}});
    }
    if (method == "run_python") {
      const json result = json::parse(python::remote_run(params.value("code", "")), nullptr, false);
      return result.is_discarded() ? failure(id, "Python gave no result") : answer(id, result);
    }
    if (method == "get_state") {
      return answer(id, state_of(C));
    }
    if (method == "send_keys") {
      return answer(id, press(C, params));
    }
    if (method == "screenshot") {
      return failure(id, "No window to take a screenshot of (background mode)");
    }
    return failure(id, "Unknown method '" + method + "'");
  }
  catch (const std::exception &e) {
    return failure(id, e.what());
  }
}

std::string base64(const unsigned char *data, size_t size)
{
  static const char *const DIGITS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t a = data[i], b = i + 1 < size ? data[i + 1] : 0, c = i + 2 < size ? data[i + 2] : 0;
    const uint32_t v = (a << 16) | (b << 8) | c;
    out += DIGITS[(v >> 18) & 63];
    out += DIGITS[(v >> 12) & 63];
    out += i + 1 < size ? DIGITS[(v >> 6) & 63] : '=';
    out += i + 2 < size ? DIGITS[v & 63] : '=';
  }
  return out;
}

/* Where an editor is on the window, in pixels from the top left: the visible editor whose tab
 * says `name` (any case). */
bool editor_rect(const std::string &name, int width, int height, int r_rect[4], std::vector<std::string> &r_names)
{
  ImGuiContext *g = ImGui::GetCurrentContext();
  if (!g) {
    return false;
  }
  const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
  for (ImGuiWindow *window : g->Windows) {
    const std::string full = window->Name;
    /* An editor's window ("<editor>###area<id>"), or a floating one ("Brush Settings###..."). */
    size_t hash = full.find("###area");
    const bool floating = hash == std::string::npos && !(window->Flags & ImGuiWindowFlags_ChildWindow) &&
                          !(window->Flags & ImGuiWindowFlags_Tooltip) && full.find("###") != std::string::npos;
    if (floating) {
      hash = full.find("###");
    }
    if (hash == std::string::npos || !window->WasActive || window->Hidden) {
      continue;
    }
    const std::string label = full.substr(0, hash);
    if (std::find(r_names.begin(), r_names.end(), label) == r_names.end()) {
      r_names.push_back(label);
    }
    if (upper(label) != upper(name)) {
      continue;
    }
    r_rect[0] = std::clamp(int(window->Pos.x * scale.x), 0, width);
    r_rect[1] = std::clamp(int(window->Pos.y * scale.y), 0, height);
    r_rect[2] = std::clamp(int((window->Pos.x + window->Size.x) * scale.x), 0, width);
    r_rect[3] = std::clamp(int((window->Pos.y + window->Size.y) * scale.y), 0, height);
    return r_rect[2] > r_rect[0] && r_rect[3] > r_rect[1];
  }
  return false;
}

std::string screenshot(const json &message, const std::vector<unsigned char> &frame, int width, int height)
{
  const json id = message.value("id", json());
  const json params = message.contains("params") && message["params"].is_object() ? message["params"] : json::object();
  const std::string area = params.value("area", "");
  int rect[4] = {0, 0, width, height};
  std::vector<std::string> names;
  if (!area.empty() && !editor_rect(area, width, height, rect, names)) {
    std::string list;
    for (const std::string &n : names) {
      list += (list.empty() ? "" : ", ") + n;
    }
    return failure(id, "No editor '" + area + "' showing (showing: " + list + ")");
  }
  /* Scaled down by whole steps (averaging) to fit max_size, so big windows stay small to send. */
  const int w = rect[2] - rect[0], h = rect[3] - rect[1];
  const int max_size = std::max(64, params.value("max_size", 1600));
  const int step = std::max(1, (std::max(w, h) + max_size - 1) / max_size);
  const int ow = std::max(1, w / step), oh = std::max(1, h / step);
  std::vector<unsigned char> rgb(size_t(ow) * size_t(oh) * 3);
  for (int y = 0; y < oh; y++) {
    for (int x = 0; x < ow; x++) {
      int sum[3] = {0, 0, 0};
      for (int dy = 0; dy < step; dy++) {
        /* The frame's rows run bottom up. */
        const int fy = height - 1 - (rect[1] + y * step + dy);
        for (int dx = 0; dx < step; dx++) {
          const unsigned char *p = &frame[(size_t(fy) * size_t(width) + size_t(rect[0] + x * step + dx)) * 4];
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
        }
      }
      unsigned char *o = &rgb[(size_t(y) * size_t(ow) + size_t(x)) * 3];
      for (int c = 0; c < 3; c++) {
        o[c] = static_cast<unsigned char>(sum[c] / (step * step));
      }
    }
  }
  size_t size = 0;
  void *png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), ow, oh, 3, &size, MZ_DEFAULT_LEVEL, MZ_FALSE);
  if (!png) {
    return failure(id, "Couldn't make the PNG");
  }
  const std::string data = base64(static_cast<const unsigned char *>(png), size);
  mz_free(png);
  return answer(id, {{"png", data}, {"width", ow}, {"height", oh}});
}

}  // namespace

int remote_start(int port, bool publish)
{
  if (g_server.running) {
    return g_server.port;
  }
  /* 128 random bits, new each time. */
  std::random_device random;
  char token[33];
  for (int i = 0; i < 4; i++) {
    std::snprintf(token + i * 8, 9, "%08x", unsigned(random()));
  }
  g_server.token = token;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    report(ReportType::Error, "Remote control: networking isn't available");
    return 0;
  }
  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  /* This computer only. */
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<u_short>(port));
  /* No other program may take over the port while it is ours. */
  BOOL exclusive = TRUE;
  setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
  if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
      listen(listener, 4) != 0)
  {
    if (listener != INVALID_SOCKET) {
      closesocket(listener);
    }
    WSACleanup();
    report(ReportType::Error, "Remote control: port " + std::to_string(port) + " is in use");
    return 0;
  }
  int length = sizeof(address);
  getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
  g_server.listener = listener;
  g_server.port = ntohs(address.sin_port);
  g_server.running = true;
  g_server.accept_thread = std::thread(accept_loop);
  if (publish) {
    g_server.published = std::filesystem::path(reinterpret_cast<const char8_t *>(user_data_dir().c_str())) / "remote.json";
    std::ofstream out(g_server.published, std::ios::trunc);
    out << json{{"port", g_server.port}, {"token", g_server.token}}.dump() << "\n";
  }
  report(ReportType::Info, "Remote control: listening on 127.0.0.1:" + std::to_string(g_server.port), false);
  return g_server.port;
}

void remote_stop()
{
  if (!g_server.running) {
    return;
  }
  g_server.running = false;
  closesocket(g_server.listener);
  if (g_server.accept_thread.joinable()) {
    g_server.accept_thread.join();
  }
  std::deque<std::shared_ptr<Request>> waiting;
  {
    std::lock_guard lock(g_server.mutex);
    for (const SOCKET s : g_server.clients) {
      closesocket(s);
    }
    g_server.clients.clear();
    waiting.swap(g_server.queue);
  }
  for (const auto &r : waiting) {
    r->reply.set_value(failure(r->message.value("id", json()), "Remote control stopped"));
  }
  for (const auto &r : g_server.screenshots) {
    r->reply.set_value(failure(r->message.value("id", json()), "Remote control stopped"));
  }
  g_server.screenshots.clear();
  /* The connections end once their sockets are closed. */
  for (int i = 0; i < 200 && g_server.serving > 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  g_server.port = 0;
  g_server.token.clear();
  if (!g_server.published.empty()) {
    std::error_code ec;
    std::filesystem::remove(g_server.published, ec);
    g_server.published.clear();
  }
  WSACleanup();
}

std::string remote_token()
{
  return g_server.running ? g_server.token : std::string();
}

int remote_port()
{
  return g_server.running ? g_server.port : 0;
}

void remote_process(Context &C)
{
  std::deque<std::shared_ptr<Request>> todo;
  {
    std::lock_guard lock(g_server.mutex);
    todo.swap(g_server.queue);
  }
  for (const auto &r : todo) {
    if (r->message.value("method", "") == "screenshot" && !C.wm->background) {
      g_server.screenshots.push_back(r);
      continue;
    }
    r->reply.set_value(dispatch(C, r->message));
  }
}

void remote_capture(int width, int height)
{
  if (g_server.screenshots.empty() || width <= 0 || height <= 0) {
    return;
  }
  std::vector<unsigned char> frame(size_t(width) * size_t(height) * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
  for (const auto &r : g_server.screenshots) {
    r->reply.set_value(screenshot(r->message, frame, width, height));
  }
  g_server.screenshots.clear();
}

std::string remote_dispatch(Context &C, const std::string &request)
{
  const json message = json::parse(request, nullptr, false);
  if (message.is_discarded() || !message.is_object()) {
    return failure(nullptr, "Not a JSON object");
  }
  return dispatch(C, message);
}

}  // namespace oanim
