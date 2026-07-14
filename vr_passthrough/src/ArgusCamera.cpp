#include "ArgusCamera.hpp"
#include <nvbufsurface.h>
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
        uint64_t frameDurationNs =
            static_cast<uint64_t>(1e9 / m_frameRateHz);
        iSourceSettings->setFrameDurationRange(
            Range<uint64_t>(frameDurationNs, frameDurationNs));
    }

    IDenoiseSettings* iDenoise = interface_cast<IDenoiseSettings>(request);
    if (iDenoise) iDenoise->setDenoiseMode(DENOISE_MODE_OFF);

    IEdgeEnhanceSettings* iEE = interface_cast<IEdgeEnhanceSettings>(request);
    if (iEE) iEE->setEdgeEnhanceMode(EDGE_ENHANCE_MODE_OFF);

    IAutoControlSettings* iAutoControl =
        interface_cast<IAutoControlSettings>(iRequest->getAutoControlSettings());
    if (iAutoControl) {
        iAutoControl->setAwbMode(AWB_MODE_AUTO);
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
    EGLContext threadCtx = createSharedThreadContext(m_eglDisplay, m_sharedEglContext);
    bool haveThreadCtx = (threadCtx != EGL_NO_CONTEXT);

    while (m_running) {
        Status status = STATUS_OK;
        UniqueObj<Frame> frame(iFrameConsumer->acquireFrame(1'000'000'000ULL /*1s Timeout*/, &status));
        if (status != STATUS_OK || !frame) {
            if (!m_running) break;
            continue;
        }

        IFrame* iFrame = interface_cast<IFrame>(frame);
        Image* image = iFrame->getImage();
        NV::IImageNativeBuffer* iNativeBuffer =
            interface_cast<NV::IImageNativeBuffer>(image);
        if (!iNativeBuffer) {
            std::cerr << "Kein IImageNativeBuffer verfuegbar\n";
            continue;
        }
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

        frame.reset();

        NvBufSurface* surf = nullptr;
        if (NvBufSurfaceFromFd(dmabufFd, reinterpret_cast<void**>(&surf)) != 0 || !surf) {
            std::cerr << "NvBufSurfaceFromFd fehlgeschlagen\n";
            continue;
        }

        if (NvBufSurfaceMapEglImage(surf, 0) != 0) {
            std::cerr << "NvBufSurfaceMapEglImage fehlgeschlagen\n";
            continue;
        }
        EGLImageKHR eglImage = surf->surfaceList[0].mappedAddr.eglImage;

        Slot newSlot;
        newSlot.bufSurface = surf;
        newSlot.eglImage   = eglImage;
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