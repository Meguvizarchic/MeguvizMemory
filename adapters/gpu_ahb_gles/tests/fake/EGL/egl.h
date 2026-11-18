#ifndef MGMEM_FAKE_EGL_H
#define MGMEM_FAKE_EGL_H
#include <stdint.h>
typedef void *EGLDisplay;
typedef void *EGLContext;
typedef void *EGLClientBuffer;
typedef void *EGLImageKHR;
typedef void *EGLSyncKHR;
typedef int32_t EGLint;
typedef uint32_t EGLBoolean;
typedef uint32_t EGLenum;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);
#define EGL_NO_DISPLAY nullptr
#define EGL_NO_CONTEXT nullptr
#define EGL_NO_IMAGE_KHR nullptr
#define EGL_NO_SYNC_KHR nullptr
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_EXTENSIONS 0x3055
#define EGL_SUCCESS 0x3000
#define EGL_CONTEXT_LOST 0x300E
#define EGL_BAD_ALLOC 0x3003
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define EGL_NATIVE_BUFFER_ANDROID 0x3140
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
extern "C" EGLDisplay eglGetCurrentDisplay();
extern "C" EGLContext eglGetCurrentContext();
extern "C" EGLenum eglQueryAPI();
extern "C" EGLint eglGetError();
extern "C" const char *eglQueryString(EGLDisplay, EGLint);
extern "C" __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *);
#endif
