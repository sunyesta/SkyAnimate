#pragma once

/* Media layer operators (media.*): Add Media and the rest, in media_ops.cc. */

#include <string>

namespace oanim {

struct Context;
struct Layer;
struct MediaInfo;

void media_operators_register();

/* What a file is, for reports and the Media tab: "Video, 1920 x 1080, 30 fps, 4.2 s, with sound". */
std::string media_info_text(const MediaInfo &info);
/* The file dialog's pattern for everything Media layers read. */
const char *media_file_pattern();
/* The Media layer an operator works on: `layer` (its id) or the active layer, when it is one. */
Layer *media_target_layer(Context &C);

}  // namespace oanim
