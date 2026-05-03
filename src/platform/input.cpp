#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "platform/input.h"
#include "core/log.h"

#include <cstring>

static constexpr s32 NUM_KEYS = SDL_NUM_SCANCODES;
static constexpr s32 NUM_MOUSE_BUTTONS = 5;

static u8 s_currentKeys[NUM_KEYS];
static u8 s_previousKeys[NUM_KEYS];

static u8 s_currentMouseButtons[NUM_MOUSE_BUTTONS];
static u8 s_previousMouseButtons[NUM_MOUSE_BUTTONS];
static s32 s_mouseDX = 0;
static s32 s_mouseDY = 0;
static s32 s_mouseX = 0;
static s32 s_mouseY = 0;
static s32 s_mouseWheelY = 0; // accumulated wheel delta this frame

static SDL_GameController* s_controllers[Input::MAX_GAMEPADS] = {};

void Input::init() {
    memset(s_currentKeys, 0, sizeof(s_currentKeys));
    memset(s_previousKeys, 0, sizeof(s_previousKeys));
    memset(s_currentMouseButtons, 0, sizeof(s_currentMouseButtons));
    memset(s_previousMouseButtons, 0, sizeof(s_previousMouseButtons));
    s_mouseDX = 0;
    s_mouseDY = 0;

    // Open any controllers already connected
    for (s32 i = 0; i < SDL_NumJoysticks() && i < MAX_GAMEPADS; ++i) {
        if (SDL_IsGameController(i)) {
            s_controllers[i] = SDL_GameControllerOpen(i);
            if (s_controllers[i]) {
                LOG_INFO("Gamepad %d connected: %s", i,
                         SDL_GameControllerName(s_controllers[i]));
            }
        }
    }

    LOG_INFO("Input system initialized");
}

void Input::shutdown() {
    for (s32 i = 0; i < MAX_GAMEPADS; ++i) {
        if (s_controllers[i]) {
            SDL_GameControllerClose(s_controllers[i]);
            s_controllers[i] = nullptr;
        }
    }
    LOG_INFO("Input system shut down");
}

void Input::update() {
    // Copy current state to previous
    memcpy(s_previousKeys, s_currentKeys, sizeof(s_currentKeys));
    memcpy(s_previousMouseButtons, s_currentMouseButtons, sizeof(s_currentMouseButtons));

    // Reset per-frame accumulators
    s_mouseWheelY = 0;

    // Snapshot keyboard
    int numKeys = 0;
    const u8* state = SDL_GetKeyboardState(&numKeys);
    s32 copyCount = (numKeys < NUM_KEYS) ? numKeys : NUM_KEYS;
    memcpy(s_currentKeys, state, copyCount);

    // Mouse
    u32 mouseState = SDL_GetRelativeMouseState(&s_mouseDX, &s_mouseDY);
    SDL_GetMouseState(&s_mouseX, &s_mouseY);
    for (s32 i = 0; i < NUM_MOUSE_BUTTONS; ++i) {
        s_currentMouseButtons[i] = (mouseState & SDL_BUTTON(i + 1)) ? 1 : 0;
    }
}

// Keyboard
bool Input::isKeyDown(s32 scancode) {
    if (scancode < 0 || scancode >= NUM_KEYS) return false;
    return s_currentKeys[scancode] != 0;
}

bool Input::isKeyPressed(s32 scancode) {
    if (scancode < 0 || scancode >= NUM_KEYS) return false;
    return s_currentKeys[scancode] && !s_previousKeys[scancode];
}

bool Input::isKeyReleased(s32 scancode) {
    if (scancode < 0 || scancode >= NUM_KEYS) return false;
    return !s_currentKeys[scancode] && s_previousKeys[scancode];
}

// Mouse
void Input::getMouseDelta(s32& dx, s32& dy) {
    dx = s_mouseDX;
    dy = s_mouseDY;
}

void Input::getMousePosition(s32& x, s32& y) {
    x = s_mouseX;
    y = s_mouseY;
}

bool Input::isMouseButtonDown(u8 button) {
    if (button == 0 || button > NUM_MOUSE_BUTTONS) return false;
    return s_currentMouseButtons[button - 1] != 0;
}

bool Input::isMouseButtonPressed(u8 button) {
    if (button == 0 || button > NUM_MOUSE_BUTTONS) return false;
    return s_currentMouseButtons[button - 1] && !s_previousMouseButtons[button - 1];
}

bool Input::isMouseButtonReleased(u8 button) {
    if (button == 0 || button > NUM_MOUSE_BUTTONS) return false;
    return !s_currentMouseButtons[button - 1] && s_previousMouseButtons[button - 1];
}

void Input::setRelativeMouseMode(bool enabled) {
    SDL_SetRelativeMouseMode(enabled ? SDL_TRUE : SDL_FALSE);
}

s32 Input::getMouseWheelDelta() { return s_mouseWheelY; }

void Input::handleMouseWheel(s32 y) { s_mouseWheelY += y; }

// Gamepad
f32 Input::getAxis(s32 gamepadIndex, s32 axis) {
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return 0.0f;
    if (!s_controllers[gamepadIndex]) return 0.0f;
    s16 raw = SDL_GameControllerGetAxis(s_controllers[gamepadIndex],
                                         static_cast<SDL_GameControllerAxis>(axis));
    return static_cast<f32>(raw) / 32767.0f;
}

bool Input::isButtonDown(s32 gamepadIndex, s32 button) {
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return false;
    if (!s_controllers[gamepadIndex]) return false;
    return SDL_GameControllerGetButton(s_controllers[gamepadIndex],
                                        static_cast<SDL_GameControllerButton>(button)) != 0;
}

bool Input::isGamepadConnected(s32 gamepadIndex) {
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return false;
    return s_controllers[gamepadIndex] != nullptr;
}

void Input::handleControllerEvent(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        s32 index = event.cdevice.which;
        if (index >= 0 && index < MAX_GAMEPADS && !s_controllers[index]) {
            s_controllers[index] = SDL_GameControllerOpen(index);
            if (s_controllers[index]) {
                LOG_INFO("Gamepad %d connected: %s", index,
                         SDL_GameControllerName(s_controllers[index]));
            }
        }
    } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        // Find which controller was removed
        SDL_JoystickID id = event.cdevice.which;
        for (s32 i = 0; i < MAX_GAMEPADS; ++i) {
            if (s_controllers[i]) {
                SDL_Joystick* joy = SDL_GameControllerGetJoystick(s_controllers[i]);
                if (SDL_JoystickInstanceID(joy) == id) {
                    LOG_INFO("Gamepad %d disconnected", i);
                    SDL_GameControllerClose(s_controllers[i]);
                    s_controllers[i] = nullptr;
                    break;
                }
            }
        }
    }
}
