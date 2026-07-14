#include "ArgusCamera.hpp"

#include <nvbufsurface.h>   // NvBufSurface API (JetPack 5.1+/6.x)
// Falls dein JetPack noch die alte nvbuf_utils.h API nutzt (<=5.0),
// ersetze die mit "ALT:" markierten Stellen weiter unten.

#include <iostream>
#include <cstring>
#include <algorithm>

using namespace Argus;
using namespace EGLStream;

ArgusCamera::ArgusCamera(ICameraProvider* iCameraProvider,
                          CameraDevice* cameraDevice,
                          EGLDisplay eglDisplay,
                          EGLContext sharedEglContext,
                          uint32_t width, uint32_t height,
                          uint32_t sensorModeIndex,
                          float frameRateHz)
    : m_iCameraProvider(iCameraProvider)
    , m_cameraDevice(cameraDevice)
    , m_eglDisplay(eglDisplay)
    , m_sharedEglContext(sharedEglContext)
    , m_width(width)
    , m_height(height)
    , m_sensorModeIndex(sensorModeIndex)
    , m_frameRateHz(frameRateHz)
{}

ArgusCamera::~ArgusCamera() {
    stop();
    drainAll();
}

bool ArgusCamera::start() {
    m_captureSession = UniqueObj<CaptureSession>(
        m_iCameraProvider->createCaptureSession(m_cameraDevice));
    ICaptureSession* iCaptureSession = interface_cast<ICaptureSession>(m_captureSession);
    if (!iCaptureSession) {
        std::cerr << "Konnte CaptureSession nicht erstellen\n";
        return false;
    }

    UniqueObj<OutputStreamSettings> streamSettings(
        iCaptureSession->createOutputStreamSettings(STREAM_TYPE_EGL));
    IEGLOutputStreamSettings* iEglSettings =
        interface_cast<IEGLOutputStreamSettings>(streamSettings);
    if (!iEglSettings) {
        std::cerr << "Keine IEGLOutputStreamSettings\n";
        return false;
    }

    iEglSettings->setPixelFormat(PIXEL_FMT_YCbCr_420_888);
    iEglSettings->setResolution(Size2D<uint32_t>(m_width, m_height));
    iEglSettings->setEGLDisplay(m_eglDisplay);
    // Entscheidend fuer niedrige Latenz: MAILBOX statt FIFO -> Argus
    // wirft alte, noch nicht konsumierte Frames weg statt sie zu queuen.
    iEglSettings->setMode(EGL_STREAM_MODE_MAILBOX);
    iEglSettings->setMetadataEnable(true);

    m_outputStream = UniqueObj<OutputStream>(
        iCaptureSession->createOutputStream(streamSettings.get()));
    if (!m_outputStream) {
        std::cerr << "Konnte OutputStream nicht erstellen\n";
        return false;
    }

    m_frameConsumer = UniqueObj<FrameConsumer>(
        FrameConsumer::create(m_outputStream.get()));
    if (!m_frameConsumer) {
        std::cerr << "Konnte FrameConsumer nicht erstellen\n";
        return false;
    }

    // Request mit repeat() aufsetzen
    UniqueObj<Request> request(iCaptureSession->createRequest());
    IRequest* iRequest = interface_cast<IRequest>(request);
    iRequest->enableOutputStream(m_outputStream.get());

    ISourceSettings* iSourceSettings =
        interface_cast<ISourceSettings>(iRequest->getSourceSettings());
    if (iSourceSettings) {
        ICameraProperties* iCameraProperties =
            interface_cast<ICameraProperties>(m_cameraDevice);
        std::vector<SensorMode*> sensorModes;
        iCameraProperties->getAllSensorModes(&sensorModes);
        // if (m_sensorModeIndex < sensorModes.size()) {
        //     iSourceSettings->setSensorMode(sensorModes[m_sensorModeIndex]);
        //     ISensorMode* iSensorMode = interface_cast<ISensorMode>(sensorModes[m_sensorModeIndex]);
        //     // if (iSensorMode) {
        //         // Size2D<uint32_t> res = iSensorMode->getResolution();
        //         // std::cerr << "[ArgusCamera] SensorMode " << m_sensorModeIndex
        //                 //   << " Aufloesung=" << res.width() << "x" << res.height()
        //                 //   << " (Config nimmt an: siehe config_dewarper.txt width/height)\n";
        //     // }
        // }
        uint64_t frameDurationNs =
            static_cast<uint64_t>(1e9 / m_frameRateHz);
        iSourceSettings->setFrameDurationRange(
            Range<uint64_t>(frameDurationNs, frameDurationNs));
    }

    // Denoise / Edge-Enhance aus, analog tnr-mode=0 ee-mode=0
    IDenoiseSettings* iDenoise = interface_cast<IDenoiseSettings>(request);
    if (iDenoise) iDenoise->setDenoiseMode(DENOISE_MODE_OFF);

    IEdgeEnhanceSettings* iEE = interface_cast<IEdgeEnhanceSettings>(request);
    if (iEE) iEE->setEdgeEnhanceMode(EDGE_ENHANCE_MODE_OFF);

    // wbmode=1 -> Auto-Weissabgleich
    IAutoControlSettings* iAutoControl =
        interface_cast<IAutoControlSettings>(iRequest->getAutoControlSettings());
    if (iAutoControl) {
        iAutoControl->setAwbMode(AWB_MODE_AUTO);

        // ENTSCHEIDEND fuer die IMX900: ohne diese Zeile bleibt das Bild
        // schwarz (per glReadPixels bestaetigt -- die GL/EGL-Pipeline war
        // die ganze Zeit korrekt, das Problem sass in der ISP-Konfig).
        // Entspricht 1:1 dem "ispdigitalgainrange=1 1" Property aus der
        // funktionierenden nvarguscamerasrc-Gstreamer-Pipeline: der
        // ISP-Digital-Gain wird auf genau 1.0 fixiert statt der Kamera
        // (bzw. deren AE-Tuning fuer diesen Sensor) einen Regelbereich zu
        // ueberlassen, in dem sie offenbar haengen bleibt/kollabiert.
        iAutoControl->setIspDigitalGainRange(Range<float>(1.0f, 1.0f));
    }

    Status status = iCaptureSession->repeat(request.get());
    if (status != STATUS_OK) {
        std::cerr << "iCaptureSession->repeat() fehlgeschlagen: " << status << "\n";
        return false;
    }

    m_running = true;
    m_thread = std::thread(&ArgusCamera::captureThreadFunc, this);
    return true;
}

