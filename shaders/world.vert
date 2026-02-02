#version 450

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    vec4 tint;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_color;
layout(location = 2) in vec3 in_normal;
layout(location = 3) in vec2 in_uv;

layout(location = 0) out vec3 v_color;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec2 v_uv;
layout(location = 3) flat out float v_tex_index;

void main() {
    v_color = in_color * pc.tint.rgb;
    v_normal = in_normal;
    v_uv = in_uv;
    v_tex_index = pc.tint.a;
    gl_Position = pc.mvp * vec4(in_pos, 1.0);
}