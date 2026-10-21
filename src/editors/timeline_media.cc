#define IMGUI_DEFINE_MATH_OPERATORS
#include "editors/timeline_media.hh"

#include "imgui_internal.h"

#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "data/document.hh"
#include "data/media_layer.hh"
#include "editors/ui.hh"
#include "media/media.hh"
#include "ops/media_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

static constexpr ImU32 COL_SELECTED = IM_COL32(0xff, 0xbe, 0x33, 255);
static constexpr ImU32 COL_TEXT = IM_COL32(0xf2, 0xf2, 0xf2, 255);
static constexpr ImU32 COL_MISSING = IM_COL32(0xa0, 0x30, 0x30, 230);
/* Screen distance an edge snaps from, and how far either side of a drawn edge grabs it: twice the
 * drawn handle's width, as img.ly's editor found handles need. */
static constexpr float SNAP_PX = 8.0f;
static constexpr float EDGE_PX = 7.0f;

static ImU32 clip_color(MediaKind kind)
{
  switch (kind) {
    case MediaKind::Video:
      return IM_COL32(0x32, 0x55, 0x86, 255);
    case MediaKind::Sequence:
      return IM_COL32(0x2a, 0x6e, 0x69, 255);
    case MediaKind::Audio:
      return IM_COL32(0x35, 0x6b, 0x3c, 255);
    default:
      return IM_COL32(0x5e, 0x44, 0x84, 255);
  }
}

static std::string file_name(const std::string &path)
{
  const std::u8string name = std::filesystem::path(std::u8string(path.begin(), path.end())).filename().u8string();
  return std::string(name.begin(), name.end());
}

/* Frames per second the clip's In point is counted in (as data/media_layer.cc plays it). */
static double clip_rate(const Document &doc, const MediaClip &clip, const MediaInfo &info)
{
  return clip.timing == MediaTiming::FrameByFrame && info.kind == MediaKind::Video && info.fps > 0.0 ? info.fps
                                                                                                     : double(doc.fps);
}

/* A texture per filmstrip picture, made when first drawn. */
static unsigned int thumb_texture(const MediaFrame &f)
{
  static std::map<uint64_t, unsigned int> textures;
  auto it = textures.find(f.serial);
  if (it != textures.end()) {
    return it->second;
  }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.width, f.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.rgba.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  textures[f.serial] = tex;
  return tex;
}

/* Where a clip's bar runs: its frames are cells centred on the ruler's ticks. */
static float bar_left(const TimelineMediaView &v, int start)
{
  return v.x_of(float(start) - 0.5f);
}

static float bar_right(const TimelineMediaView &v, int start, int length)
{
  return v.x_of(float(start + length) - 0.5f);
}

/* --- Dragging --------------------------------------------------------------------------------- */

enum class DragMode { Move, TrimStart, TrimEnd, Slip };

struct MediaDrag {
  bool active = false;
  uint64_t layer = 0, clip = 0;
  DragMode mode = DragMode::Move;
  float start_x = 0.0f;
  MediaClip original;
  bool moved = false;
};
static MediaDrag g_drag;

static Layer *drag_layer(Context &C)
{
  Layer *l = C.document ? layer_find(*C.document, g_drag.layer) : nullptr;
  return l && l->is_media() ? l : nullptr;
}

