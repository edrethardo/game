#version 330 core
in vec4 vColor;
in vec2 vUV;

uniform sampler2D u_texture0;

out vec4 FragColor;

void main() {
    vec4 texColor = texture(u_texture0, vUV);
    FragColor = texColor * vColor;
}
