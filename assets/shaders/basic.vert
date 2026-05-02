#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 u_mvp;
uniform mat4 u_model;

out vec3 vWorldNormal;
out vec2 vUV;

void main() {
    gl_Position  = u_mvp * vec4(aPos, 1.0);
    vWorldNormal = mat3(u_model) * aNormal;
    vUV          = aUV;
}
