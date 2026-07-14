#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include "GLExtensions.hpp"

#include <Argus/Argus.h>
#include <EGLStream/EGLStream.h>
#include <EGLStream/NV/ImageNativeBuffer.h>

#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <deque>

class ArgusCamera {
public:
    ArgusCamera(Argus::ICameraProvider* iCameraProvider,
                Argus::CameraDevice* cameraDevice,
                EGLDisplay eglDisplay,
                EGLContext sharedEglContext,
                uint32_t width, uint32_t height,
                uint32_t sensorModeIndex,
                float frameRateHz);
    ~ArgusCamera();

    ArgusCamera(const ArgusCamera&) = delete;
    ArgusCamera& operator=(const ArgusCamera&) = delete;

    bool start();
    void stop();
    bool getLatestEGLImage(EGLImageKHR& outImage, bool& isNew);

    uint32_t width()  const { return m_width; }
    uint32_t height() const { return m_height; }

private:
    static constexpr int kRetireDepth = 3;
    struct Slot {
        void*        bufSurface = nullptr;
        EGLImageKHR  eglImage   = EGL_NO_IMAGE_KHR;
    };

    void captureThreadFunc();
    void destroySlot(Slot& slot);
    void drainAll();

    Argus::ICameraProvider* m_iCameraProvider;
    Argus::CameraDevice*    m_cameraDevice;
    EGLDisplay               m_eglDisplay;
    EGLContext               m_sharedEglContext;

    uint32_t m_width, m_height;
    uint32_t m_sensorModeIndex;
    float    m_frameRateHz;

    Argus::UniqueObj<Argus::CaptureSession>   m_captureSession;
    Argus::UniqueObj<Argus::OutputStream>     m_outputStream;
    Argus::UniqueObj<EGLStream::FrameConsumer> m_frameConsumer;

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::mutex               m_slotMutex;
    Slot                     m_current;
    std::deque<Slot>         m_retired;
    std::atomic<bool>        m_hasNewFrame{false};
    std::atomic<bool>        m_haveAnyFrame{false};
};