void ArgusCamera::stop() {
    if (!m_running.exchange(false)) return;
    if (m_captureSession) {
        ICaptureSession* iCaptureSession = interface_cast<ICaptureSession>(m_captureSession);
        if (iCaptureSession) iCaptureSession->stopRepeat();
    }
    if (m_thread.joinable()) m_thread.join();
}

void ArgusCamera::destroySlot(Slot& slot) {
    // Diese Funktion wird ausschliesslich fuer Slots aufgerufen, die
    // bereits kRetireDepth Generationen "alt" sind -- zu dem Zeitpunkt
    // hat die GPU laengst fertig gesampelt.
    if (slot.eglImage != EGL_NO_IMAGE_KHR) {
        pEglDestroyImageKHR(m_eglDisplay, slot.eglImage);
        slot.eglImage = EGL_NO_IMAGE_KHR;
    }
    if (slot.bufSurface) {
        NvBufSurfaceUnMapEglImage(reinterpret_cast<NvBufSurface*>(slot.bufSurface), 0);
        NvBufSurfaceDestroy(reinterpret_cast<NvBufSurface*>(slot.bufSurface));
        slot.bufSurface = nullptr;
    }
}

void ArgusCamera::drainAll() {
    std::lock_guard<std::mutex> lock(m_slotMutex);
    destroySlot(m_current);
    while (!m_retired.empty()) {
        destroySlot(m_retired.front());
        m_retired.pop_front();
    }
}

namespace {

// Erzeugt einen surfaceless EGL-Context, der mit shareContext geshared
// ist -- siehe Erklaerung in ArgusCamera.hpp ("EGL-Context auf dem
// Capture-Thread"). Der Config-Typ ist fuer surfaceless egal (wir
// binden diesen Context nie an eine Surface), er muss nur zur
// Client-Version passen.
EGLContext createSharedThreadContext(EGLDisplay display, EGLContext shareContext) {
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(display, configAttribs, &config, 1, &numConfigs) || numConfigs < 1) {
        std::cerr << "[ArgusCamera] eglChooseConfig fehlgeschlagen (Capture-Thread-Context)\n";
        return EGL_NO_CONTEXT;
    }
    const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(display, config, shareContext, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        std::cerr << "[ArgusCamera] eglCreateContext fehlgeschlagen (Capture-Thread-Context): 0x"
                  << std::hex << eglGetError() << std::dec << "\n";
    }
    return ctx;
}

} // namespace

