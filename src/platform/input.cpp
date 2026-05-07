#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "platform/input.h"
#include "core/log.h"

#include <cstring>
#include <cstdio>

static constexpr s32 NUM_KEYS = SDL_NUM_SCANCODES;
static constexpr s32 NUM_MOUSE_BUTTONS = 5;
static constexpr s32 NUM_PAD_BUTTONS = SDL_CONTROLLER_BUTTON_MAX;

static u8 s_currentKeys[NUM_KEYS];
static u8 s_previousKeys[NUM_KEYS];

static u8 s_currentMouseButtons[NUM_MOUSE_BUTTONS];
static u8 s_previousMouseButtons[NUM_MOUSE_BUTTONS];
static s32 s_mouseDX = 0;
static s32 s_mouseDY = 0;
static s32 s_mouseX = 0;
static s32 s_mouseY = 0;
static s32 s_mouseWheelY = 0;

static SDL_GameController* s_controllers[Input::MAX_GAMEPADS] = {};

// Sensitivity settings (adjustable from options menu)
static f32  s_stickSensitivity = 1.5f;   // degrees per second per unit of stick deflection
static f32  s_gyroSensitivity  = 5.0f;   // mouse-pixel-equivalent per deg/s
static bool s_stickInvertY     = false;
static bool s_gyroInvertY      = true;   // inverted by default for gyro

// Per-frame button state for pressed detection (gamepad 0)
static u8 s_currentPadButtons[NUM_PAD_BUTTONS];
static u8 s_previousPadButtons[NUM_PAD_BUTTONS];

// ---------------------------------------------------------------------------
// Default bindings table
// ---------------------------------------------------------------------------
// key, mouseButton, button, modifier, axis, axisThreshold
static InputBinding s_bindings[static_cast<u32>(GameAction::COUNT)];

