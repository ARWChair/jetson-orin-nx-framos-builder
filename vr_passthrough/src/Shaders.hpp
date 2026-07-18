#pragma once

static const char* kVertexShaderSrc = R"GLSL(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
out vec2 vOutUV;
void main() {
    vOutUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)GLSL";


static const char* kDewarpFragmentShaderSrc = R"GLSL(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;

in vec2 vOutUV;
out vec4 fragColor;

uniform samplerExternalOES uTex;
uniform vec2  uInputSize;
uniform vec2  uOutputSize;
uniform float uFocalLength;
uniform float uVFovRad;
uniform mat3  uRot;
uniform float k1;
uniform float k2;
uniform bool  uShowGrid;
uniform float uGridSpacing;

void main() {
    vec2 uv = vOutUV * 2.0 - 1.0;
    uv.x *= uOutputSize.x / uOutputSize.y;

    float r2 = dot(uv, uv);
    float distortion = 1.0 + k1 * r2 + k2 * r2 * r2;
    vec2 uvDistorted = uv * distortion;

    uvDistorted.x /= (uOutputSize.x / uOutputSize.y);
    vec2 centered = uvDistorted * (uOutputSize * 0.5);

    float focOut = (uOutputSize.y * 0.5) / tan(uVFovRad * 0.5);

    vec3 dir = normalize(vec3(centered.x, -centered.y, focOut));
    vec3 dirRot = normalize(uRot * dir);

    float theta = acos(clamp(dirRot.z, -1.0, 1.0));
    float phi   = atan(dirRot.y, dirRot.x);

    float r = uFocalLength * theta;

    vec2 inputCenter = uInputSize * 0.5;
    vec2 srcPix = inputCenter + r * vec2(cos(phi), -sin(phi));
    vec2 srcUV  = srcPix / uInputSize;

    vec4 color;
    if (srcUV.x < 0.0 || srcUV.x > 1.0 || srcUV.y < 0.0 || srcUV.y > 1.0) {
        color = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        color = texture(uTex, srcUV);
    }

    // 3) Gitter-Overlay in Ausgabekoordinaten (vOutUV), damit du siehst
    //    ob die Linien durch die Linse betrachtet gerade wirken
    if (uShowGrid) {
        vec2 gridCoord = vOutUV / uGridSpacing;
        vec2 gridFrac = abs(fract(gridCoord - 0.5) - 0.5) / fwidth(gridCoord);
        float lineIntensity = 1.0 - min(min(gridFrac.x, gridFrac.y), 1.0);
        vec3 gridColor = vec3(0.0, 1.0, 0.0); // gruen
        color.rgb = mix(color.rgb, gridColor, lineIntensity);
    }

    fragColor = color;
}
)GLSL";
