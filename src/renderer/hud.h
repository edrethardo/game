#pragma once

#include "core/types.h"
#include "core/math.h"

namespace HUD {
    void init();
    void shutdown();

    // Render crosshair at screen centre. Call after 3D rendering, before swap.
    void drawCrosshair(u32 screenWidth, u32 screenHeight, Vec3 color);

    // Render hit marker (X shape) at screen centre. alpha fades out.
    void drawHitMarker(u32 screenWidth, u32 screenHeight, f32 alpha);

    // Render a simple health bar at bottom-left.
    void drawHealthBar(u32 screenWidth, u32 screenHeight,
                       f32 health, f32 maxHealth);

    // Render weapon name text indicator (just a colored bar placeholder).
    void drawWeaponIndicator(u32 screenWidth, u32 screenHeight, u8 weaponSlot);

    // Menu elements
    void drawMenuOption(u32 screenWidth, u32 screenHeight,
                        f32 y, f32 width, f32 height,
                        Vec3 color, bool selected);

    // Network stats overlay
    void drawNetStats(u32 screenWidth, u32 screenHeight,
                      u32 playerCount, u32 ping, const char* role);
}
