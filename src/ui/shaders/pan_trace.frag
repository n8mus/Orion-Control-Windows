#version 440
// AetherSDR spectrum.frag coverage; colour = the trace colour, or the heat
// ramp by height (heat style).
layout(location = 0) in float v_edge;
layout(location = 1) in float v_t;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Line {
    mat4 mvp;
    vec4 color;                                // premultiplied
    vec4 size;
};
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    if (t < 0.25) return vec3(0.0, t / 0.25, 1.0);
    if (t < 0.5)  return vec3(0.0, 1.0, 1.0 - (t - 0.25) / 0.25);
    if (t < 0.75) return vec3((t - 0.5) / 0.25, 1.0, 0.0);
    return vec3(1.0, 1.0 - (t - 0.75) / 0.25, 0.0);
}
void main() {
    float edge = abs(v_edge);
    float aa = max(fwidth(v_edge), 0.03);
    float cov = 1.0 - smoothstep(1.0 - aa, 1.0, edge);
    vec4 c = size.z > 0.5 ? vec4(heat(v_t), 1.0) : color;
    fragColor = c * cov;
}
