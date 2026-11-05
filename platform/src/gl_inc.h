// gl_inc.h — OpenGL ES 2 headers: vitaGL on the Vita, Mesa (EGL + GLES2) on the PC test build.
#pragma once
#if defined(__vita__)
#include <vitaGL.h>
#define W3D_GL_CG 1          // shaders are compiled as Cg (GL_CG_*_SHADER_EXT), the Vita's native language
#else
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#define W3D_GL_CG 0
#endif
