#include "data/media_layer.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include "data/document.hh"

namespace oanim {

namespace fs = std::filesystem;

static fs::path path_from_utf8(const std::string &path)
{
  return fs::path(std::u8string(path.begin(), path.end()));
}

static std::string path_to_utf8(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(s.begin(), s.end());
}

const MediaClip *media_clip_at(const Layer &layer, int frame)
{
  const int f = std::max(frame, 1);
  for (const MediaClip &c : layer.media.clips) {
    if (f >= c.start && f < c.start + c.length) {
      return &c;
    }
  }
  return nullptr;
}

MediaClip *media_clip_at(Layer &layer, int frame)
{
  return const_cast<MediaClip *>(media_clip_at(static_cast<const Layer &>(layer), frame));
}

std::string media_clip_file(const MediaClip &clip)
{
  return clip.packed.empty() ? clip.filepath : media_packed_path(clip.packed);
}

MediaInfo media_clip_info(const MediaClip &clip)
{
  return media_probe(media_clip_file(clip), clip.sequence);
}

bool media_clip_missing(const MediaClip &clip)
{
  if (!clip.packed.empty()) {
    return !media_packed_has(clip.packed);
  }
  std::error_code ec;
  return !fs::is_regular_file(path_from_utf8(clip.filepath), ec);
}

/* Frames per second of a clip's file as it plays: the document's for real time (and for sequences,
 * a file per frame), the file's own frame by frame. */
static double clip_rate(const Document &doc, const MediaClip &clip, const MediaInfo &info)
{
  if (clip.timing == MediaTiming::FrameByFrame && info.kind == MediaKind::Video && info.fps > 0.0) {
    return info.fps;
  }
  return double(doc.fps);
}

static double file_frames(const Document &doc, const MediaClip &clip, const MediaInfo &info)
{
  switch (info.kind) {
    case MediaKind::Sequence:
      return double(info.frame_count);
    case MediaKind::Video:
    case MediaKind::Audio:
      return info.duration * clip_rate(doc, clip, info);
    default:
      return 0.0;
  }
}

double media_clip_file_frames(const Document &doc, const MediaClip &clip)
{
  return file_frames(doc, clip, media_clip_info(clip));
}

double media_clip_in_frames(const Document &doc, const MediaClip &clip)
{
  return clip.in * clip_rate(doc, clip, media_clip_info(clip));
}

double media_clip_seconds(const Document &doc, const MediaClip &clip, double frame)
{
  const MediaInfo info = media_clip_info(clip);
  if (info.kind == MediaKind::Image || info.kind == MediaKind::None) {
    return 0.0;
  }
  const double rate = clip_rate(doc, clip, info);
  const double total = file_frames(doc, clip, info);
  const double in = clip.in * rate;
  const double rel = frame - double(clip.start);
  double at = in + rel;
  if (at < 0.0) {
    return -1.0;
  }
  if (total > 0.0 && at >= total - 1e-6) {
    switch (clip.end) {
      case MediaEnd::Hold:
        if (info.kind != MediaKind::Sequence) {
          /* The very last picture of the file. */
          return std::max(info.duration - 1e-3, 0.0);
        }
        at = std::max(total - 1.0, 0.0);
        break;
      case MediaEnd::Loop: {
        const double span = std::max(total - in, 1.0);
        at = in + std::fmod(std::max(rel, 0.0), span);
        break;
      }
      case MediaEnd::Hide:
        return -1.0;
    }
  }
  if (info.kind == MediaKind::Sequence) {
    return std::floor(at + 1e-6) / MEDIA_SEQUENCE_RATE;
  }
  return at / rate;
}

MediaShown media_shown_at(const Document &doc, const Layer &layer, int frame)
{
  const MediaClip *clip = media_clip_at(layer, frame);
  if (!clip) {
    return {};
  }
  const double seconds = media_clip_seconds(doc, *clip, double(std::max(frame, 1)));
  if (seconds < 0.0) {
    return {};
  }
  return {clip, seconds};
}

MediaClip media_clip_new(const Document &doc, Layer &layer, const std::string &path, bool sequence, int start)
{
  MediaClip clip;
  clip.id = layer.media.next_clip_id++;
  clip.filepath = path;
  clip.sequence = sequence;
  clip.start = std::max(start, 1);
  clip.timing = sequence ? MediaTiming::FrameByFrame : MediaTiming::RealTime;
  const MediaInfo info = media_clip_info(clip);
  const double frames = file_frames(doc, clip, info);
  clip.length = frames > 0.0 ? std::max(1, int(std::ceil(frames - 1e-6))) : std::max(doc.frame_end - clip.start + 1, 1);
  return clip;
}

bool media_clip_preview(const Document &doc, const std::string &path, int start, MediaClip *r_clip, MediaInfo *r_info)
{
  if (media_kind_from_path(path) == MediaKind::None) {
    return false;
  }
  static std::map<std::string, bool> sequences;
  auto it = sequences.find(path);
  if (it == sequences.end()) {
    it = sequences.emplace(path, !media_sequence_files(path).empty()).first;
  }
  *r_info = media_probe(path, it->second);
  if (!r_info->ok()) {
    return false;
  }
  Layer scratch;
  *r_clip = media_clip_new(doc, scratch, path, it->second, start);
  return true;
}

void media_clips_tidy(const Document &doc, Layer &layer, uint64_t keep)
{
  std::vector<MediaClip> &clips = layer.media.clips;
  for (MediaClip &c : clips) {
    c.start = std::max(c.start, 1);
    c.length = std::max(c.length, 1);
  }
  const auto winner = std::find_if(clips.begin(), clips.end(), [&](const MediaClip &c) { return c.id == keep; });
  if (winner != clips.end()) {
    const MediaClip k = *winner;
    const int ks = k.start, ke = k.start + k.length;
    std::vector<MediaClip> out;
    for (const MediaClip &o : clips) {
      const int os = o.start, oe = o.start + o.length;
      if (o.id == k.id || oe <= ks || os >= ke) {
        out.push_back(o);
        continue;
      }
      /* Overwritten: what is left of it before the winner and after it. */
      if (os < ks) {
        MediaClip left = o;
        left.length = ks - os;
        out.push_back(left);
      }
      if (oe > ke) {
        MediaClip right = o;
        if (os < ks) {
          right.id = layer.media.next_clip_id++;
        }
        right.start = ke;
        right.length = oe - ke;
        right.in += double(ke - os) / clip_rate(doc, o, media_clip_info(o));
        out.push_back(right);
      }
    }
    clips = std::move(out);
  }
  std::stable_sort(clips.begin(), clips.end(), [](const MediaClip &a, const MediaClip &b) { return a.start < b.start; });
  /* Any overlap left (from a file, or two moved at once): the later one starts where the earlier ends. */
  for (size_t i = 1; i < clips.size(); i++) {
    const int end = clips[i - 1].start + clips[i - 1].length;
    if (clips[i].start < end) {
      const int cut = end - clips[i].start;
      clips[i].in += double(cut) / clip_rate(doc, clips[i], media_clip_info(clips[i]));
      clips[i].start = end;
      clips[i].length -= cut;
    }
  }
  clips.erase(std::remove_if(clips.begin(), clips.end(), [](const MediaClip &c) { return c.length < 1; }), clips.end());
}

bool media_layer_range(const Layer &layer, int *r_first, int *r_last)
{
  if (layer.media.clips.empty()) {
    return false;
  }
  *r_first = layer.media.clips.front().start;
  *r_last = layer.media.clips.back().start + layer.media.clips.back().length - 1;
  return true;
}

static int clip_end(const MediaClip &c)
{
  return c.start + c.length;
}

void media_clip_drag_bounds(const std::vector<MediaClip> &clips, uint64_t id, MediaDragKind kind, bool magnet,
                            int *r_lowest_start, int *r_highest_end)
{
  *r_lowest_start = 1;
  *r_highest_end = INT32_MAX / 2;
  const auto self = std::find_if(clips.begin(), clips.end(), [&](const MediaClip &c) { return c.id == id; });
  if (self == clips.end() || kind == MediaDragKind::Move) {
    return;
  }
  for (const MediaClip &c : clips) {
    if (c.id == id) {
      continue;
    }
    if (c.start < self->start) {
      /* Before it: without the magnet its end is a wall; with it, the clip before gives way down to a
       * frame of its own. */
      *r_lowest_start = std::max(*r_lowest_start, magnet ? c.start + 1 : clip_end(c));
    }
    else if (!magnet) {
      *r_highest_end = std::min(*r_highest_end, c.start);
    }
  }
}

bool media_clips_drag(const std::vector<MediaClip> &before, const MediaClip &dragged, MediaDragKind kind, bool magnet,
                      std::vector<MediaClip> &r_clips)
{
  r_clips = before;
  std::stable_sort(r_clips.begin(), r_clips.end(), [](const MediaClip &a, const MediaClip &b) { return a.start < b.start; });
  const auto at = std::find_if(r_clips.begin(), r_clips.end(), [&](const MediaClip &c) { return c.id == dragged.id; });
  if (at == r_clips.end()) {
    return false;
  }
  const MediaClip was = *at;
  *at = dragged;
  const int ns = dragged.start, ne = clip_end(dragged);
  if (!magnet) {
    for (const MediaClip &c : r_clips) {
      if (c.id != dragged.id && c.start < ne && clip_end(c) > ns) {
        return false;
      }
    }
    return true;
  }
  /* The others in time order, as they were. */
  std::vector<MediaClip *> others;
  for (MediaClip &c : r_clips) {
    if (c.id != dragged.id) {
      others.push_back(&c);
    }
  }
  if (kind == MediaDragKind::TrimEnd) {
    /* The clips stuck to its end (no gap, one after another) move with it... */
    const int delta = ne - clip_end(was);
    int stuck_end = clip_end(was);
    for (MediaClip *c : others) {
      if (c->start < was.start) {
        continue;
      }
      if (c->start != stuck_end) {
        break;
      }
      stuck_end = clip_end(*c);
      c->start += delta;
    }
  }
  else if (kind == MediaDragKind::TrimStart) {
    /* ...and the clip before its start stays against it (or gives way). */
    for (auto it = others.rbegin(); it != others.rend(); ++it) {
      MediaClip &c = **it;
      if (c.start >= was.start) {
        continue;
      }
      if (clip_end(c) == was.start || clip_end(c) > ns) {
        c.length = std::max(ns - c.start, 1);
      }
      break;
    }
    return true;
  }
  /* What it now runs into is pushed along, one after another. */
  int cursor = ne;
  for (MediaClip *c : others) {
    if (clip_end(*c) <= ns) {
      continue; /* Wholly before it: left alone. */
    }
    if (c->start < cursor) {
      c->start = cursor;
    }
    cursor = clip_end(*c);
  }
  std::stable_sort(r_clips.begin(), r_clips.end(), [](const MediaClip &a, const MediaClip &b) { return a.start < b.start; });
  return true;
}

void media_quad(const Document &doc, const Layer &layer, int width, int height, float2 r_corners[4])
{
  /* The camera's frame at zoom 1: 2 units tall, as wide as the document's shape (data/camera.hh). */
  const float aspect = float(std::max(doc.width, 1)) / float(std::max(doc.height, 1));
  const float picture = float(std::max(width, 1)) / float(std::max(height, 1));
  float hw = aspect, hh = 1.0f;
  switch (layer.media.fit) {
    case MediaFit::Fit:
      (picture > aspect ? hh : hw) = picture > aspect ? aspect / picture : picture;
      break;
    case MediaFit::Fill:
      (picture > aspect ? hw : hh) = picture > aspect ? picture : aspect / picture;
      break;
    case MediaFit::Stretch:
      break;
    case MediaFit::Original: {
      const float pixel = 2.0f / float(std::max(doc.height, 1));
      hw = float(width) * pixel * 0.5f;
      hh = float(height) * pixel * 0.5f;
      break;
    }
  }
  r_corners[0] = {-hw, -hh};
  r_corners[1] = {hw, -hh};
  r_corners[2] = {hw, hh};
  r_corners[3] = {-hw, hh};
}

/* ---------------------------------------------------------------------------------
 * Packed files
 */

struct PackedFile {
  std::string name;
  std::shared_ptr<const std::string> bytes;
  std::string path;
};

static std::map<std::string, PackedFile> &packed()
{
  static std::map<std::string, PackedFile> map;
  return map;
}

static std::string content_id(const std::string &bytes)
{
  uint64_t h = 1469598103934665603ull;
  for (const char c : bytes) {
    h = (h ^ uint8_t(c)) * 1099511628211ull;
  }
  char text[48];
  std::snprintf(text, sizeof(text), "%016llx%08llx", static_cast<unsigned long long>(h),
                static_cast<unsigned long long>(bytes.size() & 0xffffffffull));
  return text;
}

std::string media_pack_file(const std::string &path, std::string *r_error)
{
  std::ifstream in(path_from_utf8(path), std::ios::binary);
  if (!in) {
    *r_error = "Can't read " + path;
    return {};
  }
  std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::string id = content_id(bytes);
  media_packed_add(id, path_to_utf8(path_from_utf8(path).filename()), std::move(bytes));
  return id;
}

void media_packed_add(const std::string &id, const std::string &name, std::string bytes)
{
  if (packed().count(id)) {
    return;
  }
  packed()[id] = {name.empty() ? std::string("media") : name, std::make_shared<const std::string>(std::move(bytes)), {}};
}

bool media_packed_has(const std::string &id)
{
  return packed().count(id) > 0;
}

const std::string *media_packed_bytes(const std::string &id)
{
  auto it = packed().find(id);
  return it == packed().end() ? nullptr : it->second.bytes.get();
}

std::string media_packed_name(const std::string &id)
{
  auto it = packed().find(id);
  return it == packed().end() ? std::string() : it->second.name;
}

std::string media_packed_path(const std::string &id)
{
  auto it = packed().find(id);
  if (it == packed().end()) {
    return {};
  }
  PackedFile &p = it->second;
  if (p.path.empty()) {
    std::error_code ec;
    const fs::path folder = fs::temp_directory_path(ec) / "OpenAnimate" / "packed" / id;
    fs::create_directories(folder, ec);
    const fs::path file = folder / path_from_utf8(p.name);
    if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) != p.bytes->size()) {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out.write(p.bytes->data(), std::streamsize(p.bytes->size()));
    }
    p.path = path_to_utf8(file);
  }
  return p.path;
}

