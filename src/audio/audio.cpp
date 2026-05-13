// AudioSystem — SDL_mixer-based audio playback for SFX and music.
// Loads all WAV files from assets/audio/ at init, maps them to SfxId enum.
// play() fires on the first free mixing channel. playAt() adds distance attenuation.

#include "audio/audio.h"
#include "core/log.h"

#include <SDL_mixer.h>
#include <cstring>
#include <cmath>
#include <cstdio>

// SfxId → WAV filename mapping (must match gen_audio.py output names)
static const char* s_sfxFiles[static_cast<u32>(SfxId::SFX_COUNT)] = {
    // Weapons
    "sfx_weapon_sword.wav",
    "sfx_weapon_dagger.wav",
    "sfx_weapon_axe.wav",
    "sfx_weapon_claymore.wav",
    "sfx_weapon_pistol.wav",
    "sfx_weapon_smg.wav",
    "sfx_weapon_carbine.wav",
    "sfx_weapon_revolver.wav",
    "sfx_weapon_bow.wav",
    "sfx_weapon_crossbow.wav",
    "sfx_weapon_throw.wav",
    "sfx_weapon_molotov.wav",
    "sfx_weapon_wand.wav",
    "sfx_weapon_staff.wav",
    "sfx_reload.wav",
    // Combat
    "sfx_hit_melee.wav",
    "sfx_hit_hitscan.wav",
    "sfx_hit_projectile.wav",
    "sfx_enemy_hit.wav",
    "sfx_enemy_death.wav",
    "sfx_player_hit.wav",
    "sfx_player_death.wav",
    // Skills
    "sfx_skill_fire.wav",
    "sfx_skill_ice.wav",
    "sfx_skill_lightning.wav",
    "sfx_skill_blood.wav",
    "sfx_skill_dash.wav",
    "sfx_skill_heal.wav",
    "sfx_skill_buff.wav",
    "sfx_skill_summon.wav",
    "sfx_skill_explosion.wav",
    "sfx_skill_stun.wav",
    // Items
    "sfx_item_pickup.wav",
    "sfx_item_equip.wav",
    "sfx_item_drop.wav",
    "sfx_potion_use.wav",
    // UI
    "sfx_ui_click.wav",
    "sfx_ui_back.wav",
    "sfx_ui_confirm.wav",
    "sfx_menu_hover.wav",
    // Movement
    "sfx_footstep_stone.wav",
    "sfx_footstep_metal.wav",
    // Enemies
    "sfx_enemy_footstep.wav",
    "sfx_enemy_attack.wav",
    "sfx_boss_roar.wav",
    "sfx_boss_stomp.wav",
    // Environment
    "sfx_door_open.wav",
    "sfx_level_up.wav",
};

static Mix_Chunk* s_sfx[static_cast<u32>(SfxId::SFX_COUNT)] = {};
static Mix_Music* s_music = nullptr;
static f32 s_masterVol = 1.0f;
static f32 s_sfxVol    = 1.0f;
static f32 s_musicVol  = 0.7f;
static bool s_initialized = false;

// Asset path helper — prepends "assets/audio/" (or romfs path on Switch)
static void buildPath(char* out, u32 outSize, const char* filename) {
#ifdef __SWITCH__
    std::snprintf(out, outSize, "romfs:/audio/%s", filename);
#else
    std::snprintf(out, outSize, "assets/audio/%s", filename);
#endif
}

