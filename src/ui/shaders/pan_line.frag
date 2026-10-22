#version 440
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Line {
    mat4 mvp;
    vec4 color;                                // premultiplied
    vec4 size;
};
void main() { fragColor = color; }