static MediaClip *clip_by_id(Layer &layer, uint64_t id)
{
  for (MediaClip &c : layer.media.clips) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

/* What a drag at x over a clip's bar would do: its edges trim, Alt anywhere slips, else it moves. */
static DragMode mode_at(const TimelineMediaView &v, const MediaClip &c, float x, bool alt, bool can_slip)
{
  const float a = bar_left(v, c.start), b = bar_right(v, c.start, c.length);
  if (alt && can_slip) {
    return DragMode::Slip;
  }
  /* A narrow bar keeps a middle to move it by. */
  const float edge = std::min(EDGE_PX, (b - a) * 0.3f);
  if (x < a + edge) {
    return DragMode::TrimStart;
  }
  if (x > b - edge) {
    return DragMode::TrimEnd;
  }
  return DragMode::Move;
}

static const MediaClip *clip_under(const TimelineMediaView &v, const Layer &layer, float x)
{
  for (const MediaClip &c : layer.media.clips) {
    if (x >= bar_left(v, c.start) - EDGE_PX * 0.5f && x < bar_right(v, c.start, c.length) + EDGE_PX * 0.5f) {
      return &c;
    }
  }
  return nullptr;
}

/* Frames an edge snaps to: the playhead, the frame range's ends, and every other clip's edges. */
static std::vector<int> snap_targets(const Document &doc, uint64_t moving)
{
  std::vector<int> t = {doc.frame_current, doc.frame_start, doc.frame_end + 1};
  for (const Layer *l : media_layers(const_cast<Document &>(doc))) {
    for (const MediaClip &c : l->media.clips) {
      if (c.id != moving || l->id != g_drag.layer) {
        t.push_back(c.start);
        t.push_back(c.start + c.length);
      }
    }
  }
  return t;
}

/* The nearest target to `frame` within SNAP_PX, as how far to move (0: none). */
static int snap_offset(const TimelineMediaView &v, const std::vector<int> &targets, int frame, float *r_best_px)
{
  int best = 0;
  for (const int t : targets) {
    const float px = std::fabs(float(t - frame)) * v.px_per_frame();
    if (px <= SNAP_PX && px < *r_best_px) {
      *r_best_px = px;
      best = t - frame;
    }
  }
  return best;
}

bool timeline_media_row_press(Context &/*C*/, const TimelineMediaView &v, Layer &layer, float top, float h)
{
  const ImGuiIO &io = ImGui::GetIO();
  if (io.MousePos.y < top || io.MousePos.y >= top + h) {
    return false;
  }
  const MediaClip *under = clip_under(v, layer, io.MousePos.x);
  if (!io.KeyShift) {
    for (MediaClip &c : layer.media.clips) {
      c.selected = false;
    }
  }
  if (!under) {
    return true;
  }
  MediaClip &clip = *clip_by_id(layer, under->id);
  clip.selected = io.KeyShift ? !clip.selected : true;
  const MediaInfo info = media_clip_info(clip);
  g_drag = {};
  g_drag.active = true;
  g_drag.layer = layer.id;
  g_drag.clip = clip.id;
  g_drag.mode = mode_at(v, clip, io.MousePos.x, io.KeyAlt, info.kind != MediaKind::Image);
  g_drag.start_x = io.MousePos.x;
  g_drag.original = clip;
  return true;
}

/* The drag's readout at the mouse. */
static void drag_readout(const Document &doc, const MediaClip &c)
{
  const char *what = g_drag.mode == DragMode::Move ? "Move" : g_drag.mode == DragMode::Slip ? "Slip" : "Trim";
  char text[160];
  std::snprintf(text, sizeof(text), "%s   Start %d   Length %d   End %d   In %.2f s", what, c.start, c.length,
                c.start + c.length - 1, c.in);
  (void)doc;
  ImGui::SetTooltip("%s", text);
}

bool timeline_media_drag_update(Context &C, const TimelineMediaView &v)
{
  if (!g_drag.active) {
    return false;
  }
  Document &doc = *C.document;
  Layer *layer = drag_layer(C);
  MediaClip *clip = layer ? clip_by_id(*layer, g_drag.clip) : nullptr;
  if (!clip) {
    g_drag = {};
    return false;
  }
  const ImGuiIO &io = ImGui::GetIO();
  const MediaClip &o = g_drag.original;
  /* Esc or a right-click: put it back. */
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    *clip = o;
    g_drag = {};
    return false;
  }
  const float dx = io.MousePos.x - g_drag.start_x;
  if (!g_drag.moved && std::fabs(dx) < 3.0f) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      g_drag = {};
      return false;
    }
    return true;
  }
  g_drag.moved = true;
  const MediaInfo info = media_clip_info(o);
  const double rate = clip_rate(doc, o, info);
  int d = int(std::lround(dx / v.px_per_frame()));
  /* Ctrl: no snapping, for a precise place next to something. */
  const bool snap = !io.KeyCtrl;
  const std::vector<int> targets = snap ? snap_targets(doc, o.id) : std::vector<int>();
  const int end = o.start + o.length;
  float best = SNAP_PX + 1.0f;
  *clip = o;
  switch (g_drag.mode) {
    case DragMode::Move: {
      if (snap) {
        const int a = snap_offset(v, targets, o.start + d, &best);
        const int b = snap_offset(v, targets, end + d, &best);
        d += b != 0 ? b : a;
      }
      clip->start = std::max(o.start + d, 1);
      break;
    }
    case DragMode::TrimStart: {
      int s = o.start + d;
      if (snap) {
        s += snap_offset(v, targets, s, &best);
      }
      /* Not before the file's start (its In can't go below 0), and at least a frame long. */
      const int earliest = info.kind == MediaKind::Image ? 1 : o.start - int(std::floor(o.in * rate + 1e-6));
      s = std::clamp(s, std::max(earliest, 1), end - 1);
      clip->start = s;
      clip->length = end - s;
      if (info.kind != MediaKind::Image) {
        clip->in = std::max(o.in + double(s - o.start) / rate, 0.0);
      }
      break;
    }
    case DragMode::TrimEnd: {
      int e = end + d;
      if (snap) {
        e += snap_offset(v, targets, e, &best);
      }
      clip->length = std::max(e, o.start + 1) - o.start;
      break;
    }
    case DragMode::Slip: {
      /* Dragging right brings earlier parts of the file into the bar. */
      const double file_seconds = info.kind == MediaKind::Sequence ? double(info.frame_count) / doc.fps : info.duration;
      clip->in = std::clamp(o.in - double(d) / rate, 0.0, std::max(file_seconds - 1.0 / rate, 0.0));
      break;
    }
  }
  drag_readout(doc, *clip);
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    /* Dropped: what it now covers is overwritten, and the step goes in the undo history. */
    media_clips_tidy(doc, *layer, clip->id);
    C.undo->push(g_drag.mode == DragMode::Move   ? "Move Clip"
                 : g_drag.mode == DragMode::Slip ? "Slip Clip"
                                                 : "Trim Clip",
                 doc);
    g_drag = {};
    return false;
  }
  return true;
}

