#version 450
#extension GL_GOOGLE_include_directive : require
#include "display_transfer.glsl"
layout(location=0) in vec2 fragNdc;
layout(set=2,binding=0) uniform sampler2D sceneTexture;
layout(set=2,binding=1,std430) readonly buffer Exposure { float value; } adapted;
layout(set=3,binding=0) uniform DisplaySettings { vec4 camera; } settings;
layout(location=0) out vec4 outColor;
void main()
{
    // Viewport targets have identical extents; pixel coordinates preserve SDL's
    // framebuffer orientation without an NDC-to-texture Y convention mismatch.
    vec3 radiance = texelFetch(sceneTexture, ivec2(gl_FragCoord.xy), 0).rgb;
    float exposure=settings.camera.y>0.5 ? adapted.value : settings.camera.x;
    outColor = vec4(displayTransfer(radiance.r, exposure),
        displayTransfer(radiance.g, exposure), displayTransfer(radiance.b, exposure), 1.0);
}
