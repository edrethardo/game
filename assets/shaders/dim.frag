#version 330 core
// dim.frag — flat, uniform screen scrim.
//
// Exists because neither existing full-screen shader can do this: unlit.frag multiplies by a
// texture (so a flat fill would need a white one bound), and vignette.frag is deliberately RADIAL
// and keeps the screen centre clear — which is exactly where a menu's text sits. Used to darken the
// live, frozen game scene behind the in-game options screens so their thin text stays readable
// without hiding the run behind them.
uniform vec4 u_color;   // rgb = scrim tint, a = strength

out vec4 FragColor;

void main() {
    FragColor = u_color;
}
