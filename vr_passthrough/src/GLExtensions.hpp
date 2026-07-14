#pragma once
// EGL/GLES-Extension-Funktionen (eglDestroyImageKHR,
// glEGLImageTargetTexture2DOES, ...) sind auf vielen Systemen nicht als
// direkte Linker-Symbole in libEGL/libGLESv2 vorhanden, sondern muessen
// zur Laufzeit ueber eglGetProcAddress() aufgeloest werden. Diese Datei
// stellt entsprechende Funktionszeiger bereit.

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

extern PFNEGLDESTROYIMAGEKHRPROC             pEglDestroyImageKHR;
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC   pGlEGLImageTargetTexture2DOES;

// Muss einmal aufgerufen werden, NACHDEM ein aktueller EGL/GL-Kontext
// existiert (z.B. direkt nach SDL_GL_CreateContext).
void loadGLExtensions();
