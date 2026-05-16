#version 450
#extension GL_ARB_shader_draw_parameters : require

layout(set=1, binding=0) uniform CameraData
{
    mat4 viewProjection;
} camera;

struct TextGroup
{
    vec4 origin;
    vec4 right;
    vec4 up;
    vec4 drawBounds;
    uvec4 glyphRangeAndStyle;
};

layout(set=0, binding=0, std430) readonly buffer TextGroupBuffer
{
    TextGroup groups[];
} textGroupBuffer;

layout(location = 0) in vec2 inCorner;
layout(location = 0) out vec2 fragLocalFontPosition;
layout(location = 1) flat out uint fragGroupIndex;

void main()
{
    uint groupIndex = gl_DrawIDARB;
    TextGroup group = textGroupBuffer.groups[groupIndex];
    vec2 localPosition = mix(group.drawBounds.xy, group.drawBounds.zw, inCorner);

    vec3 worldPosition =
        group.origin.xyz +
        (group.right.xyz * localPosition.x) +
        (group.up.xyz * localPosition.y);

    gl_Position = camera.viewProjection * vec4(worldPosition, 1.0);
    fragLocalFontPosition = localPosition;
    fragGroupIndex = groupIndex;
}
