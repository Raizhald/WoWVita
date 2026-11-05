#pragma once
#include <EGL/egl.h>
#define EGL_NO_CONFIG_KHR ((EGLConfig)0)
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
typedef EGLDisplay (*PFNEGLGETPLATFORMDISPLAYEXTPROC)(EGLenum, void*, const EGLint*);
