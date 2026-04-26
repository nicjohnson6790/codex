#version 450

layout(set=1, binding=0) uniform CameraData
{
    mat4 viewProjection;
} camera;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inInstanceOffset;

layout(location = 0) out vec3 fragColor;

void main()
{
    vec3 position = inPosition + inInstanceOffset;
    gl_Position = camera.viewProjection * vec4(position, 1.0);
    fragColor = inColor;
}
