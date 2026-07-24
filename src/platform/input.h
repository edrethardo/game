#pragma once

#include "core/types.h"

union SDL_Event;

// Abstract game actions — used for rebindable input
enum struct GameAction : u8 {
    // ORDINALS ARE THE ON-DISK FORMAT. saveBindings() writes one row per action keyed by its enum
    // ordinal, so members are RENAMED IN PLACE and new ones APPENDED — never inserted or removed,
    // or every existing controls.json silently re-maps onto the wrong actions. Where a rename also
    // changes an action's DEFAULT binding, bump CFG_BINDINGS_REV and repair it in loadBindings.
    MOVE_FORWARD, MOVE_BACKWARD, MOVE_LEFT, MOVE_RIGHT,
    // QUICKBAR_USE was TARGET_LOCK until the lock-on feature was cut (its lockActive was never set
    // true). Kept at this ordinal because it must stay <= INVENTORY to remain listed in the rebind
    // UI — see REBIND_COUNT in engine_render_menus.cpp.
    JUMP, FIRE, BLOCK, CLASS_SKILL, QUICKBAR_USE,
    POTION, PICKUP, RELOAD,
    SKILL_1, SKILL_2, SKILL_3, SKILL_4,
    BOOT_SKILL, HELMET_SKILL,
    INVENTORY, PAUSE,
    // The 4 quickbar slots, on L + D-pad (Up/Right/Down/Left) — the same direction order as the
    // bare-D-pad class skills. Slots 1-2 reuse the ordinals of the old QUICKBAR_PREV/QUICKBAR_NEXT
    // cycle actions they replace; slots 3-4 are appended at the tail. Ugly split, but ordinals are
    // the file format. (These sit above INVENTORY, so like PREV/NEXT before them they are not in
    // the rebind UI.)
    QUICKBAR_SLOT_1, QUICKBAR_SLOT_2,
    MENU_UP, MENU_DOWN, MENU_CONFIRM, MENU_BACK,
    DODGE,
    CHARACTER_SCREEN,
    QUICKBAR_SLOT_3, QUICKBAR_SLOT_4,
    COUNT
};

// Rebindable actions shown in the options rebind UI, in display order. NOT simply the contiguous
// [MOVE_FORWARD..INVENTORY] range: DODGE was appended after INVENTORY (ordinals are the on-disk format
// and can't be reordered), yet it is combat-critical (its i-frames drive the whole CC-negation system),
// so it must be rebindable — included here via an explicit tail. rebindActionAt(row) maps a UI row to
// its action; REBIND_ROWS is the total row count. The three rebind sites (the render + the KM/controller
// capture handlers) all key off these, so adding a late-appended rebindable action means only editing
// kRebindTail (saveBindings already persists every ordinal). CHARACTER_SCREEN is intentionally NOT here
// — it is a fixed T/K convenience key whose K alias lives in the non-serialized InputBinding.key2.
namespace Input {
    inline constexpr u32 REBIND_CONTIG = static_cast<u32>(GameAction::INVENTORY) + 1;   // rows 0..INVENTORY
    inline constexpr GameAction kRebindTail[] = { GameAction::DODGE };
    inline constexpr u32 REBIND_ROWS = REBIND_CONTIG + sizeof(kRebindTail) / sizeof(kRebindTail[0]);
    inline constexpr GameAction rebindActionAt(u32 row) {
        return row < REBIND_CONTIG ? static_cast<GameAction>(row) : kRebindTail[row - REBIND_CONTIG];
    }
}

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
    // Secondary keyboard alias. Checked alongside `key` in checkActionRaw so one action can answer to
    // two keys (character screen = T and K). In-memory ONLY: seeded by buildDefaults, never written to
    // or read from controls.json (that format is a fixed 7 tokens), so it needs no format/rev bump.
    // Meant for FIXED, non-rebindable convenience keys — a rebindable action would lose it on save.
    // Placed last with a default initializer so the 6-field aggregate init in buildDefaults leaves it -1.
    s32 key2 = -1;
};

namespace Input {
    void init();
    void shutdown();
    // Call once per RENDER frame after pollEvents. `dt` is real elapsed frame time (not the fixed
    // timestep) — it drives the menu hold-to-repeat clock in isMenuNavPressed().
    void update(f32 dt);
    void consumePressedState(); // Call after first accumulator tick to prevent multi-fire

    // --- Action-based input (keyboard + gamepad unified) ---
    bool isActionDown(GameAction action);     // true while held
    bool isActionPressed(GameAction action);  // true only first frame

