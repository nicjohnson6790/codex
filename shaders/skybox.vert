#version 450

layout(location = 0) in vec2 inPosition;

layout(location = 0) out vec2 fragNdc;

void main()
{
    fragNdc = inPosition;
    gl_Position = vec4(inPosition, 0.0, 1.0);
}
