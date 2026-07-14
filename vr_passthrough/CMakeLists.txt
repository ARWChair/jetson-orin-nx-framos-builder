cmake_minimum_required(VERSION 3.16)
project(imx900_vr_passthrough CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Anpassen falls dein JetPack/L4T diese Pfade anders legt.
# Standard für JetPack 6.x (L4T R36):
#   Argus-Header:        /usr/src/jetson_multimedia_api/argus/include
#   Multimedia-Header:    /usr/src/jetson_multimedia_api/include
#   Tegra-Libs:            /usr/lib/aarch64-linux-gnu/tegra
# ---------------------------------------------------------------------------
set(JETSON_MM_API "/usr/src/jetson_multimedia_api" CACHE PATH "Pfad zur jetson_multimedia_api")
set(TEGRA_LIB_DIR "/usr/lib/aarch64-linux-gnu/tegra" CACHE PATH "Pfad zu den Tegra-Libs")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED sdl3)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES2 REQUIRED glesv2)

add_executable(vr_passthrough
    src/main.cpp
    src/ArgusCamera.cpp
    src/GLExtensions.cpp
)

target_include_directories(vr_passthrough PRIVATE
    ${JETSON_MM_API}/argus/include
    ${JETSON_MM_API}/include
    ${SDL3_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    src
)

target_link_directories(vr_passthrough PRIVATE ${TEGRA_LIB_DIR})

target_link_libraries(vr_passthrough PRIVATE
    ${SDL3_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
    nvargus_socketclient   # libargus IPC client
    nvbufsurface           # NvBufSurface API (JetPack 5.1+/6.x)
    nvbufsurftransform
    pthread
)

target_compile_options(vr_passthrough PRIVATE -O3 -Wall)