    // --- Autoplay synthetic-input overlay ---------------------------------------------------
    // The bot's held/pressed action state, OR'd into isActionDown/isActionPressed below the
    // real-device read so a bot press is indistinguishable from a human one. Off unless armed.
    void setBotOverlayActive(bool on);
    bool botOverlayActive();
    void setBotHeld(GameAction action, bool on);   // bot arms/clears one action for this tick
    void clearBotHeld();                            // drop all bot-held actions
    // True if a human touched any gameplay device THIS render frame (the kbmActive/padActive
    // computation already done in update(), threshold-filtered). Autoplay's takeover trigger.
    // Keyboard/mouse only counts while the window is FOCUSED — see windowFocused() below.
    bool humanActivityThisFrame();

    // --- Window focus gate ------------------------------------------------------------------
    // While the window is UNFOCUSED the engine keeps simulating and rendering (so Autoplay can
    // play on a second screen), but every real keyboard/mouse read reports "nothing pressed",
    // the mouse delta is discarded, relative mouse mode is released (cursor freed for whatever
    // app the player is actually using) and the Autoplay takeover latch ignores the keyboard and
    // mouse. Focusing again restores the mode the current screen wants, with no aim snap. The
    // rules themselves are pure and unit-tested in platform/input_focus.h.
    // Window::pollEvents() pushes SDL's SDL_WINDOW_INPUT_FOCUS state in here once per frame;
    // nothing else should call the setter. Gamepads stay live while unfocused by design.
    void setWindowFocused(bool focused);
    bool windowFocused();

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
    // Show/hide the OS cursor (menus hide it while keyboard/controller drives them). Edge-tracked
    // internally, so calling it every frame is cheap. setRelativeMouseMode() re-shows the cursor as
    // a baseline whenever it runs, so a menu that hid the cursor can never leave it stuck hidden
    // after any state transition (which all go through setRelativeMouseMode).
    void setCursorVisible(bool visible);
    // True exactly once after relative-mouse mode goes ON→OFF — i.e. gameplay handed control to a
    // cursor screen (menu/pause/death). Menus consume this to re-arm their "last input device" gate
    // (start with the pointer disabled + discard the bogus first mouse delta the mode switch emits).
    bool consumeRelativeReleased();
    s32  getMouseWheelDelta();
    void handleMouseWheel(s32 y);

    // --- Gamepad ---
    static constexpr s32 MAX_GAMEPADS = 4;
    static constexpr f32 STICK_DEADZONE = 0.15f;

    // Sensitivity settings (mutable — adjustable from options menu)
    f32  getStickSensitivity();
    void setStickSensitivity(f32 v);
    // Shared default (fresh install + menu "Reset to Defaults") for right-stick look. A saved
    // controls.json row always overrides it, so lowering this only reaches new installations.
    static constexpr f32 STICK_SENS_DEFAULT = 0.7f;
    f32  getGyroSensitivity();
    void setGyroSensitivity(f32 v);
    bool getStickInvertY();
    void setStickInvertY(bool v);
    bool getGyroInvertY();
    void setGyroInvertY(bool v);
    // Gyro (motion) aim master switch. OFF by default — gyro is opt-in, so a fresh install never
    // aims from controller motion; the Controller options submenu toggles it. readGyroInto returns
    // a zero delta while this is off, so every consumer (look + NetInput packing) sees no gyro.
    bool getGyroEnabled();
    void setGyroEnabled(bool v);
    // Mouse look sensitivity — a unitless MULTIPLIER on the base radians/pixel used in player.cpp
    // mouse-look. Adjustable from the Keyboard & Mouse options submenu. A saved row overrides this,
    // so lowering the default only reaches new installations.
    f32  getMouseSensitivity();
    void setMouseSensitivity(f32 v);
    static constexpr f32 MOUSE_SENS_DEFAULT = 0.6f;  // shared reset value (menu + save)

    f32  getAxis(s32 gamepadIndex, s32 axis);
    bool isButtonDown(s32 gamepadIndex, s32 button);
    bool isButtonPressed(s32 gamepadIndex, s32 button);  // frame-edge detection
    bool isGamepadConnected(s32 gamepadIndex = 0);
    void handleControllerEvent(const SDL_Event& event);

    // Which input device the player is ACTIVELY using — distinct from isGamepadConnected() (physical
    // presence). On Steam Deck a gamepad is ALWAYS connected, so presence can't decide whether to
    // show keyboard/mouse vs controller button prompts; this tracks per-frame activity (last device
    // wins, sticky), updated at the end of update(). Use it to pick which glyphs/prompts to draw.
    // NOTE: orthogonal to the menu mouse gate (Engine::updateMenuMouseActive), which is a
    // mouse-vs-everything axis — a DIFFERENT grouping; don't conflate the two.
    // Caveat: the Steam Deck's trackpads/gyro surface as SDL mouse motion, so trackpad use may read
    // as keyboard/mouse (accepted limitation; filtered by a small movement threshold).
    enum struct InputDevice : u8 { KeyboardMouse, Gamepad };
    InputDevice activeDevice();
    bool        activeDeviceIsGamepad();   // convenience: activeDevice() == Gamepad

