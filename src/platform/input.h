#pragma once

#include "core/types.h"

union SDL_Event;

namespace Input {
    void init();
    void shutdown();
    void update(); // Call once per frame after pollEvents

    // Keyboard
    bool isKeyDown(s32 scancode);
    bool isKeyPressed(s32 scancode);    // True only the frame it was pressed
    bool isKeyReleased(s32 scancode);   // True only the frame it was released

    // Mouse
    void getMouseDelta(s32& dx, s32& dy);
    void getMousePosition(s32& x, s32& y);
    bool isMouseButtonDown(u8 button);
    bool isMouseButtonPressed(u8 button);
    bool isMouseButtonReleased(u8 button); // True only the frame it was released
    void setRelativeMouseMode(bool enabled);

    // Gamepad
    static constexpr s32 MAX_GAMEPADS = 4;
    f32  getAxis(s32 gamepadIndex, s32 axis);
    bool isButtonDown(s32 gamepadIndex, s32 button);
    bool isGamepadConnected(s32 gamepadIndex);

    // Called by Window::pollEvents for controller hotplug
    void handleControllerEvent(const SDL_Event& event);
}
