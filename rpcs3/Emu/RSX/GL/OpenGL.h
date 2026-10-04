#pragma once
#ifndef _WIN32
#include <GL/glew.h>
#endif

#ifdef _WIN32
#include <Windows.h>
#ifdef RPCS3_UWP_MESA
// Mesa gl.h declares GL 1.2/1.3 entry points that RPCS3 loads dynamically.
// Keep its UWP-visible GL 1.1 declarations without colliding with our pointers.
#define glTexImage3D mesa_header_glTexImage3D
#define glTexSubImage3D mesa_header_glTexSubImage3D
#define glSampleCoverage mesa_header_glSampleCoverage
#define glBlendColor mesa_header_glBlendColor
#define glBlendEquation mesa_header_glBlendEquation
#define glActiveTexture mesa_header_glActiveTexture
#endif
#include "GL/gl.h"
#ifdef RPCS3_UWP_MESA
#undef glTexImage3D
#undef glTexSubImage3D
#undef glSampleCoverage
#undef glBlendColor
#undef glBlendEquation
#undef glActiveTexture
#endif
#include <glext.h>
typedef BOOL (WINAPI* PFNWGLSWAPINTERVALEXTPROC) (int interval);

#define OPENGL_PROC(p, n) extern p gl##n
#define WGL_PROC(p, n) extern p wgl##n
#define OPENGL_PROC2(p, n, tn) OPENGL_PROC(p, n)
	#include "GLProcTable.h"
#undef OPENGL_PROC
#undef WGL_PROC
#undef OPENGL_PROC2
#else
#include <GL/gl.h>
#ifdef HAVE_X11
#include <GL/glxew.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#endif
#endif

#ifndef GL_TEXTURE_BUFFER_BINDING
//During spec release, this enum was removed during upgrade from ARB equivalent
//See https://www.khronos.org/bugzilla/show_bug.cgi?id=844
#define GL_TEXTURE_BUFFER_BINDING 0x8C2A
#endif

namespace gl
{
	void init();
	void set_swapinterval(int interval);
}
