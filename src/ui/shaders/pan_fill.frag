#version 440
// Fill under the trace, transparent above it.
//   p0 = (regionW, regionH, gamma, style)
// style 0: KE9NS — colour keyed to the dB AXIS (bands run horizontally),
//          gamma-compressed.
// style 1: AetherSDR heat — per column, the heat colour of its level at the
//          trace (alpha 0.7 x 0.3) fading to dark blue (0,0,77) at alpha 0.7.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Quad {
    mat4 mvp;
    vec4 p0;
    vec4 p1;
    vec4 p2;
};
layout(binding = 1) uniform sampler2D cols;    // R32F, one 0..1 level per pixel column
layout(binding = 2) uniform sampler2D palette;
vec3 heat(float t) {
    t = clamp(t, 0.0, 1.0);
    if (t < 0.25) return vec3(0.0, t / 0.25, 1.0);
    if (t < 0.5)  return vec3(0.0, 1.0, 1.0 - (t - 0.25) / 0.25);
    if (t < 0.75) return vec3((t - 0.5) / 0.25, 1.0, 0.0);
    return vec3(1.0, 1.0 - (t - 0.75) / 0.25, 0.0);
}
void main() {
    int x = int(floor(v_uv.x * p0.x));
    float h = p0.y;
    float y = floor(v_uv.y * h);
    float level = texelFetch(cols, ivec2(x, 0), 0).r;
    float colY = floor(h * (1.0 - level));
    if (y < colY) { fragColor = vec4(0.0); return; }
    if (p0.w > 0.5) {
        float f = (y - colY) / max(1.0, h - colY);
        float a = mix(0.7 * 0.3, 0.7, f);
        vec3 c = mix(heat(level), vec3(0.0, 0.0, 77.0 / 255.0), f);
        fragColor = vec4(c * a, a);
        return;
    }
    float t = 1.0 - y / max(h - 1.0, 1.0);
    fragColor = vec4(texture(palette, vec2(pow(t, p0.z), 0.5)).rgb, 1.0);
}
