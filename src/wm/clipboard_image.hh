#pragma once

#include <cstdint>
#include <string>

namespace oanim {

/* Puts an image (RGBA, rows from the top, straight alpha) on the system clipboard, as a PNG (which keeps
 * its transparency, for apps that read it: Photoshop, browsers, Figma) and as a bitmap (CF_DIBV5, for the
 * rest). False, with `r_error`, when it can't. */
bool clipboard_set_image(const uint8_t *rgba, int width, int height, std::string *r_error);

}  // namespace oanim
