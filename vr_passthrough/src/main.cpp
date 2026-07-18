// Pipeline:  libargus (ISP, dmabuf) -> NvBufSurface -> EGLImage
#include <SDL3/SDL.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include "GLExtensions.hpp"

#include <Argus/Argus.h>

#include <iostream>
#include <vector>
#include <csignal>
#include <atomic>

#include "ArgusCamera.hpp"
#include "Config.hpp"
#include "Shaders.hpp"

using namespace Argus;

namespace {

	std::atomic<bool> g_quit{false};

	void handleSignal(int) { g_quit = true; }

	GLuint compileShader(GLenum type, const char* src) {
	    GLuint shader = glCreateShader(type);
	    glShaderSource(shader, 1, &src, nullptr);
	    glCompileShader(shader);
	    GLint ok = 0;
	    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	    if (!ok) {
	        char log[2048];
	        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
	        std::cerr << "Shader-Compile-Fehler:\n" << log << "\n";
	        std::exit(1);
	    }
	    return shader;
	}

	GLuint linkProgram(GLuint vs, GLuint fs) {
	    GLuint prog = glCreateProgram();
	    glAttachShader(prog, vs);
	    glAttachShader(prog, fs);
	    glLinkProgram(prog);
	    GLint ok = 0;
	    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	    if (!ok) {
	        char log[2048];
	        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
	        std::cerr << "Programm-Link-Fehler:\n" << log << "\n";
	        std::exit(1);
	    }
	    return prog;
	}

