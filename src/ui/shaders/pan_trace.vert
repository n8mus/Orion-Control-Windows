#version 440
// Wide anti-aliased trace: one quad per segment (triangles), vertices in
// widget pixels, `edge` = -1..1 across the line for the coverage ramp.
layout(location = 0) in vec3 position;         // (x, y, edge)
layout(location = 0) out float v_edge;
layout(location = 1) out float v_t;
layout(std140, binding = 0) uniform Line {
    mat4 mvp;
    vec4 color;
    vec4 size;                                 // (widgetW, widgetH, heat?, spectrumH)
};
void main() {
    vec2 ndc = vec2(position.x / size.x * 2.0 - 1.0, 1.0 - position.y / size.y * 2.0);
    gl_Position = mvp * vec4(ndc, 0.0, 1.0);
    v_edge = position.z;
    v_t = 1.0 - position.y / max(size.w, 1.0);
}
