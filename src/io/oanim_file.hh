#pragma once

/* .oanim files: a zip archive holding document.json, the whole Document as JSON.
 *
 * A zip leaves room for images, audio and brushes saved beside the JSON in later milestones,
 * and the JSON stays readable and diffable. "version" counts format changes: a file from a
 * newer Open Animate is refused rather than half-read, and older versions are upgraded on load.
 *
 * Paths are UTF-8. */

#include <string>

#include "data/document.hh"

namespace oanim {

/* Current format version, written to every file. */
/* 2: bones' position and angle keys hold Transform and Angle (the change from their setup pose; the
 * Transform along the bone's rest axes). 3: points' keys hold their Transform and handles' their
 * Angle and Length, the change from the setup pose. 4: a key's interpolation is for the stretch
 * arriving at it (before, the stretch after it). */
inline constexpr int OANIM_FILE_VERSION = 4;

/* Writes through a temporary file and renames it into place, so a failed save never damages
 * the existing file. Returns false and sets r_error on failure. */
bool oanim_file_write(const Document &doc, const std::string &path, std::string *r_error);

/* Reads a file into r_doc, which is left untouched on failure. Damaged parts (a curve
 * pointing at a missing point, a reference to a missing layer) are dropped, not trusted. */
bool oanim_file_read(const std::string &path, Document &r_doc, std::string *r_error);

/* The JSON text of a document, as stored in document.json. */
std::string oanim_document_to_json(const Document &doc);
bool oanim_document_from_json(const std::string &text, Document &r_doc, std::string *r_error);
/* Brush settings as the file stores them (JSON text, Moho's names), for brush presets. */
std::string brush_settings_to_json(const BrushSettings &settings);
bool brush_settings_from_json(const std::string &text, BrushSettings *r_settings);

}  // namespace oanim
