// Engine module — see engine.h for class definition.
// Split from engine.cpp for manageability. All methods are Engine:: members.

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "audio/audio.h"
#include "audio/audio_settings.h"

#include "engine/engine.h"
#include "platform/window.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "platform/user_paths.h"
#include "platform/steam.h"
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
#include <ctime>

// Shared statics defined in engine.cpp
// Shared statics defined in engine.cpp
extern Engine* s_engine;
extern FrameAllocator s_frameAllocator;
extern bool s_firstKillDropGiven;


// kClassDefs (the player class table) moved to game/class_defs.cpp so the balance-lab
// tests can link it without the Engine; still declared extern in game/item.h.

// ---------------------------------------------------------------------------
// Engine lifecycle
// ---------------------------------------------------------------------------
void Engine::init() {
    s_engine = this;

#ifdef __SWITCH__
    Log::init();  // Switch: console only (app storage may be read-only)
#else
    // Write a persistent log to the user-data dir so Windows Release (console output is compiled off in
    // log.cpp) and any hard crash leave a diagnosable trail. Path:
    //   Windows: %APPDATA%\EdRethardo\DungeonEngine\DungeonEngine.log
    //   Linux:   ~/.local/share/EdRethardo/DungeonEngine/DungeonEngine.log
    // SDL_GetPrefPath needs no SDL_Init, and log.cpp fflushes every line, so the last line written
    // survives a hard crash — which is exactly what we need to localize a startup crash.
    static char s_logPath[512];
    Log::init(Platform::userDataPath("DungeonEngine.log", s_logPath, sizeof(s_logPath)));
#endif
    LOG_INFO("Engine initializing...");

    // Seed the global RNG once from wall-clock entropy so gameplay RNG (loot/procs/
    // particles) varies between runs. The dungeon uses a dedicated per-run seed
    // (m_level.levelSeed), so multiplayer determinism is unaffected by this.
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    // Demo builds advertise themselves in the title bar (kDemoBuild is constexpr).
    const char* windowTitle = GameConst::kDemoBuild ? "Curse of the Dungeon Engine - DEMO"
                                                    : "Curse of the Dungeon Engine";
    if (!Window::init(windowTitle, 1280, 720)) {
        LOG_ERROR("Failed to initialize window");
        return;
    }

    // Relocate legacy CWD / assets-config user files (saves, progression, settings) into the
    // per-user data dir on first run, so the loads below and Steam Cloud both see them. Desktop
    // only; no-op on Switch. Runs after Window::init (SDL is up) and before any user-file load/scan.
    Platform::migrateLegacyUserData(MAX_SAVE_SLOTS);

    AudioSystem::init(); // non-fatal — game works without audio
    AudioSystem::setMusicVolume(AudioSettings::DEFAULT_MUSIC); // ambient music sits below SFX
    // Restore the player's saved volume levels — overrides the defaults just set. No-op on first
    // launch (missing file keeps defaults). Must come AFTER the setMusicVolume default above.
#ifdef __SWITCH__
    AudioSystem::loadSettings(ASSET_PATH("assets/config/audio.json"));  // romfs default on Switch
#else
    char audioCfgPath[512];
    AudioSystem::loadSettings(Platform::userDataPath("audio.json", audioCfgPath, sizeof(audioCfgPath)));
#endif

    if (!GLContext::init(Window::getHandle())) {
        LOG_ERROR("Failed to initialize GL context");
        return;
    }

#ifndef __SWITCH__
    // Restore the saved borderless-fullscreen preference now that the window + GL context exist,
    // so the very first rendered frame is at the right viewport. Desktop-only (Switch is always
    // fullscreen). No-op on first launch (missing file keeps the windowed default). Read from the
    // per-user dir; video.cfg is deliberately NOT Steam-Cloud-synced (display is machine-specific).
    char videoCfgPath[512];
    Window::loadVideoSettings(Platform::userDataPath("video.cfg", videoCfgPath, sizeof(videoCfgPath)));
#endif

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
    // Reuses unlit.vert (it forwards UVs); only the fragment stage differs.
    m_vignetteShader = ShaderSystem::load(ASSET_PATH("assets/shaders/unlit.vert"),
                                          ASSET_PATH("assets/shaders/vignette.frag"));
    // Flat scrim for the in-game options overlay. Shares unlit.vert; dim.frag just emits u_color.
    m_dimShader = ShaderSystem::load(ASSET_PATH("assets/shaders/unlit.vert"),
                                     ASSET_PATH("assets/shaders/dim.frag"));
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

    // Init Steam before networking so the relay is warming up and lobby callbacks are live. No-op /
    // returns false when the SDK is absent or no Steam client is running (game runs Steam-less).
    Steam::init();

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
        char diffPath[512];
        FILE* f = std::fopen(Platform::userDataPath("difficulty_unlock.dat", diffPath, sizeof(diffPath)), "rb");
        if (f) {
            (void)std::fread(&m_highestUnlocked, 1, 1, f);
            std::fclose(f);
            if (m_highestUnlocked > 2) m_highestUnlocked = 0; // sanitize bad data
        }
    }

    // Pet menagerie collection (menagerie.dat, same profile-wide pattern as the unlock above).
    loadMenagerie();
    loadStash();   // shared account stash (after item defs & before any UI can open it)
    {   // account-wide town unlock (1-byte sidecar, difficulty_unlock pattern)
        char tuPath[512];
        FILE* tf = std::fopen(Platform::userDataPath("town_unlock.dat", tuPath, sizeof(tuPath)), "rb");
        if (tf) { u8 v = 0; m_townUnlocked = (std::fread(&v, 1, 1, tf) == 1 && v != 0); std::fclose(tf); }
    }

    LOG_INFO("Engine initialized — Phase 4 multiplayer ready");
}


void Engine::shutdown() {
    LOG_INFO("Engine shutting down...");

    saveStash();   // no-op unless dirty — last-chance flush for the shared account stash

    AudioSystem::shutdown();
    Net::shutdown();
    Steam::shutdown();

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

    // Character-inspect offscreen FBO (created lazily in renderInspectModelToFbo). The render-scale
    // m_sceneFbo is owned by the GL context teardown, but the inspect target is explicit here so the
    // texture/renderbuffer don't leak across an init/shutdown cycle (e.g. window recreation).
    if (m_inspectFbo)      glDeleteFramebuffers(1, &m_inspectFbo);
    if (m_inspectColorTex) glDeleteTextures(1, &m_inspectColorTex);
    if (m_inspectDepthRbo) glDeleteRenderbuffers(1, &m_inspectDepthRbo);
    m_inspectFbo = m_inspectColorTex = m_inspectDepthRbo = m_inspectFboW = m_inspectFboH = 0;
    // Unit quad used by drawTexturedQuad to composite the inspect FBO into screen-space.
    if (m_inspectQuadVao) glDeleteVertexArrays(1, &m_inspectQuadVao);
    if (m_inspectQuadVbo) glDeleteBuffers(1, &m_inspectQuadVbo);
    m_inspectQuadVao = m_inspectQuadVbo = 0;

    MaterialSystem::shutdown();
    ShaderSystem::destroy(m_basicShader);
    ShaderSystem::destroy(m_unlitShader);
    ShaderSystem::destroy(m_vignetteShader);
    ShaderSystem::destroy(m_dimShader);
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