    // Split-screen glyph routing. In couch co-op each viewport's HUD/prompts must show the device
    // THAT player is on, not the single global last-used device — otherwise P2 nudging a pad flips
    // P1's on-screen prompts to controller glyphs (and vice-versa). Two hooks share one per-lane
    // device tracker:
    //   - laneDevice(lane): the device a specific local lane is on. A lane with no assigned pad
    //     (single-controller couch P1) is keyboard/mouse; lane >=1 is controller-only by design
    //     (keyboard is gated to lane 0 in checkActionRaw) so it's Gamepad while a pad is attached;
    //     lane 0 with a pad is last-device-wins between that pad and the keyboard/mouse. Outside
    //     split-screen it returns the global activeDevice(). Use where you hold a concrete lane index
    //     (e.g. seeding a per-lane UI mode).
    //   - setGlyphLane(lane): points the CURRENT render lane at a viewport so the activeDevice()
    //     calls already baked into drawKeySymbol and the skill/quick bars resolve per-player with NO
    //     signature changes. The render loop sets it per viewport and clears it (-1) afterwards; -1
    //     (the default, and any non-split context) means "use the global device", so menus and
    //     single-player are unchanged.
    InputDevice laneDevice(u8 lane);
    bool        laneDeviceIsGamepad(u8 lane);
    void        setGlyphLane(s8 lane);

    // Analog stick with deadzone applied (returns 0 inside deadzone)
    f32  getStickX(bool rightStick, s32 gamepadIndex = 0);
    f32  getStickY(bool rightStick, s32 gamepadIndex = 0);

    // Left-stick MENU navigation as an edge-triggered "press" (like a D-pad tap): fires once per
    // deflection, debounced with hysteresis, and consumed each frame so it can't multi-fire across
    // the fixed-timestep substeps. Computed in update(); read by the menu MENU_UP/DOWN path and the
    // couch screens. gamepadIndex 0 → active player.
    enum class StickNav : u8 { Up, Down, Left, Right };
    bool isMenuStickPressed(StickNav dir, s32 gamepadIndex = 0);

    // Menu directional navigation as ONE question: "is the player asking to go LEFT right now?"
    // Unions the arrow keys, WASD, the D-pad and the left stick, so no menu screen ever has to
    // spell that union out again. Every screen used to, and they had all drifted — of the six
    // left/right sites, four accepted A/D, two didn't, and one ignored the stick entirely. Same
    // failure mode that left the main menu as the only screen with no navigation sound.
    //
    // HOLD-TO-REPEAT: fires on the press edge, then — while still held — after MENU_REPEAT_DELAY
    // and once per MENU_REPEAT_PERIOD after that, so a 50-floor list or a volume slider can be
    // scrolled by holding rather than machine-gunning the key. Fires are consumed per frame like
    // every other pressed-edge, so one repeat can never double-step across fixed-timestep substeps.
    //
    // `wasd`: pass FALSE on the two text-entry screens (the lobby-code and Join-IP on-screen
    // keyboards), where a physical 'A' must type the letter A rather than pan the cursor. That is
    // the ONLY reason this parameter exists; everywhere else the default is what you want.
    bool isMenuNavPressed(StickNav dir, s32 gamepadIndex = 0, bool wasd = true);

    // L shoulder modifier state
    bool isModifierHeld(s32 gamepadIndex = 0);

    // Rumble player `slot`'s controller at `strength` (0..1) for `durationMs` ms. No-op if no controller.
    void rumble(u8 slot, f32 strength, u32 durationMs);

    // Split-screen: set which player's controller to read (0 or 1)
    void setActivePlayer(u8 index);
    u8   getActivePlayer();
    // Enable/disable per-controller separation (disables merge-all behavior). Turning it OFF also
    // resets the couch lane->pad map to identity.
    void setSplitScreen(bool active);
    // Number of physical SDL controllers currently connected (PC; 0 on Switch, which uses libnx pads).
    u8   connectedGamepadCount();
    // Assign controllers to couch lanes when a split-screen game starts. With exactly ONE controller,
    // Player 1 is the keyboard and the controller becomes Player 2 (lane 1 -> pad 0); with two or more,
    // the identity map (P1=pad0, P2=pad1) is kept. Fixes "one controller, both players on P1".
    void assignCouchPads();

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
    // Per-category resets (rebindable actions 0..INVENTORY only) for the options submenus:
    // keyboard-only restores just .key/.mouseButton; controller-only just .button/.modifier/.axis.
    void resetKeyboardBindingsToDefaults();
    void resetControllerBindingsToDefaults();
    void saveBindings(const char* path);
    void loadBindings(const char* path);

    // Display name for a GameAction (for options menu)
    const char* actionName(GameAction action);
    // Display name for a controller button
    const char* buttonName(s32 button);
}
