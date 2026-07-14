#pragma once

// Ein simples Fullscreen-Quad (2 Dreiecke), Positionen in NDC [-1,1],
// UVs in [0,1] fuer den Output-Raum (nicht den Input!).
static const char* kVertexShaderSrc = R"GLSL(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vOutUV;
void main() {
    vOutUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";

// Fisheye-(equidistant)-zu-rektilinear-Dewarp, analog zu nvdewarper
// projection-type=2 (Perspective). Pro Ausgabepixel wird ein Strahl der
// virtuellen Lochkamera erzeugt, per Rotationsmatrix (pitch/yaw/roll)
// gedreht und dann per Fisheye-Modell (r = f * theta) auf das Original-
// Sensorbild zurueckprojiziert.
//
// Sampler ist samplerExternalOES, weil das EGLImage direkt aus dem
// NV12-dmabuf der Kamera erzeugt wird -- die YUV->RGB Konvertierung
// passiert dabei implizit in der Textur-Sample-Hardware (kein extra
// Konvertierungsschritt, kein Extra-Copy).
static const char* kDewarpFragmentShaderSrc = R"GLSL(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;

in vec2 vOutUV;
out vec4 fragColor;

uniform samplerExternalOES uTex;
uniform vec2  uInputSize;     // z.B. 2064x1552 (volles Sensorbild)
uniform vec2  uOutputSize;    // z.B. 1552x1552
uniform float uFocalLength;   // Fisheye-Brennweite in Pixeln
uniform float uVFovRad;       // vertikales FOV der virtuellen Ausgabekamera
uniform mat3  uRot;           // pitch/yaw/roll Rotation

void main() {
    // Ausgabepixel-Koordinate, zentriert um den Hauptpunkt
    vec2 outPix = vOutUV * uOutputSize;
    vec2 centered = outPix - uOutputSize * 0.5;

    // Virtuelle Lochkamera-Brennweite aus dem gewuenschten vertikalen FOV
    float focOut = (uOutputSize.y * 0.5) / tan(uVFovRad * 0.5);

    // Strahlrichtung der virtuellen Ausgabekamera (Rechtssystem, +z nach vorn)
    vec3 dir = normalize(vec3(centered.x, -centered.y, focOut));

    // In die Sensor-Orientierung rotieren
    vec3 dirRot = normalize(uRot * dir);

    // In Kugelkoordinaten relativ zur optischen Achse
    float theta = acos(clamp(dirRot.z, -1.0, 1.0));
    float phi   = atan(dirRot.y, dirRot.x);

    // Equidistantes Fisheye-Modell: r = f * theta
    float r = uFocalLength * theta;

    vec2 inputCenter = uInputSize * 0.5;
    vec2 srcPix = inputCenter + r * vec2(cos(phi), -sin(phi));
    vec2 srcUV  = srcPix / uInputSize;

    if (srcUV.x < 0.0 || srcUV.x > 1.0 || srcUV.y < 0.0 || srcUV.y > 1.0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    fragColor = texture(uTex, srcUV);
}
)GLSL";
