#pragma once

/* Files dragged over the window from Explorer, while they are still being dragged: Open Animate's
 * own OLE drop target in place of SDL's, which only names the files once they are dropped. It reads
 * them as the drag enters, so the editors can show where they would land (a clip's bar in the
 * timeline, the picture in the camera's frame), and on the drop sends SDL's usual
 * SDL_EVENT_DROP_FILE events, at the position dropped. */

#include <string>
#include <vector>

struct SDL_Window;

namespace oanim {

struct FileDrag {
  bool active = false;
  /* The files being dragged (UTF-8), and where the mouse is in the window (as ImGui counts). */
  std::vector<std::string> files;
  float x = 0.0f, y = 0.0f;
};

/* Replaces SDL's drop target on the window (after SDL made the window, OLE set up). */
void file_drop_install(SDL_Window *window);
const FileDrag &file_drag();
/* As if `drag` were being dragged over the window (oanim.wm.file_drag_simulate, checking the previews). */
void file_drag_set(const FileDrag &drag);

}  // namespace oanim