	void checkGLError(const char* tag) {
	    GLenum err;
	    while ((err = glGetError()) != GL_NO_ERROR) {
	        std::cerr << "[GL-Fehler] " << tag << ": 0x" << std::hex << err << std::dec << "\n";
	    }
	}

}

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    if (argc != 2)
	std::cerr << "Error! Usage: ./program [path/config_file]" << std::endl;

    DewarpConfig dewarp = DewarpConfig::loadFromFile(argv[1]);

    const uint32_t kSensorWidth  = static_cast<uint32_t>(dewarp.inputWidth);
    const uint32_t kSensorHeight = static_cast<uint32_t>(dewarp.inputHeight);
    const uint32_t kEyeWidth     = static_cast<uint32_t>(dewarp.outputWidth);
    const uint32_t kEyeHeight    = static_cast<uint32_t>(dewarp.outputHeight);
    const uint32_t kWindowWidth  = kEyeWidth * 2;
    const uint32_t kWindowHeight = kEyeHeight;
    const int kScreenWidth  = 2880;
    const int kScreenHeight = 1440;

    const float    kFrameRateHz  = 72.0f;
    const uint32_t kSensorModeIndex = 0;
    SDL_SetHint("SDL_VIDEO_FORCE_EGL", "1");
    SDL_SetHint("SDL_VIDEO_X11_FORCE_EGL", "1");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init fehlgeschlagen: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window* window = SDL_CreateWindow(
        "IMX900 VR Passthrough",
        kScreenWidth, kScreenHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
    if (!window) {
        std::cerr << "SDL_CreateWindow fehlgeschlagen: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        std::cerr << "SDL_GL_CreateContext fehlgeschlagen: " << SDL_GetError() << "\n";
        return 1;
    }
    SDL_GL_SetSwapInterval(0);

    loadGLExtensions();
    int screenPxWidth = kScreenWidth;
    int screenPxHeight = kScreenHeight;
    SDL_GetWindowSizeInPixels(window, &screenPxWidth, &screenPxHeight);

    EGLDisplay eglDisplay = eglGetCurrentDisplay();
    if (eglDisplay == EGL_NO_DISPLAY) {
        eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (eglDisplay != EGL_NO_DISPLAY) {
            EGLint major = 0, minor = 0;
            eglInitialize(eglDisplay, &major, &minor);
        }
    }
    if (eglDisplay == EGL_NO_DISPLAY) {
        std::cerr << "Kein aktives EGLDisplay gefunden\n";
        return 1;
    }
    EGLContext mainEglContext = eglGetCurrentContext();
    if (mainEglContext == EGL_NO_CONTEXT) {
        std::cerr << "[main] WARNUNG: kein aktiver EGLContext ueber eglGetCurrentContext() "
                     "gefunden (SDL nutzt evtl. GLX statt EGL). Capture-Threads bekommen "
                     "einen EIGENSTAENDIGEN EGL-Context ohne Sharing -- sollte trotzdem "
                     "funktionieren, da dmabuf-EGLImages context-unabhaengig sind.\n";
    }
    UniqueObj<CameraProvider> cameraProvider(CameraProvider::create());
    ICameraProvider* iCameraProvider = interface_cast<ICameraProvider>(cameraProvider);
    if (!iCameraProvider) {
        std::cerr << "Konnte ICameraProvider nicht erstellen\n";
        return 1;
    }

    std::vector<CameraDevice*> cameraDevices;
    iCameraProvider->getCameraDevices(&cameraDevices);
    if (cameraDevices.size() < 2) {
        std::cerr << "Es wurden weniger als 2 Kameras gefunden ("
                  << cameraDevices.size() << ")\n";
        return 1;
    }

    ArgusCamera camLeft(iCameraProvider, cameraDevices[0], eglDisplay, mainEglContext,
                        kSensorWidth, kSensorHeight, kSensorModeIndex, kFrameRateHz);
    ArgusCamera camRight(iCameraProvider, cameraDevices[1], eglDisplay, mainEglContext,
                         kSensorWidth, kSensorHeight, kSensorModeIndex, kFrameRateHz);

    if (!camLeft.start() || !camRight.start()) {
        std::cerr << "Konnte Kamera-Capture nicht starten\n";
        return 1;
    }
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShaderSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kDewarpFragmentShaderSrc);
    GLuint prog = linkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint locTex         = glGetUniformLocation(prog, "uTex");
    GLint locInputSize   = glGetUniformLocation(prog, "uInputSize");
    GLint locOutputSize  = glGetUniformLocation(prog, "uOutputSize");
    GLint locFocalLength = glGetUniformLocation(prog, "uFocalLength");
    GLint locVFov        = glGetUniformLocation(prog, "uVFovRad");
    GLint locRot         = glGetUniformLocation(prog, "uRot");
    GLint locK1		 = glGetUniformLocation(prog, "k1");
    GLint locK2		 = glGetUniformLocation(prog, "k2");

    const float quadVerts[] = {
        -1.f, -1.f,  0.f, 1.f,
         1.f, -1.f,  1.f, 1.f,
        -1.f,  1.f,  0.f, 0.f,
         1.f,  1.f,  1.f, 0.f,
    };
    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    GLuint texLeft = 0, texRight = 0;
    glGenTextures(1, &texLeft);
    glGenTextures(1, &texRight);
    for (GLuint tex : { texLeft, texRight }) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    GLuint renderFbo = 0, renderColorRbo = 0;
    glGenFramebuffers(1, &renderFbo);
    glGenRenderbuffers(1, &renderColorRbo);

    glBindRenderbuffer(GL_RENDERBUFFER, renderColorRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8,
                           static_cast<GLsizei>(kWindowWidth),
                           static_cast<GLsizei>(kWindowHeight));

    glBindFramebuffer(GL_FRAMEBUFFER, renderFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_RENDERBUFFER, renderColorRbo);

    GLenum fboStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fboStatus != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[main] Offscreen-FBO unvollstaendig: 0x"
                  << std::hex << fboStatus << std::dec << "\n";
        return 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    float rotMat[9];
    dewarp.buildRotationMatrix(rotMat);
    const float vfov = static_cast<float>(dewarp.verticalFovRad());
    const float focalLength = static_cast<float>(dewarp.focalLength);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    uint64_t drawCounterLeft = 0;
    uint64_t drawCounterRight = 0;

    auto drawEye = [&](ArgusCamera& cam, GLuint tex, int viewportX, uint64_t& drawCounter) {
        EGLImageKHR img = EGL_NO_IMAGE_KHR;
        bool isNew = false;
        bool haveFrame = cam.getLatestEGLImage(img, isNew);
        drawCounter++;
        if (haveFrame) {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
            if (isNew) {
                pGlEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
                checkGLError("glEGLImageTargetTexture2DOES");
            }
        } else {
            return;
        }

        glViewport(viewportX, 0, static_cast<int>(kEyeWidth), static_cast<int>(kEyeHeight));
        glUniform1i(locTex, 0);
        glUniform2f(locInputSize, static_cast<float>(kSensorWidth), static_cast<float>(kSensorHeight));
        glUniform2f(locOutputSize, static_cast<float>(kEyeWidth), static_cast<float>(kEyeHeight));
        glUniform1f(locFocalLength, focalLength);
        glUniform1f(locVFov, vfov);
        glUniformMatrix3fv(locRot, 1, GL_FALSE, rotMat);

	glUniform1f(locK1, dewarp.k1);
	glUniform1f(locK2, dewarp.k2);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        checkGLError("glDrawArrays");
    };
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);

    while (!g_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) g_quit = true;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) g_quit = true;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, renderFbo);
        glClear(GL_COLOR_BUFFER_BIT);
        drawEye(camLeft,  texLeft,  0,                             drawCounterLeft);
        drawEye(camRight, texRight, static_cast<int>(kEyeWidth),   drawCounterRight);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, renderFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, static_cast<GLint>(kWindowWidth), static_cast<GLint>(kWindowHeight),
                           0, 0, screenPxWidth, screenPxHeight,
                           GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        SDL_GL_SwapWindow(window);
    }
    camLeft.stop();
    camRight.stop();

    glDeleteRenderbuffers(1, &renderColorRbo);
    glDeleteFramebuffers(1, &renderFbo);
    glDeleteTextures(1, &texLeft);
    glDeleteTextures(1, &texRight);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);

    SDL_GL_DestroyContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
