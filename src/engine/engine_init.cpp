// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio/audio.h"

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "renderer/gl_context.h"
#include "renderer/renderer.h"
#include "renderer/debug_draw.h"
#include "renderer/hud.h"
#include "renderer/minimap.h"
#include "renderer/font.h"
#include "renderer/item_icons.h"
#include "renderer/material.h"
#include "renderer/obj_loader.h"
#include "renderer/projectile_renderer.h"
#include "world/level_gen.h"
#include "world/level_mesh.h"
#include "world/level_loader.h"
#include "world/collision.h"
#include "world/combat_query.h"
#include "game/player.h"
#include "game/combat.h"
#include "game/enemy_ai.h"
#include "game/squad.h"
#include "game/limb_system.h"
#include "game/projectile.h"
#include "game/item.h"
#include "game/skill.h"
#include "game/inventory_ui.h"
#include "game/game_constants.h"
#include "game/boss_def.h"
#include "game/boss_ai.h"
#include "game/boss_loader.h"
#include "game/enemy_loader.h"
#include "net/net.h"
#include "net/server.h"
#include "net/client.h"
#include "net/snapshot.h"
#include "net/packet.h"
#include "core/log.h"
#include "core/math.h"
#include "core/frame_allocator.h"
#include "core/allocation_tracker.h"
#include "core/profiler.h"

#include <glad/glad.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Shared statics defined in engine.cpp
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;


