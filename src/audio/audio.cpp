// AudioSystem — SDL_mixer-based audio playback for SFX and music.
// Loads all WAV files from assets/audio/ at init, maps them to SfxId enum.
// play() fires on the first free mixing channel. playAt() adds distance attenuation.

#include "audio/audio.h"
#include "audio/audio_settings.h"
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
    "sfx_weapon_cleaver.wav",
    "sfx_weapon_pistol.wav",
    "sfx_weapon_smg.wav",
    "sfx_weapon_carbine.wav",
    "sfx_weapon_revolver.wav",
    "sfx_weapon_bow.wav",
    "sfx_weapon_crossbow.wav",
    "sfx_weapon_throw.wav",
    "sfx_weapon_molotov.wav",
    "sfx_weapon_chakram.wav",
    "sfx_weapon_wand.wav",
    "sfx_weapon_staff.wav",
    "sfx_reload.wav",
    // Combat
    "sfx_hit_melee.wav",
    "sfx_hit_hitscan.wav",
    "sfx_hit_projectile.wav",
    "sfx_ricochet.wav",
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
    "sfx_shrine_activate.wav",
    // Ambient monster cries (3 variants, rolled randomly)
    "sfx_monster_cry_1.wav",
    "sfx_monster_cry_2.wav",
    "sfx_monster_cry_3.wav",
};

// Compile-time guard: this positional table must stay index-aligned with SfxId. If it fires, an
// SfxId was added/removed without updating s_sfxFiles (or vice-versa).
static_assert(sizeof(s_sfxFiles) / sizeof(s_sfxFiles[0]) == static_cast<u32>(SfxId::SFX_COUNT),
              "s_sfxFiles is out of sync with SfxId::SFX_COUNT");

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
    // Initialize OGG Vorbis decoder for music playback
    int mixFlags = MIX_INIT_OGG;
    int mixInitted = Mix_Init(mixFlags);
    if ((mixInitted & mixFlags) != mixFlags) {
        LOG_WARN("Mix_Init(OGG) incomplete: %s — music may not play", Mix_GetError());
    }

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
    Mix_Quit();
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
    // Fallback: .ogg → .wav or vice versa (Switch ships OGG, PC may have WAV)
    if (!s_music) {
        char fallback[256];
        std::strncpy(fallback, path, sizeof(fallback) - 1);
        fallback[sizeof(fallback) - 1] = '\0';
        char* ext = std::strrchr(fallback, '.');
        if (ext && std::strcmp(ext, ".ogg") == 0) {
            std::strcpy(ext, ".wav");
            s_music = Mix_LoadMUS(fallback);
        } else if (ext && std::strcmp(ext, ".wav") == 0) {
            std::strcpy(ext, ".ogg");
            s_music = Mix_LoadMUS(fallback);
        }
    }
    if (s_music) {
        int vol = static_cast<int>(s_musicVol * s_masterVol * MIX_MAX_VOLUME);
        Mix_VolumeMusic(vol);
        int rc = Mix_PlayMusic(s_music, -1);
        LOG_INFO("Music playing: %s (vol=%d, rc=%d)", path, vol, rc);
        if (rc < 0) LOG_WARN("Mix_PlayMusic failed: %s", Mix_GetError());
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

f32 AudioSystem::getMasterVolume() { return s_masterVol; }
f32 AudioSystem::getSfxVolume()    { return s_sfxVol; }
f32 AudioSystem::getMusicVolume()  { return s_musicVol; }

// Persist the three volume levels as one float per line (master, sfx, music). Plain text to mirror
// controls.json (which, despite its extension, is also plain text). Silently no-ops on I/O failure.
void AudioSystem::saveSettings(const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) { LOG_WARN("Audio: could not open %s for writing", path); return; }
    std::fprintf(f, "%.4f\n%.4f\n%.4f\n", s_masterVol, s_sfxVol, s_musicVol);
    std::fclose(f);
    LOG_INFO("Audio: saved settings to %s", path);
}

// Load the three volume levels and apply them through the setters. A missing file (first launch)
// or a short/garbled read leaves the corresponding level at its current default — never fails.
void AudioSystem::loadSettings(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return;  // no saved settings yet — keep defaults
    f32 master = s_masterVol, sfx = s_sfxVol, music = s_musicVol;
    int got = std::fscanf(f, "%f %f %f", &master, &sfx, &music);
    std::fclose(f);
    if (got >= 1) {
        setMasterVolume(AudioSettings::clampVol(master));
        setSfxVolume(AudioSettings::clampVol(sfx));
        setMusicVolume(AudioSettings::clampVol(music));
        LOG_INFO("Audio: loaded settings from %s", path);
    }
}
