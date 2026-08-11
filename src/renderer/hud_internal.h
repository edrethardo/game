#pragma once
// hud_internal.h — shared GL batch primitives for HUD split files.
// These functions are defined in hud.cpp (promoted from static to non-static)
// and declared here so the split translation units can call them without
// duplicating the batch state (s_verts, s_vertCount, s_vao, etc.).

#include "core/types.h"
#include "core/math.h"

// Push a 2D line into the current HUD vertex batch.
void pushLine(f32 x0, f32 y0, f32 x1, f32 y1, Vec3 color);

// Push a 2D quad outline (4 lines) into the current HUD vertex batch.
void pushQuad(f32 x0, f32 y0, f32 x1, f32 y1, Vec3 color);

// Fill an axis-aligned rect by scanlines. Every panel in the HUD was hand-rolling this same
// `for (f32 y = y0; y < y1; y += 1.0f) pushLine(...)` loop — six copies, one of which walked the
// rect in the wrong direction on a flipped input and drew nothing. Rects are normalised here, so
// a caller may pass its corners in either order.
void fillRect(f32 x0, f32 y0, f32 x1, f32 y1, Vec3 color);

// Upload and draw all batched HUD lines, then reset the batch.
void flushHUD();
