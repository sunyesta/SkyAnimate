#pragma once

/* Media files for Media layers: still images, numbered image sequences, video and sound, read with
 * Windows' own decoders (WIC for images, Media Foundation for video and sound), so what plays in
 * Windows' Photos and Media Player opens here.
 *
 * A source is a file (or, for a sequence, its first file) and is opened once, whoever shows it.
 * Video decodes on a worker thread per source, as Blender's movie clip cache does: a request seeks to
 * the keyframe before the frame and decodes forward to it, then keeps going a little ahead, so
 * playback finds its frames waiting. Until the asked-for frame is ready the nearest decoded one shows,
 * never a blank. Sound decodes whole, in the background, to 48 kHz stereo. Pictures keep their full
 * size up to MEDIA_MAX_SIZE on their longer side (larger ones are shrunk to just fit, averaging the
 * pixels each one covers), so a 5000 pixel render shows sharp. */

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oanim {

inline constexpr int MEDIA_MAX_SIZE = 8192;
/* The size a picture of w x h decodes at: as it is, or shrunk to fit MEDIA_MAX_SIZE. */
void media_fit_size(int w, int h, int *r_w, int *r_h);
/* Sound is mixed at this rate, in stereo. */
inline constexpr int MEDIA_AUDIO_RATE = 48000;

enum class MediaKind { None, Image, Sequence, Video, Audio };

struct MediaInfo {
  MediaKind kind = MediaKind::None;
  /* The picture's size as decoded (after MEDIA_MAX_SIZE), and as the file has it. */
  int width = 0, height = 0;
  int source_width = 0, source_height = 0;
  /* Seconds; 0 for a still image. A sequence lasts its frame count at `fps`. */
  double duration = 0.0;
  /* Video: its (average) frame rate. Sequences: 0, they play one file per document frame unless timed. */
  double fps = 0.0;
  /* Video (from its duration and rate) and sequences (their files). */
  int frame_count = 0;
  bool has_audio = false;
  /* Why it can't be read; empty when it can. */
  std::string error;
  bool ok() const { return kind != MediaKind::None; }
};

/* A decoded picture: RGBA, 8 bits, premultiplied, top row first. */
struct MediaFrame {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  /* Where in the source it shows, and for how long (seconds). */
  double time = 0.0;
  double duration = 0.0;
  /* Changes with every frame decoded, so a texture showing it knows to update. */
  uint64_t serial = 0;
};
using MediaFramePtr = std::shared_ptr<const MediaFrame>;

/* A source's sound, decoded: interleaved stereo floats at MEDIA_AUDIO_RATE, and its loudness for
 * the timeline's waveform: the peak of each MEDIA_PEAK_SECONDS. */
inline constexpr double MEDIA_PEAK_SECONDS = 0.01;
struct MediaSound {
  std::vector<float> samples;
  std::vector<float> peaks;
  size_t frames() const { return samples.size() / 2; }
};
using MediaSoundPtr = std::shared_ptr<const MediaSound>;

/* COM and Media Foundation, once at startup (and closed at exit). */
void media_init();
void media_shutdown();

/* The kind a file's extension says (None: not one Media layers read). */
MediaKind media_kind_from_path(const std::string &path);
/* For a file like "walk_0001.png": its numbered siblings, in order (itself first when it is the
 * lowest), when there are at least two; empty otherwise. */
std::vector<std::string> media_sequence_files(const std::string &path);

/* A source by its file, `sequence` for a numbered image sequence starting there. Probing reads only
 * the file's header (fast); it is remembered. */
MediaInfo media_probe(const std::string &path, bool sequence = false);

/* The picture `seconds` into a source (a sequence: its file number `seconds` * the rate it plays at,
 * see media_sequence_time). The exact frame when it is ready (*r_exact true), else the nearest one
 * decoded so far, and the exact one is asked for. `wait`: decode it first (renders, scripts). Null
 * when nothing is decoded yet, or the file can't be read. */
MediaFramePtr media_frame(const std::string &path, bool sequence, double seconds, bool wait, bool *r_exact = nullptr);
/* A sequence has no rate of its own: its file `index` is at index / MEDIA_SEQUENCE_RATE seconds. */
inline constexpr double MEDIA_SEQUENCE_RATE = 1000.0;

/* The source's sound (null while decoding, or when it has none). `wait`: decode it first. */
MediaSoundPtr media_sound(const std::string &path, bool wait = false);

/* A filmstrip for the timeline's clip bars: small pictures (MEDIA_THUMB_HEIGHT tall) every
 * `interval` seconds of the file (a sequence: every `interval` * MEDIA_SEQUENCE_RATE images), made in
 * the background by a reader of their own, so they never hold up playback. Null until made. */
inline constexpr int MEDIA_THUMB_HEIGHT = 48;
struct MediaThumbs {
  double interval = 1.0;
  std::vector<MediaFramePtr> frames;
};
using MediaThumbsPtr = std::shared_ptr<const MediaThumbs>;
MediaThumbsPtr media_thumbnails(const std::string &path, bool sequence);

/* Forgets what was read from a file (Reload, or a file put back where it was), so it is read again. */
void media_reload(const std::string &path);
/* Counts decodes finishing anywhere, for whatever shows them to know to redraw. */
uint64_t media_generation();

}  // namespace oanim