static void layers_walk(std::vector<Layer> &layers, std::vector<Layer *> &out)
{
  for (Layer &l : layers) {
    if (l.is_media()) {
      out.push_back(&l);
    }
    layers_walk(l.children, out);
  }
}

std::vector<Layer *> media_layers(Document &doc)
{
  std::vector<Layer *> out;
  layers_walk(doc.layers, out);
  return out;
}

std::vector<std::string> media_packed_used(const Document &doc)
{
  std::vector<std::string> ids;
  for (const Layer *l : media_layers(const_cast<Document &>(doc))) {
    for (const MediaClip &c : l->media.clips) {
      if (!c.packed.empty() && std::find(ids.begin(), ids.end(), c.packed) == ids.end()) {
        ids.push_back(c.packed);
      }
    }
  }
  return ids;
}

int media_missing_count(const Document &doc)
{
  int missing = 0;
  for (const Layer *l : media_layers(const_cast<Document &>(doc))) {
    for (const MediaClip &c : l->media.clips) {
      missing += media_clip_missing(c) ? 1 : 0;
    }
  }
  return missing;
}

int media_relink_folder(Document &doc, const std::string &folder)
{
  int found = 0;
  for (Layer *l : media_layers(doc)) {
    for (MediaClip &c : l->media.clips) {
      if (!media_clip_missing(c)) {
        continue;
      }
      const fs::path candidate = path_from_utf8(folder) / path_from_utf8(c.filepath).filename();
      std::error_code ec;
      if (fs::is_regular_file(candidate, ec)) {
        c.filepath = path_to_utf8(candidate);
        found++;
      }
    }
  }
  return found;
}

