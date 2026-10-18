#include "gpu/gl.hh"

#include <SDL3/SDL_video.h>

#include "core/log.hh"

namespace sky::gl {

#define SKY_GL_DEFINE(type, name) type name = nullptr;
SKY_GL_FUNCTIONS(SKY_GL_DEFINE)
#undef SKY_GL_DEFINE

bool load()
{
  bool ok = true;
#define SKY_GL_LOAD(type, name) \
  name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name)); \
  if (!name) { \
    report(ReportType::Error, "OpenGL function missing: gl" #name); \
    ok = false; \
  }
  SKY_GL_FUNCTIONS(SKY_GL_LOAD)
#undef SKY_GL_LOAD
  return ok;
}

}  // namespace sky::gl
