#version 440
// A cached QPainter layer (premultiplied RGBA), composited as-is.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D tex;
void main() { fragColor = texture(tex, v_uv); }