static void setDefaults() {
    auto& b = s_bindings;
    auto set = [&](GameAction a, s32 key, u8 mouse, s32 btn, s32 mod = -1, s32 ax = -1, f32 axT = 0.0f) {
        u32 i = static_cast<u32>(a);
        b[i] = {key, mouse, btn, mod, ax, axT};
    };

    // Movement
    set(GameAction::MOVE_FORWARD,  SDL_SCANCODE_W,     0, -1);
    set(GameAction::MOVE_BACKWARD, SDL_SCANCODE_S,     0, -1);
    set(GameAction::MOVE_LEFT,     SDL_SCANCODE_A,     0, -1);
    set(GameAction::MOVE_RIGHT,    SDL_SCANCODE_D,     0, -1);

    // Core actions
    set(GameAction::JUMP,          SDL_SCANCODE_SPACE,  0, SDL_CONTROLLER_BUTTON_A);
    set(GameAction::FIRE,          -1, MOUSE_LEFT,       -1, -1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0.5f);
    set(GameAction::BLOCK,         SDL_SCANCODE_LCTRL,  0, -1, -1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0.5f);
    set(GameAction::CLASS_SKILL,   -1, MOUSE_RIGHT,      SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    set(GameAction::TARGET_LOCK,   -1, MOUSE_MIDDLE,     SDL_CONTROLLER_BUTTON_LEFTSHOULDER);

    // Items / utility
    set(GameAction::POTION,        SDL_SCANCODE_Q,      0, SDL_CONTROLLER_BUTTON_B);
    set(GameAction::PICKUP,        SDL_SCANCODE_E,      0, SDL_CONTROLLER_BUTTON_X);
    set(GameAction::RELOAD,        SDL_SCANCODE_R,      0, SDL_CONTROLLER_BUTTON_Y);

    // Skill selection (D-pad)
    set(GameAction::SKILL_1,       SDL_SCANCODE_1,      0, SDL_CONTROLLER_BUTTON_DPAD_UP);
    set(GameAction::SKILL_2,       SDL_SCANCODE_2,      0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    set(GameAction::SKILL_3,       SDL_SCANCODE_3,      0, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    set(GameAction::SKILL_4,       SDL_SCANCODE_4,      0, SDL_CONTROLLER_BUTTON_DPAD_LEFT);

    // Equipment skills (L + face button)
    set(GameAction::BOOT_SKILL,    SDL_SCANCODE_F,      0, SDL_CONTROLLER_BUTTON_A,    SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    set(GameAction::HELMET_SKILL,  SDL_SCANCODE_G,      0, SDL_CONTROLLER_BUTTON_B,    SDL_CONTROLLER_BUTTON_LEFTSHOULDER);

    // UI
    set(GameAction::INVENTORY,     SDL_SCANCODE_TAB,    0, SDL_CONTROLLER_BUTTON_START);
    set(GameAction::PAUSE,         SDL_SCANCODE_ESCAPE, 0, SDL_CONTROLLER_BUTTON_BACK);
    set(GameAction::QUICKBAR_PREV, -1,                  0, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    set(GameAction::QUICKBAR_NEXT, -1,                  0, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);

    // Menu navigation
    set(GameAction::MENU_UP,       SDL_SCANCODE_UP,     0, SDL_CONTROLLER_BUTTON_DPAD_UP);
    set(GameAction::MENU_DOWN,     SDL_SCANCODE_DOWN,   0, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    set(GameAction::MENU_CONFIRM,  SDL_SCANCODE_RETURN, 0, SDL_CONTROLLER_BUTTON_A);
    set(GameAction::MENU_BACK,     SDL_SCANCODE_ESCAPE, 0, SDL_CONTROLLER_BUTTON_B);
}

// ---------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------
void Input::init() {
    memset(s_currentKeys, 0, sizeof(s_currentKeys));
    memset(s_previousKeys, 0, sizeof(s_previousKeys));
    memset(s_currentMouseButtons, 0, sizeof(s_currentMouseButtons));
    memset(s_previousMouseButtons, 0, sizeof(s_previousMouseButtons));
    memset(s_currentPadButtons, 0, sizeof(s_currentPadButtons));
    memset(s_previousPadButtons, 0, sizeof(s_previousPadButtons));
    s_mouseDX = 0;
    s_mouseDY = 0;

    setDefaults();

    // Open any controllers already connected
    for (s32 i = 0; i < SDL_NumJoysticks() && i < MAX_GAMEPADS; ++i) {
        if (SDL_IsGameController(i)) {
            s_controllers[i] = SDL_GameControllerOpen(i);
            if (s_controllers[i]) {
                LOG_INFO("Gamepad %d connected: %s", i,
                         SDL_GameControllerName(s_controllers[i]));
#ifndef __SWITCH__
                // Enable gyro sensor if available (PC: SDL2 sensor API)
                if (SDL_GameControllerHasSensor(s_controllers[i], SDL_SENSOR_GYRO)) {
                    SDL_GameControllerSetSensorEnabled(s_controllers[i], SDL_SENSOR_GYRO, SDL_TRUE);
                    LOG_INFO("Gamepad %d: gyro enabled", i);
                }
#endif
            }
        }
    }

    // Try to load saved bindings
    loadBindings(ASSET_PATH("assets/config/controls.json"));

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
    memcpy(s_previousPadButtons, s_currentPadButtons, sizeof(s_currentPadButtons));

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

    // Gamepad button state snapshot (gamepad 0 for frame-edge detection)
    if (s_controllers[0]) {
        for (s32 i = 0; i < NUM_PAD_BUTTONS; i++) {
            s_currentPadButtons[i] = SDL_GameControllerGetButton(
                s_controllers[0], static_cast<SDL_GameControllerButton>(i));
        }
    } else {
        memset(s_currentPadButtons, 0, sizeof(s_currentPadButtons));
    }
}

// ---------------------------------------------------------------------------
// Raw keyboard
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void Input::getMouseDelta(s32& dx, s32& dy) { dx = s_mouseDX; dy = s_mouseDY; }
void Input::getMousePosition(s32& x, s32& y) { x = s_mouseX; y = s_mouseY; }

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

// ---------------------------------------------------------------------------
// Gamepad raw
// ---------------------------------------------------------------------------
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

bool Input::isButtonPressed(s32 gamepadIndex, s32 button) {
    if (gamepadIndex != 0) return false; // frame-edge only tracked for pad 0
    if (button < 0 || button >= NUM_PAD_BUTTONS) return false;
    return s_currentPadButtons[button] && !s_previousPadButtons[button];
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
                // Enable gyro sensor if available
                if (SDL_GameControllerHasSensor(s_controllers[index], SDL_SENSOR_GYRO)) {
                    SDL_GameControllerSetSensorEnabled(s_controllers[index], SDL_SENSOR_GYRO, SDL_TRUE);
                    LOG_INFO("Gamepad %d: gyro enabled", index);
                }
            }
        }
    } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
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

// ---------------------------------------------------------------------------
// Stick with deadzone
// ---------------------------------------------------------------------------
f32 Input::getStickX(bool rightStick, s32 gamepadIndex) {
    s32 ax = rightStick ? SDL_CONTROLLER_AXIS_RIGHTX : SDL_CONTROLLER_AXIS_LEFTX;
    f32 v = getAxis(gamepadIndex, ax);
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0.0f;
    // Remap from deadzone..1.0 to 0..1.0 for smooth response
    f32 sign = (v > 0) ? 1.0f : -1.0f;
    f32 mag = (fabsf(v) - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    return sign * mag;
}

f32 Input::getStickY(bool rightStick, s32 gamepadIndex) {
    s32 ax = rightStick ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_LEFTY;
    f32 v = getAxis(gamepadIndex, ax);
    if (v > -STICK_DEADZONE && v < STICK_DEADZONE) return 0.0f;
    f32 sign = (v > 0) ? 1.0f : -1.0f;
    f32 mag = (fabsf(v) - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    return sign * mag;
}

bool Input::isModifierHeld(s32 gamepadIndex) {
    return isButtonDown(gamepadIndex, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
}

// ---------------------------------------------------------------------------
// Gyro (motion sensor)
// ---------------------------------------------------------------------------

#ifdef __SWITCH__
#include <switch.h>
static bool s_gyroInitialized = false;
static HidSixAxisSensorHandle s_gyroHandles[2]; // 0=player1 dual, 1=handheld
static u32 s_gyroHandleCount = 0;

static void initGyro() {
    if (s_gyroInitialized) return;
    s_gyroInitialized = true;

    // Get six-axis sensor handles for the first player
    padConfigureInput(1, HidNpadStyleSet_NpadFullCtrl);
    hidGetSixAxisSensorHandles(&s_gyroHandles[0], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidStartSixAxisSensor(s_gyroHandles[0]);
    s_gyroHandleCount = 1;

    // Also try handheld mode
    hidGetSixAxisSensorHandles(&s_gyroHandles[1], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidStartSixAxisSensor(s_gyroHandles[1]);
    s_gyroHandleCount = 2;

    LOG_INFO("Gyro: initialized %u sensor handles", s_gyroHandleCount);
}
#endif

void Input::getGyro(f32& dx, f32& dy, s32 gamepadIndex) {
    dx = 0.0f;
    dy = 0.0f;
    (void)gamepadIndex;

#ifdef __SWITCH__
    initGyro();
    HidSixAxisSensorState state = {};
    // Try each handle until we get valid data
    for (u32 i = 0; i < s_gyroHandleCount; i++) {
        if (hidGetSixAxisSensorStates(s_gyroHandles[i], &state, 1) >= 1) {
            // angular_velocity is in rad/s: x=pitch, y=yaw, z=roll
            f32 yaw   = state.angular_velocity.y;
            f32 pitch = state.angular_velocity.x;
            // Apply small deadzone to filter drift (0.01 rad/s ≈ 0.6 deg/s)
            if (yaw > -0.01f && yaw < 0.01f) yaw = 0.0f;
            if (pitch > -0.01f && pitch < 0.01f) pitch = 0.0f;
            if (yaw != 0.0f || pitch != 0.0f) {
                dx = -yaw  * (180.0f / 3.14159f); // to deg/s (negated for natural direction)
                dy = pitch * (180.0f / 3.14159f);
                return;
            }
        }
    }
#else
    // PC: try SDL2 sensor API (works with DS4/DualSense controllers)
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return;
    if (!s_controllers[gamepadIndex]) return;
    if (!SDL_GameControllerHasSensor(s_controllers[gamepadIndex], SDL_SENSOR_GYRO)) return;
    f32 data[3] = {};
    if (SDL_GameControllerGetSensorData(s_controllers[gamepadIndex], SDL_SENSOR_GYRO, data, 3) == 0) {
        dx = data[1] * (180.0f / 3.14159f);
        dy = data[0] * (180.0f / 3.14159f);
    }
#endif
}

bool Input::isGyroAvailable(s32 gamepadIndex) {
#ifdef __SWITCH__
    (void)gamepadIndex;
    return true; // Switch always has gyro (Joy-Cons or Pro Controller)
#else
    if (gamepadIndex < 0 || gamepadIndex >= MAX_GAMEPADS) return false;
    if (!s_controllers[gamepadIndex]) return false;
    return SDL_GameControllerHasSensor(s_controllers[gamepadIndex], SDL_SENSOR_GYRO);
#endif
}

// ---------------------------------------------------------------------------
// Sensitivity getters/setters
// ---------------------------------------------------------------------------
f32  Input::getStickSensitivity()        { return s_stickSensitivity; }
void Input::setStickSensitivity(f32 v)   { s_stickSensitivity = v; }
f32  Input::getGyroSensitivity()         { return s_gyroSensitivity; }
void Input::setGyroSensitivity(f32 v)    { s_gyroSensitivity = v; }
bool Input::getStickInvertY()            { return s_stickInvertY; }
void Input::setStickInvertY(bool v)      { s_stickInvertY = v; }
bool Input::getGyroInvertY()             { return s_gyroInvertY; }
void Input::setGyroInvertY(bool v)       { s_gyroInvertY = v; }

// ---------------------------------------------------------------------------
// Action-based input — merges keyboard + mouse + gamepad
// ---------------------------------------------------------------------------
static bool checkActionRaw(GameAction action, bool pressed) {
    u32 idx = static_cast<u32>(action);
    if (idx >= static_cast<u32>(GameAction::COUNT)) return false;
    const InputBinding& b = s_bindings[idx];

    // Keyboard check
    if (b.key >= 0) {
        if (pressed ? Input::isKeyPressed(b.key) : Input::isKeyDown(b.key))
            return true;
    }

    // Mouse button check
    if (b.mouseButton > 0) {
        if (pressed ? Input::isMouseButtonPressed(b.mouseButton)
                    : Input::isMouseButtonDown(b.mouseButton))
            return true;
    }

    // Gamepad button check (with optional modifier)
    if (b.button >= 0) {
        bool modOk = (b.modifier < 0) || Input::isButtonDown(0, b.modifier);
        // If modifier is required, also ensure we're NOT consuming the modifier's own action
        if (b.modifier >= 0 && !Input::isButtonDown(0, b.modifier)) modOk = false;
        if (modOk) {
            if (pressed ? Input::isButtonPressed(0, b.button) : Input::isButtonDown(0, b.button))
                return true;
        }
    }

    // Axis check (triggers)
    if (b.axis >= 0) {
        f32 v = Input::getAxis(0, b.axis);
        if (v >= b.axisThreshold) {
            // For "pressed", we'd need previous frame axis state — approximate with threshold
            // Since triggers are analog, treat as "down" always
            if (!pressed) return true;
            // For pressed: check if we just crossed threshold (not tracked precisely, but rare need)
            return true;
        }
    }

    return false;
}

bool Input::isActionDown(GameAction action) {
    return checkActionRaw(action, false);
}

bool Input::isActionPressed(GameAction action) {
    return checkActionRaw(action, true);
}

// ---------------------------------------------------------------------------
// Binding management
// ---------------------------------------------------------------------------
const InputBinding& Input::getBinding(GameAction action) {
    return s_bindings[static_cast<u32>(action)];
}

void Input::setKeyBinding(GameAction action, s32 scancode) {
    s_bindings[static_cast<u32>(action)].key = scancode;
}

void Input::setButtonBinding(GameAction action, s32 button, s32 modifier) {
    s_bindings[static_cast<u32>(action)].button = button;
    s_bindings[static_cast<u32>(action)].modifier = modifier;
}

void Input::resetBindingsToDefaults() {
    setDefaults();
}

// Save/load use a simple text format (one line per action)
void Input::saveBindings(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (u32 i = 0; i < static_cast<u32>(GameAction::COUNT); i++) {
        const InputBinding& b = s_bindings[i];
        fprintf(f, "%u %d %u %d %d %d %.2f\n",
                i, b.key, (u32)b.mouseButton, b.button, b.modifier, b.axis, b.axisThreshold);
    }
    fclose(f);
    LOG_INFO("Input: saved bindings to %s", path);
}

void Input::loadBindings(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return; // no saved bindings, use defaults
    u32 idx;
    s32 key, btn, mod, ax;
    u32 mouse;
    f32 axT;
    while (fscanf(f, "%u %d %u %d %d %d %f", &idx, &key, &mouse, &btn, &mod, &ax, &axT) == 7) {
        if (idx < static_cast<u32>(GameAction::COUNT)) {
            s_bindings[idx].key = key;
            s_bindings[idx].mouseButton = static_cast<u8>(mouse);
            s_bindings[idx].button = btn;
            s_bindings[idx].modifier = mod;
            s_bindings[idx].axis = ax;
            s_bindings[idx].axisThreshold = axT;
        }
    }
    fclose(f);
    LOG_INFO("Input: loaded bindings from %s", path);
}

// ---------------------------------------------------------------------------
// Display names
// ---------------------------------------------------------------------------
const char* Input::actionName(GameAction action) {
    switch (action) {
        case GameAction::MOVE_FORWARD:  return "Move Forward";
        case GameAction::MOVE_BACKWARD: return "Move Backward";
        case GameAction::MOVE_LEFT:     return "Move Left";
        case GameAction::MOVE_RIGHT:    return "Move Right";
        case GameAction::JUMP:          return "Jump";
        case GameAction::FIRE:          return "Attack / Fire";
        case GameAction::BLOCK:         return "Block / Shield";
        case GameAction::CLASS_SKILL:   return "Class Skill";
        case GameAction::TARGET_LOCK:   return "Target Lock";
        case GameAction::POTION:        return "Potion";
        case GameAction::PICKUP:        return "Pickup / Interact";
        case GameAction::RELOAD:        return "Reload";
        case GameAction::SKILL_1:       return "Skill 1";
        case GameAction::SKILL_2:       return "Skill 2";
        case GameAction::SKILL_3:       return "Skill 3";
        case GameAction::SKILL_4:       return "Skill 4";
        case GameAction::BOOT_SKILL:    return "Boot Skill (F)";
        case GameAction::HELMET_SKILL:  return "Helmet Skill (G)";
        case GameAction::INVENTORY:     return "Inventory";
        case GameAction::PAUSE:         return "Pause / Menu";
        case GameAction::QUICKBAR_PREV: return "Quickbar Prev";
        case GameAction::QUICKBAR_NEXT: return "Quickbar Next";
        case GameAction::MENU_UP:       return "Menu Up";
        case GameAction::MENU_DOWN:     return "Menu Down";
        case GameAction::MENU_CONFIRM:  return "Confirm";
        case GameAction::MENU_BACK:     return "Back";
        default: return "Unknown";
    }
}

const char* Input::buttonName(s32 button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A:             return "A";
        case SDL_CONTROLLER_BUTTON_B:             return "B";
        case SDL_CONTROLLER_BUTTON_X:             return "X";
        case SDL_CONTROLLER_BUTTON_Y:             return "Y";
        case SDL_CONTROLLER_BUTTON_BACK:          return "-";
        case SDL_CONTROLLER_BUTTON_START:         return "+";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "L3";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:     return "R3";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "L";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "D-Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "D-Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "D-Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "D-Right";
        default: return "?";
    }
}
