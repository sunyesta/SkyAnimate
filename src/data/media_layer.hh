#pragma once

/* Media layers' clips (data/layer.hh: MediaClip, MediaSettings): which clip shows on a frame and what
 * part of its file, clips' lengths from their files, the files packed into a .oanim, and the sound all
 * the layers make together. Decoding is media/media.hh's. */

#include <cstdint>
#include <string>
#include <vector>

#include "core/math.hh"
#include "data/layer.hh"
#include "media/media.hh"

namespace oanim {

struct Document;

/* What a Media layer shows on a frame: its clip, and where in the clip's file (seconds; a sequence's
 * file number / MEDIA_SEQUENCE_RATE, as media_frame takes it). Null clip: nothing (between clips, past
 * a Hide clip's file). */
struct MediaShown {
  const MediaClip *clip = nullptr;
  double seconds = 0.0;
};
MediaShown media_shown_at(const Document &doc, const Layer &layer, int frame);
/* Where in its file `clip` is on `frame` (any frame, in the clip or not, fractions too: the
 * timeline's filmstrip), or -1 where it shows nothing (past a Hide clip's file, before its In). */
double media_clip_seconds(const Document &doc, const MediaClip &clip, double frame);
/* The clip covering `frame` (from its Start, for its Length), or null. The setup frame (0) shows
 * what frame 1 does. */
const MediaClip *media_clip_at(const Layer &layer, int frame);
MediaClip *media_clip_at(Layer &layer, int frame);

/* The file a clip decodes from: its own, or, packed, the packed copy's. */
std::string media_clip_file(const MediaClip &clip);
MediaInfo media_clip_info(const MediaClip &clip);
/* The file is gone (and not packed). */
bool media_clip_missing(const MediaClip &clip);
/* How long a clip's file runs, in document frames as the clip plays it (0: a still image, which
 * runs as long as it is given). */
double media_clip_file_frames(const Document &doc, const MediaClip &clip);
/* The clip's In point in document frames as it plays (In seconds at its speed). */
double media_clip_in_frames(const Document &doc, const MediaClip &clip);

/* A new clip for `path` starting at `start`: as long as its file (a still image: to the end of the
 * frame range), a numbered image sequence when `sequence`. Its id comes from the layer. */
MediaClip media_clip_new(const Document &doc, Layer &layer, const std::string &path, bool sequence, int start);
/* Keeps the clips in time order without overlaps: a clip moved over another trims the one it lands
 * on (the moved clip wins), as a video editor's overwrite. `keep` is the clip that wins (0: none). */
void media_clips_tidy(const Document &doc, Layer &layer, uint64_t keep = 0);
/* The first and last frame of all its clips (false for none). */
bool media_layer_range(const Layer &layer, int *r_first, int *r_last);

/* Where the picture goes in the layer's own space, from the file's size and the layer's Fit: its
 * corners bottom left, bottom right, top right, top left. */
void media_quad(const Document &doc, const Layer &layer, int width, int height, float2 r_corners[4]);

/* --- Packed files: a copy kept inside the .oanim. Kept here by content (an id from it), so undo
 * steps and copies of a document share them; the .oanim stores media/<id>/<name>. */
std::string media_pack_file(const std::string &path, std::string *r_error);
void media_packed_add(const std::string &id, const std::string &name, std::string bytes);
bool media_packed_has(const std::string &id);
const std::string *media_packed_bytes(const std::string &id);
std::string media_packed_name(const std::string &id);
/* A file holding the packed copy (written once, in the temp folder), for the decoders. */
std::string media_packed_path(const std::string &id);
/* Every packed id the document's clips use. */
std::vector<std::string> media_packed_used(const Document &doc);

/* The Media layers (anywhere in the tree), and those with clips whose files are missing. */
std::vector<Layer *> media_layers(Document &doc);
int media_missing_count(const Document &doc);
/* Points every missing clip whose file name is in `folder` at it there. Returns how many. */
int media_relink_folder(Document &doc, const std::string &folder);

/* The sound every Media layer makes at `doc_seconds` (the document's frame f is at f / fps seconds)
 * for `frames` stereo frames at MEDIA_AUDIO_RATE, added into `out` (interleaved). Sources still
 * decoding are silent. Returns whether any sound was there. */
bool media_mix(const Document &doc, double doc_seconds, size_t frames, float *out);
/* Whether anything in the document has sound to play. */
bool media_has_sound(const Document &doc);

}  // namespace oanim
