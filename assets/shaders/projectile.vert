#version 330 core

// Per-vertex attributes (from mesh VBO)
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// Per-instance attributes (from instance VBO, divisor=1)
layout(location = 3) in vec4 iModelRow0;
layout(location = 4) in vec4 iModelRow1;
layout(location = 5) in vec4 iModelRow2;
layout(location = 6) in vec4 iModelRow3;
layout(location = 7) in vec4 iColor;

uniform mat4 u_vp;        // View-Projection (shared across all instances)
uniform vec4 u_groupTint; // per-material-group tint (1,1,1,1 = none); multiplies instance color

out vec3 vWorldNormal;
out vec3 vWorldPos;
out vec2 vUV;
out vec4 vColor;

void main() {
    mat4 model = mat4(iModelRow0, iModelRow1, iModelRow2, iModelRow3);
    vec4 worldPos = model * vec4(aPos, 1.0);
    gl_Position  = u_vp * worldPos;
    vWorldNormal = mat3(model) * aNormal;
    vWorldPos    = worldPos.xyz;
    vUV          = aUV;
    vColor       = iColor * u_groupTint;
}