/* --- Drawing ---------------------------------------------------------------------------------- */

static void hatch(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col)
{
  dl->PushClipRect(a, b, true);
  const float step = 6.0f, hgt = b.y - a.y;
  for (float x = a.x - hgt; x < b.x; x += step) {
    dl->AddLine(ImVec2(x, b.y), ImVec2(x + hgt, a.y), col, 1.0f);
  }
  dl->PopClipRect();
}

void timeline_media_row_draw(Context &C, ImDrawList *dl, const TimelineMediaView &v, const Layer &layer, float top,
                             float h)
{
  const Document &doc = *C.document;
  const float y0 = top + 2.0f, y1 = top + h - 2.0f;
  const float font = ImGui::GetFontSize();
  for (const MediaClip &c : layer.media.clips) {
    const float xa = bar_left(v, c.start), xb = bar_right(v, c.start, c.length);
    if (xb < v.x0 || xa > v.x1) {
      continue;
    }
    const MediaInfo info = media_clip_info(c);
    const bool missing = media_clip_missing(c);
    const std::string file = media_clip_file(c);
    const ImVec2 a(xa, y0), b(std::max(xb, xa + 2.0f), y1);
    dl->AddRectFilled(a, b, missing ? COL_MISSING : clip_color(info.kind), 3.0f);
    dl->PushClipRect(ImVec2(std::max(a.x, v.x0), a.y), ImVec2(std::min(b.x, v.x1), b.y), true);
    if (!missing && info.kind != MediaKind::Audio) {
      /* The filmstrip: a picture every so often, each the one its place in the bar shows. */
      if (MediaThumbsPtr strip = media_thumbnails(file, c.sequence); strip && !strip->frames.empty()) {
        const MediaFrame &first = *strip->frames.front();
        const float th = y1 - y0 - 2.0f;
        const float tw = std::max(th * float(first.width) / float(std::max(first.height, 1)), 4.0f);
        const float from = std::max(a.x, v.x0 - tw), to = std::min(b.x, v.x1);
        for (float x = from - std::fmod(from - a.x, tw); x < to; x += tw) {
          const double seconds = media_clip_seconds(doc, c, double(v.frame_of(x + tw * 0.5f)));
          if (seconds < 0.0) {
            continue;
          }
          const size_t k = std::min(strip->frames.size() - 1,
                                    size_t(std::max(0.0, std::floor(seconds / strip->interval + 0.5))));
          const MediaFrame &f = *strip->frames[k];
          dl->AddImage(ImTextureRef(ImTextureID(intptr_t(thumb_texture(f)))), ImVec2(x, y0 + 1.0f),
                       ImVec2(x + tw, y1 - 1.0f), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 200));
        }
      }
    }
    if (!missing && info.has_audio) {
      /* The sound's loudness, along the bottom half (all of it, for sound alone). */
      if (MediaSoundPtr sound = media_sound(file); sound && !sound->peaks.empty()) {
        const float mid = info.kind == MediaKind::Audio ? (y0 + y1) * 0.5f : y1 - (y1 - y0) * 0.22f;
        const float amp = info.kind == MediaKind::Audio ? (y1 - y0) * 0.45f : (y1 - y0) * 0.2f;
        const double speed = c.timing == MediaTiming::FrameByFrame && info.kind == MediaKind::Video && info.fps > 0.0
                                 ? double(doc.fps) / info.fps
                                 : 1.0;
        for (float x = std::max(a.x, v.x0); x < std::min(b.x, v.x1); x += 1.0f) {
          double t = c.in + (double(v.frame_of(x)) - double(c.start)) / double(doc.fps) * speed;
          const double length = double(sound->peaks.size()) * MEDIA_PEAK_SECONDS;
          if (t >= length && c.end == MediaEnd::Loop && length > c.in) {
            t = c.in + std::fmod(t - c.in, length - c.in);
          }
          if (t < 0.0 || t >= length) {
            continue;
          }
          const float p = sound->peaks[std::min(sound->peaks.size() - 1, size_t(t / MEDIA_PEAK_SECONDS))];
          dl->AddLine(ImVec2(x, mid - p * amp), ImVec2(x, mid + p * amp), IM_COL32(0xb8, 0xf0, 0xb8, 190));
        }
      }
    }
    /* Past the file's end: hatched, saying what shows there. */
    const double file_frames = media_clip_file_frames(doc, c);
    if (file_frames > 0.0) {
      const double in_frames = media_clip_in_frames(doc, c);
      const float xe = v.x_of(float(double(c.start) + file_frames - in_frames) - 0.5f);
      if (xe < b.x) {
        const ImVec2 ha(std::max(xe, a.x), a.y);
        dl->AddRectFilled(ha, b, IM_COL32(0, 0, 0, c.end == MediaEnd::Hide ? 120 : 60));
        hatch(dl, ha, b, IM_COL32(255, 255, 255, 50));
        const char *label = c.end == MediaEnd::Hold ? "Hold" : c.end == MediaEnd::Loop ? "Loop" : "Hidden";
        if (b.x - ha.x > ImGui::CalcTextSize(label).x + 8.0f) {
          dl->AddText(ImVec2(ha.x + 4.0f, (y0 + y1 - font) * 0.5f), IM_COL32(0xe0, 0xe0, 0xe0, 200), label);
        }
      }
    }
    if (missing) {
      hatch(dl, a, b, IM_COL32(0, 0, 0, 70));
    }
    /* Its name, on a dark backing so it reads over the pictures. */
    const std::string name = (missing ? "Missing: " : "") + file_name(c.filepath);
    const ImVec2 ts = ImGui::CalcTextSize(name.c_str());
    const float tx = std::max(a.x, v.x0) + 4.0f;
    if (b.x - tx > 12.0f) {
      dl->AddRectFilled(ImVec2(tx - 2.0f, (y0 + y1 - ts.y) * 0.5f - 1.0f),
                        ImVec2(std::min(tx + ts.x + 2.0f, b.x - 2.0f), (y0 + y1 + ts.y) * 0.5f + 1.0f),
                        IM_COL32(0, 0, 0, 120), 2.0f);
      dl->AddText(ImVec2(tx, (y0 + y1 - ts.y) * 0.5f), COL_TEXT, name.c_str());
    }
    dl->PopClipRect();
    /* The grab handles: a light strip at each end, brighter when picked. */
    const ImU32 edge = c.selected ? COL_SELECTED : IM_COL32(255, 255, 255, 90);
    dl->AddRectFilled(a, ImVec2(std::min(a.x + 3.0f, b.x), b.y), edge, 3.0f, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(std::max(b.x - 3.0f, a.x), a.y), b, edge, 3.0f, ImDrawFlags_RoundCornersRight);
    dl->AddRect(a, b, c.selected ? COL_SELECTED : IM_COL32(0, 0, 0, 160), 3.0f, 0, c.selected ? 2.0f : 1.0f);
  }
  /* While trimming or slipping: the whole file, faded, so what is left out shows. */
  if (g_drag.active && g_drag.moved && g_drag.layer == layer.id && g_drag.mode != DragMode::Move) {
    for (const MediaClip &c : layer.media.clips) {
      if (c.id != g_drag.clip) {
        continue;
      }
      const double file_frames = media_clip_file_frames(doc, c);
      if (file_frames <= 0.0) {
        break;
      }
      const double first = double(c.start) - media_clip_in_frames(doc, c);
      const float fa = v.x_of(float(first) - 0.5f), fb = v.x_of(float(first + file_frames) - 0.5f);
      dl->AddRectFilled(ImVec2(fa, y0), ImVec2(fb, y1), IM_COL32(255, 255, 255, 22), 3.0f);
      dl->AddRect(ImVec2(fa, y0), ImVec2(fb, y1), IM_COL32(255, 255, 255, 70), 3.0f);
    }
  }
}

