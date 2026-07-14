// IMX900 (FRAMOS) Dual-Camera VR-Passthrough fuer Jetson Orin NX
//
// Pipeline:  libargus (ISP, dmabuf) -> NvBufSurface -> EGLImage
//            -> GLES3 (samplerExternalOES, Fisheye-Dewarp im Shader)
//            -> Offscreen-FBO in nativer Render-Aufloesung
//            -> skaliertes Blit -> SDL3-Fenster (physische Display-Aufloesung, Fullscreen)
//
// Ziel: minimale Ende-zu-Ende-Latenz durch Wegfall der GStreamer-Queues
// und Zero-Copy vom ISP bis zur GPU-Textur.
//
// Getestet gegen die dokumentierte Argus/EGL/NvBufSurface-API von
// JetPack 6.x (L4T R36). Kleinere Symbolnamen koennen sich zwischen
// L4T-Minor-Versionen unterscheiden -- im Zweifel gegen die Header in
// /usr/src/jetson_multimedia_api pruefen.

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
#include "DewarpConfig.hpp"
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

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // -----------------------------------------------------------------
    // Konfiguration
    // -----------------------------------------------------------------
    std::string configPath = "config/config_dewarper.txt";
    if (argc > 1) configPath = argv[1];

    DewarpConfig dewarp = DewarpConfig::loadFromFile(configPath);

    const uint32_t kSensorWidth  = static_cast<uint32_t>(dewarp.inputWidth);
    const uint32_t kSensorHeight = static_cast<uint32_t>(dewarp.inputHeight);
    const uint32_t kEyeWidth     = static_cast<uint32_t>(dewarp.outputWidth);
    const uint32_t kEyeHeight    = static_cast<uint32_t>(dewarp.outputHeight);
    // Native Render-Aufloesung (Offscreen-FBO): pro Auge kEyeWidth x kEyeHeight,
    // side-by-side macht das kEyeWidth*2 x kEyeHeight (bei euch z.B. 1552*2 x 1552).
    const uint32_t kWindowWidth  = kEyeWidth * 2;
    const uint32_t kWindowHeight = kEyeHeight;

    // Physische Fenster-/Display-Aufloesung. Das Fenster wird in DIESER
    // Aufloesung erstellt (nicht in kWindowWidth/kWindowHeight!) -- wir
    // rendern intern in kWindowWidth x kWindowHeight in ein FBO und
    // blitten das Ergebnis am Ende jedes Frames skaliert auf die
    // tatsaechliche Fenstergroesse.
    const int kScreenWidth  = 2880;
    const int kScreenHeight = 1440;

    const float    kFrameRateHz  = 72.0f;
    const uint32_t kSensorModeIndex = 0;

    // -----------------------------------------------------------------
    // SDL3 + GLES3 Fenster/Kontext
    // -----------------------------------------------------------------
    // Erzwingt EGL statt GLX unter X11 -- wir brauchen einen echten
    // EGLDisplay/EGLContext, damit die dmabuf->EGLImage-Interop
    // funktioniert (glX bietet das nicht).
    // SDL_VIDEO_X11_FORCE_EGL ist in SDL3 deprecated (ersetzt durch das
    // plattformunabhaengige SDL_VIDEO_FORCE_EGL) und wird auf manchen
    // Systemen offenbar nicht mehr zuverlaessig ausgewertet -- SDL faellt
    // dann still auf GLX zurueck, wodurch eglGetCurrentContext() spaeter
    // EGL_NO_CONTEXT liefert. Wir setzen beide Hints, um sowohl alte als
    // auch neue SDL3-Minor-Versionen abzudecken.
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

    // 0 = kein Wait-for-Vsync -> niedrigste Latenz, aber moegliches
    // Tearing. Auf 1 setzen, falls Tearing staerker stoert als die
    // zusaetzliche Latenz von max. einem Frame.
    SDL_GL_SetSwapInterval(0);

    loadGLExtensions();

    // Tatsaechliche physische Framebuffer-Groesse des Fensters ermitteln
    // (kann sich vom angeforderten kScreenWidth/kScreenHeight
    // unterscheiden, z.B. durch HighDPI-Skalierung oder wenn der
    // Fullscreen-Modus eine andere native Aufloesung erzwingt).
    int screenPxWidth = kScreenWidth;
    int screenPxHeight = kScreenHeight;
    SDL_GetWindowSizeInPixels(window, &screenPxWidth, &screenPxHeight);

    EGLDisplay eglDisplay = eglGetCurrentDisplay();
    if (eglDisplay == EGL_NO_DISPLAY) {
        // Fallback, falls SDL den Kontext nicht ueber eglMakeCurrent
        // "current" gemacht hat (sollte mit SDL_VIDEO_X11_FORCE_EGL
        // eigentlich nicht mehr noetig sein, schadet aber nicht).
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

    // Der Haupt-EGL-Context (falls SDL tatsaechlich EGL statt z.B. GLX
    // verwendet). Wird an ArgusCamera weitergereicht, damit deren
    // Capture-Thread sich optional einen GESHARETEN Context erzeugen
    // kann. Sharing ist hier aber nur ein Bonus, kein Muss: dmabuf-
    // basierte EGLImages (EGL_LINUX_DMA_BUF_EXT) sind laut Spec
    // context-unabhaengig -- der Capture-Thread braucht nur IRGENDEINEN
    // aktiven EGL-Context, damit der Treiber intern sauber initialisiert
    // ist. Deshalb hier kein harter Abbruch, falls SDL keinen "sichtbaren"
    // EGL-Context hat (z.B. weil es unter der Haube GLX statt EGL nutzt).
    EGLContext mainEglContext = eglGetCurrentContext();
    if (mainEglContext == EGL_NO_CONTEXT) {
        std::cerr << "[main] WARNUNG: kein aktiver EGLContext ueber eglGetCurrentContext() "
                     "gefunden (SDL nutzt evtl. GLX statt EGL). Capture-Threads bekommen "
                     "einen EIGENSTAENDIGEN EGL-Context ohne Sharing -- sollte trotzdem "
                     "funktionieren, da dmabuf-EGLImages context-unabhaengig sind.\n";
    }

    // -----------------------------------------------------------------
    // Argus CameraProvider + zwei Kameras
    // -----------------------------------------------------------------
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

    // -----------------------------------------------------------------
    // GL-Ressourcen: Shader, Fullscreen-Quad, externe Texturen
    // -----------------------------------------------------------------
    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShaderSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kDewarpFragmentShaderSrc);
    GLuint prog = linkProgram(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint locTex        = glGetUniformLocation(prog, "uTex");
    GLint locInputSize   = glGetUniformLocation(prog, "uInputSize");
    GLint locOutputSize  = glGetUniformLocation(prog, "uOutputSize");
    GLint locFocalLength = glGetUniformLocation(prog, "uFocalLength");
    GLint locVFov        = glGetUniformLocation(prog, "uVFovRad");
    GLint locRot         = glGetUniformLocation(prog, "uRot");

    // Fullscreen-Quad: Pos (x,y) + UV (u,v)
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

    // -----------------------------------------------------------------
    // Offscreen-FBO in nativer Render-Aufloesung (kWindowWidth x
    // kWindowHeight). Wir zeichnen beide Augen hier hinein und blitten
    // das Ergebnis pro Frame skaliert auf die tatsaechliche
    // Fenster-/Displaygroesse (kScreenWidth x kScreenHeight). Farbpuffer
    // reicht als Renderbuffer aus, da wir das FBO nur blitten und nicht
    // als Textur weitersampeln.
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

    // Debug-Diagnose: waehrend wir das Rendering absichern, faerben wir
    // den Hintergrund knallig ein. Bleibt das Fenster komplett magenta,
    // wird der Draw-Call gar nicht erst erreicht/ausgefuehrt (State- oder
    // Viewport-Problem). Zeigen sich schwarze Rechtecke INNERHALB des
    // magentafarbenen Hintergrunds, laeuft der Draw-Call, aber die
    // Textur liefert (noch) keine Bilddaten -- dann ist es das
    // Buffer-Lifetime-Thema in ArgusCamera. Nach dem Debuggen einfach
    // wieder auf glClearColor(0,0,0,1) zurueckstellen.
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);

    // Zaehler jetzt PRO AUGE statt einem gemeinsamen, damit die Logzeilen
    // tatsaechlich beide Augen gleichmaessig abdecken (vorher: ein
    // einziger "static thread_local" Counter wurde von beiden
    // drawEye()-Aufrufen gemeinsam hochgezaehlt, wodurch die Modulo-144-
    // Logzeilen fast nur beim selben Auge auftauchten).
    uint64_t drawCounterLeft = 0;
    uint64_t drawCounterRight = 0;

    auto drawEye = [&](ArgusCamera& cam, GLuint tex, int viewportX, uint64_t& drawCounter) {
        EGLImageKHR img = EGL_NO_IMAGE_KHR;
        bool isNew = false;
        bool haveFrame = cam.getLatestEGLImage(img, isNew);
        drawCounter++;
        // if (drawCounter <= 5 || drawCounter % 144 == 0) {
        //     std::cerr << "[drawEye] viewportX=" << viewportX
        //               << " haveFrame=" << haveFrame
        //               << " isNew=" << isNew
        //               << " img=" << img << "\n";
        // }
        if (haveFrame) {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
            if (isNew) {
                pGlEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
                checkGLError("glEGLImageTargetTexture2DOES");
            }
        } else {
            return; // noch kein Frame verfuegbar
        }

        glViewport(viewportX, 0, static_cast<int>(kEyeWidth), static_cast<int>(kEyeHeight));
        glUniform1i(locTex, 0);
        glUniform2f(locInputSize, static_cast<float>(kSensorWidth), static_cast<float>(kSensorHeight));
        glUniform2f(locOutputSize, static_cast<float>(kEyeWidth), static_cast<float>(kEyeHeight));
        glUniform1f(locFocalLength, focalLength);
        glUniform1f(locVFov, vfov);
        glUniformMatrix3fv(locRot, 1, GL_FALSE, rotMat);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        checkGLError("glDrawArrays");

        // DIAGNOSE: liest einen Pixel aus der Mitte des gerade gezeichneten
        // Auges zurueck und printet den tatsaechlichen Farbwert. Damit
        // koennen wir unterscheiden zwischen "Textur liefert wirklich
        // schwarze Daten" (RGB liest hier (0,0,0) obwohl haveFrame=1) und
        // "Daten sind da, aber das Fenster zeigt sie nicht an" (RGB waere
        // hier NICHT (0,0,0), das Fenster bliebe aber trotzdem schwarz --
        // dann liegt's am Compositor/Swap, nicht an der Pipeline).
        // Kann nach dem Debuggen wieder raus.
        // if (drawCounter % 144 == 0) {
        //     unsigned char px[4] = {0, 0, 0, 0};
        //     glReadPixels(viewportX + static_cast<int>(kEyeWidth) / 2,
        //                  static_cast<int>(kEyeHeight) / 2,
        //                  1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        //     std::cerr << "[readback] viewportX=" << viewportX
        //               << " centerPixelRGBA=("
        //               << (int)px[0] << "," << (int)px[1] << ","
        //               << (int)px[2] << "," << (int)px[3] << ")\n";
        // }
    };

    // -----------------------------------------------------------------
    // Render-Loop
    // -----------------------------------------------------------------
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);

    while (!g_quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) g_quit = true;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) g_quit = true;
        }

        // --- Beide Augen in nativer Aufloesung ins Offscreen-FBO zeichnen ---
        glBindFramebuffer(GL_FRAMEBUFFER, renderFbo);
        glClear(GL_COLOR_BUFFER_BIT);
        drawEye(camLeft,  texLeft,  0,                             drawCounterLeft);
        drawEye(camRight, texRight, static_cast<int>(kEyeWidth),   drawCounterRight);

        // --- Skaliertes Blit vom Offscreen-FBO auf das sichtbare Fenster ---
        glBindFramebuffer(GL_READ_FRAMEBUFFER, renderFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, static_cast<GLint>(kWindowWidth), static_cast<GLint>(kWindowHeight),
                           0, 0, screenPxWidth, screenPxHeight,
                           GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        SDL_GL_SwapWindow(window);
    }

    // -----------------------------------------------------------------
    // Aufraeumen
    // -----------------------------------------------------------------
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