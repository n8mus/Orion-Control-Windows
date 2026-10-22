#version 440
// Trace / peak-hold polyline, vertices in widget pixels.
layout(location = 0) in vec2 position;
layout(std140, binding = 0) uniform Line {
    mat4 mvp;
    vec4 color;
    vec4 size;                                 // (widgetW, widgetH, unused, unused)
};
void main() {
    vec2 ndc = vec2(position.x / size.x * 2.0 - 1.0, 1.0 - position.y / size.y * 2.0);
    gl_Position = mvp * vec4(ndc, 0.0, 1.0);
}
