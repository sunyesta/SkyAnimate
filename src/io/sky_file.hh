#pragma once

/* .sky files: a zip archive holding document.json, the whole Document as JSON.
 *
 * A zip leaves room for images, audio and brushes saved beside the JSON in later milestones,
 * and the JSON stays readable and diffable. "version" counts format changes: a file from a
 * newer SkyAnimate is refused rather than half-read, and older versions are upgraded on load.
 *
 * Paths are UTF-8. */

#include <string>

#include "data/document.hh"

namespace sky {

/* Current format version, written to every file. */
inline constexpr int SKY_FILE_VERSION = 1;

/* Writes through a temporary file and renames it into place, so a failed save never damages
 * the existing file. Returns false and sets r_error on failure. */
bool sky_file_write(const Document &doc, const std::string &path, std::string *r_error);

/* Reads a file into r_doc, which is left untouched on failure. Damaged parts (a curve
 * pointing at a missing point, a reference to a missing layer) are dropped, not trusted. */
bool sky_file_read(const std::string &path, Document &r_doc, std::string *r_error);

/* The JSON text of a document, as stored in document.json. */
std::string sky_document_to_json(const Document &doc);
bool sky_document_from_json(const std::string &text, Document &r_doc, std::string *r_error);

}  // namespace sky
