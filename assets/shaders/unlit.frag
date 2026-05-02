#version 330 core
in vec2 vUV;

uniform sampler2D u_texture0;
uniform vec4 u_color;

out vec4 FragColor;

void main() {
    vec4 texColor = texture(u_texture0, vUV);
    FragColor = texColor * u_color;
}
