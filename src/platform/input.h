#pragma once

#include "core/types.h"

union SDL_Event;

// Abstract game actions — used for rebindable input
enum struct GameAction : u8 {
    MOVE_FORWARD, MOVE_BACKWARD, MOVE_LEFT, MOVE_RIGHT,
    JUMP, FIRE, BLOCK, CLASS_SKILL, TARGET_LOCK,
    POTION, PICKUP, RELOAD,
    SKILL_1, SKILL_2, SKILL_3, SKILL_4,
    BOOT_SKILL, HELMET_SKILL,
    INVENTORY, PAUSE,
    QUICKBAR_PREV, QUICKBAR_NEXT,
    MENU_UP, MENU_DOWN, MENU_CONFIRM, MENU_BACK,
    DODGE,
    COUNT
};

// Mouse button constants (matches SDL_BUTTON_*)
static constexpr u8 MOUSE_NONE   = 0;
static constexpr u8 MOUSE_LEFT   = 1;
static constexpr u8 MOUSE_MIDDLE = 2;
static constexpr u8 MOUSE_RIGHT  = 3;

// Binding for one action — keyboard key + optional mouse button + controller button
struct InputBinding {
    s32 key;            // SDL_Scancode (-1 if unbound)
    u8  mouseButton;    // SDL mouse button (0 if unbound)
    s32 button;         // SDL_GameControllerButton (-1 if unbound)
    s32 modifier;       // SDL_GameControllerButton modifier required (-1 = none, typically L shoulder)
    // Axis-based binding (for triggers)
    s32 axis;           // SDL_GameControllerAxis (-1 if not axis-bound)
    f32 axisThreshold;  // trigger threshold (0.5 for triggers)
};

namespace Input {
    void init();
    void shutdown();
    void update();              // Call once per frame after pollEvents
    void consumePressedState(); // Call after first accumulator tick to prevent multi-fire

    // --- Action-based input (keyboard + gamepad unified) ---
    bool isActionDown(GameAction action);     // true while held
    bool isActionPressed(GameAction action);  // true only first frame

    // --- Raw keyboard (still available for debug keys) ---
    bool isKeyDown(s32 scancode);
    bool isKeyPressed(s32 scancode);
    bool isKeyReleased(s32 scancode);

    // --- Mouse ---
    void getMouseDelta(s32& dx, s32& dy);
    void getMousePosition(s32& x, s32& y);
    bool isMouseButtonDown(u8 button);
    bool isMouseButtonPressed(u8 button);
    bool isMouseButtonReleased(u8 button);
    void setRelativeMouseMode(bool enabled);
    s32  getMouseWheelDelta();
    void handleMouseWheel(s32 y);

    // --- Gamepad ---
    static constexpr s32 MAX_GAMEPADS = 4;
    static constexpr f32 STICK_DEADZONE = 0.15f;

    // Sensitivity settings (mutable — adjustable from options menu)
    f32  getStickSensitivity();
    void setStickSensitivity(f32 v);
    f32  getGyroSensitivity();
    void setGyroSensitivity(f32 v);
    bool getStickInvertY();
    void setStickInvertY(bool v);
    bool getGyroInvertY();
    void setGyroInvertY(bool v);

    f32  getAxis(s32 gamepadIndex, s32 axis);
    bool isButtonDown(s32 gamepadIndex, s32 button);
    bool isButtonPressed(s32 gamepadIndex, s32 button);  // frame-edge detection
    bool isGamepadConnected(s32 gamepadIndex = 0);
    void handleControllerEvent(const SDL_Event& event);

    // Analog stick with deadzone applied (returns 0 inside deadzone)
    f32  getStickX(bool rightStick, s32 gamepadIndex = 0);
    f32  getStickY(bool rightStick, s32 gamepadIndex = 0);

    // Left-stick MENU navigation as an edge-triggered "press" (like a D-pad tap): fires once per
    // deflection, debounced with hysteresis, and consumed each frame so it can't multi-fire across
    // the fixed-timestep substeps. Computed in update(); read by the menu MENU_UP/DOWN path and the
    // couch screens. gamepadIndex 0 → active player.
    enum class StickNav : u8 { Up, Down, Left, Right };
    bool isMenuStickPressed(StickNav dir, s32 gamepadIndex = 0);

    // L shoulder modifier state
    bool isModifierHeld(s32 gamepadIndex = 0);

    // Rumble player `slot`'s controller at `strength` (0..1) for `durationMs` ms. No-op if no controller.
    void rumble(u8 slot, f32 strength, u32 durationMs);

    // Split-screen: set which player's controller to read (0 or 1)
    void setActivePlayer(u8 index);
    u8   getActivePlayer();
    // Enable/disable per-controller separation (disables merge-all behavior)
    void setSplitScreen(bool active);

    // Gyro (motion sensor) — returns angular velocity in deg/s
    // dx = yaw (horizontal turn), dy = pitch (vertical tilt)
    //
    // getGyro is consume-on-read on Switch: the gyro cache is zeroed after the
    // call so only the FIRST PlayerController::update substep in a render frame
    // applies the delta. Engine::run's fixed-timestep accumulator can run up to
    // 4 substeps per render frame; without the consume, every substep would
    // re-apply the same buffered gyro delta and over-rotate proportional to
    // substep count.
    //
    // peekGyro returns the same delta WITHOUT consuming. Use it from non-mutating
    // readers that need to see the same gyro the gameplay step will apply this
    // frame — specifically, PlayerController::captureLocalInput packs gyro into
    // the wire's NetInput before gameUpdate runs, so it must peek (consuming
    // here would starve PlayerController::update's getGyro and the local screen
    // wouldn't rotate). PC sensor read is a state query, so peek and consume are
    // behaviorally identical there.
    void getGyro (f32& dx, f32& dy, s32 gamepadIndex = 0);
    void peekGyro(f32& dx, f32& dy, s32 gamepadIndex = 0);
    bool isGyroAvailable(s32 gamepadIndex = 0);

    // Switch software keyboard. Opens a modal on-screen keyboard so the user can type text
    // (e.g. a multiplayer host IP) without a physical keyboard. Blocks until the user accepts
    // or cancels. `initial` (may be empty) prefills the field; `headerText` shows above it.
    // Returns true if the user accepted with a non-empty result (written to `outBuf`).
    // On PC this is a no-op stub returning false (PC has SDL text input from the keyboard).
    bool openVirtualKeyboard(const char* headerText, const char* initial,
                             char* outBuf, size_t outBufSize);

    // --- Binding management ---
    const InputBinding& getBinding(GameAction action);
    void setKeyBinding(GameAction action, s32 scancode);
    void setButtonBinding(GameAction action, s32 button, s32 modifier = -1);
    void resetBindingsToDefaults();
    void saveBindings(const char* path);
    void loadBindings(const char* path);

    // Display name for a GameAction (for options menu)
    const char* actionName(GameAction action);
    // Display name for a controller button
    const char* buttonName(s32 button);
}
