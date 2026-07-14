#include "GLExtensions.hpp"
#include <cstdio>
#include <cstdlib>

PFNEGLDESTROYIMAGEKHRPROC           pEglDestroyImageKHR = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pGlEGLImageTargetTexture2DOES = nullptr;

void loadGLExtensions() {
    pEglDestroyImageKHR =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
            eglGetProcAddress("eglDestroyImageKHR"));
    pGlEGLImageTargetTexture2DOES =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (!pEglDestroyImageKHR || !pGlEGLImageTargetTexture2DOES) {
        std::fprintf(stderr,
            "Konnte benoetigte EGL/GLES-Extensions nicht laden "
            "(eglDestroyImageKHR=%p, glEGLImageTargetTexture2DOES=%p)\n",
            (void*)pEglDestroyImageKHR, (void*)pGlEGLImageTargetTexture2DOES);
        std::exit(1);
    }
}
