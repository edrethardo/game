#version 330 core
in vec3 vWorldNormal;
in vec3 vWorldPos;
in vec2 vUV;
in float vViewDepth;

uniform sampler2D u_texture0;
uniform vec3 u_lightDir;
uniform vec3 u_lightColor;
uniform vec3 u_ambientColor;
uniform vec4 u_color;

// Distance fog: lit color fades to u_fogColor between u_fogParams.x (start) and .y (end).
// Off by default (params are huge so fog≈0); the Switch build sets them to ramp into the
// clamped far plane so distant geometry fades to fog instead of hard-popping at the clip.
uniform vec3 u_fogColor;
uniform vec2 u_fogParams;

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
    float alpha = texColor.a * u_color.a;
    if (alpha < 0.05) discard;  // skip fully transparent fragments (webs, etc.)
    vec3 lit = texColor.rgb * lighting * u_color.rgb;
    float fog = clamp((vViewDepth - u_fogParams.x) / max(u_fogParams.y - u_fogParams.x, 0.001), 0.0, 1.0);
    FragColor = vec4(mix(lit, u_fogColor, fog), alpha);
}