bool AudioSystem::init() {
    // Switch hardware runs at 48kHz — match it to avoid sinc resampling artifacts
#ifdef __SWITCH__
    if (Mix_OpenAudio(48000, MIX_DEFAULT_FORMAT, 2, 1024) < 0) {
#else
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) < 0) {
#endif
        LOG_WARN("Mix_OpenAudio failed: %s — audio disabled", Mix_GetError());
        return false;
    }
    // 16 mixing channels for simultaneous SFX
    Mix_AllocateChannels(16);

    // Load SFX — try .ogg first (CC0 packs), then .wav (procedural fallback)
    u32 loaded = 0;
    char path[256];
    char altPath[256];
    for (u32 i = 0; i < static_cast<u32>(SfxId::SFX_COUNT); i++) {
        // Build OGG path by replacing .wav extension
        const char* base = s_sfxFiles[i];
        u32 baseLen = static_cast<u32>(std::strlen(base));
        char oggName[128];
        std::strncpy(oggName, base, sizeof(oggName) - 1);
        if (baseLen > 4) {
            oggName[baseLen - 4] = '\0'; // strip .wav
            std::strncat(oggName, ".ogg", sizeof(oggName) - std::strlen(oggName) - 1);
        }
        // Try OGG first
        buildPath(path, sizeof(path), oggName);
        s_sfx[i] = Mix_LoadWAV(path);
        if (!s_sfx[i]) {
            // Fall back to WAV
            buildPath(altPath, sizeof(altPath), base);
            s_sfx[i] = Mix_LoadWAV(altPath);
        }
        if (s_sfx[i]) {
            loaded++;
        } else {
            LOG_WARN("Failed to load SFX: %s (.ogg and .wav) (%s)", base, Mix_GetError());
        }
    }

    s_initialized = true;
    LOG_INFO("AudioSystem: loaded %u/%u SFX", loaded, static_cast<u32>(SfxId::SFX_COUNT));
    return true;
}

void AudioSystem::shutdown() {
    if (!s_initialized) return;

    for (u32 i = 0; i < static_cast<u32>(SfxId::SFX_COUNT); i++) {
        if (s_sfx[i]) { Mix_FreeChunk(s_sfx[i]); s_sfx[i] = nullptr; }
    }
    if (s_music) { Mix_FreeMusic(s_music); s_music = nullptr; }

    Mix_CloseAudio();
    s_initialized = false;
    LOG_INFO("AudioSystem: shutdown");
}

void AudioSystem::play(SfxId id, f32 volume) {
    if (!s_initialized) return;
    u32 idx = static_cast<u32>(id);
    if (idx >= static_cast<u32>(SfxId::SFX_COUNT) || !s_sfx[idx]) return;

    f32 vol = volume * s_sfxVol * s_masterVol;
    if (vol <= 0.0f) return;

    // Play on first free channel (-1 = auto)
    int ch = Mix_PlayChannel(-1, s_sfx[idx], 0);
    if (ch >= 0) {
        Mix_Volume(ch, static_cast<int>(vol * MIX_MAX_VOLUME));
    }
}

void AudioSystem::playAt(SfxId id, Vec3 worldPos, Vec3 listenerPos, f32 maxDist) {
    if (!s_initialized) return;
    Vec3 diff = worldPos - listenerPos;
    f32 dist = sqrtf(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
    f32 attenuation = 1.0f - (dist / maxDist);
    if (attenuation <= 0.0f) return;
    play(id, attenuation);
}

void AudioSystem::playMusic(const char* path) {
    if (!s_initialized) return;
    if (s_music) { Mix_FreeMusic(s_music); s_music = nullptr; }
    s_music = Mix_LoadMUS(path);
    if (s_music) {
        Mix_VolumeMusic(static_cast<int>(s_musicVol * s_masterVol * MIX_MAX_VOLUME));
        Mix_PlayMusic(s_music, -1); // loop forever
    } else {
        LOG_WARN("Failed to load music: %s (%s)", path, Mix_GetError());
    }
}

void AudioSystem::stopMusic() {
    if (!s_initialized) return;
    Mix_HaltMusic();
}

void AudioSystem::setMasterVolume(f32 vol) {
    s_masterVol = vol;
    if (s_initialized && s_music) {
        Mix_VolumeMusic(static_cast<int>(s_musicVol * s_masterVol * MIX_MAX_VOLUME));
    }
}

void AudioSystem::setSfxVolume(f32 vol) { s_sfxVol = vol; }

void AudioSystem::setMusicVolume(f32 vol) {
    s_musicVol = vol;
    if (s_initialized && s_music) {
        Mix_VolumeMusic(static_cast<int>(s_musicVol * s_masterVol * MIX_MAX_VOLUME));
    }
}
