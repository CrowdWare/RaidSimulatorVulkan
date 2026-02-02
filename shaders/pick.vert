#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    uint id;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUv;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
}