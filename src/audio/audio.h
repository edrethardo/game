#pragma once

#include "core/types.h"
#include "core/math.h"

// Sound effect IDs — loaded once at init, played by ID.
// Names match WAV filenames: SfxId::WEAPON_SWORD → "sfx_weapon_sword.wav"
enum struct SfxId : u8 {
    // Player weapons (one per subtype)
    WEAPON_SWORD, WEAPON_DAGGER, WEAPON_AXE, WEAPON_CLAYMORE, WEAPON_CLEAVER,
    WEAPON_PISTOL, WEAPON_SMG, WEAPON_CARBINE, WEAPON_REVOLVER,
    WEAPON_BOW, WEAPON_CROSSBOW, WEAPON_THROW, WEAPON_MOLOTOV, WEAPON_CHAKRAM,
    WEAPON_WAND, WEAPON_STAFF,
    RELOAD,
    // Combat feedback (RICOCHET = chakram/bounce-projectile wall reflect)
    HIT_MELEE, HIT_HITSCAN, HIT_PROJECTILE, RICOCHET,
    ENEMY_HIT, ENEMY_DEATH,
    PLAYER_HIT, PLAYER_DEATH,
    // Skills
    SKILL_FIRE, SKILL_ICE, SKILL_LIGHTNING, SKILL_BLOOD,
    SKILL_DASH, SKILL_HEAL, SKILL_BUFF, SKILL_SUMMON,
    SKILL_EXPLOSION, SKILL_STUN,
    // Items
    ITEM_PICKUP, ITEM_EQUIP, ITEM_DROP,
    POTION_USE,
    // UI
    UI_CLICK, UI_BACK, UI_CONFIRM,
    MENU_HOVER,
    // Movement
    FOOTSTEP_STONE, FOOTSTEP_METAL,
    // Enemies
    ENEMY_FOOTSTEP, ENEMY_ATTACK,
    BOSS_ROAR, BOSS_STOMP,
    // Environment
    DOOR_OPEN, LEVEL_UP,
    SHRINE_ACTIVATE,   // walk-up buff shrine (press E) — hand-picked via tools/pick_sfx.py
    // Count sentinel
    SFX_COUNT
};

namespace AudioSystem {
    // Call once at startup after SDL_Init. Loads all WAV files from assets/audio/.
    bool init();
    // Free all audio resources and close the audio device.
    void shutdown();

    // Fire-and-forget one-shot sound. Volume 0.0-1.0.
    void play(SfxId id, f32 volume = 1.0f);
    // Distance-attenuated one-shot. Quieter the further worldPos is from listenerPos.
    void playAt(SfxId id, Vec3 worldPos, Vec3 listenerPos, f32 maxDist = 20.0f);

    // Loop background music from file path (WAV or OGG).
    void playMusic(const char* path);
    void stopMusic();

    // Global volume controls (0.0-1.0). Persist across play calls.
    void setMasterVolume(f32 vol);
    void setSfxVolume(f32 vol);
    void setMusicVolume(f32 vol);

    // Current volume levels (0.0-1.0) — read back by the options-menu sliders.
    f32 getMasterVolume();
    f32 getSfxVolume();
    f32 getMusicVolume();

    // Persist the three volume levels to a small plain-text file (mirrors controls.json).
    // saveSettings writes the current levels; loadSettings parses + applies them via the setters
    // and is a no-op on a missing/corrupt file (defaults are kept).
    void saveSettings(const char* path);
    void loadSettings(const char* path);
}
