// Minimal EGL 1.5 declarations for the WoW3DS PC test build (only what platform_pc.cpp uses).
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int EGLBoolean; typedef int32_t EGLint; typedef unsigned int EGLenum;
typedef void* EGLDisplay; typedef void* EGLConfig; typedef void* EGLContext; typedef void* EGLSurface;
typedef void* EGLNativeDisplayType;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_SUCCESS 0x3000
#define EGL_NONE 0x3038
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0
EGLDisplay eglGetDisplay(EGLNativeDisplayType);
EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*);
EGLBoolean eglTerminate(EGLDisplay);
EGLBoolean eglBindAPI(EGLenum);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLint eglGetError(void);
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char*);
#ifdef __cplusplus
}
#endif
