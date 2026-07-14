# IMX900 VR Passthrough (libargus → NvBufSurface → EGLImage → SDL3/GLES)

Ersetzt die GStreamer-Pipeline (`nvarguscamerasrc` × 2 → `nvvidconv` →
`nvstreammux` → `nvmultistreamtiler` → `nv3dsink`) durch ein natives
C++-Programm, um die durch GStreamer-Queues, `nvstreammux`-Batching und
`nvmultistreamtiler` verursachte Latenz zu eliminieren.

## Architektur / warum das schneller ist

| GStreamer-Pipeline | dieses Programm |
|---|---|
| `nvarguscamerasrc` (intern FIFO) | libargus **EGLStream im MAILBOX-Modus** – alte, unkonsumierte Frames werden verworfen statt gequeued |
| `nvstreammux` batched, `batched-push-timeout=2000` | kein Batching, jede Kamera läuft in eigenem Thread, Render-Loop liest immer nur den *aktuellsten* Frame |
| `nvvidconv` (Crop, separater HW-Task + Sync) | entfällt, Crop/Dewarp passiert direkt im Fragment-Shader beim Sampling |
| `nv3dsink` | direktes GLES3-Rendering in ein SDL3-Fenster, `SDL_GL_SetSwapInterval(0)` |
| kein Dewarping in der gezeigten Pipeline (nur Crop) | vollständiges Fisheye→rektilinear-Dewarping analog zur `config_dewarper.txt`, im Shader |

Der Bildweg ist durchgehend **zero-copy**: ISP schreibt in einen dmabuf,
der als `NvBufSurface` direkt als `EGLImageKHR` gemappt und per
`GL_TEXTURE_EXTERNAL_OES` von der GPU gesampelt wird (inkl. impliziter
NV12→RGB-Konvertierung durch die Textureinheit). Es gibt keinen
CPU-Memcopy und keinen zusätzlichen Konvertierungs-Buffer.

## Annahmen (bitte prüfen / anpassen)

- **JetPack 6.x / L4T R36** mit der aktuellen `NvBufSurface`-API
  (`nvbufsurface.h`, `NvBufSurfaceFromFd`, `NvBufSurfaceMapEglImage`,
  `NVBUF_COLOR_FORMAT_NV12`, `NVBUF_LAYOUT_PITCH`). Falls du noch auf
  JetPack ≤5.0 bist, nutzt `NV::IImageNativeBuffer::createNvBuffer`
  stattdessen die alten Enums aus `nvbuf_utils.h`
  (`NvBufferColorFormat_NV12`, `NvBufferLayout_Pitch`) und das Mapping
  läuft über `NvEGLImageFromFd()` statt `NvBufSurfaceMapEglImage()`.
  Beides ist in `src/ArgusCamera.cpp` klar markiert und leicht
  austauschbar.
- **sensor-mode=0** für beide Sensoren (wie im Original), 72 fps,
  Auto-Weißabgleich, TNR/EE aus – identisch zu deinen bisherigen
  `nvarguscamerasrc`-Properties.
- Beide Sensoren sind baugleich montiert, daher wird **eine** gemeinsame
  `config_dewarper.txt` für beide Augen genutzt. Falls IPD/Konvergenz
  pro Auge unterschiedliche `yaw`/`pitch` brauchen, einfach zwei
  Config-Dateien anlegen und `main.cpp` entsprechend erweitern (zwei
  `DewarpConfig`-Instanzen statt einer).
- Der Dewarp implementiert `projection-type=2` (Perspective/rektilinear)
  mit dem **equidistanten** Fisheye-Modell (`r = f · θ`), was dem
  Standardverhalten von `nvdewarper` für IMX-Weitwinkelsensoren
  entspricht. Falls deine Linse eher equisolid-angle ist, in
  `Shaders.hpp` die Zeile `float r = uFocalLength * theta;` z. B. zu
  `2.0 * uFocalLength * tan(theta * 0.5)` ändern.

## Bauen

Voraussetzungen auf dem Jetson:

```bash
sudo apt install libegl1-mesa-dev libgles2-mesa-dev pkg-config
# SDL3 ist auf den meisten L4T-Repos noch nicht als Paket verfügbar,
# ggf. aus dem Quellcode bauen: https://github.com/libsdl-org/SDL
```

```bash
mkdir build && cd build
cmake .. -DJETSON_MM_API=/usr/src/jetson_multimedia_api \
         -DTEGRA_LIB_DIR=/usr/lib/aarch64-linux-gnu/tegra
make -j$(nproc)
```

Falls `nvbufsurface`/`nvargus_socketclient` beim Linken nicht gefunden
werden: `find / -iname "*nvbufsurface*"` bzw. `*nvargus*` ausführen und
`TEGRA_LIB_DIR` entsprechend anpassen (variiert leicht zwischen
JetPack-Minor-Versionen).

## Ausführen

```bash
./vr_passthrough ../config/config_dewarper.txt
```

Öffnet ein 3104x1552-Fenster mit beiden entzerrten Kamerabildern
nebeneinander. ESC oder Ctrl-C zum Beenden.

## Weitere Latenz-Stellschrauben

- `SDL_GL_SetSwapInterval(0)` in `main.cpp` ist bereits auf minimale
  Latenz gestellt (kein VSync-Wait). Falls Tearing stört: auf `1`
  setzen (kostet bis zu einem Frame ≈ 13,9 ms bei 72 Hz).
- Aktuell wird `sensor-mode=0` 1:1 übernommen – falls es einen Sensor-
  Mode mit kürzerer Belichtungs-/Ausleseregelung gibt, kann das die
  ISP-interne Latenz weiter senken.
- Für noch mehr Kontrolle über die ISP-Pipeline-Latenz lohnt sich ein
  Blick auf `AutoControlSettings::setAeAntibandingMode` /
  `setAeLock` – feste statt Auto-Belichtung reduziert AE-Regelzeiten,
  ist aber für VR-Passthrough meist nicht nötig.
- Eine denkbare weitere Beschleunigung wäre, den Dewarp per CUDA/NPP
  statt per GLES-Fragment-Shader zu rechnen (Interop über dasselbe
  EGLImage) — bringt aber auf der iGPU des Orin NX in der Praxis kaum
  einen Vorteil gegenüber dem Shader-Ansatz hier, da beides auf
  derselben GPU läuft; der Shader-Weg ist deutlich einfacher zu warten.

## Bekannte Einschränkungen dieser ersten Version

- Kein hardware-synchronisiertes Stereo-Capture (jede Kamera läuft
  frei-laufend in eigenem Thread/eigener Session). Falls deine FRAMOS
  IMX900-Module einen Hardware-Sync-Pin/Genlock unterstützen und ihr
  Frame-exaktes Stereo braucht, müssten beide Sensoren in **einer**
  `CaptureSession` mit synchronisierten Requests laufen (siehe
  NVIDIA-Sample `09_camera_syncsensor`) – aktuell reicht "beide laufen
  mit 72 fps, Render nimmt jeweils neuesten Frame" für Passthrough i.d.R.
  völlig aus.
- Die dmabuf-Buffer-Lebensdauer wird per Mutex statt EGL-Fences
  synchronisiert (siehe Kommentar in `ArgusCamera.cpp`). Für den
  Passthrough-Anwendungsfall unkritisch, für harte Echtzeitgarantien
  könnte man das noch mit `EGLSyncKHR` absichern.