// ---------------------------------------------------------------------------
// Player class definitions — 8 classes with 4 skills each
// ---------------------------------------------------------------------------
const ClassDef kClassDefs[static_cast<u32>(PlayerClass::CLASS_COUNT)] = {
    // WARRIOR — Melee Tank
    {"Warrior", "Heavy melee fighter with crowd control",
     150.0f, 5.0f, 80.0f, "Iron Sword",
     {SkillId::THUNDERCLAP, SkillId::WAR_CRY, SkillId::WHIRLWIND, SkillId::EARTHQUAKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE},

    // RANGER — Rapid-Fire Sharpshooter
    {"Ranger", "Rapid-fire sharpshooter with piercing shots and volleys",
     80.0f, 6.5f, 100.0f, "Short Bow",
     {SkillId::VOLLEY, SkillId::PIERCING_SHOT, SkillId::BARRAGE, SkillId::MARK_PREY},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::PROJECTILE},

    // SORCERER — Glass Cannon
    {"Sorcerer", "Devastating elemental magic, fragile body",
     70.0f, 5.5f, 150.0f, "Wand of Sparks",
     {SkillId::FIREBALL, SkillId::FROZEN_ORB, SkillId::CHAIN_LIGHTNING, SkillId::METEOR_STRIKE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::PROJECTILE},

    // ROGUE — Hit-and-Run Assassin
    {"Rogue", "Hit-and-run assassin with stealth and backstabs",
     85.0f, 7.0f, 100.0f, "Rusty Dagger",
     {SkillId::FAN_OF_KNIVES, SkillId::SHADOW_STEP, SkillId::POISON_CLOUD, SkillId::SHADOW_DANCE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE},

    // PALADIN — Wrathful Holy Warrior
    {"Paladin", "Wrathful holy warrior raining judgment",
     130.0f, 5.0f, 90.0f, "Iron Sword",
     {SkillId::HOLY_SMITE, SkillId::HOLY_BOMBARDMENT, SkillId::HOLY_NOVA, SkillId::DIVINE_JUDGMENT},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::MELEE},

    // COMBAT ENGINEER — Gadget Specialist
    {"Combat Engineer", "Turrets, tesla coils, and gadgets",
     100.0f, 5.5f, 120.0f, "Pistol",
     {SkillId::SHOCK_BOLT, SkillId::DEPLOY_TURRET, SkillId::TESLA_COIL, SkillId::MECH_OVERDRIVE},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN},

    // MARKSMAN — Precision Sniper
    {"Marksman", "Anti-materiel sniper with devastating penetrating shots",
     75.0f, 6.0f, 100.0f, "Revolver",
     {SkillId::AIMED_SHOT, SkillId::EXPLOSIVE_ROUND, SkillId::OVERCHARGED_MAGAZINE, SkillId::HEADSHOT},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN},

    // TINKERER — Swarm Overlord
    {"Tinkerer", "Swarm overlord who overwhelms with drone armies",
     90.0f, 5.5f, 110.0f, "Pistol",
     {SkillId::SWARM_DEPLOY, SkillId::OVERCLOCK, SkillId::DETONATE_SWARM, SkillId::SWARM_QUEEN},
     {1, 10, 20, 30}, {5, 20, 30, 40}, WeaponType::HITSCAN},

    // WANDERER — evasive counter-attacker, dodge roll replaces block
    {
        "Wanderer",
        "Evasive counter-attacker. Dodge through enemy attacks for counter-hits and stacking attack speed.",
        90.0f,   // baseHealth — low, dodge is the defense
        6.5f,    // baseMoveSpeed — fast, tied with Ranger
        110.0f,  // baseEnergy
        "Iron Sword",
        {SkillId::DEFLECT, SkillId::EXPLOIT_WEAKNESS, SkillId::ADRENALINE_SURGE, SkillId::DEATHS_DANCE},
        {1, 10, 20, 30},   // skill unlock floors
        {5, 20, 30, 40},   // skill upgrade floors
        WeaponType::MELEE   // +20% melee damage
    },
};


// ---------------------------------------------------------------------------
// Engine lifecycle
// ---------------------------------------------------------------------------
void Engine::init() {
    s_engine = this;

    Log::init();
    LOG_INFO("Engine initializing...");

    if (!Window::init("Curse of the Dungeon Engine", 1280, 720)) {
        LOG_ERROR("Failed to initialize window");
        return;
    }

    AudioSystem::init(); // non-fatal — game works without audio
    AudioSystem::setMusicVolume(0.3f); // ambient music sits below SFX

    if (!GLContext::init(Window::getHandle())) {
        LOG_ERROR("Failed to initialize GL context");
        return;
    }

    Clock::init();
    Input::init();
    Input::setRelativeMouseMode(false); // Start with mouse visible for menu

    s_frameAllocator.init(1024 * 1024);
    AllocationTracker::init();

    Renderer::init();
    DebugDraw::init();
    HUD::init();
    FontSystem::init();
    // Font UI scale is set dynamically per-frame based on viewport height
    // (see renderHUD). No fixed scale needed here.
    ItemIconSystem::init();
    // NOTE: LimbSystem::init is called later, after OBJ meshes are loaded

    // Shaders
    m_basicShader = ShaderSystem::load(ASSET_PATH("assets/shaders/basic.vert"),
                                       ASSET_PATH("assets/shaders/basic.frag"));
    m_unlitShader = ShaderSystem::load(ASSET_PATH("assets/shaders/unlit.vert"),
                                       ASSET_PATH("assets/shaders/unlit.frag"));
    m_particleShader = ShaderSystem::load(ASSET_PATH("assets/shaders/particle.vert"),
                                           ASSET_PATH("assets/shaders/particle.frag"));

    // Materials (loads textures from assets/materials.json)
    MaterialSystem::init(ASSET_PATH("assets/materials.json"));

    // Meshes
    m_cubeMesh = MeshSystem::createCube();
    m_quadMesh = MeshSystem::createQuad();

    // Load meshes/materials/JSON content defs and resolve all visual references
    initAssets();

    // Wire all Combat/SkillSystem/ProjectileSystem/Inventory event callbacks
    initCallbacks();

    // Init networking
    Net::init();

    // Start in menu
    m_gameState = GameState::MENU;
    m_menu.selection = 0;
    m_running = true;
    m_accumulator = 0.0;
    m_statsTimer = 0.0;
    m_updateCount = 0;
    m_frameCount = 0;

    // Load global difficulty unlock so the menu can gray out locked tiers
    {
        FILE* f = std::fopen("difficulty_unlock.dat", "rb");
        if (f) {
            (void)std::fread(&m_highestUnlocked, 1, 1, f);
            std::fclose(f);
            if (m_highestUnlocked > 2) m_highestUnlocked = 0; // sanitize bad data
        }
    }

    LOG_INFO("Engine initialized — Phase 4 multiplayer ready");
}


void Engine::shutdown() {
    LOG_INFO("Engine shutting down...");

    AudioSystem::shutdown();
    Net::shutdown();

    // Destroy loaded OBJ meshes (skip index 0 = cube, destroyed below)
    for (u32 i = 1; i < m_meshDefCount; i++) {
        if (m_meshDefs[i].mesh.vao) MeshSystem::destroy(m_meshDefs[i].mesh);
    }

    MeshSystem::destroy(m_cubeMesh);
    MeshSystem::destroy(m_quadMesh);
    MeshSystem::destroy(m_handMesh);
    LevelMeshSystem::destroyAll(m_level.sections, m_level.sectionCount);
    LevelGridSystem::shutdown(m_level.grid);

    ParticleSystem::shutdownBatchBuffers(m_particles);
    MaterialSystem::shutdown();
    ShaderSystem::destroy(m_basicShader);
    ShaderSystem::destroy(m_unlitShader);
    ShaderSystem::destroy(m_particleShader);

    FontSystem::shutdown();
    ItemIconSystem::shutdown();
    HUD::shutdown();
    ProjectileRenderer::shutdown();
    Minimap::shutdown();
    DebugDraw::shutdown();
    Renderer::shutdown();
    s_frameAllocator.shutdown();
    Input::shutdown();
    GLContext::shutdown();
    Window::shutdown();
    AllocationTracker::shutdown();
    Log::shutdown();

    s_engine = nullptr;
}

