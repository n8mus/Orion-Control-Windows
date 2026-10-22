#version 440
// A full-viewport quad; the draw's viewport places it (overlays, waterfall,
// fill). uv (0,0) = top-left of the region.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec2 v_uv;
layout(std140, binding = 0) uniform Quad {
    mat4 mvp;                                  // QRhi clip-space correction
    vec4 p0;                                   // per-shader parameters (see .frag)
    vec4 p1;
    vec4 p2;
};
void main() {
    v_uv = texcoord;
    gl_Position = mvp * vec4(position, 0.0, 1.0);
}
