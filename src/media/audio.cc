#include "media/audio.hh"

#include <SDL3/SDL.h>

#include <algorithm>
#include <vector>

#include "data/document.hh"
#include "data/media_layer.hh"

namespace oanim {

static SDL_AudioStream *g_stream = nullptr;
static bool g_playing = false;
/* The document time of the next sample to be queued. */
static double g_cursor = 0.0;
/* Queued ahead of what is heard while playing: enough to ride out a slow frame. */
static constexpr double AHEAD = 0.12;
static constexpr int CHUNK = 1024;

bool audio_open()
{
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    return false;
  }
  const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, MEDIA_AUDIO_RATE};
  g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (!g_stream) {
    return false;
  }
  SDL_ResumeAudioStreamDevice(g_stream);
  return true;
}

void audio_close()
{
  if (g_stream) {
    SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
  }
  g_playing = false;
}

static double queued_seconds()
{
  return g_stream ? double(SDL_GetAudioStreamQueued(g_stream)) / (sizeof(float) * 2.0 * MEDIA_AUDIO_RATE) : 0.0;
}

static void queue(const Document &doc, double seconds, int frames)
{
  std::vector<float> mix(size_t(frames) * 2, 0.0f);
  media_mix(doc, seconds, size_t(frames), mix.data());
  for (float &v : mix) {
    v = std::clamp(v, -1.0f, 1.0f);
  }
  SDL_PutAudioStreamData(g_stream, mix.data(), int(mix.size() * sizeof(float)));
}

void audio_play_from(const Document &doc, double seconds)
{
  if (!g_stream) {
    return;
  }
  SDL_ClearAudioStream(g_stream);
  g_cursor = seconds;
  g_playing = true;
  audio_feed(doc);
}

void audio_feed(const Document &doc)
{
  if (!g_stream || !g_playing) {
    return;
  }
  while (queued_seconds() < AHEAD) {
    queue(doc, g_cursor, CHUNK);
    g_cursor += double(CHUNK) / MEDIA_AUDIO_RATE;
  }
}

void audio_stop()
{
  if (g_stream) {
    SDL_ClearAudioStream(g_stream);
  }
  g_playing = false;
}

bool audio_playing()
{
  return g_playing;
}

double audio_clock()
{
  return g_cursor - queued_seconds();
}

void audio_scrub(const Document &doc, double seconds)
{
  if (!g_stream) {
    return;
  }
  g_playing = false;
  SDL_ClearAudioStream(g_stream);
  /* A frame long (at least 50 ms, to be heard), with 3 ms fades so it doesn't click. */
  const int frames = int(std::max(1.0 / std::max(double(doc.fps), 1.0), 0.05) * MEDIA_AUDIO_RATE);
  std::vector<float> mix(size_t(frames) * 2, 0.0f);
  if (!media_mix(doc, seconds, size_t(frames), mix.data())) {
    return;
  }
  const int fade = std::min(int(0.003 * MEDIA_AUDIO_RATE), frames / 2);
  for (int i = 0; i < frames; i++) {
    const float g = std::min({1.0f, float(i) / float(std::max(fade, 1)), float(frames - 1 - i) / float(std::max(fade, 1))});
    mix[size_t(i) * 2] = std::clamp(mix[size_t(i) * 2] * g, -1.0f, 1.0f);
    mix[size_t(i) * 2 + 1] = std::clamp(mix[size_t(i) * 2 + 1] * g, -1.0f, 1.0f);
  }
  SDL_PutAudioStreamData(g_stream, mix.data(), int(mix.size() * sizeof(float)));
}

}  // namespace oanim
