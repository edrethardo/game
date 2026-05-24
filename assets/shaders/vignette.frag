#version 330 core
// Damage/low-HP vignette — BioShock-style radial red bleed.
// The center of the screen stays fully clear; red blooms in from the edges and,
// most strongly, the corners with a smooth organic falloff. Intensity is driven
// entirely from the CPU via u_color.a (per-hit fade + steady low-HP glow) so this
// never oscillates on its own — safe for photosensitivity (WCAG 2.3.1).
in vec2 vUV;

uniform vec4 u_color;   // rgb = tint (deep red); a = overall intensity 0..1 (from CPU)

out vec4 FragColor;

void main() {
    // Per-axis distance from screen center: 0 at center, 1 at the edges,
    // ~1.414 in the corners (which is why corners read as the most intense).
    vec2 d = abs(vUV - vec2(0.5)) * 2.0;
    float r = length(d);

    // Clear center (r < ~0.55), then a smooth ramp toward the edges/corners.
    float v = smoothstep(0.55, 1.15, r);
    v *= v;                 // square it for a tighter, edge-hugging bleed (organic, not a flat ring)

    float a = clamp(u_color.a * v, 0.0, 1.0);
    FragColor = vec4(u_color.rgb, a);
}
