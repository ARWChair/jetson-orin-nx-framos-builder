#pragma once
// Kapselt eine einzelne libargus-CaptureSession fuer einen Sensor und
// liefert kontinuierlich EGLImageKHR-Handles auf die neuesten Frames.
//
// Latenz-Design:
//  - EGLStream im MAILBOX-Modus  -> Argus haelt nur den jeweils neuesten
//    Frame vor, aeltere werden verworfen statt gequeued (kein FIFO-Stau
//    wie bei den GStreamer-Queues).
//  - Ein eigener Thread pro Kamera ruft blockierend acquireFrame() auf
//    und schreibt das Ergebnis (EGLImage + Lifetime-Handle) in einen
//    mutex-geschuetzten "letzter Frame"-Slot. Der Render-Thread liest
//    nur diesen Slot, nie eine Queue -> es wird nie auf alte Frames
//    gewartet, im schlechtesten Fall wird ein Frame doppelt gerendert.
//
// Buffer-Lifetime:
//  - WICHTIG: Ein dmabuf/EGLImage darf nicht zerstoert werden, solange
//    die GPU noch daraus liest. glEGLImageTargetTexture2DOES() bindet
//    das Image nur asynchron -- der eigentliche Sample-Zugriff der GPU
//    passiert erst spaeter (beim Draw-Call bzw. Swap), moeglicherweise
//    lange NACHDEM der Capture-Thread (auf der CPU) schon laengst
//    weitergelaufen ist. Wuerde man den alten Buffer sofort nach dem
//    Ersetzen freigeben (wie in der ersten Version), rennt der
//    Capture-Thread der GPU davon und der Buffer wird unter der GPU
//    weggerissen -> schwarze/kaputte Texturen.
//    Deshalb: alte Slots kommen erst in eine kleine Retire-Queue und
//    werden erst zerstoert, nachdem sie von kRetireDepth neueren Frames
//    ueberholt wurden (bei 72fps und kRetireDepth=3 sind das >~40ms
//    Vorlauf -- mehr als genug fuer die GPU, um fertig zu sampeln).
//
// EGL-Context auf dem Capture-Thread:
//  - NvBufSurfaceMapEglImage() (erzeugt die eigentliche EGLImageKHR aus
//    dem dmabuf) laeuft im Capture-Thread. Ohne einen dort AKTIVEN
//    EGL-Context registriert der Tegra-Treiber das EGLImage offenbar
//    nicht vollstaendig -- glEGLImageTargetTexture2DOES() auf dem
//    GL-Thread liefert dann zwar keinen Fehler, aber texture() sampelt
//    trotzdem konstant (0,0,0) (per glReadPixels UND CPU-Rohbild-Dump
//    bestaetigt: die Kameradaten selbst sind da, nur die GPU-Textur
//    bleibt schwarz). Deshalb erzeugt der Capture-Thread sich beim
//    Start einen eigenen, mit dem Haupt-GL-Context GESHARETEN,
//    surfaceless EGL-Context und macht ihn fuer die Dauer des Threads
//    aktiv.

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

    // Startet den Capture-Thread (repeat-Request + FrameConsumer-Loop).
    bool start();
    void stop();

    // Liefert true, wenn seit dem letzten Aufruf ein neues Bild da war,
    // und schreibt das aktuelle EGLImage nach outImage.
    // Gibt auch dann ein gueltiges Bild zurueck, wenn es nicht "neu" ist
    // (Wiederverwendung des letzten Frames), solange schon einmal ein
    // Frame angekommen ist.
    bool getLatestEGLImage(EGLImageKHR& outImage, bool& isNew);

    uint32_t width()  const { return m_width; }
    uint32_t height() const { return m_height; }

private:
    // Wie viele "Generationen" ein Slot ueberleben muss, bevor er
    // wirklich zerstoert wird. 3 = grosszuegiger Sicherheitsabstand.
    static constexpr int kRetireDepth = 3;

    // WICHTIG: Das originale Argus::Frame wird hier bewusst NICHT
    // gehalten. createNvBuffer() (siehe .cpp) erzeugt bereits eine
    // vollstaendig unabhaengige Kopie der Bilddaten in einem neuen
    // dmabuf -- das Argus-Frame-Objekt wird direkt danach nicht mehr
    // gebraucht und sofort an Argus zurueckgegeben. Wuerde man es
    // zusaetzlich in der Retire-Queue (s.u.) laenger festhalten,
    // blockiert man Argus' eigenen internen Stream-Buffer-Pool, bis der
    // irgendwann leerlaeuft und die RPC-Verbindung zum nvargus-daemon
    // abreisst ("Unexpected error in reading socket").
    //
    // Slot besteht dadurch nur noch aus trivialen Typen (Pointer/Handle)
    // -> Default-Move-Konstruktor/-Assignment funktionieren ohne
    // Handschriftliches Gefrickel.
    struct Slot {
        void*        bufSurface = nullptr;          // NvBufSurface*
        EGLImageKHR  eglImage   = EGL_NO_IMAGE_KHR;
    };

    void captureThreadFunc();
    void destroySlot(Slot& slot);   // gibt eine einzelne Slot-Ressource frei
    void drainAll();                // beim Stop/Destruktor: alles freigeben

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

    // "Letzter Frame"-Slot + Retire-Queue fuer verzoegertes Freigeben
    std::mutex               m_slotMutex;
    Slot                     m_current;
    std::deque<Slot>         m_retired;      // aelteste zuerst
    std::atomic<bool>        m_hasNewFrame{false};
    std::atomic<bool>        m_haveAnyFrame{false};
};