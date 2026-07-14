#pragma once

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

extern PFNEGLDESTROYIMAGEKHRPROC             pEglDestroyImageKHR;
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC   pGlEGLImageTargetTexture2DOES;
void loadGLExtensions();
