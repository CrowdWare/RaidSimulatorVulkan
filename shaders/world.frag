#version 450

layout(location = 0) in vec3 v_color;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) flat in float v_tex_index;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_ground;
layout(set = 0, binding = 1) uniform sampler2D u_blocks[8];

void main() {
    vec3 n = normalize(v_normal);
    vec3 light_dir = normalize(vec3(0.2, 1.0, 0.1));
    float ndotl = max(dot(n, light_dir), 0.0);
    float ambient = 0.65;
    vec3 sunlight = vec3(1.0, 0.95, 0.85) * ndotl * 0.75;
    vec3 base = v_color;
    if (v_tex_index < -1.5) {
        // no texture, vertex color only
    } else if (v_tex_index < -0.5) {
        base *= texture(u_ground, v_uv).rgb;
    } else {
        vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
        vec3 an = abs(n);
        if (an.x > an.y && an.x > an.z) {
            if (n.x > 0.0)
                uv.x = 1.0 - uv.x;
        } else if (an.z > an.x && an.z > an.y) {
            if (n.z < 0.0)
                uv.x = 1.0 - uv.x;
        }
        int idx = int(v_tex_index + 0.5);
        idx = clamp(idx, 0, 7);
        base *= texture(u_blocks[idx], uv).rgb;
    }
    vec3 lit = base * (ambient + sunlight);
    out_color = vec4(lit, 1.0);
}