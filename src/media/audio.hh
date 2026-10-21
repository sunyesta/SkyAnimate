#pragma once

/* Sound playback, as Blender's: the Media layers' sound (media_mix) fed into one SDL audio stream
 * while the animation plays forward, and a frame's worth of it each time the frame changes while
 * scrubbing (the Playback popover's Scrubbing). Sync to Audio moves the frame to the sound being
 * heard. Nothing plays without a window (tests and scripts run silent). */

namespace oanim {

struct Document;

bool audio_open();
void audio_close();

/* Starts the sound at `seconds` into the document (frame f is at f / fps), replacing what was queued. */
void audio_play_from(const Document &doc, double seconds);
/* Keeps a little sound queued ahead of what is heard; call every frame while playing. */
void audio_feed(const Document &doc);
void audio_stop();
bool audio_playing();
/* Where the sound being heard is, in document seconds. */
double audio_clock();
/* One frame's worth of sound at `seconds`, faded in and out (scrubbing). */
void audio_scrub(const Document &doc, double seconds);

}  // namespace oanim
