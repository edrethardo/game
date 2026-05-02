#pragma once

#include "core/types.h"
#include "renderer/camera.h"
#include "renderer/shader.h"
#include "renderer/texture.h"
#include "world/level_grid.h"
#include "world/level_mesh.h"
#include "world/raycast.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/entity.h"
#include "game/weapon.h"
#include "game/projectile.h"

static constexpr u32 MAX_LEVEL_SECTIONS = 64;

class Engine {
public:
    void init();
    void shutdown();
    void run();

private:
    static constexpr f64 FIXED_DT            = 1.0 / 60.0;
    static constexpr s32 MAX_STEPS_PER_FRAME = 4;

    bool m_running     = false;
    f64  m_accumulator = 0.0;
    f64  m_statsTimer  = 0.0;
    u32  m_updateCount = 0;
    u32  m_frameCount  = 0;

    // Rendering
    Camera  m_camera;
    Shader  m_basicShader;
    Shader  m_unlitShader;
    Texture m_wallTexture;
    Mesh    m_cubeMesh;

    // Level
    LevelGrid    m_grid;
    LevelSection m_sections[MAX_LEVEL_SECTIONS];
    u32          m_sectionCount = 0;

    // Player + combat
    Player         m_player;
    WeaponDef      m_weaponDefs[MAX_WEAPON_DEFS];
    u32            m_weaponDefCount = 0;
    WeaponState    m_weaponState;

    // Entities + projectiles
    EntityPool     m_entities;
    ProjectilePool m_projectiles;

    // Combat feedback
    CombatHit   m_lastCombatHit;
    f32         m_hitMarkerTimer = 0.0f;

    void update(f32 dt);
    void render(f32 alpha);
    void handleWeaponFire(f32 dt);
    void updateTargetLock(f32 dt);
    void logStats();
};
