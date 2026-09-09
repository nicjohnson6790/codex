#version 450
#extension GL_GOOGLE_include_directive : require
#include "display_transfer.glsl"
layout(location=0) in vec2 fragNdc;
layout(set=2,binding=0) uniform sampler2D sceneTexture;
layout(set=3,binding=0) uniform DisplaySettings { vec4 camera; } settings;
layout(location=0) out vec4 outColor;
void main()
{
    // Viewport targets have identical extents; pixel coordinates preserve SDL's
    // framebuffer orientation without an NDC-to-texture Y convention mismatch.
    vec3 radiance = texelFetch(sceneTexture, ivec2(gl_FragCoord.xy), 0).rgb;
    outColor = vec4(displayTransfer(radiance.r, settings.camera.x),
        displayTransfer(radiance.g, settings.camera.x),
        displayTransfer(radiance.b, settings.camera.x), 1.0);
}
