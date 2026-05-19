#version 330 core
// Batched particle shader — vertices are pre-transformed to world space,
// per-vertex color replaces the per-object u_color uniform.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;

uniform mat4 u_vp;

out vec4 vColor;
out vec2 vUV;

void main() {
    gl_Position = u_vp * vec4(aPos, 1.0);
    vColor = aColor;
    vUV = aUV;
}
