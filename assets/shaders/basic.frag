#version 330 core
in vec3 vWorldNormal;
in vec3 vWorldPos;
in vec2 vUV;

uniform sampler2D u_texture0;
uniform vec3 u_lightDir;
uniform vec3 u_lightColor;
uniform vec3 u_ambientColor;
uniform vec4 u_color;

// Point lights (max 4 nearest to camera, set per frame)
uniform vec3 u_pointLightPos[4];
uniform vec3 u_pointLightColor[4];
uniform int  u_pointLightCount;

out vec4 FragColor;

void main() {
    vec3  N       = normalize(vWorldNormal);
    float NdotL   = max(dot(N, -u_lightDir), 0.0);
    vec3  lighting = u_ambientColor + u_lightColor * NdotL;

    // Accumulate point light contributions
    for (int i = 0; i < u_pointLightCount; i++) {
        vec3  toLight = u_pointLightPos[i] - vWorldPos;
        float dist    = length(toLight);
        float atten   = 1.0 / (1.0 + 0.3 * dist + 0.1 * dist * dist);
        float pNdotL  = max(dot(N, normalize(toLight)), 0.0);
        lighting += u_pointLightColor[i] * pNdotL * atten;
    }

    vec4 texColor = texture(u_texture0, vUV);
    FragColor = vec4(texColor.rgb * lighting * u_color.rgb, texColor.a * u_color.a);
}
