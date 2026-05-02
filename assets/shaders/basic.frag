#version 330 core
in vec3 vWorldNormal;
in vec2 vUV;

uniform sampler2D u_texture0;
uniform vec3 u_lightDir;
uniform vec3 u_lightColor;
uniform vec3 u_ambientColor;
uniform vec4 u_color;

out vec4 FragColor;

void main() {
    vec3  N       = normalize(vWorldNormal);
    float NdotL   = max(dot(N, -u_lightDir), 0.0);
    vec3  lighting = u_ambientColor + u_lightColor * NdotL;
    vec4  texColor = texture(u_texture0, vUV);
    FragColor = vec4(texColor.rgb * lighting * u_color.rgb, texColor.a * u_color.a);
}