void timeline_media_row_hover(Context &C, const TimelineMediaView &v, const Layer &layer, float top, float h)
{
  const ImGuiIO &io = ImGui::GetIO();
  if (g_drag.active || io.MousePos.y < top || io.MousePos.y >= top + h) {
    return;
  }
  const MediaClip *c = clip_under(v, layer, io.MousePos.x);
  if (!c) {
    return;
  }
  const MediaInfo info = media_clip_info(*c);
  const DragMode mode = mode_at(v, *c, io.MousePos.x, io.KeyAlt, info.kind != MediaKind::Image);
  ImGui::SetMouseCursor(mode == DragMode::Move ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    const char *what = mode == DragMode::Move        ? "Drag to move it"
                       : mode == DragMode::Slip      ? "Drag to slip it: the bar stays, the part of the file it shows moves"
                       : mode == DragMode::TrimStart ? "Drag to trim its start"
                                                     : "Drag to trim its end";
    ImGui::SetTooltip("%s\n%s\nFrames %d to %d, %.2f s into the file\n\n%s (edges trim, Alt-drag slips, Ctrl: no "
                      "snapping; right-click for more)",
                      file_name(c->filepath).c_str(),
                      media_clip_missing(*c) ? "Missing: right-click > Locate..." : media_info_text(info).c_str(),
                      c->start, c->start + c->length - 1, c->in, what);
  }
  (void)C;
}