/* ---------------------------------------------------------------------------------
 * Sound
 */

bool media_has_sound(const Document &doc)
{
  bool any = false;
  for (const Layer *l : media_layers(const_cast<Document &>(doc))) {
    if (l->media.muted) {
      continue;
    }
    for (const MediaClip &c : l->media.clips) {
      if (!media_clip_missing(c) && media_clip_info(c).has_audio) {
        /* Asking starts its decoding, so it is ready by the time it plays. */
        media_sound(media_clip_file(c));
        any = true;
      }
    }
  }
  return any;
}

bool media_mix(const Document &doc, double doc_seconds, size_t frames, float *out)
{
  bool any = false;
  const double fps = std::max(double(doc.fps), 1.0);
  const double rate = double(MEDIA_AUDIO_RATE);
  for (const Layer *l : media_layers(const_cast<Document &>(doc))) {
    if (l->media.muted || l->media.volume <= 0.0f) {
      continue;
    }
    for (const MediaClip &c : l->media.clips) {
      const double clip_start = double(c.start) / fps, clip_end = double(c.start + c.length) / fps;
      if (clip_end <= doc_seconds || clip_start >= doc_seconds + double(frames) / rate) {
        continue;
      }
      const MediaInfo info = media_clip_info(c);
      if (!info.has_audio) {
        continue;
      }
      const MediaSoundPtr sound = media_sound(media_clip_file(c));
      if (!sound || sound->frames() == 0) {
        continue;
      }
      /* Frame by frame plays the sound at the speed the frames go (a 30 fps video at 24: slower). */
      const double speed = c.timing == MediaTiming::FrameByFrame && info.kind == MediaKind::Video && info.fps > 0.0
                               ? fps / info.fps
                               : 1.0;
      const double length = double(sound->frames()) / rate;
      const float gain = l->media.volume;
      for (size_t i = 0; i < frames; i++) {
        const double t = doc_seconds + double(i) / rate;
        if (t < clip_start || t >= clip_end) {
          continue;
        }
        double src = c.in + (t - clip_start) * speed;
        if (src >= length) {
          if (c.end != MediaEnd::Loop || length - c.in <= 0.0) {
            continue;
          }
          src = c.in + std::fmod(src - c.in, length - c.in);
        }
        const double at = src * rate;
        const size_t a = std::min(size_t(at), sound->frames() - 1), b = std::min(a + 1, sound->frames() - 1);
        const float u = float(at - double(a));
        for (int ch = 0; ch < 2; ch++) {
          const float va = sound->samples[a * 2 + ch], vb = sound->samples[b * 2 + ch];
          out[i * 2 + ch] += (va + (vb - va) * u) * gain;
        }
        any = true;
      }
    }
  }
  return any;
}

}  // namespace oanim
