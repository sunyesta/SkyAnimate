#pragma once

/* OpenGL 3.3 functions beyond OpenGL 1.1, loaded through SDL at startup. Call them through the
 * `gl::` names, e.g. gl::GenFramebuffers(). */

#include <SDL3/SDL_opengl.h>

namespace sky::gl {

#define SKY_GL_FUNCTIONS(X) \
  X(PFNGLACTIVETEXTUREPROC, ActiveTexture) \
  X(PFNGLATTACHSHADERPROC, AttachShader) \
  X(PFNGLBINDBUFFERPROC, BindBuffer) \
  X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer) \
  X(PFNGLBINDRENDERBUFFERPROC, BindRenderbuffer) \
  X(PFNGLBINDVERTEXARRAYPROC, BindVertexArray) \
  X(PFNGLBLITFRAMEBUFFERPROC, BlitFramebuffer) \
  X(PFNGLBLENDEQUATIONPROC, BlendEquation) \
  X(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate) \
  X(PFNGLBUFFERDATAPROC, BufferData) \
  X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus) \
  X(PFNGLCOMPILESHADERPROC, CompileShader) \
  X(PFNGLCREATEPROGRAMPROC, CreateProgram) \
  X(PFNGLCREATESHADERPROC, CreateShader) \
  X(PFNGLDELETEBUFFERSPROC, DeleteBuffers) \
  X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers) \
  X(PFNGLDELETEPROGRAMPROC, DeleteProgram) \
  X(PFNGLDELETERENDERBUFFERSPROC, DeleteRenderbuffers) \
  X(PFNGLDELETESHADERPROC, DeleteShader) \
  X(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays) \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray) \
  X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, FramebufferRenderbuffer) \
  X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D) \
  X(PFNGLGENBUFFERSPROC, GenBuffers) \
  X(PFNGLGENERATEMIPMAPPROC, GenerateMipmap) \
  X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers) \
  X(PFNGLGENRENDERBUFFERSPROC, GenRenderbuffers) \
  X(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays) \
  X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog) \
  X(PFNGLGETPROGRAMIVPROC, GetProgramiv) \
  X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) \
  X(PFNGLGETSHADERIVPROC, GetShaderiv) \
  X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) \
  X(PFNGLLINKPROGRAMPROC, LinkProgram) \
  X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, RenderbufferStorageMultisample) \
  X(PFNGLSHADERSOURCEPROC, ShaderSource) \
  X(PFNGLUNIFORM1FPROC, Uniform1f) \
  X(PFNGLUNIFORM1IPROC, Uniform1i) \
  X(PFNGLUNIFORMMATRIX3FVPROC, UniformMatrix3fv) \
  X(PFNGLUSEPROGRAMPROC, UseProgram) \
  X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer)

#define SKY_GL_DECLARE(type, name) extern type name;
SKY_GL_FUNCTIONS(SKY_GL_DECLARE)
#undef SKY_GL_DECLARE

/* Loads every function above. Needs a current OpenGL context. Returns false if any is missing. */
bool load();

}  // namespace sky::gl
