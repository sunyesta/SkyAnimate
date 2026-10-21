#include "gpu/gl.hh"

#include <SDL3/SDL_video.h>

#include "core/log.hh"

namespace oanim::gl {

#define OANIM_GL_DEFINE(type, name) type name = nullptr;
OANIM_GL_FUNCTIONS(OANIM_GL_DEFINE)
#undef OANIM_GL_DEFINE

bool load()
{
  bool ok = true;
#define OANIM_GL_LOAD(type, name) \
  name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name)); \
  if (!name) { \
    report(ReportType::Error, "OpenGL function missing: gl" #name); \
    ok = false; \
  }
  OANIM_GL_FUNCTIONS(OANIM_GL_LOAD)
#undef OANIM_GL_LOAD
  return ok;
}

}  // namespace oanim::gl
