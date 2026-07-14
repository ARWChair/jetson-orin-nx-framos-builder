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

void main() {
    vec2 outPix = vOutUV * uOutputSize;
    vec2 centered = outPix - uOutputSize * 0.5;

    float focOut = (uOutputSize.y * 0.5) / tan(uVFovRad * 0.5);

    vec3 dir = normalize(vec3(centered.x, -centered.y, focOut));

    vec3 dirRot = normalize(uRot * dir);

    float theta = acos(clamp(dirRot.z, -1.0, 1.0));
    float phi   = atan(dirRot.y, dirRot.x);

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
