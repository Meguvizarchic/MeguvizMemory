#ifndef MGMEM_FAKE_EGLEXT_H
#define MGMEM_FAKE_EGLEXT_H
#include "egl.h"
struct AHardwareBuffer;
typedef EGLClientBuffer (*PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)(const AHardwareBuffer *);
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint *);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef EGLSyncKHR (*PFNEGLCREATESYNCKHRPROC)(EGLDisplay, EGLenum, const EGLint *);
typedef EGLBoolean (*PFNEGLDESTROYSYNCKHRPROC)(EGLDisplay, EGLSyncKHR);
typedef EGLint (*PFNEGLDUPNATIVEFENCEFDANDROIDPROC)(EGLDisplay, EGLSyncKHR);
#endif
