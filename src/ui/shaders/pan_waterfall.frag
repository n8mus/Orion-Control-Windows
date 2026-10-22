#version 440
// The waterfall from the raw dB history (R32F, bins x rows ring). Each
// pixel column takes the MAX of its bin range — the CPU path's rule, so
// narrow signals never vanish on wide spans — then the palette.
//   p0 = (binLo, binHi, regionW, regionH)   pixels / bins
//   p1 = (head, count, rows, unused)         ring state
//   p2 = (dbMin, dbMax, unused, unused)
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Quad {
    mat4 mvp;
    vec4 p0;
    vec4 p1;
    vec4 p2;
};
layout(binding = 1) uniform sampler2D hist;
layout(binding = 2) uniform sampler2D palette;
void main() {
    int x = int(floor(v_uv.x * p0.z));
    int y = int(floor(v_uv.y * p0.w));         // 0 = newest row, at the top
    int count = int(p1.y);
    if (y >= count) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    int rows = int(p1.z);
    // Non-negative left operand: GLSL % of a negative int is undefined.
    int row = (int(p1.x) - 1 - y + 2 * rows) % rows;
    int binLo = int(p0.x), nv = int(p0.y) - binLo, w = int(p0.z);
    int b0 = binLo + (x * nv) / w;
    int b1 = binLo + ((x + 1) * nv) / w;
    if (b1 <= b0) b1 = b0 + 1;
    // Wide spans pack many bins per pixel; stride so a column costs <= 64
    // fetches (still max-of-samples, so peaks survive).
    int span = b1 - b0;
    int step = max(1, (span + 63) / 64);
    float m = -1000.0;
    for (int b = b0; b < b1; b += step)
        m = max(m, texelFetch(hist, ivec2(b, row), 0).r);
    float t = clamp((m - p2.x) / (p2.y - p2.x), 0.0, 1.0);
    fragColor = vec4(texture(palette, vec2(t, 0.5)).rgb, 1.0);
}
