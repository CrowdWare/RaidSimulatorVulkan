#version 450

layout(location = 0) out uint outId;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    uint id;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

void main() {
    outId = pc.id;
}