void ArgusCamera::captureThreadFunc() {
    IFrameConsumer* iFrameConsumer = interface_cast<IFrameConsumer>(m_frameConsumer);

    // Eigener EGL-Context fuer diesen Thread, geshared mit dem
    // Haupt-GL-Context -- noetig, damit NvBufSurfaceMapEglImage() unten
    // ein EGLImage erzeugt, das der GL-Thread spaeter auch wirklich mit
    // Bilddaten befuellt sampeln kann (siehe Kommentar in .hpp).
    EGLContext threadCtx = createSharedThreadContext(m_eglDisplay, m_sharedEglContext);
    bool haveThreadCtx = (threadCtx != EGL_NO_CONTEXT);
    // if (haveThreadCtx) {
    //     if (eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, threadCtx) == EGL_FALSE) {
    //         std::cerr << "[ArgusCamera] eglMakeCurrent fehlgeschlagen (Capture-Thread): 0x"
    //                   << std::hex << eglGetError() << std::dec << "\n";
    //     } else {
    //         std::cerr << "[ArgusCamera] Capture-Thread EGL-Context aktiv\n";
    //     }
    // } else {
    //     std::cerr << "[ArgusCamera] WARNUNG: kein Capture-Thread-EGL-Context -- "
    //                  "EGLImages werden vermutlich schwarz bleiben\n";
    // }

    while (m_running) {
        Status status = STATUS_OK;
        UniqueObj<Frame> frame(iFrameConsumer->acquireFrame(1'000'000'000ULL /*1s Timeout*/, &status));
        if (status != STATUS_OK || !frame) {
            if (!m_running) break;
            continue; // Timeout o.ae. -> einfach erneut versuchen
        }

        IFrame* iFrame = interface_cast<IFrame>(frame);
        Image* image = iFrame->getImage();
        NV::IImageNativeBuffer* iNativeBuffer =
            interface_cast<NV::IImageNativeBuffer>(image);
        if (!iNativeBuffer) {
            std::cerr << "Kein IImageNativeBuffer verfuegbar\n";
            continue;
        }

        // dmabuf-Fd aus dem Argus-Frame erzeugen (zero-copy, ISP schreibt
        // direkt in diesen Buffer)
        // WICHTIG: BLOCK_LINEAR statt PITCH. Die GPU-Textureinheit
        // erwartet fuer direktes Zero-Copy-Sampling via
        // GL_TEXTURE_EXTERNAL_OES auf Tegra block-lineares (gekacheltes)
        // Layout -- mit PITCH (linear, eher fuer CPU-Zugriff/Encoding
        // gedacht) liefert glEGLImageTargetTexture2DOES() zwar keinen
        // GL-Fehler, aber texture() liest trotzdem durchgehend (0,0,0)
        // aus (per glReadPixels bestaetigt).
        int dmabufFd = iNativeBuffer->createNvBuffer(
            Size2D<uint32_t>(m_width, m_height),
            NVBUF_COLOR_FORMAT_NV12,
            NVBUF_LAYOUT_BLOCK_LINEAR,
            EGLStream::NV::ROTATION_0,
            &status);
        if (dmabufFd < 0 || status != STATUS_OK) {
            std::cerr << "createNvBuffer fehlgeschlagen\n";
            continue;
        }

        // static thread_local uint64_t frameCounter = 0;
        // frameCounter++;
        // if (frameCounter == 1 || frameCounter % 72 == 0) {
        //     // std::cerr << "[ArgusCamera] Frame " << frameCounter
        //     //           << " erhalten, dmabufFd=" << dmabufFd << "\n";

        //     // Belichtungs-Metadaten mitloggen (setMetadataEnable(true) ist
        //     // oben schon aktiv) -- falls Exposure/Gain bei Minimum
        //     // haengen, waere das ein klares Zeichen, dass die AE nicht
        //     // konvergiert (z.B. weil der Frame-Duration-Range oben zu eng
        //     // ist und der AE-Regler keinen Spielraum hat).
        //     CaptureMetadata* metadata = nullptr;
        //     IArgusCaptureMetadata* iArgusCaptureMetadata =
        //         interface_cast<IArgusCaptureMetadata>(frame);
        //     if (iArgusCaptureMetadata) {
        //         metadata = iArgusCaptureMetadata->getMetadata();
        //     }
        //     ICaptureMetadata* iMetadata = interface_cast<ICaptureMetadata>(metadata);
        //     if (iMetadata) {
        //         // std::cerr << "[ArgusCamera]   Exposure="
        //                 //   << iMetadata->getSensorExposureTime() << "ns"
        //                 //   << " AnalogGain=" << iMetadata->getSensorAnalogGain()
        //                 //   << " SceneLux=" << iMetadata->getSceneLux() << "\n";
        //     } else {
        //         std::cerr << "[ArgusCamera]   Keine ICaptureMetadata verfuegbar\n";
        //     }
        // }

        frame.reset();

        NvBufSurface* surf = nullptr;
        if (NvBufSurfaceFromFd(dmabufFd, reinterpret_cast<void**>(&surf)) != 0 || !surf) {
            std::cerr << "NvBufSurfaceFromFd fehlgeschlagen\n";
            continue;
        }

        // DIAGNOSE: Rohbild direkt von der CPU aus dem dmabuf lesen, BEVOR
        // die GPU/EGLImage/OpenGL-Kette ueberhaupt ins Spiel kommt.
        // if (frameCounter == 1 || frameCounter % 72 == 0) {
        //     if (NvBufSurfaceMap(surf, 0, -1, NVBUF_MAP_READ) == 0) {
        //         NvBufSurfaceSyncForCpu(surf, 0, -1);
        //         uint8_t* yPlane = static_cast<uint8_t*>(surf->surfaceList[0].mappedAddr.addr[0]);
        //         uint32_t pitch  = surf->surfaceList[0].planeParams.pitch[0];
        //         uint32_t w      = surf->surfaceList[0].width;
        //         uint32_t h      = surf->surfaceList[0].height;
        //         uint32_t cx = w / 2, cy = h / 2;
        //         if (yPlane) {
        //             uint8_t centerY = yPlane[cy * pitch + cx];
        //             uint8_t rowMin = 255, rowMax = 0;
        //             for (uint32_t x = 0; x < w; x++) {
        //                 uint8_t v = yPlane[cy * pitch + x];
        //                 rowMin = std::min(rowMin, v);
        //                 rowMax = std::max(rowMax, v);
        //             }
        //             std::cerr << "[ArgusCamera]   CPU-Rohbild (NV12 Y-Plane): "
        //                       << "Mitte=" << (int)centerY
        //                       << " ZeilenMin=" << (int)rowMin
        //                       << " ZeilenMax=" << (int)rowMax << "\n";
        //         } else {
        //             std::cerr << "[ArgusCamera]   CPU-Mapping lieferte nullptr fuer Y-Plane\n";
        //         }
        //         NvBufSurfaceUnMap(surf, 0, -1);
        //     } else {
        //         std::cerr << "[ArgusCamera]   NvBufSurfaceMap (CPU) fehlgeschlagen\n";
        //     }
        // }

        if (NvBufSurfaceMapEglImage(surf, 0) != 0) {
            std::cerr << "NvBufSurfaceMapEglImage fehlgeschlagen\n";
            continue;
        }
        EGLImageKHR eglImage = surf->surfaceList[0].mappedAddr.eglImage;

        Slot newSlot;
        newSlot.bufSurface = surf;
        newSlot.eglImage   = eglImage;

        // Slot tauschen: das bisherige "current" wandert in die
        // Retire-Queue statt sofort zerstoert zu werden. Erst wenn die
        // Queue kRetireDepth Eintraege ueberschreitet, wird der
        // AELTESTE Eintrag wirklich freigegeben -- der ist dann
        // garantiert schon lange nicht mehr von der GPU in Benutzung.
        Slot toDestroy;
        bool haveToDestroy = false;
        {
            std::lock_guard<std::mutex> lock(m_slotMutex);
            m_retired.push_back(std::move(m_current));
            m_current = std::move(newSlot);
            m_hasNewFrame  = true;
            m_haveAnyFrame = true;

            if (static_cast<int>(m_retired.size()) > kRetireDepth) {
                toDestroy = std::move(m_retired.front());
                m_retired.pop_front();
                haveToDestroy = true;
            }
        }
        // Destroy bewusst AUSSERHALB des Locks, damit der Render-Thread
        // (getLatestEGLImage) nicht auf die EGL/NvBufSurface-Aufrufe
        // warten muss.
        if (haveToDestroy) {
            destroySlot(toDestroy);
        }
    }

    if (haveThreadCtx) {
        eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(m_eglDisplay, threadCtx);
    }
}

bool ArgusCamera::getLatestEGLImage(EGLImageKHR& outImage, bool& isNew) {
    if (!m_haveAnyFrame) return false;
    std::lock_guard<std::mutex> lock(m_slotMutex);
    outImage = m_current.eglImage;
    isNew = m_hasNewFrame.exchange(false);
    return true;
}