/* --- The clip menu ---------------------------------------------------------------------------- */

static struct {
  uint64_t layer = 0, clip = 0;
  int frame = 1;
} g_menu;

void timeline_media_menu_open(Context &C, const TimelineMediaView &v, const Layer &layer)
{
  const ImGuiIO &io = ImGui::GetIO();
  const MediaClip *c = clip_under(v, layer, io.MousePos.x);
  g_menu = {layer.id, c ? c->id : 0, std::max(1, int(std::lround(v.frame_of(io.MousePos.x))))};
  (void)C;
  ImGui::OpenPopup("##media_clip");
}

void timeline_media_menu(Context &C)
{
  if (!ui_menu_begin("##media_clip")) {
    return;
  }
  Layer *layer = C.document ? layer_find(*C.document, g_menu.layer) : nullptr;
  MediaClip *clip = layer && layer->is_media() ? clip_by_id(*layer, g_menu.clip) : nullptr;
  const std::string lid = std::to_string(g_menu.layer), cid = std::to_string(g_menu.clip);
  const std::string frame = std::to_string(g_menu.frame);
  if (layer && clip) {
    ImGui::TextDisabled("%s", file_name(clip->filepath).c_str());
    ImGui::Separator();
    if (media_clip_missing(*clip) && ui_menu_item("Locate...")) {
      operator_call(C, "media.locate", {{"layer", lid}, {"clip", cid}});
    }
    if (ui_menu_item("Replace...")) {
      operator_call(C, "media.replace", {{"layer", lid}, {"clip", cid}});
    }
  }
  if (layer && ui_menu_item(("Replace from Frame " + frame + "...").c_str())) {
    operator_call(C, "media.replace_from_frame", {{"layer", lid}, {"frame", frame}});
  }
  if (layer && clip) {
    if (ui_menu_item(("Split at Frame " + frame).c_str(), nullptr, false,
                     g_menu.frame > clip->start && g_menu.frame < clip->start + clip->length))
    {
      operator_call(C, "media.clip_split", {{"layer", lid}, {"frame", frame}});
    }
    if (ui_menu_item("Delete Clip")) {
      operator_call(C, "media.clip_delete", {{"layer", lid}, {"clip", cid}});
    }
    ImGui::Separator();
    if (ui_submenu_begin("After End")) {
      static const char *const ids[] = {"HOLD", "LOOP", "HIDE"};
      static const char *const names[] = {"Hold Last Frame", "Loop", "Hide"};
      for (int i = 0; i < 3; i++) {
        if (ui_menu_item(names[i], nullptr, int(clip->end) == i)) {
          operator_call(C, "media.clip_set", {{"layer", lid}, {"clip", cid}, {"end", ids[i]}});
        }
      }
      ui_submenu_end();
    }
    if (media_clip_info(*clip).kind == MediaKind::Video && ui_submenu_begin("Timing")) {
      if (ui_menu_item("Real Time", nullptr, clip->timing == MediaTiming::RealTime)) {
        operator_call(C, "media.clip_set", {{"layer", lid}, {"clip", cid}, {"timing", "REAL_TIME"}});
      }
      if (ui_menu_item("Frame by Frame", nullptr, clip->timing == MediaTiming::FrameByFrame)) {
        operator_call(C, "media.clip_set", {{"layer", lid}, {"clip", cid}, {"timing", "FRAME_BY_FRAME"}});
      }
      ui_submenu_end();
    }
  }
  ui_menu_end();
}

}  // namespace oanim
